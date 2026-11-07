/* clip.c - clipboard.
 *
 * OSC 52 (ESC ] 52 ; c ; base64 BEL) asks the terminal emulator itself to
 * set the clipboard, so it works from a remote machine over SSH without X
 * forwarding.  The data is base64-encoded straight into the output buffer
 * in small blocks: a copy never needs a second full-size buffer.  Inside
 * tmux the sequence is sent both plainly (tmux handles it with
 * `set-clipboard on`) and wrapped in a DCS passthrough (`allow-passthrough
 * on`); inside GNU screen it is wrapped in DCS in 768-byte pieces. */
#include "clip.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "terminal.h"
#include "util.h"

static const char *const NAMES[] = {"auto", "osc52", "tmux", "screen", "command", "file"};

int clip_mode_find(const char *name)
{
    for (int i = 0; i < (int)ARRAY_LEN(NAMES); i++)
        if (!strcmp(name, NAMES[i]))
            return i;
    return -1;
}

/* ---- base64 into the terminal output buffer --------------------------- */

typedef struct {
    sink_t        s;
    unsigned char rem[3];
    int           nrem, screen;
    size_t        col; /* encoded bytes in the current screen piece */
    size_t        olen;
    char          out[4096];
} b64_t;

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_flush(b64_t *b)
{
    if (!b->screen) {
        ob_write(b->out, b->olen);
    } else { /* screen limits a DCS string: split into pieces */
        size_t i = 0;
        while (i < b->olen) {
            size_t k = 768 - b->col;
            if (k > b->olen - i)
                k = b->olen - i;
            ob_write(b->out + i, k);
            i += k;
            b->col += k;
            if (b->col == 768) {
                ob_str("\x1b\\\x1bP");
                b->col = 0;
            }
        }
    }
    b->olen = 0;
}

static void b64_quad(b64_t *b, const unsigned char *t, int n)
{
    if (b->olen + 4 > sizeof b->out)
        b64_flush(b);
    uint32_t v = (uint32_t)t[0] << 16 | (n > 1 ? (uint32_t)t[1] << 8 : 0) | (n > 2 ? t[2] : 0);
    char *o = b->out + b->olen;
    o[0] = B64[v >> 18 & 63];
    o[1] = B64[v >> 12 & 63];
    o[2] = n > 1 ? B64[v >> 6 & 63] : '=';
    o[3] = n > 2 ? B64[v & 63] : '=';
    b->olen += 4;
}

static void b64_put(sink_t *s, const char *p, size_t n)
{
    b64_t *b = s->ctx;
    const unsigned char *u = (const unsigned char *)p;
    while (n && b->nrem) {
        b->rem[b->nrem++] = *u++;
        n--;
        if (b->nrem == 3) {
            b64_quad(b, b->rem, 3);
            b->nrem = 0;
        }
    }
    while (n >= 3) {
        b64_quad(b, u, 3);
        u += 3;
        n -= 3;
    }
    while (n--)
        b->rem[b->nrem++] = *u++;
}

static void b64_init(b64_t *b, int screen)
{
    memset(b, 0, sizeof *b);
    b->s.put = b64_put;
    b->s.ctx = b;
    b->screen = screen;
}

static void b64_end(b64_t *b)
{
    if (b->nrem)
        b64_quad(b, b->rem, b->nrem);
    b->nrem = 0;
    b64_flush(b);
}

static void osc_begin(int mode)
{
    if (mode == CLIP_TMUX)
        ob_str("\x1bPtmux;\x1b\x1b]52;c;");
    else if (mode == CLIP_SCREEN)
        ob_str("\x1bP\x1b]52;c;");
    else
        ob_str("\x1b]52;c;");
}

static void osc_end(int mode)
{
    ob_str(mode == CLIP_TMUX || mode == CLIP_SCREEN ? "\x07\x1b\\" : "\x07");
}

static void osc_send(int mode, clip_gen_fn gen, void *ctx)
{
    b64_t b;
    b64_init(&b, mode == CLIP_SCREEN);
    osc_begin(mode);
    gen(ctx, &b.s);
    b64_end(&b);
    osc_end(mode);
    ob_flush();
}

typedef struct {
    const char *p;
    size_t      n;
} mem_t;

static int mem_gen(void *ctx, sink_t *s)
{
    mem_t *m = ctx;
    sink_put(s, m->p, m->n);
    return 0;
}

