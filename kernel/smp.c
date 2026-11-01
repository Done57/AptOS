/*
 * smp.c —— 对称多处理(SMP)支持
 *
 * 流程(BSP 视角):
 *   1. acpi_init() 从 MADT 拿到每个核的 Local APIC ID 列表;
 *   2. smp_init(): 映射 LAPIC 寄存器,把 trampoline 拷到物理 0x8000,
 *      填 CR3/入口/栈/编号,对每个 AP 发 INIT-SIPI-SIPI;
 *   3. AP 醒来 -> trampoline -> ap_entry32(): 重载内核 GDT/IDT,
 *      开 LAPIC 与本地定时器,报告上线,hlt 等中断;
 *   4. LAPIC 定时器中断(向量 0xFE)递增各核心跳计数,`smp` 命令查看。
 *
 * 多核安全:
 *   - AP 不调用任何未加锁的内核子系统(堆/kprintf/FS 都只在 BSP 跑),
 *     只碰自己的栈 + LAPIC 寄存器 + 原子计数,避免踩坏单核数据结构;
 *   - 提供 spinlock(原子 test-and-set)给后续把子系统多核化时使用。
 */

#include "smp.h"
#include "acpi.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "timer.h"
#include "util.h"
#include "heap.h"

#ifndef SMP_MAX_CPUS
#define SMP_MAX_CPUS 8
#endif

/* ---- trampoline 符号(汇编导出) ---- */
extern u8 smp_trampoline_start[];
extern u8 smp_trampoline_end[];

extern void isr254(void);
extern idt_ptr_t idt_ptr;
extern gdt_ptr_t gp;

#define TRAMP_PHYS    0x8000
#define T_CR3_OFF     0xF00
#define T_ENTRY_OFF   0xF04
#define T_CPUID_OFF   0xF08
#define T_STACK_OFF   0xF0C
#define T_GDTPTR_OFF  0xF10
#define T_GDT_OFF     0xF18             /* trampoline 内临时 GDT 表 */

/* ---- Local APIC 寄存器(相对基址的 4KB) ---- */
#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_SIVR      0x0F0
#define LAPIC_ICRLO     0x300
#define LAPIC_ICRHI     0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_ICR 0x380
#define LAPIC_TIMER_DCR 0x3E0

#define AP_STACK_SIZE   0x8000         /* 每核 32KB 栈 */

/* ---- 全局状态 ---- */
static volatile u32 lapic_base = 0;
static volatile int  ncpu = 1;         /* BSP 算 1 */
static volatile int  boot_progress = 0;

static volatile u32 ap_heartbeat[SMP_MAX_CPUS];
static volatile int ap_alive[SMP_MAX_CPUS];
static volatile u32 ap_apic_id_tab[SMP_MAX_CPUS];

/* ---------- LAPIC ---------- */
static inline u32 lapic_read(u32 reg)
{
    return *(volatile u32 *)(lapic_base + reg);
}

static inline void lapic_write(u32 reg, u32 val)
{
    *(volatile u32 *)(lapic_base + reg) = val;
}

static void lapic_wait_icr(void)
{
    for (int i = 0; i < 1000; i++) {
        if (!(lapic_read(LAPIC_ICRLO) & (1 << 12)))
            return;
        io_wait();
    }
}

static void lapic_send_ipi(u8 apic_id, u32 icrlo)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICRHI, ((u32)apic_id << 24));
    lapic_write(LAPIC_ICRLO, icrlo);
}

u32 smp_apic_read_id(void)
{
    if (!lapic_base)
        return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* ---------- 自旋锁 ---------- */
void spin_lock(spinlock_t *lk)
{
    for (;;) {
        if (__atomic_test_and_set(&lk->locked, __ATOMIC_ACQUIRE) == 0)
            return;
        __asm__ volatile("pause");
    }
}

void spin_unlock(spinlock_t *lk)
{
    __atomic_clear(&lk->locked, __ATOMIC_RELEASE);
}

int spin_trylock(spinlock_t *lk)
{
    return __atomic_test_and_set(&lk->locked, __ATOMIC_ACQUIRE) == 0;
}

void smp_lapic_timer_ap(void);

/* ---------- 中断(idt.c 的 0xFE 分发钩子) ---------- */
void smp_lapic_timer(regs_t *r)
{
    (void)r;
    smp_lapic_timer_ap();
}

/* AP 定时器 ISR 的实际入口:汇编 stub isr254 压入 vector 后走 isr_common。
 * 核号用 LAPIC ID 反查——ISR 在哪个核上跑,读出来的就是那个核的 ID,
 * 天然无竞态,不需要 per-CPU GS 段。 */
void smp_lapic_timer_ap(void);
void smp_lapic_timer_ap(void)
{
    lapic_write(LAPIC_TIMER_ICR, 0x2000000);   /* 重装(单次模式) */
    lapic_write(LAPIC_EOI, 0);
    if (!lapic_base)
        return;
    u32 aid = (lapic_read(LAPIC_ID) >> 24) & 0xFF;
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        if (ap_apic_id_tab[i] == aid) {
            ap_heartbeat[i]++;
            break;
        }
    }
}

