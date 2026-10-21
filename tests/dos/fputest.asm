; FPUTEST.COM: exercises the 80387. Results are compared with their exact bit patterns
; where the chip defines them, within a tolerance for the transcendental functions.
; Prints one line saying how it went.
        org 0x100
        bits 16
        cpu 386

%macro STEP 0
        inc word [step]
%endmacro
%macro EXPECT80 2                       ; ST(0), popped, must be exactly %1:%2
        fstp tword [r80]
        cmp dword [r80], (%2) & 0xFFFFFFFF
        jne fail
        cmp dword [r80 + 4], (%2) >> 32
        jne fail
        cmp word [r80 + 8], %1
        jne fail
%endmacro
%macro NEAR 1                           ; |ST(0) - %1| < 1e-15, popped
        fsub qword [%1]
        fabs
        fcomp qword [eps]
        fnstsw ax
        sahf
        jae fail
%endmacro

start:  STEP                            ; 1: there is a coprocessor, in its reset state
        fninit
        mov word [w], 0x5A5A
        fnstsw [w]
        cmp word [w], 0
        jne fail
        fnstcw [w]
        cmp word [w], 0x037F
        jne fail

        STEP                            ; 2: division and square root, to the last bit
        fld1
        fidiv word [three]
        EXPECT80 0x3FFD, 0xAAAAAAAAAAAAAAAB
        fld1
        fadd st0, st0
        fsqrt
        EXPECT80 0x3FFF, 0xB504F333F9DE6484
        fldpi
        EXPECT80 0x4000, 0xC90FDAA22168C235

        STEP                            ; 3: 64-bit integers and packed decimal pass through
        fild qword [big]
        fistp qword [r80]
        mov eax, [r80]
        cmp eax, [big]
        jne fail
        mov eax, [r80 + 4]
        cmp eax, [big + 4]
        jne fail
        fbld [bcd]
        fbstp [r80]
        mov si, bcd
        mov di, r80
        mov cx, 10
        repe cmpsb
        jne fail

        STEP                            ; 4: double precision arithmetic
        fld qword [d1_5]
        fmul qword [d2_25]
        fstp qword [r80]
        cmp dword [r80], 0
        jne fail
        cmp dword [r80 + 4], 0x400B0000 ; 3.375
        jne fail
        fld dword [f0_1]                ; 0.1f + 0.2f, rounded to single
        fadd dword [f0_2]
        fstp dword [r80]
        cmp dword [r80], 0x3E99999A
        jne fail

        STEP                            ; 5: comparison and the condition codes
        fld1
        fldz
        fcompp                          ; 0 < 1
        fnstsw ax
        sahf
        jnb fail
        fldz
        ftst
        fnstsw ax
        fstp st0
        sahf
        jne fail
        fld1
        fchs
        fxam                            ; a negative normal number: C2 and C1
        fnstsw ax
        fstp st0
        and ax, 0x4700
        cmp ax, 0x0600
        jne fail

        STEP                            ; 6: rounding control
        fld qword [d2_7]
        fistp word [w]
        cmp word [w], 3
        jne fail
        mov word [cw], 0x0F7F           ; toward zero
        fldcw [cw]
        fld qword [d2_7]
        fistp word [w]
        cmp word [w], 2
        jne fail
        fld qword [d2_7]
        frndint
        fistp word [w]
        cmp word [w], 2
        jne fail
        mov word [cw], 0x037F
        fldcw [cw]

        STEP                            ; 7: remainder, scaling, exponent extraction
        fild word [three]
        fild word [ten]
        fprem                           ; 10 mod 3 = 1, quotient 3: C1 and C3
        fnstsw ax
        and ax, 0x4700
        cmp ax, 0x4200
        jne fail
        fld1
        fcompp
        fnstsw ax
        fstp st0
        sahf
        jne fail
        fild word [three]
        fld1
        fscale                          ; 1 * 2^3
        fistp word [w]
        fstp st0
        cmp word [w], 8
        jne fail
        fild word [ten]
        fxtract                         ; 10 = 1.25 * 2^3
        fstp qword [r80]
        fistp word [w]
        cmp word [w], 3
        jne fail
        cmp dword [r80 + 4], 0x3FF40000
        jne fail

        STEP                            ; 8: the transcendental functions
        fldpi
        fidiv word [six]
        fsin                            ; sin(pi/6) = 0.5
        NEAR d0_5
        fldpi
        fidiv word [three]
        fcos                            ; cos(pi/3) = 0.5
        NEAR d0_5
        fld1
        fld1
        fpatan                          ; atan(1) = pi/4
        fimul word [four]
        fldpi
        fsubp st1, st0
        fabs
        fcomp qword [eps]
        fnstsw ax
        sahf
        jae fail
        fld1
        fild word [eight]
        fyl2x                           ; log2(8) = 3
        fisub word [three]
        fabs
        fcomp qword [eps]
        fnstsw ax
        sahf
        jae fail
        fld1
        f2xm1                           ; 2^1 - 1 = 1
        fld1
        fsubp st1, st0
        fabs
        fcomp qword [eps]
        fnstsw ax
        sahf
        jae fail
        fldpi
        fidiv word [four]
        fptan                           ; tan(pi/4) = 1, and 1.0 on top of it
        fcomp qword [d1_0]
        fnstsw ax
        sahf
        jne fail
        NEAR d1_0

        STEP                            ; 9: the stack is empty now; popping it is a fault
        fnstsw ax
        test ax, 0x3800
        jnz fail                        ; TOP is back at 0
        fstp st0
        fnstsw ax
        and ax, 0x0041
        cmp ax, 0x0041                  ; invalid operation, stack fault
        jne fail
        fnclex

        STEP                            ; 10: saving and restoring everything
        fld1
        fldpi
        fnsave [state]
        fnstcw [w]
        cmp word [w], 0x037F
        jne fail
        fnstsw ax
        or ax, ax
        jnz fail
        frstor [state]
        EXPECT80 0x4000, 0xC90FDAA22168C235
        EXPECT80 0x3FFF, 0x8000000000000000

        STEP                            ; 11: an unmasked exception arrives as IRQ 13
        mov ax, 0x3575
        int 0x21
        mov [old75], bx
        mov [old75 + 2], es
        mov dx, on_irq13
        mov ax, 0x2575
        int 0x21
        mov word [cw], 0x037B           ; unmask division by zero
        fldcw [cw]
        fld1
        fldz
        fdivp st1, st0                  ; 1 / 0
        mov cx, 1000
