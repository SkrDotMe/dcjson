/* view.h - visible rows of the tree: expand / collapse and navigation.
 *
 * There is no list of visible rows.  A row is a node index plus a flag for
 * the closing-bracket row of an expanded container; which rows are visible
 * follows from the JF_OPEN bits alone.  So expanding or collapsing is O(1),
 * and moving to the next row is O(1) (O(depth) at the end of a container). */
#ifndef DJ_VIEW_H
#define DJ_VIEW_H

#include "json.h"

typedef uint64_t row_t;

#define ROW_NONE      UINT64_MAX
#define ROW(i, c)     ((row_t)(i) << 1 | (row_t)(c))
#define ROW_NODE(r)   ((uint32_t)((r) >> 1))
#define ROW_CLOSER(r) ((int)((r) & 1))

typedef struct {
    const jdoc_t *d;
    int           closers; /* closing brackets get rows of their own */
} view_t;

/* container with visible children */
static inline int view_expanded(const jdoc_t *d, uint32_t i)
{
    const jnode_t *n = &d->node[i];
    return n->type >= JT_ARR && (n->flags & JF_OPEN) && n->n > 0;
}

row_t view_first(const view_t *v);
row_t view_last(const view_t *v);            /* last row parsed so far       */
row_t view_next(const view_t *v, row_t r);   /* ROW_NONE at the end          */
row_t view_prev(const view_t *v, row_t r);
row_t view_fix(const view_t *v, row_t r);    /* nearest visible row          */
int   view_cmp(const view_t *v, row_t a, row_t b); /* document order: <0 =0 >0 */
row_t view_step(const view_t *v, row_t r, long k, long *moved); /* k rows, +/- */

uint32_t view_next_sibling(const jdoc_t *d, uint32_t i); /* JN_NONE if none */
uint32_t view_prev_sibling(const jdoc_t *d, uint32_t i);

void  view_reveal(jdoc_t *d, uint32_t i);              /* expand all ancestors */
void  view_set_depth(jdoc_t *d, unsigned depth);       /* expand depth < N     */
void  view_subtree(jdoc_t *d, uint32_t i, int open);   /* whole subtree        */

#endif
