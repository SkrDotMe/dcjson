/* render.c - layout and drawing.
 *
 * Every screen line is built in a reusable line buffer, hashed (FNV-1a)
 * and compared with the hash of the line already on screen; only changed
 * lines are sent (the dcmon scheme).  Colour escapes are emitted only when
 * the colour changes, and every line ends in the default colour, so lines
 * can be sent on their own.  When the tree view moves by k rows, the
 * terminal scrolls the content area itself (scroll region + SU / SD), the
 * line hashes move along, and only the k uncovered lines are drawn: one
 * line down costs one line of output instead of a screenful.
 *
 * Input text never reaches the terminal raw: control characters, C1
 * controls, bidirectional overrides and invalid UTF-8 are drawn as U+FFFD. */
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "terminal.h"
#include "utf8.h"
#include "util.h"

uint64_t g_lines_sent, g_scrolls;

typedef struct {
    const char *open, *closed, *guide, *ell, *hline, *repl;
} glyphs_t;

static const glyphs_t G_UTF8 = {"\xe2\x96\xbe", "\xe2\x96\xb8", "\xe2\x94\x82", "\xe2\x80\xa6",
                                "\xe2\x94\x80", "\xef\xbf\xbd"};
static const glyphs_t G_ASCII = {"v", ">", "|", "~", "-", "?"};
static const glyphs_t *G = &G_UTF8;

static const char RESET[] = "\x1b[0m";
static const app_t *A;
static int g_cm, g_theme;

/* ---- line builder ----------------------------------------------------- */

static struct {
    char       *p;
    size_t      len, cap;
    int         col, skip, lim, full, hl;
    int         slot; /* colour slot in effect, -1 = terminal default */
    int         attr; /* something other than the default was sent  */
    size_t      lpos; /* start of the last visible cell, for the ellipsis */
    int         lslot;
    int         lcol;
} L;

static void lb_bytes(const char *s, size_t n)
{
    if (L.len + n <= L.cap) {
        memcpy(L.p + L.len, s, n);
        L.len += n;
    }
}

static void lb_begin(int width, int skip, int hl)
{
    L.len = 0;
    L.col = 0;
    L.skip = skip;
    L.lim = skip + width;
    L.full = 0;
    L.hl = hl;
    L.slot = -1;
    L.attr = 0;
    L.lcol = -1;
}

/* Switch to colour slot c.  When only the foreground changes, only the
 * foreground is sent (about half the bytes of a full sequence). */
static void lb_color(int c)
{
    if (c == L.slot)
        return;
    const char *s;
    if (L.slot >= 0 && c != C_MATCH && L.slot != C_MATCH && theme_bold(c) == theme_bold(L.slot))
        s = theme_fg(g_theme, g_cm, c);
    else
        s = theme_sgr(g_theme, g_cm, c, L.hl);
    if (L.slot < 0 && !strcmp(s, RESET)) /* CM_NONE: already the default */
        s = "";
    if (*s)
        L.attr = 1;
    lb_bytes(s, strlen(s));
    L.slot = c;
}

static void lb_reset(void)
{
    if (L.slot >= 0 && L.attr)
        lb_bytes(RESET, sizeof RESET - 1);
    L.slot = -1;
    L.attr = 0;
}

/* the line is full: the last cell becomes an ellipsis */
static void lb_overflow(void)
{
    if (L.full)
        return;
    L.full = 1;
    if (L.col >= L.skip && L.col < L.lim) {
        lb_color(C_DIM);
        lb_bytes(G->ell, strlen(G->ell));
        L.col++;
    } else if (L.lcol >= L.skip) {
        L.len = L.lpos;
        L.slot = L.lslot;
        L.col = L.lcol;
        lb_color(C_DIM);
        lb_bytes(G->ell, strlen(G->ell));
        L.col++;
    }
}

