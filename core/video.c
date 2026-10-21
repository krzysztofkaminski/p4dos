/* VGA registers, planar memory and display timing. See video.h. */
#include "video.h"

#include <string.h>

#include "pc.h"

video_t video;
uint8_t *vga_plane[4];
uint32_t vga_dirty[VGA_PLANE_SIZE / 32];

/* The renderer may run on another core: it takes the bits with an atomic exchange, and
 * the release makes sure it then finds the plane bytes they stand for. */
static inline void mark(uint32_t off)
{
    __atomic_fetch_or(&vga_dirty[off >> 5], 1u << (off & 31), __ATOMIC_RELEASE);
}

void vga_touch(uint32_t off, uint32_t n)
{
    for (uint32_t i = 0; i < n && i < VGA_PLANE_SIZE; i++) {
        mark((off + i) & 0xFFFF);
    }
    video.dirty = 1;
}

#define LINE_NS 31778u /* one scan line at 31.47 kHz */

void video_init(uint8_t *vram)
{
    for (int p = 0; p < 4; p++) {
        vga_plane[p] = vram + (size_t)p * VGA_PLANE_SIZE;
    }
    memset(vram, 0, VGA_RAM_SIZE);
}

/* Planar <-> ordinary memory. Chained 256-colour mode keeps pixel x of a row in plane
 * x & 3 at address x >> 2, which is exactly where an unchained mode expects it, so the
 * picture survives a program switching mode 13h to "Mode X" and back. */
static void flat_to_planes(void)
{
    const uint8_t *flat = cpu_mem + 0xA0000;
    for (uint32_t a = 0; a < 0x10000; a++) {
        vga_plane[a & 3][a >> 2] = flat[a];
    }
}

static void planes_to_flat(void)
{
    uint8_t *flat = cpu_mem + 0xA0000;
    for (uint32_t a = 0; a < 0x10000; a++) {
        flat[a] = vga_plane[a & 3][a >> 2];
    }
}

void video_set_kind(int kind)
{
    if (kind == VK_PLANAR && video.kind == VK_MCGA256) {
        flat_to_planes();
    } else if (kind == VK_MCGA256 && video.kind == VK_PLANAR) {
        planes_to_flat();
    }
    video.kind = (uint8_t)kind;
    video.generation++;
    pc_update_mem_limits();
}

void video_reset(void)
{
    uint32_t gen = video.generation;
    memset(&video, 0, sizeof(video));
    video.dac_mask = 0xFF;
    video.kind = VK_TEXT80;
    video.mode_ctl = 0x29;
    video.color_sel = 0x30;
    video.generation = gen + 1;
    pc_update_mem_limits();
}

/* --- planar memory ---------------------------------------------------------- */

uint8_t vga_mem_read(uint32_t off)
{
    off &= 0xFFFF;
    video.latch[0] = vga_plane[0][off];
    video.latch[1] = vga_plane[1][off];
    video.latch[2] = vga_plane[2][off];
    video.latch[3] = vga_plane[3][off];
    if (!(video.gc[5] & 0x08)) {
        return video.latch[video.gc[4] & 3];
    }
    /* Read mode 1: one bit per pixel, set where the colour matches. */
    uint8_t r = 0xFF;
    for (int p = 0; p < 4; p++) {
        if (video.gc[7] & (1 << p)) {
            r &= (uint8_t)~(video.latch[p] ^ ((video.gc[2] & (1 << p)) ? 0xFF : 0));
        }
    }
    return r;
}

/* The four planes side by side in a word, plane p in byte p. A nibble with a bit per
 * plane, spread out to FFh for each bit that is set: */
static const uint32_t nib4[16] = {
    0x00000000, 0x000000FF, 0x0000FF00, 0x0000FFFF, 0x00FF0000, 0x00FF00FF, 0x00FFFF00, 0x00FFFFFF,
    0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF, 0xFFFF0000, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF,
};

static inline uint32_t alu(uint32_t v, uint32_t latch)
{
    switch (video.gc[3] & 0x18) {
    case 0x08: return v & latch;
    case 0x10: return v | latch;
    case 0x18: return v ^ latch;
    default: return v;
    }
}

