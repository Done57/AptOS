BITS 16
ORG 0x8000

%define BOOTINFO      0x5000
%define E820_ADDR     0x5200
%define VBE_CTRL      0x5600
%define VBE_MODE      0x5900
%define MODELIST      0x5C00
; 内核最终装载地址由"目标模式"决定：
;   32 位内核  装在 0x10000（64KB），历史上一直如此
;   64 位内核  装在 0x100000（1MB），避开实模式低端内存
;
; 两条路都要经过"先读进低内存缓冲、再搬到目标"的过程，因为
; int 13h 直接读 1MB 会静默失败（部分 BIOS 拒绝），而读 0x10000
; 虽然可行，但用同一条路径更不容易出错。
%define STAGE_BUF     0x20000
%define KERNEL_ADDR32 0x10000
%define KERNEL_ADDR64 0x100000
%define KERNEL_BUF    STAGE_BUF
%define SPT           18
%define HEADS         2

start:
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        cld
        mov     [boot_drive], dl

        ; ---- 清 VGA 文本屏(0xB8000)，把光标归零，保证屏幕日志从顶上干净显示 ----
        pushad
        mov     edi, 0xB8000
        mov     ecx, 80*25
        mov     ax, 0x0720          ; 空格 + 灰底黑字属性
.clrscr:
        mov     [edi], ax
        add     edi, 2
        loop    .clrscr
        mov     byte [vga_cursor], 0
        popad

        call    serial_init16
        mov     si, msg_boot
        call    puts16

        call    do_e820
        call    do_vbe
        call    do_a20
        call    load_kernel
        call    fill_bootinfo

        mov     si, msg_pm
        call    puts16
        cli
        lgdt    [gdt_ptr]
        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax
        jmp     0x08:pm_entry

do_e820:
        xor     ax, ax
        mov     es, ax
        mov     di, E820_ADDR
        xor     ebx, ebx
        xor     bp, bp
.loop:
        mov     eax, 0xE820
        mov     edx, 0x534D4150
        mov     ecx, 24
        mov     dword [di + 20], 1
        push    es
        push    di
        push    ds
        push    es
        int     0x15
        pop     es
        pop     ds
        pop     di
        pop     es
        jc      .done
        cmp     eax, 0x534D4150
        jne     .done
        mov     eax, [di + 8]
        or      eax, [di + 12]
        jz      .skip
        inc     bp
        add     di, 24
        cmp     bp, 40
        jae     .done
.skip:
        test    ebx, ebx
        jnz     .loop
.done:
        movzx   eax, bp
        push    ax
        xor     ax, ax
        mov     es, ax
        pop     ax
        mov     [es:BOOTINFO + 0x14], eax
        mov     dword [es:BOOTINFO + 0x10], E820_ADDR
        mov     si, msg_e820
        call    puts16
        ret

do_vbe:
        xor     ax, ax
        mov     es, ax
        mov     ax, 0x4F00
        mov     di, VBE_CTRL
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .fallback

        mov     si, [VBE_CTRL + 0x0E]
        test    si, si
        jz      .fallback
        mov     ax, [VBE_CTRL + 0x10]
        mov     fs, ax
        mov     di, MODELIST
        mov     cx, 256
.copy_modes:
        mov     ax, [fs:si]
        add     si, 2
        mov     [di], ax
        add     di, 2
        cmp     ax, 0xFFFF
        je      .scan
        loop    .copy_modes
.scan:
        mov     word [best_mode], 0xFFFF
        mov     dword [best_score], 0
        mov     si, MODELIST
.next:
        mov     ax, [si]
        add     si, 2
        cmp     ax, 0xFFFF
        je      .choose
        test    ax, ax
        jz      .next
        mov     [cur_mode], ax
        mov     cx, ax
        mov     ax, 0x4F01
        mov     di, VBE_MODE
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .next
        cmp     byte [VBE_MODE + 0x1B], 6
        jne     .next
        mov     dx, [VBE_MODE + 0x12]
        cmp     dx, 640
        jb      .next
        cmp     dx, 1024
        ja      .next
        mov     dx, [VBE_MODE + 0x14]
        cmp     dx, 480
        jb      .next
        cmp     dx, 768
        ja      .next

        movzx   eax, byte [VBE_MODE + 0x19]
        cmp     al, 32
        je      .rank32
        cmp     al, 24
        je      .rank24
        cmp     al, 16
        je      .rank16
        jmp     .next