static void lb_cell(const char *s, size_t nb, int w, int c)
{
    if (L.full)
        return;
    if (w <= 0) { /* combining mark: belongs to the previous cell */
        if (L.lcol >= L.skip && L.col <= L.lim)
            lb_bytes(s, nb);
        return;
    }
    if (L.col + w > L.lim) {
        lb_overflow();
        return;
    }
    if (L.col < L.skip) { /* scrolled off on the left */
        if (L.col + w > L.skip) {
            lb_color(c);
            for (int k = L.skip; k < L.col + w; k++)
                lb_bytes(" ", 1);
        }
        L.col += w;
        return;
    }
    lb_color(c);
    L.lpos = L.len;
    L.lslot = L.slot;
    L.lcol = L.col;
    lb_bytes(s, nb);
    L.col += w;
}

static void lb_ascii(const char *s, int c)
{
    for (; *s && !L.full; s++)
        lb_cell(s, 1, 1, c);
}

static void lb_glyph(const char *g, int c) { lb_cell(g, strlen(g), 1, c); }

/* UTF-8 text from memory (messages, prompt, paths, the text view) */
static void lb_text(const char *s0, size_t n, int c)
{
    const unsigned char *s = (const unsigned char *)s0;
    for (size_t i = 0; i < n && !L.full;) {
        unsigned ch = s[i];
        if (ch >= 0x20 && ch < 0x7f) {
            lb_cell((const char *)s + i, 1, 1, c);
            i++;
            continue;
        }
        if (ch == '\t') {
            lb_cell(" ", 1, 1, c);
            i++;
            continue;
        }
        uint32_t cp;
        int l = utf8_decode(s + i, n - i, &cp);
        int w = l > 0 ? utf8_width(cp) : -1;
        if (l <= 0)
            l = 1;
        if (w < 0)
            lb_glyph(G->repl, C_ESC);
        else if (!A->utf8)
            lb_cell("?", 1, 1, c);
        else
            lb_cell((const char *)s + i, (size_t)l, w, c);
        i += (size_t)l;
    }
}

static int text_width(const char *s0, size_t n)
{
    const unsigned char *s = (const unsigned char *)s0;
    int w = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int l = utf8_decode(s + i, n - i, &cp);
        int cw = l > 0 ? utf8_width(cp) : 1;
        w += cw < 0 ? 1 : A->utf8 ? cw : 1;
        i += l > 0 ? (size_t)l : 1;
    }
    return w;
}

/* source text of the document: escapes and search matches coloured */
static void lb_src(uint64_t off, uint64_t n, int color, int is_str)
{
    const jdoc_t *d = &A->doc;
    const unsigned char *b = d->buf;
    uint64_t end = off + n, vis = off + (uint64_t)L.lim * 8 + 64;
    uint64_t send = end < vis ? end : vis, ms = SQ_NONE, me = 0, qn = A->sq.n;
    if (A->hl && qn) {
        ms = sq_find(&A->sq, b, off, send, d->size);
        me = ms == SQ_NONE ? 0 : ms + qn;
    }
    unsigned esc = 0;
    for (uint64_t p = off; p < end && !L.full;) {
        if (ms != SQ_NONE && p >= me) {
            ms = sq_find(&A->sq, b, me, send, d->size);
            me = ms == SQ_NONE ? 0 : ms + qn;
        }
        if (is_str && !esc && b[p] == '\\' && p + 1 < end)
            esc = b[p + 1] == 'u' ? 6 : 2;
        int c = esc ? C_ESC : color;
        if (ms != SQ_NONE && p >= ms && p < me)
            c = C_MATCH;
        unsigned ch = b[p];
        int l = 1;
        if (ch >= 0x20 && ch < 0x7f) {
            lb_cell((const char *)b + p, 1, 1, c);
        } else {
            uint32_t cp;
            l = utf8_decode(b + p, (size_t)(end - p < 4 ? end - p : 4), &cp);
            int w = l > 0 ? utf8_width(cp) : -1;
            if (l <= 0)
                l = 1;
            if (w < 0)
                lb_glyph(G->repl, C_ESC);
            else if (!A->utf8)
                lb_cell("?", 1, 1, c);
            else
                lb_cell((const char *)b + p, (size_t)l, w, c);
        }
        if (esc)
            esc = (unsigned)l < esc ? esc - (unsigned)l : 0;
        p += (uint64_t)l;
    }
}

