; WELCOME.COM -- the greeting at the end of AUTOEXEC.BAT. It names the processor the machine
; runs, which the firmware configuration picks while the disk image stays the same: the
; 80386DX, with or without its 80387, an 80186 or an 8086.
        bits 16
        cpu 8086
        org 0x100

        mov dx, m_hello
        call print
        ; 8086/80186 push the decremented SP; later processors the old value
        push sp
        pop ax
        cmp ax, sp
        je .is386
        ; the 80186 takes shift counts modulo 32, the 8086 does not
        mov ax, 1
        mov cl, 33
        shl ax, cl
        mov dx, m_8086
        or ax, ax
        jz .done
        mov dx, m_186
        jmp .done
.is386: mov dx, m_386
        call print
        ; a coprocessor stores a zero status word after FNINIT; without one nothing is stored
        fninit
        mov word [fpu_sw], 0x5A5A
        fnstsw [fpu_sw]
        cmp word [fpu_sw], 0
        jne .end
        mov dx, m_387
.done:  call print
.end:   mov dx, m_end
        call print
        mov ax, 0x4C00
        int 0x21

print:  mov ah, 9
        int 0x21
        ret

m_hello: db 'Welcome to FreeDOS (on an emulated $'
m_8086:  db '8086$'
m_186:   db '80186$'
m_386:   db '80386DX$'
m_387:   db ' with 80387$'
m_end:   db ').', 13, 10, '$'
fpu_sw:  dw 0
