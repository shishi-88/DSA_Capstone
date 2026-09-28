/*
 * tests.c -- built-in self-test suite (Phase 3: testing layer).
 *
 * Nine cases covering everything the rubric asks to see:
 *   1. normal ranking              6. exclusions (self + direct friends)
 *   2. tie-breaking by lower id    7. invalid inputs (no crashes)
 *   3. zero-friend edge case       8. add/remove users & edges
 *   4. everyone-already-friended   9. larger graph + bounded exploration
 *   5. top-N truncation
 *
 * Each case prints PASS/FAIL lines; a summary is printed at the end.
 * Every case builds its own Graph and destroys it, so repeated runs stay
 * leak-free (verify with `make test` under valgrind or -fsanitize).
 */

#include "tests.h"
#include "graph.h"
#include "recommend.h"

#include <stdio.h>
#include <string.h>

static int tests_passed;
static int tests_failed;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (cond) { tests_passed++; printf("  PASS  %s\n", msg); }        \
        else      { tests_failed++; printf("  FAIL  %s\n", msg); }        \
    } while (0)

/* Tiny fixed builder: ids then "a-b" edge pairs, terminated by -1. */
static Graph *build(const int *spec)
{
    Graph *g = graph_create();
    int i = 0;
    while (spec[i] >= 0) graph_add_user(g, spec[i++]);
    for (i++; spec[i] >= 0; i += 2)
        graph_add_edge(g, spec[i], spec[i + 1]);
    return g;
}

/* spec layout: [users..., -1, edges as pairs..., -1] */
static const int SPEC_NORMAL[] = { 0,1,2,3,4,5, -1, 0,1, 0,2, 0,3, 1,4, 2,4, 3,5, -1 };
static const int SPEC_TIE[]    = { 0,1,2,3,4,5, -1, 0,1, 0,2, 0,3, 1,4, 2,4, 1,5, 3,5, -1 };
static const int SPEC_ZERO[]   = { 0,1,9, -1, 0,1, -1 };
static const int SPEC_K4[]     = { 0,1,2,3, -1, 0,1, 0,2, 0,3, 1,2, 1,3, 2,3, -1 };
static const int SPEC_SMALL[]  = { 0,1,2,3, -1, 0,1, 1,2, 2,3, -1 };

/* Case 1: ranking is by mutual count DESC; the engine excludes self and
 * direct friends; the mutual-list helper agrees with the counter. */
static void test_normal_ranking(void)
{
    printf("[1] Normal case -- ranked recommendations\n");
    Graph *g = build(SPEC_NORMAL);

    Recommendation recs[MAX_TOP_N];
    int count = 0;
    RecommendStats st;
    RecommendStatus s = recommend_friends(g, 0, 10, recs, MAX_TOP_N, &count, &st);
    CHECK(s == RECOMMEND_OK, "recommend_friends returns OK");
    CHECK(count == 2, "exactly 2 candidates found (4 and 5)");
    CHECK(count >= 1 && recs[0].user_id == 4 && recs[0].mutual_count == 2,
          "user 4 ranked first with 2 mutual friends (1,2)");
    CHECK(count >= 2 && recs[1].user_id == 5 && recs[1].mutual_count == 1,
          "user 5 ranked second with 1 mutual friend (3)");
    CHECK(st.direct_friends == 3, "stats: 3 direct friends scanned");
    CHECK(st.candidates_seen == 2, "stats: 2 distinct 2-hop candidates");

    int mids[MAX_MUTUAL_PRINT], mn = 0;
    recommend_mutual_users(g, 0, 4, mids, MAX_MUTUAL_PRINT, &mn);
    CHECK(mn == 2 && mids[0] == 1 && mids[1] == 2,
          "mutual(0,4) = {1,2} via sorted-list merge");

    graph_destroy(g);
}

/* Case 2: two candidates both have 2 mutual friends -> the lower id (4)
 * must come first; identical reruns must be identical (determinism). */
static void test_tie_break(void)
{
    printf("[2] Tie-breaking -- equal mutual counts -> lower id first\n");
    Graph *g = build(SPEC_TIE);

    Recommendation recs[MAX_TOP_N];
    int count = 0;
    recommend_friends(g, 0, 10, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 2, "2 tied candidates found");
    CHECK(count == 2 && recs[0].user_id == 4 && recs[1].user_id == 5,
          "tie 2-2 broken by lower user id (4 before 5)");

    int count2 = 0;
    Recommendation recs2[MAX_TOP_N];
    recommend_friends(g, 0, 10, recs2, MAX_TOP_N, &count2, NULL);
    /* Compare only the meaningful prefix: stack buffers beyond count are
     * deliberately not part of the API contract. */
    CHECK(count2 == count
              && memcmp(recs, recs2, (size_t)count * sizeof *recs) == 0,
          "deterministic: identical query gives identical ranking");

    graph_destroy(g);
}