/* Erase to the end of the line in the line's background.  The cursor line
 * relies on background colour erase, which every terminal with 256 or more
 * colours has; with 16 colours or none the highlight may be reverse video,
 * which erasing does not paint, so spaces are sent there. */
static int lb_erase(void)
{
    if (L.hl) {
        if (g_cm < CM_256)
            return 0;
        lb_color(C_TEXT);
    } else {
        lb_reset();
    }
    lb_bytes("\x1b[K", 3);
    return 1;
}

/* move to column `to` without sending spaces where possible */
static void lb_gap(int to)
{
    if (L.col >= to)
        return;
    if (L.col >= L.skip && lb_erase()) {
        char t[24];
        int k = snprintf(t, sizeof t, "\x1b[%dC", to - L.col);
        lb_bytes(t, (size_t)k);
        L.col = to;
        return;
    }
    while (L.col < to && !L.full)
        lb_cell(" ", 1, 1, C_TEXT);
}

static void lb_end(void)
{
    L.full = 0;
    if (L.col < L.skip)
        L.col = L.skip;
    if (L.col < L.lim && !lb_erase()) {
        lb_color(C_TEXT);
        for (; L.col < L.lim; L.col++)
            lb_bytes(" ", 1);
    }
    lb_reset();
}

/* ---- tree rows -------------------------------------------------------- */

static void draw_key(uint32_t i)
{
    uint64_t ko, kl;
    if (json_key(&A->doc, i, &ko, &kl)) {
        lb_src(ko - 1, kl + 2, C_KEY, 1);
        lb_ascii(": ", C_PUNCT);
    }
}

static void draw_scalar(uint32_t i)
{
    const jnode_t *n = &A->doc.node[i];
    static const int COL[] = {[JT_NULL] = C_NULL, [JT_FALSE] = C_BOOL, [JT_TRUE] = C_BOOL,
                              [JT_NUM] = C_NUM,   [JT_STR] = C_STR};
    lb_src(n->off, json_span(&A->doc, i), COL[n->type], n->type == JT_STR);
}

static const char *open_br(const jnode_t *n) { return n->type == JT_ARR ? "[" : "{"; }
static const char *close_br(const jnode_t *n) { return n->type == JT_ARR ? "]" : "}"; }

/* a collapsed container on one line: {"id": 1, "tags": ["a", …]} */
static void draw_preview(uint32_t i)
{
    const jdoc_t *d = &A->doc;
    const jnode_t *n = &d->node[i];
    uint32_t st[64], sp = 0, stop = n->end == JN_PENDING ? d->count : n->end;
    lb_ascii(open_br(n), C_PUNCT);
    for (uint32_t j = i + 1; j < stop && !L.full; j++) {
        const jnode_t *c = &d->node[j];
        while (sp && d->node[st[sp - 1]].end <= j)
            lb_ascii(close_br(&d->node[st[--sp]]), C_PUNCT);
        if (j != c->parent + 1)
            lb_ascii(", ", C_PUNCT);
        if (d->node[c->parent].type == JT_OBJ)
            draw_key(j);
        if (c->type >= JT_ARR) {
            if (!c->n && c->end != JN_PENDING) {
                lb_ascii(c->type == JT_ARR ? "[]" : "{}", C_PUNCT);
            } else {
                if (sp == 64) {
                    lb_overflow();
                    break;
                }
                lb_ascii(open_br(c), C_PUNCT);
                st[sp++] = j;
            }
        } else {
            draw_scalar(j);
        }
    }
    if (L.full)
        return;
    while (sp) {
        const jnode_t *c = &d->node[st[--sp]];
        if (c->end == JN_PENDING) {
            lb_glyph(G->ell, C_DIM);
            return;
        }
        lb_ascii(close_br(c), C_PUNCT);
    }
    if (n->end == JN_PENDING)
        lb_glyph(G->ell, C_DIM);
    else
        lb_ascii(close_br(n), C_PUNCT);
}

