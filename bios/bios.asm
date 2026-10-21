; P4DOS system BIOS, mapped at F000:0000-FFFF (64 KiB).
;
; Nearly every service is implemented in C inside the emulator (core/bios.c,
; core/biosvid.c, core/disk.c). The stubs here trap into it with the two-byte
; hypercall "F1 nn", which the CPU core honours only while CS = F000h. Hardware
; interrupt stubs still talk to the emulated PIC/keyboard ports so that DOS
; programs hooking INT 8/9/15h/1Ch see the usual behaviour.
;
; Entry points and tables sit at the classic IBM PC addresses where software is
; known to depend on them.

        bits 16
        cpu 186
        org 0

%macro HCALL 1
        db 0xF1, %1
%endmacro

%macro AT 1
        times %1-($-$$) db 0xFF
%endmacro

HC_POST   equ 0xE0
HC_BOOT   equ 0xE1
HC_KEY    equ 0xE2
HC_TICK   equ 0xE3
HC_INVOP  equ 0xE4
HC_EMS    equ 0xE7
HC_XMS    equ 0xE8

; --- header read by the C side ------------------------------------------------
        db 'GDOSBIOS'                   ; 00  what EMM.SYS and XMS.SYS look for
        dw ivt_tab                      ; 08
        dw font8x16                     ; 0A
        dw font8x8_lo                   ; 0C
        dw font8x8_hi                   ; 0E
        dw hd0_parm                     ; 10
        dw hd1_parm                     ; 12
        dw fd_parm                      ; 14
        dw ems_entry                    ; 16
        dw font8x14                     ; 18
        dw xms_entry                    ; 1A

; Vectors to install at POST: count, then (vector, offset) pairs. Everything
; else points at dummy_iret.
ivt_tab:
        dw (ivt_end - ivt_tab - 2) / 4
        dw 0x06, int06
        dw 0x08, int08
        dw 0x09, int09
        dw 0x0A, irq_default
        dw 0x0B, irq_default
        dw 0x0C, irq_default
        dw 0x0D, irq_default
        dw 0x0E, irq_default
        dw 0x0F, irq_default
        dw 0x10, int10
        dw 0x11, int11
        dw 0x12, int12
        dw 0x13, int13
        dw 0x14, int14
        dw 0x15, int15
        dw 0x16, int16
        dw 0x17, int17
        dw 0x18, int18
        dw 0x19, int19
        dw 0x1A, int1a
        dw 0x1D, vid_parm
        dw 0x1E, fd_parm
        dw 0x1F, font8x8_hi
        dw 0x40, int13
        dw 0x41, hd0_parm
        dw 0x46, hd1_parm
        dw 0x70, irq_slave
        dw 0x71, irq_slave
        dw 0x72, irq_slave
        dw 0x73, irq_slave
        dw 0x74, irq_slave
        dw 0x75, int75
        dw 0x76, irq_slave
        dw 0x77, irq_slave
ivt_end:

font8x16:
        incbin "font8x16.bin"
font8x14:
        incbin "font8x14.bin"
font8x8_hi:
        incbin "font8x8.bin", 1024, 1024

irq_default:
        push ax
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

; Interrupts of the second controller (IRQ 8-15; 80386 machine only).
irq_slave:
        push ax
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop ax
        iret

; IRQ 13, the coprocessor's error line: release it and pass the event on as a PC does,
; through the NMI vector that programs written for the 8087 hook.
int75:  push ax
        xor al, al
        out 0xF0, al
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop ax
        int 0x02
        iret

int06:  HCALL HC_INVOP
        iret

int18:  HCALL 0x18
        jmp int19

; Far entry used by the EMS driver (EMM.SYS): CALL FAR F000:ems_entry from its
; INT 67h handler, which then IRETs to the application.
ems_entry:
        HCALL HC_EMS
        retf

; The same for the XMS driver (XMS.SYS): its control entry far-calls this.
xms_entry:
        HCALL HC_XMS
        retf

; --- power-on / reset ---------------------------------------------------------
        AT 0xE05B
post:   cli
        cld
        xor ax, ax
        mov ss, ax
        mov sp, 0x7C00
        mov ds, ax
        mov es, ax
        HCALL HC_POST
        sti
        int 0x19
.halt:  hlt
        jmp .halt

        AT 0xE401
hd0_parm:
        times 16 db 0
hd1_parm:
        times 16 db 0

        AT 0xE6F2
int19:  sti
.again: HCALL HC_BOOT                   ; jumps to 0000:7C00 on success
        xor ax, ax
        int 0x16
        jmp .again

        AT 0xE739
int14:  HCALL 0x14
        iret

        AT 0xE82E
int16:  sti
        HCALL 0x16
        iret

        AT 0xE987
int09:  push ax
        in al, 0x60
        mov ah, 0x4F
        stc
        int 0x15                        ; keyboard intercept
        jnc .eoi
        HCALL HC_KEY                    ; AL = scancode; AH = follow-up action
        cmp ah, 1
        je .break
        cmp ah, 2
        je .prtsc
.eoi:   mov al, 0x20
        out 0x20, al
        pop ax
        iret
.break: mov al, 0x20
        out 0x20, al
        int 0x1B
        pop ax
        iret
.prtsc: mov al, 0x20
        out 0x20, al
        int 0x05
        pop ax
        iret

        AT 0xEC59
int13:  HCALL 0x13
        iret

        AT 0xEFC7
fd_parm:
        db 0xDF, 0x02, 0x25, 0x02, 18, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08

        AT 0xEFD2
int17:  HCALL 0x17
        iret

        AT 0xF065
int10:  HCALL 0x10
        iret

        AT 0xF0A4
vid_parm:
        db 0x38, 0x28, 0x2D, 0x0A, 0x1F, 0x06, 0x19, 0x1C, 0x02, 0x07, 0x06, 0x07, 0, 0, 0, 0
        db 0x71, 0x50, 0x5A, 0x0A, 0x1F, 0x06, 0x19, 0x1C, 0x02, 0x07, 0x06, 0x07, 0, 0, 0, 0
        db 0x38, 0x28, 0x2D, 0x0A, 0x7F, 0x06, 0x64, 0x70, 0x02, 0x01, 0x06, 0x07, 0, 0, 0, 0
        db 0x61, 0x50, 0x52, 0x0F, 0x19, 0x06, 0x19, 0x19, 0x02, 0x0D, 0x0B, 0x0C, 0, 0, 0, 0
        dw 0x0800, 0x1000, 0x4000, 0x4000
        db 40, 40, 80, 80, 40, 40, 80, 80
        db 0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29

        AT 0xF841
int12:  HCALL 0x12
        iret

        AT 0xF84D
int11:  HCALL 0x11
        iret

        AT 0xF859
int15:  HCALL 0x15
        iret

        AT 0xFA6E
font8x8_lo:
        incbin "font8x8.bin", 0, 1024

        AT 0xFE6E
int1a:  HCALL 0x1A
        iret

        AT 0xFEA5
int08:  push ax
        HCALL HC_TICK
        int 0x1C
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

        AT 0xFF53
dummy_iret:
        iret

        AT 0xFFF0
        jmp 0xF000:post
        db '10/08/26'
        db 0
        db 0xFE                         ; model: PC/XT; pc.c makes it FCh (AT) for the 80386
        db 0
