#include "gui.h"
#include "fs.h"
#include "i18n.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "mouse.h"
#include "rtc.h"
#include "timer.h"
#include "util.h"

#define MAX_WINDOWS 8

#define C_DESK_TOP   RGB(26, 34, 58)
#define C_DESK_BOT   RGB(10, 13, 22)
#define C_LOGO       RGB(38, 50, 78)
#define C_LOGO_SUB   RGB(30, 40, 62)
#define C_WIN_BG     RGB(30, 34, 42)
#define C_WIN_EDGE   RGB(74, 82, 96)
#define C_TITLE_A    RGB(52, 118, 210)
#define C_TITLE_B    RGB(34, 78, 150)
#define C_TITLE_UA   RGB(58, 64, 76)
#define C_TITLE_UB   RGB(46, 51, 62)
#define C_TEXT       RGB(226, 232, 242)
#define C_DIM        RGB(146, 156, 172)
#define C_TASKBAR    RGB(17, 20, 27)
#define C_TASK_EDGE  RGB(58, 68, 88)
#define C_BTN        RGB(52, 58, 70)
#define C_BTN_HOT    RGB(72, 122, 200)
#define C_BTN_EDGE   RGB(96, 106, 126)
#define C_SHADOW     RGB(6, 8, 12)

static window_t  windows[MAX_WINDOWS];
static window_t *zorder[MAX_WINDOWS];
static int       zcount;
static int       scene_dirty;
static int       start_menu_open;
static int       input_enabled = 1;
static char      clock_text[10] = "--:--:--";
static u32       clock_tick;

#define CURSOR_W 12
#define CURSOR_H 19

static const char *cursor_shape[CURSOR_H] = {
    "1...........",
    "11..........",
    "121.........",
    "1221........",
    "12221.......",
    "122221......",
    "1222221.....",
    "12222221....",
    "122222221...",
    "1222222221..",
    "12222222221.",
    "122222222221",
    "122222211111",
    "1221..1221..",
    "121...1221..",
    "11.....1221.",
    "1......1221.",
    "........1221",
    ".........11."
};

static int  cur_x = -1, cur_y = -1;
static u32  cursor_under[CURSOR_W * CURSOR_H];
static int  cursor_saved;

static window_t *focused_window;
static window_t *mouse_grab;
static int dragging;
static int drag_dx, drag_dy;

window_t *gui_focused(void) { return focused_window; }
void gui_dirty(void) { scene_dirty = 1; }

const char *app_title(const app_t *app)
{
    if (!app)
        return "";
    if (app->title_id > 0)
        return tr(app->title_id);
    return app->title ? app->title : "";
}

#define ICON_W   76
#define ICON_H   62
#define ICON_ROWS 9
#define ICON_COLS 3
#define ICON_TOP 10
#define ICON_LEFT 10
#define ICON_MAX (ICON_ROWS * ICON_COLS)

static void set_content_rect(window_t *w)
{
    w->cx = w->x + GUI_BORDER;
    w->cy = w->y + GUI_TITLE_H + GUI_BORDER;
    w->cw = w->w - GUI_BORDER * 2;
    w->ch = w->h - GUI_TITLE_H - GUI_BORDER * 2;
}

static void raise_window(window_t *w)
{
    int idx = -1;
    for (int i = 0; i < zcount; i++)
        if (zorder[i] == w)
            idx = i;
    if (idx < 0 || idx == zcount - 1)
        return;
    for (int i = idx; i < zcount - 1; i++)
        zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = w;
}

void gui_focus(window_t *w)
{
    if (!w)
        return;
    focused_window = w;
    raise_window(w);
    gui_dirty();
}

void gui_resize(window_t *w, int cw, int ch)
{
    int maxw = gfx_width() - 2 * GUI_BORDER;
    int maxh = gfx_height() - GUI_TASKBAR_H - GUI_TITLE_H - 4 * GUI_BORDER;

    if (cw > maxw) cw = maxw;
    if (ch > maxh) ch = maxh;
    w->w = cw + GUI_BORDER * 2;
    w->h = ch + GUI_TITLE_H + GUI_BORDER * 2;
    if (w->x + w->w > gfx_width())
        w->x = gfx_width() - w->w;
    if (w->x < 0)
        w->x = 0;
    if (w->y + w->h > gfx_height() - GUI_TASKBAR_H)
        w->y = 20;
    set_content_rect(w);
    gui_dirty();
}

