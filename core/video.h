/*
 * Display adapter: a VGA. Text modes, the CGA graphics modes, the 16-colour planar modes,
 * 320x200x256 and register-programmed unchained 256-colour modes ("Mode X"), with a
 * renderer that scales the picture onto the 1280x800 landscape canvas of the Guition panel,
 * optionally rotated into the panel's native 800x1280 portrait frame buffer.
 *
 * Text, CGA and chained 256-colour pictures live in ordinary guest memory (B8000h, A0000h).
 * The planar modes use four 64 KiB planes; while one is active the CPU's accesses to
 * A0000h-AFFFFh are routed through vga_mem_read/vga_mem_write.
 */
#ifndef P4DOS_VIDEO_H
#define P4DOS_VIDEO_H

#include <stdint.h>

enum {
    VK_TEXT40,  /* 40x25 text, B800 */
    VK_TEXT80,  /* 80x25 text, B800 */
    VK_CGA320,  /* 320x200, 4 colours, B800 interlaced */
    VK_CGA640,  /* 640x200, 2 colours, B800 interlaced */
    VK_MCGA256, /* 320x200, 256 colours, A000, chained */
    VK_MCGA640, /* 640x480, 2 colours, A000 */
    VK_PLANAR,  /* planar 16-colour or unchained 256-colour; geometry from the registers */
};

#define VGA_PLANE_SIZE 0x10000u
#define VGA_RAM_SIZE (4 * VGA_PLANE_SIZE)

typedef struct {
    uint8_t kind;
    uint8_t mode_ctl;  /* 3D8, CGA compatibility */
    uint8_t color_sel; /* 3D9, CGA compatibility */
    uint8_t misc;
    uint8_t crtc_idx, crtc[32];
    uint8_t seq_idx, seq[8];
    uint8_t gc_idx, gc[16];
    uint8_t attr_idx, attr_flip, attr[32];
    uint8_t dac[256][3]; /* 6-bit components */
    uint8_t dac_widx, dac_ridx, dac_wphase, dac_rphase, dac_mask;
    uint8_t latch[4];
    uint8_t dirty;       /* plane memory written since the last render */
    uint32_t generation; /* bumped on any palette, register or mode change */
    uint64_t frame_t0;   /* start of the current display frame, for the status register */
} video_t;

extern video_t video;
extern uint8_t *vga_plane[4];
/* Plane offsets written since the renderer last looked, a bit each, so that it redraws
 * only what changed (planar modes). */
extern uint32_t vga_dirty[VGA_PLANE_SIZE / 32];

void video_init(uint8_t *vram); /* VGA_RAM_SIZE bytes */
void video_reset(void);
void video_set_kind(int kind);
uint8_t video_inb(uint16_t port);
void video_outb(uint16_t port, uint8_t v);
uint8_t vga_mem_read(uint32_t off);
void vga_mem_write(uint32_t off, uint8_t v);
/* n bytes of plane memory from off (wrapping at 64 KiB) were written other than through
 * vga_mem_write. */
void vga_touch(uint32_t off, uint32_t n);

/* Geometry of the picture in a VK_PLANAR mode, derived from the CRTC and friends. */
typedef struct {
    int width, rows;       /* pixels */
    int is256;             /* 256-colour (one byte per pixel, plane = x & 3) */
    uint32_t start;        /* display start address */
    uint32_t row_bytes;    /* address increment per pixel row */
    int row_scanlines;     /* scan lines per pixel row */
    int split_row;         /* first row drawn from address 0 (line compare), or rows */
    int pan;               /* horizontal pixel panning */
} vga_geometry_t;

void vga_geometry(vga_geometry_t *g);

/* --- renderer ------------------------------------------------------------- */

#define SCREEN_W 1280
#define SCREEN_H 800

enum {
    ROT_NONE, /* frame buffer is 1280x800, upright */
    ROT_CW,   /* frame buffer is 800x1280: fb(x, y) = screen(y, 799 - x) */
    ROT_CCW,  /* frame buffer is 800x1280: fb(x, y) = screen(1279 - y, x) */
};

typedef struct {
    uint16_t *fb; /* RGB565 */
    int stride;   /* pixels per frame buffer row */
    int rotate;
} render_target_t;

/* Redraw whatever changed since the last call. Returns 0 if nothing was drawn, else 1
 * with the inclusive range of touched frame buffer rows in *row0 and *row1. */
int render_update(const render_target_t *t, uint64_t now_ns, int force, int *row0, int *row1);

#endif
