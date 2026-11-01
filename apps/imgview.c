/* imgview - image viewer for aptos
 *
 * Reads uncompressed Windows BMP files (1/4/8/24/32 bpp) and displays them
 * scaled to fit a window.  Usage:  run /APPS/IMGVIEW.APP <file.bmp>
 *
 * The decoder is deliberately small: BMP is the one raster format that can
 * be produced and understood without a compression library, which keeps the
 * viewer useful for files copied onto the aptos disk.
 */
typedef unsigned long long u64;

#include "aptos.h"
#include "i18n.h"

#define WIN_W 440
#define WIN_H 380
#define VIEW_W 400
#define VIEW_H 300
/* The window only ever shows VIEW_W x VIEW_H pixels, so the decoded image is
 * kept at that size and scaled on load.  A big static buffer would blow up
 * the ring 3 BSS, which the loader has to allocate and zero in one piece. */
#define MAX_PIXELS (VIEW_W * VIEW_H)

static u32 pixels[MAX_PIXELS];            /* decoded + pre-scaled, 0x00RRGGBB */
static u8 raw[512 * 1024];                /* compressed source file */

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 rd16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

/* Read only the dimensions out of a BMP header.  The viewer needs these
 * before it can decide the viewport size, and doing it here keeps bmp_decode
 * free of a "scale factor" special case. */
static int bmp_probe(const u8 *buf, int len, int *pw, int *ph)
{
    u32 w, h, bpp, compression;
    int top_down = 0;

    if (len < 54)
        return -1;
    if (buf[0] != 'B' || buf[1] != 'M')
        return -2;
    if (rd32(buf + 14) < 40)
        return -3;

    w = rd32(buf + 18);
    h = rd32(buf + 22);
    bpp = rd16(buf + 28);
    compression = rd32(buf + 30);

    if ((int)h < 0) { top_down = 1; h = (u32)(-(int)h); }
    (void)top_down;
    if (compression != 0)
        return -4;
    if (w == 0 || h == 0 || w > 8192 || h > 8192)
        return -5;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return -7;
    if (w * h > 64u * 1024u * 1024u)
        return -6;

    *pw = (int)w;
    *ph = (int)h;
    return 0;
}

/* Decode a BMP file image into `pixels`.  Returns 0 on success and fills
 * *pw / *ph with the dimensions. */
