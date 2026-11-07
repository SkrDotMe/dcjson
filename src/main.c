/* main.c - dcjson: input, the viewer's event loop and the print modes.
 *
 * One poll() waits on a signalfd (INT TERM HUP QUIT WINCH TSTP CONT), the
 * keyboard and, while a pipe is still being read, the input.  While parsing
 * or a search is under way the loop runs in slices of a few milliseconds
 * and checks the keyboard between them, so the view works while a large
 * file is still being parsed.  When nothing happens, nothing runs. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "fmt.h"
#include "input.h"
#include "path.h"
#include "render.h"
#include "terminal.h"
#include "utf8.h"
#include "util.h"

#define SLICE_NS     (8 * NS_PER_MS)  /* parse / search work between key checks */
#define PROGRESS_NS  (250 * NS_PER_MS)
#define MSG_NS       (4 * NS_PER_SEC)
#define ESC_NS       (25 * NS_PER_MS)
#define SMALL_INPUT  (64u << 10)      /* auto-expand everything below this     */
#define FIRST_PARSE  (4u << 20)       /* parsed in full before the first frame */
#define SEARCH_CHUNK (32u << 20)
#define TEXT_MAX     (256u << 20)     /* largest value the text view shows     */

static app_t A;
static char *g_buf;
static uint64_t g_len, g_cap;
static int g_mapped, g_data_fd = -1, g_quit, g_paste;
static uint64_t g_t0, g_parse_ns, g_last_frame;
static uint32_t g_text_node;
static uint64_t g_click_ns;
static row_t g_click_row = ROW_NONE;

/* ---- input ------------------------------------------------------------ */

static int open_input(const char *file, char *err, size_t cap)
{
    int fd = STDIN_FILENO;
    if (file && strcmp(file, "-")) {
        fd = open(file, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            snprintf(err, cap, "cannot open %.200s: %s", file, strerror(errno));
            return -1;
        }
    }
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            snprintf(err, cap, "%.200s is a directory", file ? file : "standard input");
            return -1;
        }
        if (S_ISREG(st.st_mode) && st.st_size > 0) {
            void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                madvise(p, (size_t)st.st_size, MADV_SEQUENTIAL);
                g_buf = p;
                g_len = (uint64_t)st.st_size;
                g_mapped = 1;
                A.total = g_len;
                if (fd != STDIN_FILENO)
                    close(fd);
                return 0;
            }
        }
        if (S_ISREG(st.st_mode))
            A.total = (uint64_t)st.st_size;
    }
    g_data_fd = fd;
    A.reading = 1;
    return 0;
}

/* read up to max bytes; 1 = got data (or retry), 0 = end of input */
static int read_more(size_t max, char *err, size_t cap)
{
    if (g_data_fd < 0)
        return 0;
    if (g_cap - g_len < (1u << 16)) {
        uint64_t nc = g_cap ? g_cap * 2 : (1u << 20);
        char *p = realloc(g_buf, nc);
        if (!p) {
            snprintf(err, cap, "out of memory reading the input");
            goto eof;
        }
        g_buf = p;
        g_cap = nc;
    }
    size_t room = (size_t)(g_cap - g_len);
    ssize_t r = read(g_data_fd, g_buf + g_len, room < max ? room : max);
    if (r > 0) {
        g_len += (uint64_t)r;
        return 1;
    }
    if (r < 0 && (errno == EINTR || errno == EAGAIN))
        return 1;
    if (r < 0)
        snprintf(err, cap, "read error: %s", strerror(errno));
eof:
    if (g_data_fd != STDIN_FILENO)
        close(g_data_fd);
    g_data_fd = -1;
    A.reading = 0;
    return 0;
}

static void feed(void) { json_input(&A.doc, g_buf, g_len, !A.reading); }

static void free_input(void)
{
    if (g_mapped)
        munmap(g_buf, (size_t)g_len);
    else
        free(g_buf);
    g_buf = NULL;
}

/* ---- print modes ------------------------------------------------------ */

static void print_error(void)
{
    uint64_t l, c;
    json_linecol(&A.doc, A.doc.err_pos, &l, &c);
    fprintf(stderr, "%s:%llu:%llu: %s\n", A.cfg.file && strcmp(A.cfg.file, "-") ? A.cfg.file : "<stdin>",
            (unsigned long long)l, (unsigned long long)c, A.doc.err);
}

