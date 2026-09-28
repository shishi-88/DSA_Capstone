/*
 * main.c -- menu-driven CLI (Phase 3: interface layer)
 *
 * LAYERING: this file is the ONLY place that talks to the user.  It holds
 * no graph or algorithm logic of its own -- every operation delegates to
 * the backend modules (graph.c = data structures, recommend.c = 2-hop BFS
 * + ranking), so the front end can be swapped (CLI -> HTTP -> GUI) without
 * touching the algorithm.
 *
 * Input handling: every prompt goes through read_int(), which uses
 * strtol + full-line consumption, so junk input like "3x" or an empty
 * line can never crash or desynchronise the menu (basic error handling
 * instead of crashing, as required by the rubric).
 */

#include "graph.h"
#include "recommend.h"
#include "tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEFAULT_DATASET "data/sample_network.txt"
#define LINE_MAX_LEN    512
#define LIST_COLS       8    /* ids per row when dumping long lists     */

/* ------------------------------------------------------------------ */
/* Small input utilities (frontend only)                               */
/* ------------------------------------------------------------------ */

/* Read one integer; returns true on success.  Everything up to the next
 * newline is consumed either way, so a bad token cannot poison later
 * reads.  range-checks keep bad ids out of the backend. */
static bool read_int(const char *prompt, int *out)
{
    char buf[64];
    for (;;) {
        printf("%s", prompt);
        if (!fgets(buf, sizeof buf, stdin)) return false;
        char *end = NULL;
        long v = strtol(buf, &end, 10);
        /* Skip trailing spaces; anything else (e.g. "3x") is rejected. */
        while (end && *end && isspace((unsigned char)*end)) end++;
        if (end && *end == '\0' && buf[0] != '\n') {
            *out = (int)v;
            return true;
        }
        printf("  ! Please enter a whole number.\n");
    }
}

/* User id with a sane lower bound; upper bound guards against overflow. */
static bool read_user_id(const char *prompt, int *out)
{
    int v;
    if (!read_int(prompt, &v)) return false;
    if (v < 0) {
        printf("  ! User ids must be >= 0.\n");
        return false;
    }
    *out = v;
    return true;
}

/* Print a possibly long list of ids in wrapped columns. */
static void print_id_list(const int *ids, int n)
{
    for (int i = 0; i < n; i++) {
        printf("%d%s", ids[i], (i + 1 == n) ? "\n" : ((i + 1) % LIST_COLS ? ", " : ",\n                                "));
    }
    if (n == 0) printf("(none)\n");
}

/* ------------------------------------------------------------------ */
/* Dataset loader (Phase 3: reusable network files)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int users_loaded;
    int edges_loaded;
    int bad_lines;
} LoadStats;

/*
 * Format (one instruction per line; '#' starts a comment):
 *     u <id>        add user
 *     e <a> <b>     add friendship between existing users
 * Bad lines are reported and SKIPPED -- one typo must not abort the load.
 */
static bool load_network(Graph *g, const char *path, LoadStats *ls)
{
    memset(ls, 0, sizeof *ls);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("  ! Cannot open dataset file '%s'.\n", path);
        return false;
    }
    char line[LINE_MAX_LEN];
    int line_no = 0;
    while (fgets(line, sizeof line, fp)) {
        line_no++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;      /* blank or comment */

        char kind;
        int a = 0, b = 0;
        if (sscanf(p, " %c %d %d", &kind, &a, &b) < 2) {
            printf("  ! %s:%d: malformed line skipped\n", path, line_no);
            ls->bad_lines++;
            continue;
        }
        GraphStatus st;
        if (kind == 'u') {
            st = graph_add_user(g, a);
            if (st == GRAPH_OK) ls->users_loaded++;
            else if (st == GRAPH_ERR_DUP) { /* re-loading the same file is fine */ }
            else { printf("  ! %s:%d: user %d rejected (%s)\n",
                          path, line_no, a, graph_status_str(st)); ls->bad_lines++; }
        } else if (kind == 'e') {
            st = graph_add_edge(g, a, b);
            if (st == GRAPH_OK) ls->edges_loaded++;
            else if (st == GRAPH_ERR_DUP) { }   /* duplicate edge: ignore */
            else { printf("  ! %s:%d: edge %d-%d rejected (%s)\n",
                          path, line_no, a, b, graph_status_str(st)); ls->bad_lines++; }
        } else {
            printf("  ! %s:%d: unknown directive '%c' skipped\n", path, line_no, kind);
            ls->bad_lines++;
        }
    }
    fclose(fp);
    return true;
}

/* ------------------------------------------------------------------ */
/* Action handlers (thin wrappers around the backend API)              */
/* ------------------------------------------------------------------ */

