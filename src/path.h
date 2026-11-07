/* path.h - paths of values (".users[3].name") */
#ifndef DJ_PATH_H
#define DJ_PATH_H

#include <stddef.h>

#include "json.h"

enum { PATH_JQ, PATH_JS };

/* position of node i among its siblings (roots: among the roots); cached,
 * so walking a long array row by row stays O(1) per row */
uint32_t    path_index(const jdoc_t *d, uint32_t i);

/* The path of node i within its top-level value: jq style ".a[0][\"x y\"]"
 * or JavaScript style "a[0][\"x y\"]".  The string stays valid until the
 * next call. */
const char *path_build(const jdoc_t *d, uint32_t i, int style, size_t *len);

/* Find the node a path names.  Accepts both styles, a leading "#N" to pick
 * the N-th top-level value (0-based), quoted keys (."a b", ["a b"]) and
 * negative indices.  Returns 0 and sets *out, 1 when the parser has not
 * reached the target yet, or -1 with err set. */
int         path_find(const jdoc_t *d, const char *expr, uint32_t *out, char *err, size_t errcap);

#endif
