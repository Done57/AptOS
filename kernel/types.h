#ifndef APTOS_TYPES_H
#define APTOS_TYPES_H

typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;
typedef signed long long    s64;

/* Pointer-sized integers.  The kernel is built for both i386 and x86-64
 * while the long-mode port is in progress, so anything that holds an
 * address -- or a size that gets added to one -- has to be pointer wide.
 * Using plain u32 for these silently truncates a 64-bit address. */
#if defined(__x86_64__)
typedef unsigned long       uptr;
typedef signed long         sptr;
typedef unsigned long       size_t;
typedef signed long         ssize_t;
#else
typedef unsigned int        uptr;
typedef signed int          sptr;
typedef unsigned int        size_t;
typedef signed int          ssize_t;
#endif

#define NULL ((void *)0)
#define true  1
#define false 0

#define PACKED __attribute__((packed))

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif
