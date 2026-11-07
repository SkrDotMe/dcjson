/* util.c - small helpers */
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

void fmt_bytes(char *out, size_t cap, uint64_t v)
{
    static const char *const U[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    if (v < 1024) {
        snprintf(out, cap, "%u B", (unsigned)v);
        return;
    }
    double x = (double)v / 1024.0;
    int u = 0;
    while (x >= 1024.0 && u < 4) {
        x /= 1024.0;
        u++;
    }
    if (x < 10.0)
        snprintf(out, cap, "%.2f %s", x, U[u]);
    else if (x < 100.0)
        snprintf(out, cap, "%.1f %s", x, U[u]);
    else
        snprintf(out, cap, "%.0f %s", x, U[u]);
}

void fmt_count(char *out, size_t cap, uint64_t v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < cap; i++) {
        if (i && (n - i) % 3 == 0 && o + 2 < cap)
            out[o++] = ',';
        out[o++] = tmp[i];
    }
    if (cap)
        out[o < cap ? o : cap - 1] = '\0';
}

int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int sb_put(sbuf_t *s, const char *p, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 256;
        while (nc < s->len + n + 1)
            nc *= 2;
        char *q = realloc(s->p, nc);
        if (!q)
            return -1;
        s->p = q;
        s->cap = nc;
    }
    memcpy(s->p + s->len, p, n);
    s->len += n;
    s->p[s->len] = '\0';
    return 0;
}

int sb_str(sbuf_t *s, const char *z) { return sb_put(s, z, strlen(z)); }

void sb_free(sbuf_t *s)
{
    free(s->p);
    s->p = NULL;
    s->len = s->cap = 0;
}