.rank32:
        mov     ebx, 3
        jmp     .ranked
.rank24:
        mov     ebx, 2
        jmp     .ranked
.rank16:
        mov     ebx, 1
.ranked:
        shl     ebx, 16
        movzx   eax, word [VBE_MODE + 0x12]
        movzx   ecx, word [VBE_MODE + 0x14]
        imul    eax, ecx
        shr     eax, 6
        add     ebx, eax
        cmp     ebx, [best_score]
        jbe     .next
        mov     [best_score], ebx
        mov     ax, [cur_mode]
        mov     [best_mode], ax
        jmp     .next

.choose:
        mov     ax, [best_mode]
        cmp     ax, 0xFFFF
        je      .fallback
        mov     cx, ax
        mov     bx, ax
        or      bx, 0x4000
        mov     ax, 0x4F02
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .fallback
        mov     cx, [best_mode]
        mov     ax, 0x4F01
        mov     di, VBE_MODE
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        cmp     ax, 0x004F
        jne     .fallback
        xor     ax, ax
        mov     es, ax
        mov     eax, [VBE_MODE + 0x28]
        mov     [es:BOOTINFO + 0x04], eax
        mov     ax, [VBE_MODE + 0x10]
        mov     [es:BOOTINFO + 0x08], ax
        mov     ax, [VBE_MODE + 0x12]
        mov     [es:BOOTINFO + 0x0A], ax
        mov     ax, [VBE_MODE + 0x14]
        mov     [es:BOOTINFO + 0x0C], ax
        mov     al, [VBE_MODE + 0x19]
        mov     [es:BOOTINFO + 0x0E], al
        mov     si, msg_vbe
        call    puts16
        ret
.fallback:
        mov     ax, 0x13
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        xor     ax, ax
        mov     es, ax
        mov     dword [es:BOOTINFO + 0x04], 0xA0000
        mov     word  [es:BOOTINFO + 0x08], 320
        mov     word  [es:BOOTINFO + 0x0A], 320
        mov     word  [es:BOOTINFO + 0x0C], 200
        mov     byte  [es:BOOTINFO + 0x0E], 8
        mov     si, msg_vga
        call    puts16
        ret

do_a20:
        mov     ax, 0x2401
        push    ds
        push    es
        int     0x15
        pop     es
        pop     ds
        call    empty_8042
        mov     al, 0xD1
        out     0x64, al
        call    empty_8042
        mov     al, 0xDF
        out     0x60, al
        call    empty_8042
        in      al, 0x92
        or      al, 2
        and     al, 0xFE
        out     0x92, al
        ret

empty_8042:
        mov     cx, 0xFFFF
.wait:
        in      al, 0x64
        test    al, 2
        jz      .done
        loop    .wait
.done:
        ret

; ---------------------------------------------------------------------------
; 加载内核并搬到 1MB。
;
; 为什么不直接让 int 13h 读到 1MB：BIOS 的扩展读对跨 64KB 物理边界的
; 传输限制很严，而 1MB 正好是个敏感边界（有些 BIOS 干脆拒绝）。实测
; 直接读 1MB 会静默失败，内存在 1MB 处仍然是空的。
;
; 所以分两步：
;   1. 用 int 13h 读到低内存 0x20000（128KB），这里 BIOS 一定支持；
;   2. 关掉 A20 之外的干扰后，用 rep movsb 整块搬到 0x100000。
;
; 低内存 0x20000 起是安全的：0x5000-0x5D00 是引导信息结构，0x7C00 是
; 栈，0x8000 是 stage2 自己，都在 128KB 以下。
; ---------------------------------------------------------------------------
%define KERNEL_BUF    0x20000

