/* json.c - resumable single-pass JSON parser.
 *
 * The parser is an explicit state machine with its own stack of open
 * containers, so it can stop at any token boundary (budget used up, input
 * not yet arrived) and continue later.  A token that is cut off by the end
 * of the available input is scanned again from its start next time, except
 * strings, which remember how far they were scanned (a long string arriving
 * slowly over a pipe is not rescanned from the beginning every time).
 *
 * String contents are scanned eight bytes at a time with SWAR bit tricks:
 * one 64-bit load finds the first quote, backslash or control byte, and the
 * high bits tell whether the string is pure ASCII. */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "utf8.h"

enum { S_TOP, S_VALUE, S_ARR_FIRST, S_ARR_NEXT, S_OBJ_FIRST, S_KEY, S_COLON, S_AFTER };
enum { R_OK, R_NEED, R_ERR };

static unsigned g_gen;

static const unsigned char WS[256] = {[' '] = 1, ['\t'] = 1, ['\n'] = 1, ['\r'] = 1};

#define ONES  0x0101010101010101ULL
#define HIGHS 0x8080808080808080ULL

static inline int is_digit(unsigned c) { return c - '0' < 10u; }

/* First '"', '\\' or byte < 0x20 at or after p; ORs the high bits of the
 * bytes before it into *hi. */
static inline uint64_t str_scan(const unsigned char *b, uint64_t p, uint64_t n, uint64_t *hi)
{
    uint64_t acc = 0;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    while (p + 8 <= n) {
        uint64_t v;
        memcpy(&v, b + p, 8);
        uint64_t q = v ^ (ONES * '"'), s = v ^ (ONES * '\\');
        uint64_t m = ((q - ONES) & ~q) | ((s - ONES) & ~s) | (v - ONES * 0x20);
        m &= ~v & HIGHS;
        if (m) {
            unsigned k = (unsigned)__builtin_ctzll(m) >> 3;
            if (k)
                acc |= v & ((1ULL << (8 * k)) - 1);
            p += k;
            *hi |= acc & HIGHS;
            return p;
        }
        acc |= v;
        p += 8;
    }
#endif
    while (p < n) {
        unsigned c = b[p];
        if (c == '"' || c == '\\' || c < 0x20)
            break;
        acc |= c;
        p++;
    }
    *hi |= acc & HIGHS;
    return p;
}

uint64_t json_str_end(const unsigned char *b, uint64_t p, uint64_t n)
{
    for (;;) {
        uint64_t hi = 0;
        p = str_scan(b, p, n, &hi);
        if (p >= n || b[p] == '"')
            return p < n ? p : n;
        p += b[p] == '\\' ? 2 : 1;
    }
}

void json_init(jdoc_t *d, int lines, int lenient, unsigned max_depth, unsigned open_depth)
{
    memset(d, 0, sizeof *d);
    d->gen = ++g_gen;
    d->lines = lines;
    d->lenient = lenient;
    d->max_depth = max_depth && max_depth <= 65535 ? max_depth : 65535;
    d->open_depth = open_depth;
    d->st = S_TOP;
}

/* The node table lives in its own mapping: it grows with mremap() (no
 * copying), untouched capacity costs no memory, and transparent huge pages
 * cut the page faults of a big table to a few hundred. */
