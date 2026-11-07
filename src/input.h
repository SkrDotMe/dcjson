/* input.h - keyboard and mouse decoding */
#ifndef DJ_INPUT_H
#define DJ_INPUT_H

#include <stddef.h>

enum {
    KEY_ENTER = 13,
    KEY_BS = 127,
    KEY_UP = 0x110000,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INS,
    KEY_DEL,
    KEY_F1,
    KEY_ESC,
    KEY_BTAB,
    KEY_MOUSE,
    KEY_PASTE_BEGIN,
    KEY_PASTE_END,
    KEY_UNKNOWN,
};

#define MOD_SHIFT 1
#define MOD_ALT   2
#define MOD_CTRL  4

typedef struct {
    int key;  /* Unicode code point, a control byte (1..31) or KEY_*  */
    int mod;  /* MOD_* for special keys                              */
    int mb;   /* mouse: 0 left, 1 middle, 2 right, 64 wheel up, 65 wheel down */
    int mx, my; /* mouse cell, 0-based                                 */
    int mrelease;
} kev_t;

/* Decode one event from b[0..n).  Returns the bytes used, or 0 when b
 * holds the start of a sequence that may still continue; with flush set
 * such a start is decoded as it stands (a lone ESC is the Esc key). */
size_t input_decode(const unsigned char *b, size_t n, int flush, kev_t *ev);

#endif