load_kernel:
        mov     si, msg_kern
        call    puts16

        xor     ax, ax
        mov     es, ax

        mov     word [dst_seg], KERNEL_BUF >> 4
        mov     word [dst_off], 0

        mov     eax, [kernel_lba]
        mov     [cur_lba], eax
        movzx   eax, word [kernel_sec]
        mov     [left], ax
        mov     byte [chs_mode], 0
        mov     byte [retries], 0

.loop:
        cmp     word [left], 0
        je      .loaded
        cmp     byte [chs_mode], 0
        jne     .chs_read

        ; 一次最多 32 扇区（16KB），且不能跨 64KB 边界
        mov     ax, 32
        cmp     [left], ax
        jae     .cap_left
        mov     ax, [left]
.cap_left:
        ; 段内还剩多少扇区可读。
        ; 0x10000 放不进 16 位寄存器，分两种情况算：
        ;   dst_off == 0 -> 整段 64KB，即 128 扇区
        ;   dst_off != 0 -> (0x10000 - dst_off) >> 9，用 -dst_off 的低 16 位
        ; 漏掉第一种会死循环：0-0 仍是 0，于是每轮都判定"本段已满"
        ; 而直接进段，一个字节都没读。
        mov     bx, [dst_off]
        test    bx, bx
        jnz     .part
        mov     bx, 128
        jmp     .havail
.part:
        xor     bx, bx
        sub     bx, [dst_off]
        shr     bx, 9
.havail:
        cmp     bx, ax
        jae     .cnt
        mov     ax, bx
.cnt:
        test    ax, ax
        jnz     .have
        ; 本段满了，进下一段
        mov     ax, [dst_seg]
        add     ax, 0x1000
        mov     [dst_seg], ax
        mov     word [dst_off], 0
        jmp     .loop
.have:
        mov     [dap_count], ax
        mov     eax, [cur_lba]
        mov     [dap_lba], eax
        mov     ax, [dst_seg]
        mov     [dap_seg], ax
        mov     ax, [dst_off]
        mov     [dap_off], ax
        mov     si, dap
        mov     dl, [boot_drive]
        mov     ah, 0x42
        push    ds
        push    es
        int     0x13
        pop     es
        pop     ds
        jnc     .ok

        xor     ah, ah
        mov     dl, [boot_drive]
        push    ds
        push    es
        int     0x13
        pop     es
        pop     ds
        inc     byte [retries]
        cmp     byte [retries], 2
        jbe     .loop
        mov     byte [chs_mode], 1
        mov     byte [retries], 0
        jmp     .loop

.ok:
        movzx   eax, word [dap_count]
        mov     bx, ax
        shl     eax, 9
        ; 推进偏移。这里必须看 CF：add 溢出 16 位意味着正好跨过
        ; 64KB 段边界（比如 0xC000 + 0x4000 = 0x10000），回绕后的
        ; 低 16 位就是正确的新偏移，但段寄存器必须跟着 +0x1000。
        ; 之前无视 CF，回绕成 0 后 cmp 判定"离边界还远"，下一批
        ; 就写到段首把已读数据覆盖掉 —— 内核超过 64KB 必坏。
        add     [dst_off], ax
        jc      .segadv
        cmp     word [dst_off], 0xFF00
        jb      .noadv
        mov     word [dst_off], 0       ; 逼近边界，主动归零留余量
.segadv:
        mov     ax, [dst_seg]
        add     ax, 0x1000
        mov     [dst_seg], ax
.noadv:
        sub     [left], bx
        movzx   eax, bx
        add     [cur_lba], eax
        jmp     .loop

.chs_read:
        mov     ax, [dst_seg]
        mov     es, ax
        mov     bx, [dst_off]
        call    lba_to_chs
        mov     ax, 0x0201
        mov     dl, [boot_drive]
        push    ds
        int     0x13
        pop     ds
        jc      .chs_err
        add     word [dst_off], 512
        jnc     .chs_nw
        mov     ax, [dst_seg]
        add     ax, 0x1000
        mov     [dst_seg], ax
