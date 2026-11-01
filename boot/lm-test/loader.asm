; 最小引导扇区：把磁盘上后续 64 个扇区读到 0x8000，然后跳过去。
BITS 16
ORG 0x7C00
SECTORS equ 64
start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        cld
        sti
        mov     [drive], dl

        mov     ax, 0x0800          ; es = 0x0800 -> 物理 0x8000
        mov     es, ax
        xor     bx, bx
        mov     cx, 2               ; CHS: 柱0, 头0, 扇2
        mov     dh, 0
        mov     dl, [drive]
        mov     bp, SECTORS
.read:
        mov     ax, 0x0201
        int     0x13
        jc      .err
        add     bx, 512
        dec     bp
        jz      .go
        inc     cl
        cmp     cl, 19
        jb      .read
        mov     cl, 1
        inc     dh
        cmp     dh, 2
        jb      .read
        mov     dh, 0
        inc     ch
        jmp     .read
.go:
        mov     dl, [drive]
        jmp     0x0800:0x0000
.err:
        mov     si, msg
        call    puts
        cli
        hlt
puts:
        lodsb
        test    al, al
        jz      .d
        mov     ah, 0x0E
        int     0x10
        jmp     puts
.d:     ret
msg     db "disk read error", 13, 10, 0
drive   db 0
        times 510 - ($ - $$) db 0
        dw 0xAA55
