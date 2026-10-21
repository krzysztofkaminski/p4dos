/*
 * Sound Blaster 2.0 compatible sound (DSP 2.01) at port 220h, IRQ 7, DMA channel 1,
 * together with the 8237 DMA controller registers it is programmed through. Digital output
 * only: 8-bit PCM by DMA (single-cycle, auto-init, "high-speed") and direct DAC writes. No
 * FM synthesiser.
 *
 * The card plays in emulated time: whenever time has passed, the bytes due at the
 * programmed sample rate are fetched from guest memory through the DMA channel, converted
 * to the output rate and queued for the audio thread; block ends raise IRQ 7.
 */
#include <string.h>

#include "audio.h"
#include "pc.h"

#define SB_IRQ 7
#define SB_DMA 1

/* --- 8237 DMA controller (channels 0-3) --------------------------------------- */

typedef struct {
    uint16_t base_addr, base_count, addr, count;
    uint8_t page, mode, masked;
} dma_ch_t;

static struct {
    dma_ch_t ch[4];
    uint8_t flipflop, status;
} dma;

static const uint8_t page_port[4] = {0x87, 0x83, 0x81, 0x82};

uint8_t dma_inb(uint16_t port)
{
    if (port < 8) {
        dma_ch_t *c = &dma.ch[port >> 1];
        uint16_t v = (port & 1) ? c->count : c->addr;
        dma.flipflop ^= 1;
        return dma.flipflop ? (uint8_t)v : (uint8_t)(v >> 8);
    }
    if (port == 0x08) {
        uint8_t s = dma.status;
        dma.status &= 0xF0;
        return s;
    }
    for (int i = 0; i < 4; i++) {
        if (port == page_port[i]) {
            return dma.ch[i].page;
        }
    }
    return 0xFF;
}

void dma_outb(uint16_t port, uint8_t v)
{
    if (port < 8) {
        dma_ch_t *c = &dma.ch[port >> 1];
        uint16_t *base = (port & 1) ? &c->base_count : &c->base_addr;
        uint16_t *cur = (port & 1) ? &c->count : &c->addr;
        if (!dma.flipflop) {
            *base = (uint16_t)((*base & 0xFF00) | v);
        } else {
            *base = (uint16_t)((*base & 0x00FF) | (v << 8));
        }
        *cur = *base;
        dma.flipflop ^= 1;
        return;
    }
    switch (port) {
    case 0x0A: dma.ch[v & 3].masked = (v >> 2) & 1; break;
    case 0x0B: dma.ch[v & 3].mode = v; break;
    case 0x0C: dma.flipflop = 0; break;
    case 0x0D: /* master clear */
        dma.flipflop = 0;
        dma.status = 0;
        for (int i = 0; i < 4; i++) {
            dma.ch[i].masked = 1;
        }
        break;
    case 0x0E:
        for (int i = 0; i < 4; i++) {
            dma.ch[i].masked = 0;
        }
        break;
    case 0x0F:
        for (int i = 0; i < 4; i++) {
            dma.ch[i].masked = (v >> i) & 1;
        }
        break;
    default:
        for (int i = 0; i < 4; i++) {
            if (port == page_port[i]) {
                dma.ch[i].page = v;
            }
        }
        break;
    }
}

/* Fetch the next byte the device on this channel is to play. Returns 0 if the channel
 * is masked or has run out (terminal count without auto-init). */
static int dma_read_byte(int n, uint8_t *out)
{
    dma_ch_t *c = &dma.ch[n];
    if (c->masked) {
        return 0;
    }
    *out = cpu_mem[(((uint32_t)c->page << 16) | c->addr) & CPU_MEM_MASK];
    c->addr = (c->mode & 0x20) ? (uint16_t)(c->addr - 1) : (uint16_t)(c->addr + 1);
    if (c->count-- == 0) {
        dma.status |= (uint8_t)(1 << n);
        if (c->mode & 0x10) {
            c->addr = c->base_addr;
            c->count = c->base_count;
        } else {
            c->masked = 1;
        }
    }
    return 1;
}

