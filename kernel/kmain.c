#include "acpi.h"
#include "ata.h"
#include "blkdev.h"
#include "floppy.h"
#include "bootinfo.h"
#include "console.h"
#include "splash.h"
#include "fs.h"
#include "gdt.h"
#include "gfx.h"
#include "gui.h"
#include "heap.h"
#include "i18n.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "mouse.h"
#include "paging.h"
#include "part.h"
#include "net.h"
#include "pci.h"
#include "e1000.h"
#include "hda.h"
#include "pkg.h"

void term_say(const char *fmt, ...);
#include "syscall.h"
#include "rtc.h"
#include "serial.h"
#include "timer.h"
#include "util.h"

void kmain(void)
{
    serial_init();
    klog_init();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  aptos 2.0 - 32-bit x86 kernel\n");
    kprintf("========================================\n");

    if (BOOTINFO->magic != BOOTINFO_MAGIC) {
        kprintf("[boot] FATAL: bad bootinfo magic 0x%x\n", BOOTINFO->magic);
        for (;;)
            hlt();
    }
    kprintf("[boot] framebuffer 0x%x %dx%d %dbpp, kernel lba %d, fs lba %d\n",
            BOOTINFO->fb_addr, BOOTINFO->fb_width, BOOTINFO->fb_height,
            BOOTINFO->fb_bpp, BOOTINFO->kernel_lba, BOOTINFO->fs_lba);

    heap_init();
    kprintf("[mem] usable RAM: %d MiB\n", mem_total_kb() / 1024);

    gdt_init();
    idt_init();

    paging_init();

    gfx_init();

    /* 用图形文本控制台把内核日志实时画到 VBE 帧缓冲屏幕上，这样在没有
     * 串口的真实机器 / Limbo x86 上也能看到启动进度，而不是一片黑。 */
    {
        extern void console_init(void);
        console_init();
    }
    splash_begin();

    splash_step(1, 14, "starting the timer");
    timer_init(100);
    splash_step2(2, 14, "keyboard and mouse", "键盘与鼠标");
    keyboard_init();
    mouse_init();
    splash_step2(3, 14, "ACPI", "电源管理 ACPI");
    acpi_init();
    splash_step2(4, 14, "system calls", "系统调用");
    syscall_init();
    splash_step2(5, 14, "block devices", "块设备");
    blk_init();
    splash_step2(6, 14, "PCI bus", "PCI 总线");
    pci_init();
    splash_step2(7, 14, "disk drives", "磁盘驱动器");
    ata_init();
    part_scan_all();
    floppy_init();
    splash_step2(8, 14, "mounting the filesystem", "挂载文件系统");
    fs_init();
    fs_automount_all();
    splash_step2(9, 14, "network card", "网卡");
    e1000_init();
    net_init();
    splash_step2(10, 14, "sound card", "声卡");
    hda_init();
    splash_step2(11, 14, "language support", "语言支持");
    i18n_init();

    splash_step2(12, 14, "reading the clock", "读取系统时钟");
    sti();

    /* SMP: 唤醒其余 CPU(若有)。AP 进入心跳循环,不碰未加锁的子系统。 */
    {
        extern int smp_init(void);
        smp_init();
    }

    rtc_time_t t;
    rtc_read(&t);
    kprintf("[rtc] %04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day,
            t.hour, t.minute, t.second);
    kprintf("[init] all subsystems up - bringing up text console\n");
    splash_step2(13, 14, "starting the text console", "正在启动文本控制台");
    splash_finish("aptos");

    extern void term_external_write(const char *buf, u32 len);
    syscall_set_output(term_external_write);
    kprintf("[init] user mode ready: paging + ring 3 + int 0x80 syscalls\n");

    /* 光盘引导：跑图形安装向导（setup 需要 GUI） */
    if (!is_booting_from_hdd()) {
        kprintf("[init] booted from CD-ROM (drive 0x%x) - running installer\n",
                BOOTINFO->boot_drive);
        gui_init();
        gui_open(&app_terminal);
        term_say("%s\n", tr(S_BOOT_SETUP_DISC));
        if (fs_mount("cd0", "/") == 0) {
            if (fs_exists("/SETUP.LST")) {
                int r = pkg_run("/SETUP.LST");
                kprintf("[init] setup result %d\n", r);
                term_say("%s: %d\n", tr(S_BOOT_SETUP_EXIT), r);
            } else {
                term_say("%s\n", tr(S_BOOT_NO_SETUP));
            }
        } else {
            term_say("%s\n", tr(S_BOOT_NO_CD));
        }
        /* 安装向导结束后停在图形界面（用户可重启进入已装系统） */
        gui_run();
        return;
    }

    /* 硬盘引导：默认纯文本命令行，不初始化图形桌面。
     * 用户若装了桌面包，敲 `desk -o start` 再由 textshell 惰性进图形。 */
    extern void textshell_run(void);
    textshell_run();
}