static int table_resize(jdoc_t *d, uint64_t nc)
{
    size_t nb = (size_t)nc * sizeof(jnode_t), ob = (size_t)d->cap * sizeof(jnode_t);
    void *p = d->node ? mremap(d->node, ob, nb, MREMAP_MAYMOVE)
                      : mmap(NULL, nb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
#ifdef MADV_HUGEPAGE
    if (nb >= (4u << 20))
        madvise(p, nb, MADV_HUGEPAGE);
#endif
    d->node = p;
    d->cap = (uint32_t)nc;
    return 0;
}

void json_input(jdoc_t *d, const void *buf, uint64_t size, int eof)
{
    if (size != d->size || eof != d->eof)
        d->waiting = 0;
    d->buf = buf;
    d->size = size;
    d->eof = eof;
    if (!d->cap && size > (1u << 20)) { /* first guess: one node per 16 bytes */
        uint64_t c = size / 16;
        table_resize(d, c > JSON_MAX_NODES ? JSON_MAX_NODES : c);
    }
}

void json_free(jdoc_t *d)
{
    if (d->node)
        munmap(d->node, (size_t)d->cap * sizeof *d->node);
    free(d->stack);
    d->node = NULL;
    d->stack = NULL;
    d->count = d->cap = 0;
}

static void fail(jdoc_t *d, uint64_t pos, const char *msg)
{
    if (!d->failed) {
        d->failed = 1;
        d->err_pos = pos;
        snprintf(d->err, sizeof d->err, "%s", msg);
    }
}

/* parsing has ended: containers still open are closed where they stand */
static void finish(jdoc_t *d)
{
    while (d->sp) {
        uint32_t c = d->stack[--d->sp];
        d->node[c].end = d->count;
        d->node[c].flags |= JF_CUT;
    }
    d->in_str = 0;
    d->done = 1;
}

static uint32_t new_node(jdoc_t *d, int type, uint64_t off)
{
    if (d->count >= d->cap) {
        if (d->cap >= JSON_MAX_NODES)
            return JN_NONE;
        uint64_t nc = d->cap ? (uint64_t)d->cap * 2 : 4096;
        if (nc > JSON_MAX_NODES)
            nc = JSON_MAX_NODES;
        if (table_resize(d, nc) != 0)
            return JN_NONE;
    }
    uint32_t i = d->count++;
    jnode_t *n = &d->node[i];
    n->off = off;
    n->len = 0;
    n->end = JN_PENDING;
    n->n = 0;
    n->kd = 0;
    n->depth = (uint16_t)d->sp;
    n->type = (uint8_t)type;
    n->flags = 0;
    n->parent = d->sp ? d->stack[d->sp - 1] : JN_NONE;
    if (d->have_key) {
        uint64_t k = off - d->key_off;
        n->kd = k < UINT32_MAX ? (uint32_t)k : 0;
        d->have_key = 0;
    }
    if (n->parent != JN_NONE)
        d->node[n->parent].n++;
    else
        d->nroots++;
    return i;
}

static inline uint32_t clen(uint64_t l) { return l < JLEN_BIG ? (uint32_t)l : JLEN_BIG; }

static void after_value(jdoc_t *d) { d->st = d->sp ? S_AFTER : S_TOP; }

static int scalar(jdoc_t *d, int type, uint64_t off, uint64_t end, uint8_t flags)
{
    uint32_t i = new_node(d, type, off);
    if (i == JN_NONE) {
        fail(d, off, "too many values (out of memory)");
        return R_ERR;
    }
    d->node[i].len = clen(end - off);
    d->node[i].end = i + 1;
    d->node[i].flags = flags;
    after_value(d);
    return R_OK;
}

/* Scan a string whose opening quote is at `start`, continuing at q. */
static int scan_str(jdoc_t *d, uint64_t start, uint64_t q, uint8_t fl, uint64_t *out)
{
    const unsigned char *b = d->buf;
    uint64_t n = d->size;
    for (;;) {
        uint64_t hi = 0;
        q = str_scan(b, q, n, &hi);
        if (hi)
            fl |= JF_NONASCII;
        if (q >= n)
            break;
        unsigned c = b[q];
        if (c == '"') {
            *out = q + 1;
            d->str_flags = fl;
            return R_OK;
        }
        if (c == '\\') {
            if (q + 1 >= n)
                break;
            unsigned e = b[q + 1];
            fl |= JF_ESC;
            if (e == 'u') {
                if (q + 6 > n)
                    break;
                for (int k = 2; k < 6; k++) {
                    unsigned h = b[q + (unsigned)k];
                    if (!is_digit(h) && (h | 32u) - 'a' >= 6u) {
                        fail(d, q, "invalid \\u escape (four hex digits expected)");
                        return R_ERR;
                    }
                }
                q += 6;
            } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' ||
                       e == 'r' || e == 't') {
                q += 2;
            } else {
                fail(d, q, "invalid escape sequence in a string");
                return R_ERR;
            }
            continue;
        }
        if (!d->lenient) {
            fail(d, q, "control character in a string (it must be escaped)");
            return R_ERR;
        }
        fl |= JF_ESC;
        q++;
    }
    if (d->eof) {
        fail(d, start, "unterminated string");
        return R_ERR;
    }
    d->in_str = 1;
    d->str_start = start;
    d->str_scan = q;
    d->str_flags = fl;
    return R_NEED;
}

