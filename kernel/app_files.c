#include "gui.h"
#include "i18n.h"
#include "util.h"
#include "fs.h"
#include "blkdev.h"
#include "keyboard.h"
#include "util.h"

#define FS_TYPE_NONE 0

#define FL_W 430
#define FL_H 320
#define FL_TOOLBAR 22
#define FL_ROW_H 18

static window_t *win;
static int scroll;
static int count;
static char cwd[128] = "/";

/* 「此电脑」视图：根层级列出全部挂载点（盘符） */
static fs_mountinfo_t drives[FS_MAX_MOUNTS];
static int ndrives;
static int sel_drive;

static void drives_refresh(void)
{
    ndrives = fs_mounts(drives, FS_MAX_MOUNTS);
}

static int has_parent(void)
{
    return strcmp(cwd, "/") != 0;
}

static int row_count(void)
{
    if (strcmp(cwd, "/") == 0)
        return ndrives;
    return count + (has_parent() ? 1 : 0);
}

static void enter(const char *path)
{
    strncpy(cwd, path, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
    scroll = 0;
    count = fs_count(cwd);
    gui_dirty();
}

void app_files_open_at(const char *path)
{
    enter(path);
}

static void refresh(void)
{
    if (strcmp(cwd, "/") == 0)
        drives_refresh();
    count = fs_count(cwd);
    if (scroll > row_count() - 1)
        scroll = 0;
    if (scroll < 0)
        scroll = 0;
    gui_dirty();
}

static int rows_visible(window_t *w)
{
    int r = (w->ch - FL_TOOLBAR - 14) / FL_ROW_H;
    return r < 1 ? 1 : r;
}

static void fl_draw(window_t *w)
{
    gfx_fill_rect(w->cx, w->cy, w->cw, FL_TOOLBAR, RGB(42, 47, 58));
    ui_button(w->cx + 4, w->cy + 3, 52, 16, tr(S_UI_UP), 0);
    ui_button(w->cx + 60, w->cy + 3, 56, 16, tr(S_UI_REFRESH), 0);

    fs_stat_t st;
    fs_stat(&st);
    char info[96];
    snprintf(info, sizeof(info), tr(S_FILES_INFO), st.files, st.dirs, st.free_kb);
    gfx_text(w->cx + 122, w->cy + 7, info, RGB(180, 190, 206), 1);

    gfx_fill_rect(w->cx, w->cy + FL_TOOLBAR, w->cw, 14, RGB(34, 38, 47));
    gfx_text(w->cx + 6, w->cy + FL_TOOLBAR + 3, cwd, RGB(120, 200, 255), 1);

    int rows = rows_visible(w);
    int y = w->cy + FL_TOOLBAR + 14;
    int total = row_count();

    if (strcmp(cwd, "/") == 0) {
        /* —— 「此电脑」：磁盘盘符视图 —— */
        char cap[64];
        snprintf(cap, sizeof(cap), tr(S_MYPC_DRIVES), ndrives);
        gfx_text(w->cx + 6, y + 4, cap, RGB(150, 160, 176), 1);
        y += FL_ROW_H;
        for (int r = 0; r < rows - 1; r++) {
            int idx = scroll + r;
            if (idx >= ndrives)
                break;
            if (idx == sel_drive)
                gfx_fill_rect(w->cx + 1, y - 1, w->cw - 2, FL_ROW_H,
                              RGB(50, 66, 100));
            static const char letters[] = "CDEFGH";
            char label[96];
            const fs_mountinfo_t *m = &drives[idx];
            char typ[12];
            fs_type_name_of(m->type, typ, sizeof(typ));
            char used[48];
            snprintf(used, sizeof(used), "%s: %s [%s%s]", typ,
                     m->writable ? tr(S_MYPC_RW) : tr(S_MYPC_RO),
                     m->label, "");
            if (m->label[0] == 0)
                snprintf(used, sizeof(used), "%s: %s", typ,
                         m->writable ? tr(S_MYPC_RW) : tr(S_MYPC_RO));
            snprintf(label, sizeof(label), "%c: %-10s %-20s %u/%u MB",
                     letters[idx % 6], m->point, used,
                     m->total_kb / 1024, m->free_kb / 1024);
            gfx_text(w->cx + 8, y + 4, label,
                     m->type == FS_TYPE_MINIFS ? RGB(140, 220, 140)
                                               : RGB(120, 200, 255), 1);
            y += FL_ROW_H;
        }
        if (ndrives == 0)
            gfx_text(w->cx + 10, y + 4, tr(S_MYPC_FSNONE),
                     RGB(150, 160, 176), 1);
        gfx_text(w->cx + 4, w->cy + w->ch - 13, tr(S_FILES_HINT),
                 RGB(120, 130, 146), 1);
        return;
    }

    for (int r = 0; r < rows; r++) {
        int idx = scroll + r;
        if (idx >= total)
            break;

        if (r & 1)
            gfx_fill_rect(w->cx + 1, y - 1, w->cw - 2, FL_ROW_H, RGB(34, 38, 47));

        if (has_parent() && idx == 0) {
            gfx_text(w->cx + 6, y + 4, tr(S_FILES_PARENT), RGB(150, 200, 255), 1);
            gfx_text(w->cx + w->cw - gfx_text_w(tr(S_FILES_UP), 1) - 8, y + 4, tr(S_FILES_UP),
                     RGB(120, 130, 146), 1);
            y += FL_ROW_H;
            continue;
        }

        fs_dirent_t e;
        if (fs_list(cwd, idx - (has_parent() ? 1 : 0), &e) != 0)
            break;

        char label[64];
        if (e.flags & FS_FLAG_DIR) {
            snprintf(label, sizeof(label), "%-6s %s", tr(S_FILES_DIRTAG), e.name);
            gfx_text(w->cx + 6, y + 4, label, RGB(255, 214, 120), 1);
        } else {
            snprintf(label, sizeof(label), "       %s", e.name);
            gfx_text(w->cx + 6, y + 4, label, RGB(226, 232, 242), 1);
            char size[24];
            utoa(e.size, size, 10);
            strcat(size, " B");
            int sw = gfx_text_w(size, 1);
            gfx_text(w->cx + w->cw - sw - 8, y + 4, size, RGB(150, 160, 176), 1);
        }
        y += FL_ROW_H;
    }

    if (total == 0)
        gfx_text(w->cx + 10, w->cy + FL_TOOLBAR + 20, tr(S_FILES_EMPTY),
                 RGB(150, 160, 176), 1);

    gfx_text(w->cx + 4, w->cy + w->ch - 13,
             tr(S_FILES_HINT), RGB(120, 130, 146), 1);
}

static int is_package_name(const char *name)
{
    int n = (int)strlen(name);
    if (n < 5)
        return 0;
    return strcasecmp(name + n - 4, ".LST") == 0;
}

static void fl_mouse(window_t *w, int x, int y, int buttons, int event)
{
    (void)w;
    if (event != GUI_MOUSE_DOWN || !(buttons & MOUSE_LEFT))
        return;

    if (y < FL_TOOLBAR) {
        if (x >= 4 && x < 56) {
            char up[160];
            fs_parent_path(cwd, up, sizeof(up));
            enter(up);
        } else if (x >= 60 && x < 116) {
            refresh();
        }
        return;
    }

    if (y < FL_TOOLBAR + 14)
        return;

    int row = (y - FL_TOOLBAR - 14) / FL_ROW_H + scroll;
    if (row >= row_count())
        return;

    if (strcmp(cwd, "/") == 0) {
        if (row < ndrives)
            enter(drives[row].point);
        return;
    }

    if (has_parent() && row == 0) {
        char up[160];
        fs_parent_path(cwd, up, sizeof(up));
        enter(up);
        return;
    }

    fs_dirent_t e;
    if (fs_list(cwd, row - (has_parent() ? 1 : 0), &e) != 0)
        return;

    char path[256];
    snprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", e.name);

    if (e.flags & FS_FLAG_DIR) {
        enter(path);
    } else if (is_package_name(e.name)) {
        extern void term_install_package(const char *path);
        gui_open(&app_terminal);
        term_install_package(path);
        gui_dirty();
    } else {
        app_editor_load(path);
    }
}

static void fl_key(window_t *w, int key, u32 mods)
{
    (void)mods;
    int rows = rows_visible(w);
    int total = row_count();

    if (strcmp(cwd, "/") == 0) {
        if (key == KEY_DOWN && sel_drive < ndrives - 1) {
            sel_drive++;
            gui_dirty();
        } else if (key == KEY_UP && sel_drive > 0) {
            sel_drive--;
            gui_dirty();
        } else if (key == '\n' && sel_drive < ndrives) {
            enter(drives[sel_drive].point);
        }
        return;
    }

    if (key == KEY_DOWN && scroll + rows < total) {
        scroll++;
        gui_dirty();
    } else if (key == KEY_UP && scroll > 0) {
        scroll--;
        gui_dirty();
    } else if (key == KEY_PGUP) {
        scroll -= rows;
        if (scroll < 0) scroll = 0;
        gui_dirty();
    } else if (key == KEY_PGDN) {
        scroll += rows;
        if (scroll > total - 1) scroll = total - 1;
        if (scroll < 0) scroll = 0;
        gui_dirty();
    } else if (key == '\b') {
        char up[160];
        fs_parent_path(cwd, up, sizeof(up));
        enter(up);
    }
}

static void fl_init(window_t *w)
{
    (void)w;
    strcpy(cwd, "/");
    scroll = 0;
    sel_drive = 0;
    refresh();
}

static void fl_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_files = {
    .title  = "Files",
    .title_id = S_APP_FILES,
    .width  = FL_W,
    .height = FL_H,
    .init   = fl_init,
    .draw   = fl_draw,
    .key    = fl_key,
    .mouse  = fl_mouse,
    .close  = fl_close,
};
