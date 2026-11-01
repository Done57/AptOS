/*
 * 64 位内核的第一阶段最小实现。
 *
 * 目标只有一个：证明"长模式 + 64 位 C 代码 + 页表 + 中断禁用"这条链
 * 能稳定跑起来。不碰图形、不碰磁盘、不碰用户态 —— 那些等骨架验证
 * 通过了再逐个接上。
 *
 * 这里用串口输出而不是屏幕输出，是因为屏幕上还什么都没有：VBE 模式
 * 是 stage2 设的，而图形栈要等 gfx 子系统迁过来才能用。串口是这个
 * 阶段唯一可靠的信道。
 */

#include "types.h"
#include "serial.h"
#include "klog.h"
#include "paging64.h"
#include "idt64.h"
#include "timer.h"
#include "keyboard.h"
#include "util.h"

/* ---- 十六进制输出（kprintf 只有 %x 而没有固定宽度，用这个补齐）---- */

static void serial_hex(u64 v, int digits)
{
    static const char d[] = "0123456789abcdef";
    char buf[17];
    int i;

    for (i = 0; i < digits; i++) {
        buf[i] = d[(v >> ((digits - 1 - i) * 4)) & 0xF];
    }
    buf[digits] = 0;
    serial_puts(buf);
}

/*
 * klog 里有一个"把日志同时刷到屏幕"的钩子。64 位内核目前还没有图形栈
 * （gfx/console 要等阶段三），所以先给个空实现占位。
 * klog_screen(1) 在 64 位下会被忽略，日志只走串口。
 */
void console_pump(void)
{
}

/* ---- 运行时自检 ---- */

/*
 * 这些检查用编译期断言做，任何一条不成立就直接编译失败 ——
 * 比运行到一半才发现指针被截断要好得多。
 */
typedef char assert_ptr64[(sizeof(void *) == 8) ? 1 : -1];
typedef char assert_size_t64[(sizeof(size_t) == 8) ? 1 : -1];
typedef char assert_uptr64[(sizeof(uptr) == 8) ? 1 : -1];
typedef char assert_u32is4[(sizeof(u32) == 4) ? 1 : -1];
typedef char assert_u64is8[(sizeof(u64) == 8) ? 1 : -1];