static int run_batch(void)
{
    char err[256] = "";
    while (A.reading)
        read_more(1u << 24, err, sizeof err);
    if (err[0]) {
        fprintf(stderr, "dcjson: %s\n", err);
        return 2;
    }
    feed();
    uint64_t t = mono_ns();
    while (!json_parse(&A.doc, UINT64_MAX)) {
    }
    g_parse_ns = mono_ns() - t;
    if (A.doc.failed) {
        print_error();
        return 1;
    }
    if (A.cfg.check)
        return 0;
    fdsink_t *out = malloc(sizeof *out);
    if (!out)
        return 2;
    fd_sink_init(out, STDOUT_FILENO);
    int rc = 0;
    if (A.cfg.path) {
        uint32_t i;
        char msg[160];
        if (path_find(&A.doc, A.cfg.path, &i, msg, sizeof msg) != 0) {
            fprintf(stderr, "dcjson: %s: %s\n", A.cfg.path, msg);
            free(out);
            return 1;
        }
        int st = A.cfg.raw ? FMT_TEXT : A.cfg.minify ? FMT_MINIFY : FMT_PRETTY;
        fmt_value(&A.doc, i, st, A.cfg.indent, &out->s);
        sink_put(&out->s, "\n", 1);
    } else {
        for (uint32_t i = 0; i < A.doc.count; i = A.doc.node[i].end) {
            fmt_value(&A.doc, i, A.cfg.minify ? FMT_MINIFY : FMT_PRETTY, A.cfg.indent, &out->s);
            sink_put(&out->s, "\n", 1);
        }
    }
    if (fd_sink_flush(out) != 0) {
        if (errno != EPIPE)
            fprintf(stderr, "dcjson: write error: %s\n", strerror(errno));
        rc = errno == EPIPE ? 0 : 2;
    }
    free(out);
    return rc;
}

/* ---- viewport --------------------------------------------------------- */

static int content_rows(void) { return A.rows - 2 > 1 ? A.rows - 2 : 1; }

static void set_msg(const char *s, int alert)
{
    snprintf(A.msg, sizeof A.msg, "%s", s);
    A.msg_alert = alert;
    A.msg_until = mono_ns() + MSG_NS;
}

/* keep the cursor on screen, with a margin, and the screen filled */
static void follow(void)
{
    view_t *v = &A.view;
    if (A.cur == ROW_NONE)
        return;
    int ch = content_rows(), so = ch > 8 ? 2 : 0;
    if (A.top == ROW_NONE)
        A.top = A.cur;
    if (view_cmp(v, A.cur, A.top) < 0) {
        A.top = view_step(v, A.cur, -so, NULL);
    } else {
        row_t r = A.top;
        int k = 0;
        while (k < ch && r != A.cur) {
            r = view_next(v, r);
            if (r == ROW_NONE)
                break;
            k++;
        }
        if (r != A.cur || k >= ch - so)
            A.top = view_step(v, A.cur, -(ch - 1 - so), NULL);
        else if (k < so)
            A.top = view_step(v, A.cur, -so, NULL);
    }
    long below;
    view_step(v, A.top, ch - 1, &below);
    if (below < ch - 1)
        A.top = view_step(v, A.top, -(ch - 1 - below), NULL);
}

static void center(void)
{
    A.top = view_step(&A.view, A.cur, -(content_rows() / 2), NULL);
    follow();
}

static void refix(void)
{
    A.cur = view_fix(&A.view, A.cur);
    A.top = view_fix(&A.view, A.top);
    follow();
}

static void move(long k)
{
    if (A.cur == ROW_NONE)
        return;
    A.cur = view_step(&A.view, A.cur, k, NULL);
    follow();
}

static void page(long k)
{
    if (A.cur == ROW_NONE)
        return;
    long moved;
    A.top = view_step(&A.view, A.top, k, &moved);
    A.cur = view_step(&A.view, A.cur, moved ? moved : k, NULL);
    follow();
}

static void scroll_view(long k)
{
    if (A.cur == ROW_NONE)
        return;
    view_t *v = &A.view;
    int ch = content_rows();
    A.top = view_step(v, A.top, k, NULL);
    long below;
    view_step(v, A.top, ch - 1, &below);
    if (below < ch - 1)
        A.top = view_step(v, A.top, -(ch - 1 - below), NULL);
    if (view_cmp(v, A.cur, A.top) < 0) {
        A.cur = A.top;
    } else {
        row_t bottom = view_step(v, A.top, ch - 1, NULL);
        if (view_cmp(v, A.cur, bottom) > 0)
            A.cur = bottom;
    }
}

static void go_node(uint32_t i)
{
    view_reveal(&A.doc, i);
    A.cur = ROW(i, 0);
    A.top = view_fix(&A.view, A.top);
    center();
}

/* ---- text view -------------------------------------------------------- */

static void wrap_text(int W)
{
    A.tnl = 0;
    A.twidth = W;
    for (uint64_t p = 0, ls = 0;;) {
        if (A.tnl == A.tcap) {
            size_t nc = A.tcap ? A.tcap * 2 : 1024;
            uint64_t *t = realloc(A.tline, nc * sizeof *t);
            if (!t)
                return;
            A.tline = t;
            A.tcap = nc;
        }
        A.tline[A.tnl++] = ls;
        int col = 0;
        for (;;) {
            if (p >= A.tlen)
                return;
            const unsigned char *s = (const unsigned char *)A.tbuf + p;
            if (*s == '\n') {
                p++;
                break;
            }
            uint32_t cp;
            int l = utf8_decode(s, (size_t)(A.tlen - p), &cp);
            int w = l > 0 ? utf8_width(cp) : 1;
            if (w < 0 || !A.utf8)
                w = 1;
            if (l <= 0)
                l = 1;
            if (col + w > W && col > 0)
                break;
            col += w;
            p += (uint64_t)l;
        }
        ls = p;
        if (p >= A.tlen && A.tbuf[A.tlen - 1] != '\n')
            return;
    }
}

