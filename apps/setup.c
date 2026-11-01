#include <stdarg.h>
#include "aptos.h"
#include "i18n.h"

/* These must stay above the sizes tools/mkert.py produces: the snapshot holds
 * the compressed boot image plus every system file, and the boot image grows
 * with the kernel (currently ~132 KiB).  1 MiB leaves plenty of headroom. */
#define ERT_MAX (1024 * 1024)
#define BLOB_MAX (1024 * 1024)
#define FILE_MAX (256 * 1024)

#define ERT_HEADER 24
#define MIN_MATCH 3

#define WIN_W 760
#define WIN_H 520
#define HEADER_H 52
#define FOOTER_H 56
#define BTN_W 104
#define BTN_H 30
#define BTN_Y (WIN_H - BTN_H - 14)

#define C_HEAD   RGB(22, 54, 100)
#define C_HEAD2  RGB(36, 82, 142)
#define C_BODY   RGB(255, 255, 255)
#define C_FRAME  RGB(158, 168, 182)
#define C_TEXT   RGB(38, 44, 56)
#define C_DIM    RGB(118, 128, 144)
#define C_GREEN  RGB(38, 132, 62)
#define C_BLUE   RGB(38, 108, 178)
#define C_RED    RGB(190, 40, 40)
#define C_FOOT   RGB(238, 240, 245)
#define C_BTN    RGB(228, 232, 240)
#define C_BTNHOT RGB(206, 220, 240)

#define PAGE_WELCOME 0
#define PAGE_TARGET  1
#define PAGE_INSTALL 2
#define PAGE_DONE    3

static unsigned char ert[ERT_MAX];
static unsigned char blob[BLOB_MAX];
static unsigned char filebuf[FILE_MAX];

static int have_win;
static int page;
static int ert_len;
static int entry_count;
static int blob_len;
static int blob_c;
static int fs_lba;
static int pos;
static int step;
static int total_steps;
static int done_steps;
static int cur_dev;
static int disk_count;
static int min_sectors;
static int disk_fits(int i);
static char disk_name[8][16];
static int  disk_sec[8];
static char status[128];
static int  failed;
static int  installed;
static int  title_lang = -1;

static int prev_btn;
static int last_mx = -1, last_my = -1;

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static int lzss_decompress(const unsigned char *src, int slen, unsigned char *dst,
                           int dlen)
{
    int si = 0, di = 0, bits = 0;
    unsigned char flags = 0;

    while (si < slen && di < dlen) {
        if (bits == 0) {
            if (si >= slen)
                break;
            flags = src[si++];
            bits = 8;
        }
        if (flags & 1) {
            if (si >= slen)
                break;
            dst[di++] = src[si++];
        } else {
            int b0, b1, off, len, p;
            if (si + 1 >= slen)
                break;
            b0 = src[si];
            b1 = src[si + 1];
            si += 2;
            off = b0 | ((b1 & 0x0F) << 8);
            len = (b1 >> 4) + MIN_MATCH;
            p = di - off;
            if (p < 0)
                return -1;
            while (len-- > 0 && di < dlen)
                dst[di++] = dst[p++];
        }
        flags >>= 1;
        bits--;
    }
    return di;
}

static void center(int y, const char *s, int color, int scale);

static void text(int x, int y, const char *s, int color, int scale)
{
    if (have_win)
        sys_text(x, y, s, color, scale);
}

static void box(int x, int y, int w, int h, int fill, int border)
{
    if (!have_win)
        return;
    if (fill >= 0)
        sys_rect(x, y, w, h, fill);
    if (border >= 0)
        sys_frame(x, y, w, h, border);
}

