; T186.COM -- self-checking test of the instructions the 80186 added to the 8086
; (the SingleStepTests suite used for the rest of the CPU only covers the 8088).
        bits 16
        cpu 186
        org 0x100

%macro CHECK 2                          ; CHECK reg, expected
        cmp %1, %2
        jne fail
%endmacro

        mov word [step], 1              ; PUSH imm16 / PUSH imm8 (sign-extended)
        push 0x1234
        pop ax
        CHECK ax, 0x1234
        push byte -2
        pop ax
        CHECK ax, 0xFFFE

        inc word [step]                 ; 2: PUSHA / POPA
        mov ax, 0x1111
        mov cx, 0x2222
        mov dx, 0x3333
        mov bx, 0x4444
        mov bp, 0x5555
        mov si, 0x6666
        mov di, 0x7777
        mov [saved_sp], sp
        pusha
        mov bp, sp
        CHECK word [bp + 14], 0x1111    ; AX is pushed first
        CHECK word [bp + 0], 0x7777     ; DI last
        mov ax, [saved_sp]
        CHECK word [bp + 6], ax         ; the SP slot holds the value before PUSHA
        xor ax, ax
        xor cx, cx
        xor dx, dx
        xor bx, bx
        xor si, si
        xor di, di
        popa
        CHECK ax, 0x1111
        CHECK cx, 0x2222
        CHECK dx, 0x3333
        CHECK bx, 0x4444
        CHECK bp, 0x5555
        CHECK si, 0x6666
        CHECK di, 0x7777
        CHECK sp, [saved_sp]

        inc word [step]                 ; 3: IMUL r16, r/m16, imm
        mov bx, 300
        imul ax, bx, 100
        jo fail
        CHECK ax, 30000
        imul ax, bx, -7
        CHECK ax, -2100
        imul ax, bx, 1000               ; 300000 overflows 16 bits
        jno fail
        mov word [value], -5
        imul cx, [value], 12
        CHECK cx, -60

        inc word [step]                 ; 4: shifts and rotates by an immediate count
        mov ax, 0x8001
        shl ax, 4
        CHECK ax, 0x0010
        mov ax, 0x8001
        shr ax, 3
        CHECK ax, 0x1000
        mov ax, 0x8010
        sar ax, 4
        CHECK ax, 0xF801
        mov ax, 0x8001
        rol ax, 4
        CHECK ax, 0x0018
        mov ax, 0x8001
        ror ax, 1
        jnc fail
        CHECK ax, 0xC000
        mov al, 0x81
        stc
        rcl al, 2
        CHECK al, 0x07
        mov byte [value], 0xF0
        shr byte [value], 4
        CHECK byte [value], 0x0F
        mov ax, 1                       ; the count is taken modulo 32
        mov cl, 33
        shl ax, cl
        CHECK ax, 2

        inc word [step]                 ; 5: ENTER / LEAVE
        mov bp, 0x5A5A
        mov bx, sp
        enter 8, 0
        mov ax, bx
        sub ax, 2
        CHECK bp, ax                    ; BP = SP after pushing the old BP
        sub ax, 8
        CHECK sp, ax
        CHECK word [bp], 0x5A5A
        leave
        CHECK bp, 0x5A5A
        CHECK sp, bx
        enter 4, 1                      ; nesting level 1 pushes the new frame pointer
        mov ax, bx
        sub ax, 2
        CHECK bp, ax
        CHECK word [bp - 2], ax
        sub ax, 6
        CHECK sp, ax
        leave
        CHECK sp, bx

        inc word [step]                 ; 6: BOUND inside the limits does not trap
        mov ax, 5
        bound ax, [limits]
        mov ax, -3
        bound ax, [limits]

        inc word [step]                 ; 7: undocumented flag bits read as on an 80186
        pushf
        pop ax
        and ax, 0xF000
        CHECK ax, 0xF000

        mov dx, m_ok
        jmp done
fail:   mov ax, [step]
        add al, '0'
        mov [m_step], al
        mov dx, m_fail
done:   mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

step:     dw 0
saved_sp: dw 0
value:    dw 0
limits:   dw -3, 5
m_ok:     db '80186 instruction test passed', 13, 10, '$'
m_fail:   db '80186 instruction test FAILED at step '
m_step:   db '0', 13, 10, '$'
