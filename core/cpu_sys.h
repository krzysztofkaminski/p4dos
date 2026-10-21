/*
 * 80386 system programming model: descriptors and segment loading, far control transfers
 * through gates, interrupts and exceptions, task switches, and the privileged
 * instructions. Part of cpu.c.
 *
 * Everything here may raise an exception (RAISE): nothing of the visible state is changed
 * before the checks that can fail have been made, as far as the processor guarantees it.
 */

static void update_mode(void)
{
    cpu.pm = (cpu.cr0 & CR0_PE) && !cpu.f_vm;
    if (!(cpu.cr0 & CR0_PE)) {
        cpu.cpl = 0;
    } else if (cpu.f_vm) {
        cpu.cpl = 3;
    }
    cpu.lim1 = 0;
}

static void set_cr0(uint32_t v)
{
    const uint32_t old = cpu.cr0;
    cpu.cr0 = v & (CR0_PG | CR0_TS | CR0_EM | CR0_MP | CR0_PE);
    if ((old ^ cpu.cr0) & (CR0_PG | CR0_PE)) {
        tlb_flush();
        all_windows();
    }
    update_mode();
}

/* --- the stack, with a stack pointer that is only committed at the end --------- */

static inline uint32_t sp_mask(void) { return (cpu.dmode & 2) ? 0xFFFFFFFFu : 0xFFFFu; }
static inline void set_sp(uint32_t sp)
{
    if (cpu.dmode & 2) {
        ESP = sp;
    } else {
        SP = (uint16_t)sp;
    }
}
/* With a 16-bit stack only the low half of the pointer moves. */
static inline uint32_t sp_add(uint32_t sp, uint32_t n)
{
    const uint32_t mask = sp_mask();
    return (sp & ~mask) | ((sp + n) & mask);
}
static void spush(uint32_t *sp, uint32_t v, int wide)
{
    const uint32_t nsp = sp_add(*sp, wide ? 0u - 4 : 0u - 2);
    if (wide) {
        sw32(SEG(SS), nsp & sp_mask(), v);
    } else {
        sw16(SEG(SS), nsp & sp_mask(), v);
    }
    *sp = nsp;
}
static uint32_t spop(uint32_t *sp, int wide)
{
    uint32_t v = wide ? sr32(SEG(SS), *sp & sp_mask()) : sr16(SEG(SS), *sp & sp_mask());
    *sp = sp_add(*sp, wide ? 4 : 2);
    return v;
}

/* --- descriptors ------------------------------------------------------------- */

typedef struct {
    uint32_t base, limit;
    uint32_t lo, hi; /* the raw descriptor */
    uint32_t addr;   /* its linear address */
    uint8_t ar, big;
} desc_t;

#define AR_P 0x80
#define AR_S 0x10
#define AR_CODE 0x08
#define AR_DPL(ar) (((ar) >> 5) & 3)
#define SEL_NULL(sel) (((sel) & 0xFFFC) == 0)
#define SEL_ERR(sel) ((sel) & 0xFFFCu)

static void desc_decode(desc_t *d)
{
    d->base = (d->lo >> 16) | ((d->hi & 0xFF) << 16) | (d->hi & 0xFF000000u);
    d->limit = (d->lo & 0xFFFF) | (d->hi & 0xF0000);
    if (d->hi & 0x800000) {
        d->limit = (d->limit << 12) | 0xFFF;
    }
    d->ar = (uint8_t)(d->hi >> 8);
    d->big = (d->hi >> 22) & 1;
}

/* Read the descriptor a selector names; 0 if it lies outside its table. */
static int desc_fetch(uint32_t sel, desc_t *d)
{
    const uint32_t idx = sel & 0xFFF8;
    uint32_t base, limit;
    if (sel & 4) {
        if (SEL_NULL(cpu.ldt_sel)) {
            return 0;
        }
        base = cpu.ldt.base;
        limit = cpu.ldt.limit;
    } else {
        base = cpu.gdt.base;
        limit = cpu.gdt.limit;
    }
    if (idx + 7 > limit) {
        return 0;
    }
    d->addr = base + idx;
    d->lo = sys_r32(d->addr);
    d->hi = sys_r32(d->addr + 4);
    desc_decode(d);
    return 1;
}

static void desc_accessed(const desc_t *d)
{
    if (!(d->ar & 1)) {
        sys_w8(d->addr + 5, d->ar | 1);
    }
}

static void seg_fill(cpu_seg_t *sg, const desc_t *d)
{
    sg->base = d->base;
    sg->limit = d->limit;
    sg->ar = d->ar;
    if (d->ar & AR_CODE) {
        sg->flags = (d->ar & 2) ? CPU_SF_R : 0;
    } else {
        sg->flags = CPU_SF_R | ((d->ar & 2) ? CPU_SF_W : 0) | ((d->ar & 4) ? CPU_SF_DOWN : 0);
    }
    if (d->big) {
        sg->flags |= CPU_SF_BIG;
    }
}