static void mem_put(sink_t *s, const char *p, size_t n)
{
    sbuf_t *b = s->ctx;
    if (b->len + n > TEXT_MAX || sb_put(b, p, n) != 0)
        s->put = NULL; /* too large: stop collecting */
}

static void open_text(uint32_t i)
{
    const jnode_t *n = &A.doc.node[i];
    sbuf_t b = {0};
    sink_t s = {mem_put, 0, &b};
    int st = n->type == JT_STR ? FMT_TEXT : n->type >= JT_ARR ? FMT_PRETTY : FMT_RAW;
    if (fmt_value(&A.doc, i, st, A.cfg.indent, &s) != 0) {
        sb_free(&b);
        set_msg("this value is still being parsed", 1);
        return;
    }
    if (!s.put) {
        sb_free(&b);
        set_msg("the value is too large to show in full (over 256 MiB)", 1);
        return;
    }
    free(A.tbuf);
    A.tbuf = b.p ? b.p : calloc(1, 1);
    A.tlen = b.len;
    A.tscroll = 0;
    A.twidth = 0;
    size_t pl;
    const char *path = path_build(&A.doc, i, PATH_JQ, &pl);
    snprintf(A.ttitle, sizeof A.ttitle, "%s", path);
    g_text_node = i;
    A.mode = MODE_TEXT;
    render_invalidate(0);
}

/* ---- copying ---------------------------------------------------------- */

typedef struct {
    uint32_t i;
    int      style;
} vgen_t;

static int gen_value(void *ctx, sink_t *s)
{
    vgen_t *g = ctx;
    return fmt_value(&A.doc, g->i, g->style, A.cfg.indent, s);
}

typedef struct {
    const char *p;
    size_t      n;
} tgen_t;

static int gen_text(void *ctx, sink_t *s)
{
    tgen_t *t = ctx;
    sink_put(s, t->p, t->n);
    return 0;
}

static void copy(int key, uint32_t i)
{
    char msg[256];
    int r;
    if (key == '.' || key == 'j') {
        size_t n;
        const char *p = path_build(&A.doc, i, key == 'j' ? PATH_JS : PATH_JQ, &n);
        tgen_t t = {p, n};
        r = clip_copy(&A.clip, gen_text, &t, "path", msg, sizeof msg);
    } else {
        int st = key == 'p' ? FMT_PRETTY : key == 'm' ? FMT_MINIFY : key == 'r' ? FMT_TEXT
               : key == 'k' ? FMT_KEY : FMT_RAW;
        if (st == FMT_KEY && !A.doc.node[i].kd) {
            set_msg("this value has no key", 1);
            return;
        }
        vgen_t g = {i, st};
        r = clip_copy(&A.clip, gen_value, &g, st == FMT_KEY ? "key" : "value", msg, sizeof msg);
    }
    set_msg(msg, r != 0);
}

/* ---- search ----------------------------------------------------------- */

static uint64_t parsed_end(void) { return A.doc.done ? A.doc.size : A.doc.pos; }

static void start_search(int dir)
{
    if (!A.sq.n || A.cur == ROW_NONE)
        return;
    const jdoc_t *d = &A.doc;
    uint32_t i = ROW_NODE(A.cur);
    const jnode_t *n = &d->node[i];
    A.hl = 1;
    A.sj_active = 1;
    A.sj_dir = dir;
    A.sj_wrapped = 0;
    A.sj_from = i;
    if (dir > 0)
        A.sj_pos = n->type >= JT_ARR ? n->off + 1 : n->off + json_span(d, i);
    else
        A.sj_pos = n->off - n->kd;
    A.msg[0] = '\0';
}

static int search_runnable(void)
{
    if (!A.sj_active)
        return 0;
    return A.sj_dir < 0 || A.sj_wrapped || A.sj_pos < parsed_end() || A.doc.done;
}

static void search_found(uint32_t i)
{
    A.sj_active = 0;
    go_node(i);
    if (A.sj_wrapped)
        set_msg(A.sj_dir > 0 ? "search wrapped to the top" : "search wrapped to the bottom", 0);
}

static void search_fail(void)
{
    char m[300];
    A.sj_active = 0;
    snprintf(m, sizeof m, "not found: %s", A.sq.q);
    set_msg(m, 1);
}