void clip_osc52(int mode, const char *data, size_t n)
{
    mem_t m = {data, n};
    osc_send(mode, mem_gen, &m);
}

/* ---- command and file ------------------------------------------------- */

const char *clip_file_path(void)
{
    static char path[512];
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || dir[0] != '/')
        dir = getenv("TMPDIR");
    if (!dir || dir[0] != '/')
        dir = "/tmp";
    snprintf(path, sizeof path, "%s/dcjson-clipboard-%u.json", dir, (unsigned)getuid());
    return path;
}

static int to_file(clip_gen_fn gen, void *ctx, char *err, size_t cap)
{
    const char *path = clip_file_path();
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        snprintf(err, cap, "cannot write %.200s: %s", path, strerror(errno));
        return -1;
    }
    fdsink_t *f = malloc(sizeof *f);
    if (!f) {
        close(fd);
        snprintf(err, cap, "out of memory");
        return -1;
    }
    fd_sink_init(f, fd);
    gen(ctx, &f->s);
    int r = fd_sink_flush(f);
    free(f);
    if (close(fd) != 0 || r != 0) {
        snprintf(err, cap, "cannot write %.200s", path);
        return -1;
    }
    return 0;
}

static int to_command(const char *cmd, clip_gen_fn gen, void *ctx, char *err, size_t cap)
{
    if (!cmd || !*cmd) {
        snprintf(err, cap, "no copy-command is set");
        return -1;
    }
    int p[2];
    if (pipe(p) != 0) {
        snprintf(err, cap, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        snprintf(err, cap, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(p[0], 0);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        close(p[0]);
        close(p[1]);
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    fdsink_t *f = malloc(sizeof *f);
    int r = -1;
    if (f) {
        fd_sink_init(f, p[1]);
        gen(ctx, &f->s);
        r = fd_sink_flush(f);
        free(f);
    }
    close(p[1]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (r != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(err, cap, "copy-command failed (%.60s)", cmd);
        return -1;
    }
    return 0;
}

int clip_copy(const clipcfg_t *c, clip_gen_fn gen, void *ctx, const char *what, char *msg, size_t cap)
{
    sink_t count = {NULL, 0, NULL};
    if (gen(ctx, &count) != 0) {
        snprintf(msg, cap, "the %s is not complete yet (still parsing)", what);
        return -1;
    }
    char sz[32], err[256];
    fmt_bytes(sz, sizeof sz, count.total);
    int mode = c->mode;
    if (mode == CLIP_AUTO)
        mode = getenv("TMUX") ? CLIP_TMUX : getenv("STY") ? CLIP_SCREEN : CLIP_OSC52;
    int osc = mode == CLIP_OSC52 || mode == CLIP_TMUX || mode == CLIP_SCREEN;
    if (osc && c->max && count.total > c->max) {
        char mx[32];
        fmt_bytes(mx, sizeof mx, c->max);
        if (to_file(gen, ctx, err, sizeof err) != 0) {
            snprintf(msg, cap, "%s is larger than clip-max %s, and %s", sz, mx, err);
            return -1;
        }
        snprintf(msg, cap, "%s %s is larger than clip-max %s: written to %s", what, sz, mx,
                 clip_file_path());
        return 0;
    }
    switch (mode) {
    case CLIP_COMMAND:
        if (to_command(c->command, gen, ctx, err, sizeof err) != 0) {
            snprintf(msg, cap, "%s", err);
            return -1;
        }
        snprintf(msg, cap, "copied %s, %s (copy-command)", what, sz);
        return 0;
    case CLIP_FILE:
        if (to_file(gen, ctx, err, sizeof err) != 0) {
            snprintf(msg, cap, "%s", err);
            return -1;
        }
        snprintf(msg, cap, "%s, %s, written to %s", what, sz, clip_file_path());
        return 0;
    case CLIP_TMUX:
        if (c->mode == CLIP_AUTO) /* tmux with set-clipboard on */
            osc_send(CLIP_OSC52, gen, ctx);
        osc_send(CLIP_TMUX, gen, ctx);
        snprintf(msg, cap, "copied %s, %s (OSC 52 via tmux)", what, sz);
        return 0;
    default:
        osc_send(mode, gen, ctx);
        snprintf(msg, cap, "copied %s, %s (OSC 52%s)", what, sz, mode == CLIP_SCREEN ? " via screen" : "");
        return 0;
    }
}
