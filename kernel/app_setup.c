/*
 * aptos 2.0 安装程序 —— 分区 / 格式化 / 装系统，三步向导。
 *
 * 步骤 1：列出可写硬盘（hd*），选中目标盘。
 * 步骤 2：为新分区选文件系统（MINIFS / FAT32 / NTFS / ext4）。
 * 步骤 3：写 MBR 单分区 → 按所选类型格式化 → 自动挂载 →
 *         把系统文件复制过去（fs_install 系）。
 *
 * 纯键盘操作：↑/↓ 选择，Enter 确认，Esc 关闭。危险动作需在
 * 步骤 3 明确再按一次 Enter 才执行。
 */
#include "gui.h"
#include "i18n.h"
#include "util.h"
#include "fs.h"
#include "blkdev.h"
#include "keyboard.h"

#define SU_W 460
#define SU_H 300

/* 步骤 */
enum { SU_STEP_DISK = 0, SU_STEP_FS, SU_STEP_RUN, SU_STEPS };

/* 文件系统选项 */
static const struct {
    int  type;
    u8   ptype;          /* MBR 分区类型字节 */
    int  name_id;
    int  desc_id;
} fs_opts[] = {
    { FS_TYPE_MINIFS, 0x83, S_SETUP_FS_MINIFS, 0 },
    { FS_TYPE_FAT32,  0x0B, S_SETUP_FS_FAT32,  0 },
    { FS_TYPE_NTFS,   0x07, S_SETUP_FS_NTFS,   0 },
    { FS_TYPE_EXT4,   0x83, S_SETUP_FS_EXT4,   0 },
};
#define FS_N ((int)(sizeof(fs_opts) / sizeof(fs_opts[0])))

static window_t *su_win;
static int step;                 /* 当前步骤 */
static int sel;                  /* 列表选择 */
static int done;                 /* 步骤3：执行结果，0=未执行 */
static int result;               /* 执行返回码 */
static char devname[8];          /* 选中的整盘 */
static char partname[8];         /* 建出的分区名 */
static int  fstype;              /* 选中的文件系统类型 */
static int  executing;

static void su_refresh(void)
{
    gui_dirty();
}

/* 可写 HDD 列表（排除引导盘） */
static int disk_count(void)
{
    int n = 0;
    for (int i = 0; i < blk_count(); i++) {
        const blk_info_t *bi = blk_info(i);
        if (bi->type == BLK_HDD && bi->writable && bi->sector_size == 512)
            n++;
    }
    return n;
}

static int disk_dev(int idx)
{
    int n = 0;
    for (int i = 0; i < blk_count(); i++) {
        const blk_info_t *bi = blk_info(i);
        if (bi->type == BLK_HDD && bi->writable && bi->sector_size == 512) {
            if (n == idx)
                return i;
            n++;
        }
    }
    return -1;
}

static void su_draw(window_t *w)
{
    char line[128];

    /* 标题条 */
    ui_panel(w->cx, w->cy, w->cw, 24, RGB(52, 61, 82));
    snprintf(line, sizeof(line), tr(S_SETUP_STEP), step + 1);
    gfx_text(w->cx + 8, w->cy + 7, tr(S_APP_SETUP), RGB(255, 255, 255), 1);
    gfx_text(w->cx + w->cw - gfx_text_w(line, 1) - 8, w->cy + 7, line,
             RGB(160, 180, 210), 1);

    int y = w->cy + 34;

    if (step == SU_STEP_DISK) {
        gfx_text(w->cx + 10, y, tr(S_SETUP_PICKDISK), RGB(220, 228, 240), 1);
        y += 20;
        int n = disk_count();
        if (n == 0) {
            gfx_text(w->cx + 16, y + 4, "no writable hard disk found",
                     RGB(255, 120, 120), 1);
            return;
        }
        for (int i = 0; i < n; i++) {
            const blk_info_t *bi = blk_info(disk_dev(i));
            int active = (i == sel);
            u32 bg = active ? RGB(64, 96, 160) : RGB(38, 43, 54);
            gfx_fill_rect(w->cx + 10, y, w->cw - 20, 24, bg);
            char label[96];
            snprintf(label, sizeof(label), tr(S_SETUP_DISKINFO),
                     bi->name, bi->sectors / 2048);
            gfx_text(w->cx + 18, y + 7, label,
                     active ? RGB(255, 255, 255) : RGB(200, 210, 226), 1);
            /* 已有分区提示：读 MBR 签名 */
            u8 mbr[512];
            int has = 0;
            if (blk_read(i, 0, 1, mbr) == 0 && mbr[510] == 0x55
                && mbr[511] == 0xAA && mbr[446 + 4] != 0)
                has = 1;
            const char *tag = has ? tr(S_SETUP_HASPART) : tr(S_SETUP_EMPTY);
            gfx_text(w->cx + 120, y + 7, tag, RGB(150, 200, 255), 1);
            y += 28;
        }
        gfx_text(w->cx + 4, w->cy + w->ch - 13, tr(S_FILES_HINT),
                 RGB(120, 130, 146), 1);
    } else if (step == SU_STEP_FS) {
        gfx_text(w->cx + 10, y, tr(S_SETUP_PICKFS), RGB(220, 228, 240), 1);
        y += 20;
        for (int i = 0; i < FS_N; i++) {
            int active = (i == sel);
            u32 bg = active ? RGB(64, 96, 160) : RGB(38, 43, 54);
            gfx_fill_rect(w->cx + 10, y, w->cw - 20, 24, bg);
            gfx_text(w->cx + 18, y + 7, tr(fs_opts[i].name_id),
                     active ? RGB(255, 255, 255) : RGB(200, 210, 226), 1);
            y += 28;
        }
        y += 6;
        snprintf(line, sizeof(line), tr(S_SETUP_WARN), devname);
        gfx_text(w->cx + 10, y, line, RGB(255, 130, 110), 1);
        gfx_text(w->cx + 4, w->cy + w->ch - 13, tr(S_FILES_HINT),
                 RGB(120, 130, 146), 1);
    } else {
        /* 执行 / 结果页 */
        if (!executing && done == 0) {
            snprintf(line, sizeof(line), "%s -> %s", devname,
                     tr(fs_opts[fstype].name_id));
            gfx_text(w->cx + 10, y, line, RGB(220, 228, 240), 1);
            y += 22;
            gfx_text(w->cx + 10, y, tr(S_SETUP_WARN), RGB(255, 130, 110), 1);
            y += 26;
            ui_button(w->cx + 10, y, 150, 24, tr(S_SETUP_PARTITION), 1);
            ui_button(w->cx + 170, y, 80, 24, tr(S_SETUP_BACK), 0);
        } else if (executing) {
            gfx_text(w->cx + 10, y, tr(S_SETUP_WORKING), RGB(255, 220, 120), 1);
        } else {
            y += 6;
            if (result == 0) {
                snprintf(line, sizeof(line), tr(S_SETUP_DONE_OK), partname,
                         tr(fs_opts[fstype].name_id));
                gfx_text(w->cx + 10, y, line, RGB(140, 230, 140), 1);
                y += 20;
                gfx_text(w->cx + 10, y, tr(S_SETUP_INSTALLED),
                         RGB(200, 210, 226), 1);
            } else {
                snprintf(line, sizeof(line), tr(S_SETUP_DONE_FAIL), result);
                gfx_text(w->cx + 10, y, line, RGB(255, 120, 120), 1);
            }
            y += 30;
            ui_button(w->cx + 10, y, 80, 24, tr(S_SETUP_EXIT), 0);
        }
    }
}

