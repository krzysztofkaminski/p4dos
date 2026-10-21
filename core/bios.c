/*
 * System BIOS services implemented on the host side: POST, bootstrap, keyboard, timer,
 * equipment, serial/printer stubs, real-time clock. Entered through the F1 nn hypercalls
 * in the ROM stubs (bios/bios.asm).
 */
#include <stdio.h>
#include <string.h>

#include "hostdisk.h"
#include "pc.h"
#include "regs.h"

#define HC_POST 0xE0
#define HC_BOOT 0xE1
#define HC_KEY 0xE2
#define HC_TICK 0xE3
#define HC_INVOP 0xE4
#define HC_EMS 0xE7
#define HC_XMS 0xE8

#define TICKS_PER_DAY 0x1800B0u

static struct {
    uint8_t e0, e1_skip;
    uint16_t alt_num;
    uint8_t alt_num_used;
    uint64_t wait_deadline;
    uint8_t waiting;
    uint32_t polls;       /* empty INT 16h polls since the last timer tick */
    uint8_t invop_shown;
} st;

static uint16_t rom16(int off)
{
    return (uint16_t)(p4dos_rom[off] | (p4dos_rom[off + 1] << 8));
}

/* The interrupt stubs IRET straight after the hypercall, so FLAGS sits at SS:SP+4. */
static void set_stack_flag(uint16_t bit, int on)
{
    uint32_t a = SEGOFF(SS, SP + 4);
    uint16_t f = mem_r16(a);
    mem_w16(a, on ? (uint16_t)(f | bit) : (uint16_t)(f & ~bit));
}

void bios_set_cf(int cf) { set_stack_flag(FL_CF, cf); }
void bios_set_zf(int zf) { set_stack_flag(FL_ZF, zf); }

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

/* --- keyboard ------------------------------------------------------------- */

/* Characters by scancode 00h-39h, unshifted and shifted. */
static const char key_lo[0x3B] =
    "\0\x1B" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char key_hi[0x3B] =
    "\0\x1B" "!@#$%^&*()_+\b\0QWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
/* Numeric keypad, scancodes 47h-53h. */
static const char pad_num[14] = "789-456+1230.";
static const uint8_t pad_ctrl[13] = {0x77, 0x8D, 0x84, 0x8E, 0x73, 0x8F, 0x74,
                                     0x90, 0x75, 0x91, 0x76, 0x92, 0x93};

static void kb_enqueue(uint16_t code)
{
    uint16_t head = mem_r16(BDA(0x1A)), tail = mem_r16(BDA(0x1C));
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= mem_r16(BDA(0x82))) {
        next = mem_r16(BDA(0x80));
    }
    if (next == head) {
        pc_beep();
        return;
    }
    mem_w16(0x400u + tail, code);
    mem_w16(BDA(0x1C), next);
}

static int kb_peek(uint16_t *code)
{
    uint16_t head = mem_r16(BDA(0x1A));
    if (head == mem_r16(BDA(0x1C))) {
        return 0;
    }
    *code = mem_r16(0x400u + head);
    return 1;
}

static void kb_drop(void)
{
    uint16_t head = (uint16_t)(mem_r16(BDA(0x1A)) + 2);
    if (head >= mem_r16(BDA(0x82))) {
        head = mem_r16(BDA(0x80));
    }
    mem_w16(BDA(0x1A), head);
}

static void kb_sync_leds(void)
{
    kbd_set_leds((mem_r8(BDA(0x17)) >> 4) & 7);
}