static int str_done(jdoc_t *d, uint64_t start, uint64_t end)
{
    if (d->str_key) {
        d->key_off = start;
        d->have_key = 1;
        d->st = S_COLON;
        return R_OK;
    }
    return scalar(d, JT_STR, start, end, d->str_flags);
}

static int scan_num(const jdoc_t *d, uint64_t p, uint64_t *e)
{
    const unsigned char *b = d->buf;
    uint64_t n = d->size, q = p;
    if (b[q] == '-')
        q++;
    if (q >= n)
        return d->eof ? R_ERR : R_NEED;
    if (b[q] == '0')
        q++;
    else if (b[q] >= '1' && b[q] <= '9')
        while (q < n && is_digit(b[q]))
            q++;
    else
        return R_ERR;
    if (q < n && b[q] == '.') {
        q++;
        if (q >= n)
            return d->eof ? R_ERR : R_NEED;
        if (!is_digit(b[q]))
            return R_ERR;
        while (q < n && is_digit(b[q]))
            q++;
    }
    if (q < n && (b[q] | 32u) == 'e') {
        q++;
        if (q < n && (b[q] == '+' || b[q] == '-'))
            q++;
        if (q >= n)
            return d->eof ? R_ERR : R_NEED;
        if (!is_digit(b[q]))
            return R_ERR;
        while (q < n && is_digit(b[q]))
            q++;
    }
    if (q >= n && !d->eof)
        return R_NEED; /* the number may go on */
    if (q < n) {
        unsigned c = b[q];
        if (is_digit(c) || (c | 32u) - 'a' < 26u || c == '.' || c == '+' || c == '-')
            return R_ERR; /* 01, 1x, 1.2.3 */
    }
    *e = q;
    return R_OK;
}

static int scan_lit(const jdoc_t *d, uint64_t p, const char *w, uint64_t wl)
{
    const unsigned char *b = d->buf;
    uint64_t avail = d->size - p;
    if (avail > wl)
        avail = wl;
    if (memcmp(b + p, w, avail) != 0)
        return R_ERR;
    if (avail < wl)
        return d->eof ? R_ERR : R_NEED;
    if (p + wl < d->size) {
        unsigned c = b[p + wl];
        if (is_digit(c) || (c | 32u) - 'a' < 26u || c == '_')
            return R_ERR;
    } else if (!d->eof) {
        return R_NEED;
    }
    return R_OK;
}

static int skip_comment(jdoc_t *d, uint64_t *pp)
{
    const unsigned char *b = d->buf;
    uint64_t p = *pp, n = d->size;
    if (p + 1 >= n)
        return d->eof ? R_ERR : R_NEED;
    if (b[p + 1] == '/') {
        const unsigned char *e = memchr(b + p + 2, '\n', n - p - 2);
        if (!e) {
            if (!d->eof)
                return R_NEED;
            *pp = n;
            return R_OK;
        }
        *pp = (uint64_t)(e - b) + 1;
        return R_OK;
    }
    if (b[p + 1] == '*') {
        for (uint64_t q = p + 2; q + 1 < n; q++) {
            const unsigned char *s = memchr(b + q, '*', n - q - 1);
            if (!s)
                break;
            q = (uint64_t)(s - b);
            if (b[q + 1] == '/') {
                *pp = q + 2;
                return R_OK;
            }
        }
        if (!d->eof)
            return R_NEED;
        fail(d, p, "unterminated comment");
        return R_ERR;
    }
    fail(d, p, "unexpected '/'");
    return R_ERR;
}

