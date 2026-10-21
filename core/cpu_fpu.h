/*
 * 80387 numeric coprocessor. Part of cpu.c; the arithmetic is in fx80.h.
 *
 * The registers hold 80-bit numbers and every result is rounded as the control word
 * says, so programs see what they would on the chip. The exception is the transcendental
 * functions (FSIN, FCOS, FSINCOS, FPTAN, FPATAN, FYL2X, FYL2XP1, F2XM1), which are
 * computed with the host's double-precision math library: 53 bits, not 64.
 *
 * An unmasked exception sets ES and B in the status word and is reported the way a PC
 * wires it: interrupt request 13.
 */
#include <math.h>

#include "fx80.h"

int cpu_fpu_fitted;

static struct {
    fx_t st[8];       /* the registers; ST(i) is st[(top + i) & 7] */
    uint16_t cw, sw;  /* sw without the TOP field */
    uint8_t top;
    uint8_t empty;    /* one bit per register */
    uint16_t fop, fcs, fds; /* last instruction: opcode, code and operand segment */
    uint32_t fip, fdp;      /* and their offsets */
    uint8_t fast;           /* the control word allows the fast path (see fpu_fast) */
    uint8_t rc;             /* rounding control: 0 to nearest */
    uint64_t rmask;         /* significand bits below the precision in force */
} fpu;

#define SW_SF 0x0040
#define SW_ES 0x0080
#define SW_C0 0x0100
#define SW_C1 0x0200
#define SW_C2 0x0400
#define SW_C3 0x4000
#define SW_B 0x8000
#define SW_CC (SW_C0 | SW_C1 | SW_C2 | SW_C3)

#define FST(i) fpu.st[(fpu.top + (i)) & 7]
#define FEMPTY(i) ((fpu.empty >> ((fpu.top + (i)) & 7)) & 1)

/* After anything that may have changed the control word or loaded the status word. */
static void fpu_mode(void)
{
    static const uint8_t drop[4] = {40, 0, 11, 0};
    const uint32_t n = drop[(fpu.cw >> 8) & 3];
    fpu.rmask = n ? (1ull << n) - 1 : 0;
    fpu.rc = (uint8_t)((fpu.cw >> 10) & 3);
    /* every exception masked and none pending */
    fpu.fast = cpu_fpu_fitted && IS386 && (fpu.cw & 0x003F) == 0x003F && !(fpu.sw & (SW_ES | SW_B));
}

static void fpu_init(void)
{
    memset(&fpu, 0, sizeof(fpu));
    fpu.cw = 0x037F;
    fpu.empty = 0xFF;
    fpu_mode();
}

static fx_env_t fpu_env(void)
{
    static const uint8_t pc[4] = {24, 64, 53, 64};
    fx_env_t e = {(uint8_t)((fpu.cw >> 10) & 3), pc[(fpu.cw >> 8) & 3], (uint8_t)(fpu.cw & 0x3F), 0};
    return e;
}

/* ES and B follow the unmasked exception flags; a new one pulls the interrupt line. */
static void fpu_check_es(void)
{
    if (fpu.sw & ~fpu.cw & 0x3F) {
        if (!(fpu.sw & SW_ES)) {
            fpu.sw |= SW_ES | SW_B;
            pc_fpu_error();
        }
    } else {
        fpu.sw &= (uint16_t)~(SW_ES | SW_B);
    }
}

/* Record what an operation raised (FX_* and SW_SF). Returns non-zero if an unmasked
 * invalid-operation, denormal-operand or zero-divide exception is among it: those stop
 * the instruction before it changes anything. */
static int fpu_raise(uint32_t flags)
{
    fpu.sw |= (uint16_t)(flags & 0x7F);
    if (flags & FX_P) {
        fpu.sw = (uint16_t)((fpu.sw & ~SW_C1) | ((flags & FX_UP) ? SW_C1 : 0));
    }
    fpu_check_es();
    return (flags & ~fpu.cw & (FX_I | FX_D | FX_Z)) != 0;
}

/* A stack fault: C1 says which way. Non-zero if unmasked. */
static int fpu_stack_fault(int overflow)
{
    fpu.sw = (uint16_t)((fpu.sw & ~SW_C1) | (overflow ? SW_C1 : 0));
    return fpu_raise(FX_I | SW_SF);
}

static void fpu_set_cc(uint32_t cc)
{
    fpu.sw = (uint16_t)((fpu.sw & ~SW_CC) | cc);
}

static void fpu_push(fx_t v)
{
    const uint32_t slot = (fpu.top - 1) & 7;
    if (!((fpu.empty >> slot) & 1)) {
        if (fpu_stack_fault(1)) {
            return;
        }
        v = fx_indefinite;
    }
    fpu.top = (uint8_t)slot;
    fpu.st[slot] = v;
    fpu.empty &= (uint8_t)~(1u << slot);
}

static void fpu_pop(void)
{
    fpu.empty |= (uint8_t)(1u << fpu.top);
    fpu.top = (fpu.top + 1) & 7;
}

static void fpu_set(uint32_t i, fx_t v)
{
    FST(i) = v;
    fpu.empty &= (uint8_t)~(1u << ((fpu.top + i) & 7));
}

/* --- memory operands ----------------------------------------------------------- */

typedef struct {
    const cpu_seg_t *sg;
    uint32_t off, mask;
} fmem_t;

static uint32_t fm_r16(const fmem_t *m, uint32_t d) { return sr16(m->sg, (m->off + d) & m->mask); }
static uint32_t fm_r32(const fmem_t *m, uint32_t d) { return sr32(m->sg, (m->off + d) & m->mask); }
static uint64_t fm_r64(const fmem_t *m, uint32_t d)
{
    const uint64_t lo = fm_r32(m, d);
    return lo | ((uint64_t)fm_r32(m, d + 4) << 32);
}
static void fm_w16(const fmem_t *m, uint32_t d, uint32_t v) { sw16(m->sg, (m->off + d) & m->mask, v); }
static void fm_w32(const fmem_t *m, uint32_t d, uint32_t v) { sw32(m->sg, (m->off + d) & m->mask, v); }
static void fm_w64(const fmem_t *m, uint32_t d, uint64_t v)
{
    fm_w32(m, d, (uint32_t)v);
    fm_w32(m, d + 4, (uint32_t)(v >> 32));
}
/* Make sure all of a longer store can be done before any of it is. */
static void fm_probe(const fmem_t *m, uint32_t len)
{
    (void)slow_rw(m->sg, m->off, 1);
    (void)slow_rw(m->sg, (m->off + len - 1) & m->mask, 1);
}
static fx_t fm_r80(const fmem_t *m, uint32_t d)
{
    fx_t v;
    v.m = fm_r64(m, d);
    v.se = (uint16_t)fm_r16(m, d + 8);
    return v;
}
static void fm_w80(const fmem_t *m, uint32_t d, fx_t v)
{
    fm_w64(m, d, v.m);
    fm_w16(m, d + 8, v.se);
}

/* --- arithmetic and comparison -------------------------------------------------- */

/* dst = ST(di): 0 dst + src, 1 dst * src, 4 dst - src, 5 src - dst, 6 dst / src,
 * 7 src / dst (the numbering of the opcodes' reg field). */
static void fpu_arith(uint32_t op, uint32_t di, fx_t src, int src_empty, int pop)
{
    fx_env_t e = fpu_env();
    fx_t r;
    if (FEMPTY(di) || src_empty) {
        if (fpu_stack_fault(0)) {
            return;
        }
        r = fx_indefinite;
    } else {
        const fx_t a = FST(di);
        switch (op) {
        case 0: r = fx_add(&e, a, src, 0); break;
        case 1: r = fx_mul(&e, a, src); break;
        case 4: r = fx_add(&e, a, src, 1); break;
        case 5: r = fx_add(&e, src, a, 1); break;
        case 6: r = fx_div(&e, a, src); break;
        default: r = fx_div(&e, src, a); break;
        }
        fpu.sw &= (uint16_t)~SW_C1;
        if (fpu_raise(e.flags)) {
            return;
        }
    }
    fpu_set(di, r);
    if (pop) {
        fpu_pop();
    }
}

