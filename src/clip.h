/* clip.h - copying to the clipboard, also through SSH (OSC 52) */
#ifndef DJ_CLIP_H
#define DJ_CLIP_H

#include <stddef.h>
#include <stdint.h>

#include "fmt.h"

enum { CLIP_AUTO, CLIP_OSC52, CLIP_TMUX, CLIP_SCREEN, CLIP_COMMAND, CLIP_FILE };

typedef struct {
    int         mode;    /* CLIP_*                                       */
    const char *command; /* CLIP_COMMAND: run with sh -c, data on stdin  */
    uint64_t    max;     /* larger copies go to a file; 0 = no limit     */
} clipcfg_t;

/* produces the data to copy; called twice (count, then write) */
typedef int (*clip_gen_fn)(void *ctx, sink_t *s);

/* Copy; what names the data for the status message ("value", "path").
 * Returns 0 (msg says where it went) or -1 (msg says why not). */
int         clip_copy(const clipcfg_t *c, clip_gen_fn gen, void *ctx, const char *what, char *msg,
                      size_t cap);
int         clip_mode_find(const char *name); /* -1 if unknown */
const char *clip_file_path(void);

/* for tests: write the escape sequence for data[0..n) in mode m */
void        clip_osc52(int mode, const char *data, size_t n);

#endif