static int close_c(jdoc_t *d, uint64_t p)
{
    uint32_t c = d->stack[--d->sp];
    jnode_t *n = &d->node[c];
    n->end = d->count;
    n->len = clen(p - n->off);
    after_value(d);
    return R_OK;
}

static int value(jdoc_t *d, uint64_t *pp)
{
    const unsigned char *b = d->buf;
    uint64_t p = *pp, e;
    unsigned c = b[p];
    int r;
    switch (c) {
    case '{':
    case '[': {
        if (d->sp >= d->max_depth) {
            char m[64];
            snprintf(m, sizeof m, "nesting deeper than %u levels", d->max_depth);
            fail(d, p, m);
            return R_ERR;
        }
        if (d->sp >= d->scap) {
            unsigned nc = d->scap ? d->scap * 2 : 64;
            uint32_t *s = realloc(d->stack, nc * sizeof *s);
            if (!s) {
                fail(d, p, "out of memory");
                return R_ERR;
            }
            d->stack = s;
            d->scap = nc;
        }
        uint32_t i = new_node(d, c == '{' ? JT_OBJ : JT_ARR, p);
        if (i == JN_NONE) {
            fail(d, p, "too many values (out of memory)");
            return R_ERR;
        }
        if (d->sp < d->open_depth)
            d->node[i].flags = JF_OPEN;
        d->stack[d->sp++] = i;
        d->st = c == '{' ? S_OBJ_FIRST : S_ARR_FIRST;
        *pp = p + 1;
        return R_OK;
    }
    case '"':
        d->str_key = 0;
        r = scan_str(d, p, p + 1, 0, &e);
        if (r == R_OK && (r = str_done(d, p, e)) == R_OK)
            *pp = e;
        return r;
    case 't':
    case 'f':
    case 'n': {
        const char *w = c == 't' ? "true" : c == 'f' ? "false" : "null";
        uint64_t wl = c == 'f' ? 5 : 4;
        r = scan_lit(d, p, w, wl);
        if (r == R_ERR) {
            fail(d, p, "invalid literal (true, false or null expected)");
            return R_ERR;
        }
        if (r == R_OK && (r = scalar(d, c == 't' ? JT_TRUE : c == 'f' ? JT_FALSE : JT_NULL, p, p + wl,
                                     0)) == R_OK)
            *pp = p + wl;
        return r;
    }
    default:
        if (c == '-' || is_digit(c)) {
            r = scan_num(d, p, &e);
            if (r == R_ERR) {
                fail(d, p, "invalid number");
                return R_ERR;
            }
            if (r == R_OK && (r = scalar(d, JT_NUM, p, e, 0)) == R_OK)
                *pp = e;
            return r;
        }
        if (c == ']' || c == '}')
            fail(d, p, c == ']' ? "unexpected ']' (a value was expected)" : "unexpected '}' (a value was expected)");
        else if (c == ',')
            fail(d, p, "unexpected ',' (a value was expected)");
        else
            fail(d, p, "a value was expected");
        return R_ERR;
    }
}