static void button(int id, const char *label, int x, int y, int enabled)
{
    int mx = 0, my = 0, mb = 0, inside = 0;
    int hot = 0;
    int tw;

    sys_mouse(&mx, &my, &mb, &inside);
    if (inside && mx >= x && my >= y && mx < x + BTN_W && my < y + BTN_H)
        hot = 1;

    box(x, y, BTN_W, BTN_H, (hot && enabled) ? C_BTNHOT : C_BTN, C_FRAME);
    if (enabled)
        sys_rect(x + 1, y + 1, BTN_W - 2, 1, RGB(255, 255, 255));
    tw = sys_text_w(label, 1);
    text(x + (BTN_W - tw) / 2, y + (BTN_H - 8) / 2, label,
         enabled ? C_TEXT : C_DIM, 1);
    (void)id;
}

static void header(const char *title, const char *sub)
{
    box(0, 0, WIN_W, HEADER_H, C_HEAD, -1);
    sys_rect(0, HEADER_H - 6, WIN_W, 6, C_HEAD2);
    text(20, 10, title, RGB(255, 255, 255), 2);
    if (sub)
        text(20, 34, sub, RGB(186, 206, 232), 1);
}

static void progress_bar(int percent)
{
    int x = 60, y = 232, w = WIN_W - 120, h = 22;
    int fill = (w - 4) * percent / 100;
    char buf[16];

    box(x, y, w, h, RGB(246, 248, 250), C_FRAME);
    if (fill > 0)
        sys_rect(x + 2, y + 2, fill, h - 4, C_GREEN);
    for (int i = 1; i < 10; i++)
        sys_rect(x + i * (w / 10), y + 2, 1, h - 4, RGB(220, 226, 234));
    snprintf(buf, sizeof(buf), "%d%%", percent);
    text(WIN_W / 2 - (int)strlen(buf) * 4, y + h + 10, buf, C_DIM, 1);
}

static void checkmark(int cx, int cy)
{
    for (int i = 0; i < 24; i++)
        sys_rect(cx - 34 + i, cy + i, 6, 6, C_GREEN);
    for (int i = 0; i < 54; i++)
        sys_rect(cx - 10 + i, cy + 24 - i, 6, 6, C_GREEN);
}

static void disk_scan(void)
{
    int i, n = 0;

    for (i = 0; i < 8 && n < 8; i++) {
        char nm[16];
        int sec = 0, type = 0;
        if (sys_disk_info(i, nm, &sec, &type) != 1)
            continue;
        if (type != 1)
            continue;
        for (int k = 0; k < 16; k++)
            disk_name[n][k] = nm[k];
        disk_sec[n] = sec;
        n++;
    }
    disk_count = n;
}

