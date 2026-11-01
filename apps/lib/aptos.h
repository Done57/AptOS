#ifndef APTOS_APP_H
#define APTOS_APP_H

#include <stdarg.h>

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READKEY     2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_READ        5
#define SYS_MKDIR       6
#define SYS_UNLINK      7
#define SYS_LIST        8
#define SYS_SLEEP       9
#define SYS_TIME        10
#define SYS_GETKEY      11
#define SYS_ARGS        12
#define SYS_PKG_OPEN    13
#define SYS_PKG_COUNT   14
#define SYS_PKG_INFO    15
#define SYS_PKG_EXTRACT 16
#define SYS_EXISTS      17
#define SYS_ISDIR       18
#define SYS_WIN_OPEN    20
#define SYS_WIN_CLEAR   21
#define SYS_GFX_RECT    22
#define SYS_GFX_TEXT    23
#define SYS_WIN_FLUSH   24
#define SYS_MOUSE       25
#define SYS_WIN_FRAME   26
#define SYS_WIN_SIZE    27
#define SYS_PUMP        28
#define SYS_TICKS       29
#define SYS_POWEROFF    30
#define SYS_BOOTMODE    31
#define SYS_RAWWRITE    32
#define SYS_INSTALL     33
#define SYS_REBOOT      34
#define SYS_DISK_INFO   35
#define SYS_LANG        36
#define SYS_TEXT_W      37
#define SYS_AUDIO_INFO  38
#define SYS_AUDIO_PLAY  39
#define SYS_AUDIO_BEEP  40
#define SYS_ARGS_LEN    49
#define SYS_APT_OPEN    41
#define SYS_APT_PRESENT 42
#define SYS_APT_INFO    43
#define SYS_APT_TEXT    44
#define SYS_WIN_CLOSE   45
#define SYS_WIN_ALIVE   46

#define MOUSE_BTN_LEFT  1
#define MOUSE_BTN_RIGHT 2

static inline int __syscall(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
                     : "memory", "cc");
    return r;
}

static inline int __syscall5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory", "cc");
    return r;
}

static inline void sys_exit(int code)
{
    __syscall(SYS_EXIT, code, 0, 0, 0);
    for (;;)
        ;
}

static inline int sys_write(const char *s, int len)
{
    return __syscall(SYS_WRITE, 1, (int)s, len, 0);
}

static inline int sys_writefd(int fd, const void *buf, int len)
{
    return __syscall(SYS_WRITE, fd, (int)buf, len, 0);
}

static inline int sys_open(const char *path, int mode)
{
    return __syscall(SYS_OPEN, (int)path, mode, 0, 0);
}

static inline int sys_close(int fd) { return __syscall(SYS_CLOSE, fd, 0, 0, 0); }

static inline int sys_read(int fd, void *buf, int len)
{
    return __syscall(SYS_READ, fd, (int)buf, len, 0);
}

static inline int sys_mkdir(const char *path)
{
    return __syscall(SYS_MKDIR, (int)path, 0, 0, 0);
}

static inline int sys_unlink(const char *path)
{
    return __syscall(SYS_UNLINK, (int)path, 0, 0, 0);
}

static inline int sys_list(const char *path, int index, char *buf, int bufsize)
{
    return __syscall(SYS_LIST, (int)path, index, (int)buf, bufsize);
}

static inline void sys_sleep(int ms) { __syscall(SYS_SLEEP, ms, 0, 0, 0); }

static inline int sys_time(int *t) { return __syscall(SYS_TIME, (int)t, 0, 0, 0); }

static inline int sys_getkey(void) { return __syscall(SYS_GETKEY, 0, 0, 0, 0); }

static inline int sys_readkey(void) { return __syscall(SYS_READKEY, 0, 0, 0, 0); }

/* Length of an argument, or -1 if there is none.  sys_args() copies the text
 * but reports only success/failure, so a program that needs to tell an absent
 * argument from an empty one uses this. */
static inline int sys_args_len(int index)
{
    return __syscall(SYS_ARGS_LEN, index, 0, 0, 0);
}

static inline int sys_args(int index, char *buf, int bufsize)
{
    return __syscall(SYS_ARGS, index, (int)buf, bufsize, 0);
}

static inline int sys_pkg_open(const char *path)
{
    return __syscall(SYS_PKG_OPEN, (int)path, 0, 0, 0);
}

static inline int sys_pkg_count(int h) { return __syscall(SYS_PKG_COUNT, h, 0, 0, 0); }

static inline int sys_pkg_info(int h, int index, char *name)
{
    return __syscall(SYS_PKG_INFO, h, index, (int)name, 32);
}

static inline int sys_pkg_extract(int h, int index, const char *dest)
{
    return __syscall(SYS_PKG_EXTRACT, h, index, (int)dest, 0);
}

