; EMM.SYS -- LIM EMS 4.0 expanded memory manager for P4DOS.
;
; The emulator does the real work (core/ems.c). This driver only provides what
; DOS programs look for: the EMMXXXX0 device and an INT 67h handler in a segment
; whose offset 0Ah holds the device name. INT 67h is forwarded to the BIOS ROM
; stub with a far call.
;
;   DEVICE=C:\FDOS\BIN\EMM.SYS

        bits 16
        cpu 8086
        org 0

header: dd -1
        dw 0xC000                       ; character device, IOCTL supported
        dw strategy
        dw interrupt
        db 'EMMXXXX0'

request:    dd 0
bios_entry: dw 0, 0xF000

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
        mov word [bx + 18], 0           ; no bytes transferred
        mov word [bx + 3], 0x0100       ; done
        pop ds
        pop bx
        pop ax
        retf

int67:  call far [cs:bios_entry]
        iret

resident_end:

init:   push cx
        push dx
        push si
        push es
        mov ax, 0xF000
        mov es, ax
        cmp word [es:0], 'GD'
        jne .absent
        cmp word [es:2], 'OS'
        jne .absent
        mov ax, [es:0x16]
        mov [cs:bios_entry], ax
        push bx
        mov ah, 0x42                    ; DX = total pages (BX = free pages)
        call far [cs:bios_entry]
        pop bx
        or ah, ah
        jnz .absent

        xor ax, ax
        mov es, ax
        mov word [es:0x67 * 4], int67
        mov [es:0x67 * 4 + 2], cs

        mov ax, dx                      ; pages -> KB, as decimal
        mov cl, 4
        shl ax, cl
        mov si, msg_kb
        call decimal
        mov dx, msg_ok
        call print
        mov word [bx + 14], resident_end
        jmp .done

.absent:
        mov dx, msg_no
        call print
        mov byte [bx + 13], 0           ; nothing installed
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

; Print the $-terminated string at CS:DX.
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

msg_ok: db 'EMS 4.0:'
msg_kb: db '      KB expanded memory, page frame E000', 13, 10, '$'
msg_no: db 'EMM.SYS: expanded memory is not enabled', 13, 10, '$'
