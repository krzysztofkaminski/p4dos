; XMSTEST.COM -- allocate an extended memory block, move a pattern into it and
; back out of it through the XMS driver, and compare.
        bits 16
        cpu 8086
        org 0x100

        mov ax, 0x4300
        int 0x2F
        cmp al, 0x80
        jne fail
        mov ax, 0x4310
        int 0x2F
        mov [xms], bx
        mov [xms + 2], es

        mov ah, 0x09                    ; 64 KB block
        mov dx, 64
        call far [xms]
        cmp ax, 1
        jne fail
        mov [handle], dx

        push ds
        pop es
        mov di, buf                     ; pattern
        mov cx, 2048
        mov ax, 0xA55A
.fill:  stosw
        add ax, 0x0101
        loop .fill

        mov word [m_len], 4096          ; conventional -> XMS at offset 30000
        mov word [m_shandle], 0
        mov word [m_soff], buf
        mov [m_soff + 2], ds
        mov ax, [handle]
        mov [m_dhandle], ax
        mov word [m_doff], 30000
        mov word [m_doff + 2], 0
        mov si, move
        mov ah, 0x0B
        call far [xms]
        cmp ax, 1
        jne fail

        mov di, buf2                    ; XMS -> a second buffer
        mov cx, 2048
        xor ax, ax
        rep stosw
        mov ax, [handle]
        mov [m_shandle], ax
        mov word [m_soff], 30000
        mov word [m_soff + 2], 0
        mov word [m_dhandle], 0
        mov word [m_doff], buf2
        mov [m_doff + 2], ds
        mov si, move
        mov ah, 0x0B
        call far [xms]
        cmp ax, 1
        jne fail

        mov si, buf
        mov di, buf2
        mov cx, 2048
        repe cmpsw
        jne fail

        mov ah, 0x0A
        mov dx, [handle]
        call far [xms]
        cmp ax, 1
        jne fail
        mov dx, m_ok
        jmp done
fail:   mov dx, m_fail
done:   mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

xms:    dd 0
handle: dw 0
move:
m_len:      dd 0
m_shandle:  dw 0
m_soff:     dd 0
m_dhandle:  dw 0
m_doff:     dd 0
m_ok:   db 'XMS test passed', 13, 10, '$'
m_fail: db 'XMS test FAILED', 13, 10, '$'
buf:    times 4096 db 0
buf2:   times 4096 db 0
