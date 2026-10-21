/*
 * LIM EMS 4.0 expanded memory, served to the guest through EMM.SYS (dosfiles/emm.asm),
 * whose INT 67h handler far-calls the hypercall stub in the BIOS ROM.
 *
 * The page frame is E000:0000 (four 16 KiB pages). Guest memory is one flat array, so
 * mapping a page copies it into the frame and copies the page it displaces back out.
 */
#include <stdlib.h>
#include <string.h>

#include "pc.h"
#include "regs.h"

#define PAGE 0x4000u
#define FRAME 0xE0000u
#define NHANDLES 64
#define UNMAPPED 0xFFFFu

typedef struct {
    uint8_t used;
    uint16_t npages;
    uint16_t *pages; /* logical page -> pool page */
    char name[8];
    uint16_t saved[4];
    uint8_t has_saved;
} handle_t;

static uint8_t *pool;
static uint16_t total, nfree;
static uint8_t *page_used;
static handle_t handles[NHANDLES];
static uint16_t slot[4]; /* pool page currently in each frame slot */

void ems_reset(void)
{
    for (int h = 0; h < NHANDLES; h++) {
        free(handles[h].pages);
    }
    memset(handles, 0, sizeof(handles));
    handles[0].used = 1; /* the system handle */
    if (page_used) {
        memset(page_used, 0, total);
    }
    nfree = total;
    for (int i = 0; i < 4; i++) {
        slot[i] = UNMAPPED;
    }
}

void ems_init(uint8_t *ram, uint32_t kb)
{
    pool = ram;
    total = ram ? (uint16_t)(kb / 16) : 0;
    free(page_used);
    page_used = total ? calloc(total, 1) : 0;
    if (total && !page_used) {
        total = 0;
    }
    if (total) {
        pc_ram_limit = 0xF0000; /* make the page frame writable */
        pc_update_mem_limits();
    }
    ems_reset();
}

static void slot_set(int i, uint16_t page)
{
    if (slot[i] == page) {
        return;
    }
    if (slot[i] != UNMAPPED) {
        memcpy(pool + (uint32_t)slot[i] * PAGE, cpu_mem + FRAME + (uint32_t)i * PAGE, PAGE);
    }
    slot[i] = page;
    if (page != UNMAPPED) {
        memcpy(cpu_mem + FRAME + (uint32_t)i * PAGE, pool + (uint32_t)page * PAGE, PAGE);
    }
}

/* Write the frame contents back so the pool is current, e.g. before a region move. */
static void frame_sync(int reload)
{
    for (int i = 0; i < 4; i++) {
        if (slot[i] == UNMAPPED) {
            continue;
        }
        if (reload) {
            memcpy(cpu_mem + FRAME + (uint32_t)i * PAGE, pool + (uint32_t)slot[i] * PAGE, PAGE);
        } else {
            memcpy(pool + (uint32_t)slot[i] * PAGE, cpu_mem + FRAME + (uint32_t)i * PAGE, PAGE);
        }
    }
}

static handle_t *get(uint16_t h)
{
    return (h < NHANDLES && handles[h].used) ? &handles[h] : 0;
}

static uint8_t resize(handle_t *h, uint16_t n)
{
    if (n > h->npages) {
        uint16_t need = (uint16_t)(n - h->npages);
        if (need > total) {
            return 0x87;
        }
        if (need > nfree) {
            return 0x88;
        }
        uint16_t *p = realloc(h->pages, n * sizeof(uint16_t));
        if (!p) {
            return 0x88;
        }
        h->pages = p;
        for (uint16_t pg = 0; pg < total && h->npages < n; pg++) {
            if (!page_used[pg]) {
                page_used[pg] = 1;
                memset(pool + (uint32_t)pg * PAGE, 0, PAGE);
                h->pages[h->npages++] = pg;
                nfree--;
            }
        }
    } else {
        while (h->npages > n) {
            uint16_t pg = h->pages[--h->npages];
            for (int i = 0; i < 4; i++) {
                if (slot[i] == pg) {
                    slot[i] = UNMAPPED;
                }
            }
            page_used[pg] = 0;
            nfree++;
        }
    }
    return 0;
}

