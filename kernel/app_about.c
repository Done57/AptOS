#include "gui.h"
#include "ata.h"
#include "i18n.h"
#include "blkdev.h"
#include "bootinfo.h"
#include "fs.h"
#include "heap.h"
#include "klog.h"
#include "timer.h"
#include "util.h"

#define AB_W 510
#define AB_H 268

static window_t *win;

static void ab_draw(window_t *w)
{
    int x = w->cx + 18;
    int y = w->cy + 14;
    int step = ui_line_h() + 3;   /* 12px for ASCII, 19px when CJK is showing */

    gfx_text(x, y, "aptos", RGB(120, 190, 255), 3);
    y += 34;
    gfx_text(x, y, tr(S_ABOUT_VERSION),
             RGB(170, 180, 196), 1);
    y += 20;
    gfx_hline(w->cx + 12, y, w->cw - 24, RGB(60, 68, 84));
    y += 10;

    char line[96];

    snprintf(line, sizeof(line), "%s", tr(S_ABOUT_CPU));
    gfx_text(x, y, line, RGB(206, 214, 226), 1); y += step;

    snprintf(line, sizeof(line), tr(S_ABOUT_DISPLAY),
             gfx_width(), gfx_height(), (int)gfx_bpp());
    gfx_text(x, y, line, RGB(206, 214, 226), 1); y += step;

    snprintf(line, sizeof(line), tr(S_ABOUT_MEMORY),
             mem_total_kb() / 1024, heap_used() / 1024, heap_total() / (1024 * 1024));
    gfx_text(x, y, line, RGB(206, 214, 226), 1); y += step;

    {
        const blk_info_t *bi = blk_info(0);
        snprintf(line, sizeof(line), tr(S_ABOUT_DISK), bi ? bi->model : "none");
    }
    gfx_text(x, y, line, RGB(206, 214, 226), 1); y += step;

    snprintf(line, sizeof(line), tr(S_ABOUT_VOLUME),
             fs_volume_label(), fs_free_kb());
    gfx_text(x, y, line, RGB(206, 214, 226), 1); y += step;

    snprintf(line, sizeof(line), tr(S_ABOUT_UPTIME),
             timer_uptime_ms() / 1000, timer_uptime_ms() % 1000);
    gfx_text(x, y, line, RGB(206, 214, 226), 1); y += 18;

    gfx_text(x, y, tr(S_ABOUT_BOOT),
             RGB(130, 145, 165), 1);
    y += step;
    gfx_text(x, y, tr(S_ABOUT_TIP),
             RGB(130, 145, 165), 1);
}

static void ab_init(window_t *w)
{
    (void)w;
    gui_dirty();
}

static void ab_close(window_t *w)
{
    (void)w;
    win = NULL;
}

const app_t app_about = {
    .title  = "About",
    .title_id = S_APP_ABOUT,
    .width  = AB_W,
    .height = AB_H,
    .init   = ab_init,
    .draw   = ab_draw,
    .close  = ab_close,
};