/* Set while the window manager is shutting a window down, so re-entrant
 * gui_open() calls cannot pick up a窗口 that is on its way out. */
static int closing;

/* A window whose owning process is gone is a zombie: it still occupies a
 * slot in `windows[]`, still sits in `zorder` and still holds the dead
 * program's pixels.  Everything that walks the window table goes through
 * this so such an entry is never handed out or drawn. */
static int window_is_live(const window_t *w)
{
    return w->used && !w->orphan;
}

/* Mark every window that belongs to a user program as orphaned.  Called when
 * a ring 3 process dies: the process may not have reached uapp_close() (it
 * can be killed by an exception), and a leftover window would otherwise be
 * reused by the next program, which is how opening the terminal after a
 * spider solitaire crash showed the solitaire board. */
void gui_orphan_user_windows(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].app && windows[i].app->is_user)
            windows[i].orphan = 1;
    }
}

/* Drop orphaned windows from the display lists.  Safe to call at any time;
 * does nothing when there is nothing to reap. */
void gui_reap_orphans(void)
{
    int changed = 0;

    if (closing)
        return;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];

        if (!w->used || !w->orphan)
            continue;

        if (w->app && w->app->close)
            w->app->close(w);

        for (int j = 0; j < zcount; j++) {
            if (zorder[j] == w) {
                for (int k = j; k < zcount - 1; k++)
                    zorder[k] = zorder[k + 1];
                zcount--;
                break;
            }
        }
        if (focused_window == w)
            focused_window = zcount ? zorder[zcount - 1] : NULL;
        if (mouse_grab == w)
            mouse_grab = NULL;
        w->used = 0;
        w->orphan = 0;
        w->state = NULL;
        changed = 1;
    }

    if (changed)
        gui_dirty();
}

window_t *gui_open(const app_t *app)
{
    if (!closing) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (window_is_live(&windows[i]) && windows[i].app == app) {
                gui_focus(&windows[i]);
                return &windows[i];
            }
        }
    }
    if (zcount >= MAX_WINDOWS)
        return NULL;

    window_t *w = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used) {
            w = &windows[i];
            break;
        }
    }
    if (!w)
        return NULL;

    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->orphan = 0;
    w->app = app;

    int cw = app->width;
    int ch = app->height;
    int maxw = gfx_width() - 2 * GUI_BORDER;
    int maxh = gfx_height() - GUI_TASKBAR_H - GUI_TITLE_H - 4 * GUI_BORDER;
    if (cw > maxw) cw = maxw;
    if (ch > maxh) ch = maxh;

    w->w = cw + GUI_BORDER * 2;
    w->h = ch + GUI_TITLE_H + GUI_BORDER * 2;

    {
        int step = 26 * zcount;
        int desk = ICON_LEFT + ICON_COLS * ICON_W + 12;
        w->x = desk + step;
        w->y = 24 + step;
        if (w->x + w->w > gfx_width())
            w->x = gfx_width() - w->w - 10;
        if (w->x < 8)
            w->x = 8;
        if (w->y + w->h > gfx_height() - GUI_TASKBAR_H)
            w->y = 20;
    }
    set_content_rect(w);

    zorder[zcount++] = w;
    focused_window = w;
    if (app->init)
        app->init(w);
    gui_dirty();
    return w;
}

void gui_close(window_t *w)
{
    if (!w || !w->used)
        return;
    if (w->app->close)
        w->app->close(w);

    int idx = -1;
    for (int i = 0; i < zcount; i++)
        if (zorder[i] == w)
            idx = i;
    if (idx >= 0) {
        for (int i = idx; i < zcount - 1; i++)
            zorder[i] = zorder[i + 1];
        zcount--;
    }
    w->used = 0;
    w->orphan = 0;
    w->state = NULL;
    if (focused_window == w)
        focused_window = zcount ? zorder[zcount - 1] : NULL;
    if (mouse_grab == w)
        mouse_grab = NULL;
    gui_dirty();
}

