/* Minimal machine stubs so the CPU core can be driven through the 80386 test suite. */
#include "../core/cpu.h"

#define RAM_SIZE 0x1000000u
static uint8_t ram[RAM_SIZE + 16];
int t_invalid, t_shutdown;

uint8_t pc_inb(uint16_t port) { (void)port; return 0xFF; }
void pc_outb(uint16_t port, uint8_t v) { (void)port; (void)v; }
uint8_t pc_irq_ack(void) { return 0; }
void pc_hypercall(uint8_t fn) { (void)fn; }
void pc_invalid_opcode(uint8_t op) { (void)op; t_invalid++; }
uint8_t pc_mem_read(uint32_t a) { return ram[a]; }
void pc_mem_write(uint32_t a, uint8_t v) { ram[a] = v; }
void pc_cpu_shutdown(void) { t_shutdown++; cpu.halted = 1; }
void pc_fpu_error(void) {}

uint8_t *t386_init(void)
{
    cpu_mem = ram;
    cpu_ram_size = RAM_SIZE;
    cpu_init(CPU_80386);
    cpu_set_mem_limits(CPU_MEM_SIZE, CPU_MEM_SIZE);
    return ram;
}

/* Register order: cr0 cr3 eax ebx ecx edx esi edi ebp esp cs ds es fs gs ss eip eflags
 * dr6 dr7. Runs the instruction at CS:EIP and whatever follows it up to a HLT; returns
 * non-zero if that came. */
int t386_run(uint32_t *r)
{
    static const uint8_t gpr[8] = {REG_AX, REG_BX, REG_CX, REG_DX, REG_SI, REG_DI, REG_BP, REG_SP};
    static const uint8_t segs[6] = {SEG_CS, SEG_DS, SEG_ES, SEG_FS, SEG_GS, SEG_SS};
    cpu_reset();
    cpu.cr0 = r[0] & 0x0E;
    cpu.cr3 = r[1];
    for (int i = 0; i < 8; i++) {
        cpu.r.d[gpr[i]] = r[2 + i];
    }
    for (int i = 0; i < 6; i++) {
        cpu_set_seg(segs[i], (uint16_t)r[10 + i]);
    }
    cpu.ip = r[16];
    cpu_set_eflags(r[17] & ~(uint32_t)(FL_TF | FL_VM));
    cpu.dr[6] = r[18];
    cpu.dr[7] = r[19];
    cpu.halted = 0;
    cpu_run(8);
    r[0] = (r[0] & ~0x8000000Fu) | cpu.cr0;
    r[1] = cpu.cr3;
    for (int i = 0; i < 8; i++) {
        r[2 + i] = cpu.r.d[gpr[i]];
    }
    for (int i = 0; i < 6; i++) {
        r[10 + i] = cpu.seg[segs[i]];
    }
    r[16] = cpu.ip;
    r[17] = cpu_get_eflags();
    r[18] = cpu.dr[6];
    r[19] = cpu.dr[7];
    return cpu.halted;
}
