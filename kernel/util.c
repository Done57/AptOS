#include "util.h"

void *memset(void *dst, int c, size_t n)
{
    u8 *d = (u8 *)dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

static int lower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    return c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower((u8)*a) - lower((u8)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && lower(*a) == lower(*b)) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return lower((u8)*a) - lower((u8)*b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if (*s == (char)c)
            last = s;
    if (c == 0)
        return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl)
        return (char *)hay;
    for (; *hay; hay++)
        if (!strncmp(hay, needle, nl))
            return (char *)hay;
    return NULL;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}

void utoa(u32 v, char *buf, int base)
{
    char tmp[36];
    int i = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (v) {
        u32 d = v % (u32)base;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= (u32)base;
    }
    int j = 0;
    while (i)
        buf[j++] = tmp[--i];
    buf[j] = 0;
}

void itoa(int v, char *buf, int base)
{
    if (v < 0 && base == 10) {
        buf[0] = '-';
        utoa((u32)(-v), buf + 1, base);
    } else {
        utoa((u32)v, buf, base);
    }
}

typedef struct {
    char  *out;
    size_t size;
    size_t written;
} fmt_out_t;

static void fmt_putc(fmt_out_t *o, char c)
{
    if (o->written + 1 < o->size)
        o->out[o->written] = c;
    o->written++;
}

static void fmt_puts(fmt_out_t *o, const char *s)
{
    while (*s)
        fmt_putc(o, *s++);
}

static void fmt_pad(fmt_out_t *o, int count, char c)
{
    while (count-- > 0)
        fmt_putc(o, c);
}

/* 64 位值格式化。
 *
 * 参数类型必须是 u64 而不是 u32：64 位内核里所有地址都是 64 位的，
 * 早先的 u32 版本会把高 32 位悄悄丢掉，于是 0x100000000 打印成 0 ——
 * 这种错误看上去像"地址算错了"，实际是打印错了，很容易带偏排查方向。
 *
 * 但这里有个陷阱：**32 位平台上没有 64 位除法指令**。直接写
 * `v / base`（v 是 u64）会让编译器去调 libgcc 的 __udivmoddi4，
 * 而内核不链接 libgcc，于是链接期才报 undefined reference。
 *
 * 所以按平台分开：
 *   64 位 —— 原生 64 位除法，一条 div 指令。
 *   32 位 —— 根本不存在 64 位的值，退化成原来的 32 位除法，
 *           既不需要运行时，也保持原来的行为。
 */
#if defined(__x86_64__)
typedef u64 fmt_val_t;
#define FMT_DIV(a, b) ((a) / (b))
#define FMT_MOD(a, b) ((a) % (b))
#else
typedef u32 fmt_val_t;
#define FMT_DIV(a, b) ((a) / (b))
#define FMT_MOD(a, b) ((a) % (b))
#endif

static void fmt_uint(fmt_out_t *o, u64 vfull, int base, int upper, int width,
                     int zero, int left)
{
    char tmp[36];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    fmt_val_t v = (fmt_val_t)vfull;

    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[FMT_MOD(v, (fmt_val_t)base)];
        v = FMT_DIV(v, (fmt_val_t)base);
    }
    int pad = width - n;
    if (!left && !zero)
        fmt_pad(o, pad, ' ');
    if (!left && zero)
        fmt_pad(o, pad, '0');
    while (n--)
        fmt_putc(o, tmp[n]);
    if (left)
        fmt_pad(o, pad, ' ');
}

int vsnprintf(char *out, size_t size, const char *fmt, va_list ap)
{
    fmt_out_t o;
    o.out = out;
    o.size = size;
    o.written = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            fmt_putc(&o, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, width = 0, is_long = 0;

        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-')
                left = 1;
            else
                zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') {
            if (*fmt == 'l')
                is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            s64 v = is_long ? (s64)va_arg(ap, long) : (s64)va_arg(ap, int);
            if (v < 0) {
                fmt_putc(&o, '-');
                if (width > 0)
                    width--;
                fmt_uint(&o, (u64)(-v), 10, 0, width, zero, left);
            } else {
                fmt_uint(&o, (u64)v, 10, 0, width, zero, left);
            }
            break;
        }
        case 'u':
            fmt_uint(&o, is_long ? (u64)va_arg(ap, unsigned long)
                                 : (u64)va_arg(ap, unsigned int),
                     10, 0, width, zero, left);
            break;
        case 'x':
            fmt_uint(&o, is_long ? (u64)va_arg(ap, unsigned long)
                                 : (u64)va_arg(ap, unsigned int),
                     16, 0, width, zero, left);
            break;
        case 'X':
            fmt_uint(&o, is_long ? (u64)va_arg(ap, unsigned long)
                                 : (u64)va_arg(ap, unsigned int),
                     16, 1, width, zero, left);
            break;
        case 'p':
            fmt_puts(&o, "0x");
            fmt_uint(&o, (u64)(uptr)va_arg(ap, void *), 16, 0,
                     (int)(sizeof(void *) * 2), 1, 0);
            break;
        case 'c':
            fmt_putc(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strlen(s);
            if (!left) {
                int pad = width - len;
                while (pad-- > 0)
                    fmt_putc(&o, ' ');
            }
            fmt_puts(&o, s);
            if (left) {
                int pad = width - len;
                while (pad-- > 0)
                    fmt_putc(&o, ' ');
            }
            break;
        }
        case '%':
            fmt_putc(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            fmt_putc(&o, '%');
            fmt_putc(&o, *fmt);
            break;
        }
    }

    if (o.size > 0) {
        size_t idx = o.written < o.size ? o.written : o.size - 1;
        o.out[idx] = 0;
    }
    return (int)o.written;
}

int snprintf(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    return n;
}

unsigned long long __udivdi3(unsigned long long a, unsigned long long b)
{
    unsigned long long q = 0, r = 0;
    int i;
    if (b == 0)
        return 0;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1ULL);
        if (r >= b) {
            r -= b;
            q |= (1ULL << i);
        }
    }
    return q;
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b)
{
    unsigned long long r = 0;
    int i;
    if (b == 0)
        return 0;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1ULL);
        if (r >= b)
            r -= b;
    }
    return r;
}

/* GCC 的三合一版本：一次给出商和余数。与上面两个同族 ——
 * 编译器在不同表达式里会挑不同的入口，缺一个就链接失败。 */
unsigned long long __udivmoddi4(unsigned long long a, unsigned long long b,
                                unsigned long long *rem)
{
    unsigned long long q = 0, r = 0;
    int i;

    if (b == 0) {
        if (rem)
            *rem = 0;
        return 0;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1ULL);
        if (r >= b) {
            r -= b;
            q |= (1ULL << i);
        }
    }
    if (rem)
        *rem = r;
    return q;
}

/* ---- 小端字节写入（格式化骨架共用） ---- */
void fmt_le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void fmt_le32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
void fmt_le64(u8 *p, u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (u8)(v >> (8 * i));
}
