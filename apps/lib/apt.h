/* apt - the aptos graphics API
 *
 * A small 2D graphics library for ring 3 programs.  The design goal is that
 * ordinary drawing costs no system calls at all: a program gets a shared
 * canvas (a block of memory mapped into both its address space and the
 * kernel's) and writes pixels into it directly.  Only two things cross the
 * kernel boundary - asking for the canvas, and telling the window manager
 * that a region changed.
 *
 *     apt_canvas *c = apt_open("my app", 640, 480);
 *     for (;;) {
 *         apt_clear(c, APT_BLACK);
 *         apt_fill_rect(c, 10, 10, 100, 60, APT_BLUE);
 *         apt_flush(c);              // one syscall for the whole frame
 *         apt_pump(c);               // input + events
 *     }
 *
 * Coordinates are ints by default.  Anything that needs sub-pixel precision
 * goes through apt_fx (16.16 fixed point), which is what the transform and
 * texture paths use.
 */
#ifndef APT_H
#define APT_H

#include <stdarg.h>

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef signed int     s32;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ------------------------------------------------------------------ */
/* colours                                                             */

#define APT_RGB(r, g, b) ((u32)(((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b)))
#define APT_RGBA(r, g, b, a) \
    ((u32)(((u32)(a) << 24) | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b)))

#define APT_BLACK   APT_RGB(0, 0, 0)
#define APT_WHITE   APT_RGB(255, 255, 255)
#define APT_RED     APT_RGB(224, 74, 74)
#define APT_GREEN   APT_RGB(74, 224, 74)
#define APT_BLUE    APT_RGB(74, 124, 224)
#define APT_YELLOW  APT_RGB(224, 210, 74)
#define APT_CYAN    APT_RGB(74, 210, 224)
#define APT_MAGENTA APT_RGB(224, 74, 210)
#define APT_GRAY    APT_RGB(128, 128, 128)
#define APT_DGRAY   APT_RGB(48, 48, 48)
#define APT_LGRAY   APT_RGB(192, 192, 192)
#define APT_ORANGE  APT_RGB(232, 150, 60)

#define APT_R(c) (((c) >> 16) & 0xFF)
#define APT_G(c) (((c) >> 8) & 0xFF)
#define APT_B(c) ((c) & 0xFF)
#define APT_A(c) (((c) >> 24) & 0xFF)

/* ------------------------------------------------------------------ */
/* fixed point                                                         */

typedef s32 apt_fx;
#define APT_FX_BITS 16
#define APT_FX_ONE  (1 << APT_FX_BITS)
#define APT_FX(i)   ((apt_fx)((i) << APT_FX_BITS))
#define APT_FX_INT(f) ((int)((f) >> APT_FX_BITS))
/* 16.16 multiply.  The naive ((a*b)>>16) overflows the moment either operand
 * is more than a few thousand pixels: 600 * 65536 alone is already 25 bits,
 * and squaring that needs 41.  Splitting the shift keeps every intermediate
 * inside 32 bits, which matters because a ring 3 program here has no 64-bit
 * helpers to fall back on.
 *
 *   a*b = (a_hi<<16 + a_lo) * b  ->  a_hi*b + (a_lo*b)>>16
 */
#define APT_FX_MUL(a, b) \
    ((apt_fx)((((s32)(a) >> APT_FX_BITS) * (s32)(b)) + \
              ((((s32)(a) & 0xFFFF) * (s32)(b)) >> APT_FX_BITS)))
/* Divide, guarding against the zero divisor: a scale of 0 is a program bug,
 * and faulting the whole process over it is worse than returning 0. */
#define APT_FX_DIV(a, b) \
    ((apt_fx)((b) ? ((((s32)(a) / (s32)(b)) << APT_FX_BITS) + \
                     ((((s32)(a) % (s32)(b)) << APT_FX_BITS) / (s32)(b))) : 0))

/* ------------------------------------------------------------------ */
/* geometry                                                            */

typedef struct { int x, y; } apt_point;
typedef struct { int x, y, w, h; } apt_box;
typedef struct { apt_fx x, y; } apt_pointfx;

#define APT_MAX_CLIP 8

