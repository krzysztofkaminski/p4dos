/*
 * The instructions that have a 16-bit and a 32-bit form. Part of run_loop() in cpu.c,
 * which includes this file once for each operand size (OS = 16 or 32).
 */
#undef H
#undef H0F
#undef HN
#undef OPS_CAT
#undef OPS_CAT2
#undef RV
#undef uV
#undef sV
#undef OSB
#undef VBITS
#undef FETCHV
#undef IPV
#undef mrV
#undef mrwV
#undef mwV
#undef FV
#undef SEXTV
#undef OS32
#undef SETZSPV
#undef GET_RMV
#undef GET_RMWV
#undef SET_RMV
#undef ALUV
#undef EACH_LO
#undef EACH_HI
#undef B_INC
#undef B_DEC
#undef B_XCHG
#undef B_MOVI
#undef H_JCC
#undef H_JCCN
#undef BT_OP
#undef SYS

#define OPS_CAT2(os, x) op##os##_##x
#define OPS_CAT(os, x) OPS_CAT2(os, x)
#define H(x) OPS_CAT(OS, x)
#define H0F(x) OPS_CAT(OS, 0F_##x)
#define HN(hi, n) OPS_CAT(OS, hi##n)
#if OS == 16
#define RV(i) R16(i)
#define uV uint16_t
#define sV int16_t
#define OSB 2u
#define VBITS 16
#define FETCHV() FETCH16()
#define IPV(x) ((x) & 0xFFFF)
#define mrV(sg, o) mr16(sg, o)
#define mrwV(sg, o) mrw16(sg, o)
#define mwV(sg, o, v) mw16(sg, o, v)
#define FV(n) f_##n##16
#define SEXTV(x) (SEXT8(x) & 0xFFFF)
#define SETZSPV(r) SETZSP16(r)
#define OS32 0
#else
#define RV(i) R32(i)
#define uV uint32_t
#define sV int32_t
#define OSB 4u
#define VBITS 32
#define FETCHV() FETCH32()
#define IPV(x) (x)
#define mrV(sg, o) mr32(sg, o)
#define mrwV(sg, o) mrw32(sg, o)
#define mwV(sg, o, v) mw32(sg, o, v)
#define FV(n) f_##n##32
#define SEXTV(x) SEXT8(x)
#define SETZSPV(r) SETZSP32(r)
#define OS32 1
#endif
#define GET_RMV() (IS_REG() ? (uint32_t)RV(rm) : mrV(ea_seg, ea_off))
#define GET_RMWV() (IS_REG() ? (uint32_t)RV(rm) : mrwV(ea_seg, ea_off))
#define SET_RMV(v) \
    do { \
        if (IS_REG()) { \
            RV(rm) = (uV)(v); \
        } else { \
            mwV(ea_seg, ea_off, (v)); \
        } \
    } while (0)
/* A call into cpu_sys.h that may raise an exception. */
#define SYS(call) \
    do { \
        cpu.slice = slice; \
        call; \
    } while (0)

#define ALUV(c1, c3, c5, F, STORE) \
    H(c1): \
        RM_OP( \
            t = F(STORE ? GET_RMWV() : GET_RMV(), (uint32_t)RV(reg)); \
            if (STORE) { \
                SET_RMV(t); \
            } \
        ); \
    H(c3): \
        RM_OP( \
            t = F((uint32_t)RV(reg), GET_RMV()); \
            if (STORE) { \
                RV(reg) = (uV)t; \
            } \
        ); \
    H(c5): \
        t = FETCHV(); \
        t = F((uint32_t)RV(REG_AX), t); \
        if (STORE) { \
            RV(REG_AX) = (uV)t; \
        } \
        NEXT();

            ALUV(0x01, 0x03, 0x05, FV(add), 1)
            ALUV(0x09, 0x0B, 0x0D, FV(or), 1)
            ALUV(0x11, 0x13, 0x15, FV(adc), 1)
            ALUV(0x19, 0x1B, 0x1D, FV(sbb), 1)
            ALUV(0x21, 0x23, 0x25, FV(and), 1)
            ALUV(0x29, 0x2B, 0x2D, FV(sub), 1)
            ALUV(0x31, 0x33, 0x35, FV(xor), 1)
            ALUV(0x39, 0x3B, 0x3D, FV(sub), 0)

        H(0x0F):
            if (unlikely(!IS386)) {
                if (cpu.model == CPU_8086) { /* POP CS */
                    uint32_t sp_ = ESP;
                    t = spop(&sp_, 0);
                    set_sp(sp_);
                    seg_load_real(CS, t);
                    lim1 = 0;
                    NEXT();
                }
                goto invalid;
            }
            op = FETCH8();
            goto *dispatch_0f[OS32][op];

