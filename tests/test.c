/* test.c - dcjson test suite (built with ASan + UBSan by `make test`).
 *
 *   build/tests [DCJSON-BINARY]
 *
 * Unit tests for every module, randomised property tests (chunked parsing
 * equals one-shot parsing, row navigation equals a naive walk, paths and
 * printers round-trip), renderer tests through a small VT emulator
 * (including the scroll-region optimisation and escape-sequence safety),
 * and end-to-end runs of the binary when its path is given. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "cli.h"
#include "clip.h"
#include "fmt.h"
#include "input.h"
#include "json.h"
#include "path.h"
#include "render.h"
#include "search.h"
#include "terminal.h"
#include "utf8.h"
#include "util.h"
#include "view.h"

static int g_fail, g_checks;

#define CHECK(c, ...)                                         \
    do {                                                      \
        g_checks++;                                           \
        if (!(c)) {                                           \
            g_fail++;                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                     \
            fputc('\n', stderr);                              \
        }                                                     \
    } while (0)

static uint64_t rs = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

/* ---- helpers ---------------------------------------------------------- */

static int parse_all(jdoc_t *d, const char *s, size_t n, int lines, int lenient)
{
    json_init(d, lines, lenient, 10000, 65535);
    json_input(d, s, n, 1);
    while (!json_parse(d, UINT64_MAX)) {
    }
    return d->failed ? -1 : 0;
}

static int valid(const char *s, int lenient)
{
    jdoc_t d;
    int r = parse_all(&d, s, strlen(s), 0, lenient);
    json_free(&d);
    return r == 0;
}

static void mem_put(sink_t *s, const char *p, size_t n) { sb_put(s->ctx, p, n); }

static char *fmt_str(const jdoc_t *d, uint32_t i, int style)
{
    static sbuf_t b;
    b.len = 0;
    sink_t s = {mem_put, 0, &b};
    fmt_value(d, i, style, 2, &s);
    sb_put(&b, "", 0);
    return b.p ? b.p : (char *)"";
}

/* ---- random JSON ------------------------------------------------------ */

static void gen_str(sbuf_t *b)
{
    static const char *const P[] = {"a", "b", "key", " ", ".", "[", "\\\"", "\\\\", "\\n", "\\u00e4",
                                    "\\ud83d\\ude00", "ä", "東", "x y", "0", "_", "$", "\\/"};
    sb_str(b, "\"");
    unsigned n = rn(6);
    for (unsigned i = 0; i < n; i++)
        sb_str(b, P[rn(ARRAY_LEN(P))]);
    sb_str(b, "\"");
}

static void ws(sbuf_t *b)
{
    static const char *const W[] = {"", "", "", " ", "\n", "\t", "\r\n  "};
    sb_str(b, W[rn(ARRAY_LEN(W))]);
}

static void gen_value(sbuf_t *b, int depth)
{
    unsigned t = depth > 5 ? rn(5) : rn(8);
    char tmp[64];
    switch (t) {
    case 0: sb_str(b, "null"); break;
    case 1: sb_str(b, rn(2) ? "true" : "false"); break;
    case 2:
        snprintf(tmp, sizeof tmp, "%s%u%s%s", rn(2) ? "-" : "", rn(100000), rn(2) ? ".5" : "", rn(3) ? "" : "e-7");
        sb_str(b, tmp);
        break;
    case 3: case 4: gen_str(b); break;
    case 5: case 6: {
        int obj = t == 6;
        sb_str(b, obj ? "{" : "[");
        unsigned n = rn(5);
        for (unsigned i = 0; i < n; i++) {
            if (i)
                sb_str(b, ",");
            ws(b);
            if (obj) {
                gen_str(b);
                ws(b);
                sb_str(b, ":");
                ws(b);
            }
            gen_value(b, depth + 1);
            ws(b);
        }
        sb_str(b, obj ? "}" : "]");
        break;
    }
    default: {
        sb_str(b, "{");
        unsigned n = rn(4) + 1;
        for (unsigned i = 0; i < n; i++) {
            if (i)
                sb_str(b, ", ");
            gen_str(b);
            sb_str(b, ": ");
            gen_value(b, depth + 1);
        }
        sb_str(b, "}");
        break;
    }
    }
}

/* ---- parser ----------------------------------------------------------- */