static void info_text(const jnode_t *n, char *o, size_t cap)
{
    char cnt[32];
    fmt_count(cnt, sizeof cnt, n->n);
    const char *unit = n->type == JT_OBJ ? (n->n == 1 ? "key" : "keys") : (n->n == 1 ? "item" : "items");
    snprintf(o, cap, "%s%s %s%s", cnt, n->end == JN_PENDING ? "+" : "", unit,
             (n->flags & JF_CUT) ? " (cut short)" : "");
}

static int has_next(uint32_t i) { return view_next_sibling(&A->doc, i) != JN_NONE; }

int render_marker_col(const app_t *a, row_t r)
{
    return (int)a->doc.node[ROW_NODE(r)].depth * 2 - a->hscroll;
}

static void draw_row(row_t r, int W)
{
    const jdoc_t *d = &A->doc;
    uint32_t i = ROW_NODE(r);
    const jnode_t *n = &d->node[i];
    lb_begin(W, A->hscroll, A->mode == MODE_TREE && r == A->cur);
    for (int k = 0; k < n->depth && !L.full; k++) {
        if (A->guides) {
            lb_glyph(G->guide, C_GUIDE);
            lb_ascii(" ", C_GUIDE);
        } else {
            lb_ascii("  ", C_TEXT);
        }
    }
    if (ROW_CLOSER(r)) {
        lb_ascii("  ", C_TEXT);
        lb_ascii(close_br(n), C_PUNCT);
        if (has_next(i))
            lb_ascii(",", C_PUNCT);
        lb_end();
        return;
    }
    int cont = n->type >= JT_ARR, kids = cont && (n->n > 0 || n->end == JN_PENDING);
    if (kids) {
        lb_glyph((n->flags & JF_OPEN) ? G->open : G->closed, C_PUNCT);
        lb_ascii(" ", C_PUNCT);
    } else {
        lb_ascii("  ", C_TEXT);
    }
    char t[48];
    if (n->parent != JN_NONE) {
        if (d->node[n->parent].type == JT_OBJ) {
            draw_key(i);
        } else if (A->indices) {
            snprintf(t, sizeof t, "%u", path_index(d, i));
            lb_ascii(t, C_INFO);
            lb_ascii(": ", C_PUNCT);
        }
    } else if (d->nroots > 1 || d->lines) {
        snprintf(t, sizeof t, "#%u ", path_index(d, i));
        lb_ascii(t, C_INFO);
    }
    char info[64] = "";
    int iw = 0;
    if (kids) {
        info_text(n, info, sizeof info);
        iw = (int)strlen(info);
    }
    int lim = L.lim, reserve = iw && W - iw - 2 > L.col - L.skip + 8;
    if (reserve)
        L.lim = L.skip + W - iw - 2;
    if (!cont) {
        draw_scalar(i);
        if (has_next(i))
            lb_ascii(",", C_PUNCT);
    } else if (!kids) {
        lb_ascii(n->type == JT_ARR ? "[]" : "{}", C_PUNCT);
        if (has_next(i))
            lb_ascii(",", C_PUNCT);
    } else if (n->flags & JF_OPEN) {
        lb_ascii(open_br(n), C_PUNCT);
    } else {
        draw_preview(i);
        if (has_next(i))
            lb_ascii(",", C_PUNCT);
    }
    if (reserve) {
        L.lim = lim;
        L.full = 0;
        if (L.col < L.skip)
            L.col = L.skip; /* nothing visible yet: lb_gap pads from here */
        lb_gap(L.skip + W - iw);
        lb_ascii(info, C_INFO);
    }
    lb_end();
}

/* ---- bars ------------------------------------------------------------- */

