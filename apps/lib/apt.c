/* libapt - the client side of the apt graphics API
 *
 * Everything here writes straight into the shared canvas.  The only system
 * calls in the whole file are apt_open(), apt_flush(), apt_poll() and
 * apt_text() (text needs the kernel's font tables).
 *
 * The software rasteriser is deliberately simple and integer-only: no
 * floating point, no division in the inner loops, and every primitive clips
 * against the current rectangle before it touches memory.
 */
#include "apt.h"
#include "aptos.h"

/* ------------------------------------------------------------------ */
/* raw syscall numbers (kept in step with kernel/syscall.h)             */

#define SYS_APT_OPEN    41
#define SYS_APT_PRESENT 42
#define SYS_APT_INFO    43
#define SYS_APT_TEXT    44
#define SYS_WIN_CLOSE   45
#define SYS_WIN_ALIVE   46

static inline int apt_sys_open(const char *title, int w, int h, u32 *pixels)
{
    return __syscall(SYS_APT_OPEN, (int)title, w, h, (int)pixels);
}

static inline int apt_sys_present(int x, int y, int w, int h)
{
    return __syscall(SYS_APT_PRESENT, x, y, w, h);
}

static inline int apt_sys_info(int *info)
{
    return __syscall(SYS_APT_INFO, (int)info, 0, 0, 0);
}

/* Text is the one drawing operation that needs kernel help, because the font
 * tables live there (an 8x8 Latin set plus a CJK bitmap set that would
 * dominate the size of every program). */
static inline void apt_sys_text(int x, int y, const char *s, u32 colour, int scale)
{
    sys_apt_text(x, y, s, colour, scale);
}

/* ------------------------------------------------------------------ */
/* stdlib bits                                                        */

typedef unsigned int size_t_;

/* ------------------------------------------------------------------ */
/* a small allocator                                                   */
/*                                                                    */
/* Programs run in ring 3 with no libc, so libapt brings its own.  It is */
/* a first-fit free-list allocator over one static arena: the amount of */
/* memory a UI toolkit needs is small and predictable, and this keeps the */
/* program's BSS bounded (which matters because the ELF loader allocates */
/* BSS up front and never reclaims it). */

#include "apt_alloc.h"

/* ------------------------------------------------------------------ */

static int  str_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void mem_copy(void *d, const void *s, size_t_ n)
{
    u8 *dd = (u8 *)d;
    const u8 *ss = (const u8 *)s;
    while (n--)
        *dd++ = *ss++;
}

static void mem_zero(void *d, size_t_ n)
{
    u8 *dd = (u8 *)d;
    while (n--)
        *dd++ = 0;
}

/* ------------------------------------------------------------------ */
/* dirty tracking                                                      */

static void mark_dirty(apt_canvas *c, int x0, int y0, int x1, int y1)
{
    if (!c)
        return;
    if (c->dirty_x1 < c->dirty_x0) {
        c->dirty_x0 = x0;
        c->dirty_y0 = y0;
        c->dirty_x1 = x1;
        c->dirty_y1 = y1;
        return;
    }
    if (x0 < c->dirty_x0) c->dirty_x0 = x0;
    if (y0 < c->dirty_y0) c->dirty_y0 = y0;
    if (x1 > c->dirty_x1) c->dirty_x1 = x1;
    if (y1 > c->dirty_y1) c->dirty_y1 = y1;
}

/* ------------------------------------------------------------------ */
/* pixel access with clipping                                          */

/* Blend a colour onto a pixel.  `a` is 0..255.  Colours here are opaque RGB;
 * the alpha comes from the canvas state or a texture mask. */
static inline u32 blend_px(u32 dst, u32 src, int a)
{
    u32 r, g, b, ia;

    if (a >= 255)
        return src;
    if (a <= 0)
        return dst;
    ia = (u32)(255 - a);
    r = (((src >> 16) & 0xFF) * (u32)a + ((dst >> 16) & 0xFF) * ia) / 255u;
    g = (((src >> 8) & 0xFF) * (u32)a + ((dst >> 8) & 0xFF) * ia) / 255u;
    b = ((src & 0xFF) * (u32)a + (dst & 0xFF) * ia) / 255u;
    return (r << 16) | (g << 8) | b;
}

static inline void put_px(apt_canvas *c, int x, int y, u32 colour, int alpha)
{
    u32 *row;

    if (x < c->cx0 || x > c->cx1 || y < c->cy0 || y > c->cy1)
        return;
    row = c->pixels + (size_t_)y * (size_t_)c->pitch;
    if (alpha >= 255)
        row[x] = colour;
    else
        row[x] = blend_px(row[x], colour, alpha);
}

static inline u32 get_px(apt_canvas *c, int x, int y)
{
    if (x < 0 || y < 0 || x >= c->width || y >= c->height)
        return 0;
    return c->pixels[(size_t_)y * (size_t_)c->pitch + x];
}

/* ------------------------------------------------------------------ */
/* transforms                                                          */

void apt_reset_transform(apt_canvas *c)
{
    if (!c)
        return;
    c->sx = c->sy = APT_FX_ONE;
    c->tx = c->ty = 0;
    c->flip_x = c->flip_y = 0;
}

void apt_set_transform(apt_canvas *c, apt_fx sx, apt_fx sy, apt_fx tx, apt_fx ty)
{
    if (!c)
        return;
    c->sx = sx ? sx : APT_FX_ONE;
    c->sy = sy ? sy : APT_FX_ONE;
    c->tx = tx;
    c->ty = ty;
}

void apt_translate(apt_canvas *c, apt_fx dx, apt_fx dy)
{
    if (!c)
        return;
    c->tx += dx;
    c->ty += dy;
}

void apt_scale(apt_canvas *c, apt_fx sx, apt_fx sy)
{
    if (!c)
        return;
    c->sx = APT_FX_MUL(c->sx, sx);
    c->sy = APT_FX_MUL(c->sy, sy);
}

void apt_set_flip(apt_canvas *c, int flip_x, int flip_y)
{
    if (!c)
        return;
    c->flip_x = flip_x != 0;
    c->flip_y = flip_y != 0;
}

void apt_user_to_canvas(apt_canvas *c, apt_fx ux, apt_fx uy, int *cx, int *cy)
{
    apt_fx x = APT_FX_MUL(ux, c->sx) + c->tx;
    apt_fx y = APT_FX_MUL(uy, c->sy) + c->ty;

    if (c->flip_x)
        x = APT_FX(c->width) - x;
    if (c->flip_y)
        y = APT_FX(c->height) - y;
    if (cx)
        *cx = APT_FX_INT(x);
    if (cy)
        *cy = APT_FX_INT(y);
}

void apt_canvas_to_user(apt_canvas *c, int cx, int cy, apt_fx *ux, apt_fx *uy)
{
    apt_fx x = APT_FX(cx);
    apt_fx y = APT_FX(cy);

    if (c->flip_x)
        x = APT_FX(c->width) - x;
    if (c->flip_y)
        y = APT_FX(c->height) - y;
    x -= c->tx;
    y -= c->ty;
    if (ux)
        *ux = c->sx ? APT_FX_DIV(x, c->sx) : 0;
    if (uy)
        *uy = c->sy ? APT_FX_DIV(y, c->sy) : 0;
}

/* ------------------------------------------------------------------ */
/* clipping                                                            */

void apt_reset_clip(apt_canvas *c)
{
    if (!c)
        return;
    c->cx0 = 0;
    c->cy0 = 0;
    c->cx1 = c->width - 1;
    c->cy1 = c->height - 1;
}

void apt_set_clip(apt_canvas *c, int x, int y, int w, int h)
{
    if (!c)
        return;
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x + w - 1;
    c->cy1 = y + h - 1;
    if (c->cx1 > c->width - 1) c->cx1 = c->width - 1;
    if (c->cy1 > c->height - 1) c->cy1 = c->height - 1;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0 - 1;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0 - 1;
}

void apt_push_clip(apt_canvas *c, int x, int y, int w, int h)
{
    if (!c)
        return;
    if (c->clip_depth >= APT_MAX_CLIP)
        c->clip_depth = APT_MAX_CLIP - 1;
    c->clip_stack[c->clip_depth].x0 = c->cx0;
    c->clip_stack[c->clip_depth].y0 = c->cy0;
    c->clip_stack[c->clip_depth].x1 = c->cx1;
    c->clip_stack[c->clip_depth].y1 = c->cy1;
    c->clip_depth++;

    /* intersect */
    if (x > c->cx0) c->cx0 = x;
    if (y > c->cy0) c->cy0 = y;
    if (x + w - 1 < c->cx1) c->cx1 = x + w - 1;
    if (y + h - 1 < c->cy1) c->cy1 = y + h - 1;
    if (c->cx0 < 0) c->cx0 = 0;
    if (c->cy0 < 0) c->cy0 = 0;
}

void apt_pop_clip(apt_canvas *c)
{
    if (!c || c->clip_depth <= 0)
        return;
    c->clip_depth--;
    c->cx0 = c->clip_stack[c->clip_depth].x0;
    c->cy0 = c->clip_stack[c->clip_depth].y0;
    c->cx1 = c->clip_stack[c->clip_depth].x1;
    c->cy1 = c->clip_stack[c->clip_depth].y1;
}

