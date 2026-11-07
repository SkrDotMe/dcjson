/* utf8.h - UTF-8 decoding and terminal cell width */
#ifndef DJ_UTF8_H
#define DJ_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decode one code point from s[0..n) (n >= 1).  Returns its length 1..4,
 * or 0 for an invalid or truncated sequence (overlong forms, surrogates
 * and values above U+10FFFF are invalid). */
int    utf8_decode(const unsigned char *s, size_t n, uint32_t *cp);

/* Cells the code point takes: 0 (combining), 1 or 2 (East Asian wide,
 * emoji), or -1 when it must not be sent to a terminal (C0 / C1 controls,
 * DEL, bidirectional overrides).  Locale-independent. */
int    utf8_width(uint32_t cp);

size_t utf8_encode(uint32_t cp, char *out); /* 1..4 bytes */

#endif
