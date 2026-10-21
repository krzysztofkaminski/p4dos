/*
 * Keyboard front end: collects key events from the platform (USB HID boot reports, or a
 * serial terminal byte stream) and hands XT set-1 scancodes to the emulated keyboard
 * interface, generating typematic repeat for USB keyboards, which do not repeat by
 * themselves.
 */
#include <string.h>

#include "audio.h"
#include "pc.h"

#define QSIZE 256

static uint8_t q[QSIZE];
static unsigned q_head, q_tail;

static uint16_t rep_code; /* scancode | 0x100 if E0-prefixed; 0 = nothing held */
static uint8_t rep_fresh;
static uint64_t rep_next;
static uint32_t rep_delay_ms = 500, rep_period_ms = 92;
static int pop_pending = -1;
static uint64_t pause_until; /* queue byte 00h = hold the next scancode back for a moment */
#define KBD_PAUSE_NS 30000000u
static uint8_t pop_e0, held_mods; /* bit0 LCtrl, bit1 RCtrl, bit2 LAlt, bit3 RAlt */

static uint8_t hid_mods, hid_keys[6];
static uint8_t hid_swallowed[6]; /* keys taken by a firmware shortcut, hidden from DOS */
static uint8_t leds;

static struct {
    uint8_t state; /* 0 idle, 1 ESC, 2 CSI, 3 SS3 */
    int param[3], nparam;
} term;

void kbd_init(void)
{
    q_head = q_tail = 0;
    rep_code = 0;
    pop_pending = -1;
    pause_until = 0;
    pop_e0 = held_mods = 0;
    hid_mods = 0;
    memset(hid_keys, 0, sizeof(hid_keys));
    memset(hid_swallowed, 0, sizeof(hid_swallowed));
    memset(&term, 0, sizeof(term));
}

/* Caller holds the input lock. */
static void push(uint8_t sc)
{
    unsigned n = (q_head + 1) % QSIZE;
    if (n != q_tail) {
        q[q_head] = sc;
        q_head = n;
    }
}

void kbd_push(uint8_t sc)
{
    hal_input_lock();
    push(sc);
    hal_input_unlock();
}

void kbd_set_typematic(uint32_t delay_ms, uint32_t period_ms)
{
    rep_delay_ms = delay_ms;
    rep_period_ms = period_ms;
}

uint8_t kbd_leds(void) { return leds; }
void kbd_set_leds(uint8_t v) { leds = v; }
int kbd_ctrl_alt(void) { return (held_mods & 0x03) && (held_mods & 0x0C); }

int kbd_pop(uint64_t now)
{
    int sc = -1;

    if (pop_pending >= 0) {
        sc = pop_pending;
        pop_pending = -1;
        return sc;
    }
    if (now < pause_until) {
        return -1;
    }
    hal_input_lock();
    if (q_tail != q_head) {
        sc = q[q_tail];
        q_tail = (q_tail + 1) % QSIZE;
        if (sc == 0) {
            pause_until = now + KBD_PAUSE_NS;
            sc = -1;
        }
    } else if (rep_code) {
        if (rep_fresh) {
            rep_fresh = 0;
            rep_next = now + (uint64_t)rep_delay_ms * 1000000u;
        } else if (now >= rep_next) {
            rep_next = now + (uint64_t)rep_period_ms * 1000000u;
            if (rep_code & 0x100) {
                sc = 0xE0;
                pop_pending = rep_code & 0xFF;
            } else {
                sc = rep_code;
            }
        }
    }
    hal_input_unlock();

    if (sc == 0xE0) {
        pop_e0 = 1;
    } else if (sc >= 0) {
        uint8_t bit = 0;
        if ((sc & 0x7F) == 0x1D) {
            bit = pop_e0 ? 0x02 : 0x01;
        } else if ((sc & 0x7F) == 0x38) {
            bit = pop_e0 ? 0x08 : 0x04;
        }
        if (sc & 0x80) {
            held_mods &= (uint8_t)~bit;
        } else {
            held_mods |= bit;
        }
        pop_e0 = 0;
    }
    return sc;
}

/* --- USB HID boot keyboard ------------------------------------------------ */