/* One handler per register for the opcodes with a register number in their low bits. */
#define EACH_LO(hi, BODY) \
    HN(hi, 0): BODY(0); HN(hi, 1): BODY(1); HN(hi, 2): BODY(2); HN(hi, 3): BODY(3); \
    HN(hi, 4): BODY(4); HN(hi, 5): BODY(5); HN(hi, 6): BODY(6); HN(hi, 7): BODY(7);
#define EACH_HI(hi, BODY) \
    HN(hi, 8): BODY(0); HN(hi, 9): BODY(1); HN(hi, A): BODY(2); HN(hi, B): BODY(3); \
    HN(hi, C): BODY(4); HN(hi, D): BODY(5); HN(hi, E): BODY(6); HN(hi, F): BODY(7);
#define B_INC(n) RV(n) = (uV)FV(inc)(RV(n)); NEXT()
#define B_DEC(n) RV(n) = (uV)FV(dec)(RV(n)); NEXT()
#define B_XCHG(n) t = RV(REG_AX); RV(REG_AX) = RV(n); RV(n) = (uV)t; NEXT()
#define B_MOVI(n) RV(n) = (uV)FETCHV(); NEXT()
            EACH_LO(0x4, B_INC)
            EACH_HI(0x4, B_DEC)

        H(0x62): /* BOUND */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            DECODE();
            if (IS_REG()) {
                if (IS386) {
                    goto invalid;
                }
                NEXT();
            }
            {
                const sV v = (sV)RV(reg);
                const sV lo = (sV)mrV(ea_seg, ea_off);
                const sV hi = (sV)mrV(ea_seg, (ea_off + OSB) & eap->mask);
                if (v < lo || v > hi) {
                    if (IS386) {
                        FAULT(EXC_BR);
                    }
                    ip = cpu.insn_ip;
                    SYNC_OUT();
                    do_interrupt(5, 0, 0, 0, ip);
                    SYNC_IN();
                    goto next;
                }
            }
            NEXT();
        H(0x69): /* IMUL r, r/m, imm */
        H(0x6B): /* IMUL r, r/m, imm8 */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            DECODE();
            t = GET_RMV();
            u = op == 0x69 ? FETCHV() : SEXTV(FETCH8());
            {
#if OS == 16
                const int32_t p = (int32_t)(int16_t)t * (int32_t)(int16_t)u;
                t = p != (int16_t)p;
#else
                const int64_t p = (int64_t)(int32_t)t * (int32_t)u;
                t = p != (int32_t)p;
#endif
                RV(reg) = (uV)p;
                set_cf_of(t, t);
            }
            NEXT();
        H(0x6D): /* INSW */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            IO_STRING(STR_INS, OSB);
            NEXT();
        H(0x6F): /* OUTSW */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            IO_STRING(STR_OUTS, OSB);
            BREAK();
            NEXT();

