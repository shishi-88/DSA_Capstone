/*
 * recommend.c -- bounded 2-hop BFS recommendation engine (Phase 2 core).
 *
 * The engine NEVER looks at the whole graph.  It expands exactly two BFS
 * levels from the query user and touches nothing else:
 *
 *      level 0: the query user u            (never a candidate)
 *      level 1: u's direct friends          (excluded -- already friends)
 *      level 2: friends of u's friends      (the only candidates)
 *
 * Scoring trick that avoids recomputation: for each level-1 friend f we
 * walk f's adjacency list ONCE and bump a counter for every neighbour w.
 * When the walk is done, counter[w] == |friends(u) ∩ friends(w)| exactly --
 * the mutual-friend count -- without ever building the two lists and
 * intersecting them pairwise.  w is counted once per shared friend, so a
 * candidate added by several friends accumulates its full count
 * automatically (this is why direct friends must be skipped, not stopped
 * at: we need f's OTHER neighbours, but f itself is not a candidate).
 *
 * The candidate hash map doubles as the visited set.  A separate visited
 * array would be redundant state; first touch = insert, later touch =
 * increment, so every 2-hop user is processed exactly once.  Direct
 * friends are blocked in the same map with the sentinel value
 * BLOCKED_MUTUALS, which also makes the is-direct-friend test O(1).
 *
 * Ranking uses a bounded min-heap of at most top_n entries (root = worst
 * of the current best N).  Every candidate is compared against the root:
 * either it cannot beat the root (O(1) discard) or it replaces it and
 * sifts down (O(log N)).  Total ranking cost is O(C log N) with C
 * candidates, versus O(C log C) for sorting every candidate -- and the
 * heap never grows with C, so its memory footprint is O(N) regardless of
 * how busy the neighbourhood is.
 *
 * Determinism: ties in mutual_count are broken by the SMALLER user id
 * winning, so identical inputs always produce identical output.
 */

#include "recommend.h"

#include <stdlib.h>
#include <string.h>

/* Sentinel stored in the candidate map for users that must never be
 * recommended (the query user and its direct friends).  Any real count is
 * >= 0, so this value is unambiguous. */
#define BLOCKED_MUTUALS (-1)
#define CANDIDATE_MAP_MIN_CAP 16

/* --------------------------------------------------------------------- */
/* Candidate map: open-addressing hash of user id -> mutual-friend count  */
/* --------------------------------------------------------------------- */

typedef struct {
    int *keys;    /* user id    */
    int *vals;    /* mutual count, or BLOCKED_MUTUALS */
    char *used;   /* slot flags: 0 empty, 1 occupied (no deletions happen) */
    int   cap;    /* power of two */
    int   count;
} CandidateMap;

static unsigned cand_hash(int id, int cap)
{
    unsigned long h = (unsigned long)(unsigned)id * 2654435761UL;
    return (unsigned)(h & (unsigned)(cap - 1));
}

static bool cand_map_init(CandidateMap *m, int cap)
{
    memset(m, 0, sizeof *m);
    m->cap = cap;
    m->keys  = malloc((size_t)cap * sizeof *m->keys);
    m->vals  = malloc((size_t)cap * sizeof *m->vals);
    m->used  = calloc((size_t)cap, 1);
    return m->keys && m->vals && m->used;
}

static void cand_map_free(CandidateMap *m)
{
    free(m->keys); free(m->vals); free(m->used);
}

static bool cand_map_grow(CandidateMap *m)
{
    CandidateMap bigger;
    if (!cand_map_init(&bigger, m->cap * 2)) return false;
    for (int i = 0; i < m->cap; i++) {
        if (!m->used[i]) continue;
        unsigned j = cand_hash(m->keys[i], bigger.cap);
        while (bigger.used[j]) j = (j + 1u) & (unsigned)(bigger.cap - 1);
        bigger.used[j] = 1;
        bigger.keys[j] = m->keys[i];
        bigger.vals[j] = m->vals[i];
        bigger.count++;
    }
    cand_map_free(m);
    *m = bigger;
    return true;
}

/* Get-or-insert: returns the slot of id.  *created tells the caller whether
 * this was the FIRST touch (insert) -- the visited-set semantics live here. */
static int cand_map_slot(CandidateMap *m, int id, bool *created,
                         bool *grew_ok)
{
    unsigned mask = (unsigned)m->cap - 1u;
    unsigned i    = cand_hash(id, m->cap);
    while (m->used[i]) {
        if (m->keys[i] == id) { *created = false; return (int)i; }
        i = (i + 1u) & mask;
    }
    *created = true;
    m->used[i] = 1;
    m->keys[i] = id;
    m->vals[i] = 0;
    m->count++;
    if (m->count * 2 > m->cap && !cand_map_grow(m)) {
        /* Grow failed: keep going at higher load (correct, just slower). */
        *grew_ok = false;
    }
    /* After a grow the slot index is stale; re-probe in the new table. */
    i = cand_hash(id, m->cap);
    while (m->keys[i] != id) i = (i + 1u) & (unsigned)(m->cap - 1);
    return (int)i;
}

