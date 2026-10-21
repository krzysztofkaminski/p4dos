; EMSTEST.COM -- allocate EMS pages, write a different pattern through the page
; frame into each, then map them back and verify.
        bits 16
        cpu 8086
        org 0x100

PAGES   equ 12

        mov ah, 0x41
        int 0x67
        or ah, ah
        jnz fail
        mov [frame], bx
        mov ah, 0x43
        mov bx, PAGES
        int 0x67
        or ah, ah
        jnz fail
        mov [handle], dx

        xor si, si                      ; logical page
.fill:  call map
        mov es, [frame]
        xor di, di
        mov cx, 0x2000
        mov ax, si
        add ax, 0x5A00
        cld
        rep stosw
        inc si
        cmp si, PAGES
        jb .fill

        xor si, si
.check: call map
        mov es, [frame]
        xor di, di
        mov cx, 0x2000
        mov ax, si
        add ax, 0x5A00
        cld
        repe scasw
        jne fail
        inc si
        cmp si, PAGES
        jb .check

        mov ah, 0x45
        mov dx, [handle]
        int 0x67
        or ah, ah
        jnz fail
        mov dx, m_ok
        jmp done
fail:   mov dx, m_fail
done:   mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

map:    mov ax, 0x4400                  ; physical page 0
        mov bx, si
        mov dx, [handle]
        int 0x67
        or ah, ah
        jnz fail
        ret

frame:  dw 0
handle: dw 0
m_ok:   db 'EMS test passed', 13, 10, '$'
m_fail: db 'EMS test FAILED', 13, 10, '$'
