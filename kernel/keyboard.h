#ifndef APTOS_KEYBOARD_H
#define APTOS_KEYBOARD_H

#include "types.h"

#define KEY_UP      0x101
#define KEY_DOWN    0x102
#define KEY_LEFT    0x103
#define KEY_RIGHT   0x104
#define KEY_HOME    0x105
#define KEY_END     0x106
#define KEY_DELETE  0x107
#define KEY_PGUP    0x108
#define KEY_PGDN    0x109
#define KEY_F1      0x111
#define KEY_F2      0x112
#define KEY_F3      0x113
#define KEY_F4      0x114
#define KEY_F5      0x115
#define KEY_F6      0x116
#define KEY_F7      0x117
#define KEY_F8      0x118
#define KEY_F9      0x119
#define KEY_F10     0x11A
#define KEY_F11     0x11B
#define KEY_F12     0x11C

#define KMOD_SHIFT  1
#define KMOD_CTRL   2
#define KMOD_ALT    4
#define KMOD_CAPS   8

void keyboard_init(void);
u32  keyboard_mods(void);
int  keyboard_has_key(void);
int  keyboard_getkey(void);

#endif