int json_parse(jdoc_t *d, uint64_t budget)
{
    if (d->done)
        return 1;
    const unsigned char *b = d->buf;
    uint64_t n = d->size, p = d->pos, e;
    uint64_t lim = budget < n - p ? p + budget : n;
    int r = R_OK;

    if (!d->bom) {
        if (n - p < 3 && !d->eof && (n == p || b[p] == 0xef)) {
            d->waiting = 1;
            return 0;
        }
        if (n - p >= 3 && b[p] == 0xef && b[p + 1] == 0xbb && b[p + 2] == 0xbf)
            p += 3;
        d->bom = 1;
    }
    if (d->in_str) {
        d->in_str = 0;
        uint64_t start = d->str_start;
        r = scan_str(d, start, d->str_scan, d->str_flags, &e);
        if (r == R_OK && (r = str_done(d, start, e)) == R_OK)
            p = e;
        else if (r == R_NEED)
            goto need;
        else
            goto error;
    }

    for (;;) {
        if (p >= lim && lim < n)
            break;
        while (p < n && WS[b[p]])
            p++;
        if (p < n && b[p] == '/' && d->lenient) {
            r = skip_comment(d, &p);
            if (r == R_NEED)
                goto need;
            if (r == R_ERR)
                goto error;
            continue;
        }
        if (p >= n) {
            if (!d->eof)
                goto need;
            if (d->st != S_TOP) {
                char m[96];
                if (d->sp)
                    snprintf(m, sizeof m, "unexpected end of input (%u %s still open)", d->sp,
                             d->sp == 1 ? "container" : "containers");
                else
                    snprintf(m, sizeof m, "unexpected end of input");
                fail(d, n, m);
            } else if (!d->nroots) {
                fail(d, n, "no JSON value in the input");
            }
            d->pos = n;
            finish(d);
            return 1;
        }
        unsigned c = b[p];
        switch (d->st) {
        case S_TOP:
            if (d->nroots && !d->lines) {
                fail(d, p, "unexpected data after the JSON value (--lines reads several values)");
                goto error;
            }
            r = value(d, &p);
            break;
        case S_VALUE:
            r = value(d, &p);
            break;
        case S_ARR_FIRST:
        case S_ARR_NEXT:
            if (c == ']' && (d->st == S_ARR_FIRST || d->lenient)) {
                r = close_c(d, p);
                p++;
            } else {
                r = value(d, &p);
            }
            break;
        case S_OBJ_FIRST:
        case S_KEY:
            if (c == '}' && (d->st == S_OBJ_FIRST || d->lenient)) {
                r = close_c(d, p);
                p++;
                break;
            }
            if (c != '"') {
                fail(d, p, d->st == S_KEY && c == '}' ? "trailing comma before '}'"
                                                      : "a string key was expected");
                goto error;
            }
            d->str_key = 1;
            r = scan_str(d, p, p + 1, 0, &e);
            if (r == R_OK && (r = str_done(d, p, e)) == R_OK)
                p = e;
            break;
        case S_COLON:
            if (c != ':') {
                fail(d, p, "':' was expected after the key");
                goto error;
            }
            p++;
            d->st = S_VALUE;
            break;
        case S_AFTER: {
            int t = d->node[d->stack[d->sp - 1]].type;
            if (c == ',') {
                p++;
                d->st = t == JT_ARR ? S_ARR_NEXT : S_KEY;
            } else if ((c == ']' && t == JT_ARR) || (c == '}' && t == JT_OBJ)) {
                r = close_c(d, p);
                p++;
            } else {
                fail(d, p, t == JT_ARR ? "',' or ']' was expected after an array element"
                                       : "',' or '}' was expected after an object member");
                goto error;
            }
            break;
        }
        }
        if (r == R_NEED)
            goto need;
        if (r == R_ERR)
            goto error;
    }
    d->pos = p;
    d->waiting = 0;
    return 0;
need:
    d->pos = p;
    d->waiting = 1;
    return 0;
error:
    d->pos = p;
    finish(d);
    return 1;
}

uint64_t json_span(const jdoc_t *d, uint32_t i)
{
    const jnode_t *n = &d->node[i];
    const unsigned char *b = d->buf;
    if (n->type < JT_ARR) {
        if (n->len != JLEN_BIG)
            return n->len;
        uint64_t q = n->off;
        if (n->type == JT_STR)
            return json_str_end(b, q + 1, d->size) + 1 - q;
        while (q < d->size && !WS[b[q]] && b[q] != ',' && b[q] != ']' && b[q] != '}')
            q++;
        return q - n->off;
    }
    if (n->end == JN_PENDING || (n->flags & JF_CUT))
        return 0;
    if (n->len != JLEN_BIG)
        return (uint64_t)n->len + 1;
    /* the closing bracket follows the last child (after blanks, a
     * trailing comma or comments in lenient mode) */
    uint64_t q = n->off + 1;
    if (n->n) {
        uint32_t c = n->end - 1;
        while (d->node[c].parent != i)
            c = d->node[c].parent;
        q = d->node[c].off + json_span(d, c);
    }
    unsigned close = n->type == JT_ARR ? ']' : '}';
    while (q < d->size && b[q] != close) {
        if (b[q] == '/' && q + 1 < d->size && b[q + 1] == '*') {
            q += 2;
            while (q + 1 < d->size && !(b[q] == '*' && b[q + 1] == '/'))
                q++;
            q++;
        } else if (b[q] == '/' && q + 1 < d->size && b[q + 1] == '/') {
            while (q < d->size && b[q] != '\n')
                q++;
        }
        q++;
    }
    return q + 1 - n->off;
}