void ui_panel(int x, int y, int w, int h, u32 bg)
{
    gfx_fill_rect(x, y, w, h, bg);
}

void ui_text(int x, int y, const char *s, u32 color)
{
    gfx_text(x, y, s, color, 1);
}

int ui_button(int x, int y, int w, int h, const char *label, int active)
{
    gfx_fill_rect(x, y, w, h, active ? C_BTN_HOT : C_BTN);
    gfx_rect(x, y, w, h, C_BTN_EDGE);
    int tw = gfx_text_w(label, 1);
    gfx_text(x + (w - tw) / 2, y + (h - 8) / 2, label, C_TEXT, 1);
    return 0;
}

static const app_t *menu_apps[] = {
    &app_terminal, &app_files, &app_editor, &app_paint, &app_about,
    &app_snake, &app_mines, &app_2048
};
#define MENU_APP_ITEMS ((int)ARRAY_LEN(menu_apps))
#define MENU_ITEMS (MENU_APP_ITEMS + 1)   /* the last entry switches language */
#define MENU_ITEM_H 22
#define MENU_W 168

static int in_rect(int x, int y, int rx, int ry, int rw, int rh);
static int icon_selected = -1;

#define ICON_W   76
#define ICON_H   62
#define ICON_ROWS 9
#define ICON_COLS 3
#define ICON_TOP 10
#define ICON_LEFT 10
#define ICON_MAX (ICON_ROWS * ICON_COLS)

typedef struct {
    char name[24];
    char path[160];
    int  is_system;
    int  app_index;
    int  is_folder;
    int  col;
    int  row;
} desktop_icon_t;

static desktop_icon_t icons[ICON_ROWS * ICON_COLS];
static int icon_count;
static u32 icon_scan_tick;

static int icon_add(int *n, int col, int row, const char *name, const char *path,
                    int is_system, int app_index, int is_folder)
{
    desktop_icon_t *ic;

    if (*n >= ICON_MAX || col >= ICON_COLS || row >= ICON_ROWS)
        return -1;
    ic = &icons[(*n)++];
    memset(ic, 0, sizeof(*ic));
    strncpy(ic->name, name, sizeof(ic->name) - 1);
    if (path)
        strncpy(ic->path, path, sizeof(ic->path) - 1);
    ic->is_system = is_system;
    ic->app_index = app_index;
    ic->is_folder = is_folder;
    ic->col = col;
    ic->row = row;
    return 0;
}

static void icon_scan(void)
{
    int n = 0;
    int i;

    icon_count = 0;

    for (i = 0; i < MENU_APP_ITEMS; i++)
        if (icon_add(&n, 0, i, app_title(menu_apps[i]), NULL, 1, i, 0) != 0)
            break;

    if (fs_is_dir("/APPS")) {
        int row = 0;

        for (i = 0; row < ICON_ROWS; i++) {
            fs_dirent_t e;
            int len;
            char nm[24];
            char path[160];

            if (fs_list("/APPS", i, &e) != 0)
                break;
            if (e.flags & FS_FLAG_DIR)
                continue;
            len = (int)strlen(e.name);
            if (len < 5 || strcasecmp(e.name + len - 4, ".APP") != 0)
                continue;
            strncpy(nm, e.name, sizeof(nm) - 1);
            nm[sizeof(nm) - 1] = 0;
            if (len - 4 < (int)sizeof(nm))
                nm[len - 4] = 0;
            snprintf(path, sizeof(path), "/APPS/%s", e.name);
            icon_add(&n, 1, row, nm, path, 0, 0, 0);
            row++;
        }
    }

    {
        int row = 0;

        for (i = 0; row < ICON_ROWS; i++) {
            fs_dirent_t e;
            char path[160];

            if (fs_list("/", i, &e) != 0)
                break;
            if (!(e.flags & FS_FLAG_DIR))
                continue;
            snprintf(path, sizeof(path), "/%s", e.name);
            icon_add(&n, 2, row, e.name, path, 0, 0, 1);
            row++;
        }
    }

    icon_count = n;
    icon_scan_tick = timer_ticks();
}

