; A20TEST.COM -- what the DOS extender of Windows 3.1 asks of the XMS driver before it
; starts: local enable A20, query, local disable; each has to report success. DOS, running
; from the HMA, holds an A20 lock of its own at that moment.
        bits 16
        cpu 8086
        org 0x100

        mov ah, 0x30                    ; any DOS call: DOS takes its lock
        int 0x21
        mov ax, 0x4300
        int 0x2F
        cmp al, 0x80
        jne fail
        mov ax, 0x4310
        int 0x2F
        mov [xms], bx
        mov [xms + 2], es

        mov ah, 0x05                    ; local enable
        call far [xms]
        or ax, ax
        jz fail
        mov ah, 0x07                    ; query: on
        call far [xms]
        or ax, ax
        jz fail
        mov ah, 0x06                    ; local disable
        call far [xms]
        or ax, ax
        jz fail
        mov dx, m_ok
        jmp done
fail:   mov dx, m_fail
done:   mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

xms:    dd 0
m_ok:   db 'A20 test passed', 13, 10, '$'
m_fail: db 'A20 test FAILED', 13, 10, '$'