static void draw_page(void)
{
    if (!have_win)
        return;
    app_lang_refresh();          /* F1 may have switched the language meanwhile */
    if (app_lang() != title_lang) {
        /* re-open the same window so its title bar follows the language */
        title_lang = app_lang();
        sys_win_open(T("aptos 2.0 Setup", "aptos 2.0 安装程序"), WIN_W, WIN_H);
    }
    sys_clear(C_BODY);

    if (page == PAGE_WELCOME) {
        header(T("Welcome to the aptos 2.0 Setup Wizard",
                 "欢迎使用 aptos 2.0 安装向导"), NULL);
        text(60, 92, T("This wizard will install aptos 2.0 on your computer.",
                       "这个向导会把 aptos 2.0 安装到你的电脑上。"), C_TEXT, 1);
        text(60, 124, T("Setup will:", "安装程序会:"), C_TEXT, 1);
        text(78, 146, T("- write the boot loader and the kernel to the hard disk",
                        "- 把引导程序和内核写到硬盘上"), C_DIM, 1);
        text(78, 164, T("- create a MINIFS02 filesystem on the selected disk",
                        "- 在选中的硬盘上建立 MINIFS02 文件系统"), C_DIM, 1);
        text(78, 182, T("- install the system files, the shell and the applications",
                        "- 安装系统文件、终端和应用程序"), C_DIM, 1);
        text(78, 200, T("- put every application on the desktop, ready to run",
                        "- 把每个应用都放到桌面上，随时可用"), C_DIM, 1);
        text(60, 240, T("To continue, click Next. To quit setup, click Cancel.",
                        "要继续请点“下一步”，要退出安装请点“取消”。"), C_TEXT, 1);
        text(60, 286, T("This disc is also a live system: if you boot it with a hard",
                        "这张光盘同时也是一个可直接运行的系统: 接上硬盘启动就能安装，"), C_DIM, 1);
        text(60, 304, T("disk attached you can install, if you boot setup from an",
                        "如果从装好的硬盘上启动安装程序的话，你会得到你应得的东西。"), C_DIM, 1);
        text(60, 322, T("installed disk you will get what you deserve.", ""), C_DIM, 1);
    } else if (page == PAGE_TARGET) {
        header(T("Select the target disk", "选择目标硬盘"),
               T("Setup lists the hard disks it found", "下面是安装程序找到的硬盘"));
        text(60, 84, T("Disk", "硬盘"), C_TEXT, 1);
        text(220, 84, T("Size", "容量"), C_TEXT, 1);
        text(360, 84, T("Type", "类型"), C_TEXT, 1);
        if (disk_count == 0) {
            text(60, 118, T("No writable hard disk found. Attach one and restart setup.",
                            "没有找到可写的硬盘。请接上一块硬盘再重新运行安装程序。"),
                 C_RED, 1);
        }
        for (int i = 0; i < disk_count; i++) {
            int y = 106 + i * 34;
            int sel = (i == cur_dev);
            int fits = disk_fits(i);
            char buf[48];
            box(52, y, WIN_W - 104, 30, sel ? RGB(220, 234, 250) : RGB(248, 250, 252),
                sel ? C_BLUE : C_FRAME);
            text(64, y + 11, disk_name[i], sel ? C_HEAD : C_TEXT, 1);
            snprintf(buf, sizeof(buf), "%d MiB", disk_sec[i] / 2048);
            text(220, y + 11, buf, C_TEXT, 1);
            text(360, y + 11, T("IDE hard disk", "IDE 硬盘"), C_DIM, 1);
            if (!fits)
                text(WIN_W - 156, y + 11, T("too small", "容量不足"), C_RED, 1);
            else if (sel)
                text(WIN_W - 116, y + 11, T("selected", "已选中"), C_GREEN, 1);
        }
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     T("aptos needs a disk of at least %d KiB (system image %d KiB).",
                       "aptos 至少需要 %d KiB 的硬盘空间 (系统镜像 %d KiB)。"),
                     min_sectors / 2, blob_len / 1024);
            text(60, WIN_H - 168, buf, C_DIM, 1);
        }
        if (disk_count > 0 && !disk_fits(cur_dev))
            text(60, WIN_H - 150,
                 T("The selected disk is too small. Pick another one.",
                   "选中的硬盘容量不足，请另选一块。"), C_RED, 1);
        else
            text(60, WIN_H - 150,
                 T("Warning: everything on the selected disk will be erased.",
                   "警告: 选中硬盘上的所有数据都会被清空。"), C_RED, 1);
        text(60, WIN_H - 126,
             T("The disk gets a fresh MINIFS02 filesystem, the boot loader goes to",
               "硬盘会被重新格式化成 MINIFS02，引导程序写到第 0 号扇区，"), C_DIM, 1);
        text(60, WIN_H - 108, T("sector 0, and the system snapshot is unpacked onto it.",
                                "系统快照也会被解包写进去。"), C_DIM, 1);
    } else if (page == PAGE_INSTALL) {
        int percent = total_steps ? (done_steps * 100 / total_steps) : 0;
        header(T("Installing aptos 2.0", "正在安装 aptos 2.0"),
               T("This may take a moment", "请稍等一会儿"));
        progress_bar(percent);
        text(60, 300, status, failed ? C_RED : C_TEXT, 1);
        if (!failed)
            text(60, 326, T("Setup is copying files, please wait.",
                            "安装程序正在复制文件，请稍候。"), C_DIM, 1);
    } else if (installed) {
        header(T("Completing the aptos 2.0 Setup Wizard",
                 "正在完成 aptos 2.0 安装向导"), NULL);
        checkmark(WIN_W / 2 - 46, 116);
        center(206, T("Setup completed successfully.", "安装成功完成。"), C_GREEN, 2);
        text(60, 256, T("aptos 2.0 is now on your hard disk. Remove the CD-ROM and",
                        "aptos 2.0 已经装到你的硬盘上了。请取出光盘并重启，"), C_TEXT, 1);
        text(60, 276, T("reboot: the machine will start from the hard disk and show",
                        "机器会从硬盘启动，然后显示带有全部应用程序的桌面。"), C_TEXT, 1);
        text(60, 296, T("the desktop with all applications ready to run.", ""), C_TEXT, 1);
        text(60, 336, T("Click Finish to reboot now.", "点“完成”立即重启。"), C_DIM, 1);
    } else {
        /* the snapshot could not be read, or the user cancelled: say why */
        header(T("Completing the aptos 2.0 Setup Wizard",
                 "正在完成 aptos 2.0 安装向导"), NULL);
        center(120, T("Setup did not finish", "安装没有完成"), C_RED, 2);
        text(60, 176, status, C_RED, 1);
        text(60, 220, T("No change was made to your hard disk.",
                        "你的硬盘没有被改动。"), C_TEXT, 1);
        text(60, 240, T("Reboot from a freshly written install CD-ROM.",
                        "请用重新刻录的安装光盘再启动一次。"), C_TEXT, 1);
        text(60, 336, T("Click Finish to reboot now.", "点“完成”立即重启。"), C_DIM, 1);
    }

    box(0, WIN_H - FOOTER_H, WIN_W, FOOTER_H, C_FOOT, -1);
    sys_rect(0, WIN_H - FOOTER_H, WIN_W, 1, C_FRAME);

    if (page == PAGE_WELCOME) {
        button(0, T("Cancel", "取消"), WIN_W - 2 * (BTN_W + 10) - 18, BTN_Y, 1);
        button(1, T("Next >", "下一步 >"), WIN_W - (BTN_W + 10) - 18, BTN_Y, 1);
    } else if (page == PAGE_TARGET) {
        button(2, T("< Back", "< 上一步"), WIN_W - 3 * (BTN_W + 10) - 18, BTN_Y, 1);
        button(3, T("Install", "安装"), WIN_W - (BTN_W + 10) - 18, BTN_Y,
               disk_count > 0 && disk_fits(cur_dev));
    } else if (page == PAGE_DONE) {
        button(4, T("Finish", "完成"), WIN_W - (BTN_W + 10) - 18, BTN_Y, 1);
    } else {
        button(5, T("Working...", "处理中..."), WIN_W - (BTN_W + 10) - 18, BTN_Y, 0);
    }
    sys_flush();
}

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    if (!have_win)
        printf("%s\n", status);
}

