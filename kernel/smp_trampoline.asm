; smp_trampoline.asm —— AP 唤醒跳板（全部用固定物理偏移，无链接期重定位）
;
; AP 从物理 0x8000 起执行(SIPI 向量 0x08)。为避免 nasm/ld 在 elf32 里
; 对 16 位 far jmp / lgdt 产生 R_386_16 重定位(section .data 链接地址
; 远超 16 位会截断成垃圾),这里所有地址都用【固定物理偏移硬编码】:
;   +0x00  16 位入口
;   +0x40  32 位入口(tramp32)
;   +0x100 临时 GDT 指针(gdtpr): limit=23, base=0x8108
;   +0x108 临时 GDT 表(3 描述符)
;
; 运行物理地址恒为 0x8000, 引用 [cs:0x??] 即 0x8000+0x??, 稳定。
; AP 不开分页: 线性==物理, 直接 call ap_entry32(链接符号, 平坦可达)。
; 栈固定 0x7F000。本简化版支持 1 个 AP。

BITS 16
global smp_trampoline_start
global smp_trampoline_end

section .data
align 16
smp_trampoline_start:

; ---- 16 位入口(偏移 0) ----
        cli
        cld
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        lgdt    [cs:0x0100]             ; gdtpr 固定在偏移 0x100
        mov     eax, cr0
        or      eax, 1                  ; PE
        mov     cr0, eax
        ; 用 32 位 offset 的 far jmp(xv6 风格: EA + long offset + word sel)
        data16 db 0x66
        db      0xEA
        dd      0x8020          ; 32 位目标物理地址(tramp32 在偏移 0x20)
        dw      0x0008          ; 代码段选择子

; ---- 32 位入口(紧接 far jmp 之后, 偏移 0x2c) ----
BITS 32
tramp32:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0x7F000            ; AP 专用栈(固定区)
        extern ap_entry32
        mov     eax, ap_entry32
        call    eax
.hang:  cli
        hlt
        jmp     .hang

; ---- 固定数据区: gdtpr @0x100, GDT @0x108 ----
times (0x100-($-smp_trampoline_start)) nop
gdtpr:
        dw 23                           ; limit = 3*8 - 1
        dd 0x8108                       ; GDT 物理地址 = 0x8000+0x108
times (0x108-($-smp_trampoline_start)) db 0    ; 强制定位 GDT 到 0x108
gdt:
        dq 0
        dq 0x00CF9A000000FFFF
        dq 0x00CF92000000FFFF

smp_trampoline_end:
