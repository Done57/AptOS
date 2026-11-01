; aptos 64 位中断桩。
;
; 与 32 位版本的差别（每一条都会让不改的代码直接死）：
;   1. 64 位下没有 pusha/popa —— 必须手动压 15 个通用寄存器。
;   2. 栈上的压入都是 8 字节，没有"dword 可省"的余地。
;   3. CPU 在中断时会把 SS 和 RSP 一起压栈（长模式强制），
;      所以 iretq 的帧是 5 个 8 字节：RIP/CS/RFLAGS/RSP/SS。
;   4. 返回指令是 iretq，不是 iret。
;   5. 段寄存器在长模式下不再有基址，但 DS/ES 的值仍必须合法，
;      中断后要恢复；FS/GS 的基址走 MSR，内核线程切换时另行处理。

BITS 64

section .text

%macro ISR_NOERR64 1
global isr64_%1
isr64_%1:
        push    qword 0
        push    qword %1
        jmp     isr64_common
%endmacro

%macro ISR_ERR64 1
global isr64_%1
isr64_%1:
        push    qword %1
        jmp     isr64_common
%endmacro

; 异常 0..31：有错误码的（8/10/11/12/13/14/17）必须用 ERR 变体，
; 否则栈上的中断号和错误码会错位，处理程序全部读歪。
ISR_NOERR64 0
ISR_NOERR64 1
ISR_NOERR64 2
ISR_NOERR64 3
ISR_NOERR64 4
ISR_NOERR64 5
ISR_NOERR64 6
ISR_NOERR64 7
ISR_ERR64   8
ISR_NOERR64 9
ISR_ERR64   10
ISR_ERR64   11
ISR_ERR64   12
ISR_ERR64   13
ISR_ERR64   14
ISR_NOERR64 15
ISR_NOERR64 16
ISR_ERR64   17
ISR_NOERR64 18
ISR_NOERR64 19
ISR_NOERR64 20
ISR_NOERR64 21
ISR_NOERR64 22
ISR_NOERR64 23
ISR_NOERR64 24
ISR_NOERR64 25
ISR_NOERR64 26
ISR_NOERR64 27
ISR_NOERR64 28
ISR_NOERR64 29
ISR_ERR64   30
ISR_NOERR64 31

%assign i 32
%rep 16
ISR_NOERR64 i
%assign i i+1
%endrep

extern isr64_handler

; 通用保存/分发/恢复。
; 15 个 GPR：rax rbx rcx rdx rsi rdi rbp r8..r15
isr64_common:
        push    rax
        push    rbx
        push    rcx
        push    rdx
        push    rsi
        push    rdi
        push    rbp
        push    r8
        push    r9
        push    r10
        push    r11
        push    r12
        push    r13
        push    r14
        push    r15

        ; 段寄存器：值本身要压栈保存，再统一换成内核数据段
        mov     ax, ds
        push    rax
        mov     ax, es
        push    rax
        mov     ax, fs
        push    rax
        mov     ax, gs
        push    rax

        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax

        ; 传栈指针（指向完整寄存器帧）给 C 处理程序
        mov     rdi, rsp
        call    isr64_handler

        pop     rax
        mov     gs, ax
        pop     rax
        mov     fs, ax
        pop     rax
        mov     es, ax
        pop     rax
        mov     ds, ax

        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     r11
        pop     r10
        pop     r9
        pop     r8
        pop     rbp
        pop     rdi
        pop     rsi
        pop     rdx
        pop     rcx
        pop     rbx
        pop     rax

        ; 跳过 C 错误码/中断号（CPU 压的错误码+我们的中断号）
        add     rsp, 16
        iretq

; 系统调用入口沿用 int 0x80（64 位下重排为 0x80 号门）
global isr64_128
isr64_128:
        push    qword 0
        push    qword 0x80
        jmp     isr64_common

section .data
global isr64_stub_table
isr64_stub_table:
%assign i 0
%rep 48
        dq isr64_%+i
%assign i i+1
%endrep
