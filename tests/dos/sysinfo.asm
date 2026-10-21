; SYSINFO.COM -- prints what the machine looks like to a DOS program.
        bits 16
        cpu 8086
        org 0x100

        mov dx, m_cpu
        call print
        ; 8086/80186 push the decremented SP; 286+ push the old value
        push sp
        pop ax
        cmp ax, sp
        je .at_least_286
        ; 80186 masks shift counts to 5 bits, the 8086 does not
        mov ax, 1
        mov cl, 33
        shl ax, cl
        mov dx, m_8086
        or ax, ax
        jz .cpu_done
        mov dx, m_186
        jmp .cpu_done
.at_least_286:
        ; the 80386 lets real-mode code set the I/O privilege and nested task flags
        pushf
        pop ax
        or ax, 0x7000
        push ax
        popf
        pushf
        pop ax
        mov dx, m_286
        test ax, 0x7000
        jz .cpu_done
        and ax, 0x8FFF
        push ax
        popf
        mov dx, m_386
.cpu_done:
        call print

        ; a coprocessor answers FNSTSW after FNINIT with zero; without one nothing is stored
        mov dx, m_fpu
        call print
        fninit
        mov word [fpu_sw], 0x5A5A
        fnstsw [fpu_sw]
        mov dx, m_fpuno
        cmp word [fpu_sw], 0
        jne .fpu_done
        mov dx, m_fpuyes
.fpu_done:
        call print

        mov dx, m_mem
        call print
        int 0x12
        call decimal
        mov dx, m_kb
        call print

        mov dx, m_video
        call print
        mov ax, 0x1A00
        int 0x10
        cmp al, 0x1A
        mov dx, m_cga
        jne .vid_done
        mov dx, m_vga
        cmp bl, 0x08
        je .vid_done
        mov dx, m_mcga
        cmp bl, 0x0C
        je .vid_done
        mov dx, m_other
.vid_done:
        call print

        mov dx, m_ems
        call print
        mov ax, 0x3567
        int 0x21                        ; ES:BX = INT 67h vector
        mov di, 10
        mov si, emm_name
        mov cx, 8
        cld
        repe cmpsb
        mov dx, m_none
        jne .ems_done
        mov ah, 0x42
        int 0x67
        mov ax, dx
        mov cl, 4
        shl ax, cl
        call decimal
        mov dx, m_kb
.ems_done:
        call print

        mov dx, m_mouse
        call print
        mov ax, 0x3533
        int 0x21
        mov ax, es
        or ax, bx
        mov dx, m_none
        jz .mouse_done
        xor ax, ax
        int 0x33
        cmp ax, 0xFFFF
        jne .mouse_done
        mov dx, m_yes
.mouse_done:
        call print
        mov ax, 0x4C00
        int 0x21

print:  mov ah, 9
        int 0x21
        ret

decimal:                                ; print AX in decimal
        xor cx, cx
        mov bx, 10
.d1:    xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d1
.d2:    pop dx
        add dl, '0'
        mov ah, 2
        int 0x21
        loop .d2
        ret

emm_name: db 'EMMXXXX0'
m_cpu:   db 'CPU:    $'
m_8086:  db '8086/8088', 13, 10, '$'
m_186:   db '80186', 13, 10, '$'
m_286:   db '80286', 13, 10, '$'
m_386:   db '80386 or later', 13, 10, '$'
m_fpu:   db 'FPU:    $'
m_fpuyes: db 'present', 13, 10, '$'
m_fpuno: db 'none', 13, 10, '$'
fpu_sw:  dw 0
m_mem:   db 'Memory: $'
m_kb:    db ' KB', 13, 10, '$'
m_video: db 'Video:  $'
m_cga:   db 'CGA', 13, 10, '$'
m_vga:   db 'VGA, analog color', 13, 10, '$'
m_mcga:  db 'MCGA, analog color', 13, 10, '$'
m_other: db 'other', 13, 10, '$'
m_ems:   db 'EMS:    $'
m_mouse: db 'Mouse:  $'
m_none:  db 'none', 13, 10, '$'
m_yes:   db 'present', 13, 10, '$'
