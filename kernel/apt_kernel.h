#ifndef APTOS_APT_KERNEL_H
#define APTOS_APT_KERNEL_H

#include "types.h"

/* Create a shared canvas and map it into the calling program's address
 * space.  `out_pixels` receives the user-space address of the pixels. */
int  apt_kernel_open(const char *title, int w, int h, u32 *out_pixels);

/* Tear the canvas down, dropping the user mapping. */
void apt_kernel_close(void);

/* Called when a ring 3 program exits, however it exits. */
void apt_kernel_reset(void);

/* True when a canvas is currently open. */
int  apt_kernel_active(void);

u32  apt_kernel_pixels(void);       /* kernel-side address, for compositing */
u32  apt_kernel_user_pixels(void); /* program-side address                */
int  apt_kernel_width(void);
int  apt_kernel_height(void);

/* Composite the given canvas region to the screen. */
int  apt_kernel_present(int x, int y, int w, int h);

/* Program heap.  libapt falls back to this when its arena cannot serve a
 * request, so a program can allocate a large texture without every program
 * reserving that much memory up front. */
u32  apt_mmap(int bytes);
int  apt_munmap(u32 vaddr, u32 size);
void apt_heap_reset(void);

/* Draw a UTF-8 string into the canvas.  Returns the advance width. */
int  apt_render_text(u32 *pixels, int w, int h, int x, int y, const char *s,
                     u32 colour, int scale);
int  apt_text_width(const char *s, int scale);

#endif
