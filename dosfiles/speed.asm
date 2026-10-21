; SPEED.COM -- how fast is this machine? Runs four instruction mixes for about a second
; each, timed by the BIOS clock tick, and prints thousands of instructions per second.
;
;   register   arithmetic between registers
;   memory     loads, stores and read-modify-write through pointers
;   stack      push/pop and near call/return
;   branch     compares and conditional jumps
;   string     REP MOVSW block copies, in KB per second
;
; For comparison: an IBM PC/XT runs these at very roughly 300, a 12 MHz 286 at 2,500 and a
; 33 MHz 386DX at 9,000.

        bits 16
        cpu 8086
        org 0x100

TICKS   equ 18                          ; measuring time per test: 18 ticks = 0.989 s

%macro RUN 2                            ; RUN label-of-block, instructions per block
        mov word [block], %1
        call measure                    ; -> DX:AX = blocks done
        mov bx, %2
        call mul32                      ; DX:AX = thousands of instructions (blocks of 1000)
        add [total], ax
        adc [total + 2], dx
        call print_dec
        mov dx, m_kips
        call print
%endmacro

        mov dx, m_title
        call print

        mov dx, m_reg
        call print
        RUN blk_reg, 5
        mov dx, m_mem
        call print
        RUN blk_mem, 5
        mov dx, m_stack
        call print
        RUN blk_stack, 5
        mov dx, m_branch
        call print
        RUN blk_branch, 6

        mov dx, m_avg
        call print
        mov ax, [total]
        mov dx, [total + 2]
        shr dx, 1                       ; / 4
        rcr ax, 1
        shr dx, 1
        rcr ax, 1
        call print_dec
        mov dx, m_kips
        call print

        mov dx, m_string
        call print
        mov word [block], blk_string
        call measure                    ; blocks of 16 KB
        mov bx, 16
        call mul32
        call print_dec
        mov dx, m_kbs
        call print
        mov ax, 0x4C00
        int 0x21

; DX:AX = DX:AX * BX
mul32:  push cx
        mov cx, dx
        mul bx                          ; low word
        xchg ax, cx                     ; cx = low result, ax = old high word
        push dx
        mul bx
        pop dx
        add dx, ax
        mov ax, cx
        pop cx
        ret

; Run the block at [block] repeatedly for TICKS clock ticks; DX:AX = number of runs.
measure:
        push es
        xor ax, ax
        mov es, ax
        mov ax, [es:0x46C]
.edge:  cmp ax, [es:0x46C]              ; start right after a tick
        je .edge
        mov ax, [es:0x46C]
        add ax, TICKS
        mov [until], ax
        xor bp, bp
        mov word [runs_hi], 0
.again: call [block]
        inc bp
        jnz .nc
        inc word [runs_hi]
.nc:    xor ax, ax
        mov es, ax
        mov ax, [es:0x46C]
        cmp ax, [until]
        jne .again
        mov ax, bp
        mov dx, [runs_hi]
        pop es
        ret

; Each block executes 1000 times the stated number of instructions.
blk_reg:
        mov cx, 1000
.l:     add ax, bx
        xor dx, ax
        inc bx
        dec dx
        loop .l
        ret

blk_mem:
        mov cx, 1000
        mov si, data
        mov di, data + 8
        mov bx, data + 16
.l:     mov ax, [si]
        add ax, [si + 2]
        mov [di], ax
        inc word [bx]
        loop .l
        ret

blk_stack:
        mov cx, 1000
.l:     push ax
        call .sub
        pop ax
        loop .l
        ret
.sub:   ret

blk_branch:
        mov cx, 1000
.l:     test al, 1
        jz .a
        inc dx
.a:     cmp ax, bx
        jne .b
        nop
.b:     inc ax
        loop .l
        ret

blk_string:                             ; copy 16 KB inside our own segment
        push ds
        pop es
        mov si, buffer
        mov di, buffer + 0x4000
        mov cx, 0x2000
        cld
        rep movsw
        ret

print:  mov ah, 9
        int 0x21
        ret

; Print DX:AX in decimal, right-aligned in 7 columns.
print_dec:
        mov di, numbuf + 7
        mov cx, 7
.digit: push cx
        mov cx, 10                      ; DX:AX / 10 -> DX:AX, remainder in BX
        push ax
        mov ax, dx
        xor dx, dx
        div cx
        mov si, ax
        pop ax
        div cx
        mov bx, dx
        mov dx, si
        pop cx
        dec di
        add bl, '0'
        mov [di], bl
        mov bx, ax
        or bx, dx
        jz .pad
        loop .digit
        jmp .out
.pad:   dec cx
        jz .out
.blank: dec di
        mov byte [di], ' '
        loop .blank
.out:   mov dx, numbuf
        jmp print

m_title:  db 'CPU speed, thousands of instructions per second', 13, 10, '$'
m_reg:    db '  register $'
m_mem:    db '  memory   $'
m_stack:  db '  stack    $'
m_branch: db '  branch   $'
m_avg:    db '  average  $'
m_string: db '  string   $'
m_kips:   db ' KIPS', 13, 10, '$'
m_kbs:    db ' KB/s', 13, 10, '$'
numbuf:   db '       $'
block:    dw 0
until:    dw 0
runs_hi:  dw 0
total:    dd 0
data:     times 24 db 0
buffer:
