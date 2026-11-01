#include <stdarg.h>
#include "aptos.h"

int putchar(int c)
{
    char b = (char)c;
    sys_write(&b, 1);
    return c;
}

static int unused_putchar_marker;

int puts(const char *s)
{
    unsigned n = strlen(s);
    sys_write(s, (int)n);
    putchar('\n');
    return 0;
}

unsigned strlen(const char *s)
{
    unsigned n = 0;
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

int strncmp(const char *a, const char *b, unsigned n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (int)(u8)*a - (int)(u8)*b : 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *strncpy(char *d, const char *s, unsigned n)
{
    unsigned i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

void *memset(void *d, int c, unsigned n)
{
    u8 *p = (u8 *)d;
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    u8 *a = (u8 *)d;
    const u8 *b = (const u8 *)s;
    while (n--)
        *a++ = *b++;
    return d;
}

void sys_sleep_ms(int ms)
{
    sys_sleep(ms);
}

int atoi(const char *s)
{
    int v = 0, sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}

typedef struct {
    char  *out;
    int    size;
    int    n;
    int    to_stdout;
} sink_t;

static void emit(sink_t *s, char c)
{
    if (s->to_stdout) {
        sys_write(&c, 1);
    } else if (s->n + 1 < s->size) {
        s->out[s->n] = c;
    }
    s->n++;
}

static void emit_str(sink_t *s, const char *str)
{
    while (*str)
        emit(s, *str++);
}

static void emit_pad(sink_t *s, int count, char c)
{
    while (count-- > 0)
        emit(s, c);
}

static void emit_num(sink_t *s, u32 v, int base, int upper, int width, int zero, int left)
{
    char tmp[36];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, pad;

    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % (u32)base];
        v /= (u32)base;
    }
    pad = width - n;
    if (!left && !zero)
        emit_pad(s, pad, ' ');
    if (!left && zero)
        emit_pad(s, pad, '0');
    while (n--)
        emit(s, tmp[n]);
    if (left)
        emit_pad(s, pad, ' ');
}

static int format(sink_t *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        int left = 0, zero = 0, width = 0;

        if (*fmt != '%') {
            emit(s, *fmt);
            continue;
        }
        fmt++;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-')
                left = 1;
            else
                zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z')
            fmt++;

        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                emit(s, '-');
                if (width)
                    width--;
                emit_num(s, (u32)(-v), 10, 0, width, zero, left);
            } else {
                emit_num(s, (u32)v, 10, 0, width, zero, left);
            }
            break;
        }
        case 'u':
            emit_num(s, va_arg(ap, u32), 10, 0, width, zero, left);
            break;
        case 'x':
            emit_num(s, va_arg(ap, u32), 16, 0, width, zero, left);
            break;
        case 'X':
            emit_num(s, va_arg(ap, u32), 16, 1, width, zero, left);
            break;
        case 'c':
            emit(s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            int len;
            if (!str)
                str = "(null)";
            len = (int)strlen(str);
            if (!left)
                emit_pad(s, width - len, ' ');
            emit_str(s, str);
            if (left)
                emit_pad(s, width - len, ' ');
            break;
        }
        case '%':
            emit(s, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            emit(s, '%');
            emit(s, *fmt);
            break;
        }
    }
    return s->n;
}

int printf(const char *fmt, ...)
{
    static char buf[512];
    sink_t s;
    va_list ap;
    int r;

    s.out = buf;
    s.size = sizeof(buf);
    s.n = 0;
    s.to_stdout = 0;
    va_start(ap, fmt);
    r = format(&s, fmt, ap);
    va_end(ap);
    {
        int idx = (s.n < (int)sizeof(buf)) ? s.n : (int)sizeof(buf) - 1;
        buf[idx] = 0;
        if (idx > 0)
            sys_write(buf, idx);
    }
    return r;
}

int vsnprintf(char *out, int size, const char *fmt, va_list ap)
{
    sink_t s;
    int r;

    s.out = out;
    s.size = size;
    s.n = 0;
    s.to_stdout = 0;
    r = format(&s, fmt, ap);
    if (size > 0) {
        int idx = (s.n < size) ? s.n : size - 1;
        out[idx] = 0;
    }
    return r;
}

int snprintf(char *out, int size, const char *fmt, ...)
{
    sink_t s;
    va_list ap;
    int r;

    s.out = out;
    s.size = size;
    s.n = 0;
    s.to_stdout = 0;
    va_start(ap, fmt);
    r = format(&s, fmt, ap);
    va_end(ap);
    if (size > 0) {
        int idx = (s.n < size) ? s.n : size - 1;
        out[idx] = 0;
    }
    return r;
}
