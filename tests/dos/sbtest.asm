; SBTEST.COM -- Sound Blaster check: reset and identify the DSP, raise a test interrupt,
; then play a quarter second of a 441 Hz square wave by single-cycle DMA and wait for the
; end-of-block interrupt.
        bits 16
        cpu 8086
        org 0x100

BASE    equ 0x220
LEN     equ 2756                        ; samples: 0.25 s at 11025 Hz

        mov dx, BASE + 6                ; DSP reset
        mov al, 1
        out dx, al
        mov cx, 100
.w1:    loop .w1
        xor al, al
        out dx, al
        mov cx, 2000
.ready: mov dx, BASE + 0x0E
        in al, dx
        test al, 0x80
        jnz .got
        loop .ready
        jmp fail
.got:   mov dx, BASE + 0x0A
        in al, dx
        cmp al, 0xAA
        jne fail

        mov al, 0xE1                    ; version
        call dsp_write
        call dsp_read
        mov [ver], al
        call dsp_read
        mov [ver + 1], al

        cli                             ; IRQ 7 handler
        xor ax, ax
        mov es, ax
        mov ax, [es:0x0F * 4]
        mov [old], ax
        mov ax, [es:0x0F * 4 + 2]
        mov [old + 2], ax
        mov word [es:0x0F * 4], irq
        mov [es:0x0F * 4 + 2], cs
        in al, 0x21
        mov [mask], al
        and al, 0x7F
        out 0x21, al
        sti

        mov al, 0xF2                    ; "raise an interrupt now"
        call dsp_write
        mov cx, 0xFFFF
.w2:    cmp byte [hits], 1
        je .irq_ok
        loop .w2
        jmp fail_restore
.irq_ok:

        ; pick a buffer that does not cross a 64 KiB DMA page
        mov ax, ds
        mov cl, 4
        shl ax, cl
        add ax, buf
        mov bx, buf
        add ax, LEN
        jnc .fits
        mov bx, buf + LEN
.fits:  mov [bufp], bx
        push ds                         ; square wave: 25 samples per period
        pop es
        mov di, bx
        mov cx, LEN
        xor dx, dx
.fill:  mov al, 0xD0
        cmp dx, 12
        jb .hi
        mov al, 0x30
.hi:    stosb
        inc dx
        cmp dx, 25
        jb .next
        xor dx, dx
.next:  loop .fill

        mov ax, ds                      ; linear address of the buffer
        mov dx, ax
        mov cl, 4
        shl ax, cl
        mov cl, 12
        shr dx, cl
        add ax, [bufp]
        adc dx, 0                       ; dl = page, ax = offset in page
        mov bx, ax
        mov al, 5                       ; mask channel 1
        out 0x0A, al
        out 0x0C, al                    ; clear the flip-flop
        mov al, 0x49                    ; single transfer, read from memory, channel 1
        out 0x0B, al
        mov al, bl
        out 0x02, al
        mov al, bh
        out 0x02, al
        mov al, dl
        out 0x83, al
        mov ax, LEN - 1
        out 0x03, al
        mov al, ah
        out 0x03, al
        mov al, 1                       ; unmask channel 1
        out 0x0A, al

        mov al, 0xD1                    ; speaker on
        call dsp_write
        mov al, 0x40                    ; time constant for 11025 Hz: 256 - 1000000/11025
        call dsp_write
        mov al, 165
        call dsp_write
        mov al, 0x14                    ; single-cycle output
        call dsp_write
        mov ax, LEN - 1
        call dsp_write
        mov al, ah
        call dsp_write

        xor ax, ax                      ; wait for the block to end: about 5 timer ticks
        mov es, ax
        mov bx, [es:0x46C]
.play:  cmp byte [hits], 2
        je .played
        mov ax, [es:0x46C]
        sub ax, bx
        cmp ax, 18
        jb .play
        jmp fail_restore
.played:
        mov ax, [es:0x46C]
        sub ax, bx
        mov [ticks], ax
        mov al, 0xD3
        call dsp_write
        call restore

        mov dx, m_ok
        mov ah, 9
        int 0x21
        mov al, [ver]
        add al, '0'
        mov dl, al
        mov ah, 2
        int 0x21
        mov dl, '.'
        int 0x21
        mov al, [ver + 1]
        aam
        add ax, 0x3030
        push ax
        mov dl, ah
        mov ah, 2
        int 0x21
        pop ax
        mov dl, al
        mov ah, 2
        int 0x21
        mov dx, m_ticks
        mov ah, 9
        int 0x21
        mov dl, [ticks]
        add dl, '0'
        mov ah, 2
        int 0x21
        mov dx, m_nl
        mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

fail_restore:
        call restore
fail:   mov dx, m_fail
        mov ah, 9
        int 0x21
        mov ax, 0x4C01
        int 0x21

restore:
        cli
        xor ax, ax
        mov es, ax
        mov ax, [old]
        mov [es:0x0F * 4], ax
        mov ax, [old + 2]
        mov [es:0x0F * 4 + 2], ax
        mov al, [mask]
        out 0x21, al
        sti
        ret

irq:    push ax
        push dx
        inc byte [cs:hits]
        mov dx, BASE + 0x0E             ; acknowledge the card
        in al, dx
        mov al, 0x20
        out 0x20, al
        pop dx
        pop ax
        iret

dsp_write:
        push ax
        mov dx, BASE + 0x0C
.busy:  in al, dx
        test al, 0x80
        jnz .busy
        pop ax
        out dx, al
        ret

dsp_read:
        mov dx, BASE + 0x0E
.wait:  in al, dx
        test al, 0x80
        jz .wait
        mov dx, BASE + 0x0A
        in al, dx
        ret

ver:    db 0, 0
hits:   db 0
mask:   db 0
ticks:  dw 0
old:    dw 0, 0
bufp:   dw 0
m_ok:   db 'Sound Blaster test passed: DSP $'
m_ticks: db ', block played in ticks: $'
m_nl:   db 13, 10, '$'
m_fail: db 'Sound Blaster test FAILED', 13, 10, '$'
buf:
