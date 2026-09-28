#ifndef RECOMMEND_H
#define RECOMMEND_H

#include "graph.h"

/*
 * recommend.h -- bounded 2-hop BFS friend recommendation (Phase 2 layer)
 *
 * ALGORITHM (bounded 2-hop breadth-first search):
 *   The engine deliberately never traverses the whole network.  For a
 *   query user u it expands exactly two BFS levels:
 *
 *       level 0: u                    (never a candidate)
 *       level 1: u's direct friends   (never candidates -- already friends)
 *       level 2: friends of friends   (the only candidates)
 *
 *   Each level-2 user w is scored with
 *       mutual_count = |friends(u) ∩ friends(w)|
 *   computed implicitly: w's counter is incremented once for every
 *   level-1 friend whose adjacency list contains w.  The candidate hash
 *   map doubles as the visited set -- the first touch inserts the user,
 *   later touches just increment -- so every 2-hop user is processed
 *   exactly once (no revisits, no recomputation).
 *
 * RANKING: descending mutual_count; ties broken by smaller user id.
 *
 * TOP-N: a bounded min-heap holding at most N candidates (root = current
 * worst of the best N) keeps ranking cheap: every candidate is compared
 * against the root and discarded in O(1) unless it beats it, so scoring
 * all C candidates costs O(C log N) instead of a full O(C log C) sort.
 *
 * COMPLEXITY of one query, over the local neighbourhood only:
 *       time  O(E' + C log N + N log N)
 *       space O(V' + N)
 *   where E' = adjacency entries within 2 hops (E' << E), C = distinct
 *   2-hop candidates and V' = C + degree(u) + 1 users touched (V' << V).
 *   This is the payoff of bounding the BFS: cost tracks the size of the
 *   neighbourhood, not the size of the network.
 */

typedef struct {
    int user_id;        /* recommended user                              */
    int mutual_count;   /* number of friends shared with the query user  */
} Recommendation;

typedef struct {
    int  direct_friends;   /* |level 1| = degree of the query user        */
    long edges_scanned;    /* level-1 -> level-2 adjacency entries read   */
    int  candidates_seen;  /* distinct level-2 users (= candidate map size) */
    long heap_comparisons; /* comparisons done by the bounded top-N heap   */
    int  graph_users;      /* total users in the network (locality demo)  */
    long graph_edges;      /* total edges in the network (locality demo)  */
} RecommendStats;

typedef enum {
    RECOMMEND_OK          =  0,
    RECOMMEND_ERR_NULL    = -1,  /* NULL out-parameter or NULL graph     */
    RECOMMEND_ERR_MEMORY  = -2,  /* allocation failure                   */
    RECOMMEND_ERR_NO_USER = -3,  /* query user id does not exist         */
    RECOMMEND_ERR_BAD_ARG = -4   /* e.g. negative top_n                  */
} RecommendStatus;

/* Buffer sizes for CLI-side result buffers (no magic numbers): the CLI
 * allocates these on the stack, so they stay modest; the engine itself
 * allocates only O(N) heap regardless of these caps. */
#define MAX_TOP_N        64   /* max recommendations rendered per query  */
#define MAX_MUTUAL_PRINT 64   /* max mutual friends listed in one lookup */

const char *recommend_status_str(RecommendStatus st);

/*
 * Rank the best top_n non-friend users reachable within 2 hops of user_id.
 * Results are written to out (capacity out_cap; at most min(top_n, out_cap)
 * entries) in ranked order, and *out_count receives the number written
 * (0 when the neighbourhood yields no candidates).  stats may be NULL;
 * when non-NULL it receives exploration statistics that demonstrate the
 * bounded traversal.  Never traverses beyond 2 hops from user_id.
 */
RecommendStatus recommend_friends(const Graph *g, int user_id, int top_n,
                                  Recommendation *out, int out_cap,
                                  int *out_count, RecommendStats *stats);

/*
 * Intersection of two users' sorted friend lists -- their mutual friends,
 * written ascending into out_ids (capacity out_cap; output is truncated to
 * fit).  *out_count receives the number written.  Runs in
 * O(min(deg(a), deg(b))) via a linear merge of the two sorted lists.
 */
RecommendStatus recommend_mutual_users(const Graph *g, int a, int b,
                                       int *out_ids, int out_cap,
                                       int *out_count);

#endif /* RECOMMEND_H */