static void fpu_compare(fx_t src, int src_empty, int quiet, int pops)
{
    fx_env_t e = fpu_env();
    int rel = FX_UN;
    if (FEMPTY(0) || src_empty) {
        fpu_set_cc(SW_C3 | SW_C2 | SW_C0);
        if (fpu_stack_fault(0)) {
            return;
        }
    } else {
        rel = fx_cmp(&e, FST(0), src, quiet);
        fpu_set_cc(rel == FX_UN ? SW_C3 | SW_C2 | SW_C0 : rel == FX_LT ? SW_C0 : rel == FX_EQ ? SW_C3 : 0);
        if (fpu_raise(e.flags)) {
            return;
        }
    }
    while (pops--) {
        fpu_pop();
    }
}

/* --- transcendental functions, with the host's doubles --------------------------- */

static const fx_t fx_one = {FX_J, FX_BIAS};

/* Constants; some are off by one unit in the last place depending on the rounding mode,
 * as on the chip (which rounds a longer internal value). */
static fx_t fpu_const(uint32_t which)
{
    static const uint64_t m[7] = {
        FX_J, 0xD49A784BCD1B8AFEull, 0xB8AA3B295C17F0BCull, 0xC90FDAA22168C235ull,
        0x9A209A84FBCFF799ull, 0xB17217F7D1CF79ACull, 0,
    };
    static const uint16_t se[7] = {0x3FFF, 0x4000, 0x3FFF, 0x4000, 0x3FFD, 0x3FFE, 0};
    const uint32_t rc = (fpu.cw >> 10) & 3;
    fx_t v = fx_make(0, se[which], m[which]);
    if (which == 1 && rc == 2) {
        v.m++;
    } else if (which >= 2 && which <= 5 && (rc & 1)) {
        v.m--;
    }
    return v;
}

/* NaNs, unsupported operands and denormals of a one-operand function. */
static int fpu_fn_special(fx_env_t *e, fx_t x, fx_t *r)
{
    return fx_special(e, x, x, r);
}

static fx_t fpu_from_double(fx_env_t *e, double d)
{
    e->flags |= FX_P;
    return fx_from_double(d);
}

/* 0 sin, 1 cos, 2 tan. *out_of_range: the operand is too large (C2), nothing is done. */
static fx_t fpu_trig(fx_env_t *e, fx_t x, int fn, int *out_of_range)
{
    fx_t r;
    double d;
    *out_of_range = 0;
    if (fpu_fn_special(e, x, &r)) {
        return r;
    }
    if (fx_is_inf(x)) {
        return fx_invalid(e);
    }
    if (fx_exp(x) >= FX_BIAS + 63) {
        *out_of_range = 1;
        return x;
    }
    if (fx_exp(x) < FX_BIAS - 32) { /* sin x = tan x = x, cos x = 1 to all 64 bits */
        if (!fx_is_zero(x)) {
            e->flags |= FX_P;
        }
        return fn == 1 ? fx_one : x;
    }
    d = fx_to_double(x);
    return fpu_from_double(e, fn == 0 ? sin(d) : fn == 1 ? cos(d) : tan(d));
}

/* log2(x) for finite x > 0, in extended precision as far as a double goes. */
static fx_t fpu_log2(fx_t x)
{
    int32_t ex;
    uint64_t mx;
    fx_env_t q = {0, 64, 0x3F, 0};
    fx_unpack(x, &ex, &mx);
    ex -= FX_BIAS;
    if (ex == 0 || ex == -1) {
        /* near 1: go by x - 1, which is exact, to keep the leading digits */
        const fx_t d = fx_add(&q, x, fx_one, 1);
        return fx_from_double(log1p(fx_to_double(d)) * 1.4426950408889634074);
    }
    return fx_from_double((double)ex + log2(fx_to_double(fx_make(0, FX_BIAS, mx))));
}

/* FYL2X: y * log2(x) */
static fx_t fpu_yl2x(fx_env_t *e, fx_t x, fx_t y)
{
    fx_t r;
    if (fx_special(e, x, y, &r)) {
        return r;
    }
    if (fx_is_zero(x)) {
        if (fx_is_zero(y)) {
            return fx_invalid(e);
        }
        e->flags = (uint16_t)((e->flags & ~FX_D) | FX_Z);
        return fx_make(!fx_sign(y), 0x7FFF, FX_J);
    }
    if (fx_sign(x)) {
        return fx_invalid(e);
    }
    if (fx_is_inf(x)) {
        return fx_is_zero(y) ? fx_invalid(e) : fx_make(fx_sign(y), 0x7FFF, FX_J);
    }
    if (fx_is_inf(y)) {
        const int rel = fx_cmp(e, x, fx_one, 1);
        return rel == FX_EQ ? fx_invalid(e) : fx_make(fx_sign(y) ^ (uint32_t)(rel == FX_LT), 0x7FFF, FX_J);
    }
    r = fx_mul(e, y, fpu_log2(x));
    e->flags |= FX_P;
    return r;
}

/* FYL2XP1: y * log2(x + 1), for x near zero */
static fx_t fpu_yl2xp1(fx_env_t *e, fx_t x, fx_t y)
{
    static const fx_t inv_ln2 = {0xB8AA3B295C17F0BCull, 0x3FFF};
    fx_t r;
    if (fx_special(e, x, y, &r)) {
        return r;
    }
    if (fx_is_inf(x) || fx_is_inf(y)) {
        if ((fx_is_inf(x) && fx_sign(x)) || fx_is_zero(x) || fx_is_zero(y)) {
            return fx_invalid(e);
        }
        return fx_make(fx_sign(x) ^ fx_sign(y), 0x7FFF, FX_J);
    }
    if (fx_is_zero(x)) {
        return fx_make(fx_sign(x) ^ fx_sign(y), 0, 0);
    }
    if (fx_exp(x) < FX_BIAS - 32) {
        r = fx_mul(e, x, inv_ln2); /* log2(1 + x) = x / ln 2 this close to zero */
    } else {
        const double d = fx_to_double(x);
        if (d <= -1.0) {
            return fx_invalid(e);
        }
        r = fx_from_double(log1p(d) * 1.4426950408889634074);
    }
    r = fx_mul(e, y, r);
    e->flags |= FX_P;
    return r;
}

/* F2XM1: 2^x - 1, for -1 <= x <= 1 */
static fx_t fpu_2xm1(fx_env_t *e, fx_t x)
{
    static const fx_t ln2 = {0xB17217F7D1CF79ACull, 0x3FFE};
    fx_t r;
    if (fpu_fn_special(e, x, &r)) {
        return r;
    }
    if (fx_is_zero(x)) {
        return x;
    }
    if (fx_is_inf(x)) {
        return fx_sign(x) ? fx_make(1, FX_BIAS, FX_J) : x;
    }
    if (fx_exp(x) < FX_BIAS - 32) {
        r = fx_mul(e, x, ln2);
        e->flags |= FX_P;
        return r;
    }
    return fpu_from_double(e, expm1(fx_to_double(x) * 0.69314718055994530942));
}

/* FPATAN: the angle of the point (x, y) */
static fx_t fpu_patan(fx_env_t *e, fx_t y, fx_t x)
{
    fx_env_t q = {0, 64, 0x3F, 0};
    fx_t r;
    double a;
    if (fx_special(e, y, x, &r)) {
        return r;
    }
    if (fx_is_zero(y)) {
        r = fx_sign(x) ? fpu_const(3) : fx_make(0, 0, 0);
        r.se = (uint16_t)(r.se | (fx_sign(y) << 15));
        return r;
    }
    if (fx_is_zero(x) || (fx_is_inf(y) && !fx_is_inf(x))) {
        r = fpu_const(3);
        r.se = (uint16_t)((r.se - 1) | (fx_sign(y) << 15)); /* pi / 2 */
        e->flags |= FX_P;
        return r;
    }
    if (fx_is_inf(x)) {
        if (fx_is_inf(y)) {
            a = fx_sign(x) ? 2.3561944901923449288 : 0.78539816339744830962;
        } else if (fx_sign(x)) {
            a = 3.14159265358979323846;
        } else {
            return fx_make(fx_sign(y), 0, 0);
        }
    } else {
        /* by the quotient, which is safe from the exponent range of a double */
        const fx_t t = fx_div(&q, y, x);
        const int32_t te = (int32_t)fx_exp(t) - FX_BIAS;
        if (te < -32 && !fx_sign(x)) {
            e->flags |= FX_P;
            return t; /* atan t = t this close to zero */
        }
        a = te > 1000 ? 1.57079632679489661923 : te < -1000 ? 0.0 : atan(fabs(fx_to_double(t)));
        if (fx_sign(x)) {
            a = 3.14159265358979323846 - a;
        }
    }
    r = fpu_from_double(e, a);
    r.se = (uint16_t)(r.se | (fx_sign(y) << 15));
    return r;
}