static uint8_t map(handle_t *h, unsigned phys, uint16_t logical)
{
    if (phys > 3) {
        return 0x8B;
    }
    if (logical == 0xFFFF) {
        slot_set((int)phys, UNMAPPED);
        return 0;
    }
    if (logical >= h->npages) {
        return 0x8A;
    }
    slot_set((int)phys, h->pages[logical]);
    return 0;
}

/* One end of a move/exchange: conventional memory or a handle's pages. */
static uint8_t *region(uint32_t desc, uint32_t pos, uint8_t *err)
{
    uint8_t type = mem_r8(desc);
    uint16_t hnd = mem_r16(desc + 1), off = mem_r16(desc + 3), seg = mem_r16(desc + 5);
    if (type == 0) {
        uint32_t a = (SEGOFF(seg, off) + pos);
        if (a >= CPU_MEM_SIZE) {
            *err = 0xA2;
            return 0;
        }
        return cpu_mem + a;
    }
    handle_t *h = get(hnd);
    if (!h) {
        *err = 0x83;
        return 0;
    }
    uint32_t a = (uint32_t)seg * PAGE + off + pos;
    if (a / PAGE >= h->npages) {
        *err = 0x93;
        return 0;
    }
    return pool + (uint32_t)h->pages[a / PAGE] * PAGE + a % PAGE;
}

static uint8_t move_region(int exchange)
{
    uint32_t p = SEGOFF(DS, SI);
    uint32_t len = mem_r16(p) | ((uint32_t)mem_r16(p + 2) << 16);
    uint8_t err = 0;
    if (len > 0x100000) {
        return 0x96;
    }
    frame_sync(0);
    for (uint32_t i = 0; i < len && !err; i++) {
        uint8_t *s = region(p + 4, i, &err), *d = region(p + 11, i, &err);
        if (s && d) {
            uint8_t t = *d;
            *d = *s;
            if (exchange) {
                *s = t;
            }
        }
    }
    frame_sync(1);
    return err;
}

