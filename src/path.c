/* path.c - building and resolving value paths */
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* one cache slot per depth: the last sibling index computed there */
static struct {
    unsigned gen;
    uint32_t parent, child, idx;
} cache[64];

uint32_t path_index(const jdoc_t *d, uint32_t i)
{
    uint32_t p = d->node[i].parent, c, k;
    unsigned s = d->node[i].depth & 63u;
    if (cache[s].gen == d->gen && cache[s].parent == p && cache[s].child <= i && cache[s].child < d->count) {
        c = cache[s].child;
        k = cache[s].idx;
    } else {
        c = p == JN_NONE ? 0 : p + 1;
        k = 0;
    }
    while (c < i) {
        uint32_t e = d->node[c].end;
        if (e == JN_PENDING || e > i) /* i is not a sibling after c: restart */
            break;
        c = e;
        k++;
    }
    if (c != i) { /* stale cache entry: count from the first sibling */
        c = p == JN_NONE ? 0 : p + 1;
        k = 0;
        while (c < i) {
            c = d->node[c].end;
            k++;
        }
    }
    cache[s].gen = d->gen;
    cache[s].parent = p;
    cache[s].child = i;
    cache[s].idx = k;
    return k;
}

static int is_ident(const unsigned char *s, size_t n, int js)
{
    if (!n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = s[i];
        int alpha = (c | 32u) - 'a' < 26u || c == '_' || (js && c == '$');
        if (!alpha && (i == 0 || c - '0' >= 10u))
            return 0;
    }
    return 1;
}

const char *path_build(const jdoc_t *d, uint32_t i, int style, size_t *len)
{
    static sbuf_t sb;
    static uint32_t *anc;
    static size_t acap;
    sb.len = 0;
    size_t na = 0;
    for (uint32_t a = i; d->node[a].parent != JN_NONE; a = d->node[a].parent) {
        if (na == acap) {
            size_t nc = acap ? acap * 2 : 64;
            uint32_t *p = realloc(anc, nc * sizeof *p);
            if (!p)
                break;
            anc = p;
            acap = nc;
        }
        anc[na++] = a;
    }
    int js = style == PATH_JS;
    while (na) {
        uint32_t a = anc[--na];
        const jnode_t *pn = &d->node[d->node[a].parent];
        if (pn->type == JT_OBJ) {
            uint64_t ko = 0, kl = 0;
            json_key(d, a, &ko, &kl);
            const unsigned char *k = d->buf + ko;
            if (is_ident(k, kl, js)) {
                if (!js || sb.len)
                    sb_put(&sb, ".", 1);
                sb_put(&sb, (const char *)k, kl);
            } else {
                sb_str(&sb, !js && !sb.len ? ".[\"" : "[\"");
                sb_put(&sb, (const char *)k, kl);
                sb_put(&sb, "\"]", 2);
            }
        } else {
            char t[24];
            snprintf(t, sizeof t, "%s[%u]", !js && !sb.len ? "." : "", path_index(d, a));
            sb_str(&sb, t);
        }
    }
    if (!sb.len)
        sb_str(&sb, js ? "" : ".");
    if (!sb.p)
        sb_str(&sb, "");
    *len = sb.len;
    return sb.p ? sb.p : "";
}

/* ---- resolving -------------------------------------------------------- */

static int key_eq(const jdoc_t *d, uint32_t c, const char *k, size_t kl)
{
    uint64_t ko, n;
    if (!json_key(d, c, &ko, &n))
        return 0;
    const char *s = (const char *)d->buf + ko;
    if (!memchr(s, '\\', n))
        return n == kl && !memcmp(s, k, kl);
    if (n < kl)
        return 0;
    char tmp[512];
    char *buf = n <= sizeof tmp ? tmp : malloc(n);
    if (!buf)
        return 0;
    size_t m = json_unescape(s, n, buf);
    int eq = m == kl && !memcmp(buf, k, kl);
    if (buf != tmp)
        free(buf);
    return eq;
}

