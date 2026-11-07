/* fmt.h - writing values out: source text, pretty-printed, minified,
 * decoded strings.  Output goes to a sink, so nothing has to fit in memory. */
#ifndef DJ_FMT_H
#define DJ_FMT_H

#include <stddef.h>
#include <stdint.h>

#include "json.h"

typedef struct sink {
    void (*put)(struct sink *s, const char *p, size_t n); /* NULL: count only */
    uint64_t total;
    void    *ctx;
} sink_t;

static inline void sink_put(sink_t *s, const char *p, size_t n)
{
    s->total += n;
    if (s->put)
        s->put(s, p, n);
}

enum {
    FMT_RAW,    /* the source text as it is                          */
    FMT_PRETTY, /* re-indented                                       */
    FMT_MINIFY, /* without white space                               */
    FMT_TEXT,   /* strings decoded, without quotes; others like RAW  */
    FMT_KEY,    /* the decoded key                                   */
};

/* Write node i in the given style.  Returns 0, or -1 when the value is not
 * complete yet (still being parsed) or has no key (FMT_KEY).  A container
 * that was cut short by a parse error is written with its brackets closed
 * (FMT_RAW then falls back to FMT_PRETTY). */
int fmt_value(const jdoc_t *d, uint32_t i, int style, unsigned indent, sink_t *s);

/* buffered sink on a file descriptor; fd_sink_flush returns 0 or -1 */
typedef struct {
    sink_t s;
    int    fd, err;
    size_t len;
    char   buf[65536];
} fdsink_t;

void fd_sink_init(fdsink_t *f, int fd);
int  fd_sink_flush(fdsink_t *f);

#endif
