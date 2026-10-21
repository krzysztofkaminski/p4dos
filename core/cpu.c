/*
 * x86 CPU interpreter: 8086, 80186 and 80386DX. See cpu.h.
 *
 * This file has the state, the arithmetic, memory access and the interpreter loop.
 * cpu_sys.h has the protected-mode machinery of the 80386, cpu_ops.h and cpu_stk.h the
 * instructions that exist in a 16-bit and a 32-bit form.
 */
#include "cpu.h"

#include <stddef.h>
#include <string.h>

cpu_t cpu;
uint8_t *cpu_mem;
uint32_t cpu_ram_size = CPU_MEM_SIZE;
/* Limits of directly accessible memory within the first megabyte (cpu_set_mem_limits). */
static uint32_t ram_rd = CPU_MEM_SIZE;
static uint32_t ram_wr = 0xC0000;

void (*cpu_exc_trace)(int vector, uint32_t code);

static uint8_t parity_tab[256];

#define ES SEG_ES
#define CS SEG_CS
#define SS SEG_SS
#define DS SEG_DS
#define FS SEG_FS
#define GS SEG_GS

#define EAX cpu.r.d[REG_AX]
#define ECX cpu.r.d[REG_CX]
#define EDX cpu.r.d[REG_DX]
#define EBX cpu.r.d[REG_BX]
#define ESP cpu.r.d[REG_SP]
#define EBP cpu.r.d[REG_BP]
#define ESI cpu.r.d[REG_SI]
#define EDI cpu.r.d[REG_DI]
#define AX cpu.r.w[REG_AX * 2]
#define CX cpu.r.w[REG_CX * 2]
#define DX cpu.r.w[REG_DX * 2]
#define BX cpu.r.w[REG_BX * 2]
#define SP cpu.r.w[REG_SP * 2]
#define BP cpu.r.w[REG_BP * 2]
#define SI cpu.r.w[REG_SI * 2]
#define DI cpu.r.w[REG_DI * 2]
#define AL cpu.r.b[0]
#define AH cpu.r.b[1]
#define CL cpu.r.b[4]

#define R8(i) cpu.r.b[cpu.reg8_idx[i]]
#define R16(i) cpu.r.w[(i) * 2]
#define R32(i) cpu.r.d[i]
#define SEG(s) (&cpu.sg[s])

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define NOINLINE __attribute__((noinline))
#define COLD __attribute__((noinline, cold))

#define IS386 (cpu.model >= CPU_80386)

/* --- flags ---------------------------------------------------------------- */

#define GET_CF() ((cpu.f_co >> 8) & 1)
#define GET_OF() (((cpu.f_co >> 7) ^ (cpu.f_co >> 8)) & 1)
#define GET_AF() ((cpu.f_ax >> 4) & 1)
#define GET_ZF() (cpu.f_zr == 0)
#define GET_SF() (cpu.f_sp >> 31)
#define GET_PF() (parity_tab[cpu.f_sp & 0xFF])

#define SETZSP8(r) do { cpu.f_zr = cpu.f_sp = (uint32_t)(int32_t)(int8_t)(r); } while (0)
#define SETZSP16(r) do { cpu.f_zr = cpu.f_sp = (uint32_t)(int32_t)(int16_t)(r); } while (0)
#define SETZSP32(r) do { cpu.f_zr = cpu.f_sp = (uint32_t)(r); } while (0)

static inline void set_cf_of(uint32_t cf, uint32_t of)
{
    cpu.f_co = (cf << 8) | ((cf ^ of) << 7);
}
static inline void set_cf(uint32_t cf) { set_cf_of(cf, GET_OF()); }
static inline void set_af(uint32_t af) { cpu.f_ax = af << 4; }

static uint32_t get_eflags(void)
{
    uint32_t f = 2;
    f |= GET_CF();
    f |= GET_PF() << 2;
    f |= GET_AF() << 4;
    f |= GET_ZF() << 6;
    f |= GET_SF() << 7;
    f |= (uint32_t)cpu.f_tf << 8;
    f |= (uint32_t)cpu.f_if << 9;
    f |= (uint32_t)cpu.f_df << 10;
    f |= GET_OF() << 11;
    if (IS386) {
        f |= (uint32_t)cpu.f_iopl << 12 | (uint32_t)cpu.f_nt << 14 | (uint32_t)cpu.f_rf << 16 |
             (uint32_t)cpu.f_vm << 17;
    } else {
        f |= 0xF000;
    }
    return f;
}

static void update_mode(void);

/* Set the EFLAGS bits selected by mask. */
static void set_eflags(uint32_t f, uint32_t mask)
{
    f = (get_eflags() & ~mask) | (f & mask);
    set_cf_of(f & 1, (f >> 11) & 1);
    cpu.f_ax = f & FL_AF;
    cpu.f_zr = !(f & FL_ZF);
    cpu.f_sp = ((f & FL_SF) ? 0x80000000u : 0) | ((f & FL_PF) ? 0 : 1);
    cpu.f_tf = (f >> 8) & 1;
    cpu.f_if = (f >> 9) & 1;
    cpu.f_df = (f >> 10) & 1;
    if (IS386) {
        cpu.f_iopl = (f >> 12) & 3;
        cpu.f_nt = (f >> 14) & 1;
        cpu.f_rf = (f >> 16) & 1;
        if (cpu.f_vm != ((f >> 17) & 1)) {
            cpu.f_vm = (f >> 17) & 1;
            update_mode();
        }
    }
}

uint16_t cpu_get_flags(void) { return (uint16_t)get_eflags(); }
void cpu_set_flags(uint16_t f) { set_eflags(f, 0xFFFF); }
uint32_t cpu_get_eflags(void) { return get_eflags(); }
void cpu_set_eflags(uint32_t f) { set_eflags(f, 0xFFFFFFFFu); }

/* --- arithmetic ----------------------------------------------------------- */

#define ARITH8(name, expr) \
    static inline uint32_t name(uint32_t a, uint32_t b) \
    { \
        uint32_t r = (expr), x = a ^ b ^ r; \
        cpu.f_co = x; \
        cpu.f_ax = x; \
        SETZSP8(r); \
        return r & 0xFF; \
    }
#define ARITH16(name, expr) \
    static inline uint32_t name(uint32_t a, uint32_t b) \
    { \
        uint32_t r = (expr), x = a ^ b ^ r; \
        cpu.f_co = x >> 8; \
        cpu.f_ax = x; \
        SETZSP16(r); \
        return r & 0xFFFF; \
    }
#define LOGIC(name, op, SETZSP) \
    static inline uint32_t name(uint32_t a, uint32_t b) \
    { \
        uint32_t r = a op b; \
        cpu.f_co = 0; \
        cpu.f_ax = 0; \
        SETZSP(r); \
        return r; \
    }
ARITH8(f_add8, a + b)
ARITH8(f_adc8, a + b + GET_CF())
ARITH8(f_sub8, a - b)
ARITH8(f_sbb8, a - b - GET_CF())
LOGIC(f_or8, |, SETZSP8)
LOGIC(f_and8, &, SETZSP8)
LOGIC(f_xor8, ^, SETZSP8)
ARITH16(f_add16, a + b)
ARITH16(f_adc16, a + b + GET_CF())
ARITH16(f_sub16, a - b)
ARITH16(f_sbb16, a - b - GET_CF())
LOGIC(f_or16, |, SETZSP16)
LOGIC(f_and16, &, SETZSP16)
LOGIC(f_xor16, ^, SETZSP16)
LOGIC(f_or32, |, SETZSP32)
LOGIC(f_and32, &, SETZSP32)
LOGIC(f_xor32, ^, SETZSP32)

/* With 32 bits the carry out of the top bit has to be worked out separately; bit 31 of
 * a ^ b ^ r is the carry into the top bit, which goes where OF is derived from. */
static inline uint32_t flags32(uint32_t a, uint32_t b, uint32_t r, uint32_t cf)
{
    uint32_t x = a ^ b ^ r;
    cpu.f_co = (cf << 8) | ((x >> 24) & 0x80);
    cpu.f_ax = x;
    SETZSP32(r);
    return r;
}
static inline uint32_t f_add32(uint32_t a, uint32_t b)
{
    uint32_t r = a + b;
    return flags32(a, b, r, r < a);
}
static inline uint32_t f_adc32(uint32_t a, uint32_t b)
{
    uint64_t r = (uint64_t)a + b + GET_CF();
    return flags32(a, b, (uint32_t)r, (uint32_t)(r >> 32));
}
static inline uint32_t f_sub32(uint32_t a, uint32_t b)
{
    return flags32(a, b, a - b, a < b);
}
static inline uint32_t f_sbb32(uint32_t a, uint32_t b)
{
    uint64_t r = (uint64_t)a - b - GET_CF();
    return flags32(a, b, (uint32_t)r, (uint32_t)(r >> 32) & 1);
}

/* INC/DEC leave CF alone: keep bit 8 of f_co and rebuild bit 7 (= CF ^ OF). */
static inline void incdec_flags(uint32_t a, uint32_t r, uint32_t of)
{
    uint32_t c = cpu.f_co & 0x100;
    cpu.f_co = c | ((c >> 1) ^ (of << 7));
    cpu.f_ax = a ^ r;
}
static inline uint32_t f_inc8(uint32_t a)
{
    uint32_t r = (a + 1) & 0xFF;
    incdec_flags(a, r, r == 0x80);
    SETZSP8(r);
    return r;
}
static inline uint32_t f_dec8(uint32_t a)
{
    uint32_t r = (a - 1) & 0xFF;
    incdec_flags(a, r, r == 0x7F);
    SETZSP8(r);
    return r;
}
static inline uint32_t f_inc16(uint32_t a)
{
    uint32_t r = (a + 1) & 0xFFFF;
    incdec_flags(a, r, r == 0x8000);
    SETZSP16(r);
    return r;
}
static inline uint32_t f_dec16(uint32_t a)
{
    uint32_t r = (a - 1) & 0xFFFF;
    incdec_flags(a, r, r == 0x7FFF);
    SETZSP16(r);
    return r;
}
static inline uint32_t f_inc32(uint32_t a)
{
    uint32_t r = a + 1;
    incdec_flags(a, r, r == 0x80000000u);
    SETZSP32(r);
    return r;
}
static inline uint32_t f_dec32(uint32_t a)
{
    uint32_t r = a - 1;
    incdec_flags(a, r, r == 0x7FFFFFFFu);
    SETZSP32(r);
    return r;
}

/* Shift/rotate group. bits is 8, 16 or 32. */
static uint32_t do_shift(uint32_t op, uint32_t v, uint32_t cnt, uint32_t bits)
{
    const uint32_t top = bits - 1;
    const uint32_t mask = (2u << top) - 1;
    uint32_t r = v, cf, c;

    if (cpu.model >= CPU_80186) {
        cnt &= 0x1F;
    }
    if (cnt == 0) {
        return v;
    }
    switch (op) {
    case 0: /* ROL */
        c = cnt % bits;
        if (c) {
            r = ((v << c) | (v >> (bits - c))) & mask;
        }
        cf = r & 1;
        set_cf_of(cf, ((r >> top) ^ cf) & 1);
        return r;
    case 1: /* ROR */
        c = cnt % bits;
        if (c) {
            r = ((v >> c) | (v << (bits - c))) & mask;
        }
        set_cf_of((r >> top) & 1, ((r >> top) ^ (r >> (top - 1))) & 1);
        return r;
    case 2: /* RCL */
        cf = GET_CF();
        for (c = cnt % (bits + 1); c; c--) {
            uint32_t n = (r >> top) & 1;
            r = ((r << 1) | cf) & mask;
            cf = n;
        }
        set_cf_of(cf, ((r >> top) ^ cf) & 1);
        return r;
    case 3: /* RCR */
        cf = GET_CF();
        for (c = cnt % (bits + 1); c; c--) {
            uint32_t n = r & 1;
            r = (r >> 1) | (cf << top);
            cf = n;
        }
        set_cf_of(cf, ((r >> top) ^ (r >> (top - 1))) & 1);
        return r;
    case 6:
        if (cpu.model == CPU_8086) { /* SETMO: undocumented, result is all ones */
            r = mask;
            cpu.f_co = 0;
            cpu.f_ax = 0;
            break;
        }
        __attribute__((fallthrough)); /* SAL is SHL */
    case 4: /* SHL */
        if (cnt > bits) {
            r = 0;
            /* undefined; the 80386 shows the last bit out whenever the count is a
             * multiple of the operand size */
            cf = (IS386 && cnt % bits == 0) ? v & 1 : 0;
        } else {
            cf = (v >> (bits - cnt)) & 1;
            r = cnt == 32 ? 0 : (v << cnt) & mask;
        }
        set_cf_of(cf, ((r >> top) ^ cf) & 1);
        /* undefined: on the 8088 AF follows bit 4 of the result, the 80386 sets it */
        cpu.f_ax = IS386 ? 0x10 : r;
        break;
    case 5: /* SHR */
        if (cnt > bits) {
            r = 0;
            cf = (IS386 && cnt % bits == 0) ? (v >> top) & 1 : 0;
        } else {
            cf = (v >> (cnt - 1)) & 1;
            r = cnt == 32 ? 0 : v >> cnt;
        }
        set_cf_of(cf, cnt == 1 ? (v >> top) & 1 : 0);
        cpu.f_ax = IS386 ? 0x10 : 0;
        break;
    default: /* SAR */ {
        int32_t sv = bits == 8 ? (int32_t)(int8_t)v : bits == 16 ? (int32_t)(int16_t)v : (int32_t)v;
        if (cnt >= bits) {
            cf = sv < 0;
            r = (sv < 0) ? mask : 0;
        } else {
            cf = ((uint32_t)sv >> (cnt - 1)) & 1;
            r = (uint32_t)(sv >> cnt) & mask;
        }
        set_cf_of(cf, 0);
        cpu.f_ax = 0;
        break;
    }
    }
    if (bits == 8) {
        SETZSP8(r);
    } else if (bits == 16) {
        SETZSP16(r);
    } else {
        SETZSP32(r);
    }
    return r;
}

/* What programs shift nearly all the time, done in line: SHL, SHR and SAR by 1 to the
 * operand size less one, as do_shift() does them. */
