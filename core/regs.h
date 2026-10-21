/* Register shorthands for the BIOS and device code. */
#ifndef P4DOS_REGS_H
#define P4DOS_REGS_H

#include "cpu.h"

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
#define CH cpu.r.b[5]
#define DL cpu.r.b[8]
#define DH cpu.r.b[9]
#define BL cpu.r.b[12]
#define BH cpu.r.b[13]
#define ES cpu.seg[SEG_ES]
#define CS cpu.seg[SEG_CS]
#define SS cpu.seg[SEG_SS]
#define DS cpu.seg[SEG_DS]

#endif