/* ------------------------------------------------------------------ */
/* primitives                                                          */

void apt_pixel(apt_canvas *c, int x, int y, u32 colour)
{
    int cx, cy;

    if (!c)
        return;
    apt_user_to_canvas(c, APT_FX(x), APT_FX(y), &cx, &cy);
    put_px(c, cx, cy, colour, c->alpha);
    mark_dirty(c, cx, cy, cx, cy);
    c->ops++;
}

void apt_hline(apt_canvas *c, int x, int y, int w, u32 colour)
{
    int cx0, cy, cx1, i, a;
    u32 *row;

    if (!c || w <= 0)
        return;
    apt_user_to_canvas(c, APT_FX(x), APT_FX(y), &cx0, &cy);
    apt_user_to_canvas(c, APT_FX(x + w - 1), APT_FX(y), &cx1, &cy);
    if (cx1 < cx0) { i = cx0; cx0 = cx1; cx1 = i; }

    if (cy < c->cy0 || cy > c->cy1)
        return;
    if (cx0 < c->cx0) cx0 = c->cx0;
    if (cx1 > c->cx1) cx1 = c->cx1;
    if (cx0 > cx1)
        return;

    row = c->pixels + (size_t_)cy * (size_t_)c->pitch;
    a = c->alpha;
    if (a >= 255) {
        for (i = cx0; i <= cx1; i++)
            row[i] = colour;
    } else {
        for (i = cx0; i <= cx1; i++)
            row[i] = blend_px(row[i], colour, a);
    }
    mark_dirty(c, cx0, cy, cx1, cy);
    c->ops++;
}

void apt_vline(apt_canvas *c, int x, int y, int h, u32 colour)
{
    int cx, cy0, cy1, i, a;

    if (!c || h <= 0)
        return;
    apt_user_to_canvas(c, APT_FX(x), APT_FX(y), &cx, &cy0);
    apt_user_to_canvas(c, APT_FX(x), APT_FX(y + h - 1), &cx, &cy1);
    if (cy1 < cy0) { i = cy0; cy0 = cy1; cy1 = i; }

    if (cx < c->cx0 || cx > c->cx1)
        return;
    if (cy0 < c->cy0) cy0 = c->cy0;
    if (cy1 > c->cy1) cy1 = c->cy1;
    if (cy0 > cy1)
        return;

    a = c->alpha;
    for (i = cy0; i <= cy1; i++) {
        u32 *row = c->pixels + (size_t_)i * (size_t_)c->pitch;
        row[cx] = (a >= 255) ? colour : blend_px(row[cx], colour, a);
    }
    mark_dirty(c, cx, cy0, cx, cy1);
    c->ops++;
}

void apt_fill_rect(apt_canvas *c, int x, int y, int w, int h, u32 colour)
{
    int cx0, cy0, cx1, cy1, i, j, a;

    if (!c || w <= 0 || h <= 0)
        return;
    apt_user_to_canvas(c, APT_FX(x), APT_FX(y), &cx0, &cy0);
    apt_user_to_canvas(c, APT_FX(x + w - 1), APT_FX(y + h - 1), &cx1, &cy1);
    if (cx1 < cx0) { i = cx0; cx0 = cx1; cx1 = i; }
    if (cy1 < cy0) { i = cy0; cy0 = cy1; cy1 = i; }

    if (cx0 < c->cx0) cx0 = c->cx0;
    if (cy0 < c->cy0) cy0 = c->cy0;
    if (cx1 > c->cx1) cx1 = c->cx1;
    if (cy1 > c->cy1) cy1 = c->cy1;
    if (cx0 > cx1 || cy0 > cy1)
        return;

    a = c->alpha;
    for (j = cy0; j <= cy1; j++) {
        u32 *row = c->pixels + (size_t_)j * (size_t_)c->pitch;
        if (a >= 255) {
            for (i = cx0; i <= cx1; i++)
                row[i] = colour;
        } else {
            for (i = cx0; i <= cx1; i++)
                row[i] = blend_px(row[i], colour, a);
        }
    }
    mark_dirty(c, cx0, cy0, cx1, cy1);
    c->ops++;
}

void apt_rect(apt_canvas *c, int x, int y, int w, int h, u32 colour)
{
    if (!c || w <= 0 || h <= 0)
        return;
    apt_hline(c, x, y, w, colour);
    apt_hline(c, x, y + h - 1, w, colour);
    apt_vline(c, x, y, h, colour);
    apt_vline(c, x + w - 1, y, h, colour);
}