static int load_snapshot(void)
{
    int fd, n, total = 0;

    fd = sys_open("/SYSTEM.ERT", 0);
    if (fd < 0) {
        say(T("cannot open /SYSTEM.ERT (error %d)", "无法打开 /SYSTEM.ERT (错误 %d)"), fd);
        return -1;
    }
    while (total < ERT_MAX) {
        n = sys_read(fd, ert + total, 32768);
        if (n <= 0)
            break;
        total += n;
    }
    sys_close(fd);
    if (total < ERT_HEADER || ert[0] != 'E' || ert[1] != 'R' || ert[2] != 'T' ||
        ert[3] != '1') {
        say(T("SYSTEM.ERT is not a valid snapshot", "SYSTEM.ERT 不是有效的系统快照"));
        return -1;
    }
    return total;
}

static int snapshot_ok;

#define FS_MIN_SECTORS 2048

static int read_snapshot_header(void)
{
    ert_len = load_snapshot();
    if (ert_len <= 0)
        return -1;
    blob_len = (int)rd32(ert + 8);
    blob_c = (int)rd32(ert + 12);
    entry_count = (int)rd32(ert + 16);
    if (blob_len <= 0 || blob_len > BLOB_MAX || entry_count <= 0) {
        say(T("the system snapshot looks damaged", "系统快照似乎已经损坏"));
        return -1;
    }
    fs_lba = blob_len / 512;
    min_sectors = fs_lba + FS_MIN_SECTORS;
    return 0;
}