static void test_parser(void)
{
    static const char *const ok[] = {
        "0", "-0", "1.5e10", "-12.25E-3", "1E+2", "\"\"", "\"a\\u00e4\\n\"", "[]", "{}", "[1,[2,[3]]]",
        "{\"a\":{\"b\":[true,false,null]}}", " \t\r\n[ 1 , 2 ] \n", "\xef\xbb\xbf{}", "\"\xe6\x9d\xb1\"",
        "[\"\\ud83d\\ude00\"]", "{\"\":0}", "1e5", "[-1]", "\"\\/\""};
    static const char *const bad[] = {
        "", "  ", "[", "[1,]", "{\"a\":1,}", "01", "1.", ".5", "-", "+1", "1e", "1e+", "[1 2]",
        "{\"a\" 1}", "{1:2}", "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "tru", "nul", "falsey",
        "[}", "{]", "1 2", "[1]]", "{\"a\":}", "[,1]", "//c\n1", "NaN", "'a'", "\"\\u00g0\"", "[1,,2]"};
    for (size_t i = 0; i < ARRAY_LEN(ok); i++)
        CHECK(valid(ok[i], 0), "should parse: %s", ok[i]);
    for (size_t i = 0; i < ARRAY_LEN(bad); i++)
        CHECK(!valid(bad[i], 0), "should fail: %s", bad[i]);
    /* lenient */
    CHECK(valid("[1,2,]", 1), "lenient trailing comma");
    CHECK(valid("{\"a\":1,}", 1), "lenient trailing comma in object");
    CHECK(valid("// c\n{/* x */\"a\":1 // y\n}", 1), "lenient comments");
    CHECK(valid("\"a\tb\"", 1), "lenient control character");
    CHECK(!valid("[1,,2]", 1), "lenient still rejects double comma");
    CHECK(!valid("/* x", 1), "unterminated comment");

    /* node table */
    jdoc_t d;
    const char *s = "{\"a\": [1, \"x\"], \"b\": {}, \"c\": null}";
    CHECK(parse_all(&d, s, strlen(s), 0, 0) == 0, "parse");
    CHECK(d.count == 6, "count %u", d.count);
    CHECK(d.node[0].type == JT_OBJ && d.node[0].n == 3 && d.node[0].end == 6, "root");
    CHECK(d.node[1].type == JT_ARR && d.node[1].n == 2 && d.node[1].end == 4 && d.node[1].parent == 0, "a");
    CHECK(d.node[3].type == JT_STR && json_span(&d, 3) == 3, "x span");
    CHECK(json_span(&d, 0) == strlen(s), "root span");
    CHECK(json_span(&d, 1) == 8, "array span %llu", (unsigned long long)json_span(&d, 1));
    uint64_t ko, kl;
    CHECK(json_key(&d, 4, &ko, &kl) && kl == 1 && s[ko] == 'b', "key b");
    CHECK(!json_key(&d, 2, &ko, &kl), "array element has no key");
    CHECK(json_node_at(&d, 9) == 2, "node at '1'");
    CHECK(json_node_at(&d, 17) == 4, "node at key b: %u", json_node_at(&d, 17));
    json_free(&d);

    /* errors: position */
    s = "{\n  \"a\": [1,\n  2,,]\n}";
    CHECK(parse_all(&d, s, strlen(s), 0, 0) != 0, "error");
    uint64_t l, c;
    json_linecol(&d, d.err_pos, &l, &c);
    CHECK(l == 3 && c == 5, "error at %llu:%llu", (unsigned long long)l, (unsigned long long)c);
    CHECK(d.done && d.node[0].end == d.count && (d.node[0].flags & JF_CUT), "cut containers closed");
    json_free(&d);

    /* several values */
    s = "{\"a\":1}\n{\"a\":2}\n3";
    CHECK(parse_all(&d, s, strlen(s), 1, 0) == 0 && d.nroots == 3, "lines");
    json_free(&d);
    CHECK(parse_all(&d, s, strlen(s), 0, 0) != 0, "two values without --lines");
    json_free(&d);

    /* depth limit */
    char deep[300];
    memset(deep, '[', 150);
    memset(deep + 150, ']', 150);
    json_init(&d, 0, 0, 100, 1);
    json_input(&d, deep, 300, 1);
    while (!json_parse(&d, UINT64_MAX)) {
    }
    CHECK(d.failed && strstr(d.err, "deeper"), "depth limit: %s", d.err);
    json_free(&d);

    /* unescape */
    char out[64];
    const char *e = "a\\n\\u00e4\\ud83d\\ude00\\udc00\\\"x";
    size_t n = json_unescape(e, strlen(e), out);
    CHECK(n == 1 + 1 + 2 + 4 + 3 + 1 + 1 && !memcmp(out, "a\n\xc3\xa4\xf0\x9f\x98\x80\xef\xbf\xbd\"x", n),
          "unescape");
}

/* Feed the input in pieces with small budgets; the result must be the same
 * node table (or the same error) as parsing it in one go. */
static int same_parse(const char *s, size_t n, int lines, int lenient, unsigned piece, uint64_t budget)
{
    jdoc_t a, b;
    parse_all(&a, s, n, lines, lenient);
    json_init(&b, lines, lenient, 10000, 65535);
    size_t have = 0;
    int guard = 0;
    while (!b.done && guard++ < 10000000) {
        if (b.waiting || have == 0) {
            have = have + piece < n ? have + piece : n;
            json_input(&b, s, have, have == n);
        }
        json_parse(&b, budget);
    }
    int same = a.count == b.count && a.failed == b.failed && a.nroots == b.nroots &&
               (!a.failed || (a.err_pos == b.err_pos && !strcmp(a.err, b.err)));
    for (uint32_t i = 0; same && i < a.count; i++) {
        jnode_t *x = &a.node[i], *y = &b.node[i];
        same = x->off == y->off && x->len == y->len && x->end == y->end && x->parent == y->parent &&
               x->n == y->n && x->kd == y->kd && x->depth == y->depth && x->type == y->type &&
               x->flags == y->flags;
        if (!same)
            fprintf(stderr, "  node %u differs\n", i);
    }
    if (!same)
        fprintf(stderr, "  a: count %u failed %d (%s @%llu)  b: count %u failed %d (%s @%llu)\n", a.count,
                a.failed, a.err, (unsigned long long)a.err_pos, b.count, b.failed, b.err,
                (unsigned long long)b.err_pos);
    json_free(&a);
    json_free(&b);
    return same;
}

static void test_chunked(void)
{
    static const char *const docs[] = {
        "{\"a\": [1, 2.5e3, \"x\\u00e4\\\"y\"], \"b\": {\"c\": null, \"d\": true}, \"e\": false}",
        "[1, 2", "{\"a\":\"unterminated", "  123  ", "\xef\xbb\xbf[\"bom\"]", "[1,]",
        "{\"k\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\n\\\\\"}",
        "{\"a\":1}\n{\"b\":[2]}\n\n7\n\"s\""};
    for (size_t i = 0; i < ARRAY_LEN(docs); i++)
        for (unsigned piece = 1; piece < 9; piece++)
            CHECK(same_parse(docs[i], strlen(docs[i]), i == 7, 0, piece, 1 + piece % 3),
                  "chunked parse of doc %zu, piece %u", i, piece);
    CHECK(same_parse("// c\n[1, /* x */ 2,]", 20, 0, 1, 1, 1), "chunked lenient");

    /* random documents and random mutations of them */
    sbuf_t b = {0};
    for (int it = 0; it < 3000; it++) {
        b.len = 0;
        int lines = it % 7 == 0;
        int nroots = lines ? (int)rn(4) + 1 : 1;
        for (int r = 0; r < nroots; r++) {
            gen_value(&b, 0);
            sb_str(&b, "\n");
        }
        if (it % 3 == 0 && b.len) { /* mutate */
            unsigned k = rn(4) + 1;
            for (unsigned j = 0; j < k; j++) {
                size_t pos = rn((unsigned)b.len);
                static const char M[] = "{}[],:\"\\0aE-.\n\x01\xc3";
                b.p[pos] = M[rn(sizeof M - 1)];
            }
        }
        unsigned piece = rn(16) + 1;
        if (!same_parse(b.p, b.len, lines, it % 5 == 0, piece, rn(8) + 1)) {
            CHECK(0, "random chunked parse %d (piece %u): %.200s", it, piece, b.p);
            break;
        }
        g_checks++;
    }
    sb_free(&b);
}

