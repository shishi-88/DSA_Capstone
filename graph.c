/*
 * graph.c -- adjacency-list friendship graph with an id->slot hash index.
 *
 * Internal layout: one dense vector of GraphNodes plus a separate
 * open-addressing table that maps a raw user id to a node's position.
 * The two structures are always kept in sync:
 *
 *      nodes[i].user_id  <->  hash table entry (id -> i)
 *
 * On graph_remove_user() the LAST node is swapped into the freed slot, so
 * the removal cost is O(hash removal + deg edges) rather than a shift of
 * the whole node vector.  Friend lists are kept sorted (insertion into the
 * sorted position, O(deg) worst case but friend lists are short) so that:
 *   - membership tests are O(log deg) binary searches,
 *   - mutual-friend intersection is a linear merge (see recommend.c),
 *   - all printed output is deterministic.
 *
 * The hash table uses linear probing at 50% max load -- a deliberate
 * O(1)-average design point: chaining would add a pointer per entry and
 * pointer-chasing cache misses, while higher load factors would degrade
 * lookups that the recommendation engine performs once per explored node.
 */

#include "graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH_EMPTY (-1)     /* slot holds no mapping                */
#define HASH_TOMBSTONE (-2) /* slot vacated after a deletion        */
#define HASH_MIN_CAP 16     /* initial hash table capacity          */
#define LOAD_PERCENT_NUM 1  /* grow when used > capacity * 1/2 ...  */
#define LOAD_PERCENT_DEN 2  /* ... i.e. at 50% load factor          */

struct Graph {
    GraphNode *nodes;      /* dense vector of users                */
    int        user_count;
    int        node_cap;
    long       edge_count; /* each undirected edge counted once    */
    int       *table;      /* id -> node index, or HASH_* sentinel */
    int        table_cap;  /* power of two                         */
    int        table_used; /* occupied or tombstoned slots         */
};

/* --------------------------------------------------------------------- */
/* Internal helpers                                                      */
/* --------------------------------------------------------------------- */

/* Knuth multiplicative hash; the table capacity is a power of two, so the
 * modulo collapses to a cheap mask. */
static unsigned hash_id(int id, int table_cap)
{
    unsigned long h = (unsigned long)(unsigned)id * 2654435761UL;
    return (unsigned)(h & (unsigned)(table_cap - 1));
}

static int probe_slot(const Graph *g, int id)
{
    unsigned mask = (unsigned)g->table_cap - 1u;
    unsigned i    = hash_id(id, g->table_cap);
    for (;;) {
        int slot = g->table[i];
        if (slot == HASH_EMPTY)            return HASH_EMPTY;  /* miss    */
        if (slot >= 0 && g->nodes[slot].user_id == id) return (int)i;
        i = (i + 1u) & mask;               /* linear probing      */
    }
}

static GraphStatus table_grow(Graph *g);

/* Insert mapping id -> index (caller guarantees no duplicate id). */
static GraphStatus table_insert(Graph *g, int id, int index)
{
    unsigned mask = (unsigned)g->table_cap - 1u;
    unsigned i    = hash_id(id, g->table_cap);
    for (;;) {
        if (g->table[i] == HASH_EMPTY || g->table[i] == HASH_TOMBSTONE) {
            g->table[i] = index;
            g->table_used++;
            /* Keep the load factor bounded so probes stay O(1) on average. */
            if (g->table_used * LOAD_PERCENT_DEN >
                g->table_cap * LOAD_PERCENT_NUM)
                return table_grow(g);
            return GRAPH_OK;
        }
        i = (i + 1u) & mask;
    }
}

static GraphStatus table_grow(Graph *g)
{
    int old_cap  = g->table_cap;
    int *old_tbl = g->table;

    int new_cap = g->table_cap * 2;
    int *tbl = malloc((size_t)new_cap * sizeof *tbl);
    if (!tbl) return GRAPH_ERR_MEMORY;
    for (int i = 0; i < new_cap; i++) tbl[i] = HASH_EMPTY;

    g->table      = tbl;
    g->table_cap  = new_cap;
    g->table_used = 0;
    for (int i = 0; i < g->user_count; i++) {
        GraphStatus st = table_insert(g, g->nodes[i].user_id, i);
        if (st != GRAPH_OK) { free(tbl); g->table = old_tbl;
                              g->table_cap = old_cap; return st; }
    }
    free(old_tbl);
    return GRAPH_OK;
}