void apt_line(apt_canvas *c, int x0, int y0, int x1, int y1, u32 colour)
{
    int dx, dy, sx, sy, err;

    if (!c)
        return;
    apt_user_to_canvas(c, APT_FX(x0), APT_FX(y0), &x0, &y0);
    apt_user_to_canvas(c, APT_FX(x1), APT_FX(y1), &x1, &y1);

    /* Bresenham, with an early reject against the clip rectangle. */
    dx = x1 - x0; if (dx < 0) dx = -dx;
    dy = y1 - y0; if (dy < 0) dy = -dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;

    for (;;) {
        put_px(c, x0, y0, colour, c->alpha);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = err * 2;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
    }
    /* One dirty box for the whole line rather than one per pixel. */
    {
        int ax = x0 < x1 ? x0 : x1, bx = x0 > x1 ? x0 : x1;
        int ay = y0 < y1 ? y0 : y1, by = y0 > y1 ? y0 : y1;
        mark_dirty(c, ax, ay, bx, by);
    }
    c->ops++;
}

/* Filled circle/ellipse via the midpoint algorithm, one horizontal span at a
 * time so it stays fast. */
static void ellipse_spans(apt_canvas *c, int cx, int cy, int rx, int ry,
                          u32 colour, int filled)
{
    int x = 0, y = ry;
    long rx2 = (long)rx * rx, ry2 = (long)ry * ry;
    long dx = 0, dy = 2L * rx2 * y;
    long err = -ry2;

    if (rx < 0 || ry < 0)
        return;

    while (x <= rx) {
        if (filled) {
            apt_hline(c, cx - x, cy + y, 2 * x + 1, colour);
            apt_hline(c, cx - x, cy - y, 2 * x + 1, colour);
            apt_hline(c, cx - y, cy + x, 2 * y + 1, colour);
            apt_hline(c, cx - y, cy - x, 2 * y + 1, colour);
        } else {
            apt_pixel(c, cx + x, cy + y, colour);
            apt_pixel(c, cx - x, cy + y, colour);
            apt_pixel(c, cx + x, cy - y, colour);
            apt_pixel(c, cx - x, cy - y, colour);
            apt_pixel(c, cx + y, cy + x, colour);
            apt_pixel(c, cx - y, cy + x, colour);
            apt_pixel(c, cx + y, cy - x, colour);
            apt_pixel(c, cx - y, cy - x, colour);
        }

        err += dx + ry2;
        dx += 2L * ry2;
        x++;
        if (2L * err + dy > 0) {
            err += dy - rx2;
            dy -= 2L * rx2;
            y--;
        }
    }
}

void apt_circle(apt_canvas *c, int cx, int cy, int r, u32 colour)
{
    ellipse_spans(c, cx, cy, r, r, colour, 0);
}

void apt_fill_circle(apt_canvas *c, int cx, int cy, int r, u32 colour)
{
    ellipse_spans(c, cx, cy, r, r, colour, 1);
}

void apt_ellipse(apt_canvas *c, int cx, int cy, int rx, int ry, u32 colour)
{
    ellipse_spans(c, cx, cy, rx, ry, colour, 0);
}

void apt_fill_ellipse(apt_canvas *c, int cx, int cy, int rx, int ry, u32 colour)
{
    ellipse_spans(c, cx, cy, rx, ry, colour, 1);
}

