/*
 * Renderer: draws the emulated display onto a 1280x800 canvas.
 *
 *   80x25 text  -> 8x16 font doubled        = 1280x800
 *   40x25 text  -> 4x horizontally, 2x vert = 1280x800
 *   320x200     -> 4x                       = 1280x800
 *   640x200     -> 2x horizontally, 4x vert = 1280x800
 *   640x480     -> 2x horizontally, 5/3 vertically
 *   320x240     -> 3x (960x720, centred): square pixels, as on a 4:3 monitor
 *   other register-programmed VGA modes: the nearest whole factors, centred
 *
 * The Guition panel is 800x1280 portrait glass, so for ROT_CW / ROT_CCW each frame buffer
 * row holds one *column* of the picture. Only changed cells/columns are redrawn; the caller
 * gets the range of touched frame buffer rows so it can sync just that part to the panel.
 */
#include <string.h>

#include "pc.h"
#include "video.h"

#define FB_ROT_W 800  /* frame buffer width when rotated */
#define FB_ROT_H 1280

static uint16_t pal565[256];
static uint16_t pal_text[16]; /* text and 16-colour modes: through the attribute palette */
static uint16_t pal_cga[16];  /* CGA modes: the sixteen fixed CGA colours */
static uint16_t font_cols[256][8]; /* per glyph column: bit n = pixel in glyph row n */
static uint32_t spread[256]; /* a plane byte, a bit per nibble: leftmost pixel on top */
static int font_ready;
static uint32_t seen_generation;
static int seen_valid;
static int seen_rotate = -1;
static const uint16_t *seen_fb;
static uint32_t text_shadow[80 * 25];
/* The graphics modes' working memory. The CGA and MCGA modes keep a copy of video memory as
 * last drawn, the planar ones their tables; switching between them redraws everything. */
static union {
    uint8_t shadow[0x10000];
    struct {
        uint32_t taken[VGA_PLANE_SIZE / 32]; /* vga_dirty as taken for this redraw */
        uint16_t cols[8][480];               /* colours of up to eight source columns */
    } planar;
} gfx HAL_EXT_BSS;
static uint8_t dirty_col[640];
static uint16_t ymap[SCREEN_H]; /* frame buffer x -> source row, rotated modes */

static const uint8_t *rom_font16(void)
{
    return p4dos_rom + (p4dos_rom[0x0A] | (p4dos_rom[0x0B] << 8));
}

static void font_init(void)
{
    const uint8_t *f = rom_font16();
    for (int ch = 0; ch < 256; ch++) {
        for (int gx = 0; gx < 8; gx++) {
            uint16_t bits = 0;
            for (int gy = 0; gy < 16; gy++) {
                if (f[ch * 16 + gy] & (0x80 >> gx)) {
                    bits |= (uint16_t)(1u << gy);
                }
            }
            font_cols[ch][gx] = bits;
        }
    }
    for (int b = 0; b < 256; b++) {
        uint32_t w = 0;
        for (int j = 0; j < 8; j++) {
            w |= (uint32_t)((b >> j) & 1) << (4 * j);
        }
        spread[b] = w;
    }
    font_ready = 1;
}

static void palette_update(void)
{
    /* Where the sixteen CGA colours sit in the 64-entry EGA palette the BIOS loads. */
    static const uint8_t cga_dac[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
                                        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};
    for (int i = 0; i < 256; i++) {
        const uint8_t *c = video.dac[i & video.dac_mask];
        pal565[i] = (uint16_t)(((c[0] >> 1) << 11) | (c[1] << 5) | (c[2] >> 1));
    }
    for (int i = 0; i < 16; i++) {
        pal_text[i] = pal565[video.attr[i] & 0x3F];
        pal_cga[i] = pal565[cga_dac[i]];
    }
}

/* --- text ----------------------------------------------------------------- */

