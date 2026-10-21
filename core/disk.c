/*
 * Disk BIOS (INT 13h) over the platform's block devices: two floppy drives (00h, 01h) and
 * up to three hard disks (80h-82h), with the IBM/MS INT 13h extensions (41h-48h) on hard
 * disks.
 */
#include <stdio.h>
#include <string.h>

#include "pc.h"
#include "regs.h"

typedef struct {
    blkdev_t *dev;
    uint32_t cyls, heads, spt;
    uint8_t fd_type; /* 1 = 360K, 2 = 1.2M, 3 = 720K, 4 = 1.44M, 6 = 2.88M */
} drive_t;

static drive_t drives[DRIVE_COUNT];
/* Aligned so that the platform's storage driver can transfer into it by DMA. */
static uint8_t xfer[64 * 512] __attribute__((aligned(64)));

static const struct {
    uint32_t sectors;
    uint8_t cyls, heads, spt, type;
} fd_formats[] = {
    {320, 40, 1, 8, 1},   {360, 40, 1, 9, 1},   {640, 40, 2, 8, 1},   {720, 40, 2, 9, 1},
    {1440, 80, 2, 9, 3},  {2400, 80, 2, 15, 2}, {2880, 80, 2, 18, 4}, {3360, 80, 2, 21, 4},
    {5760, 80, 2, 36, 6},
};

static uint16_t rom16(int off)
{
    return (uint16_t)(p4dos_rom[off] | (p4dos_rom[off + 1] << 8));
}

void disk_init(void)
{
    memset(drives, 0, sizeof(drives));
}

static void probe(int d)
{
    drive_t *dr = &drives[d];
    blkdev_t *dev = dr->dev;

    if (d < DRIVE_HD0) {
        dr->cyls = 80;
        dr->heads = 2;
        dr->spt = 18;
        dr->fd_type = 4;
        for (unsigned i = 0; i < sizeof(fd_formats) / sizeof(fd_formats[0]); i++) {
            if (fd_formats[i].sectors == dev->sectors) {
                dr->cyls = fd_formats[i].cyls;
                dr->heads = fd_formats[i].heads;
                dr->spt = fd_formats[i].spt;
                dr->fd_type = fd_formats[i].type;
            }
        }
        return;
    }

    /* Hard disk: take the translation the partition table was written with, if any;
     * otherwise 16 heads up to 504 MB and 255 above, as BIOSes do. */
    dr->heads = dev->sectors > 1024u * 16 * 63 ? 255 : 16;
    dr->spt = 63;
    if (dev->read(dev, 0, 1, xfer) == 0 && xfer[510] == 0x55 && xfer[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = xfer + 446 + i * 16;
            if (e[4] && (e[6] & 63) && e[5] < 255) {
                dr->heads = e[5] + 1u;
                dr->spt = e[6] & 63;
                break;
            }
        }
    }
    dr->cyls = dev->sectors / (dr->heads * dr->spt);
    if (dr->cyls > 1024) {
        dr->cyls = 1024;
    }
}

void disk_attach(int d, blkdev_t *dev)
{
    if (d < 0 || d >= DRIVE_COUNT) {
        return;
    }
    drives[d].dev = dev;
    if (dev) {
        probe(d);
    }
}

int disk_count_fd(void)
{
    return (drives[DRIVE_FD0].dev != 0) + (drives[DRIVE_FD1].dev != 0);
}

int disk_count_hd(void)
{
    int n = 0;
    for (int d = DRIVE_HD0; d < DRIVE_COUNT; d++) {
        n += drives[d].dev != 0;
    }
    return n;
}

void disk_flush(void)
{
    for (int d = 0; d < DRIVE_COUNT; d++) {
        if (drives[d].dev && drives[d].dev->flush) {
            drives[d].dev->flush(drives[d].dev);
        }
    }
}

