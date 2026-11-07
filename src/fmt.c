/* fmt.c - value writers.
 *
 * The pretty printer and the minifier walk the node table in order and
 * copy scalars and keys verbatim from the source, so escapes and number
 * spellings stay exactly as they were. */
#include "fmt.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char SPACES[] = "                                                                ";

static void put_indent(sink_t *s, uint64_t n)
{
    sink_put(s, "\n", 1);
    while (n) {
        size_t k = n < sizeof SPACES - 1 ? (size_t)n : sizeof SPACES - 1;
        sink_put(s, SPACES, k);
        n -= k;
    }
}

static void put_close(const jdoc_t *d, uint32_t c, int pretty, unsigned ind, unsigned base, sink_t *s)
{
    if (pretty)
        put_indent(s, (uint64_t)ind * (d->node[c].depth - base));
    sink_put(s, d->node[c].type == JT_ARR ? "]" : "}", 1);
}

static int emit(const jdoc_t *d, uint32_t i, int pretty, unsigned ind, sink_t *s)
{
    uint32_t stop = d->node[i].end, *st = NULL;
    size_t sp = 0, cap = 0;
    unsigned base = d->node[i].depth;
    for (uint32_t j = i; j < stop; j++) {
        const jnode_t *n = &d->node[j];
        while (sp && d->node[st[sp - 1]].end <= j)
            put_close(d, st[--sp], pretty, ind, base, s);
        if (j != i) {
            if (j != n->parent + 1)
                sink_put(s, ",", 1);
            if (pretty)
                put_indent(s, (uint64_t)ind * (n->depth - base));
            if (d->node[n->parent].type == JT_OBJ) {
                uint64_t ko, kl;
                json_key(d, j, &ko, &kl);
                sink_put(s, (const char *)d->buf + ko - 1, (size_t)kl + 2);
                sink_put(s, pretty ? ": " : ":", pretty ? 2 : 1);
            }
        }
        if (n->type >= JT_ARR) {
            if (!n->n) {
                sink_put(s, n->type == JT_ARR ? "[]" : "{}", 2);
                continue;
            }
            sink_put(s, n->type == JT_ARR ? "[" : "{", 1);
            if (sp == cap) {
                size_t nc = cap ? cap * 2 : 64;
                uint32_t *p = realloc(st, nc * sizeof *p);
                if (!p) {
                    free(st);
                    return -1;
                }
                st = p;
                cap = nc;
            }
            st[sp++] = j;
        } else {
            sink_put(s, (const char *)d->buf + n->off, (size_t)json_span(d, j));
        }
    }
    while (sp)
        put_close(d, st[--sp], pretty, ind, base, s);
    free(st);
    return 0;
}

static int put_decoded(const char *src, uint64_t n, sink_t *s)
{
    if (!memchr(src, '\\', (size_t)n)) {
        sink_put(s, src, (size_t)n);
        return 0;
    }
    char *buf = malloc(n ? (size_t)n : 1);
    if (!buf)
        return -1;
    size_t m = json_unescape(src, (size_t)n, buf);
    sink_put(s, buf, m);
    free(buf);
    return 0;
}

int fmt_value(const jdoc_t *d, uint32_t i, int style, unsigned indent, sink_t *s)
{
    const jnode_t *n = &d->node[i];
    if (n->end == JN_PENDING)
        return -1;
    switch (style) {
    case FMT_KEY: {
        uint64_t ko, kl;
        if (!json_key(d, i, &ko, &kl))
            return -1;
        return put_decoded((const char *)d->buf + ko, kl, s);
    }
    case FMT_TEXT:
        if (n->type == JT_STR) {
            uint64_t sp = json_span(d, i);
            return put_decoded((const char *)d->buf + n->off + 1, sp >= 2 ? sp - 2 : 0, s);
        }
        /* fallthrough */
    case FMT_RAW:
        if (n->type < JT_ARR || !(n->flags & JF_CUT)) {
            sink_put(s, (const char *)d->buf + n->off, (size_t)json_span(d, i));
            return 0;
        }
        return emit(d, i, 1, indent, s);
    case FMT_MINIFY:
        return emit(d, i, 0, indent, s);
    default:
        return emit(d, i, 1, indent, s);
    }
}

static void fd_put(sink_t *s, const char *p, size_t n)
{
    fdsink_t *f = s->ctx;
    if (f->err)
        return;
    if (n > sizeof f->buf - f->len) {
        if (fd_sink_flush(f) != 0)
            return;
        if (n > sizeof f->buf) {
            if (write_all(f->fd, p, n) != 0)
                f->err = 1;
            return;
        }
    }
    memcpy(f->buf + f->len, p, n);
    f->len += n;
}

void fd_sink_init(fdsink_t *f, int fd)
{
    f->s.put = fd_put;
    f->s.total = 0;
    f->s.ctx = f;
    f->fd = fd;
    f->err = 0;
    f->len = 0;
}

int fd_sink_flush(fdsink_t *f)
{
    if (!f->err && f->len && write_all(f->fd, f->buf, f->len) != 0)
        f->err = 1;
    f->len = 0;
    return f->err ? -1 : 0;
}
