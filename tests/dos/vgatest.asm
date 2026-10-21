; VGATEST.COM -- exercises the VGA beyond the BIOS 256-colour mode:
;   1. mode 12h (640x480, 16 colours, planar) through the BIOS
;   2. "Mode X": 320x240 unchained, with a split screen and a scrolled start address
;   3. display timing: displayed scan lines counted from the status register, and a
;      one-shot timer interrupt landing on a chosen scan line
; A key steps from one picture to the next; the timing results are printed at the end.
        bits 16
        cpu 186
        org 0x100

        ; ---- 1: mode 12h --------------------------------------------------------
        mov ax, 0x0012
        int 0x10
        xor dx, dx                      ; sixteen vertical bars with the BIOS pixel call
.bar_y: xor cx, cx
.bar_x: mov ax, cx
        mov bl, 40
        div bl
        mov ah, 0x0C
        xor bh, bh
        int 0x10
        inc cx
        cmp cx, 640
        jb .bar_x
        add dx, 4
        cmp dx, 480
        jb .bar_y
        mov si, t12
        call title
        call key

        ; ---- 2: Mode X ----------------------------------------------------------
        mov ax, 0x0013
        int 0x10
        mov dx, 0x3C4                   ; unchain
        mov ax, 0x0604
        out dx, ax
        mov dx, 0x3D4
        mov ax, 0x0011                  ; unprotect CRTC 0-7 (index 11h bit 7)
        out dx, ax
        mov si, crtc_240
        mov cx, crtc_240_n
.crtc:  lodsw
        out dx, ax
        loop .crtc
        mov dx, 0x3C2                   ; 480-line sync polarities
        mov al, 0xE3
        out dx, al

        mov ax, 0xA000
        mov es, ax
        mov dx, 0x3C4                   ; clear all four planes
        mov ax, 0x0F02
        out dx, ax
        xor di, di
        xor ax, ax
        mov cx, 0x8000
        rep stosw
        ; rows 0-63: a panel for the split screen; from row 64 on: the picture
        xor bx, bx                      ; bx = x
.col:   mov cx, bx                      ; select the plane of this column
        and cl, 3
        mov ax, 0x0102
        shl ah, cl
        mov dx, 0x3C4
        out dx, ax
        mov di, bx
        shr di, 2
        xor dx, dx                      ; dx = y
.pix:   mov al, 0x0E                    ; panel: yellow with a red frame
        cmp dx, 64
        jae .pic
        cmp dx, 2
        jb .frame
        cmp dx, 62
        jae .frame
        cmp bx, 2
        jb .frame
        cmp bx, 318
        jb .put
.frame: mov al, 0x0C
        jmp .put
.pic:   mov ax, bx                      ; picture: 20x20 checkerboard in colours 32-47...
        push dx
        xor dx, dx
        mov cx, 20
        div cx
        mov cx, ax
        pop dx
        push dx
        mov ax, dx
        xor dx, dx
        push cx
        mov cx, 20
        div cx
        pop cx
        pop dx
        add al, cl
        and al, 0x0F
        add al, 32
.put:   mov [es:di], al
        add di, 80
        inc dx
        cmp dx, 400
        jb .pix
        inc bx
        cmp bx, 320
        jb .col

        ; display starts at row 64 (the picture); the last 64 rows show address 0 (the panel)
        mov dx, 0x3D4
        mov ax, (((64 * 80) >> 8) << 8) | 0x0C
        out dx, ax
        mov ax, (((64 * 80) & 0xFF) << 8) | 0x0D
        out dx, ax
        mov ax, 0x5F18                  ; line compare = 351 (= 2 * 176 - 1): bits 0-7
        out dx, ax
        mov ax, 0x3E07                  ; overflow: line compare bit 8 = 1 (rest unchanged)
        out dx, ax
        mov ax, 0x0109                  ; max scan line 1, line compare bit 9 = 0
        out dx, ax
        call key

        ; ---- 3: timing ----------------------------------------------------------
        mov dx, 0x3DA
        call wait_vsync
.vsend: in al, dx                       ; let this retrace pass, then count the displayed
        test al, 8                      ; scan lines up to the next one (the "display
        jnz .vsend                      ; disabled" bit pulses once per displayed line)
        xor bx, bx
.count: in al, dx
        test al, 8
        jnz .counted
        test al, 1
        jnz .count
.active:
        in al, dx
        test al, 8
        jnz .counted
        test al, 1
        jz .active
        inc bx
        jmp .count
.counted:
        mov [lines], bx

        ; one-shot timer for 100 scan lines after the retrace, then see where it fires
        cli
        xor ax, ax
        mov es, ax
        mov ax, [es:0x20]
        mov [old8], ax
        mov ax, [es:0x22]
        mov [old8 + 2], ax
        mov word [es:0x20], tick
        mov [es:0x22], cs
        sti
        mov dx, 0x3DA
        call wait_vsync
        mov al, 0x30                    ; channel 0, mode 0 (one shot)
        out 0x43, al
        mov ax, 3792                    ; 100 lines * 31.778 us * 1.193182 MHz
        out 0x40, al
        mov al, ah
        out 0x40, al
        xor bx, bx
.wait:  in al, dx                       ; count lines until the interrupt stops us
        test al, 1
        jnz .wait
.act2:  in al, dx
        test al, 1
        jz .act2
        inc bx
        jnz .wait
        cli
        mov ax, [old8]
        mov [es:0x20], ax
        mov ax, [old8 + 2]
        mov [es:0x22], ax
        mov al, 0x36                    ; back to the 18.2 Hz system tick
        out 0x43, al
        xor al, al
        out 0x40, al
        out 0x40, al
        sti

        mov ax, 0x0003
        int 0x10
        mov dx, m_lines
        call print
        mov ax, [lines]
        call decimal
        mov dx, m_fired
        call print
        mov ax, [fired]
        call decimal
        mov dx, m_nl
        call print
        mov ax, 0x4C00
        int 0x21

tick:   push ax                         ; timer interrupt: note the line count, stop the loop
        mov [cs:fired], bx
        mov bx, 0xFFFF
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

wait_vsync:                             ; returns right at the start of a vertical retrace
        in al, dx
        test al, 8
        jnz wait_vsync
.w2:    in al, dx
        test al, 8
        jz .w2
        ret

title:  lodsb
        or al, al
        jz .ret
        mov ah, 0x0E
        mov bx, 0x000F
        int 0x10
        jmp title
.ret:   ret

key:    xor ax, ax
        int 0x16
        ret

print:  mov ah, 9
        int 0x21
        ret

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

; CRTC values that turn mode 13h into 320x240 (index in the low byte)
crtc_240: dw 0x0D06, 0x3E07, 0x4109, 0xEA10, 0xAC11, 0xDF12, 0x0014, 0xE715, 0x0616, 0xE317
crtc_240_n equ ($ - crtc_240) / 2

lines:  dw 0
fired:  dw 0
old8:   dw 0, 0
t12:    db 'Mode 12h 640x480x16', 0
m_lines: db 'Displayed scan lines: $'
m_fired: db 13, 10, 'Timer fired at line: $'
m_nl:   db 13, 10, '$'