/* ---- view ------------------------------------------------------------- */

static row_t *g_naive;
static size_t g_nn;

static void naive(const jdoc_t *d, uint32_t i, int closers)
{
    g_naive[g_nn++] = ROW(i, 0);
    const jnode_t *n = &d->node[i];
    if (!view_expanded(d, i))
        return;
    for (uint32_t c = i + 1; c < n->end; c = d->node[c].end)
        naive(d, c, closers);
    if (closers)
        g_naive[g_nn++] = ROW(i, 1);
}

static void test_view(void)
{
    sbuf_t b = {0};
    for (int it = 0; it < 400; it++) {
        b.len = 0;
        int nroots = (int)rn(3) + 1;
        for (int r = 0; r < nroots; r++)
            gen_value(&b, 0);
        jdoc_t d;
        /* roots separated by nothing would merge numbers: use newlines */
        b.len = 0;
        for (int r = 0; r < nroots; r++) {
            gen_value(&b, 0);
            sb_str(&b, "\n");
        }
        if (parse_all(&d, b.p, b.len, 1, 0) != 0) {
            CHECK(0, "random doc should parse");
            json_free(&d);
            continue;
        }
        for (uint32_t i = 0; i < d.count; i++)
            if (rn(3) == 0)
                d.node[i].flags &= (uint8_t)~JF_OPEN;
        for (int closers = 0; closers < 2; closers++) {
            view_t v = {&d, closers};
            g_naive = malloc(sizeof *g_naive * (2 * (size_t)d.count + 2));
            g_nn = 0;
            for (uint32_t r = 0; r < d.count; r = d.node[r].end)
                naive(&d, r, closers);
            size_t k = 0;
            row_t r = view_first(&v), prev = ROW_NONE;
            int ok = 1;
            for (; r != ROW_NONE && k < g_nn; r = view_next(&v, r), k++) {
                if (r != g_naive[k]) {
                    ok = 0;
                    break;
                }
                if (view_prev(&v, r) != prev)
                    ok = 0;
                if (prev != ROW_NONE && view_cmp(&v, prev, r) >= 0)
                    ok = 0;
                if (view_fix(&v, r) != r)
                    ok = 0;
                prev = r;
            }
            CHECK(ok && r == ROW_NONE && k == g_nn, "navigation equals naive walk (doc %d closers %d)", it, closers);
            CHECK(view_last(&v) == g_naive[g_nn - 1], "view_last");
            long moved;
            CHECK(view_step(&v, view_first(&v), (long)g_nn + 5, &moved) == g_naive[g_nn - 1] &&
                      moved == (long)g_nn - 1, "view_step clamps");
            /* every node maps to a visible row */
            for (uint32_t i = 0; i < d.count; i++) {
                row_t f = view_fix(&v, ROW(i, 0));
                int found = 0;
                for (size_t j = 0; j < g_nn && !found; j++)
                    found = g_naive[j] == f;
                if (!found) {
                    CHECK(0, "view_fix gives a visible row");
                    break;
                }
            }
            free(g_naive);
        }
        json_free(&d);
    }
    /* expand helpers */
    jdoc_t d;
    const char *s = "{\"a\":{\"b\":{\"c\":[1]}},\"d\":[2]}";
    parse_all(&d, s, strlen(s), 0, 0);
    view_set_depth(&d, 1);
    CHECK((d.node[0].flags & JF_OPEN) && !(d.node[1].flags & JF_OPEN), "set_depth 1");
    view_reveal(&d, 4);
    CHECK((d.node[1].flags & JF_OPEN) && (d.node[2].flags & JF_OPEN) && (d.node[3].flags & JF_OPEN), "reveal");
    view_subtree(&d, 0, 0);
    CHECK(!(d.node[0].flags & JF_OPEN) && !(d.node[3].flags & JF_OPEN), "collapse subtree");
    CHECK(view_next_sibling(&d, 1) == 5 && view_prev_sibling(&d, 5) == 1 && view_next_sibling(&d, 5) == JN_NONE,
          "siblings");
    json_free(&d);
    sb_free(&b);
}

/* rows of a document that is still being parsed */
static void test_view_partial(void)
{
    const char *s = "[{\"a\":1},{\"b\":[1,2,3]},4]";
    jdoc_t d;
    json_init(&d, 0, 0, 100, 65535);
    json_input(&d, s, 17, 0); /* up to inside the second object's array */
    while (!d.waiting)
        json_parse(&d, UINT64_MAX);
    view_t v = {&d, 1};
    int n = 0;
    row_t last = ROW_NONE;
    for (row_t r = view_first(&v); r != ROW_NONE; r = view_next(&v, r))
        last = r, n++;
    CHECK(last == view_last(&v), "partial: last row %llx vs %llx", (unsigned long long)last,
          (unsigned long long)view_last(&v));
    CHECK(n == 7, "partial rows %d", n); /* [ { a } { [b: 1 2 */
    json_input(&d, s, strlen(s), 1);
    while (!json_parse(&d, UINT64_MAX)) {
    }
    n = 0;
    for (row_t r = view_first(&v); r != ROW_NONE; r = view_next(&v, r))
        n++;
    CHECK(n == 13, "complete rows %d", n);
    json_free(&d);
}

/* ---- paths and printers ----------------------------------------------- */

static void test_paths(void)
{
    jdoc_t d;
    const char *s = "{\"users\":[{\"name\":\"a\",\"odd key\":[1,{\"x.y\":true}]},{\"\\u00e4\":2}],"
                    "\"$v\":null,\"_ok1\":3}";
    parse_all(&d, s, strlen(s), 0, 0);
    size_t n;
    char err[160];
    uint32_t i;
    CHECK(!strcmp(path_build(&d, 0, PATH_JQ, &n), "."), "root path");
    CHECK(!strcmp(path_build(&d, 3, PATH_JQ, &n), ".users[0].name"), "jq: %s", path_build(&d, 3, PATH_JQ, &n));
    CHECK(!strcmp(path_build(&d, 7, PATH_JQ, &n), ".users[0][\"odd key\"][1][\"x.y\"]"), "jq: %s",
          path_build(&d, 7, PATH_JQ, &n));
    CHECK(!strcmp(path_build(&d, 3, PATH_JS, &n), "users[0].name"), "js: %s", path_build(&d, 3, PATH_JS, &n));
    CHECK(!strcmp(path_build(&d, 10, PATH_JS, &n), "$v"), "js $: %s", path_build(&d, 10, PATH_JS, &n));
    CHECK(!strcmp(path_build(&d, 10, PATH_JQ, &n), ".[\"$v\"]"), "jq $: %s", path_build(&d, 10, PATH_JQ, &n));
    CHECK(path_find(&d, ".users[1].ä", &i, err, sizeof err) == 0 && i == 9, "find decoded key: %s", err);
    CHECK(path_find(&d, "users[-1]", &i, err, sizeof err) == 0 && i == 8, "negative index");
    CHECK(path_find(&d, ".users[0].\"odd key\"[1]", &i, err, sizeof err) == 0 && i == 6, "quoted key");
    CHECK(path_find(&d, ".users[5]", &i, err, sizeof err) < 0 && strstr(err, "range"), "out of range");
    CHECK(path_find(&d, ".nope", &i, err, sizeof err) < 0, "missing key");
    CHECK(path_find(&d, ".users.name", &i, err, sizeof err) < 0, "key on an array");
    /* round trip for every node */
    for (uint32_t k = 0; k < d.count; k++)
        for (int st = 0; st < 2; st++) {
            char p[256];
            snprintf(p, sizeof p, "%s", path_build(&d, k, st, &n));
            if (!p[0])
                snprintf(p, sizeof p, ".");
            CHECK(path_find(&d, p, &i, err, sizeof err) == 0 && i == k, "round trip %u '%s': %s", k, p, err);
        }
    json_free(&d);

    /* several roots */
    s = "{\"a\":1}\n{\"a\":2}";
    parse_all(&d, s, strlen(s), 1, 0);
    CHECK(path_find(&d, "#1.a", &i, err, sizeof err) == 0 && i == 3, "#1.a");
    CHECK(path_index(&d, 2) == 1, "root index");
    json_free(&d);

    /* printers: pretty and minified output parse to the same thing */
    sbuf_t b = {0};
    for (int it = 0; it < 500; it++) {
        b.len = 0;
        gen_value(&b, 0);
        jdoc_t x, y, z;
        if (parse_all(&x, b.p, b.len, 0, 0) != 0) {
            json_free(&x);
            continue;
        }
        char *pretty = strdup(fmt_str(&x, 0, FMT_PRETTY));
        char *mini = strdup(fmt_str(&x, 0, FMT_MINIFY));
        int okp = parse_all(&y, pretty, strlen(pretty), 0, 0) == 0;
        int okm = parse_all(&z, mini, strlen(mini), 0, 0) == 0;
        CHECK(okp && okm && y.count == x.count && z.count == x.count, "printers reparse: %s", b.p);
        CHECK(!strcmp(fmt_str(&y, 0, FMT_MINIFY), mini), "minify(pretty) == minify");
        CHECK(!strchr(mini, ' ') || strstr(b.p, " ") , "minified has no spaces outside strings");
        free(pretty);
        free(mini);
        json_free(&x);
        json_free(&y);
        json_free(&z);
    }
    sb_free(&b);

    s = "{\"a\": [1,2], \"k\\u00e4\": \"x\\ny\", \"e\": {}}";
    parse_all(&d, s, strlen(s), 0, 0);
    CHECK(!strcmp(fmt_str(&d, 0, FMT_PRETTY), "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"k\\u00e4\": \"x\\ny\",\n  \"e\": {}\n}"),
          "pretty: %s", fmt_str(&d, 0, FMT_PRETTY));
    CHECK(!strcmp(fmt_str(&d, 0, FMT_MINIFY), "{\"a\":[1,2],\"k\\u00e4\":\"x\\ny\",\"e\":{}}"), "minify");
    CHECK(!strcmp(fmt_str(&d, 1, FMT_RAW), "[1,2]"), "raw");
    CHECK(!strcmp(fmt_str(&d, 4, FMT_TEXT), "x\ny"), "text");
    CHECK(!strcmp(fmt_str(&d, 4, FMT_KEY), "k\xc3\xa4"), "key");
    json_free(&d);

    /* a container cut short by an error is printed closed */
    s = "{\"a\": [1, 2";
    parse_all(&d, s, strlen(s), 0, 0);
    CHECK(!strcmp(fmt_str(&d, 0, FMT_MINIFY), "{\"a\":[1,2]}"), "cut: %s", fmt_str(&d, 0, FMT_MINIFY));
    CHECK(!strcmp(fmt_str(&d, 0, FMT_RAW), "{\n  \"a\": [\n    1,\n    2\n  ]\n}"), "cut raw");
    json_free(&d);
}

/* ---- search, utf8, input ---------------------------------------------- */

static uint64_t naive_find(const char *b, uint64_t from, uint64_t to, uint64_t size, const char *q, int icase)
{
    size_t n = strlen(q);
    for (uint64_t p = from; p < to && p + n <= size; p++)
        if (icase ? !strncasecmp(b + p, q, n) : !strncmp(b + p, q, n))
            return p;
    return SQ_NONE;
}