static void draw_cell(const render_target_t *t, int col, int row, int cw, uint32_t key)
{
    const uint8_t ch = (uint8_t)key;
    const uint8_t attr = (uint8_t)(key >> 8);
    const int blink_attr = (video.mode_ctl & 0x20) != 0;
    uint16_t fg = pal_text[attr & 0x0F];
    uint16_t bg = pal_text[blink_attr ? (attr >> 4) & 7 : attr >> 4];
    uint16_t cursor = 0;

    if (key & 0x20000) {
        fg = bg; /* blinking character, off phase */
    }
    if (key & 0x10000) {
        int s = video.crtc[0x0A] & 0x1F, e = video.crtc[0x0B] & 0x1F;
        for (int y = s; y <= e && y < 16; y++) {
            cursor |= (uint16_t)(1u << y);
        }
        fg = pal_text[attr & 0x0F];
    }

    if (t->rotate == ROT_NONE) {
        const uint8_t *g = rom_font16() + ch * 16;
        for (int j = 0; j < 32; j++) {
            int gy = j >> 1;
            uint8_t bits = (cursor >> gy) & 1 ? 0xFF : g[gy];
            uint16_t *p = t->fb + (row * 32 + j) * t->stride + col * cw;
            for (int i = 0; i < cw; i++) {
                p[i] = (bits & (0x80 >> (i * 8 / cw))) ? fg : bg;
            }
        }
        return;
    }

    uint16_t lines[8][32];
    for (int gx = 0; gx < 8; gx++) {
        uint32_t bits = font_cols[ch][gx] | cursor;
        uint16_t *l = lines[gx];
        if (t->rotate == ROT_CW) {
            for (int k = 0; k < 32; k++) {
                l[k] = (bits >> ((31 - k) >> 1)) & 1 ? fg : bg;
            }
        } else {
            for (int k = 0; k < 32; k++) {
                l[k] = (bits >> (k >> 1)) & 1 ? fg : bg;
            }
        }
    }
    const int x0 = t->rotate == ROT_CW ? FB_ROT_W - 32 - row * 32 : row * 32;
    for (int i = 0; i < cw; i++) {
        int lx = col * cw + i;
        int py = t->rotate == ROT_CW ? lx : FB_ROT_H - 1 - lx;
        memcpy(t->fb + py * t->stride + x0, lines[i * 8 / cw], sizeof(lines[0]));
    }
}

static int render_text(const render_target_t *t, uint64_t now_ns, int force, int *row0, int *row1)
{
    const int cols = video.kind == VK_TEXT40 ? 40 : 80;
    const int cw = SCREEN_W / cols;
    const uint32_t start = ((uint32_t)(video.crtc[0x0C] & 0x3F) << 8) | video.crtc[0x0D];
    const uint32_t curs = ((uint32_t)(video.crtc[0x0E] & 0x3F) << 8) | video.crtc[0x0F];
    const int cursor_on = !(video.crtc[0x0A] & 0x20) && ((now_ns / 266000000u) & 1) == 0;
    const int blink_off = (video.mode_ctl & 0x20) && ((now_ns / 533000000u) & 1);
    const uint8_t *vram = cpu_mem + 0xB8000;
    int c0 = cols, c1 = -1, r0 = 25, r1 = -1;

    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < cols; col++) {
            uint32_t idx = (uint32_t)(row * cols + col);
            uint32_t off = ((start + idx) * 2) & 0x7FFE;
            uint32_t key = vram[off] | ((uint32_t)vram[off + 1] << 8);
            if (cursor_on && ((start + idx) & 0x3FFF) == curs) {
                key |= 0x10000;
            }
            if (blink_off && (key & 0x8000)) {
                key |= 0x20000;
            }
            if (force || key != text_shadow[idx]) {
                text_shadow[idx] = key;
                draw_cell(t, col, row, cw, key);
                if (col < c0) c0 = col;
                if (col > c1) c1 = col;
                if (row < r0) r0 = row;
                if (row > r1) r1 = row;
            }
        }
    }
    if (c1 < 0) {
        return 0;
    }
    if (t->rotate == ROT_NONE) {
        *row0 = r0 * 32;
        *row1 = r1 * 32 + 31;
    } else if (t->rotate == ROT_CW) {
        *row0 = c0 * cw;
        *row1 = c1 * cw + cw - 1;
    } else {
        *row0 = FB_ROT_H - 1 - (c1 * cw + cw - 1);
        *row1 = FB_ROT_H - 1 - c0 * cw;
    }
    return 1;
}

