; DPMITEST.COM: enters 32-bit protected mode through a DPMI host (run CWSDPMI -p first),
; allocates a megabyte of extended memory, fills and checks it, and runs a few 80386
; instructions. Prints one line saying how it went.
        org 0x100
        bits 16

start:  mov bx, 0x1000                  ; keep 64 KB: the host needs DOS memory too
        mov ah, 0x4A
        int 0x21
        mov ax, 0x1687                  ; DPMI installation check
        int 0x2F
        test ax, ax
        jnz no_host
        mov [dpmi_entry], di
        mov [dpmi_entry + 2], es
        test bx, 1
        jz no_host                      ; no 32-bit programs
        mov bx, si                      ; paragraphs the host wants for itself
        test bx, bx
        jz .enter
        mov ah, 0x48
        int 0x21
        jc no_host
        mov es, ax
.enter: mov ax, 1                       ; a 32-bit program
        call far [dpmi_entry]
        jc no_host

        ; Protected mode, in a 16-bit code segment. Make a 32-bit one over the same
        ; memory, and a data segment that spans the whole address space.
        xor ax, ax
        mov cx, 1
        int 0x31
        jc fail16
        mov [code32], ax
        mov bx, cs
        mov ax, 6                       ; CX:DX = base of CS
        int 0x31
        mov bx, [code32]
        mov ax, 7
        int 0x31
        xor cx, cx
        mov dx, 0xFFFF
        mov ax, 8
        int 0x31
        mov ax, cs
        and al, 3
        shl al, 5
        mov cl, 0x9B                    ; present, code, readable
        or cl, al
        mov ch, 0x40                    ; 32-bit
        mov ax, 9
        int 0x31
        jc fail16

        xor ax, ax
        mov cx, 1
        int 0x31
        jc fail16
        mov [flat], ax
        mov bx, ax
        mov cx, 0xFFFF
        mov dx, 0xFFFF
        mov ax, 8
        int 0x31
        mov ax, cs
        and al, 3
        shl al, 5
        mov cl, 0x93                    ; present, data, writable
        or cl, al
        mov ch, 0xC0                    ; 32-bit, page granular
        mov bx, [flat]
        mov ax, 9
        int 0x31
        jc fail16

        push word [code32]
        push word main32
        retf

no_host:
        mov dx, msg_none
        mov ah, 9
        int 0x21
        mov ax, 0x4C02
        int 0x21

fail16: mov si, msg_fail
.next:  lodsb
        test al, al
        jz .out
        mov dl, al
        mov ah, 2
        int 0x21
        jmp .next
.out:   mov ax, 0x4C01
        int 0x21

        bits 32
main32: mov es, [flat]
        mov ax, 0x0501                  ; allocate BX:CX = 1 MB
        mov bx, 0x0010
        xor cx, cx
        int 0x31
        jc fail32
        shl ebx, 16
        mov bx, cx
        mov [block], ebx

        mov edi, ebx
        mov ecx, 0x40000
        mov eax, 0x12345678
.fill:  mov [es:edi], eax
        add eax, 0x9E3779B9
        rol eax, 3
        add edi, 4
        dec ecx
        jnz .fill

        mov esi, [block]
        mov ecx, 0x40000
        mov eax, 0x12345678
.check: cmp [es:esi], eax
        jne fail32
        add eax, 0x9E3779B9
        rol eax, 3
        add esi, 4
        dec ecx
        jnz .check

        ; a block move inside it, and a few of the instructions the 80386 added
        push ds
        push es
        pop ds
        mov esi, [es:ebx]               ; (value only; keeps the assembler honest)
        mov esi, ebx
        lea edi, [ebx + 0x80000]
        mov ecx, 0x1000
        cld
        rep movsd
        pop ds
        mov eax, [es:ebx + 0x80000 + 4 * 0xFFF]
        cmp eax, [es:ebx + 4 * 0xFFF]
        jne fail32

        mov eax, 0x00F00000
        bsf ebx, eax                    ; 20
        bsr ecx, eax                    ; 23
        movzx edx, bl
        imul edx, ecx                   ; 460
        shld edx, eax, 4                ; 7360
        cmp edx, 7360
        jne fail32
        mov eax, -7
        cdq
        mov ecx, 2
        idiv ecx                        ; -3 remainder -1
        cmp eax, -3
        jne fail32
        cmp edx, -1
        jne fail32

        ; a segment load with an operand-size prefix, then a prefixed instruction: the
        ; second one has 32-bit operands again
        o16 mov es, [flat]
        ds mov edx, 0x12345678
        cmp edx, 0x12345678
        jne fail32

        mov esi, msg_ok
        call puts32
        mov ax, 0x4C00
        int 0x21

fail32: mov esi, msg_fail
        call puts32
        mov ax, 0x4C01
        int 0x21

puts32: lodsb
        test al, al
        jz .done
        mov dl, al
        mov ah, 2
        int 0x21
        jmp puts32
.done:  ret

msg_none: db 'DPMITEST: no 32-bit DPMI host (run CWSDPMI -p first)', 13, 10, '$'
msg_ok:   db 'DPMITEST: 32-bit protected mode, 1024 KB of extended memory checked', 13, 10, 0
msg_fail: db 'DPMITEST: FAILED', 13, 10, 0

        align 4
dpmi_entry: dd 0
block:      dd 0
code32:     dw 0
flat:       dw 0
