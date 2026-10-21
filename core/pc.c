/*
 * Machine core: guest memory, port I/O dispatch, 8259 interrupt controller, 8253 timer,
 * XT keyboard/PPI interface and the run loop that paces them against real (or virtual) time.
 */
#include "pc.h"

#include <string.h>

#include "audio.h"
#include "hostdisk.h"
#include "video.h"

#define PIT_HZ 1193182u
int pc_batch = 4096;          /* instructions between device polls */
#define VIRT_NS_PER_INSN 200  /* virtual-time mode: a 5 MIPS machine, about the board's speed */
#define IRQ0_MAX_LAG 250000000ull

typedef struct {
    uint32_t reload; /* 1..65536 */
    uint8_t mode, rw, wphase, rphase, latched;
    uint16_t latch, wlo;
    uint64_t t0;
} pit_ch_t;

static struct {
    /* 8259s: the master and, as in the AT, a second one on its line 2 */
    struct pic {
        uint8_t irr, isr, imr, vec_base, icw1, icw_step, read_isr;
    } pic[2];
    /* 8253 */
    pit_ch_t pit[3];
    uint64_t next_irq0, period0;
    uint8_t irq0_armed;
    /* CMOS RAM and its index register; port 92h; the keyboard controller's command */
    uint8_t cmos[64], cmos_idx, port92, kbc_cmd;
    /* keyboard interface */
    uint8_t kb_data, kb_obf, p61_toggle;
    uint64_t kb_time;
    uint8_t cad_state;
    uint64_t cad_time;
    uint32_t resets;
    /* time */
    int virtual_time;
    uint64_t vtime;
    uint64_t wait_deadline, beep_until;
    uint32_t spk_hz;
    uint32_t invop_logged;
} pc;

uint8_t pc_port61;
void (*pc_io_trace)(int is_write, uint16_t port, uint8_t value);

uint32_t pc_ram_limit = 0xC0000;

/* In virtual-time mode the clock advances with every instruction, also inside a slice, so
 * that code timing itself against the display or the timer chip sees time pass. */
uint64_t pc_now(void)
{
    return pc.virtual_time ? pc.vtime + (uint64_t)cpu_executed() * VIRT_NS_PER_INSN
                           : hal_time_ns();
}

/* --- guest memory outside plain RAM ------------------------------------------ */

void pc_update_mem_limits(void)
{
    if (video.kind == VK_PLANAR) {
        /* The VGA planes sit behind A0000h: route everything from there up through us. */
        cpu_set_mem_limits(0xA0000, 0xA0000);
    } else {
        cpu_set_mem_limits(CPU_MEM_SIZE, pc_ram_limit);
    }
}

uint8_t pc_mem_read(uint32_t a)
{
    if (video.kind == VK_PLANAR && a - 0xA0000u < 0x10000u) {
        return vga_mem_read(a - 0xA0000u);
    }
    return cpu_mem[a];
}

void pc_mem_write(uint32_t a, uint8_t v)
{
    if (video.kind == VK_PLANAR && a - 0xA0000u < 0x10000u) {
        vga_mem_write(a - 0xA0000u, v);
    } else if (a < pc_ram_limit) {
        cpu_mem[a] = v;
    }
}

/* --- 8259 PIC ------------------------------------------------------------- */

/* Whether a controller asks for an interrupt: an unmasked request with nothing of the
 * same or a higher priority in service. */
static int pic_out(const struct pic *p, uint8_t irr)
{
    const uint8_t pend = irr & (uint8_t)~p->imr;
    if (!pend) {
        return 0;
    }
    return !p->isr || (uint8_t)(pend & -pend) < (uint8_t)(p->isr & -p->isr);
}

/* The master's request lines: its own, and the second controller's output on line 2. */
static uint8_t master_irr(void)
{
    return (uint8_t)(pc.pic[0].irr | (pic_out(&pc.pic[1], pc.pic[1].irr) ? 4 : 0));
}

static void pic_update(void)
{
    const uint8_t line = (uint8_t)pic_out(&pc.pic[0], master_irr());
    if (line && !cpu.irq_line) {
        cpu_break();
    }
    cpu.irq_line = line;
}

void pic_raise(int irq)
{
    pc.pic[irq >> 3].irr |= (uint8_t)(1u << (irq & 7));
    pic_update();
}