const char *disk_describe(int d, char *buf, int len)
{
    drive_t *dr = &drives[d];
    if (!dr->dev) {
        return 0;
    }
    uint32_t kb = dr->dev->sectors / 2;
    if (d < DRIVE_HD0) {
        snprintf(buf, (size_t)len, "Floppy %c: %u KB (%u/%u/%u)%s", 'A' + d, (unsigned)kb,
                 (unsigned)dr->cyls, (unsigned)dr->heads, (unsigned)dr->spt,
                 dr->dev->readonly ? ", read-only" : "");
    } else {
        snprintf(buf, (size_t)len, "%s: %u.%u MB (%u/%u/%u)%s",
                 d == DRIVE_HD2 ? "Host disk" : d == DRIVE_HD1 ? "Hard disk 1" : "Hard disk 0",
                 (unsigned)(kb / 1024), (unsigned)(kb % 1024 * 10 / 1024), (unsigned)dr->cyls,
                 (unsigned)dr->heads, (unsigned)dr->spt, dr->dev->readonly ? ", read-only" : "");
    }
    return buf;
}

static drive_t *lookup(uint8_t dl)
{
    int n = dl & 0x7F;
    if (!(dl & 0x80)) {
        return n < DRIVE_HD0 && drives[n].dev ? &drives[n] : 0;
    }
    /* Hard disks are numbered without gaps, whichever slots they sit in. */
    for (int d = DRIVE_HD0; d < DRIVE_COUNT; d++) {
        if (drives[d].dev && n-- == 0) {
            return &drives[d];
        }
    }
    return 0;
}

/* Fill the fixed disk parameter tables (INT 41h / 46h) in the ROM image. */
void disk_post(void)
{
    for (int i = 0; i < 2; i++) {
        drive_t *dr = lookup((uint8_t)(0x80 + i));
        uint8_t *t = cpu_mem + 0xF0000 + rom16(0x10 + i * 2);
        memset(t, 0, 16);
        if (!dr) {
            continue;
        }
        t[0] = (uint8_t)dr->cyls;
        t[1] = (uint8_t)(dr->cyls >> 8);
        t[2] = (uint8_t)dr->heads;
        t[5] = t[6] = 0xFF;
        t[8] = dr->heads > 8 ? 0x08 : 0;
        t[12] = (uint8_t)dr->cyls;
        t[13] = (uint8_t)(dr->cyls >> 8);
        t[14] = (uint8_t)dr->spt;
    }
    if (drives[DRIVE_FD0].dev) {
        cpu_mem[0xF0000 + rom16(0x14) + 4] = (uint8_t)drives[DRIVE_FD0].spt;
    }
}

/* Copy between a sector buffer and guest memory at seg:off, wrapping within the segment
 * and leaving alone what is not RAM. The address is a linear one: with an 80386 memory
 * manager loaded it goes through its page tables, piece by piece. */
static void guest_copy(uint16_t seg, uint16_t off, uint8_t *buf, uint32_t len, int to_guest)
{
    while (len) {
        const uint32_t lin = SEGOFF(seg, off);
        uint32_t n = 0x10000u - off;
        uint8_t *p;
        if (n > 0x1000 - (lin & 0xFFF)) {
            n = 0x1000 - (lin & 0xFFF);
        }
        if (n > len) {
            n = len;
        }
        p = cpu_lin_ptr(lin, n, to_guest);
        if (p && to_guest) {
            memcpy(p, buf, n);
        } else if (p) {
            memcpy(buf, p, n);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                p = cpu_lin_ptr(lin + i, 1, to_guest);
                if (to_guest && p) {
                    *p = buf[i];
                } else if (!to_guest) {
                    buf[i] = p ? *p : 0xFF;
                }
            }
        }
        buf += n;
        off = (uint16_t)(off + n);
        len -= n;
    }
}

enum { OP_READ, OP_WRITE, OP_VERIFY };