/* ------------------------------------------------------------------ */
/* canvas                                                              */

/* One pending clip rectangle change, replayed into the kernel when the
 * program flushes.  Keeping them batched is what makes clipping cheap. */
typedef struct {
    int x0, y0, x1, y1;
} apt_clip_rect;

typedef struct apt_canvas {
    u32  *pixels;        /* the shared framebuffer, directly writable     */
    int   width, height;
    int   pitch;         /* in pixels                                     */

    /* current clip, in canvas coordinates, inclusive */
    int   cx0, cy0, cx1, cy1;
    apt_clip_rect clip_stack[APT_MAX_CLIP];
    int   clip_depth;

    /* transform: canvas_point = (user_point * scale) + offset            */
    apt_fx tx, ty;       /* translation                                    */
    apt_fx sx, sy;       /* scale                                          */
    int    flip_x, flip_y;

    /* batching */
    int   dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    int   ops;           /* drawing operations since the last flush        */
    u32   colour;
    int   alpha;         /* 0..255, global blend factor                    */

    void *priv;          /* kernel handle, do not touch                    */
} apt_canvas;

/* ------------------------------------------------------------------ */
/* font                                                                */

#define APT_ALIGN_LEFT    0
#define APT_ALIGN_CENTER  1
#define APT_ALIGN_RIGHT   2

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */

/* Open a window and get a shared canvas for it.  Returns NULL on failure.
 * The canvas is ready to draw into immediately, with clipping set to the
 * whole window. */
apt_canvas *apt_open(const char *title, int w, int h);

/* Close the window and release the canvas. */
void apt_close(apt_canvas *c);

/* Present the region touched since the last flush, then reset the dirty
 * rectangle.  This is the one call per frame that costs a system call. */
void apt_flush(apt_canvas *c);

/* Input.  Returns the number of events handled.  Fills in the mouse
 * position (canvas coordinates) and buttons. */
typedef struct {
    int mouse_x, mouse_y;
    int mouse_buttons;
    int mouse_inside;
    int key;              /* 0 when nothing was pressed this call          */
    int quit;             /* set when the window was closed                */
} apt_event;

int  apt_poll(apt_canvas *c, apt_event *e);

/* Convenience: poll, and block until something happens.  Returns the same
 * thing apt_poll does. */
int  apt_wait(apt_canvas *c, apt_event *e);

/* True when the window has been closed. */
int  apt_should_close(apt_canvas *c);

/* ------------------------------------------------------------------ */
/* state                                                               */

void apt_clear(apt_canvas *c, u32 colour);
void apt_set_colour(apt_canvas *c, u32 colour);
void apt_set_alpha(apt_canvas *c, int alpha);

/* Clipping.  apt_push_clip intersects the current clip with the given
 * rectangle; apt_pop_clip restores the previous one. */
void apt_set_clip(apt_canvas *c, int x, int y, int w, int h);
void apt_reset_clip(apt_canvas *c);
void apt_push_clip(apt_canvas *c, int x, int y, int w, int h);
void apt_pop_clip(apt_canvas *c);

/* ------------------------------------------------------------------ */
/* primitives                                                          */

void apt_pixel(apt_canvas *c, int x, int y, u32 colour);
void apt_hline(apt_canvas *c, int x, int y, int w, u32 colour);
void apt_vline(apt_canvas *c, int x, int y, int h, u32 colour);
void apt_line(apt_canvas *c, int x0, int y0, int x1, int y1, u32 colour);
void apt_rect(apt_canvas *c, int x, int y, int w, int h, u32 colour);
void apt_fill_rect(apt_canvas *c, int x, int y, int w, int h, u32 colour);
void apt_circle(apt_canvas *c, int cx, int cy, int r, u32 colour);
void apt_fill_circle(apt_canvas *c, int cx, int cy, int r, u32 colour);
void apt_ellipse(apt_canvas *c, int cx, int cy, int rx, int ry, u32 colour);
void apt_fill_ellipse(apt_canvas *c, int cx, int cy, int rx, int ry, u32 colour);
void apt_round_rect(apt_canvas *c, int x, int y, int w, int h, int r, u32 colour);
void apt_fill_round_rect(apt_canvas *c, int x, int y, int w, int h, int r, u32 colour);

