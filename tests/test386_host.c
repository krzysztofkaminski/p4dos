/*
 * Runs test386.asm (https://github.com/barotto/test386.asm), a ROM that exercises an
 * 80386 including protected mode, paging and task switches, on the CPU core alone.
 *
 * usage: test386_host ROM [EE-OUTPUT-FILE]
 * The ROM is assembled with POST_PORT = 190h and OUT_PORT = E9h. Exit status 0 when the
 * ROM reports POST code FFh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cpu.h"

#define RAM_SIZE 0x1000000u
static uint8_t ram[RAM_SIZE + 16];
static int post = -1, shutdowns;
static FILE *out;

uint8_t pc_inb(uint16_t port) { (void)port; return 0xFF; }
void pc_outb(uint16_t port, uint8_t v)
{
    if (port == 0x190) {
        post = v;
    } else if (port == 0xE9 && out) {
        fputc(v, out);
    }
}
uint8_t pc_irq_ack(void) { return 0; }
void pc_hypercall(uint8_t fn) { (void)fn; }
void pc_invalid_opcode(uint8_t op) { (void)op; }
uint8_t pc_mem_read(uint32_t a) { return ram[a]; }
void pc_mem_write(uint32_t a, uint8_t v) { (void)a; (void)v; }
void pc_cpu_shutdown(void) { shutdowns++; cpu.halted = 1; }
void pc_fpu_error(void) {}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint64_t total = 0;
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: test386_host ROM [EE-OUTPUT-FILE]\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n != 0x10000 && n != 0x20000) {
        fprintf(stderr, "ROM must be 64 or 128 KiB\n");
        return 2;
    }
    if (fread(ram + 0x100000 - n, 1, (size_t)n, f) != (size_t)n) {
        return 2;
    }
    fclose(f);
    out = argc > 2 ? fopen(argv[2], "w") : NULL;

    cpu_mem = ram;
    cpu_ram_size = RAM_SIZE;
    cpu_init(CPU_80386);
    cpu_set_mem_limits(CPU_MEM_SIZE, 0x100000 - (uint32_t)n);
    if (getenv("TEST386_TRACE")) {
        /* Single-step and show where the last instructions before the stop were. */
        enum { N = 4000 };
        static struct { uint16_t cs; uint32_t ip, ax, sp; uint8_t post; } h[N];
        unsigned k = 0;
        const char *stop_s = getenv("TEST386_STOP");
        const uint32_t stop = stop_s ? (uint32_t)strtoul(stop_s, NULL, 16) : 0xFFFFFFFFu;
        while (!cpu.halted && total < 2000000000ull && cpu.ip != stop) {
            h[k % N].cs = cpu.seg[SEG_CS];
            h[k % N].ip = cpu.ip;
            h[k % N].ax = cpu.r.d[REG_AX];
            h[k % N].sp = cpu.r.d[REG_SP];
            h[k % N].post = (uint8_t)post;
            k++;
            total += (uint64_t)cpu_run(1);
        }
        for (unsigned i = k > N ? k - N : 0; i < k; i++) {
            printf("  %04X:%08X eax=%08X esp=%08X post=%02X\n", h[i % N].cs, (unsigned)h[i % N].ip,
                   (unsigned)h[i % N].ax, (unsigned)h[i % N].sp, h[i % N].post);
        }
    }
    while (!cpu.halted && total < 2000000000ull && !getenv("TEST386_STOP")) {
        total += (uint64_t)cpu_run(1000000);
    }
    if (out) {
        fclose(out);
    }
    printf("POST %02X after %llu instructions, at %04X:%08X%s%s\n", post & 0xFF,
           (unsigned long long)total, cpu.seg[SEG_CS], (unsigned)cpu.ip,
           cpu.halted ? "" : " (still running)", shutdowns ? " (CPU shutdown)" : "");
    return post == 0xFF && !shutdowns ? 0 : 1;
}