/* --- graphics ------------------------------------------------------------- */

typedef struct {
    int w, h;
    int bpr;            /* bytes per scan line */
    int px_per_byte;
    uint32_t base, len; /* video memory window that holds the picture */
    int interlaced;     /* CGA: even lines in the first 8 KiB bank, odd in the second */
} gfx_mode_t;

static const gfx_mode_t gfx_modes[] = {
    [VK_CGA320] = {320, 200, 80, 4, 0xB8000, 0x4000, 1},
    [VK_CGA640] = {640, 200, 80, 8, 0xB8000, 0x4000, 1},
    [VK_MCGA256] = {320, 200, 320, 1, 0xA0000, 64000, 0},
    [VK_MCGA640] = {640, 480, 80, 8, 0xA0000, 38400, 0},
};

static inline uint32_t line_off(const gfx_mode_t *m, int y)
{
    return m->interlaced ? (uint32_t)((y & 1) * 0x2000 + (y >> 1) * m->bpr)
                         : (uint32_t)(y * m->bpr);
}

/* Colours of source column sx, top to bottom, as RGB565. */
static void fetch_column(const gfx_mode_t *m, int sx, const uint16_t *cga, uint16_t *out)
{
    const uint8_t *v = cpu_mem + m->base;
    switch (video.kind) {
    case VK_MCGA256:
        for (int y = 0; y < m->h; y++) {
            out[y] = pal565[v[y * 320 + sx]];
        }
        break;
    case VK_CGA320: {
        int sh = 6 - 2 * (sx & 3);
        for (int y = 0; y < m->h; y++) {
            out[y] = cga[(v[line_off(m, y) + (sx >> 2)] >> sh) & 3];
        }
        break;
    }
    default: {
        uint8_t bit = (uint8_t)(0x80 >> (sx & 7));
        for (int y = 0; y < m->h; y++) {
            out[y] = cga[(v[line_off(m, y) + (sx >> 3)] & bit) ? 1 : 0];
        }
        break;
    }
    }
}

