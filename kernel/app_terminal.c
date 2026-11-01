#include "gui.h"
#include "acpi.h"
#include "e1000.h"
#include "hda.h"
#include "i18n.h"
#include "net.h"
#include "ata.h"
#include "blkdev.h"
#include "bootinfo.h"
#include "fs.h"
#include "heap.h"
#include "io.h"
#include "keyboard.h"
#include "heap.h"
#include "klog.h"
#include "lst.h"
#include "pkg.h"
#include "user.h"
#include "rtc.h"
#include "timer.h"
#include "util.h"

#define TERM_COLS 78              /* display cells: ASCII is 1, CJK is 2        */
#define TERM_MAX  240             /* bytes per line (a CJK char is up to 3)     */
#define TERM_ROWS 18
#define TERM_LINE_H 16            /* room for a 16x16 CJK glyph                 */
#define TERM_W (TERM_COLS * 8)
#define TERM_H (TERM_ROWS * TERM_LINE_H)
#define HIST_MAX 8

typedef struct {
    char lines[TERM_ROWS][TERM_MAX + 1];
    int  cx, cy;
    char inbuf[TERM_COLS + 1];
    int  inlen;
    char hist[HIST_MAX][TERM_COLS + 1];
    int  hist_count;
    int  hist_pos;
} term_t;

static term_t    term;

/* 文本控制台模式开关：打开时命令输出不再写窗口缓冲，而是直接 kprintf
 * 到 VGA 文本/串口，供无图形环境的命令行复用同一套命令实现。 */
int  g_term_textmode = 0;
void term_enter_textmode(void) { g_term_textmode = 1; }
void term_leave_textmode(void) { g_term_textmode = 0; }

static window_t *win;
static char      file_buf[4096];

static void term_scroll(void)
{
    for (int i = 0; i < TERM_ROWS - 1; i++)
        memcpy(term.lines[i], term.lines[i + 1], TERM_MAX + 1);
    memset(term.lines[TERM_ROWS - 1], 0, TERM_MAX + 1);
    term.cy = TERM_ROWS - 1;
}

static void term_newline(void)
{
    term.cx = 0;
    term.cy++;
    if (term.cy >= TERM_ROWS)
        term_scroll();
}

/* Append one glyph: UTF-8 bytes plus the number of cells it occupies. */
static void term_put_cp(u32 cp, const char *bytes, int n)
{
    int width = (cp < 0x80) ? 1 : 2;
    int len;

    if (term.cx + width > TERM_COLS)
        term_newline();
    len = (int)strlen(term.lines[term.cy]);
    if (len + n > TERM_MAX)
        term_newline();
    len = (int)strlen(term.lines[term.cy]);
    for (int i = 0; i < n; i++)
        term.lines[term.cy][len + i] = bytes[i];
    term.lines[term.cy][len + n] = 0;
    term.cx += width;
}

static void term_putc(int c)
{
    char b = (char)c;

    if (c == '\n') {
        term_newline();
        return;
    }
    if (c == '\r') {
        term.cx = 0;
        return;
    }
    if (c == '\t') {
        do {
            term_put_cp(' ', " ", 1);
        } while (term.cx % 8 && term.cx < TERM_COLS);
        return;
    }
    term_put_cp((u32)(u8)c, &b, 1);
}

static void term_puts(const char *s)
{
    if (g_term_textmode) {         /* 文本控制台：整串直接发 VGA/串口 */
        if (s) kprintf("%s", s);
        return;
    }
    while (s && *s) {
        int adv = 0;
        u32 cp;

        if ((u8)*s < 0x80) {
            term_putc((u8)*s);
            s++;
            continue;
        }
        cp = gfx_utf8_decode(s, &adv);
        if (adv <= 0) {
            s++;
            continue;
        }
        term_put_cp(cp, s, adv);
        s += adv;
    }
}

/* Same as term_puts for a buffer that is not NUL terminated (ring 3 output). */
static void term_write(const char *s, int len)
{
    int i = 0;

    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        int need, adv = 0;
        u32 cp;

        if (c < 0x80) {
            term_putc(c);
            i++;
            continue;
        }
        need = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 :
               ((c & 0xF8) == 0xF0) ? 4 : 1;
        if (need == 1 || i + need > len) {
            term_put_cp(0xFFFD, "?", 1);
            i++;
            continue;
        }
        cp = gfx_utf8_decode(s + i, &adv);
        term_put_cp(cp, s + i, adv);
        i += adv;
    }
}

static void term_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

static char cwd[128] = "/";

static void term_prompt(void)
{
    term_puts(cwd);
    term_puts("> ");
}

static void cmd_ls(const char *arg)
{
    char path[256];
    fs_dirent_t e;
    int i = 0, n = 0;

    fs_path(path, sizeof(path), cwd, arg && *arg ? arg : ".");
    if (!fs_is_dir(path)) {
        term_printf(tr(S_LS_NOTDIR), path);
        return;
    }

    term_printf(tr(S_LS_DIR), path);
    term_printf("%-20s %9s  %s\n", tr(S_LS_NAME), tr(S_LS_SIZE), tr(S_LS_DATE));
    while (fs_list(path, i++, &e) == 0) {
        if (e.flags & FS_FLAG_DIR)
            term_printf("%-20s %9s  %s\n", e.name, "-", tr(S_LS_DIRTAG));
        else
            term_printf("%-20s %9d  %08d\n", e.name, e.size, e.mdate);
        n++;
    }
    term_printf(tr(S_LS_SUMMARY), n, fs_free_kb());
}