/* Returns a BIOS status code; *done receives the number of sectors transferred. */
static uint8_t transfer(drive_t *dr, int op, uint32_t lba, uint32_t count, uint16_t seg,
                        uint16_t off, uint32_t *done)
{
    blkdev_t *dev = dr->dev;
    *done = 0;
    if (lba >= dev->sectors || count > dev->sectors - lba) {
        return 0x04;
    }
    if (op == OP_WRITE && dev->readonly) {
        return 0x03;
    }
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        if (op == OP_WRITE) {
            guest_copy(seg, off, xfer, n * 512, 0);
            if (dev->write(dev, lba, n, xfer)) {
                return 0x04;
            }
        } else {
            if (dev->read(dev, lba, n, xfer)) {
                return 0x04;
            }
            if (op == OP_READ) {
                guest_copy(seg, off, xfer, n * 512, 1);
            }
        }
        lba += n;
        count -= n;
        off = (uint16_t)(off + n * 512);
        *done += n;
    }
    return 0;
}

static void finish(uint8_t dl, uint8_t status)
{
    AH = status;
    mem_w8((dl & 0x80) ? BDA(0x74) : BDA(0x41), status);
    bios_set_cf(status != 0);
}

void disk_int13(void)
{
    const uint8_t fn = AH, dl = DL;
    drive_t *dr = lookup(dl);
    uint32_t done;

    if (fn == 0x00) { /* reset works for any drive number */
        finish(dl, 0);
        return;
    }
    if (fn == 0x01) {
        AH = mem_r8((dl & 0x80) ? BDA(0x74) : BDA(0x41));
        bios_set_cf(AH != 0);
        return;
    }
    if (fn == 0x08 && !dr) { /* drive parameters of an absent drive */
        if (dl & 0x80) {
            finish(dl, 0x07);
        } else {
            AX = BX = CX = DX = 0;
            cpu_set_seg(SEG_ES, 0);
            DI = 0;
            DL = (uint8_t)disk_count_fd();
            bios_set_cf(0);
        }
        return;
    }
    if (fn == 0x15 && !dr) {
        AH = 0;
        bios_set_cf(0);
        return;
    }
    if (!dr) {
        finish(dl, (dl & 0x80) ? 0x01 : 0x80);
        return;
    }

    switch (fn) {
    case 0x02:
    case 0x03:
    case 0x04: {
        uint32_t cyl = CH | ((uint32_t)(CL & 0xC0) << 2), sec = CL & 63, head = DH;
        if (!AL || !sec || sec > dr->spt || head >= dr->heads) {
            finish(dl, 0x04);
            AL = 0;
            return;
        }
        uint32_t lba = (cyl * dr->heads + head) * dr->spt + sec - 1;
        uint8_t st = transfer(dr, fn == 0x02 ? OP_READ : fn == 0x03 ? OP_WRITE : OP_VERIFY, lba,
                              AL, ES, BX, &done);
        finish(dl, st);
        AL = (uint8_t)done;
        break;
    }
    case 0x05: /* format track: nothing to do on an image */
        finish(dl, dr->dev->readonly ? 0x03 : 0);
        break;
    case 0x08:
        if (dl & 0x80) {
            uint32_t c = dr->cyls - 1;
            CH = (uint8_t)c;
            CL = (uint8_t)(dr->spt | ((c >> 2) & 0xC0));
            DH = (uint8_t)(dr->heads - 1);
            DL = (uint8_t)disk_count_hd();
        } else {
            BX = dr->fd_type;
            CH = (uint8_t)(dr->cyls - 1);
            CL = (uint8_t)dr->spt;
            DH = (uint8_t)(dr->heads - 1);
            DL = (uint8_t)disk_count_fd();
            cpu_set_seg(SEG_ES, 0xF000);
            DI = rom16(0x14);
        }
        AX = 0;
        mem_w8((dl & 0x80) ? BDA(0x74) : BDA(0x41), 0);
        bios_set_cf(0);
        break;
    case 0x09: case 0x0C: case 0x0D: case 0x10: case 0x11: case 0x14:
    case 0x17: case 0x18:
        finish(dl, 0);
        break;
    case 0x15:
        if (dl & 0x80) {
            uint32_t n = dr->cyls * dr->heads * dr->spt;
            CX = (uint16_t)(n >> 16);
            DX = (uint16_t)n;
            AH = 3;
        } else {
            AH = 1; /* floppy without change-line support */
        }
        bios_set_cf(0);
        break;
    case 0x16: finish(dl, 0); break;
    case 0x41:
        if (!(dl & 0x80) || BX != 0x55AA) {
            finish(dl, 0x01);
            break;
        }
        BX = 0xAA55;
        AH = 0x21;
        CX = 0x0001;
        bios_set_cf(0);
        break;
    case 0x42:
    case 0x43:
    case 0x44: {
        uint32_t p = SEGOFF(DS, SI);
        uint32_t count = mem_r16(p + 2);
        uint32_t lba = mem_r16(p + 8) | ((uint32_t)mem_r16(p + 10) << 16);
        if (!(dl & 0x80) || mem_r8(p) < 16 || mem_r16(p + 12) || mem_r16(p + 14)) {
            finish(dl, 0x01);
            break;
        }
        uint8_t st = transfer(dr, fn == 0x42 ? OP_READ : fn == 0x43 ? OP_WRITE : OP_VERIFY, lba,
                              count, mem_r16(p + 6), mem_r16(p + 4), &done);
        mem_w16(p + 2, (uint16_t)done);
        finish(dl, st);
        break;
    }
    case 0x47: finish(dl, (dl & 0x80) ? 0 : 0x01); break;
    case 0x48: {
        uint32_t p = SEGOFF(DS, SI);
        if (!(dl & 0x80) || mem_r16(p) < 26) {
            finish(dl, 0x01);
            break;
        }
        mem_w16(p, 26);
        mem_w16(p + 2, 0x0002); /* CHS information is valid */
        mem_w16(p + 4, (uint16_t)dr->cyls);
        mem_w16(p + 6, (uint16_t)(dr->cyls >> 16));
        mem_w16(p + 8, (uint16_t)dr->heads);
        mem_w16(p + 10, 0);
        mem_w16(p + 12, (uint16_t)dr->spt);
        mem_w16(p + 14, 0);
        mem_w16(p + 16, (uint16_t)dr->dev->sectors);
        mem_w16(p + 18, (uint16_t)(dr->dev->sectors >> 16));
        mem_w16(p + 20, 0);
        mem_w16(p + 22, 0);
        mem_w16(p + 24, 512);
        finish(dl, 0);
        break;
    }
    default: finish(dl, 0x01); break;
    }
}

/* INT 19h: load a boot sector to 0000:7C00 and start it. Floppy first, then hard disk. */
int disk_boot(void)
{
    static const int order[] = {DRIVE_FD0, DRIVE_HD0};
    for (unsigned i = 0; i < 2; i++) {
        drive_t *dr = &drives[order[i]];
        if (!dr->dev || dr->dev->read(dr->dev, 0, 1, xfer)) {
            continue;
        }
        if (xfer[510] != 0x55 || xfer[511] != 0xAA) {
            continue;
        }
        memcpy(cpu_mem + 0x7C00, xfer, 512);
        cpu_set_seg(SEG_CS, 0);
        cpu_set_seg(SEG_DS, 0);
        cpu_set_seg(SEG_ES, 0);
        cpu_set_seg(SEG_SS, 0);
        SP = 0x7C00;
        cpu.ip = 0x7C00;
        DX = order[i] == DRIVE_FD0 ? 0x0000 : 0x0080;
        cpu.f_if = 1;
        return 1;
    }
    return 0;
}
