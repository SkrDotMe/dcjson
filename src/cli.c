/* cli.c - argument and configuration file parsing.
 *
 * The configuration file uses the long option names without dashes, one
 * per line: `theme = nord`, `closers = off`, `no-mouse`, `# comment`.
 * Both sources go through the same validation; command line options win
 * over the file, option by option. */
#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clip.h"
#include "theme.h"

enum { OP_LINES, OP_LENIENT, OP_CHECK, OP_PRINT, OP_MINIFY, OP_RAW, OP_PATH, OP_EXPAND, OP_CLOSERS,
       OP_GUIDES, OP_INDICES, OP_MAXDEPTH, OP_INDENT, OP_THEME, OP_NOCOL, OP_ASCII, OP_NOMOUSE, OP_CLIP,
       OP_COPYCMD, OP_CLIPMAX, OP_CONFIG, OP_NOCONFIG, OP_STATS, OP_HELP, OP_VERSION };

enum { A_FLAG, A_VALUE, A_BOOL };           /* argument kind          */
enum { W_CLI = 1, W_FILE = 2, W_BOTH = 3 }; /* where it is accepted   */

static const struct {
    const char *lng;
    char        shrt;
    uint8_t     id, arg, where;
} opts[] = {
    {"lines", 'l', OP_LINES, A_FLAG, W_CLI},           {"lenient", 0, OP_LENIENT, A_FLAG, W_BOTH},
    {"check", 'c', OP_CHECK, A_FLAG, W_CLI},           {"print", 'p', OP_PRINT, A_FLAG, W_CLI},
    {"minify", 'm', OP_MINIFY, A_FLAG, W_CLI},         {"raw", 'r', OP_RAW, A_FLAG, W_CLI},
    {"path", 'P', OP_PATH, A_VALUE, W_CLI},            {"expand", 'e', OP_EXPAND, A_VALUE, W_BOTH},
    {"closers", 0, OP_CLOSERS, A_BOOL, W_BOTH},        {"guides", 0, OP_GUIDES, A_BOOL, W_BOTH},
    {"indices", 0, OP_INDICES, A_BOOL, W_BOTH},        {"max-depth", 0, OP_MAXDEPTH, A_VALUE, W_BOTH},
    {"indent", 0, OP_INDENT, A_VALUE, W_BOTH},         {"theme", 0, OP_THEME, A_VALUE, W_BOTH},
    {"no-colors", 0, OP_NOCOL, A_FLAG, W_BOTH},        {"no-color", 0, OP_NOCOL, A_FLAG, W_BOTH},
    {"ascii", 0, OP_ASCII, A_FLAG, W_BOTH},            {"no-mouse", 0, OP_NOMOUSE, A_FLAG, W_BOTH},
    {"clipboard", 0, OP_CLIP, A_VALUE, W_BOTH},        {"copy-command", 0, OP_COPYCMD, A_VALUE, W_BOTH},
    {"clip-max", 0, OP_CLIPMAX, A_VALUE, W_BOTH},      {"config", 0, OP_CONFIG, A_VALUE, W_CLI},
    {"no-config", 0, OP_NOCONFIG, A_FLAG, W_CLI},      {"stats", 0, OP_STATS, A_FLAG, W_CLI},
    {"help", 'h', OP_HELP, A_FLAG, W_CLI},             {"version", 'V', OP_VERSION, A_FLAG, W_CLI},
};

#define SET(c, id) ((c)->set >> (id) & 1u)

