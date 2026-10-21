/*
 * Video BIOS (INT 10h), implemented on the host side.
 *
 * The adapter is a VGA with an analogue colour display: text modes 0-3, CGA graphics 4-6,
 * the 16-colour planar modes 0Dh, 0Eh, 10h and 12h, 640x480x2 (11h) and 320x200x256 (13h).
 * A mode set loads the standard register values, so software that reprograms the CRTC or
 * sequencer from there (unchained "Mode X" and the like) starts from what it expects.
 */
#include <string.h>

#include "pc.h"
#include "regs.h"
#include "video.h"

#define V_MODE BDA(0x49)
#define V_COLS BDA(0x4A)
#define V_PGSIZE BDA(0x4C)
#define V_PGOFF BDA(0x4E)
#define V_CURPOS BDA(0x50)
#define V_CURTYPE BDA(0x60)
#define V_PAGE BDA(0x62)
#define V_CRTC BDA(0x63)
#define V_MODECTL BDA(0x65)
#define V_PALETTE BDA(0x66)
#define V_ROWS BDA(0x84)
#define V_CHARH BDA(0x85)
#define V_INFO BDA(0x87)

extern const uint8_t p4dos_vgapal[768];

/* Register values of the standard modes (IBM VGA parameter table). */
typedef struct {
    uint8_t misc, seq[4], crtc[25], gc[9], attr[20];
} vregs_t;

#define GC_TEXT {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF}
#define GC_PLANAR {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF}
#define ATTR_EGA(mode, planes) \
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, \
     0x3F, mode, 0x00, planes, 0x00}
#define ATTR_200(mode, planes) \
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, \
     0x17, mode, 0x00, planes, 0x00}

enum { R_TEXT40, R_TEXT80, R_CGA320, R_CGA640, R_0D, R_0E, R_10, R_11, R_12, R_13 };

static const vregs_t vregs[] = {
    [R_TEXT40] = {0x67, {0x08, 0x03, 0x00, 0x02},
                  {0x2D, 0x27, 0x28, 0x90, 0x2B, 0xA0, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00,
                   0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x14, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
                  GC_TEXT, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A,
                            0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08}},
    [R_TEXT80] = {0x67, {0x00, 0x03, 0x00, 0x02},
                  {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00,
                   0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
                  GC_TEXT, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A,
                            0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08}},
    [R_CGA320] = {0x63, {0x09, 0x03, 0x00, 0x02},
                  {0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC1, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xA2, 0xFF},
                  {0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0F, 0x00, 0xFF},
                  {0x00, 0x13, 0x15, 0x17, 0x02, 0x04, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14,
                   0x15, 0x16, 0x17, 0x01, 0x00, 0x03, 0x00}},
    [R_CGA640] = {0x63, {0x01, 0x01, 0x00, 0x06},
                  {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0xC1, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xC2, 0xFF},
                  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x00, 0xFF},
                  {0x00, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17,
                   0x17, 0x17, 0x17, 0x01, 0x00, 0x01, 0x00}},
    [R_0D] = {0x63, {0x09, 0x0F, 0x00, 0x06},
              {0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xE3, 0xFF},
              GC_PLANAR, ATTR_200(0x01, 0x0F)},
    [R_0E] = {0x63, {0x01, 0x0F, 0x00, 0x06},
              {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xE3, 0xFF},
              GC_PLANAR, ATTR_200(0x01, 0x0F)},
    [R_10] = {0xA3, {0x01, 0x0F, 0x00, 0x06},
              {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x40, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x83, 0x85, 0x5D, 0x28, 0x0F, 0x63, 0xBA, 0xE3, 0xFF},
              GC_PLANAR, ATTR_EGA(0x01, 0x0F)},
    [R_11] = {0xE3, {0x01, 0x0F, 0x00, 0x06},
              {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xC3, 0xFF},
              GC_PLANAR, {0x00, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F,
                          0x3F, 0x3F, 0x3F, 0x3F, 0x01, 0x00, 0x0F, 0x00}},
    [R_12] = {0xE3, {0x01, 0x0F, 0x00, 0x06},
              {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF},
              GC_PLANAR, ATTR_EGA(0x01, 0x0F)},
    [R_13] = {0x63, {0x01, 0x0F, 0x00, 0x0E},
              {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF},
              {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF},
              {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
               0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00}},
};

enum { DAC_EGA, DAC_200, DAC_256 };

typedef struct {
    uint8_t mode, kind, cols, rows, charh, ctl, pal, pages;
    uint16_t pgsize;
    uint8_t regs, dac;
} vmode_t;

static const vmode_t vmodes[] = {
    {0x00, VK_TEXT40, 40, 25, 16, 0x2C, 0x30, 8, 0x0800, R_TEXT40, DAC_EGA},
    {0x01, VK_TEXT40, 40, 25, 16, 0x28, 0x30, 8, 0x0800, R_TEXT40, DAC_EGA},
    {0x02, VK_TEXT80, 80, 25, 16, 0x2D, 0x30, 8, 0x1000, R_TEXT80, DAC_EGA},
    {0x03, VK_TEXT80, 80, 25, 16, 0x29, 0x30, 8, 0x1000, R_TEXT80, DAC_EGA},
    {0x04, VK_CGA320, 40, 25, 8, 0x2A, 0x30, 1, 0x4000, R_CGA320, DAC_EGA},
    {0x05, VK_CGA320, 40, 25, 8, 0x2E, 0x30, 1, 0x4000, R_CGA320, DAC_EGA},
    {0x06, VK_CGA640, 80, 25, 8, 0x1E, 0x3F, 1, 0x4000, R_CGA640, DAC_EGA},
    {0x0D, VK_PLANAR, 40, 25, 8, 0x2A, 0x30, 8, 0x2000, R_0D, DAC_200},
    {0x0E, VK_PLANAR, 80, 25, 8, 0x1E, 0x30, 4, 0x4000, R_0E, DAC_200},
    {0x10, VK_PLANAR, 80, 25, 14, 0x1E, 0x30, 2, 0x8000, R_10, DAC_EGA},
    {0x11, VK_MCGA640, 80, 30, 16, 0x1E, 0x3F, 1, 0xA000, R_11, DAC_EGA},
    {0x12, VK_PLANAR, 80, 30, 16, 0x1E, 0x30, 1, 0xA000, R_12, DAC_EGA},
    {0x13, VK_MCGA256, 40, 25, 8, 0x0A, 0x30, 1, 0x2000, R_13, DAC_256},
};

static const vmode_t *cur_mode(void)
{
    uint8_t mode = mem_r8(V_MODE);
    for (unsigned i = 0; i < sizeof(vmodes) / sizeof(vmodes[0]); i++) {
        if (vmodes[i].mode == mode) {
            return &vmodes[i];
        }
    }
    return &vmodes[3];
}

static uint16_t rom16(int off)
{
    return (uint16_t)(p4dos_rom[off] | (p4dos_rom[off + 1] << 8));
}

static int is_text(void)
{
    return cur_mode()->kind <= VK_TEXT80;
}

/* --- cursor --------------------------------------------------------------- */

static void crtc_cursor_addr(void)
{
    uint8_t page = mem_r8(V_PAGE);
    uint16_t pos = mem_r16(V_CURPOS + page * 2);
    uint16_t a = (uint16_t)(mem_r16(V_PGOFF) / 2 + (pos >> 8) * mem_r16(V_COLS) + (pos & 0xFF));
    video.crtc[0x0E] = (uint8_t)(a >> 8);
    video.crtc[0x0F] = (uint8_t)a;
}

static void set_cursor_pos(uint8_t page, uint8_t row, uint8_t col)
{
    page &= 7;
    mem_w16(V_CURPOS + page * 2, (uint16_t)((row << 8) | col));
    if (page == mem_r8(V_PAGE)) {
        crtc_cursor_addr();
    }
}

static void set_cursor_shape(uint8_t start, uint8_t end)
{
    mem_w16(V_CURTYPE, (uint16_t)((start << 8) | end));
    start &= 0x3F;
    end &= 0x1F;
    /* CGA-style 8-line shapes are stretched to the 16-line character cell. */
    if (end < 8 && start < 0x20) {
        if (end != start + 1) {
            start = (uint8_t)((start + 1) * 2 - 1);
        } else {
            start = (uint8_t)((end + 1) * 2 - 2);
        }
        end = (uint8_t)((end + 1) * 2 - 1);
    }
    if ((start & 0x1F) > end) {
        start |= 0x20;
    }
    video.crtc[0x0A] = start;
    video.crtc[0x0B] = end;
}

static void set_vector(uint8_t n, uint16_t seg, uint16_t off)
{
    mem_w16(n * 4u, off);
    mem_w16(n * 4u + 2, seg);
}

/* --- mode set ------------------------------------------------------------- */

/* DAC contents after a mode set: the 64 EGA colours for text and 16-colour modes, the
 * CGA-style arrangement the 200-line modes use, or the 256-colour default palette. */
static void load_dac(int which)
{
    static const uint8_t cga[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0x2A}, {0x00, 0x2A, 0x00}, {0x00, 0x2A, 0x2A},
        {0x2A, 0x00, 0x00}, {0x2A, 0x00, 0x2A}, {0x2A, 0x15, 0x00}, {0x2A, 0x2A, 0x2A},
        {0x15, 0x15, 0x15}, {0x15, 0x15, 0x3F}, {0x15, 0x3F, 0x15}, {0x15, 0x3F, 0x3F},
        {0x3F, 0x15, 0x15}, {0x3F, 0x15, 0x3F}, {0x3F, 0x3F, 0x15}, {0x3F, 0x3F, 0x3F}};
    memcpy(video.dac, p4dos_vgapal, sizeof(video.dac));
    for (int i = 0; i < 64 && which != DAC_256; i++) {
        uint8_t *d = video.dac[i];
        if (which == DAC_EGA) { /* bits: 0 B, 1 G, 2 R at 2/3 intensity; 3 b, 4 g, 5 r at 1/3 */
            d[0] = (uint8_t)(((i >> 2) & 1) * 0x2A + ((i >> 5) & 1) * 0x15);
            d[1] = (uint8_t)(((i >> 1) & 1) * 0x2A + ((i >> 4) & 1) * 0x15);
            d[2] = (uint8_t)((i & 1) * 0x2A + ((i >> 3) & 1) * 0x15);
        } else {
            memcpy(d, cga[(i & 7) | ((i & 0x10) >> 1)], 3);
        }
    }
    video.dac_mask = 0xFF;
}

static void set_mode(uint8_t al)
{
    const vmode_t *m = NULL;
    for (unsigned i = 0; i < sizeof(vmodes) / sizeof(vmodes[0]); i++) {
        if (vmodes[i].mode == (al & 0x7F)) {
            m = &vmodes[i];
        }
    }
    if (!m) {
        return;
    }
    mem_w8(V_MODE, m->mode);
    mem_w16(V_COLS, m->cols);
    mem_w16(V_PGSIZE, m->pgsize);
    mem_w16(V_PGOFF, 0);
    for (int i = 0; i < 8; i++) {
        mem_w16(V_CURPOS + i * 2, 0);
    }
    mem_w8(V_PAGE, 0);
    mem_w16(V_CRTC, 0x3D4);
    mem_w8(V_MODECTL, m->ctl);
    mem_w8(V_PALETTE, m->pal);
    mem_w8(V_ROWS, (uint8_t)(m->rows - 1));
    mem_w16(V_CHARH, m->charh);
    mem_w8(V_INFO, (uint8_t)((mem_r8(V_INFO) & 0x7F) | (al & 0x80)));

    const vregs_t *r = &vregs[m->regs];
    video.mode_ctl = m->ctl;
    video.color_sel = m->pal;
    video.misc = r->misc;
    video.seq[0] = 0x03;
    memcpy(video.seq + 1, r->seq, sizeof(r->seq));
    memset(video.crtc, 0, sizeof(video.crtc));
    memcpy(video.crtc, r->crtc, sizeof(r->crtc));
    memset(video.gc, 0, sizeof(video.gc));
    memcpy(video.gc, r->gc, sizeof(r->gc));
    memset(video.attr, 0, sizeof(video.attr));
    memcpy(video.attr, r->attr, sizeof(r->attr));
    video.attr_flip = 0;
    load_dac(m->dac);
    set_cursor_shape(6, 7);
    video_set_kind(m->kind);

    if (!(al & 0x80)) {
        if (m->kind <= VK_TEXT80) {
            for (uint32_t a = 0xB8000; a < 0xC0000; a += 2) {
                cpu_mem[a] = 0x20;
                cpu_mem[a + 1] = 0x07;
            }
        } else if (m->kind <= VK_CGA640) {
            memset(cpu_mem + 0xB8000, 0, 0x4000);
        } else {
            memset(cpu_mem + 0xA0000, 0, 0x10000);
            memset(vga_plane[0], 0, VGA_RAM_SIZE);
            vga_touch(0, VGA_PLANE_SIZE);
        }
    }
    set_vector(0x43, 0xF000, m->charh == 16 ? rom16(0x0A) : m->charh == 14 ? rom16(0x18) : rom16(0x0C));
}

void bios_video_init(void)
{
    set_vector(0x1F, 0xF000, rom16(0x0E));
    mem_w8(BDA(0x87), 0x60); /* 256 KiB of video memory */
    mem_w8(BDA(0x88), 0xF9); /* feature bits and switches: EGA+ colour */
    mem_w8(BDA(0x89), 0x51); /* VGA active, 400-line text */
    mem_w8(BDA(0x8A), 0x0B); /* display combination: VGA with colour display */
    set_mode(3);
}

/* --- graphics helpers ----------------------------------------------------- */

static uint32_t gfx_line(int y)
{
    switch (cur_mode()->kind) {
    case VK_CGA320:
    case VK_CGA640: return 0xB8000u + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80);
    case VK_MCGA640: return 0xA0000u + (uint32_t)y * 80;
    default: return 0xA0000u + (uint32_t)y * 320;
    }
}

static int gfx_bytes_per_char(void)
{
    switch (cur_mode()->kind) {
    case VK_CGA320: return 2;
    case VK_MCGA256: return 8;
    default: return 1;
    }
}

static const uint8_t *gfx_glyph(uint8_t ch)
{
    if (cur_mode()->charh == 16) {
        return p4dos_rom + rom16(0x0A) + ch * 16;
    }
    if (cur_mode()->charh == 14) {
        return p4dos_rom + rom16(0x18) + ch * 14;
    }
    if (ch < 0x80) {
        return p4dos_rom + rom16(0x0C) + ch * 8;
    }
    /* Upper half comes from the user-replaceable INT 1Fh table. */
    return cpu_mem + ((SEGOFF(mem_r16(0x1F * 4 + 2), mem_r16(0x1F * 4)) + (ch - 0x80u) * 8) &
                      CPU_MEM_MASK);
}

/* --- 16-colour planar modes: one bit per pixel in each of four planes --------- */

static int is_planar(void)
{
    return cur_mode()->kind == VK_PLANAR;
}

static void planar_putc(uint8_t ch, uint8_t color, int row, int col)
{
    const vmode_t *m = cur_mode();
    const uint8_t *g = gfx_glyph(ch);
    for (int gy = 0; gy < m->charh; gy++) {
        uint32_t a = ((uint32_t)(row * m->charh + gy) * m->cols + (uint32_t)col) & 0xFFFF;
        for (int p = 0; p < 4; p++) {
            if (color & 0x80) {
                if (color & (1 << p)) {
                    vga_plane[p][a] ^= g[gy];
                }
            } else {
                vga_plane[p][a] = (color & (1 << p)) ? g[gy] : 0;
            }
        }
        vga_touch(a, 1);
    }
}

static uint8_t planar_get_pixel(int x, int y)
{
    uint32_t a = ((uint32_t)y * cur_mode()->cols + (uint32_t)(x >> 3)) & 0xFFFF;
    uint8_t bit = (uint8_t)(0x80 >> (x & 7)), c = 0;
    for (int p = 0; p < 4; p++) {
        if (vga_plane[p][a] & bit) {
            c |= (uint8_t)(1 << p);
        }
    }
    return c;
}

static void planar_put_pixel(int x, int y, uint8_t color)
{
    uint32_t a = ((uint32_t)y * cur_mode()->cols + (uint32_t)(x >> 3)) & 0xFFFF;
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    for (int p = 0; p < 4; p++) {
        uint8_t on = (color >> p) & 1;
        if (color & 0x80) {
            if (on) {
                vga_plane[p][a] ^= bit;
            }
        } else if (on) {
            vga_plane[p][a] |= bit;
        } else {
            vga_plane[p][a] &= (uint8_t)~bit;
        }
    }
    vga_touch(a, 1);
}

static void planar_scroll(int up, int lines, uint8_t attr, int r0, int c0, int r1, int c1)
{
    const vmode_t *m = cur_mode();
    size_t w = (size_t)(c1 - c0 + 1);
    int y0 = r0 * m->charh, y1 = (r1 + 1) * m->charh - 1, n = lines * m->charh;
    for (int p = 0; p < 4; p++) {
        uint8_t fill = (attr & (1 << p)) ? 0xFF : 0;
        for (int i = 0; i <= y1 - y0; i++) {
            int y = up ? y0 + i : y1 - i;
            int src = up ? y + n : y - n;
            uint8_t *dst = vga_plane[p] + y * m->cols + c0;
            if (src >= y0 && src <= y1) {
                memmove(dst, vga_plane[p] + src * m->cols + c0, w);
            } else {
                memset(dst, fill, w);
            }
            if (p == 0) {
                vga_touch((uint32_t)(y * m->cols + c0), (uint32_t)w);
            }
        }
    }
}

static void gfx_putc(uint8_t ch, uint8_t color, int row, int col)
{
    const vmode_t *m = cur_mode();
    const uint8_t *g = gfx_glyph(ch);
    if (row >= m->rows || col >= m->cols) {
        return;
    }
    if (is_planar()) {
        planar_putc(ch, color, row, col);
        return;
    }
    for (int gy = 0; gy < m->charh; gy++) {
        uint32_t a = gfx_line(row * m->charh + gy);
        uint8_t bits = g[gy];
        switch (m->kind) {
        case VK_CGA320: {
            uint16_t w = 0;
            for (int b = 0; b < 8; b++) {
                if (bits & (0x80 >> b)) {
                    w |= (uint16_t)((color & 3) << (14 - 2 * b));
                }
            }
            a += (uint32_t)col * 2;
            if (color & 0x80) {
                cpu_mem[a] ^= (uint8_t)(w >> 8);
                cpu_mem[a + 1] ^= (uint8_t)w;
            } else {
                cpu_mem[a] = (uint8_t)(w >> 8);
                cpu_mem[a + 1] = (uint8_t)w;
            }
            break;
        }
        case VK_MCGA256:
            a += (uint32_t)col * 8;
            for (int b = 0; b < 8; b++) {
                cpu_mem[a + b] = (bits & (0x80 >> b)) ? color : 0;
            }
            break;
        default:
            a += (uint32_t)col;
            if (color & 0x80) {
                cpu_mem[a] ^= bits;
            } else {
                cpu_mem[a] = (color & 1) ? bits : 0;
            }
            break;
        }
    }
}

static uint8_t gfx_getc(int row, int col)
{
    const vmode_t *m = cur_mode();
    uint8_t pat[16];
    for (int gy = 0; gy < m->charh; gy++) {
        uint32_t a = gfx_line(row * m->charh + gy);
        uint8_t bits = 0;
        if (is_planar()) {
            for (int b = 0; b < 8; b++) {
                if (planar_get_pixel(col * 8 + b, row * m->charh + gy)) {
                    bits |= (uint8_t)(0x80 >> b);
                }
            }
            pat[gy] = bits;
            continue;
        }
        switch (m->kind) {
        case VK_CGA320: {
            uint16_t w = (uint16_t)((cpu_mem[a + col * 2] << 8) | cpu_mem[a + col * 2 + 1]);
            for (int b = 0; b < 8; b++) {
                if ((w >> (14 - 2 * b)) & 3) {
                    bits |= (uint8_t)(0x80 >> b);
                }
            }
            break;
        }
        case VK_MCGA256:
            for (int b = 0; b < 8; b++) {
                if (cpu_mem[a + col * 8 + b]) {
                    bits |= (uint8_t)(0x80 >> b);
                }
            }
            break;
        default: bits = cpu_mem[a + col]; break;
        }
        pat[gy] = bits;
    }
    for (int ch = 0; ch < 256; ch++) {
        if (!memcmp(pat, gfx_glyph((uint8_t)ch), m->charh)) {
            return (uint8_t)ch;
        }
    }
    return 0;
}

static void put_pixel(int x, int y, uint8_t color)
{
    const vmode_t *m = cur_mode();
    if (is_planar()) {
        planar_put_pixel(x, y, color);
        return;
    }
    uint32_t a = gfx_line(y);
    switch (m->kind) {
    case VK_CGA320: {
        int sh = 6 - 2 * (x & 3);
        a += (uint32_t)(x >> 2);
        if (color & 0x80) {
            cpu_mem[a] ^= (uint8_t)((color & 3) << sh);
        } else {
            cpu_mem[a] = (uint8_t)((cpu_mem[a] & ~(3 << sh)) | ((color & 3) << sh));
        }
        break;
    }
    case VK_MCGA256: cpu_mem[a + x] = color; break;
    default: {
        uint8_t bit = (uint8_t)(0x80 >> (x & 7));
        a += (uint32_t)(x >> 3);
        if (color & 0x80) {
            if (color & 1) {
                cpu_mem[a] ^= bit;
            }
        } else if (color & 1) {
            cpu_mem[a] |= bit;
        } else {
            cpu_mem[a] &= (uint8_t)~bit;
        }
        break;
    }
    }
}

static uint8_t get_pixel(int x, int y)
{
    if (is_planar()) {
        return planar_get_pixel(x, y);
    }
    uint32_t a = gfx_line(y);
    switch (cur_mode()->kind) {
    case VK_CGA320: return (cpu_mem[a + (x >> 2)] >> (6 - 2 * (x & 3))) & 3;
    case VK_MCGA256: return cpu_mem[a + x];
    default: return (cpu_mem[a + (x >> 3)] >> (7 - (x & 7))) & 1;
    }
}

/* --- scrolling ------------------------------------------------------------ */

static void scroll(int up, int lines, uint8_t attr, int r0, int c0, int r1, int c1, int page)
{
    const vmode_t *m = cur_mode();
    if (r1 >= m->rows) {
        r1 = m->rows - 1;
    }
    if (c1 >= m->cols) {
        c1 = m->cols - 1;
    }
    if (r0 > r1 || c0 > c1) {
        return;
    }
    int h = r1 - r0 + 1;
    if (lines == 0 || lines > h) {
        lines = h;
    }

    if (m->kind <= VK_TEXT80) {
        uint8_t *base = cpu_mem + 0xB8000 + (uint32_t)(page & 7) * m->pgsize;
        size_t w = (size_t)(c1 - c0 + 1) * 2;
#define TROW(r) (base + ((r) * m->cols + c0) * 2)
        for (int i = 0; i < h; i++) {
            int r = up ? r0 + i : r1 - i;
            int src = up ? r + lines : r - lines;
            if (src >= r0 && src <= r1) {
                memmove(TROW(r), TROW(src), w);
            } else {
                uint8_t *p = TROW(r);
                for (size_t k = 0; k < w; k += 2) {
                    p[k] = 0x20;
                    p[k + 1] = attr;
                }
            }
        }
#undef TROW
        return;
    }

    if (is_planar()) {
        planar_scroll(up, lines, attr, r0, c0, r1, c1);
        return;
    }
    int bpc = gfx_bytes_per_char();
    size_t w = (size_t)(c1 - c0 + 1) * (size_t)bpc;
    uint8_t fill = attr;
    if (m->kind == VK_CGA320) {
        fill = (uint8_t)((attr & 3) * 0x55);
    } else if (m->kind != VK_MCGA256) {
        fill = (attr & 1) ? 0xFF : 0;
    }
    int y0 = r0 * m->charh, y1 = (r1 + 1) * m->charh - 1, n = lines * m->charh;
    for (int i = 0; i <= y1 - y0; i++) {
        int y = up ? y0 + i : y1 - i;
        int src = up ? y + n : y - n;
        uint8_t *dst = cpu_mem + gfx_line(y) + c0 * bpc;
        if (src >= y0 && src <= y1) {
            memmove(dst, cpu_mem + gfx_line(src) + c0 * bpc, w);
        } else {
            memset(dst, fill, w);
        }
    }
}

/* --- character output ----------------------------------------------------- */

static void write_cell(uint8_t page, int row, int col, uint8_t ch, uint8_t attr, int use_attr)
{
    const vmode_t *m = cur_mode();
    if (m->kind <= VK_TEXT80) {
        uint32_t a = 0xB8000u + (uint32_t)(page & 7) * m->pgsize + (uint32_t)(row * m->cols + col) * 2;
        cpu_mem[a] = ch;
        if (use_attr) {
            cpu_mem[a + 1] = attr;
        }
    } else {
        gfx_putc(ch, attr, row, col);
    }
}

/* Teletype output on the given page. With use_attr the attribute is written too. */
static void tty_out(uint8_t page, uint8_t ch, uint8_t attr, int use_attr)
{
    const vmode_t *m = cur_mode();
    uint16_t pos = mem_r16(V_CURPOS + (page & 7) * 2);
    int row = pos >> 8, col = pos & 0xFF;

    switch (ch) {
    case 7: pc_beep(); return;
    case 8:
        if (col > 0) {
            col--;
        }
        break;
    case 10: row++; break;
    case 13: col = 0; break;
    default:
        write_cell(page, row, col, ch, attr, use_attr);
        if (++col >= m->cols) {
            col = 0;
            row++;
        }
        break;
    }
    if (row >= m->rows) {
        uint8_t fill = 0;
        row = m->rows - 1;
        if (m->kind <= VK_TEXT80) {
            fill = cpu_mem[0xB8000u + (uint32_t)(page & 7) * m->pgsize +
                           (uint32_t)(row * m->cols + col) * 2 + 1];
        }
        scroll(1, 1, fill, 0, 0, m->rows - 1, m->cols - 1, page);
    }
    set_cursor_pos(page, (uint8_t)row, (uint8_t)col);
}

void bios_tty(uint8_t ch, uint8_t attr)
{
    if (!is_text() && !attr) {
        attr = 7;
    }
    tty_out(mem_r8(V_PAGE), ch, attr, attr != 0);
}

void bios_puts(const char *s)
{
    while (*s) {
        bios_tty((uint8_t)*s++, 0);
    }
}

/* --- INT 10h -------------------------------------------------------------- */

static void int10_dac(void)
{
    switch (AL) {
    case 0x00: /* one attribute palette register */
        if (BL < 0x15) {
            video.attr[BL] = BH;
            video.generation++;
        }
        break;
    case 0x01: video.attr[0x11] = BH; break;
    case 0x02: /* all sixteen palette registers and the overscan colour */
        for (int i = 0; i < 17; i++) {
            video.attr[i] = mem_r8(SEGOFF(ES, DX + i));
        }
        video.generation++;
        break;
    case 0x03: /* blink / intensity */
        video.mode_ctl = (uint8_t)((video.mode_ctl & ~0x20) | ((BL & 1) << 5));
        video.attr[0x10] = (uint8_t)((video.attr[0x10] & ~0x08) | ((BL & 1) << 3));
        mem_w8(V_MODECTL, video.mode_ctl);
        video.generation++;
        break;
    case 0x07:
        if (BL < 0x15) {
            BH = video.attr[BL];
        }
        break;
    case 0x08: BH = video.attr[0x11]; break;
    case 0x09:
        for (int i = 0; i < 17; i++) {
            mem_w8(SEGOFF(ES, DX + i), video.attr[i]);
        }
        break;
    case 0x10:
        video.dac[BX & 0xFF][0] = DH & 0x3F;
        video.dac[BX & 0xFF][1] = CH & 0x3F;
        video.dac[BX & 0xFF][2] = CL & 0x3F;
        video.generation++;
        break;
    case 0x12:
        for (uint32_t i = 0; i < CX; i++) {
            uint32_t a = SEGOFF(ES, DX + i * 3);
            uint8_t *d = video.dac[(BX + i) & 0xFF];
            d[0] = mem_r8(a) & 0x3F;
            d[1] = mem_r8(a + 1) & 0x3F;
            d[2] = mem_r8(a + 2) & 0x3F;
        }
        video.generation++;
        break;
    case 0x15:
        DH = video.dac[BX & 0xFF][0];
        CH = video.dac[BX & 0xFF][1];
        CL = video.dac[BX & 0xFF][2];
        break;
    case 0x17:
        for (uint32_t i = 0; i < CX; i++) {
            uint32_t a = SEGOFF(ES, DX + i * 3);
            const uint8_t *d = video.dac[(BX + i) & 0xFF];
            if ((a & CPU_MEM_MASK) + 2 < pc_ram_limit) {
                mem_w8(a, d[0]);
                mem_w8(a + 1, d[1]);
                mem_w8(a + 2, d[2]);
            }
        }
        break;
    case 0x1A: BX = 0; break;
    case 0x1B: /* grey-scale summing */
        for (uint32_t i = 0; i < CX; i++) {
            uint8_t *d = video.dac[(BX + i) & 0xFF];
            uint8_t g = (uint8_t)((d[0] * 30 + d[1] * 59 + d[2] * 11) / 100);
            d[0] = d[1] = d[2] = g;
        }
        video.generation++;
        break;
    default: break;
    }
}

void bios_int10(void)
{
    const vmode_t *m = cur_mode();
    uint8_t page;

    switch (AH) {
    case 0x00: set_mode(AL); break;
    case 0x01: set_cursor_shape(CH, CL); break;
    case 0x02: set_cursor_pos(BH, DH, DL); break;
    case 0x03:
        DX = mem_r16(V_CURPOS + (BH & 7) * 2);
        CX = mem_r16(V_CURTYPE);
        break;
    case 0x04: AH = 0; break;
    case 0x05:
        if (AL < m->pages) {
            mem_w8(V_PAGE, AL);
            mem_w16(V_PGOFF, (uint16_t)(AL * m->pgsize));
            video.crtc[0x0C] = (uint8_t)((AL * m->pgsize / 2) >> 8);
            video.crtc[0x0D] = (uint8_t)(AL * m->pgsize / 2);
            crtc_cursor_addr();
        }
        break;
    case 0x06: scroll(1, AL, BH, CH, CL, DH, DL, mem_r8(V_PAGE)); break;
    case 0x07: scroll(0, AL, BH, CH, CL, DH, DL, mem_r8(V_PAGE)); break;
    case 0x08: {
        uint16_t pos = mem_r16(V_CURPOS + (BH & 7) * 2);
        if (is_text()) {
            AX = mem_r16(0xB8000u + (uint32_t)(BH & 7) * m->pgsize +
                         (uint32_t)((pos >> 8) * m->cols + (pos & 0xFF)) * 2);
        } else {
            AL = gfx_getc(pos >> 8, pos & 0xFF);
        }
        break;
    }
    case 0x09:
    case 0x0A: {
        page = is_text() ? BH : 0;
        uint16_t pos = mem_r16(V_CURPOS + (page & 7) * 2);
        int row = pos >> 8, col = pos & 0xFF;
        for (uint32_t i = 0; i < CX && row < m->rows; i++) {
            write_cell(page, row, col, AL, BL, AH == 0x09 || !is_text());
            if (++col >= m->cols) {
                col = 0;
                row++;
            }
        }
        break;
    }
    case 0x0B:
        if (BH == 0) {
            video.color_sel = (uint8_t)((video.color_sel & 0xE0) | (BL & 0x1F));
        } else {
            video.color_sel = (uint8_t)((video.color_sel & 0xDF) | ((BL & 1) << 5));
        }
        mem_w8(V_PALETTE, video.color_sel);
        video.generation++;
        break;
    case 0x0C:
        if (!is_text() && CX < m->cols * 8u && DX < (uint32_t)(m->rows * m->charh)) {
            put_pixel(CX, DX, AL);
        }
        break;
    case 0x0D:
        if (!is_text() && CX < m->cols * 8u && DX < (uint32_t)(m->rows * m->charh)) {
            AL = get_pixel(CX, DX);
        }
        break;
    case 0x0E: tty_out(mem_r8(V_PAGE), AL, BL, !is_text()); break;
    case 0x0F:
        AL = (uint8_t)(mem_r8(V_MODE) | (mem_r8(V_INFO) & 0x80));
        AH = (uint8_t)mem_r16(V_COLS);
        BH = mem_r8(V_PAGE);
        break;
    case 0x10: int10_dac(); break;
    case 0x11:
        if (AL == 0x30) {
            uint16_t seg = 0xF000, off;
            switch (BH) {
            case 0:
                off = mem_r16(0x1F * 4);
                seg = mem_r16(0x1F * 4 + 2);
                break;
            case 1:
                off = mem_r16(0x43 * 4);
                seg = mem_r16(0x43 * 4 + 2);
                break;
            case 2: off = rom16(0x18); break;
            case 3: off = rom16(0x0C); break;
            case 4: off = rom16(0x0E); break;
            default: off = rom16(0x0A); break;
            }
            cpu_set_seg(SEG_ES, seg);
            BP = off;
            CX = mem_r16(V_CHARH);
            DL = mem_r8(V_ROWS);
        }
        break;
    case 0x13: {
        page = BH;
        uint16_t saved = mem_r16(V_CURPOS + (page & 7) * 2);
        uint32_t src = SEGOFF(ES, BP);
        uint8_t attr = BL;
        set_cursor_pos(page, DH, DL);
        for (uint32_t i = 0; i < CX; i++) {
            uint8_t ch = mem_r8(src++);
            if (AL & 2) {
                attr = mem_r8(src++);
            }
            tty_out(page, ch, attr, 1);
        }
        if (!(AL & 1)) {
            set_cursor_pos(page, (uint8_t)(saved >> 8), (uint8_t)saved);
        }
        break;
    }
    case 0x12:
        if (BL == 0x10) { /* EGA/VGA information */
            BX = 0x0003; /* colour, 256 KiB */
            CX = 0x0009;
        } else if (BL >= 0x30 && BL <= 0x36) {
            AL = 0x12;
        }
        break;
    case 0x1A:
        if (AL == 0) {
            BX = 0x0008; /* VGA with analogue colour display */
        }
        if (AL <= 1) {
            AL = 0x1A;
        }
        break;
    case 0x1B:
        if (BX == 0) { /* functionality / state information */
            uint32_t p = SEGOFF(ES, DI);
            for (int i = 0; i < 64; i++) {
                mem_w8(p + (uint32_t)i, 0);
            }
            mem_w8(p + 0x04, mem_r8(V_MODE));
            mem_w16(p + 0x05, mem_r16(V_COLS));
            mem_w16(p + 0x07, mem_r16(V_PGSIZE));
            mem_w16(p + 0x09, mem_r16(V_PGOFF));
            for (int i = 0; i < 16; i++) {
                mem_w8(p + 0x0B + (uint32_t)i, mem_r8(V_CURPOS + (uint32_t)i));
            }
            mem_w16(p + 0x1B, mem_r16(V_CURTYPE));
            mem_w8(p + 0x1D, mem_r8(V_PAGE));
            mem_w16(p + 0x1E, 0x3D4);
            mem_w8(p + 0x20, video.mode_ctl);
            mem_w8(p + 0x21, video.color_sel);
            mem_w8(p + 0x22, (uint8_t)m->rows);
            mem_w16(p + 0x23, m->charh);
            mem_w8(p + 0x25, 0x08);
            mem_w16(p + 0x27, m->kind == VK_MCGA256 ? 256 : 16);
            mem_w8(p + 0x29, m->pages);
            mem_w8(p + 0x2A, m->rows * m->charh >= 480 ? 3 : m->rows * m->charh >= 400 ? 2 : 0);
            mem_w8(p + 0x31, 3);
            AL = 0x1B;
        }
        break;
    default: break;
    }
}