.chs_nw:
        dec     word [left]
        inc     dword [cur_lba]
        xor     ax, ax
        mov     es, ax
        jmp     .loop
.chs_err:
        xor     ax, ax
        mov     es, ax
        mov     si, msg_derr
        call    puts16
        cli
        hlt

.loaded:
        xor     ax, ax
        mov     es, ax
        ret


hex16:
        push    ax
        push    cx
        push    dx
        mov     cx, 4
.h:     rol     bx, 4
        mov     al, bl
        and     al, 0x0F
        cmp     al, 10
        jb      .d
        add     al, 'a' - 10
        jmp     .o
.d:     add     al, '0'
.o:     mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        push    cx
        mov     cx, 0xFFFF
.w:     in      al, dx
        test    al, 0x20
        jnz     .r
        loop    .w
.r:     pop     cx
        loop    .h
        pop     dx
        pop     cx
        pop     ax
        ret

msg_loop  db "  loop left=", 0
msg_spc   db " dst=", 0
msg_colon db ":", 0
msg_err   db "  int13h err=0x", 0
msg_batch db "  read ", 0
msg_left  db " sectors, ", 0
crlf16    db 13, 10, 0

msg_copy   db "[boot] relocating kernel to 1MB", 13, 10, 0
msg_copy32 db "[boot] relocating kernel to 64KB", 13, 10, 0
msg_jump32 db "[boot] entering 32-bit kernel at 64KB", 13, 10, 0

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


fill_bootinfo:
        xor     ax, ax
        mov     es, ax
        mov     dword [es:BOOTINFO + 0x00], 0x544F4F42
        mov     eax, [kernel_lba]
        mov     [es:BOOTINFO + 0x18], eax
        movzx   eax, word [kernel_sec]
        mov     [es:BOOTINFO + 0x1C], eax
        mov     eax, [fs_lba]
        mov     [es:BOOTINFO + 0x20], eax
        movzx   eax, byte [boot_drive]
        mov     [es:BOOTINFO + 0x24], eax
        ret

serial_init16:
        mov     dx, 0x3F9
        xor     al, al
        out     dx, al
        mov     dx, 0x3FB
        mov     al, 0x80
        out     dx, al
        mov     dx, 0x3F8
        mov     al, 1
        out     dx, al
        mov     dx, 0x3F9
        xor     al, al
        out     dx, al
        mov     dx, 0x3FB
        mov     al, 3
        out     dx, al
        mov     dx, 0x3FA
        mov     al, 0xC7
        out     dx, al
        mov     dx, 0x3FC
        mov     al, 0x0B
        out     dx, al
        ret

puts16:
        lodsb
        test    al, al
        jz      .done
        call    putc16
        jmp     puts16
.done:
        ret

putc16:
        push    ax
        push    dx
        mov     ah, 0x0E
        mov     bh, 0
        mov     bl, 7
        push    ds
        push    es
        int     0x10
        pop     es
        pop     ds
        pop     dx
        pop     ax
        push    ax
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        mov     cx, 0xFFFF
.wait:
        in      al, dx
        test    al, 0x20
        jnz     .out
        loop    .wait
.out:
        pop     dx
        pop     ax
        ret

msg_boot  db "[boot] aptos stage2", 13, 10, 0
msg_e820  db "[boot] e820 memory map collected", 13, 10, 0
msg_vbe   db "[boot] VBE linear framebuffer ready", 13, 10, 0
msg_vga   db "[boot] VBE unavailable, using VGA mode 13h", 13, 10, 0
msg_kern  db "[boot] loading kernel", 13, 10, 0
msg_pm    db "[boot] entering protected mode", 13, 10, 0
msg_derr  db "[boot] disk read error!", 13, 10, 0

cur_mode   dw 0
best_mode  dw 0xFFFF
best_score dd 0
boot_drive db 0
vga_cursor db 0        ; VGA 文本光标位置(字符格), 屏幕日志用
retries   db 0
chs_mode  db 0
left      dw 0
cur_lba   dd 0
chs_cs    dw 0
chs_cyl    dw 0
chs_dh    dw 0
dst_seg   dw KERNEL_BUF >> 4
dst_off   dw 0