/* --------------------------------------------------------------------- */
/* Bounded min-heap over (mutual_count DESC, user_id ASC) priority         */
/* --------------------------------------------------------------------- */

typedef struct {
    Recommendation *items;
    int size;
    int cap;
    long comparisons;   /* instrumentation for the stats report */
} TopHeap;

/* true when a has HIGHER priority than b: more mutuals first, and on a tie
 * the smaller user id ranks first (deterministic tie-break, as specified). */
static bool beats(const Recommendation *a, const Recommendation *b)
{
    if (a->mutual_count != b->mutual_count) return a->mutual_count > b->mutual_count;
    return a->user_id < b->user_id;
}

static bool heap_init(TopHeap *h, int cap)
{
    if (cap < 1) cap = 1;
    h->items = malloc((size_t)cap * sizeof *h->items);
    h->size = 0;
    h->cap = cap;
    h->comparisons = 0;
    return h->items != NULL;
}

static void heap_free(TopHeap *h) { free(h->items); }

/* Sift the root down after a replacement.  This heap is a MIN-heap on
 * ranking priority: the invariant is that every CHILD beats its PARENT,
 * so the root is always the WORST of the stored best-N and can be tested
 * against newcomers in O(1).  Sift-down therefore moves the WORST of
 * {node, children} upward (note the comparison direction vs. a max-heap). */
static void heap_sift_down(TopHeap *h, int start)
{
    int i = start;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, worst = i;
        if (l < h->size) { h->comparisons++;
            if (beats(&h->items[worst], &h->items[l])) worst = l; }
        if (r < h->size) { h->comparisons++;
            if (beats(&h->items[worst], &h->items[r])) worst = r; }
        if (worst == i) break;
        Recommendation tmp = h->items[i]; h->items[i] = h->items[worst];
        h->items[worst] = tmp;
        i = worst;
    }
}

/* Offer a candidate: O(1) reject when the heap is full and the candidate
 * cannot beat the root (the current worst of the best N). */
static void heap_offer(TopHeap *h, int user_id, int mutual_count)
{
    Recommendation cand = { user_id, mutual_count };
    if (h->size < h->cap) {
        int i = h->size++;
        h->items[i] = cand;
        while (i > 0) {                          /* sift up (min-heap) */
            int parent = (i - 1) / 2;
            h->comparisons++;
            if (beats(&h->items[i], &h->items[parent]))
                break;   /* child already beats parent: invariant holds */
            /* Parent beats the child -> pull the WORSE element toward the
             * root so the root stays the worst of the best-N. */
            Recommendation tmp = h->items[i];
            h->items[i] = h->items[parent];
            h->items[parent] = tmp;
            i = parent;
        }
        return;
    }
    h->comparisons++;
    if (!beats(&cand, &h->items[0])) return;     /* not good enough: discard */
    h->items[0] = cand;                          /* replace the worst */
    heap_sift_down(h, 0);
}

static int cmp_rec_desc(const void *pa, const void *pb)
{
    const Recommendation *a = pa, *b = pb;
    if (a->mutual_count != b->mutual_count) return b->mutual_count - a->mutual_count;
    return a->user_id - b->user_id;
}

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

const char *recommend_status_str(RecommendStatus st)
{
    switch (st) {
    case RECOMMEND_OK:          return "ok";
    case RECOMMEND_ERR_NULL:    return "null argument";
    case RECOMMEND_ERR_MEMORY:  return "out of memory";
    case RECOMMEND_ERR_NO_USER: return "unknown user id";
    case RECOMMEND_ERR_BAD_ARG: return "invalid argument";
    }
    return "unknown error";
}

