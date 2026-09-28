/*
 * server.c -- Phase 3 HTTP interface layer (web frontend backend)
 *
 * A deliberately minimal, single-threaded HTTP server for localhost that
 * exposes the ALREADY-IMPLEMENTED Phase 1/2 backend:
 *
 *      graph.c      -> adjacency-list friendship graph   (untouched)
 *      recommend.c  -> bounded 2-hop BFS recommender     (untouched)
 *
 * ARCHITECTURE (matches the project's layering: main.c is one frontend,
 * this file is another -- neither contains algorithm code):
 *
 *      Browser  --HTTP-->  server.c  --function calls-->  graph.c / recommend.c
 *
 * ENDPOINTS (all responses are JSON, all carry CORS headers):
 *      GET /api/users              -> {"users":[{"id":..},..]}
 *      GET /api/friends?user=ID    -> {"user":ID,"friends":[ids...]}
 *      GET /api/recommend?user=ID&top=N
 *                                  -> {"user":ID,"top":N,
 *                                      "recommendations":[{"user_id":..,
 *                                       "mutual_count":..},..],
 *                                      "stats":{...}}
 *      GET /api/network-summary    -> {"users":V,"friendships":E}
 *
 * IMPORTANT (source of truth): /api/recommend calls recommend_friends()
 * directly.  There is NO ranking or BFS logic in this file -- it only
 * serialises what recommend.c produced.  The stats block (edges scanned,
 * candidates seen, graph size) is reported verbatim by the engine and
 * demonstrates the bounded 2-hop traversal to graders.
 *
 * Static files: GET / and files under web/ are served from disk (whitelisted
 * extensions, ".." rejected) so the frontend in web/ can be served by the
 * same process during the demo.
 *
 * Robustness: malformed requests, unknown paths, non-GET methods, missing
 * or non-numeric parameters, and unknown user ids all produce 4xx JSON
 * errors; the accept loop never crashes and keeps serving.
 *
 * Build (see Makefile):
 *      gcc -std=c99 -Wall -Wextra -Werror -O2 -o friend_server \
 *          server.c graph.c recommend.c [-lws2_32 on Windows]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include "graph.h"
#include "recommend.h"

/* ------------------------------------------------------------------ */
/* Platform sockets: Winsock on Windows, BSD sockets elsewhere.        */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define CLOSE_SOCKET(s)  closesocket(s)
#  define SOCK_INVALID(s)  ((s) == INVALID_SOCKET)
   typedef int  socklen_t_alias;           /* Winsock socklen_t stand-in */
#else
#  include <unistd.h>
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netinet/in.h>
#  define CLOSE_SOCKET(s)  close(s)
#  define SOCK_INVALID(s)  ((s) < 0)
   typedef socklen_t socklen_t_alias;
#endif

#define SERVER_PORT        8080
#define DEFAULT_DATASET    "data/sample_network.txt"
#define WEB_ROOT           "web"

#define REQ_BUF_SIZE       8192    /* request-line buffer; requests beyond
                                      this are truncated (still parsed)   */
#define RESP_CHUNK         4096
#define MAX_PATH_LEN       256
#define DEFAULT_TOP_N      5       /* when ?top= is absent                */
#define SERVER_NAME        "friend_server/1.0 (DSA capstone, C99)"

/* ------------------------------------------------------------------ */
/* Display metadata (NOT part of the graph model).                     */
/* The friendship graph is deliberately id-only (that IS the DSA       */
/* object); human names are presentation labels served by this API     */
/* layer so the browser needs no built-in user data.  Users absent     */
/* from the table get a generic label.                                 */
/* ------------------------------------------------------------------ */
typedef struct { int id; const char *name; const char *group; } UserMeta;

