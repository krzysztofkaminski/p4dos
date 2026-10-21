/*
 * x86 CPU interpreter: 8086, 80186 and 80386DX.
 *
 * One global CPU instance. Physical memory is a flat array (cpu_mem): the first megabyte,
 * and above it, for the 80386, extended memory up to cpu_ram_size. Port I/O, interrupt
 * acknowledge and the BIOS hypercall are provided by the machine layer (pc.c).
 *
 * The 80386 has the complete programming model: 32-bit operands and addressing, real,
 * protected and virtual-8086 mode, segmentation with its privilege rules, call, interrupt
 * and task gates, task switches and paging, and optionally an 80387 coprocessor.
 */
#ifndef P4DOS_CPU_H
#define P4DOS_CPU_H

#include <setjmp.h>
#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the register file layout assumes a little-endian host"
#endif

enum { CPU_8086 = 0, CPU_80186 = 1, CPU_80386 = 3 };
enum { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3, SEG_FS = 4, SEG_GS = 5 };
enum { REG_AX = 0, REG_CX, REG_DX, REG_BX, REG_SP, REG_BP, REG_SI, REG_DI };

#define CPU_MEM_SIZE 0x100000u /* the first megabyte */
#define CPU_MEM_MASK 0x0FFFFFu

/* FLAGS bits */
#define FL_CF 0x0001
#define FL_PF 0x0004
#define FL_AF 0x0010
#define FL_ZF 0x0040
#define FL_SF 0x0080
#define FL_TF 0x0100
#define FL_IF 0x0200
#define FL_DF 0x0400
#define FL_OF 0x0800
#define FL_IOPL 0x3000
#define FL_NT 0x4000
#define FL_RF 0x10000
#define FL_VM 0x20000

/* CR0 bits */
#define CR0_PE 0x00000001u
#define CR0_MP 0x00000002u
#define CR0_EM 0x00000004u
#define CR0_TS 0x00000008u
#define CR0_PG 0x80000000u

/* A segment register: the selector's descriptor as the processor caches it, and how the
 * interpreter reaches the segment's memory.
 *
 * The window is a range of offsets, starting at lo, whose bytes are plain RAM that the
 * host can address as mem + offset. One unsigned compare of (offset - lo) against rd[] or
 * wr[] (indexed by the access size: 1, 2 or 4 bytes) both checks the segment limit and
 * tells whether the access can be done right there. Everything else (limit violations,
 * ROM, video memory, paging, the A20 wrap-around) is sorted out by slower code. */
typedef struct {
    uint8_t *mem;
    uint32_t lo;
    uint32_t rd[3], wr[3];
    uint32_t base;
    uint32_t limit; /* last valid offset; for expand-down segments, the one before the first */
    uint8_t flags;  /* CPU_SF_* */
    uint8_t ar;     /* access rights byte of the descriptor: P, DPL, S, type */
} cpu_seg_t;

#define CPU_SF_R 0x01    /* readable */
#define CPU_SF_W 0x02    /* writable */
#define CPU_SF_DOWN 0x04 /* expand-down data segment */
#define CPU_SF_BIG 0x08  /* the D/B bit: 32-bit code, 32-bit stack pointer */

typedef struct {
    uint32_t base;
    uint32_t limit;
} cpu_dtr_t;