static int disk_fits(int i)
{
    return disk_sec[i] >= min_sectors;
}

static void install_begin(void)
{
    page = PAGE_INSTALL;
    step = 0;
    done_steps = 0;
    failed = 0;
    pos = 0;
    total_steps = 5 + entry_count;      /* +1: carry the UI language over */
    say(T("Preparing to install...", "正在准备安装..."));
    draw_page();
}

static int install_step(void)
{
    if (failed)
        return 1;

    if (step == 0) {
        say(T("Snapshot read: %d bytes, %d entries", "已读入快照: %d 字节, %d 个条目"), ert_len, entry_count);
    } else if (step == 1) {
        int got = lzss_decompress(ert + ERT_HEADER, blob_c, blob, blob_len);
        if (got != blob_len) {
            say(T("boot image decompression failed (%d of %d)", "引导镜像解压失败 (%d / %d)"), got, blob_len);
            failed = 1;
        } else {
            say(T("Boot image unpacked (%d bytes)", "引导镜像已解压 (%d 字节)"), blob_len);
        }
    } else if (step == 2) {
        int r = sys_rawwrite(disk_name[cur_dev], 0, blob, blob_len / 512);
        if (r < 0) {
            say(T("cannot write the boot image to %s (error %d)", "无法把引导镜像写入 %s (错误 %d)"), disk_name[cur_dev], r);
            failed = 1;
        } else {
            say(T("Boot loader and kernel written to sector 0", "引导程序和内核已写入第 0 号扇区"));
        }
    } else if (step == 3) {
        int r = sys_install(disk_name[cur_dev], fs_lba);
        if (r == -2 || r == -3) {
            say(T("%s is too small: the system needs %d KiB, the disk has %d KiB",
                  "%s 容量太小: 系统需要 %d KiB, 这块硬盘只有 %d KiB"),
                disk_name[cur_dev], min_sectors / 2, disk_sec[cur_dev] / 2);
            failed = 1;
        } else if (r != 0) {
            say(T("cannot create the filesystem on %s (error %d)", "无法在 %s 上建立文件系统 (错误 %d)"), disk_name[cur_dev], r);
            failed = 1;
        } else {
            say(T("MINIFS02 filesystem created at sector %d", "已在第 %d 号扇区建立 MINIFS02 文件系统"), fs_lba);
            pos = ERT_HEADER + blob_c;
        }
    } else {
        int idx = step - 4;
        int kind, nlen, rawsize, compsize;
        char name[200];
        char path[260];

        if (idx >= entry_count) {
            /* last step: remember the language the user picked on the CD */
            const char *cfg = app_lang() == APP_LANG_ZH ? "zh\n" : "en\n";
            int fd = sys_open("/TARGET/LANG.CFG", 1);

            if (fd < 0) {
                say(T("cannot write /LANG.CFG (error %d)", "无法写入 /LANG.CFG (错误 %d)"), fd);
            } else if (sys_writefd(fd, cfg, 3) != 3 || sys_close(fd) != 0) {
                say(T("cannot write /LANG.CFG", "无法写入 /LANG.CFG"));
            } else {
                say(T("Language preference saved", "语言偏好已保存"));
            }
            step++;
            done_steps++;
            return 1;
        }
        if (pos + 12 > ert_len) {
            say(T("the snapshot is truncated", "系统快照被截断了"));
            failed = 1;
            return 1;
        }
        kind = ert[pos];
        nlen = (int)rd16(ert + pos + 2);
        rawsize = (int)rd32(ert + pos + 4);
        compsize = (int)rd32(ert + pos + 8);
        pos += 12;
        if (nlen > 190 || pos + nlen > ert_len || pos + nlen + compsize > ert_len) {
            say(T("bad entry in the snapshot", "系统快照中有损坏的条目"));
            failed = 1;
            return 1;
        }
        for (int k = 0; k < nlen; k++)
            name[k] = (char)ert[pos + k];
        name[nlen] = 0;
        pos += nlen;
        snprintf(path, sizeof(path), "/TARGET/%s", name);

        if (kind == 1) {
            sys_mkdir(path);
            say(T("Creating folder %s", "正在创建目录 %s"), name);
        } else if (rawsize > FILE_MAX) {
            say(T("%s is too big for this build, skipped", "%s 对这个版本来说太大了，已跳过"), name);
        } else {
            int got = lzss_decompress(ert + pos, compsize, filebuf, rawsize);
            int fd;
            if (got != rawsize) {
                say(T("%s: decompression failed", "%s: 解压失败"), name);
                failed = 1;
            } else if ((fd = sys_open(path, 1)) < 0) {
                say(T("%s: cannot create (error %d)", "%s: 无法创建 (错误 %d)"), name, fd);
                failed = 1;
            } else if (sys_writefd(fd, filebuf, rawsize) != rawsize) {
                say(T("%s: write failed", "%s: 写入失败"), name);
                sys_close(fd);
                failed = 1;
            } else if (sys_close(fd) != 0) {
                say(T("%s: could not flush to the disk", "%s: 无法写回磁盘"), name);
                failed = 1;
            } else {
                say(T("Installing %s  (%d bytes)", "正在安装 %s  (%d 字节)"), name, rawsize);
            }
        }
        pos += compsize;
    }

    step++;
    done_steps++;
    return step > total_steps - 1;
}

