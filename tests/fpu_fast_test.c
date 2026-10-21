/*
 * Holds the coprocessor's fast path (the escape handlers in core/cpu.c and fpu_fast in
 * core/cpu_fpu.h) against the general code: from the same state, every escape opcode must
 * leave the same registers, status, pointers, memory and AX whether it is run as an
 * instruction, which takes the fast path where it can, or handed to the general code.
 * That is in turn checked against a real x87 by fx80_test.
 * usage: fpu_fast_test [CASES [SEED]]
 */
#include <stdio.h>
#include <stdlib.h>

#include "../core/cpu.c"

#define RAM_SIZE 0x200000u
static uint8_t ram[RAM_SIZE + 16];

uint8_t pc_inb(uint16_t port) { (void)port; return 0xFF; }
void pc_outb(uint16_t port, uint8_t v) { (void)port; (void)v; }
uint8_t pc_irq_ack(void) { return 0; }
void pc_hypercall(uint8_t fn) { (void)fn; }
void pc_invalid_opcode(uint8_t op) { (void)op; }
uint8_t pc_mem_read(uint32_t a) { return ram[a]; }
void pc_mem_write(uint32_t a, uint8_t v) { ram[a] = v; }
void pc_cpu_shutdown(void) {}
void pc_fpu_error(void) {}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Mostly ordinary numbers close to each other in size, some special ones. */
static fx_t random_fx(void)
{
    fx_t v;
    const uint32_t kind = (uint32_t)(rnd() % 64);
    uint64_t m = rnd();
    uint32_t exp;
    memset(&v, 0, sizeof(v));
    switch (rnd() % 6) { /* significands with structure */
    case 0: m &= 0xFFFFFF0000000000ull; break;
    case 1: m &= 0xFFFFFFFFFFFFF800ull; break;
    case 2: m = (m & 3) ? ~0ull << (rnd() % 64) : ~(~0ull << (rnd() % 64)); break;
    case 3: m = 1ull << (rnd() % 64); break;
    default: break;
    }
    switch (rnd() % 8) {
    case 0: exp = (uint32_t)(rnd() % 0x7FFF); break;
    case 1: exp = (rnd() & 1) ? (uint32_t)(1 + rnd() % 80) : (uint32_t)(0x7FFE - rnd() % 80); break;
    case 2: exp = (uint32_t)(FX_BIAS - 130 + rnd() % 260); break;   /* around the 32-bit range */
    case 3: exp = (uint32_t)(FX_BIAS - 1030 + rnd() % 2060); break; /* and the 64-bit one */
    default: exp = (uint32_t)(FX_BIAS - 70 + rnd() % 140); break;
    }
    m |= FX_J;
    if (kind == 0) {
        exp = 0, m = 0;
    } else if (kind == 1) {
        exp = 0x7FFF, m = FX_J;
    } else if (kind == 2) {
        exp = 0x7FFF, m = FX_J | (rnd() >> 1) | 1;
    } else if (kind == 3) {
        exp = 0, m = rnd() >> (1 + rnd() % 40);
    } else if (kind == 4) {
        m &= ~FX_J; /* unnormal */
    } else if (kind < 9) {
        exp = 0, m = 0;
    }
    v.m = m;
    v.se = (uint16_t)(exp | ((rnd() & 1) << 15));
    return v;
}

static void random_operand(uint8_t *p)
{
    fx_env_t e = {0, 64, 0x3F, 0};
    const fx_t x = random_fx();
    uint64_t v;
    for (int i = 0; i < 16; i++) {
        p[i] = (uint8_t)rnd();
    }
    switch (rnd() % 6) {
    case 0: /* a 32-bit real */
        v = fx_to_f32(&e, x);
        memcpy(p, &v, 4);
        break;
    case 1: /* a 64-bit real */
        v = fx_to_f64(&e, x);
        memcpy(p, &v, 8);
        break;
    case 2: /* a small integer, whatever its width */
        v = (uint64_t)((int64_t)(rnd() % 2000001) - 1000000);
        memcpy(p, &v, 8);
        break;
    case 3: /* an extended real */
        memcpy(p, &x.m, 8);
        memcpy(p + 8, &x.se, 2);
        break;
    default: break; /* any bits */
    }
}

typedef struct {
    fx_t st[8];
    uint16_t cw, sw, fop, fcs, fds, ax;
    uint8_t top, empty;
    uint32_t fip, fdp;
    uint8_t mem[128];
} snap_t;

static uint32_t mem_off, mem_mask;

