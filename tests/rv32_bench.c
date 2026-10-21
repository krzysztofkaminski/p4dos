/*
 * Bare CPU core benchmark for the RV32 build: runs the SPEED.COM instruction mixes and
 * reports how many RISC-V instructions the interpreter needs per x86 instruction, counted
 * exactly with the minstret CSR. Run under qemu-system-riscv32 with -icount shift=0
 * (make bench-rv32).
 */
#include <stdio.h>
#include <string.h>

#include "cpu.h"

#define UART ((volatile uint8_t *)0x10000000)

static uint8_t ram[CPU_MEM_SIZE + 16];

uint8_t pc_inb(uint16_t p) { (void)p; return 0xFF; }
void pc_outb(uint16_t p, uint8_t v) { (void)p; (void)v; }
uint8_t pc_irq_ack(void) { return 0; }
void pc_hypercall(uint8_t fn) { (void)fn; }
void pc_invalid_opcode(uint8_t op) { (void)op; }
uint8_t pc_mem_read(uint32_t a) { return ram[a]; }
void pc_mem_write(uint32_t a, uint8_t v) { ram[a] = v; }
void pc_cpu_shutdown(void) {}
void pc_fpu_error(void) {}

static void put(const char *s)
{
    while (*s) {
        while (!(UART[5] & 0x20)) {
        }
        UART[0] = (uint8_t)*s++;
    }
}

void trap_report(unsigned cause, unsigned epc, unsigned tval)
{
    char b[80];
    snprintf(b, sizeof(b), "trap %08x at %08x (%08x)\n", cause, epc, tval);
    put(b);
}

static uint64_t instret(void)
{
    uint32_t hi, lo;
    do {
        __asm__ volatile("csrr %0, minstreth" : "=r"(hi));
        __asm__ volatile("csrr %0, minstret" : "=r"(lo));
        uint32_t hi2;
        __asm__ volatile("csrr %0, minstreth" : "=r"(hi2));
        if (hi == hi2) {
            break;
        }
    } while (1);
    return ((uint64_t)hi << 32) | lo;
}

static const struct {
    const char *name;
    uint8_t code[28];
} blocks[] = {
    /* mov cx,0; body; loop body; jmp start */
    {"register", {0xB9, 0, 0, 0x01, 0xD8, 0x31, 0xC2, 0x43, 0x4A, 0xE2, 0xF8, 0xEB, 0xF3}},
    {"memory", {0xB9, 0, 0, 0xBE, 0x00, 0x20, 0xBF, 0x08, 0x20, 0xBB, 0x10, 0x20, 0x8B, 0x04,
                0x03, 0x44, 0x02, 0x89, 0x05, 0xFF, 0x07, 0xE2, 0xF5, 0xEB, 0xE7}},
    {"stack", {0xB9, 0, 0, 0x50, 0xE8, 0x05, 0x00, 0x58, 0xE2, 0xF9, 0xEB, 0xF4, 0xC3}},
    {"branch", {0xB9, 0, 0, 0xA8, 0x01, 0x74, 0x01, 0x42, 0x39, 0xD8, 0x75, 0x01, 0x90, 0x40,
                0xE2, 0xF3, 0xEB, 0xEE}},
    /* byte registers and immediates: mov al,[si]; add al,3; mov [di],al; cmp al,7Fh; jb +0 */
    {"bytes", {0xB9, 0, 0, 0xBE, 0x00, 0x20, 0xBF, 0x08, 0x20, 0x8A, 0x04, 0x04, 0x03, 0x88,
               0x05, 0x3C, 0x7F, 0x72, 0x00, 0xE2, 0xF4, 0xEB, 0xE9}},
};

int main(void)
{
    char line[96];
    cpu_mem = ram;
    cpu_set_mem_limits(CPU_MEM_SIZE, CPU_MEM_SIZE);
    unsigned sum = 0; /* hundredths; no floating point: the bare-metal libc cannot print it */
    for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        memset(ram, 0, 0x30000);
        cpu_init(CPU_80386);
        cpu_set_seg(SEG_CS, 0x1000);
        cpu_set_seg(SEG_DS, 0x1000);
        cpu_set_seg(SEG_ES, 0x1000);
        cpu_set_seg(SEG_SS, 0x1000);
        cpu.r.w[REG_SP * 2] = 0xFFFE;
        cpu.ip = 0x100;
        memcpy(ram + 0x10100, blocks[b].code, sizeof(blocks[b].code));
        const int n = 2000000;
        uint64_t t0 = instret();
        int done = 0;
        for (int i = 0; i < 4; i++) {
            done += cpu_run(n / 4); /* slices of the size the machine layer uses are 4096 */
        }
        uint64_t t1 = instret();
        unsigned per = (unsigned)((t1 - t0) * 100 / (unsigned)done);
        sum += per;
        snprintf(line, sizeof(line), "%-9s %3u.%02u RISC-V instructions per x86 instruction\n",
                 blocks[b].name, per / 100, per % 100);
        put(line);
    }
    sum /= sizeof(blocks) / sizeof(blocks[0]);
    snprintf(line, sizeof(line), "%-9s %3u.%02u\n", "average", sum / 100, sum % 100);
    put(line);
    put("DONE\n");
    *(volatile uint32_t *)0x100000 = 0x5555; /* QEMU "virt" test device: power off */
    for (;;) {
    }
}

/* newlib stubs */
#include <reent.h>
#include <sys/stat.h>
extern char __heap_start[], __heap_end[];
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
int _write(int fd, const char *buf, int len) { (void)fd; (void)buf; return len; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _getpid(void) { return 1; }
void _exit(int code) { (void)code; for (;;) { } }