static const UserMeta USER_META[] = {
    { 0,  "Aarav Sharma",    "College"   },
    { 1,  "Diya Patel",      "College"   },
    { 2,  "Rohan Mehta",     "College"   },
    { 3,  "Ishaan Verma",    "College"   },
    { 4,  "Ananya Rao",      "College"   },
    { 5,  "Kabir Singh",     "Work"      },
    { 6,  "Meera Iyer",      "Work"      },
    { 7,  "Arjun Nair",      "Work"      },
    { 8,  "Sanya Kapoor",    "Work"      },
    { 9,  "Vivaan Joshi",    "Work"      },
    { 10, "Aditya Kulkarni", "Bridge"    },
    { 11, "Nisha Menon",     "Bridge"    },
    { 12, "Rahul Desai",     "Family"    },
    { 13, "Priya Bhatt",     "Family"    },
    { 14, "Karan Shetty",    "Family"    },
    { 15, "Tara Gupta",      "Leaf"      },
    { 16, "Nikhil Chandra",  "Leaf"      },
    { 17, "Riya Saxena",     "Leaf"      },
    { 18, "Dev Malhotra",    "Leaf"      },
    { 19, "Lone Wolf",       "Isolated"  }
};

static const UserMeta *meta_for(int id)
{
    size_t i;
    for (i = 0; i < sizeof USER_META / sizeof USER_META[0]; i++)
        if (USER_META[i].id == id) return &USER_META[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Tiny dynamic string used to assemble JSON bodies.                   */
/* ------------------------------------------------------------------ */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool  oom;          /* sticky: once set, appends are ignored */
} StrBuf;

static void sb_init(StrBuf *sb)
{
    sb->cap = RESP_CHUNK;
    sb->len = 0;
    sb->oom = false;
    sb->data = malloc(sb->cap);
    if (sb->data) sb->data[0] = '\0';
    else          sb->oom = true;
}

static void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static void sb_append(StrBuf *sb, const char *s)
{
    size_t need;
    if (sb->oom || !s) return;
    need = sb->len + strlen(s) + 1;
    if (need > sb->cap) {
        size_t nc = sb->cap;
        char  *nd;
        while (nc < need) nc *= 2;
        nd = realloc(sb->data, nc);
        if (!nd) { sb->oom = true; return; }
        sb->data = nd;
        sb->cap  = nc;
    }
    memcpy(sb->data + sb->len, s, strlen(s) + 1);
    sb->len += strlen(s);
}

static void sb_append_int(StrBuf *sb, long v)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%ld", v);
    sb_append(sb, tmp);
}

/* JSON string escape (quotes, backslash, control chars). */
static void sb_append_json_str(StrBuf *sb, const char *s)
{
    sb_append(sb, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        char buf[8];
        switch (c) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\b': sb_append(sb, "\\b");  break;
        case '\f': sb_append(sb, "\\f");  break;
        case '\n': sb_append(sb, "\\n");  break;
        case '\r': sb_append(sb, "\\r");  break;
        case '\t': sb_append(sb, "\\t");  break;
        default:
            if (c < 0x20) {
                snprintf(buf, sizeof buf, "\\u%04x", c);
                sb_append(sb, buf);
            } else {
                buf[0] = (char)c;
                buf[1] = '\0';
                sb_append(sb, buf);
            }
        }
    }
    sb_append(sb, "\"");
}

/* ------------------------------------------------------------------ */
/* Socket plumbing                                                     */
/* ------------------------------------------------------------------ */
static void sockets_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "error: WSAStartup failed\n");
        exit(EXIT_FAILURE);
    }
#endif
}

static void sockets_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

/* ------------------------------------------------------------------ */
/* Dataset loading -- same "u <id>" / "e <a> <b>" format as main.c.     */
/* (Data loading only; no algorithm logic lives here.)                 */
/* ------------------------------------------------------------------ */
typedef struct {
    int  users_loaded;
    int  edges_loaded;
    int  bad_lines;
} LoadStats;

#define DATASET_LINE_MAX 512

