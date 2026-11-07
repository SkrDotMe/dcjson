/* util.h - small helpers shared by all modules */
#ifndef DJ_UTIL_H
#define DJ_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NS_PER_MS    1000000ULL
#define NS_PER_SEC   1000000000ULL

uint64_t mono_ns(void);
void     fmt_bytes(char *out, size_t cap, uint64_t v); /* "48.2 MiB"  */
void     fmt_count(char *out, size_t cap, uint64_t v); /* "1,204,331" */
int      write_all(int fd, const char *p, size_t n);   /* 0 or -1     */

/* growable byte string */
typedef struct {
    char  *p;
    size_t len, cap;
} sbuf_t;

int  sb_put(sbuf_t *s, const char *p, size_t n); /* 0 or -1 (out of memory) */
int  sb_str(sbuf_t *s, const char *z);
void sb_free(sbuf_t *s);

#endif
