/* theme.h - colour themes and their escape sequences */
#ifndef DJ_THEME_H
#define DJ_THEME_H

/* colour slots used by the renderer */
enum { C_RESET, C_TEXT, C_DIM, C_BORDER, C_TITLE, C_KEY, C_STR, C_ESC, C_NUM, C_BOOL, C_NULL,
       C_PUNCT, C_INFO, C_GUIDE, C_MATCH, C_ALERT, C_GOOD, C_COUNT };

/* colour modes: none, 16 colours, 256 colours, 24-bit true colour */
enum { CM_NONE, CM_16, CM_256, CM_TRUE };

int         theme_count(void);
const char *theme_name(int t);
int         theme_find(const char *name); /* index, or -1 */
const char *theme_names(void);            /* "default, nord, ..." for messages */

/* Escape sequence for slot c of theme t in colour mode m; hl = drawn on the
 * cursor line (highlight background).  Every sequence starts with a reset,
 * so it fully defines the attributes.  The string stays valid until the
 * next call with a different t or m. */
const char *theme_sgr(int t, int m, int c, int hl);

/* Only the foreground part of slot c ("" without colours): enough when the
 * previous slot had the same boldness and background.  Not for C_MATCH. */
const char *theme_fg(int t, int m, int c);
int         theme_bold(int c);

#endif
