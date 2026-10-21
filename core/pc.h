/*
 * The emulated machine: a PC with an 80386DX (or, for old software, an 80186 or 8086),
 * 640 KiB of conventional memory and, with the 80386, extended memory behind an A20 gate,
 * a VGA display adapter, an XT-style keyboard interface, up to two floppy and two hard
 * drives, Sound Blaster compatible sound and a Microsoft-compatible serial mouse on COM1.
 */
#ifndef P4DOS_PC_H
#define P4DOS_PC_H

#include <stdint.h>

#include "cpu.h"
#include "hal.h"

#define P4DOS_VERSION "1.4"

typedef struct {
    uint8_t *ram;      /* guest memory: ram_kb KiB + 16 bytes */
    uint32_t ram_kb;   /* 1024, or more with the 80386: the rest is extended memory */
    int cpu_model;     /* CPU_80386, CPU_80186 or CPU_8086 */
    int fpu;           /* an 80387 next to the 80386 */
    int virtual_time;  /* derive time from the instruction count (deterministic tests) */
    uint32_t ems_kb;   /* expanded memory offered through EMM.SYS, 0 = none */
    uint8_t *ems_ram;  /* ems_kb KiB */
    uint8_t *vram;     /* VGA_RAM_SIZE (256 KiB) of video memory for the planar modes */
    uint32_t xms_kb;   /* memory offered as XMS blocks by XMS.SYS on a machine without
                          extended memory (8086, 80186); 0 = none */
    uint8_t *xms_ram;  /* xms_kb KiB */
} pc_config_t;

/* DRIVE_HD2 is the host disk (hostdisk.c). */
enum { DRIVE_FD0 = 0, DRIVE_FD1 = 1, DRIVE_HD0 = 2, DRIVE_HD1 = 3, DRIVE_HD2 = 4, DRIVE_COUNT = 5 };

void pc_init(const pc_config_t *cfg);
void pc_attach(int drive, blkdev_t *dev);
void pc_reset(void);
/* Run one slice of emulation (about a millisecond of work), idling when the CPU waits. */
void pc_step(void);
uint64_t pc_now(void);
extern int pc_batch; /* instructions per slice; 1 single-steps (debugging) */
/* Debugging aid: called for every port access when set. */
extern void (*pc_io_trace)(int is_write, uint16_t port, uint8_t value);

/* --- interrupt controller / timers (pc.c) --- */
void pic_raise(int irq);
/* Top of writable guest memory (below the ROMs); and re-evaluate what the CPU may access
 * directly after it or the video mode changed. */
extern uint32_t pc_ram_limit;
void pc_update_mem_limits(void);
void pc_beep(void);
void pc_wait_until(uint64_t deadline_ns);
extern uint8_t pc_port61;

/* --- BIOS (bios.c, biosvid.c) --- */
void bios_hypercall(uint8_t fn);
void bios_int10(void);
void bios_video_init(void);
void bios_tty(uint8_t ch, uint8_t attr);
void bios_puts(const char *s);
/* Carry flag in the FLAGS image the interrupt stub will IRET with. */
void bios_set_cf(int cf);
void bios_set_zf(int zf);

/* --- disks (disk.c) --- */
void disk_init(void);
void disk_attach(int drive, blkdev_t *dev);
void disk_post(void);
void disk_int13(void);
int disk_boot(void);
int disk_count_fd(void);
int disk_count_hd(void);
void disk_flush(void);
const char *disk_describe(int drive, char *buf, int len);

/* --- keyboard (kbd.c) --- */
void kbd_init(void);
void kbd_push(uint8_t scancode);                       /* raw set-1 byte, any thread */
void kbd_hid_report(uint8_t mods, const uint8_t *keys, int nkeys); /* USB boot report */
void kbd_term_char(int ch);                            /* terminal byte; -1 = input idle */
void kbd_type_ascii(int ch);
int kbd_pop(uint64_t now_ns);                          /* -1 when empty; emulator thread */
void kbd_set_typematic(uint32_t delay_ms, uint32_t period_ms);
uint8_t kbd_leds(void);
int kbd_ctrl_alt(void);                                /* Ctrl and Alt both held */
void kbd_set_leds(uint8_t leds);                       /* bit0 scroll, bit1 num, bit2 caps */

/* --- serial mouse on COM1 (uart.c) --- */
void uart_init(void);
uint8_t uart_inb(uint16_t port);
void uart_outb(uint16_t port, uint8_t v);
void uart_poll(void);
void mouse_event(int dx, int dy, int buttons);         /* any thread; bit0 left, bit1 right */
extern int mouse_present;

/* --- expanded memory (ems.c) --- */
void ems_init(uint8_t *ram, uint32_t kb);
void ems_reset(void);
void ems_call(void);

/* --- Sound Blaster and DMA controller (sb.c) --- */
void sb_reset(void);
void sb_poll(uint64_t now_ns);
uint8_t sb_inb(uint16_t port);
void sb_outb(uint16_t port, uint8_t v);
uint8_t dma_inb(uint16_t port);
void dma_outb(uint16_t port, uint8_t v);

/* --- extended memory blocks (xms.c) --- */
void xms_init(uint8_t *ram, uint32_t kb);
void xms_reset(void);
void xms_call(void);

/* guest memory helpers */
static inline uint8_t mem_r8(uint32_t a) { return cpu_mem[a & CPU_MEM_MASK]; }
static inline uint16_t mem_r16(uint32_t a)
{
    return (uint16_t)(cpu_mem[a & CPU_MEM_MASK] | (cpu_mem[(a + 1) & CPU_MEM_MASK] << 8));
}
static inline void mem_w8(uint32_t a, uint8_t v) { cpu_mem[a & CPU_MEM_MASK] = v; }
static inline void mem_w16(uint32_t a, uint16_t v)
{
    cpu_mem[a & CPU_MEM_MASK] = (uint8_t)v;
    cpu_mem[(a + 1) & CPU_MEM_MASK] = (uint8_t)(v >> 8);
}
#define SEGOFF(seg, off) ((((uint32_t)(seg)) << 4) + (uint16_t)(off))
#define BDA(off) (0x400u + (off))

extern const uint8_t p4dos_rom[0x10000];

#endif