uint8_t pc_irq_ack(void)
{
    struct pic *m = &pc.pic[0], *sl = &pc.pic[1];
    const uint8_t pend = master_irr() & (uint8_t)~m->imr;
    uint8_t bit, vector;
    if (!pend) {
        return (uint8_t)(m->vec_base + 7); /* spurious */
    }
    bit = pend & (uint8_t)-pend;
    m->isr |= bit;
    if (bit == 4 && pic_out(sl, sl->irr)) {
        const uint8_t sp = sl->irr & (uint8_t)~sl->imr, sb = sp & (uint8_t)-sp;
        sl->irr &= (uint8_t)~sb;
        sl->isr |= sb;
        vector = (uint8_t)(sl->vec_base + __builtin_ctz(sb));
    } else {
        m->irr &= (uint8_t)~bit;
        vector = (uint8_t)(m->vec_base + __builtin_ctz(bit));
    }
    pic_update();
    return vector;
}

static void pic_write(struct pic *p, int a0, uint8_t v)
{
    if (!a0) {
        if (v & 0x10) { /* ICW1 */
            p->icw1 = v;
            p->icw_step = 1;
            p->imr = 0;
            p->isr = 0;
            p->irr = 0;
            p->read_isr = 0;
        } else if (v & 0x08) { /* OCW3 */
            if (v & 0x02) {
                p->read_isr = v & 1;
            }
        } else if ((v & 0xE0) == 0x20) { /* non-specific EOI */
            p->isr &= (uint8_t)(p->isr - 1);
        } else if ((v & 0xE0) == 0x60) { /* specific EOI */
            p->isr &= (uint8_t)~(1u << (v & 7));
        }
    } else if (p->icw_step == 1) {
        p->vec_base = v & 0xF8;
        p->icw_step = (p->icw1 & 0x02) ? ((p->icw1 & 0x01) ? 3 : 0) : 2;
    } else if (p->icw_step == 2) {
        p->icw_step = (p->icw1 & 0x01) ? 3 : 0;
    } else if (p->icw_step == 3) {
        p->icw_step = 0;
    } else {
        p->imr = v;
    }
    pic_update();
}

void pc_fpu_error(void)
{
    pic_raise(13);
}

/* --- 8253 PIT ------------------------------------------------------------- */

static uint64_t ns_to_pit(uint64_t ns)
{
    return (ns / 1000000000u) * PIT_HZ + (ns % 1000000000u) * PIT_HZ / 1000000000u;
}

static uint32_t pit_count(int ch, uint64_t now)
{
    pit_ch_t *p = &pc.pit[ch];
    uint64_t e = ns_to_pit(now - p->t0);
    if (p->mode == 3) {
        return (p->reload - (uint32_t)((e * 2) % p->reload)) & 0xFFFE;
    }
    if (p->mode == 2) {
        return (p->reload - (uint32_t)(e % p->reload)) & 0xFFFF;
    }
    return (uint32_t)(p->reload - e) & 0xFFFF;
}

static int pit_out(int ch, uint64_t now)
{
    pit_ch_t *p = &pc.pit[ch];
    uint64_t e = ns_to_pit(now - p->t0);
    return (e % p->reload) < p->reload / 2;
}

static void spk_update(void)
{
    uint32_t hz = 0;
    if (pc.beep_until) {
        hz = 1000;
    } else if ((pc_port61 & 3) == 3 && pc.pit[2].reload >= 20) {
        hz = PIT_HZ / pc.pit[2].reload;
    }
    if (hz != pc.spk_hz) {
        pc.spk_hz = hz;
        audio_speaker(hz);
    }
}

void pc_beep(void)
{
    pc.beep_until = pc_now() + 180000000ull;
    spk_update();
}

static void pit_loaded(int ch, uint32_t count)
{
    pit_ch_t *p = &pc.pit[ch];
    uint64_t now = pc_now();
    p->reload = count ? count : 0x10000;
    p->t0 = now;
    if (ch == 0) {
        pc.period0 = (uint64_t)p->reload * 1000000000u / PIT_HZ;
        pc.next_irq0 = now + pc.period0;
        pc.irq0_armed = 1;
    } else if (ch == 2) {
        spk_update();
    }
}