static inline uint32_t shift(uint32_t op, uint32_t v, uint32_t cnt, uint32_t bits)
{
    const uint32_t c = cpu.model >= CPU_80186 ? cnt & 0x1F : cnt;
    if (likely(c - 1 < bits - 1 && (op == 4 || op == 5 || op == 7))) {
        const uint32_t top = bits - 1, mask = (2u << top) - 1;
        uint32_t r, cf;
        if (op == 4) {
            cf = (v >> (bits - c)) & 1;
            r = (v << c) & mask;
            set_cf_of(cf, ((r >> top) ^ cf) & 1);
            cpu.f_ax = IS386 ? 0x10 : r;
        } else if (op == 5) {
            cf = (v >> (c - 1)) & 1;
            r = v >> c;
            set_cf_of(cf, c == 1 ? (v >> top) & 1 : 0);
            cpu.f_ax = IS386 ? 0x10 : 0;
        } else {
            const int32_t sv = (int32_t)(v << (32 - bits)) >> (32 - bits);
            cf = ((uint32_t)sv >> (c - 1)) & 1;
            r = (uint32_t)(sv >> c) & mask;
            set_cf_of(cf, 0);
            cpu.f_ax = 0;
        }
        cpu.f_zr = cpu.f_sp = (uint32_t)((int32_t)(r << (32 - bits)) >> (32 - bits));
        return r;
    }
    return do_shift(op, v, cnt, bits);
}

/* SHLD/SHRD: shift v by cnt, filling from fill. bits is 16 or 32. With 16 bits, counts
 * above 16 are undefined; this does what the 80386 does. */
static uint32_t do_shiftd(int right, uint32_t v, uint32_t fill, uint32_t cnt, uint32_t bits)
{
    uint32_t r, cf, of;
    cnt &= 0x1F;
    if (cnt == 0) {
        return v;
    }
    if (bits == 16) {
        /* Past 16 places the 80386 goes on with the filling operand alone. */
        if (right) {
            const uint32_t wide = cnt > 16 ? (fill << 16) | fill : (fill << 16) | v;
            const uint32_t c = cnt > 16 ? cnt - 16 : cnt;
            r = (wide >> c) & 0xFFFF;
            cf = (wide >> (c - 1)) & 1;
            of = ((r << 1) ^ r) >> 15;
        } else {
            const uint32_t wide = cnt > 16 ? (fill << 16) | fill : (v << 16) | fill;
            const uint32_t c = cnt > 16 ? cnt - 16 : cnt;
            r = ((wide << c) >> 16) & 0xFFFF;
            cf = (wide >> (32 - c)) & 1;
            of = cf ^ (r >> 15);
        }
        SETZSP16(r);
    } else {
        if (right) {
            r = (v >> cnt) | (fill << (32 - cnt));
            cf = (v >> (cnt - 1)) & 1;
            of = ((r << 1) ^ r) >> 31;
        } else {
            r = (v << cnt) | (fill >> (32 - cnt));
            cf = (v >> (32 - cnt)) & 1;
            of = cf ^ (r >> 31);
        }
        SETZSP32(r);
    }
    set_cf_of(cf, of & 1);
    cpu.f_ax = 0x10; /* undefined; the 80386 sets AF */
    return r;
}

/* Flags of the bit test instructions: CF is the bit. OF is undefined; the 80386 leaves
 * there what a rotate right through carry by n + 1 would, which is how it gets at the bit. */
static inline void bt_flags(uint32_t v, uint32_t n, uint32_t bits)
{
    const uint32_t top = n >= 1 ? (v >> (n - 1)) & 1 : 0;
    const uint32_t next = n >= 2 ? (v >> (n - 2)) & 1 : n == 1 ? 0 : (v >> (bits - 1)) & 1;
    set_cf_of((v >> n) & 1, top ^ next);
}

/* --- exceptions (80386) ------------------------------------------------------ */

enum {
    EXC_DE = 0, EXC_DB = 1, EXC_BP = 3, EXC_OF = 4, EXC_BR = 5, EXC_UD = 6, EXC_NM = 7,
    EXC_DF = 8, EXC_TS = 10, EXC_NP = 11, EXC_SS = 12, EXC_GP = 13, EXC_PF = 14,
};

/* Abandon the instruction being executed: the run loop restarts from its beginning by
 * entering the handler of the exception. */
static void __attribute__((noreturn)) COLD raise_exc(uint32_t vector, int has_code, uint32_t code)
{
    cpu.exc_vector = (uint8_t)vector;
    cpu.exc_has_code = (uint8_t)has_code;
    cpu.exc_code = code;
    cpu.exc_pending = 1;
    longjmp(cpu.exc_jmp, 1);
}
#define RAISE(v) raise_exc(v, 0, 0)
#define RAISE_C(v, c) raise_exc(v, 1, c)

/* --- memory --------------------------------------------------------------- */

static inline uint32_t ld16(const uint8_t *p)
{
    uint16_t v;
    __builtin_memcpy(&v, p, 2);
    return v;
}
static inline uint32_t ld32(const uint8_t *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline void st16(uint8_t *p, uint32_t v)
{
    uint16_t w = (uint16_t)v;
    __builtin_memcpy(p, &w, 2);
}
static inline void st32(uint8_t *p, uint32_t v)
{
    __builtin_memcpy(p, &v, 4);
}

/* Physical memory. The A20 mask is 0FFFFFh for the 8086 and 80186: their 20 address lines. */
static inline int ram_r(uint32_t p)
{
    return p < ram_rd || p - CPU_MEM_SIZE < cpu_ram_size - CPU_MEM_SIZE;
}
static inline int ram_w(uint32_t p)
{
    return p < ram_wr || p - CPU_MEM_SIZE < cpu_ram_size - CPU_MEM_SIZE;
}
static uint32_t phys_r8(uint32_t p)
{
    p &= cpu.a20_mask;
    if (likely(ram_r(p))) {
        return cpu_mem[p];
    }
    return p < CPU_MEM_SIZE ? pc_mem_read(p) : 0xFF;
}
static void phys_w8(uint32_t p, uint32_t v)
{
    p &= cpu.a20_mask;
    if (likely(ram_w(p))) {
        cpu_mem[p] = (uint8_t)v;
    } else if (p < CPU_MEM_SIZE) {
        pc_mem_write(p, (uint8_t)v);
    }
}
/* n bytes at p are RAM in one piece. */
static inline int ram_span_r(uint32_t p, uint32_t n)
{
    return p + n <= ram_rd || (p >= CPU_MEM_SIZE && p <= cpu_ram_size - n);
}
static inline int ram_span_w(uint32_t p, uint32_t n)
{
    return p + n <= ram_wr || (p >= CPU_MEM_SIZE && p <= cpu_ram_size - n);
}
static uint32_t phys_r16(uint32_t p)
{
    uint32_t q = p & cpu.a20_mask;
    if (likely(ram_span_r(q, 2))) {
        return ld16(cpu_mem + q);
    }
    return phys_r8(p) | (phys_r8(p + 1) << 8);
}
static uint32_t phys_r32(uint32_t p)
{
    uint32_t q = p & cpu.a20_mask;
    if (likely(ram_span_r(q, 4))) {
        return ld32(cpu_mem + q);
    }
    return phys_r16(p) | (phys_r16(p + 2) << 16);
}
static void phys_w16(uint32_t p, uint32_t v)
{
    uint32_t q = p & cpu.a20_mask;
    if (likely(ram_span_w(q, 2))) {
        st16(cpu_mem + q, v);
    } else {
        phys_w8(p, v);
        phys_w8(p + 1, v >> 8);
    }
}
static void phys_w32(uint32_t p, uint32_t v)
{
    uint32_t q = p & cpu.a20_mask;
    if (likely(ram_span_w(q, 4))) {
        st32(cpu_mem + q, v);
    } else {
        phys_w16(p, v);
        phys_w16(p + 2, v >> 16);
    }
}

/* Paging: a small direct-mapped cache of translations. A tag is the linear page number
 * with, in its low bits, the kinds of access the entry is good for (bit number: 0 read,
 * 1 write, plus 2 for user mode). Writes are only cached once the page is marked dirty. */
#define TLB_N 256
static struct {
    uint32_t tag, phys;
} tlb[TLB_N];

static void tlb_flush(void)
{
    memset(tlb, 0, sizeof(tlb));
    cpu.lim1 = 0;
}

static NOINLINE uint32_t page_walk(uint32_t lin, uint32_t kind)
{
    const uint32_t write = kind & 1, user = kind >> 1;
    uint32_t pde_a = (cpu.cr3 & ~0xFFFu) + ((lin >> 22) << 2);
    uint32_t pde = phys_r32(pde_a), pte_a, pte = 0, both, perm;
    if (!(pde & 1)) {
        goto fault;
    }
    pte_a = (pde & ~0xFFFu) + (((lin >> 12) & 0x3FF) << 2);
    pte = phys_r32(pte_a);
    if (!(pte & 1)) {
        goto fault;
    }
    both = pde & pte;
    /* The 80386 lets supervisor code write to any present page. */
    if (user && (!(both & 4) || (write && !(both & 2)))) {
        pte = 1;
        pde = 1;
        goto fault;
    }
    if (!(pde & 0x20)) {
        phys_w32(pde_a, pde | 0x20);
    }
    if ((pte & 0x20) == 0 || (write && !(pte & 0x40))) {
        pte |= 0x20 | (write ? 0x40 : 0);
        phys_w32(pte_a, pte);
    }
    perm = 1 | ((both & 4) ? 4 : 0);
    if (pte & 0x40) {
        perm |= 2 | ((both & 6) == 6 ? 8 : 0);
    }
    tlb[(lin >> 12) & (TLB_N - 1)].tag = (lin & ~0xFFFu) | perm;
    tlb[(lin >> 12) & (TLB_N - 1)].phys = pte & ~0xFFFu;
    return pte & ~0xFFFu;
fault:
    cpu.cr2 = lin;
    RAISE_C(EXC_PF, (pde & pte & 1) | (write << 1) | (user << 2));
}

/* Linear to physical address. kind: 0 read, 1 write, plus 2 for an access by user code. */
static inline uint32_t lin_phys(uint32_t lin, uint32_t kind)
{
    if (!(cpu.cr0 & CR0_PG)) {
        return lin;
    }
    uint32_t tag = tlb[(lin >> 12) & (TLB_N - 1)].tag;
    if (likely(((tag ^ lin) & ~0xFFFu) == 0 && ((tag >> kind) & 1))) {
        return tlb[(lin >> 12) & (TLB_N - 1)].phys | (lin & 0xFFF);
    }
    return page_walk(lin, kind) | (lin & 0xFFF);
}

#define USER() (cpu.cpl == 3 ? 2u : 0u)

static uint32_t lin_r8(uint32_t lin, uint32_t user)
{
    return phys_r8(lin_phys(lin, user));
}
static uint32_t lin_r16(uint32_t lin, uint32_t user)
{
    if (likely((lin & 0xFFF) != 0xFFF)) {
        return phys_r16(lin_phys(lin, user));
    }
    uint32_t lo = lin_r8(lin, user);
    return lo | (lin_r8(lin + 1, user) << 8);
}
static uint32_t lin_r32(uint32_t lin, uint32_t user)
{
    if (likely((lin & 0xFFF) <= 0xFFC)) {
        return phys_r32(lin_phys(lin, user));
    }
    uint32_t lo = lin_r16(lin, user);
    return lo | (lin_r16(lin + 2, user) << 16);
}
static void lin_w8(uint32_t lin, uint32_t v, uint32_t user)
{
    phys_w8(lin_phys(lin, user | 1), v);
}
static void lin_w16(uint32_t lin, uint32_t v, uint32_t user)
{
    if (likely((lin & 0xFFF) != 0xFFF)) {
        phys_w16(lin_phys(lin, user | 1), v);
    } else {
        /* Both pages must be there before anything is written. */
        uint32_t a = lin_phys(lin, user | 1), b = lin_phys(lin + 1, user | 1);
        phys_w8(a, v);
        phys_w8(b, v >> 8);
    }
}
static void lin_w32(uint32_t lin, uint32_t v, uint32_t user)
{
    if (likely((lin & 0xFFF) <= 0xFFC)) {
        phys_w32(lin_phys(lin, user | 1), v);
    } else {
        (void)lin_phys(lin, user | 1);
        (void)lin_phys(lin + 3, user | 1);
        for (int i = 0; i < 4; i++) {
            lin_w8(lin + (uint32_t)i, v >> (8 * i), user);
        }
    }
}
/* Tables of the processor itself (descriptors, task state) are read with supervisor rights. */
#define sys_r8(a) lin_r8(a, 0)
#define sys_r16(a) lin_r16(a, 0)
#define sys_r32(a) lin_r32(a, 0)
#define sys_w8(a, v) lin_w8(a, v, 0)
#define sys_w16(a, v) lin_w16(a, v, 0)
#define sys_w32(a, v) lin_w32(a, v, 0)

uint8_t *cpu_lin_ptr(uint32_t lin, uint32_t n, int write)
{
    uint32_t p = lin;
    if (n == 0) {
        n = 1;
    }
    if (cpu.cr0 & CR0_PG) {
        /* Walk the tables without raising faults or touching the accessed bits. */
        uint32_t pde = phys_r32((cpu.cr3 & ~0xFFFu) + ((lin >> 22) << 2)), pte;
        if (!(pde & 1) || (lin & 0xFFF) + n > 0x1000) {
            return NULL;
        }
        pte = phys_r32((pde & ~0xFFFu) + (((lin >> 12) & 0x3FF) << 2));
        if (!(pte & 1)) {
            return NULL;
        }
        p = (pte & ~0xFFFu) | (lin & 0xFFF);
    }
    p &= cpu.a20_mask;
    if (p + n < p || !(write ? ram_span_w(p, n) : ram_span_r(p, n))) {
        return NULL;
    }
    return cpu_mem + p;
}

/* Linear address of an n-byte access to a segment, after the checks the 80386 makes. */
static NOINLINE uint32_t seg_lin(const cpu_seg_t *sg, uint32_t off, uint32_t n, uint32_t write)
{
    const uint32_t last = off + n - 1;
    int bad;
    if (!(sg->flags & (write ? CPU_SF_W : CPU_SF_R))) {
        bad = 1;
    } else if (sg->flags & CPU_SF_DOWN) {
        bad = off <= sg->limit || last < off || (!(sg->flags & CPU_SF_BIG) && last > 0xFFFF);
    } else {
        bad = last < off || last > sg->limit;
    }
    if (unlikely(bad)) {
        RAISE_C(sg == SEG(SS) ? EXC_SS : EXC_GP, 0);
    }
    return sg->base + off;
}

/* The accesses the window of a segment does not cover. On the 8086 and 80186 offsets
 * wrap around at 64 KiB and addresses at 1 MiB, and nothing faults. */
static COLD uint32_t slow_r8(const cpu_seg_t *sg, uint32_t off)
{
    if (!IS386) {
        return phys_r8(sg->base + off);
    }
    return lin_r8(seg_lin(sg, off, 1, 0), USER());
}
static COLD uint32_t slow_r16(const cpu_seg_t *sg, uint32_t off)
{
    if (!IS386) {
        return phys_r8(sg->base + off) | (phys_r8(sg->base + ((off + 1) & 0xFFFF)) << 8);
    }
    return lin_r16(seg_lin(sg, off, 2, 0), USER());
}
static COLD uint32_t slow_r32(const cpu_seg_t *sg, uint32_t off)
{
    return lin_r32(seg_lin(sg, off, 4, 0), USER());
}
static COLD void slow_w8(const cpu_seg_t *sg, uint32_t off, uint32_t v)
{
    if (!IS386) {
        phys_w8(sg->base + off, v);
    } else {
        lin_w8(seg_lin(sg, off, 1, 1), v, USER());
    }
}
static COLD void slow_w16(const cpu_seg_t *sg, uint32_t off, uint32_t v)
{
    if (!IS386) {
        phys_w8(sg->base + off, v);
        phys_w8(sg->base + ((off + 1) & 0xFFFF), v >> 8);
    } else {
        lin_w16(seg_lin(sg, off, 2, 1), v, USER());
    }
}
static COLD void slow_w32(const cpu_seg_t *sg, uint32_t off, uint32_t v)
{
    lin_w32(seg_lin(sg, off, 4, 1), v, USER());
}
/* A read that a write to the same place will follow: the write must not be what faults,
 * the instruction has changed flags by then. */
static COLD uint32_t slow_rw(const cpu_seg_t *sg, uint32_t off, uint32_t n)
{
    if (IS386) {
        uint32_t lin = seg_lin(sg, off, n, 1);
        (void)lin_phys(lin, USER() | 1);
        (void)lin_phys(lin + n - 1, USER() | 1);
    }
    return n == 1 ? slow_r8(sg, off) : n == 2 ? slow_r16(sg, off) : slow_r32(sg, off);
}

/* Segment-relative access for everything outside the interpreter loop (which has its own
 * versions of these, as macros). The rare case is written first because the interpreter
 * is compiled without block reordering: this way the usual case runs on past the access
 * with one taken branch and no jump. */
#define SEG_RD(n, i, slow, load) \
    static inline uint32_t sr##n(const cpu_seg_t *sg, uint32_t off) \
    { \
        if (unlikely(off - sg->lo >= sg->rd[i])) { \
            return slow(sg, off); \
        } \
        return load; \
    }
#define SEG_WR(n, i, slow, store) \
    static inline void sw##n(const cpu_seg_t *sg, uint32_t off, uint32_t v) \
    { \
        if (unlikely(off - sg->lo >= sg->wr[i])) { \
            slow(sg, off, v); \
        } else { \
            store; \
        } \
    }
SEG_RD(8, 0, slow_r8, sg->mem[off])
SEG_RD(16, 1, slow_r16, ld16(sg->mem + off))
SEG_RD(32, 2, slow_r32, ld32(sg->mem + off))
SEG_WR(8, 0, slow_w8, sg->mem[off] = (uint8_t)v)
SEG_WR(16, 1, slow_w16, st16(sg->mem + off, v))
SEG_WR(32, 2, slow_w32, st32(sg->mem + off, v))

/* Work out the window of a segment register (see cpu_seg_t). */
static void seg_window(cpu_seg_t *sg)
{
    const uint32_t b = sg->base;
    uint64_t end = (uint64_t)b + (IS386 ? (uint64_t)sg->limit + 1 : 0x10000);
    uint32_t top = (IS386 && cpu.a20) ? cpu_ram_size : CPU_MEM_SIZE;
    uint32_t rd = 0, wr = 0, lo = 0;

    if (!(cpu.cr0 & CR0_PG) && !(sg->flags & CPU_SF_DOWN)) {
        if (b >= CPU_MEM_SIZE) {
            if (b < top) {
                rd = wr = (uint32_t)(end < top ? end : top) - b;
            }
        } else if (end > CPU_MEM_SIZE && top > CPU_MEM_SIZE && ((cpu.dmode & 1) || b >= ram_wr)) {
            /* The segment reaches into extended memory. 32-bit code lives there, and so
             * does what is behind a segment that starts among the ROMs (FFFFh: the high
             * memory area): the first megabyte is left to the slow path. 16-bit code
             * with such limits (they stay after a visit to protected mode) still works
             * below 1 MB. */
            lo = CPU_MEM_SIZE - b;
            rd = wr = (uint32_t)(end < top ? end : top) - CPU_MEM_SIZE;
        } else {
            if (b < ram_rd) {
                rd = (uint32_t)(end < ram_rd ? end : ram_rd) - b;
            }
            if (b < ram_wr) {
                wr = (uint32_t)(end < ram_wr ? end : ram_wr) - b;
            }
        }
    }
    if (!(sg->flags & CPU_SF_R)) {
        rd = 0;
    }
    if (!(sg->flags & CPU_SF_W)) {
        wr = 0;
    }
    sg->lo = lo;
    sg->mem = cpu_mem + b;
    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t n = (1u << i) - 1;
        sg->rd[i] = rd > n ? rd - n : 0;
        sg->wr[i] = wr > n ? wr - n : 0;
    }
}

