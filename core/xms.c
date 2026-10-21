/*
 * XMS (extended memory specification) services for the guest, served through XMS.SYS
 * (dosfiles/xms.asm), whose control entry far-calls the hypercall stub in the BIOS ROM.
 *
 * An 80186 has no memory above 1 MB, so there is no HMA and no A20 line: only the extended
 * memory block functions exist, backed by host memory. That is what matters in practice:
 * FreeCOM swaps itself out to XMS while a program runs, which frees about 80 KB of
 * conventional memory, and programs can park data there with "move block".
 */
#include <string.h>

#include "pc.h"
#include "regs.h"

#define NHANDLES 32

typedef struct {
    uint8_t used;
    uint32_t start, size; /* KiB */
} block_t;

static uint8_t *pool;
static uint32_t total_kb;
static block_t blocks[NHANDLES];

void xms_reset(void)
{
    memset(blocks, 0, sizeof(blocks));
}

void xms_init(uint8_t *ram, uint32_t kb)
{
    pool = ram;
    total_kb = ram ? kb : 0;
    xms_reset();
}

/* Lowest position where `need` KiB fit, ignoring block `skip`; total_kb + 1 if nowhere. */
static uint32_t find_gap(uint32_t need, int skip, uint32_t *largest, uint32_t *free_total)
{
    uint32_t pos = 0, best = total_kb + 1, big = 0, sum = 0;
    for (;;) {
        /* next used block at or after pos */
        int next = -1;
        for (int i = 0; i < NHANDLES; i++) {
            if (blocks[i].used && i != skip && blocks[i].size && blocks[i].start >= pos &&
                (next < 0 || blocks[i].start < blocks[next].start)) {
                next = i;
            }
        }
        uint32_t end = next < 0 ? total_kb : blocks[next].start;
        uint32_t gap = end - pos;
        sum += gap;
        if (gap > big) {
            big = gap;
        }
        if (gap >= need && best > total_kb) {
            best = pos;
        }
        if (next < 0) {
            break;
        }
        pos = blocks[next].start + blocks[next].size;
    }
    if (largest) {
        *largest = big;
    }
    if (free_total) {
        *free_total = sum;
    }
    return best;
}

/* One end of a move: handle 0 means a real-mode seg:off pointer. */
static uint8_t *move_ptr(uint16_t handle, uint32_t off, uint32_t len, uint8_t bad_handle,
                         uint8_t bad_offset, uint8_t *err)
{
    if (handle == 0) {
        uint32_t lin = ((off >> 16) << 4) + (off & 0xFFFF);
        if (lin + len > CPU_MEM_SIZE) {
            *err = bad_offset;
            return 0;
        }
        return cpu_mem + lin;
    }
    if (handle > NHANDLES || !blocks[handle - 1].used) {
        *err = bad_handle;
        return 0;
    }
    const block_t *b = &blocks[handle - 1];
    if (off + len > b->size * 1024u) {
        *err = bad_offset;
        return 0;
    }
    return pool + (size_t)b->start * 1024u + off;
}

void xms_call(void)
{
    uint32_t largest, free_kb;
    uint8_t err = 0;
    int ok = 1;

    if (!total_kb) {
        AX = 0;
        BL = 0x80;
        return;
    }
    switch (AH) {
    case 0x00:
        AX = 0x0200; /* XMS 2.0 */
        BX = 0x0100;
        DX = 0;      /* no HMA */
        return;
    case 0x01: case 0x02: err = 0x90; break;             /* HMA does not exist */
    case 0x03: case 0x05: err = 0x82; break;             /* A20 cannot be enabled */
    case 0x04: case 0x06: break;
    case 0x07:
        AX = 0;
        BL = 0;
        return;
    case 0x08:
        find_gap(0, -1, &largest, &free_kb);
        AX = (uint16_t)(largest > 0xFFFF ? 0xFFFF : largest);
        DX = (uint16_t)(free_kb > 0xFFFF ? 0xFFFF : free_kb);
        BL = free_kb ? 0 : 0xA0;
        return;
    case 0x09: {
        int h;
        for (h = 0; h < NHANDLES && blocks[h].used; h++) {
        }
        if (h == NHANDLES) {
            err = 0xA1;
            break;
        }
        uint32_t pos = find_gap(DX, -1, 0, 0);
        if (pos > total_kb) {
            err = 0xA0;
            break;
        }
        blocks[h].used = 1;
        blocks[h].start = pos;
        blocks[h].size = DX;
        DX = (uint16_t)(h + 1);
        break;
    }
    case 0x0A:
        if (DX == 0 || DX > NHANDLES || !blocks[DX - 1].used) {
            err = 0xA2;
        } else {
            blocks[DX - 1].used = 0;
        }
        break;
    case 0x0B: {
        uint32_t p = SEGOFF(DS, SI);
        uint32_t len = mem_r16(p) | ((uint32_t)mem_r16(p + 2) << 16);
        uint32_t soff = mem_r16(p + 6) | ((uint32_t)mem_r16(p + 8) << 16);
        uint32_t doff = mem_r16(p + 12) | ((uint32_t)mem_r16(p + 14) << 16);
        uint8_t *s = move_ptr(mem_r16(p + 4), soff, len, 0xA3, 0xA4, &err);
        uint8_t *d = s ? move_ptr(mem_r16(p + 10), doff, len, 0xA5, 0xA6, &err) : 0;
        if (s && d) {
            if (len & 1) {
                err = 0xA7;
            } else {
                memmove(d, s, len);
            }
        }
        break;
    }
    case 0x0C: err = 0xAD; break; /* lock: there is no linear address to hand out */
    case 0x0D: break;
    case 0x0E:
        if (DX == 0 || DX > NHANDLES || !blocks[DX - 1].used) {
            err = 0xA2;
        } else {
            int free_handles = 0;
            for (int i = 0; i < NHANDLES; i++) {
                free_handles += !blocks[i].used;
            }
            uint32_t size = blocks[DX - 1].size;
            BH = 0;
            BL = (uint8_t)free_handles;
            DX = (uint16_t)size;
            AX = 1;
            return;
        }
        break;
    case 0x0F:
        if (DX == 0 || DX > NHANDLES || !blocks[DX - 1].used) {
            err = 0xA2;
        } else {
            block_t *b = &blocks[DX - 1];
            uint32_t pos = find_gap(BX, DX - 1, 0, 0);
            if (pos > total_kb) {
                err = 0xA0;
            } else {
                uint32_t keep = (b->size < BX ? b->size : BX) * 1024u;
                memmove(pool + (size_t)pos * 1024u, pool + (size_t)b->start * 1024u, keep);
                b->start = pos;
                b->size = BX;
            }
        }
        break;
    case 0x10:
        err = 0xB1; /* no upper memory blocks */
        DX = 0;
        break;
    case 0x11: err = 0xB2; break;
    default: err = 0x80; break;
    }
    ok = !err;
    AX = (uint16_t)ok;
    if (err) {
        BL = err;
    }
}