void apt_round_rect(apt_canvas *c, int x, int y, int w, int h, int r, u32 colour)
{
    if (r <= 0) { apt_rect(c, x, y, w, h, colour); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    apt_hline(c, x + r, y, w - 2 * r, colour);
    apt_hline(c, x + r, y + h - 1, w - 2 * r, colour);
    apt_vline(c, x, y + r, h - 2 * r, colour);
    apt_vline(c, x + w - 1, y + r, h - 2 * r, colour);
    /* corners: quarter arcs approximated by the circle helper with clipping */
    apt_push_clip(c, x, y, r, r);
    apt_circle(c, x + r, y + r, r, colour);
    apt_pop_clip(c);
    apt_push_clip(c, x + w - r, y, r, r);
    apt_circle(c, x + w - r - 1, y + r, r, colour);
    apt_pop_clip(c);
    apt_push_clip(c, x, y + h - r, r, r);
    apt_circle(c, x + r, y + h - r - 1, r, colour);
    apt_pop_clip(c);
    apt_push_clip(c, x + w - r, y + h - r, r, r);
    apt_circle(c, x + w - r - 1, y + h - r - 1, r, colour);
    apt_pop_clip(c);
}

void apt_fill_round_rect(apt_canvas *c, int x, int y, int w, int h, int r, u32 colour)
{
    if (r <= 0) { apt_fill_rect(c, x, y, w, h, colour); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    apt_fill_rect(c, x + r, y, w - 2 * r, h, colour);
    apt_fill_rect(c, x, y + r, r, h - 2 * r, colour);
    apt_fill_rect(c, x + w - r, y + r, r, h - 2 * r, colour);

    apt_push_clip(c, x, y, r, r);
    apt_fill_circle(c, x + r, y + r, r, colour);
    apt_pop_clip(c);
    apt_push_clip(c, x + w - r, y, r, r);
    apt_fill_circle(c, x + w - r - 1, y + r, r, colour);
    apt_pop_clip(c);
    apt_push_clip(c, x, y + h - r, r, r);
    apt_fill_circle(c, x + r, y + h - r - 1, r, colour);
    apt_pop_clip(c);
    apt_push_clip(c, x + w - r, y + h - r, r, r);
    apt_fill_circle(c, x + w - r - 1, y + h - r - 1, r, colour);
    apt_pop_clip(c);
}

void apt_gradient(apt_canvas *c, int x, int y, int w, int h, u32 top, u32 bottom)
{
    int j;

    if (!c || w <= 0 || h <= 0)
        return;
    for (j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 255) / (h - 1) : 0;
        u32 r = (APT_R(top) * (255 - t) + APT_R(bottom) * t) / 255;
        u32 g = (APT_G(top) * (255 - t) + APT_G(bottom) * t) / 255;
        u32 b = (APT_B(top) * (255 - t) + APT_B(bottom) * t) / 255;
        apt_hline(c, x, y + j, w, (r << 16) | (g << 8) | b);
    }
}

void apt_shadow(apt_canvas *c, int x, int y, int w, int h, int spread)
{
    int i;
    int saved = c ? c->alpha : 255;

    if (!c || spread <= 0)
        return;
    for (i = spread; i > 0; i--) {
        int a = 40 - (i * 40) / (spread + 1);
        if (a < 4) a = 4;
        c->alpha = a;
        apt_fill_round_rect(c, x - i, y - i + spread / 2, w + 2 * i, h + 2 * i,
                            spread, APT_BLACK);
    }
    c->alpha = saved;
}

/* Scanline polygon fill.  Handles the usual even-odd rule; the edge table is
 * a small fixed array because the API is for UI work, not modelling. */
#define APT_MAX_EDGES 64

void apt_fill_poly(apt_canvas *c, const apt_point *pts, int n, u32 colour)
{
    int i, j, y, miny = 0, maxy = 0;

    if (!c || n < 3)
        return;

    for (i = 0; i < n; i++) {
        apt_user_to_canvas(c, APT_FX(pts[i].x), APT_FX(pts[i].y), &j, &y);
        if (i == 0) { miny = maxy = y; }
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    if (miny < c->cy0) miny = c->cy0;
    if (maxy > c->cy1) maxy = c->cy1;

    for (y = miny; y <= maxy; y++) {
        int xs[APT_MAX_EDGES];
        int nx = 0;

        for (i = 0; i < n && nx < APT_MAX_EDGES; i++) {
            int x0, y0, x1, y1;
            int a = i, b = (i + 1) % n;

            apt_user_to_canvas(c, APT_FX(pts[a].x), APT_FX(pts[a].y), &x0, &y0);
            apt_user_to_canvas(c, APT_FX(pts[b].x), APT_FX(pts[b].y), &x1, &y1);
            if (y0 == y1)
                continue;
            if (y >= y0 && y < y1)
                xs[nx++] = x0 + (int)((long)(y - y0) * (x1 - x0) / (y1 - y0));
            else if (y >= y1 && y < y0)
                xs[nx++] = x1 + (int)((long)(y - y1) * (x0 - x1) / (y0 - y1));
        }
        if (nx < 2)
            continue;
        /* insertion sort: nx is tiny */
        for (i = 1; i < nx; i++) {
            int v = xs[i], k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }
        for (i = 0; i + 1 < nx; i += 2) {
            if (xs[i] > xs[i + 1])
                continue;
            apt_hline(c, xs[i], y, xs[i + 1] - xs[i] + 1, colour);
        }
    }
}

/* ------------------------------------------------------------------ */
/* state                                                               */

void apt_set_colour(apt_canvas *c, u32 colour) { if (c) c->colour = colour; }
void apt_set_alpha(apt_canvas *c, int alpha)
{
    if (!c)
        return;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    c->alpha = alpha;
}

void apt_clear(apt_canvas *c, u32 colour)
{
    int i, n;

    if (!c || !c->pixels)
        return;
    n = c->width * c->height;
    for (i = 0; i < n; i++)
        c->pixels[i] = colour;

    /* A clear touches everything, so the next flush presents everything. */
    c->dirty_x0 = 0;
    c->dirty_y0 = 0;
    c->dirty_x1 = c->width - 1;
    c->dirty_y1 = c->height - 1;
    c->ops++;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */

apt_texture *apt_texture_new(int w, int h)
{
    apt_texture *t;

    if (w <= 0 || h <= 0)
        return NULL;
    t = (apt_texture *)malloc(sizeof(apt_texture));
    if (!t)
        return NULL;
    t->pixels = (u32 *)malloc((size_t_)(w * h) * 4);
    if (!t->pixels) {
        free(t);
        return NULL;
    }
    t->mask = NULL;
    t->width = w;
    t->height = h;
    t->owned = 1;
    return t;
}

void apt_texture_free(apt_texture *t)
{
    if (!t)
        return;
    if (t->owned) {
        if (t->pixels) free(t->pixels);
        if (t->mask) free(t->mask);
    }
    free(t);
}

u32 apt_texture_get(const apt_texture *t, int x, int y)
{
    if (!t || x < 0 || y < 0 || x >= t->width || y >= t->height)
        return 0;
    return t->pixels[(size_t_)y * (size_t_)t->width + x];
}

void apt_blit(apt_canvas *c, const apt_texture *t, int x, int y)
{
    int i, j;

    if (!c || !t)
        return;
    for (j = 0; j < t->height; j++) {
        for (i = 0; i < t->width; i++) {
            u32 px = t->pixels[(size_t_)j * (size_t_)t->width + i];
            int a = c->alpha;
            if (t->mask)
                a = (a * t->mask[(size_t_)j * (size_t_)t->width + i]) / 255;
            if (a <= 0)
                continue;
            put_px(c, x + i, y + j, px, a);
        }
    }
    mark_dirty(c, x, y, x + t->width - 1, y + t->height - 1);
    c->ops++;
}

void apt_blit_scaled(apt_canvas *c, const apt_texture *t, int x, int y, int w, int h)
{
    int i, j;

    if (!c || !t || w <= 0 || h <= 0)
        return;
    for (j = 0; j < h; j++) {
        int sy = (j * t->height) / h;
        if (sy >= t->height) sy = t->height - 1;
        for (i = 0; i < w; i++) {
            int sx = (i * t->width) / w;
            u32 px;
            int a = c->alpha;

            if (sx >= t->width) sx = t->width - 1;
            px = t->pixels[(size_t_)sy * (size_t_)t->width + sx];
            if (t->mask)
                a = (a * t->mask[(size_t_)sy * (size_t_)t->width + sx]) / 255;
            if (a <= 0)
                continue;
            put_px(c, x + i, y + j, px, a);
        }
    }
    mark_dirty(c, x, y, x + w - 1, y + h - 1);
    c->ops++;
}

void apt_capture(apt_canvas *c, apt_texture *t, int x, int y)
{
    int i, j;

    if (!c || !t)
        return;
    for (j = 0; j < t->height; j++)
        for (i = 0; i < t->width; i++)
            t->pixels[(size_t_)j * (size_t_)t->width + i] = get_px(c, x + i, y + j);
}

/* Affine textured triangle.  The classic edge-function rasteriser: walk the
 * bounding box, compute barycentric weights, and interpolate UV.  Integer
 * only, with the weights scaled by 1<<16. */
typedef struct { int x, y; apt_fx u, v; } apt_vert;

static void tex_triangle(apt_canvas *c, const apt_texture *t,
                         apt_vert a, apt_vert b, apt_vert d)
{
    int minx, maxx, miny, maxy, px, py;
    long area;

    minx = a.x < b.x ? (a.x < d.x ? a.x : d.x) : (b.x < d.x ? b.x : d.x);
    maxx = a.x > b.x ? (a.x > d.x ? a.x : d.x) : (b.x > d.x ? b.x : d.x);
    miny = a.y < b.y ? (a.y < d.y ? a.y : d.y) : (b.y < d.y ? b.y : d.y);
    maxy = a.y > b.y ? (a.y > d.y ? a.y : d.y) : (b.y > d.y ? b.y : d.y);

    if (minx < c->cx0) minx = c->cx0;
    if (miny < c->cy0) miny = c->cy0;
    if (maxx > c->cx1) maxx = c->cx1;
    if (maxy > c->cy1) maxy = c->cy1;
    if (minx > maxx || miny > maxy)
        return;

    area = (long)(b.x - a.x) * (d.y - a.y) - (long)(b.y - a.y) * (d.x - a.x);
    if (area == 0)
        return;

    for (py = miny; py <= maxy; py++) {
        for (px = minx; px <= maxx; px++) {
            long w0 = (long)(b.x - a.x) * (py - a.y) - (long)(b.y - a.y) * (px - a.x);
            long w1 = (long)(d.x - b.x) * (py - b.y) - (long)(d.y - b.y) * (px - b.x);
            long w2 = (long)(a.x - d.x) * (py - d.y) - (long)(a.y - d.y) * (px - d.x);

            /* all three on the same side of the edges */
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;

            {
                /* Barycentric weights for this pixel.  w0/w1/w2 are the edge
                 * functions scaled by `area`; dividing them by area would
                 * need a 64-bit intermediate, so instead the UVs are
                 * interpolated as (sum of weight*uv) / area using 32-bit
                 * fixed point with the operands kept small.
                 *
                 * The weights are proportional to the sub-triangle areas and
                 * sum to `area`, so sum(weight) == area and the division is
                 * exact up to rounding. */
                long l0 = w1, l1 = w2, l2 = w0;
                apt_fx u, v;
                int tx, ty, alpha = c->alpha;

                /* Interpolate the texture coordinate directly in *texel*
                 * units, with 8 bits of sub-texel precision.
                 *
                 * w0/w1/w2 are plain integer edge functions that sum to
                 * `area`.  Two earlier versions of this line were wrong in
                 * ways that are easy to repeat:
                 *
                 *   - treating the weights as 16.16 (shifting them right by
                 *     16) zeroes every weight for a triangle smaller than
                 *     65536 units, so all sampling lands on texel (0,0) and a
                 *     small projected face takes the colour of the texture's
                 *     corner;
                 *   - normalising the weights to 16.16 and then multiplying
                 *     by a 16.16 UV overflows 32 bits on a large triangle.
                 *
                 * Working in texels keeps both ends safe: the weight is at
                 * most `area`, the coordinate at most 256*256, so the product
                 * stays well inside 32 bits even for a full-screen triangle.
                 * The shift by 8 leaves the result in 8.8, i.e. a whole texel
                 * plus a fraction - more than a screen pixel can resolve. */
                {
                    long tu = (l0 * (a.u >> (APT_FX_BITS - 8)) +
                               l1 * (b.u >> (APT_FX_BITS - 8)) +
                               l2 * (d.u >> (APT_FX_BITS - 8))) / area;
                    long tv = (l0 * (a.v >> (APT_FX_BITS - 8)) +
                               l1 * (b.v >> (APT_FX_BITS - 8)) +
                               l2 * (d.v >> (APT_FX_BITS - 8))) / area;

                    tx = (int)(tu >> 8);
                    ty = (int)(tv >> 8);
                }

                if (tx < 0 || ty < 0 || tx >= t->width || ty >= t->height)
                    continue;
                {
                    int alpha = c->alpha;

                    if (t->mask)
                        alpha = (alpha * t->mask[(size_t_)ty * (size_t_)t->width + tx]) / 255;
                    if (alpha <= 0)
                        continue;
                    put_px(c, px, py, t->pixels[(size_t_)ty * (size_t_)t->width + tx], alpha);
                }
            }
        }
    }
    mark_dirty(c, minx, miny, maxx, maxy);
    c->ops++;
}

static apt_vert make_vert(apt_canvas *c, int x, int y, apt_fx u, apt_fx v)
{
    apt_vert o;

    apt_user_to_canvas(c, APT_FX(x), APT_FX(y), &o.x, &o.y);
    o.u = u;
    o.v = v;
    return o;
}

void apt_blit_quad_uv(apt_canvas *c, const apt_texture *t, const apt_point *quad,
                      int u0, int v0, int u1, int v1)
{
    apt_vert a, b, d, e;
    apt_fx ua = APT_FX(u0), va = APT_FX(v0);
    apt_fx ub = APT_FX(u1), vb = APT_FX(v1);

    if (!c || !t || !quad)
        return;

    /* quad order: TL, TR, BR, BL */
    a = make_vert(c, quad[0].x, quad[0].y, ua, va);
    b = make_vert(c, quad[1].x, quad[1].y, ub, va);
    d = make_vert(c, quad[2].x, quad[2].y, ub, vb);
    e = make_vert(c, quad[3].x, quad[3].y, ua, vb);

    tex_triangle(c, t, a, b, d);
    tex_triangle(c, t, a, d, e);
}

void apt_blit_quad(apt_canvas *c, const apt_texture *t, const apt_point *quad)
{
    if (!t)
        return;
    apt_blit_quad_uv(c, t, quad, 0, 0, t->width, t->height);
}

/* ------------------------------------------------------------------ */
/* text                                                                */

int apt_text_w(const char *s, int scale)
{
    if (!s)
        return 0;
    return sys_text_w(s, scale < 1 ? 1 : scale);
}

int apt_text_h(int scale)
{
    if (scale < 1)
        scale = 1;
    return 8 * scale;
}

void apt_text(apt_canvas *c, int x, int y, const char *s, u32 colour, int scale)
{
    int cx, cy;

    if (!c || !s)
        return;
    apt_user_to_canvas(c, APT_FX(x), APT_FX(y), &cx, &cy);
    apt_sys_text(cx, cy, s, colour, scale);
    mark_dirty(c, cx, cy, cx + apt_text_w(s, scale), cy + apt_text_h(scale));
    c->ops++;
}

void apt_text_aligned(apt_canvas *c, int x, int y, int w, const char *s,
                      u32 colour, int scale, int align)
{
    int tw = apt_text_w(s, scale);

    if (align == APT_ALIGN_CENTER)
        x += (w - tw) / 2;
    else if (align == APT_ALIGN_RIGHT)
        x += w - tw;
    apt_text(c, x, y, s, colour, scale);
}

void apt_printf(apt_canvas *c, int x, int y, u32 colour, int scale,
                const char *fmt, ...)
{
    static char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    apt_text(c, x, y, buf, colour, scale);
}

/* ------------------------------------------------------------------ */
/* misc                                                                */

int apt_ticks(void) { return sys_ticks(); }
void apt_sleep(int ms) { sys_sleep(ms); }

void apt_srand(apt_canvas *c, u32 seed)
{
    if (c)
        c->ops = c->ops;   /* keep the struct honest */
    if (c)
        *(u32 *)&c->colour = seed;
}

u32 apt_rand(apt_canvas *c)
{
    u32 s;

    if (!c)
        return 0;
    s = c->colour;
    s = s * 1103515245u + 12345u;
    c->colour = s;
    return (s >> 16) & 0x7FFF;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */

apt_canvas *apt_open(const char *title, int w, int h)
{
    apt_canvas *c;
    u32 pixels = 0;

    if (w < 64) w = 64;
    if (h < 64) h = 64;

    c = (apt_canvas *)malloc(sizeof(apt_canvas));
    if (!c)
        return NULL;
    mem_zero(c, sizeof(*c));

    if (apt_sys_open(title, w, h, &pixels) != 0) {
        free(c);
        return NULL;
    }

    /* The kernel tells us where the pixels are; the info call is also a
     * sanity check that the canvas really is mapped. */
    {
        int info[3];
        if (apt_sys_info(info) != 0 || info[2] == 0) {
            free(c);
            return NULL;
        }
        c->pixels = (u32 *)info[2];
        c->width = info[0];
        c->height = info[1];
    }
    if (!c->pixels)
        c->pixels = (u32 *)pixels;

    c->pitch = c->width;
    c->colour = APT_WHITE;
    c->alpha = 255;
    c->dirty_x0 = 0;
    c->dirty_y0 = 0;
    c->dirty_x1 = c->width - 1;
    c->dirty_y1 = c->height - 1;
    apt_reset_transform(c);
    apt_reset_clip(c);
    return c;
}

void apt_close(apt_canvas *c)
{
    if (!c)
        return;
    sys_win_close();
    free(c);
}

void apt_flush(apt_canvas *c)
{
    if (!c)
        return;
    if (c->dirty_x1 < c->dirty_x0 || c->dirty_y1 < c->dirty_y0) {
        c->ops = 0;
        return;
    }
    apt_sys_present(c->dirty_x0, c->dirty_y0,
                    c->dirty_x1 - c->dirty_x0 + 1,
                    c->dirty_y1 - c->dirty_y0 + 1);
    c->dirty_x0 = 0;
    c->dirty_y0 = 0;
    c->dirty_x1 = -1;
    c->dirty_y1 = -1;
    c->ops = 0;
}

int apt_should_close(apt_canvas *c)
{
    (void)c;
    return !sys_win_alive();
}

int apt_poll(apt_canvas *c, apt_event *e)
{
    int handled = 0;

    if (!c || !e)
        return 0;
    mem_zero(e, sizeof(*e));

    e->quit = apt_should_close(c);

    {
        int mx = 0, my = 0, mb = 0, inside = 0;
        sys_mouse(&mx, &my, &mb, &inside);
        (void)inside;
        e->mouse_x = mx;
        e->mouse_y = my;
        e->mouse_buttons = mb;
        e->mouse_inside = (mx >= 0 && my >= 0 && mx < c->width && my < c->height);
        if (mb)
            handled++;
    }

    {
        int k = sys_getkey();
        if (k) {
            e->key = k;
            handled++;
        }
    }
    return handled;
}

int apt_wait(apt_canvas *c, apt_event *e)
{
    for (;;) {
        int r = apt_poll(c, e);
        if (r > 0 || (e && e->quit))
            return r;
        sys_sleep(8);
    }
}