static void search_slice(uint64_t budget_ns)
{
    const jdoc_t *d = &A.doc;
    uint64_t t0 = mono_ns();
    while (A.sj_active && mono_ns() - t0 < budget_ns) {
        uint64_t end = parsed_end(), origin = d->node[A.sj_from].off;
        if (A.sj_dir > 0) {
            uint64_t lim = A.sj_wrapped ? origin + A.sq.n : end;
            if (lim > end)
                lim = end;
            if (A.sj_pos >= lim) {
                if (!A.sj_wrapped && !d->done)
                    return; /* wait for the parser */
                if (!A.sj_wrapped) {
                    A.sj_wrapped = 1;
                    A.sj_pos = 0;
                    continue;
                }
                search_fail();
                return;
            }
            uint64_t to = A.sj_pos + SEARCH_CHUNK < lim ? A.sj_pos + SEARCH_CHUNK : lim;
            uint64_t m = sq_find(&A.sq, d->buf, A.sj_pos, to, d->size);
            if (m == SQ_NONE) {
                A.sj_pos = to;
                continue;
            }
            uint32_t i = json_node_at(d, m);
            if (i == JN_NONE || (!A.sj_wrapped && i <= A.sj_from)) {
                A.sj_pos = m + 1;
                continue;
            }
            search_found(i);
            return;
        }
        uint64_t low = A.sj_wrapped ? origin : 0;
        if (A.sj_pos <= low) {
            if (!A.sj_wrapped) {
                A.sj_wrapped = 1;
                A.sj_pos = end;
                continue;
            }
            search_fail();
            return;
        }
        uint64_t from = A.sj_pos - low > SEARCH_CHUNK ? A.sj_pos - SEARCH_CHUNK : low;
        uint64_t m = sq_rfind(&A.sq, d->buf, from, A.sj_pos, d->size);
        if (m == SQ_NONE) {
            A.sj_pos = from;
            continue;
        }
        uint32_t i = json_node_at(d, m);
        if (i == JN_NONE || (!A.sj_wrapped && i >= A.sj_from)) {
            A.sj_pos = m;
            continue;
        }
        search_found(i);
        return;
    }
}

/* ---- parsing in the background ---------------------------------------- */

static void parse_done(void)
{
    g_parse_ns = mono_ns() - g_t0;
    if (g_mapped)
        madvise(g_buf, (size_t)g_len, MADV_RANDOM);
    if (A.cfg.expand < 0 && A.doc.size < SMALL_INPUT && g_parse_ns < NS_PER_SEC)
        view_set_depth(&A.doc, 65535);
    if (A.doc.failed) {
        json_linecol(&A.doc, A.doc.err_pos, &A.err_line, &A.err_col);
        char m[256];
        snprintf(m, sizeof m, "parse error at %llu:%llu: %s (e shows it)", (unsigned long long)A.err_line,
                 (unsigned long long)A.err_col, A.doc.err);
        set_msg(m, 1);
    }
    if (A.cur == ROW_NONE && A.doc.count)
        A.cur = A.top = view_first(&A.view);
    refix();
}

static void parse_slice(uint64_t budget_ns)
{
    uint64_t t0 = mono_ns();
    while (!A.doc.done && !A.doc.waiting) {
        json_parse(&A.doc, 1u << 20);
        if (mono_ns() - t0 >= budget_ns)
            break;
    }
    if (A.cur == ROW_NONE && A.doc.count) {
        A.cur = A.top = view_first(&A.view);
        follow();
    }
    if (A.doc.done)
        parse_done();
}

static int screen_short(void)
{
    if (A.mode != MODE_TREE || A.top == ROW_NONE)
        return 1;
    long moved;
    view_step(&A.view, A.top, content_rows() - 1, &moved);
    return moved < content_rows() - 1;
}

/* ---- keys ------------------------------------------------------------- */

static void toggle(uint32_t i)
{
    jnode_t *n = &A.doc.node[i];
    if (n->type < JT_ARR || (!n->n && n->end != JN_PENDING))
        return;
    n->flags ^= JF_OPEN;
    A.cur = ROW(i, 0);
    refix();
}

static void save_option(const char *key, const char *val, const char *what)
{
    char m[200], err[160];
    if (A.cfg.no_config)
        snprintf(m, sizeof m, "%s (not saved: --no-config)", what);
    else if (cli_config_save(A.cfg.config, key, val, err, sizeof err) == 0)
        snprintf(m, sizeof m, "%s, saved", what);
    else
        snprintf(m, sizeof m, "%s (not saved: %.80s)", what, err);
    set_msg(m, 0);
}

static void prompt_key(const kev_t *e)
{
    int k = e->key;
    if (k == KEY_ESC) {
        A.prompt = 0;
        return;
    }
    if (k == KEY_ENTER) {
        if (g_paste)
            return;
        int p = A.prompt;
        A.prompt = 0;
        if (p == '/') {
            if (A.plen) {
                sq_set(&A.sq, A.pbuf, A.plen);
                start_search(1);
            }
        } else if (A.plen) {
            uint32_t i;
            char err[160], m[200];
            A.pbuf[A.plen] = '\0';
            int r = path_find(&A.doc, A.pbuf, &i, err, sizeof err);
            if (r == 0) {
                go_node(i);
            } else if (r > 0) {
                set_msg("that part of the input has not been parsed yet", 1);
            } else {
                snprintf(m, sizeof m, "%s", err);
                set_msg(m, 1);
            }
        }
        return;
    }
    if (k == KEY_BS) {
        while (A.plen && (A.pbuf[A.plen - 1] & 0xc0) == 0x80)
            A.plen--;
        if (A.plen)
            A.plen--;
        else if (!g_paste)
            A.prompt = 0;
        return;
    }
    if (k == 21) { /* Ctrl-U */
        A.plen = 0;
        return;
    }
    if (k == 23) { /* Ctrl-W */
        while (A.plen && A.pbuf[A.plen - 1] == ' ')
            A.plen--;
        while (A.plen && A.pbuf[A.plen - 1] != ' ')
            A.plen--;
        return;
    }
    if (k == KEY_UP && A.prompt == '/' && A.sq.n) {
        memcpy(A.pbuf, A.sq.q, A.sq.n);
        A.plen = A.sq.n;
        return;
    }
    if (k >= 0x20 && k < 0x110000 && k != 0x7f) {
        char u[4];
        size_t l = utf8_encode((uint32_t)k, u);
        if (A.plen + l < SQ_MAX) {
            memcpy(A.pbuf + A.plen, u, l);
            A.plen += l;
        }
    }
}