#define H_JCC(c) \
    t = FETCH8(); \
    if (c) { \
        ip = IPV(ip + SEXT8(t)); \
        NEXT_J(); \
    } \
    NEXT()
        H(0x70): H_JCC(GET_OF());
        H(0x71): H_JCC(!GET_OF());
        H(0x72): H_JCC(GET_CF());
        H(0x73): H_JCC(!GET_CF());
        H(0x74): H_JCC(GET_ZF());
        H(0x75): H_JCC(!GET_ZF());
        H(0x76): H_JCC(GET_CF() | GET_ZF());
        H(0x77): H_JCC(!(GET_CF() | GET_ZF()));
        H(0x78): H_JCC(GET_SF());
        H(0x79): H_JCC(!GET_SF());
        H(0x7A): H_JCC(GET_PF());
        H(0x7B): H_JCC(!GET_PF());
        H(0x7C): H_JCC(GET_SF() != GET_OF());
        H(0x7D): H_JCC(GET_SF() == GET_OF());
        H(0x7E): H_JCC(GET_ZF() | (GET_SF() != GET_OF()));
        H(0x7F): H_JCC(!(GET_ZF() | (GET_SF() != GET_OF())));

        H(0x81): H(0x83):
            RM_OP_M(
                t = reg == 7 ? GET_RMV() : GET_RMWV();
                u = op == 0x81 ? FETCHV() : SEXTV(FETCH8());
                switch (reg) {
                case 0: t = FV(add)(t, u); break;
                case 1: t = FV(or)(t, u); break;
                case 2: t = FV(adc)(t, u); break;
                case 3: t = FV(sbb)(t, u); break;
                case 4: t = FV(and)(t, u); break;
                case 5: t = FV(sub)(t, u); break;
                case 6: t = FV(xor)(t, u); break;
                default: FV(sub)(t, u); NEXT();
                }
                SET_RMV(t);
            );
        H(0x85):
            RM_OP(
                FV(and)(GET_RMV(), RV(reg));
            );
        H(0x87):
            RM_OP(
                t = GET_RMWV();
                SET_RMV(RV(reg));
                RV(reg) = (uV)t;
            );
        H(0x89):
            RM_OP_M(
                SET_RMV(RV(reg));
            );
        H(0x8B):
            RM_OP_M(
                RV(reg) = (uV)GET_RMV();
            );
        H(0x8C): /* MOV from a segment register */
            DECODE();
            if (IS386 && reg > GS) {
                goto invalid;
            }
            t = cpu.seg[IS386 ? reg : reg & 3];
            if (IS_REG()) {
                RV(rm) = (uV)t;
            } else {
                mw16(ea_seg, ea_off, t);
            }
            NEXT();
        H(0x8D): /* LEA */
            DECODE();
            if (IS_REG()) {
                if (IS386) {
                    goto invalid;
                }
                NEXT();
            }
            RV(reg) = (uV)ea_off;
            NEXT();

            EACH_LO(0x9, B_XCHG) /* 90, XCHG AX,AX, is NOP */
        H(0x98):
#if OS == 16
            AX = (uint16_t)(int16_t)(int8_t)AL;
#else
            EAX = (uint32_t)(int32_t)(int16_t)AX;
#endif
            NEXT();
        H(0x99):
#if OS == 16
            DX = (AX & 0x8000) ? 0xFFFF : 0;
#else
            EDX = (EAX & 0x80000000u) ? 0xFFFFFFFFu : 0;
