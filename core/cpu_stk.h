/*
 * The instructions that use the stack, which depend on the operand size and on whether the
 * stack segment makes the stack pointer SP or ESP. Part of run_loop() in cpu.c, which
 * includes this file for each combination (OS and SSZ, each 16 or 32), after cpu_ops.h.
 *
 * The stack pointer is only changed once nothing can fault any more.
 */
#undef HS
#undef HSN
#undef STK_CAT
#undef STK_CAT2
#undef SPREG
#undef BPREG
#undef SPOFF
#undef PUSHV
#undef POPV
#undef FAR_CALL
#undef PUSH_SEG
#undef EACH_LO_S
#undef EACH_HI_S
#undef B_PUSH
#undef B_POP

#define STK_CAT2(os, ss, x) op##os##s##ss##_##x
#define STK_CAT(os, ss, x) STK_CAT2(os, ss, x)
#define HS(x) STK_CAT(OS, SSZ, x)
#define HSN(hi, n) STK_CAT(OS, SSZ, hi##n)
#if SSZ == 16
#define SPREG SP
#define BPREG BP
#define SPOFF(x) ((x) & 0xFFFF)
#else
#define SPREG ESP
#define BPREG EBP
#define SPOFF(x) (x)
#endif
#define PUSHV(v) \
    do { \
        const uint32_t pv_ = (v), ps_ = SPOFF(SPREG - OSB); \
        mwV(SEG(SS), ps_, pv_); \
        SPREG = ps_; \
    } while (0)
#define POPV() ({ const uint32_t pv_ = mrV(SEG(SS), SPREG); SPREG += OSB; pv_; })
#define FAR_CALL(sel, off) \
    do { \
        const uint32_t sel_ = (sel), off_ = (off); \
        if (likely(!cpu.pm)) { \
            const uint32_t ps_ = SPOFF(SPREG - 2 * OSB); \
            if (unlikely(IS386 && off_ > cpu.sg[CS].limit)) { \
                FAULT_C(EXC_GP, 0); \
            } \
            mwV(SEG(SS), SPOFF(ps_ + OSB), cpu.seg[CS]); \
            mwV(SEG(SS), ps_, ip); \
            SPREG = ps_; \
            ip = off_; \
            seg_load_real(CS, sel_); \
            lim1 = 0; \
        } else { \
            SYNC_OUT(); \
            far_call(sel_, off_, OS32, ip); \
            SYNC_IN(); \
        } \
        goto next; \
    } while (0)

/* A segment register is pushed as two bytes, whatever room the operand size makes for
 * it: the 80386 leaves the rest of a 32-bit slot as it was. */
#define PUSH_SEG(s) \
    do { \
        const uint32_t ps_ = SPOFF(SPREG - OSB); \
        mw16(SEG(SS), ps_, cpu.seg[s]); \
        SPREG = ps_; \
    } while (0)
        HS(0x06): PUSH_SEG(ES); NEXT();
        HS(0x0E): PUSH_SEG(CS); NEXT();
        HS(0x16): PUSH_SEG(SS); NEXT();
        HS(0x1E): PUSH_SEG(DS); NEXT();
        HS(0x07):
            t = mr16(SEG(SS), SPREG);
            LOADSEG(ES, t);
            SPREG += OSB;
            NEXT();
        HS(0x17):
            t = mr16(SEG(SS), SPREG);
            LOADSEG(SS, t);
            SPREG += OSB;
            goto inhibit;
        HS(0x1F):
            t = mr16(SEG(SS), SPREG);
            LOADSEG(DS, t);
            SPREG += OSB;
            NEXT();

#define EACH_LO_S(hi, BODY) \
    HSN(hi, 0): BODY(0); HSN(hi, 1): BODY(1); HSN(hi, 2): BODY(2); HSN(hi, 3): BODY(3); \
    HSN(hi, 5): BODY(5); HSN(hi, 6): BODY(6); HSN(hi, 7): BODY(7);