void vga_mem_write(uint32_t off, uint8_t v)
{
    const uint8_t planes = video.seq[2] & 0x0F;
    const uint8_t mode = video.gc[5] & 3;
    uint8_t mask = video.gc[8];
    uint32_t latch, src;

    off &= 0xFFFF;
    video.dirty = 1;
    mark(off);

    /* The common case in 256-colour code: a plain byte store to the selected planes. */
    if (mode == 0 && mask == 0xFF && !(video.gc[1] & 0x0F) && !(video.gc[3] & 0x1F)) {
        if (planes & 1) vga_plane[0][off] = v;
        if (planes & 2) vga_plane[1][off] = v;
        if (planes & 4) vga_plane[2][off] = v;
        if (planes & 8) vga_plane[3][off] = v;
        return;
    }
    if (mode == 1) {
        for (int p = 0; p < 4; p++) {
            if (planes & (1 << p)) {
                vga_plane[p][off] = video.latch[p];
            }
        }
        return;
    }
    if (mode != 2) {
        int rot = video.gc[3] & 7;
        v = (uint8_t)((v >> rot) | (v << (8 - rot)));
    }
    if (mode == 3) {
        mask &= v;
    }
    memcpy(&latch, video.latch, 4);
    switch (mode) {
    case 0: { /* the CPU's byte, or set/reset where it is enabled */
        const uint32_t sr = nib4[video.gc[1] & 0x0F];
        src = alu(((v * 0x01010101u) & ~sr) | (nib4[video.gc[0] & 0x0F] & sr), latch);
        break;
    }
    case 2: /* a colour: a bit per plane */
        src = alu(nib4[v & 0x0F], latch);
        break;
    default: /* 3: set/reset, under a mask from the CPU's byte */
        src = nib4[video.gc[0] & 0x0F];
        break;
    }
    const uint32_t m = mask * 0x01010101u;
    src = (src & m) | (latch & ~m);
    if (planes & 1) vga_plane[0][off] = (uint8_t)src;
    if (planes & 2) vga_plane[1][off] = (uint8_t)(src >> 8);
    if (planes & 4) vga_plane[2][off] = (uint8_t)(src >> 16);
    if (planes & 8) vga_plane[3][off] = (uint8_t)(src >> 24);
}

/* --- geometry and timing ------------------------------------------------------ */

static uint32_t crtc_vtotal(void)
{
    return (video.crtc[0x06] | ((video.crtc[0x07] & 0x01u) << 8) | ((video.crtc[0x07] & 0x20u) << 4)) + 2;
}

static uint32_t crtc_vdisplay(void)
{
    return (video.crtc[0x12] | ((video.crtc[0x07] & 0x02u) << 7) | ((video.crtc[0x07] & 0x40u) << 3)) + 1;
}

void vga_geometry(vga_geometry_t *g)
{
    uint32_t lines = crtc_vdisplay();
    uint32_t linecmp = video.crtc[0x18] | ((video.crtc[0x07] & 0x10u) << 4) |
                       ((video.crtc[0x09] & 0x40u) << 3);

    g->is256 = (video.gc[5] & 0x40) != 0;
    g->width = (video.crtc[0x01] + 1) * 8;
    if (g->is256) {
        g->width /= 2;
    }
    g->row_scanlines = ((video.crtc[0x09] & 0x1F) + 1) << (video.crtc[0x09] >> 7);
    g->rows = (int)(lines / (uint32_t)g->row_scanlines);
    g->row_bytes = video.crtc[0x13] * 2u;
    g->start = ((uint32_t)video.crtc[0x0C] << 8) | video.crtc[0x0D];
    g->split_row = linecmp >= lines ? g->rows : (int)((linecmp + 1) / (uint32_t)g->row_scanlines);
    g->pan = video.attr[0x13] & 0x0F;
    if (g->is256) {
        g->pan >>= 1;
    }
    if (g->width < 8 || g->width > 720) {
        g->width = 320;
    }
    if (g->rows < 1 || g->rows > 480) {
        g->rows = 200;
    }
}

/* Input status 1: bit 0 = display not active (horizontal or vertical blanking), bit 3 =
 * vertical retrace. Both follow the programmed CRTC timing against the machine clock, as
 * software synchronises timers and palette changes to them. */
static uint8_t status1(void)
{
    uint32_t vtotal = crtc_vtotal(), vdisp = crtc_vdisplay();
    uint32_t vrs = video.crtc[0x10] | ((video.crtc[0x07] & 0x04u) << 6) |
                   ((video.crtc[0x07] & 0x80u) << 2);
    uint32_t vre;
    uint64_t now = pc_now();

    if (vtotal < 200 || vdisp >= vtotal) { /* CRTC not programmed: 400 lines at 70 Hz */
        vtotal = 449;
        vdisp = 400;
        vrs = 412;
    }
    if (vrs < vdisp || vrs >= vtotal) {
        vrs = vdisp + 2 < vtotal ? vdisp + 2 : vdisp;
    }
    vre = vrs + (((video.crtc[0x11] & 0x0Fu) - vrs) & 0x0F);
    if (vre == vrs) {
        vre = vrs + 2;
    }

    const uint32_t frame_ns = vtotal * LINE_NS;
    uint64_t dt = now - video.frame_t0;
    if (dt >= frame_ns) {
        if (dt >= 4ull * frame_ns) {
            video.frame_t0 = now - dt % frame_ns;
        } else {
            do {
                video.frame_t0 += frame_ns;
                dt -= frame_ns;
            } while (dt >= frame_ns);
        }
        dt = now - video.frame_t0;
    }
    uint32_t t = (uint32_t)dt;
    uint32_t line = t / LINE_NS, pos = t % LINE_NS;
    uint8_t st = 0;
    if (line >= vdisp || pos >= LINE_NS * 4 / 5) {
        st |= 0x01;
    }
    if (line >= vrs && line < vre) {
        st |= 0x08;
    }
    return st;
}