#endif
            NEXT();

        H(0xA1):
            t = eap->a32 ? FETCH32() : FETCH16();
            RV(REG_AX) = (uV)mrV(STR_SRC(), t);
            NEXT();
        H(0xA3):
            t = eap->a32 ? FETCH32() : FETCH16();
            mwV(STR_SRC(), t, RV(REG_AX));
            NEXT();
        H(0xA5): /* MOVSW */
            if (STR_PLAIN()) {
                mwV(SEG(ES), DI, mrV(STR_SRC(), SI));
                SI += DF_STEP(OSB);
                DI += DF_STEP(OSB);
                NEXT();
            }
            STRING(STR_MOVS, OSB);
            NEXT();
        H(0xA7): /* CMPSW */
            STRING(STR_CMPS, OSB);
            NEXT();
        H(0xA9):
            t = FETCHV();
            FV(and)(RV(REG_AX), t);
            NEXT();
        H(0xAB): /* STOSW */
            if (STR_PLAIN()) {
                mwV(SEG(ES), DI, RV(REG_AX));
                DI += DF_STEP(OSB);
                NEXT();
            }
            STRING(STR_STOS, OSB);
            NEXT();
        H(0xAD): /* LODSW */
            if (STR_PLAIN()) {
                RV(REG_AX) = (uV)mrV(STR_SRC(), SI);
                SI += DF_STEP(OSB);
                NEXT();
            }
            STRING(STR_LODS, OSB);
            NEXT();
        H(0xAF): /* SCASW */
            STRING(STR_SCAS, OSB);
            NEXT();

            EACH_HI(0xB, B_MOVI)

        H(0xC1): /* shift r/m, imm8 */
            if (cpu.model == CPU_8086) {
                goto *dispatch[0][0xC3];
            }
            DECODE();
            t = GET_RMWV();
            u = FETCH8();
            SET_RMV(shift(reg, t, u, VBITS));
            NEXT();
        H(0xC4): /* LES */
        H(0xC5): /* LDS */
            DECODE();
            if (IS_REG()) {
                if (IS386) {
                    goto invalid;
                }
                NEXT();
            }
            t = mrV(ea_seg, ea_off);
            u = mr16(ea_seg, (ea_off + OSB) & eap->mask);
            LOADSEG(op == 0xC4 ? ES : DS, u);
            RV(reg) = (uV)t;
            NEXT();
        H(0xC7):
            RM_OP_M(
                if (IS386 && reg) {
                    goto invalid;
                }
                t = FETCHV();
                SET_RMV(t);
            );
        H(0xCF): /* IRET */
            SYNC_OUT();
            do_iret(OS32);
            SYNC_IN();
            BREAK();
            goto next;

        H(0xD1):
            RM_OP(
                t = GET_RMWV();
                SET_RMV(shift(reg, t, 1, VBITS));
            );
        H(0xD3):
            RM_OP(
                t = GET_RMWV();
                SET_RMV(shift(reg, t, CL, VBITS));
            );

        H(0xE0): /* LOOPNZ */
            t = FETCH8();
            u = eap->a32 ? --ECX : (uint32_t)--CX;
            if (u && !GET_ZF()) {
                ip = IPV(ip + SEXT8(t));
                NEXT_J();
            }
            NEXT();
        H(0xE1): /* LOOPZ */
            t = FETCH8();
            u = eap->a32 ? --ECX : (uint32_t)--CX;
            if (u && GET_ZF()) {
                ip = IPV(ip + SEXT8(t));
                NEXT_J();
            }
            NEXT();
        H(0xE2): /* LOOP */
            t = FETCH8();
            u = eap->a32 ? --ECX : (uint32_t)--CX;
            if (u) {
                ip = IPV(ip + SEXT8(t));
                NEXT_J();
            }
            NEXT();
        H(0xE3): /* JCXZ */
            t = FETCH8();
            u = eap->a32 ? ECX : (uint32_t)CX;
            if (!u) {
                ip = IPV(ip + SEXT8(t));
                NEXT_J();
            }
            NEXT();
        H(0xE5):
            t = FETCH8();
            IO_CHECK(t, OSB);
            RV(REG_AX) = (uV)IO_IN(t, OSB);
            NEXT();
        H(0xE7):
            t = FETCH8();
            IO_CHECK(t, OSB);
            IO_OUT(t, OSB, RV(REG_AX));
            BREAK();
            NEXT();
        H(0xE9):
            t = FETCHV();
            ip = IPV(ip + t);
            NEXT_J();
        H(0xEA):
            t = FETCHV();
            u = FETCH16();
            FAR_JMP(u, t);
        H(0xEB):
            t = FETCH8();
            ip = IPV(ip + SEXT8(t));
            NEXT_J();
        H(0xED):
            IO_CHECK(DX, OSB);
            RV(REG_AX) = (uV)IO_IN(DX, OSB);
            NEXT();
        H(0xEF):
            IO_CHECK(DX, OSB);
            IO_OUT(DX, OSB, RV(REG_AX));
            BREAK();
            NEXT();

        H(0xF7):
            DECODE();
            t = (reg == 2 || reg == 3) ? GET_RMWV() : GET_RMV();
            switch (reg) {
            case 0: case 1:
                u = FETCHV();
                FV(and)(t, u);
                break;
            case 2: SET_RMV(~t); break;
            case 3:
                t = FV(sub)(0, t);
                SET_RMV(t);
                break;
#if OS == 16
            case 4: /* MUL */
                t *= AX;
                AX = (uint16_t)t;
                DX = (uint16_t)(t >> 16);
                u = DX != 0;
                set_cf_of(u, u);
                cpu.f_ax = 0;
                SETZSP16(IS386 ? t : t >> 16);
                break;
            case 5: { /* IMUL */
                const int32_t p = (int32_t)(int16_t)t * (int32_t)(int16_t)AX;
                AX = (uint16_t)p;
                DX = (uint16_t)((uint32_t)p >> 16);
                u = p != (int16_t)p;
                set_cf_of(u, u);
                cpu.f_ax = 0;
                SETZSP16(IS386 ? (uint32_t)p : (uint32_t)p >> 16);
                break;
            }
            case 6: { /* DIV */
                const uint32_t nn = ((uint32_t)DX << 16) | AX;
                if (t == 0 || (u = nn / t) > 0xFFFF) {
                    goto div0;
                }
                DX = (uint16_t)(nn % t);
                AX = (uint16_t)u;
                break;
            }
            default: { /* IDIV */
                const int64_t d = (int16_t)t, nn = (int32_t)(((uint32_t)DX << 16) | AX);
                int64_t q;
                if (d == 0) {
                    goto div0;
                }
                q = nn / d;
                if (q > 32767 || q < (cpu.model == CPU_8086 ? -32767 : -32768)) {
                    goto div0;
                }
                if (cpu.model == CPU_8086 && rep) {
                    q = -q;
                }
                DX = (uint16_t)(nn % d);
                AX = (uint16_t)q;
                break;
            }
#else
            case 4: { /* MUL */
                const uint64_t p = (uint64_t)t * EAX;
                EAX = (uint32_t)p;
                EDX = (uint32_t)(p >> 32);
                u = EDX != 0;
                set_cf_of(u, u);
                cpu.f_ax = 0;
                SETZSP32(EAX);
                break;
            }
            case 5: { /* IMUL */
                const int64_t p = (int64_t)(int32_t)t * (int32_t)EAX;
                EAX = (uint32_t)p;
                EDX = (uint32_t)((uint64_t)p >> 32);
                u = p != (int32_t)p;
                set_cf_of(u, u);
                cpu.f_ax = 0;
                SETZSP32(EAX);
                break;
            }
            case 6: { /* DIV */
                const uint64_t nn = ((uint64_t)EDX << 32) | EAX;
                uint64_t q;
                if (t == 0 || (q = nn / t) > 0xFFFFFFFFu) {
                    goto div0;
                }
                EDX = (uint32_t)(nn % t);
                EAX = (uint32_t)q;
                break;
            }
            default: { /* IDIV */
                const int64_t d = (int32_t)t, nn = (int64_t)(((uint64_t)EDX << 32) | EAX);
                int64_t q;
                if (d == 0 || (d == -1 && nn == INT64_MIN)) {
                    goto div0;
                }
                q = nn / d;
                if (q > INT32_MAX || q < INT32_MIN) {
                    goto div0;
                }
                EDX = (uint32_t)(nn % d);
                EAX = (uint32_t)q;
                break;
            }
#endif
            }
            NEXT();

        /* --- the two-byte opcodes (80386) --- */

        H0F(0x00):
            if (!cpu.pm) {
                goto invalid;
            }
            DECODE();
            switch (reg) {
            case 0: case 1: /* SLDT, STR */
                t = reg ? cpu.tr_sel : cpu.ldt_sel;
                if (IS_REG()) {
                    RV(rm) = (uV)t;
                } else {
                    mw16(ea_seg, ea_off, t);
                }
                break;
            case 2: case 3: /* LLDT, LTR */
                if (cpu.cpl != 0) {
                    FAULT_C(EXC_GP, 0);
                }
                t = GET_RM16();
                if (reg == 2) {
                    SYS(ldt_load(t, EXC_GP));
                } else {
                    SYS(do_ltr(t));
                }
                break;
            case 4: case 5: /* VERR, VERW */
                t = GET_RM16();
                SYS(do_verify(reg == 5, t));
                break;
            default: goto invalid;
            }
            NEXT();
        H0F(0x01):
            DECODE();
            switch (reg) {
            case 0: case 1: /* SGDT, SIDT */
                if (IS_REG()) {
                    goto invalid;
                }
                (void)mrw16(ea_seg, ea_off);
                (void)mrw32(ea_seg, (ea_off + 2) & eap->mask);
                mw16(ea_seg, ea_off, reg ? cpu.idt.limit : cpu.gdt.limit);
                mw32(ea_seg, (ea_off + 2) & eap->mask, reg ? cpu.idt.base : cpu.gdt.base);
                break;
            case 2: case 3: /* LGDT, LIDT */
                if (IS_REG()) {
                    goto invalid;
                }
                if (cpu.cpl != 0) {
                    FAULT_C(EXC_GP, 0);
                }
                t = mr16(ea_seg, ea_off);
                u = mr32(ea_seg, (ea_off + 2) & eap->mask);
                if (!OS32) {
                    u &= 0x00FFFFFF;
                }
                if (reg == 2) {
                    cpu.gdt.limit = t;
                    cpu.gdt.base = u;
                } else {
                    cpu.idt.limit = t;
                    cpu.idt.base = u;
                }
                break;
            case 4: /* SMSW */
                t = cpu.cr0 | 0x7FFFFFE0u | (cpu_fpu_fitted ? 0x10 : 0);
                if (IS_REG()) {
                    RV(rm) = (uV)t;
                } else {
                    mw16(ea_seg, ea_off, t);
                }
                break;
            case 6: /* LMSW: may set PE but not clear it */
                if (cpu.cpl != 0) {
                    FAULT_C(EXC_GP, 0);
                }
                t = GET_RM16();
                SYNC_OUT();
                set_cr0((cpu.cr0 & ~0xEu) | (t & 0xF) | (cpu.cr0 & 1));
                SYNC_IN();
                goto next;
            default: goto invalid;
            }
            NEXT();
        H0F(0x02): /* LAR */
        H0F(0x03): /* LSL */
            if (!cpu.pm) {
                goto invalid;
            }
            DECODE();
            t = GET_RM16();
            cpu.slice = slice;
            {
                const lar_t r_ = do_lar_lsl(op == 0x03, t);
                if (r_.ok) {
                    RV(reg) = (uV)r_.value;
                }
            }
            NEXT();
        H0F(0x06): /* CLTS */
            if (cpu.cpl != 0) {
                FAULT_C(EXC_GP, 0);
            }
            cpu.cr0 &= ~CR0_TS;
            NEXT();
        H0F(0x20): H0F(0x21): H0F(0x22): H0F(0x23): H0F(0x24): H0F(0x26):
            /* MOV to and from control, debug and test registers */
            modrm = FETCH8();
            SYNC_OUT();
            mov_special(op, (modrm >> 3) & 7, modrm & 7);
            SYNC_IN();
            goto next;