static void pit_write(int port, uint8_t v)
{
    if (port == 3) {
        int ch = v >> 6;
        if (ch == 3) {
            return;
        }
        pit_ch_t *p = &pc.pit[ch];
        if (((v >> 4) & 3) == 0) {
            if (!p->latched) {
                p->latch = (uint16_t)pit_count(ch, pc_now());
                p->latched = 1;
                p->rphase = 0;
            }
            return;
        }
        p->rw = (v >> 4) & 3;
        p->mode = (v >> 1) & 7;
        if (p->mode > 5) {
            p->mode &= 3;
        }
        p->wphase = p->rphase = 0;
        p->latched = 0;
        return;
    }
    pit_ch_t *p = &pc.pit[port];
    switch (p->rw) {
    case 1: pit_loaded(port, v); break;
    case 2: pit_loaded(port, (uint32_t)v << 8); break;
    default:
        if (!p->wphase) {
            p->wlo = v;
            p->wphase = 1;
        } else {
            p->wphase = 0;
            pit_loaded(port, p->wlo | ((uint32_t)v << 8));
        }
        break;
    }
}

static uint8_t pit_read(int port)
{
    if (port == 3) {
        return 0xFF;
    }
    pit_ch_t *p = &pc.pit[port];
    uint16_t c = p->latched ? p->latch : (uint16_t)pit_count(port, pc_now());
    switch (p->rw) {
    case 1: p->latched = 0; return (uint8_t)c;
    case 2: p->latched = 0; return (uint8_t)(c >> 8);
    default:
        if (!p->rphase) {
            p->rphase = 1;
            return (uint8_t)c;
        }
        p->rphase = 0;
        p->latched = 0;
        return (uint8_t)(c >> 8);
    }
}

/* --- AT odds and ends: the A20 gate, CMOS RAM ---------------------------------- */

static void set_a20(int on)
{
    pc.port92 = (uint8_t)((pc.port92 & ~2) | (on ? 2 : 0));
    cpu_set_a20(on);
}

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static uint8_t cmos_read(uint8_t idx)
{
    const uint32_t ext = cpu_ram_size / 1024 - 1024;
    hal_rtc_t r;
    idx &= 0x3F;
    if (idx < 10 || idx == 0x32) {
        hal_rtc_get(&r);
        switch (idx) {
        case 0: return bcd(r.sec);
        case 2: return bcd(r.min);
        case 4: return bcd(r.hour);
        case 7: return bcd(r.day);
        case 8: return bcd(r.month);
        case 9: return bcd((unsigned)r.year % 100);
        case 0x32: return bcd((unsigned)r.year / 100);
        default: return 0;
        }
    }
    switch (idx) {
    case 0x0A: return 0x26;
    case 0x0B: return 0x02; /* 24-hour, BCD */
    case 0x0C: return 0;
    case 0x0D: return 0x80; /* battery good */
    case 0x14: return (uint8_t)(0x01 | (cpu_fpu_fitted ? 0x02 : 0)); /* equipment */
    case 0x15: return 0x80; /* 640 KB base memory */
    case 0x16: return 0x02;
    case 0x17: case 0x30: return (uint8_t)ext;
    case 0x18: case 0x31: return (uint8_t)(ext >> 8);
    default: return pc.cmos[idx];
    }
}

/* --- port I/O ------------------------------------------------------------- */

static uint8_t port_read(uint16_t port)
{
    if (port >= 0x3C0 && port <= 0x3DF) {
        return video_inb(port);
    }
    if (port >= 0x3F8 && port <= 0x3FF) {
        return uart_inb(port);
    }
    if (port >= 0x220 && port <= 0x22F) {
        return sb_inb(port);
    }
    if (port < 0x10 || (port >= 0x80 && port <= 0x8F)) {
        return dma_inb(port);
    }
    switch (port) {
    case 0x20: return pc.pic[0].read_isr ? pc.pic[0].isr : pc.pic[0].irr;
    case 0x21: return pc.pic[0].imr;
    case 0x40: case 0x41: case 0x42: case 0x43:
        return pit_read(port & 3);
    case 0x60:
        pc.kb_obf = 0;
        return pc.kb_data;
    case 0x61:
        pc.p61_toggle ^= 0x10;
        return (uint8_t)((pc_port61 & 0x0F) | pc.p61_toggle |
                         (pit_out(2, pc_now()) ? 0x20 : 0));
    case 0x62: return pit_out(2, pc_now()) ? 0x20 : 0;
    case 0x64: return (uint8_t)(0x14 | pc.kb_obf);
    case 0x70: return pc.cmos_idx;
    case 0x71: return cmos_read(pc.cmos_idx);
    case 0x92: return pc.port92;
    case 0xA0: return pc.pic[1].read_isr ? pc.pic[1].isr : pc.pic[1].irr;
    case 0xA1: return pc.pic[1].imr;
    default: return 0xFF;
    }
}