/* --- DSP -------------------------------------------------------------------------- */

static struct {
    uint8_t reset_latch;
    uint8_t cmd, need, have, args[4];
    uint8_t out[8];
    uint8_t out_n, out_pos;
    uint8_t speaker, test_reg, irq_pending;
    uint8_t time_const;
    uint32_t rate;
    /* playback */
    uint8_t playing, paused, autoinit, stop_after_block;
    uint32_t block_len, left;
    uint8_t sample;          /* last byte fetched (unsigned 8-bit) */
    uint32_t phase;          /* source/output rate conversion */
    uint64_t silence_frames; /* command 80h */
    /* direct DAC */
    uint64_t direct_until;
    /* time */
    uint64_t last_ns;
    uint32_t frac;           /* output frames, in billionths, carried over */
} sb;

static void out_byte(uint8_t v)
{
    if (sb.out_n < sizeof(sb.out)) {
        sb.out[sb.out_n++] = v;
    }
}

static void raise_irq(void)
{
    sb.irq_pending = 1;
    pic_raise(SB_IRQ);
}

/* Produce the output due since the last call. */
static void generate(uint64_t now)
{
    int16_t buf[256];
    uint64_t dt = now - sb.last_ns;
    sb.last_ns = now;
    if (dt > 100000000ull) {
        dt = 100000000ull; /* a long stall: do not replay it all */
    }
    uint64_t f = dt * AUDIO_RATE + sb.frac;
    uint32_t frames = (uint32_t)(f / 1000000000ull);
    sb.frac = (uint32_t)(f % 1000000000ull);

    const int direct = now < sb.direct_until;
    if (!sb.playing && !direct && !sb.silence_frames) {
        return; /* idle: the audio thread fades to silence by itself */
    }
    while (frames) {
        int n = frames > 256 ? 256 : (int)frames;
        for (int i = 0; i < n; i++) {
            if (sb.silence_frames) {
                if (--sb.silence_frames == 0) {
                    raise_irq();
                }
                sb.sample = 0x80;
            } else if (sb.playing && !sb.paused) {
                sb.phase += sb.rate;
                while (sb.phase >= AUDIO_RATE && sb.playing) {
                    uint8_t b;
                    sb.phase -= AUDIO_RATE;
                    if (!dma_read_byte(SB_DMA, &b)) {
                        break; /* DMA channel not ready: hold the last sample */
                    }
                    sb.sample = b;
                    if (sb.left-- == 0) {
                        raise_irq();
                        if (sb.autoinit && !sb.stop_after_block) {
                            sb.left = sb.block_len;
                        } else {
                            sb.playing = 0;
                        }
                    }
                }
            }
            buf[i] = sb.speaker ? (int16_t)((sb.sample - 128) * 256) : 0;
        }
        audio_push(buf, n);
        frames -= (uint32_t)n;
    }
}

static void start_playback(uint32_t len, int autoinit)
{
    sb.block_len = len;
    sb.left = len;
    sb.autoinit = (uint8_t)autoinit;
    sb.stop_after_block = 0;
    sb.paused = 0;
    sb.playing = 1;
    sb.phase = 0;
}

static void dsp_reset(void)
{
    uint64_t t = sb.last_ns;
    memset(&sb, 0, sizeof(sb));
    sb.last_ns = t;
    sb.sample = 0x80;
    sb.time_const = 0xD2;
    sb.rate = 22050;
    sb.out[0] = 0xAA;
    sb.out_n = 1;
}

static int param_count(uint8_t cmd)
{
    switch (cmd) {
    case 0x10: case 0x40: case 0xE0: case 0xE4: return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x48: case 0x74: case 0x75: case 0x76:
    case 0x77: case 0x80: return 2;
    default: return 0;
    }
}