int json_key(const jdoc_t *d, uint32_t i, uint64_t *off, uint64_t *len)
{
    const jnode_t *n = &d->node[i];
    if (!n->kd)
        return 0;
    uint64_t k = n->off - n->kd + 1;
    *off = k;
    *len = json_str_end(d->buf, k, n->off) - k;
    return 1;
}

void json_linecol(const jdoc_t *d, uint64_t pos, uint64_t *line, uint64_t *col)
{
    const unsigned char *b = d->buf;
    if (pos > d->size)
        pos = d->size;
    uint64_t ln = 1, ls = 0;
    for (uint64_t p = 0; p < pos;) {
        const unsigned char *e = memchr(b + p, '\n', pos - p);
        if (!e)
            break;
        ln++;
        p = ls = (uint64_t)(e - b) + 1;
    }
    uint64_t c = 1;
    for (uint64_t p = ls; p < pos; p++)
        c += (b[p] & 0xc0) != 0x80;
    *line = ln;
    *col = c;
}

static int hexval(unsigned c)
{
    if (is_digit(c))
        return (int)(c - '0');
    c |= 32u;
    return c - 'a' < 6u ? (int)(c - 'a' + 10) : -1;
}

static int hex4(const char *s, size_t n, size_t i, uint32_t *v)
{
    if (i + 4 > n)
        return -1;
    uint32_t r = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval((unsigned char)s[i + (size_t)k]);
        if (h < 0)
            return -1;
        r = r << 4 | (uint32_t)h;
    }
    *v = r;
    return 0;
}

size_t json_unescape(const char *s, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        const char *bs = memchr(s + i, '\\', n - i);
        size_t run = bs ? (size_t)(bs - (s + i)) : n - i;
        memcpy(out + o, s + i, run);
        o += run;
        i += run;
        if (i >= n)
            break;
        if (i + 1 >= n) {
            out[o++] = '\\';
            break;
        }
        char e = s[i + 1];
        i += 2;
        switch (e) {
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (hex4(s, n, i, &cp) != 0) {
                o += utf8_encode(0xfffd, out + o);
                break;
            }
            i += 4;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                uint32_t lo;
                if (i + 1 < n && s[i] == '\\' && s[i + 1] == 'u' && hex4(s, n, i + 2, &lo) == 0 &&
                    lo >= 0xdc00 && lo <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 6;
                } else {
                    cp = 0xfffd;
                }
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                cp = 0xfffd;
            }
            o += utf8_encode(cp, out + o);
            break;
        }
        default: out[o++] = e; break; /* \" \\ \/ (and anything in lenient mode) */
        }
    }
    return o;
}

uint32_t json_node_at(const jdoc_t *d, uint64_t pos)
{
    if (!d->count)
        return JN_NONE;
    uint32_t lo = 0, hi = d->count; /* last node with off <= pos */
    while (hi - lo > 1) {
        uint32_t m = lo + (hi - lo) / 2;
        if (d->node[m].off <= pos)
            lo = m;
        else
            hi = m;
    }
    if (lo + 1 < d->count) {
        const jnode_t *nx = &d->node[lo + 1];
        if (nx->kd && pos >= nx->off - nx->kd)
            return lo + 1;
    }
    return lo;
}