/* --- the environment and the whole state ------------------------------------------ */

static uint32_t fpu_tags(void)
{
    uint32_t tw = 0;
    for (uint32_t i = 0; i < 8; i++) {
        const fx_t v = fpu.st[i];
        const uint32_t tag = ((fpu.empty >> i) & 1) ? 3
                             : fx_is_zero(v) ? 1
                             : (fx_exp(v) == 0 || fx_exp(v) == 0x7FFF || !(v.m >> 63)) ? 2 : 0;
        tw |= tag << (2 * i);
    }
    return tw;
}

static uint32_t fpu_status(void)
{
    return fpu.sw | ((uint32_t)fpu.top << 11);
}

static void fpu_store_env(const fmem_t *m, int wide)
{
    const uint32_t step = wide ? 4 : 2;
    const uint32_t ip = cpu.pm ? fpu.fip : ((uint32_t)fpu.fcs << 4) + fpu.fip;
    const uint32_t dp = cpu.pm ? fpu.fdp : ((uint32_t)fpu.fds << 4) + fpu.fdp;
    uint32_t w[7];
    w[0] = fpu.cw;
    w[1] = fpu_status();
    w[2] = fpu_tags();
    if (cpu.pm) {
        w[3] = ip;
        w[4] = fpu.fcs | (wide ? (uint32_t)(fpu.fop & 0x7FF) << 16 : 0);
        w[5] = dp;
        w[6] = fpu.fds;
    } else if (wide) {
        w[3] = ip & 0xFFFF;
        w[4] = ((ip >> 16) << 12) | (fpu.fop & 0x7FF);
        w[5] = dp & 0xFFFF;
        w[6] = (dp >> 16) << 12;
    } else {
        w[3] = ip & 0xFFFF;
        w[4] = ((ip >> 4) & 0xF000) | (fpu.fop & 0x7FF);
        w[5] = dp & 0xFFFF;
        w[6] = (dp >> 4) & 0xF000;
    }
    for (uint32_t i = 0; i < 7; i++) {
        if (wide) {
            fm_w32(m, i * step, i < 3 ? w[i] | 0xFFFF0000u : w[i]);
        } else {
            fm_w16(m, i * step, w[i]);
        }
    }
}

static void fpu_load_env(const fmem_t *m, int wide)
{
    const uint32_t step = wide ? 4 : 2;
    uint32_t w[7], tw;
    for (uint32_t i = 0; i < 7; i++) {
        w[i] = wide ? fm_r32(m, i * step) : fm_r16(m, i * step);
    }
    fpu.cw = (uint16_t)w[0];
    fpu.sw = (uint16_t)(w[1] & ~0x3800u);
    fpu.top = (w[1] >> 11) & 7;
    tw = w[2];
    fpu.empty = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (((tw >> (2 * i)) & 3) == 3) {
            fpu.empty |= (uint8_t)(1u << i);
        }
    }
    if (cpu.pm) {
        fpu.fip = wide ? w[3] : w[3] & 0xFFFF;
        fpu.fcs = (uint16_t)w[4];
        fpu.fop = wide ? (w[4] >> 16) & 0x7FF : 0;
        fpu.fdp = wide ? w[5] : w[5] & 0xFFFF;
        fpu.fds = (uint16_t)w[6];
    } else {
        const uint32_t ip = wide ? (w[3] & 0xFFFF) | ((w[4] >> 12) << 16) : (w[3] & 0xFFFF) | ((w[4] & 0xF000) << 4);
        const uint32_t dp = wide ? (w[5] & 0xFFFF) | ((w[6] >> 12) << 16) : (w[5] & 0xFFFF) | ((w[6] & 0xF000) << 4);
        fpu.fop = w[4] & 0x7FF;
        fpu.fcs = 0;
        fpu.fip = ip;
        fpu.fds = 0;
        fpu.fdp = dp;
    }
    fpu.sw &= (uint16_t)~(SW_ES | SW_B);
    fpu_check_es();
}

/* --- stores ------------------------------------------------------------------------ */

/* FST/FSTP to a 32- or 64-bit real, FIST/FISTP to an integer, FBSTP. kind: 32, 64 (real),
 * 116, 132, 164 (integers), 80 (extended), 10 (packed decimal). */
static void fpu_store(const fmem_t *m, uint32_t kind, int pop)
{
    fx_env_t e = fpu_env();
    const int empty = FEMPTY(0);
    const fx_t x = empty ? fx_indefinite : FST(0);
    uint64_t v = 0;

    if (empty && fpu_stack_fault(0)) {
        return;
    }
    switch (kind) {
    case 32: v = fx_to_f32(&e, x); break;
    case 64: v = fx_to_f64(&e, x); break;
    case 116: v = (uint64_t)fx_to_int(&e, x, 16); break;
    case 132: v = (uint64_t)fx_to_int(&e, x, 32); break;
    case 164: v = (uint64_t)fx_to_int(&e, x, 64); break;
    case 10: {
        const int64_t i = fx_to_int(&e, x, 64);
        const uint64_t mag = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
        if ((e.flags & FX_I) || mag > 999999999999999999ull) {
            e.flags = FX_I;
        }
        v = mag;
        break;
    }
    default: break;
    }
    fpu.sw &= (uint16_t)~SW_C1;
    /* An unmasked invalid operation, overflow or underflow leaves memory and stack alone. */
    if (e.flags & ~fpu.cw & (FX_I | FX_O | FX_U)) {
        fpu_raise(e.flags & (FX_I | FX_O | FX_U | FX_D));
        return;
    }
    switch (kind) {
    case 32: fm_w32(m, 0, (uint32_t)v); break;
    case 64:
    case 164:
        fm_probe(m, 8);
        fm_w64(m, 0, v);
        break;
    case 116: fm_w16(m, 0, (uint32_t)v); break;
    case 132: fm_w32(m, 0, (uint32_t)v); break;
    case 80:
        fm_probe(m, 10);
        fm_w80(m, 0, x);
        break;
    default: {
        uint8_t b[10];
        fm_probe(m, 10);
        if (e.flags & FX_I) {
            static const uint8_t ind[10] = {0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0xFF};
            memcpy(b, ind, 10);
        } else {
            for (int i = 0; i < 9; i++) {
                b[i] = (uint8_t)(v % 10);
                v /= 10;
                b[i] |= (uint8_t)((v % 10) << 4);
                v /= 10;
            }
            b[9] = fx_sign(x) ? 0x80 : 0;
        }
        for (uint32_t i = 0; i < 10; i++) {
            sw8(m->sg, (m->off + i) & m->mask, b[i]);
        }
        break;
    }
    }
    fpu_raise(e.flags);
    if (pop) {
        fpu_pop();
    }
}

/* --- the instructions ------------------------------------------------------------- */