static void seg_set(int s, uint32_t sel, const desc_t *d)
{
    seg_fill(&cpu.sg[s], d);
    cpu.seg[s] = (uint16_t)sel;
    if (s == CS) {
        cpu.cpl = sel & 3;
        cpu.lim1 = 0;
        if ((cpu.dmode & 1) != d->big) {
            cpu.dmode ^= 1;
            all_windows(); /* they depend on the code size */
        }
    } else if (s == SS) {
        cpu.dmode = (uint8_t)((cpu.dmode & ~2u) | (d->big << 1));
    }
    seg_window(&cpu.sg[s]);
}

static void seg_null(int s, uint32_t sel)
{
    cpu.seg[s] = (uint16_t)sel;
    cpu.sg[s].base = 0;
    cpu.sg[s].limit = 0;
    cpu.sg[s].flags = 0;
    cpu.sg[s].ar = 0;
    seg_window(&cpu.sg[s]);
}

/* Checks on a stack segment for privilege level cpl; gp is the exception for a selector
 * that will not do (#GP, or #TS when a task switch or a gate supplies it). */
static void ss_check(uint32_t sel, uint32_t cpl, uint32_t gp, desc_t *d)
{
    if (SEL_NULL(sel)) {
        RAISE_C(gp, gp == EXC_TS ? SEL_ERR(sel) : 0);
    }
    if (!desc_fetch(sel, d) || (sel & 3) != cpl || (d->ar & (AR_S | AR_CODE | 2)) != (AR_S | 2) ||
        AR_DPL(d->ar) != cpl) {
        RAISE_C(gp, SEL_ERR(sel));
    }
    if (!(d->ar & AR_P)) {
        RAISE_C(EXC_SS, SEL_ERR(sel));
    }
}

static void load_ss(uint32_t sel, uint32_t cpl, uint32_t gp)
{
    desc_t d;
    ss_check(sel, cpl, gp, &d);
    desc_accessed(&d);
    seg_set(SS, sel, &d);
}