#define EACH_HI_S(hi, BODY) \
    HSN(hi, 8): BODY(0); HSN(hi, 9): BODY(1); HSN(hi, A): BODY(2); HSN(hi, B): BODY(3); \
    HSN(hi, C): BODY(4); HSN(hi, D): BODY(5); HSN(hi, E): BODY(6); HSN(hi, F): BODY(7);
#define B_PUSH(n) PUSHV(RV(n)); NEXT()
#define B_POP(n) t = POPV(); RV(n) = (uV)t; NEXT()
            EACH_LO_S(0x5, B_PUSH)
        HS(0x54): /* PUSH SP: the 8086 and 80186 store the already decremented value */
            if (IS386) {
                PUSHV(RV(REG_SP));
            } else {
                SP -= 2;
                mw16(SEG(SS), SP, SP);
            }
            NEXT();
            EACH_HI_S(0x5, B_POP)

        HS(0x60): /* PUSHA */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            {
                /* From the lowest address up, as the 80386 does: it shows when the
                 * stack is too small and this faults halfway. */
                const uint32_t s0_ = SPREG;
                for (uint32_t i_ = 8; i_ > 0; i_--) {
                    mwV(SEG(SS), SPOFF(s0_ - i_ * OSB), RV(i_ - 1));
                }
                SPREG = SPOFF(s0_ - 8 * OSB);
            }
            NEXT();
        HS(0x61): /* POPA */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            {
                const uint32_t s0_ = SPREG;
                for (uint32_t i_ = 0; i_ < 8; i_++) {
                    const uint32_t v2_ = mrV(SEG(SS), SPOFF(s0_ + i_ * OSB));
                    if (i_ != 3) {
                        RV(7 - i_) = (uV)v2_;
                    }
#if OS == 32 && SSZ == 16
                    else {
                        ESP = v2_; /* the 80386 takes the upper half from the stack */
                    }
#endif
                }
                SPREG = SPOFF(s0_ + 8 * OSB);
            }
            NEXT();
        HS(0x68): /* PUSH imm */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            t = FETCHV();
            PUSHV(t);
            NEXT();
        HS(0x6A): /* PUSH imm8 (sign-extended) */
            if (cpu.model == CPU_8086) {
                goto jcc_alias;
            }
            t = SEXTV(FETCH8());
            PUSHV(t);
            NEXT();
        HS(0x8F): /* POP r/m */
            t = mrV(SEG(SS), SPREG);
            MODRM();
            if (IS386 && reg) {
                goto invalid;
            }
            if (IS_REG()) {
                SPREG += OSB;
                RV(rm) = (uV)t;
                NEXT();
            }
            SPREG += OSB; /* an address using the stack pointer sees it already moved */
            EA();
            SPREG -= OSB;
            mwV(ea_seg, ea_off, t);
            SPREG += OSB;
            NEXT();
        HS(0x9A): /* CALL far */
            t = FETCHV();
            u = FETCH16();
            FAR_CALL(u, t);
        HS(0x9C): /* PUSHF */
            if (unlikely(cpu.f_vm) && cpu.f_iopl != 3) {
                FAULT_C(EXC_GP, 0);
            }
            t = get_eflags() & 0x00FCFFFFu;
            PUSHV(t);
            NEXT();
        HS(0x9D): /* POPF */
            if (unlikely(cpu.f_vm) && cpu.f_iopl != 3) {
                FAULT_C(EXC_GP, 0);
            }
            t = POPV();
            set_eflags(t, IS386 ? popf_mask(OS32) : 0xFFFFu);
            BREAK();
            NEXT();
        HS(0xC2): /* RET imm16 */
            u = FETCH16();
            t = mrV(SEG(SS), SPREG);
#if OS == 32
            if (unlikely(t > cpu.sg[CS].limit)) { /* faults with the stack untouched */
                FAULT_C(EXC_GP, 0);
            }