static int bmp_decode(const u8 *buf, int len, int *pw, int *ph,
                      int ds_w, int ds_h)
{
    u32 data_off, hdr_size, w, h, bpp, compression, colors, row_bytes;
    int top_down, y;

    if (len < 54)
        return -1;
    if (buf[0] != 'B' || buf[1] != 'M')
        return -2;                        /* not a BMP */

    data_off = rd32(buf + 10);
    hdr_size = rd32(buf + 14);
    if (hdr_size < 40)
        return -3;                        /* OS/2 or exotic header */

    w = rd32(buf + 18);
    h = rd32(buf + 22);
    bpp = rd16(buf + 28);
    compression = rd32(buf + 30);
    colors = rd32(buf + 46);

    top_down = 0;
    if ((int)h < 0) {                     /* negative height = top-down rows */
        top_down = 1;
        h = (u32)(-(int)h);
    }
    if (compression != 0)
        return -4;                        /* RLE / bitfields unsupported */
    if (w == 0 || h == 0 || w > 8192 || h > 8192)
        return -5;
    if (w > 8192u || h > 8192u)
        return -6;
    if (w * h > 64u * 1024u * 1024u)
        return -6;   /* refuse absurd pixel counts */
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return -7;

    if (!colors && bpp <= 8)
        colors = 1u << bpp;
    row_bytes = ((w * bpp + 31) / 32) * 4;   /* rows pad to 4 bytes */

    if (data_off + row_bytes * h > (u32)len)
        return -8;                        /* truncated */

    {
        const u8 *pal = buf + 14 + hdr_size;
        u32 pal_count = colors;

        if ((u32)(14 + hdr_size) + pal_count * 4 > (u32)len)
            return -9;

        for (y = 0; y < (int)h; y++) {
            /* In a normal BMP the bottom row is stored first. */
            const u8 *row = buf + data_off + (u32)(top_down ? y : (int)h - 1 - y) * row_bytes;
            u32 x;

            /* Destination row/column step: scale into the viewport as we go
             * so we never need a full-size staging buffer. */
            for (x = 0; x < w; x++) {
                u32 px;
                int dx = (x * ds_w) / w;
                int dy = (y * ds_h) / h;

                if (dx >= ds_w) dx = ds_w - 1;
                if (dy >= ds_h) dy = ds_h - 1;

                if (bpp == 24) {
                    const u8 *s = row + x * 3;
                    px = ((u32)s[2] << 16) | ((u32)s[1] << 8) | s[0];
                } else if (bpp == 32) {
                    const u8 *s = row + x * 4;
                    px = ((u32)s[2] << 16) | ((u32)s[1] << 8) | s[0];
                } else {
                    u32 idx;

                    if (bpp == 8) {
                        idx = row[x];
                    } else if (bpp == 4) {
                        u8 b = row[x / 2];
                        idx = (x & 1) ? (b & 0x0F) : (b >> 4);
                    } else {
                        u8 b = row[x / 8];
                        idx = (b >> (7 - (x & 7))) & 1;
                    }
                    if (idx >= pal_count)
                        idx = 0;
                    px = ((u32)pal[idx * 4 + 2] << 16) |
                         ((u32)pal[idx * 4 + 1] << 8) |
                         pal[idx * 4];
                }
                pixels[dy * ds_w + dx] = px;
            }
        }
    }

    *pw = (int)w;
    *ph = (int)h;
    return 0;
}

static const char *err_text(int e)
{
    switch (e) {
    case -1: return T("file is too small to be an image",
                      "文件太小，不是图像");
    case -2: return T("not a BMP file (missing 'BM' magic)",
                      "不是 BMP 文件（缺少 'BM' 标记）");
    case -3: return T("unsupported BMP header",
                      "不支持的 BMP 头部");
    case -4: return T("compressed BMP is not supported",
                      "不支持压缩的 BMP");
    case -5: return T("bad image dimensions", "图像尺寸无效");
    case -6: return T("image is too large to display",
                      "图像太大，无法显示");
    case -7: return T("unsupported colour depth",
                      "不支持的颜色深度");
    case -8: return T("file is truncated", "文件被截断");
    case -9: return T("colour palette is missing", "缺少调色板");
    default: return T("cannot read this image", "无法读取该图像");
    }
}

static int load_file(const char *path, u8 *buf, int max)
{
    int fd;
    int n;

    fd = sys_open(path, 0);
    if (fd < 0)
        return -1;
    n = sys_read(fd, buf, max);
    sys_close(fd);
    return n;
}