static void all_windows(void)
{
    for (int s = 0; s < 6; s++) {
        seg_window(&cpu.sg[s]);
    }
    cpu.lim1 = 0;
}

void cpu_set_mem_limits(uint32_t rd, uint32_t wr)
{
    ram_rd = rd;
    ram_wr = wr;
    all_windows();
}

void cpu_set_a20(int on)
{
    if (!IS386 || cpu.a20 == !!on) {
        return;
    }
    cpu.a20 = !!on;
    cpu.a20_mask = on ? 0xFFFFFFFFu : 0xFFEFFFFFu;
    tlb_flush();
    all_windows();
}

/* Real mode and virtual-8086 mode: base = selector * 16. The segment becomes an ordinary
 * readable and writable one. In real mode the limit stays as it is (what was loaded in
 * protected mode remains in force), except that of CS, which the 80386 resets with every
 * load. */
static void seg_load_real(int s, uint32_t sel)
{
    cpu_seg_t *sg = &cpu.sg[s];
    cpu.seg[s] = (uint16_t)sel;
    sg->base = (sel & 0xFFFF) << 4;
    sg->flags = (uint8_t)((sg->flags & CPU_SF_BIG) | CPU_SF_R | CPU_SF_W);
    sg->ar = cpu.f_vm ? 0xF3 : 0x93;
    if (cpu.f_vm || s == CS) {
        sg->limit = 0xFFFF;
        sg->flags &= (uint8_t)~CPU_SF_BIG;
        if (s == CS && (cpu.dmode & 1)) {
            cpu.dmode &= (uint8_t)~1u;
            all_windows(); /* they depend on the code size */
        } else if (s == SS) {
            cpu.dmode &= (uint8_t)~2u;
        }
    }
    seg_window(sg);
    if (s == CS) {
        cpu.lim1 = 0;
    }
}

void cpu_set_seg(int s, uint16_t v) { seg_load_real(s, v); }

void cpu_break(void)
{
    cpu.saved += cpu.slice;
    cpu.slice = 0;
}

#include "cpu_sys.h"
#include "cpu_fpu.h"

/* --- effective addresses ------------------------------------------------------ */

/* How to form the effective address of a memory operand, for 16-bit and for 32-bit
 * addressing. */
static struct ea {
    /* The operand's segment: [0] DS and [1] SS, or the override of the instruction being
     * executed. */
    const cpu_seg_t *seg[2];
    uint32_t mask; /* of an offset: FFFFh or FFFFFFFFh */
    uint32_t a32;
    /* Per ModRM byte: the byte offsets in the CPU state of two registers to add (one or
     * both may be the always-zero one), whether the stack segment is the default, and what
     * else there is to it. */
    uint32_t tab[192];
} ea_mode[2];
#define D_OFF(n) ((uint32_t)(offsetof(cpu_t, r.d) + 4 * (n)))
#define EA_SS 0x00010000u
#define EA_DISP8 0x00020000u
#define EA_DISP16 0x00040000u
#define EA_DISP32 0x00080000u
#define EA_SIB 0x00100000u
#define EA_MORE 0x80000000u /* any of the above but EA_SS */

static void init_tables(void)
{
    static const uint8_t regs[8][2] = {
        {REG_BX, REG_SI}, {REG_BX, REG_DI}, {REG_BP, REG_SI}, {REG_BP, REG_DI},
        {REG_SI, 8},      {REG_DI, 8},      {REG_BP, 8},      {REG_BX, 8},
    };
    static const uint8_t r8[8] = {0, 4, 8, 12, 1, 5, 9, 13};
    for (uint32_t m = 0; m < 192; m++) {
        const uint32_t mod = m >> 6, rm = m & 7;
        uint32_t r1 = regs[rm][0], f;
        if (mod == 0 && rm == 6) {
            r1 = 8; /* a 16-bit address and nothing else */
            f = EA_DISP16;
        } else {
            f = (r1 == REG_BP ? EA_SS : 0) | (mod == 1 ? EA_DISP8 : mod == 2 ? EA_DISP16 : 0);
        }
        ea_mode[0].tab[m] = D_OFF(r1) | (D_OFF(regs[rm][1]) << 8) | f | ((f & ~EA_SS) ? EA_MORE : 0);

        r1 = rm;
        f = mod == 1 ? EA_DISP8 : mod == 2 ? EA_DISP32 : 0;
        if (rm == 4) {
            r1 = 8;
            f = EA_SIB; /* whatever follows the SIB byte is ea_sib()'s business */
        } else if (mod == 0 && rm == 5) {
            r1 = 8;
            f = EA_DISP32;
        } else if (rm == REG_BP) {
            f |= EA_SS;
        }
        ea_mode[1].tab[m] = D_OFF(r1) | (D_OFF(8) << 8) | f | ((f & ~EA_SS) ? EA_MORE : 0);
    }
    memcpy(cpu.reg8_idx, r8, sizeof(r8));
    for (int i = 0; i < 2; i++) {
        ea_mode[i].seg[0] = SEG(DS);
        ea_mode[i].seg[1] = SEG(SS);
        ea_mode[i].mask = i ? 0xFFFFFFFFu : 0xFFFFu;
        ea_mode[i].a32 = (uint32_t)i;
    }
}

/* A 32-bit address with a SIB byte, which is at p: base, index and displacement. info: the
 * number of bytes used, and EA_SS if the stack segment is the default. */
typedef struct {
    uint32_t off, info;
} sib_t;
static NOINLINE sib_t ea_sib(const uint8_t *p, uint32_t modrm)
{
    const uint32_t sib = p[0], base = sib & 7, idx = (sib >> 3) & 7, scale = sib >> 6;
    sib_t r = {0, 1};
    if (base == 5 && modrm < 0x40) {
        r.off = ld32(p + 1);
        r.info = 5;
    } else {
        r.off = R32(base);
        if (base == REG_SP || base == REG_BP) {
            r.info |= EA_SS;
        }
        if (idx == 4) {
            r.off <<= scale; /* undefined; this is what the 80386 does */
        }
        if (modrm >= 0x80) {
            r.off += ld32(p + 1);
            r.info += 4;
        } else if (modrm >= 0x40) {
            r.off += (uint32_t)(int8_t)p[1];
            r.info += 1;
        }
    }
    if (idx != 4) {
        r.off += R32(idx) << scale;
    }
    return r;
}

/* --- where instructions come from ----------------------------------------------- */

/* Length of the instruction in b[0..n-1], or more than n if it does not end there. */
enum { A_M = 1, A_I8 = 2, A_IV = 4, A_I16 = 8, A_AP = 16, A_OV = 32, A_GRP3 = 64 };
static uint32_t insn_attr(uint32_t op)
{
    static const uint8_t alu[8] = {A_M, A_M, A_M, A_M, A_I8, A_IV, 0, 0};
    static const uint8_t r6[16] = {0, 0, A_M, A_M, 0, 0, 0, 0, A_IV, A_M | A_IV, A_I8, A_M | A_I8};
    static const uint8_t rc[16] = {A_M | A_I8, A_M | A_I8, A_I16, 0, A_M, A_M, A_M | A_I8,
                                   A_M | A_IV, A_I16 | A_I8, 0, A_I16, 0, 0, A_I8, 0, 0};
    static const uint8_t re[8] = {A_IV, A_IV, A_AP, A_I8, 0, 0, 0, 0};
    if (op < 0x40) return alu[op & 7];
    if (op < 0x60) return 0;
    if (op < 0x70) return r6[op & 15];
    if (op < 0x80) return A_I8;
    if (op < 0x90) return op == 0x81 ? A_M | A_IV : op < 0x84 ? A_M | A_I8 : A_M;
    if (op < 0xA0) return op == 0x9A ? A_AP : 0;
    if (op < 0xB0) return op < 0xA4 ? A_OV : op == 0xA8 ? A_I8 : op == 0xA9 ? A_IV : 0;
    if (op < 0xB8) return A_I8;
    if (op < 0xC0) return A_IV;
    if (op < 0xD0) return rc[op & 15];
    if (op < 0xD4) return A_M;
    if (op < 0xD8) return op < 0xD6 ? A_I8 : 0;
    if (op < 0xE0) return A_M;
    if (op < 0xE8) return A_I8;
    if (op < 0xF0) return re[op & 7];
    if (op == 0xF6 || op == 0xF7) return A_M | A_GRP3;
    return op >= 0xFE ? A_M : 0;
}
static uint32_t insn_len(const uint8_t *b, uint32_t n, uint32_t big)
{
    uint32_t i = 0, os = big, as = big, op, attr;
    for (;;) {
        if (i >= n) {
            return n + 1;
        }
        op = b[i++];
        if (op == 0x66) {
            os = !big;
        } else if (op == 0x67) {
            as = !big;
        } else if ((op & 0xE7) != 0x26 && (op & 0xFE) != 0x64 && op != 0xF0 && (op & 0xFE) != 0xF2) {
            break;
        }
    }
    if (op == 0x0F) {
        if (i >= n) {
            return n + 1;
        }
        op = b[i++];
        attr = (op & 0xF0) == 0x80 ? A_IV
               : (op == 0xA4 || op == 0xAC || op == 0xBA) ? A_M | A_I8
               : (op == 0x06 || (op & 0xF6) == 0xA0) ? 0 : A_M;
    } else {
        attr = insn_attr(op);
    }
    if (attr & A_M) {
        uint32_t m, mod, rm;
        if (i >= n) {
            return n + 1;
        }
        m = b[i++];
        mod = m >> 6;
        rm = m & 7;
        if (mod != 3 && as) {
            if (rm == 4) {
                if (i >= n) {
                    return n + 1;
                }
                if (mod == 0 && (b[i] & 7) == 5) {
                    i += 4;
                }
                i++;
            }
            i += mod == 1 ? 1 : (mod == 2 || (mod == 0 && rm == 5)) ? 4 : 0;
        } else if (mod != 3) {
            i += mod == 1 ? 1 : (mod == 2 || (mod == 0 && rm == 6)) ? 2 : 0;
        }
        if ((attr & A_GRP3) && ((m >> 3) & 7) < 2) {
            i += op == 0xF6 ? 1 : os ? 4 : 2;
        }
    }
    i += (attr & A_I8) ? 1 : 0;
    i += (attr & A_IV) ? (os ? 4 : 2) : 0;
    i += (attr & A_I16) ? 2 : 0;
    i += (attr & A_AP) ? (os ? 6 : 4) : 0;
    i += (attr & A_OV) ? (as ? 4 : 2) : 0;
    return i;
}