/* HID usage 00h-65h -> set-1 scancode; bit 7 marks an E0-prefixed key. */
static const uint8_t hid_map[0x66] = {
    0,    0,    0,    0,    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A,
    0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
    0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0xB7, 0x46, 0,    0xD2, 0xC7, 0xC9, 0xD3, 0xCF, 0xD1, 0xCD,
    0xCB, 0xD0, 0xC8, 0x45, 0xB5, 0x37, 0x4A, 0x4E, 0x9C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47,
    0x48, 0x49, 0x52, 0x53, 0x56, 0xDD,
};
/* Modifier bits 0-7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI. */
static const uint8_t hid_mod_map[8] = {0x1D, 0x2A, 0x38, 0xDB, 0x9D, 0x36, 0xB8, 0xDC};

static void push_key(uint8_t code, int release)
{
    if (!code) {
        return;
    }
    if (code & 0x80) {
        push(0xE0);
    }
    push((uint8_t)((code & 0x7F) | (release ? 0x80 : 0)));
}

void kbd_hid_report(uint8_t mods, const uint8_t *keys, int nkeys)
{
    uint8_t now[6] = {0};
    if (nkeys > 6) {
        nkeys = 6;
    }
    memcpy(now, keys, (size_t)nkeys);
    if (now[0] == 0x01) {
        return; /* rollover error: keep the previous state */
    }

    /* Firmware shortcuts: Ctrl+Alt+Up / Ctrl+Alt+Down change the volume. The arrow key
     * itself never reaches DOS. */
    const int chord = (mods & 0x11) && (mods & 0x44);
    for (int i = 0; i < 6; i++) {
        uint8_t k = now[i];
        if (chord && (k == 0x52 || k == 0x51) && !memchr(hid_keys, k, 6) &&
            !memchr(hid_swallowed, k, 6)) {
            uint8_t *slot = memchr(hid_swallowed, 0, 6);
            if (slot) {
                *slot = k;
            }
            audio_volume_step(k == 0x52 ? 1 : -1);
        }
    }
    for (int i = 0; i < 6; i++) {
        uint8_t *held = hid_swallowed[i] ? memchr(now, hid_swallowed[i], 6) : NULL;
        if (held) {
            *held = 0; /* still down: keep hiding it */
        } else {
            hid_swallowed[i] = 0;
        }
    }

    hal_input_lock();
    for (int b = 0; b < 8; b++) {
        if ((mods ^ hid_mods) & (1 << b)) {
            push_key(hid_mod_map[b], !(mods & (1 << b)));
        }
    }
    for (int i = 0; i < 6; i++) {
        uint8_t k = hid_keys[i];
        if (k >= 4 && k < sizeof(hid_map) && !memchr(now, k, 6)) {
            push_key(hid_map[k], 1);
            if (rep_code == ((hid_map[k] & 0x7F) | ((hid_map[k] & 0x80) ? 0x100 : 0))) {
                rep_code = 0;
            }
        }
    }
    for (int i = 0; i < 6; i++) {
        uint8_t k = now[i];
        if (k >= 4 && k < sizeof(hid_map) && !memchr(hid_keys, k, 6) && hid_map[k]) {
            push_key(hid_map[k], 0);
            rep_code = (uint16_t)((hid_map[k] & 0x7F) | ((hid_map[k] & 0x80) ? 0x100 : 0));
            rep_fresh = 1;
        }
    }
    hid_mods = mods;
    memcpy(hid_keys, now, 6);
    hal_input_unlock();
}

/* --- terminal input ------------------------------------------------------- */

static const char asc_lo[0x3B] =
    "\0\x1B" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char asc_hi[0x3B] =
    "\0\x1B" "!@#$%^&*()_+\b\0QWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

enum { M_SHIFT = 1, M_ALT = 2, M_CTRL = 4, M_EXT = 8 };

/* Press and release one key with the given modifiers held. Ctrl and Alt are held for a
 * moment on either side, as a person would: programs that poll the shift state (menus
 * opened with Alt+letter) otherwise never see them down. */
static void tap(uint8_t sc, int mods)
{
    const int hold = mods & (M_CTRL | M_ALT);
    hal_input_lock();
    if (mods & M_CTRL) push(0x1D);
    if (mods & M_ALT) push(0x38);
    if (hold) push(0x00);
    if (mods & M_SHIFT) push(0x2A);
    if (mods & M_EXT) push(0xE0);
    push(sc);
    if (mods & M_EXT) push(0xE0);
    push(sc | 0x80);
    if (mods & M_SHIFT) push(0xAA);
    if (hold) push(0x00);
    if (mods & M_ALT) push(0xB8);
    if (mods & M_CTRL) push(0x9D);
    hal_input_unlock();
}

