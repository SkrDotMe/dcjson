/* json.h - resumable JSON parser and the value table.
 *
 * Every JSON value becomes one jnode_t in a flat array, in document order
 * (pre-order).  Nothing is copied or decoded: a node points into the input
 * buffer.  The parser can stop after any number of bytes and continue
 * later, also when the input itself is still arriving (a pipe). */
#ifndef DJ_JSON_H
#define DJ_JSON_H

#include <stddef.h>
#include <stdint.h>

enum { JT_NULL, JT_FALSE, JT_TRUE, JT_NUM, JT_STR, JT_ARR, JT_OBJ };

enum {
    JF_OPEN = 1,     /* container is expanded in the view            */
    JF_ESC = 2,      /* string contains escapes or raw control bytes */
    JF_NONASCII = 4, /* string contains bytes >= 0x80                */
    JF_CUT = 8,      /* container never closed (error / end of input) */
};

#define JN_NONE        UINT32_MAX /* no parent                       */
#define JN_PENDING     UINT32_MAX /* end of a container still open   */
#define JLEN_BIG       UINT32_MAX /* length did not fit: recompute   */
#define JSON_MAX_NODES (UINT32_MAX - 2u)

typedef struct {
    uint64_t off;    /* first byte of the value                                   */
    uint32_t len;    /* scalar: text length; container: closing bracket - off      */
    uint32_t end;    /* index one past the subtree, JN_PENDING while still open   */
    uint32_t parent; /* JN_NONE for top-level values                              */
    uint32_t n;      /* children (containers)                                     */
    uint32_t kd;     /* off - offset of the key's opening quote; 0 = no key       */
    uint16_t depth;
    uint8_t  type, flags;
} jnode_t;

typedef struct {
    const unsigned char *buf; /* input bytes                                 */
    uint64_t  size;           /* bytes available                             */
    int       eof;            /* no more input will arrive                   */
    jnode_t  *node;
    uint32_t  count, cap;
    uint32_t  nroots;
    unsigned  gen;            /* differs between documents (caches)          */
    int       lines, lenient; /* several top-level values; relaxed syntax    */
    unsigned  max_depth, open_depth;
    int       done, failed;   /* parsing finished; with an error             */
    int       waiting;        /* stopped because the input ran out (not eof) */
    uint64_t  err_pos;
    char      err[128];
    uint64_t  pos;            /* bytes consumed                              */
    /* parser state */
    int       st, bom;
    uint32_t *stack;
    unsigned  sp, scap;
    uint64_t  key_off;
    int       have_key, in_str, str_key;
    uint64_t  str_start, str_scan;
    uint8_t   str_flags;
} jdoc_t;

/* lines: accept several top-level values (JSON Lines / concatenated JSON);
 * lenient: comments, trailing commas and raw control bytes in strings;
 * open_depth: new containers shallower than this start expanded */
void     json_init(jdoc_t *d, int lines, int lenient, unsigned max_depth, unsigned open_depth);

/* (Re)point the parser at the input; the buffer may have moved or grown. */
void     json_input(jdoc_t *d, const void *buf, uint64_t size, int eof);

/* Parse about `budget` more bytes.  Returns 1 when parsing has finished
 * (d->failed tells whether with an error), 0 when more work or more input
 * is needed. */
int      json_parse(jdoc_t *d, uint64_t budget);
void     json_free(jdoc_t *d);

/* bytes of the value's source text (containers: including both brackets;
 * 0 for a container that is still open or was cut short) */
uint64_t json_span(const jdoc_t *d, uint32_t i);

/* the key of node i, without quotes; 0 when it has none */
int      json_key(const jdoc_t *d, uint32_t i, uint64_t *off, uint64_t *len);

/* 1-based line and column (in code points) of a byte offset */
void     json_linecol(const jdoc_t *d, uint64_t pos, uint64_t *line, uint64_t *col);

/* Decode the contents of a JSON string (without the quotes) into out,
 * which needs n bytes of room; returns the decoded length.  Invalid or
 * lone surrogate escapes become U+FFFD. */
size_t   json_unescape(const char *s, size_t n, char *out);

/* offset of the closing quote of the string whose contents start at p,
 * or n when the buffer ends first */
uint64_t json_str_end(const unsigned char *b, uint64_t p, uint64_t n);

/* index of the node whose text contains (or most closely precedes) byte
 * offset pos; keys belong to their value */
uint32_t json_node_at(const jdoc_t *d, uint64_t pos);

#endif