/* Physical addresses instructions can be read from directly. */
static inline int ram_code(uint32_t p)
{
    return p < 0xA0000 || (p >= 0xC0000 && p < cpu_ram_size);
}

/* Find where the instruction at cpu.ip can be fetched from: sets cpu.code, cpu.win_lo and
 * cpu.lim1 for a range of instruction pointers around it, or copies that one instruction
 * to cpu.wrapped if it is in an awkward place (lim1 = 0 then). May raise #GP or #PF. */
static NOINLINE void code_window(void)
{
    const cpu_seg_t *cs = SEG(CS);
    uint32_t ip = cpu.ip, n;

    cpu.win_lo = 0;
    cpu.lim1 = 0;
    if (!IS386) {
        /* The 8086 wraps around at the end of the segment and of the address space. */
        const int32_t room = (int32_t)(CPU_MEM_SIZE - 16) - (int32_t)cs->base;
        ip &= 0xFFFF;
        cpu.ip = ip;
        if ((int32_t)ip <= (room > 0xFFF0 ? 0xFFF0 : room)) {
            cpu.code = cpu_mem + cs->base;
            cpu.lim1 = (uint32_t)(room > 0xFFF0 ? 0xFFF0 : room) + 1;
            return;
        }
        for (uint32_t i = 0; i < sizeof(cpu.wrapped); i++) {
            cpu.wrapped[i] = cpu_mem[(cs->base + ((ip + i) & 0xFFFF)) & CPU_MEM_MASK];
        }
        cpu.code = (const uint8_t *)((uintptr_t)cpu.wrapped - ip);
        return;
    }

    if (ip > cs->limit) {
        RAISE_C(EXC_GP, 0);
    }
    const uint32_t lin = cs->base + ip, room = cs->limit - ip;
    if (!(cpu.cr0 & CR0_PG)) {
        const uint32_t top = cpu.a20 ? cpu_ram_size : CPU_MEM_SIZE;
        if (lin >= cs->base && lin < top) {
            uint32_t span = top - lin - 1;
            if (span > room) {
                span = room;
            }
            if (span >= 15) {
                cpu.code = (const uint8_t *)((uintptr_t)cpu_mem + cs->base);
                cpu.lim1 = ip + span - 14;
                return;
            }
        }
    } else {
        const uint32_t p = lin_phys(lin, USER()) & cpu.a20_mask;
        uint32_t span = 0xFFF - (lin & 0xFFF);
        if (span > room) {
            span = room;
        }
        if (span >= 15 && ram_code(p) && ram_code(p + span)) {
            cpu.code = (const uint8_t *)((uintptr_t)cpu_mem + p - (uintptr_t)ip);
            cpu.win_lo = ip > (lin & 0xFFF) ? ip - (lin & 0xFFF) : 0;
            cpu.lim1 = ip + span - 14;
            return;
        }
    }

    /* Near the end of the segment, of a page or of memory: take a copy of what can be
     * had of the instruction without faulting, and see whether that is all of it. */
    for (n = 0; n < 16 && n <= room; n++) {
        uint8_t *h = cpu_lin_ptr(lin + n, 1, 0);
        if (h) {
            cpu.wrapped[n] = *h;
        } else if (cpu.cr0 & CR0_PG) {
            break;
        } else {
            cpu.wrapped[n] = (uint8_t)phys_r8(lin + n);
        }
    }
    if (n < 16) {
        const uint32_t len = insn_len(cpu.wrapped, n, cpu.dmode & 1);
        if (len > n) {
            if (n > room) {
                RAISE_C(EXC_GP, 0);
            }
            cpu.wrapped[n] = (uint8_t)lin_r8(lin + n, USER()); /* raises the page fault */
        }
        memset(cpu.wrapped + n, 0, sizeof(cpu.wrapped) - n);
    }
    cpu.code = (const uint8_t *)((uintptr_t)cpu.wrapped - ip);
}

/* Whether the instruction after a LOCK prefix (at p) is one that may be locked. */
static int lock_ok(const uint8_t *p)
{
    uint32_t op, m, reg, i = 0;
    do {
        op = p[i++];
    } while (i < 12 && ((op & 0xE7) == 0x26 || (op & 0xFC) == 0x64 || (op & 0xFE) == 0xF2));
    m = p[i];
    if (op == 0x0F) {
        op = p[i];
        m = p[i + 1];
        reg = (m >> 3) & 7;
        return m < 0xC0 && (op == 0xAB || op == 0xB3 || op == 0xBB || (op == 0xBA && reg >= 5));
    }
    reg = (m >> 3) & 7;
    if (m >= 0xC0) {
        return 0;
    }
    if (op < 0x40) {
        return op < 0x38 && (op & 6) == 0;
    }
    return ((op & 0xFC) == 0x80 && reg != 7) || (op & 0xFE) == 0x86 ||
           ((op & 0xFE) == 0xF6 && (reg == 2 || reg == 3)) || ((op & 0xFE) == 0xFE && reg < 2);
}

/* --- string instructions ------------------------------------------------------- */

/* Host address of bytes at a segment offset that can be accessed in one go, for as many
 * elements as possible (*n in, at most; out, how many: zero if this must go the slow
 * way). Raises what the access to the first element would raise. */
static uint8_t *str_span(const cpu_seg_t *sg, uint32_t off, uint32_t size, uint32_t *n, int write)
{
    uint32_t room, lin, p, bytes = *n * size;
    *n = 0;
    if (!(sg->flags & (write ? CPU_SF_W : CPU_SF_R)) || (sg->flags & CPU_SF_DOWN) ||
        off > sg->limit || sg->limit - off < size - 1) {
        return NULL;
    }
    room = sg->limit - off;
    if (bytes - 1 > room) {
        bytes = room + 1;
    }
    lin = sg->base + off;
    if (cpu.cr0 & CR0_PG) {
        if ((lin & 0xFFF) + size > 0x1000) {
            return NULL;
        }
        p = lin_phys(lin, USER() | (uint32_t)write);
        if (bytes > 0x1000 - (lin & 0xFFF)) {
            bytes = 0x1000 - (lin & 0xFFF);
        }
    } else {
        p = lin;
        if (lin + bytes < lin) {
            return NULL;
        }
    }
    p &= cpu.a20_mask;
    if (p >= CPU_MEM_SIZE) {
        if (p >= cpu_ram_size) {
            return NULL;
        }
        if (bytes > cpu_ram_size - p) {
            bytes = cpu_ram_size - p;
        }
    } else {
        const uint32_t top = write ? ram_wr : ram_rd;
        if (p >= top) {
            return NULL;
        }
        if (bytes > top - p) {
            bytes = top - p;
        }
        if (!cpu.a20 && bytes > CPU_MEM_SIZE - p) {
            bytes = CPU_MEM_SIZE - p;
        }
    }
    *n = bytes / size;
    return *n ? cpu_mem + p : NULL;
}

enum { STR_MOVS, STR_CMPS, STR_STOS, STR_LODS, STR_SCAS, STR_INS, STR_OUTS };

static uint32_t str_rd(const cpu_seg_t *sg, uint32_t off, uint32_t size)
{
    return size == 1 ? sr8(sg, off) : size == 2 ? sr16(sg, off) : sr32(sg, off);
}
static void str_wr(const cpu_seg_t *sg, uint32_t off, uint32_t size, uint32_t v)
{
    if (size == 1) {
        sw8(sg, off, v);
    } else if (size == 2) {
        sw16(sg, off, v);
    } else {
        sw32(sg, off, v);
    }
}
static uint32_t port_in(uint32_t port, uint32_t size)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++) {
        v |= (uint32_t)pc_inb((uint16_t)(port + i)) << (8 * i);
    }
    return v;
}
static void port_out(uint32_t port, uint32_t size, uint32_t v)
{
    for (uint32_t i = 0; i < size; i++) {
        pc_outb((uint16_t)(port + i), (uint8_t)(v >> (8 * i)));
    }
}

/* One string instruction, with its repeat prefix if any (rep: 0, F2h or F3h). The index
 * and count registers are kept up to date element by element, so that a fault in the
 * middle leaves the instruction ready to be resumed. */
static NOINLINE void str_op(uint32_t kind, uint32_t size, uint32_t rep, const cpu_seg_t *src,
                            uint32_t a32)
{
    const uint32_t am = a32 ? 0xFFFFFFFFu : 0xFFFFu;
    const uint32_t step = cpu.f_df ? 0u - size : size;
    const uint32_t acc = size == 1 ? AL : size == 2 ? AX : EAX;
#define CNT (a32 ? ECX : (uint32_t)CX)
#define ADV(r) do { if (a32) { R32(r) += step; } else { R16(r) = (uint16_t)(R16(r) + step); } } while (0)
#define DEC_CNT() do { if (a32) { ECX--; } else { CX--; } } while (0)

    if (rep && CNT == 0) {
        return;
    }
    /* Whole runs at once where memory allows it. */
    if (rep && !cpu.f_df && (kind == STR_MOVS || kind == STR_STOS)) {
        for (;;) {
            uint32_t n = CNT, m, si = ESI & am, di = EDI & am, i;
            uint8_t *d, *s = NULL;
            if (n == 0) {
                return;
            }
            if (!a32) { /* up to where an index wraps */
                const uint32_t lim = (0x10000 - (kind == STR_MOVS && si > di ? si : di)) / size;
                if (n > lim) {
                    n = lim;
                }
            }
            if (kind == STR_MOVS) {
                m = n;
                s = str_span(src, si, size, &m, 0);
                n = m;
            }
            m = n;
            d = n ? str_span(SEG(ES), di, size, &m, 1) : NULL;
            n = m;
            if (n == 0) {
                break;
            }
            if (kind == STR_STOS) {
                if (size == 1) {
                    memset(d, (int)acc, n);
                } else {
                    for (i = 0; i < n; i++) {
                        if (size == 2) {
                            st16(d + 2 * i, acc);
                        } else {
                            st32(d + 4 * i, acc);
                        }
                    }
                }
            } else if (d >= s + n * size || s >= d + n * size) {
                memcpy(d, s, n * size);
            } else if (d < s) {
                memmove(d, s, n * size);
            } else {
                break; /* an overlapping forward copy replicates data: element by element */
            }
            if (a32) {
                ECX -= n;
                EDI += n * size;
                if (kind == STR_MOVS) {
                    ESI += n * size;
                }
            } else {
                CX = (uint16_t)(CX - n);
                DI = (uint16_t)(DI + n * size);
                if (kind == STR_MOVS) {
                    SI = (uint16_t)(SI + n * size);
                }
            }
        }
    }
    do {
        uint32_t a, b;
        switch (kind) {
        case STR_MOVS:
            str_wr(SEG(ES), EDI & am, size, str_rd(src, ESI & am, size));
            ADV(REG_SI);
            ADV(REG_DI);
            break;
        case STR_CMPS:
            a = str_rd(src, ESI & am, size);
            b = str_rd(SEG(ES), EDI & am, size);
            ADV(REG_SI);
            ADV(REG_DI);
            (void)(size == 1 ? f_sub8(a, b) : size == 2 ? f_sub16(a, b) : f_sub32(a, b));
            break;
        case STR_STOS:
            str_wr(SEG(ES), EDI & am, size, acc);
            ADV(REG_DI);
            break;
        case STR_LODS:
            a = str_rd(src, ESI & am, size);
            if (size == 1) {
                AL = (uint8_t)a;
            } else if (size == 2) {
                AX = (uint16_t)a;
            } else {
                EAX = a;
            }
            ADV(REG_SI);
            break;
        case STR_SCAS:
            b = str_rd(SEG(ES), EDI & am, size);
            ADV(REG_DI);
            (void)(size == 1 ? f_sub8(acc, b) : size == 2 ? f_sub16(acc, b) : f_sub32(acc, b));
            break;
        case STR_INS:
            /* The store must not be what faults once the port has been read. */
            (void)slow_rw(SEG(ES), EDI & am, size);
            str_wr(SEG(ES), EDI & am, size, port_in(DX, size));
            ADV(REG_DI);
            break;
        default:
            port_out(DX, size, str_rd(src, ESI & am, size));
            ADV(REG_SI);
            break;
        }
        if (!rep) {
            return;
        }
        DEC_CNT();
        if ((kind == STR_CMPS || kind == STR_SCAS) && GET_ZF() != (rep == 0xF3)) {
            return;
        }
    } while (CNT != 0);
#undef CNT
#undef ADV
#undef DEC_CNT
}

/* ENTER with a nesting level. */
static NOINLINE void do_enter(int os32, uint32_t size, uint32_t level)
{
    const uint32_t mask = sp_mask();
    uint32_t sp = ESP, bp = EBP, frame;
    spush(&sp, os32 ? EBP : BP, os32);
    frame = sp;
    for (uint32_t i = 1; i < level; i++) {
        bp -= os32 ? 4 : 2;
        spush(&sp, os32 ? sr32(SEG(SS), bp & mask) : sr16(SEG(SS), bp & mask), os32);
    }
    spush(&sp, frame, os32);
    if (IS386) { /* the new top of the stack must be writable */
        (void)slow_rw(SEG(SS), sp_add(sp, 0u - size) & mask, 1);
    }
    if (os32) {
        EBP = frame;
    } else {
        BP = (uint16_t)frame;
    }
    set_sp(sp_add(sp, 0u - size));
}

static inline int cond(uint32_t cc)
{
    int r;
    switch (cc >> 1) {
    case 0: r = GET_OF(); break;
    case 1: r = GET_CF(); break;
    case 2: r = GET_ZF(); break;
    case 3: r = GET_CF() | GET_ZF(); break;
    case 4: r = GET_SF(); break;
    case 5: r = GET_PF(); break;
    case 6: r = GET_SF() != GET_OF(); break;
    default: r = GET_ZF() | (GET_SF() != GET_OF()); break;
    }
    return r ^ (int)(cc & 1);
}

/* CLI, STI, and in virtual-8086 mode PUSHF, POPF, INT n and IRET, need I/O privilege. */
static inline int iopl_ok(void)
{
    return !(cpu.cr0 & CR0_PE) || (cpu.f_vm ? cpu.f_iopl == 3 : cpu.cpl <= cpu.f_iopl);
}

/* --- the interpreter ------------------------------------------------------ */

