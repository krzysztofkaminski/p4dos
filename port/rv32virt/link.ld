/* QEMU virt: RAM at 0x80000000. The disk image is loaded at 0x84000000 by QEMU. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64M
}

SECTIONS
{
    .text : {
        KEEP(*(.init))
        *(.text .text.*)
    } > RAM
    .rodata : ALIGN(8) {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > RAM
    .data : ALIGN(8) {
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.* .data .data.*)
    } > RAM
    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM
    . = ALIGN(16);
    __heap_start = .;
    __heap_end = ORIGIN(RAM) + LENGTH(RAM) - 0x40000;
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
