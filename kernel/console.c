#include "console.h"
#include "gfx.h"
#include "klog.h"
#include "util.h"

#define BG      RGB(10, 14, 22)
#define FG      RGB(198, 208, 220)
#define ACCENT  RGB(90, 190, 255)
#define DIM     RGB(110, 120, 135)

#define LINE_H   10
#define TOP      34
#define LEFT     10

static int ready;
static int dirty;

/* ---- 增量刷新缓存：记录"屏幕上当前已画的内容"，每次只重绘变化的行 ----
 * 慢的根源是旧实现每来一个字符就 gfx_clear(全屏) + 重画所有行 +
 * gfx_present(整帧 1024x768x4 ≈ 3MB 拷到显存)。打一个字全屏刷一遍，
 * 自然一顿一顿。这里改成行级比较：只重画内容变了的行，并且只用
 * gfx_present_rect 把该行的条带刷到显存，打字时每键只动 10 像素高
 * 的一条，快一到两个数量级。 */
static char   shown[KLOG_LINES][KLOG_COLS];  /* 屏幕第 r 行当前显示的文字 */
static int    shown_first;                   /* 屏幕顶行对应的 klog 绝对行号 */
static int    shown_valid;
static int    cur_x, cur_y = -1;             /* 上次画的光标位置(用于擦除) */

static int lines_on_screen(void)
{
    int rows = (gfx_height() - TOP - 8) / LINE_H;
    return rows < 1 ? 1 : rows;
}

/* 画一整行（先清该行背景，再写文字），只把这条带刷到显存 */
static void draw_line(int y, const char *text, u32 color)
{
    int w = gfx_width();
    gfx_fill_rect(0, y - 1, w, LINE_H + 1, BG);
    if (text && *text)
        gfx_text(LEFT, y, text, color, 1);
    gfx_present_rect(0, y - 1, w, LINE_H + 1);
}

/* 画固定的页眉/页脚（只在 init 时全屏画一次） */
static void draw_header(void)
{
    int w = gfx_width();
    int h = gfx_height();
    gfx_reset_clip();
    gfx_clear(BG);
    gfx_text(LEFT, 8, "aptos", ACCENT, 2);
    gfx_text(LEFT + 4 * 8 * 2 + 8, 14, "v2.0  -  32-bit x86 kernel", DIM, 1);
    gfx_hline(0, 30, w, RGB(30, 40, 55));
    gfx_text(LEFT, h - 12,
             "serial: COM1 115200 8N1   |   screen: VBE linear framebuffer",
             DIM, 1);
    gfx_present();
}

void console_init(void)
{
    ready = 1;
    dirty = 1;
    shown_valid = 0;
    shown_first = 0;
    for (int i = 0; i < KLOG_LINES; i++)
        shown[i][0] = 0;
    klog_screen(1);
    draw_header();
    console_update();
}

void console_shutdown(void)
{
    ready = 0;
    klog_screen(0);
}

void console_pump(void)
{
    dirty = 1;
}

void console_update(void)
{
    if (!ready || !dirty)
        return;
    dirty = 0;

    int rows = lines_on_screen();
    int total = klog_count();
    int first = total > rows ? total - rows : 0;

    if (!shown_valid || first != shown_first) {
        /* 滚动了（或首次）：整屏行重画（仍是一次，不是每字符一次） */
        for (int r = 0; r < rows; r++) {
            int abs = first + r;
            const char *t = (abs < total) ? klog_line(abs) : "";
            draw_line(TOP + r * LINE_H, t, FG);
            strncpy(shown[r], t ? t : "", KLOG_COLS - 1);
            shown[r][KLOG_COLS - 1] = 0;
        }
        shown_first = first;
        shown_valid = 1;
    } else {
        /* 没滚动：只重画内容变化了的行 */
        for (int r = 0; r < rows; r++) {
            int abs = first + r;
            const char *t = (abs < total) ? klog_line(abs) : "";
            if (strcmp(shown[r], t) != 0) {
                draw_line(TOP + r * LINE_H, t, FG);
                strncpy(shown[r], t ? t : "", KLOG_COLS - 1);
                shown[r][KLOG_COLS - 1] = 0;
            }
        }
    }

    /* 光标小方块：先擦掉上一次的位置，再画新的，只刷 7x7 小块 */
    int lastrow = (total - 1 > first) ? (total - 1 - first) : 0;
    if (lastrow >= rows)
        lastrow = rows - 1;
    const char *last = (total > 0) ? klog_line(total - 1) : "";
    int cx = LEFT + (int)strlen(last) * 8;
    int cy = TOP + lastrow * LINE_H;
    if (cx < gfx_width() - 12) {
        if (cur_y >= 0 && (cur_x != cx || cur_y != cy)) {
            /* 旧光标位置重画该行文字，把方块擦掉 */
            gfx_fill_rect(cur_x, cur_y + 1, 7, 7, BG);
            gfx_present_rect(cur_x, cur_y + 1, 7, 7);
        }
        gfx_fill_rect(cx, cy + 1, 7, 7, ACCENT);
        gfx_present_rect(cx, cy + 1, 7, 7);
        cur_x = cx;
        cur_y = cy;
    }
}
