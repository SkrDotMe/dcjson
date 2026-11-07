/* search.c - substring search.
 *
 * Case-sensitive search is memmem() (glibc's is vectorised).  The
 * case-insensitive (ASCII) search looks for the first byte of the query in
 * both cases with memchr() and remembers the next position of each, so the
 * buffer is not scanned twice. */
#include "search.h"

#include <string.h>

static inline unsigned lower(unsigned c) { return c - 'A' < 26u ? c | 32u : c; }

void sq_set(squery_t *s, const char *q, size_t n)
{
    if (n > SQ_MAX)
        n = SQ_MAX;
    memcpy(s->q, q, n);
    s->q[n] = '\0';
    s->n = n;
    s->icase = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)q[i];
        if (c - 'A' < 26u)
            s->icase = 0;
        s->lq[i] = (char)lower(c);
    }
    s->lq[n] = '\0';
}

static int eq_icase(const unsigned char *b, const char *lq, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (lower(b[i]) != (unsigned char)lq[i])
            return 0;
    return 1;
}

uint64_t sq_find(const squery_t *s, const unsigned char *b, uint64_t from, uint64_t to, uint64_t size)
{
    if (!s->n || from >= to)
        return SQ_NONE;
    if (to + s->n > size + 1)
        to = size + 1 >= s->n ? size + 1 - s->n : 0;
    if (from >= to)
        return SQ_NONE;
    if (!s->icase) {
        const unsigned char *m = memmem(b + from, (size_t)(to - from) + s->n - 1, s->q, s->n);
        return m ? (uint64_t)(m - b) : SQ_NONE;
    }
    unsigned c0 = (unsigned char)s->lq[0], c1 = c0 - 'a' < 26u ? c0 & ~32u : c0;
    /* next occurrence of each case; E = none left */
    const unsigned char *E = b + to, *na = NULL, *nb = c1 != c0 ? NULL : E;
    uint64_t p = from;
    while (p < to) {
        if (!na || na < b + p) {
            na = memchr(b + p, (int)c0, (size_t)(to - p));
            if (!na)
                na = E;
        }
        if (!nb || nb < b + p) {
            nb = memchr(b + p, (int)c1, (size_t)(to - p));
            if (!nb)
                nb = E;
        }
        const unsigned char *m = na < nb ? na : nb;
        if (m == E)
            return SQ_NONE;
        if (eq_icase(m, s->lq, s->n))
            return (uint64_t)(m - b);
        p = (uint64_t)(m - b) + 1;
    }
    return SQ_NONE;
}

uint64_t sq_rfind(const squery_t *s, const unsigned char *b, uint64_t from, uint64_t to, uint64_t size)
{
    uint64_t last = SQ_NONE;
    /* forward in blocks from the end, so the common case is one block */
    uint64_t blk = 1u << 20, hi = to;
    while (hi > from) {
        uint64_t lo = hi - from > blk ? hi - blk : from;
        for (uint64_t p = lo;;) {
            uint64_t m = sq_find(s, b, p, hi, size);
            if (m == SQ_NONE)
                break;
            last = m;
            p = m + 1;
        }
        if (last != SQ_NONE)
            return last;
        hi = lo;
        if (blk < (1u << 26))
            blk *= 2;
    }
    return SQ_NONE;
}