typedef struct {
    union {
        /* EAX ECX EDX EBX ESP EBP ESI EDI; d[8] stays zero (address decoding) */
        uint32_t d[10];
        uint16_t w[20]; /* AX is w[0], CX w[2], ... */
        uint8_t b[40];  /* AL b[0], AH b[1], CL b[4], ... */
    } r;
    uint16_t seg[6]; /* selectors: ES CS SS DS FS GS */
    cpu_seg_t sg[6];
    uint32_t ip;

    /* Lazily evaluated arithmetic flags:
     *   ZF = (f_zr == 0)
     *   SF = bit 31 of f_sp, PF = even parity of the low byte of f_sp
     *   CF = bit 8 of f_co, OF = bit 7 ^ bit 8 of f_co
     *   AF = bit 4 of f_ax */
    uint32_t f_zr, f_sp, f_co, f_ax;
    uint8_t f_tf, f_if, f_df;
    uint8_t f_iopl, f_nt, f_rf, f_vm; /* 80386 */

    uint8_t model;       /* CPU_8086, CPU_80186 or CPU_80386 */
    uint8_t halted;      /* HLT (or a BIOS wait) until the next interrupt */
    uint8_t irq_line;    /* INTR input, driven by the PIC */
    uint8_t trap_armed;  /* single-step trap due after the current instruction */
    uint8_t test_mode;   /* run exactly one instruction per slice (CPU test harness) */
    uint8_t idle_waited; /* the Windows idle call about to be made has waited for an interrupt */
    uint8_t reg8_idx[8]; /* byte register number -> index into r.b */

    int32_t slice; /* instructions left before the next event check */
    int32_t saved; /* rest of the cpu_run() budget, parked while a short slice runs */
    int32_t run_n; /* budget of the cpu_run() call in progress, 0 outside */

    /* Where instructions are fetched from: the host address of CS:0, good for the offsets
     * from win_lo up to, but not including, lim1. lim1 = 0 when there is no such range. */
    const uint8_t *code;
    uint32_t win_lo, lim1;
    uint32_t insn_ip; /* start of the instruction being executed (for faults) */

    /* 80386 system state */
    uint8_t pm;    /* protected mode proper: CR0.PE set and not virtual-8086 mode */
    uint8_t cpl;   /* current privilege level */
    uint8_t dmode; /* bit 0: 32-bit code segment, bit 1: 32-bit stack segment */
    uint8_t a20;   /* address line 20 enabled */
    uint32_t a20_mask;
    uint32_t cr0, cr2, cr3;
    uint32_t dr[8];
    uint32_t tr67[2]; /* test registers TR6, TR7 */
    cpu_dtr_t gdt, idt;
    uint16_t ldt_sel, tr_sel;
    cpu_seg_t ldt, tr; /* base, limit and access rights only */

    /* An exception on its way to its handler. */
    uint8_t exc_pending, exc_vector, exc_has_code, exc_busy;
    uint32_t exc_code;
    uint8_t in_run;
    jmp_buf exc_jmp;

    uint8_t wrapped[32]; /* copy of an instruction that straddles the end of something */
    uint64_t icount;     /* total instructions executed */
} cpu_t;

extern cpu_t cpu;
/* cpu_ram_size bytes (CPU_MEM_SIZE for the 8086 and 80186) plus 16; set both before
 * cpu_init(). */
extern uint8_t *cpu_mem;
extern uint32_t cpu_ram_size;
/* Set before cpu_init() to give the 80386 an 80387 numeric coprocessor. */
extern int cpu_fpu_fitted;

void cpu_init(int model);
void cpu_reset(void);
/* Run up to n instructions; returns the number executed. Stops early when halted. */
int cpu_run(int n);
/* Ask the run loop to re-check interrupts after the current instruction. */
void cpu_break(void);
/* Enter the handler of an interrupt, as the INTR pin would. To be called between
 * instructions (the BIOS hypercall is such a point). */
void cpu_interrupt(uint8_t vector);
/* Instructions executed so far by the cpu_run() call in progress. */
static inline int32_t cpu_executed(void) { return cpu.run_n - cpu.saved - cpu.slice; }

/* The low 16 bits of EFLAGS, and all of it. */
uint16_t cpu_get_flags(void);
void cpu_set_flags(uint16_t f);
uint32_t cpu_get_eflags(void);
void cpu_set_eflags(uint32_t f);
/* Load a segment register the real-mode way: base = selector * 16. */
void cpu_set_seg(int s, uint16_t v);
/* Within the first megabyte, physical addresses below these limits are plain RAM and
 * accessed directly; at or above, the machine layer decides (ROM, unpopulated space, VGA
 * planar memory). */
void cpu_set_mem_limits(uint32_t rd, uint32_t wr);
/* The A20 gate (80386): when off, bit 20 of every physical address is forced to zero. */
void cpu_set_a20(int on);
/* Host address of n bytes of guest memory at a linear address, as the current paging
 * setup maps them; NULL when they are not present or not n contiguous bytes of RAM. For
 * the BIOS, which works on behalf of real-mode or virtual-8086 code. */
uint8_t *cpu_lin_ptr(uint32_t lin, uint32_t n, int write);

/* Debugging aid: called for every exception the 80386 raises, when set. */
extern void (*cpu_exc_trace)(int vector, uint32_t code);

/* Provided by the machine layer. */
uint8_t pc_inb(uint16_t port);
void pc_outb(uint16_t port, uint8_t v);
uint8_t pc_irq_ack(void);
void pc_hypercall(uint8_t fn);
void pc_invalid_opcode(uint8_t op);
uint8_t pc_mem_read(uint32_t addr);
void pc_mem_write(uint32_t addr, uint8_t v);
/* The CPU shut down (triple fault): the machine resets it. */
void pc_cpu_shutdown(void);
/* The coprocessor reports an unmasked exception (a PC raises interrupt request 13). */
void pc_fpu_error(void);

#endif