static inline int sys_exists(const char *path)
{
    return __syscall(SYS_EXISTS, (int)path, 0, 0, 0);
}

static inline int sys_isdir(const char *path)
{
    return __syscall(SYS_ISDIR, (int)path, 0, 0, 0);
}

static inline int sys_win_open(const char *title, int w, int h)
{
    return __syscall(SYS_WIN_OPEN, (int)title, w, h, 0);
}

static inline void sys_clear(int color) { __syscall(SYS_WIN_CLEAR, color, 0, 0, 0); }

static inline void sys_rect(int x, int y, int w, int h, int color)
{
    __syscall5(SYS_GFX_RECT, x, y, w, h, color);
}

static inline void sys_frame(int x, int y, int w, int h, int color)
{
    __syscall5(SYS_WIN_FRAME, x, y, w, h, color);
}

static inline void sys_text(int x, int y, const char *s, int color, int scale)
{
    __syscall5(SYS_GFX_TEXT, x, y, (int)s, color, scale);
}

static inline int sys_audio_info(void) { return __syscall(SYS_AUDIO_INFO, 0, 0, 0, 0); }

static inline int sys_audio_play(const void *pcm, int bytes, int volume)
{
    return __syscall(SYS_AUDIO_PLAY, (int)pcm, bytes, volume, 0);
}

static inline int sys_audio_beep(int freq, int ms, int volume)
{
    return __syscall(SYS_AUDIO_BEEP, freq, ms, volume, 0);
}

static inline int sys_text_w(const char *s, int scale)
{
    return __syscall(SYS_TEXT_W, (int)s, scale, 0, 0);
}

/* 0 = English, 1 = Chinese (see kernel/i18n.h) */
static inline int sys_lang(void) { return __syscall(SYS_LANG, 0, 0, 0, 0); }

static inline void sys_flush(void) { __syscall(SYS_WIN_FLUSH, 0, 0, 0, 0); }

static inline void sys_mouse(int *x, int *y, int *buttons, int *inside)
{
    __syscall(SYS_MOUSE, (int)x, (int)y, (int)buttons, (int)inside);
}

static inline int sys_win_size(void) { return __syscall(SYS_WIN_SIZE, 0, 0, 0, 0); }

#define SYS_MMAP        47
#define SYS_MUNMAP      48

static inline void *sys_mmap(int bytes)
{
    return (void *)__syscall(SYS_MMAP, bytes, 0, 0, 0);
}

static inline int sys_munmap(void *addr, int bytes)
{
    return __syscall(SYS_MUNMAP, (int)addr, bytes, 0, 0);
}

static inline void sys_win_close(void) { __syscall(SYS_WIN_CLOSE, 0, 0, 0, 0); }
static inline int  sys_win_alive(void) { return __syscall(SYS_WIN_ALIVE, 0, 0, 0, 0); }
static inline int  sys_apt_text(int x, int y, const char *s, u32 colour, int scale)
{
    return __syscall5(SYS_APT_TEXT, x, y, (int)s, (int)colour, scale);
}

static inline void sys_pump(void) { __syscall(SYS_PUMP, 0, 0, 0, 0); }

static inline int sys_ticks(void) { return __syscall(SYS_TICKS, 0, 0, 0, 0); }

static inline void sys_poweroff(void) { __syscall(SYS_POWEROFF, 0, 0, 0, 0); }

static inline int sys_bootmode(void) { return __syscall(SYS_BOOTMODE, 0, 0, 0, 0); }

static inline int sys_install(const char *dev, int base)
{
    return __syscall(SYS_INSTALL, (int)dev, base, 0, 0);
}

static inline void sys_reboot(void) { __syscall(SYS_REBOOT, 0, 0, 0, 0); }

static inline int sys_disk_info(int index, char *name, int *sectors, int *type)
{
    return __syscall(SYS_DISK_INFO, index, (int)name, (int)sectors, (int)type);
}

static inline int sys_rawwrite(const char *dev, int lba, const void *buf, int count)
{
    return __syscall(SYS_RAWWRITE, (int)dev, lba, count, (int)buf);
}

int  printf(const char *fmt, ...);
int  puts(const char *s);
int  putchar(int c);
int  snprintf(char *out, int size, const char *fmt, ...);
int  vsnprintf(char *out, int size, const char *fmt, va_list ap);

unsigned strlen(const char *s);
int  strcmp(const char *a, const char *b);
int  strncmp(const char *a, const char *b, unsigned n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, unsigned n);
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int  atoi(const char *s);
void sys_sleep_ms(int ms);

#define RGB(r, g, b) ((int)(((r) << 16) | ((g) << 8) | (b)))

#endif
