; aptos 64 位内核入口。
;
; stage2 已经把我们从实模式一路带到长模式，并且建好了一张恒等映射
; 前 1GB 的页表，内核被加载在 0x100000。这里要做的就只剩：
; 清 BSS、装 GDT、调 kmain。
;
; 注意 64 位下的几个硬性约束：
;   - 段寄存器在长模式下基本被忽略，但 DS/ES/SS 仍必须指向一个合法的
;     数据段描述符，否则装载时就会 #GP。
;   - 堆栈必须 16 字节对齐（System V ABI 要求），否则 SSE 指令
;     （编译器可能生成）会因为未对齐而崩。
;   - 不能假设 red zone 可用，编译时已用 -mno-red-zone。

BITS 64

global _start
extern kmain
extern __bss_start
extern __bss_end

%define KERNEL_DS 0x10

section .text.entry
_start:
        cli

        ; 先建立数据段，长模式下虽然段基址恒为 0，但寄存器里放着
        ; 无效选择子会在下一次装载时触发 #GP。
        mov     ax, KERNEL_DS
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax

        ; 装我们自己的 GDT（stage2 的那张表在低地址，内核起来后
        ; 应该换成内核自己的，避免依赖引导阶段的内存布局）。
        lgdt    [gdt64_ptr]

        ; 重载 CS，真正切到内核自己的代码段
        push    qword 0x08
        lea     rax, [rel .reload_cs]
        push    rax
        retfq

.reload_cs:
        mov     ax, KERNEL_DS
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax

        ; 装载内核堆栈
        lea     rsp, [rel stack_top]
        ; ABI 要求进入函数体时 RSP 16 字节对齐
        and     rsp, ~0xF

        ; 清 BSS
        lea     rdi, [rel __bss_start]
        lea     rcx, [rel __bss_end]
        sub     rcx, rdi
        xor     eax, eax
        rep     stosb

        call    kmain

.hang:
        cli
        hlt
        jmp     .hang

section .rodata
align 16
gdt64:
        dq      0x0000000000000000      ; 空描述符
gdt64_code:
        dq      0x00AF9A000000FFFF      ; 64 位代码段，选择子 0x08
gdt64_data:
        dq      0x00CF92000000FFFF      ; 数据段，选择子 0x10
gdt64_end:

gdt64_ptr:
        dw      gdt64_end - gdt64 - 1
        dq      gdt64

section .bss
align 16
stack_bottom:
        resb    65536
stack_top:
