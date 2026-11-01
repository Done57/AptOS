#include "gfx.h"
#include "bootinfo.h"
#include "font8x8.h"
#include "fontcjk.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "util.h"

static u32  *back;
static u8   *lfb;
static int   width, height;
static int   pitch;
static int   bpp;
static rect_t clip;

/* saved state for gfx_push_target()/gfx_pop_target() */
static u32  *target_saved_back;
static int   target_saved_w, target_saved_h;
static rect_t target_saved_clip;

int gfx_width(void)  { return width; }
int gfx_height(void) { return height; }
u32 gfx_bpp(void)    { return (u32)bpp; }
u32 *gfx_backbuffer(void) { return back; }

/* Temporarily draw into somebody else's buffer.  The apt layer uses this to
 * reuse the glyph renderer for text written straight into a shared canvas
 * instead of duplicating the font tables in every program.  Returns the
 * previous target so the caller can restore it. */
void gfx_push_target(u32 *pixels, int w, int h, rect_t new_clip)
{
    target_saved_back = back;
    target_saved_w = width;
    target_saved_h = height;
    target_saved_clip = clip;

    back = pixels;
    width = w;
    height = h;
    pitch = w;
    clip = new_clip;
}

void gfx_pop_target(void)
{
    back = target_saved_back;
    width = target_saved_w;
    height = target_saved_h;
    clip = target_saved_clip;
}

rect_t gfx_screen_rect(void)
{
    rect_t r;
    r.x0 = 0;
    r.y0 = 0;
    r.x1 = width - 1;
    r.y1 = height - 1;
    return r;
}

static void set_vga332_palette(void)
{
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        u8 r = (u8)((i >> 5) & 0x07);
        u8 g = (u8)((i >> 2) & 0x07);
        u8 b = (u8)(i & 0x03);
        outb(0x3C9, (u8)(r * 255 / 7));
        outb(0x3C9, (u8)(g * 255 / 7));
        outb(0x3C9, (u8)(b * 255 / 3));
    }
}

void gfx_init(void)
{
    lfb    = (u8 *)(u32)BOOTINFO->fb_addr;
    width  = BOOTINFO->fb_width;
    height = BOOTINFO->fb_height;
    pitch  = BOOTINFO->fb_pitch;
    bpp    = BOOTINFO->fb_bpp;

    if (width <= 0 || height <= 0) {
        width = 640;
        height = 480;
        pitch = 640 * 4;
        bpp = 32;
    }
    if (pitch < width * (bpp / 8))
        pitch = width * (bpp / 8);

    back = (u32 *)kmalloc((size_t)width * (size_t)height * 4);
    if (!back)
        for (;;)
            hlt();
    memset(back, 0, (size_t)width * (size_t)height * 4);

    if (bpp == 8)
        set_vga332_palette();

    gfx_reset_clip();
    kprintf("[gfx] %dx%d %dbpp, framebuffer @ 0x%x, pitch %d\n",
            width, height, bpp, BOOTINFO->fb_addr, pitch);
    kprintf("[gfx] backbuffer @ 0x%x (%d bytes)\n", (u32)back,
            width * height * 4);
}

void gfx_set_clip(int x0, int y0, int x1, int y1)
{
    clip.x0 = x0 < 0 ? 0 : x0;
    clip.y0 = y0 < 0 ? 0 : y0;
    clip.x1 = x1 > width - 1 ? width - 1 : x1;
    clip.y1 = y1 > height - 1 ? height - 1 : y1;
}

void gfx_set_clip_rect(rect_t r) { gfx_set_clip(r.x0, r.y0, r.x1, r.y1); }
rect_t gfx_get_clip(void) { return clip; }
void gfx_reset_clip(void) { gfx_set_clip(0, 0, width - 1, height - 1); }

void gfx_pixel(int x, int y, u32 color)
{
    if (x < clip.x0 || x > clip.x1 || y < clip.y0 || y > clip.y1)
        return;
    back[y * width + x] = color;
}

void gfx_clear(u32 color)
{
    u32 *p = back;
    u32 n = (u32)width * (u32)height;
    while (n--)
        *p++ = color;
}

void gfx_hline(int x, int y, int w, u32 color)
{
    if (y < clip.y0 || y > clip.y1 || w <= 0)
        return;
    if (x < clip.x0) {
        w -= clip.x0 - x;
        x = clip.x0;
    }
    if (x + w - 1 > clip.x1)
        w = clip.x1 - x + 1;
    if (w <= 0)
        return;
    u32 *p = back + y * width + x;
    for (int i = 0; i < w; i++)
        p[i] = color;
}

void gfx_vline(int x, int y, int h, u32 color)
{
    if (x < clip.x0 || x > clip.x1 || h <= 0)
        return;
    if (y < clip.y0) {
        h -= clip.y0 - y;
        y = clip.y0;
    }
    if (y + h - 1 > clip.y1)
        h = clip.y1 - y + 1;
    if (h <= 0)
        return;
    u32 *p = back + y * width + x;
    for (int i = 0; i < h; i++, p += width)
        *p = color;
}

void gfx_fill_rect(int x, int y, int w, int h, u32 color)
{
    for (int i = 0; i < h; i++)
        gfx_hline(x, y + i, w, color);
}

void gfx_rect(int x, int y, int w, int h, u32 color)
{
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_line(int x0, int y0, int x1, int y1, u32 color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_fill_circle(int cx, int cy, int r, u32 color)
{
    for (int y = -r; y <= r; y++) {
        int x = 0;
        while (x * x + y * y <= r * r)
            x++;
        gfx_hline(cx - (x - 1), cy + y, 2 * (x - 1) + 1, color);
    }
}

void gfx_gradient(int x, int y, int w, int h, u32 top, u32 bottom)
{
    if (h <= 0)
        return;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (int i = 0; i < h; i++) {
        int r = tr + (br - tr) * i / (h > 1 ? h - 1 : 1);
        int g = tg + (bg - tg) * i / (h > 1 ? h - 1 : 1);
        int b = tb + (bb - tb) * i / (h > 1 ? h - 1 : 1);
        gfx_hline(x, y + i, w, RGB(r, g, b));
    }
}

void gfx_blit(int dx, int dy, int w, int h, const u32 *src)
{
    for (int j = 0; j < h; j++) {
        int py = dy + j;
        if (py < clip.y0 || py > clip.y1)
            continue;
        const u32 *s = src + j * w;
        for (int i = 0; i < w; i++) {
            int px = dx + i;
            if (px < clip.x0 || px > clip.x1)
                continue;
            back[py * width + px] = s[i];
        }
    }
}

u32 gfx_blend(u32 fg, u32 bg, int alpha)
{
    if (alpha <= 0)
        return bg;
    if (alpha >= 255)
        return fg;
    u32 r = ((((fg >> 16) & 0xFF) * (u32)alpha + ((bg >> 16) & 0xFF) * (u32)(255 - alpha)) / 255) & 0xFF;
    u32 g = ((((fg >> 8) & 0xFF) * (u32)alpha + ((bg >> 8) & 0xFF) * (u32)(255 - alpha)) / 255) & 0xFF;
    u32 b = (((fg & 0xFF) * (u32)alpha + (bg & 0xFF) * (u32)(255 - alpha)) / 255) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

void gfx_blit_alpha(int dx, int dy, int w, int h, const u32 *src, int alpha)
{
    for (int j = 0; j < h; j++) {
        int py = dy + j;
        if (py < clip.y0 || py > clip.y1)
            continue;
        for (int i = 0; i < w; i++) {
            int px = dx + i;
            if (px < clip.x0 || px > clip.x1)
                continue;
            back[py * width + px] = gfx_blend(src[j * w + i], back[py * width + px], alpha);
        }
    }
}

u32 gfx_utf8_decode(const char *s, int *adv)
{
    const unsigned char *p = (const unsigned char *)s;
    u32 cp;
    int n;

    if (!p || !p[0]) {
        if (adv)
            *adv = 0;
        return 0;
    }
    if (p[0] < 0x80) {
        if (adv)
            *adv = 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1Fu;
        n = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0Fu;
        n = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07u;
        n = 4;
    } else {
        if (adv)
            *adv = 1;
        return 0xFFFD;
    }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            if (adv)
                *adv = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (u32)(p[i] & 0x3F);
    }
    if (adv)
        *adv = n;
    return cp;
}

int gfx_glyph_w(u32 cp, int scale)
{
    if (scale < 1)
        scale = 1;
    return (cp < 0x80 ? FONT_W : FONT_CJK_W) * scale;
}

/* Vertical shift so a 16px tall CJK cell is centred on an 8px ASCII cell. */
#define CJK_DY ((FONT_CJK_H - FONT_H) / 2)

int gfx_glyph(int x, int y, u32 cp, u32 color, int scale)
{
    if (scale < 1)
        scale = 1;

    if (cp < 0x80) {
        const unsigned char *glyph = font8x8_basic[cp];
        int gw = FONT_W * scale;
        int gh = FONT_H * scale;

        if (x > clip.x1 || y > clip.y1 || x + gw - 1 < clip.x0 || y + gh - 1 < clip.y0)
            return gw;

        for (int row = 0; row < FONT_H; row++) {
            u8 bits = glyph[row];
            if (!bits)
                continue;
            for (int col = 0; col < FONT_W; col++) {
                if (!(bits & (1u << col)))
                    continue;
                if (scale == 1)
                    gfx_pixel(x + col, y + row, color);
                else
                    gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
        return gw;
    }

    {
        const unsigned short *rows = fontcjk_lookup(cp);
        int gw = FONT_CJK_W * scale;
        int gh = FONT_CJK_H * scale;
        int oy = y - CJK_DY * scale;

        if (x > clip.x1 || oy > clip.y1 || x + gw - 1 < clip.x0 || oy + gh - 1 < clip.y0)
            return gw;

        for (int row = 0; row < FONT_CJK_H; row++) {
            u16 bits = rows ? rows[row] : (u16)0;
            if (!rows) {
                /* no glyph: draw a hollow box so a missing character is visible */
                if (row == 0 || row == FONT_CJK_H - 1)
                    bits = 0xFFFF;
                else
                    bits = 0x8001;
            }
            if (!bits)
                continue;
            for (int col = 0; col < FONT_CJK_W; col++) {
                if (!(bits & (0x8000u >> col)))
                    continue;
                if (scale == 1)
                    gfx_pixel(x + col, oy + row, color);
                else
                    gfx_fill_rect(x + col * scale, oy + row * scale, scale, scale, color);
            }
        }
        return gw;
    }
}

int gfx_char(int x, int y, char ch, u32 color, int scale)
{
    return gfx_glyph(x, y, (u8)ch, color, scale);
}

int gfx_text(int x, int y, const char *s, u32 color, int scale)
{
    int gx = x;
    int adv = 0;

    if (scale < 1)
        scale = 1;
    if (!s)
        return 0;
    while (*s) {
        u32 cp = gfx_utf8_decode(s, &adv);
        if (adv <= 0)
            break;
        gfx_glyph(gx, y, cp, color, scale);
        gx += gfx_glyph_w(cp, scale);
        s += adv;
    }
    return gx - x;
}

int gfx_text_w(const char *s, int scale)
{
    int w = 0;
    int adv = 0;

    if (scale < 1)
        scale = 1;
    if (!s)
        return 0;
    while (*s) {
        u32 cp = gfx_utf8_decode(s, &adv);
        if (adv <= 0)
            break;
        w += gfx_glyph_w(cp, scale);
        s += adv;
    }
    return w;
}

int gfx_chars(const char *s)
{
    int n = 0;
    int adv = 0;

    if (!s)
        return 0;
    while (*s) {
        gfx_utf8_decode(s, &adv);
        if (adv <= 0)
            break;
        s += adv;
        n++;
    }
    return n;
}


void gfx_present(void)
{
    u32 n = (u32)width * (u32)height;

    switch (bpp) {
    case 32:
        if (pitch == width * 4) {
            rep_movsl(lfb, back, n);
        } else {
            for (int y = 0; y < height; y++)
                rep_movsl(lfb + (u32)y * pitch, back + (u32)y * width, (u32)width);
        }
        break;
    case 24:
        for (int y = 0; y < height; y++) {
            u8 *d = lfb + (u32)y * pitch;
            const u32 *s = back + (u32)y * width;
            for (int x = 0; x < width; x++) {
                u32 c = s[x];
                *d++ = (u8)(c & 0xFF);
                *d++ = (u8)((c >> 8) & 0xFF);
                *d++ = (u8)((c >> 16) & 0xFF);
            }
        }
        break;
    case 16:
        for (int y = 0; y < height; y++) {
            u16 *d = (u16 *)(lfb + (u32)y * pitch);
            const u32 *s = back + (u32)y * width;
            for (int x = 0; x < width; x++) {
                u32 c = s[x];
                d[x] = (u16)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) |
                             ((c & 0xF8) >> 3));
            }
        }
        break;
    case 8:
        for (int y = 0; y < height; y++) {
            u8 *d = lfb + (u32)y * pitch;
            const u32 *s = back + (u32)y * width;
            for (int x = 0; x < width; x++) {
                u32 c = s[x];
                d[x] = (u8)((((c >> 16) & 0xE0)) | (((c >> 8) & 0xE0) >> 3) |
                            ((c & 0xC0) >> 6));
            }
        }
        break;
    default:
        break;
    }
}

void gfx_present_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width)  w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int j = 0; j < h; j++) {
        const u32 *s = back + (y + j) * width + x;
        u8 *d = lfb + (u32)(y + j) * pitch + (u32)x * (bpp / 8);
        switch (bpp) {
        case 32:
            rep_movsl(d, s, (u32)w);
            break;
        case 24:
            for (int i = 0; i < w; i++) {
                u32 c = s[i];
                d[i * 3 + 0] = (u8)(c & 0xFF);
                d[i * 3 + 1] = (u8)((c >> 8) & 0xFF);
                d[i * 3 + 2] = (u8)((c >> 16) & 0xFF);
            }
            break;
        case 16:
            for (int i = 0; i < w; i++) {
                u32 c = s[i];
                ((u16 *)d)[i] = (u16)((((c >> 16) & 0xF8) << 8) |
                                      (((c >> 8) & 0xFC) << 3) | ((c & 0xF8) >> 3));
            }
            break;
        case 8:
            for (int i = 0; i < w; i++) {
                u32 c = s[i];
                d[i] = (u8)(((c >> 16) & 0xE0) | (((c >> 8) & 0xE0) >> 3) |
                            ((c & 0xC0) >> 6));
            }
            break;
        default:
            break;
        }
    }
}