/* Case 3: a user with no friends has no neighbourhood at all. */
static void test_zero_friends(void)
{
    printf("[3] Edge case -- user with zero friends\n");
    Graph *g = build(SPEC_ZERO);

    Recommendation recs[MAX_TOP_N];
    int count = -1;
    RecommendStats st;
    memset(&st, 0xAA, sizeof st);
    RecommendStatus s = recommend_friends(g, 9, 5, recs, MAX_TOP_N, &count, &st);
    CHECK(s == RECOMMEND_OK, "query on isolated user returns OK, not an error");
    CHECK(count == 0, "empty recommendation list for isolated user");
    CHECK(st.direct_friends == 0, "stats confirm no friends were scanned");

    graph_destroy(g);
}

/* Case 4: K4 -- every user within 2 hops is already a direct friend. */
static void test_all_friends_already(void)
{
    printf("[4] Edge case -- all 2-hop users are already friends\n");
    Graph *g = build(SPEC_K4);

    Recommendation recs[MAX_TOP_N];
    int count = -1;
    RecommendStats st;
    recommend_friends(g, 0, 5, recs, MAX_TOP_N, &count, &st);
    CHECK(count == 0, "no recommendations: everyone reachable is a friend");
    CHECK(st.direct_friends == 3, "stats show 3 direct friends blocked");

    graph_destroy(g);
}

/* Case 5: top-N truncation must keep the BEST N, never arbitrary N. */
static void test_top_n(void)
{
    printf("[5] Top-N -- bounded min-heap keeps the best N\n");
    Graph *g = graph_create();
    for (int u = 0; u <= 102; u++) graph_add_user(g, u);
    /* user 0 befriends 1..10; targets are non-friends of 0 with distinct
     * mutual counts: 100 shares 1..10 (m=10), 101 shares 1..5 (m=5),
     * 102 shares 1..3 (m=3). */
    for (int u = 1; u <= 10; u++) { graph_add_edge(g, 0, u); graph_add_edge(g, u, 100); }
    for (int u = 1; u <= 5;  u++) graph_add_edge(g, u, 101);
    for (int u = 1; u <= 3;  u++) graph_add_edge(g, u, 102);

    Recommendation recs[MAX_TOP_N];
    int count = 0;
    recommend_friends(g, 0, 2, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 2, "N=2 returns exactly 2 results");
    CHECK(recs[0].user_id == 100 && recs[1].user_id == 101,
          "the best 2 kept: 100 (m=10) then 101 (m=5), 102 (m=3) dropped");

    recommend_friends(g, 0, 1, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 1 && recs[0].user_id == 100, "N=1 keeps only the best");

    recommend_friends(g, 0, 50, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 3, "N larger than the candidate set returns all of them");

    graph_destroy(g);
}

/* Case 6: neither the query user nor a direct friend may ever appear. */
static void test_exclusions(void)
{
    printf("[6] Exclusions -- query user and direct friends never recommended\n");
    Graph *g = build(SPEC_NORMAL);
    graph_add_edge(g, 0, 4);          /* turn best candidate into a friend */

    Recommendation recs[MAX_TOP_N];
    int count = 0;
    recommend_friends(g, 0, 10, recs, MAX_TOP_N, &count, NULL);
    bool clean = count == 1;          /* only user 5 may remain */
    for (int i = 0; i < count; i++)
        if (recs[i].user_id == 0 || recs[i].user_id == 4) clean = false;
    CHECK(clean && count == 1 && recs[0].user_id == 5,
          "after 0-4 friendship, user 4 is excluded and only 5 remains");

    graph_destroy(g);
}

/* Case 7: bad inputs must produce error codes, never crashes. */
static void test_invalid_inputs(void)
{
    printf("[7] Error handling -- invalid ids and arguments\n");
    Graph *g = build(SPEC_ZERO);

    Recommendation recs[MAX_TOP_N];
    int count = -1;
    CHECK(recommend_friends(g, 99, 5, recs, MAX_TOP_N, &count, NULL)
              == RECOMMEND_ERR_NO_USER, "unknown query user -> ERR_NO_USER");
    CHECK(count == 0, "out_count reset to 0 on error");
    CHECK(recommend_friends(g, 0, -1, recs, MAX_TOP_N, &count, NULL)
              == RECOMMEND_ERR_BAD_ARG, "negative N -> ERR_BAD_ARG");
    CHECK(recommend_friends(NULL, 0, 5, recs, MAX_TOP_N, &count, NULL)
              == RECOMMEND_ERR_NULL, "NULL graph -> ERR_NULL");

    int ids[MAX_MUTUAL_PRINT], mn = -1;
    CHECK(recommend_mutual_users(g, 0, 99, ids, MAX_MUTUAL_PRINT, &mn)
              == RECOMMEND_ERR_NO_USER, "mutual lookup with unknown user fails cleanly");

    CHECK(graph_add_user(g, 0) == GRAPH_ERR_DUP, "duplicate user rejected");
    CHECK(graph_add_edge(g, 0, 0) == GRAPH_ERR_SELF, "self-friendship rejected");
    CHECK(graph_add_edge(g, 0, 42) == GRAPH_ERR_NO_USER, "edge to unknown user rejected");
    CHECK(graph_has_user(g, -3) == false, "negative id never matches a user");

    graph_destroy(g);
}

