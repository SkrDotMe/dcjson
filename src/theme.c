/* theme.c - colour themes.
 *
 * A theme is seventeen colours, each given twice: as RGB for true-colour
 * terminals and as an xterm 256-colour index (indices 0-15 are avoided
 * because terminals redefine them).  16-colour terminals keep their own
 * palette: there a theme only picks which of the basic colours to use.
 * Without colours (CM_NONE) the cursor line is drawn in reverse video and
 * search matches are underlined.  The escape strings are built once per
 * theme change. */
#include "theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { K_TEXT, K_DIM, K_BORDER, K_TITLE, K_KEY, K_STR, K_ESC, K_NUM, K_BOOL, K_NULL, K_PUNCT,
       K_INFO, K_GUIDE, K_CURBG, K_MATCHBG, K_ALERT, K_GOOD, K_COUNT };

enum { P_DARK, P_MONO, P_LIGHT }; /* 16-colour variants */

typedef struct {
    const char *name;
    uint32_t    rgb[K_COUNT];
    uint8_t     x256[K_COUNT];
    int         p16;
} theme_t;

/* order: text dim border title key string escape number bool null punct
 *        info guide cursor-bg match-bg alert good */
static const theme_t THEMES[] = {
    {"default",
     {0xd0d0d0, 0x8a8a8a, 0x585858, 0xeeeeee, 0x5fafff, 0x87d787, 0x5fd7d7, 0xffd75f, 0xff875f,
      0xaf87ff, 0x8a8a8a, 0x6c6c6c, 0x3a3a3a, 0x303030, 0xffd75f, 0xff5f5f, 0x87d787},
     {252, 245, 240, 255, 75, 114, 80, 221, 209, 141, 245, 242, 237, 236, 221, 203, 114},
     P_DARK},
    /* https://www.nordtheme.com/docs/colors-and-palettes */
    {"nord",
     {0xd8dee9, 0x7b88a1, 0x4c566a, 0xeceff4, 0x88c0d0, 0xa3be8c, 0x8fbcbb, 0xb48ead, 0xd08770,
      0x81a1c1, 0x7b88a1, 0x616e88, 0x3b4252, 0x434c5e, 0xebcb8b, 0xbf616a, 0xa3be8c},
     {253, 103, 240, 255, 110, 150, 109, 139, 173, 67, 103, 60, 237, 239, 222, 131, 150},
     P_DARK},
    /* gruvbox dark, bright accents; 256 indices from gruvbox.vim */
    {"gruvbox",
     {0xebdbb2, 0x928374, 0x665c54, 0xfbf1c7, 0x83a598, 0xb8bb26, 0x8ec07c, 0xd3869b, 0xfe8019,
      0xfabd2f, 0x928374, 0x7c6f64, 0x3c3836, 0x504945, 0xfabd2f, 0xfb4934, 0xb8bb26},
     {223, 245, 241, 229, 109, 142, 108, 175, 208, 214, 245, 243, 237, 239, 214, 167, 142},
     P_DARK},
    /* https://draculatheme.com/contribute; 256 indices from dracula/vim */
    {"dracula",
     {0xf8f8f2, 0x8a93b8, 0x6272a4, 0xffffff, 0x8be9fd, 0xf1fa8c, 0x50fa7b, 0xbd93f9, 0xffb86c,
      0xff79c6, 0x8a93b8, 0x6272a4, 0x383a46, 0x44475a, 0xf1fa8c, 0xff5555, 0x50fa7b},
     {253, 103, 61, 231, 117, 228, 84, 141, 215, 212, 103, 61, 237, 239, 228, 203, 84},
     P_DARK},
    /* Solarized dark: body base0, secondary base00, borders base01,
     * emphasis base1, cursor line base02 */
    {"solarized",
     {0x839496, 0x657b83, 0x586e75, 0x93a1a1, 0x268bd2, 0x2aa198, 0x6c71c4, 0xd33682, 0xcb4b16,
      0xb58900, 0x657b83, 0x586e75, 0x2b4b55, 0x073642, 0xb58900, 0xdc322f, 0x859900},
     {244, 241, 240, 245, 33, 37, 61, 125, 166, 136, 241, 240, 238, 235, 136, 160, 64},
     P_DARK},
    {"mono",
     {0xd0d0d0, 0x8a8a8a, 0x585858, 0xffffff, 0xffffff, 0xbcbcbc, 0x8a8a8a, 0xd0d0d0, 0xd0d0d0,
      0x8a8a8a, 0x8a8a8a, 0x6c6c6c, 0x3a3a3a, 0x444444, 0xeeeeee, 0xffffff, 0xd0d0d0},
     {252, 245, 240, 231, 231, 250, 245, 252, 252, 245, 245, 242, 237, 238, 255, 231, 252},
     P_MONO},
    {"light", /* for terminals with a light background */
     {0x303030, 0x6c6c6c, 0xa8a8a8, 0x000000, 0x005fd7, 0x5f8700, 0x008787, 0x8700af, 0xd75f00,
      0xaf0087, 0x6c6c6c, 0x8a8a8a, 0xd0d0d0, 0xe4e4e4, 0xffd75f, 0xd70000, 0x5f8700},
     {236, 242, 248, 16, 26, 64, 30, 91, 166, 126, 242, 245, 252, 254, 221, 160, 64},
     P_LIGHT},
};
#define NTHEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

/* SGR parameters of the 16-colour variants, per K_* (CURBG and MATCHBG are
 * background codes; "7" is reverse video) */
