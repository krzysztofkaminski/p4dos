/* Bare-metal entry for the QEMU "virt" RISC-V machine (RV32, machine mode). */
    .section .init, "ax"
    .globl _start
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      t0, trap_entry
    csrw    mtvec, t0
    csrr    t0, mhartid
    bnez    t0, 3f                  /* other harts (qemu -smp 2) are parked */
    la      sp, __stack_top
    la      a0, __bss_start
    la      a1, __bss_end
1:  bgeu    a0, a1, 2f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       1b
2:  call    main
3:  wfi
    j       3b

    .align  2
trap_entry:
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval
    la      sp, __stack_top
    call    trap_report
4:  wfi
    j       4b