/* ---------- AP 入口(trampoline 无参调用; 核号从 LAPIC ID 反查) ---------- */
void ap_entry32(void);
void ap_entry32(void)
{
    /* 从 LAPIC ID 反查分配给我的 CPU 编号 */
    u32 aid = (lapic_read(LAPIC_ID) >> 24) & 0xFF;
    int cpu_id = -1;
    for (int i = 0; i < SMP_MAX_CPUS; i++)
        if (ap_apic_id_tab[i] == aid) { cpu_id = i; break; }
    if (cpu_id < 0)
        cpu_id = 1;

    /* 重载内核 GDT/IDT(BSP 已初始化好, AP 沿用同一套) */
    __asm__ volatile("lgdt %0" ::"m"(gp));
    __asm__ volatile("lidt %0" ::"m"(idt_ptr));
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        :
        :
        : "eax", "memory");

    /* 开本地 APIC: SIVR = 使能 + 向量 0xFF(spurious) */
    lapic_write(LAPIC_SIVR, 0x100 | 0xFF);

    /* 本地 APIC 定时器: 分频 1:16, 周期触发, 向量 0xFE(心跳) */
    lapic_write(LAPIC_TIMER_DCR, 0x0B);       /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, 0x200FE);    /* 周期模式, 向量 0xFE */
    lapic_write(LAPIC_TIMER_ICR, 0x100000);   /* 初值 */

    ap_alive[cpu_id] = 1;
    boot_progress++;

    /* 开中断, 进入空闲循环(定时器中断驱动心跳) */
    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}

/* ---------- 初始化 ---------- */
int smp_cpu_count(void) { return ncpu; }

int smp_init(void)
{
    const acpi_info_t *ai = acpi_info();
    if (!ai || !ai->present || !ai->lapic_addr) {
        kprintf("[smp] no ACPI/MADT - staying uniprocessor\n");
        return 0;
    }
    if (ai->cpu_count <= 1) {
        kprintf("[smp] single processor detected, nothing to wake\n");
        return 0;
    }

    /* 1) 映射 LAPIC 寄存器空间(恒等映射分页下按设备页映射) */
    if (paging_map_device(ai->lapic_addr, 4096) != 0) {
        kprintf("[smp] cannot map LAPIC at 0x%x\n", ai->lapic_addr);
        return 0;
    }
    lapic_base = ai->lapic_addr;

    /* 2) BSP 也开 APIC(SIVR 使能),供 IPI 发送 */
    lapic_write(LAPIC_SIVR, 0x100 | 0xFF);
    ap_apic_id_tab[0] = smp_apic_read_id();

    /* 3) 挂 0xFE 向量(IDT 表项,内核选择子,中断门) */
    idt_set_gate(0xFE, (u32)isr254, 0x08, 0x8E);

    /* 4) 拷 trampoline 到 0x8000(无需填参数: 平坦段直达, 栈/核号自洽) */
    u32 len = (u32)(smp_trampoline_end - smp_trampoline_start);
    memcpy((void *)TRAMP_PHYS, smp_trampoline_start, len);

    /* 5) 逐个唤醒 AP(跳过 BSP 自己: MADT 第 0 个通常就是 BSP) */
    int woke = 0;
    int n = ai->cpu_count;
    if (n > SMP_MAX_CPUS)
        n = SMP_MAX_CPUS;
    for (int i = 1; i < n; i++) {
        u8 apic_id = ai->cpu_apic_id[i];

        /* 分配 AP 内核栈(16KB 对齐) */
        u8 *stack = (u8 *)kmalloc_a(AP_STACK_SIZE, 16);
        if (!stack) {
            kprintf("[smp] cpu%d: no memory for stack, skipping\n", i);
            break;
        }
        (void)stack;

        boot_progress = 0;
        ap_alive[i] = 0;
        ap_apic_id_tab[i] = apic_id;

        /* 完整 Intel SDM 协议: INIT-assert -> INIT-deassert -> SIPI */
        lapic_send_ipi(apic_id, 0x00004500);   /* INIT level assert */
        timer_sleep_ms(10);
        lapic_send_ipi(apic_id, 0x00008500);   /* INIT level de-assert */
        timer_sleep_ms(10);
        lapic_send_ipi(apic_id, 0x00004608);   /* SIPI, 向量 0x08 -> 0x8000 */
        timer_sleep_ms(300);

        /* 等待上线(最多 ~2s) */
        int ok = 0;
        for (int w = 0; w < 20; w++) {
            if (ap_alive[i]) { ok = 1; break; }
            timer_sleep_ms(100);
        }
        if (ok) {
            woke++;
            kprintf("[smp] cpu%d (apic %d) online\n", i, apic_id);
        } else {
            kprintf("[smp] cpu%d (apic %d) did not come up\n", i, apic_id);
        }
    }

    ncpu = 1 + woke;
    kprintf("[smp] %d cpu(s) active (BSP + %d AP)\n", ncpu, woke);
    return woke;
}

/* `smp` 命令的数据接口 */
int  smp_cpu_alive(int i) { return (i == 0) ? 1 : ap_alive[i]; }
u32  smp_cpu_apic(int i)  { return ap_apic_id_tab[i]; }
u32  smp_cpu_hb(int i)    { return ap_heartbeat[i]; }
int  smp_max_cpus(void)   { return SMP_MAX_CPUS; }
