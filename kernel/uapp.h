#ifndef APTOS_UAPP_H
#define APTOS_UAPP_H

#include "types.h"

/* Largest canvas the window manager will accept. */
#define UAPP_MAX_W 1004
#define UAPP_MAX_H 700

int  uapp_open(const char *title, int w, int h);
void uapp_clear(u32 color);
void uapp_rect(int x, int y, int w, int h, u32 color);
void uapp_frame(int x, int y, int w, int h, u32 color);
void uapp_text(int x, int y, const char *s, u32 color, int scale);
int  uapp_text_w(const char *s, int scale);
void uapp_flush(void);
void uapp_close(void);
void uapp_mouse(int *x, int *y, int *buttons, int *inside);
int  uapp_width(void);
int  uapp_height(void);
int  uapp_open_window(void);
int  uapp_attach_canvas(u32 *pixels, int w, int h, const char *title);
void uapp_present(int x, int y, int w, int h);
int  uapp_alive(void);

#endif