static void fpu_exec_mem(uint32_t op, uint32_t reg, const fmem_t *m, int wide)
{
    fx_env_t e = fpu_env();
    fx_t v;

    switch (op) {
    case 0xD8: /* arithmetic with a 32-bit real */
    case 0xDC: /* a 64-bit real */
    case 0xDA: /* a 32-bit integer */
    case 0xDE: /* a 16-bit integer */
        if (op == 0xD8) {
            v = fx_from_f32(&e, fm_r32(m, 0));
        } else if (op == 0xDC) {
            v = fx_from_f64(&e, fm_r64(m, 0));
        } else if (op == 0xDA) {
            v = fx_from_i64((int32_t)fm_r32(m, 0));
        } else {
            v = fx_from_i64((int16_t)fm_r16(m, 0));
        }
        if (reg == 2 || reg == 3) {
            if (!fpu_raise(e.flags & ~FX_D)) { /* fx_cmp reports the denormal */
                fpu_compare(v, 0, 0, reg == 3);
            }
        } else if (!fpu_raise(e.flags)) {
            fpu_arith(reg, 0, v, 0, 0);
        }
        return;
    case 0xD9:
        switch (reg) {
        case 0: /* FLD m32 */
            v = fx_from_f32(&e, fm_r32(m, 0));
            if (!fpu_raise(e.flags)) {
                fpu_push(v);
            }
            break;
        case 2: fpu_store(m, 32, 0); break;
        case 3: fpu_store(m, 32, 1); break;
        case 4: fpu_load_env(m, wide); break;
        case 5: /* FLDCW */
            fpu.cw = (uint16_t)fm_r16(m, 0);
            fpu.sw &= (uint16_t)~(SW_ES | SW_B);
            fpu_check_es();
            break;
        case 6: /* FNSTENV */
            fm_probe(m, wide ? 28 : 14);
            fpu_store_env(m, wide);
            fpu.cw |= 0x3F;
            fpu_check_es();
            break;
        case 7: fm_w16(m, 0, fpu.cw); break;
        default: break;
        }
        return;
    case 0xDB:
        switch (reg) {
        case 0: fpu_push(fx_from_i64((int32_t)fm_r32(m, 0))); break;
        case 2: fpu_store(m, 132, 0); break;
        case 3: fpu_store(m, 132, 1); break;
        case 5: fpu_push(fm_r80(m, 0)); break;
        case 7: fpu_store(m, 80, 1); break;
        default: break;
        }
        return;
    case 0xDD:
        switch (reg) {
        case 0: /* FLD m64 */
            v = fx_from_f64(&e, fm_r64(m, 0));
            if (!fpu_raise(e.flags)) {
                fpu_push(v);
            }
            break;
        case 2: fpu_store(m, 64, 0); break;
        case 3: fpu_store(m, 64, 1); break;
        case 4: { /* FRSTOR */
            const uint32_t base = wide ? 28 : 14;
            fx_t r[8];
            for (uint32_t i = 0; i < 8; i++) {
                r[i] = fm_r80(m, base + 10 * i);
            }
            fpu_load_env(m, wide);
            for (uint32_t i = 0; i < 8; i++) {
                FST(i) = r[i];
            }
            break;
        }
        case 6: { /* FNSAVE */
            const uint32_t base = wide ? 28 : 14;
            fm_probe(m, base + 80);
            fpu_store_env(m, wide);
            for (uint32_t i = 0; i < 8; i++) {
                fm_w80(m, base + 10 * i, FST(i));
            }
            fpu_init();
            break;
        }
        case 7: fm_w16(m, 0, fpu_status()); break;
        default: break;
        }
        return;
    default: /* DF */
        switch (reg) {
        case 0: fpu_push(fx_from_i64((int16_t)fm_r16(m, 0))); break;
        case 2: fpu_store(m, 116, 0); break;
        case 3: fpu_store(m, 116, 1); break;
        case 4: { /* FBLD */
            uint64_t n = 0;
            for (int i = 8; i >= 0; i--) {
                const uint32_t b = sr8(m->sg, (m->off + (uint32_t)i) & m->mask);
                n = n * 100 + (b >> 4) * 10 + (b & 15);
            }
            v = fx_from_i64((int64_t)n);
            v.se = (uint16_t)(v.se | ((sr8(m->sg, (m->off + 9) & m->mask) & 0x80) << 8));
            fpu_push(v);
            break;
        }
        case 5: fpu_push(fx_from_i64((int64_t)fm_r64(m, 0))); break;
        case 6: fpu_store(m, 10, 1); break;
        case 7: fpu_store(m, 164, 1); break;
        default: break;
        }
        return;
    }
}

/* One-operand functions on ST(0). */
static void fpu_unary(uint32_t fn)
{
    fx_env_t e = fpu_env();
    fx_t x, r;
    int flag = 0;
    if (FEMPTY(0)) {
        if (!fpu_stack_fault(0)) {
            fpu_set(0, fx_indefinite);
        }
        return;
    }
    x = FST(0);
    switch (fn) {
    case 0xF0: r = fpu_2xm1(&e, x); break;
    case 0xFA: r = fx_sqrt(&e, x); break;
    case 0xFC: r = fx_rndint(&e, x); break;
    case 0xFE: r = fpu_trig(&e, x, 0, &flag); break;
    default: r = fpu_trig(&e, x, 1, &flag); break;
    }
    fpu.sw = (uint16_t)((fpu.sw & ~(SW_C1 | SW_C2)) | (flag ? SW_C2 : 0));
    if (!fpu_raise(e.flags)) {
        fpu_set(0, r);
    }
}

