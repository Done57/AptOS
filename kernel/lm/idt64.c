/*
 * 64 位 IDT 与中断分发。
 *
 * 与 32 位版本的结构性差别：
 *   - 门描述符 16 字节（不是 8 字节），偏移字段被拆成三段，
 *     完整承载 64 位处理程序地址。
 *   - 多了 IST（Interrupt Stack Table）字段，本内核暂不用（填 0）。
 *   - 类型值与 32 位相同（0x8E 中断门 / 0xEE 用户态陷阱门），
 *     但长模式下中断门强制关 IF、返回用 iretq。
 */

#include "types.h"
#include "util.h"
#include "klog.h"
#include "io.h"
#include "idt64.h"

#define IDT64_ENTRIES 256

typedef struct {
    u16 offset_low;     /* 0..15   */
    u16 selector;       /* 内核代码段 0x08 */
    u8  ist;            /* IST 索引，0 = 不用 */
    u8  flags;          /* 类型 + DPL + P */
    u16 offset_mid;     /* 16..31  */
    u32 offset_high;    /* 32..63  */
    u32 reserved;
} PACKED idt64_entry_t;

typedef struct {
    u16 limit;
    u64 base;
} PACKED idt64_ptr_t;

static idt64_entry_t idt[IDT64_ENTRIES];
static idt64_ptr_t   idt_ptr;

/* 16 个传统 IRQ 的 C 级处理程序 */
static irq64_handler_t irq_handlers[16];

/* 桩表来自 isr64.asm：48 个入口（32 异常 + 16 IRQ） */
extern uptr isr64_stub_table[];

/* 重映射 PIC：IRQ 0..7 -> 0x20..0x27，IRQ 8..15 -> 0x28..0x2F */
static void pic_remap(void)
{
    outb(0x20, 0x11);           /* ICW1：初始化 + 级联 */
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);           /* ICW2：主片向量基址 */
    outb(0xA1, 0x28);           /* ICW2：从片向量基址 */
    io_wait();
    outb(0x21, 0x04);           /* ICW3：主片的从片位 */
    outb(0xA1, 0x02);           /* ICW3：从片接主片 IRQ2 */
    io_wait();
    outb(0x21, 0x01);           /* ICW4：8086 模式 */
    outb(0xA1, 0x01);
    io_wait();
    outb(0x21, 0xFF);           /* 先全屏蔽，驱动按需打开 */
    outb(0xA1, 0xFF);
}

void idt64_set_gate(int n, uptr handler, u8 flags)
{
    idt[n].offset_low  = (u16)(handler & 0xFFFF);
    idt[n].offset_mid  = (u16)((handler >> 16) & 0xFFFF);
    idt[n].offset_high = (u32)((handler >> 32) & 0xFFFFFFFF);
    idt[n].selector    = 0x08;
    idt[n].ist         = 0;
    idt[n].flags       = flags;
    idt[n].reserved    = 0;
}

void irq64_register(int irq, irq64_handler_t h)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = h;
}

void irq64_unmask(int irq)
{
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8  bit  = (u8)(irq & 7);
    u8  v    = inb(port) & (u8)~(1u << bit);
    outb(port, v);
}

void irq64_mask(int irq)
{
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8  bit  = (u8)(irq & 7);
    u8  v    = inb(port) | (u8)(1u << bit);
    outb(port, v);
}

void irq64_eoi(int irq)
{
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static const char *exc_name(u64 n)
{
    static const char *names[32] = {
        "#DE 除零", "#DB 调试", "NMI", "#BP 断点", "#OF 溢出",
        "#BR 边界", "#UD 无效指令", "#NM 无FPU", "#DF 双重错误",
        "保留", "#TS 无效TSS", "#NP 段不存在", "#SS 栈错误",
        "#GP 一般保护", "#PF 页错误", "保留", "#MF x87",
        "#AC 对齐检查", "#MC 机器检查", "#XM SIMD", "#VE 虚拟化",
        "CP 控制保护", "保留", "保留", "保留", "保留",
        "保留", "保留", "保留", "保留", "#SX 安全", "保留"
    };
    return (n < 32) ? names[n] : "未知";
}

void isr64_handler(regs64_t *r)
{
    u64 n = r->int_no;

    if (n < 32) {
        /* ---- 异常 ---- */
        u64 cr2 = 0;
        if (n == 14)
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        kprintf("[exc] %s (%d) at rip=%p err=%d rsp=%p\n",
                exc_name(n), (int)n, (void *)(uptr)r->rip,
                (int)r->err_code, (void *)(uptr)r->rsp);
        if (n == 14)
            kprintf("[exc]   缺陷地址 cr2=%p\n", (void *)(uptr)cr2);

        for (;;)
            __asm__ volatile("hlt");
        return;
    }

    /* ---- IRQ ---- */
    int irq = (int)n - 32;
    if (irq >= 0 && irq < 16) {
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        irq64_eoi(irq);
    }
}

/* ---- 共享模块的兼容入口 ----
 *
 * timer.c / keyboard.c 包含的是 kernel/idt.h（32 位声明），调用
 * irq_register 等名字。预处理器的引号 include 规则决定了它们永远
 * 找到 kernel/idt.h 而不是任何 -I 替身，所以兼容做在链接层：
 * 这里定义同名符号，把调用转发到 64 位实现。
 *
 * 处理程序指针按 ABI 兼容处理：它们只收到 regs64_t*，而这些
 * 处理程序本来就忽略参数。
 */
void irq_register(int irq, void (*h)(void *))
{
    irq64_register(irq, (irq64_handler_t)h);
}

void irq_unmask(int irq) { irq64_unmask(irq); }
void irq_mask(int irq)   { irq64_mask(irq); }
void irq_eoi(int irq)    { irq64_eoi(irq); }

void idt64_init(void)
{
    memset(irq_handlers, 0, sizeof(irq_handlers));
    memset(idt, 0, sizeof(idt));

    /* 48 个桩：0..31 异常，32..47 IRQ */
    for (int i = 0; i < 48; i++)
        idt64_set_gate(i, isr64_stub_table[i], 0x8E);

    /* 系统调用门：int 0x80 从 ring3 进来 */
    idt64_set_gate(0x80, (uptr)isr64_stub_table[0x80 & 0x3F], 0xEE);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (u64)(uptr)idt;

    pic_remap();

    __asm__ volatile("lidt %0" :: "m"(idt_ptr) : "memory");

    kprintf("[idt64] 256 个门就绪（48 桩 + 0x80 系统调用），PIC 已重映射\n");
}
