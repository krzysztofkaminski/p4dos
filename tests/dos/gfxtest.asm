; GFXTEST.COM -- shows a test picture in each graphics mode; any key steps on.
        bits 16
        cpu 8086
        org 0x100

        ; --- mode 13h: 320x200, 256 colours: the whole palette as a 16x16 grid
        mov ax, 0x0013
        int 0x10
        mov ax, 0xA000
        mov es, ax
        xor di, di
        xor dx, dx                      ; dx = y
.row13: xor cx, cx                      ; cx = x
.px13:  mov ax, dx
        mov bl, 12
        div bl                          ; al = y / 12 (0..16)
        cmp al, 16
        jb .in
        xor al, al
        jmp .put
.in:    mov bh, al
        mov ax, cx
        mov bl, 20
        div bl                          ; al = x / 20 (0..15)
        mov ah, bh
        shl ah, 1
        shl ah, 1
        shl ah, 1
        shl ah, 1
        or al, ah
.put:   stosb
        inc cx
        cmp cx, 320
        jb .px13
        inc dx
        cmp dx, 200
        jb .row13
        mov si, t13
        call title
        call key

        ; --- mode 4: 320x200, 4 colours: vertical bars
        mov ax, 0x0004
        int 0x10
        mov dx, 16
.row4:  xor cx, cx
.px4:   mov ax, cx
        mov bl, 80
        div bl
        mov ah, 0x0C
        xor bh, bh
        int 0x10
        inc cx
        cmp cx, 320
        jb .px4
        inc dx
        cmp dx, 200
        jb .row4
        mov si, t4
        call title
        call key

        ; --- mode 6: 640x200, 2 colours: a diagonal cross and a frame
        mov ax, 0x0006
        int 0x10
        xor cx, cx
.diag:  mov ax, cx
        mov bl, 200
        mul bl                          ; ax = x * 200 (fits for x < 320)
        mov dx, cx
        cmp cx, 320
        jb .lo
        mov dx, 639
        sub dx, cx
.lo:    mov ax, dx
        mov bx, 5
        mul bx
        shr ax, 1
        shr ax, 1
        shr ax, 1
        mov dx, ax                      ; y = min(x, 639-x) * 5 / 8
        mov ax, 0x0C01
        xor bh, bh
        int 0x10
        inc cx
        cmp cx, 640
        jb .diag
        mov si, t6
        call title
        call key

        ; --- mode 11h: 640x480, 2 colours: horizontal stripes every 16 lines
        mov ax, 0x0011
        int 0x10
        mov dx, 32
.row11: xor cx, cx
.px11:  mov ax, 0x0C01
        xor bh, bh
        int 0x10
        add cx, 2
        cmp cx, 640
        jb .px11
        add dx, 16
        cmp dx, 480
        jb .row11
        mov si, t11
        call title
        call key

        mov ax, 0x0003
        int 0x10
        mov dx, m_done
        mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21

title:  lodsb                           ; teletype the string at SI in colour 3 / 15
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

t13:    db 'Mode 13h 320x200x256', 0
t4:     db 'Mode 4 320x200x4', 0
t6:     db 'Mode 6 640x200x2', 0
t11:    db 'Mode 11h 640x480x2', 0
m_done: db 'Graphics test done', 13, 10, '$'