static void fpu_exec_reg(uint32_t op, uint32_t modrm)
{
    const uint32_t i = modrm & 7, reg = (modrm >> 3) & 7;
    fx_env_t e = fpu_env();
    fx_t x, y, r;
    int flag = 0;

    switch (op) {
    case 0xD8:
        if (reg == 2 || reg == 3) {
            fpu_compare(FST(i), FEMPTY(i), 0, reg == 3);
        } else {
            fpu_arith(reg, 0, FST(i), FEMPTY(i), 0);
        }
        return;
    case 0xDC:
    case 0xDE:
        /* the destination is ST(i); subtraction and division swap their encodings */
        if (reg == 2 || reg == 3) {
            if (op == 0xDE && reg == 3 && i == 1) {
                fpu_compare(FST(1), FEMPTY(1), 0, 2); /* FCOMPP */
            } else {
                fpu_compare(FST(i), FEMPTY(i), 0, reg == 3 || op == 0xDE);
            }
        } else {
            fpu_arith(reg >= 4 ? reg ^ 1 : reg, i, FST(0), FEMPTY(0), op == 0xDE);
        }
        return;
    case 0xDA:
        if (modrm == 0xE9) { /* FUCOMPP */
            fpu_compare(FST(1), FEMPTY(1), 1, 2);
        }
        return;
    case 0xDB:
        if (modrm == 0xE2) { /* FNCLEX */
            fpu.sw &= 0x7F00;
        } else if (modrm == 0xE3) { /* FNINIT */
            fpu_init();
        }
        return; /* FENI, FDISI, FSETPM: nothing to do on an 80387 */
    case 0xDD:
        switch (reg) {
        case 0: fpu.empty |= (uint8_t)(1u << ((fpu.top + i) & 7)); break; /* FFREE */
        case 2:
        case 3: /* FST, FSTP ST(i) */
            if (FEMPTY(0)) {
                if (fpu_stack_fault(0)) {
                    return;
                }
                fpu_set(i, fx_indefinite);
            } else {
                fpu_set(i, FST(0));
            }
            if (reg == 3) {
                fpu_pop();
            }
            break;
        case 4: fpu_compare(FST(i), FEMPTY(i), 1, 0); break; /* FUCOM */
        case 5: fpu_compare(FST(i), FEMPTY(i), 1, 1); break; /* FUCOMP */
        default: break;
        }
        return;
    case 0xDF:
        if (modrm == 0xE0) { /* FNSTSW AX */
            AX = (uint16_t)fpu_status();
        } else if (reg == 0) { /* FFREE and pop (undocumented) */
            fpu.empty |= (uint8_t)(1u << ((fpu.top + i) & 7));
            fpu_pop();
        }
        return;
    default: break; /* D9 */
    }

    if (modrm < 0xC8) { /* FLD ST(i) */
        if (FEMPTY(i)) {
            if (fpu_stack_fault(0)) {
                return;
            }
            fpu_push(fx_indefinite);
        } else {
            fpu_push(FST(i));
        }
        return;
    }
    if (modrm < 0xD0) { /* FXCH */
        if (FEMPTY(0) || FEMPTY(i)) {
            if (fpu_stack_fault(0)) {
                return;
            }
            if (FEMPTY(0)) {
                fpu_set(0, fx_indefinite);
            }
            if (FEMPTY(i)) {
                fpu_set(i, fx_indefinite);
            }
        }
        x = FST(0);
        FST(0) = FST(i);
        FST(i) = x;
        fpu.sw &= (uint16_t)~SW_C1;
        return;
    }
    if (modrm < 0xE0) {
        if (modrm >= 0xD8) { /* FSTP ST(i), an alias */
            fpu_exec_reg(0xDD, modrm);
        }
        return; /* FNOP */
    }
    x = FEMPTY(0) ? fx_indefinite : FST(0);
    y = FEMPTY(1) ? fx_indefinite : FST(1);
    switch (modrm) {
    case 0xE0: /* FCHS */
    case 0xE1: /* FABS */
        if (FEMPTY(0)) {
            if (!fpu_stack_fault(0)) {
                fpu_set(0, fx_indefinite);
            }
            return;
        }
        FST(0).se = (uint16_t)(modrm == 0xE0 ? x.se ^ 0x8000 : x.se & 0x7FFF);
        fpu.sw &= (uint16_t)~SW_C1;
        return;
    case 0xE4: /* FTST */
        if (FEMPTY(0)) {
            fpu_set_cc(SW_C3 | SW_C2 | SW_C0);
            fpu_stack_fault(0);
        } else {
            fpu_compare(fx_make(0, 0, 0), 0, 0, 0);
        }
        return;
    case 0xE5: { /* FXAM */
        uint32_t cc;
        if (FEMPTY(0)) {
            cc = SW_C3 | SW_C0;
        } else if (fx_is_zero(x)) {
            cc = SW_C3;
        } else if (fx_is_unsup(x)) {
            cc = 0;
        } else if (fx_is_inf(x)) {
            cc = SW_C2 | SW_C0;
        } else if (fx_exp(x) == 0x7FFF) {
            cc = SW_C0;
        } else if (fx_exp(x) == 0) {
            cc = SW_C3 | SW_C2;
        } else {
            cc = SW_C2;
        }
        fpu_set_cc(cc | (fx_sign(FST(0)) ? SW_C1 : 0));
        return;
    }
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE:
        fpu.sw &= (uint16_t)~SW_C1;
        fpu_push(fpu_const(modrm - 0xE8));
        return;
    case 0xF0: case 0xFA: case 0xFC: case 0xFE: case 0xFF:
        fpu_unary(modrm);
        return;
    case 0xF6: /* FDECSTP */
        fpu.top = (fpu.top - 1) & 7;
        fpu.sw &= (uint16_t)~SW_C1;
        return;
    case 0xF7: /* FINCSTP */
        fpu.top = (fpu.top + 1) & 7;
        fpu.sw &= (uint16_t)~SW_C1;
        return;
    default: break;
    }

    /* What is left works on ST(0) and ST(1), or turns ST(0) into two results. */
    if (modrm == 0xF2 || modrm == 0xF4 || modrm == 0xFB) {
        /* FPTAN, FXTRACT, FSINCOS: one in, two out */
        if (FEMPTY(0) || !FEMPTY(7)) {
            if (!fpu_stack_fault(FEMPTY(0) ? 0 : 1)) {
                fpu_set(0, fx_indefinite);
                fpu_push(fx_indefinite);
            }
            return;
        }
        if (modrm == 0xF4) { /* FXTRACT: exponent, then significand on top */
            if (fx_special(&e, x, x, &r)) {
                y = r;
            } else if (fx_is_zero(x)) {
                e.flags |= FX_Z;
                r = fx_make(1, 0x7FFF, FX_J);
                y = x;
            } else if (fx_is_inf(x)) {
                r = fx_make(0, 0x7FFF, FX_J);
                y = x;
            } else {
                int32_t ex;
                uint64_t mx;
                fx_unpack(x, &ex, &mx);
                r = fx_from_i64(ex - FX_BIAS);
                y = fx_make(fx_sign(x), FX_BIAS, mx);
            }
        } else {
            r = fpu_trig(&e, x, modrm == 0xF2 ? 2 : 0, &flag);
            y = modrm == 0xF2 ? fx_one : fpu_trig(&e, x, 1, &flag);
            if (fx_is_nan(r)) {
                y = r;
            }
        }
        fpu.sw = (uint16_t)((fpu.sw & ~(SW_C1 | SW_C2)) | (flag ? SW_C2 : 0));
        if (flag || fpu_raise(e.flags)) {
            return;
        }
        fpu_set(0, r);
        fpu_push(y);
        return;
    }
    if (FEMPTY(0) || FEMPTY(1)) {
        if (!fpu_stack_fault(0)) {
            if (modrm == 0xF1 || modrm == 0xF3 || modrm == 0xF9) {
                fpu_set(1, fx_indefinite);
                fpu_pop();
            } else {
                fpu_set(0, fx_indefinite);
            }
        }
        return;
    }
    fpu.sw &= (uint16_t)~SW_C1;
    switch (modrm) {
    case 0xF1: /* FYL2X */
    case 0xF3: /* FPATAN */
    case 0xF9: /* FYL2XP1 */
        r = modrm == 0xF1 ? fpu_yl2x(&e, x, y) : modrm == 0xF3 ? fpu_patan(&e, y, x) : fpu_yl2xp1(&e, x, y);
        if (!fpu_raise(e.flags)) {
            fpu_set(1, r);
            fpu_pop();
        }
        return;
    case 0xF5: /* FPREM1 */
    case 0xF8: { /* FPREM */
        uint32_t q = 0;
        r = fx_rem(&e, x, y, modrm == 0xF5, &q, &flag);
        if (fpu_raise(e.flags)) {
            return;
        }
        fpu_set_cc(flag ? SW_C2 : ((q & 4) ? SW_C0 : 0) | ((q & 2) ? SW_C3 : 0) | ((q & 1) ? SW_C1 : 0));
        fpu_set(0, r);
        return;
    }
    case 0xFD: /* FSCALE */
        r = fx_scale(&e, x, y);
        if (!fpu_raise(e.flags)) {
            fpu_set(0, r);
        }
        return;
    default: return;
    }
}

/* --- the fast path ----------------------------------------------------------------- */

/* Programs run with every exception masked, on ordinary numbers. For that case the
 * frequent instructions are done here, in a fraction of the time the general code above
 * takes and with the same results, bit for bit (tests/fpu_fast_test.c holds the two
 * against each other). Anything else - a special operand, a result near the ends of the
 * exponent range, an empty or a full register, memory that is not plain RAM - is declined
 * before anything has changed, and left to fpu_exec(). Nothing here can fault. */

#define FPU_NORMAL(x) ((uint32_t)(((x).se & 0x7FFF) - 1) < 0x7FFE && (int64_t)(x).m < 0)
#define FPU_ZERO(x) (!((x).se & 0x7FFF) && !(x).m)
#define FPU_SLOT(i) ((fpu.top + (i)) & 7)

/* Whether a result that is not exact is rounded away from zero, in the rounding modes
 * other than to nearest. */
static inline int fpu_dir_up(uint32_t sign)
{
    return fpu.rc != 3 && (fpu.rc == 1) == (int)sign;
}

/* Round the significand hi (integer bit on top), with lo behind it, to the precision in
 * force. Returns FX_P and FX_UP as they apply. */
static inline uint32_t fpu_rnd(uint64_t *hi_io, uint64_t lo, int32_t *exp, uint32_t sign)
{
    const uint64_t mask = fpu.rmask;
    uint64_t hi = *hi_io;
    if (mask) {
        const uint64_t drop = hi & mask, half = (mask >> 1) + 1;
        if (!(drop | lo)) {
            return 0;
        }
        hi &= ~mask;
        if (fpu.rc ? !fpu_dir_up(sign) : drop < half || (drop == half && !lo && !(hi & (mask + 1)))) {
            *hi_io = hi;
            return FX_P;
        }
        hi += mask + 1;
    } else {
        if (!lo) {
            return 0;
        }
        if (fpu.rc ? !fpu_dir_up(sign) : lo < FX_J || (lo == FX_J && !(hi & 1))) {
            return FX_P;
        }
        hi++;
    }
    if (!hi) {
        hi = FX_J;
        ++*exp;
    }
    *hi_io = hi;
    return FX_P | FX_UP;
}

/* hi:lo / d for hi < d and d with its top bit set: the quotient, and the remainder in *r.
 * (Long division in 32-bit digits, each from a 64-by-32 division.) */
static uint64_t fpu_div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *r)
{
    const uint64_t b = 1ull << 32, d1 = d >> 32, d0 = (uint32_t)d;
    const uint64_t n1 = lo >> 32, n0 = (uint32_t)lo;
    uint64_t q1, q0, rhat, mid;

    q1 = hi / d1;
    rhat = hi - q1 * d1;
    while (q1 >= b || q1 * d0 > ((rhat << 32) | n1)) {
        q1--;
        rhat += d1;
        if (rhat >= b) {
            break;
        }
    }
    mid = (hi << 32) + n1 - q1 * d;
    q0 = mid / d1;
    rhat = mid - q0 * d1;
    while (q0 >= b || q0 * d0 > ((rhat << 32) | n0)) {
        q0--;
        rhat += d1;
        if (rhat >= b) {
            break;
        }
    }
    *r = (mid << 32) + n0 - q0 * d;
    return (q1 << 32) | q0;
}

/* A register and another operand, normal numbers or zeros: *d + *s (fn 0), *d * *s (1),
 * *d - *s (4), *s - *d (5), *d / *s (6) or *s / *d (7) becomes the new *d. 0: declined,
 * nothing changed. */