static void text_key(const kev_t *e)
{
    int ch = content_rows(), k = e->key;
    size_t maxs = A.tnl > (size_t)ch ? A.tnl - (size_t)ch : 0;
    switch (k) {
    case 'q': case KEY_ESC: case KEY_ENTER: case 'v':
        A.mode = MODE_TREE;
        render_invalidate(0);
        return;
    case 'j': case KEY_DOWN: A.tscroll++; break;
    case 'k': case KEY_UP: if (A.tscroll) A.tscroll--; break;
    case ' ': case 'f': case KEY_PGDN: case 6: A.tscroll += (size_t)ch - 1; break;
    case 'b': case KEY_PGUP: case 2:
        A.tscroll = A.tscroll > (size_t)ch - 1 ? A.tscroll - ((size_t)ch - 1) : 0;
        break;
    case 'g': case KEY_HOME: A.tscroll = 0; break;
    case 'G': case KEY_END: A.tscroll = maxs; break;
    case 'y': A.prefix = 'y'; break;
    default: break;
    }
    if (A.tscroll > maxs)
        A.tscroll = maxs;
}

static void help_key(const kev_t *e)
{
    int ch = content_rows();
    size_t n = render_help_count(), maxs = n > (size_t)ch ? n - (size_t)ch : 0;
    switch (e->key) {
    case 'j': case KEY_DOWN: A.help_scroll++; break;
    case 'k': case KEY_UP: if (A.help_scroll) A.help_scroll--; break;
    case ' ': case KEY_PGDN: A.help_scroll += (size_t)ch; break;
    case KEY_PGUP: A.help_scroll = A.help_scroll > (size_t)ch ? A.help_scroll - (size_t)ch : 0; break;
    case 'g': A.help_scroll = 0; break;
    case 'G': A.help_scroll = maxs; break;
    default:
        A.mode = MODE_TREE;
        render_invalidate(0);
        return;
    }
    if (A.help_scroll > maxs)
        A.help_scroll = maxs;
}

static void mouse(const kev_t *e)
{
    if (e->mb & 128)
        return; /* motion */
    if (e->mb == 64 || e->mb == 65) {
        int d = e->mb == 64 ? -3 : 3;
        if (A.mode == MODE_TREE)
            scroll_view(d);
        else {
            kev_t k = {0};
            k.key = d < 0 ? KEY_UP : KEY_DOWN;
            for (int i = 0; i < 3; i++) {
                if (A.mode == MODE_TEXT)
                    text_key(&k);
                else
                    help_key(&k);
            }
        }
        return;
    }
    if (e->mb != 0 || e->mrelease || A.mode != MODE_TREE)
        return;
    row_t r = render_row_at(e->my);
    if (r == ROW_NONE)
        return;
    uint64_t now = mono_ns();
    int mc = render_marker_col(&A, r);
    int dbl = r == g_click_row && now - g_click_ns < 400 * NS_PER_MS;
    g_click_row = r;
    g_click_ns = now;
    A.cur = r;
    if (!ROW_CLOSER(r) && (e->mx == mc || e->mx == mc + 1 || dbl)) {
        toggle(ROW_NODE(r));
        g_click_row = ROW_NONE;
    }
    follow();
}

