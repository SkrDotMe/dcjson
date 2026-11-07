/* input.c - decoding of terminal input: UTF-8 text, CSI / SS3 key
 * sequences (xterm, VT220 and rxvt forms), SGR mouse reports and
 * bracketed-paste markers. */
#include "input.h"

#include <string.h>

#include "utf8.h"

static int csi_mod(int p) { return p > 1 ? (p - 1) & 7 : 0; }

static size_t csi(const unsigned char *b, size_t n, int flush, kev_t *ev)
{
    /* b[0] = ESC, b[1] = '[' */
    size_t i = 2;
    int lt = 0;
    if (i < n && (b[i] == '<' || b[i] == '?' || b[i] == '>')) {
        lt = b[i] == '<';
        i++;
    }
    int par[8] = {0}, np = 0;
    while (i < n && b[i] >= 0x30 && b[i] <= 0x3f) {
        if (b[i] == ';') {
            if (np < 7)
                np++;
        } else if (b[i] >= '0' && b[i] <= '9' && par[np] < 100000) {
            par[np] = par[np] * 10 + (b[i] - '0');
        }
        i++;
    }
    np++;
    while (i < n && b[i] >= 0x20 && b[i] <= 0x2f)
        i++;
    if (i >= n) {
        if (!flush && i < 64)
            return 0;
        ev->key = KEY_UNKNOWN;
        return i;
    }
    unsigned f = b[i++];
    ev->mod = 0;
    if (lt && (f == 'M' || f == 'm')) {
        ev->key = KEY_MOUSE;
        int btn = par[0];
        ev->mod = (btn & 4 ? MOD_SHIFT : 0) | (btn & 8 ? MOD_ALT : 0) | (btn & 16 ? MOD_CTRL : 0);
        ev->mb = btn & 64 ? 64 + (btn & 1) : btn & 3;
        if (btn & 32) /* motion */
            ev->mb |= 128;
        ev->mx = par[1] > 0 ? par[1] - 1 : 0;
        ev->my = par[2] > 0 ? par[2] - 1 : 0;
        ev->mrelease = f == 'm';
        return i;
    }
    if (lt) {
        ev->key = KEY_UNKNOWN;
        return i;
    }
    switch (f) {
    case 'A': ev->key = KEY_UP; break;
    case 'B': ev->key = KEY_DOWN; break;
    case 'C': ev->key = KEY_RIGHT; break;
    case 'D': ev->key = KEY_LEFT; break;
    case 'H': ev->key = KEY_HOME; break;
    case 'F': ev->key = KEY_END; break;
    case 'Z': ev->key = KEY_BTAB; break;
    case 'P': ev->key = KEY_F1; break;
    case '~':
        switch (par[0]) {
        case 1: case 7: ev->key = KEY_HOME; break;
        case 4: case 8: ev->key = KEY_END; break;
        case 2: ev->key = KEY_INS; break;
        case 3: ev->key = KEY_DEL; break;
        case 5: ev->key = KEY_PGUP; break;
        case 6: ev->key = KEY_PGDN; break;
        case 11: ev->key = KEY_F1; break;
        case 200: ev->key = KEY_PASTE_BEGIN; break;
        case 201: ev->key = KEY_PASTE_END; break;
        default: ev->key = KEY_UNKNOWN; break;
        }
        break;
    default: ev->key = KEY_UNKNOWN; break;
    }
    if (np > 1)
        ev->mod = csi_mod(par[1]);
    return i;
}

size_t input_decode(const unsigned char *b, size_t n, int flush, kev_t *ev)
{
    memset(ev, 0, sizeof *ev);
    if (!n)
        return 0;
    unsigned c = b[0];
    if (c == 0x1b) {
        if (n == 1) {
            if (!flush)
                return 0;
            ev->key = KEY_ESC;
            return 1;
        }
        if (b[1] == '[')
            return csi(b, n, flush, ev);
        if (b[1] == 'O') {
            if (n < 3) {
                if (!flush)
                    return 0;
                ev->key = KEY_ESC;
                return 1;
            }
            switch (b[2]) {
            case 'A': ev->key = KEY_UP; break;
            case 'B': ev->key = KEY_DOWN; break;
            case 'C': ev->key = KEY_RIGHT; break;
            case 'D': ev->key = KEY_LEFT; break;
            case 'H': ev->key = KEY_HOME; break;
            case 'F': ev->key = KEY_END; break;
            case 'P': ev->key = KEY_F1; break;
            case 'M': ev->key = KEY_ENTER; break;
            default: ev->key = KEY_UNKNOWN; break;
            }
            return 3;
        }
        ev->key = KEY_ESC; /* Alt+key arrives as ESC, then the key */
        return 1;
    }
    if (c == '\r' || c == '\n') {
        ev->key = KEY_ENTER;
        return 1;
    }
    if (c == 0x7f || c == 0x08) {
        ev->key = KEY_BS;
        return 1;
    }
    if (c < 0x80) {
        ev->key = (int)c;
        return 1;
    }
    uint32_t cp;
    int l = utf8_decode(b, n, &cp);
    if (l > 0) {
        ev->key = (int)cp;
        return (size_t)l;
    }
    /* incomplete multi-byte character? */
    size_t need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if (n < need && !flush)
        return 0;
    ev->key = KEY_UNKNOWN;
    return 1;
}