static NOINLINE int fpu_fast_op(uint32_t fn, fx_t *d, const fx_t *s)
{
    /* a and b: the operands in the order of the operation */
    const int swap = fn >= 4 && (fn & 1);
    const fx_t *const pa = swap ? s : d, *const pb = swap ? d : s;
    const uint32_t ase = pa->se, bse = pb->se;
    const uint64_t ma = pa->m, mb = pb->m;
    uint32_t sa = ase >> 15, sb = bse >> 15;
    int32_t ea = (int32_t)(ase & 0x7FFF), eb = (int32_t)(bse & 0x7FFF);
    const int na = (uint32_t)(ea - 1) < 0x7FFE && (int64_t)ma < 0;
    const int nb = (uint32_t)(eb - 1) < 0x7FFE && (int64_t)mb < 0;
    uint64_t hi, lo = 0;
    uint32_t fl;

    if (unlikely(!(na & nb))) {
        if (!(na || !(ea | (ma != 0))) || !(nb || !(eb | (mb != 0)))) {
            return 0; /* not a number of the ordinary kind */
        }
        if (fn == 1 || (fn >= 6 && nb)) { /* a product with zero, zero divided by something */
            sa ^= sb;
            goto zero;
        }
        if (fn >= 6) {
            return 0; /* a division by zero */
        }
        sb ^= fn >= 4;
        if (!(na | nb)) { /* zeros: of the same sign they add up to that one */
            sa = sa == sb ? sa : fpu.rc == 1;
            goto zero;
        }
        /* x + 0 is x, at the precision in force */
        if (na) {
            hi = ma;
        } else {
            hi = mb;
            ea = eb;
            sa = sb;
        }
    } else if (fn == 1) {
        const uint32_t a0 = (uint32_t)ma, a1 = (uint32_t)(ma >> 32);
        const uint32_t b0 = (uint32_t)mb, b1 = (uint32_t)(mb >> 32);
        hi = (uint64_t)a1 * b1;
        /* the low halves are zero in a number that came from a 32-bit real */
        if (a0 | b0) {
            const uint64_t p01 = (uint64_t)a0 * b1, p10 = (uint64_t)a1 * b0;
            const uint64_t mid = p01 + (uint32_t)p10 + (b0 && a0 ? (uint64_t)a0 * b0 >> 32 : 0);
            lo = (mid << 32) | (b0 && a0 ? (uint32_t)(a0 * b0) : 0);
            hi += (mid >> 32) + (p10 >> 32);
        }
        ea += eb - FX_BIAS + 1;
        if (!(hi >> 63)) {
            hi = (hi << 1) | (lo >> 63);
            lo <<= 1;
            ea--;
        }
        sa ^= sb;
    } else if (fn >= 6) {
        uint64_t rem;
        ea += FX_BIAS - eb;
        if (ma >= mb) {
            hi = fpu_div128(ma >> 1, ma << 63, mb, &rem);
        } else {
            hi = fpu_div128(ma, 0, mb, &rem);
            ea--;
        }
        /* what is left over, as far as rounding wants to know: more than, exactly or
         * less than half a unit in the last place */
        if (rem) {
            lo = rem > mb - rem ? FX_J | 1 : rem == mb - rem ? FX_J : 1;
        }
        sa ^= sb;
    } else {
        uint64_t x = ma, y = mb;
        uint32_t n;
        sb ^= fn >= 4;
        if (ea < eb || (ea == eb && x < y)) {
            const int32_t te = ea;
            const uint32_t ts = sa;
            ea = eb, eb = te;
            x = mb, y = ma;
            sa = sb, sb = ts;
        }
        n = (uint32_t)(ea - eb);
        if (n) {
            if (n > 63) {
                return 0;
            }
            lo = y << (64 - n);
            y >>= n;
        }
        if (sa == sb) {
            hi = x + y;
            if (hi < x) { /* carry out: one place to the right */
                lo = (hi << 63) | (lo >> 1) | (lo & 1);
                hi = (hi >> 1) | FX_J;
                ea++;
            }
        } else {
            hi = x - y - (lo != 0);
            lo = 0 - lo;
            if (!(hi >> 63)) {
                if (!hi) {
                    if (lo) {
                        return 0; /* next to nothing left */
                    }
                    sa = fpu.rc == 1; /* nothing: the difference of equals */
                    goto zero;
                }
                n = fx_clz64(hi);
                hi = (hi << n) | (lo >> (64 - n));
                lo <<= n;
                ea -= (int32_t)n;
            }
        }
    }
    if (ea <= 0 || ea >= 0x7FFE) {
        return 0;
    }
    fl = fpu_rnd(&hi, lo, &ea, sa);
    d->m = hi;
    d->se = (uint16_t)((sa << 15) | (uint32_t)ea);
    fpu.sw = (uint16_t)((fpu.sw & ~SW_C1) | fl);
    return 1;

zero:
    d->m = 0;
    d->se = (uint16_t)(sa << 15);
    fpu.sw &= (uint16_t)~SW_C1;
    return 1;
}

/* The condition codes of a comparison of normal numbers and zeros. */
static inline uint32_t fpu_fast_cmp(fx_t a, fx_t b)
{
    const uint32_t sa = a.se >> 15, ea = a.se & 0x7FFF, eb = b.se & 0x7FFF;
    int lt;
    if (!(ea | eb)) {
        return SW_C3;
    }
    if (sa != (uint32_t)(b.se >> 15)) {
        return sa ? SW_C0 : 0;
    }
    if (ea == eb && a.m == b.m) {
        return SW_C3;
    }
    lt = ea < eb || (ea == eb && a.m < b.m);
    return lt != (int)sa ? SW_C0 : 0;
}

/* A 32- or 64-bit real that is a normal number or zero. */
static inline int fpu_fast_f32(uint32_t v, fx_t *r)
{
    const uint32_t e = (v >> 23) & 0xFF;
    if (likely(e - 1 < 0xFE)) {
        r->m = (uint64_t)((v << 8) | 0x80000000u) << 32;
        r->se = (uint16_t)(((v >> 31) << 15) | (e + (FX_BIAS - 127)));
        return 1;
    }
    r->m = 0;
    r->se = (uint16_t)((v >> 31) << 15);
    return !(v << 1);
}

static inline int fpu_fast_f64(uint64_t v, fx_t *r)
{
    const uint32_t h = (uint32_t)(v >> 32), e = (h >> 20) & 0x7FF;
    if (likely(e - 1 < 0x7FE)) {
        r->m = (v << 11) | FX_J;
        r->se = (uint16_t)(((h >> 31) << 15) | (e + (FX_BIAS - 1023)));
        return 1;
    }
    r->m = 0;
    r->se = (uint16_t)((h >> 31) << 15);
    return !(v << 1);
}

/* x as a real of 24 or 53 significant bits in a field of ebits exponent bits; x is a
 * normal number or zero. 0: out of the format's normal range. */
static inline int fpu_fast_real(fx_t x, uint32_t pbits, uint32_t ebits, uint64_t *out, uint32_t *fl)
{
    const uint32_t emax = (1u << ebits) - 1;
    const uint64_t sign = (uint64_t)(x.se >> 15) << (pbits - 1 + ebits);
    int32_t e = (int32_t)(x.se & 0x7FFF) - FX_BIAS + (int32_t)(emax >> 1);
    const uint32_t rbits = 64 - pbits;
    const uint64_t drop = x.m & ((1ull << rbits) - 1), half = 1ull << (rbits - 1);
    uint64_t f = x.m >> rbits;

    *fl = 0;
    if (!(x.se & 0x7FFF)) {
        *out = sign;
        return 1;
    }
    if (e <= 0 || e >= (int32_t)emax - 1) {
        return 0;
    }
    if (drop) {
        *fl = FX_P;
        if (fpu.rc ? fpu_dir_up(x.se >> 15) : drop > half || (drop == half && (f & 1))) {
            *fl = FX_P | FX_UP;
            f++;
            if (f >> pbits) {
                f >>= 1;
                e++;
            }
        }
    }
    *out = sign | ((uint64_t)(uint32_t)e << (pbits - 1)) | (f & ((1ull << (pbits - 1)) - 1));
    return 1;
}

