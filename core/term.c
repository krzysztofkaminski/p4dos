/*
 * Mirrors the 80x25 (or 40x25) text screen onto an ANSI/VT100 terminal: the Linux console
 * of the host build, the UART of the bare-metal RISC-V build, and the serial console of
 * the Guition board. Only cells that changed are sent.
 */
#include "term.h"

#include <stdio.h>
#include <string.h>

#include "pc.h"
#include "video.h"

static const uint16_t cp437[256] = {
    0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x0020,
};

static term_out_fn out_fn;
static char obuf[4096];
static int olen;
static uint16_t shadow[25 * 80];
static int valid, lost, cur_row, cur_col, cur_attr, cur_visible, shown_kind = -1;
static char shown_gfx[48];

static void flush(void)
{
    if (olen && out_fn && out_fn(obuf, olen)) {
        /* The terminal missed something: its picture can no longer be trusted. */
        valid = 0;
        lost = 1;
    }
    olen = 0;
}

static void put(const char *s)
{
    int n = (int)strlen(s);
    if (olen + n > (int)sizeof(obuf)) {
        flush();
    }
    memcpy(obuf + olen, s, (size_t)n);
    olen += n;
}

static void put_utf8(unsigned cp)
{
    char b[4];
    int n = 0;
    if (cp < 0x80) {
        b[n++] = (char)cp;
    } else if (cp < 0x800) {
        b[n++] = (char)(0xC0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        b[n++] = (char)(0xE0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    }
    b[n] = 0;
    put(b);
}

static void set_attr(int attr)
{
    static const uint8_t ansi[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    char b[32];
    int fg = attr & 15, bg = (attr >> 4) & ((video.mode_ctl & 0x20) ? 7 : 15);
    if (attr == cur_attr) {
        return;
    }
    cur_attr = attr;
    snprintf(b, sizeof(b), "\x1b[0;%d;%dm", (fg & 8 ? 90 : 30) + ansi[fg & 7],
             (bg & 8 ? 100 : 40) + ansi[bg & 7]);
    put(b);
}

static void move_to(int row, int col)
{
    char b[24];
    if (row == cur_row && col == cur_col) {
        return;
    }
    snprintf(b, sizeof(b), "\x1b[%d;%dH", row + 1, col + 1);
    put(b);
    cur_row = row;
    cur_col = col;
}

void term_init(term_out_fn out)
{
    out_fn = out;
    valid = 0;
    shown_kind = -1;
}

void term_close(void)
{
    put("\x1b[0m\x1b[r\x1b[?25h\x1b[25;1H\r\n");
    flush();
}

int term_refresh(int force)
{
    const int cols = video.kind == VK_TEXT40 ? 40 : 80;
    const uint32_t start = ((uint32_t)(video.crtc[0x0C] & 0x3F) << 8) | video.crtc[0x0D];
    const uint32_t curs = ((uint32_t)(video.crtc[0x0E] & 0x3F) << 8) | video.crtc[0x0F];
    const uint8_t *vram = cpu_mem + 0xB8000;
    static uint16_t now[25 * 80];

    if (video.kind != shown_kind) {
        shown_kind = video.kind;
        force = 1;
    }
    lost = 0;
    if (video.kind > VK_TEXT80) {
        /* Graphics cannot be mirrored; say what the panel is showing instead. */
        char label[48];
        int w = 320, h = 200, colours = 256;
        switch (video.kind) {
        case VK_CGA320: colours = 4; break;
        case VK_CGA640: w = 640; colours = 2; break;
        case VK_MCGA640: w = 640; h = 480; colours = 2; break;
        case VK_PLANAR: {
            vga_geometry_t g;
            vga_geometry(&g);
            w = g.width;
            h = g.rows;
            colours = g.is256 ? 256 : 16;
            break;
        }
        default: break;
        }
        snprintf(label, sizeof(label), "%dx%d, %d colours", w, h, colours);
        if (force || !valid || strcmp(label, shown_gfx)) {
            valid = 1;
            strcpy(shown_gfx, label);
            put("\x1b[0m\x1b[2J\x1b[1;1H[graphics mode ");
            put(label);
            put(": shown on the panel only]");
            flush();
        }
        return lost;
    }
    if (force || !valid) {
        put("\x1b[0m\x1b[1;25r\x1b[2J");
        memset(shadow, 0xFF, sizeof(shadow));
        cur_row = cur_col = cur_attr = cur_visible = -1;
        valid = 1;
    }

    for (int i = 0; i < 25 * cols; i++) {
        uint32_t off = ((start + (uint32_t)i) * 2) & 0x7FFE;
        now[i] = (uint16_t)(vram[off] | (vram[off + 1] << 8));
    }

    /* A screen that scrolled up by one line is sent as a terminal scroll. */
    if (memcmp(now, shadow, sizeof(uint16_t) * (size_t)cols) &&
        !memcmp(now, shadow + cols, sizeof(uint16_t) * (size_t)(24 * cols))) {
        put("\x1b[0m\x1b[25;1H\n");
        cur_attr = -1;
        cur_row = 24;
        cur_col = 0;
        memmove(shadow, shadow + cols, sizeof(uint16_t) * (size_t)(24 * cols));
        for (int c = 0; c < cols; c++) {
            shadow[24 * cols + c] = 0x0020;
        }
    }

    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < cols; col++) {
            int i = row * cols + col;
            if (now[i] == shadow[i]) {
                continue;
            }
            shadow[i] = now[i];
            move_to(row, col);
            set_attr(now[i] >> 8);
            put_utf8(cp437[now[i] & 0xFF]);
            cur_col++;
            if (cur_col >= 80) {
                cur_row = -1; /* position after the last column is terminal-dependent */
            }
        }
    }

    uint32_t rel = (curs - start) & 0x3FFF;
    int visible = !(video.crtc[0x0A] & 0x20) && rel < (uint32_t)(25 * cols);
    if (visible) {
        move_to((int)(rel / (uint32_t)cols), (int)(rel % (uint32_t)cols));
    }
    if (visible != cur_visible) {
        cur_visible = visible;
        put(visible ? "\x1b[?25h" : "\x1b[?25l");
    }
    flush();
    return lost;
}
