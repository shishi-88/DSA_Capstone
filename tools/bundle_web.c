/*
 * tools/bundle_web.c -- build web/index.standalone.html (Phase 3 preview tool)
 *
 * Reads web/index.html and inlines style.css + app.js into a single
 * self-contained HTML file.  Purpose: sandboxed preview panes that serve
 * only ONE file (no sibling requests) can still show the real frontend.
 *
 * Usage:  bundle_web [index.html] [out.html]
 *         defaults: web/index.html  ->  web/index.standalone.html
 * Re-run after editing any web/ file (also: `make bundle`).
 * Exit code 0 = success, 1 = I/O or allocation failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_CHUNK 65536

static char *read_whole_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = READ_CHUNK, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    for (;;) {
        if (len + READ_CHUNK + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(fp); return NULL; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, READ_CHUNK, fp);
        len += n;
        if (n < READ_CHUNK) break;          /* EOF or error */
    }
    fclose(fp);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* Replace every "</" with "<\/" so "</script>" inside inlined JS can
 * never terminate the tag early.  "\/" is a legal (pass-through) escape
 * in both string literals and template literals, and a "</" sequence
 * never occurs in plain code, so this is safe everywhere. */
static char *js_escape(const char *s, size_t *out_len)
{
    size_t n_slash = 0;
    for (const char *p = s; p[0]; p++) if (p[0] == '<' && p[1] == '/') n_slash++;
    size_t len = strlen(s);
    char *out = malloc(len + n_slash + 1);
    if (!out) return NULL;
    char *w = out;
    for (const char *p = s; *p; p++) {
        if (p[0] == '<' && p[1] == '/') { *w++ = '<'; *w++ = '\\'; *w++ = '/'; p++; }
        else *w++ = *p;
    }
    *w = '\0';
    *out_len = (size_t)(w - out);
    return out;
}

/* Return heap copy of s with needle replaced by repl (NULL if OOM). */
static char *str_replace_all(const char *s, const char *needle, const char *repl)
{
    size_t nlen = strlen(needle), rlen = strlen(repl);
    size_t count = 0;
    const char *p;
    for (p = s; (p = strstr(p, needle)) != NULL; p += nlen) count++;
    char *out = malloc(strlen(s) + count * (rlen > nlen ? rlen - nlen : 0) + 1);
    if (!out) return NULL;
    char *w = out;
    const char *scan = s;
    while ((p = strstr(scan, needle)) != NULL) {
        memcpy(w, scan, (size_t)(p - scan)); w += p - scan;
        memcpy(w, repl, rlen);               w += rlen;
        scan = p + nlen;
    }
    strcpy(w, scan);
    return out;
}

int main(int argc, char **argv)
{
    const char *in_path  = argc > 1 ? argv[1] : "web/index.html";
    const char *out_path = argc > 2 ? argv[2] : "web/index.standalone.html";

    size_t html_len = 0;
    char *html = read_whole_file(in_path, &html_len);
    if (!html) { fprintf(stderr, "error: cannot read %s\n", in_path); return 1; }

    size_t css_len = 0, js_len = 0;
    char *css = read_whole_file("web/style.css", &css_len);
    char *js  = read_whole_file("web/app.js", &js_len);
    if (!css || !js) {
        fprintf(stderr, "error: cannot read web/style.css or web/app.js\n");
        free(html); free(css); free(js);
        return 1;
    }

    /* 1) inline the stylesheet */
    char *tag = malloc(css_len + 64);
    if (!tag) { free(html); free(css); free(js); return 1; }
    sprintf(tag, "<style>\n%s\n</style>", css);
    char *step1 = str_replace_all(html,
        "<link rel=\"stylesheet\" href=\"style.css\" />", tag);
    free(tag);
    if (!step1) { free(html); free(css); free(js); return 1; }

    /* 2) inline the script, escaping '<' so "</script>" inside the JS
     *    can never close the tag early */
    char *esc = js_escape(js, &js_len);
    char *stag = malloc(js_len + 64);
    if (!esc || !stag) { free(html); free(css); free(js); free(esc); free(stag); free(step1); return 1; }
    sprintf(stag, "<script>\n%s\n</script>", esc);
    char *step2 = str_replace_all(step1,
        "<script src=\"app.js\"></script>", stag);
    free(stag);
    free(esc);
    free(step1);
    if (!step2) { free(html); free(css); free(js); return 1; }

    /* 3) write the bundle */
    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "error: cannot write %s\n", out_path);
                free(step2); free(html); free(css); free(js); return 1; }
    fputs(step2, out);
    fclose(out);
    printf("wrote %s (%zu bytes) from %s + style.css + app.js\n",
           out_path, strlen(step2), in_path);

    free(step2); free(html); free(css); free(js);
    return 0;
}