#define H_JCCN(c) \
    t = FETCHV(); \
    if (c) { \
        ip = IPV(ip + t); \
        NEXT_J(); \
    } \
    NEXT()
        H0F(0x80): H_JCCN(GET_OF());
        H0F(0x81): H_JCCN(!GET_OF());
        H0F(0x82): H_JCCN(GET_CF());
        H0F(0x83): H_JCCN(!GET_CF());
        H0F(0x84): H_JCCN(GET_ZF());
        H0F(0x85): H_JCCN(!GET_ZF());
        H0F(0x86): H_JCCN(GET_CF() | GET_ZF());
        H0F(0x87): H_JCCN(!(GET_CF() | GET_ZF()));
        H0F(0x88): H_JCCN(GET_SF());
        H0F(0x89): H_JCCN(!GET_SF());
        H0F(0x8A): H_JCCN(GET_PF());
        H0F(0x8B): H_JCCN(!GET_PF());
        H0F(0x8C): H_JCCN(GET_SF() != GET_OF());
        H0F(0x8D): H_JCCN(GET_SF() == GET_OF());
        H0F(0x8E): H_JCCN(GET_ZF() | (GET_SF() != GET_OF()));
        H0F(0x8F): H_JCCN(!(GET_ZF() | (GET_SF() != GET_OF())));

        H0F(0x90): H0F(0x91): H0F(0x92): H0F(0x93): H0F(0x94): H0F(0x95): H0F(0x96): H0F(0x97):
        H0F(0x98): H0F(0x99): H0F(0x9A): H0F(0x9B): H0F(0x9C): H0F(0x9D): H0F(0x9E): H0F(0x9F):
            /* SETcc */
            DECODE();
            t = (uint32_t)cond(op & 0x0F);
            SET_RM8(t);
            NEXT();

        H0F(0xA0): /* PUSH FS */
        H0F(0xA8): /* PUSH GS */
            /* two bytes, whatever the operand size (see PUSH_SEG) */
            u = sp_add(ESP, 0u - OSB);
            mw16(SEG(SS), u & sp_mask(), cpu.seg[op == 0xA0 ? FS : GS]);
            set_sp(u);
            NEXT();
        H0F(0xA1): /* POP FS */
        H0F(0xA9): /* POP GS */
            /* Only the selector's two bytes are read, whatever the operand size. */
            t = mr16(SEG(SS), ESP & sp_mask());
            u = sp_add(ESP, OSB);
            LOADSEG(op == 0xA1 ? FS : GS, t);
            set_sp(u);
            NEXT();