/* *x as a 32-bit real, if it is a normal number that stays one, or zero. */
static inline int fpu_fast_to_f32(const fx_t *x, uint32_t *out, uint32_t *fl)
{
    const uint32_t se = x->se, e = se & 0x7FFF, hi = (uint32_t)(x->m >> 32), lo = (uint32_t)x->m;
    const int32_t fe = (int32_t)e - (FX_BIAS - 127);
    const uint32_t rest = (hi << 24) | (lo != 0); /* what is dropped, the half bit on top */
    uint32_t f = hi >> 8, bits;

    *fl = 0;
    if (!e) {
        *out = (se >> 15) << 31;
        return !(hi | lo);
    }
    if (!(hi >> 31) || fe <= 0 || fe >= 0xFE) {
        return 0;
    }
    bits = ((se >> 15) << 31) + ((uint32_t)fe << 23) + (f & 0x7FFFFF);
    if (rest) {
        *fl = FX_P;
        if (fpu.rc ? fpu_dir_up(se >> 15) : rest > 0x80000000u || (rest == 0x80000000u && (f & 1))) {
            *fl = FX_P | FX_UP;
            bits++; /* a carry out of the fraction lands in the exponent, as it must */
        }
    }
    *out = bits;
    return 1;
}

/* x, a normal number or zero, as an integer of the given size. 0: it does not fit. */
static inline int fpu_fast_int(fx_t x, uint32_t bits, uint64_t *out, uint32_t *fl)
{
    const uint32_t sign = x.se >> 15, e = x.se & 0x7FFF;
    uint64_t hi = 0, lo = 1;
    uint32_t n;

    *fl = 0;
    if (!e) {
        *out = 0;
        return 1;
    }
    if (e > FX_BIAS + 62) {
        return 0;
    }
    n = FX_BIAS + 63 - e;
    if (n < 64) {
        hi = x.m >> n;
        lo = x.m << (64 - n);
    } else if (n == 64) {
        lo = x.m;
    }
    if (lo) {
        *fl = FX_P;
        if (fpu.rc ? fpu_dir_up(sign) : lo > FX_J || (lo == FX_J && (hi & 1))) {
            *fl = FX_P | FX_UP;
            hi++;
        }
    }
    if (hi > (1ull << (bits - 1)) - (sign ? 0 : 1)) {
        return 0;
    }
    *out = sign ? 0 - hi : hi;
    return 1;
}

/* The same outside the window, for what is plain RAM in the first megabyte all the same:
 * a flat 32-bit segment has its window on extended memory, and the display memory, for
 * one, is below it. */
static NOINLINE uint8_t *fpu_low_mem(const cpu_seg_t *sg, uint32_t off, uint32_t n, int write)
{
    const uint32_t lin = sg->base + off, top = write ? ram_wr : ram_rd;
    if ((cpu.cr0 & CR0_PG) || (sg->flags & CPU_SF_DOWN) || !(sg->flags & (write ? CPU_SF_W : CPU_SF_R))) {
        return 0;
    }
    if (off > sg->limit || sg->limit - off < n - 1 || lin >= top || top - lin < n) {
        return 0;
    }
    return cpu_mem + lin;
}

/* Host address of n bytes (2, 4 or 8) of a memory operand, if they are in the segment's
 * window or otherwise plain RAM. */
static inline uint8_t *fpu_fast_mem(const cpu_seg_t *sg, uint32_t off, uint32_t n, int write)
{
    const uint32_t a = off - sg->lo;
    const uint32_t *w = write ? sg->wr : sg->rd;
    if (likely(n == 8 ? (a < w[2] && a + 4 < w[2]) : a < w[n >> 1])) {
        return sg->mem + off;
    }
    return fpu_low_mem(sg, off, n, write);
}

static inline uint64_t fpu_ld64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline void fpu_st64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