/* INT 9 back end: translate one scancode. Returns 1 for Ctrl-Break, 2 for PrtSc. */
static int key_process(uint8_t sc)
{
    uint8_t *f1 = &cpu_mem[BDA(0x17)], *f2 = &cpu_mem[BDA(0x18)], *f3 = &cpu_mem[BDA(0x96)];
    int ext, brk;
    uint16_t code = 0;

    if (sc == 0xE0) {
        st.e0 = 1;
        return 0;
    }
    if (sc == 0xE1) { /* Pause: E1 1D 45 E1 9D C5 -- ignored */
        st.e1_skip = 2;
        return 0;
    }
    if (st.e1_skip) {
        st.e1_skip--;
        return 0;
    }
    ext = st.e0;
    st.e0 = 0;
    brk = sc & 0x80;
    sc &= 0x7F;

#define FLAG(p, bit) do { if (brk) *(p) &= (uint8_t)~(bit); else *(p) |= (bit); } while (0)
#define TOGGLE(bit) \
    do { \
        if (brk) { \
            *f2 &= (uint8_t)~(bit); \
        } else if (!(*f2 & (bit))) { \
            *f2 |= (bit); \
            *f1 ^= (bit); \
            kb_sync_leds(); \
        } \
    } while (0)

    switch (sc) {
    case 0x2A:
        if (!ext) {
            FLAG(f1, 0x02);
        }
        return 0;
    case 0x36:
        if (!ext) {
            FLAG(f1, 0x01);
        }
        return 0;
    case 0x1D:
        if (ext) {
            FLAG(f3, 0x04);
        } else {
            FLAG(f2, 0x01);
        }
        if ((*f3 & 0x04) || (*f2 & 0x01)) {
            *f1 |= 0x04;
        } else {
            *f1 &= (uint8_t)~0x04;
        }
        return 0;
    case 0x38:
        if (ext) {
            FLAG(f3, 0x08);
        } else {
            FLAG(f2, 0x02);
        }
        if ((*f3 & 0x08) || (*f2 & 0x02)) {
            *f1 |= 0x08;
        } else {
            *f1 &= (uint8_t)~0x08;
            if (st.alt_num_used) { /* Alt+keypad number entry */
                kb_enqueue((uint16_t)(st.alt_num & 0xFF));
            }
            st.alt_num = 0;
            st.alt_num_used = 0;
        }
        return 0;
    case 0x3A: TOGGLE(0x40); return 0;
    case 0x45: TOGGLE(0x20); return 0;
    case 0x46:
        if ((*f1 & 0x04) && !brk) { /* Ctrl-Break */
            mem_w16(BDA(0x1A), mem_r16(BDA(0x80)));
            mem_w16(BDA(0x1C), mem_r16(BDA(0x80)));
            mem_w8(BDA(0x71), 0x80);
            kb_enqueue(0);
            return 1;
        }
        TOGGLE(0x10);
        return 0;
    case 0x52: TOGGLE(0x80); break; /* Insert also produces a key code */
    default: break;
    }
#undef FLAG
#undef TOGGLE

    if (brk) {
        return 0;
    }

    const int shift = (*f1 & 0x03) != 0, ctrl = (*f1 & 0x04) != 0, alt = (*f1 & 0x08) != 0;

    if (sc == 0x53 && ctrl && alt) {
        mem_w16(BDA(0x72), 0x1234);
        hal_log("Ctrl+Alt+Del\n");
        pc_reset();
        return 0;
    }
    if (ext && sc == 0x37) {
        return 2;
    }

    if (ext) {
        if (sc == 0x1C) {
            code = alt ? 0xA600 : ctrl ? 0xE00A : 0xE00D;
        } else if (sc == 0x35) {
            code = alt ? 0xA400 : ctrl ? 0x9500 : 0xE02F;
        } else if (sc >= 0x47 && sc <= 0x53) {
            if (alt) {
                code = (uint16_t)((sc + 0x50) << 8);
            } else if (ctrl) {
                code = (uint16_t)((pad_ctrl[sc - 0x47] << 8) | 0xE0);
            } else {
                code = (uint16_t)((sc << 8) | 0xE0);
            }
        }
    } else if (sc >= 0x47 && sc <= 0x53) {
        char num = pad_num[sc - 0x47];
        if (sc == 0x4A || sc == 0x4E) {
            code = alt ? (uint16_t)(sc << 8) : ctrl ? (uint16_t)(pad_ctrl[sc - 0x47] << 8)
                                                    : (uint16_t)((sc << 8) | num);
        } else if (alt) {
            if (num >= '0' && num <= '9') {
                st.alt_num = (uint16_t)(st.alt_num * 10 + (num - '0'));
                st.alt_num_used = 1;
            }
        } else if (ctrl) {
            code = (uint16_t)(pad_ctrl[sc - 0x47] << 8);
        } else if (((*f1 & 0x20) != 0) != shift) {
            code = (uint16_t)((sc << 8) | num);
        } else {
            code = (uint16_t)(sc << 8);
        }
    } else if (sc >= 0x3B && sc <= 0x44) {
        code = (uint16_t)((sc + (alt ? 0x2D : ctrl ? 0x23 : shift ? 0x19 : 0)) << 8);
    } else if (sc == 0x57 || sc == 0x58) {
        code = (uint16_t)((0x85 + (sc - 0x57) + (alt ? 6 : ctrl ? 4 : shift ? 2 : 0)) << 8);
    } else if (sc == 0x56) {
        code = alt ? 0 : (uint16_t)((sc << 8) | (shift ? '|' : '\\'));
    } else if (sc < 0x3A) {
        char lo = key_lo[sc], hi = key_hi[sc];
        int letter = lo >= 'a' && lo <= 'z';
        if (alt) {
            if (sc == 0x39) {
                code = 0x3920;
            } else if (sc >= 0x02 && sc <= 0x0D) {
                code = (uint16_t)((sc + 0x76) << 8);
            } else if (sc == 0x0F) {
                code = 0xA500;
            } else if (lo) {
                code = (uint16_t)(sc << 8);
            }
        } else if (ctrl) {
            if (letter) {
                code = (uint16_t)((sc << 8) | (lo & 0x1F));
            } else {
                switch (sc) {
                case 0x01: code = 0x011B; break;
                case 0x03: code = 0x0300; break;
                case 0x07: code = 0x071E; break;
                case 0x0C: code = 0x0C1F; break;
                case 0x0E: code = 0x0E7F; break;
                case 0x0F: code = 0x9400; break;
                case 0x1A: code = 0x1A1B; break;
                case 0x1B: code = 0x1B1D; break;
                case 0x1C: code = 0x1C0A; break;
                case 0x2B: code = 0x2B1C; break;
                case 0x37: code = 0x9600; break;
                case 0x39: code = 0x3920; break;
                default: break;
                }
            }
        } else if (letter ? (((*f1 & 0x40) != 0) != shift) : shift) {
            code = (uint16_t)((sc << 8) | (uint8_t)hi);
        } else if (lo) {
            code = (uint16_t)((sc << 8) | (uint8_t)lo);
        }
    }

    if (code) {
        kb_enqueue(code);
    }
    return 0;
}

