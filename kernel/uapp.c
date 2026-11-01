#include "uapp.h"
#include "font8x8.h"
#include "fontcjk.h"
#include "gfx.h"
#include "gui.h"
#include "heap.h"
#include "klog.h"
#include "mouse.h"
#include "util.h"

#define FONT_W 8
#define FONT_H 8

static u32 *canvas;
static int cw, ch;
static window_t *uwin;
static int have_window;
static char title_buf[32];

static void uapp_draw(window_t *w)
{
    if (!canvas)
        return;
    gfx_set_clip(w->cx, w->cy, w->cx + w->cw - 1, w->cy + w->ch - 1);
    gfx_blit(w->cx, w->cy, cw, ch, canvas);
    gfx_reset_clip();
}

static app_t uapp_app = {
    "app", 0, 640, 480, NULL, uapp_draw, NULL, NULL, NULL, NULL, 1
};

int uapp_alive(void) { return have_window && uwin != NULL; }
int uapp_width(void) { return cw; }
int uapp_height(void) { return ch; }
int uapp_open_window(void) { return have_window; }

/* Attach a canvas that somebody else allocated (the apt layer) and open a
 * window showing it.  Unlike uapp_open() the pixels are not owned here: the
 * caller keeps them and is responsible for their lifetime. */
int uapp_attach_canvas(u32 *pixels, int w, int h, const char *title)
{
    if (!pixels || w < 64 || h < 64)
        return -1;
    if (w > UAPP_MAX_W)
        w = UAPP_MAX_W;
    if (h > UAPP_MAX_H)
        h = UAPP_MAX_H;

    canvas = pixels;
    cw = w;
    ch = h;

    strncpy(title_buf, title ? title : "application", sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = 0;
    uapp_app.title = title_buf;
    uapp_app.width = w;
    uapp_app.height = h;

    if (!uwin) {
        uwin = gui_open(&uapp_app);
        if (!uwin) {
            kprintf("[uapp] could not open a window\n");
            return -1;
        }
    } else {
        gui_resize(uwin, w, h);
    }
    have_window = 1;
    gui_focus(uwin);
    return 0;
}

/* Composite just one rectangle of the canvas.  apt uses this so a program
 * that only redrew a small area does not pay for a full-window blit. */
void uapp_present(int x, int y, int w, int h)
{
    if (!uwin || !canvas)
        return;

    /* The window may be smaller than the canvas. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cw) w = cw - x;
    if (y + h > ch) h = ch - y;
    if (w <= 0 || h <= 0)
        return;

    gfx_set_clip(uwin->cx, uwin->cy, uwin->cx + uwin->cw - 1, uwin->cy + uwin->ch - 1);
    gfx_blit(uwin->cx + x, uwin->cy + y, w, h, canvas + (size_t)y * cw + x);
    gfx_reset_clip();
    gfx_present_rect(uwin->cx + x, uwin->cy + y, w, h);
    gui_cursor_refresh();
}

int uapp_open(const char *title, int w, int h)
{
    u8 *buf;

    if (w < 64)
        w = 64;
    if (h < 64)
        h = 64;
    if (w > UAPP_MAX_W)
        w = UAPP_MAX_W;
    if (h > UAPP_MAX_H)
        h = UAPP_MAX_H;

    if (!canvas || w > cw || h > ch) {
        buf = (u8 *)kmalloc((size_t)w * (size_t)h * 4);
        if (!buf) {
            kprintf("[uapp] out of memory for a %dx%d canvas\n", w, h);
            return -1;
        }
        canvas = (u32 *)buf;
        cw = w;
        ch = h;
    }

    memset(canvas, 0, (size_t)cw * (size_t)ch * 4);
    strncpy(title_buf, title ? title : "application", sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = 0;
    uapp_app.title = title_buf;
    uapp_app.width = w;
    uapp_app.height = h;

    if (!uwin) {
        uwin = gui_open(&uapp_app);
        if (!uwin) {
            kprintf("[uapp] could not open a window\n");
            return -1;
        }
    } else {
        gui_resize(uwin, w, h);
    }
    have_window = 1;

    gfx_reset_clip();
    gui_focus(uwin);
    uapp_clear(RGB(28, 34, 46));
    uapp_flush();
    return 0;
}

void uapp_clear(u32 color)
{
    int i, n = cw * ch;
    if (!canvas)
        return;
    for (i = 0; i < n; i++)
        canvas[i] = color;
}

void uapp_rect(int x, int y, int w, int h, u32 color)
{
    if (!canvas)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > cw)
        w = cw - x;
    if (y + h > ch)
        h = ch - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = y; row < y + h; row++) {
        u32 *d = canvas + (size_t)row * cw + x;
        for (int col = 0; col < w; col++)
            d[col] = color;
    }
}

void uapp_frame(int x, int y, int w, int h, u32 color)
{
    uapp_rect(x, y, w, 1, color);
    uapp_rect(x, y + h - 1, w, 1, color);
    uapp_rect(x, y, 1, h, color);
    uapp_rect(x + w - 1, y, 1, h, color);
}

void uapp_text(int x, int y, const char *s, u32 color, int scale)
{
    if (!canvas || !s)
        return;
    if (scale < 1)
        scale = 1;

    while (*s) {
        int adv = 0;
        u32 cp = gfx_utf8_decode(s, &adv);
        if (adv <= 0)
            break;

        if (cp < 0x80) {
            const unsigned char *glyph = font8x8_basic[cp];
            for (int row = 0; row < FONT_H; row++) {
                u8 bits = glyph[row];
                for (int col = 0; col < FONT_W; col++) {
                    if (bits & (1u << col))
                        uapp_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
            x += FONT_W * scale;
        } else {
            const unsigned short *rows = fontcjk_lookup(cp);
            int oy = y - ((FONT_CJK_H - FONT_H) / 2) * scale;
            for (int row = 0; row < FONT_CJK_H; row++) {
                u16 bits = rows ? rows[row] : (u16)(row == 0 || row == FONT_CJK_H - 1
                                                    ? 0xFFFF : 0x8001);
                for (int col = 0; col < FONT_CJK_W; col++) {
                    if (bits & (0x8000u >> col))
                        uapp_rect(x + col * scale, oy + row * scale, scale, scale, color);
                }
            }
            x += FONT_CJK_W * scale;
        }
        s += adv;
    }
}

int uapp_text_w(const char *s, int scale)
{
    int w = 0;

    if (scale < 1)
        scale = 1;
    while (s && *s) {
        int adv = 0;
        u32 cp = gfx_utf8_decode(s, &adv);
        if (adv <= 0)
            break;
        w += (cp < 0x80 ? FONT_W : FONT_CJK_W) * scale;
        s += adv;
    }
    return w;
}

void uapp_flush(void)
{
    if (!uwin || !canvas)
        return;
    gfx_set_clip(uwin->cx, uwin->cy, uwin->cx + uwin->cw - 1, uwin->cy + uwin->ch - 1);
    gfx_blit(uwin->cx, uwin->cy, cw, ch, canvas);
    gfx_reset_clip();
    gfx_present_rect(uwin->x, uwin->y, uwin->w, uwin->h);
    gui_cursor_refresh();
}

void uapp_mouse(int *x, int *y, int *buttons, int *inside)
{
    int mx, my, mb;

    mouse_get(&mx, &my, &mb);
    if (uwin) {
        mx -= uwin->cx;
        my -= uwin->cy;
    }
    if (x)
        *x = mx;
    if (y)
        *y = my;
    if (buttons)
        *buttons = mb;
    if (inside)
        *inside = (uwin && mx >= 0 && my >= 0 && mx < cw && my < ch) ? 1 : 0;
}

void uapp_close(void)
{
    if (uwin) {
        gui_close(uwin);
        uwin = NULL;
    }
    have_window = 0;

    /* Drop the reference to the pixels but do not free them: the apt layer
     * owns its canvas and frees it (or drops the mapping) itself. */
    canvas = NULL;
    cw = ch = 0;
}