/* An escape opcode and its ModRM byte, as for fpu_exec(). Non-zero if it was executed. */
static NOINLINE int fpu_fast(uint32_t op, uint32_t modrm, const cpu_seg_t *sg, uint32_t off)
{
    const uint32_t reg = (modrm >> 3) & 7, i = modrm & 7;
    const uint32_t top = fpu.top, st0 = top, empty = fpu.empty;
    uint32_t fl = 0, cc;
    uint64_t v;
    uint8_t *p;
    fx_t x;

    if (unlikely(!fpu.fast)) {
        return 0;
    }
#define ST0_FULL() (!((empty >> st0) & 1))
#define FULL(n) (!((empty >> FPU_SLOT(n)) & 1))
#define NUMBER(x) (FPU_NORMAL(x) || FPU_ZERO(x))
#define POP() \
    do { \
        fpu.empty = (uint8_t)(fpu.empty | (1u << fpu.top)); \
        fpu.top = (fpu.top + 1) & 7; \
    } while (0)
#define PUSH(val) \
    do { \
        const uint32_t slot_ = (top - 1) & 7; \
        if (!((empty >> slot_) & 1)) { \
            return 0; \
        } \
        fpu.st[slot_] = (val); \
        fpu.top = (uint8_t)slot_; \
        fpu.empty = (uint8_t)(empty & ~(1u << slot_)); \
    } while (0)
/* the pointers of the last instruction; not for those that only move control words */
#define NOTE() \
    do { \
        fpu.fip = cpu.insn_ip; \
        fpu.fcs = cpu.seg[CS]; \
        fpu.fop = (uint16_t)(((op & 7) << 8) | modrm); \
        if (sg) { \
            fpu.fdp = off; \
            fpu.fds = cpu.seg[sg - cpu.sg]; \
        } \
    } while (0)

    if (sg) {
        switch (((op & 7) << 3) | reg) {
        case 000: case 001: case 004: case 005: case 006: case 007: /* FADD, FMUL, FSUB, FSUBR, FDIV, FDIVR m32 */
        case 040: case 041: case 044: case 045: case 046: case 047: /* and m64 */
        case 002: case 003: case 042: case 043: /* FCOM, FCOMP */
            if (op == 0xD8) {
                if (!(p = fpu_fast_mem(sg, off, 4, 0)) || !fpu_fast_f32(ld32(p), &x)) {
                    return 0;
                }
            } else if (!(p = fpu_fast_mem(sg, off, 8, 0)) || !fpu_fast_f64(fpu_ld64(p), &x)) {
                return 0;
            }
            if (!ST0_FULL()) {
                return 0;
            }
            if ((reg & 6) == 2) {
                if (!NUMBER(fpu.st[st0])) {
                    return 0;
                }
                cc = fpu_fast_cmp(fpu.st[st0], x);
                goto compared;
            }
            if (!fpu_fast_op(reg, &fpu.st[st0], &x)) {
                return 0;
            }
            NOTE();
            return 1;
        case 010: /* FLD m32 */
            if (!(p = fpu_fast_mem(sg, off, 4, 0)) || !fpu_fast_f32(ld32(p), &x)) {
                return 0;
            }
            PUSH(x);
            NOTE();
            return 1;
        case 050: /* FLD m64 */
            if (!(p = fpu_fast_mem(sg, off, 8, 0)) || !fpu_fast_f64(fpu_ld64(p), &x)) {
                return 0;
            }
            PUSH(x);
            NOTE();
            return 1;
        case 012: case 013: /* FST, FSTP m32 */
            if (!ST0_FULL() || !(p = fpu_fast_mem(sg, off, 4, 1)) || !fpu_fast_to_f32(&fpu.st[st0], &cc, &fl)) {
                return 0;
            }
            st32(p, cc);
            goto stored;
        case 052: case 053: /* FST, FSTP m64 */
            x = fpu.st[st0];
            if (!ST0_FULL() || !NUMBER(x) || !(p = fpu_fast_mem(sg, off, 8, 1)) ||
                !fpu_fast_real(x, 53, 11, &v, &fl)) {
                return 0;
            }
            fpu_st64(p, v);
            goto stored;
        case 030: /* FILD m32 */
        case 070: /* FILD m16 */
        case 075: { /* FILD m64 */
            int64_t n;
            uint64_t mag;
            uint32_t z;
            if (reg == 5) {
                if (!(p = fpu_fast_mem(sg, off, 8, 0))) {
                    return 0;
                }
                n = (int64_t)fpu_ld64(p);
            } else if (op == 0xDB) {
                if (!(p = fpu_fast_mem(sg, off, 4, 0))) {
                    return 0;
                }
                n = (int32_t)ld32(p);
            } else {
                if (!(p = fpu_fast_mem(sg, off, 2, 0))) {
                    return 0;
                }
                n = (int16_t)ld16(p);
            }
            x.m = 0;
            x.se = 0;
            if (n) {
                mag = n < 0 ? 0 - (uint64_t)n : (uint64_t)n;
                z = fx_clz64(mag);
                x.m = mag << z;
                x.se = (uint16_t)((n < 0 ? 0x8000u : 0) | (FX_BIAS + 63 - z));
            }
            PUSH(x);
            NOTE();
            return 1;
        }
        case 032: case 033: /* FIST, FISTP m32 */
            x = fpu.st[st0];
            if (!ST0_FULL() || !NUMBER(x) || !(p = fpu_fast_mem(sg, off, 4, 1)) ||
                !fpu_fast_int(x, 32, &v, &fl)) {
                return 0;
            }
            st32(p, (uint32_t)v);
            goto stored;
        case 072: case 073: /* FIST, FISTP m16 */
            x = fpu.st[st0];
            if (!ST0_FULL() || !NUMBER(x) || !(p = fpu_fast_mem(sg, off, 2, 1)) ||
                !fpu_fast_int(x, 16, &v, &fl)) {
                return 0;
            }
            st16(p, (uint32_t)v);
            goto stored;
        case 077: /* FISTP m64 */
            x = fpu.st[st0];
            if (!ST0_FULL() || !NUMBER(x) || !(p = fpu_fast_mem(sg, off, 8, 1)) ||
                !fpu_fast_int(x, 64, &v, &fl)) {
                return 0;
            }
            fpu_st64(p, v);
            goto stored;
        case 017: /* FNSTCW */
            if (!(p = fpu_fast_mem(sg, off, 2, 1))) {
                return 0;
            }
            st16(p, fpu.cw);
            return 1;
        case 057: /* FNSTSW */
            if (!(p = fpu_fast_mem(sg, off, 2, 1))) {
                return 0;
            }
            st16(p, fpu_status());
            return 1;
        default:
            return 0;
        }
    stored:
        NOTE();
        fpu.sw = (uint16_t)((fpu.sw & ~SW_C1) | fl);
        if (reg != 2) {
            POP();
        }
        return 1;
    }

    switch (op & 7) {
    case 0: /* D8: ST(0) and ST(i) */
    case 4: /* DC: ST(i) and ST(0) */
    case 6: { /* DE: the same, and pop */
        const uint32_t sti = FPU_SLOT(i);
        uint32_t dst = st0, fn = reg;
        if (!ST0_FULL() || !FULL(i)) {
            return 0;
        }
        if ((reg & 6) == 2) {
            if (op == 0xDE && (reg != 3 || i != 1)) {
                return 0; /* FCOMPP is the only one of these that is documented */
            }
            if (!NUMBER(fpu.st[st0]) || !NUMBER(fpu.st[sti])) {
                return 0;
            }
            cc = fpu_fast_cmp(fpu.st[st0], fpu.st[sti]);
            NOTE();
            fpu_set_cc(cc);
            if (reg == 3) {
                POP();
            }
            if (op == 0xDE) {
                POP();
            }
            return 1;
        }
        if (op != 0xD8) {
            dst = sti;
            fn = reg >= 4 ? reg ^ 1 : reg;
        }
        /* fn: what becomes of the destination, with ST(0) or ST(i) as the other operand */
        if (!fpu_fast_op(fn, &fpu.st[dst], &fpu.st[dst ^ st0 ^ sti])) {
            return 0;
        }
        NOTE();
        if (op == 0xDE) {
            POP();
        }
        return 1;
    }
    case 1: /* D9 */
        if (modrm < 0xC8) { /* FLD ST(i) */
            if (!FULL(i)) {
                return 0;
            }
            x = fpu.st[FPU_SLOT(i)];
            PUSH(x);
            NOTE();
            return 1;
        }
        if (modrm < 0xD0) { /* FXCH */
            const uint32_t sti = FPU_SLOT(i);
            if (!ST0_FULL() || !FULL(i)) {
                return 0;
            }
            x = fpu.st[st0];
            fpu.st[st0] = fpu.st[sti];
            fpu.st[sti] = x;
            fpu.sw &= (uint16_t)~SW_C1;
            NOTE();
            return 1;
        }
        if (modrm >= 0xD8 && modrm < 0xE0) { /* FSTP ST(i), an alias */
            goto store_reg;
        }
        switch (modrm) {
        case 0xE0: /* FCHS */
        case 0xE1: /* FABS */
            if (!ST0_FULL()) {
                return 0;
            }
            fpu.st[st0].se = (uint16_t)(modrm == 0xE0 ? fpu.st[st0].se ^ 0x8000 : fpu.st[st0].se & 0x7FFF);
            fpu.sw &= (uint16_t)~SW_C1;
            NOTE();
            return 1;
        case 0xE4: /* FTST */
            if (!ST0_FULL() || !NUMBER(fpu.st[st0])) {
                return 0;
            }
            x.m = 0;
            x.se = 0;
            cc = fpu_fast_cmp(fpu.st[st0], x);
            NOTE();
            fpu_set_cc(cc);
            return 1;
        case 0xE8: /* FLD1 */
        case 0xEE: /* FLDZ */
            x.m = modrm == 0xE8 ? FX_J : 0;
            x.se = modrm == 0xE8 ? FX_BIAS : 0;
            PUSH(x);
            fpu.sw &= (uint16_t)~SW_C1;
            NOTE();
            return 1;
        default:
            return 0;
        }
    case 2: /* DA */
        if (modrm != 0xE9) { /* FUCOMPP */
            return 0;
        }
        if (!ST0_FULL() || !FULL(1) || !NUMBER(fpu.st[st0]) || !NUMBER(fpu.st[FPU_SLOT(1)])) {
            return 0;
        }
        cc = fpu_fast_cmp(fpu.st[st0], fpu.st[FPU_SLOT(1)]);
        NOTE();
        fpu_set_cc(cc);
        POP();
        POP();
        return 1;
    case 5: /* DD */
        if (reg == 2 || reg == 3) { /* FST, FSTP ST(i) */
        store_reg:
            if (!ST0_FULL()) {
                return 0;
            }
            fpu.st[FPU_SLOT(i)] = fpu.st[st0];
            fpu.empty = (uint8_t)(empty & ~(1u << FPU_SLOT(i)));
            NOTE();
            if (reg == 3) {
                POP();
            }
            return 1;
        }
        if (reg == 4 || reg == 5) { /* FUCOM, FUCOMP */
            if (!ST0_FULL() || !FULL(i) || !NUMBER(fpu.st[st0]) || !NUMBER(fpu.st[FPU_SLOT(i)])) {
                return 0;
            }
            cc = fpu_fast_cmp(fpu.st[st0], fpu.st[FPU_SLOT(i)]);
            NOTE();
            fpu_set_cc(cc);
            if (reg == 5) {
                POP();
            }
            return 1;
        }
        return 0;
    case 7: /* DF */
        if (modrm == 0xE0) { /* FNSTSW AX */
            AX = (uint16_t)fpu_status();
            return 1;
        }
        return 0;
    default:
        return 0;
    }

compared:
    NOTE();
    fpu_set_cc(cc);
    if (reg == 3) {
        POP();
    }
    return 1;
#undef ST0_FULL
#undef FULL
#undef NUMBER
#undef POP
#undef PUSH
#undef NOTE
}

/* An escape opcode (D8-DF) with its ModRM byte; sg is NULL for the register forms. */
static NOINLINE void fpu_exec(uint32_t op, uint32_t modrm, const cpu_seg_t *sg, uint32_t off,
                              uint32_t mask, int wide)
{
    const uint32_t reg = (modrm >> 3) & 7;
    /* The instructions that only move control information leave the pointers alone. */
    const int control = sg ? ((op == 0xD9 && reg >= 4) || (op == 0xDD && (reg == 4 || reg >= 6)))
                           : (op == 0xDB || (op == 0xDF && modrm == 0xE0));
    if (!control) {
        fpu.fip = cpu.insn_ip;
        fpu.fcs = cpu.seg[CS];
        fpu.fop = (uint16_t)(((op & 7) << 8) | modrm);
        if (sg) {
            fpu.fdp = off;
            fpu.fds = cpu.seg[sg - cpu.sg];
        }
    }
    if (sg) {
        const fmem_t m = {sg, off, mask};
        fpu_exec_mem(op, reg, &m, wide);
    } else {
        fpu_exec_reg(op, modrm);
    }
    fpu_mode();
}
