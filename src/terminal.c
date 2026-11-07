/* terminal.c - terminal state handling (from dcmon).
 *
 * Keyboard signals (ISIG) stay enabled, so Ctrl-C / Ctrl-Z arrive as
 * signals which the main loop receives through a signalfd.  Crashing
 * signals restore the terminal with async-signal-safe calls only. */
#include "terminal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int g_out_fd = STDOUT_FILENO;
int g_in_fd = STDIN_FILENO;

static struct termios saved;
static volatile sig_atomic_t have_saved, active;
static char obuf[65536];
static size_t olen;
static uint64_t ototal;
static int odead;

/* alternate screen, hide cursor, no autowrap, bracketed paste */
static const char ENTER[] = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?2004h\x1b[0m\x1b[H\x1b[2J";
static const char MOUSE_ON[] = "\x1b[?1000h\x1b[?1006h";
static const char LEAVE[] = "\x1b[?1000l\x1b[?1006l\x1b[?2004l\x1b[0m\x1b[r\x1b[?7h\x1b[?25h\x1b[?1049l";

static void raw_write(const char *s, size_t n)
{
    while (n && !odead) {
        ssize_t r = write(g_out_fd, s, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = {g_out_fd, POLLOUT, 0};
                if (poll(&p, 1, 5000) > 0)
                    continue;
            }
            odead = 1;
            return;
        }
        s += r;
        n -= (size_t)r;
        ototal += (uint64_t)r;
    }
}

void ob_flush(void)
{
    if (olen) {
        raw_write(obuf, olen);
        olen = 0;
    }
}

void ob_write(const char *p, size_t n)
{
    if (n > sizeof obuf - olen) {
        ob_flush();
        if (n > sizeof obuf) {
            raw_write(p, n);
            return;
        }
    }
    memcpy(obuf + olen, p, n);
    olen += n;
}

void ob_str(const char *s) { ob_write(s, strlen(s)); }

uint64_t ob_total(void) { return ototal + olen; }

int term_dead(void) { return odead; }

int term_open(int mouse)
{
    if (!have_saved) {
        if (tcgetattr(g_in_fd, &saved) != 0)
            return -1;
        have_saved = 1;
    }
    struct termios t = saved;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN);
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | INLCR);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(g_in_fd, TCSANOW, &t) != 0)
        return -1;
    active = 1;
    raw_write(ENTER, sizeof ENTER - 1);
    if (mouse)
        raw_write(MOUSE_ON, sizeof MOUSE_ON - 1);
    return 0;
}

void term_close(void)
{
    if (!active)
        return;
    ob_flush();
    raw_write(LEAVE, sizeof LEAVE - 1);
    tcsetattr(g_in_fd, TCSANOW, &saved);
    active = 0;
}

int term_size(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(g_out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
    *cols = 80;
    *rows = 24;
    return -1;
}

#if !defined(__SANITIZE_ADDRESS__)
static void fatal_handler(int sig)
{
    if (active) {
        ssize_t r = write(g_out_fd, LEAVE, sizeof LEAVE - 1);
        (void)r;
        tcsetattr(g_in_fd, TCSANOW, &saved);
        active = 0;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void term_install_fatal_handlers(void)
{
#if !defined(__SANITIZE_ADDRESS__) /* keep sanitizer reports intact */
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fatal_handler;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
#endif
}
