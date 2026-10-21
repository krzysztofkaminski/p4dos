/*
 * Checks core/fx80.h against the x87 unit of the machine this runs on (x86 only): the
 * same operations on the same operands must give the same bits and the same exception
 * flags, in every rounding mode and precision. usage: fx80_test [CASES [SEED]]
 */
#include <stdio.h>
#include <stdlib.h>

#include "../core/fx80.h"

#if !defined(__x86_64__) && !defined(__i386__)
#error "this test compares against a real x87: build it on x86"
#endif

typedef struct __attribute__((packed)) {
    uint64_t m;
    uint16_t se;
} t80;

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static fx_t random_fx(void)
{
    fx_t v;
    const uint32_t kind = (uint32_t)(rnd() % 32);
    uint64_t m = rnd();
    uint32_t exp;
    switch (rnd() % 6) { /* significands with structure */
    case 0: m &= 0xFFFFFF0000000000ull; break;
    case 1: m &= 0xFFFFFFFFFFFFF800ull; break;
    case 2: m = (m & 3) ? ~0ull << (rnd() % 64) : ~(~0ull << (rnd() % 64)); break;
    case 3: m = 1ull << (rnd() % 64); break;
    default: break;
    }
    if (kind == 0) {
        exp = 0, m = 0;
    } else if (kind == 1) {
        exp = 0x7FFF, m = FX_J;
    } else if (kind == 2) {
        exp = 0x7FFF, m |= FX_J; /* NaN, quiet or signalling */
    } else if (kind == 3) {
        exp = 0; /* denormal or pseudo-denormal */
    } else if (kind == 4) {
        exp = 1 + (uint32_t)(rnd() % 0x7FFE), m &= ~FX_J; /* unnormal */
    } else if (kind < 12) {
        exp = FX_BIAS - 70 + (uint32_t)(rnd() % 140), m |= FX_J;
    } else if (kind < 16) {
        exp = (rnd() & 1) ? 1 + (uint32_t)(rnd() % 80) : 0x7FFE - (uint32_t)(rnd() % 80), m |= FX_J;
    } else if (kind < 20) {
        exp = FX_BIAS - 1100 + (uint32_t)(rnd() % 2200), m |= FX_J; /* around the double range */
    } else if (kind < 23) {
        exp = FX_BIAS - 160 + (uint32_t)(rnd() % 320), m |= FX_J; /* around the single range */
    } else {
        exp = 1 + (uint32_t)(rnd() % 0x7FFE), m |= FX_J;
    }
    v.m = m;
    v.se = (uint16_t)(exp | ((rnd() & 1) << 15));
    return v;
}

static uint16_t host_cw(uint32_t rc, uint32_t pc)
{
    return (uint16_t)(0x003F | (rc << 10) | ((pc == 24 ? 0u : pc == 53 ? 2u : 3u) << 8));
}

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SQRT, OP_RNDINT, OP_PREM, OP_PREM1, OP_SCALE, OP_COM,
       OP_UCOM, OP_TO64, OP_TO32, OP_FROM64, OP_FROM32, OP_IST16, OP_IST32, OP_IST64, OP_ILD64,
       OP_COUNT };
static const char *const op_name[OP_COUNT] = {
    "add", "sub", "mul", "div", "sqrt", "rndint", "prem", "prem1", "scale", "com", "ucom",
    "to f64", "to f32", "from f64", "from f32", "to i16", "to i32", "to i64", "from i64"};

typedef struct {
    t80 r;        /* result, for the operations that have an extended one */
    uint64_t aux; /* other results: memory image, condition codes */
    uint16_t sw;
} outcome_t;

/* The status word is taken right after the operation: the store that follows has its
 * own say about C1. */
#define X87(pre, op, post) \
    __asm__ volatile("fnclex\n fldcw %[cw]\n" pre "\n" op "\n fnstsw %[sw]\n" post "\n fnclex" \
                     : [sw] "=m"(sw), [r] "=m"(o.r), [aux] "=m"(aux) \
                     : [cw] "m"(cw), [a] "m"(a), [b] "m"(b), [in] "m"(in))
#define LD2 "fldt %[b]\n fldt %[a]"
#define ST2 "fstpt %[r]\n fstp %%st(0)"