static void int16(void)
{
    uint16_t code;

    switch (AH) {
    case 0x00:
    case 0x10:
        if (!kb_peek(&code)) {
            /* Nothing yet: sleep until the next interrupt, then run this call again. */
            cpu.ip -= 2;
            cpu.halted = 1;
            return;
        }
        kb_drop();
        if (AH == 0x00 && (code & 0xFF) == 0xE0 && (code >> 8)) {
            code &= 0xFF00;
        }
        AX = code;
        break;
    case 0x01:
    case 0x11:
        if (!kb_peek(&code)) {
            bios_set_zf(1);
            /* A program spinning on "key available?" is idle: once it has polled a lot
             * within one timer tick, park the CPU until the next interrupt. */
            if (++st.polls > 64) {
                cpu.halted = 1;
            }
            return;
        }
        if (AH == 0x01 && (code & 0xFF) == 0xE0 && (code >> 8)) {
            code &= 0xFF00;
        }
        AX = code;
        bios_set_zf(0);
        break;
    case 0x02: AL = mem_r8(BDA(0x17)); break;
    case 0x12:
        AL = mem_r8(BDA(0x17));
        AH = (uint8_t)((mem_r8(BDA(0x18)) & 0x73) | (mem_r8(BDA(0x96)) & 0x0C));
        break;
    case 0x03:
        if (AL == 0x05) {
            static const uint16_t rate_ms[32] = {
                33, 37, 42, 46, 50, 54, 58, 63, 67, 75, 83, 92, 100, 109, 116, 125,
                133, 149, 167, 182, 200, 217, 233, 250, 270, 303, 333, 370, 400, 435, 476, 500};
            kbd_set_typematic(250u * ((BH & 3) + 1u), rate_ms[BL & 31]);
        }
        break;
    case 0x05: {
        uint16_t tail = mem_r16(BDA(0x1C)), next = (uint16_t)(tail + 2);
        if (next >= mem_r16(BDA(0x82))) {
            next = mem_r16(BDA(0x80));
        }
        if (next == mem_r16(BDA(0x1A))) {
            AL = 1;
        } else {
            mem_w16(0x400u + tail, CX);
            mem_w16(BDA(0x1C), next);
            AL = 0;
        }
        break;
    }
    case 0x09: AL = 0x20; break; /* enhanced keyboard functions supported */
    case 0x0A: BX = 0x41AB; break;
    default: break;
    }
}

