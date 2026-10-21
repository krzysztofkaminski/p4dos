/*
 * 80-bit extended-precision floating point in software, as the 80387 does it: the
 * arithmetic underneath the coprocessor emulation in cpu_fpu.h. Everything is exact and
 * correctly rounded for the rounding mode and precision asked for; the exceptions an
 * operation raises are collected in the environment it is given.
 */
#ifndef P4DOS_FX80_H
#define P4DOS_FX80_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t m;  /* significand, with its explicit integer bit on top */
    uint16_t se; /* sign and biased exponent */
} fx_t;

/* Exceptions, as in the status word. FX_UP: the result was rounded away from zero in
 * magnitude (what condition code C1 reports). */
#define FX_I 0x01
#define FX_D 0x02
#define FX_Z 0x04
#define FX_O 0x08
#define FX_U 0x10
#define FX_P 0x20
#define FX_UP 0x200

typedef struct {
    uint8_t rc;     /* rounding: 0 to nearest even, 1 down, 2 up, 3 toward zero */
    uint8_t pc;     /* precision of results: 24, 53 or 64 bits */
    uint8_t mask;   /* exceptions that are masked */
    uint16_t flags; /* raised so far */
} fx_env_t;

#define FX_BIAS 0x3FFF
#define FX_J 0x8000000000000000ull

static const fx_t fx_indefinite = {0xC000000000000000ull, 0xFFFF};

static inline uint32_t fx_exp(fx_t a) { return a.se & 0x7FFF; }
static inline uint32_t fx_sign(fx_t a) { return a.se >> 15; }
static inline int fx_is_zero(fx_t a) { return fx_exp(a) == 0 && a.m == 0; }
static inline int fx_is_inf(fx_t a) { return fx_exp(a) == 0x7FFF && a.m == FX_J; }
static inline int fx_is_nan(fx_t a) { return fx_exp(a) == 0x7FFF && (a.m >> 63) && (a.m << 1); }
static inline int fx_is_snan(fx_t a) { return fx_is_nan(a) && !(a.m & 0x4000000000000000ull); }
/* Unnormals, pseudo-NaNs and pseudo-infinities: the 80387 does not accept them. */
static inline int fx_is_unsup(fx_t a) { return fx_exp(a) != 0 && !(a.m >> 63); }
static inline int fx_is_denorm(fx_t a) { return fx_exp(a) == 0 && a.m != 0; }

static inline fx_t fx_make(uint32_t sign, uint32_t exp, uint64_t m)
{
    fx_t r;
    r.m = m;
    r.se = (uint16_t)((sign << 15) | exp);
    return r;
}

static inline uint32_t fx_clz64(uint64_t v)
{
    return (v >> 32) ? (uint32_t)__builtin_clz((uint32_t)(v >> 32))
                     : 32 + (uint32_t)__builtin_clz((uint32_t)v);
}

/* hi:lo >>= n, with everything that falls off ORed into the lowest bit. */
static void fx_shr_sticky(uint64_t *hi, uint64_t *lo, uint32_t n)
{
    if (n == 0) {
        return;
    }
    if (n < 64) {
        const uint64_t lost = *lo << (64 - n);
        *lo = (*hi << (64 - n)) | (*lo >> n) | (lost != 0);
        *hi >>= n;
    } else if (n < 128) {
        const uint64_t lost = *lo | (n > 64 ? *hi << (128 - n) : 0);
        *lo = (*hi >> (n - 64)) | (lost != 0);
        *hi = 0;
    } else {
        *lo = (*hi | *lo) != 0;
        *hi = 0;
    }
}

static void fx_mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    const uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = p01 + (p00 >> 32);
    const uint64_t mid2 = p10 + (uint32_t)mid;
    *lo = (mid2 << 32) | (uint32_t)p00;
    *hi = p11 + (mid >> 32) + (mid2 >> 32);
}

/* Whether hi (of which the low rbits bits are dropped) and lo behind it round up. */
static int fx_round_inc(uint32_t rc, uint32_t sign, uint64_t hi, uint64_t lo, uint32_t rbits)
{
    const uint64_t unit = 1ull << rbits, drop = hi & (unit - 1);
    if (!(drop | lo)) {
        return 0;
    }
    switch (rc) {
    case 0:
        if (rbits == 0) {
            return lo > FX_J || (lo == FX_J && (hi & 1));
        }
        return drop > (unit >> 1) || (drop == (unit >> 1) && (lo || (hi & unit)));
    case 1: return (int)sign;
    case 2: return !sign;
    default: return 0;
    }
}

/* Round the value hi.lo * 2^(exp - emid) to pbits significant bits for a format whose
 * biased exponent field ranges up to emax (the value is hi's top bit times 2^(exp - bias)
 * when normalised). Leaves the significand in *hi (left-aligned, integer bit on top) and
 * the exponent field in *exp. wrap: what to adjust the exponent by on an unmasked
 * overflow or underflow, 0 if the caller deals with those. Returns 1 if the result
 * overflowed to infinity, 2 if to the largest finite number, else 0. */
static int fx_round_core(fx_env_t *e, uint32_t sign, int32_t *exp_io, uint64_t *hi_io, uint64_t lo,
                         uint32_t pbits, int32_t emax, int32_t wrap)
{
    const uint32_t rbits = 64 - pbits;
    const uint64_t unit = 1ull << rbits;
    uint64_t hi = *hi_io;
    int32_t exp = *exp_io;
    int tiny = 0, inc, inexact;
    uint32_t n;

    if (!(hi | lo)) {
        *exp_io = 0;
        return 0;
    }
    if (hi == 0) {
        hi = lo;
        lo = 0;
        exp -= 64;
    }
    n = fx_clz64(hi);
    if (n) {
        hi = (hi << n) | (lo >> (64 - n));
        lo <<= n;
        exp -= (int32_t)n;
    }
    if (exp <= 0) {
        /* Tiny, unless rounding carries it up to the smallest normal number. */
        tiny = !(exp == 0 && fx_round_inc(e->rc, sign, hi, lo, rbits) && (hi | (unit - 1)) == ~0ull);
        if (tiny && !(e->mask & FX_U)) {
            e->flags |= FX_U;
            exp += wrap;
            tiny = 0;
            if (!wrap) {
                *exp_io = 0;
                *hi_io = 0;
                return 0; /* the caller abandons the operation */
            }
        } else if (exp <= 0) {
            fx_shr_sticky(&hi, &lo, (uint32_t)(1 - exp));
            exp = 0;
        }
    }
    inc = fx_round_inc(e->rc, sign, hi, lo, rbits);
    inexact = ((hi & (unit - 1)) | lo) != 0;
    hi &= ~(unit - 1);
    if (inc) {
        hi += unit;
        if (hi == 0) {
            hi = FX_J;
            exp++;
        }
    }
    if (inexact) {
        e->flags |= (uint16_t)(FX_P | (inc ? FX_UP : 0) | (tiny ? FX_U : 0));
    }
    if (exp == 0 && (hi >> 63)) {
        exp = 1; /* a denormal that rounded up into the normal range */
    }
    if (exp >= emax) {
        e->flags |= FX_O | FX_P;
        if (!(e->mask & FX_O) && wrap) {
            exp -= wrap;
        } else {
            const int to_inf = e->rc == 0 || (e->rc == 2 && !sign) || (e->rc == 1 && sign);
            e->flags = (uint16_t)((e->flags & ~FX_UP) | (to_inf ? FX_UP : 0));
            *hi_io = to_inf ? FX_J : ~(unit - 1);
            *exp_io = to_inf ? emax : emax - 1;
            return to_inf ? 1 : 2;
        }
    }
    *hi_io = hi;
    *exp_io = exp;
    return 0;
}

/* The same for an extended-precision result, at the precision the environment sets. */
static fx_t fx_round(fx_env_t *e, uint32_t sign, int32_t exp, uint64_t hi, uint64_t lo)
{
    fx_round_core(e, sign, &exp, &hi, lo, e->pc, 0x7FFF, 0x6000);
    return fx_make(sign, (uint32_t)exp & 0x7FFF, hi);
}

/* A result that is known to be exact. */
static fx_t fx_exact(uint32_t sign, int32_t exp, uint64_t m)
{
    fx_env_t e = {0, 64, 0x3F, 0};
    return fx_round(&e, sign, exp, m, 0);
}

/* Significand and exponent of a finite non-zero number, normalised. */
static void fx_unpack(fx_t a, int32_t *exp, uint64_t *m)
{
    uint32_t n;
    *exp = fx_exp(a) ? (int32_t)fx_exp(a) : 1;
    *m = a.m;
    n = fx_clz64(a.m);
    *m <<= n;
    *exp -= (int32_t)n;
}

/* What an operation on a and b gives because of what they are, if anything: an operand
 * the 80387 does not support (invalid), or a NaN (the result is that NaN made quiet, of
 * two the one with the larger significand). Also notes denormal operands. */
static int fx_special(fx_env_t *e, fx_t a, fx_t b, fx_t *r)
{
    if (fx_is_unsup(a) || fx_is_unsup(b)) {
        e->flags |= FX_I;
        *r = fx_indefinite;
        return 1;
    }
    if (fx_is_nan(a) || fx_is_nan(b)) {
        if (fx_is_snan(a) || fx_is_snan(b)) {
            e->flags |= FX_I;
        }
        if (!fx_is_nan(b)) {
            *r = a;
        } else if (!fx_is_nan(a)) {
            *r = b;
        } else if (fx_is_snan(a) != fx_is_snan(b)) {
            *r = fx_is_snan(a) ? b : a; /* of a signalling and a quiet one, the quiet one */
        } else {
            a.m |= 0x4000000000000000ull;
            b.m |= 0x4000000000000000ull;
            *r = a.m > b.m || (a.m == b.m && !fx_sign(a)) ? a : b;
        }
        r->m |= 0x4000000000000000ull;
        return 1;
    }
    if (fx_is_denorm(a) || fx_is_denorm(b)) {
        e->flags |= FX_D;
    }
    return 0;
}

static fx_t fx_invalid(fx_env_t *e)
{
    e->flags = (uint16_t)((e->flags & ~FX_D) | FX_I); /* nothing else is reported with it */
    return fx_indefinite;
}

static fx_t fx_add(fx_env_t *e, fx_t a, fx_t b, int subtract)
{
    fx_t r;
    uint32_t sa, sb;
    int32_t ea, eb;
    uint64_t ma, mb, hi, lo;

    if (fx_special(e, a, b, &r)) {
        return r;
    }
    sa = fx_sign(a);
    sb = fx_sign(b) ^ (uint32_t)(subtract != 0);
    if (fx_is_inf(a) || fx_is_inf(b)) {
        if (fx_is_inf(a) && fx_is_inf(b) && sa != sb) {
            return fx_invalid(e);
        }
        return fx_is_inf(a) ? a : fx_make(sb, 0x7FFF, FX_J);
    }
    if (fx_is_zero(a) && fx_is_zero(b)) {
        return fx_make(sa == sb ? sa : e->rc == 1, 0, 0);
    }
    ea = fx_exp(a) ? (int32_t)fx_exp(a) : 1;
    eb = fx_exp(b) ? (int32_t)fx_exp(b) : 1;
    ma = a.m;
    mb = b.m;
    if (ea < eb || (ea == eb && ma < mb)) {
        const int32_t te = ea;
        const uint64_t tm = ma;
        const uint32_t ts = sa;
        ea = eb, eb = te;
        ma = mb, mb = tm;
        sa = sb, sb = ts;
    }
    hi = mb;
    lo = 0;
    fx_shr_sticky(&hi, &lo, (uint32_t)(ea - eb));
    if (sa == sb) {
        hi += ma;
        if (hi < ma) { /* carry out: one place to the right */
            lo = (hi << 63) | (lo >> 1) | (lo & 1);
            hi = (hi >> 1) | FX_J;
            ea++;
        }
    } else {
        hi = ma - hi - (lo != 0);
        lo = 0 - lo;
        if (!(hi | lo)) {
            return fx_make(e->rc == 1, 0, 0);
        }
    }
    return fx_round(e, sa, ea, hi, lo);
}

static fx_t fx_mul(fx_env_t *e, fx_t a, fx_t b)
{
    fx_t r;
    const uint32_t sign = fx_sign(a) ^ fx_sign(b);
    int32_t ea, eb;
    uint64_t ma, mb, hi, lo;

    if (fx_special(e, a, b, &r)) {
        return r;
    }
    if (fx_is_inf(a) || fx_is_inf(b)) {
        if (fx_is_zero(a) || fx_is_zero(b)) {
            return fx_invalid(e);
        }
        return fx_make(sign, 0x7FFF, FX_J);
    }
    if (fx_is_zero(a) || fx_is_zero(b)) {
        return fx_make(sign, 0, 0);
    }
    fx_unpack(a, &ea, &ma);
    fx_unpack(b, &eb, &mb);
    fx_mul64(ma, mb, &hi, &lo);
    return fx_round(e, sign, ea + eb - FX_BIAS + 1, hi, lo);
}

static fx_t fx_div(fx_env_t *e, fx_t a, fx_t b)
{
    fx_t r;
    const uint32_t sign = fx_sign(a) ^ fx_sign(b);
    int32_t ea, eb, exp;
    uint64_t ma, mb, rem, q = 0, lo = 0;

    if (fx_special(e, a, b, &r)) {
        return r;
    }
    if (fx_is_inf(a)) {
        return fx_is_inf(b) ? fx_invalid(e) : fx_make(sign, 0x7FFF, FX_J);
    }
    if (fx_is_inf(b)) {
        return fx_make(sign, 0, 0);
    }
    if (fx_is_zero(b)) {
        if (fx_is_zero(a)) {
            return fx_invalid(e);
        }
        e->flags = (uint16_t)((e->flags & ~FX_D) | FX_Z);
        return fx_make(sign, 0x7FFF, FX_J);
    }
    if (fx_is_zero(a)) {
        return fx_make(sign, 0, 0);
    }
    fx_unpack(a, &ea, &ma);
    fx_unpack(b, &eb, &mb);
    exp = ea - eb + FX_BIAS;
    rem = ma;
    if (rem >= mb) {
        rem -= mb; /* the quotient's integer bit */
    } else {
        /* a's significand is the smaller one: scale the quotient into [1, 2) */
        const uint64_t top = rem >> 63;
        rem <<= 1;
        if (top || rem >= mb) {
            rem -= mb;
        }
        exp--;
    }
    q = 1;
    /* 63 more quotient bits, then two for rounding */
    for (int i = 0; i < 65; i++) {
        const uint64_t top = rem >> 63;
        uint64_t bit = 0;
        rem <<= 1;
        if (top || rem >= mb) {
            rem -= mb;
            bit = 1;
        }
        if (i < 63) {
            q = (q << 1) | bit;
        } else {
            lo |= bit << (63 - (i - 63));
        }
    }
    return fx_round(e, sign, exp, q, lo | (rem != 0));
}

static fx_t fx_sqrt(fx_env_t *e, fx_t a)
{
    fx_t r;
    int32_t ea, t;
    uint64_t ma, rem_hi = 0, rem_lo = 0, root_hi = 0, root_lo = 0;
    int odd;

    if (fx_special(e, a, a, &r)) {
        return r;
    }
    if (fx_is_zero(a)) {
        return a;
    }
    if (fx_sign(a)) {
        return fx_invalid(e);
    }
    if (fx_is_inf(a)) {
        return a;
    }
    fx_unpack(a, &ea, &ma);
    t = ea - FX_BIAS - 63; /* the value is ma * 2^t */
    odd = t & 1;
    /* The 65-bit integer root of ma * 2^66 (t even) or ma * 2^65 (t odd), two bits of the
     * radicand at a time from the top. */
    for (int i = 0; i < 65; i++) {
        uint32_t two;
        uint64_t trial_hi, trial_lo;
        if (odd) {
            two = i == 0 ? (uint32_t)(ma >> 63)
                  : i <= 31 ? (uint32_t)(ma >> (63 - 2 * i)) & 3
                  : i == 32 ? (uint32_t)(ma & 1) << 1 : 0;
        } else {
            two = i <= 31 ? (uint32_t)(ma >> (62 - 2 * i)) & 3 : 0;
        }
        rem_hi = (rem_hi << 2) | (rem_lo >> 62);
        rem_lo = (rem_lo << 2) | two;
        trial_hi = (root_hi << 2) | (root_lo >> 62);
        trial_lo = (root_lo << 2) | 1;
        root_hi = (root_hi << 1) | (root_lo >> 63);
        root_lo <<= 1;
        if (rem_hi > trial_hi || (rem_hi == trial_hi && rem_lo >= trial_lo)) {
            rem_hi = rem_hi - trial_hi - (rem_lo < trial_lo);
            rem_lo -= trial_lo;
            root_lo |= 1;
        }
    }
    /* root_hi:root_lo is the 65-bit root; the result is root * 2^s */
    t = (odd ? (t - 1) / 2 - 32 : t / 2 - 33) + FX_BIAS + 64;
    return fx_round(e, 0, t, (root_hi << 63) | (root_lo >> 1),
                    (root_lo << 63) | ((rem_hi | rem_lo) != 0));
}

/* Round to an integer. */
static fx_t fx_rndint(fx_env_t *e, fx_t a)
{
    fx_t r;
    uint64_t hi, lo = 0;
    int32_t exp = (int32_t)fx_exp(a);

    if (fx_special(e, a, a, &r)) {
        return r;
    }
    if (fx_is_zero(a) || exp >= FX_BIAS + 63) {
        return a;
    }
    hi = a.m;
    fx_shr_sticky(&hi, &lo, (uint32_t)(FX_BIAS + 63 - (exp ? exp : 1)));
    if (lo) {
        const int inc = fx_round_inc(e->rc, fx_sign(a), hi, lo, 0);
        e->flags |= (uint16_t)(FX_P | (inc ? FX_UP : 0));
        hi += (uint64_t)inc;
    }
    r = fx_exact(fx_sign(a), FX_BIAS + 63, hi);
    r.se = (uint16_t)((r.se & 0x7FFF) | (a.se & 0x8000));
    return r;
}

/* FPREM (nearest = 0) and FPREM1 (1). *q: the low bits of the quotient; *partial: set if
 * the reduction is not complete (the exponents were too far apart). */
static fx_t fx_rem(fx_env_t *e, fx_t a, fx_t b, int nearest, uint32_t *q_out, int *partial)
{
    fx_t r;
    int32_t ea, eb, d, n, rexp;
    uint64_t ma, mb, rem;
    uint32_t q = 0, sign = fx_sign(a);

    *q_out = 0;
    *partial = 0;
    if (fx_special(e, a, b, &r)) {
        return r;
    }
    if (fx_is_inf(a) || fx_is_zero(b)) {
        return fx_invalid(e);
    }
    if (fx_is_inf(b) || fx_is_zero(a)) {
        if (fx_exp(a) == 0 && (a.m >> 63)) {
            a.se |= 1; /* a pseudo-denormal comes back as the normal number it is */
        }
        return a;
    }
    fx_unpack(a, &ea, &ma);
    fx_unpack(b, &eb, &mb);
    d = ea - eb;
    if (d >= 64) {
        n = 32 + (d & 31);
        *partial = 1;
    } else {
        n = d + 1;
    }
    rem = ma;
    rexp = ea;
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            uint64_t top = 0;
            if (i) {
                top = rem >> 63;
                rem <<= 1;
            }
            q <<= 1;
            if (top || rem >= mb) {
                rem -= mb;
                q |= 1;
            }
        }
        rexp = ea - n + 1;
    }
    if (nearest && !*partial && n >= 0) {
        /* rem and mb are at the same scale when n > 0; when n == 0, b is twice that */
        int over;
        if (n > 0) {
            over = (rem >> 63) || (rem << 1) > mb || ((rem << 1) == mb && (q & 1));
        } else {
            over = rem > mb;
        }
        if (over) {
            if (n > 0) {
                rem = mb - rem;
            } else {
                /* b - a at a's scale: 2 * mb - rem, which fits since rem > mb */
                rem = (mb << 1) - rem;
            }
            sign ^= 1;
            q++;
        }
    }
    *q_out = q;
    if (rem == 0) {
        return fx_make(fx_sign(a), 0, 0);
    }
    return fx_exact(sign, rexp, rem);
}

