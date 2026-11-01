#ifndef APTOS_IDT64_H
#define APTOS_IDT64_H

#include "types.h"

/* 64 位中断帧 —— 与 isr64.asm 的压栈顺序严格对应。
 * 栈从高到低：SS RSP RFLAGS CS RIP (+err) int_no (+err 我们压的)
 * 然后 15 个 GPR + 4 个段寄存器。
 * isr64_common 里先 push rdi 再 call，所以结构体第一个字段是 r15。
 */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 ds, es, fs, gs;
    u64 int_no;         /* 我们压的中断号 */
    u64 err_code;       /* CPU 压的错误码（无则为 0） */
    u64 rip, cs, rflags, rsp, ss;
} PACKED regs64_t;

typedef void (*irq64_handler_t)(regs64_t *r);

void idt64_init(void);
void idt64_set_gate(int n, uptr handler, u8 flags);
void irq64_register(int irq, irq64_handler_t h);
void irq64_unmask(int irq);
void irq64_mask(int irq);
void irq64_eoi(int irq);

#endif