/* --- timer / clock -------------------------------------------------------- */

static void tick(void)
{
    uint32_t t = mem_r16(BDA(0x6C)) | ((uint32_t)mem_r16(BDA(0x6E)) << 16);
    if (++t >= TICKS_PER_DAY) {
        t = 0;
        mem_w8(BDA(0x70), 1);
    }
    mem_w16(BDA(0x6C), (uint16_t)t);
    mem_w16(BDA(0x6E), (uint16_t)(t >> 16));
    uint8_t motor = mem_r8(BDA(0x40));
    if (motor) {
        mem_w8(BDA(0x40), (uint8_t)(motor - 1));
    }
    st.polls = 0;
}

static void int1a(void)
{
    hal_rtc_t r;

    switch (AH) {
    case 0x00:
        DX = mem_r16(BDA(0x6C));
        CX = mem_r16(BDA(0x6E));
        AL = mem_r8(BDA(0x70));
        mem_w8(BDA(0x70), 0);
        break;
    case 0x01:
        mem_w16(BDA(0x6C), DX);
        mem_w16(BDA(0x6E), CX);
        mem_w8(BDA(0x70), 0);
        break;
    case 0x02:
        hal_rtc_get(&r);
        CH = bcd(r.hour);
        CL = bcd(r.min);
        DH = bcd(r.sec);
        DL = 0;
        bios_set_cf(0);
        break;
    case 0x03:
        hal_rtc_get(&r);
        r.hour = unbcd(CH);
        r.min = unbcd(CL);
        r.sec = unbcd(DH);
        hal_rtc_set(&r);
        bios_set_cf(0);
        break;
    case 0x04:
        hal_rtc_get(&r);
        CH = bcd(r.year / 100);
        CL = bcd(r.year % 100);
        DH = bcd(r.month);
        DL = bcd(r.day);
        bios_set_cf(0);
        break;
    case 0x05:
        hal_rtc_get(&r);
        r.year = unbcd(CH) * 100 + unbcd(CL);
        r.month = unbcd(DH);
        r.day = unbcd(DL);
        hal_rtc_set(&r);
        bios_set_cf(0);
        break;
    default: bios_set_cf(1); break;
    }
}

/* --- miscellaneous -------------------------------------------------------- */

static void int15(void)
{
    switch (AH) {
    case 0x4F: bios_set_cf(1); break; /* keyboard intercept: pass the scancode on */
    case 0x86: { /* wait CX:DX microseconds */
        uint64_t now = pc_now();
        if (!st.waiting) {
            st.waiting = 1;
            st.wait_deadline = now + (((uint64_t)CX << 16) | DX) * 1000u;
        }
        if (now < st.wait_deadline) {
            cpu.ip -= 2;
            cpu.halted = 1;
            pc_wait_until(st.wait_deadline);
            return;
        }
        st.waiting = 0;
        bios_set_cf(0);
        break;
    }
    case 0x24: /* A20 gate */
        if (cpu.model < CPU_80386) {
            AH = 0x86;
            bios_set_cf(1);
            break;
        }
        if (AL < 2) {
            pc_outb(0x92, (uint8_t)((pc_inb(0x92) & ~3) | (AL ? 2 : 0)));
        } else if (AL == 2) {
            AL = (pc_inb(0x92) >> 1) & 1;
        } else {
            BX = 3; /* keyboard controller and port 92h */
        }
        AH = 0;
        bios_set_cf(0);
        break;
    case 0x87: { /* move CX words between anywhere in memory; ES:SI -> a GDT */
        const uint32_t g = SEGOFF(ES, SI), n = (uint32_t)CX * 2;
        const uint32_t src = mem_r16(g + 0x12) | ((uint32_t)mem_r8(g + 0x14) << 16) |
                             ((uint32_t)mem_r8(g + 0x17) << 24);
        const uint32_t dst = mem_r16(g + 0x1A) | ((uint32_t)mem_r8(g + 0x1C) << 16) |
                             ((uint32_t)mem_r8(g + 0x1F) << 24);
        if (cpu.model < CPU_80386 || src + n > cpu_ram_size || dst + n > cpu_ram_size) {
            AH = 0x86;
            bios_set_cf(1);
            break;
        }
        memmove(cpu_mem + dst, cpu_mem + src, n);
        AH = 0;
        bios_set_cf(0);
        bios_set_zf(1);
        break;
    }
    case 0x88: /* extended memory, in KB */
        AX = (uint16_t)(cpu_ram_size / 1024 - 1024 > 0xFFFF ? 0xFFFF : cpu_ram_size / 1024 - 1024);
        bios_set_cf(0);
        break;
    case 0x90:
    case 0x91:
        AH = 0;
        bios_set_cf(0);
        break;
    default:
        AH = 0x86;
        bios_set_cf(1);
        break;
    }
}