align 4
dap:
        db 0x10, 0
dap_count dw 32
dap_off   dw 0
dap_seg   dw KERNEL_BUF >> 4
dap_lba   dq 1

patch_marker:
        db 'MYO2'
kernel_lba  dd 9
kernel_sec  dw 64
fs_lba      dd 0
kernel_bytes dd 0
long_mode   db 1        ; 由 mkimg.py 按内核类型打补丁：1=64位，0=32位

align 16
gdt_start:
        dq 0x0000000000000000
gdt_code32:                             ; 选择子 0x08
        dq 0x00CF9A000000FFFF
gdt_data:                               ; 选择子 0x10
        dq 0x00CF92000000FFFF
gdt_code64:                             ; 选择子 0x18，L 位必须为 1
        dq 0x00AF9A000000FFFF
gdt_end:

gdt_ptr:
        dw gdt_end - gdt_start - 1
        dd gdt_start

%define PML4_ADDR     0x0000A000
%define PDPT_ADDR     0x0000B000
%define PD_ADDR       0x0000C000

BITS 32
pm_entry:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0x7C00

        ; ---- 按目标模式决定内核装载地址 ----
        ; 为什么不在实模式里搬：16 位下 SI/DI/CX 只有 16 位，
        ; 0x20000 这种地址根本放不进去，而且 rep movsb 跨 64KB
        ; 会回绕。进了保护模式就完全没有这些限制。
        cmp     byte [long_mode], 0
        je      .dest32

        mov     esi, msg_copy
        call    puts32
        mov     esi, KERNEL_BUF
        mov     edi, KERNEL_ADDR64
        mov     ecx, [kernel_bytes]
        cld
        rep     movsb
        jmp     .after_move

.dest32:
        ; 32 位内核历史上就装载在 0x10000，保持原样 ——
        ; 它的链接脚本与绝对地址假设都基于这个位置。
        mov     esi, msg_copy32
        call    puts32
        mov     esi, KERNEL_BUF
        mov     edi, KERNEL_ADDR32
        mov     ecx, [kernel_bytes]
        cld
        rep     movsb
        ; 32 位路径到此为止：不需要页表，也不需要长模式。
        mov     esi, msg_jump32
        call    puts32
        jmp     KERNEL_ADDR32

.after_move:
        mov     esi, msg_lm
        call    puts32

        ; ---- 建四级页表，恒等映射前 1GB ----
        ; 先清空这三页，避免残留的引导数据被当成页表项。
        mov     edi, PML4_ADDR
        mov     ecx, 4096 * 3 / 4
        xor     eax, eax
        rep     stosd

        ; PML4[0] -> PDPT
        mov     eax, PDPT_ADDR
        or      eax, 0x03                   ; 存在 + 可写
        mov     [PML4_ADDR], eax

        ; PDPT[0] -> PD
        mov     eax, PD_ADDR
        or      eax, 0x03
        mov     [PDPT_ADDR], eax

        ; PD：512 个 2MB 大页，恒等映射 0..1GB。
        ; 用大页是因为一条表项就能覆盖 2MB，省掉了 512 张页表；
        ; 内核对这段内存本来就是恒等映射，不需要 4KB 粒度。
        mov     edi, PD_ADDR
        mov     eax, 0x83                   ; 存在 + 可写 + PS(大页)
        mov     ecx, 512
.fill_pd:
        mov     [edi], eax
        add     eax, 0x200000
        add     edi, 8
        loop    .fill_pd

        ; ---- 打开长模式 ----
        mov     eax, PML4_ADDR
        mov     cr3, eax

        mov     eax, cr4
        or      eax, (1 << 5)               ; PAE
        mov     cr4, eax

        mov     ecx, 0xC0000080             ; EFER
        rdmsr
        or      eax, (1 << 8)               ; LME
        wrmsr

        mov     eax, cr0
        or      eax, 0x80000000             ; PG
        mov     cr0, eax

        ; 远跳进 64 位代码段 —— 这一刻才真正进入长模式。
        ; 必须用 dword 形式的远跳，否则 nasm 会编成 16 位版本，
        ; 选择子被折进偏移，跳过去用的是空选择子。
        jmp     dword 0x18:lm_entry