static void sep(void)
{
    lb_ascii(" ", C_BORDER);
    lb_glyph(G->hline, C_BORDER);
    lb_ascii(" ", C_BORDER);
}

static void draw_title(int W)
{
    const jdoc_t *d = &A->doc;
    char st[96] = "", pct[16] = "", sz[32], cnt[32];
    int stc = C_INFO;
    if (d->failed) {
        snprintf(st, sizeof st, "parse error at %llu:%llu", (unsigned long long)A->err_line,
                 (unsigned long long)A->err_col);
        stc = C_ALERT;
    } else if (A->sj_active) {
        snprintf(st, sizeof st, "searching");
    } else if (!d->done) {
        if (A->reading || !A->total)
            fmt_bytes(st, sizeof st, d->size), strncat(st, " read", sizeof st - strlen(st) - 1);
        else
            snprintf(st, sizeof st, "parsing %u%%", (unsigned)(d->pos * 100 / (A->total ? A->total : 1)));
    }
    if (A->mode == MODE_TEXT && A->tnl)
        snprintf(pct, sizeof pct, "%u%%", (unsigned)((A->tscroll + 1) * 100 / A->tnl));
    else if (A->mode == MODE_TREE && A->cur != ROW_NONE && d->count)
        snprintf(pct, sizeof pct, "%u%%", (unsigned)((uint64_t)(ROW_CLOSER(A->cur) ? d->node[ROW_NODE(A->cur)].end - 1 : ROW_NODE(A->cur)) * 100 / d->count));
    int rw = 2 + (st[0] ? (int)strlen(st) + 3 : 0) + (pct[0] ? (int)strlen(pct) + 3 : 0);

    lb_begin(W, 0, 0);
    int lim = L.lim;
    L.lim = W - rw > 10 ? W - rw : W;
    lb_glyph(G->hline, C_BORDER);
    lb_ascii(" ", C_BORDER);
    lb_ascii("dcjson", C_TITLE);
    sep();
    if (A->mode == MODE_TEXT)
        lb_text(A->ttitle, strlen(A->ttitle), C_TEXT);
    else
        lb_text(A->name, strlen(A->name), C_TEXT);
    sep();
    fmt_bytes(sz, sizeof sz, A->total ? A->total : d->size);
    lb_ascii(sz, C_DIM);
    sep();
    fmt_count(cnt, sizeof cnt, d->count);
    lb_ascii(cnt, C_DIM);
    lb_ascii(d->count == 1 ? " value" : " values", C_DIM);
    L.lim = lim;
    L.full = 0;
    lb_ascii(" ", C_BORDER);
    while (L.col < W - rw + 1)
        lb_glyph(G->hline, C_BORDER);
    if (st[0]) {
        lb_ascii(" ", C_BORDER);
        lb_ascii(st, stc);
        lb_ascii(" ", C_BORDER);
        lb_glyph(G->hline, C_BORDER);
    }
    if (pct[0]) {
        lb_ascii(" ", C_BORDER);
        lb_ascii(pct, C_DIM);
        lb_ascii(" ", C_BORDER);
        lb_glyph(G->hline, C_BORDER);
    }
    lb_end();
}

static void hint(const char *k, const char *what)
{
    lb_ascii(k, C_KEY);
    lb_ascii(" ", C_DIM);
    lb_ascii(what, C_DIM);
    lb_ascii("  ", C_DIM);
}

