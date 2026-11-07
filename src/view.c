/* view.c - row navigation over the node table (see view.h) */
#include "view.h"

#include <stdlib.h>

static inline int finished(const jnode_t *n) { return n->end != JN_PENDING; }

row_t view_first(const view_t *v) { return v->d->count ? ROW(0, 0) : ROW_NONE; }

/* the row after the whole subtree of the finished node i */
static row_t after(const view_t *v, uint32_t i)
{
    const jdoc_t *d = v->d;
    for (;;) {
        const jnode_t *n = &d->node[i];
        uint32_t p = n->parent;
        if (p != JN_NONE) {
            const jnode_t *pn = &d->node[p];
            if (finished(pn) && pn->end == n->end) { /* i was p's last child */
                if (v->closers)
                    return ROW(p, 1);
                i = p;
                continue;
            }
        }
        return n->end < d->count ? ROW(n->end, 0) : ROW_NONE;
    }
}

row_t view_next(const view_t *v, row_t r)
{
    if (r == ROW_NONE)
        return ROW_NONE;
    const jdoc_t *d = v->d;
    uint32_t i = ROW_NODE(r);
    if (ROW_CLOSER(r))
        return after(v, i);
    if (view_expanded(d, i))
        return ROW(i + 1, 0);
    if (!finished(&d->node[i]))
        return ROW_NONE;
    return after(v, i);
}

/* Last visible row of the subtree of s (s itself visible); m is the last
 * node of that subtree. */
static row_t lastrow(const view_t *v, uint32_t s, uint32_t m)
{
    static uint32_t *path;
    static size_t cap;
    const jdoc_t *d = v->d;
    if (!view_expanded(d, s))
        return ROW(s, 0);
    if (v->closers && finished(&d->node[s]))
        return ROW(s, 1);
    /* the chain from s down to m, deepest first */
    size_t np = 0;
    for (uint32_t a = m; a != s; a = d->node[a].parent) {
        if (np == cap) {
            size_t nc = cap ? cap * 2 : 256;
            uint32_t *p = realloc(path, nc * sizeof *p);
            if (!p)
                return ROW(s, 0);
            path = p;
            cap = nc;
        }
        path[np++] = a;
    }
    while (np) {
        uint32_t a = path[--np];
        if (!view_expanded(d, a))
            return ROW(a, 0);
        if (v->closers && finished(&d->node[a]))
            return ROW(a, 1);
    }
    return ROW(m, 0);
}

row_t view_prev(const view_t *v, row_t r)
{
    if (r == ROW_NONE)
        return ROW_NONE;
    const jdoc_t *d = v->d;
    uint32_t i = ROW_NODE(r);
    if (ROW_CLOSER(r)) { /* the last row inside i */
        uint32_t m = d->node[i].end - 1, c = m;
        while (d->node[c].parent != i)
            c = d->node[c].parent;
        return lastrow(v, c, m);
    }
    if (i == 0)
        return ROW_NONE;
    uint32_t p = d->node[i].parent;
    if (p == i - 1)
        return ROW(p, 0);
    uint32_t s = i - 1; /* previous sibling: the ancestor of i-1 under p */
    while (d->node[s].parent != p)
        s = d->node[s].parent;
    return lastrow(v, s, i - 1);
}

row_t view_last(const view_t *v)
{
    const jdoc_t *d = v->d;
    if (!d->count)
        return ROW_NONE;
    uint32_t m = d->count - 1, r = m;
    while (d->node[r].parent != JN_NONE)
        r = d->node[r].parent;
    return lastrow(v, r, m);
}

row_t view_fix(const view_t *v, row_t r)
{
    const jdoc_t *d = v->d;
    if (!d->count)
        return ROW_NONE;
    if (r == ROW_NONE || ROW_NODE(r) >= d->count)
        return view_first(v);
    uint32_t i = ROW_NODE(r), top = JN_NONE;
    int c = ROW_CLOSER(r) && v->closers && view_expanded(d, i) && finished(&d->node[i]);
    for (uint32_t a = d->node[i].parent; a != JN_NONE; a = d->node[a].parent)
        if (!(d->node[a].flags & JF_OPEN))
            top = a;
    return top != JN_NONE ? ROW(top, 0) : ROW(i, c);
}

int view_cmp(const view_t *v, row_t a, row_t b)
{
    if (a == b)
        return 0;
    const jdoc_t *d = v->d;
    uint64_t ka, kb;
    int da = 0, db = 0;
    ka = ROW_CLOSER(a) ? 2 * (uint64_t)d->node[ROW_NODE(a)].end - 1 : 2 * (uint64_t)ROW_NODE(a);
    kb = ROW_CLOSER(b) ? 2 * (uint64_t)d->node[ROW_NODE(b)].end - 1 : 2 * (uint64_t)ROW_NODE(b);
    if (ka != kb)
        return ka < kb ? -1 : 1;
    if (ROW_CLOSER(a))
        da = d->node[ROW_NODE(a)].depth;
    if (ROW_CLOSER(b))
        db = d->node[ROW_NODE(b)].depth;
    return da > db ? -1 : da < db ? 1 : 0; /* inner closers come first */
}

row_t view_step(const view_t *v, row_t r, long k, long *moved)
{
    long m = 0;
    while (k > 0) {
        row_t n = view_next(v, r);
        if (n == ROW_NONE)
            break;
        r = n;
        k--;
        m++;
    }
    while (k < 0) {
        row_t n = view_prev(v, r);
        if (n == ROW_NONE)
            break;
        r = n;
        k++;
        m--;
    }
    if (moved)
        *moved = m;
    return r;
}

uint32_t view_next_sibling(const jdoc_t *d, uint32_t i)
{
    const jnode_t *n = &d->node[i];
    if (!finished(n) || n->end >= d->count)
        return JN_NONE;
    return d->node[n->end].parent == n->parent ? n->end : JN_NONE;
}

uint32_t view_prev_sibling(const jdoc_t *d, uint32_t i)
{
    if (i == 0)
        return JN_NONE;
    uint32_t p = d->node[i].parent;
    if (p == i - 1)
        return JN_NONE;
    uint32_t s = i - 1;
    while (d->node[s].parent != p)
        s = d->node[s].parent;
    return s;
}

void view_reveal(jdoc_t *d, uint32_t i)
{
    for (uint32_t a = d->node[i].parent; a != JN_NONE; a = d->node[a].parent)
        d->node[a].flags |= JF_OPEN;
}

void view_set_depth(jdoc_t *d, unsigned depth)
{
    d->open_depth = depth;
    for (uint32_t i = 0; i < d->count; i++) {
        jnode_t *n = &d->node[i];
        if (n->type >= JT_ARR) {
            if (n->depth < depth)
                n->flags |= JF_OPEN;
            else
                n->flags &= (uint8_t)~JF_OPEN;
        }
    }
}

void view_subtree(jdoc_t *d, uint32_t i, int open)
{
    uint32_t e = finished(&d->node[i]) ? d->node[i].end : d->count;
    for (uint32_t j = i; j < e; j++) {
        jnode_t *n = &d->node[j];
        if (n->type >= JT_ARR) {
            if (open)
                n->flags |= JF_OPEN;
            else
                n->flags &= (uint8_t)~JF_OPEN;
        }
    }
}
