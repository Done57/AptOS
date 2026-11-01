#ifndef APTOS_IDT_H
#define APTOS_IDT_H

#include "types.h"

typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags;
} regs_t;

typedef void (*irq_handler_t)(regs_t *r);

void idt_init(void);
void idt_set_gate(int n, u32 handler, u16 selector, u8 flags);
typedef struct PACKED { u16 limit; u32 base; } idt_ptr_t;
extern idt_ptr_t idt_ptr;

void irq_register(int irq, irq_handler_t handler);
void irq_unmask(int irq);
void irq_mask(int irq);
void irq_eoi(int irq);

#endif