static void int14(void)
{
    if (DX != 0) {
        AX = 0x8000;
        return;
    }
    switch (AH) {
    case 0x01: AH = 0x60; break;
    case 0x02: AH = 0x80; break; /* nothing to receive */
    default: AX = 0x6030; break;
    }
}

static void int17(void)
{
    if (DX != 0) {
        AH = 0x01;
        return;
    }
    if (AH == 0x00) {
        hal_lpt_putc(AL);
    }
    AH = 0x90;
}

static void invop(void)
{
    if (!st.invop_shown) {
        char msg[64];
        st.invop_shown = 1;
        snprintf(msg, sizeof(msg), "\r\nInvalid opcode at %04X:%04X - system halted\r\n",
                 mem_r16(SEGOFF(SS, SP + 2)), mem_r16(SEGOFF(SS, SP)));
        bios_puts(msg);
    }
    cpu.ip -= 2;
    cpu.f_if = 1;
    cpu.halted = 1;
}

/* --- POST and bootstrap --------------------------------------------------- */

static void post(void)
{
    char line[96], desc[64];
    hal_rtc_t r;
    int nfd, nhd;

    memset(cpu_mem, 0, 0xA0000);
    memset(&st, 0, sizeof(st));
    /* DOS programs count on addresses wrapping around at 1 MB until an XMS driver opens
     * the gate. */
    cpu_set_a20(0);

    for (int i = 0; i < 256; i++) {
        mem_w16((uint32_t)i * 4, 0xFF53);
        mem_w16((uint32_t)i * 4 + 2, 0xF000);
    }
    uint16_t tab = rom16(0x08), n = rom16(tab);
    for (int i = 0; i < n; i++) {
        mem_w16((uint32_t)rom16(tab + 2 + i * 4) * 4, rom16(tab + 4 + i * 4));
    }

    /* Interrupt controller, system timer (18.2 Hz) and speaker timer, as a real BIOS
     * programs them. */
    if (cpu.model >= CPU_80386) {
        /* two controllers, as in the AT; the second one carries the coprocessor's
         * error interrupt on its line 5 (IRQ 13) */
        pc_outb(0x20, 0x11);
        pc_outb(0x21, 0x08);
        pc_outb(0x21, 0x04);
        pc_outb(0x21, 0x01);
        pc_outb(0x21, 0xB8); /* enable timer, keyboard, the second controller, floppy */
        pc_outb(0xA0, 0x11);
        pc_outb(0xA1, 0x70);
        pc_outb(0xA1, 0x02);
        pc_outb(0xA1, 0x01);
        pc_outb(0xA1, 0xDF);
    } else {
        pc_outb(0x20, 0x13);
        pc_outb(0x21, 0x08);
        pc_outb(0x21, 0x09);
        pc_outb(0x21, 0xBC); /* enable timer, keyboard, floppy */
    }
    pc_outb(0x43, 0x36);
    pc_outb(0x40, 0);
    pc_outb(0x40, 0);
    pc_outb(0x43, 0xB6);
    pc_outb(0x42, 0x33);
    pc_outb(0x42, 0x05);

    hal_disks_refresh();
    hostdisk_refresh();
    disk_post();
    nfd = disk_count_fd();
    nhd = disk_count_hd();

    mem_w16(BDA(0x00), 0x3F8); /* COM1 */
    mem_w16(BDA(0x08), 0x378); /* LPT1 */
    mem_w8(BDA(0x78), 1);
    mem_w8(BDA(0x7C), 1);
    /* one printer, one serial port, 80x25 colour, floppy count */
    mem_w16(BDA(0x10), (uint16_t)(0x4220 | (nfd ? 1 | ((nfd - 1) << 6) : 0) |
                                  (cpu_fpu_fitted ? 2 : 0))); /* bit 1: coprocessor */
    mem_w16(BDA(0x13), 640);
    mem_w8(BDA(0x17), 0x20); /* NumLock on */
    mem_w16(BDA(0x1A), 0x1E);
    mem_w16(BDA(0x1C), 0x1E);
    mem_w16(BDA(0x80), 0x1E);
    mem_w16(BDA(0x82), 0x3E);
    mem_w8(BDA(0x96), 0x10); /* enhanced keyboard */
    mem_w8(BDA(0x75), (uint8_t)nhd);
    kb_sync_leds();

    hal_rtc_get(&r);
    uint64_t secs = (uint64_t)r.hour * 3600 + (uint64_t)r.min * 60 + (uint64_t)r.sec;
    uint32_t ticks = (uint32_t)(secs * 1193182u / 65536u);
    mem_w16(BDA(0x6C), (uint16_t)ticks);
    mem_w16(BDA(0x6E), (uint16_t)(ticks >> 16));

    bios_video_init();

    static const char banner[] = "BIOS " P4DOS_VERSION;
    for (const char *p = banner; *p; p++) {
        bios_tty((uint8_t)*p, 0x0F);
    }
    snprintf(line, sizeof(line), "\r\nESP32-P4 RISC-V host, %s CPU, VGA display\r\n\r\n",
             cpu.model < CPU_80386 ? (cpu.model == CPU_80186 ? "80186-compatible" : "8086-compatible")
                                   : cpu_fpu_fitted ? "80386DX and 80387" : "80386DX");
    bios_puts(line);
    bios_puts("640 KB conventional memory\r\n");
    if (cpu_ram_size > CPU_MEM_SIZE) {
        snprintf(line, sizeof(line), "%u KB extended memory\r\n",
                 (unsigned)(cpu_ram_size / 1024 - 1024));
        bios_puts(line);
    }
    bios_puts("Sound Blaster 2.0 compatible at port 220h, IRQ 7, DMA 1\r\n");
    for (int d = 0; d < DRIVE_COUNT; d++) {
        if (disk_describe(d, desc, sizeof(desc))) {
            snprintf(line, sizeof(line), "%s\r\n", desc);
            bios_puts(line);
        }
    }
    if (!nfd && !nhd) {
        bios_puts("No drives attached\r\n");
    }
    bios_puts("\r\n");
}

void bios_hypercall(uint8_t fn)
{
    switch (fn) {
    case 0x10: bios_int10(); break;
    case 0x11: AX = mem_r16(BDA(0x10)); break;
    case 0x12: AX = mem_r16(BDA(0x13)); break;
    case 0x13: disk_int13(); break;
    case 0x14: int14(); break;
    case 0x15: int15(); break;
    case 0x16: int16(); break;
    case 0x17: int17(); break;
    case 0x18: bios_puts("\r\nNo ROM BASIC\r\n"); break;
    case 0x1A: int1a(); break;
    case HC_POST: post(); break;
    case HC_BOOT:
        if (!disk_boot()) {
            bios_puts("No bootable disk. Insert a system disk and press any key.\r\n");
        }
        break;
    case HC_KEY: AH = (uint8_t)key_process(AL); break;
    case HC_TICK: tick(); break;
    case HC_INVOP: invop(); break;
    case HC_EMS: ems_call(); break;
    case HC_XMS: xms_call(); break;
    default: hal_log("unknown BIOS hypercall %02X\n", fn); break;
    }
}