RecommendStatus recommend_friends(const Graph *g, int user_id, int top_n,
                                  Recommendation *out, int out_cap,
                                  int *out_count, RecommendStats *stats)
{
    if (out_count) *out_count = 0;
    if (stats) memset(stats, 0, sizeof *stats);

    if (!g || !out_count) return RECOMMEND_ERR_NULL;
    if (top_n < 0) return RECOMMEND_ERR_BAD_ARG;
    if (top_n == 0 || out_cap <= 0 || !out) {   /* nothing requested: still
                                                   run the stats traversal   */
        if (!out && (top_n > 0 && out_cap > 0)) return RECOMMEND_ERR_NULL;
    }
    const GraphNode *u = graph_get_node(g, user_id);
    if (!u) return RECOMMEND_ERR_NO_USER;
    if (stats) {
        stats->graph_users = graph_user_count(g);
        stats->graph_edges = graph_edge_count(g);
    }

    CandidateMap map;
    if (!cand_map_init(&map, CANDIDATE_MAP_MIN_CAP)) return RECOMMEND_ERR_MEMORY;
    bool grew_ok = true;

    /* ---- Level 0 + level 1: block the query user and direct friends. ---
     * Blocking in the SAME map that later receives candidates gives O(1)
     * "is this user already a friend?" checks during the level-2 walk and
     * merges the visited set with the exclusion set -- one structure, one
     * probe per user. */
    {
        bool created = false;
        int s = cand_map_slot(&map, user_id, &created, &grew_ok);
        map.vals[s] = BLOCKED_MUTUALS;
    }
    for (int i = 0; i < u->friends.count; i++) {
        bool created = false;
        int s = cand_map_slot(&map, u->friends.ids[i], &created, &grew_ok);
        map.vals[s] = BLOCKED_MUTUALS;
    }
    if (stats) stats->direct_friends = u->friends.count;

    /* ---- Level 2: one adjacency walk per direct friend. -----------------
     * This loop is the entire "BFS": each level-1 friend f contributes
     * +1 mutual friend to every neighbour w that is neither the query user
     * nor already a direct friend.  No full-graph scan happens anywhere. */
    for (int i = 0; i < u->friends.count; i++) {
        const GraphNode *f = graph_get_node(g, u->friends.ids[i]);
        if (!f) continue;                        /* defensive; cannot happen */
        for (int j = 0; j < f->friends.count; j++) {
            int w = f->friends.ids[j];
            if (stats) stats->edges_scanned++;
            bool created = false;
            int s = cand_map_slot(&map, w, &created, &grew_ok);
            if (map.vals[s] == BLOCKED_MUTUALS) continue;  /* u or direct friend */
            if (created && stats) stats->candidates_seen++; /* first visit = new candidate */
            map.vals[s]++;                       /* one more shared friend */
        }
    }

    /* ---- Rank into the bounded heap. ------------------------------------ */
    /* Zero-initialised so that every field is defined even on the rare
     * paths where heap_init never runs (also silences maybe-uninitialized
     * under -Werror); heap_ok gates all real use below. */
    TopHeap heap = { NULL, 0, 0, 0 };
    int want = top_n < out_cap ? top_n : out_cap;
    bool heap_ok = want > 0 && heap_init(&heap, want);
    if (want > 0 && !heap_ok) { cand_map_free(&map); return RECOMMEND_ERR_MEMORY; }

    for (int i = 0; i < map.cap; i++) {
        if (!map.used[i]) continue;
        if (map.vals[i] <= 0) continue;          /* BLOCKED or zero mutuals */
        if (heap_ok) heap_offer(&heap, map.keys[i], map.vals[i]);
    }

    int n = 0;
    if (heap_ok) {
        n = heap.size;
        if (n > out_cap) n = out_cap;
        /* Copy the heap's survivors, then sort the (at most N) winners. */
        memcpy(out, heap.items, (size_t)heap.size * sizeof *out);
        if (heap.size > 1) qsort(out, (size_t)heap.size, sizeof *out, cmp_rec_desc);
        if (stats) stats->heap_comparisons = heap.comparisons;
        heap_free(&heap);
    }
    *out_count = n;
    cand_map_free(&map);
    return RECOMMEND_OK;
}

RecommendStatus recommend_mutual_users(const Graph *g, int a, int b,
                                       int *out_ids, int out_cap,
                                       int *out_count)
{
    if (out_count) *out_count = 0;
    if (!g || !out_count) return RECOMMEND_ERR_NULL;
    if (out_cap < 0 || (!out_ids && out_cap > 0)) return RECOMMEND_ERR_BAD_ARG;

    const GraphNode *na = graph_get_node(g, a);
    const GraphNode *nb = graph_get_node(g, b);
    if (!na || !nb) return RECOMMEND_ERR_NO_USER;

    /* Linear merge of two ASCENDING lists: O(min(deg(a), deg(b))) instead
     * of the O(deg(a) * deg(b)) nested-loop intersection a naive version
     * would use. */
    int i = 0, j = 0, n = 0;
    while (i < na->friends.count && j < nb->friends.count && n < out_cap) {
        if (na->friends.ids[i] == nb->friends.ids[j]) {
            if (out_ids) out_ids[n] = na->friends.ids[i];
            n++; i++; j++;
        } else if (na->friends.ids[i] < nb->friends.ids[j]) {
            i++;
        } else {
            j++;
        }
    }
    *out_count = n;
    return RECOMMEND_OK;
}