static void dsp_exec(void)
{
    const uint32_t len = sb.args[0] | ((uint32_t)sb.args[1] << 8);
    uint64_t now = pc_now();

    generate(now);
    switch (sb.cmd) {
    case 0x10: /* direct DAC */
        sb.sample = sb.args[0];
        sb.direct_until = now + 50000000ull;
        break;
    case 0x14: case 0x91: /* single-cycle 8-bit output */
        start_playback(sb.cmd == 0x14 ? len : sb.block_len, 0);
        break;
    case 0x1C: case 0x90: /* auto-init 8-bit output */
        start_playback(sb.block_len, 1);
        break;
    case 0x16: case 0x17: case 0x74: case 0x75: case 0x76: case 0x77:
        /* ADPCM output is not decoded: honour the block timing with silence */
        sb.silence_frames = (uint64_t)(len + 1) * 2 * AUDIO_RATE / (sb.rate ? sb.rate : 11025);
        break;
    case 0x24: /* record: no input; signal completion */
        raise_irq();
        break;
    case 0x40:
        sb.time_const = sb.args[0];
        sb.rate = 1000000u / (256u - sb.args[0]);
        break;
    case 0x48: sb.block_len = len; break;
    case 0x80:
        sb.silence_frames = (uint64_t)(len + 1) * AUDIO_RATE / (sb.rate ? sb.rate : 11025);
        if (!sb.silence_frames) {
            sb.silence_frames = 1;
        }
        break;
    case 0xD0: sb.paused = 1; break;
    case 0xD4: sb.paused = 0; break;
    case 0xDA: sb.stop_after_block = 1; break;
    case 0xD1: sb.speaker = 1; break;
    case 0xD3: sb.speaker = 0; break;
    case 0xD8: out_byte(sb.speaker ? 0xFF : 0x00); break;
    case 0xE0: out_byte((uint8_t)~sb.args[0]); break;
    case 0xE1:
        out_byte(0x02);
        out_byte(0x01);
        break;
    case 0xE4: sb.test_reg = sb.args[0]; break;
    case 0xE8: out_byte(sb.test_reg); break;
    case 0xF2: raise_irq(); break;
    case 0x20: out_byte(0x80); break; /* direct ADC: silence */
    default: break;
    }
}

uint8_t sb_inb(uint16_t port)
{
    switch (port & 0x0F) {
    case 0x0A:
        if (sb.out_pos < sb.out_n) {
            uint8_t v = sb.out[sb.out_pos++];
            if (sb.out_pos == sb.out_n) {
                sb.out_pos = sb.out_n = 0;
            }
            return v;
        }
        return 0xFF;
    case 0x0C: return 0x7F; /* always ready for a command */
    case 0x0E:
        sb.irq_pending = 0;
        return sb.out_pos < sb.out_n ? 0xFF : 0x7F;
    default: return 0xFF;
    }
}

void sb_outb(uint16_t port, uint8_t v)
{
    switch (port & 0x0F) {
    case 0x06:
        if ((sb.reset_latch & 1) && !(v & 1)) {
            generate(pc_now());
            dsp_reset();
        }
        sb.reset_latch = v;
        break;
    case 0x0C:
        if (sb.need == 0) {
            sb.cmd = v;
            sb.have = 0;
            sb.need = (uint8_t)param_count(v);
            if (!sb.need) {
                dsp_exec();
            }
        } else {
            sb.args[sb.have++] = v;
            if (sb.have == sb.need) {
                sb.need = 0;
                dsp_exec();
            }
        }
        break;
    default: break;
    }
}

void sb_poll(uint64_t now)
{
    generate(now);
}

void sb_reset(void)
{
    memset(&dma, 0, sizeof(dma));
    for (int i = 0; i < 4; i++) {
        dma.ch[i].masked = 1;
    }
    sb.last_ns = pc_now();
    dsp_reset();
    sb.out_n = 0; /* no 0xAA until the guest resets the DSP itself */
    audio_reset();
}
