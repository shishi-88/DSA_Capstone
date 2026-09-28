#ifndef GRAPH_H
#define GRAPH_H

#include <stdbool.h>

/*
 * graph.h -- undirected friendship graph (Phase 1: data-structure layer)
 *
 * PHASE 1 RATIONALE (why these structures were chosen):
 *
 *  1. Adjacency LIST, not an adjacency MATRIX.  Real social graphs are
 *     sparse: a user has tens/hundreds of friends out of millions of
 *     users, so a matrix wastes O(V^2) space while a list needs only
 *     O(V + E).  Listing a user's friends also costs O(deg) instead of
 *     scanning a full O(V) matrix row.
 *
 *  2. Hash index (user id -> slot).  User IDs are arbitrary integers, so
 *     direct array indexing by id is impossible and binary search over
 *     the user table would cost O(log V) per lookup.  An open-addressing
 *     hash table with linear probing gives O(1) average lookup/insert,
 *     which the recommendation engine hits once per explored node.
 *
 *  3. Per-user friend lists are kept SORTED.  Membership tests become
 *     O(log deg) binary searches (used to exclude direct friends from
 *     recommendations), mutual-friend intersection is a linear merge,
 *     and all printed output is deterministic.
 *
 * Every function that can fail returns a GraphStatus; negative values are
 * errors.  The Graph handle itself is opaque (defined in graph.c) so that
 * callers cannot bypass the invariants maintained here.
 */

typedef enum {
    GRAPH_OK          =  0,  /* success                                    */
    GRAPH_ERR_NULL    = -1,  /* NULL argument                              */
    GRAPH_ERR_MEMORY  = -2,  /* allocation failure                         */
    GRAPH_ERR_NO_USER = -3,  /* a referenced user id does not exist        */
    GRAPH_ERR_DUP     = -4,  /* duplicate user or friendship edge          */
    GRAPH_ERR_SELF    = -5,  /* a user tried to befriend themselves        */
    GRAPH_ERR_INVALID = -6   /* malformed argument (e.g. negative user id) */
} GraphStatus;

/* Sorted list of friend user ids -- the adjacency list of one vertex. */
typedef struct FriendList {
    int *ids;        /* ascending order; this order is an API invariant */
    int  count;
    int  capacity;
} FriendList;

/* One user: its id plus its adjacency list. */
typedef struct GraphNode {
    int        user_id;
    FriendList friends;
} GraphNode;

typedef struct Graph Graph;   /* opaque; internals live in graph.c */

/* ---- lifecycle ------------------------------------------------------- */
Graph *graph_create(void);
void   graph_destroy(Graph *g);

/* ---- mutation -------------------------------------------------------- */
GraphStatus graph_add_user(Graph *g, int user_id);
GraphStatus graph_remove_user(Graph *g, int user_id);
GraphStatus graph_add_edge(Graph *g, int a, int b);   /* both users must exist */
GraphStatus graph_remove_edge(Graph *g, int a, int b);

/* ---- queries --------------------------------------------------------- */
bool graph_has_user(const Graph *g, int user_id);
bool graph_has_edge(const Graph *g, int a, int b);
int  graph_user_count(const Graph *g);
long graph_edge_count(const Graph *g);
int  graph_degree(const Graph *g, int user_id);       /* -1 if user absent     */

/* O(1)-average lookup of a user's node (NULL if the user does not exist). */
const GraphNode *graph_get_node(const Graph *g, int user_id);

/* Positional access for iteration/admin dumps: i in [0, graph_user_count). */
const GraphNode *graph_node_at(const Graph *g, int i);

/* Human-readable name for a status code (used by the CLI error paths). */
const char *graph_status_str(GraphStatus st);

#endif /* GRAPH_H */