static void draw_status(int W)
{
    lb_begin(W, 0, 0);
    if (A->prompt) {
        char p[2] = {(char)A->prompt, 0};
        lb_ascii(p, C_TITLE);
        size_t s = 0; /* keep the end of a long input visible */
        while (s < A->plen && text_width(A->pbuf + s, A->plen - s) > W - 3)
            s++;
        while (s < A->plen && (A->pbuf[s] & 0xc0) == 0x80)
            s++;
        lb_text(A->pbuf + s, A->plen - s, C_TEXT);
        L.hl = 1;
        lb_ascii(" ", C_TEXT);
        L.hl = 0;
        lb_end();
        return;
    }
    if (A->prefix == 'y') {
        lb_ascii("copy: ", C_TITLE);
        hint("y", "value");
        hint("p", "pretty");
        hint("m", "minified");
        hint("r", "text");
        hint("k", "key");
        hint(".", "jq path");
        hint("j", "js path");
        hint("Esc", "cancel");
        lb_end();
        return;
    }
    if (A->prefix == 'z') {
        lb_ascii("scroll: ", C_TITLE);
        hint("z", "center");
        hint("t", "top");
        hint("b", "bottom");
        lb_end();
        return;
    }
    if (A->msg[0]) {
        lb_text(A->msg, strlen(A->msg), A->msg_alert ? C_ALERT : C_TEXT);
        lb_end();
        return;
    }
    const char *hints = A->mode == MODE_TREE ? "/ search  y copy  ? help  q quit"
                        : A->mode == MODE_TEXT ? "y copy  q close"
                                               : "q close";
    int hw = (int)strlen(hints);
    const char *path = "";
    size_t pl = 0;
    char root[24] = "";
    if (A->mode == MODE_TREE && A->cur != ROW_NONE) {
        uint32_t i = ROW_NODE(A->cur);
        path = path_build(&A->doc, i, PATH_JQ, &pl);
        if (A->doc.nroots > 1 || A->doc.lines) {
            uint32_t r = i;
            while (A->doc.node[r].parent != JN_NONE)
                r = A->doc.node[r].parent;
            snprintf(root, sizeof root, "#%u ", path_index(&A->doc, r));
        }
    } else if (A->mode == MODE_TEXT) {
        path = A->ttitle;
        pl = strlen(path);
    }
    int avail = W - hw - 4 - (int)strlen(root);
    if (avail > 4) {
        lb_ascii(" ", C_DIM);
        lb_ascii(root, C_INFO);
        size_t s = 0;
        if (text_width(path, pl) > avail) {
            while (s < pl && text_width(path + s, pl - s) > avail - 1)
                s++;
            while (s < pl && (path[s] & 0xc0) == 0x80)
                s++;
            lb_glyph(G->ell, C_DIM);
        }
        lb_text(path + s, pl - s, C_TEXT);
    }
    lb_gap(W - hw - 1);
    const char *h = hints;
    while (*h) { /* key letters in the key colour */
        const char *sp = strchr(h, ' ');
        size_t kl = sp ? (size_t)(sp - h) : strlen(h);
        char k[16];
        snprintf(k, sizeof k, "%.*s", (int)(kl < 15 ? kl : 15), h);
        lb_ascii(k, C_KEY);
        h += kl;
        const char *e = strstr(h, "  ");
        size_t wl = e ? (size_t)(e - h) : strlen(h);
        char w[32];
        snprintf(w, sizeof w, "%.*s", (int)(wl < 31 ? wl : 31), h);
        lb_ascii(w, C_DIM);
        h += wl;
        if (*h) {
            lb_ascii("  ", C_DIM);
            h += 2;
        }
    }
    lb_end();
}

/* ---- help and text views ---------------------------------------------- */