/* a * 2^trunc(b) */
static fx_t fx_scale(fx_env_t *e, fx_t a, fx_t b)
{
    fx_t r;
    int32_t ea, n;
    uint64_t ma;

    if (fx_special(e, a, b, &r)) {
        return r;
    }
    if (fx_is_inf(b)) {
        if (fx_sign(b)) {
            return fx_is_inf(a) ? fx_invalid(e) : fx_make(fx_sign(a), 0, 0);
        }
        return fx_is_zero(a) ? fx_invalid(e) : fx_make(fx_sign(a), 0x7FFF, FX_J);
    }
    if (fx_is_inf(a) || fx_is_zero(a)) {
        return a;
    }
    if (fx_exp(b) >= FX_BIAS + 16) {
        n = fx_sign(b) ? -0x10000 : 0x10000;
    } else if (fx_exp(b) < FX_BIAS) {
        n = 0;
    } else {
        n = (int32_t)(b.m >> (63 - (fx_exp(b) - FX_BIAS)));
        if (fx_sign(b)) {
            n = -n;
        }
    }
    fx_unpack(a, &ea, &ma);
    {
        /* not subject to the precision control */
        fx_env_t full = {e->rc, 64, e->mask, 0};
        r = fx_round(&full, fx_sign(a), ea + n, ma, 0);
        e->flags |= full.flags;
    }
    return r;
}

/* Relations: */
enum { FX_EQ = 0, FX_LT = 1, FX_GT = 2, FX_UN = 3 };

/* Compare; quiet: a quiet NaN is not an invalid operand (FUCOM). */
static int fx_cmp(fx_env_t *e, fx_t a, fx_t b, int quiet)
{
    uint32_t ea, eb;
    int lt;
    if (fx_is_unsup(a) || fx_is_unsup(b) || fx_is_nan(a) || fx_is_nan(b)) {
        if (!quiet || fx_is_unsup(a) || fx_is_unsup(b) || fx_is_snan(a) || fx_is_snan(b)) {
            e->flags |= FX_I;
        }
        return FX_UN;
    }
    if (fx_is_denorm(a) || fx_is_denorm(b)) {
        e->flags |= FX_D;
    }
    if (fx_is_zero(a) && fx_is_zero(b)) {
        return FX_EQ;
    }
    if (fx_sign(a) != fx_sign(b)) {
        return fx_sign(a) ? FX_LT : FX_GT;
    }
    ea = fx_exp(a) ? fx_exp(a) : 1;
    eb = fx_exp(b) ? fx_exp(b) : 1;
    if (ea == eb && a.m == b.m) {
        return FX_EQ;
    }
    /* a denormal's significand has no integer bit: compare as numbers */
    if (!(a.m >> 63) || !(b.m >> 63)) {
        int32_t xa, xb;
        uint64_t ya, yb;
        if (!a.m || !b.m) {
            lt = !a.m;
        } else {
            fx_unpack(a, &xa, &ya);
            fx_unpack(b, &xb, &yb);
            lt = xa < xb || (xa == xb && ya < yb);
        }
    } else {
        lt = ea < eb || (ea == eb && a.m < b.m);
    }
    return (lt != (int)fx_sign(a)) ? FX_LT : FX_GT;
}

/* --- conversions ------------------------------------------------------------- */

static fx_t fx_from_i64(int64_t v)
{
    const uint32_t sign = v < 0;
    const uint64_t mag = sign ? 0 - (uint64_t)v : (uint64_t)v;
    return fx_exact(sign, FX_BIAS + 63, mag);
}

/* To a signed integer of the given width, rounded as the environment says. Invalid, with
 * the "integer indefinite" (the most negative number) as the result, if it does not fit. */
static int64_t fx_to_int(fx_env_t *e, fx_t a, uint32_t bits)
{
    const int64_t indefinite = (int64_t)(0 - (1ull << (bits - 1)));
    const int32_t exp = (int32_t)fx_exp(a);
    uint64_t hi = a.m, lo = 0;
    int inc = 0;

    if (fx_is_zero(a)) {
        return 0;
    }
    if (exp == 0x7FFF || fx_is_unsup(a) || exp > FX_BIAS + 63) {
        e->flags |= FX_I;
        return indefinite;
    }
    fx_shr_sticky(&hi, &lo, (uint32_t)(FX_BIAS + 63 - (exp ? exp : 1)));
    if (lo) {
        inc = fx_round_inc(e->rc, fx_sign(a), hi, lo, 0);
        hi += (uint64_t)inc;
    }
    if (hi > (1ull << (bits - 1)) - (fx_sign(a) ? 0 : 1)) {
        e->flags |= FX_I;
        return indefinite;
    }
    if (lo) {
        e->flags |= (uint16_t)(FX_P | (inc ? FX_UP : 0));
    }
    return fx_sign(a) ? (int64_t)(0 - hi) : (int64_t)hi;
}

static fx_t fx_from_f64(fx_env_t *e, uint64_t v)
{
    const uint32_t sign = (uint32_t)(v >> 63), exp = (uint32_t)(v >> 52) & 0x7FF;
    const uint64_t frac = v << 12 >> 12;
    if (exp == 0x7FF) {
        if (!frac) {
            return fx_make(sign, 0x7FFF, FX_J);
        }
        if (!(frac >> 51)) {
            e->flags |= FX_I;
        }
        return fx_make(sign, 0x7FFF, FX_J | 0x4000000000000000ull | (frac << 11));
    }
    if (exp == 0) {
        if (!frac) {
            return fx_make(sign, 0, 0);
        }
        e->flags |= FX_D;
        return fx_exact(sign, 1 - 1023 + FX_BIAS, frac << 11);
    }
    return fx_make(sign, exp - 1023 + FX_BIAS, FX_J | (frac << 11));
}

static fx_t fx_from_f32(fx_env_t *e, uint32_t v)
{
    const uint32_t sign = v >> 31, exp = (v >> 23) & 0xFF, frac = v & 0x7FFFFF;
    if (exp == 0xFF) {
        if (!frac) {
            return fx_make(sign, 0x7FFF, FX_J);
        }
        if (!(frac >> 22)) {
            e->flags |= FX_I;
        }
        return fx_make(sign, 0x7FFF, FX_J | 0x4000000000000000ull | ((uint64_t)frac << 40));
    }
    if (exp == 0) {
        if (!frac) {
            return fx_make(sign, 0, 0);
        }
        e->flags |= FX_D;
        return fx_exact(sign, 1 - 127 + FX_BIAS, (uint64_t)frac << 40);
    }
    return fx_make(sign, exp - 127 + FX_BIAS, FX_J | ((uint64_t)frac << 40));
}

/* To double or single precision: the sign, exponent field and left-aligned significand
 * of the result, for a format of pbits significant bits and an exponent field up to emax
 * with the given bias. */
static void fx_to_small(fx_env_t *e, fx_t a, uint32_t pbits, int32_t emax, int32_t bias,
                        int32_t *exp_out, uint64_t *m_out)
{
    int32_t exp;
    uint64_t m;
    if (fx_exp(a) == 0x7FFF) {
        if (fx_is_snan(a)) {
            e->flags |= FX_I;
        }
        *exp_out = emax;
        *m_out = fx_is_inf(a) ? FX_J : (a.m | 0x4000000000000000ull) & ~((1ull << (64 - pbits)) - 1);
        return;
    }
    if (fx_is_zero(a)) {
        *exp_out = 0;
        *m_out = 0;
        return;
    }
    fx_unpack(a, &exp, &m);
    exp = exp - FX_BIAS + bias;
    fx_round_core(e, fx_sign(a), &exp, &m, 0, pbits, emax, 0);
    *exp_out = exp;
    *m_out = m;
}

static uint64_t fx_to_f64(fx_env_t *e, fx_t a)
{
    int32_t exp;
    uint64_t m;
    if (fx_is_unsup(a)) {
        e->flags |= FX_I;
        return 0xFFF8000000000000ull;
    }
    fx_to_small(e, a, 53, 0x7FF, 1023, &exp, &m);
    return ((uint64_t)fx_sign(a) << 63) | ((uint64_t)exp << 52) | (m << 1 >> 12);
}

static uint32_t fx_to_f32(fx_env_t *e, fx_t a)
{
    int32_t exp;
    uint64_t m;
    if (fx_is_unsup(a)) {
        e->flags |= FX_I;
        return 0xFFC00000u;
    }
    fx_to_small(e, a, 24, 0xFF, 127, &exp, &m);
    return (fx_sign(a) << 31) | ((uint32_t)exp << 23) | (uint32_t)(m << 1 >> 41);
}

/* To and from the host's double, for the functions computed with its math library. */
static double fx_to_double(fx_t a)
{
    fx_env_t e = {0, 64, 0x3F, 0};
    const uint64_t bits = fx_to_f64(&e, a);
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static fx_t fx_from_double(double d)
{
    fx_env_t e = {0, 64, 0x3F, 0};
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return fx_from_f64(&e, bits);
}

#endif