static void test_search(void)
{
    char buf[2000];
    for (int it = 0; it < 300; it++) {
        size_t n = rn(sizeof buf - 1) + 1;
        for (size_t i = 0; i < n; i++)
            buf[i] = "abAB c"[rn(6)];
        char q[4] = {0};
        size_t ql = rn(3) + 1;
        for (size_t i = 0; i < ql; i++)
            q[i] = "abAB"[rn(it % 2 ? 2 : 4)];
        squery_t s;
        sq_set(&s, q, ql);
        uint64_t from = rn((unsigned)n), to = from + rn((unsigned)(n - from) + 1);
        uint64_t a = sq_find(&s, (const unsigned char *)buf, from, to, n);
        uint64_t b = naive_find(buf, from, to, n, q, s.icase);
        CHECK(a == b, "find '%s' icase %d: %llu vs %llu", q, s.icase, (unsigned long long)a, (unsigned long long)b);
        uint64_t last = SQ_NONE;
        for (uint64_t p = from; p < to; p++) {
            uint64_t m = naive_find(buf, p, to, n, q, s.icase);
            if (m == SQ_NONE)
                break;
            last = m;
            p = m;
        }
        CHECK(sq_rfind(&s, (const unsigned char *)buf, from, to, n) == last, "rfind");
    }
    squery_t s;
    sq_set(&s, "Ab", 2);
    CHECK(!s.icase, "smart case: upper case makes it exact");
    sq_set(&s, "ab", 2);
    CHECK(s.icase, "smart case: lower case ignores case");
}

static void test_utf8_input(void)
{
    uint32_t cp;
    CHECK(utf8_decode((const unsigned char *)"\xc3\xa4", 2, &cp) == 2 && cp == 0xe4, "decode ä");
    CHECK(utf8_decode((const unsigned char *)"\xc0\x80", 2, &cp) == 0, "overlong");
    CHECK(utf8_decode((const unsigned char *)"\xed\xa0\x80", 3, &cp) == 0, "surrogate");
    CHECK(utf8_decode((const unsigned char *)"\xf4\x90\x80\x80", 4, &cp) == 0, "above U+10FFFF");
    CHECK(utf8_decode((const unsigned char *)"\xe6\x9d", 2, &cp) == 0, "truncated");
    CHECK(utf8_width(0x6771) == 2 && utf8_width(0x1f600) == 2 && utf8_width(0x301) == 0 && utf8_width('a') == 1,
          "widths");
    CHECK(utf8_width(0x1b) < 0 && utf8_width(0x9b) < 0 && utf8_width(0x202e) < 0 && utf8_width(0x7f) < 0,
          "unsafe code points");
    char o[4];
    CHECK(utf8_encode(0x1f600, o) == 4 && !memcmp(o, "\xf0\x9f\x98\x80", 4), "encode");

    kev_t e;
    CHECK(input_decode((const unsigned char *)"\x1b[A", 3, 0, &e) == 3 && e.key == KEY_UP, "up");
    CHECK(input_decode((const unsigned char *)"\x1b[1;5C", 6, 0, &e) == 6 && e.key == KEY_RIGHT && e.mod == MOD_CTRL,
          "ctrl-right");
    CHECK(input_decode((const unsigned char *)"\x1b[6~", 4, 0, &e) == 4 && e.key == KEY_PGDN, "pgdn");
    CHECK(input_decode((const unsigned char *)"\x1bOH", 3, 0, &e) == 3 && e.key == KEY_HOME, "ss3 home");
    CHECK(input_decode((const unsigned char *)"\x1b", 1, 0, &e) == 0, "lone esc waits");
    CHECK(input_decode((const unsigned char *)"\x1b", 1, 1, &e) == 1 && e.key == KEY_ESC, "lone esc flushed");
    CHECK(input_decode((const unsigned char *)"\x1b[", 2, 0, &e) == 0, "partial csi waits");
    CHECK(input_decode((const unsigned char *)"\x1b[<0;12;5M", 10, 0, &e) == 10 && e.key == KEY_MOUSE && e.mb == 0 &&
              e.mx == 11 && e.my == 4 && !e.mrelease, "mouse press");
    CHECK(input_decode((const unsigned char *)"\x1b[<65;1;1M", 10, 0, &e) == 10 && e.mb == 65, "wheel down");
    CHECK(input_decode((const unsigned char *)"\x1b[200~", 6, 0, &e) == 6 && e.key == KEY_PASTE_BEGIN, "paste");
    CHECK(input_decode((const unsigned char *)"\xc3\xa4", 2, 0, &e) == 2 && e.key == 0xe4, "utf-8 key");
    CHECK(input_decode((const unsigned char *)"\xc3", 1, 0, &e) == 0, "partial utf-8 waits");
    CHECK(input_decode((const unsigned char *)"\r", 1, 0, &e) == 1 && e.key == KEY_ENTER, "enter");
    CHECK(input_decode((const unsigned char *)"\x7f", 1, 0, &e) == 1 && e.key == KEY_BS, "backspace");
}

/* ---- output capture ---------------------------------------------------- */

static char *g_cap;
static size_t g_capn;

static int cap_begin(void)
{
    char t[] = "/tmp/dcjson-test-XXXXXX";
    int fd = mkstemp(t);
    unlink(t);
    ob_flush();
    g_out_fd = fd;
    return fd;
}

static void cap_end(int fd)
{
    ob_flush();
    off_t n = lseek(fd, 0, SEEK_END);
    free(g_cap);
    g_cap = malloc((size_t)n + 1);
    pread(fd, g_cap, (size_t)n, 0);
    g_cap[n] = '\0';
    g_capn = (size_t)n;
    close(fd);
    g_out_fd = STDOUT_FILENO;
}

static void test_clip(void)
{
    int fd = cap_begin();
    clip_osc52(CLIP_OSC52, "hello", 5);
    cap_end(fd);
    CHECK(!strcmp(g_cap, "\x1b]52;c;aGVsbG8=\x07"), "osc52: %s", g_cap + 1);
    fd = cap_begin();
    clip_osc52(CLIP_TMUX, "ab", 2);
    cap_end(fd);
    CHECK(!strcmp(g_cap, "\x1bPtmux;\x1b\x1b]52;c;YWI=\x07\x1b\\"), "tmux wrap");
    char big[2000];
    memset(big, 'x', sizeof big);
    fd = cap_begin();
    clip_osc52(CLIP_SCREEN, big, sizeof big);
    cap_end(fd);
    CHECK(!strncmp(g_cap, "\x1bP\x1b]52;c;", 9) && strstr(g_cap, "\x1b\\\x1bP"), "screen pieces");
    /* every base64 length works */
    static const char *const want[] = {"", "YQ==", "YWI=", "YWJj", "YWJjZA=="};
    for (int n = 0; n < 5; n++) {
        fd = cap_begin();
        clip_osc52(CLIP_OSC52, "abcd", (size_t)n);
        cap_end(fd);
        char exp[64];
        snprintf(exp, sizeof exp, "\x1b]52;c;%s\x07", want[n]);
        CHECK(!strcmp(g_cap, exp), "base64 length %d", n);
    }
}