static void icon_rect(int index, int *ix, int *iy)
{
    *ix = ICON_LEFT + icons[index].col * ICON_W;
    *iy = ICON_TOP + icons[index].row * ICON_H;
}

static int icon_scanned;

static void icons_poll(void)
{
    int before;

    if (icon_scanned && timer_ticks() - icon_scan_tick <= 200)
        return;
    before = icon_count;
    icon_scan();
    icon_scanned = 1;
    if (icon_count != before)
        scene_dirty = 1;
}

static int icon_at(int x, int y, int *index)
{
    for (int i = 0; i < icon_count; i++) {
        int ix, iy;
        icon_rect(i, &ix, &iy);
        if (in_rect(x, y, ix, iy, ICON_W - 6, ICON_H - 6)) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static void draw_desktop(void)
{
    int i;

    gfx_reset_clip();
    gfx_gradient(0, 0, gfx_width(), gfx_height(), C_DESK_TOP, C_DESK_BOT);

    {
        const char *logo = "aptos";
        int lw = gfx_text_w(logo, 4);
        gfx_text((gfx_width() - lw) / 2, gfx_height() / 2 - 60, logo, C_LOGO, 4);
        {
            const char *sub = tr(S_DESK_HINT);
            int sw = gfx_text_w(sub, 1);
            gfx_text((gfx_width() - sw) / 2, gfx_height() / 2 - 10, sub, C_LOGO_SUB, 1);
        }
    }

    {
        int mx, my, mb;
        mouse_get(&mx, &my, &mb);

        for (i = 0; i < icon_count; i++) {
            int ix, iy, hot;
            u32 face;
            int sel = (i == icon_selected);

            icon_rect(i, &ix, &iy);
            hot = in_rect(mx, my, ix, iy, ICON_W - 6, ICON_H - 6);
            face = (icons[i].is_system) ? RGB(64, 132, 196) : RGB(196, 128, 56);
            if (icons[i].is_folder)
                face = RGB(226, 176, 66);
            if (icons[i].is_system) {
                switch (icons[i].app_index) {
                case 1: face = RGB(206, 168, 62); break;
                case 2: face = RGB(160, 160, 168); break;
                case 3: face = RGB(196, 88, 132); break;
                case 4: face = RGB(96, 176, 128); break;
                case 5: face = RGB(112, 176, 92); break;
                case 6: face = RGB(176, 96, 88); break;
                case 7: face = RGB(128, 112, 200); break;
                default: break;
                }
            }

            if (hot || sel)
                gfx_fill_rect(ix + 1, iy + 1, ICON_W - 8, ICON_H - 8, C_BTN_HOT);

            if (icons[i].is_folder) {
                gfx_fill_rect(ix + 18, iy + 8, 16, 6, face);
                gfx_fill_rect(ix + 14, iy + 12, 40, 24, face);
                gfx_rect(ix + 14, iy + 12, 40, 24, RGB(120, 90, 20));
                gfx_fill_rect(ix + 14, iy + 14, 40, 3,
                              gfx_blend(face, RGB(255, 255, 255), 100));
            } else {
                gfx_fill_rect(ix + 14, iy + 6, 40, 30, face);
                gfx_rect(ix + 14, iy + 6, 40, 30, RGB(20, 24, 32));
                gfx_fill_rect(ix + 18, iy + 10, 32, 4,
                              gfx_blend(face, RGB(255, 255, 255), 90));
                gfx_fill_rect(ix + 18, iy + 18, 22, 3,
                              gfx_blend(face, RGB(0, 0, 0), 60));
                gfx_fill_rect(ix + 18, iy + 25, 26, 3,
                              gfx_blend(face, RGB(0, 0, 0), 60));
            }

            {
                int tw = gfx_text_w(icons[i].name, 1);
                int tx = ix + (ICON_W - 6 - tw) / 2;
                if (tx < ix + 1)
                    tx = ix + 1;
                gfx_text(tx, iy + 42, icons[i].name, C_TEXT, 1);
            }
        }
    }
}

static void draw_close_button(window_t *w)
{
    int bx = w->x + w->w - 18;
    int by = w->y + 4;
    gfx_fill_rect(bx, by, 14, 13, RGB(190, 70, 70));
    gfx_rect(bx, by, 14, 13, RGB(230, 130, 130));
    gfx_line(bx + 4, by + 4, bx + 9, by + 9, COLOR_WHITE);
    gfx_line(bx + 9, by + 4, bx + 4, by + 9, COLOR_WHITE);
}

static void draw_window(window_t *w)
{
    int active = (w == focused_window);

    gfx_reset_clip();
    gfx_fill_rect(w->x + 4, w->y + 4, w->w, w->h, C_SHADOW);
    gfx_fill_rect(w->x, w->y, w->w, w->h, C_WIN_EDGE);

    int ty = w->y + 1;
    int th = GUI_TITLE_H - 1;
    gfx_gradient(w->x + 1, ty, w->w - 2, th,
                 active ? C_TITLE_A : C_TITLE_UA,
                 active ? C_TITLE_B : C_TITLE_UB);
    gfx_text(w->x + 6, ty + (th - 8) / 2, app_title(w->app), COLOR_WHITE, 1);
    draw_close_button(w);

    gfx_fill_rect(w->cx, w->cy, w->cw, w->ch, C_WIN_BG);

    if (w->app->draw) {
        gfx_set_clip(w->cx, w->cy, w->cx + w->cw - 1, w->cy + w->ch - 1);
        w->app->draw(w);
        gfx_reset_clip();
    }
}

static int menu_x(void) { return 4; }
static int menu_y(void) { return gfx_height() - GUI_TASKBAR_H - MENU_ITEMS * MENU_ITEM_H - 10; }

static void draw_start_menu(void)
{
    if (!start_menu_open)
        return;
    int x = menu_x(), y = menu_y();
    int h = MENU_ITEMS * MENU_ITEM_H + 8;

    gfx_reset_clip();
    gfx_fill_rect(x + 3, y + 3, MENU_W, h, C_SHADOW);
    gfx_fill_rect(x, y, MENU_W, h, RGB(26, 30, 38));
    gfx_rect(x, y, MENU_W, h, C_TASK_EDGE);

    int mx, my, mb;
    mouse_get(&mx, &my, &mb);

    for (int i = 0; i < MENU_APP_ITEMS; i++) {
        int iy = y + 4 + i * MENU_ITEM_H;
        int hot = (mx >= x && mx < x + MENU_W && my >= iy && my < iy + MENU_ITEM_H);
        if (hot)
            gfx_fill_rect(x + 2, iy, MENU_W - 4, MENU_ITEM_H, C_BTN_HOT);
        gfx_text(x + 12, iy + 7, app_title(menu_apps[i]), C_TEXT, 1);
    }

    {
        int iy = y + 4 + MENU_APP_ITEMS * MENU_ITEM_H;
        int hot = (mx >= x && mx < x + MENU_W && my >= iy && my < iy + MENU_ITEM_H);
        const char *cur = i18n_name(i18n_lang());

        gfx_hline(x + 6, iy - 2, MENU_W - 12, C_TASK_EDGE);
        if (hot)
            gfx_fill_rect(x + 2, iy, MENU_W - 4, MENU_ITEM_H, C_BTN_HOT);
        gfx_text(x + 12, iy + 7, tr(S_MENU_LANG), C_TEXT, 1);
        gfx_text(x + MENU_W - gfx_text_w(cur, 1) - 10, iy + 7, cur, C_DIM, 1);
    }
}

static void draw_taskbar(void)
{
    int y = gfx_height() - GUI_TASKBAR_H;
    gfx_reset_clip();
    gfx_fill_rect(0, y, gfx_width(), GUI_TASKBAR_H, C_TASKBAR);
    gfx_hline(0, y, gfx_width(), C_TASK_EDGE);

    ui_button(4, y + 4, 62, 18, tr(S_START), start_menu_open);

    int bx = 72;
    for (int i = 0; i < zcount; i++) {
        window_t *w = zorder[i];
        int bw = gfx_text_w(app_title(w->app), 1) + 16;
        if (bx + bw > gfx_width() - 70)
            break;
        ui_button(bx, y + 4, bw, 18, app_title(w->app), w == focused_window);
        bx += bw + 4;
    }

    int cw = gfx_text_w(clock_text, 1);
    gfx_text(gfx_width() - cw - 8, y + 9, clock_text, C_TEXT, 1);
}

static void cursor_save(void)
{
    if (cur_x < 0 || cur_y < 0)
        return;
    for (int j = 0; j < CURSOR_H; j++) {
        for (int i = 0; i < CURSOR_W; i++) {
            int x = cur_x + i, y = cur_y + j;
            u32 c = 0;
            if (x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height())
                c = gfx_backbuffer()[y * gfx_width() + x];
            cursor_under[j * CURSOR_W + i] = c;
        }
    }
    cursor_saved = 1;
}

static void cursor_restore(void)
{
    if (!cursor_saved || cur_x < 0 || cur_y < 0)
        return;
    rect_t old = gfx_get_clip();
    gfx_reset_clip();
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++)
            gfx_pixel(cur_x + i, cur_y + j, cursor_under[j * CURSOR_W + i]);
    gfx_set_clip_rect(old);
    cursor_saved = 0;
}

static void cursor_paint(void)
{
    if (cur_x < 0 || cur_y < 0)
        return;
    rect_t old = gfx_get_clip();
    gfx_reset_clip();
    for (int j = 0; j < CURSOR_H; j++) {
        const char *row = cursor_shape[j];
        for (int i = 0; i < CURSOR_W && row[i]; i++) {
            if (row[i] == '1')
                gfx_pixel(cur_x + i, cur_y + j, RGB(20, 20, 24));
            else if (row[i] == '2')
                gfx_pixel(cur_x + i, cur_y + j, RGB(245, 248, 255));
        }
    }
    gfx_set_clip_rect(old);
}

static void cursor_move_to(int nx, int ny)
{
    if (nx == cur_x && ny == cur_y)
        return;

    int old_x = cur_x, old_y = cur_y;
    cursor_restore();
    if (old_x >= 0)
        gfx_present_rect(old_x - 1, old_y - 1, CURSOR_W + 2, CURSOR_H + 2);

    cur_x = nx;
    cur_y = ny;
    cursor_save();
    cursor_paint();
    gfx_present_rect(cur_x - 1, cur_y - 1, CURSOR_W + 2, CURSOR_H + 2);
}

void gui_cursor_refresh(void)
{
    int mx, my, mb;

    mouse_get(&mx, &my, &mb);
    if (mx != cur_x || my != cur_y) {
        cursor_move_to(mx, my);
        return;
    }
    if (cur_x < 0)
        return;
    cursor_saved = 0;
    cursor_restore();
    cursor_save();
    cursor_paint();
    gfx_present_rect(cur_x - 1, cur_y - 1, CURSOR_W + 2, CURSOR_H + 2);
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static window_t *window_at(int x, int y)
{
    for (int i = zcount - 1; i >= 0; i--) {
        window_t *w = zorder[i];
        if (in_rect(x, y, w->x, w->y, w->w, w->h))
            return w;
    }
    return NULL;
}

static void app_mouse(window_t *w, int x, int y, int buttons, int event)
{
    if (!w || !w->app->mouse)
        return;
    w->app->mouse(w, x - w->cx, y - w->cy, buttons, event);
}

static void mouse_event(int event, int x, int y, int buttons)
{
    if (event == GUI_MOUSE_DOWN) {
        if (start_menu_open) {
            int mx = menu_x(), my = menu_y();
            int mh = MENU_ITEMS * MENU_ITEM_H + 8;
            if (in_rect(x, y, mx, my, MENU_W, mh)) {
                int idx = (y - my - 4) / MENU_ITEM_H;
                if (idx >= 0 && idx < MENU_APP_ITEMS) {
                    start_menu_open = 0;
                    gui_dirty();
                    gui_open(menu_apps[idx]);
                } else if (idx == MENU_APP_ITEMS) {
                    i18n_toggle();
                    start_menu_open = 0;
                    icon_scanned = 0;
                    gui_dirty();
                }
                return;
            }
            start_menu_open = 0;
            gui_dirty();
        }

        int ty = gfx_height() - GUI_TASKBAR_H;
        if (y >= ty) {
            if (in_rect(x, y, 4, ty + 4, 62, 18)) {
                start_menu_open = !start_menu_open;
                gui_dirty();
                return;
            }
            int bx = 72;
            for (int i = 0; i < zcount; i++) {
                window_t *w = zorder[i];
                int bw = gfx_text_w(app_title(w->app), 1) + 16;
                if (bx + bw > gfx_width() - 70)
                    break;
                if (in_rect(x, y, bx, ty + 4, bw, 18)) {
                    gui_focus(w);
                    return;
                }
                bx += bw + 4;
            }
            return;
        }

        window_t *w = window_at(x, y);
        if (!w) {
            int idx;
            if (icon_at(x, y, &idx)) {
                desktop_icon_t *ic = &icons[idx];
                icon_selected = idx;
                if (ic->is_folder) {
                    extern void app_files_open_at(const char *path);
                    gui_open(&app_files);
                    app_files_open_at(ic->path);
                } else if (ic->is_system) {
                    gui_open(menu_apps[ic->app_index]);
                } else {
                    extern void term_run_program(const char *path);
                    term_run_program(ic->path);
                }
                gui_dirty();
            }
            return;
        }

        gui_focus(w);

        if (in_rect(x, y, w->x + w->w - 18, w->y + 4, 14, 13)) {
            gui_close(w);
            return;
        }
        if (y < w->cy) {
            dragging = 1;
            drag_dx = x - w->x;
            drag_dy = y - w->y;
            mouse_grab = w;
            return;
        }
        mouse_grab = w;
        app_mouse(w, x, y, buttons, GUI_MOUSE_DOWN);
        return;
    }

    if (event == GUI_MOUSE_UP) {
        if (dragging) {
            dragging = 0;
            return;
        }
        if (mouse_grab) {
            app_mouse(mouse_grab, x, y, buttons, GUI_MOUSE_UP);
            mouse_grab = NULL;
        }
        return;
    }

    if (dragging && mouse_grab) {
        window_t *w = mouse_grab;
        int nx = x - drag_dx;
        int ny = y - drag_dy;
        if (nx < -w->w + 40) nx = -w->w + 40;
        if (ny < 0) ny = 0;
        if (nx > gfx_width() - 40) nx = gfx_width() - 40;
        if (ny > gfx_height() - GUI_TASKBAR_H - 20) ny = gfx_height() - GUI_TASKBAR_H - 20;
        if (nx != w->x || ny != w->y) {
            w->x = nx;
            w->y = ny;
            set_content_rect(w);
            gui_dirty();
        }
        return;
    }

    if (mouse_grab && (buttons & MOUSE_LEFT))
        app_mouse(mouse_grab, x, y, buttons, GUI_MOUSE_MOVE);
}

static void handle_mouse(void)
{
    static int last_x = -1, last_y = -1;
    int x, y, b, ev;

    while ((ev = mouse_next_event(&x, &y, &b)) != 0)
        mouse_event(ev == MOUSE_EV_DOWN ? GUI_MOUSE_DOWN : GUI_MOUSE_UP, x, y, b);

    mouse_get(&x, &y, &b);
    if (x != last_x || y != last_y)
        mouse_event(GUI_MOUSE_MOVE, x, y, b);
    last_x = x;
    last_y = y;
}

static int frames_drawn;

static void clock_rect(rect_t *r)
{
    int ty = gfx_height() - GUI_TASKBAR_H;
    int w = gfx_text_w("00:00:00", 1);

    r->x0 = gfx_width() - w - 12;
    r->y0 = ty + 1;
    r->x1 = gfx_width() - 2;
    r->y1 = gfx_height() - 1;
}

static void redraw_clock(void)
{
    rect_t r, old;
    int w, h;

    clock_rect(&r);
    if (frames_drawn == 0) {
        scene_dirty = 1;
        return;
    }
    if (cur_x >= 0 && cur_y >= 0 && cur_x + CURSOR_W >= r.x0 &&
        cur_x <= r.x1 && cur_y + CURSOR_H >= r.y0 && cur_y <= r.y1) {
        scene_dirty = 1;
        return;
    }

    old = gfx_get_clip();
    gfx_set_clip_rect(r);
    w = r.x1 - r.x0 + 1;
    h = r.y1 - r.y0 + 1;
    gfx_fill_rect(r.x0, r.y0, w, h, C_TASKBAR);
    gfx_text(r.x0 + 4, r.y0 + 8, clock_text, C_TEXT, 1);
    gfx_set_clip_rect(old);
    gfx_present_rect(r.x0, r.y0, w, h);
}

static void update_clock(void)
{
    icons_poll();
    if (timer_ticks() - clock_tick < timer_hz() / 2)
        return;
    clock_tick = timer_ticks();

    rtc_time_t t;
    rtc_read(&t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
    if (strcmp(buf, clock_text) != 0) {
        strcpy(clock_text, buf);
        redraw_clock();
    }
}

void gui_init(void)
{
    memset(windows, 0, sizeof(windows));
    zcount = 0;
    focused_window = NULL;
    scene_dirty = 1;
    start_menu_open = 0;
    cursor_saved = 0;
    cur_x = -1;
    cur_y = -1;

    mouse_set_bounds(gfx_width(), gfx_height());
    mouse_set_position(gfx_width() / 2, gfx_height() / 2);

    kprintf("[gui] desktop %dx%d, taskbar %dpx\n", gfx_width(), gfx_height(),
            GUI_TASKBAR_H);
}

void gui_set_input(int enabled)
{
    input_enabled = enabled;
}

void gui_poll(void)
{
    int k;

    /* Reap windows whose owning program has exited.  Doing it here (rather
     * than only in gui_close) also covers a process killed by an exception,
     * which never gets to run its own cleanup. */
    gui_reap_orphans();

    if (!input_enabled) {
        int mx, my, mb;
        mouse_get(&mx, &my, &mb);
        if (scene_dirty) {
            scene_dirty = 0;
            draw_desktop();
            for (int i = 0; i < zcount; i++)
                draw_window(zorder[i]);
            draw_taskbar();
            draw_start_menu();
            cur_x = mx;
            cur_y = my;
            gfx_reset_clip();
            cursor_save();
            cursor_paint();
            gfx_present();
            frames_drawn++;
        }
        return;
    }

    while ((k = keyboard_getkey()) != -1) {
        /* F1 (or Ctrl+Alt+L) switches between English and Chinese windows-wide */
        if (k == KEY_F1 || (k == 12 && (keyboard_mods() & KMOD_ALT))) {
            i18n_toggle();
            icon_scanned = 0;
            gui_dirty();
            continue;
        }
        if (focused_window && focused_window->app->key)
            focused_window->app->key(focused_window, k, keyboard_mods());
    }

    int mx, my, mb;
    mouse_get(&mx, &my, &mb);
    int moved = (mx != cur_x || my != cur_y);

    handle_mouse();
    update_clock();

    if (focused_window && focused_window->app->tick)
        focused_window->app->tick(focused_window);

    if (scene_dirty) {
        scene_dirty = 0;
        draw_desktop();
        for (int i = 0; i < zcount; i++)
            draw_window(zorder[i]);
        draw_taskbar();
        draw_start_menu();

        cur_x = mx;
        cur_y = my;
        gfx_reset_clip();
        cursor_save();
        cursor_paint();
        gfx_present();
        frames_drawn++;
    } else if (moved) {
        cursor_move_to(mx, my);
    }
}

void gui_run(void)
{
    for (;;) {
        gui_poll();
        hlt();
    }
}