#endif
            SPREG += OSB + u;
            ip = t;
            NEXT_J();
        HS(0xC3): /* RET */
            t = mrV(SEG(SS), SPREG);
#if OS == 32
            if (unlikely(t > cpu.sg[CS].limit)) {
                FAULT_C(EXC_GP, 0);
            }
#endif
            SPREG += OSB;
            ip = t;
            NEXT_J();
        HS(0xC8): /* ENTER */
            if (cpu.model == CPU_8086) {
                goto *dispatch[0][0xCA];
            }
            u = FETCH16();
            t = FETCH8() & 0x1F;
            if (unlikely(t)) {
                cpu.slice = slice;
                do_enter(OS32, u, t);
                NEXT();
            }
            {
                const uint32_t ps_ = SPOFF(SPREG - OSB);
                mwV(SEG(SS), ps_, RV(REG_BP));
                if (IS386) { /* the new top of the stack must be writable */
                    (void)mrw8(SEG(SS), SPOFF(ps_ - u));
                }
#if SSZ == 16
                RV(REG_BP) = (uV)((ESP & 0xFFFF0000u) | ps_);
#else
                RV(REG_BP) = (uV)ps_;
#endif
                SPREG = ps_ - u;
            }
            NEXT();
        HS(0xC9): /* LEAVE */
            if (cpu.model == CPU_8086) {
                goto *dispatch[0][0xCB];
            }
            {
                const uint32_t b_ = BPREG;
                t = mrV(SEG(SS), b_);
                SPREG = b_ + OSB;
                RV(REG_BP) = (uV)t;
            }
            NEXT();
        HS(0xCA): /* RETF imm16 */
            u = FETCH16();
            goto HS(retf);
        HS(0xCB): /* RETF */
            u = 0;
        HS(retf):
            if (likely(!cpu.pm)) {
                const uint32_t cs_ = mrV(SEG(SS), SPOFF(SPREG + OSB)) & 0xFFFF;
                t = mrV(SEG(SS), SPREG);
                if (unlikely(IS386 && t > cpu.sg[CS].limit)) {
                    FAULT_C(EXC_GP, 0);
                }
                SPREG += 2 * OSB + u;
                ip = t;
                seg_load_real(CS, cs_);
                lim1 = 0;
            } else {
                SYNC_OUT();
                far_ret(OS32, u);
                SYNC_IN();
            }
            goto next;
        HS(0xE8): /* CALL */
            t = FETCHV();
            PUSHV(ip);
            ip = IPV(ip + t);
            NEXT_J();

        HS(0xFF):
            DECODE();
            switch (reg) {
            case 0:
                t = GET_RMWV();
                SET_RMV(FV(inc)(t));
                break;
            case 1:
                t = GET_RMWV();
                SET_RMV(FV(dec)(t));
                break;
            case 2:
                t = GET_RMV();
                PUSHV(ip);
                ip = t;
                NEXT_J();
            case 3:
                if (IS_REG()) {
                    goto invalid;
                }
                t = mrV(ea_seg, ea_off);
                u = mr16(ea_seg, (ea_off + OSB) & eap->mask);
                FAR_CALL(u, t);
            case 4:
                ip = GET_RMV();
                NEXT_J();
            case 5:
                if (IS_REG()) {
                    goto invalid;
                }
                t = mrV(ea_seg, ea_off);
                u = mr16(ea_seg, (ea_off + OSB) & eap->mask);
                FAR_JMP(u, t);
            default:
                if (IS386 && reg == 7) {
                    goto invalid;
                }
                if (!IS386 && IS_REG() && rm == REG_SP) {
                    SP -= 2;
                    mw16(SEG(SS), SP, SP);
                } else {
                    t = GET_RMV();
                    PUSHV(t);
                }
                break;
            }
            NEXT();