/* ---- VT emulator for renderer tests ----------------------------------- */

#define VW 60
#define VH 12
static uint32_t vt[VH][VW];
static int vx, vy, vtop, vbot;

static void vt_clear(void)
{
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < VW; x++)
            vt[y][x] = ' ';
    vx = vy = 0;
    vtop = 0;
    vbot = VH - 1;
}

static void vt_scroll(int n, int up)
{
    for (int k = 0; k < n; k++) {
        if (up) {
            for (int y = vtop; y < vbot; y++)
                memcpy(vt[y], vt[y + 1], sizeof vt[y]);
            for (int x = 0; x < VW; x++)
                vt[vbot][x] = ' ';
        } else {
            for (int y = vbot; y > vtop; y--)
                memcpy(vt[y], vt[y - 1], sizeof vt[y]);
            for (int x = 0; x < VW; x++)
                vt[vtop][x] = ' ';
        }
    }
}

/* returns -1 if a byte sequence reached the "terminal" that could be a
 * control sequence the renderer did not mean to send */
static int vt_feed(const char *s0, size_t n)
{
    const unsigned char *s = (const unsigned char *)s0;
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        if (c == 0x1b) {
            if (i + 1 < n && s[i + 1] == '[') {
                size_t j = i + 2;
                int p[4] = {0, 0, 0, 0}, np = 0;
                while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
                    if (s[j] == ';')
                        np < 3 ? np++ : 0;
                    else
                        p[np] = p[np] * 10 + (s[j] - '0');
                    j++;
                }
                char f = (char)s[j];
                switch (f) {
                case 'H': vy = (p[0] ? p[0] : 1) - 1; vx = (p[1] ? p[1] : 1) - 1; break;
                case 'K': for (int x = vx; x < VW; x++) vt[vy][x] = ' '; break;
                case 'J': vt_clear(); break;
                case 'C': vx += p[0] ? p[0] : 1; if (vx >= VW) vx = VW - 1; break;
                case 'r': vtop = (p[0] ? p[0] : 1) - 1; vbot = (p[1] ? p[1] : VH) - 1; vx = vy = 0; break;
                case 'S': vt_scroll(p[0] ? p[0] : 1, 1); break;
                case 'T': vt_scroll(p[0] ? p[0] : 1, 0); break;
                case 'm': break;
                default: return -1;
                }
                i = j + 1;
                continue;
            }
            return -1;
        }
        if (c == '\r') {
            vx = 0;
            i++;
            continue;
        }
        if (c == '\n') {
            if (vy == vbot)
                vt_scroll(1, 1);
            else if (vy < VH - 1)
                vy++;
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f)
            return -1;
        uint32_t cp = c;
        int l = 1;
        if (c >= 0x80) {
            l = utf8_decode(s + i, n - i, &cp);
            if (l <= 0 || utf8_width(cp) < 0)
                return -1;
        }
        int w = utf8_width(cp);
        if (w > 0 && vx < VW) {
            vt[vy][vx] = cp;
            if (w == 2 && vx + 1 < VW)
                vt[vy][vx + 1] = 0;
            vx += w;
            if (vx >= VW)
                vx = VW - 1 + 1 > VW ? VW - 1 : VW - 1;
        }
        i += (size_t)l;
    }
    return 0;
}

