BITS 16
ORG 0x7C00

%define STAGE2_SEG 0x0800
%define SPT 18
%define HEADS 2

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        cld
        sti
        mov     [boot_drive], dl
        mov     byte [chs_mode], 0
        mov     ax, [stage2_sec]
        mov     [left], ax
        mov     eax, [stage2_lba]
        mov     [cur_lba], eax
        mov     di, STAGE2_SEG

.rd:
        cmp     byte [chs_mode], 0
        jne     .rd_chs

        mov     eax, [cur_lba]
        mov     [dap_lba], eax
        mov     [dap_seg], di
        mov     word [dap_off], 0
        mov     word [dap_count], 1
        mov     si, dap
        mov     dl, [boot_drive]
        mov     ah, 0x42
        int     0x13
        jnc     .next

        mov     byte [chs_mode], 1
        call    lba_to_chs

.rd_chs:
        mov     ax, 0x0201
        mov     es, di
        xor     bx, bx
        mov     cx, [chs_cs]
        mov     dx, [chs_dh]
        int     0x13
        jc      .err

        inc     cl
        cmp     cl, SPT + 1
        jb      .st
        mov     cl, 1
        inc     dh
        cmp     dh, HEADS
        jb      .st
        mov     dh, 0
        inc     ch
.st:
        mov     [chs_cs], cx
        mov     [chs_dh], dx

.next:
        add     di, 0x20
        dec     word [left]
        jz      .go
        inc     dword [cur_lba]
        jmp     .rd

.go:
        mov     dl, [boot_drive]
        jmp     STAGE2_SEG:0x0000

.err:
        mov     si, msg_err
        call    puts16
        cli
        hlt

lba_to_chs:
        mov     eax, [cur_lba]
        xor     edx, edx
        mov     ecx, SPT * HEADS
        div     ecx                     ; EAX = 柱面, EDX = 柱内扇区偏移
        mov     [chs_cyl], ax           ; 柱面存内存：调用方的 DI/BX 都不能踩
        mov     eax, edx
        xor     edx, edx
        mov     ecx, SPT
        div     ecx                     ; EAX = 磁头, EDX = 扇区-1
        mov     dh, al
        mov     cl, dl
        inc     cl
        movzx   eax, word [chs_cyl]
        mov     ch, al                  ; 柱面低 8 位
        shr     eax, 8
        and     al, 0x03
        shl     al, 6
        or      cl, al                  ; 柱面高 2 位 -> CL bit6-7
        mov     dl, [boot_drive]
        mov     [chs_cs], cx
        mov     [chs_dh], dx
        ret

puts16:
        lodsb
        test    al, al
        jz      .done
        mov     ah, 0x0E
        mov     bh, 0
        mov     bl, 7
        int     0x10
        jmp     puts16
.done:
        ret

msg_err    db "aptos: disk error", 13, 10, 0
boot_drive db 0
chs_mode   db 0
left       dw 0
cur_lba    dd 0
chs_cs     dw 0
chs_cyl    dw 0
chs_dh     dw 0

align 4
dap:
        db 0x10, 0
dap_count dw 1
dap_off   dw 0
dap_seg   dw STAGE2_SEG
dap_lba   dq 1

patch_marker:
        db 'MYO1'
stage2_lba  dd 1
stage2_sec  dw 8

        times 510 - ($ - $$) db 0
        dw 0xAA55