static void load_data_seg(int s, uint32_t sel, uint32_t gp)
{
    desc_t d;
    if (SEL_NULL(sel)) {
        seg_null(s, sel);
        return;
    }
    if (!desc_fetch(sel, &d) || !(d.ar & AR_S) || (d.ar & (AR_CODE | 2)) == AR_CODE) {
        RAISE_C(gp, SEL_ERR(sel)); /* not a segment, or execute-only code */
    }
    if ((d.ar & (AR_CODE | 4)) != (AR_CODE | 4)) { /* all but conforming code */
        uint32_t rpl = sel & 3;
        if (AR_DPL(d.ar) < (rpl > cpu.cpl ? rpl : cpu.cpl)) {
            RAISE_C(gp, SEL_ERR(sel));
        }
    }
    if (!(d.ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
    desc_accessed(&d);
    seg_set(s, sel, &d);
}

/* MOV to a segment register, POP, LDS and the like. */
static void load_seg(int s, uint32_t sel)
{
    sel &= 0xFFFF;
    if (!cpu.pm) {
        seg_load_real(s, sel);
    } else if (s == SS) {
        load_ss(sel, cpu.cpl, EXC_GP);
    } else {
        load_data_seg(s, sel, EXC_GP);
    }
}

/* After a return to a less privileged level: data segments the new level may not use
 * are made unusable. */
static void drop_inner_segs(void)
{
    static const uint8_t segs[4] = {ES, DS, FS, GS};
    for (int i = 0; i < 4; i++) {
        const cpu_seg_t *sg = &cpu.sg[segs[i]];
        if ((sg->ar & (AR_CODE | 4)) != (AR_CODE | 4) && AR_DPL(sg->ar) < cpu.cpl) {
            seg_null(segs[i], 0);
        }
    }
}

/* --- task switches ------------------------------------------------------------- */

enum { TS_JMP, TS_CALL, TS_IRET };

static void ldt_load(uint32_t sel, uint32_t gp)
{
    desc_t d;
    cpu.ldt_sel = (uint16_t)sel;
    cpu.ldt.base = 0;
    cpu.ldt.limit = 0;
    if (SEL_NULL(sel)) {
        return;
    }
    if ((sel & 4) || !desc_fetch(sel, &d) || (d.ar & 0x1F) != 2) {
        cpu.ldt_sel = 0;
        RAISE_C(gp, SEL_ERR(sel));
    }
    if (!(d.ar & AR_P)) {
        cpu.ldt_sel = 0;
        RAISE_C(gp == EXC_TS ? EXC_TS : EXC_NP, SEL_ERR(sel));
    }
    cpu.ldt.base = d.base;
    cpu.ldt.limit = d.limit;
}

static void cs_check_ts(uint32_t sel, desc_t *d)
{
    if (SEL_NULL(sel) || !desc_fetch(sel, d) || (d->ar & (AR_S | AR_CODE)) != (AR_S | AR_CODE)) {
        RAISE_C(EXC_TS, SEL_ERR(sel));
    }
    if ((d->ar & 4) ? AR_DPL(d->ar) > (sel & 3) : AR_DPL(d->ar) != (sel & 3)) {
        RAISE_C(EXC_TS, SEL_ERR(sel));
    }
    if (!(d->ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
}

static void task_switch(uint32_t sel, int kind, uint32_t ret_ip)
{
    static const uint8_t seg_order[6] = {ES, CS, SS, DS, FS, GS};
    desc_t d, cd;
    uint32_t ob = cpu.tr.base, nb, fl, nip, nfl, ncr3 = cpu.cr3, nldt, nsel[6], nreg[8];
    int new32, i;

    if ((sel & 4) || !desc_fetch(sel, &d)) {
        RAISE_C(kind == TS_IRET ? EXC_TS : EXC_GP, SEL_ERR(sel));
    }
    if ((d.ar & 0x17) != (kind == TS_IRET ? 3 : 1)) {
        RAISE_C(kind == TS_IRET ? EXC_TS : EXC_GP, SEL_ERR(sel));
    }
    if (!(d.ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
    new32 = (d.ar & 8) != 0;
    if (d.limit < (new32 ? 0x67u : 0x2Bu)) {
        RAISE_C(EXC_TS, SEL_ERR(sel));
    }
    nb = d.base;

    /* Save the outgoing task. */
    fl = get_eflags();
    if (kind == TS_IRET) {
        fl &= ~(uint32_t)FL_NT;
    }
    if (cpu.tr.ar & 8) {
        sys_w32(ob + 0x20, ret_ip);
        sys_w32(ob + 0x24, fl);
        for (i = 0; i < 8; i++) {
            sys_w32(ob + 0x28 + 4 * (uint32_t)i, R32(i));
        }
        for (i = 0; i < 6; i++) {
            sys_w16(ob + 0x48 + 4 * (uint32_t)i, cpu.seg[seg_order[i]]);
        }
    } else {
        sys_w16(ob + 0x0E, ret_ip);
        sys_w16(ob + 0x10, fl);
        for (i = 0; i < 8; i++) {
            sys_w16(ob + 0x12 + 2 * (uint32_t)i, R16(i));
        }
        for (i = 0; i < 4; i++) {
            sys_w16(ob + 0x22 + 2 * (uint32_t)i, cpu.seg[seg_order[i]]);
        }
    }
    if (kind != TS_CALL) { /* the outgoing task is no longer busy */
        uint32_t a = cpu.gdt.base + (cpu.tr_sel & 0xFFF8u) + 5;
        sys_w8(a, sys_r8(a) & ~2u);
    }

    /* Read the incoming one. */
    if (new32) {
        ncr3 = sys_r32(nb + 0x1C);
        nip = sys_r32(nb + 0x20);
        nfl = sys_r32(nb + 0x24);
        for (i = 0; i < 8; i++) {
            nreg[i] = sys_r32(nb + 0x28 + 4 * (uint32_t)i);
        }
        for (i = 0; i < 6; i++) {
            nsel[i] = sys_r16(nb + 0x48 + 4 * (uint32_t)i);
        }
        nldt = sys_r16(nb + 0x60);
    } else {
        nip = sys_r16(nb + 0x0E);
        nfl = sys_r16(nb + 0x10);
        for (i = 0; i < 8; i++) {
            /* the 80386 fills the upper halves with ones */
            nreg[i] = 0xFFFF0000u | sys_r16(nb + 0x12 + 2 * (uint32_t)i);
        }
        for (i = 0; i < 4; i++) {
            nsel[i] = sys_r16(nb + 0x22 + 2 * (uint32_t)i);
        }
        nsel[4] = nsel[5] = 0;
        nldt = sys_r16(nb + 0x2A);
    }
    if (kind == TS_CALL) {
        sys_w16(nb, cpu.tr_sel); /* back link */
        nfl |= FL_NT;
    }
    if (kind != TS_IRET) {
        sys_w8(d.addr + 5, d.ar | 2);
    }

    /* From here on the new task is the current one, also for the faults below. */
    cpu.tr_sel = (uint16_t)sel;
    cpu.tr.base = nb;
    cpu.tr.limit = d.limit;
    cpu.tr.ar = d.ar | 2;
    cpu.cr0 |= CR0_TS;
    if (new32 && ncr3 != cpu.cr3) {
        cpu.cr3 = ncr3;
        tlb_flush();
    }
    for (i = 0; i < 8; i++) {
        R32(i) = nreg[i];
    }
    cpu.ip = nip;
    cpu.insn_ip = nip;
    set_eflags(nfl, 0xFFFFFFFFu);
    for (i = 0; i < 6; i++) {
        cpu.seg[seg_order[i]] = (uint16_t)nsel[i];
        cpu.sg[seg_order[i]].flags = 0;
    }
    ldt_load(nldt, EXC_TS);
    if (cpu.f_vm) {
        for (i = 0; i < 6; i++) {
            seg_load_real(seg_order[i], nsel[i]);
        }
    } else {
        cs_check_ts(nsel[1], &cd);
        seg_set(CS, nsel[1], &cd);
        load_ss(nsel[2], cpu.cpl, EXC_TS);
        load_data_seg(ES, nsel[0], EXC_TS);
        load_data_seg(DS, nsel[3], EXC_TS);
        load_data_seg(FS, nsel[4], EXC_TS);
        load_data_seg(GS, nsel[5], EXC_TS);
    }
    all_windows();
}

/* A task gate: its selector names the TSS to switch to. */
static void task_gate(const desc_t *g, int kind, uint32_t ret_ip)
{
    task_switch(g->lo >> 16, kind, ret_ip);
}

/* --- far jumps, calls and returns ---------------------------------------------- */

/* Checks on the code segment a far transfer goes to directly. */
static void cs_check_direct(uint32_t sel, const desc_t *d)
{
    if (d->ar & 4) { /* conforming */
        if (AR_DPL(d->ar) > cpu.cpl) {
            RAISE_C(EXC_GP, SEL_ERR(sel));
        }
    } else if ((sel & 3) > cpu.cpl || AR_DPL(d->ar) != cpu.cpl) {
        RAISE_C(EXC_GP, SEL_ERR(sel));
    }
    if (!(d->ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
}

static void tss_stack(uint32_t dpl, uint32_t *ss, uint32_t *esp)
{
    if (cpu.tr.ar & 8) {
        if (dpl * 8 + 9 > cpu.tr.limit) {
            RAISE_C(EXC_TS, SEL_ERR(cpu.tr_sel));
        }
        *esp = sys_r32(cpu.tr.base + 4 + dpl * 8);
        *ss = sys_r16(cpu.tr.base + 8 + dpl * 8);
    } else {
        if (dpl * 4 + 5 > cpu.tr.limit) {
            RAISE_C(EXC_TS, SEL_ERR(cpu.tr_sel));
        }
        *esp = sys_r16(cpu.tr.base + 2 + dpl * 4);
        *ss = sys_r16(cpu.tr.base + 4 + dpl * 4);
    }
}

/* Push onto a stack that is not the current one yet (an inner privilege level's), with
 * supervisor rights. */
static void npush(const cpu_seg_t *ns, uint32_t *sp, uint32_t v, int wide)
{
    const uint32_t mask = (ns->flags & CPU_SF_BIG) ? 0xFFFFFFFFu : 0xFFFFu;
    if (wide) {
        *sp -= 4;
        sys_w32(seg_lin(ns, *sp & mask, 4, 1), v);
    } else {
        *sp -= 2;
        sys_w16(seg_lin(ns, *sp & mask, 2, 1), v);
    }
}

static void far_jmp(uint32_t sel, uint32_t off, uint32_t next_ip)
{
    desc_t d;
    sel &= 0xFFFF;
    if (!cpu.pm) {
        seg_load_real(CS, sel);
        cpu.ip = off;
        return;
    }
    if (SEL_NULL(sel)) {
        RAISE_C(EXC_GP, 0);
    }
    if (!desc_fetch(sel, &d)) {
        RAISE_C(EXC_GP, SEL_ERR(sel));
    }
    if (d.ar & AR_S) {
        if (!(d.ar & AR_CODE)) {
            RAISE_C(EXC_GP, SEL_ERR(sel));
        }
        cs_check_direct(sel, &d);
        if (off > d.limit) {
            RAISE_C(EXC_GP, 0);
        }
        desc_accessed(&d);
        seg_set(CS, (sel & ~3u) | cpu.cpl, &d);
        cpu.ip = off;
        return;
    }
    if (AR_DPL(d.ar) < cpu.cpl || AR_DPL(d.ar) < (sel & 3)) {
        RAISE_C(EXC_GP, SEL_ERR(sel));
    }
    if (!(d.ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
    switch (d.ar & 0x0F) {
    case 1: case 9: task_switch(sel, TS_JMP, next_ip); break;
    case 5: task_gate(&d, TS_JMP, next_ip); break;
    case 4: case 12: { /* call gate: the same privilege level only */
        const uint32_t gsel = d.lo >> 16;
        const uint32_t goff = (d.ar & 8) ? (d.lo & 0xFFFF) | (d.hi & 0xFFFF0000u) : d.lo & 0xFFFF;
        desc_t c;
        if (SEL_NULL(gsel)) {
            RAISE_C(EXC_GP, 0);
        }
        if (!desc_fetch(gsel, &c) || (c.ar & (AR_S | AR_CODE)) != (AR_S | AR_CODE) ||
            ((c.ar & 4) ? AR_DPL(c.ar) > cpu.cpl : AR_DPL(c.ar) != cpu.cpl)) {
            RAISE_C(EXC_GP, SEL_ERR(gsel));
        }
        if (!(c.ar & AR_P)) {
            RAISE_C(EXC_NP, SEL_ERR(gsel));
        }
        if (goff > c.limit) {
            RAISE_C(EXC_GP, 0);
        }
        desc_accessed(&c);
        seg_set(CS, (gsel & ~3u) | cpu.cpl, &c);
        cpu.ip = goff;
        break;
    }
    default: RAISE_C(EXC_GP, SEL_ERR(sel));
    }
}

static void far_call(uint32_t sel, uint32_t off, int os32, uint32_t ret_ip)
{
    desc_t d;
    uint32_t sp = ESP;
    sel &= 0xFFFF;
    if (!cpu.pm) {
        spush(&sp, cpu.seg[CS], os32);
        spush(&sp, ret_ip, os32);
        set_sp(sp);
        seg_load_real(CS, sel);
        cpu.ip = off;
        return;
    }
    if (SEL_NULL(sel)) {
        RAISE_C(EXC_GP, 0);
    }
    if (!desc_fetch(sel, &d)) {
        RAISE_C(EXC_GP, SEL_ERR(sel));
    }
    if (d.ar & AR_S) {
        if (!(d.ar & AR_CODE)) {
            RAISE_C(EXC_GP, SEL_ERR(sel));
        }
        cs_check_direct(sel, &d);
        spush(&sp, cpu.seg[CS], os32);
        spush(&sp, ret_ip, os32);
        if (off > d.limit) {
            RAISE_C(EXC_GP, 0);
        }
        set_sp(sp);
        desc_accessed(&d);
        seg_set(CS, (sel & ~3u) | cpu.cpl, &d);
        cpu.ip = off;
        return;
    }
    if (AR_DPL(d.ar) < cpu.cpl || AR_DPL(d.ar) < (sel & 3)) {
        RAISE_C(EXC_GP, SEL_ERR(sel));
    }
    if (!(d.ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
    switch (d.ar & 0x0F) {
    case 1: case 9: task_switch(sel, TS_CALL, ret_ip); break;
    case 5: task_gate(&d, TS_CALL, ret_ip); break;
    case 4: case 12: {
        const int g32 = (d.ar & 8) != 0;
        const uint32_t gsel = d.lo >> 16;
        const uint32_t goff = g32 ? (d.lo & 0xFFFF) | (d.hi & 0xFFFF0000u) : d.lo & 0xFFFF;
        const uint32_t nparam = d.hi & 0x1F;
        desc_t c;
        if (SEL_NULL(gsel)) {
            RAISE_C(EXC_GP, 0);
        }
        if (!desc_fetch(gsel, &c) || (c.ar & (AR_S | AR_CODE)) != (AR_S | AR_CODE) ||
            AR_DPL(c.ar) > cpu.cpl) {
            RAISE_C(EXC_GP, SEL_ERR(gsel));
        }
        if (!(c.ar & AR_P)) {
            RAISE_C(EXC_NP, SEL_ERR(gsel));
        }
        if (!(c.ar & 4) && AR_DPL(c.ar) < cpu.cpl) {
            /* To a more privileged level: onto its stack, taking the parameters along. */
            const uint32_t ncpl = AR_DPL(c.ar), old_ss = cpu.seg[SS], old_sp = ESP;
            const uint32_t omask = sp_mask();
            uint32_t nss, nsp, param[32], i;
            desc_t sd;
            cpu_seg_t ns;
            tss_stack(ncpl, &nss, &nsp);
            ss_check(nss, ncpl, EXC_TS, &sd);
            seg_fill(&ns, &sd);
            for (i = 0; i < nparam; i++) {
                param[i] = g32 ? sr32(SEG(SS), (old_sp + 4 * i) & omask)
                               : sr16(SEG(SS), (old_sp + 2 * i) & omask);
            }
            npush(&ns, &nsp, old_ss, g32);
            npush(&ns, &nsp, old_sp, g32);
            for (i = nparam; i > 0; i--) {
                npush(&ns, &nsp, param[i - 1], g32);
            }
            npush(&ns, &nsp, cpu.seg[CS], g32);
            npush(&ns, &nsp, ret_ip, g32);
            if (goff > c.limit) {
                RAISE_C(EXC_GP, 0);
            }
            desc_accessed(&c);
            seg_set(CS, (gsel & ~3u) | ncpl, &c);
            seg_set(SS, nss, &sd);
            set_sp(nsp);
        } else {
            spush(&sp, cpu.seg[CS], g32);
            spush(&sp, ret_ip, g32);
            if (goff > c.limit) {
                RAISE_C(EXC_GP, 0);
            }
            set_sp(sp);
            desc_accessed(&c);
            seg_set(CS, (gsel & ~3u) | cpu.cpl, &c);
        }
        cpu.ip = goff;
        break;
    }
    default: RAISE_C(EXC_GP, SEL_ERR(sel));
    }
}

/* The code segment a far return or IRET goes back to. */
static void cs_check_return(uint32_t sel, desc_t *d)
{
    const uint32_t rpl = sel & 3;
    if (SEL_NULL(sel)) {
        RAISE_C(EXC_GP, 0);
    }
    if (!desc_fetch(sel, d) || (d->ar & (AR_S | AR_CODE)) != (AR_S | AR_CODE) || rpl < cpu.cpl ||
        ((d->ar & 4) ? AR_DPL(d->ar) > rpl : AR_DPL(d->ar) != rpl)) {
        RAISE_C(EXC_GP, SEL_ERR(sel));
    }
    if (!(d->ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
}

static void far_ret(int os32, uint32_t imm)
{
    uint32_t sp = ESP, nip, ncs;
    desc_t d;
    nip = spop(&sp, os32);
    ncs = spop(&sp, os32) & 0xFFFF;
    if (!cpu.pm) {
        if (IS386 && nip > cpu.sg[CS].limit) {
            RAISE_C(EXC_GP, 0);
        }
        set_sp(sp + imm);
        seg_load_real(CS, ncs);
        cpu.ip = nip;
        return;
    }
    cs_check_return(ncs, &d);
    if (nip > d.limit) {
        RAISE_C(EXC_GP, 0);
    }
    if ((ncs & 3) == cpu.cpl) {
        set_sp(sp + imm);
        seg_set(CS, ncs, &d);
    } else {
        uint32_t nsp, nss;
        desc_t sd;
        sp += imm;
        nsp = spop(&sp, os32);
        nss = spop(&sp, os32) & 0xFFFF;
        ss_check(nss, ncs & 3, EXC_GP, &sd);
        seg_set(CS, ncs, &d);
        seg_set(SS, nss, &sd);
        set_sp(nsp + imm);
        drop_inner_segs();
    }
    cpu.ip = nip;
}

/* --- interrupts and exceptions ------------------------------------------------- */

static void int_real(uint32_t vector, uint32_t ret_ip)
{
    uint32_t sp = ESP, a = cpu.idt.base + vector * 4, nip, ncs;
    if (IS386 && vector * 4 + 3 > cpu.idt.limit) {
        RAISE_C(EXC_GP, vector * 8 + 2);
    }
    nip = phys_r16(a);
    ncs = phys_r16(a + 2);
    spush(&sp, get_eflags(), 0);
    spush(&sp, cpu.seg[CS], 0);
    spush(&sp, ret_ip, 0);
    set_sp(sp);
    cpu.f_if = 0;
    cpu.f_tf = 0;
    seg_load_real(CS, ncs);
    cpu.ip = nip;
}

/* soft: caused by an INT instruction (INT n, INT 3, INTO), which the gate's privilege
 * level must allow. */
static void int_pm(uint32_t vector, int soft, int has_code, uint32_t code, uint32_t ret_ip)
{
    const uint32_t ext = soft ? 0 : 1;
    const uint32_t gerr = vector * 8 + 2 + ext;
    uint32_t lo, hi, type, sel, off, fl, sp;
    int g32;
    desc_t d;

    if (vector * 8 + 7 > cpu.idt.limit) {
        RAISE_C(EXC_GP, gerr);
    }
    lo = sys_r32(cpu.idt.base + vector * 8);
    hi = sys_r32(cpu.idt.base + vector * 8 + 4);
    type = (hi >> 8) & 0x1F;
    if (type != 5 && (type & 0x16) != 6) {
        RAISE_C(EXC_GP, gerr);
    }
    if (soft && ((hi >> 13) & 3) < cpu.cpl) {
        RAISE_C(EXC_GP, vector * 8 + 2);
    }
    if (!(hi & 0x8000)) {
        RAISE_C(EXC_NP, gerr);
    }
    sel = lo >> 16;
    if (type == 5) {
        task_switch(sel, TS_CALL, ret_ip);
        if (has_code) {
            sp = ESP;
            spush(&sp, code, (cpu.tr.ar & 8) != 0);
            set_sp(sp);
        }
        return;
    }
    g32 = (type & 8) != 0;
    off = g32 ? (lo & 0xFFFF) | (hi & 0xFFFF0000u) : lo & 0xFFFF;
    if (SEL_NULL(sel)) {
        RAISE_C(EXC_GP, ext);
    }
    if (!desc_fetch(sel, &d) || (d.ar & (AR_S | AR_CODE)) != (AR_S | AR_CODE) ||
        AR_DPL(d.ar) > cpu.cpl) {
        RAISE_C(EXC_GP, SEL_ERR(sel) + ext);
    }
    if (!(d.ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel) + ext);
    }
    fl = get_eflags();
    if (!(d.ar & 4) && AR_DPL(d.ar) < cpu.cpl) {
        /* To a more privileged level, on the stack the task state segment names for it. */
        const uint32_t ncpl = AR_DPL(d.ar), old_ss = cpu.seg[SS], old_sp = ESP;
        uint32_t nss, nsp;
        desc_t sd;
        cpu_seg_t ns;
        if (cpu.f_vm && ncpl != 0) {
            RAISE_C(EXC_GP, SEL_ERR(sel) + ext);
        }
        tss_stack(ncpl, &nss, &nsp);
        ss_check(nss, ncpl, EXC_TS, &sd);
        seg_fill(&ns, &sd);
        if (cpu.f_vm) {
            npush(&ns, &nsp, cpu.seg[GS], g32);
            npush(&ns, &nsp, cpu.seg[FS], g32);
            npush(&ns, &nsp, cpu.seg[DS], g32);
            npush(&ns, &nsp, cpu.seg[ES], g32);
        }
        npush(&ns, &nsp, old_ss, g32);
        npush(&ns, &nsp, old_sp, g32);
        npush(&ns, &nsp, fl, g32);
        npush(&ns, &nsp, cpu.seg[CS], g32);
        npush(&ns, &nsp, ret_ip, g32);
        if (has_code) {
            npush(&ns, &nsp, code, g32);
        }
        if (off > d.limit) {
            RAISE_C(EXC_GP, 0);
        }
        if (cpu.f_vm) {
            cpu.f_vm = 0;
            update_mode();
            seg_null(ES, 0);
            seg_null(DS, 0);
            seg_null(FS, 0);
            seg_null(GS, 0);
        }
        desc_accessed(&d);
        seg_set(CS, (sel & ~3u) | ncpl, &d);
        seg_set(SS, nss, &sd);
        set_sp(nsp);
    } else {
        if (cpu.f_vm) {
            RAISE_C(EXC_GP, SEL_ERR(sel) + ext);
        }
        sp = ESP;
        spush(&sp, fl, g32);
        spush(&sp, cpu.seg[CS], g32);
        spush(&sp, ret_ip, g32);
        if (has_code) {
            spush(&sp, code, g32);
        }
        if (off > d.limit) {
            RAISE_C(EXC_GP, 0);
        }
        set_sp(sp);
        desc_accessed(&d);
        seg_set(CS, (sel & ~3u) | cpu.cpl, &d);
    }
    cpu.ip = off;
    cpu.f_tf = 0;
    cpu.f_nt = 0;
    cpu.f_rf = 0;
    if (!(type & 1)) {
        cpu.f_if = 0;
    }
}

static void do_interrupt(uint32_t vector, int soft, int has_code, uint32_t code, uint32_t ret_ip)
{
    if (cpu.cr0 & CR0_PE) {
        int_pm(vector, soft, has_code, code, ret_ip);
    } else {
        int_real(vector, ret_ip);
    }
}

static void iret_real(int os32)
{
    uint32_t sp = ESP, nip, ncs, nfl;
    nip = spop(&sp, os32);
    ncs = spop(&sp, os32) & 0xFFFF;
    nfl = spop(&sp, os32);
    if (IS386 && nip > cpu.sg[CS].limit) {
        RAISE_C(EXC_GP, 0);
    }
    set_sp(sp);
    seg_load_real(CS, ncs);
    cpu.ip = nip;
    if (cpu.f_vm) { /* virtual-8086 mode: IOPL stays */
        set_eflags(nfl, os32 ? 0x14FD5u : 0x4FD5u);
    } else {
        set_eflags(nfl, os32 ? 0x17FD5u : 0x7FD5u);
    }
}

static void do_iret(int os32)
{
    uint32_t sp = ESP, nip, ncs, nfl, mask;
    desc_t d;
    if (!cpu.pm) {
        if (cpu.f_vm && cpu.f_iopl != 3) {
            RAISE_C(EXC_GP, 0);
        }
        iret_real(os32);
        return;
    }
    if (cpu.f_nt) {
        task_switch(sys_r16(cpu.tr.base), TS_IRET, cpu.ip);
        return;
    }
    nip = spop(&sp, os32);
    ncs = spop(&sp, os32) & 0xFFFF;
    nfl = spop(&sp, os32);
    if (os32 && (nfl & FL_VM) && cpu.cpl == 0) {
        /* Back to virtual-8086 mode. */
        uint32_t nsp, v[6];
        static const uint8_t order[5] = {SS, ES, DS, FS, GS};
        nsp = spop(&sp, 1);
        for (int i = 0; i < 5; i++) {
            v[i] = spop(&sp, 1);
        }
        set_eflags(nfl, 0x37FD5u);
        seg_load_real(CS, ncs);
        for (int i = 0; i < 5; i++) {
            seg_load_real(order[i], v[i]);
        }
        ESP = nsp;
        cpu.ip = nip & 0xFFFF;
        return;
    }
    cs_check_return(ncs, &d);
    if (nip > d.limit) {
        RAISE_C(EXC_GP, 0);
    }
    mask = 0x4DD5u | (os32 ? FL_RF : 0);
    if (cpu.cpl == 0) {
        mask |= FL_IOPL;
    }
    if (cpu.cpl <= cpu.f_iopl) {
        mask |= FL_IF;
    }
    if ((ncs & 3) == cpu.cpl) {
        set_sp(sp);
        seg_set(CS, ncs, &d);
    } else {
        uint32_t nsp, nss;
        desc_t sd;
        nsp = spop(&sp, os32);
        nss = spop(&sp, os32) & 0xFFFF;
        ss_check(nss, ncs & 3, EXC_GP, &sd);
        seg_set(CS, ncs, &d);
        seg_set(SS, nss, &sd);
        set_sp(nsp);
        drop_inner_segs();
    }
    cpu.ip = nip;
    set_eflags(nfl, mask);
}

/* The EFLAGS bits POPF may change, by mode and privilege. */
static uint32_t popf_mask(int os32)
{
    uint32_t mask = 0x4DD5u | (os32 ? FL_RF : 0);
    if (!(cpu.cr0 & CR0_PE)) {
        return mask | FL_IOPL | FL_IF;
    }
    if (cpu.cpl == 0) {
        mask |= FL_IOPL;
    }
    if (cpu.cpl <= cpu.f_iopl) {
        mask |= FL_IF;
    }
    return mask;
}

/* --- I/O permission -------------------------------------------------------------- */

static void io_check(uint32_t port, uint32_t size)
{
    uint32_t base, bits;
    if (!(cpu.cr0 & CR0_PE) || (!cpu.f_vm && cpu.cpl <= cpu.f_iopl)) {
        return;
    }
    if (!(cpu.tr.ar & 8) || cpu.tr.limit < 0x67) {
        RAISE_C(EXC_GP, 0);
    }
    base = sys_r16(cpu.tr.base + 0x66);
    if (base + (port >> 3) + 1 > cpu.tr.limit) {
        RAISE_C(EXC_GP, 0);
    }
    bits = sys_r16(cpu.tr.base + base + (port >> 3)) >> (port & 7);
    if (bits & ((1u << size) - 1)) {
        RAISE_C(EXC_GP, 0);
    }
}

/* --- protected-mode instructions ------------------------------------------------- */

static void set_zf(int z) { set_eflags(z ? FL_ZF : 0, FL_ZF); }

/* A selector's descriptor, if code at the current privilege level may look at it. */
static int desc_visible(uint32_t sel, desc_t *d)
{
    if (SEL_NULL(sel) || !desc_fetch(sel, d)) {
        return 0;
    }
    if ((d->ar & (AR_S | AR_CODE | 4)) != (AR_S | AR_CODE | 4)) { /* not conforming code */
        if (AR_DPL(d->ar) < cpu.cpl || AR_DPL(d->ar) < (sel & 3)) {
            return 0;
        }
    }
    return 1;
}

/* LAR (what = 0), LSL (1): sets ZF and gives the value if the selector is one the program
 * may ask about. */
typedef struct {
    uint32_t ok, value;
} lar_t;
static lar_t do_lar_lsl(int what, uint32_t sel)
{
    static const uint16_t ok[2] = {0x1A3E, 0x0A0E}; /* system types each accepts */
    desc_t d;
    lar_t r = {0, 0};
    if (desc_visible(sel, &d) && ((d.ar & AR_S) || ((ok[what] >> (d.ar & 15)) & 1))) {
        r.ok = 1;
        r.value = what ? d.limit : d.hi & 0x00F0FF00u;
    }
    set_zf((int)r.ok);
    return r;
}

/* VERR (0), VERW (1) */
static void do_verify(int write, uint32_t sel)
{
    desc_t d;
    int ok = desc_visible(sel, &d) && (d.ar & AR_S);
    if (ok) {
        if (write) {
            ok = (d.ar & (AR_CODE | 2)) == 2;
        } else {
            ok = (d.ar & (AR_CODE | 2)) != AR_CODE;
        }
    }
    set_zf(ok);
}

static void do_ltr(uint32_t sel)
{
    desc_t d;
    if (SEL_NULL(sel)) {
        RAISE_C(EXC_GP, 0);
    }
    if ((sel & 4) || !desc_fetch(sel, &d) || (d.ar & 0x17) != 1) {
        RAISE_C(EXC_GP, SEL_ERR(sel));
    }
    if (!(d.ar & AR_P)) {
        RAISE_C(EXC_NP, SEL_ERR(sel));
    }
    sys_w8(d.addr + 5, d.ar | 2);
    cpu.tr_sel = (uint16_t)sel;
    cpu.tr.base = d.base;
    cpu.tr.limit = d.limit;
    cpu.tr.ar = d.ar | 2;
}

/* MOV from (dir = 0) or to a control, debug or test register. */
static void mov_special(uint32_t op, uint32_t n, uint32_t gr)
{
    if (cpu.cpl != 0) {
        RAISE_C(EXC_GP, 0);
    }
    switch (op) {
    case 0x20:
        R32(gr) = n == 0 ? cpu.cr0 | 0x7FFFFFE0u | (cpu_fpu_fitted ? 0x10 : 0) /* ET: an 80387 */
                  : n == 2 ? cpu.cr2 : n == 3 ? cpu.cr3 : 0;
        break;
    case 0x22:
        if (n == 0) {
            set_cr0(R32(gr));
        } else if (n == 2) {
            cpu.cr2 = R32(gr);
        } else if (n == 3) {
            cpu.cr3 = R32(gr) & ~0xFFFu;
            tlb_flush();
        }
        break;
    case 0x21: R32(gr) = cpu.dr[n]; break;
    case 0x23: cpu.dr[n] = R32(gr); break;
    case 0x24: R32(gr) = n >= 6 ? cpu.tr67[n - 6] : 0; break;
    default:
        if (n >= 6) {
            cpu.tr67[n - 6] = R32(gr);
        }
        break;
    }
}

/* --- delivering an exception ----------------------------------------------------- */

static int exc_contributory(uint32_t v)
{
    return v == EXC_DE || (v >= EXC_TS && v <= EXC_GP);
}

/* Enter the handler of the exception recorded by raise_exc(). Run by cpu_run() under its
 * own setjmp: a fault in here comes back as another call. */
static void exc_deliver(void)
{
    uint32_t v = cpu.exc_vector;
    static uint8_t first;
    cpu.exc_pending = 0;
    if (cpu_exc_trace) {
        cpu_exc_trace((int)v, cpu.exc_code);
    }
    if (cpu.exc_busy) {
        if (first == EXC_DF) {
            cpu.exc_busy = 0;
            pc_cpu_shutdown(); /* triple fault */
            return;
        }
        if ((exc_contributory(first) && exc_contributory(v)) ||
            (first == EXC_PF && (v == EXC_PF || exc_contributory(v)))) {
            v = EXC_DF;
            cpu.exc_has_code = 1;
            cpu.exc_code = 0;
        }
    }
    first = (uint8_t)v;
    cpu.exc_busy = 1;
    cpu.ip = cpu.insn_ip;
    cpu.halted = 0;
    do_interrupt(v, 0, cpu.exc_has_code && (cpu.cr0 & CR0_PE), cpu.exc_code, cpu.ip);
    cpu.exc_busy = 0;
}