#define FAIL(...)                           \
    do {                                    \
        snprintf(err, errcap, __VA_ARGS__); \
        free(kb);                           \
        return -1;                          \
    } while (0)

int path_find(const jdoc_t *d, const char *e, uint32_t *out, char *err, size_t errcap)
{
    char *kb = NULL;
    while (*e == ' ')
        e++;
    if (!d->count)
        return d->done ? (snprintf(err, errcap, "the document is empty"), -1) : 1;
    uint32_t cur = 0;
    if (*e == '#') {
        char *end;
        unsigned long k = strtoul(e + 1, &end, 10);
        if (end == e + 1)
            FAIL("a number is needed after '#'");
        e = end;
        for (unsigned long r = 0; r < k; r++) {
            uint32_t nx = d->node[cur].end;
            if (nx == JN_PENDING || nx >= d->count) {
                if (!d->done)
                    return 1;
                FAIL("there are only %u top-level values", d->nroots);
            }
            cur = nx;
        }
    }
    int first = 1;
    if (*e == '.') {
        e++;
        first = e[0] != '[' && e[0] != '"';
        if (e[0] == '[' || e[0] == '"')
            first = 0;
    }
    size_t kcap = strlen(e) + 1;
    kb = malloc(kcap);
    if (!kb)
        FAIL("out of memory");
    while (*e && *e != ' ') {
        int is_key = 0;
        long idx = 0;
        size_t kl = 0;
        if (*e == '[' || *e == '"') {
            int br = *e == '[';
            if (br)
                e++;
            if (*e == '"') {
                const char *s = ++e;
                while (*e && *e != '"')
                    e += *e == '\\' && e[1] ? 2 : 1;
                if (*e != '"')
                    FAIL("unterminated quoted key");
                kl = json_unescape(s, (size_t)(e - s), kb);
                e++;
                is_key = 1;
            } else if (br) {
                char *end;
                idx = strtol(e, &end, 10);
                if (end == e)
                    FAIL("an index or a quoted key is needed after '['");
                e = end;
            }
            if (br) {
                if (*e != ']')
                    FAIL("']' expected");
                e++;
            }
        } else {
            if (*e == '.')
                e++;
            else if (!first)
                FAIL("'.' or '[' expected at \"%.20s\"", e);
            if (*e == '"' || *e == '[')
                continue;
            const char *s = e;
            while (*e && *e != '.' && *e != '[' && *e != ' ')
                e++;
            if (e == s)
                FAIL("a key is needed after '.'");
            kl = (size_t)(e - s);
            memcpy(kb, s, kl);
            is_key = 1;
        }
        first = 0;
        if (is_key)
            kb[kl] = 0;
        const jnode_t *n = &d->node[cur];
        if (is_key && n->type != JT_OBJ)
            FAIL("\"%.40s\" is looked up in a value that is not an object", kb);
        if (!is_key && n->type != JT_ARR)
            FAIL("[%ld] is used on a value that is not an array", idx);
        if (!is_key && idx < 0) {
            if (n->end == JN_PENDING) {
                free(kb);
                return 1;
            }
            idx += (long)n->n;
            if (idx < 0)
                FAIL("index out of range (%u items)", n->n);
        }
        uint32_t c = cur + 1, found = JN_NONE;
        long k = 0;
        for (;;) {
            if (n->end != JN_PENDING ? c >= n->end : c >= d->count) {
                if (n->end == JN_PENDING) {
                    free(kb);
                    return 1;
                }
                break;
            }
            if (is_key ? key_eq(d, c, kb, kl) : k == idx) {
                found = c;
                break;
            }
            k++;
            if (d->node[c].end == JN_PENDING) {
                free(kb);
                return 1;
            }
            c = d->node[c].end;
        }
        if (found == JN_NONE) {
            if (is_key)
                FAIL("no key \"%.40s\"", kb);
            FAIL("index %ld out of range (%u items)", idx, n->n);
        }
        cur = found;
    }
    free(kb);
    *out = cur;
    return 0;
}