/* 执行：分区 → 格式化 → 挂载 → 装系统文件 */
static void su_execute(void)
{
    char mnt[24];

    executing = 1;
    su_refresh();

    result = fs_part_create_single(devname, fs_opts[fstype].ptype,
                                   partname, (int)sizeof(partname));
    if (result == 0)
        result = fs_format_type(partname, fs_opts[fstype].type, "APTOS");
    if (result == 0) {
        /* automount 已在 format 后做？没有 —— format 只写盘。
         * 重新探测挂载： */
        extern int fs_automount_all(void);
        int before = 0;
        (void)before;
        int ndev = blk_find(partname);
        (void)ndev;
        snprintf(mnt, sizeof(mnt), "/mnt/%s", partname);
        result = fs_mount(partname, mnt);
        if (result == -6 || result == -8)
            result = 0;              /* 已挂载过也算成功 */
    }
    if (result == 0) {
        /* 装系统文件（minifs 原生分区才有意义；其余类型跳过复制，
         * 用户可自行用文件管理器往里拷）。 */
        if (fs_opts[fstype].type == FS_TYPE_MINIFS)
            result = fs_install(blk_find(partname), 0, "aptos");
    }
    executing = 0;
    done = 1;
    su_refresh();
}

static void su_key(window_t *w, int key, u32 mods)
{
    (void)mods;
    (void)w;

    if (key == KEY_F1)
        return;                          /* 语言切换留给全局 */

    if (step == SU_STEP_DISK) {
        int n = disk_count();
        if (key == KEY_DOWN && sel < n - 1) { sel++; su_refresh(); }
        else if (key == KEY_UP && sel > 0) { sel--; su_refresh(); }
        else if (key == '\n' && n > 0) {
            snprintf(devname, sizeof(devname), "%s",
                     blk_info(disk_dev(sel))->name);
            step = SU_STEP_FS;
            sel = 0;
            su_refresh();
        }
    } else if (step == SU_STEP_FS) {
        if (key == KEY_DOWN && sel < FS_N - 1) { sel++; su_refresh(); }
        else if (key == KEY_UP && sel > 0) { sel--; su_refresh(); }
        else if (key == '\n') {
            fstype = sel;
            step = SU_STEP_RUN;
            done = 0;
            executing = 0;
            su_refresh();
        } else if (key == 27) {          /* Esc */
            step = SU_STEP_DISK;
            sel = 0;
            su_refresh();
        }
    } else {
        if (done == 0 && !executing) {
            if (key == '\n') {
                su_execute();
            } else if (key == 27) {
                step = SU_STEP_FS;
                su_refresh();
            }
        } else if (done == 1 && key == 27) {
            gui_close(su_win);
        }
    }
}

static void su_init(window_t *w)
{
    (void)w;
    su_win = w;
    step = SU_STEP_DISK;
    sel = 0;
    done = 0;
    executing = 0;
    result = 0;
    devname[0] = 0;
    partname[0] = 0;
    fstype = 0;
}

static void su_close(window_t *w)
{
    (void)w;
    su_win = NULL;
}

const app_t app_setup = {
    .title  = "Setup",
    .title_id = S_APP_SETUP,
    .width  = SU_W,
    .height = SU_H,
    .init   = su_init,
    .draw   = su_draw,
    .key    = su_key,
    .close  = su_close,
};
