; Master boot record for P4DOS hard disk images: relocate, find the active
; partition, load its boot sector to 0000:7C00 and run it. DL (boot drive) is
; passed through from the BIOS.

        bits 16
        cpu 8086
        org 0x600

start:  cli
        xor ax, ax
        mov ss, ax
        mov sp, 0x7C00
        mov ds, ax
        mov es, ax
        sti
        cld
        mov si, 0x7C00
        mov di, 0x600
        mov cx, 256
        rep movsw
        jmp 0:next

next:   mov si, 0x600 + 446
        mov cx, 4
.find:  test byte [si], 0x80
        jnz .boot
        add si, 16
        loop .find
        mov si, msg_none
        jmp fail

.boot:  mov bp, si
        mov di, 3
.retry: mov ax, 0x0201
        mov bx, 0x7C00
        mov cx, [si + 2]
        mov dh, [si + 1]
        int 0x13
        jnc .ok
        xor ax, ax
        int 0x13
        dec di
        jnz .retry
        mov si, msg_err
        jmp fail
.ok:    cmp word [0x7DFE], 0xAA55
        mov si, msg_bad
        jne fail
        mov si, bp
        jmp 0:0x7C00

fail:   lodsb
        or al, al
        jz .stop
        mov ah, 0x0E
        mov bx, 7
        int 0x10
        jmp fail
.stop:  int 0x18

msg_none: db 'No active partition', 13, 10, 0
msg_err:  db 'Disk read error', 13, 10, 0
msg_bad:  db 'Missing operating system', 13, 10, 0

        times 446 - ($ - $$) db 0