static void action_summary(const Graph *g)
{
    printf("\n--- Network summary ---\n");
    printf("Users: %d | Friendships (undirected edges): %ld\n",
           graph_user_count(g), graph_edge_count(g));
}

static void action_list_friends(const Graph *g)
{
    int id;
    if (!read_user_id("User id to list friends for: ", &id)) return;
    const GraphNode *n = graph_get_node(g, id);
    if (!n) {
        printf("  ! Unknown user %d.\n", id);   /* error path, not a crash */
        return;
    }
    printf("\nUser %d has %d friend%s:\n  ",
           id, n->friends.count, n->friends.count == 1 ? "" : "s");
    print_id_list(n->friends.ids, n->friends.count);
}

static void action_check_friendship(const Graph *g)
{
    int a, b;
    if (!read_user_id("First user id: ", &a)) return;
    if (!read_user_id("Second user id: ", &b)) return;
    if (!graph_has_user(g, a) || !graph_has_user(g, b)) {
        if (graph_user_count(g) == 0)
            printf("  ! The network is empty -- load a dataset first.\n");
        else
            printf("  ! Both users must exist (known ids 0..%d).\n",
                   graph_user_count(g) - 1);
        return;
    }
    printf("\n%d and %d are %s.\n", a, b,
           graph_has_edge(g, a, b) ? "direct friends" : "NOT directly connected");
}

static void action_add_user(Graph *g)
{
    int id;
    if (!read_user_id("New user id: ", &id)) return;
    GraphStatus st = graph_add_user(g, id);
    if (st == GRAPH_OK) printf("\nUser %d added.\n", id);
    else printf("  ! Could not add user %d: %s.\n", id, graph_status_str(st));
}

static void action_add_friendship(Graph *g)
{
    int a, b;
    if (!read_user_id("First user id: ", &a)) return;
    if (!read_user_id("Second user id: ", &b)) return;
    GraphStatus st = graph_add_edge(g, a, b);
    if (st == GRAPH_OK) printf("\nFriendship %d -- %d added.\n", a, b);
    else printf("  ! Could not add friendship: %s.\n", graph_status_str(st));
}

static void action_remove_friendship(Graph *g)
{
    int a, b;
    if (!read_user_id("First user id: ", &a)) return;
    if (!read_user_id("Second user id: ", &b)) return;
    GraphStatus st = graph_remove_edge(g, a, b);
    if (st == GRAPH_OK) printf("\nFriendship %d -- %d removed.\n", a, b);
    else printf("  ! Could not remove friendship: %s.\n", graph_status_str(st));
}

static void action_remove_user(Graph *g)
{
    int id;
    if (!read_user_id("User id to remove: ", &id)) return;
    GraphStatus st = graph_remove_user(g, id);
    if (st == GRAPH_OK) printf("\nUser %d removed (their friendships were cleaned up).\n", id);
    else printf("  ! Could not remove user %d: %s.\n", id, graph_status_str(st));
}

/* Shared by options 9 and 11: option 11 additionally prints the
 * exploration statistics that PROVE the traversal stayed local. */
static void action_recommend(Graph *g, bool show_stats)
{
    int id, n;
    if (!read_user_id("User id to get recommendations for: ", &id)) return;
    if (!read_int("How many recommendations (N)? ", &n)) return;
    if (n < 0) { printf("  ! N must be >= 0.\n"); return; }

    RecommendStats stats;
    Recommendation recs[MAX_TOP_N];
    int count = 0;
    RecommendStatus st = recommend_friends(g, id, n, recs, MAX_TOP_N,
                                           &count, &stats);
    if (st != RECOMMEND_OK) {
        printf("  ! Recommendation failed: %s.\n", recommend_status_str(st));
        return;
    }

    if (count == 0) {
        /* Distinguish the two empty-list reasons -- the rubric's edge cases. */
        if (stats.direct_friends == 0)
            printf("\nNo recommendations: user %d has no friends yet, so there is "
                   "no neighbourhood to explore.\n", id);
        else
            printf("\nNo recommendations: everyone within 2 hops of user %d is "
                   "already their friend.\n", id);
    } else {
        printf("\nTop %d recommendation%s for user %d "
               "(ranked by mutual friends, ties -> lower id):\n",
               count, count == 1 ? "" : "s", id);
        for (int i = 0; i < count; i++)
            printf("  %2d. user %-4d (%d mutual friend%s)\n",
                   i + 1, recs[i].user_id, recs[i].mutual_count,
                   recs[i].mutual_count == 1 ? "" : "s");
    }

    if (show_stats) {
        long touched = (long)stats.candidates_seen + stats.direct_friends + 1;
        printf("\n  Exploration statistics (bounded 2-hop BFS):\n");
        printf("    direct friends scanned ........... %d\n", stats.direct_friends);
        printf("    level-1 adjacency entries read ... %ld\n", stats.edges_scanned);
        printf("    distinct 2-hop candidates ........ %d\n", stats.candidates_seen);
        printf("    heap comparisons for top-N ....... %ld\n", stats.heap_comparisons);
        printf("    users touched .................... %ld of %d (%.1f%% of the network)\n",
               touched, stats.graph_users,
               stats.graph_users ? 100.0 * (double)touched / stats.graph_users : 0.0);
        printf("    edges touched .................... ~%ld of %ld\n",
               stats.edges_scanned, stats.graph_edges);
    }
}