static const char *const HELP[][2] = {
    {NULL, "Moving"},
    {"j k  Down Up", "next / previous line"},
    {"J K", "next / previous sibling"},
    {"p P", "parent / top-level value"},
    {"g G  Home End", "first / last line"},
    {"PgDn PgUp  f b", "page down / up"},
    {"Ctrl-d Ctrl-u", "half a page down / up"},
    {"Ctrl-e Ctrl-y", "scroll one line"},
    {"zz zt zb", "cursor line to the middle / top / bottom"},
    {"< >", "scroll sideways"},
    {NULL, "Expanding"},
    {"l Right", "expand (on an expanded one: first child)"},
    {"h Left", "collapse (on a collapsed one: parent)"},
    {"Space Tab", "toggle"},
    {"L H", "expand / collapse the whole subtree"},
    {"1 ... 9", "expand everything down to that level"},
    {"0", "collapse everything"},
    {"*", "expand everything"},
    {NULL, "Searching"},
    {"/", "search keys and values (smart case)"},
    {"n N", "next / previous match"},
    {":", "go to a path, e.g. .users[3].name or #2 (third value)"},
    {"e", "go to the parse error"},
    {"Esc", "clear the highlight / cancel"},
    {NULL, "Copying (OSC 52, works over SSH)"},
    {"yy", "value as in the source"},
    {"yp ym", "value pretty-printed / minified"},
    {"yr", "string without quotes and escapes"},
    {"yk", "key"},
    {"y. yj", "path, jq / JavaScript style"},
    {NULL, "Other"},
    {"Enter v", "show the value in full (long strings, wrapped)"},
    {"c", "closing brackets on their own lines (saved)"},
    {"|", "indentation guides"},
    {"i", "array indices"},
    {"s S", "next / previous colour theme (saved)"},
    {"Ctrl-l", "redraw"},
    {"? F1", "this help"},
    {"q Ctrl-c", "quit"},
    {NULL, "Mouse: click selects, click on the marker or double-click toggles, wheel scrolls"},
};

size_t render_help_count(void) { return ARRAY_LEN(HELP); }

static void draw_help_line(size_t k, int W)
{
    lb_begin(W, 0, 0);
    if (k < ARRAY_LEN(HELP)) {
        if (!HELP[k][0]) {
            lb_ascii(" ", C_TEXT);
            lb_ascii(HELP[k][1], C_TITLE);
        } else {
            lb_ascii("   ", C_TEXT);
            lb_ascii(HELP[k][0], C_KEY);
            while (L.col < 21 && !L.full)
                lb_ascii(" ", C_TEXT);
            lb_ascii(HELP[k][1], C_TEXT);
        }
    }
    lb_end();
}

static void draw_text_line(size_t k, int W)
{
    lb_begin(W, 0, 0);
    if (k < A->tnl) {
        uint64_t s = A->tline[k], e = k + 1 < A->tnl ? A->tline[k + 1] : A->tlen;
        if (e > s && A->tbuf[e - 1] == '\n')
            e--;
        lb_text(A->tbuf + s, (size_t)(e - s), C_TEXT);
    }
    lb_end();
}

/* ---- frame ------------------------------------------------------------ */

static uint64_t *g_hash;
static row_t *g_rows, *g_prev;
static int g_hrows, g_wcols, g_force = 1, g_clear = 1;
static int g_pvalid, g_pmode, g_pskip;

static int ensure(int W, int H)
{
    size_t need = (size_t)W * 64 + 1024;
    if (L.cap < need) {
        char *p = realloc(L.p, need);
        if (!p)
            return -1;
        L.p = p;
        L.cap = need;
    }
    if (g_hrows != H) {
        uint64_t *h = realloc(g_hash, (size_t)H * sizeof *h);
        row_t *r = realloc(g_rows, (size_t)H * sizeof *r);
        row_t *pr = realloc(g_prev, (size_t)H * sizeof *pr);
        if (h)
            g_hash = h;
        if (r)
            g_rows = r;
        if (pr)
            g_prev = pr;
        if (!h || !r || !pr)
            return -1;
        g_hrows = H;
        g_force = 1;
        g_pvalid = 0;
    }
    if (g_wcols != W) {
        g_wcols = W;
        g_force = 1;
        g_pvalid = 0;
    }
    return 0;
}

static uint64_t fnv(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h | 1;
}

void render_invalidate(int clear)
{
    g_force = 1;
    g_pvalid = 0;
    if (clear)
        g_clear = 1;
}

row_t render_row_at(int y)
{
    if (!g_pvalid || y < 1 || y > g_hrows - 2)
        return ROW_NONE;
    return g_prev[y - 1];
}

/* move the content lines [1, H-2] by k (k > 0: up) with a scroll region */
static void scroll_region(int H, int k)
{
    char t[48];
    int CH = H - 2, n = k > 0 ? k : -k;
    snprintf(t, sizeof t, "\x1b[2;%dr\x1b[%d%c\x1b[r", H - 1, n, k > 0 ? 'S' : 'T');
    ob_str(t);
    if (k > 0) {
        for (int y = 1; y <= CH - n; y++)
            g_hash[y] = g_hash[y + n];
        for (int y = CH - n + 1; y <= CH; y++)
            g_hash[y] = 0;
    } else {
        for (int y = CH; y > n; y--)
            g_hash[y] = g_hash[y - n];
        for (int y = 1; y <= n; y++)
            g_hash[y] = 0;
    }
    g_scrolls++;
}

void render_frame(app_t *a)
{
    A = a;
    int W = a->cols, H = a->rows;
    if (W < 1 || H < 1)
        return;
    if (W > 2000)
        W = 2000;
    if (H > 1000)
        H = 1000;
    G = a->utf8 ? &G_UTF8 : &G_ASCII;
    g_cm = a->color_mode;
    g_theme = a->theme;
    if (ensure(W, H) != 0)
        return;
    if (g_clear) {
        ob_str("\x1b[0m\x1b[H\x1b[2J");
        g_clear = 0;
        g_force = 1;
        g_pvalid = 0;
    }
    if (H < 3 || W < 20) {
        if (g_force) {
            ob_str("\x1b[H\x1b[2Jterminal too small");
            g_force = 0;
            g_pvalid = 0;
            ob_flush();
        }
        return;
    }
    int CH = H - 2;
    if (a->mode == MODE_TREE) {
        row_t r = a->top;
        for (int y = 0; y < CH; y++) {
            g_rows[y] = r;
            if (r != ROW_NONE)
                r = view_next(&a->view, r);
        }
    } else {
        for (int y = 0; y < CH; y++)
            g_rows[y] = ROW_NONE;
    }

    if (!g_force && g_pvalid && a->mode == MODE_TREE && g_pmode == MODE_TREE && g_pskip == a->hscroll &&
        g_rows[0] != ROW_NONE && g_rows[0] != g_prev[0]) {
        for (int k = 1; k < CH - 1; k++) {
            if (g_prev[k] == g_rows[0]) {
                scroll_region(H, k);
                break;
            }
            if (g_rows[k] == g_prev[0]) {
                scroll_region(H, -k);
                break;
            }
        }
    }

    int last = -2;
    for (int y = 0; y < H; y++) {
        if (y == 0)
            draw_title(W);
        else if (y == H - 1)
            draw_status(W);
        else if (a->mode == MODE_TREE) {
            if (g_rows[y - 1] == ROW_NONE) {
                lb_begin(W, 0, 0);
                lb_end();
            } else {
                draw_row(g_rows[y - 1], W);
            }
        } else if (a->mode == MODE_HELP)
            draw_help_line(a->help_scroll + (size_t)(y - 1), W);
        else
            draw_text_line(a->tscroll + (size_t)(y - 1), W);
        uint64_t h = fnv(L.p, L.len);
        if (!g_force && g_hash[y] == h)
            continue;
        g_hash[y] = h;
        if (last == y - 1) {
            ob_write("\r\n", 2);
        } else {
            char cup[24];
            int k = snprintf(cup, sizeof cup, "\x1b[%d;1H", y + 1);
            ob_write(cup, (size_t)k);
        }
        ob_write(L.p, L.len);
        last = y;
        g_lines_sent++;
    }
    g_force = 0;
    memcpy(g_prev, g_rows, (size_t)CH * sizeof *g_prev);
    g_pvalid = 1;
    g_pmode = a->mode;
    g_pskip = a->hscroll;
    ob_flush();
}

void render_free(void)
{
    free(L.p);
    free(g_hash);
    free(g_rows);
    free(g_prev);
    L.p = NULL;
    L.cap = 0;
    g_hash = NULL;
    g_rows = g_prev = NULL;
    g_hrows = g_wcols = 0;
    g_force = g_clear = 1;
    g_pvalid = 0;
}
