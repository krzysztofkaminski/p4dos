/*
 * Host disk: a hard disk whose sectors live in an image file on the computer at the other
 * end of the console line, served by tools/hostdisk.py. This is how files reach a board
 * whose TF card stays in its slot: DOS sees one more drive and copies with its own tools.
 *
 * The line also carries the terminal mirror and its keystrokes, so everything of ours
 * travels in frames that no terminal and no keyboard produce:
 *
 *     ESC _ H D   type   seq   length (2, LE)   payload   CRC-32 (4, LE)
 *
 * with the CRC (as in zlib) over type, seq, length and payload.
 *
 *     from the host:  H  hello: session (4), sectors (4), read-only (1)
 *                     B  bye: session (4)
 *                     D  the sectors asked for        K  done        E  failed
 *     to the host:    A  hello accepted: sectors per frame (1)
 *                     R  read: LBA (4), count (1)     W  write: LBA (4), count (1), data
 *                     F  flush
 *
 * The host speaks first, so a plain terminal never sees a frame. A new session reboots
 * the emulated PC, as DOS only looks for drives when it starts. One request is in flight
 * at a time and the answer carries its seq; a lost or damaged frame is asked for again,
 * and after three silences the drive is "not ready" until the next hello.
 */
#include "hostdisk.h"

#include <string.h>

#include "pc.h"

enum { MAX_SECTORS = 16, HEADER = 8, TRIES = 3 };
#define TIMEOUT_NS 2000000000ull

static const uint8_t magic[4] = {0x1B, '_', 'H', 'D'};

static struct {
    const hostdisk_port_t *port;
    blkdev_t dev;
    uint32_t session;
    volatile int up;      /* a host is serving */
    volatile int changed; /* a session began */
    int dirty;            /* written to since the last flush */
    uint8_t seq;
    /* The request in flight: set by the emulator, answered by the input side. */
    volatile uint8_t want_seq;
    volatile int got; /* type of the answer, 0 while waiting */
    volatile uint32_t got_len;
    /* Input side: the frame coming in. */
    uint32_t n, len, crc;
    uint8_t head[HEADER], tail[4], hello[9];
    int store;
    uint8_t rx[MAX_SECTORS * 512];
    uint8_t tx[HEADER + 5 + MAX_SECTORS * 512 + 4];
} hd;