static void tree_key(const kev_t *e)
{
    jdoc_t *d = &A.doc;
    view_t *v = &A.view;
    int k = e->key, ch = content_rows();
    if (k == 'q') {
        g_quit = 1;
        return;
    }
    if (k == '?' || k == KEY_F1) {
        A.mode = MODE_HELP;
        A.help_scroll = 0;
        render_invalidate(0);
        return;
    }
    if (k == '/' || k == ':') {
        A.prompt = k;
        A.plen = 0;
        return;
    }
    if (k == KEY_ESC) {
        A.hl = 0;
        A.msg[0] = '\0';
        if (A.sj_active) {
            A.sj_active = 0;
            set_msg("search cancelled", 0);
        }
        return;
    }
    if (k == 's' || k == 'S') {
        int n = theme_count();
        A.theme = (A.theme + (k == 's' ? 1 : n - 1)) % n;
        char m[80];
        snprintf(m, sizeof m, "theme %s%s", theme_name(A.theme), A.color_mode == CM_NONE ? " (colours are off)" : "");
        save_option("theme", theme_name(A.theme), m);
        render_invalidate(0);
        return;
    }
    if (k == 12) { /* Ctrl-L */
        render_invalidate(1);
        return;
    }
    if (A.cur == ROW_NONE)
        return;
    uint32_t i = ROW_NODE(A.cur);
    jnode_t *n = &d->node[i];
    int kids = n->type >= JT_ARR && (n->n || n->end == JN_PENDING);
    switch (k) {
    case 'j': case KEY_DOWN: case 14: move(1); break;
    case 'k': case KEY_UP: case 16: move(-1); break;
    case KEY_PGDN: case 'f': case 6: page(ch - 1); break;
    case KEY_PGUP: case 'b': case 2: page(-(ch - 1)); break;
    case 4: page(ch / 2); break;   /* Ctrl-D */
    case 21: page(-(ch / 2)); break; /* Ctrl-U */
    case 5: scroll_view(1); break;  /* Ctrl-E */
    case 25: scroll_view(-1); break; /* Ctrl-Y */
    case 'g': case KEY_HOME:
        A.cur = view_first(v);
        follow();
        break;
    case 'G': case KEY_END:
        A.cur = view_last(v);
        follow();
        if (!d->done)
            set_msg("still parsing: this is the end of what has been read so far", 0);
        break;
    case 'l': case KEY_RIGHT:
        if (ROW_CLOSER(A.cur))
            A.cur = ROW(i, 0);
        else if (kids && !(n->flags & JF_OPEN))
            n->flags |= JF_OPEN;
        else if (view_expanded(d, i))
            A.cur = ROW(i + 1, 0);
        refix();
        break;
    case 'h': case KEY_LEFT:
        if (ROW_CLOSER(A.cur) || (kids && (n->flags & JF_OPEN))) {
            n->flags &= (uint8_t)~JF_OPEN;
            A.cur = ROW(i, 0);
        } else if (n->parent != JN_NONE) {
            A.cur = ROW(n->parent, 0);
        }
        refix();
        break;
    case ' ': case '\t':
        toggle(i);
        break;
    case 'L':
        view_subtree(d, i, 1);
        A.cur = ROW(i, 0);
        refix();
        break;
    case 'H':
        view_subtree(d, i, 0);
        A.cur = ROW(i, 0);
        refix();
        break;
    case '*':
        view_set_depth(d, 65535);
        refix();
        break;
    case 'J': case 'K': {
        uint32_t s = k == 'J' ? view_next_sibling(d, i) : view_prev_sibling(d, i);
        if (s != JN_NONE) {
            A.cur = ROW(s, 0);
            follow();
        }
        break;
    }
    case 'p':
        if (n->parent != JN_NONE) {
            A.cur = ROW(n->parent, 0);
            follow();
        }
        break;
    case 'P': {
        uint32_t r = i;
        while (d->node[r].parent != JN_NONE)
            r = d->node[r].parent;
        A.cur = ROW(r, 0);
        follow();
        break;
    }
    case 'n': case 'N':
        if (!A.sq.n)
            set_msg("no search yet: / starts one", 1);
        else
            start_search(k == 'n' ? 1 : -1);
        break;
    case 'e':
        if (!d->failed) {
            set_msg("there is no parse error", 0);
        } else {
            char m[256];
            if (d->count)
                go_node(json_node_at(d, d->err_pos));
            snprintf(m, sizeof m, "parse error at %llu:%llu: %s", (unsigned long long)A.err_line,
                     (unsigned long long)A.err_col, d->err);
            set_msg(m, 1);
        }
        break;
    case 'y': case 'z':
        A.prefix = k;
        break;
    case KEY_ENTER: case 'v':
        open_text(i);
        break;
    case 'c':
        A.view.closers = !A.view.closers;
        refix();
        save_option("closers", A.view.closers ? "on" : "off",
                    A.view.closers ? "closing brackets on their own lines" : "closing brackets hidden");
        break;
    case '|':
        A.guides = !A.guides;
        set_msg(A.guides ? "indentation guides on" : "indentation guides off", 0);
        break;
    case 'i':
        A.indices = !A.indices;
        set_msg(A.indices ? "array indices on" : "array indices off", 0);
        break;
    case '>': A.hscroll += 8; break;
    case '<': A.hscroll = A.hscroll > 8 ? A.hscroll - 8 : 0; break;
    default:
        if (k >= '0' && k <= '9') {
            view_set_depth(d, (unsigned)(k - '0'));
            refix();
        }
        break;
    }
}

static void handle_key(const kev_t *e)
{
    if (e->key == KEY_PASTE_BEGIN) {
        g_paste = 1;
        return;
    }
    if (e->key == KEY_PASTE_END) {
        g_paste = 0;
        return;
    }
    if (A.prompt) {
        if (!g_paste || e->key >= 0x20)
            prompt_key(e);
        return;
    }
    if (g_paste || e->key == KEY_UNKNOWN)
        return;
    if (e->key == KEY_MOUSE) {
        A.prefix = 0;
        mouse(e);
        return;
    }
    if (A.msg[0] && !A.sj_active && e->key != 'n' && e->key != 'N')
        A.msg[0] = '\0';
    if (A.prefix) {
        int p = A.prefix;
        A.prefix = 0;
        if (p == 'y') {
            uint32_t i = A.mode == MODE_TEXT ? g_text_node : ROW_NODE(A.cur);
            if (A.cur == ROW_NONE && A.mode != MODE_TEXT)
                return;
            if (strchr("ypmrk.j", e->key) && e->key)
                copy(e->key, i);
        } else if (p == 'z' && A.cur != ROW_NONE) {
            int ch = content_rows();
            if (e->key == 'z' || e->key == '.')
                center();
            else if (e->key == 't' || e->key == KEY_ENTER)
                A.top = A.cur, follow();
            else if (e->key == 'b' || e->key == '-')
                A.top = view_step(&A.view, A.cur, -(ch - 1), NULL), follow();
        }
        return;
    }
    if (A.mode == MODE_HELP)
        help_key(e);
    else if (A.mode == MODE_TEXT)
        text_key(e);
    else
        tree_key(e);
}