void kmain(void)
{
    serial_init();
    klog_init();

    serial_puts("\n");
    serial_puts("[64] ==========================================\n");
    serial_puts("[64] aptos x86-64 kernel - long mode bring-up\n");
    serial_puts("[64] ==========================================\n");

    /* 1. 确认真的在 64 位：取一个 64 位地址的高 32 位 */
    {
        u64 here = (u64)(uptr)&kmain;
        serial_puts("[64] kmain address      = 0x");
        serial_hex(here, 16);
        serial_puts("\n");

        /* 内核被链接在 1MB 附近，高 32 位必须是 0；但栈指针在
         * 高位时才能说明地址宽度真的生效。*/
        u64 sp;
        __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
        serial_puts("[64] stack pointer     = 0x");
        serial_hex(sp, 16);
        serial_puts("\n");
    }

    /* 2. 确认类型宽度符合 64 位预期 */
    serial_puts("[64] sizeof(void*)     = ");
    serial_hex(sizeof(void *), 2);
    serial_puts("\n");
    serial_puts("[64] sizeof(size_t)    = ");
    serial_hex(sizeof(size_t), 2);
    serial_puts("\n");
    serial_puts("[64] sizeof(uptr)      = ");
    serial_hex(sizeof(uptr), 2);
    serial_puts("\n");

    /* 3. 确认 EFER 里 LME 与 LMA 都置上了（LMA 是长模式真正生效的标志） */
    {
        u32 lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                         : "c"(0xC0000080));
        serial_puts("[64] EFER               = 0x");
        serial_hex(((u64)hi << 32) | lo, 16);
        serial_puts("\n");

        if (lo & (1u << 10))
            serial_puts("[64]   EFER.LMA set  -> long mode is ACTIVE\n");
        else
            serial_puts("[64]   EFER.LMA clear -> NOT in long mode!\n");
    }

    /* 4. 确认页表生效：写一个地址再读回来 */
    {
        volatile u64 *probe = (volatile u64 *)0x200000;
        *probe = 0xDEADBEEFCAFEBABEull;
        serial_puts("[64] memory probe      = 0x");
        serial_hex(*probe, 16);
        serial_puts("  (expect deadbeefcafebabe)\n");
    }

    /* 5. 确认 CR0/CR4/CR3 处于长模式应有的状态 */
    {
        u64 cr0, cr3, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        serial_puts("[64] CR0 = 0x"); serial_hex(cr0, 16); serial_puts("\n");
        serial_puts("[64] CR3 = 0x"); serial_hex(cr3, 16); serial_puts("\n");
        serial_puts("[64] CR4 = 0x"); serial_hex(cr4, 16);
        serial_puts("  (bit5 PAE must be set)\n");
    }

    /* kprintf 走的是内核真正的格式化路径（%d %x %s %p 都在里面），
     * 它跑起来说明 klog + serial + util 这三个模块在 64 位下都正常。 */
    kprintf("[64] kprintf check: dec=%d hex=%x str=%s ptr=%p\n",
            12345, 0xABCDEF, "ok", (void *)0x1234);

    /* ---- 四级页表接管 ---- */
    serial_puts("\n[64] --- 四级页表 ---\n");
    p64_init(64ull * 1024 * 1024);      /* 恒等映射 64MB，实际会取整到 1GB */

    /* 验证 1：CR3 换成了我们自己分配的 PML4 */
    {
        u64 cr3 = p64_get_cr3();
        kprintf("[64] CR3        = 0x%lx\n", cr3);
        kprintf("[64] PML4 指针  = 0x%lx  %s\n", (u64)(uptr)p64_root(),
                (cr3 == (u64)(uptr)p64_root()) ? "(一致)" : "(不一致!)");
    }

    /* 验证 2：翻译几个已知地址。
     * 恒等映射下 虚拟 == 物理，翻译结果必须相同。*/
    {
        /* 区间是左闭右开 [0, 1GB)，所以边界内最后一个地址是
         * 0x3FFFF000，而不是 0x40000000。*/
        u64 addrs[] = { 0x100000ull, 0x200000ull, 0x3FFFF000ull };
        int i;

        for (i = 0; i < 3; i++) {
            u64 pa = p64_translate(addrs[i]);
            kprintf("[64] translate 0x%lx -> 0x%lx  %s\n",
                    addrs[i], pa, (pa == addrs[i]) ? "OK" : "不符!");
        }

        /* 顺带确认超出映射范围确实查不到（这是防呆，不是失败）*/
        kprintf("[64] translate 0x%lx -> 0x%lx  (应超出映射范围，为 0)\n",
                0x40000000ull, p64_translate(0x40000000ull));
    }

    /* 验证 3：真的能通过新页表读写内存（0x2000000 = 32MB）*/
    {
        volatile u64 *p = (volatile u64 *)0x2000000ull;
        u64 pattern = 0x1122334455667788ull;
        *p = pattern;
        kprintf("[64] 32MB 读写   = 0x%lx  %s\n", *p,
                (*p == pattern) ? "OK" : "不符!");
    }

    /* 验证 4：动态映射一个新页，确认它真的生效。
     * 挑一个 4GB 以上的地址 —— 这是 32 位版本根本做不到的，
     * 能成功就说明 64 位地址通路是通的。*/
    {
        u64 va = 0x100000000ull;        /* 4GB */
        u64 pa = 0x3000000ull;          /* 物理 48MB */
        volatile u64 *q = (volatile u64 *)va;

        /* 先清零目标物理页，再映射，再写入 */
        memset((void *)(uptr)pa, 0, 4096);

        if (p64_map_page(va, pa, P64_RW) == 0) {
            *q = 0xCAFEBABEDEADF00Dull;
            kprintf("[64] 4GB 处映射   = 0x%lx  %s\n", *q,
                    (*q == 0xCAFEBABEDEADF00Dull) ? "OK" : "不符!");
            kprintf("[64]   translate 0x%lx -> 0x%lx\n", va, p64_translate(va));
        } else {
            kprintf("[64] 4GB 处映射失败\n");
        }
    }

    p64_dump_stats();

    /* ---- 中断：IDT + PIC + 时钟 ----
     * 这是长模式下的第一次开中断。若 IDT 门格式 / 压栈顺序 / iretq
     * 有任何差错，第一声 tick 就是三重故障 + 自动重启。*/
    serial_puts("\n[64] --- 中断 ---\n");
    idt64_init();

    /* 显式开中断。32 位路径里这一步隐含在别处，64 位把链路
     * 每一环都写明白：lidt 之后 IF 仍是 0，没有 sti，
     * CPU 对 PIC 上的所有请求视而不见。 */
    __asm__ volatile("sti");
    kprintf("[64] sti 已执行\n");

    timer_init(100);
    kprintf("[64] timer_init 返回\n");

    /* 让时钟跑 200ms，看 tick 是否真的在涨 —— 这验证了完整的
     * 中断链路：PIT -> PIC -> IDT 门 -> 64 位桩 -> C 分发 -> EOI。*/
    {
        u32 t0 = timer_ticks();
        kprintf("[64] t0=%d 进入 hlt 等待\n", t0);
        timer_sleep_ms(200);
        u32 t1 = timer_ticks();

        kprintf("[64] 时钟 tick: %d -> %d (200ms 内 %s)\n",
                t0, t1,
                (t1 > t0) ? "在跳" : "没跳!");
    }

    /* 键盘：IRQ1 的第二个真实中断链路。主函数不等人按键，
     * 只是注册 + 放开 IRQ1；真正的按键测试在下面进行。*/
    keyboard_init();
    irq64_unmask(1);

    /* 开中断后故意检查 sti 是否生效 */
    {
        u64 f;
        __asm__ volatile("pushfq; popq %0" : "=r"(f));
        kprintf("[64] RFLAGS.IF = %d (%s)\n",
                (int)((f >> 9) & 1),
                ((f >> 9) & 1) ? "开" : "关");
    }

    /* ---- 键盘实测：等 3 秒，期间敲进的键应当进队列 ---- */
    {
        u32 t_end = timer_ticks() + 300;
        int keys = 0;

        kprintf("[64] 键盘监听 3 秒，请敲键盘...\n");
        while (timer_ticks() < t_end) {
            if (keyboard_has_key()) {
                int k = keyboard_getkey();
                if (k > 0) {
                    keys++;
                    kprintf("[64]   按键: %d\n", k);
                }
            } else {
                __asm__ volatile("hlt");
            }
        }
        kprintf("[64] 收到 %d 个按键 %s\n", keys,
                keys > 0 ? "(键盘中断 OK)" : "(没收到键)");
    }

    serial_puts("[64] bring-up checks complete.\n");
    serial_puts("[64] halting.\n");

    for (;;)
        __asm__ volatile("hlt");
}