const char *cli_usage(void)
{
    return "Usage: dcjson [OPTIONS] [FILE]\n"
           "Fast terminal JSON viewer. Reads FILE, or standard input when FILE is - or missing.\n"
           "\n"
           "Input:\n"
           "  -l, --lines           several top-level values (JSON Lines, concatenated JSON);\n"
           "                        automatic for *.jsonl and *.ndjson\n"
           "      --lenient         allow comments, trailing commas, raw control characters\n"
           "      --max-depth N     deepest nesting accepted (default 10000, max 65535)\n"
           "\n"
           "Viewer:\n"
           "  -e, --expand N        expand N levels at start (default: all levels for inputs\n"
           "                        under 64 KiB, otherwise 2)\n"
           "      --closers on|off  closing brackets on lines of their own (default on)\n"
           "      --guides on|off   indentation guides (default on)\n"
           "      --indices on|off  array indices before elements (default on)\n"
           "      --theme NAME      colour theme: default, nord, gruvbox, dracula, solarized,\n"
           "                        mono, light (the s key cycles and saves it)\n"
           "      --no-colors       monochrome (NO_COLOR is honoured as well)\n"
           "      --ascii           plain ASCII drawing\n"
           "      --no-mouse        leave the mouse to the terminal (text selection)\n"
           "\n"
           "Clipboard:\n"
           "      --clipboard MODE  auto, osc52, tmux, screen, command or file (default auto)\n"
           "      --copy-command C  command that reads the copy on stdin (mode command),\n"
           "                        e.g. 'wl-copy', 'xclip -selection clipboard', 'pbcopy'\n"
           "      --clip-max SIZE   larger OSC 52 copies go to a file instead (default 1M,\n"
           "                        0 = no limit)\n"
           "\n"
           "Without the viewer:\n"
           "  -c, --check           only validate; errors as FILE:LINE:COL: message\n"
           "  -p, --print           pretty-print (default when stdout is not a terminal)\n"
           "  -m, --minify          print without white space\n"
           "      --indent N        indentation for --print and copies (default 2)\n"
           "  -P, --path PATH       print the value at PATH, e.g. '.users[0].name'\n"
           "  -r, --raw             print strings without quotes and escapes\n"
           "\n"
           "Other:\n"
           "      --config FILE     another configuration file (--no-config: none)\n"
           "      --stats           print timing and resource use at exit\n"
           "  -h, --help            this help\n"
           "  -V, --version         version\n"
           "\n"
           "Exit status: 0 success, 1 invalid JSON or path not found, 2 usage or I/O error.\n";
}

static int num(const char *s, long lo, long hi, long *out)
{
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (errno || e == s || *e || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static int size_val(const char *s, uint64_t *out)
{
    char *e;
    errno = 0;
    unsigned long long v = strtoull(s, &e, 10);
    if (errno || e == s || *s == '-')
        return -1;
    int sh = 0;
    switch (*e | 32) {
    case 'k': sh = 10; e++; break;
    case 'm': sh = 20; e++; break;
    case 'g': sh = 30; e++; break;
    default: break;
    }
    if (sh && (*e | 32) == 'i')
        e++;
    if ((*e | 32) == 'b')
        e++;
    if (*e || v > (UINT64_MAX >> sh))
        return -1;
    *out = (uint64_t)v << sh;
    return 0;
}

static int boolval(const char *v)
{
    static const char *const yes[] = {"1", "yes", "true", "on"}, *const no[] = {"0", "no", "false", "off"};
    if (!v)
        return 1;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(v, yes[i]))
            return 1;
        if (!strcmp(v, no[i]))
            return 0;
    }
    return -1;
}

#define FAIL(...)                           \
    do {                                    \
        snprintf(err, errcap, __VA_ARGS__); \
        return -1;                          \
    } while (0)