void ems_call(void)
{
    handle_t *h;
    uint8_t st = 0;

    if (!total) {
        AH = 0x84;
        return;
    }
    switch (AH) {
    case 0x40: break;
    case 0x41: BX = (uint16_t)(FRAME >> 4); break;
    case 0x42:
        BX = nfree;
        DX = total;
        break;
    case 0x43:
    case 0x5A: {
        int i;
        if (AH == 0x43 && BX == 0) {
            st = 0x89;
            break;
        }
        for (i = 1; i < NHANDLES && handles[i].used; i++) {
        }
        if (i == NHANDLES) {
            st = 0x85;
            break;
        }
        memset(&handles[i], 0, sizeof(handles[i]));
        handles[i].used = 1;
        st = resize(&handles[i], BX);
        if (st) {
            handles[i].used = 0;
        } else {
            DX = (uint16_t)i;
        }
        break;
    }
    case 0x44:
        h = get(DX);
        st = h ? map(h, AL, BX) : 0x83;
        break;
    case 0x45:
        h = get(DX);
        if (!h) {
            st = 0x83;
        } else {
            resize(h, 0);
            free(h->pages);
            memset(h, 0, sizeof(*h));
            if (DX == 0) {
                h->used = 1;
            }
        }
        break;
    case 0x46: AL = 0x40; break;
    case 0x47:
        h = get(DX);
        if (!h) {
            st = 0x83;
        } else if (h->has_saved) {
            st = 0x8D;
        } else {
            memcpy(h->saved, slot, sizeof(slot));
            h->has_saved = 1;
        }
        break;
    case 0x48:
        h = get(DX);
        if (!h) {
            st = 0x83;
        } else if (!h->has_saved) {
            st = 0x8E;
        } else {
            for (int i = 0; i < 4; i++) {
                slot_set(i, h->saved[i]);
            }
            h->has_saved = 0;
        }
        break;
    case 0x4B:
        BX = 0;
        for (int i = 0; i < NHANDLES; i++) {
            BX += handles[i].used;
        }
        break;
    case 0x4C:
        h = get(DX);
        if (h) {
            BX = h->npages;
        } else {
            st = 0x83;
        }
        break;
    case 0x4D: {
        uint32_t p = SEGOFF(ES, DI);
        BX = 0;
        for (int i = 0; i < NHANDLES; i++) {
            if (handles[i].used) {
                mem_w16(p, (uint16_t)i);
                mem_w16(p + 2, handles[i].npages);
                p += 4;
                BX++;
            }
        }
        break;
    }
    case 0x4E:
        if (AL == 0 || AL == 2) {
            for (int i = 0; i < 4; i++) {
                mem_w16(SEGOFF(ES, DI + i * 2), slot[i]);
            }
        }
        if (AL == 1 || AL == 2) {
            for (int i = 0; i < 4; i++) {
                uint16_t pg = mem_r16(SEGOFF(DS, SI + i * 2));
                slot_set(i, pg < total ? pg : UNMAPPED);
            }
        }
        if (AL == 3) {
            AL = 8;
        } else if (AL > 3) {
            st = 0x8F;
        }
        break;
    case 0x50:
        h = get(DX);
        if (!h) {
            st = 0x83;
            break;
        }
        for (uint32_t i = 0; i < CX && !st; i++) {
            uint16_t logical = mem_r16(SEGOFF(DS, SI + i * 4));
            uint16_t phys = mem_r16(SEGOFF(DS, SI + i * 4 + 2));
            if (AL == 1) { /* by segment address */
                phys = (uint16_t)((phys - (FRAME >> 4)) / (PAGE >> 4));
            }
            st = map(h, phys, logical);
        }
        break;
    case 0x51:
        h = get(DX);
        if (!h) {
            st = 0x83;
        } else {
            st = resize(h, BX);
            BX = h->npages;
        }
        break;
    case 0x52:
        if (AL == 2) {
            AL = 0; /* only volatile handles */
        } else if (AL == 0) {
            AL = 0;
        } else {
            st = 0x91;
        }
        break;
    case 0x53:
        h = get(DX);
        if (!h) {
            st = 0x83;
        } else if (AL == 0) {
            for (int i = 0; i < 8; i++) {
                mem_w8(SEGOFF(ES, DI + i), (uint8_t)h->name[i]);
            }
        } else if (AL == 1) {
            for (int i = 0; i < 8; i++) {
                h->name[i] = (char)mem_r8(SEGOFF(DS, SI + i));
            }
        } else {
            st = 0x8F;
        }
        break;
    case 0x54:
        if (AL == 0) {
            uint32_t p = SEGOFF(ES, DI);
            AL = 0;
            for (int i = 0; i < NHANDLES; i++) {
                if (handles[i].used) {
                    mem_w16(p, (uint16_t)i);
                    for (int k = 0; k < 8; k++) {
                        mem_w8(p + 2 + (uint32_t)k, (uint8_t)handles[i].name[k]);
                    }
                    p += 10;
                    AL++;
                }
            }
        } else if (AL == 1) {
            char name[8];
            int i;
            for (i = 0; i < 8; i++) {
                name[i] = (char)mem_r8(SEGOFF(DS, SI + i));
            }
            for (i = 0; i < NHANDLES; i++) {
                if (handles[i].used && !memcmp(handles[i].name, name, 8)) {
                    break;
                }
            }
            if (i == NHANDLES) {
                st = 0xA0;
            } else {
                DX = (uint16_t)i;
            }
        } else if (AL == 2) {
            BX = NHANDLES;
        } else {
            st = 0x8F;
        }
        break;
    case 0x57:
        if (AL > 1) {
            st = 0x8F;
        } else {
            st = move_region(AL);
        }
        break;
    case 0x58:
        if (AL == 0) {
            for (int i = 0; i < 4; i++) {
                mem_w16(SEGOFF(ES, DI + i * 4), (uint16_t)((FRAME >> 4) + i * (PAGE >> 4)));
                mem_w16(SEGOFF(ES, DI + i * 4 + 2), (uint16_t)i);
            }
        }
        CX = 4;
        break;
    case 0x59:
        if (AL == 0) {
            uint32_t p = SEGOFF(ES, DI);
            mem_w16(p, 0x0400);     /* raw page size in paragraphs */
            mem_w16(p + 2, 0);      /* alternate register sets */
            mem_w16(p + 4, 8);      /* context save area size */
            mem_w16(p + 6, 0);      /* DMA register sets */
            mem_w16(p + 8, 0);
        } else {
            BX = nfree;
            DX = total;
        }
        break;
    case 0x5B:
        if (AL == 0) {
            BL = 0;
        } else {
            st = 0x8F;
        }
        break;
    case 0x5C:
    case 0x5D: break;
    default: st = 0x84; break;
    }
    AH = st;
}
