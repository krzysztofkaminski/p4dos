/* Minimal machine stubs so the CPU core can be driven one instruction at a time. */
#include "../core/cpu.h"

static uint8_t ram[CPU_MEM_SIZE + 16];
int t_invalid;

uint8_t pc_inb(uint16_t port) { (void)port; return 0xFF; }
void pc_outb(uint16_t port, uint8_t v) { (void)port; (void)v; }
uint8_t pc_irq_ack(void) { return 0; }
void pc_hypercall(uint8_t fn) { (void)fn; }
void pc_invalid_opcode(uint8_t op) { (void)op; t_invalid++; }
uint8_t pc_mem_read(uint32_t a) { return ram[a]; }
void pc_mem_write(uint32_t a, uint8_t v) { ram[a] = v; }
void pc_cpu_shutdown(void) {}
void pc_fpu_error(void) {}

uint8_t *t_init(int model)
{
    cpu_mem = ram;
    cpu_set_mem_limits(CPU_MEM_SIZE, CPU_MEM_SIZE);
    cpu_init(model);
    cpu.test_mode = 1;
    return ram;
}

/* Register order: ax bx cx dx cs ss ds es sp bp si di ip flags */
void t_run(uint16_t *r)
{
    cpu.r.w[REG_AX * 2] = r[0];
    cpu.r.w[REG_BX * 2] = r[1];
    cpu.r.w[REG_CX * 2] = r[2];
    cpu.r.w[REG_DX * 2] = r[3];
    cpu_set_seg(SEG_CS, r[4]);
    cpu_set_seg(SEG_SS, r[5]);
    cpu_set_seg(SEG_DS, r[6]);
    cpu_set_seg(SEG_ES, r[7]);
    cpu.r.w[REG_SP * 2] = r[8];
    cpu.r.w[REG_BP * 2] = r[9];
    cpu.r.w[REG_SI * 2] = r[10];
    cpu.r.w[REG_DI * 2] = r[11];
    cpu.ip = r[12];
    cpu_set_flags(r[13]);
    cpu.halted = 0;
    cpu.trap_armed = 0;
    cpu.f_tf = 0; /* the suite never single-steps; TF in the image is only data */
    cpu_run(1);
    r[0] = cpu.r.w[REG_AX * 2];
    r[1] = cpu.r.w[REG_BX * 2];
    r[2] = cpu.r.w[REG_CX * 2];
    r[3] = cpu.r.w[REG_DX * 2];
    r[4] = cpu.seg[SEG_CS];
    r[5] = cpu.seg[SEG_SS];
    r[6] = cpu.seg[SEG_DS];
    r[7] = cpu.seg[SEG_ES];
    r[8] = cpu.r.w[REG_SP * 2];
    r[9] = cpu.r.w[REG_BP * 2];
    r[10] = cpu.r.w[REG_SI * 2];
    r[11] = cpu.r.w[REG_DI * 2];
    r[12] = cpu.ip;
    r[13] = (uint16_t)((cpu_get_flags() & ~FL_TF) | (r[13] & FL_TF));
}