static void click(int id)
{
    if (id == 0) {
        say(T("Setup cancelled.", "安装已取消。"));
        page = PAGE_DONE;
        failed = 0;
    } else if (id == 1) {
        disk_scan();
        for (int i = 0; i < disk_count; i++)
            if (disk_fits(i)) {
                cur_dev = i;
                break;
            }
        page = PAGE_TARGET;
    } else if (id == 2) {
        page = PAGE_WELCOME;
    } else if (id == 3) {
        install_begin();
    } else if (id == 4) {
        sys_reboot();
    }
}

static void handle_click(int mx, int my)
{
    if (page == PAGE_TARGET) {
        for (int i = 0; i < disk_count; i++) {
            int y = 106 + i * 34;
            if (my >= y && my < y + 30 && mx >= 52 && mx < WIN_W - 52) {
                cur_dev = i;
                return;
            }
        }
    }
    if (my < BTN_Y || my >= BTN_Y + BTN_H)
        return;
    if (page == PAGE_WELCOME) {
        if (mx >= WIN_W - 2 * (BTN_W + 10) - 18 && mx < WIN_W - (BTN_W + 10) - 18)
            click(0);
        else if (mx >= WIN_W - (BTN_W + 10) - 18)
            click(1);
    } else if (page == PAGE_TARGET) {
        if (mx >= WIN_W - 3 * (BTN_W + 10) - 18 && mx < WIN_W - 2 * (BTN_W + 10) - 18)
            click(2);
        else if (mx >= WIN_W - (BTN_W + 10) - 18 && disk_count > 0)
            click(3);
    } else if (page == PAGE_DONE) {
        if (mx >= WIN_W - (BTN_W + 10) - 18)
            click(4);
    }
}

static void big_head(int cx, int cy, int r, int color)
{
    for (int dy = -r; dy <= r; dy++) {
        int rr = 0;
        while ((rr + 1) * (rr + 1) + dy * dy <= r * r)
            rr++;
        sys_rect(cx - rr, cy + dy, rr * 2, 1, color);
    }
}