static const char *const P16[][K_COUNT] = {
    [P_DARK] = {"37", "90", "90", "97", "94", "32", "36", "33", "91", "35", "90", "90", "90", "100",
                "43", "31", "32"},
    [P_MONO] = {"37", "90", "90", "97", "97", "37", "90", "37", "37", "90", "90", "90", "90", "7",
                "7", "97", "37"},
    [P_LIGHT] = {"30", "90", "37", "30", "34", "32", "36", "35", "31", "35", "90", "90", "37", "47",
                 "43", "31", "32"},
};

/* slot -> theme colour, and which slots are bold */
static const uint8_t KEY[C_COUNT] = {
    [C_TEXT] = K_TEXT,   [C_DIM] = K_DIM,    [C_BORDER] = K_BORDER, [C_TITLE] = K_TITLE,
    [C_KEY] = K_KEY,     [C_STR] = K_STR,    [C_ESC] = K_ESC,       [C_NUM] = K_NUM,
    [C_BOOL] = K_BOOL,   [C_NULL] = K_NULL,  [C_PUNCT] = K_PUNCT,   [C_INFO] = K_INFO,
    [C_GUIDE] = K_GUIDE, [C_MATCH] = K_TEXT, [C_ALERT] = K_ALERT,   [C_GOOD] = K_GOOD,
};
static const uint32_t BOLD = 1u << C_TITLE | 1u << C_ALERT | 1u << C_MATCH;

int theme_count(void) { return NTHEMES; }

const char *theme_name(int t) { return t >= 0 && t < NTHEMES ? THEMES[t].name : "default"; }

int theme_find(const char *name)
{
    for (int i = 0; i < NTHEMES; i++)
        if (!strcmp(name, THEMES[i].name))
            return i;
    return -1;
}

const char *theme_names(void)
{
    static char buf[128];
    if (!buf[0]) {
        size_t n = 0;
        for (int i = 0; i < NTHEMES && n < sizeof buf; i++)
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s", i ? ", " : "", THEMES[i].name);
    }
    return buf;
}

static void rgb(char *o, size_t cap, int fgbg, uint32_t v)
{
    snprintf(o, cap, "%d;2;%u;%u;%u", fgbg, (unsigned)(v >> 16 & 255), (unsigned)(v >> 8 & 255),
             (unsigned)(v & 255));
}

static char g_fgonly[C_COUNT][128]; /* foreground change only */

const char *theme_sgr(int t, int m, int c, int hl);

static void build(char *o, size_t cap, const theme_t *th, int m, int s, int hl)
{
    char fg[96], bg[64] = "";
    const char *attr = (BOLD >> s & 1u) ? "1;" : "";
    if (s == C_MATCH) { /* black text on the match colour */
        if (m == CM_TRUE) {
            rgb(bg, sizeof bg, 48, th->rgb[K_MATCHBG]);
            snprintf(fg, sizeof fg, "38;2;0;0;0;%s", bg);
        } else if (m == CM_256) {
            snprintf(fg, sizeof fg, "38;5;16;48;5;%u", (unsigned)th->x256[K_MATCHBG]);
        } else {
            snprintf(fg, sizeof fg, "30;%s", P16[th->p16][K_MATCHBG]);
        }
        snprintf(o, cap, "\x1b[0;%s%sm", attr, fg);
        return;
    }
    char b2[48];
    if (m == CM_TRUE) {
        rgb(fg, sizeof fg, 38, th->rgb[KEY[s]]);
        if (hl) {
            rgb(b2, sizeof b2, 48, th->rgb[K_CURBG]);
            snprintf(bg, sizeof bg, ";%s", b2);
        }
    } else if (m == CM_256) {
        snprintf(fg, sizeof fg, "38;5;%u", (unsigned)th->x256[KEY[s]]);
        if (hl)
            snprintf(bg, sizeof bg, ";48;5;%u", (unsigned)th->x256[K_CURBG]);
    } else {
        snprintf(fg, sizeof fg, "%s", P16[th->p16][KEY[s]]);
        if (hl)
            snprintf(bg, sizeof bg, ";%s", P16[th->p16][K_CURBG]);
    }
    snprintf(o, cap, "\x1b[0;%s%s%sm", attr, fg, bg);
    snprintf(g_fgonly[s], sizeof g_fgonly[s], "\x1b[%sm", fg);
}

const char *theme_fg(int t, int m, int c)
{
    if (m == CM_NONE)
        return "";
    theme_sgr(t, m, c, 0); /* builds the tables */
    return g_fgonly[c];
}

int theme_bold(int c) { return c >= 0 && c < C_COUNT && (BOLD >> c & 1u); }

const char *theme_sgr(int t, int m, int c, int hl)
{
    static char sgr[2][C_COUNT][128];
    static int ct = -1, cm = -1;
    if (c < 0 || c >= C_COUNT)
        c = C_TEXT;
    hl = hl != 0;
    if (m == CM_NONE) {
        if (c == C_MATCH)
            return hl ? "\x1b[0;4;7m" : "\x1b[0;4m";
        return hl ? "\x1b[0;7m" : "\x1b[0m";
    }
    if (t < 0 || t >= NTHEMES)
        t = 0;
    if (t != ct || m != cm) {
        const theme_t *th = &THEMES[t];
        for (int h = 0; h < 2; h++)
            for (int s = 0; s < C_COUNT; s++) {
                if (s == C_RESET && !h)
                    snprintf(sgr[h][s], sizeof sgr[h][s], "\x1b[0m");
                else
                    build(sgr[h][s], sizeof sgr[h][s], th, m, s == C_RESET ? C_TEXT : s, h);
            }
        ct = t;
        cm = m;
    }
    return sgr[hl][c];
}