int main(void)
{
    char path[160];
    int w = 0, h = 0, err;
    int scaled;
    int vw, vh, vx, vy;
    int zoom = 1;
    int len;

    if (sys_win_open("Image Viewer", WIN_W, WIN_H) != 0) {
        printf("imgview: cannot open a window\n");
        return 1;
    }

    /* arg0 is the program path; arg1 is the file to open. */
    if (sys_args(1, path, sizeof(path)) != 0 || !path[0]) {
        sys_clear(0x101418);
        sys_text(16, 16, T("Image Viewer", "图片查看器"), 0xFFFFFF, 2);
        sys_text(16, 60, T("Usage:", "用法："), 0xC0C8D0, 1);
        sys_text(16, 80, "run /APPS/IMGVIEW.APP <file.bmp>", 0x80D0FF, 1);
        sys_text(16, 120, T("Supported: uncompressed BMP (1/4/8/24/32 bpp)",
                            "支持：未压缩 BMP（1/4/8/24/32 位）"), 0x9098A0, 1);
        sys_text(16, 150, T("Press any key to close.", "按任意键关闭。"), 0x9098A0, 1);
        sys_flush();
        while (sys_getkey() == 0)
            sys_sleep(30);
        return 0;
    }

    len = load_file(path, raw, sizeof(raw));
    if (len < 0) {
        sys_clear(0x101418);
        sys_text(16, 16, T("Image Viewer", "图片查看器"), 0xFFFFFF, 2);
        sys_text(16, 60, T("Cannot open file:", "无法打开文件："), 0xFF8080, 1);
        sys_text(16, 80, path, 0xFFFFFF, 1);
        sys_flush();
        while (sys_getkey() == 0)
            sys_sleep(30);
        return 1;
    }

    /* First pass: read the header only, so we know how to scale.  A tiny
     * resize of the same routine would need a second parser, so instead we
     * decode twice: once to learn the size, once to fill the viewport. */
    err = bmp_probe(raw, len, &w, &h);
    if (err != 0) {
        sys_clear(0x101418);
        sys_text(16, 16, T("Image Viewer", "图片查看器"), 0xFFFFFF, 2);
        sys_text(16, 60, path, 0xFFFFFF, 1);
        sys_text(16, 90, err_text(err), 0xFF8080, 1);
        sys_flush();
        while (sys_getkey() == 0)
            sys_sleep(30);
        return 1;
    }

    /* Fit to the viewport, never upscaling beyond 1:1. */
    scaled = 0;
    if (w <= VIEW_W && h <= VIEW_H) {
        vw = w;
        vh = h;
    } else {
        int zx = (VIEW_W * w) / (w ? w : 1);
        int zy = (VIEW_H * h) / (h ? h : 1);
        (void)zx; (void)zy;
        /* uniform scale: compare w/VIEW_W and h/VIEW_H without floats */
        if (w * VIEW_H > h * VIEW_W) {
            vw = VIEW_W;
            vh = (h * VIEW_W) / w;
        } else {
            vh = VIEW_H;
            vw = (w * VIEW_H) / h;
        }
        if (vw < 1) vw = 1;
        if (vh < 1) vh = 1;
        zoom = (vw * 100) / w;
        scaled = 1;
    }

    /* Decode straight into the viewport-sized buffer. */
    err = bmp_decode(raw, len, &w, &h, vw, vh);
    if (err != 0) {
        sys_clear(0x101418);
        sys_text(16, 16, "Image Viewer", 0xFFFFFF, 2);
        sys_text(16, 60, err_text(err), 0xFF8080, 1);
        sys_flush();
        while (sys_getkey() == 0)
            sys_sleep(30);
        return 1;
    }

    {
        vx = (VIEW_W - vw) / 2 + 10;
        vy = (VIEW_H - vh) / 2 + 34;
        if (vx < 0) vx = 0;
        if (vy < 34) vy = 34;

        for (;;) {
            int k = sys_getkey();

            if (k == 27 || k == 'q' || k == 'Q')
                break;

            sys_clear(0x101418);
            sys_text(10, 8, "Image Viewer", 0xFFFFFF, 1);
            sys_text(120, 8, path, 0x80D0FF, 1);
            {
                char info[96];
                snprintf(info, sizeof(info),
                         T("%dx%d px   zoom %d%%   %d bytes",
                           "%dx%d 像素   缩放 %d%%   %d 字节"),
                         w, h, scaled ? zoom : 100, len);
                sys_text(10, 468, info, 0xA0B0C0, 1);
            }
            sys_text(330, 468, T("Q/ESC close", "Q/ESC 关闭"), 0x808890, 1);

            /* checkerboard so transparent-looking areas are visible */
            sys_rect(vx - 1, vy - 1, vw + 2, vh + 2, 0x404850);

            /* pixels already holds exactly vw x vh samples */
            for (int y = 0; y < vh; y++)
                for (int x = 0; x < vw; x++)
                    sys_rect(vx + x, vy + y, 1, 1, pixels[y * vw + x]);

            sys_flush();
            sys_sleep(30);
        }
    }

    return 0;
}