/* Arbitrary polygon, scanline filled.  `pts` holds `n` points. */
void apt_fill_poly(apt_canvas *c, const apt_point *pts, int n, u32 colour);

/* Vertical gradient, top colour to bottom colour. */
void apt_gradient(apt_canvas *c, int x, int y, int w, int h, u32 top, u32 bottom);

/* Soft shadow: a rounded rectangle of gradually fading black. */
void apt_shadow(apt_canvas *c, int x, int y, int w, int h, int spread);

/* ------------------------------------------------------------------ */
/* transforms                                                          */

/* Set the transform applied to subsequent drawing.  Coordinates given to
 * the primitives are multiplied by (sx, sy), offset by (tx, ty), and
 * optionally flipped about the canvas centre. */
void apt_set_transform(apt_canvas *c, apt_fx sx, apt_fx sy, apt_fx tx, apt_fx ty);
void apt_translate(apt_canvas *c, apt_fx dx, apt_fx dy);
void apt_scale(apt_canvas *c, apt_fx sx, apt_fx sy);
void apt_set_flip(apt_canvas *c, int flip_x, int flip_y);
void apt_reset_transform(apt_canvas *c);
void apt_user_to_canvas(apt_canvas *c, apt_fx ux, apt_fx uy, int *cx, int *cy);
void apt_canvas_to_user(apt_canvas *c, int cx, int cy, apt_fx *ux, apt_fx *uy);

/* ------------------------------------------------------------------ */
/* textures                                                            */

/* A texture is just pixels plus a size.  `mask` may be NULL; when present
 * it is 1 byte per pixel and acts as an alpha channel. */
typedef struct {
    u32 *pixels;
    u8  *mask;
    int  width, height;
    int  owned;          /* free on apt_texture_free when set              */
} apt_texture;

apt_texture *apt_texture_new(int w, int h);
void         apt_texture_free(apt_texture *t);
u32          apt_texture_get(const apt_texture *t, int x, int y);

/* Copy a texture to the canvas. */
void apt_blit(apt_canvas *c, const apt_texture *t, int x, int y);
void apt_blit_scaled(apt_canvas *c, const apt_texture *t, int x, int y, int w, int h);

/* Copy from the canvas into a texture (screenshot of a region). */
void apt_capture(apt_canvas *c, apt_texture *t, int x, int y);

/* Map a rectangle of the texture onto an arbitrary quad.  The quad is given
 * as four points in the order top-left, top-right, bottom-right,
 * bottom-left.  This is the affine/textured-triangle path: the quad is
 * split into two triangles. */
void apt_blit_quad(apt_canvas *c, const apt_texture *t, const apt_point *quad);
void apt_blit_quad_uv(apt_canvas *c, const apt_texture *t, const apt_point *quad,
                      int u0, int v0, int u1, int v1);

/* ------------------------------------------------------------------ */
/* text                                                                */

void apt_text(apt_canvas *c, int x, int y, const char *s, u32 colour, int scale);
int  apt_text_w(const char *s, int scale);
int  apt_text_h(int scale);
void apt_text_aligned(apt_canvas *c, int x, int y, int w, const char *s,
                      u32 colour, int scale, int align);
void apt_printf(apt_canvas *c, int x, int y, u32 colour, int scale,
                const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* memory                                                              */
/*                                                                    */
/* Programs have no libc, so libapt carries a small allocator.  Textures */
/* and other bulk allocations come from here.  The arena is a fixed 1 MiB */
/* inside the program's BSS. */

void *malloc(int n);
void  free(void *p);

/* Bytes still free in the arena, for diagnostics. */
int   apt_mem_free(void);

/* ------------------------------------------------------------------ */
/* misc                                                                */

/* Milliseconds since boot. */
int  apt_ticks(void);
void apt_sleep(int ms);

/* A simple deterministic random number generator, parked per canvas so
 * every program does not have to invent its own. */
void apt_srand(apt_canvas *c, u32 seed);
u32  apt_rand(apt_canvas *c);

#endif /* APT_H */