static void snapshot(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++) {
        s->st[i].m = fpu.st[i].m;
        s->st[i].se = fpu.st[i].se;
    }
    s->cw = fpu.cw, s->sw = fpu.sw, s->fop = fpu.fop, s->fcs = fpu.fcs, s->fds = fpu.fds;
    s->top = fpu.top, s->empty = fpu.empty, s->fip = fpu.fip, s->fdp = fpu.fdp;
    s->ax = AX;
    memcpy(s->mem, cpu_mem + cpu.sg[DS].base + mem_off - 8, sizeof(s->mem));
}

/* The general code, which may end in a fault (an operand across the segment's end). */
static int general(uint32_t op, uint32_t modrm, const cpu_seg_t *sg, uint32_t off, int wide)
{
    if (setjmp(cpu.exc_jmp)) {
        cpu.exc_pending = 0;
        return 1;
    }
    fpu_exec(op, modrm, sg, off, mem_mask, wide);
    return 0;
}

int main(int argc, char **argv)
{
    static const uint16_t cws[] = {0x127F, 0x037F, 0x1F3F, 0x027F, 0x007F, 0x137F, 0x0F7F, 0x077F,
                                   0x0B7F, 0x0E7F, 0x0372, 0x1270, 0x023F, 0x033E};
    const long cases = argc > 1 ? atol(argv[1]) : 3000000;
    long fast = 0, taken[8] = {0}, seen[8] = {0};
    if (argc > 2) {
        rng_state = strtoull(argv[2], 0, 0) | 1;
    }
    cpu_mem = ram;
    cpu_ram_size = RAM_SIZE;
    cpu_fpu_fitted = 1;
    cpu_init(CPU_80386);
    cpu_set_mem_limits(CPU_MEM_SIZE, 0xC0000);
    cpu_set_seg(SEG_DS, 0x2000);
    cpu_set_seg(SEG_CS, 0x1000);
    cpu_set_seg(SEG_SS, 0x4000);
    cpu.r.w[REG_SP * 2] = 0x1000;

    for (long n = 0; n < cases; n++) {
        const uint32_t op = 0xD8 + (uint32_t)(rnd() % 8);
        uint32_t modrm = (uint32_t)(rnd() & 0xFF);
        const int prefixed = rnd() % 8 == 0; /* an operand-size prefix */
        const cpu_seg_t *sg;
        uint8_t operand[16], before[128], *code = cpu_mem + 0x10100;
        snap_t a, b;
        int was_fast, wide;

        if (rnd() % 3 == 0) {
            modrm |= 0xC0; /* the register forms are a quarter of the encodings */
        }
        sg = modrm >= 0xC0 ? NULL : &cpu.sg[DS];
        if (rnd() % 4 == 0) {
            /* a flat segment as 32-bit programs have them: its window is on extended
             * memory, the first megabyte is reached another way */
            cpu.dmode |= 1;
            cpu.sg[DS].base = 0;
            cpu.sg[DS].limit = 0xFFFFFFFFu;
            seg_window(&cpu.sg[DS]);
            mem_mask = 0xFFFFFFFFu;
            mem_off = (rnd() & 1 ? 0x110000 : 0x30000) + (uint32_t)(rnd() % 0x400);
            if (rnd() % 64 == 0) {
                mem_off = (rnd() & 1 ? 0xFFFF8 : 0xBFFF8) + (uint32_t)(rnd() % 8); /* ends of plain RAM */
            }
        } else {
            cpu.dmode &= (uint8_t)~1;
            cpu.sg[DS].limit = 0xFFFF;
            cpu_set_seg(SEG_DS, 0x2000);
            seg_window(&cpu.sg[DS]);
            mem_mask = 0xFFFF;
            mem_off = 0x100 + (uint32_t)(rnd() % 0x400);
            if (rnd() % 64 == 0) {
                mem_off = 0xFFF8 + (uint32_t)(rnd() % 8); /* across the end of the segment */
            }
        }
        /* FLDENV, FRSTOR, FNSTENV and FNSAVE need room that the segment has */
        if (sg && (mem_off & 0xFFFF) > 0xFF00 && ((op == 0xD9 && (modrm & 0x30) == 0x20) || (op == 0xDD && (modrm & 0x30) >= 0x20))) {
            mem_off = 0x100;
        }
        /* the instruction: the operand at an absolute address */
        wide = (int)((cpu.dmode & 1) ^ (unsigned)prefixed);
        if (prefixed) {
            *code++ = 0x66;
        }
        *code++ = (uint8_t)op;
        if (sg) {
            modrm = (modrm & 0x38) | (cpu.dmode & 1 ? 5 : 6);
            *code++ = (uint8_t)modrm;
            memcpy(code, &mem_off, cpu.dmode & 1 ? 4 : 2);
        } else {
            *code++ = (uint8_t)modrm;
        }

        memset(&fpu, 0, sizeof(fpu));
        for (int i = 0; i < 8; i++) {
            fpu.st[i] = random_fx();
        }
        fpu.cw = rnd() % 8 ? cws[rnd() % (rnd() % 4 ? 3 : sizeof(cws) / sizeof(cws[0]))] : (uint16_t)rnd();
        fpu.sw = (uint16_t)(rnd() & (rnd() % 4 ? 0x473F : 0xC7FF));
        fpu.top = (uint8_t)(rnd() & 7);
        fpu.empty = (uint8_t)(rnd() % 3 == 0 ? rnd() : rnd() % 3 == 0 ? 0 : (rnd() & rnd() & rnd()));
        fpu.fop = (uint16_t)(rnd() & 0x7FF), fpu.fcs = (uint16_t)rnd(), fpu.fds = (uint16_t)rnd();
        fpu.fip = (uint32_t)rnd(), fpu.fdp = (uint32_t)rnd();
        fpu_mode();
        for (int i = 0; i < 128; i++) {
            before[i] = (uint8_t)rnd();
        }
        random_operand(operand);
        memcpy(before + 8, operand, 16);
        cpu.insn_ip = 0x100;

        const typeof(fpu) start = fpu;
        const uint16_t ax = (uint16_t)rnd();

        /* what fpu_fast() declines it must not have touched */
        memcpy(cpu_mem + cpu.sg[DS].base + mem_off - 8, before, sizeof(before));
        AX = ax;
        was_fast = fpu_fast(op, modrm, sg, mem_off);
        if (!was_fast) {
            snapshot(&a);
            fpu = start;
            memcpy(cpu_mem + cpu.sg[DS].base + mem_off - 8, before, sizeof(before));
            AX = ax;
            snapshot(&b);
            if (memcmp(&a, &b, sizeof(a))) {
                printf("case %ld: op %02X %02X declined after changing something\n", n, op, modrm);
                return 1;
            }
        }

        /* as an instruction */
        fpu = start;
        memcpy(cpu_mem + cpu.sg[DS].base + mem_off - 8, before, sizeof(before));
        AX = ax;
        cpu.ip = 0x100;
        cpu.halted = 0;
        cpu.exc_pending = 0;
        cpu_run(1);
        snapshot(&a);

        /* and by the general code */
        cpu.sg[CS].base = 0x10000;
        cpu.seg[CS] = 0x1000;
        fpu = start;
        memcpy(cpu_mem + cpu.sg[DS].base + mem_off - 8, before, sizeof(before));
        AX = ax;
        cpu.insn_ip = 0x100;
        general(op, modrm, sg, mem_off, wide);
        snapshot(&b);

        seen[op - 0xD8]++;
        if (was_fast) {
            fast++;
            taken[op - 0xD8]++;
        }
        if (memcmp(&a, &b, sizeof(a))) {
            printf("case %ld: op %02X %02X (%s) cw %04X sw %04X top %u empty %02X differs:\n", n, op, modrm,
                   was_fast ? "fast" : "general", start.cw, start.sw, start.top, start.empty);
            for (int i = 0; i < 8; i++) {
                printf("  st[%d] %04X %016llX -> fast %04X %016llX, general %04X %016llX\n", i, start.st[i].se,
                       (unsigned long long)start.st[i].m, a.st[i].se, (unsigned long long)a.st[i].m,
                       b.st[i].se, (unsigned long long)b.st[i].m);
            }
            printf("  sw %04X / %04X, top %u / %u, empty %02X / %02X, ax %04X / %04X, fop %03X / %03X, fip %X / %X, "
                   "fdp %X / %X, fcs %X / %X, fds %X / %X\n  operand", a.sw, b.sw, a.top, b.top, a.empty, b.empty, a.ax,
                   b.ax, a.fop, b.fop, (unsigned)a.fip, (unsigned)b.fip, (unsigned)a.fdp, (unsigned)b.fdp, a.fcs, b.fcs,
                   a.fds, b.fds);
            for (int i = 0; i < 16; i++) {
                printf(" %02X", operand[i]);
            }
            printf("\n  memory");
            for (int i = 0; i < 24; i++) {
                printf(" %02X/%02X", a.mem[i], b.mem[i]);
            }
            printf("\n");
            return 1;
        }
    }
    printf("%ld instructions, %ld on the fast path, all the same as the general code\n  taken, by opcode:", cases,
           fast);
    for (int i = 0; i < 8; i++) {
        printf(" %02X %ld%%", 0xD8 + i, seen[i] ? taken[i] * 100 / seen[i] : 0);
    }
    printf("\n");
    return 0;
}
