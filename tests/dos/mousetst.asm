; MOUSETST.COM -- waits for a left click and prints the pointer position
; (INT 33h, i.e. whatever mouse driver is loaded).
        bits 16
        cpu 8086
        org 0x100

        xor ax, ax
        int 0x33
        cmp ax, 0xFFFF
        jne .none
        mov dx, m_wait
        mov ah, 9
        int 0x21
.poll:  mov ax, 3
        int 0x33
        test bl, 1
        jnz .click
        mov ah, 1
        int 0x16
        jz .poll
        xor ax, ax
        int 0x16
        jmp .exit
.click: push dx
        push cx
        mov dx, m_x
        mov ah, 9
        int 0x21
        pop ax
        call decimal
        mov dx, m_y
        mov ah, 9
        int 0x21
        pop ax
        call decimal
        mov dx, m_nl
        mov ah, 9
        int 0x21
        jmp .exit
.none:  mov dx, m_none
        mov ah, 9
        int 0x21
.exit:  mov ax, 0x4C00
        int 0x21

decimal:
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

m_wait: db 'Click the left button...', 13, 10, '$'
m_none: db 'No mouse driver', 13, 10, '$'
m_x:    db 'Click at X=$'
m_y:    db ' Y=$'
m_nl:   db 13, 10, '$'