static outcome_t host_op(int op, fx_t fa, fx_t fb, uint32_t rc, uint32_t pc)
{
    static const uint16_t cw_default = 0x037F;
    const uint16_t cw = host_cw(rc, pc);
    t80 a = {fa.m, fa.se}, b = {fb.m, fb.se};
    uint64_t in = fa.m, aux = 0;
    uint16_t sw = 0;
    outcome_t o;
    memset(&o, 0, sizeof(o));
    switch (op) {
    case OP_ADD: X87(LD2, "fadd %%st(1), %%st", ST2); break;
    case OP_SUB: X87(LD2, "fsub %%st(1), %%st", ST2); break;
    case OP_MUL: X87(LD2, "fmul %%st(1), %%st", ST2); break;
    case OP_DIV: X87(LD2, "fdiv %%st(1), %%st", ST2); break;
    case OP_SQRT: X87("fldt %[a]", "fsqrt", "fstpt %[r]"); break;
    case OP_RNDINT: X87("fldt %[a]", "frndint", "fstpt %[r]"); break;
    case OP_PREM: X87(LD2, "fprem", ST2); break;
    case OP_PREM1: X87(LD2, "fprem1", ST2); break;
    case OP_SCALE: X87(LD2, "fscale", ST2); break;
    case OP_COM: X87(LD2, "fcom %%st(1)", "fstp %%st(0)\n fstp %%st(0)"); break;
    case OP_UCOM: X87(LD2, "fucom %%st(1)", "fstp %%st(0)\n fstp %%st(0)"); break;
    case OP_TO64: X87("fldt %[a]", "fstpl %[aux]", ""); break;
    case OP_TO32: X87("fldt %[a]", "fstps %[aux]", ""); break;
    case OP_FROM64: X87("", "fldl %[in]", "fstpt %[r]"); break;
    case OP_FROM32: X87("", "flds %[in]", "fstpt %[r]"); break;
    case OP_IST16: X87("fldt %[a]", "fistps %[aux]", ""); break;
    case OP_IST32: X87("fldt %[a]", "fistpl %[aux]", ""); break;
    case OP_IST64: X87("fldt %[a]", "fistpll %[aux]", ""); break;
    default: X87("", "fildll %[in]", "fstpt %[r]"); break;
    }
    __asm__ volatile("fninit\n fldcw %0" : : "m"(cw_default));
    o.aux = aux;
    o.sw = sw;
    return o;
}

static outcome_t soft_op(int op, fx_t a, fx_t b, uint32_t rc, uint32_t pc)
{
    fx_env_t e = {(uint8_t)rc, (uint8_t)pc, 0x3F, 0};
    outcome_t o;
    fx_t r = {0, 0};
    uint32_t q = 0, cc = 0;
    int partial = 0, rel;
    memset(&o, 0, sizeof(o));
    switch (op) {
    case OP_ADD: r = fx_add(&e, a, b, 0); break;
    case OP_SUB: r = fx_add(&e, a, b, 1); break;
    case OP_MUL: r = fx_mul(&e, a, b); break;
    case OP_DIV: r = fx_div(&e, a, b); break;
    case OP_SQRT: r = fx_sqrt(&e, a); break;
    case OP_RNDINT: r = fx_rndint(&e, a); break;
    case OP_PREM:
    case OP_PREM1:
        r = fx_rem(&e, a, b, op == OP_PREM1, &q, &partial);
        if (partial) {
            cc = 0x400;
        } else if (!(e.flags & FX_I) && !fx_is_nan(r)) {
            cc = ((q & 4) ? 0x100 : 0) | ((q & 2) ? 0x4000 : 0) | ((q & 1) ? 0x200 : 0);
        }
        break;
    case OP_SCALE: r = fx_scale(&e, a, b); break;
    case OP_COM:
    case OP_UCOM:
        rel = fx_cmp(&e, a, b, op == OP_UCOM);
        cc = rel == FX_UN ? 0x4500 : rel == FX_LT ? 0x100 : rel == FX_EQ ? 0x4000 : 0;
        break;
    case OP_TO64: o.aux = fx_to_f64(&e, a); break;
    case OP_TO32: o.aux = fx_to_f32(&e, a); break;
    case OP_FROM64: r = fx_from_f64(&e, a.m); break;
    case OP_FROM32: r = fx_from_f32(&e, (uint32_t)a.m); break;
    case OP_IST16: o.aux = (uint16_t)fx_to_int(&e, a, 16); break;
    case OP_IST32: o.aux = (uint32_t)fx_to_int(&e, a, 32); break;
    case OP_IST64: o.aux = (uint64_t)fx_to_int(&e, a, 64); break;
    default: r = fx_from_i64((int64_t)a.m); break;
    }
    o.r.m = r.m;
    o.r.se = r.se;
    o.sw = (uint16_t)((e.flags & 0x3F) | cc);
    if (op != OP_PREM && op != OP_PREM1 && op != OP_COM && op != OP_UCOM && (e.flags & FX_UP)) {
        o.sw |= 0x200;
    }
    return o;
}