/* Binary-search membership test on a sorted FriendList: O(log deg) instead
 * of a linear scan; this is the hot-path duplicate check for edge inserts
 * and the O(1)-map-supported exclusion test used during recommendation. */
static bool friend_list_contains(const FriendList *fl, int id)
{
    int lo = 0, hi = fl->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (fl->ids[mid] == id) return true;
        if (fl->ids[mid] < id) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

/* Sorted-position insert into a FriendList (duplicates rejected by caller). */
static GraphStatus friend_list_insert(FriendList *fl, int id)
{
    if (fl->count == fl->capacity) {
        int new_cap = fl->capacity == 0 ? 4 : fl->capacity * 2;
        int *p = realloc(fl->ids, (size_t)new_cap * sizeof *p);
        if (!p) return GRAPH_ERR_MEMORY;
        fl->ids      = p;
        fl->capacity = new_cap;
    }
    int pos = fl->count;
    while (pos > 0 && fl->ids[pos - 1] > id) {   /* shift right to keep order */
        fl->ids[pos] = fl->ids[pos - 1];
        pos--;
    }
    fl->ids[pos] = id;
    fl->count++;
    return GRAPH_OK;
}

/* Remove one id from a sorted FriendList; returns whether it was present. */
static bool friend_list_remove(FriendList *fl, int id)
{
    int lo = 0, hi = fl->count - 1;
    while (lo <= hi) {                            /* binary search */
        int mid = lo + (hi - lo) / 2;
        if (fl->ids[mid] == id) {
            memmove(&fl->ids[mid], &fl->ids[mid + 1],
                    (size_t)(fl->count - mid - 1) * sizeof *fl->ids);
            fl->count--;
            return true;
        }
        if (fl->ids[mid] < id) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

static void friend_list_clear(FriendList *fl)
{
    free(fl->ids);
    fl->ids = NULL; fl->count = 0; fl->capacity = 0;
}

/* --------------------------------------------------------------------- */
/* Lifecycle                                                             */
/* --------------------------------------------------------------------- */

Graph *graph_create(void)
{
    Graph *g = calloc(1, sizeof *g);
    if (!g) return NULL;

    g->table_cap = HASH_MIN_CAP;
    g->table = malloc((size_t)g->table_cap * sizeof *g->table);
    if (!g->table) { free(g); return NULL; }
    for (int i = 0; i < g->table_cap; i++) g->table[i] = HASH_EMPTY;
    return g;
}

void graph_destroy(Graph *g)
{
    if (!g) return;
    for (int i = 0; i < g->user_count; i++) friend_list_clear(&g->nodes[i].friends);
    free(g->nodes);
    free(g->table);
    free(g);
}

/* --------------------------------------------------------------------- */
/* Users                                                                 */
/* --------------------------------------------------------------------- */

GraphStatus graph_add_user(Graph *g, int user_id)
{
    if (!g || user_id < 0) return user_id < 0 ? GRAPH_ERR_INVALID
                                              : GRAPH_ERR_NULL;
    if (probe_slot(g, user_id) != HASH_EMPTY) return GRAPH_ERR_DUP;

    if (g->user_count == g->node_cap) {           /* grow the node vector  */
        int new_cap = g->node_cap == 0 ? HASH_MIN_CAP : g->node_cap * 2;
        GraphNode *p = realloc(g->nodes, (size_t)new_cap * sizeof *p);
        if (!p) return GRAPH_ERR_MEMORY;
        g->nodes    = p;
        g->node_cap = new_cap;
    }
    GraphNode *n  = &g->nodes[g->user_count];
    n->user_id    = user_id;
    n->friends.ids = NULL; n->friends.count = 0; n->friends.capacity = 0;
    g->user_count++;

    return table_insert(g, user_id, g->user_count - 1);
}

GraphStatus graph_remove_user(Graph *g, int user_id)
{
    if (!g) return GRAPH_ERR_NULL;
    int slot = probe_slot(g, user_id);
    if (slot == HASH_EMPTY) return GRAPH_ERR_NO_USER;
    int idx = g->table[slot];

    /* Delete every friendship edge touching this user. */
    FriendList *fl = &g->nodes[idx].friends;
    for (int i = 0; i < fl->count; i++) {
        int slot2 = probe_slot(g, fl->ids[i]);
        if (slot2 != HASH_EMPTY)
            friend_list_remove(&g->nodes[g->table[slot2]].friends, user_id);
        g->edge_count--;
    }
    friend_list_clear(fl);

    /* Swap-and-pop keeps the node vector dense: the last node moves into
     * the hole and its hash entry is repointed at the new position. */
    int last = g->user_count - 1;
    if (idx != last) {
        g->nodes[idx] = g->nodes[last];
        int slot_last = probe_slot(g, g->nodes[idx].user_id);
        if (slot_last != HASH_EMPTY) g->table[slot_last] = idx;
    }
    g->user_count--;
    g->table[slot] = HASH_TOMBSTONE;
    g->table_used--;
    return GRAPH_OK;
}

/* --------------------------------------------------------------------- */
/* Edges                                                                 */
/* --------------------------------------------------------------------- */

GraphStatus graph_add_edge(Graph *g, int a, int b)
{
    if (!g) return GRAPH_ERR_NULL;
    if (a == b) return GRAPH_ERR_SELF;
    if (a < 0 || b < 0) return GRAPH_ERR_INVALID;

    int sa = probe_slot(g, a), sb = probe_slot(g, b);
    if (sa == HASH_EMPTY || sb == HASH_EMPTY) return GRAPH_ERR_NO_USER;
    if (friend_list_contains(&g->nodes[g->table[sa]].friends, b))
        return GRAPH_ERR_DUP;

    GraphStatus st = friend_list_insert(&g->nodes[g->table[sa]].friends, b);
    if (st != GRAPH_OK) return st;
    st = friend_list_insert(&g->nodes[g->table[sb]].friends, a);
    if (st != GRAPH_OK) {   /* roll back the first insert to stay consistent */
        friend_list_remove(&g->nodes[g->table[sa]].friends, b);
        return st;
    }
    g->edge_count++;
    return GRAPH_OK;
}

GraphStatus graph_remove_edge(Graph *g, int a, int b)
{
    if (!g) return GRAPH_ERR_NULL;
    if (a < 0 || b < 0) return GRAPH_ERR_INVALID;
    int sa = probe_slot(g, a), sb = probe_slot(g, b);
    if (sa == HASH_EMPTY || sb == HASH_EMPTY) return GRAPH_ERR_NO_USER;

    bool removed = friend_list_remove(&g->nodes[g->table[sa]].friends, b) &&
                   friend_list_remove(&g->nodes[g->table[sb]].friends, a);
    if (!removed) return GRAPH_ERR_NO_USER;   /* edge did not exist */
    g->edge_count--;
    return GRAPH_OK;
}

/* --------------------------------------------------------------------- */
/* Queries                                                               */
/* --------------------------------------------------------------------- */

bool graph_has_user(const Graph *g, int user_id)
{
    return g && user_id >= 0 && probe_slot(g, user_id) != HASH_EMPTY;
}

bool graph_has_edge(const Graph *g, int a, int b)
{
    if (!g || a < 0 || b < 0) return false;
    int sa = probe_slot(g, a);
    return sa != HASH_EMPTY &&
           friend_list_contains(&g->nodes[g->table[sa]].friends, b);
}

int graph_user_count(const Graph *g)      { return g ? g->user_count : 0; }

long graph_edge_count(const Graph *g)     { return g ? g->edge_count : 0L; }

int graph_degree(const Graph *g, int user_id)
{
    if (!g || user_id < 0) return -1;
    int slot = probe_slot(g, user_id);
    return slot == HASH_EMPTY ? -1 : g->nodes[g->table[slot]].friends.count;
}

const GraphNode *graph_get_node(const Graph *g, int user_id)
{
    if (!g || user_id < 0) return NULL;
    int slot = probe_slot(g, user_id);
    return slot == HASH_EMPTY ? NULL : &g->nodes[g->table[slot]];
}

const GraphNode *graph_node_at(const Graph *g, int i)
{
    if (!g || i < 0 || i >= g->user_count) return NULL;
    return &g->nodes[i];
}

const char *graph_status_str(GraphStatus st)
{
    switch (st) {
    case GRAPH_OK:          return "ok";
    case GRAPH_ERR_NULL:    return "null argument";
    case GRAPH_ERR_MEMORY:  return "out of memory";
    case GRAPH_ERR_NO_USER: return "unknown user id";
    case GRAPH_ERR_DUP:     return "duplicate user or friendship";
    case GRAPH_ERR_SELF:    return "a user cannot befriend themselves";
    case GRAPH_ERR_INVALID: return "invalid user id (must be >= 0)";
    }
    return "unknown error";
}
