; XMS.SYS -- extended memory (XMS 2.0) for P4DOS.
;
; The emulator does the work (core/xms.c). This driver answers the INT 2Fh
; installation check and hands out a control entry point that forwards to the
; BIOS ROM stub. There is no HMA and no A20 on an 80186-class machine; what it
; provides are extended memory blocks, which FreeCOM uses to swap itself out.
;
;   DEVICE=C:\FDOS\BIN\XMS.SYS

        bits 16
        cpu 8086
        org 0

header: dd -1
        dw 0x8000                       ; character device
        dw strategy
        dw interrupt
        db 'XMSXXXX0'

request:    dd 0
bios_entry: dw 0, 0xF000
old_2f:     dd 0

strategy:
        mov [cs:request], bx
        mov [cs:request + 2], es
        retf

interrupt:
        push ax
        push bx
        push ds
        lds bx, [cs:request]
        cmp byte [bx + 2], 0
        je init
        mov word [bx + 18], 0
        mov word [bx + 3], 0x0100
        pop ds
        pop bx
        pop ax
        retf

; XMS control function. The five-byte header is what programs that hook the
; control chain expect to find.
control:
        jmp short .go
        nop
        nop
        nop
.go:    call far [cs:bios_entry]
        retf

int2f:  cmp ax, 0x4300
        je .present
        cmp ax, 0x4310
        je .entry
        jmp far [cs:old_2f]
.present:
        mov al, 0x80
        iret
.entry: push cs
        pop es
        mov bx, control
        iret

resident_end:

init:   push cx
        push dx
        push si
        push es
        mov ax, 0x4300                  ; an XMS driver is there already (HIMEMX on
        int 0x2F                        ; the 80386): leave it to that, quietly
        cmp al, 0x80
        je .quiet
        mov ax, 0xF000
        mov es, ax
        cmp word [es:0], 'GD'
        jne .absent
        cmp word [es:2], 'OS'
        jne .absent
        mov ax, [es:0x1A]
        mov [cs:bios_entry], ax
        push bx
        mov ah, 0x08                    ; DX = free extended memory in KB
        call far [cs:bios_entry]
        pop bx
        or dx, dx
        jz .absent

        xor ax, ax
        mov es, ax
        mov ax, [es:0x2F * 4]
        mov [cs:old_2f], ax
        mov ax, [es:0x2F * 4 + 2]
        mov [cs:old_2f + 2], ax
        cli
        mov word [es:0x2F * 4], int2f
        mov [es:0x2F * 4 + 2], cs
        sti

        mov ax, dx
        mov si, msg_kb
        call decimal
        mov dx, msg_ok
        call print
        mov word [bx + 14], resident_end
        jmp .done

.absent:
        mov dx, msg_no
        call print
.quiet: mov byte [bx + 13], 0
        mov word [bx + 14], 0
.done:  mov [bx + 16], cs
        mov word [bx + 3], 0x0100
        pop es
        pop si
        pop dx
        pop cx
        pop ds
        pop bx
        pop ax
        retf

print:  push ds
        push cs
        pop ds
        mov ah, 9
        int 0x21
        pop ds
        ret

; Write AX as 5 right-aligned decimal digits at CS:SI.
decimal:
        push bx
        push dx
        mov cx, 5
        add si, 4
        mov bx, 10
.digit: xor dx, dx
        div bx
        add dl, '0'
        mov [cs:si], dl
        dec si
        or ax, ax
        jz .pad
        loop .digit
        jmp .out
.pad:   dec cx
        jz .out
.blank: mov byte [cs:si], ' '
        dec si
        loop .blank
.out:   pop dx
        pop bx
        ret

msg_ok: db 'XMS 2.0:'
msg_kb: db '      KB extended memory', 13, 10, '$'
msg_no: db 'XMS.SYS: extended memory is not enabled', 13, 10, '$'
