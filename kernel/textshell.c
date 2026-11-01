/*
 * textshell.c —— 默认的纯文本命令行（VGA 文本 + 串口）
 *
 * Aptos/SixOS 默认是无图形的命令行系统：kmain 不开 GUI、不画 splash，
 * 直接进入这里的文本 shell。命令输出经 term_run_command_line() 复用
 * app_terminal 的整套命令实现（ls/help/df/install/run ...），输出被
 * 重定向到 kprintf（VGA 文本 0xB8000 与串口）。
 *
 * 只有装配了桌面包、用户主动敲 `desk ...` 时才切换到图形（见 desk_*）。
 */
#include "keyboard.h"
#include "klog.h"
#include "gfx.h"
#include "gui.h"
#include "fs.h"
#include "io.h"

/* 来自 app_terminal.c 的文本模式开关与命令入口 */
extern void term_enter_textmode(void);
extern void term_leave_textmode(void);
extern void term_run_command_line(char *line);

void textshell_run(void);

#define TS_LINE_MAX 160

extern void console_update(void);
static void ts_puts(const char *s) { kprintf("%s", s); }
static void ts_flush(void) { console_update(); }   /* 把 klog 刷新到帧缓冲屏幕 */


/* 读一整行（键盘，带简单回显与退格）。返回长度，Esc 返回 -1。 */
static int ts_readline(char *buf, int cap)
{
    int n = 0;
    buf[0] = 0;
    for (;;) {
        int k = keyboard_getkey();
        if (k == -1) {
            hlt();            /* 空闲等中断（省 CPU，并保证 IRQ1 能及时触发收键） */
            continue;
        }

        if (k == '\n' || k == '\r') {
            kprintf("\n");
            buf[n] = 0;
            ts_flush();
            return n;
        }
        if (k == 27) {                    /* Esc */
            kprintf("\n");
            return -1;
        }
        if (k == 8 || k == 127) {         /* 退格 */
            if (n > 0) {
                n--;
                buf[n] = 0;
                kprintf("\b");
            }
            ts_flush();
            continue;
        }
        if (k >= 32 && k < 127 && n < cap - 1) {
            buf[n++] = (char)k;
            buf[n] = 0;
            char s[2]; s[0] = (char)k; s[1] = 0;
            kprintf("%s", s);
            ts_flush();
        }
    }
}

/* 判断是不是要启动图形桌面（desk 命令且参数里是 start） */
static int is_desk_start(const char *line)
{
    /* 形如 "desk" / "desk -o start" / "desk start" */
    while (*line == ' ') line++;
    if (!(line[0]=='d'||line[0]=='D')) return 0;
    if (!(line[1]=='e'||line[1]=='E')) return 0;
    if (!(line[2]=='s'||line[2]=='S')) return 0;
    if (!(line[3]=='k'||line[3]=='K')) return 0;
    if (line[4] != 0 && line[4] != ' ') return 0;
    return 1;
}

void textshell_run(void)
{
    char line[TS_LINE_MAX];

    term_enter_textmode();

    ts_puts("\n");
    ts_puts("aptos 2.0 - text console (no desktop installed by default)\n");
    ts_puts("type 'help' for commands. install desktop: install /DESKTOP.LST\n");
    ts_puts("run graphical desktop with: desk -o start  then  start\n\n");
    ts_flush();

    for (;;) {
        ts_puts("aptos:/# ");
        int n = ts_readline(line, sizeof(line));
        if (n < 0)
            continue;
        if (n == 0)
            continue;

        /* 图形桌面：desk 需要 GUI，这里惰性启动并把控制权交给图形环境 */
        if (is_desk_start(line) && fs_exists("/APPS/DESK.APP")) {
            ts_puts("[textshell] launching graphical desktop...\n");
            term_leave_textmode();
            gui_init();
            term_run_command_line(line);   /* 交给 desk（图形）执行 */
            /* desk 退出后回到文本控制台 */
            term_enter_textmode();
            ts_puts("[textshell] returned from desktop.\n");
            ts_flush();
            continue;
        }

        /* 其它命令：走同一套命令解析，输出重定向到文本控制台 */
        term_run_command_line(line);
        kprintf("\n");            /* 确保提示符另起一行 */
        ts_flush();
    }
}
