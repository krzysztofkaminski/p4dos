/*
 * Bare-metal RV32 port for the QEMU "virt" machine. It exists to run the emulator core as
 * real RISC-V code under an emulator on Linux: the same C that goes into the ESP32-P4
 * firmware, compiled with the same toolchain, booting FreeDOS with a serial console.
 *
 *   qemu-system-riscv32 -M virt -m 256M -bios none -nographic \
 *       -kernel build/rv32/p4dos.elf -device loader,file=build/p4dos-hd.img,addr=0x84000000
 */
#include <reent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "pc.h"
#include "term.h"
#include "video.h"

#define UART ((volatile uint8_t *)0x10000000)
#define MTIME_LO (*(volatile uint32_t *)0x0200BFF8)
#define MTIME_HI (*(volatile uint32_t *)0x0200BFFC)
#define RTC_LO (*(volatile uint32_t *)0x00101000)
#define RTC_HI (*(volatile uint32_t *)0x00101004)
#define DISK ((uint8_t *)0x84000000)

#define GUEST_RAM_KB 8192
static uint8_t guest_ram[GUEST_RAM_KB * 1024 + 16];
static uint8_t ems_ram[1024 * 1024];
static uint8_t vram[VGA_RAM_SIZE];

static void uart_putc(char c)
{
    while (!(UART[5] & 0x20)) {
    }
    UART[0] = (uint8_t)c;
}

static void uart_write(const char *s, int n)
{
    while (n--) {
        uart_putc(*s++);
    }
}

static int term_out(const char *s, int n)
{
    uart_write(s, n);
    return 0;
}

void trap_report(unsigned cause, unsigned epc, unsigned tval)
{
    char b[96];
    int n = snprintf(b, sizeof(b), "\r\n*** trap: mcause=%08x mepc=%08x mtval=%08x\r\n", cause, epc,
                     tval);
    uart_write(b, n);
}

/* --- HAL -------------------------------------------------------------------- */

uint64_t hal_time_ns(void)
{
    uint32_t hi, lo;
    do {
        hi = MTIME_HI;
        lo = MTIME_LO;
    } while (hi != MTIME_HI);
    return (((uint64_t)hi << 32) | lo) * 100u; /* the virt CLINT counts at 10 MHz */
}

void hal_idle(uint32_t ns)
{
    uint64_t end = hal_time_ns() + ns;
    while (hal_time_ns() < end && !(UART[5] & 1)) {
    }
}

void hal_log(const char *fmt, ...)
{
    (void)fmt; /* the UART carries the screen; there is nowhere else to log */
}

void hal_rtc_get(hal_rtc_t *t)
{
    uint64_t ns = RTC_LO;
    ns |= (uint64_t)RTC_HI << 32;
    time_t secs = (time_t)(ns / 1000000000u);
    struct tm tm;
    gmtime_r(&secs, &tm);
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day = tm.tm_mday;
    t->hour = tm.tm_hour;
    t->min = tm.tm_min;
    t->sec = tm.tm_sec;
}

void hal_rtc_set(const hal_rtc_t *t) { (void)t; }
void hal_lpt_putc(uint8_t c) { (void)c; }
void hal_disks_refresh(void) {}
void hal_input_lock(void) {}
void hal_input_unlock(void) {}

/* --- RAM disk --------------------------------------------------------------- */

static int rd_read(blkdev_t *d, uint32_t lba, uint32_t n, uint8_t *buf)
{
    (void)d;
    memcpy(buf, DISK + (size_t)lba * 512, (size_t)n * 512);
    return 0;
}

static int rd_write(blkdev_t *d, uint32_t lba, uint32_t n, const uint8_t *buf)
{
    (void)d;
    memcpy(DISK + (size_t)lba * 512, buf, (size_t)n * 512);
    return 0;
}

static blkdev_t ramdisk = {.read = rd_read, .write = rd_write};

/* The loader gives no size: take it from the partition table or the floppy BPB. */
static int probe_disk(void)
{
    const uint8_t *s = DISK;
    uint32_t end = 0;
    if (s[510] != 0x55 || s[511] != 0xAA) {
        return -1;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + i * 16;
        uint32_t start = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t size = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
        if (e[4] && start + size > end && start + size < 0x400000) {
            end = start + size;
        }
    }
    if (end) {
        ramdisk.sectors = end;
        return DRIVE_HD0;
    }
    ramdisk.sectors = s[19] | (s[20] << 8);
    return ramdisk.sectors ? DRIVE_FD0 : -1;
}

int main(void)
{
    pc_config_t cfg = {.ram = guest_ram, .ram_kb = GUEST_RAM_KB, .vram = vram,
                       .cpu_model = CPU_80386, .fpu = 1, .ems_kb = sizeof(ems_ram) / 1024,
                       .ems_ram = ems_ram};
    uint64_t last_refresh = 0, last_input = 0;
    int esc_pending = 0;

    pc_init(&cfg);
    int drive = probe_disk();
    if (drive >= 0) {
        pc_attach(drive, &ramdisk);
    }
    pc_reset();
    term_init(term_out);

    for (;;) {
        uint64_t now = hal_time_ns();
        if (UART[5] & 1) {
            kbd_term_char(UART[0]);
            esc_pending = 1;
            last_input = now;
        } else if (esc_pending && now - last_input > 50000000u) {
            kbd_term_char(-1);
            esc_pending = 0;
        }
        for (int i = 0; i < 4; i++) {
            pc_step();
        }
        if (now - last_refresh > 50000000u) {
            last_refresh = now;
            term_refresh(0);
        }
    }
}

/* --- newlib system call stubs ----------------------------------------------- */

extern char __heap_start[], __heap_end[];

/* The Espressif newlib looks its reentrancy structure up through this hook. */
static struct _reent reent_data;
struct _reent *__getreent(void) { return &reent_data; }

void *_sbrk(ptrdiff_t incr)
{
    static char *brk = __heap_start;
    char *prev = brk;
    if (brk + incr > __heap_end) {
        return (void *)-1;
    }
    brk += incr;
    return prev;
}

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    uart_write(buf, len);
    return len;
}

int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _getpid(void) { return 1; }
void _exit(int code) { (void)code; for (;;) { } }