/* The bit instructions: with a register for the bit number, a memory operand is a bit
 * string that the number may reach outside the addressed word of. */
#define BT_OP(RD, WRITE) \
    DECODE(); \
    u = RV(reg); \
    if (IS_REG()) { \
        t = RV(rm); \
    } else { \
        ea_off = (ea_off + (uint32_t)((sV)u >> (VBITS == 16 ? 4 : 5)) * OSB) & eap->mask; \
        t = RD(ea_seg, ea_off); \
    } \
    bt_flags(t, u & (VBITS - 1), VBITS); \
    u = 1u << (u & (VBITS - 1)); \
    WRITE; \
    NEXT()
        H0F(0xA3): BT_OP(mrV, (void)0);
        H0F(0xAB): BT_OP(mrwV, SET_RMV(t | u));
        H0F(0xB3): BT_OP(mrwV, SET_RMV(t & ~u));
        H0F(0xBB): BT_OP(mrwV, SET_RMV(t ^ u));
        H0F(0xBA): /* BT, BTS, BTR, BTC with an immediate bit number */
            DECODE();
            if (reg < 4) {
                goto invalid;
            }
            t = reg == 4 ? GET_RMV() : GET_RMWV();
            u = FETCH8() & (VBITS - 1);
            bt_flags(t, u, VBITS);
            u = 1u << u;
            if (reg != 4) {
                SET_RMV(reg == 5 ? t | u : reg == 6 ? t & ~u : t ^ u);
            }
            NEXT();

        H0F(0xA4): H0F(0xA5): /* SHLD */
        H0F(0xAC): H0F(0xAD): /* SHRD */
            DECODE();
            t = GET_RMWV();
            u = (op & 1) ? CL : FETCH8();
            t = do_shiftd(op >= 0xAC, t, RV(reg), u, VBITS);
            SET_RMV(t);
            NEXT();
        H0F(0xAF): /* IMUL r, r/m */
            DECODE();
            t = GET_RMV();
            {
#if OS == 16
                const int32_t p = (int32_t)(int16_t)t * (int32_t)(int16_t)R16(reg);
                t = p != (int16_t)p;
#else
                const int64_t p = (int64_t)(int32_t)t * (int32_t)R32(reg);
                t = p != (int32_t)p;
#endif
                RV(reg) = (uV)p;
                set_cf_of(t, t);
            }
            NEXT();
        H0F(0xB2): /* LSS */
        H0F(0xB4): /* LFS */
        H0F(0xB5): /* LGS */
            DECODE();
            if (IS_REG()) {
                goto invalid;
            }
            t = mrV(ea_seg, ea_off);
            u = mr16(ea_seg, (ea_off + OSB) & eap->mask);
            LOADSEG(op == 0xB2 ? SS : op == 0xB4 ? FS : GS, u);
            RV(reg) = (uV)t;
            NEXT();
        H0F(0xB6): /* MOVZX r, r/m8 */
            RM_OP(
                RV(reg) = (uV)GET_RM8();
            );
        H0F(0xB7): /* MOVZX r, r/m16 */
            RM_OP(
                RV(reg) = (uV)GET_RM16();
            );
        H0F(0xBE): /* MOVSX r, r/m8 */
            RM_OP(
                RV(reg) = (uV)(sV)(int8_t)GET_RM8();
            );
        H0F(0xBF): /* MOVSX r, r/m16 */
            RM_OP(
                RV(reg) = (uV)(sV)(int16_t)GET_RM16();
            );
        H0F(0xBC): /* BSF */
        H0F(0xBD): /* BSR */
            DECODE();
            t = GET_RMV();
            cpu.f_zr = t != 0; /* ZF set, and the register left alone, for zero */
            if (t) {
                RV(reg) = (uV)(op == 0xBC ? (uint32_t)__builtin_ctz(t) : 31u - (uint32_t)__builtin_clz(t));
            }
            NEXT();