static int render_gfx(const render_target_t *t, int force, int *row0, int *row1)
{
    const gfx_mode_t *m = &gfx_modes[video.kind];
    const uint8_t *v = cpu_mem + m->base;
    const int xrep = SCREEN_W / m->w;
    uint16_t cga[4];
    int c0 = m->w, c1 = -1;

    if (video.kind == VK_CGA320) {
        static const uint8_t pals[3][3] = {{2, 4, 6}, {3, 5, 7}, {3, 4, 7}};
        int p = (video.mode_ctl & 0x04) ? 2 : (video.color_sel >> 5) & 1;
        int hi = (video.color_sel & 0x10) ? 8 : 0;
        cga[0] = pal_cga[video.color_sel & 0x0F];
        for (int i = 0; i < 3; i++) {
            cga[i + 1] = pal_cga[pals[p][i] + hi];
        }
    } else if (video.kind == VK_CGA640) {
        cga[0] = pal_cga[0];
        cga[1] = pal_cga[video.color_sel & 0x0F];
    } else {
        cga[0] = pal_cga[0];
        cga[1] = pal_cga[15];
    }

    /* Find the source columns whose bytes changed. */
    if (force) {
        memcpy(gfx.shadow, v, m->len);
        c0 = 0;
        c1 = m->w - 1;
        memset(dirty_col, 1, sizeof(dirty_col));
    } else if (video.kind == VK_MCGA256) {
        /* A byte is a pixel and a column. A game redraws the whole picture every frame:
         * once every column is known to have changed, the rest is only copied. */
        int ndirty = 0;
        memset(dirty_col, 0, sizeof(dirty_col));
        for (int y = 0; y < 200; y++) {
            const uint8_t *const a = v + y * 320;
            uint8_t *const b = gfx.shadow + y * 320;
            if (ndirty == 320) {
                memcpy(b, a, (size_t)(200 - y) * 320);
                break;
            }
            if (!memcmp(a, b, 320)) {
                continue;
            }
            for (int x = 0; x < 320; x++) {
                if (a[x] != b[x]) {
                    b[x] = a[x];
                    ndirty += !dirty_col[x];
                    dirty_col[x] = 1;
                }
            }
        }
        if (ndirty) {
            for (c0 = 0; !dirty_col[c0]; c0++) {
            }
            for (c1 = 319; !dirty_col[c1]; c1--) {
            }
        }
    } else {
        memset(dirty_col, 0, sizeof(dirty_col));
        for (int y = 0; y < m->h; y++) {
            uint32_t o = line_off(m, y);
            if (!memcmp(v + o, gfx.shadow + o, (size_t)m->bpr)) {
                continue;
            }
            for (int b = 0; b < m->bpr; b++) {
                if (v[o + b] != gfx.shadow[o + b]) {
                    gfx.shadow[o + b] = v[o + b];
                    int x = b * m->px_per_byte;
                    for (int i = 0; i < m->px_per_byte; i++) {
                        dirty_col[x + i] = 1;
                    }
                    if (x < c0) c0 = x;
                    if (x + m->px_per_byte - 1 > c1) c1 = x + m->px_per_byte - 1;
                }
            }
        }
    }
    if (c1 < 0) {
        return 0;
    }

    if (t->rotate == ROT_NONE) {
        /* Host-side path (screenshots, SDL): redraw the changed column span, row by row. */
        uint16_t colbuf[480];
        for (int sx = c0; sx <= c1; sx++) {
            if (!dirty_col[sx]) {
                continue;
            }
            fetch_column(m, sx, cga, colbuf);
            for (int ly = 0; ly < SCREEN_H; ly++) {
                uint16_t c = colbuf[ly * m->h / SCREEN_H];
                uint16_t *p = t->fb + ly * t->stride + sx * xrep;
                for (int i = 0; i < xrep; i++) {
                    p[i] = c;
                }
            }
        }
        *row0 = 0;
        *row1 = SCREEN_H - 1;
        return 1;
    }

    if (video.kind == VK_MCGA256) {
        /* The mode games live in: a pixel becomes four by four, so a source column is
         * four frame buffer rows of 200 runs of four pixels, straight from the palette. */
        uint32_t line4[SCREEN_H / 2]; /* two pixels to the word */
        for (int sx = c0; sx <= c1; sx++) {
            if (!dirty_col[sx]) {
                continue;
            }
            const uint8_t *src = v + sx;
            uint32_t *out = line4;
            if (t->rotate == ROT_CW) {
                src += 199 * 320;
                for (int y = 0; y < 200; y++, src -= 320, out += 2) {
                    const uint32_t c = pal565[*src];
                    out[0] = out[1] = c | (c << 16);
                }
            } else {
                for (int y = 0; y < 200; y++, src += 320, out += 2) {
                    const uint32_t c = pal565[*src];
                    out[0] = out[1] = c | (c << 16);
                }
            }
            for (int i = 0; i < 4; i++) {
                const int lx = sx * 4 + i;
                const int py = t->rotate == ROT_CW ? lx : FB_ROT_H - 1 - lx;
                memcpy(t->fb + py * t->stride, line4, sizeof(line4));
            }
        }
        if (t->rotate == ROT_CW) {
            *row0 = c0 * 4;
            *row1 = c1 * 4 + 3;
        } else {
            *row0 = FB_ROT_H - 1 - (c1 * 4 + 3);
            *row1 = FB_ROT_H - 1 - c0 * 4;
        }
        return 1;
    }

    for (int px = 0; px < SCREEN_H; px++) {
        int ly = t->rotate == ROT_CW ? SCREEN_H - 1 - px : px;
        ymap[px] = (uint16_t)(ly * m->h / SCREEN_H);
    }
    uint16_t colbuf[480];
    uint16_t line[SCREEN_H];
    for (int sx = c0; sx <= c1; sx++) {
        if (!dirty_col[sx]) {
            continue;
        }
        fetch_column(m, sx, cga, colbuf);
        for (int px = 0; px < SCREEN_H; px++) {
            line[px] = colbuf[ymap[px]];
        }
        for (int i = 0; i < xrep; i++) {
            int lx = sx * xrep + i;
            int py = t->rotate == ROT_CW ? lx : FB_ROT_H - 1 - lx;
            memcpy(t->fb + py * t->stride, line, sizeof(line));
        }
    }
    if (t->rotate == ROT_CW) {
        *row0 = c0 * xrep;
        *row1 = c1 * xrep + xrep - 1;
    } else {
        *row0 = FB_ROT_H - 1 - (c1 * xrep + xrep - 1);
        *row1 = FB_ROT_H - 1 - c0 * xrep;
    }
    return 1;
}