static void vt_line(int y, char *out, size_t cap)
{
    size_t o = 0;
    for (int x = 0; x < VW && o + 5 < cap; x++)
        if (vt[y][x])
            o += utf8_encode(vt[y][x], out + o);
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

static app_t TA;

static void app_setup(const char *json, int lenient)
{
    json_free(&TA.doc);
    memset(&TA, 0, sizeof TA);
    parse_all(&TA.doc, json, strlen(json), 0, lenient);
    TA.view.d = &TA.doc;
    TA.view.closers = 1;
    TA.cols = VW;
    TA.rows = VH;
    TA.utf8 = 1;
    TA.color_mode = CM_256;
    TA.guides = 1;
    TA.indices = 1;
    TA.name = "t.json";
    TA.top = TA.cur = view_first(&TA.view);
    render_free();
    vt_clear();
}

static int frame(void)
{
    int fd = cap_begin();
    render_frame(&TA);
    cap_end(fd);
    return vt_feed(g_cap, g_capn);
}

static void test_render(void)
{
    char line[512];
    app_setup("{\"name\": \"Matti\", \"list\": [1, 2, 3], \"obj\": {\"a\": true}, \"e\": []}", 0);
    CHECK(frame() == 0, "frame is clean");
    vt_line(1, line, sizeof line);
    CHECK(strstr(line, "\xe2\x96\xbe {") && strstr(line, "4 keys"), "root line: %s", line);
    vt_line(2, line, sizeof line);
    CHECK(strstr(line, "\"name\": \"Matti\","), "member line: %s", line);
    vt_line(4, line, sizeof line);
    CHECK(strstr(line, "0: 1,"), "index line: %s", line);
    vt_line(VH - 1, line, sizeof line);
    CHECK(strstr(line, "q quit"), "status: %s", line);

    /* collapse: one-line preview */
    TA.doc.node[2].flags &= (uint8_t)~JF_OPEN;
    CHECK(frame() == 0, "frame");
    vt_line(3, line, sizeof line);
    CHECK(strstr(line, "\"list\": [1, 2, 3],") && strstr(line, "3 items"), "preview: %s", line);

    /* scroll-region frames give the same screen as full repaints */
    sbuf_t b = {0};
    sb_str(&b, "[");
    for (int i = 0; i < 200; i++) {
        char t[80];
        snprintf(t, sizeof t, "%s{\"id\": %d, \"v\": \"value %d\"}", i ? "," : "", i, i);
        sb_str(&b, t);
    }
    sb_str(&b, "]");
    app_setup(b.p, 0);
    CHECK(frame() == 0, "frame");
    uint32_t screen[VH][VW];
    int ok = 1;
    uint64_t sc0 = g_scrolls;
    for (int step = 0; step < 120 && ok; step++) {
        long k = (long)rn(7) - 3;
        if (step % 17 == 0)
            k = 12;
        TA.top = view_step(&TA.view, TA.top, k, NULL);
        TA.cur = view_step(&TA.view, TA.top, (long)rn(5), NULL);
        if (rn(9) == 0)
            TA.doc.node[ROW_NODE(TA.cur)].flags ^= JF_OPEN;
        ok = frame() == 0;
        memcpy(screen, vt, sizeof vt);
        render_invalidate(1);
        vt_clear();
        ok = ok && frame() == 0;
        ok = ok && !memcmp(screen, vt, sizeof vt);
        if (!ok)
            for (int y = 0; y < VH; y++) {
                char a2[512], b2[512];
                size_t o1 = 0, o2 = 0;
                for (int x = 0; x < VW; x++) {
                    if (screen[y][x])
                        o1 += utf8_encode(screen[y][x], a2 + o1);
                    if (vt[y][x])
                        o2 += utf8_encode(vt[y][x], b2 + o2);
                }
                a2[o1] = b2[o2] = 0;
                if (strcmp(a2, b2))
                    fprintf(stderr, "  line %d:\n   inc  '%s'\n   full '%s'\n", y, a2, b2);
            }
    }
    CHECK(ok, "incremental frames equal full repaints");
    CHECK(g_scrolls > sc0, "scroll regions were used");

    /* one line down costs one new line, not a screen */
    TA.top = TA.cur = view_first(&TA.view);
    render_invalidate(1);
    vt_clear();
    frame();
    TA.cur = view_step(&TA.view, TA.cur, VH - 3, NULL); /* last line on screen */
    frame();
    uint64_t before = g_lines_sent;
    TA.cur = view_next(&TA.view, TA.cur);
    TA.top = view_next(&TA.view, TA.top);
    frame();
    CHECK(g_lines_sent - before <= 4, "one step down drew %llu lines", (unsigned long long)(g_lines_sent - before));

    /* hostile strings never reach the terminal as control sequences */
    app_setup("[\"\x1b]0;evil\x07\", \"\xc2\x9b" "31m\", \"\xe2\x80\xae" "abc\", \"\xff\xfe\", \"ok \\u001b[2J\"]", 1);
    CHECK(frame() == 0, "hostile input: only the renderer's own sequences reach the terminal");
    CHECK(!memmem(g_cap, g_capn, "\x1b]0;evil", 8), "no OSC from the data");
    vt_line(6, line, sizeof line);
    CHECK(strstr(line, "\\u001b[2J"), "escapes shown as text: %s", line);
    vt_line(2, line, sizeof line);
    CHECK(strstr(line, "\xef\xbf\xbd]0;evil\xef\xbf\xbd"), "raw ESC and BEL drawn as U+FFFD: %s", line);

    /* wide characters and truncation */
    app_setup("{\"k\": \"\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1"
              "\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1"
              "\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1"
              "\xe4\xba\xac\xe6\x9d\xb1\xe4\xba\xac\xe6\x9d\xb1\"}", 0);
    CHECK(frame() == 0, "frame");
    vt_line(2, line, sizeof line);
    CHECK(strstr(line, "\xe2\x80\xa6"), "long line ends in an ellipsis: %s", line);

    /* no colours: still clean, cursor in reverse video */
    app_setup("[1,2]", 0);
    TA.color_mode = CM_NONE;
    CHECK(frame() == 0 && strstr(g_cap, "\x1b[0;7m"), "monochrome cursor line");
    json_free(&TA.doc);
    sb_free(&b);
    render_free();
}

/* ---- configuration ---------------------------------------------------- */

static void test_cli(void)
{
    config_t c;
    char err[256];
    char *a1[] = {"dcjson", "--closers", "off", "-e", "3", "--theme=nord", "x.json", NULL};
    CHECK(cli_parse(7, a1, &c, err, sizeof err) == CLI_RUN && !c.closers && c.expand == 3 &&
              !strcmp(c.file, "x.json") && c.theme == theme_find("nord"), "parse: %s", err);
    char *a2[] = {"dcjson", "--clip-max", "4M", "--clipboard", "tmux", NULL};
    CHECK(cli_parse(5, a2, &c, err, sizeof err) == CLI_RUN && c.clip_max == 4u << 20 && c.clipboard == CLIP_TMUX,
          "sizes");
    char *a3[] = {"dcjson", "--bogus", NULL};
    CHECK(cli_parse(2, a3, &c, err, sizeof err) == CLI_ERROR && strstr(err, "bogus"), "unknown option");
    char *a4[] = {"dcjson", "--raw", NULL};
    CHECK(cli_parse(2, a4, &c, err, sizeof err) == CLI_ERROR, "--raw needs --path");
    char *a5[] = {"dcjson", "a", "b", NULL};
    CHECK(cli_parse(3, a5, &c, err, sizeof err) == CLI_ERROR, "two files");

    char dir[] = "/tmp/dcjson-cfg-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char path[256];
    snprintf(path, sizeof path, "%s/sub/config", dir);
    CHECK(cli_config_save(path, "theme", "gruvbox", err, sizeof err) == 0, "save new: %s", err);
    FILE *f = fopen(path, "a");
    fputs("# comment\nclosers = off   # trailing comment\nno-mouse\n", f);
    fclose(f);
    CHECK(cli_config_save(path, "theme", "dracula", err, sizeof err) == 0, "save again");
    char *a6[] = {"dcjson", "--closers", "on", NULL};
    cli_parse(3, a6, &c, err, sizeof err);
    CHECK(cli_config(&c, path, err, sizeof err) == 0, "read config: %s", err);
    CHECK(c.theme == theme_find("dracula") && c.closers == 1 && c.no_mouse == 1,
          "config merged (cli wins): theme %d closers %d", c.theme, c.closers);
    f = fopen(path, "r");
    char txt[512] = "";
    fread(txt, 1, sizeof txt - 1, f);
    fclose(f);
    CHECK(!strstr(txt, "gruvbox") && strstr(txt, "# comment"), "saving keeps the rest: %s", txt);
    f = fopen(path, "w");
    fputs("colour = red\n", f);
    fclose(f);
    CHECK(cli_config(&c, path, err, sizeof err) != 0 && strstr(err, ":1:"), "bad config names the line: %s", err);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    system(cmd);
}

/* ---- the binary ------------------------------------------------------- */

static int run(const char *bin, const char *args, const char *input, char *out, size_t cap)
{
    char in[] = "/tmp/dcjson-in-XXXXXX", ot[] = "/tmp/dcjson-out-XXXXXX";
    int fi = mkstemp(in), fo = mkstemp(ot);
    write(fi, input, strlen(input));
    close(fi);
    close(fo);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s %s < %s > %s 2>&1", bin, args, in, ot);
    int st = system(cmd);
    FILE *f = fopen(ot, "r");
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    unlink(in);
    unlink(ot);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_binary(const char *bin)
{
    char out[4096];
    CHECK(run(bin, "-c", "{\"a\": [1, 2]}", out, sizeof out) == 0 && !out[0], "check ok");
    CHECK(run(bin, "-c", "{\"a\": [1, 2}", out, sizeof out) == 1 && strstr(out, "<stdin>:1:12:"), "check error: %s", out);
    CHECK(run(bin, "-m", "{ \"a\" : [ 1 , 2 ] }", out, sizeof out) == 0 && !strcmp(out, "{\"a\":[1,2]}\n"), "minify");
    CHECK(run(bin, "", "[1]", out, sizeof out) == 0 && !strcmp(out, "[\n  1\n]\n"), "pretty when not a terminal");
    CHECK(run(bin, "--indent 4 -p", "{\"a\":1}", out, sizeof out) == 0 && !strcmp(out, "{\n    \"a\": 1\n}\n"), "indent");
    CHECK(run(bin, "-r -P .a[1].b", "{\"a\":[0,{\"b\":\"x\\ty\"}]}", out, sizeof out) == 0 && !strcmp(out, "x\ty\n"),
          "path raw: %s", out);
    CHECK(run(bin, "-P .zz", "{\"a\":1}", out, sizeof out) == 1 && strstr(out, "no key"), "path missing");
    CHECK(run(bin, "-l -m", "{\"a\":1}\n\n{\"b\":2}\n", out, sizeof out) == 0 && !strcmp(out, "{\"a\":1}\n{\"b\":2}\n"),
          "lines");
    CHECK(run(bin, "--lenient -m", "[1, /* c */ 2,]", out, sizeof out) == 0 && !strcmp(out, "[1,2]\n"), "lenient");
    CHECK(run(bin, "--bogus", "", out, sizeof out) == 2, "usage error");
    CHECK(run(bin, "--version", "", out, sizeof out) == 0 && strstr(out, "dcjson"), "version");
}

/* Bytes sent to the terminal per key, 120x40, 256 colours (bench.sh). */
static void bench_render(const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f) {
        perror(file);
        exit(2);
    }
    sbuf_t b = {0};
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0 && b.len < (64u << 20))
        sb_put(&b, tmp, n);
    fclose(f);
    memset(&TA, 0, sizeof TA);
    json_init(&TA.doc, 0, 1, 10000, 3);
    json_input(&TA.doc, b.p, b.len, 1);
    while (!json_parse(&TA.doc, UINT64_MAX)) {
    }
    TA.view.d = &TA.doc;
    TA.view.closers = 1;
    TA.cols = 120;
    TA.rows = 40;
    TA.utf8 = 1;
    TA.color_mode = CM_256;
    TA.guides = TA.indices = 1;
    TA.name = "bench.json";
    TA.top = TA.cur = view_first(&TA.view);
    int fd = cap_begin();
    render_frame(&TA);
    cap_end(fd);
    size_t full = g_capn;
    TA.cur = view_step(&TA.view, TA.cur, 37, NULL); /* last line */
    fd = cap_begin();
    render_frame(&TA);
    cap_end(fd);
    uint64_t sum = 0;
    int steps = 200;
    for (int i = 0; i < steps; i++) {
        TA.cur = view_next(&TA.view, TA.cur);
        TA.top = view_next(&TA.view, TA.top);
        fd = cap_begin();
        render_frame(&TA);
        cap_end(fd);
        sum += g_capn;
    }
    uint64_t pg = 0;
    for (int i = 0; i < 20; i++) {
        TA.cur = view_step(&TA.view, TA.cur, 37, NULL);
        TA.top = view_step(&TA.view, TA.top, 37, NULL);
        fd = cap_begin();
        render_frame(&TA);
        cap_end(fd);
        pg += g_capn;
    }
    uint64_t mv = 0;
    for (int i = 0; i < 10; i++) {
        TA.cur = view_prev(&TA.view, TA.cur);
        fd = cap_begin();
        render_frame(&TA);
        cap_end(fd);
        mv += g_capn;
    }
    printf("full screen %zu B, cursor move %llu B, scroll one line %llu B, page down %llu B\n", full,
           (unsigned long long)(mv / 10), (unsigned long long)(sum / (uint64_t)steps),
           (unsigned long long)(pg / 20));
    json_free(&TA.doc);
    sb_free(&b);
    render_free();
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--bench-render")) {
        bench_render(argv[2]);
        free(g_cap);
        return 0;
    }
    test_parser();
    test_chunked();
    test_view();
    test_view_partial();
    test_paths();
    test_search();
    test_utf8_input();
    test_clip();
    test_render();
    test_cli();
    if (argc > 1)
        test_binary(argv[1]);
    else
        fprintf(stderr, "(binary tests skipped: no path given)\n");
    free(g_cap);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