static void action_mutuals(const Graph *g)
{
    int a, b;
    if (!read_user_id("First user id: ", &a)) return;
    if (!read_user_id("Second user id: ", &b)) return;
    int ids[MAX_MUTUAL_PRINT], count = 0;
    RecommendStatus st = recommend_mutual_users(g, a, b, ids, MAX_MUTUAL_PRINT,
                                                &count);
    if (st != RECOMMEND_OK) {
        printf("  ! Lookup failed: %s.\n", recommend_status_str(st));
        return;
    }
    printf("\nMutual friends of %d and %d (%d):\n  ", a, b, count);
    print_id_list(ids, count);
}

static void action_dump(const Graph *g)
{
    printf("\n--- Adjacency lists (sorted) ---\n");
    for (int i = 0; i < graph_user_count(g); i++) {
        const GraphNode *n = graph_node_at(g, i);
        if (!n) continue;
        printf("  %d -> ", n->user_id);
        print_id_list(n->friends.ids, n->friends.count);
    }
}

/* ------------------------------------------------------------------ */
/* Menu loop                                                           */
/* ------------------------------------------------------------------ */

static void print_menu(void)
{
    printf("\n========= Friend Recommendation Engine (DSA capstone) =========\n");
    printf("  1. Load network from file (default: %s)\n", DEFAULT_DATASET);
    printf("  2. Network summary\n");
    printf("  3. List a user's friends\n");
    printf("  4. Check whether two users are friends\n");
    printf("  5. Add a user\n");
    printf("  6. Add a friendship\n");
    printf("  7. Remove a friendship\n");
    printf("  8. Remove a user\n");
    printf("  9. Recommend friends (bounded 2-hop BFS, top N)\n");
    printf(" 10. Recommend friends + exploration statistics\n");
    printf(" 11. Show mutual friends of two users\n");
    printf(" 12. Dump adjacency lists\n");
    printf(" 13. Run built-in self-tests (9 cases, 45 checks)\n");
    printf("  0. Exit\n");
    printf("================================================================\n");
}

int main(void)
{
    Graph *g = graph_create();
    if (!g) {
        fprintf(stderr, "Fatal: out of memory.\n");
        return EXIT_FAILURE;
    }

    /* Pre-load the sample dataset so the demo works out of the box. */
    LoadStats ls;
    printf("Loading default dataset '%s'...\n", DEFAULT_DATASET);
    if (load_network(g, DEFAULT_DATASET, &ls))
        printf("Loaded %d users and %d friendships (%d bad lines).\n",
               ls.users_loaded, ls.edges_loaded, ls.bad_lines);

    bool running = true;
    while (running) {
        print_menu();
        int choice;
        if (!read_int("Select an option: ", &choice)) break;
        switch (choice) {
        case 1: {
            char path[LINE_MAX_LEN];
            printf("Dataset path [%s]: ", DEFAULT_DATASET);
            if (!fgets(path, sizeof path, stdin)) break;
            path[strcspn(path, "\n")] = '\0';
            if (path[0] == '\0') strcpy(path, DEFAULT_DATASET);
            if (load_network(g, path, &ls))
                printf("Loaded %d users and %d friendships (%d bad lines).\n",
                       ls.users_loaded, ls.edges_loaded, ls.bad_lines);
            break;
        }
        case 2:  action_summary(g);           break;
        case 3:  action_list_friends(g);      break;
        case 4:  action_check_friendship(g);  break;
        case 5:  action_add_user(g);          break;
        case 6:  action_add_friendship(g);    break;
        case 7:  action_remove_friendship(g); break;
        case 8:  action_remove_user(g);       break;
        case 9:  action_recommend(g, false);  break;
        case 10: action_recommend(g, true);   break;
        case 11: action_mutuals(g);           break;
        case 12: action_dump(g);              break;
        case 13: run_self_tests();            break;
        case 0:  running = false;             break;
        default:
            printf("  ! Unknown option %d -- please choose 0..13.\n", choice);
            break;
        }
    }

    /* Single exit path -> single graph_destroy call: no leaks by design. */
    graph_destroy(g);
    printf("\nGoodbye -- all memory freed.\n");
    return EXIT_SUCCESS;
}