/* --- planar 16-colour and unchained 256-colour modes --------------------------- */

static struct {
    int x0, y0, dw, dh;
    const uint16_t *fb;
    int rotate;
} planar_rect;

/* Rows r0..r1 of the eight pixels of plane byte ax (16 colours) into gfx.planar.cols. */
static void fetch_planar16(const uint32_t *rowbase, uint32_t ax, int r0, int r1)
{
    const uint8_t *const p0 = vga_plane[0], *const p1 = vga_plane[1];
    const uint8_t *const p2 = vga_plane[2], *const p3 = vga_plane[3];
    const uint32_t enable = video.attr[0x12] & 0x0F;
    for (int y = r0; y <= r1; y++) {
        const uint32_t a = (rowbase[y] + ax) & 0xFFFF;
        const uint32_t pix = spread[p0[a]] | spread[p1[a]] << 1 | spread[p2[a]] << 2 |
                             spread[p3[a]] << 3;
        for (int k = 0; k < 8; k++) {
            gfx.planar.cols[k][y] = pal_text[(pix >> (28 - 4 * k)) & enable];
        }
    }
}

/* Rows r0..r1 of source column xs (256 colours) into out. */
static void fetch_planar256(const uint32_t *rowbase, int xs, int r0, int r1, uint16_t *out)
{
    const uint8_t *const p = vga_plane[xs & 3];
    const uint32_t ax = (uint32_t)xs >> 2;
    for (int y = r0; y <= r1; y++) {
        out[y] = pal565[p[(rowbase[y] + ax) & 0xFFFF]];
    }
}

/* Picture column x, from col[] by source row, onto canvas lines ly0..ly1. */
static void draw_planar_column(const render_target_t *t, const int16_t *src_row, int x0, int sx,
                               int x, const uint16_t *col, int ly0, int ly1)
{
    if (t->rotate == ROT_NONE) {
        for (int ly = ly0; ly <= ly1; ly++) {
            const int r = src_row[ly];
            const uint16_t c = r < 0 ? 0 : col[r];
            uint16_t *p = t->fb + ly * t->stride + x0 + x * sx;
            for (int i = 0; i < sx; i++) {
                p[i] = c;
            }
        }
        return;
    }
    uint16_t line[SCREEN_H];
    const int px0 = t->rotate == ROT_CW ? SCREEN_H - 1 - ly1 : ly0;
    const int px1 = t->rotate == ROT_CW ? SCREEN_H - 1 - ly0 : ly1;
    for (int px = px0; px <= px1; px++) {
        const int r = src_row[t->rotate == ROT_CW ? SCREEN_H - 1 - px : px];
        line[px] = r < 0 ? 0 : col[r];
    }
    for (int i = 0; i < sx; i++) {
        const int lx = x0 + x * sx + i;
        const int py = t->rotate == ROT_CW ? lx : FB_ROT_H - 1 - lx;
        memcpy(t->fb + py * t->stride + px0, line + px0, (size_t)(px1 - px0 + 1) * 2);
    }
}