/*
 * While instructions run, the instruction pointer and the slice counter live in locals
 * (machine registers), as do the things derived from the code and stack segments: `code`
 * and `lim1` (see cpu_t), the table of handlers `disp` and the addressing tables `eap`.
 * SYNC_OUT publishes the first two before anything outside this loop can look at the CPU
 * state (port I/O, BIOS hypercalls, far control transfers); SYNC_IN picks up whatever
 * that changed.
 *
 * Every opcode has a handler, a labelled piece of code that ends by jumping to the handler
 * of the next instruction (NEXT). The handlers that depend on the operand size exist
 * twice (cpu_ops.h), those that also use the stack four times (cpu_stk.h), and there is
 * one table of handlers for each combination: an operand-size prefix just picks the other
 * table for one instruction.
 *
 * The compiler must lay this function out in source order (-fno-reorder-blocks): the rare
 * cases are written so that the usual ones run straight on.
 */
#define FETCH8() ((uint32_t)code[ip++])
#define FETCH16() ({ uint32_t v_ = ld16(code + ip); ip += 2; v_; })
#define FETCH32() ({ uint32_t v_ = ld32(code + ip); ip += 4; v_; })
#define SEXT8(x) ((uint32_t)(int32_t)(int8_t)(x))

/* Memory by segment and offset. Before the slow path, the slice counter is put where an
 * exception finds it. mrw: a read that the same instruction follows with a write. */
#define MEM_RD(win, i, slow, load) \
    ({ \
        const cpu_seg_t *s_ = (sg_); \
        const uint32_t o_ = (off_); \
        uint32_t v_; \
        if (unlikely(o_ - s_->lo >= s_->win[i])) { \
            cpu.slice = slice; \
            v_ = slow; \
        } else { \
            v_ = load; \
        } \
        v_; \
    })
#define mr8(sg, o) ({ const cpu_seg_t *sg_ = (sg); const uint32_t off_ = (o); MEM_RD(rd, 0, slow_r8(s_, o_), s_->mem[o_]); })
#define mr16(sg, o) ({ const cpu_seg_t *sg_ = (sg); const uint32_t off_ = (o); MEM_RD(rd, 1, slow_r16(s_, o_), ld16(s_->mem + o_)); })
#define mr32(sg, o) ({ const cpu_seg_t *sg_ = (sg); const uint32_t off_ = (o); MEM_RD(rd, 2, slow_r32(s_, o_), ld32(s_->mem + o_)); })
#define mrw8(sg, o) ({ const cpu_seg_t *sg_ = (sg); const uint32_t off_ = (o); MEM_RD(wr, 0, slow_rw(s_, o_, 1), s_->mem[o_]); })
#define mrw16(sg, o) ({ const cpu_seg_t *sg_ = (sg); const uint32_t off_ = (o); MEM_RD(wr, 1, slow_rw(s_, o_, 2), ld16(s_->mem + o_)); })
#define mrw32(sg, o) ({ const cpu_seg_t *sg_ = (sg); const uint32_t off_ = (o); MEM_RD(wr, 2, slow_rw(s_, o_, 4), ld32(s_->mem + o_)); })
#define MEM_WR(i, slow, store) \
    do { \
        if (unlikely(o_ - s_->lo >= s_->wr[i])) { \
            cpu.slice = slice; \
            slow; \
        } else { \
            store; \
        } \
    } while (0)
#define mw8(sg, o, v) do { const cpu_seg_t *s_ = (sg); const uint32_t o_ = (o); const uint32_t w_ = (v); MEM_WR(0, slow_w8(s_, o_, w_), s_->mem[o_] = (uint8_t)w_); } while (0)
#define mw16(sg, o, v) do { const cpu_seg_t *s_ = (sg); const uint32_t o_ = (o); const uint32_t w_ = (v); MEM_WR(1, slow_w16(s_, o_, w_), st16(s_->mem + o_, w_)); } while (0)
#define mw32(sg, o, v) do { const cpu_seg_t *s_ = (sg); const uint32_t o_ = (o); const uint32_t w_ = (v); MEM_WR(2, slow_w32(s_, o_, w_), st32(s_->mem + o_, w_)); } while (0)

#define PFX_SEG 1u  /* a segment override is in effect (in ea_mode[].seg) */
#define PFX_ADDR 2u /* an address-size prefix (eap is the other table) */
#define PFX_OP 4u   /* an operand-size prefix (tab is the other table) */
#define SYNC_OUT() \
    do { \
        cpu.ip = ip; \
        cpu.slice = slice; \
    } while (0)
#define SYNC_IN() \
    do { \
        ip = cpu.ip; \
        slice = cpu.slice; \
        code = cpu.code; \
        lim1 = (pfx | rep) ? 0 : cpu.lim1; \
        disp = dispatch[cpu.dmode]; \
        /* Whatever is dispatched from here on without passing the head of the loop \
         * finds the table its own prefixes need. */ \
        tab = disp; \
        if (!(pfx & PFX_ADDR)) { \
            eap = &ea_mode[cpu.dmode & 1]; \
        } \
    } while (0)
/* Re-check interrupts after this instruction (the in-loop form of cpu_break). */
#define BREAK() \
    do { \
        cpu.saved += slice; \
        slice = 0; \
    } while (0)
#define FAULT(v) \
    do { \
        cpu.slice = slice; \
        raise_exc(v, 0, 0); \
    } while (0)
#define FAULT_C(v, c) \
    do { \
        cpu.slice = slice; \
        raise_exc(v, 1, c); \
    } while (0)
/* An INT instruction; execution goes on in the handler. */
#define INT_SOFT(n) \
    do { \
        SYNC_OUT(); \
        do_interrupt(n, 1, 0, 0, ip); \
        SYNC_IN(); \
    } while (0)
#define IO_CHECK(port, size) \
    do { \
        if (unlikely(cpu.cr0 & CR0_PE)) { \
            cpu.slice = slice; \
            io_check(port, size); \
        } \
    } while (0)
#define IO_IN(port, size) ({ SYNC_OUT(); uint32_t v_ = port_in(port, size); SYNC_IN(); v_; })
#define IO_OUT(port, size, v) \
    do { \
        SYNC_OUT(); \
        port_out(port, size, v); \
        SYNC_IN(); \
    } while (0)
/* Load a segment register other than CS. */
#define LOADSEG(s, v) \
    do { \
        if (likely(!cpu.pm)) { \
            seg_load_real(s, v); \
        } else { \
            SYNC_OUT(); \
            load_seg(s, v); \
            SYNC_IN(); \
        } \
    } while (0)
/* Far transfers outside protected mode are simple enough to do on the spot. */
#define FAR_JMP(sel, off) \
    do { \
        const uint32_t sel_ = (sel), off_ = (off); \
        if (likely(!cpu.pm)) { \
            if (unlikely(IS386 && off_ > cpu.sg[CS].limit)) { \
                FAULT_C(EXC_GP, 0); \
            } \
            ip = off_; \
            seg_load_real(CS, sel_); \
            lim1 = 0; \
        } else { \
            SYNC_OUT(); \
            far_jmp(sel_, off_, ip); \
            SYNC_IN(); \
        } \
        goto next; \
    } while (0)

/* End of a handler: in the common case, go straight to the handler of the next
 * instruction. Prefixes leave lim1 at 0, so that a prefixed instruction always ends at
 * `next`, which clears them. NEXT_J is for the handlers that have changed the instruction
 * pointer other than by fetching. */
#define NEXT() \
    do { \
        if (likely(slice > 0 && ip < lim1)) { \
            slice--; \
            cpu.insn_ip = ip; \
            op = FETCH8(); \
            goto *disp[op]; \
        } \
        /* The store keeps this exit a block of its own next to the handler, so that \
         * the tests above are short branches that are not taken. */ \
        cpu.slice = slice; \
        goto next; \
    } while (0)
#define NEXT_J() \
    do { \
        if (likely(slice > 0 && ip < lim1 && ip >= cpu.win_lo)) { \
            slice--; \
            cpu.insn_ip = ip; \
            op = FETCH8(); \
            goto *disp[op]; \
        } \
        cpu.slice = slice; \
        goto next_jump; \
    } while (0)
#define PREFIX() \
    do { \
        lim1 = 0; \
        if (unlikely(ip - cpu.insn_ip > 13)) { \
            goto too_long; \
        } \
        goto prefix; \
    } while (0)
#define SEG_PREFIX(s) \
    do { \
        pfx |= PFX_SEG; \
        ea_mode[0].seg[0] = ea_mode[0].seg[1] = SEG(s); \
        ea_mode[1].seg[0] = ea_mode[1].seg[1] = SEG(s); \
        PREFIX(); \
    } while (0)

/* Decoding of a ModRM byte, and the effective address of its memory operand. */
typedef uint32_t __attribute__((may_alias)) u32a_t;
#define CPU_D(off) (*(const u32a_t *)((const uint8_t *)&cpu + (off)))
#define IS_REG() (modrm >= 0xC0)
#define MODRM() \
    do { \
        modrm = FETCH8(); \
        reg = (modrm >> 3) & 7; \
        rm = modrm & 7; \
    } while (0)
#define EA() \
    do { \
        uint32_t e_ = eap->tab[modrm]; \
        uint32_t o_ = CPU_D(e_ & 0xFF) + CPU_D((e_ >> 8) & 0xFF); \
        if ((int32_t)e_ < 0) { \
            /* in the order of how often 32-bit programs have them */ \
            if (e_ & EA_DISP8) { \
                o_ += SEXT8(FETCH8()); \
            } else if (e_ & EA_DISP32) { \
                o_ += FETCH32(); \
            } else if (e_ & EA_DISP16) { \
                o_ += FETCH16(); \
            } else { \
                const sib_t r_ = ea_sib(code + ip, modrm); \
                ip += r_.info & 7; \
                o_ = r_.off; \
                e_ |= r_.info & EA_SS; \
            } \
        } \
        ea_off = o_ & eap->mask; \
        ea_seg = eap->seg[(e_ >> 16) & 1]; \
    } while (0)
#define DECODE() \
    do { \
        MODRM(); \
        if (!IS_REG()) { \
            EA(); \
        } \
    } while (0)
/* A handler that is a ModRM byte and a body. The body is compiled twice, for a register
 * and for a memory operand, so that neither pays for the other and each has its own NEXT.
 * RM_OP puts the register form first, where execution runs straight into it; RM_OP_M the
 * memory form. */
#define RM_OP(...) \
    do { \
        MODRM(); \
        if (IS_REG()) { \
            __VA_ARGS__; \
            NEXT(); \
        } \
        EA(); \
        __VA_ARGS__; \
        NEXT(); \
    } while (0)
#define RM_OP_M(...) \
    do { \
        MODRM(); \
        if (!IS_REG()) { \
            EA(); \
            __VA_ARGS__; \
            NEXT(); \
        } \
        __VA_ARGS__; \
        NEXT(); \
    } while (0)

#define GET_RM8() (IS_REG() ? (uint32_t)R8(rm) : mr8(ea_seg, ea_off))
#define GET_RM16() (IS_REG() ? (uint32_t)R16(rm) : mr16(ea_seg, ea_off))
#define GET_RM32() (IS_REG() ? R32(rm) : mr32(ea_seg, ea_off))
#define GET_RMW8() (IS_REG() ? (uint32_t)R8(rm) : mrw8(ea_seg, ea_off))
#define GET_RMW16() (IS_REG() ? (uint32_t)R16(rm) : mrw16(ea_seg, ea_off))
#define GET_RMW32() (IS_REG() ? R32(rm) : mrw32(ea_seg, ea_off))
#define SET_RM8(v) \
    do { \
        if (IS_REG()) { \
            R8(rm) = (uint8_t)(v); \
        } else { \
            mw8(ea_seg, ea_off, (v)); \
        } \
    } while (0)
#define SET_RM16(v) \
    do { \
        if (IS_REG()) { \
            R16(rm) = (uint16_t)(v); \
        } else { \
            mw16(ea_seg, ea_off, (v)); \
        } \
    } while (0)
#define SET_RM32(v) \
    do { \
        if (IS_REG()) { \
            R32(rm) = (v); \
        } else { \
            mw32(ea_seg, ea_off, (v)); \
        } \
    } while (0)

#define OP(x) op_##x
#define ALU8(c0, c2, c4, F, STORE) \
    OP(c0): \
        RM_OP( \
            t = F(STORE ? GET_RMW8() : GET_RM8(), (uint32_t)R8(reg)); \
            if (STORE) { \
                SET_RM8(t); \
            } \
        ); \
    OP(c2): \
        RM_OP( \
            t = F((uint32_t)R8(reg), GET_RM8()); \
            if (STORE) { \
                R8(reg) = (uint8_t)t; \
            } \
        ); \
    OP(c4): \
        t = FETCH8(); \
        t = F((uint32_t)AL, t); \
        if (STORE) { \
            AL = (uint8_t)t; \
        } \
        NEXT();

/* String instructions: the plain 16-bit forms are done in place, the rest by str_op. */
#define STR_SRC() (eap->seg[0])
#define STR_PLAIN() likely(!(rep | eap->a32))
#define STRING(kind, size) \
    do { \
        cpu.slice = slice; \
        str_op(kind, size, rep, STR_SRC(), eap->a32); \
    } while (0)
#define IO_STRING(kind, size) \
    do { \
        IO_CHECK(DX, size); \
        SYNC_OUT(); \
        str_op(kind, size, rep, STR_SRC(), eap->a32); \
        SYNC_IN(); \
    } while (0)
#define DF_STEP(n) (cpu.f_df ? -(n) : (n))