puts32:
        lodsb
        test    al, al
        jz      .pdone
        call    putc32
        jmp     puts32
.pdone: ret
putc32:
        push    ax
        push    dx
        push    ecx
        push    edi
        ; ---- 写 VGA 文本缓冲 0xB8000（屏幕），不依赖串口 ----
        movzx   ecx, byte [vga_cursor]
        cmp     al, 10
        je      .pc32_nl
        cmp     al, 13
        je      .pc32_cr
        ; 普通字符：字符+属性
        mov     edi, 0xB8000
        shl     ecx, 1
        add     edi, ecx
        mov     [edi], al
        mov     byte [edi + 1], 0x07
        shr     ecx, 1
        inc     ecx
        jmp     .pc32_store
.pc32_cr:
        ; 回车：回到本行行首
        mov     eax, ecx
        xor     edx, edx
        mov     ebx, 80
        div     ebx
        sub     ecx, edx
        jmp     .pc32_store
.pc32_nl:
        ; 换行：移到下一行行首
        mov     eax, ecx
        xor     edx, edx
        mov     ebx, 80
        div     ebx
        inc     eax
        mov     ebx, 80
        mul     ebx
        mov     ecx, eax
.pc32_store:
        cmp     ecx, 80*25
        jb      .pc32_ok
        mov     ecx, 80*24          ; 滚屏简化为停在最后一行
.pc32_ok:
        mov     [vga_cursor], cl
        ; ---- 仍然写串口（若存在）----
        pop     edi
        pop     ecx
        pop     dx
        pop     ax
        push    ax
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        push    ecx
        mov     ecx, 0xFFFF
.pw:    in      al, dx
        test    al, 0x20
        jnz     .po
        loop    .pw
.po:    pop     ecx
        pop     dx
        pop     ax
        ret

msg_lm  db "[boot] building page tables, entering long mode...", 13, 10, 0

BITS 64
lm_entry:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     rsp, 0x90000

        mov     rsi, msg_64
        call    puts64

        ; 跳进内核（装载在 1MB）
        mov     rax, KERNEL_ADDR64
        jmp     rax

puts64:
        lodsb
        test    al, al
        jz      .qdone
        call    putc64
        jmp     puts64
.qdone: ret
putc64:
        push    ax
        push    dx
        push    rcx
        push    rdi
        ; VGA 文本写屏幕
        movzx   ecx, byte [vga_cursor]
        cmp     al, 10
        je      .pc64_nl
        cmp     al, 13
        je      .pc64_cr
        mov     rdi, 0xB8000
        shl     rcx, 1
        add     rdi, rcx
        mov     [rdi], al
        mov     byte [rdi + 1], 0x07
        shr     rcx, 1
        inc     rcx
        jmp     .pc64_store
.pc64_cr:
        mov     rax, rcx
        xor     rdx, rdx
        mov     rbx, 80
        div     rbx
        sub     rcx, rdx
        jmp     .pc64_store
.pc64_nl:
        mov     rax, rcx
        xor     rdx, rdx
        mov     rbx, 80
        div     rbx
        inc     rax
        mov     rbx, 80
        mul     rbx
        mov     rcx, rax
.pc64_store:
        cmp     rcx, 80*25
        jb      .pc64_ok
        mov     rcx, 80*24
.pc64_ok:
        mov     [vga_cursor], cl
        pop     rdi
        pop     rcx
        pop     dx
        pop     ax
        push    ax
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        push    rcx
        mov     ecx, 0xFFFF
.qw:    in      al, dx
        test    al, 0x20
        jnz     .qo
        loop    .qw
.qo:    pop     rcx
        pop     dx
        pop     ax
        ret

msg_64  db "[boot] long mode active, jumping to 64-bit kernel at 1MB", 13, 10, 0
