/* render.h - drawing with line diffs and scroll regions */
#ifndef DJ_RENDER_H
#define DJ_RENDER_H

#include "app.h"

void   render_frame(app_t *a);
void   render_invalidate(int clear); /* force a repaint (clear = after resize) */
row_t  render_row_at(int y);          /* tree row drawn at screen line y        */
int    render_marker_col(const app_t *a, row_t r); /* column of the ▸ / ▾ marker */
size_t render_help_count(void);
void   render_free(void);

/* for tests and --stats: lines sent, scroll operations used */
extern uint64_t g_lines_sent, g_scrolls;

#endif