.wait:  cmp byte [seen], 0
        loope .wait
        push ds
        lds dx, [old75]
        mov ax, 0x2575
        int 0x21
        pop ds
        cmp byte [seen], 1
        jne fail
        fninit

        STEP                            ; 12: which operand is which, in every encoding
%macro QUARTERS 1                       ; ST(0), popped, must be %1 / 4
        fimul word [four]
        fistp word [w]
        cmp word [w], %1
        jne fail
%endmacro
        fld qword [d2_25]
        fld1
        fsubp st1, st0                  ; 2.25 - 1
        QUARTERS 5
        fld qword [d2_25]
        fld1
        fsubrp st1, st0                 ; 1 - 2.25
        QUARTERS -5
        fld qword [d2_25]
        fld qword [d1_5]
        fdivp st1, st0                  ; 2.25 / 1.5
        QUARTERS 6
        fld qword [d1_5]
        fld qword [d2_25]
        fdivrp st1, st0                 ; 2.25 / 1.5
        QUARTERS 6
        fld qword [d2_25]
        fld1
        fsub st0, st1                   ; 1 - 2.25
        QUARTERS -5
        fstp st0
        fld qword [d2_25]
        fld1
        fsubr st0, st1                  ; 2.25 - 1
        QUARTERS 5
        fstp st0
        fld qword [d2_25]
        fld1
        fsub st1, st0                   ; ST(1) = 2.25 - 1
        fstp st0
        QUARTERS 5
        fld qword [d2_25]
        fld1
        fsubr st1, st0                  ; ST(1) = 1 - 2.25
        fstp st0
        QUARTERS -5
        fld qword [d2_25]
        fld qword [d1_5]
        fdiv st1, st0                   ; ST(1) = 2.25 / 1.5
        fstp st0
        QUARTERS 6
        fld qword [d1_5]
        fld qword [d2_25]
        fdivr st1, st0                  ; ST(1) = 2.25 / 1.5
        fstp st0
        QUARTERS 6
        fld qword [d2_25]
        fsub dword [f1_0]               ; 2.25 - 1
        QUARTERS 5
        fld qword [d2_25]
        fsubr dword [f1_0]              ; 1 - 2.25
        QUARTERS -5
        fld qword [d2_25]
        fidivr word [nine]              ; 9 / 2.25
        QUARTERS 16
        fld1
        fld qword [d2_25]
        fxch st1
        fsubp st1, st0                  ; 2.25 - 1 after the exchange
        QUARTERS 5
        fnstsw ax
        test ax, 0x3800 | 0x41          ; the stack is level again and nothing went wrong
        jnz fail

        mov dx, m_ok
        jmp done
fail:   fninit
        mov ax, [step]
        aam
        add ax, '00'
        xchg al, ah
        mov [m_step], ax
        mov dx, m_fail
done:   mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

on_irq13:
        push ax
        mov byte [cs:seen], 1
        fnclex
        xor al, al
        out 0xF0, al
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop ax
        iret

step:   dw 0
w:      dw 0
cw:     dw 0
seen:   db 0
three:  dw 3
four:   dw 4
six:    dw 6
eight:  dw 8
nine:   dw 9
ten:    dw 10
f1_0:   dd 1.0
old75:  dd 0
big:    dq 0x0123456789ABCDEF
bcd:    db 0x78, 0x56, 0x34, 0x12, 0x90, 0x78, 0x56, 0x34, 0x12, 0x80
f0_1:   dd 0.1
f0_2:   dd 0.2
d0_5:   dq 0.5
d1_0:   dq 1.0
d1_5:   dq 1.5
d2_25:  dq 2.25
d2_7:   dq 2.7
eps:    dq 1.0e-15
m_ok:   db '80387 test passed', 13, 10, '$'
m_fail: db '80387 test FAILED at step '
m_step: db '00', 13, 10, '$'
r80:    times 10 db 0
state:  times 108 db 0