#define OPCODES(B, W, S) \
    B(0x00), W(0x01), B(0x02), W(0x03), B(0x04), W(0x05), S(0x06), S(0x07), \
    B(0x08), W(0x09), B(0x0A), W(0x0B), B(0x0C), W(0x0D), S(0x0E), W(0x0F), \
    B(0x10), W(0x11), B(0x12), W(0x13), B(0x14), W(0x15), S(0x16), S(0x17), \
    B(0x18), W(0x19), B(0x1A), W(0x1B), B(0x1C), W(0x1D), S(0x1E), S(0x1F), \
    B(0x20), W(0x21), B(0x22), W(0x23), B(0x24), W(0x25), B(0x26), B(0x27), \
    B(0x28), W(0x29), B(0x2A), W(0x2B), B(0x2C), W(0x2D), B(0x2E), B(0x2F), \
    B(0x30), W(0x31), B(0x32), W(0x33), B(0x34), W(0x35), B(0x36), B(0x37), \
    B(0x38), W(0x39), B(0x3A), W(0x3B), B(0x3C), W(0x3D), B(0x3E), B(0x3F), \
    W(0x40), W(0x41), W(0x42), W(0x43), W(0x44), W(0x45), W(0x46), W(0x47), \
    W(0x48), W(0x49), W(0x4A), W(0x4B), W(0x4C), W(0x4D), W(0x4E), W(0x4F), \
    S(0x50), S(0x51), S(0x52), S(0x53), S(0x54), S(0x55), S(0x56), S(0x57), \
    S(0x58), S(0x59), S(0x5A), S(0x5B), S(0x5C), S(0x5D), S(0x5E), S(0x5F), \
    S(0x60), S(0x61), W(0x62), B(0x63), B(0x64), B(0x65), B(0x66), B(0x67), \
    S(0x68), W(0x69), S(0x6A), W(0x6B), B(0x6C), W(0x6D), B(0x6E), W(0x6F), \
    W(0x70), W(0x71), W(0x72), W(0x73), W(0x74), W(0x75), W(0x76), W(0x77), \
    W(0x78), W(0x79), W(0x7A), W(0x7B), W(0x7C), W(0x7D), W(0x7E), W(0x7F), \
    B(0x80), W(0x81), B(0x82), W(0x83), B(0x84), W(0x85), B(0x86), W(0x87), \
    B(0x88), W(0x89), B(0x8A), W(0x8B), W(0x8C), W(0x8D), B(0x8E), S(0x8F), \
    W(0x90), W(0x91), W(0x92), W(0x93), W(0x94), W(0x95), W(0x96), W(0x97), \
    W(0x98), W(0x99), S(0x9A), B(0x9B), S(0x9C), S(0x9D), B(0x9E), B(0x9F), \
    B(0xA0), W(0xA1), B(0xA2), W(0xA3), B(0xA4), W(0xA5), B(0xA6), W(0xA7), \
    B(0xA8), W(0xA9), B(0xAA), W(0xAB), B(0xAC), W(0xAD), B(0xAE), W(0xAF), \
    B(0xB0), B(0xB1), B(0xB2), B(0xB3), B(0xB4), B(0xB5), B(0xB6), B(0xB7), \
    W(0xB8), W(0xB9), W(0xBA), W(0xBB), W(0xBC), W(0xBD), W(0xBE), W(0xBF), \
    B(0xC0), W(0xC1), S(0xC2), S(0xC3), W(0xC4), W(0xC5), B(0xC6), W(0xC7), \
    S(0xC8), S(0xC9), S(0xCA), S(0xCB), B(0xCC), B(0xCD), B(0xCE), W(0xCF), \
    B(0xD0), W(0xD1), B(0xD2), W(0xD3), B(0xD4), B(0xD5), B(0xD6), B(0xD7), \
    B(0xD8), B(0xD9), B(0xDA), B(0xDB), B(0xDC), B(0xDD), B(0xDE), B(0xDF), \
    W(0xE0), W(0xE1), W(0xE2), W(0xE3), B(0xE4), W(0xE5), B(0xE6), W(0xE7), \
    S(0xE8), W(0xE9), W(0xEA), W(0xEB), B(0xEC), W(0xED), B(0xEE), W(0xEF), \
    B(0xF0), B(0xF1), B(0xF2), B(0xF3), B(0xF4), B(0xF5), B(0xF6), W(0xF7), \
    B(0xF8), B(0xF9), B(0xFA), B(0xFB), B(0xFC), B(0xFD), B(0xFE), S(0xFF),
/* The two-byte opcodes of the 80386 (0Fh and one of these). */
#define OPCODES_0F(X) \
    X(0x00) X(0x01) X(0x02) X(0x03) X(0x06) X(0x20) \
    X(0x21) X(0x22) X(0x23) X(0x24) X(0x26) X(0x80) \
    X(0x81) X(0x82) X(0x83) X(0x84) X(0x85) X(0x86) \
    X(0x87) X(0x88) X(0x89) X(0x8A) X(0x8B) X(0x8C) \
    X(0x8D) X(0x8E) X(0x8F) X(0x90) X(0x91) X(0x92) \
    X(0x93) X(0x94) X(0x95) X(0x96) X(0x97) X(0x98) \
    X(0x99) X(0x9A) X(0x9B) X(0x9C) X(0x9D) X(0x9E) \
    X(0x9F) X(0xA0) X(0xA1) X(0xA3) X(0xA4) X(0xA5) \
    X(0xA8) X(0xA9) X(0xAB) X(0xAC) X(0xAD) X(0xAF) \
    X(0xB2) X(0xB3) X(0xB4) X(0xB5) X(0xB6) X(0xB7) \
    X(0xBA) X(0xBB) X(0xBC) X(0xBD) X(0xBE) X(0xBF)

/* What may not follow an operand-size prefix without that being remembered: another
 * prefix, and the coprocessor escapes, whose one handler asks for the operand size. */
static const uint8_t prefix_byte[256] = {
    [0x26] = 1, [0x2E] = 1, [0x36] = 1, [0x3E] = 1, [0x64] = 1, [0x65] = 1,
    [0x66] = 1, [0x67] = 1, [0xF0] = 1, [0xF2] = 1, [0xF3] = 1,
    [0xD8] = 1, [0xD9] = 1, [0xDA] = 1, [0xDB] = 1, [0xDC] = 1, [0xDD] = 1, [0xDE] = 1, [0xDF] = 1,
};

static void run_loop(void)
{
    const uint8_t *code;
    uint32_t ip, lim1;
    int32_t slice;
    uint32_t modrm = 0, reg = 0, rm = 0, ea_off = 0;
    const cpu_seg_t *ea_seg = SEG(DS);
    /* Prefixes of the instruction being decoded: PFX_* and the repeat prefix. */
    uint32_t pfx = 0, rep = 0;
    uint32_t op, t, u;
    const void *const *disp, *const *tab;
    struct ea *eap;

    /* Handlers by opcode, for 16- and 32-bit operands and for 16- and 32-bit stacks. */
#define B_(x) &&op_##x
#define W16_(x) &&op16_##x
#define W32_(x) &&op32_##x
#define S16A_(x) &&op16s16_##x
#define S32A_(x) &&op32s16_##x
#define S16B_(x) &&op16s32_##x
#define S32B_(x) &&op32s32_##x
    static const void *const dispatch[4][256] = {
        {OPCODES(B_, W16_, S16A_)},
        {OPCODES(B_, W32_, S32A_)},
        {OPCODES(B_, W16_, S16B_)},
        {OPCODES(B_, W32_, S32B_)},
    };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
#define X16_(x) [x] = &&op16_0F_##x,
#define X32_(x) [x] = &&op32_0F_##x,
    static const void *const dispatch_0f[2][256] = {
        {[0 ... 255] = &&invalid, OPCODES_0F(X16_)},
        {[0 ... 255] = &&invalid, OPCODES_0F(X32_)},
    };
#pragma GCC diagnostic pop

    /* An instruction that faulted may have left its prefixes behind. */
    for (int i = 0; i < 2; i++) {
        ea_mode[i].seg[0] = SEG(DS);
        ea_mode[i].seg[1] = SEG(SS);
    }

    for (;;) {
        /* Instruction boundary with a possible event. */
        if (cpu.trap_armed) {
            cpu.trap_armed = 0;
            if (cpu.f_tf) {
                cpu.insn_ip = cpu.ip;
                do_interrupt(1, 0, 0, 0, cpu.ip);
            }
        }
        if (cpu.irq_line && cpu.f_if) {
            cpu.halted = 0;
            cpu.insn_ip = cpu.ip;
            do_interrupt(pc_irq_ack(), 0, 0, 0, cpu.ip);
        }
        if (cpu.halted || cpu.saved <= 0) {
            break;
        }
        if (cpu.f_tf || cpu.test_mode) {
            cpu.trap_armed = cpu.f_tf;
            cpu.slice = 1;
            cpu.saved -= 1;
        } else {
            cpu.slice = cpu.saved;
            cpu.saved = 0;
        }

        SYNC_IN();
        while (slice > 0) {
            slice--;
            cpu.insn_ip = ip;
            if (unlikely(ip >= lim1 || ip < cpu.win_lo)) {
                /* Outside the range instructions were last fetched from, or after a
                 * prefix or an instruction fetched from a copy. */
                cpu.ip = ip;
                cpu.slice = slice;
                code_window();
                ip = cpu.ip;
                code = cpu.code;
                lim1 = cpu.lim1;
            }
            tab = disp;
        prefix:
            op = FETCH8();
            goto *tab[op];

            ALU8(0x00, 0x02, 0x04, f_add8, 1)
            ALU8(0x08, 0x0A, 0x0C, f_or8, 1)
            ALU8(0x10, 0x12, 0x14, f_adc8, 1)
            ALU8(0x18, 0x1A, 0x1C, f_sbb8, 1)
            ALU8(0x20, 0x22, 0x24, f_and8, 1)
            ALU8(0x28, 0x2A, 0x2C, f_sub8, 1)
            ALU8(0x30, 0x32, 0x34, f_xor8, 1)
            ALU8(0x38, 0x3A, 0x3C, f_sub8, 0)

        OP(0x26): SEG_PREFIX(ES);
        OP(0x2E): SEG_PREFIX(CS);
        OP(0x36): SEG_PREFIX(SS);
        OP(0x3E): SEG_PREFIX(DS);
        OP(0x64):
            if (unlikely(!IS386)) {
                goto not_386;
            }
            SEG_PREFIX(FS);
        OP(0x65):
            if (unlikely(!IS386)) {
                goto not_386;
            }
            SEG_PREFIX(GS);
        OP(0x66):
            if (unlikely(!IS386)) {
                goto not_386;
            }
            if (likely(!(pfx | rep) && !prefix_byte[code[ip]])) {
                /* The only prefix, as nearly always: nothing to remember, the
                 * instruction is simply taken from the other table. */
                op = FETCH8();
                goto *dispatch[cpu.dmode ^ 1][op];
            }
            pfx |= PFX_OP;
            tab = dispatch[cpu.dmode ^ 1];
            PREFIX();
        OP(0x67):
            if (unlikely(!IS386)) {
                goto not_386;
            }
            pfx |= PFX_ADDR;
            eap = &ea_mode[!(cpu.dmode & 1)];
            PREFIX();
        not_386: /* 60-6F: on the 8086, aliases of the conditional jumps */
            if (cpu.model != CPU_8086) {
                goto invalid;
            }
        jcc_alias:
            t = FETCH8();
            if (cond(op & 0x0F)) {
                ip = (ip + SEXT8(t)) & 0xFFFF;
            }
            NEXT();

        OP(0x27): { /* DAA */
            uint32_t al = AL, cf = GET_CF(), af = GET_AF();
            uint32_t hi = (cpu.model == CPU_8086 && af) ? 0x9F : 0x99;
            if ((al & 0x0F) > 9 || af) {
                AL = (uint8_t)(AL + 6);
                af = 1;
            }
            if (al > hi || cf) {
                AL = (uint8_t)(AL + 0x60);
                cf = 1;
            } else {
                cf = 0;
            }
            set_cf_of(cf, ((al ^ AL) & ~al & 0x80) != 0);
            set_af(af);
            SETZSP8(AL);
            NEXT();
        }
        OP(0x2F): { /* DAS */
            uint32_t al = AL, cf = GET_CF(), af = GET_AF();
            uint32_t hi = (cpu.model == CPU_8086 && af) ? 0x9F : 0x99;
            uint32_t borrow = 0;
            if ((al & 0x0F) > 9 || af) {
                borrow = IS386 && al < 6; /* the 80386 carries this one into CF */
                AL = (uint8_t)(AL - 6);
                af = 1;
            }
            if (al > hi || cf) {
                AL = (uint8_t)(AL - 0x60);
                cf = 1;
            } else {
                cf = borrow;
            }
            set_cf_of(cf, ((al ^ AL) & al & 0x80) != 0);
            set_af(af);
            SETZSP8(AL);
            NEXT();
        }
        OP(0x37): /* AAA */
            t = (AL & 0x0F) > 9 || GET_AF();
            if (IS386) {
                /* The 80386 lets the adjusting addition set the other flags. */
                f_add8(AL, t ? 6 : 0);
                if (t) {
                    AX = (uint16_t)(AX + 0x106);
                }
                set_cf(t);
                set_af(t);
                AL &= 0x0F;
                NEXT();
            }
            if (t) {
                AL = (uint8_t)(AL + 6);
                AH = (uint8_t)(AH + 1);
            }
            set_cf_of(t, 0);
            set_af(t);
            AL &= 0x0F;
            SETZSP8(AL);
            NEXT();
        OP(0x3F): /* AAS */
            t = (AL & 0x0F) > 9 || GET_AF();
            if (IS386) {
                f_sub8(AL, t ? 6 : 0);
                if (t) {
                    AX = (uint16_t)(AX - 6);
                    AH = (uint8_t)(AH - 1);
                }
                set_cf(t);
                set_af(t);
                AL &= 0x0F;
                NEXT();
            }
            if (t) {
                AL = (uint8_t)(AL - 6);
                AH = (uint8_t)(AH - 1);
            }
            set_cf_of(t, 0);
            set_af(t);
            AL &= 0x0F;
            SETZSP8(AL);
            NEXT();

        OP(0x63): /* ARPL */
            if (unlikely(!IS386)) {
                goto not_386;
            }
            if (!cpu.pm) {
                goto invalid;
            }
            DECODE();
            t = GET_RM16(); /* written to only if it changes */
            u = R16(reg);
            if ((t & 3) < (u & 3)) {
                SET_RM16((t & ~3u) | (u & 3));
                set_zf(1);
            } else {
                set_zf(0);
            }
            NEXT();
        OP(0x6C): /* INSB */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            IO_STRING(STR_INS, 1);
            NEXT();
        OP(0x6E): /* OUTSB */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            IO_STRING(STR_OUTS, 1);
            BREAK();
            NEXT();

        OP(0x80): OP(0x82):
            RM_OP_M(
                t = reg == 7 ? GET_RM8() : GET_RMW8();
                u = FETCH8();
                switch (reg) {
                case 0: t = f_add8(t, u); break;
                case 1: t = f_or8(t, u); break;
                case 2: t = f_adc8(t, u); break;
                case 3: t = f_sbb8(t, u); break;
                case 4: t = f_and8(t, u); break;
                case 5: t = f_sub8(t, u); break;
                case 6: t = f_xor8(t, u); break;
                default: f_sub8(t, u); NEXT();
                }
                SET_RM8(t);
            );
        OP(0x84):
            RM_OP(
                f_and8(GET_RM8(), R8(reg));
            );
        OP(0x86):
            RM_OP(
                t = GET_RMW8();
                SET_RM8(R8(reg));
                R8(reg) = (uint8_t)t;
            );
        OP(0x88):
            RM_OP_M(
                SET_RM8(R8(reg));
            );
        OP(0x8A):
            RM_OP_M(
                R8(reg) = (uint8_t)GET_RM8();
            );
        OP(0x8E): /* MOV to a segment register */
            DECODE();
            if (IS386 ? (reg == CS || reg > GS) : 0) {
                goto invalid;
            }
            if (!IS386) {
                reg &= 3;
            }
            t = GET_RM16();
            if (reg == CS) { /* 8086 only */
                seg_load_real(CS, t);
                lim1 = 0;
                NEXT();
            }
            LOADSEG(reg, t);
            if (reg == SS) {
                goto inhibit;
            }
            NEXT();

        OP(0x9B): /* WAIT */
            if ((cpu.cr0 & (CR0_MP | CR0_TS)) == (CR0_MP | CR0_TS)) {
                FAULT(EXC_NM);
            }
            NEXT(); /* a pending coprocessor error has raised its interrupt already */
        OP(0x9E): /* SAHF */
            t = AH;
            set_cf(t & 1);
            cpu.f_ax = t & FL_AF;
            cpu.f_zr = !(t & FL_ZF);
            cpu.f_sp = ((t & FL_SF) ? 0x80000000u : 0) | ((t & FL_PF) ? 0 : 1);
            NEXT();
        OP(0x9F): AH = (uint8_t)get_eflags(); NEXT();

        OP(0xA0):
            t = eap->a32 ? FETCH32() : FETCH16();
            AL = (uint8_t)mr8(STR_SRC(), t);
            NEXT();
        OP(0xA2):
            t = eap->a32 ? FETCH32() : FETCH16();
            mw8(STR_SRC(), t, AL);
            NEXT();
        OP(0xA4): /* MOVSB */
            if (STR_PLAIN()) {
                mw8(SEG(ES), DI, mr8(STR_SRC(), SI));
                SI += DF_STEP(1);
                DI += DF_STEP(1);
                NEXT();
            }
            STRING(STR_MOVS, 1);
            NEXT();
        OP(0xA6): /* CMPSB */
            STRING(STR_CMPS, 1);
            NEXT();
        OP(0xA8):
            t = FETCH8();
            f_and8(AL, t);
            NEXT();
        OP(0xAA): /* STOSB */
            if (STR_PLAIN()) {
                mw8(SEG(ES), DI, AL);
                DI += DF_STEP(1);
                NEXT();
            }
            STRING(STR_STOS, 1);
            NEXT();
        OP(0xAC): /* LODSB */
            if (STR_PLAIN()) {
                AL = (uint8_t)mr8(STR_SRC(), SI);
                SI += DF_STEP(1);
                NEXT();
            }
            STRING(STR_LODS, 1);
            NEXT();
        OP(0xAE): /* SCASB */
            STRING(STR_SCAS, 1);
            NEXT();

#define H_MOV8(n, x) OP(x): cpu.r.b[((n) & 3) * 4 + ((n) >> 2)] = (uint8_t)FETCH8(); NEXT();
            H_MOV8(0, 0xB0) H_MOV8(1, 0xB1) H_MOV8(2, 0xB2) H_MOV8(3, 0xB3)
            H_MOV8(4, 0xB4) H_MOV8(5, 0xB5) H_MOV8(6, 0xB6) H_MOV8(7, 0xB7)

        OP(0xC0): /* shift rm8, imm8 */
            if (cpu.model == CPU_8086) {
                goto *dispatch[0][0xC2];
            }
            DECODE();
            t = GET_RMW8();
            u = FETCH8();
            SET_RM8(shift(reg, t, u, 8));
            NEXT();
        OP(0xC6):
            RM_OP_M(
                if (IS386 && reg) {
                    goto invalid;
                }
                t = FETCH8();
                SET_RM8(t);
            );
        OP(0xCC):
            INT_SOFT(3);
            goto next;
        OP(0xCD):
            t = FETCH8();
            if (unlikely(cpu.f_vm) && cpu.f_iopl != 3) {
                FAULT_C(EXC_GP, 0);
            }
            if (unlikely(t == 0x2F) && AX == 0x1689 && cpu.f_if) {
                /* The Windows kernel's idle call: it has nothing to do until an interrupt
                 * brings something. Wait for one as HLT does, then make the call. */
                if (!cpu.idle_waited) {
                    cpu.idle_waited = 1;
                    ip = cpu.insn_ip;
                    cpu.halted = 1;
                    BREAK();
                    NEXT();
                }
                cpu.idle_waited = 0;
            }
            INT_SOFT(t);
            goto next;
        OP(0xCE):
            if (GET_OF()) {
                INT_SOFT(4);
                goto next;
            }
            NEXT();

        OP(0xD0):
            RM_OP(
                t = GET_RMW8();
                SET_RM8(shift(reg, t, 1, 8));
            );
        OP(0xD2):
            RM_OP(
                t = GET_RMW8();
                SET_RM8(shift(reg, t, CL, 8));
            );
        OP(0xD4): /* AAM */
            t = FETCH8();
            if (t == 0) {
                if (IS386) {
                    /* The flags the 80386 leaves behind (they are in the stack frame). */
                    cpu.f_co = 0;
                    cpu.f_ax = 0;
                    cpu.f_zr = 1;
                    cpu.f_sp = parity_tab[AL];
                    FAULT(EXC_DE);
                }
                cpu.f_co = 0;
                cpu.f_ax = 0;
                SETZSP8(0);
                goto div0;
            }
            AH = (uint8_t)(AL / t);
            AL = (uint8_t)(AL % t);
            cpu.f_co = 0;
            cpu.f_ax = 0;
            SETZSP8(AL);
            NEXT();
        OP(0xD5): /* AAD */
            t = FETCH8();
            AL = (uint8_t)f_add8(AL, (AH * t) & 0xFF);
            AH = 0;
            NEXT();
        OP(0xD6): AL = GET_CF() ? 0xFF : 0; NEXT(); /* SALC */
        OP(0xD7): AL = (uint8_t)mr8(STR_SRC(), (EBX + AL) & eap->mask); NEXT();
        OP(0xE4):
            t = FETCH8();
            IO_CHECK(t, 1);
            AL = (uint8_t)IO_IN(t, 1);
            NEXT();
        OP(0xE6):
            t = FETCH8();
            IO_CHECK(t, 1);
            IO_OUT(t, 1, AL);
            BREAK();
            NEXT();
        OP(0xEC):
            IO_CHECK(DX, 1);
            AL = (uint8_t)IO_IN(DX, 1);
            NEXT();
        OP(0xEE):
            IO_CHECK(DX, 1);
            IO_OUT(DX, 1, AL);
            BREAK();
            NEXT();

        OP(0xF0): /* LOCK */
            if (IS386 && !lock_ok(code + ip)) {
                goto invalid;
            }
            PREFIX();
        OP(0xF1):
            if (cpu.seg[CS] == 0xF000 && !cpu.pm) { /* BIOS hypercall: F1 nn */
                t = FETCH8();
                SYNC_OUT();
                pc_hypercall((uint8_t)t);
                SYNC_IN();
                BREAK();
                goto next;
            }
            if (cpu.model == CPU_8086) {
                PREFIX();
            }
            goto invalid;
        OP(0xF2): rep = 0xF2; PREFIX();
        OP(0xF3): rep = 0xF3; PREFIX();
        /* --- the coprocessor ---------------------------------------------------- */

        /* The most frequent instructions are done right here while the coprocessor is in
         * its ordinary state and the operands are ordinary numbers: see the fast path in
         * cpu_fpu.h, which this is the first part of. fpu_fast() takes more of them, and
         * fpu_exec() can do everything. */
#define FPU_ESC() \
    do { \
        MODRM(); \
        if (unlikely(cpu.cr0 & (CR0_EM | CR0_TS))) { \
            FAULT(EXC_NM); \
        } \
    } while (0)
#define FPU_NOTE(esc, mem) \
    do { \
        fpu.fip = cpu.insn_ip; \
        fpu.fcs = cpu.seg[CS]; \
        fpu.fop = (uint16_t)((((esc) & 7) << 8) | modrm); \
        if (mem) { \
            fpu.fdp = ea_off; \
            fpu.fds = cpu.seg[ea_seg - cpu.sg]; \
        } \
    } while (0)
#define FPU_EMPTY(slot) ((empty >> (slot)) & 1)
#define FPU_POP() \
    do { \
        fpu.empty = (uint8_t)(empty | (1u << top)); \
        fpu.top = (uint8_t)((top + 1) & 7); \
    } while (0)
#define FPU_IS_NUMBER(x) (FPU_NORMAL(x) || FPU_ZERO(x))
/* whether 4 or 8 bytes of the memory operand are in the segment's window */
#define FPU_IN4(win) (ea_off - ea_seg->lo < ea_seg->win[2])
#define FPU_IN8(win) (ea_off - ea_seg->lo < ea_seg->win[2] && ea_off - ea_seg->lo + 4 < ea_seg->win[2])

        OP(0xD8): /* arithmetic and comparison of ST(0) with a 32-bit real or ST(i) */
        OP(0xDC): /* with a 64-bit real; of ST(i) with ST(0) */
            FPU_ESC();
            if (IS_REG()) {
                if (likely(fpu.fast) && (reg & 6) != 2) {
                    const uint32_t top = fpu.top, empty = fpu.empty, si = (top + rm) & 7;
                    if (likely(!((FPU_EMPTY(top) | FPU_EMPTY(si))))) {
                        if (op == 0xD8 ? fpu_fast_op(reg, &fpu.st[top], &fpu.st[si])
                                       : fpu_fast_op(reg >= 4 ? reg ^ 1 : reg, &fpu.st[si], &fpu.st[top])) {
                            FPU_NOTE(op, 0);
                            NEXT();
                        }
                    }
                }
                goto fpu_general;
            }
            EA();
            if (likely(fpu.fast)) {
                const uint32_t top = fpu.top, empty = fpu.empty;
                fx_t x;
                if (unlikely(FPU_EMPTY(top))) {
                    goto fpu_general;
                }
                if (op == 0xD8) {
                    if (unlikely(!FPU_IN4(rd)) || !fpu_fast_f32(ld32(ea_seg->mem + ea_off), &x)) {
                        goto fpu_general;
                    }
                } else if (unlikely(!FPU_IN8(rd)) || !fpu_fast_f64(fpu_ld64(ea_seg->mem + ea_off), &x)) {
                    goto fpu_general;
                }
                if ((reg & 6) == 2) { /* FCOM, FCOMP */
                    if (!FPU_IS_NUMBER(fpu.st[top])) {
                        goto fpu_general;
                    }
                    fpu_set_cc(fpu_fast_cmp(fpu.st[top], x));
                    if (reg == 3) {
                        FPU_POP();
                    }
                } else if (!fpu_fast_op(reg, &fpu.st[top], &x)) {
                    goto fpu_general;
                }
                FPU_NOTE(op, 1);
                NEXT();
            }
            goto fpu_general;

        OP(0xD9):
            FPU_ESC();
            if (IS_REG()) {
                if (likely(fpu.fast)) {
                    const uint32_t top = fpu.top, empty = fpu.empty, si = (top + rm) & 7;
                    if (modrm < 0xC8) { /* FLD ST(i) */
                        const uint32_t slot = (top - 1) & 7;
                        if (likely(FPU_EMPTY(slot) && !FPU_EMPTY(si))) {
                            fpu.st[slot].m = fpu.st[si].m;
                            fpu.st[slot].se = fpu.st[si].se;
                            fpu.top = (uint8_t)slot;
                            fpu.empty = (uint8_t)(empty & ~(1u << slot));
                            FPU_NOTE(0xD9, 0);
                            NEXT();
                        }
                    } else if (modrm < 0xD0) { /* FXCH */
                        if (likely(!(FPU_EMPTY(top) | FPU_EMPTY(si)))) {
                            const uint64_t m = fpu.st[top].m;
                            const uint16_t se = fpu.st[top].se;
                            fpu.st[top].m = fpu.st[si].m;
                            fpu.st[top].se = fpu.st[si].se;
                            fpu.st[si].m = m;
                            fpu.st[si].se = se;
                            fpu.sw &= (uint16_t)~SW_C1;
                            FPU_NOTE(0xD9, 0);
                            NEXT();
                        }
                    } else if (modrm == 0xE0 || modrm == 0xE1) { /* FCHS, FABS */
                        if (likely(!FPU_EMPTY(top))) {
                            const uint32_t se = fpu.st[top].se;
                            fpu.st[top].se = (uint16_t)(modrm == 0xE0 ? se ^ 0x8000 : se & 0x7FFF);
                            fpu.sw &= (uint16_t)~SW_C1;
                            FPU_NOTE(0xD9, 0);
                            NEXT();
                        }
                    } else if (modrm == 0xE8 || modrm == 0xEE) { /* FLD1, FLDZ */
                        const uint32_t slot = (top - 1) & 7;
                        if (likely(FPU_EMPTY(slot))) {
                            fpu.st[slot].m = modrm == 0xE8 ? FX_J : 0;
                            fpu.st[slot].se = modrm == 0xE8 ? FX_BIAS : 0;
                            fpu.top = (uint8_t)slot;
                            fpu.empty = (uint8_t)(empty & ~(1u << slot));
                            fpu.sw &= (uint16_t)~SW_C1;
                            FPU_NOTE(0xD9, 0);
                            NEXT();
                        }
                    }
                }
                goto fpu_general;
            }
            EA();
            if (likely(fpu.fast)) {
                const uint32_t top = fpu.top, empty = fpu.empty;
                if (reg == 0) { /* FLD m32 */
                    const uint32_t slot = (top - 1) & 7;
                    fx_t x;
                    if (likely(FPU_IN4(rd) && FPU_EMPTY(slot)) && fpu_fast_f32(ld32(ea_seg->mem + ea_off), &x)) {
                        fpu.st[slot].m = x.m;
                        fpu.st[slot].se = x.se;
                        fpu.top = (uint8_t)slot;
                        fpu.empty = (uint8_t)(empty & ~(1u << slot));
                        FPU_NOTE(0xD9, 1);
                        NEXT();
                    }
                } else if ((reg & 6) == 2) { /* FST, FSTP m32 */
                    uint32_t v, fl;
                    if (likely(!FPU_EMPTY(top) && FPU_IN4(wr)) && fpu_fast_to_f32(&fpu.st[top], &v, &fl)) {
                        st32(ea_seg->mem + ea_off, v);
                        fpu.sw = (uint16_t)((fpu.sw & ~SW_C1) | fl);
                        if (reg == 3) {
                            FPU_POP();
                        }
                        FPU_NOTE(0xD9, 1);
                        NEXT();
                    }
                }
            }
            goto fpu_general;

        OP(0xDD):
            FPU_ESC();
            if (IS_REG()) {
                if (likely(fpu.fast) && (reg & 6) == 2) { /* FST, FSTP ST(i) */
                    const uint32_t top = fpu.top, si = (top + rm) & 7;
                    uint32_t empty = fpu.empty;
                    if (likely(!FPU_EMPTY(top))) {
                        fpu.st[si].m = fpu.st[top].m;
                        fpu.st[si].se = fpu.st[top].se;
                        empty &= ~(1u << si);
                        fpu.empty = (uint8_t)empty;
                        if (reg == 3) {
                            FPU_POP();
                        }
                        FPU_NOTE(0xDD, 0);
                        NEXT();
                    }
                }
                goto fpu_general;
            }
            EA();
            if (likely(fpu.fast)) {
                const uint32_t top = fpu.top, empty = fpu.empty;
                if (reg == 0) { /* FLD m64 */
                    const uint32_t slot = (top - 1) & 7;
                    fx_t x;
                    if (likely(FPU_IN8(rd) && FPU_EMPTY(slot)) && fpu_fast_f64(fpu_ld64(ea_seg->mem + ea_off), &x)) {
                        fpu.st[slot].m = x.m;
                        fpu.st[slot].se = x.se;
                        fpu.top = (uint8_t)slot;
                        fpu.empty = (uint8_t)(empty & ~(1u << slot));
                        FPU_NOTE(0xDD, 1);
                        NEXT();
                    }
                } else if ((reg & 6) == 2) { /* FST, FSTP m64 */
                    uint64_t v;
                    uint32_t fl;
                    if (likely(!FPU_EMPTY(top) && FPU_IN8(wr)) && FPU_IS_NUMBER(fpu.st[top]) &&
                        fpu_fast_real(fpu.st[top], 53, 11, &v, &fl)) {
                        fpu_st64(ea_seg->mem + ea_off, v);
                        fpu.sw = (uint16_t)((fpu.sw & ~SW_C1) | fl);
                        if (reg == 3) {
                            FPU_POP();
                        }
                        FPU_NOTE(0xDD, 1);
                        NEXT();
                    }
                }
            }
            goto fpu_general;

        OP(0xDE): /* arithmetic of ST(i) with ST(0), and pop */
            FPU_ESC();
            if (IS_REG()) {
                if (likely(fpu.fast) && (reg & 6) != 2) {
                    const uint32_t top = fpu.top, empty = fpu.empty, si = (top + rm) & 7;
                    if (likely(!(FPU_EMPTY(top) | FPU_EMPTY(si))) &&
                        fpu_fast_op(reg >= 4 ? reg ^ 1 : reg, &fpu.st[si], &fpu.st[top])) {
                        FPU_POP();
                        FPU_NOTE(0xDE, 0);
                        NEXT();
                    }
                }
                goto fpu_general;
            }
            EA();
            goto fpu_general;

        OP(0xDF):
            FPU_ESC();
            if (IS_REG()) {
                if (modrm == 0xE0 && likely(fpu.fast)) { /* FNSTSW AX */
                    AX = (uint16_t)fpu_status();
                    NEXT();
                }
                goto fpu_general;
            }
            EA();
            if (likely(fpu.fast)) {
                const uint32_t top = fpu.top, empty = fpu.empty;
                /* FILD and FISTP of 64 bits are also how the Watcom C library copies
                 * memory, to the screen (below the window of a flat segment) for one */
                if (reg == 7) { /* FISTP m64 */
                    uint8_t *const p = likely(FPU_IN8(wr)) ? ea_seg->mem + ea_off : fpu_low_mem(ea_seg, ea_off, 8, 1);
                    uint64_t v;
                    uint32_t fl;
                    if (likely(!FPU_EMPTY(top) && p) && FPU_IS_NUMBER(fpu.st[top]) &&
                        fpu_fast_int(fpu.st[top], 64, &v, &fl)) {
                        fpu_st64(p, v);
                        fpu.sw = (uint16_t)((fpu.sw & ~SW_C1) | fl);
                        FPU_POP();
                        FPU_NOTE(0xDF, 1);
                        NEXT();
                    }
                } else if (reg == 5) { /* FILD m64 */
                    const uint32_t slot = (top - 1) & 7;
                    const uint8_t *const p = likely(FPU_IN8(rd)) ? ea_seg->mem + ea_off : fpu_low_mem(ea_seg, ea_off, 8, 0);
                    if (likely(p && FPU_EMPTY(slot))) {
                        const int64_t n = (int64_t)fpu_ld64(p);
                        uint64_t m = 0;
                        uint32_t se = 0;
                        if (n) {
                            const uint64_t mag = n < 0 ? 0 - (uint64_t)n : (uint64_t)n;
                            const uint32_t z = fx_clz64(mag);
                            m = mag << z;
                            se = (n < 0 ? 0x8000u : 0) | (FX_BIAS + 63 - z);
                        }
                        fpu.st[slot].m = m;
                        fpu.st[slot].se = (uint16_t)se;
                        fpu.top = (uint8_t)slot;
                        fpu.empty = (uint8_t)(empty & ~(1u << slot));
                        FPU_NOTE(0xDF, 1);
                        NEXT();
                    }
                }
            }
            goto fpu_general;

        OP(0xDB):
            FPU_ESC();
            if (!IS_REG()) {
                EA();
                if (likely(fpu.fast) && reg == 0) { /* FILD m32 */
                    const uint32_t top = fpu.top, empty = fpu.empty, slot = (top - 1) & 7;
                    if (likely(FPU_IN4(rd) && FPU_EMPTY(slot))) {
                        const int32_t n = (int32_t)ld32(ea_seg->mem + ea_off);
                        uint32_t m = 0, se = 0;
                        if (n) {
                            const uint32_t mag = n < 0 ? 0 - (uint32_t)n : (uint32_t)n;
                            const uint32_t z = (uint32_t)__builtin_clz(mag);
                            m = mag << z;
                            se = (n < 0 ? 0x8000u : 0) | (FX_BIAS + 31 - z);
                        }
                        fpu.st[slot].m = (uint64_t)m << 32;
                        fpu.st[slot].se = (uint16_t)se;
                        fpu.top = (uint8_t)slot;
                        fpu.empty = (uint8_t)(empty & ~(1u << slot));
                        FPU_NOTE(0xDB, 1);
                        NEXT();
                    }
                }
            }
            goto fpu_general;

        OP(0xDA):
            FPU_ESC();
            if (!IS_REG()) {
                EA();
            }
        fpu_general:
            if (cpu_fpu_fitted && IS386) {
                const cpu_seg_t *const sg = IS_REG() ? NULL : ea_seg;
                if (!fpu_fast(op, modrm, sg, ea_off)) {
                    SYNC_OUT();
                    fpu_exec(op, modrm, sg, ea_off, sg ? eap->mask : 0,
                             (cpu.dmode ^ ((pfx & PFX_OP) ? 1u : 0)) & 1);
                    SYNC_IN();
                }
            }
            NEXT();

        OP(0xF4):
            if ((cpu.cr0 & CR0_PE) && cpu.cpl != 0) {
                FAULT_C(EXC_GP, 0);
            }
            cpu.halted = 1;
            BREAK();
            NEXT();
        OP(0xF5): set_cf(!GET_CF()); NEXT();

        OP(0xF6):
            DECODE();
            t = (reg == 2 || reg == 3) ? GET_RMW8() : GET_RM8();
            switch (reg) {
            case 0: case 1:
                u = FETCH8();
                f_and8(t, u);
                break;
            case 2: SET_RM8(~t); break;
            case 3:
                t = f_sub8(0, t);
                SET_RM8(t);
                break;
            case 4: /* MUL */
                t *= AL;
                AX = (uint16_t)t;
                u = (t >> 8) != 0;
                set_cf_of(u, u);
                cpu.f_ax = 0;
                SETZSP8(IS386 ? t : t >> 8);
                break;
            case 5: { /* IMUL */
                int32_t p = (int32_t)(int8_t)t * (int32_t)(int8_t)AL;
                AX = (uint16_t)p;
                u = p != (int8_t)p;
                set_cf_of(u, u);
                cpu.f_ax = 0;
                SETZSP8(IS386 ? (uint32_t)p : (uint32_t)p >> 8);
                break;
            }
            case 6: /* DIV */
                if (t == 0 || (u = AX / t) > 0xFF) {
                    goto div0;
                }
                AH = (uint8_t)(AX % t);
                AL = (uint8_t)u;
                break;
            default: { /* IDIV */
                int32_t d = (int8_t)t, nn = (int16_t)AX, q;
                if (d == 0) {
                    goto div0;
                }
                q = nn / d;
                if (q > 127 || q < (cpu.model == CPU_8086 ? -127 : -128)) {
                    goto div0;
                }
                if (cpu.model == CPU_8086 && rep) {
                    q = -q; /* 8086 quirk: a REP prefix negates the quotient */
                }
                AH = (uint8_t)(nn % d);
                AL = (uint8_t)q;
                break;
            }
            }
            NEXT();

        OP(0xF8): set_cf(0); NEXT();
        OP(0xF9): set_cf(1); NEXT();
        OP(0xFA):
            if (unlikely(!iopl_ok())) {
                FAULT_C(EXC_GP, 0);
            }
            cpu.f_if = 0;
            NEXT();
        OP(0xFB):
            if (unlikely(!iopl_ok())) {
                FAULT_C(EXC_GP, 0);
            }
            if (!cpu.f_if) {
                /* Interrupts are recognised only after the next instruction. */
                cpu.f_if = 1;
                goto inhibit_break;
            }
            NEXT();
        OP(0xFC): cpu.f_df = 0; NEXT();
        OP(0xFD): cpu.f_df = 1; NEXT();

        OP(0xFE):
            RM_OP_M(
                if (reg == 0) {
                    t = GET_RMW8();
                    SET_RM8(f_inc8(t));
                } else if (reg == 1) {
                    t = GET_RMW8();
                    SET_RM8(f_dec8(t));
                } else if (cpu.model != CPU_8086) {
                    goto invalid;
                }
            );

#define OS 16
#include "cpu_ops.h"
#define SSZ 16
#include "cpu_stk.h"
#undef SSZ
#define SSZ 32
#include "cpu_stk.h"
#undef SSZ
#undef OS
#define OS 32
#include "cpu_ops.h"
#define SSZ 16
#include "cpu_stk.h"
#undef SSZ
#define SSZ 32
#include "cpu_stk.h"
#undef SSZ
#undef OS

        div0:
            if (IS386) {
                FAULT(EXC_DE);
            }
            SYNC_OUT();
            do_interrupt(0, 0, 0, 0, ip);
            SYNC_IN();
            goto next;
        invalid:
            if (cpu.model == CPU_8086) {
                NEXT(); /* treated as a no-op */
            }
            if (IS386) {
                FAULT(EXC_UD);
            }
            ip = cpu.insn_ip;
            SYNC_OUT();
            pc_invalid_opcode((uint8_t)op);
            do_interrupt(6, 0, 0, 0, ip);
            SYNC_IN();
            goto next;
        too_long:
            if (IS386) {
                FAULT_C(EXC_GP, 0);
            }
            /* The 8086 takes any number of prefixes: carry on from here. */
            cpu.ip = ip;
            code_window();
            ip = cpu.ip;
            code = cpu.code;
            cpu.insn_ip = ip;
            goto prefix;
        inhibit:
            /* MOV SS / POP SS: the next instruction runs before any interrupt. */
            if (slice <= 0 && !cpu.test_mode) {
                slice = 1;
                cpu.saved -= 1;
            }
            goto next;
        inhibit_break:
            if (!cpu.test_mode) {
                cpu.saved += slice - 1;
                slice = 1;
            }
            goto next;
        next_jump:
            /* A jump out of the code segment faults in the jumping instruction. */
            if (IS386 && ip > cpu.sg[CS].limit) {
                FAULT_C(EXC_GP, 0);
            }
        next:
            if (pfx | rep) { /* prefixes end with their instruction */
                pfx = 0;
                rep = 0;
                for (int i = 0; i < 2; i++) {
                    ea_mode[i].seg[0] = SEG(DS);
                    ea_mode[i].seg[1] = SEG(SS);
                }
                eap = &ea_mode[cpu.dmode & 1];
                lim1 = cpu.lim1;
            }
        }
        SYNC_OUT();
    }
}