/* Apply one option; src 0 = command line, 1 = file. */
static int apply(config_t *c, int id, int argk, const char *val, const char *shown, int src, char *err,
                 size_t errcap)
{
    long v;
    int b = 1;
    if (argk == A_BOOL || (argk == A_FLAG && src == 1)) {
        b = boolval(val);
        if (b < 0)
            FAIL("%s: expected on or off, not '%.40s'", shown, val);
    }
    c->set |= 1ull << id;
    switch (id) {
    case OP_LINES: c->lines = b; break;
    case OP_LENIENT: c->lenient = b; break;
    case OP_CHECK: c->check = b; break;
    case OP_PRINT: c->print = b; break;
    case OP_MINIFY: c->minify = b; break;
    case OP_RAW: c->raw = b; break;
    case OP_STATS: c->stats = b; break;
    case OP_NOCOL: c->no_colors = b; break;
    case OP_ASCII: c->ascii = b; break;
    case OP_NOMOUSE: c->no_mouse = b; break;
    case OP_NOCONFIG: c->no_config = b; break;
    case OP_CLOSERS: c->closers = b; break;
    case OP_GUIDES: c->guides = b; break;
    case OP_INDICES: c->indices = b; break;
    case OP_PATH: c->path = val; break;
    case OP_CONFIG: c->config = val; break;
    case OP_COPYCMD: c->copy_command = val; break;
    case OP_EXPAND:
        if (num(val, 0, 65535, &v) != 0)
            FAIL("%s: expected a number 0 ... 65535, not '%.40s'", shown, val);
        c->expand = (int)v;
        break;
    case OP_MAXDEPTH:
        if (num(val, 1, 65535, &v) != 0)
            FAIL("%s: expected a number 1 ... 65535, not '%.40s'", shown, val);
        c->max_depth = (unsigned)v;
        break;
    case OP_INDENT:
        if (num(val, 0, 16, &v) != 0)
            FAIL("%s: expected a number 0 ... 16, not '%.40s'", shown, val);
        c->indent = (unsigned)v;
        break;
    case OP_THEME:
        c->theme = theme_find(val);
        if (c->theme < 0)
            FAIL("%s: unknown theme '%.40s' (themes: %s)", shown, val, theme_names());
        break;
    case OP_CLIP:
        c->clipboard = clip_mode_find(val);
        if (c->clipboard < 0)
            FAIL("%s: expected auto, osc52, tmux, screen, command or file, not '%.40s'", shown, val);
        break;
    case OP_CLIPMAX:
        if (size_val(val, &c->clip_max) != 0)
            FAIL("%s: expected a size such as 512K or 4M, not '%.40s'", shown, val);
        break;
    default: break;
    }
    return 0;
}

static void defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    c->expand = -1;
    c->closers = c->guides = c->indices = 1;
    c->max_depth = 10000;
    c->indent = 2;
    c->theme = -1;
    c->clipboard = CLIP_AUTO;
    c->clip_max = 1u << 20;
}

int cli_parse(int argc, char **argv, config_t *c, char *err, size_t errcap)
{
    defaults(c);
    int help = 0, version = 0, dashdash = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val = NULL;
        int id = -1, kind = A_FLAG;
        char shown[64];

        if (dashdash || a[0] != '-' || !a[1]) {
            if (c->file) {
                snprintf(err, errcap, "only one input file can be given ('%.60s')", a);
                return CLI_ERROR;
            }
            c->file = a;
            continue;
        }
        if (!strcmp(a, "--")) {
            dashdash = 1;
            continue;
        }
        if (a[1] == '-') {
            const char *name = a + 2, *eq = strchr(name, '=');
            size_t nl = eq ? (size_t)(eq - name) : strlen(name);
            for (size_t k = 0; k < sizeof opts / sizeof opts[0]; k++)
                if ((opts[k].where & W_CLI) && strlen(opts[k].lng) == nl && !strncmp(opts[k].lng, name, nl)) {
                    id = opts[k].id;
                    kind = opts[k].arg;
                }
            snprintf(shown, sizeof shown, "--%.*s", (int)(nl > 40 ? 40 : nl), name);
            if (id < 0) {
                snprintf(err, errcap, "unknown option '%s'", shown);
                return CLI_ERROR;
            }
            if (eq) {
                if (kind == A_FLAG) {
                    snprintf(err, errcap, "option '%s' does not take a value", shown);
                    return CLI_ERROR;
                }
                val = eq + 1;
            }
        } else {
            if (a[2]) {
                snprintf(err, errcap, "unknown option '%.40s' (give short options one by one)", a);
                return CLI_ERROR;
            }
            for (size_t k = 0; k < sizeof opts / sizeof opts[0]; k++)
                if (opts[k].shrt == a[1]) {
                    id = opts[k].id;
                    kind = opts[k].arg;
                }
            snprintf(shown, sizeof shown, "-%c", a[1]);
            if (id < 0) {
                snprintf(err, errcap, "unknown option '%s'", shown);
                return CLI_ERROR;
            }
        }
        if (kind != A_FLAG && !val) {
            if (i + 1 >= argc) {
                snprintf(err, errcap, "option '%s' requires a value", shown);
                return CLI_ERROR;
            }
            val = argv[++i];
        }
        if (id == OP_HELP) {
            help = 1;
            continue;
        }
        if (id == OP_VERSION) {
            version = 1;
            continue;
        }
        if (apply(c, id, kind == A_BOOL ? A_BOOL : A_VALUE, val, shown, 0, err, errcap) != 0)
            return CLI_ERROR;
    }
    if (help)
        return CLI_HELP;
    if (version)
        return CLI_VERSION;
    if (c->config && c->no_config) {
        snprintf(err, errcap, "--config and --no-config cannot be combined");
        return CLI_ERROR;
    }
    if (c->print + c->minify + c->check > 1) {
        snprintf(err, errcap, "--check, --print and --minify cannot be combined");
        return CLI_ERROR;
    }
    if (c->raw && !c->path) {
        snprintf(err, errcap, "--raw needs --path");
        return CLI_ERROR;
    }
    return CLI_RUN;
}