/* --- ports ---------------------------------------------------------------------- */

uint8_t video_inb(uint16_t port)
{
    switch (port) {
    case 0x3C0: return video.attr_idx;
    case 0x3C1: return video.attr[video.attr_idx & 0x1F];
    case 0x3C2: return 0x10;
    case 0x3C4: return video.seq_idx;
    case 0x3C5: return video.seq[video.seq_idx & 7];
    case 0x3C6: return video.dac_mask;
    case 0x3C7: return 0x03;
    case 0x3C8: return video.dac_widx;
    case 0x3C9: {
        uint8_t v = video.dac[video.dac_ridx][video.dac_rphase];
        if (++video.dac_rphase == 3) {
            video.dac_rphase = 0;
            video.dac_ridx++;
        }
        return v;
    }
    case 0x3CA: return 0;
    case 0x3CC: return video.misc;
    case 0x3CE: return video.gc_idx;
    case 0x3CF: return video.gc[video.gc_idx & 15];
    case 0x3D4: return video.crtc_idx;
    case 0x3D5: return video.crtc[video.crtc_idx & 31];
    case 0x3D8: return video.mode_ctl;
    case 0x3D9: return video.color_sel;
    case 0x3DA:
        video.attr_flip = 0;
        return status1();
    default: return 0xFF;
    }
}

void video_outb(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3C0:
        if (!video.attr_flip) {
            video.attr_idx = v;
        } else {
            uint8_t i = video.attr_idx & 0x1F;
            if (video.attr[i] != v) {
                video.attr[i] = v;
                video.generation++;
            }
            if (i == 0x10) { /* blink enable is also visible to the text renderer */
                video.mode_ctl = (uint8_t)((video.mode_ctl & ~0x20) | ((v & 0x08) ? 0x20 : 0));
            }
        }
        video.attr_flip ^= 1;
        break;
    case 0x3C2: video.misc = v; break;
    case 0x3C4: video.seq_idx = v; break;
    case 0x3C5: {
        uint8_t i = video.seq_idx & 7;
        uint8_t old = video.seq[i];
        video.seq[i] = v;
        if (i == 4 && ((old ^ v) & 0x08)) {
            /* Chain-4 switched: mode 13h <-> unchained 256-colour. */
            if (!(v & 0x08) && video.kind == VK_MCGA256) {
                video_set_kind(VK_PLANAR);
            } else if ((v & 0x08) && video.kind == VK_PLANAR && (video.gc[5] & 0x40)) {
                video_set_kind(VK_MCGA256);
            }
        } else if (i == 1 && old != v) {
            video.generation++;
        }
        break;
    }
    case 0x3C6:
        video.dac_mask = v;
        video.generation++;
        break;
    case 0x3C7:
        video.dac_ridx = v;
        video.dac_rphase = 0;
        break;
    case 0x3C8:
        video.dac_widx = v;
        video.dac_wphase = 0;
        break;
    case 0x3C9:
        video.dac[video.dac_widx][video.dac_wphase] = v & 0x3F;
        if (++video.dac_wphase == 3) {
            video.dac_wphase = 0;
            video.dac_widx++;
        }
        video.generation++;
        break;
    case 0x3CE: video.gc_idx = v; break;
    case 0x3CF: {
        uint8_t i = video.gc_idx & 15;
        if (i == 5 && ((video.gc[5] ^ v) & 0x60)) {
            video.generation++;
        }
        video.gc[i] = v;
        break;
    }
    case 0x3D4: video.crtc_idx = v; break;
    case 0x3D5: {
        uint8_t i = video.crtc_idx & 31;
        if (i < 8 && (video.crtc[0x11] & 0x80)) {
            /* Registers 0-7 are write-protected, except the line compare bit. */
            if (i == 7) {
                v = (uint8_t)((video.crtc[7] & ~0x10) | (v & 0x10));
            } else {
                break;
            }
        }
        if (video.crtc[i] != v) {
            video.crtc[i] = v;
            /* Cursor registers are polled by the text renderer; the rest changes the
             * picture geometry or position. */
            if (i < 0x0A || i > 0x0F || (i <= 0x0D && video.kind == VK_PLANAR)) {
                video.generation++;
            }
        }
        break;
    }
    case 0x3D8:
        if (video.kind <= VK_CGA640) {
            if ((v ^ video.mode_ctl) & 0x13) {
                /* A program reprogramming the CGA mode control register directly selects
                 * one of the CGA modes, whatever the BIOS set before. */
                if (v & 0x02) {
                    video_set_kind((v & 0x10) ? VK_CGA640 : VK_CGA320);
                } else {
                    video_set_kind((v & 0x01) ? VK_TEXT80 : VK_TEXT40);
                }
            }
            if ((v ^ video.mode_ctl) & 0x2C) {
                video.generation++;
            }
            video.mode_ctl = v;
        }
        break;
    case 0x3D9:
        video.color_sel = v;
        video.generation++;
        break;
    default: break;
    }
}