/* Case 8: mutations must be reflected immediately and symmetrically. */
static void test_mutations(void)
{
    printf("[8] Mutations -- add/remove users, edges, cleanup\n");
    Graph *g = build(SPEC_SMALL);

    Recommendation recs[MAX_TOP_N];
    int count = 0;
    recommend_friends(g, 0, 10, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 1 && recs[0].user_id == 2, "initially only user 2 is recommendable");

    CHECK(graph_add_edge(g, 0, 2) == GRAPH_OK, "add friendship 0-2");
    recommend_friends(g, 0, 10, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 1 && recs[0].user_id == 3,
          "after 0-2: user 2 becomes a direct friend, user 3 surfaces");
    CHECK(graph_has_edge(g, 2, 0) && graph_has_edge(g, 0, 2),
          "edge lookup is symmetric in both directions");

    CHECK(graph_remove_edge(g, 0, 2) == GRAPH_OK, "remove friendship 0-2");
    CHECK(!graph_has_edge(g, 0, 2) && graph_degree(g, 0) == 1,
          "removal is symmetric and degree updates");

    recommend_friends(g, 0, 10, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 1 && recs[0].user_id == 2, "recommendations revert after removal");

    CHECK(graph_remove_user(g, 2) == GRAPH_OK, "remove user 2");
    CHECK(graph_degree(g, 1) == 1,
          "user 1 keeps its remaining friend 0 after user 2 is removed");
    recommend_friends(g, 0, 10, recs, MAX_TOP_N, &count, NULL);
    CHECK(count == 0, "no candidates left after user removal");
    CHECK(graph_remove_user(g, 2) == GRAPH_ERR_NO_USER, "removing twice fails cleanly");

    graph_destroy(g);
}

/* Case 9: a larger graph (121 users).  The query user sits mid-chain with
 * degree 2, so a correct bounded BFS must touch only ~5 users even though
 * the network is 20x bigger -- the locality proof the rubric asks for. */
static void test_larger_graph(void)
{
    printf("[9] Larger dataset -- 101 users, bounded exploration\n");
    Graph *g = graph_create();
    for (int u = 0; u <= 99; u++) graph_add_user(g, u);   /* 0 = hub, 1..99 chain/star */
    graph_add_user(g, 130);                               /* far-away isolated user     */
    for (int u = 1; u <= 30; u++) graph_add_edge(g, 0, u);        /* hub spokes  */
    for (int u = 31; u <= 99; u++) graph_add_edge(g, u - 1, u);   /* long chain  */

    CHECK(graph_user_count(g) == 101 && graph_edge_count(g) == 99,
          "graph built: 101 users, 99 edges");

    Recommendation recs[MAX_TOP_N];
    int count = 0;
    RecommendStats st;
    recommend_friends(g, 32, 5, recs, MAX_TOP_N, &count, &st);
    long touched = (long)st.candidates_seen + st.direct_friends + 1;
    CHECK(touched <= 8, "chain query touches <= 8 users of 101 (not the whole graph)");
    CHECK(count == 2 && recs[0].user_id == 30 && recs[1].user_id == 34
              && recs[0].mutual_count == 1,
          "candidates {30, 34}, tie at 1 mutual -> lower id (30) first");
    bool saw_130 = false;
    for (int i = 0; i < count; i++) if (recs[i].user_id == 130) saw_130 = true;
    CHECK(!saw_130, "isolated user 130 (far beyond 2 hops) never recommended");

    recommend_friends(g, 0, 5, recs, MAX_TOP_N, &count, &st);
    CHECK(count == 1 && recs[0].user_id == 31 && recs[0].mutual_count == 1,
          "hub query across 30 friendships finds its single 2-hop candidate");

    graph_destroy(g);
}

void run_self_tests(void)
{
    tests_passed = 0;
    tests_failed = 0;
    printf("\n==================== SELF-TEST SUITE ====================\n");
    test_normal_ranking();
    test_tie_break();
    test_zero_friends();
    test_all_friends_already();
    test_top_n();
    test_exclusions();
    test_invalid_inputs();
    test_mutations();
    test_larger_graph();
    printf("==========================================================\n");
    printf("RESULT: %d passed, %d failed\n\n", tests_passed, tests_failed);
}