static void type_char(int ch, int mods)
{
    if (ch == '\n') {
        ch = '\r';
    } else if (ch == 127) {
        ch = '\b';
    }
    if (ch >= 1 && ch <= 26 && ch != '\b' && ch != '\t' && ch != '\r') {
        mods |= M_CTRL;
        ch += 'a' - 1;
    }
    if (ch == 0) { /* Ctrl-@ */
        tap(0x03, mods | M_CTRL);
        return;
    }
    for (int sc = 1; sc < 0x3A; sc++) {
        if (asc_lo[sc] == ch && sc != 0x37) {
            tap((uint8_t)sc, mods);
            return;
        }
        if (asc_hi[sc] == ch && sc != 0x37) {
            tap((uint8_t)sc, mods | M_SHIFT);
            return;
        }
    }
}

void kbd_type_ascii(int ch)
{
    type_char(ch, 0);
}

static void csi_final(int final)
{
    int p0 = term.nparam > 0 ? term.param[0] : 0;
    int m = term.nparam > 1 ? term.param[1] - 1 : 0;
    int mods = ((m & 1) ? M_SHIFT : 0) | ((m & 2) ? M_ALT : 0) | ((m & 4) ? M_CTRL : 0);
    static const uint8_t fkeys[] = {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0,    0x40, 0x41,
                                    0x42, 0x43, 0x44, 0,    0x57, 0x58};
    if ((mods & (M_CTRL | M_ALT)) == (M_CTRL | M_ALT) && (final == 'A' || final == 'B')) {
        audio_volume_step(final == 'A' ? 1 : -1); /* Ctrl+Alt+Up / Down on a terminal */
        return;
    }
    switch (final) {
    case 'A': tap(0x48, mods | M_EXT); break;
    case 'B': tap(0x50, mods | M_EXT); break;
    case 'C': tap(0x4D, mods | M_EXT); break;
    case 'D': tap(0x4B, mods | M_EXT); break;
    case 'H': tap(0x47, mods | M_EXT); break;
    case 'F': tap(0x4F, mods | M_EXT); break;
    case 'Z': tap(0x0F, M_SHIFT); break;
    case 'P': tap(0x3B, mods); break;
    case 'Q': tap(0x3C, mods); break;
    case 'R': tap(0x3D, mods); break;
    case 'S': tap(0x3E, mods); break;
    case 'M': tap(0x1C, mods); break;
    case '~':
        switch (p0) {
        case 1: case 7: tap(0x47, mods | M_EXT); break;
        case 2: tap(0x52, mods | M_EXT); break;
        case 3: tap(0x53, mods | M_EXT); break;
        case 4: case 8: tap(0x4F, mods | M_EXT); break;
        case 5: tap(0x49, mods | M_EXT); break;
        case 6: tap(0x51, mods | M_EXT); break;
        default:
            if (p0 >= 11 && p0 <= 24 && fkeys[p0 - 11]) {
                tap(fkeys[p0 - 11], mods);
            }
            break;
        }
        break;
    default: break;
    }
}

void kbd_term_char(int ch)
{
    switch (term.state) {
    case 0:
        if (ch == 0x1B) {
            term.state = 1;
        } else if (ch >= 0) {
            type_char(ch, 0);
        }
        break;
    case 1:
        if (ch == '[' || ch == 'O') {
            term.state = ch == '[' ? 2 : 3;
            term.nparam = 0;
            memset(term.param, 0, sizeof(term.param));
        } else if (ch < 0) { /* a lone ESC */
            tap(0x01, 0);
            term.state = 0;
        } else if (ch == 0x1B) {
            tap(0x01, 0);
        } else {
            type_char(ch, M_ALT);
            term.state = 0;
        }
        break;
    default:
        if (ch < 0) {
            term.state = 0;
        } else if (ch >= '0' && ch <= '9') {
            if (term.nparam == 0) {
                term.nparam = 1;
            }
            term.param[term.nparam - 1] = term.param[term.nparam - 1] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (term.nparam == 0) {
                term.nparam = 1;
            }
            if (term.nparam < 3) {
                term.nparam++;
            }
        } else if (ch >= 0x40 && ch <= 0x7E) {
            csi_final(ch);
            term.state = 0;
        }
        break;
    }
}