/* Only what was written since the last call is redrawn (vga_dirty): for each byte column
 * of plane memory, the rows between the first and the last that changed. Windows moving
 * its mouse pointer changes a few dozen bytes, a redraw of the picture is 2 MB. */
static int render_planar(const render_target_t *t, int force, int *row0, int *row1)
{
    static uint32_t rowbase[480];
    static int16_t src_row[SCREEN_H]; /* canvas line -> source row, -1 = border */
    static int16_t first_ly[480], last_ly[480]; /* source row -> its canvas lines */
    static int16_t cmin[192], cmax[192]; /* changed rows, by byte column */
    vga_geometry_t g;
    int cleared = 0;

    if (!force && !video.dirty) {
        return 0;
    }
    video.dirty = 0;
    /* What is written from here on is for the next call. */
    for (uint32_t i = 0; i < VGA_PLANE_SIZE / 32; i++) {
        gfx.planar.taken[i] =
            vga_dirty[i] ? __atomic_exchange_n(&vga_dirty[i], 0, __ATOMIC_ACQUIRE) : 0;
    }
    vga_geometry(&g);

    /* Integer scale factors that keep the pixel shape close to the original mode. */
    const int sx = g.width <= 320 ? ((g.rows > 200 && g.rows <= 240) ? 3 : 4)
                   : g.width <= 426 ? 3 : g.width <= 640 ? 2 : 1;
    int dh = g.rows <= 200 ? g.rows * 4 : g.rows <= 266 ? g.rows * 3
             : g.rows <= 400 ? g.rows * 2 : g.rows * 5 / 3;
    if (dh > SCREEN_H) {
        dh = SCREEN_H;
    }
    const int dw = g.width * sx;
    const int x0 = (SCREEN_W - dw) / 2, y0 = (SCREEN_H - dh) / 2;

    if (planar_rect.x0 != x0 || planar_rect.y0 != y0 || planar_rect.dw != dw ||
        planar_rect.dh != dh || planar_rect.fb != t->fb || planar_rect.rotate != t->rotate) {
        /* New picture size: blank everything once, the borders stay black afterwards. */
        const int fb_rows = t->rotate == ROT_NONE ? SCREEN_H : FB_ROT_H;
        const int fb_cols = t->rotate == ROT_NONE ? SCREEN_W : FB_ROT_W;
        for (int y = 0; y < fb_rows; y++) {
            memset(t->fb + y * t->stride, 0, (size_t)fb_cols * 2);
        }
        planar_rect.x0 = x0;
        planar_rect.y0 = y0;
        planar_rect.dw = dw;
        planar_rect.dh = dh;
        planar_rect.fb = t->fb;
        planar_rect.rotate = t->rotate;
        cleared = 1;
    }

    for (int y = 0; y < g.rows; y++) {
        rowbase[y] = y < g.split_row ? g.start + (uint32_t)y * g.row_bytes
                                     : (uint32_t)(y - g.split_row) * g.row_bytes;
    }
    for (int ly = 0; ly < SCREEN_H; ly++) {
        src_row[ly] = (ly < y0 || ly >= y0 + dh) ? -1 : (int16_t)((ly - y0) * g.rows / dh);
    }
    for (int ly = y0 + dh - 1; ly >= y0; ly--) {
        first_ly[src_row[ly]] = (int16_t)ly;
    }
    for (int ly = y0; ly < y0 + dh; ly++) {
        last_ly[src_row[ly]] = (int16_t)ly;
    }

    /* The byte columns of plane memory the picture shows, and what changed in them. */
    const int ppb = g.is256 ? 4 : 8;
    const int ax0 = g.pan / ppb, ax1 = (g.width - 1 + g.pan) / ppb;
    const int full = force || cleared;
    if (!full) {
        int any = 0;
        for (int ax = ax0; ax <= ax1; ax++) {
            cmin[ax] = (int16_t)g.rows;
            cmax[ax] = -1;
        }
        for (int y = 0; y < g.rows; y++) {
            for (int ax = ax0; ax <= ax1;) {
                const uint32_t off = (rowbase[y] + (uint32_t)ax) & 0xFFFF;
                const uint32_t w = gfx.planar.taken[off >> 5] >> (off & 31);
                if (!w) {
                    ax += 32 - (int)(off & 31);
                    continue;
                }
                if (w & 1) {
                    if (cmax[ax] < 0) {
                        cmin[ax] = (int16_t)y;
                    }
                    cmax[ax] = (int16_t)y;
                    any = 1;
                }
                ax++;
            }
        }
        if (!any) {
            return 0;
        }
    }

    int xmin = g.width, xmax = -1, lymin = SCREEN_H, lymax = -1;
    for (int ax = ax0; ax <= ax1; ax++) {
        int r0 = 0, r1 = g.rows - 1, ly0, ly1;
        if (full) {
            ly0 = t->rotate == ROT_NONE ? y0 : 0;
            ly1 = t->rotate == ROT_NONE ? y0 + dh - 1 : SCREEN_H - 1;
        } else {
            if (cmax[ax] < 0) {
                continue;
            }
            r0 = cmin[ax];
            r1 = cmax[ax];
            ly0 = first_ly[r0];
            ly1 = last_ly[r1];
        }
        if (!g.is256) {
            fetch_planar16(rowbase, (uint32_t)ax, r0, r1);
        }
        for (int k = 0; k < ppb; k++) {
            const int x = ax * ppb + k - g.pan;
            if (x < 0 || x >= g.width) {
                continue;
            }
            if (g.is256) {
                fetch_planar256(rowbase, ax * 4 + k, r0, r1, gfx.planar.cols[0]);
            }
            draw_planar_column(t, src_row, x0, sx, x, gfx.planar.cols[g.is256 ? 0 : k], ly0,
                               ly1);
            xmin = x < xmin ? x : xmin;
            xmax = x;
            lymin = ly0 < lymin ? ly0 : lymin;
            lymax = ly1 > lymax ? ly1 : lymax;
        }
    }
    if (cleared) {
        *row0 = 0;
        *row1 = (t->rotate == ROT_NONE ? SCREEN_H : FB_ROT_H) - 1;
        return 1;
    }
    if (xmax < 0) {
        return 0;
    }
    const int lx0 = x0 + xmin * sx, lx1 = x0 + xmax * sx + sx - 1;
    if (t->rotate == ROT_NONE) {
        *row0 = lymin;
        *row1 = lymax;
    } else if (t->rotate == ROT_CW) {
        *row0 = lx0;
        *row1 = lx1;
    } else {
        *row0 = FB_ROT_H - 1 - lx1;
        *row1 = FB_ROT_H - 1 - lx0;
    }
    return 1;
}

int render_update(const render_target_t *t, uint64_t now_ns, int force, int *row0, int *row1)
{
    if (!font_ready) {
        font_init();
    }
    uint32_t gen = video.generation;
    if (!seen_valid || gen != seen_generation || t->rotate != seen_rotate || t->fb != seen_fb) {
        seen_valid = 1;
        seen_generation = gen;
        seen_rotate = t->rotate;
        seen_fb = t->fb;
        palette_update();
        force = 1;
    }
    if (video.kind == VK_PLANAR) {
        return render_planar(t, force, row0, row1);
    }
    planar_rect.fb = 0; /* a planar picture drawn later starts from a blank canvas */
    if (video.kind == VK_TEXT40 || video.kind == VK_TEXT80) {
        return render_text(t, now_ns, force, row0, row1);
    }
    return render_gfx(t, force, row0, row1);
}