static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n)
{
    static const uint32_t t[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    c = ~c;
    while (n--) {
        c ^= *p++;
        c = t[c & 15] ^ (c >> 4);
        c = t[c & 15] ^ (c >> 4);
    }
    return ~c;
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Wraps the len payload bytes at f + HEADER into a frame; returns its length. */
static int frame(uint8_t *f, uint8_t type, uint8_t seq, uint32_t len)
{
    memcpy(f, magic, 4);
    f[4] = type;
    f[5] = seq;
    f[6] = (uint8_t)len;
    f[7] = (uint8_t)(len >> 8);
    put32(f + HEADER + len, crc32(0, f + 4, 4 + len));
    return (int)(HEADER + len + 4);
}

/* --- input side ------------------------------------------------------------ */

static void received(void)
{
    const uint8_t type = hd.head[4], seq = hd.head[5];

    if (type == 'H' && hd.len == 9) {
        uint8_t f[HEADER + 1 + 4];
        const uint32_t session = get32(hd.hello);
        if (!hd.up || session != hd.session) {
            hd.session = session;
            hd.dev.sectors = get32(hd.hello + 4);
            hd.dev.readonly = hd.hello[8];
            hd.dirty = 0;
            hd.up = 1;
            hd.changed = 1;
        }
        f[HEADER] = MAX_SECTORS;
        hd.port->send(f, frame(f, 'A', seq, 1));
    } else if (type == 'B' && hd.len == 4) {
        if (get32(hd.hello) == hd.session) {
            hd.up = 0;
        }
    } else if (hd.store == 1 && seq == hd.want_seq) {
        hd.got_len = hd.len;
        hd.got = type;
        hd.port->wake();
    }
}

void hostdisk_input(int ch)
{
    if (!hd.port) {
        return;
    }
    if (ch < 0) { /* the line went quiet: what was held back is no frame of ours */
        const uint32_t held = hd.n < 4 ? hd.n : 0;
        hd.n = 0;
        for (uint32_t i = 0; i < held; i++) {
            hd.port->key(magic[i]);
        }
        hd.port->key(-1);
        return;
    }
    if (hd.n < 4) {
        if (ch == magic[hd.n]) {
            hd.n++;
            return;
        }
        const uint32_t held = hd.n;
        hd.n = 0;
        for (uint32_t i = 0; i < held; i++) {
            hd.port->key(magic[i]);
        }
        if (ch == magic[0]) {
            hd.n = 1;
        } else {
            hd.port->key(ch);
        }
        return;
    }
    if (hd.n < HEADER) {
        hd.head[hd.n++] = (uint8_t)ch;
        if (hd.n == HEADER) {
            const uint8_t type = hd.head[4];
            hd.len = hd.head[6] | ((uint32_t)hd.head[7] << 8);
            if (hd.len > sizeof(hd.rx)) {
                hd.n = 0;
                return;
            }
            /* Sector data goes where the emulator will read it, but only while it waits
             * for exactly this answer. */
            hd.store = type == 'H' || type == 'B' ? 2 : hd.head[5] == hd.want_seq && !hd.got;
            hd.crc = crc32(0, hd.head + 4, 4);
        }
        return;
    }
    const uint32_t i = hd.n++ - HEADER;
    if (i < hd.len) {
        const uint8_t b = (uint8_t)ch;
        hd.crc = crc32(hd.crc, &b, 1);
        if (hd.store == 1) {
            hd.rx[i] = b;
        } else if (hd.store == 2 && i < sizeof(hd.hello)) {
            hd.hello[i] = b;
        }
        return;
    }
    hd.tail[i - hd.len] = (uint8_t)ch;
    if (i - hd.len == 3) {
        hd.n = 0;
        if (get32(hd.tail) == hd.crc) {
            received();
        }
    }
}

/* --- emulator side --------------------------------------------------------- */

/* Sends the len payload bytes at hd.tx + HEADER; 0 if the answer `reply` came. */
static int request(uint8_t type, uint32_t len, uint8_t reply)
{
    for (int i = 0; i < TRIES && hd.up; i++) {
        const uint8_t seq = ++hd.seq;
        hd.got = 0;
        hd.want_seq = seq;
        if (hd.port->send(hd.tx, frame(hd.tx, type, seq, len))) {
            continue;
        }
        const uint64_t end = hal_time_ns() + TIMEOUT_NS;
        while (!hd.got) {
            const uint64_t now = hal_time_ns();
            if (now >= end) {
                break;
            }
            hd.port->wait((int)((end - now) / 1000000u) + 1);
        }
        if (hd.got == reply) {
            return 0;
        }
        if (hd.got) {
            return -1;
        }
    }
    if (hd.up) {
        hd.up = 0;
        hal_log("host disk: no answer, drive not ready\n");
    }
    return -1;
}

static int hd_read(blkdev_t *d, uint32_t lba, uint32_t count, uint8_t *buf)
{
    (void)d;
    while (count) {
        const uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        put32(hd.tx + HEADER, lba);
        hd.tx[HEADER + 4] = (uint8_t)n;
        if (request('R', 5, 'D') || hd.got_len != n * 512) {
            return -1;
        }
        memcpy(buf, hd.rx, n * 512);
        buf += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int hd_write(blkdev_t *d, uint32_t lba, uint32_t count, const uint8_t *buf)
{
    (void)d;
    while (count) {
        const uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        put32(hd.tx + HEADER, lba);
        hd.tx[HEADER + 4] = (uint8_t)n;
        memcpy(hd.tx + HEADER + 5, buf, n * 512);
        hd.dirty = 1;
        if (request('W', 5 + n * 512, 'K')) {
            return -1;
        }
        buf += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static void hd_flush(blkdev_t *d)
{
    (void)d;
    if (hd.dirty) {
        hd.dirty = 0;
        request('F', 0, 'K');
    }
}

void hostdisk_init(const hostdisk_port_t *port)
{
    hd.port = port;
    hd.dev.read = hd_read;
    hd.dev.write = hd_write;
    hd.dev.flush = hd_flush;
}

int hostdisk_changed(void)
{
    if (!hd.changed) {
        return 0;
    }
    hd.changed = 0;
    return 1;
}

void hostdisk_refresh(void)
{
    disk_attach(DRIVE_HD2, hd.up ? &hd.dev : 0);
}