static void draw_big_face(void)
{
    int cx = WIN_W / 2;
    int cy = 290;
    int r = 168;

    big_head(cx, cy, r, RGB(252, 214, 96));
    big_head(cx, cy, r - 4, RGB(255, 226, 128));
    big_head(cx - 62, cy - 40, 40, RGB(255, 255, 255));
    big_head(cx + 62, cy - 40, 40, RGB(255, 255, 255));
    big_head(cx - 62, cy - 36, 22, RGB(40, 40, 60));
    big_head(cx + 62, cy - 36, 22, RGB(40, 40, 60));
    big_head(cx - 56, cy - 46, 7, RGB(255, 255, 255));
    big_head(cx + 68, cy - 46, 7, RGB(255, 255, 255));
    for (int i = 0; i < 46; i++) {
        sys_rect(cx - 118 + i, cy - 104 + i / 3, 8, 12, RGB(60, 40, 30));
        sys_rect(cx + 110 - i, cy - 104 + i / 3, 8, 12, RGB(60, 40, 30));
    }
    for (int i = 0; i < 150; i++) {
        int y = cy + 74 + (i * i) / 260;
        if (y > cy + 128)
            break;
        sys_rect(cx - 75 + i, y, 2, 6, RGB(120, 40, 40));
    }
    big_head(cx - 118, cy + 34, 26, RGB(250, 150, 150));
    big_head(cx + 118, cy + 34, 26, RGB(250, 150, 150));
}

static void center(int y, const char *s, int color, int scale)
{
    text((WIN_W - sys_text_w(s, scale)) / 2, y, s, color, scale);
}

static void easter_egg(void)
{
    int start;

    have_win = (sys_win_open(T("Setup", "安装程序"), WIN_W, WIN_H) == 0);
    if (have_win) {
        sys_clear(RGB(18, 24, 36));
        draw_big_face();
        center(30, T("How dare you install a system", "你竟敢在硬盘上装系统"), RGB(228, 40, 40), 3);
        center(66, T("on the hard disk.", "胆子不小。"), RGB(228, 40, 40), 3);
        center(108, T("Bold!", "大胆!"), RGB(255, 90, 60), 2);
        sys_flush();
    } else {
        printf("\n%s\n", T("How dare you install a system on the hard disk. Bold!",
                           "你竟敢在硬盘上装系统。胆子不小。"));
    }

    start = sys_ticks();
    while (sys_ticks() - start < 1000) {
        sys_sleep(20);
        if (have_win)
            sys_pump();
    }
    sys_poweroff();
}

int main(void)
{
    int drive = sys_bootmode();

    if (drive >= 0x80) {
        easter_egg();
        return 0;
    }

    title_lang = app_lang();
    have_win = (sys_win_open(T("aptos 2.0 Setup", "aptos 2.0 安装程序"), WIN_W, WIN_H) == 0);
    if (!have_win)
        printf("%s\n", T("aptos 2.0 setup - no window available, using the console",
                         "aptos 2.0 安装程序 - 没有可用窗口，改用控制台输出"));

    if (read_snapshot_header() != 0) {
        snapshot_ok = 0;
        page = PAGE_DONE;
    } else {
        snapshot_ok = 1;
        page = PAGE_WELCOME;
    }
    draw_page();

    for (;;) {
        int mx = 0, my = 0, btn = 0, inside = 0;
        int k = sys_getkey();

        if (k == 27) {
            if (page == PAGE_WELCOME)
                break;
            if (page == PAGE_TARGET)
                page = PAGE_WELCOME;
        } else if (k == '\n' || k == '\r') {
            if (page == PAGE_WELCOME)
                click(1);
            else if (page == PAGE_TARGET)
                click(3);
            else if (page == PAGE_DONE)
                click(4);
        }

        if (page == PAGE_INSTALL && !failed) {
            int finished = install_step();
            if (finished && !failed) {
                installed = 1;
                page = PAGE_DONE;
            }
            draw_page();
            sys_sleep(10);
            continue;
        }

        sys_mouse(&mx, &my, &btn, &inside);
        if ((btn & MOUSE_BTN_LEFT) && !(prev_btn & MOUSE_BTN_LEFT) && inside) {
            handle_click(mx, my);
            draw_page();
        }
        prev_btn = btn;

        if (mx != last_mx || my != last_my) {
            draw_page();
            last_mx = mx;
            last_my = my;
        }
        sys_sleep(16);
    }

    return 0;
}