static bool load_network(Graph *g, const char *path, LoadStats *ls)
{
    FILE *fp = fopen(path, "r");
    char line[DATASET_LINE_MAX];

    memset(ls, 0, sizeof *ls);
    if (!fp) {
        fprintf(stderr, "error: cannot open dataset '%s'\n", path);
        return false;
    }

    while (fgets(line, sizeof line, fp)) {
        char cmd;
        int  a, b, consumed = 0;

        /* Skip blank lines and '#' comments. */
        if (sscanf(line, " %c", &cmd) != 1 || cmd == '#')
            continue;

        if (cmd == 'u' && sscanf(line + 1, " %d%n", &a, &consumed) == 1) {
            (void)consumed;
            if (graph_add_user(g, a) == GRAPH_OK) ls->users_loaded++;
            else ls->bad_lines++;
        } else if (cmd == 'e' &&
                   sscanf(line + 1, " %d %d", &a, &b) == 2) {
            if (graph_add_edge(g, a, b) == GRAPH_OK) ls->edges_loaded++;
            else ls->bad_lines++;
        } else {
            ls->bad_lines++;
        }
    }
    fclose(fp);
    return true;
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */
static void send_all(SOCKET fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, (int)(len - off), 0);
        if (n <= 0) return;          /* client gone: drop the response */
        off += (size_t)n;
    }
}

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static void respond(SOCKET fd, int code, const char *body)
{
    char header[512];
    StrBuf msg;

    snprintf(header, sizeof header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"           /* CORS: the web UI
                                                          may run from file://
                                                          or another port */
        "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: %zu\r\n"
        "Server: " SERVER_NAME "\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status_text(code), strlen(body));

    sb_init(&msg);
    sb_append(&msg, header);
    sb_append(&msg, body);
    if (!msg.oom) send_all(fd, msg.data, msg.len);
    sb_free(&msg);
}

/* Body must already be a complete JSON document. */
static void respond_error(SOCKET fd, int code, const char *message)
{
    StrBuf b;
    sb_init(&b);
    sb_append(&b, "{\"error\":");
    sb_append_json_str(&b, message);
    sb_append(&b, "}");
    if (!b.oom) respond(fd, code, b.data);
    else        respond(fd, 500, "{\"error\":\"out of memory\"}");
    sb_free(&b);
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                     */
/* ------------------------------------------------------------------ */

/* Read from the socket until the header terminator or buffer is full.
 * Returns bytes read (>=1) or <= 0 on error/disconnect. */
static int read_request(SOCKET fd, char *buf, int cap)
{
    int total = 0;
    while (total < cap - 1) {
        int n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) return n <= 0 ? -1 : -1;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;   /* end of headers */
    }
    buf[total] = '\0';
    return total;
}

/* Extract "?key=value" from a query string; *out gets the value. */
static bool query_param(const char *query, const char *key,
                        char *out, size_t out_cap)
{
    size_t klen = strlen(key);
    const char *p = query;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t i = 0;
            while (v[i] && v[i] != '&' && i + 1 < out_cap) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* Strict non-negative integer parse (whole string must be digits). */
static bool parse_uint(const char *s, long *out)
{
    char *end;
    long v;

    if (!s || !*s) return false;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0) return false;
    *out = v;
    return true;
}

/* ------------------------------------------------------------------ */
/* Endpoint handlers -- thin JSON adapters over graph.c/recommend.c    */
/* ------------------------------------------------------------------ */

/* GET /api/users */
static void handle_users(SOCKET fd, const Graph *g)
{
    StrBuf b;
    int    i, n = graph_user_count(g);

    sb_init(&b);
    sb_append(&b, "{\"users\":[");
    for (i = 0; i < n; i++) {
        const GraphNode *node = graph_node_at(g, i);
        const UserMeta  *m    = meta_for(node->user_id);
        if (i) sb_append(&b, ",");
        sb_append(&b, "{\"id\":");
        sb_append_int(&b, node->user_id);
        sb_append(&b, ",\"name\":");
        sb_append_json_str(&b, m ? m->name : "User");
        sb_append(&b, ",\"group\":");
        sb_append_json_str(&b, m ? m->group : "");
        sb_append(&b, "}");
    }
    sb_append(&b, "]}");
    if (!b.oom) respond(fd, 200, b.data);
    else        respond_error(fd, 500, "out of memory");
    sb_free(&b);
}

/* GET /api/network-summary */
static void handle_summary(SOCKET fd, const Graph *g)
{
    StrBuf b;
    sb_init(&b);
    sb_append(&b, "{\"users\":");
    sb_append_int(&b, graph_user_count(g));
    sb_append(&b, ",\"friendships\":");
    sb_append_int(&b, graph_edge_count(g));
    sb_append(&b, "}");
    if (!b.oom) respond(fd, 200, b.data);
    else        respond_error(fd, 500, "out of memory");
    sb_free(&b);
}

/* GET /api/friends?user=ID */
static void handle_friends(SOCKET fd, const Graph *g, const char *query)
{
    char            uid_s[32];
    long            uid;
    const GraphNode *node;
    StrBuf          b;
    int             i;

    if (!query_param(query, "user", uid_s, sizeof uid_s)) {
        respond_error(fd, 400, "missing required parameter 'user'");
        return;
    }
    if (!parse_uint(uid_s, &uid)) {
        respond_error(fd, 400, "parameter 'user' must be a non-negative integer");
        return;
    }
    node = graph_get_node(g, (int)uid);
    if (!node) {
        respond_error(fd, 404, "unknown user id");
        return;
    }

    sb_init(&b);
    sb_append(&b, "{\"user\":");
    sb_append_int(&b, uid);
    sb_append(&b, ",\"friends\":[");
    for (i = 0; i < node->friends.count; i++) {
        if (i) sb_append(&b, ",");
        sb_append_int(&b, node->friends.ids[i]);
    }
    sb_append(&b, "]}");
    if (!b.oom) respond(fd, 200, b.data);
    else        respond_error(fd, 500, "out of memory");
    sb_free(&b);
}

/* GET /api/recommend?user=ID&top=N
 *
 * The ranked results below are produced ENTIRELY by recommend_friends()
 * (bounded 2-hop BFS + mutual counting + top-N heap in recommend.c).
 * This handler only validates parameters and serialises the output. */
static void handle_recommend(SOCKET fd, const Graph *g, const char *query)
{
    char            uid_s[32], top_s[32];
    long            uid, top;
    Recommendation  recs[MAX_TOP_N];
    RecommendStats  stats;
    int             count = 0, i;
    RecommendStatus st;
    StrBuf          b;

    if (!query_param(query, "user", uid_s, sizeof uid_s)) {
        respond_error(fd, 400, "missing required parameter 'user'");
        return;
    }
    if (!parse_uint(uid_s, &uid)) {
        respond_error(fd, 400, "parameter 'user' must be a non-negative integer");
        return;
    }
    if (!query_param(query, "top", top_s, sizeof top_s)) {
        top = DEFAULT_TOP_N;                      /* sensible demo default */
    } else if (!parse_uint(top_s, &top)) {
        respond_error(fd, 400, "parameter 'top' must be a non-negative integer");
        return;
    }
    if (top > MAX_TOP_N) top = MAX_TOP_N;         /* engine buffer cap */

    st = recommend_friends(g, (int)uid, (int)top,
                           recs, MAX_TOP_N, &count, &stats);
    switch (st) {
    case RECOMMEND_OK:
        break;
    case RECOMMEND_ERR_NO_USER:
        respond_error(fd, 404, "unknown user id");
        return;
    case RECOMMEND_ERR_BAD_ARG:
        respond_error(fd, 400, "invalid recommendation arguments");
        return;
    case RECOMMEND_ERR_MEMORY:
        respond_error(fd, 500, "engine out of memory");
        return;
    default:
        respond_error(fd, 500, "engine error");
        return;
    }

    sb_init(&b);
    sb_append(&b, "{\"user\":");
    sb_append_int(&b, uid);
    sb_append(&b, ",\"top\":");
    sb_append_int(&b, top);
    sb_append(&b, ",\"recommendations\":[");
    for (i = 0; i < count; i++) {
        if (i) sb_append(&b, ",");
        sb_append(&b, "{\"user_id\":");
        sb_append_int(&b, recs[i].user_id);
        sb_append(&b, ",\"mutual_count\":");
        sb_append_int(&b, recs[i].mutual_count);
        sb_append(&b, "}");
    }
    /* Engine-reported traversal stats: proof of the 2-hop bound. */
    sb_append(&b, "],\"stats\":{\"direct_friends\":");
    sb_append_int(&b, stats.direct_friends);
    sb_append(&b, ",\"edges_scanned\":");
    sb_append_int(&b, stats.edges_scanned);
    sb_append(&b, ",\"candidates_seen\":");
    sb_append_int(&b, stats.candidates_seen);
    sb_append(&b, ",\"heap_comparisons\":");
    sb_append_int(&b, stats.heap_comparisons);
    sb_append(&b, ",\"graph_users\":");
    sb_append_int(&b, stats.graph_users);
    sb_append(&b, ",\"graph_edges\":");
    sb_append_int(&b, stats.graph_edges);
    sb_append(&b, "}}");
    if (!b.oom) respond(fd, 200, b.data);
    else        respond_error(fd, 500, "out of memory");
    sb_free(&b);
}

/* ------------------------------------------------------------------ */
/* Static files (serves the web/ frontend from the same process)       */
/* ------------------------------------------------------------------ */
static const char *content_type_for(const char *path)
{
    size_t n = strlen(path);
    if (n >= 5 && strcmp(path + n - 5, ".html") == 0) return "text/html; charset=utf-8";
    if (n >= 4 && strcmp(path + n - 4, ".css")  == 0) return "text/css; charset=utf-8";
    if (n >= 3 && strcmp(path + n - 3, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (n >= 4 && strcmp(path + n - 4, ".json") == 0) return "application/json; charset=utf-8";
    if (n >= 4 && strcmp(path + n - 4, ".txt")  == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void handle_static(SOCKET fd, const char *path_in)
{
    char full[MAX_PATH_LEN];
    FILE *fp;
    char  header[512];
    long  size;
    const char *ctype;

    if (strstr(path_in, "..")) {                    /* path traversal guard */
        respond_error(fd, 400, "invalid path");
        return;
    }
    if (strcmp(path_in, "/") == 0 || strcmp(path_in, "") == 0)
        path_in = "/index.html";

    if (snprintf(full, sizeof full, "%s%s", WEB_ROOT, path_in)
        >= (int)sizeof full) {
        respond_error(fd, 400, "path too long");
        return;
    }

    fp = fopen(full, "rb");
    if (!fp) {
        respond_error(fd, 404, "file not found");
        return;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) { fclose(fp); respond_error(fd, 500, "file read error"); return; }

    ctype = content_type_for(full);
    snprintf(header, sizeof header,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Server: " SERVER_NAME "\r\n"
        "Connection: close\r\n"
        "\r\n",
        ctype, size);
    send_all(fd, header, strlen(header));

    {
        char chunk[RESP_CHUNK];
        size_t got;
        while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0)
            send_all(fd, chunk, got);
    }
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* Per-connection dispatch                                             */
/* ------------------------------------------------------------------ */
static void handle_client(SOCKET fd, Graph *g)
{
    char    req[REQ_BUF_SIZE];
    char    method[8], target[MAX_PATH_LEN * 2], path[MAX_PATH_LEN * 2];
    char   *query = NULL;
    char   *qmark;

    if (read_request(fd, req, sizeof req) <= 0)
        return;

    if (sscanf(req, "%7s %255s", method, target) != 2) {
        respond_error(fd, 400, "malformed request line");
        return;
    }

    if (strcmp(method, "OPTIONS") == 0) {          /* CORS preflight */
        respond(fd, 200, "{\"ok\":true}");
        return;
    }
    if (strcmp(method, "GET") != 0) {
        respond_error(fd, 405, "only GET is supported");
        return;
    }

    /* Split path and query string. */
    snprintf(path, sizeof path, "%s", target);
    qmark = strchr(path, '?');
    if (qmark) { *qmark = '\0'; query = qmark + 1; }

    if (strcmp(path, "/api/users") == 0) {
        handle_users(fd, g);
    } else if (strcmp(path, "/api/network-summary") == 0) {
        handle_summary(fd, g);
    } else if (strcmp(path, "/api/friends") == 0) {
        handle_friends(fd, g, query ? query : "");
    } else if (strcmp(path, "/api/recommend") == 0) {
        handle_recommend(fd, g, query ? query : "");
    } else if (strncmp(path, "/api/", 5) == 0) {
        respond_error(fd, 404, "unknown API endpoint");
    } else {
        handle_static(fd, path);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    Graph         *g;
    LoadStats      ls;
    SOCKET         listener;
    struct sockaddr_in addr;
    int            opt = 1;
    long           requests = 0;

    sockets_init();

    g = graph_create();
    if (!g) {
        fprintf(stderr, "error: cannot create graph\n");
        sockets_cleanup();
        return EXIT_FAILURE;
    }
    if (load_network(g, DEFAULT_DATASET, &ls))
        printf("Loaded %d users and %d friendships (%d bad lines) from %s\n",
               ls.users_loaded, ls.edges_loaded, ls.bad_lines, DEFAULT_DATASET);
    else
        fprintf(stderr, "warning: starting with an empty graph\n");

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (SOCK_INVALID(listener)) {
        fprintf(stderr, "error: socket() failed\n");
        graph_destroy(g);
        sockets_cleanup();
        return EXIT_FAILURE;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */

    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "error: cannot bind 127.0.0.1:%d (%s)\n",
                SERVER_PORT, strerror(errno));
        CLOSE_SOCKET(listener);
        graph_destroy(g);
        sockets_cleanup();
        return EXIT_FAILURE;
    }
    if (listen(listener, 8) != 0) {
        fprintf(stderr, "error: listen() failed\n");
        CLOSE_SOCKET(listener);
        graph_destroy(g);
        sockets_cleanup();
        return EXIT_FAILURE;
    }

    printf("FriendFinder HTTP server listening on http://localhost:%d\n",
           SERVER_PORT);
    printf("  API:   /api/users  /api/friends?user=ID  "
           "/api/recommend?user=ID&top=N  /api/network-summary\n");
    printf("  Files: http://localhost:%d/  (serves %s/)\n", SERVER_PORT, WEB_ROOT);

    for (;;) {                                       /* one client at a time:
                                                        graph.c is not
                                                        thread-safe and the
                                                        capstone load is tiny */
        SOCKET   conn;
        socklen_t_alias clen = sizeof addr;

        conn = accept(listener, (struct sockaddr *)&addr, &clen);
        if (SOCK_INVALID(conn)) continue;            /* transient error: keep
                                                        serving; never crash */

        requests++;
        handle_client(conn, g);
        CLOSE_SOCKET(conn);
        (void)requests;                              /* logged per-session */
    }

    /* Unreachable, but kept for symmetry and future graceful shutdown. */
    CLOSE_SOCKET(listener);
    graph_destroy(g);                                /* no leaks */
    sockets_cleanup();
    return EXIT_SUCCESS;
}