uint8_t pc_inb(uint16_t port)
{
    uint8_t v = port_read(port);
    if (pc_io_trace) {
        pc_io_trace(0, port, v);
    }
    return v;
}

void pc_outb(uint16_t port, uint8_t v)
{
    if (pc_io_trace) {
        pc_io_trace(1, port, v);
    }
    if (port >= 0x3C0 && port <= 0x3DF) {
        video_outb(port, v);
        return;
    }
    if (port >= 0x3F8 && port <= 0x3FF) {
        uart_outb(port, v);
        return;
    }
    if (port >= 0x220 && port <= 0x22F) {
        sb_outb(port, v);
        return;
    }
    if (port < 0x10 || (port >= 0x80 && port <= 0x8F)) {
        dma_outb(port, v);
        return;
    }
    switch (port) {
    case 0x20: pic_write(&pc.pic[0], 0, v); break;
    case 0x21: pic_write(&pc.pic[0], 1, v); break;
    case 0x40: case 0x41: case 0x42: case 0x43:
        pit_write(port & 3, v);
        break;
    case 0x61:
        if (v & 0x80) {
            pc.kb_obf = 0; /* XT keyboard acknowledge */
        }
        pc_port61 = v;
        spk_update();
        break;
    case 0x60:
        if (pc.kbc_cmd == 0xD1) { /* output port of the keyboard controller: bit 1 is A20 */
            set_a20(v & 2);
        }
        pc.kbc_cmd = 0;
        break;
    case 0x64:
        if (v == 0xFE) {
            pc_reset();
        } else if (v == 0xDD || v == 0xDF) {
            set_a20(v == 0xDF);
        } else if (v == 0xD0) { /* read the output port */
            pc.kb_data = (uint8_t)(0xDD | (pc.port92 & 2));
            pc.kb_obf = 1;
        }
        pc.kbc_cmd = v;
        break;
    case 0x70: pc.cmos_idx = v & 0x7F; break;
    case 0x71: pc.cmos[pc.cmos_idx & 0x3F] = v; break;
    case 0x92:
        if (cpu.model >= CPU_80386) {
            set_a20(v & 2);
            if (v & 1) {
                pc_reset();
            }
        }
        break;
    case 0xA0: pic_write(&pc.pic[1], 0, v); break;
    case 0xA1: pic_write(&pc.pic[1], 1, v); break;
    default: break;
    }
}

void pc_hypercall(uint8_t fn)
{
    bios_hypercall(fn);
}

void pc_invalid_opcode(uint8_t op)
{
    if (pc.invop_logged < 8) {
        pc.invop_logged++;
        hal_log("invalid opcode %02X at %04X:%04X\n", op, cpu.seg[SEG_CS], (unsigned)cpu.ip);
    }
}

void pc_cpu_shutdown(void)
{
    hal_log("CPU shutdown (triple fault) at %04X:%08X: reset\n", cpu.seg[SEG_CS], (unsigned)cpu.ip);
    pc_reset();
}

void pc_wait_until(uint64_t deadline_ns)
{
    pc.wait_deadline = deadline_ns;
}

/* --- run loop ------------------------------------------------------------- */

static void pc_poll(uint64_t now)
{
    if (pc.irq0_armed && now >= pc.next_irq0 && !(pc.pic[0].irr & 1)) {
        pic_raise(0);
        if (pc.pit[0].mode == 0 || pc.pit[0].mode == 1) {
            pc.irq0_armed = 0; /* one-shot */
        } else {
            pc.next_irq0 += pc.period0;
            if (now > pc.next_irq0 + IRQ0_MAX_LAG) {
                pc.next_irq0 = now + pc.period0;
            }
        }
    }

    if (!pc.kb_obf || now - pc.kb_time > 20000000ull) {
        int sc = kbd_pop(now);
        if (sc >= 0) {
            pc.kb_data = (uint8_t)sc;
            pc.kb_obf = 1;
            pc.kb_time = now;
            pic_raise(1);
            /* Ctrl+Alt+Del watchdog: if the guest does not reboot by itself (hung with
             * interrupts off, or INT 9 replaced), force the reset. */
            if (sc == 0x53 && kbd_ctrl_alt()) {
                pc.cad_state = 1;
                pc.cad_time = now;
                pc.resets = 0;
            }
        }
    }
    if (pc.cad_state && now - pc.cad_time > 1500000000ull) {
        pc.cad_state = 0;
        if (!pc.resets) {
            hal_log("Ctrl+Alt+Del: guest did not respond, forcing reset\n");
            pc_reset();
        }
    }

    if (pc.wait_deadline && now >= pc.wait_deadline) {
        pc.wait_deadline = 0;
        cpu.halted = 0;
    }
    if (pc.beep_until && now >= pc.beep_until) {
        pc.beep_until = 0;
        spk_update();
    }
    uart_poll();
    sb_poll(now);
    if (hostdisk_changed()) {
        pc_reset();
    }
}

void pc_step(void)
{
    uint64_t now = pc_now();
    pc_poll(now);

    if (cpu.halted && !(cpu.irq_line && cpu.f_if)) {
        uint64_t next = now + 20000000ull;
        if (pc.irq0_armed && pc.next_irq0 < next) {
            next = pc.next_irq0;
        }
        if (pc.wait_deadline && pc.wait_deadline < next) {
            next = pc.wait_deadline;
        }
        if (next > now) {
            if (pc.virtual_time) {
                pc.vtime = next;
            } else {
                disk_flush();
                hal_idle((uint32_t)(next - now));
            }
        }
        return;
    }

    /* Stop the slice where the timer interrupt is due, so that it is delivered at the
     * right moment and not up to a slice late: programs calibrate timer periods against
     * the display's retrace to a fraction of a scan line. In real time the instruction
     * rate is unknown, so the deadline is approached in shrinking steps. */
    int n = pc_batch;
    if (pc.irq0_armed && !(pc.pic[0].irr & 1)) {
        uint64_t dt = pc.next_irq0 > now ? pc.next_irq0 - now : 0;
        uint64_t k = pc.virtual_time ? dt / VIRT_NS_PER_INSN + 1 : dt / 512 + 8;
        if (k < (uint64_t)n) {
            n = (int)k;
        }
    }
    n = cpu_run(n);
    if (pc.virtual_time) {
        pc.vtime += (uint64_t)n * VIRT_NS_PER_INSN;
    }
}

void pc_attach(int drive, blkdev_t *dev)
{
    disk_attach(drive, dev);
}

void pc_reset(void)
{
    pc.resets++;
    memset(pc.pic, 0, sizeof(pc.pic));
    pc.pic[0].imr = pc.pic[1].imr = 0xFF;
    pc.pic[0].vec_base = 0x08;
    pc.pic[1].vec_base = 0x70;
    memset(pc.pit, 0, sizeof(pc.pit));
    for (int i = 0; i < 3; i++) {
        pc.pit[i].reload = 0x10000;
        pc.pit[i].rw = 3;
        pc.pit[i].mode = 3;
        pc.pit[i].t0 = pc_now();
    }
    pc.irq0_armed = 0;
    pc.kb_obf = 0;
    pc.wait_deadline = 0;
    pc.beep_until = 0;
    pc_port61 = 0;
    spk_update();
    memcpy(cpu_mem + 0xF0000, p4dos_rom, sizeof(p4dos_rom));
    if (cpu.model >= CPU_80386) {
        cpu_mem[0xFFFFE] = 0xFC; /* model byte: AT class */
    }
    pc.kbc_cmd = 0;
    pc.port92 = 2;
    video_reset();
    uart_init();
    sb_reset();
    ems_reset();
    xms_reset();
    cpu_reset();
    cpu_break();
}

void pc_init(const pc_config_t *cfg)
{
    memset(&pc, 0, sizeof(pc));
    pc.virtual_time = cfg->virtual_time;
    cpu_mem = cfg->ram;
    cpu_ram_size = CPU_MEM_SIZE;
    if (cfg->cpu_model >= CPU_80386 && cfg->ram_kb > 1024) {
        cpu_ram_size = cfg->ram_kb * 1024;
    }
    cpu_fpu_fitted = cfg->cpu_model >= CPU_80386 && cfg->fpu;
    memset(cpu_mem, 0, cpu_ram_size + 16);
    /* Nothing is fitted between the video adapter and the BIOS ROM except the EMS page
     * frame; unpopulated address space reads as FFh. */
    memset(cpu_mem + 0xC0000, 0xFF, 0x30000);
    pc_ram_limit = 0xC0000;
    video_init(cfg->vram);
    cpu_init(cfg->cpu_model);
    kbd_init();
    disk_init();
    ems_init(cfg->ems_ram, cfg->ems_kb);
    xms_init(cfg->xms_ram, cfg->xms_kb);
    pc_reset();
}