int main(int argc, char **argv)
{
    static const uint32_t pcs[3] = {24, 53, 64};
    const long cases = argc > 1 ? atol(argv[1]) : 200000;
    long bad[OP_COUNT] = {0}, total = 0, failed = 0;
    if (argc > 2) {
        rng_state = strtoull(argv[2], NULL, 0) | 1;
    }
    for (long i = 0; i < cases; i++) {
        fx_t a = random_fx(), b = random_fx();
        if (rnd() % 4 == 0) { /* operands close together: cancellation, small quotients */
            b = a;
            b.se = (uint16_t)(b.se ^ ((rnd() & 1) << 15));
            if (fx_exp(b) > 70 && fx_exp(b) < 0x7FF0) {
                b.se = (uint16_t)(b.se + (int)(rnd() % 5) - 2);
            }
            b.m ^= (rnd() % 4 == 0) ? 0 : 1ull << (rnd() % 63);
        }
        for (int op = 0; op < OP_COUNT; op++) {
            const uint32_t rc = (uint32_t)(rnd() % 4), pc = pcs[rnd() % 3];
            /* flags to compare: the six exceptions, and C1 or the condition codes */
            uint16_t mask = 0x023F;
            fx_t x = a;
            outcome_t h, s;
            if (op == OP_FROM32) {
                x.m &= 0xFFFFFFFFu;
            }
            if (op == OP_PREM || op == OP_PREM1 || op == OP_COM || op == OP_UCOM) {
                mask = 0x473F;
            }
            h = host_op(op, x, b, rc, pc);
            s = soft_op(op, x, b, rc, pc);
            if ((op == OP_PREM || op == OP_PREM1) && (h.sw & s.sw & 0x400)) {
                /* A partial remainder: how far one step goes is up to the processor.
                 * Carry both on to the end, which must be the same. */
                for (int k = 0; k < 2000 && (h.sw & 0x400); k++) {
                    fx_t y = {h.r.m, h.r.se};
                    h = host_op(op, y, b, rc, pc);
                }
                for (int k = 0; k < 2000 && (s.sw & 0x400); k++) {
                    fx_t y = {s.r.m, s.r.se};
                    s = soft_op(op, y, b, rc, pc);
                }
                /* the quotient bits of the last step differ too, and so does whether
                 * its operand happened to be a denormal */
                mask = 0x043D;
            }
            if (!(h.sw & 0x20) && op != OP_PREM && op != OP_PREM1 && op != OP_COM && op != OP_UCOM) {
                mask &= ~0x200; /* C1 only says something about a rounded result */
            }
            total++;
            if (h.r.m != s.r.m || h.r.se != s.r.se || h.aux != s.aux || ((h.sw ^ s.sw) & mask)) {
                failed++;
                if (bad[op]++ < 3) {
                    printf("%-8s rc=%u pc=%u a=%04X:%016llX b=%04X:%016llX\n"
                           "    x87  %04X:%016llX aux=%016llX sw=%04X\n"
                           "    soft %04X:%016llX aux=%016llX sw=%04X\n",
                           op_name[op], rc, pc, x.se, (unsigned long long)x.m, b.se,
                           (unsigned long long)b.m, h.r.se, (unsigned long long)h.r.m,
                           (unsigned long long)h.aux, h.sw & mask, s.r.se,
                           (unsigned long long)s.r.m, (unsigned long long)s.aux, s.sw & mask);
                }
            }
        }
    }
    for (int op = 0; op < OP_COUNT; op++) {
        if (bad[op]) {
            printf("%-8s %ld mismatches\n", op_name[op], bad[op]);
        }
    }
    printf("%ld operations compared with the x87 of this machine, %ld differ\n", total, failed);
    return failed != 0;
}