void cli_config_path(char *out, size_t cap)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && x[0] == '/')
        snprintf(out, cap, "%s/dcjson/config", x);
    else if (h && *h)
        snprintf(out, cap, "%s/.config/dcjson/config", h);
    else
        out[0] = '\0';
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

int cli_config(config_t *c, const char *path, char *err, size_t errcap)
{
    static char store[16384]; /* the strings of the file stay referenced */
    char def[512];
    int explicit = path != NULL;
    if (!path) {
        cli_config_path(def, sizeof def);
        if (!def[0])
            return 0;
        path = def;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (!explicit && errno == ENOENT)
            return 0;
        FAIL("cannot read configuration file %.200s: %s", path, strerror(errno));
    }
    ssize_t n = read(fd, store, sizeof store);
    close(fd);
    if (n < 0)
        FAIL("cannot read configuration file %.200s: %s", path, strerror(errno));
    if ((size_t)n >= sizeof store)
        FAIL("configuration file %.200s is too large (max %zu bytes)", path, sizeof store - 1);
    store[n] = '\0';

    config_t f;
    defaults(&f);
    unsigned lineno = 0;
    char msg[200];
    for (char *line = store; line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        lineno++;
        char *s = trim(line);
        line = nl ? nl + 1 : NULL;
        if (!*s || *s == '#')
            continue;
        char *val = NULL, *eq = strchr(s, '=');
        if (eq) {
            *eq = '\0';
            val = trim(eq + 1);
            char *hash = strstr(val, " #"); /* trailing comment */
            if (hash)
                *hash = '\0', val = trim(val);
            size_t vl = strlen(val);
            if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl - 1] == val[0]) {
                val[vl - 1] = '\0';
                val++;
            }
        } else {
            char *hash = strstr(s, " #");
            if (hash)
                *hash = '\0';
        }
        char *key = trim(s);
        if (key[0] == '-' && key[1] == '-')
            key += 2;
        int k = -1;
        for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++)
            if ((opts[i].where & W_FILE) && !strcmp(opts[i].lng, key))
                k = (int)i;
        if (k < 0)
            FAIL("%.200s:%u: unknown option '%.40s'", path, lineno, key);
        if (opts[k].arg == A_VALUE && (!val || !*val))
            FAIL("%.200s:%u: option '%s' requires a value", path, lineno, key);
        if (apply(&f, opts[k].id, opts[k].arg, val, key, 1, msg, sizeof msg) != 0)
            FAIL("%.200s:%u: %s", path, lineno, msg);
    }