/* ---- main loop -------------------------------------------------------- */

static int detect_utf8(void)
{
    const char *v = getenv("LC_ALL");
    if (!v || !*v)
        v = getenv("LC_CTYPE");
    if (!v || !*v)
        v = getenv("LANG");
    if (!v || !*v)
        return 1; /* no locale configured: modern terminals are UTF-8 */
    return strcasestr(v, "utf-8") || strcasestr(v, "utf8");
}

static int detect_colors(const config_t *c)
{
    const char *nc = getenv("NO_COLOR"), *t = getenv("TERM"), *ct = getenv("COLORTERM");
    if (c->no_colors || (nc && *nc) || !t || !strcmp(t, "dumb"))
        return CM_NONE;
    if ((ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) || strstr(t, "direct"))
        return CM_TRUE;
    if (strstr(t, "256"))
        return CM_256;
    return CM_16;
}

static int run_viewer(void)
{
    char err[256] = "";
    if (!isatty(STDIN_FILENO)) {
        int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "dcjson: no terminal for the keyboard (/dev/tty: %s)\n", strerror(errno));
            return 2;
        }
        g_in_fd = fd;
    }
    sigset_t ss;
    sigemptyset(&ss);
    int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH, SIGTSTP, SIGCONT};
    for (size_t i = 0; i < ARRAY_LEN(sigs); i++)
        sigaddset(&ss, sigs[i]);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    int sfd = signalfd(-1, &ss, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) {
        fprintf(stderr, "dcjson: signalfd: %s\n", strerror(errno));
        return 2;
    }
    A.color_mode = detect_colors(&A.cfg);
    A.utf8 = !A.cfg.ascii && detect_utf8();
    term_size(&A.cols, &A.rows);
    term_install_fatal_handlers();
    if (term_open(!A.cfg.no_mouse) != 0) {
        fprintf(stderr, "dcjson: cannot set up the terminal: %s\n", strerror(errno));
        return 2;
    }

    /* first frame: small inputs completely, big ones as far as ~30 ms go */
    g_t0 = mono_ns();
    if (!A.reading) {
        uint64_t lim = A.doc.size <= FIRST_PARSE ? UINT64_MAX : 30 * NS_PER_MS;
        while (!A.doc.done && mono_ns() - g_t0 < lim)
            json_parse(&A.doc, 1u << 20);
        if (A.doc.done)
            parse_done();
    }
    if (A.doc.count) {
        A.cur = A.top = view_first(&A.view);
        follow();
    }
    render_frame(&A);
    g_last_frame = mono_ns();

    unsigned char ib[4096];
    size_t il = 0;
    uint64_t esc_at = 0;
    while (!g_quit && !term_dead()) {
        int work = (!A.doc.done && !A.doc.waiting) || search_runnable();
        int timeout = -1;
        uint64_t now = mono_ns();
        if (work)
            timeout = 0;
        else if (A.msg[0] && A.msg_until > now)
            timeout = (int)((A.msg_until - now) / NS_PER_MS) + 1;
        else if (A.msg[0])
            timeout = 0;
        if (il && esc_at) {
            int t = now >= esc_at ? 0 : (int)((esc_at - now) / NS_PER_MS) + 1;
            if (timeout < 0 || t < timeout)
                timeout = t;
        }
        if (!A.doc.done && !work && A.reading && timeout < 0)
            timeout = 250; /* progress display while waiting for a pipe */
        struct pollfd pf[3] = {{sfd, POLLIN, 0}, {g_in_fd, POLLIN, 0}, {g_data_fd, POLLIN, 0}};
        int np = g_data_fd >= 0 ? 3 : 2;
        int r = poll(pf, (nfds_t)np, timeout);
        if (r < 0 && errno != EINTR)
            break;
        int dirty = 0;

        if (pf[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                switch (si.ssi_signo) {
                case SIGWINCH:
                    term_size(&A.cols, &A.rows);
                    A.twidth = 0;
                    render_invalidate(1);
                    follow();
                    dirty = 1;
                    break;
                case SIGTSTP:
                    term_close();
                    raise(SIGSTOP);
                    term_open(!A.cfg.no_mouse);
                    term_size(&A.cols, &A.rows);
                    render_invalidate(1);
                    dirty = 1;
                    break;
                case SIGCONT:
                    render_invalidate(1);
                    dirty = 1;
                    break;
                default:
                    g_quit = 1;
                    break;
                }
            }
        }
        if (np == 3 && (pf[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (int k = 0; k < 4 && read_more(1u << 20, err, sizeof err); k++) {
                struct pollfd q = {g_data_fd, POLLIN, 0};
                if (g_data_fd < 0 || poll(&q, 1, 0) <= 0)
                    break;
            }
            if (err[0]) {
                set_msg(err, 1);
                err[0] = '\0';
            }
            feed();
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(g_in_fd, ib + il, sizeof ib - il);
            if (n > 0)
                il += (size_t)n;
            else if (n == 0 || (errno != EINTR && errno != EAGAIN))
                g_quit = 1;
        }
        if (il) {
            size_t o = 0;
            int flush = esc_at && mono_ns() >= esc_at;
            while (o < il) {
                kev_t e;
                size_t u = input_decode(ib + o, il - o, flush, &e);
                if (!u)
                    break;
                o += u;
                handle_key(&e);
                dirty = 1;
                if (g_quit)
                    break;
            }
            memmove(ib, ib + o, il - o);
            il -= o;
            esc_at = il ? (esc_at ? esc_at : mono_ns() + ESC_NS) : 0;
        }
        if (g_quit)
            break;

        if (!A.doc.done && !A.doc.waiting) {
            int was_short = screen_short();
            parse_slice(SLICE_NS);
            if (A.doc.done || was_short)
                dirty = 1;
        }
        if (search_runnable()) {
            search_slice(SLICE_NS);
            if (!A.sj_active)
                dirty = 1;
        }
        now = mono_ns();
        if (A.msg[0] && now >= A.msg_until && !A.prompt) {
            A.msg[0] = '\0';
            dirty = 1;
        }
        if (!A.doc.done && now - g_last_frame >= PROGRESS_NS)
            dirty = 1;
        if (A.mode == MODE_TEXT && A.twidth != A.cols) {
            wrap_text(A.cols);
            size_t ch = (size_t)content_rows();
            if (A.tscroll + ch > A.tnl)
                A.tscroll = A.tnl > ch ? A.tnl - ch : 0;
        }
        if (dirty) {
            render_frame(&A);
            g_last_frame = mono_ns();
        }
    }
    term_close();
    close(sfd);
    return 0;
}

static void print_stats(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    char sz[32], rate[32], cnt[32], out[32];
    fmt_bytes(sz, sizeof sz, A.doc.size);
    double s = (double)g_parse_ns / 1e9;
    fmt_bytes(rate, sizeof rate, s > 0 ? (uint64_t)((double)A.doc.size / s) : 0);
    fmt_count(cnt, sizeof cnt, A.doc.count);
    fmt_bytes(out, sizeof out, ob_total());
    fprintf(stderr,
            "dcjson: parsed %s in %.3f s (%s/s), %s values, node table %.1f MiB\n"
            "dcjson: cpu user %.3f s, system %.3f s, peak rss %ld KiB\n"
            "dcjson: terminal output %s, %llu lines drawn, %llu scrolls\n",
            sz, s, rate, cnt, (double)A.doc.cap * sizeof(jnode_t) / 1048576.0,
            (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6,
            (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6, ru.ru_maxrss, out,
            (unsigned long long)g_lines_sent, (unsigned long long)g_scrolls);
}

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

int main(int argc, char **argv)
{
    char err[512];
    int r = cli_parse(argc, argv, &A.cfg, err, sizeof err);
    if (r == CLI_HELP) {
        fputs(cli_usage(), stdout);
        return 0;
    }
    if (r == CLI_VERSION) {
        puts("dcjson " DCJSON_VERSION);
        return 0;
    }
    if (r == CLI_ERROR) {
        fprintf(stderr, "dcjson: %s\nTry 'dcjson --help'.\n", err);
        return 2;
    }
    if (!A.cfg.no_config && cli_config(&A.cfg, A.cfg.config, err, sizeof err) != 0) {
        fprintf(stderr, "dcjson: %s\n", err);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const char *file = A.cfg.file && strcmp(A.cfg.file, "-") ? A.cfg.file : NULL;
    if (file && (has_suffix(file, ".jsonl") || has_suffix(file, ".ndjson") || has_suffix(file, ".jsonlines")))
        A.cfg.lines = 1;
    int batch = A.cfg.check || A.cfg.print || A.cfg.minify || A.cfg.path || !isatty(STDOUT_FILENO);
    if (!batch && !file && isatty(STDIN_FILENO)) {
        fprintf(stderr, "dcjson: no input: give a FILE or pipe JSON into dcjson\nTry 'dcjson --help'.\n");
        return 2;
    }
    if (open_input(file, err, sizeof err) != 0) {
        fprintf(stderr, "dcjson: %s\n", err);
        return 2;
    }
    A.name = file ? file : "<stdin>";
    const char *slash = file ? strrchr(file, '/') : NULL;
    if (slash && slash[1])
        A.name = slash + 1;
    json_init(&A.doc, A.cfg.lines, A.cfg.lenient, A.cfg.max_depth,
              A.cfg.expand < 0 ? 2u : (unsigned)A.cfg.expand);
    feed();
    A.view.d = &A.doc;
    A.view.closers = A.cfg.closers;
    A.guides = A.cfg.guides;
    A.indices = A.cfg.indices;
    A.theme = A.cfg.theme < 0 ? 0 : A.cfg.theme;
    A.clip.mode = A.cfg.clipboard;
    A.clip.command = A.cfg.copy_command;
    A.clip.max = A.cfg.clip_max;
    A.top = A.cur = ROW_NONE;

    r = batch ? run_batch() : run_viewer();
    if (A.cfg.stats)
        print_stats();
    render_free();
    json_free(&A.doc);
    free(A.tbuf);
    free(A.tline);
    free_input();
    return r;
}
