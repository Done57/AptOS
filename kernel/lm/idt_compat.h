/*
 * 64 位构建下的 idt.h 替身。
 *
 * timer.c、keyboard.c 这类共享模块写的是 32 位中断 API（regs_t、
 * irq_register、irq_unmask）。在 64 位构建里，makefile 用 -include
 * 强制让 include "idt.h" 解析到这个头，再由宏把它们转发到 64 位
 * 中断层 —— 共享源码一行不用改。
 */
#ifndef APTOS_IDT_COMPAT_H
#define APTOS_IDT_COMPAT_H

#include "idt64.h"

typedef regs64_t        regs_t;

#define irq_register    irq64_register
#define irq_unmask      irq64_unmask
#define irq_mask        irq64_mask
#define irq_eoi         irq64_eoi

#endif
