/* cli.h - command line and configuration file */
#ifndef DJ_CLI_H
#define DJ_CLI_H

#include <stddef.h>
#include <stdint.h>

#define DCJSON_VERSION "1.0.0"

typedef struct {
    const char *file;      /* NULL or "-" = standard input             */
    int         lines, lenient, check, print, minify, raw, stats;
    int         no_config, no_colors, ascii, no_mouse;
    int         expand;    /* levels expanded at start                 */
    int         closers, guides, indices;
    unsigned    max_depth, indent;
    const char *path;      /* --path: print this value and exit        */
    int         theme;     /* index, -1 = default                      */
    int         clipboard; /* CLIP_* (clip.h)                          */
    const char *copy_command;
    uint64_t    clip_max;
    const char *config;    /* --config FILE                            */
    uint64_t    set;       /* options given (bit per option id)        */
} config_t;

enum { CLI_RUN, CLI_HELP, CLI_VERSION, CLI_ERROR };

int         cli_parse(int argc, char **argv, config_t *c, char *err, size_t errcap);
const char *cli_usage(void);

/* Merge a configuration file; command line options win.  path NULL =
 * $XDG_CONFIG_HOME/dcjson/config or ~/.config/dcjson/config, where a
 * missing file is not an error.  Returns 0, or -1 with err set. */
int         cli_config(config_t *c, const char *path, char *err, size_t errcap);
void        cli_config_path(char *out, size_t cap);

/* Set `key = value` in the configuration file (NULL = default path): the
 * last active line with that key is replaced, otherwise the line is
 * appended; everything else stays as it is.  The file is replaced
 * atomically.  Returns 0, or -1 with err set. */
int         cli_config_save(const char *path, const char *key, const char *value, char *err,
                            size_t errcap);

#endif