static void init_segs(void)
{
    for (int s = 0; s < 6; s++) {
        cpu.seg[s] = 0;
        cpu.sg[s].base = 0;
        cpu.sg[s].limit = 0xFFFF;
        cpu.sg[s].flags = CPU_SF_R | CPU_SF_W;
        cpu.sg[s].ar = 0x93;
    }
}

void cpu_init(int model)
{
    for (int i = 0; i < 256; i++) {
        int n = 0;
        for (int b = 0; b < 8; b++) {
            n += (i >> b) & 1;
        }
        parity_tab[i] = !(n & 1);
    }
    memset(&cpu, 0, sizeof(cpu));
    cpu.model = (uint8_t)model;
    cpu_reset();
}

void cpu_reset(void)
{
    /* What is not processor state survives: a reset can come from inside cpu_run(). */
    const uint8_t model = cpu.model, test = cpu.test_mode, in_run = cpu.in_run;
    const uint64_t ic = cpu.icount;
    const int32_t slice = cpu.slice, saved = cpu.saved, run_n = cpu.run_n;
    jmp_buf jb;
    memcpy(&jb, &cpu.exc_jmp, sizeof(jb));
    memset(&cpu, 0, sizeof(cpu));
    memcpy(&cpu.exc_jmp, &jb, sizeof(jb));
    cpu.model = model;
    cpu.test_mode = test;
    cpu.in_run = in_run;
    cpu.icount = ic;
    cpu.slice = slice;
    cpu.saved = saved;
    cpu.run_n = run_n;

    init_tables();
    init_segs();
    fpu_init();
    cpu.idt.limit = 0x3FF;
    cpu.gdt.limit = 0xFFFF;
    cpu.a20 = 1;
    cpu.a20_mask = IS386 ? 0xFFFFFFFFu : CPU_MEM_MASK;
    tlb_flush();
    set_eflags(2, 0xFFFFFFFFu);
    if (IS386) {
        EDX = 0x0308; /* 80386DX, stepping D1 */
        seg_load_real(CS, 0xF000);
        cpu.ip = 0xFFF0;
    } else {
        seg_load_real(CS, 0xFFFF);
    }
    all_windows();
}

int cpu_run(int n)
{
    volatile int budget = n;
    cpu.slice = 0;
    cpu.saved = n;
    cpu.run_n = n;
    cpu.in_run = 1;
    /* An exception abandons the instruction and comes back here, to enter its handler:
     * which may fault in turn. */
    while (setjmp(cpu.exc_jmp) != 0) {
        cpu.saved += cpu.slice; /* what was left of the slice the instruction ran in */
        cpu.slice = 0;
    }
    if (cpu.exc_pending) {
        exc_deliver();
    }
    run_loop();
    cpu.in_run = 0;

    n = budget - (cpu.saved + cpu.slice);
    cpu.saved = 0;
    cpu.slice = 0;
    cpu.run_n = 0;
    cpu.icount += (uint64_t)n;
    return n;
}

void cpu_interrupt(uint8_t vector)
{
    cpu.insn_ip = cpu.ip;
    do_interrupt(vector, 0, 0, 0, cpu.ip);
}