#define TAKE(id, field)                       \
    do {                                      \
        if (SET(&f, id) && !SET(c, id))       \
            c->field = f.field;               \
    } while (0)
    TAKE(OP_LENIENT, lenient);
    TAKE(OP_EXPAND, expand);
    TAKE(OP_CLOSERS, closers);
    TAKE(OP_GUIDES, guides);
    TAKE(OP_INDICES, indices);
    TAKE(OP_MAXDEPTH, max_depth);
    TAKE(OP_INDENT, indent);
    TAKE(OP_THEME, theme);
    TAKE(OP_NOCOL, no_colors);
    TAKE(OP_ASCII, ascii);
    TAKE(OP_NOMOUSE, no_mouse);
    TAKE(OP_CLIP, clipboard);
    TAKE(OP_COPYCMD, copy_command);
    TAKE(OP_CLIPMAX, clip_max);
#undef TAKE
    c->set |= f.set;
    return 0;
}

/* the key of an active configuration line, without dashes; length 0 otherwise */
static size_t line_key(const char *s, size_t len, const char **key)
{
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i < len && s[i] == '#')
        return 0;
    if (i + 1 < len && s[i] == '-' && s[i + 1] == '-')
        i += 2;
    size_t b = i;
    while (i < len && s[i] != '=' && s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
        i++;
    *key = s + b;
    return i - b;
}

static int mkdirs(char *path) /* the parent directories of path */
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        int r = mkdir(path, 0755);
        *p = '/';
        if (r != 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}

int cli_config_save(const char *path, const char *key, const char *value, char *err, size_t errcap)
{
    static char in[16384], out[16384 + 256];
    char def[512], tmp[560];
    if (!path) {
        cli_config_path(def, sizeof def);
        if (!def[0])
            FAIL("no configuration file path (HOME is not set)");
        path = def;
    }
    size_t n = 0;
    mode_t mode = 0644;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0)
            mode = st.st_mode & 07777;
        ssize_t r = read(fd, in, sizeof in);
        close(fd);
        if (r < 0)
            FAIL("cannot read %.200s: %s", path, strerror(errno));
        if ((size_t)r >= sizeof in)
            FAIL("%.200s is too large", path);
        n = (size_t)r;
    } else if (errno != ENOENT) {
        FAIL("cannot read %.200s: %s", path, strerror(errno));
    }

    size_t klen = strlen(key), hit = (size_t)-1, hit_end = 0;
    for (size_t p = 0; p < n;) {
        const char *e = memchr(in + p, '\n', n - p);
        size_t len = e ? (size_t)(e - (in + p)) : n - p;
        const char *k = NULL;
        size_t kl = line_key(in + p, len, &k);
        if (kl == klen && !memcmp(k, key, klen)) {
            hit = p;
            hit_end = p + len;
        }
        p += len + (e != NULL);
    }
    char line[160];
    int ll = snprintf(line, sizeof line, "%s = %s", key, value);
    if (ll < 0 || (size_t)ll >= sizeof line)
        FAIL("value too long");
    size_t o;
    if (hit != (size_t)-1) {
        if (hit_end > hit && in[hit_end - 1] == '\r')
            hit_end--;
        memcpy(out, in, hit);
        o = hit;
        memcpy(out + o, line, (size_t)ll);
        o += (size_t)ll;
        memcpy(out + o, in + hit_end, n - hit_end);
        o += n - hit_end;
    } else {
        memcpy(out, in, n);
        o = n;
        if (o && out[o - 1] != '\n')
            out[o++] = '\n';
        memcpy(out + o, line, (size_t)ll);
        o += (size_t)ll;
        out[o++] = '\n';
    }

    snprintf(tmp, sizeof tmp, "%s", path);
    if (mkdirs(tmp) != 0)
        FAIL("cannot create the directory of %.200s: %s", path, strerror(errno));
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        FAIL("cannot write %.200s: %s", tmp, strerror(errno));
    size_t done = 0;
    while (done < o) {
        ssize_t w = write(fd, out + done, o - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            int e = w < 0 ? errno : EIO;
            close(fd);
            unlink(tmp);
            FAIL("cannot write %.200s: %s", tmp, strerror(e));
        }
        done += (size_t)w;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        FAIL("cannot replace %.200s: %s", path, strerror(e));
    }
    return 0;
}
