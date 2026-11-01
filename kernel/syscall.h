#ifndef APTOS_SYSCALL_H
#define APTOS_SYSCALL_H

#include "idt.h"
#include "types.h"

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
#define SYS_ABOUT       19
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
#define SYS_AUDIO_INFO  38   /* 0 if no sound card */
#define SYS_AUDIO_PLAY  39   /* (ptr, bytes, volume) -> 0 ok */
#define SYS_AUDIO_BEEP  40   /* (freq, ms, volume) -> 0 ok */

/* apt graphics API.  Drawing itself never traps - these two calls are the
 * whole kernel interface: get a shared canvas, and present a region. */
#define SYS_APT_OPEN    41   /* (title, w, h [, &pixels]) -> 0 ok, -1 failed */
#define SYS_APT_PRESENT 42   /* (x, y, w, h) -> 0 ok */
#define SYS_APT_INFO    43   /* (apt_info *) -> 0 ok, fills in the struct   */
#define SYS_APT_TEXT    44   /* (x, y, str, colour, scale) draw into the canvas */
#define SYS_WIN_CLOSE   45   /* close the calling program's window */
#define SYS_WIN_ALIVE   46   /* 1 while the window still exists */
#define SYS_MMAP        47   /* (bytes) -> user address, 0 on failure */
#define SYS_MUNMAP      48   /* (addr, bytes) -> 0 ok */
#define SYS_ARGS_LEN    49   /* (index) -> argument length, no copying */

#define USER_FD_STDOUT  1
#define USER_FD_STDERR  2

void syscall_init(void);
void syscall_dispatch(regs_t *r);
void syscall_set_output(void (*fn)(const char *buf, u32 len));
void syscall_set_args(const char *a0, const char *a1);

#endif
