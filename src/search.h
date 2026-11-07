/* search.h - substring search over the raw input */
#ifndef DJ_SEARCH_H
#define DJ_SEARCH_H

#include <stddef.h>
#include <stdint.h>

#define SQ_MAX 255
#define SQ_NONE UINT64_MAX

typedef struct {
    char   q[SQ_MAX + 1]; /* as typed            */
    char   lq[SQ_MAX + 1]; /* lower-cased (icase) */
    size_t n;
    int    icase;         /* smart case: no upper-case letter in the query */
} squery_t;

void     sq_set(squery_t *s, const char *q, size_t n);

/* first match starting in [from, to) that ends at or before size */
uint64_t sq_find(const squery_t *s, const unsigned char *b, uint64_t from, uint64_t to, uint64_t size);
/* last match starting in [from, to) */
uint64_t sq_rfind(const squery_t *s, const unsigned char *b, uint64_t from, uint64_t to, uint64_t size);

#endif
