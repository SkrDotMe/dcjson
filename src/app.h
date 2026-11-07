/* app.h - application state shared by the main loop and the renderer */
#ifndef DJ_APP_H
#define DJ_APP_H

#include "cli.h"
#include "clip.h"
#include "json.h"
#include "search.h"
#include "theme.h"
#include "view.h"

enum { MODE_TREE, MODE_HELP, MODE_TEXT };

typedef struct app {
    config_t  cfg;
    jdoc_t    doc;
    view_t    view;
    clipcfg_t clip;
    row_t     top, cur;   /* first row on screen, cursor row           */
    int       cols, rows;
    int       color_mode; /* CM_* */
    int       theme;
    int       utf8;
    int       mode;       /* MODE_*                                    */
    int       guides, indices;
    int       hscroll;    /* columns scrolled off on the left          */
    int       prefix;     /* 'y' / 'z' typed, waiting for the next key */
    int       prompt;     /* '/' or ':' while a prompt is open         */
    char      pbuf[SQ_MAX + 1];
    size_t    plen;
    squery_t  sq;         /* last search                               */
    int       hl;         /* highlight its matches                     */
    char      msg[256];   /* status message                            */
    int       msg_alert;
    uint64_t  msg_until;
    const char *name;     /* shown in the title                        */
    uint64_t  total;      /* input size when known, else 0             */
    int       reading;    /* input still arriving                      */
    uint64_t  err_line, err_col; /* of the parse error                 */
    /* text view (a value shown in full) */
    char     *tbuf;
    size_t    tlen;
    uint64_t *tline;      /* line start offsets                        */
    size_t    tnl, tcap;
    size_t    tscroll;
    int       twidth;     /* width the lines were wrapped for          */
    char      ttitle[256];
    size_t    help_scroll;
    /* search running in the background */
    int       sj_active, sj_dir, sj_wrapped;
    uint64_t  sj_pos;
    uint32_t  sj_from;
} app_t;

#endif