static void cmd_cd(const char *arg)
{
    char path[256];

    fs_path(path, sizeof(path), cwd, arg && *arg ? arg : "/");
    if (!fs_is_dir(path)) {
        term_printf(tr(S_CD_NODIR), path);
        return;
    }
    strncpy(cwd, path, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
}

static void cmd_pwd(void)
{
    term_printf("%s\n", cwd);
}

static void cmd_cat(const char *arg)
{
    char path[256];
    int len;

    if (!arg || !*arg) {
        term_puts(tr(S_USAGE_CAT));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    if (fs_is_dir(path)) {
        term_printf(tr(S_CAT_ISDIR), path);
        return;
    }
    len = fs_read(path, file_buf, sizeof(file_buf) - 1);
    if (len < 0) {
        term_printf(tr(S_CAT_NOFILE), path);
        return;
    }
    file_buf[len] = 0;
    term_write(file_buf, len);
    if (len && file_buf[len - 1] != '\n')
        term_putc('\n');
}

static void cmd_write(const char *name, const char *text)
{
    char path[256];
    int n;

    if (!name || !*name) {
        term_puts(tr(S_USAGE_WRITE));
        return;
    }
    fs_path(path, sizeof(path), cwd, name);
    n = fs_write(path, text ? text : "", text ? (u32)strlen(text) : 0);
    if (n < 0)
        term_printf(tr(S_WRITE_FAIL), path);
    else
        term_printf(tr(S_WRITE_OK), n, path);
}

static void cmd_mkdir(const char *arg)
{
    char path[256];

    if (!arg || !*arg) {
        term_puts(tr(S_USAGE_MKDIR));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    if (fs_mkdir(path) == 0)
        term_printf(tr(S_MKDIR_OK), path);
    else
        term_printf(tr(S_MKDIR_FAIL), path);
}

static void cmd_rm(const char *arg)
{
    char path[256];
    int r;

    if (!arg || !*arg) {
        term_puts(tr(S_USAGE_RM));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    r = fs_delete(path);
    if (r == 0)
        term_printf(tr(S_RM_OK), path);
    else if (r == -2)
        term_printf(tr(S_RM_NOTEMPTY), path);
    else
        term_printf(tr(S_RM_NOFILE), path);
}

static void cmd_cp(const char *from, const char *to)
{
    char a[256], b[256];
    int len;

    if (!from || !to) {
        term_puts(tr(S_USAGE_CP));
        return;
    }
    fs_path(a, sizeof(a), cwd, from);
    fs_path(b, sizeof(b), cwd, to);
    if (fs_is_dir(b)) {
        char joined[256];
        snprintf(joined, sizeof(joined), "%s/%s", b, fs_basename(a));
        strncpy(b, joined, sizeof(b) - 1);
        b[sizeof(b) - 1] = 0;
    }
    len = fs_read(a, file_buf, sizeof(file_buf) - 1);
    if (len < 0) {
        term_printf(tr(S_CP_NOFILE), a);
        return;
    }
    if (fs_write(b, file_buf, (u32)len) < 0) {
        term_printf(tr(S_CP_FAIL), b);
        return;
    }
    term_printf(tr(S_CP_OK), a, b, len);
}

static void cmd_mv(const char *from, const char *to)
{
    char a[256], b[256];
    int r;

    if (!from || !to) {
        term_puts(tr(S_USAGE_MV));
        return;
    }
    fs_path(a, sizeof(a), cwd, from);
    fs_path(b, sizeof(b), cwd, to);
    if (fs_is_dir(b)) {
        char joined[256];
        snprintf(joined, sizeof(joined), "%s/%s", b, fs_basename(a));
        strncpy(b, joined, sizeof(b) - 1);
        b[sizeof(b) - 1] = 0;
    }
    r = fs_rename(a, b);
    if (r == 0)
        term_printf(tr(S_MV_OK), a, b);
    else if (r == -2)
        term_printf(tr(S_MV_EXISTS), b);
    else
        term_printf(tr(S_MV_NOFILE), a);
}

static void cmd_dmesg(void)
{
    int n = klog_count();
    for (int i = 0; i < n; i++)
        term_printf("%s\n", klog_line(i));
}

static void cmd_help(void)
{
    static const char *const help_en[] = {
        "help              this list",
        "clear             clear the screen",
        "echo <text>       print text",
        "ls [path]         list a directory",
        "cd <path> / pwd   walk the tree",
        "cat <file>        show a file",
        "write <f> <text>  create/overwrite a file",
        "mkdir <name>      create a directory",
        "rm <name>         delete file or empty dir",
        "cp <src> <dst>    copy a file",
        "mv <src> <dst>    rename or move",
        "df                filesystem usage",
        "mem               memory usage",
        "dmesg             kernel boot log",
        "date / time       hardware clock",
        "uptime            ticks since boot",
        "ver               version banner",
        "gfx               display information",
        "ifconfig          show the network card and addresses",
        "dhcp              ask the network for an IP address",
        "ping <ip>         send ICMP echo to that address",
        "nslookup <name>   resolve a hostname via DNS",
        "audio             sound hardware information",
        "beep [hz] [ms]    play a beep (default 880 Hz, 250 ms)",
        "music             play a little tune",
        "install <pkg.lst> open a package and run its installer",
        "pkginfo <pkg.lst> show what a package contains",
        "run <file>        execute an ELF32 binary in ring 3",
        "memtest [mb]      allocate and verify that much RAM (default 16)",
        "acpi              ACPI tables and power management",
        "dev               list block devices",
        "mounts            list mounted filesystems",
        "mount <dev> <dir> mount a filesystem (hd0/hd1/cd0/fd0)",
        "umount <dir>      unmount a filesystem",
        "mkfs <dev>        format a device as MINIFS02",
        "apps              list the applications",
        "files|edit <f>|paint|about   open a window",
        "snake|mines|2048  play a game",
        "lang [zh|en]      switch the interface language",
        "reboot            restart (8042 reset)",
        "poweroff          ACPI S5 power off",
    };
    static const char *const help_zh[] = {
        "help              显示这个列表",
        "clear             清屏",
        "echo <文本>       打印文本",
        "ls [路径]         列出目录内容",
        "cd <路径> / pwd   在目录树中移动",
        "cat <文件>        显示文件内容",
        "write <文件> <内容>  新建或覆盖文件",
        "mkdir <名称>      新建目录",
        "rm <名称>         删除文件或空目录",
        "cp <源> <目标>    复制文件",
        "mv <源> <目标>    重命名或移动文件",
        "df                文件系统用量",
        "mem               内存用量",
        "dmesg             内核启动日志",
        "date / time       硬件时钟",
        "uptime            开机以来的运行时间",
        "ver               版本信息",
        "gfx               显示信息",
        "ifconfig          显示网卡与地址信息",
        "dhcp              向网络请求 IP 地址",
        "ping <ip>         向该地址发送 ICMP 回显",
        "nslookup <名称>   通过 DNS 解析域名",
        "audio             声卡硬件信息",
        "beep [赫兹] [毫秒]  播放一声蜂鸣 (默认 880 赫兹, 250 毫秒)",
        "music             播放一段小曲",
        "install <包.lst>  打开安装包并运行它的安装程序",
        "pkginfo <包.lst>  查看安装包里有什么",
        "run <文件>        在 ring 3 里运行 ELF32 程序",
        "memtest [MB]      分配并校验内存 (默认 16)",
        "acpi              ACPI 表与电源管理",
        "dev               列出块设备",
        "mounts            列出已挂载的文件系统",
        "mount <设备> <目录>  挂载文件系统 (hd0/hd1/cd0/fd0)",
        "umount <目录>     卸载文件系统",
        "mkfs <设备>       把设备格式化成 MINIFS02",
        "apps              列出应用程序",
        "files|edit <文件>|paint|about   打开窗口",
        "snake|mines|2048  玩游戏",
        "lang [zh|en]      切换界面语言",
        "reboot            重启 (8042 复位)",
        "poweroff          ACPI S5 关机",
    };
    const char *const *help = help_en;
    int n = (int)ARRAY_LEN(help_en);

    if (i18n_lang() == LANG_ZH) {
        help = help_zh;
        n = (int)ARRAY_LEN(help_zh);
    }
    for (int i = 0; i < n; i++)
        term_printf("%s\n", help[i]);
}

static void cmd_lang(const char *arg)
{
    int l;

    if (!arg || !*arg || (l = i18n_parse(arg)) < 0) {
        term_printf(tr(S_LANG_USAGE), i18n_lang_name());
        return;
    }
    i18n_set_lang(l);
    term_printf(tr(S_LANG_SET), i18n_name(l));
    gui_dirty();
}

static void cmd_gfx(void)
{
    term_printf(tr(S_GFX_DISPLAY), gfx_width(), gfx_height(), (int)gfx_bpp());
    term_printf(tr(S_GFX_FB), BOOTINFO->fb_addr, BOOTINFO->fb_pitch);
}

static void install_package(const char *path)
{
    int r;

    if (!fs_exists(path)) {
        term_printf(tr(S_INSTALL_NOPKG), path);
        return;
    }
    term_printf(tr(S_INSTALL_OPEN), path);
    gui_dirty();
    r = pkg_run(path);
    if (r == 0)
        term_puts(tr(S_INSTALL_DONE));
    else
        term_printf(tr(S_INSTALL_FAIL), r);
}

static void cmd_install(const char *arg)
{
    char path[256];

    if (!arg || !*arg) {
        term_puts(tr(S_USAGE_INSTALL));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    install_package(path);
}

void term_install_package(const char *path)
{
    install_package(path);
}

static void cmd_pkginfo(const char *arg)
{
    char path[256];
    const u8 *data;
    int len;
    const lst_header_t *h;
    int i;

    if (!arg || !*arg) {
        term_puts(tr(S_USAGE_PKGINFO));
        return;
    }
    fs_path(path, sizeof(path), cwd, arg);
    len = fs_read(path, file_buf, sizeof(file_buf) - 1);
    if (len < 0) {
        term_printf(tr(S_PKGI_NOFILE), path);
        return;
    }
    data = (const u8 *)file_buf;
    if (!lst_validate(data, (u32)len)) {
        term_printf(tr(S_PKGI_INVALID), path);
        return;
    }
    h = lst_header(data);
    term_printf(tr(S_PKGI_NAME), h->name);
    term_printf(tr(S_PKGI_VENDOR), h->vendor);
    term_printf(tr(S_PKGI_DESC), h->description);
    term_printf(tr(S_PKGI_ENTRIES), h->count, h->installer);
    for (i = 0; i < (int)h->count; i++) {
        const lst_entry_t *e = lst_entry(data, (u32)len, (u32)i);
        if (!e)
            continue;
        term_printf("  %-20s %7d bytes%s\n", e->name, e->size,
                    (u32)i == h->installer ? tr(S_PKGI_INSTALLER) : "");
    }
    term_puts(tr(S_PKGI_HINT));
}

static u8 *exec_scratch;

static void run_program_args(const char *path, const char *params);

static void cmd_run(const char *arg)
{
    char path[256];
    char prog[256];
    char params[256];
    const char *sp;

    if (!arg || !*arg) {
        term_puts(tr(S_USAGE_RUN));
        return;
    }

    /* "run <program> [args...]" - everything after the first token is handed
     * to the program, so "run /APPS/IMGVIEW.APP /PIC.BMP" can load a file. */
    sp = arg;
    while (*sp && *sp != ' ')
        sp++;
    {
        u32 n = (u32)(sp - arg);

        if (n >= sizeof(prog))
            n = sizeof(prog) - 1;
        memcpy(prog, arg, n);
        prog[n] = 0;
        while (*sp == ' ')
            sp++;
        strncpy(params, sp, sizeof(params) - 1);
        params[sizeof(params) - 1] = 0;
    }

    fs_path(path, sizeof(path), cwd, prog);
    if (!fs_exists(path)) {
        char alt[300];
        snprintf(alt, sizeof(alt), "%s.APP", path);
        if (fs_exists(alt))
            strcpy(path, alt);
    }
    run_program_args(path, params);
}

static void run_program(const char *path)
{
    run_program_args(path, NULL);
}

void term_run_program(const char *path)
{
    run_program(path);
}

void term_say(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (char *p = buf; *p; p++)
        term_putc(*p);
    gui_dirty();
}

static void run_program_args(const char *path, const char *params)
{
    int len;
    int r;

    if (!exec_scratch) {
        exec_scratch = (u8 *)kmalloc(256 * 1024);
        if (!exec_scratch) {
            term_puts(tr(S_RUN_NOMEM));
            return;
        }
    }
    len = fs_read(path, exec_scratch, 256 * 1024);
    if (len < 0) {
        term_printf(tr(S_RUN_NOFILE), path);
        return;
    }
    term_printf(tr(S_RUN_START), path, len);
    gui_dirty();
    r = user_exec_image(exec_scratch, (u32)len, path, params);
    term_printf(tr(S_RUN_EXIT), r);
}

static void cmd_memtest(const char *arg)
{
    u32 mb = (arg && *arg) ? (u32)atoi(arg) : 16;
    u32 bytes, off;
    u8 *p;
    int ok = 1;

    if (mb < 1)
        mb = 1;
    if (mb > 900)
        mb = 900;
    bytes = mb * 1024u * 1024u;

    term_printf(tr(S_MEMTEST_ALLOC), mb, bytes);
    gui_dirty();
    p = (u8 *)kmalloc(bytes);
    if (!p) {
        term_printf(tr(S_MEMTEST_NOALLOC), heap_used() / 1024,
                    heap_total() / (1024 * 1024));
        return;
    }

    for (off = 0; off < bytes; off += 4096)
        p[off] = (u8)(off >> 12);
    for (off = 0; off < bytes; off += 4096) {
        if (p[off] != (u8)(off >> 12)) {
            term_printf(tr(S_MEMTEST_MISMATCH), off);
            ok = 0;
            break;
        }
    }

    term_printf(tr(S_MEMTEST_LAYOUT), heap_base(), (u32)p,
                ((u32)p - heap_base()) / (1024 * 1024));
    term_printf(tr(S_MEMTEST_VERIFY), ok ? tr(S_PASS) : tr(S_FAILED));
    term_printf(tr(S_MEMTEST_HEAP), heap_used() / 1024, heap_total() / (1024 * 1024));
}

static void cmd_acpi(void)
{
    const acpi_info_t *a = acpi_info();
    void *tbl;
    u32 len;

    if (!a->present) {
        term_puts(tr(S_ACPI_NONE));
        return;
    }
    term_printf(tr(S_ACPI_REV), a->revision, a->revision >= 2 ? "XSDT" : "RSDT");
    term_printf(tr(S_ACPI_OEM), a->oem_id, a->oem_table_id);
    term_printf(tr(S_ACPI_TABLES), a->table_count);
    term_printf(tr(S_ACPI_RSDT), (u32)a->rsdt_addr);
    if (a->xsdt_addr)
        term_printf(tr(S_ACPI_XSDT), (u32)a->xsdt_addr);
    term_printf(tr(S_ACPI_DSDT), a->dsdt_addr);
    term_printf(tr(S_ACPI_CPUS), a->cpus, a->lapic_addr, a->ioapics);
    term_printf(tr(S_ACPI_PM1), a->pm1a_cnt, a->pm1b_cnt);

    if (acpi_find_table("FACS", &tbl, &len) == 0)
        term_printf(tr(S_ACPI_FACS), (u32)tbl, len);
    term_puts(tr(S_ACPI_S5));
}

static void cmd_ifconfig(void)
{
    char ip[16], mask[16], gw[16], dns[16], srv[16];
    const net_cfg_t *c = net_cfg();

    if (!e1000_present()) {
        term_puts(tr(S_NET_NONE));
        return;
    }
    net_ip_to_str(c->ip, ip, sizeof(ip));
    net_ip_to_str(c->mask, mask, sizeof(mask));
    net_ip_to_str(c->gw, gw, sizeof(gw));
    net_ip_to_str(c->dns, dns, sizeof(dns));
    net_ip_to_str(c->dhcp_server, srv, sizeof(srv));
    term_printf(tr(S_NET_IFCONFIG), e1000_name(), ip, mask, gw, dns, srv);
}

static void cmd_dhcp(void)
{
    char ip[16], mask[16], dns[16], srv[16];

    if (!e1000_present()) {
        term_puts(tr(S_NET_NONE));
        return;
    }
    term_puts(tr(S_NET_PROBING));
    if (net_dhcp_discover(4000) != 0) {
        term_printf(tr(S_NET_DHCP_FAIL), 4000);
        return;
    }
    {
        const net_cfg_t *c = net_cfg();
        net_ip_to_str(c->ip, ip, sizeof(ip));
        net_ip_to_str(c->mask, mask, sizeof(mask));
        net_ip_to_str(c->dns, dns, sizeof(dns));
        net_ip_to_str(c->dhcp_server, srv, sizeof(srv));
        term_printf(tr(S_NET_DHCP_OK), ip, mask, dns, srv);
    }
}

static void cmd_ping(const char *arg)
{
    u8 ip[4];
    u32 rtt = 0;
    int r;

    if (!e1000_present()) {
        term_puts(tr(S_NET_NONE));
        return;
    }
    if (!arg || net_str_to_ip(arg, ip) != 0) {
        term_puts(tr(S_NET_PING_BADIP));
        return;
    }
    r = net_ping(ip, 3000, &rtt);
    if (r == 0)
        term_printf(tr(S_NET_PING_OK), arg, rtt);
    else if (r == -4)
        term_printf(tr(S_NET_PING_NOGW), arg);
    else if (r == -2)
        term_printf(tr(S_NET_PING_RES), arg);
    else
        term_printf(tr(S_NET_PING_TMO), arg);
}

static void cmd_nslookup(const char *arg)
{
    u8 ip[4];
    char out[16];
    int r;

    if (!e1000_present()) {
        term_puts(tr(S_NET_NONE));
        return;
    }
    if (!net_cfg()->dns[0]) {
        term_puts(tr(S_NET_DNS_NOIP));
        return;
    }
    if (!arg || !*arg) {
        term_puts(tr(S_NET_PING_BADIP));
        return;
    }
    r = net_dns_resolve(arg, ip, 3000);
    if (r == 0) {
        net_ip_to_str(ip, out, sizeof(out));
        term_printf(tr(S_NET_DNS_OK), arg, out);
    } else {
        term_printf(tr(S_NET_DNS_FAIL), arg, r);
    }
}

static void cmd_audio(void)
{
    if (!hda_present()) {
        term_puts(tr(S_AUDIO_NONE));
        return;
    }
    term_printf(tr(S_AUDIO_INFO), hda_info()->name, hda_info()->codec_addr,
                (int)hda_info()->gain_steps);
}

static void cmd_beep(const char *arg)
{
    int freq = 880, ms = 250;

    if (!hda_present()) {
        term_puts(tr(S_AUDIO_NONE));
        return;
    }
    if (arg && *arg) {
        freq = atoi(arg);
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
        if (*arg)
            ms = atoi(arg);
    }
    if (freq < 20)
        freq = 20;
    if (freq > 8000)
        freq = 8000;
    if (ms < 10)
        ms = 10;
    if (ms > 3000)
        ms = 3000;
    {
        int r = hda_beep(freq, ms, 70);
        if (r != 0)
            term_printf(tr(S_AUDIO_PLAYERR), r);
        else
            term_printf(tr(S_AUDIO_BEEP), freq, ms);
    }
}

/* a tiny four note tune: E5 C5 D5 G4 (the "arrival" jingle) */
static void cmd_music(void)
{
    static const int notes[] = { 659, 523, 587, 392 };

    if (!hda_present()) {
        term_puts(tr(S_AUDIO_NONE));
        return;
    }
    term_puts(tr(S_AUDIO_MUSIC));
    for (unsigned i = 0; i < sizeof(notes) / sizeof(notes[0]); i++)
        hda_beep(notes[i], 220, 65);
}

static void cmd_dev(void)
{
    int n = blk_count();

    term_printf("%-5s %-7s %10s %8s  %s\n", tr(S_DEV_NAME), tr(S_DEV_TYPE),
                tr(S_DEV_SECTORS), tr(S_DEV_SIZE), tr(S_DEV_MODEL));
    for (int i = 0; i < n; i++) {
        const blk_info_t *bi = blk_info(i);
        char sz[16];
        u32 kb = bi->sectors * bi->sector_size / 1024;
        if (kb >= 1024)
            snprintf(sz, sizeof(sz), "%d MiB", kb / 1024);
        else
            snprintf(sz, sizeof(sz), "%d KiB", kb);
        term_printf("%-5s %-7s %10d %8s  %s%s\n", bi->name, blk_type_name(bi->type),
                    bi->sectors, sz, bi->model, bi->writable ? "" : " (ro)");
    }
    if (n == 0)
        term_puts(tr(S_DEV_NONE));
}

static void cmd_mounts(void)
{
    fs_mountinfo_t mi[8];
    int n = fs_mounts(mi, 8);

    term_printf("%-14s %-5s %-8s %10s %10s  %s\n", tr(S_MOUNTS_POINT),
                tr(S_MOUNTS_DEV), tr(S_MOUNTS_TYPE), tr(S_MOUNTS_TOTAL),
                tr(S_MOUNTS_FREE), tr(S_MOUNTS_LABEL));
    for (int i = 0; i < n; i++)
        term_printf("%-14s %-5s %-8s %9dK %9dK  %s%s\n", mi[i].point, mi[i].dev,
                    fs_type_name(mi[i].type), mi[i].total_kb, mi[i].free_kb,
                    mi[i].label, mi[i].writable ? "" : " (ro)");
}

static void cmd_mount(const char *dev, const char *point)
{
    int r;

    if (!dev || !point) {
        term_puts(tr(S_USAGE_MOUNT));
        return;
    }
    r = fs_mount(dev, point);
    if (r == 0)
        term_printf(tr(S_MOUNT_OK), dev, point);
    else if (r == -2)
        term_printf(tr(S_MOUNT_NODEV), dev);
    else if (r == -3)
        term_puts(tr(S_MOUNT_ABS));
    else if (r == -4)
        term_printf(tr(S_MOUNT_BUSY), point);
    else if (r == -6)
        term_printf(tr(S_MOUNT_NOFS), dev);
    else if (r == -8)
        term_printf(tr(S_MOUNT_ALREADY), dev);
    else
        term_printf(tr(S_MOUNT_FAIL), r);
}

static void cmd_umount(const char *point)
{
    int r;
    if (!point || !*point) {
        term_puts(tr(S_USAGE_UMOUNT));
        return;
    }
    r = fs_umount(point);
    if (r == 0)
        term_printf(tr(S_UMOUNT_OK), point);
    else if (r == -2)
        term_puts(tr(S_UMOUNT_ROOT));
    else
        term_printf(tr(S_UMOUNT_NOMOUNT), point);
}

static void cmd_mkfs(const char *dev)
{
    int r;

    if (!dev || !*dev) {
        term_puts(tr(S_USAGE_MKFS));
        return;
    }
    term_printf(tr(S_MKFS_WARN), dev);
    r = fs_format(dev, "aptos");
    if (r == 0)
        term_printf(tr(S_MKFS_OK), dev);
    else if (r == -2)
        term_printf(tr(S_MKFS_NODEV), dev);
    else if (r == -3)
        term_printf(tr(S_MKFS_RO), dev);
    else if (r == -4)
        term_printf(tr(S_MKFS_UNMOUNT), dev);
    else
        term_printf(tr(S_MKFS_FAIL), r);
}

static const struct {
    const char *name;
    const app_t *app;
} app_table[] = {
    { "terminal", &app_terminal },
    { "files",    &app_files    },
    { "setup",    &app_setup    },
    { "editor",   &app_editor   },
    { "paint",    &app_paint    },
    { "about",    &app_about    },
    { "snake",    &app_snake    },
    { "mines",    &app_mines    },
    { "2048",     &app_2048     },
};

static void cmd_apps(void)
{
    term_puts(tr(S_TERM_APPS));
    for (unsigned i = 0; i < ARRAY_LEN(app_table); i++)
        term_printf(" %s", app_table[i].name);
    term_puts("\n");
}

static const app_t *find_app(const char *name)
{
    for (unsigned i = 0; i < ARRAY_LEN(app_table); i++)
        if (strcasecmp(app_table[i].name, name) == 0)
            return app_table[i].app;
    return NULL;
}

static void run_command(char *line)
{
    while (*line == ' ')
        line++;
    if (!*line)
        return;

    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ')
            arg++;
    }

    if (strcasecmp(line, "help") == 0) {
        cmd_help();
    } else if (strcasecmp(line, "clear") == 0) {
        memset(term.lines, 0, sizeof(term.lines));
        term.cx = term.cy = 0;
    } else if (strcasecmp(line, "echo") == 0) {
        term_printf("%s\n", arg ? arg : "");
    } else if (strcasecmp(line, "ls") == 0 || strcasecmp(line, "dir") == 0) {
        cmd_ls(arg);
    } else if (strcasecmp(line, "cd") == 0) {
        cmd_cd(arg);
    } else if (strcasecmp(line, "pwd") == 0) {
        cmd_pwd();
    } else if (strcasecmp(line, "mkdir") == 0 || strcasecmp(line, "md") == 0) {
        cmd_mkdir(arg);
    } else if (strcasecmp(line, "cp") == 0 || strcasecmp(line, "copy") == 0) {
        char *second = arg ? strchr(arg, ' ') : NULL;
        if (second) {
            *second++ = 0;
            while (*second == ' ')
                second++;
        }
        cmd_cp(arg, second);
    } else if (strcasecmp(line, "mv") == 0 || strcasecmp(line, "ren") == 0) {
        char *second = arg ? strchr(arg, ' ') : NULL;
        if (second) {
            *second++ = 0;
            while (*second == ' ')
                second++;
        }
        cmd_mv(arg, second);
    } else if (strcasecmp(line, "cat") == 0 || strcasecmp(line, "type") == 0) {
        cmd_cat(arg);
    } else if (strcasecmp(line, "write") == 0) {
        char *text = arg ? strchr(arg, ' ') : NULL;
        if (text) {
            *text++ = 0;
            cmd_write(arg, text);
        } else {
            cmd_write(arg, "");
        }
    } else if (strcasecmp(line, "rm") == 0 || strcasecmp(line, "del") == 0) {
        cmd_rm(arg);
    } else if (strcasecmp(line, "df") == 0) {
        fs_stat_t st;
        fs_stat(&st);
        extern int g_term_textmode;
        term_printf(tr(S_TERM_DF), fs_volume_label(), st.total_kb, st.free_kb,
                    st.files, st.dirs);
    } else if (strcasecmp(line, "mem") == 0) {
        term_printf(tr(S_TERM_MEM), mem_total_kb() / 1024, heap_used() / 1024,
                    heap_total() / 1024);
    } else if (strcasecmp(line, "lang") == 0) {
        cmd_lang(arg);
    } else if (strcasecmp(line, "language") == 0) {
        cmd_lang(arg);
    } else if (strcasecmp(line, "dmesg") == 0) {
        cmd_dmesg();
    } else if (strcasecmp(line, "date") == 0) {
        rtc_time_t t;
        rtc_read(&t);
        term_printf("%04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day,
                    t.hour, t.minute, t.second);
    } else if (strcasecmp(line, "time") == 0) {
        rtc_time_t t;
        rtc_read(&t);
        term_printf("%02d:%02d:%02d\n", t.hour, t.minute, t.second);
    } else if (strcasecmp(line, "uptime") == 0) {
        term_printf(tr(S_TERM_UPTIME), timer_uptime_ms() / 1000,
                    timer_uptime_ms() % 1000, timer_ticks(), timer_hz());
    } else if (strcasecmp(line, "ver") == 0) {
        term_puts(tr(S_TERM_VER));
    } else if (strcasecmp(line, "ifconfig") == 0 || strcasecmp(line, "ip") == 0) {
        cmd_ifconfig();
    } else if (strcasecmp(line, "dhcp") == 0) {
        cmd_dhcp();
    } else if (strcasecmp(line, "ping") == 0) {
        cmd_ping(arg);
    } else if (strcasecmp(line, "nslookup") == 0) {
        cmd_nslookup(arg);
    } else if (strcasecmp(line, "audio") == 0) {
        cmd_audio();
    } else if (strcasecmp(line, "beep") == 0) {
        cmd_beep(arg);
    } else if (strcasecmp(line, "music") == 0) {
        cmd_music();
    } else if (strcasecmp(line, "gfx") == 0) {
        cmd_gfx();
    } else if (strcasecmp(line, "install") == 0) {
        cmd_install(arg);
    } else if (strcasecmp(line, "pkginfo") == 0) {
        cmd_pkginfo(arg);
    } else if (strcasecmp(line, "run") == 0) {
        cmd_run(arg);
    } else if (strcasecmp(line, "memtest") == 0) {
        cmd_memtest(arg);
    } else if (strcasecmp(line, "acpi") == 0) {
        cmd_acpi();
    } else if (strcasecmp(line, "smp") == 0) {
        /* SMP: 显示各 CPU 状态与心跳计数 */
        {
            extern int  smp_cpu_count(void);
            extern int  smp_cpu_alive(int i);
            extern u32  smp_cpu_apic(int i);
            extern u32  smp_cpu_hb(int i);
            extern int  smp_max_cpus(void);
            int n = smp_cpu_count();
            term_printf("cpus: %d (BSP + %d AP)\n", n, n - 1);
            for (int i = 0; i < n; i++)
                term_printf(" cpu%d  apic=%u  alive=%d  heartbeat=%u\n", i,
                            smp_cpu_apic(i), smp_cpu_alive(i), smp_cpu_hb(i));
        }
    } else if (strcasecmp(line, "disk") == 0 && 0) {
        cmd_dev();
    } else if (strcasecmp(line, "dev") == 0 || strcasecmp(line, "disk") == 0) {
        cmd_dev();
    } else if (strcasecmp(line, "mounts") == 0) {
        cmd_mounts();
    } else if (strcasecmp(line, "mount") == 0) {
        char *second = arg ? strchr(arg, ' ') : NULL;
        if (second) {
            *second++ = 0;
            while (*second == ' ')
                second++;
        }
        cmd_mount(arg, second);
    } else if (strcasecmp(line, "umount") == 0 || strcasecmp(line, "unmount") == 0) {
        cmd_umount(arg);
    } else if (strcasecmp(line, "mkfs") == 0 || strcasecmp(line, "format") == 0) {
        cmd_mkfs(arg);
    } else if (strcasecmp(line, "apps") == 0) {
        cmd_apps();
    } else if (strcasecmp(line, "reboot") == 0) {
        term_puts(tr(S_TERM_REBOOT));
        outb(0x64, 0xFE);
    } else if (strcasecmp(line, "poweroff") == 0 || strcasecmp(line, "halt") == 0 ||
               strcasecmp(line, "shutdown") == 0) {
        term_puts(tr(S_TERM_POWEROFF));
        gui_dirty();
        acpi_shutdown();
        cli();
        for (;;)
            hlt();
    } else if (strcasecmp(line, "edit") == 0) {
        char path[256];
        if (arg && *arg) {
            fs_path(path, sizeof(path), cwd, arg);
            app_editor_load(path);
        } else {
            app_editor_load("/UNTITLED.TXT");
        }
    } else {
        const app_t *a = find_app(line);
        if (a) {
            gui_open(a);
            term_printf(tr(S_TERM_OPENED), line);
        } else {
            /* 不是内核内置窗口程序：去 /APPS 里找同名用户态可执行文件。
             * 桌面包等可安装组件就是这样"解耦"进来的——没装包时这里
             * 找不到文件，自然落到 command not found。参数原样透传，
             * 让 "desk -o start" 之类能拿到 -o start。 */
            char path[300];
            int found = 0;

            snprintf(path, sizeof(path), "/APPS/%s.APP", line);
            if (!fs_exists(path)) {
                snprintf(path, sizeof(path), "/APPS/%s", line);
                if (!fs_exists(path))
                    found = 0;
                else
                    found = 1;
            } else {
                found = 1;
            }

            if (found) {
                run_program_args(path, arg && *arg ? arg : NULL);
                term_printf(tr(S_TERM_OPENED), line);
            } else {
                term_printf(tr(S_TERM_NOTFOUND), line);
            }
        }
    }
}

static void term_draw(window_t *w)
{
    int y = w->cy;
    for (int i = 0; i < TERM_ROWS; i++) {
        if (term.lines[i][0])
            gfx_text(w->cx + 2, y + 4, term.lines[i], RGB(210, 225, 200), 1);
        y += TERM_LINE_H;
    }

    int iy = w->cy + term.cy * TERM_LINE_H;
    if (iy + 14 < w->cy + w->ch) {
        gfx_text(w->cx + 2 + 7 * 8, iy + 4, term.inbuf, RGB(240, 240, 240), 1);
        int caret = w->cx + 2 + 7 * 8 + term.inlen * 8;
        gfx_fill_rect(caret, iy + 1, 7, 13, RGB(120, 200, 120));
    }
}

static void term_history(int dir)
{
    if (term.hist_count == 0)
        return;
    if (dir < 0) {
        if (term.hist_pos > 0)
            term.hist_pos--;
    } else {
        if (term.hist_pos < term.hist_count - 1)
            term.hist_pos++;
        else {
            term.hist_pos = term.hist_count;
            term.inbuf[0] = 0;
            term.inlen = 0;
            return;
        }
    }
    strncpy(term.inbuf, term.hist[term.hist_pos], TERM_COLS);
    term.inlen = (int)strlen(term.inbuf);
}

static void term_key(window_t *w, int key, u32 mods)
{
    (void)w;
    (void)mods;

    if (key == KEY_UP) {
        term_history(-1);
    } else if (key == KEY_DOWN) {
        term_history(1);
    } else if (key == '\n') {
        char cmd[TERM_COLS + 1];

        term.inbuf[term.inlen] = 0;
        strncpy(cmd, term.inbuf, TERM_COLS);
        cmd[TERM_COLS] = 0;
        term.inlen = 0;
        term.inbuf[0] = 0;

        term_puts(cmd);
        term_putc('\n');

        if (cmd[0]) {
            if (term.hist_count < HIST_MAX) {
                strcpy(term.hist[term.hist_count++], cmd);
            } else {
                for (int i = 1; i < HIST_MAX; i++)
                    strcpy(term.hist[i - 1], term.hist[i]);
                strcpy(term.hist[HIST_MAX - 1], cmd);
            }
            term.hist_pos = term.hist_count;
            run_command(cmd);
        }
        if (term.cx != 0)
            term_putc('\n');
        term_prompt();
    } else if (key == '\b') {
        if (term.inlen > 0)
            term.inbuf[--term.inlen] = 0;
    } else if (key >= 32 && key < 127) {
        if (term.inlen < TERM_COLS - 1) {
            term.inbuf[term.inlen++] = (char)key;
            term.inbuf[term.inlen] = 0;
        }
    }
    gui_dirty();
}

/* Feed a complete command line through the very same path a typed line
 * takes (echo, history, run_command, prompt).  Used by automated tests
 * that cannot inject keystrokes into the GUI. */

void term_run_line(const char *line)
{
    int i;

    if (win && win != gui_focused())
        gui_open(&app_terminal);
    for (i = 0; line[i] && term.inlen < TERM_COLS - 1; i++) {
        term.inbuf[term.inlen++] = line[i];
        term.inbuf[term.inlen] = 0;
        term_putc(line[i]);
    }
    term_key(win, '\n', 0);
}

void term_external_write(const char *buf, u32 len)
{
    if (win && win != gui_focused())
        gui_open(&app_terminal);

    term_write(buf, (int)len);
    gui_dirty();
}

static void term_mouse(window_t *w, int x, int y, int buttons, int event)
{
    (void)w;
    (void)x;
    (void)y;
    (void)buttons;
    (void)event;
}

static void term_init(window_t *w)
{
    (void)w;
    memset(&term, 0, sizeof(term));
    win = w;
    strcpy(cwd, "/");
    term_puts(tr(S_TERM_WELCOME));
    term_putc('\n');
    term_puts(tr(S_TERM_MOUNTED));
    term_putc('\n');
    term_putc('\n');
    term_prompt();
}

static void term_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_terminal = {
    .title  = "Terminal",
    .title_id = S_APP_TERMINAL,
    .width  = TERM_W,
    .height = TERM_H,
    .init   = term_init,
    .draw   = term_draw,
    .key    = term_key,
    .mouse  = term_mouse,
    .close  = term_close,
};

/* 文本控制台入口 */
void term_run_command_line(char *line);
void term_run_command_line(char *line) { run_command(line); }
