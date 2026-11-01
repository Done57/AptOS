#include "syscall.h"
#include "fs.h"
#include "gui.h"
#include "i18n.h"
#include "io.h"
#include "heap.h"
#include "keyboard.h"
#include "klog.h"
#include "lst.h"
#include "paging.h"
#include "rtc.h"
#include "timer.h"
#include "uapp.h"
#include "hda.h"
#include "apt_kernel.h"
#include "acpi.h"
#include "blkdev.h"
#include "bootinfo.h"
#include "user.h"
#include "util.h"

#define UFD_MAX 8
#define UPKG_MAX 4
/* Largest file the ring 3 file API will pull into memory at once.  The
 * installer reads /SYSTEM.ERT (the whole system snapshot) through this, so it
 * has to stay comfortably above the kernel + filesystem image size. */
#define UFILE_MAX (1024 * 1024)
/* Largest PCM block a ring 3 app may hand the sound card in one call. */
#define AUDIO_MAX_BYTES (512 * 1024)

typedef struct {
    int  used;
    int  writing;
    char path[160];
    u8  *buf;
    u32  size;
    u32  cap;
    u32  pos;
} ufd_t;

typedef struct {
    int  used;
    u8  *data;
    u32  size;
} upkg_t;

static ufd_t ufds[UFD_MAX];
static upkg_t upkgs[UPKG_MAX];
static void (*out_fn)(const char *buf, u32 len);
static char arg0[160], arg1[160];

void syscall_set_output(void (*fn)(const char *buf, u32 len))
{
    out_fn = fn;
}

void syscall_set_args(const char *a0, const char *a1)
{
    arg0[0] = arg1[0] = 0;
    if (a0)
        strncpy(arg0, a0, sizeof(arg0) - 1);
    if (a1)
        strncpy(arg1, a1, sizeof(arg1) - 1);
}

void syscall_init(void)
{
    memset(ufds, 0, sizeof(ufds));
    memset(upkgs, 0, sizeof(upkgs));
    out_fn = NULL;
}

static int uok(u32 ptr, u32 len)
{
    return paging_user_range_ok(ptr, len);
}

static int ustr(u32 uptr, char *dst, u32 max)
{
    u32 i;

    if (!max)
        return -1;
    for (i = 0; i < max - 1; i++) {
        if (!uok(uptr + i, 1))
            return -1;
        dst[i] = *(const char *)(uptr + i);
        if (!dst[i])
            return 0;
    }
    dst[max - 1] = 0;
    return 0;
}

static u32 last_pump;

static void pump(void)
{
    u32 now = timer_uptime_ms();
    if (now - last_pump < 60)
        return;
    last_pump = now;
    gui_poll();
}

static void uprint(const char *s, u32 len)
{
    if (out_fn)
        out_fn(s, len);
    else
        for (u32 i = 0; i < len; i++)
            kprintf("%c", s[i]);
}

static int fd_alloc(void)
{
    for (int i = 3; i < UFD_MAX; i++)
        if (!ufds[i].used)
            return i;
    return -1;
}

/* Next real keystroke, swallowing the language hotkey (F1 / Ctrl+Alt+L) and
 * switching the UI language for the kernel and every app that asks sys_lang(). */
static int pop_key(void)
{
    int k;

    while ((k = keyboard_getkey()) != -1) {
        if (k == KEY_F1 || (k == 12 && (keyboard_mods() & KMOD_ALT))) {
            i18n_toggle();
            gui_dirty();
            continue;
        }
        return k;
    }
    return -1;
}

static int sys_open(u32 upath, u32 mode)
{
    char path[160];
    int fd;
    int len;

    if (ustr(upath, path, sizeof(path)) != 0)
        return -1;
    fd = fd_alloc();
    if (fd < 0)
        return -1;

    {
        u8 *keep = ufds[fd].buf;
        memset(&ufds[fd], 0, sizeof(ufds[fd]));
        ufds[fd].buf = keep;
    }
    strncpy(ufds[fd].path, path, sizeof(ufds[fd].path) - 1);
    ufds[fd].used = 1;
    ufds[fd].cap = UFILE_MAX;

    if (mode == 0) {
        fs_dirent_t st;
        u32 need = UFILE_MAX;

        if (fs_resolve(path, &st) != 0) {
            ufds[fd].used = 0;
            return -2;
        }
        if (st.size + 32 < need)
            need = st.size + 32;
        if (!ufds[fd].buf || ufds[fd].cap < need) {
            ufds[fd].buf = (u8 *)kmalloc(need);
            if (!ufds[fd].buf) {
                ufds[fd].used = 0;
                return -1;
            }
            ufds[fd].cap = need;
        }
        len = fs_read(path, ufds[fd].buf, ufds[fd].cap);
        if (len < 0) {
            ufds[fd].used = 0;
            return -2;
        }
        ufds[fd].size = (u32)len;
        ufds[fd].pos = 0;
    } else {
        if (!ufds[fd].buf) {
            ufds[fd].buf = (u8 *)kmalloc(UFILE_MAX);
            if (!ufds[fd].buf) {
                ufds[fd].used = 0;
                return -1;
            }
            ufds[fd].cap = UFILE_MAX;
        }
        ufds[fd].writing = 1;
        ufds[fd].size = 0;
    }
    return fd;
}

static int sys_close(u32 fd)
{
    if (fd < 3 || fd >= UFD_MAX || !ufds[fd].used)
        return -1;
    if (ufds[fd].writing) {
        if (fs_write(ufds[fd].path, ufds[fd].buf, ufds[fd].size) < 0) {
            ufds[fd].used = 0;
            return -2;
        }
    }
    ufds[fd].used = 0;
    return 0;
}

static int sys_list(u32 upath, u32 index, u32 ubuf, u32 bufsize)
{
    char path[160];
    char name[64];
    fs_dirent_t e;

    if (ustr(upath, path, sizeof(path)) != 0)
        return -1;
    if (fs_list(path, (int)index, &e) != 0)
        return -1;
    snprintf(name, sizeof(name), "%s", e.name);
    if (bufsize < strlen(name) + 1)
        return -2;
    if (!uok(ubuf, (u32)strlen(name) + 1))
        return -1;
    memcpy((void *)ubuf, name, strlen(name) + 1);
    return (e.flags & FS_FLAG_DIR) ? -3 : (int)e.size;
}

static int sys_pkg_open(u32 upath)
{
    char path[160];
    int len;
    int h;
    u8 *data;

    if (ustr(upath, path, sizeof(path)) != 0)
        return -1;

    for (h = 0; h < UPKG_MAX; h++)
        if (!upkgs[h].used)
            break;
    if (h >= UPKG_MAX)
        return -2;

    data = (u8 *)kmalloc(512 * 1024);
    if (!data)
        return -3;
    len = fs_read(path, data, 512 * 1024);
    if (len < 0) {
        return -4;
    }
    if (!lst_validate(data, (u32)len)) {
        kprintf("[pkg] %s is not a valid .lst package\n", path);
        return -5;
    }
    upkgs[h].used = 1;
    upkgs[h].data = data;
    upkgs[h].size = (u32)len;
    return h;
}

static int sys_pkg_info(u32 h, u32 index, u32 ubuf, u32 bufsize)
{
    const lst_entry_t *e;

    if (h >= UPKG_MAX || !upkgs[h].used)
        return -1;
    e = lst_entry(upkgs[h].data, upkgs[h].size, index);
    if (!e)
        return -1;
    if (bufsize < 32)
        return -2;
    if (!uok(ubuf, 32))
        return -1;
    memcpy((void *)ubuf, e->name, 32);
    return (int)e->size;
}

static int sys_pkg_extract(u32 h, u32 index, u32 udest)
{
    const lst_entry_t *e;
    const u8 *payload;
    char dest[160];

    if (h >= UPKG_MAX || !upkgs[h].used)
        return -1;
    e = lst_entry(upkgs[h].data, upkgs[h].size, index);
    if (!e)
        return -1;
    if (ustr(udest, dest, sizeof(dest)) != 0)
        return -1;
    payload = lst_payload(upkgs[h].data, upkgs[h].size, index, NULL);
    if (!payload)
        return -1;
    if (fs_write(dest, payload, e->size) < 0)
        return -2;
    return (int)e->size;
}

void syscall_dispatch(regs_t *r)
{
    u32 n = r->eax;

    __asm__ volatile("sti");
    u32 a = r->ebx, b = r->ecx, c = r->edx, d = r->esi;
    s32 ret = -1;
    /* This buffer used to live on the stack, which matters because
     * syscall_dispatch() is the outermost frame of every syscall and the
     * kernel stack is shared with nested syscall helpers.  Keeping it
     * static frees 160 bytes of the deepest path. */
    static char tmp[160];

    switch (n) {
    case SYS_EXIT:
        /* user_exit() tears the program's window down itself, so every exit
         * path (normal return and exception kill) cleans up identically. */
        apt_kernel_reset();
        user_exit((int)a);
        break;

    case SYS_WRITE:
        if (a == USER_FD_STDOUT || a == USER_FD_STDERR) {
            u32 len = c;
            if (len > 4096)
                len = 4096;
            if (uok(b, len) == 0)
                break;
            uprint((const char *)b, len);
            pump();
            ret = (s32)len;
        } else if (a < UFD_MAX && ufds[a].used && ufds[a].writing) {
            u32 len = c;
            if (ufds[a].size + len > ufds[a].cap)
                len = ufds[a].cap - ufds[a].size;
            if (uok(b, len) == 0)
                break;
            memcpy(ufds[a].buf + ufds[a].size, (const void *)b, len);
            ufds[a].size += len;
            ret = (s32)len;
        }
        break;

    case SYS_READKEY:
        while ((ret = pop_key()) == -1) {
            pump();
            hlt();
        }
        break;

    case SYS_GETKEY:
        ret = pop_key();
        break;

    case SYS_OPEN:
        ret = sys_open(a, b);
        break;

    case SYS_CLOSE:
        ret = sys_close(a);
        break;

    case SYS_READ:
        if (a >= 3 && a < UFD_MAX && ufds[a].used && !ufds[a].writing) {
            u32 want = c;
            u32 avail = ufds[a].size - ufds[a].pos;
            if (want > avail)
                want = avail;
            if (uok(b, want) == 0) {
                ret = -1;
                break;
            }
            memcpy((void *)b, ufds[a].buf + ufds[a].pos, want);
            ufds[a].pos += want;
            ret = (s32)want;
        }
        break;

    case SYS_MKDIR:
        if (ustr(a, tmp, sizeof(tmp)) == 0)
            ret = (fs_mkdir(tmp) == 0) ? 0 : -2;
        break;

    case SYS_UNLINK:
        if (ustr(a, tmp, sizeof(tmp)) == 0)
            ret = (fs_delete(tmp) == 0) ? 0 : -2;
        break;

    case SYS_LIST:
        ret = sys_list(a, b, c, d);
        break;

    case SYS_EXISTS:
        if (ustr(a, tmp, sizeof(tmp)) == 0)
            ret = fs_exists(tmp) ? 1 : 0;
        break;

    case SYS_ISDIR:
        if (ustr(a, tmp, sizeof(tmp)) == 0)
            ret = fs_is_dir(tmp) ? 1 : 0;
        break;

    case SYS_SLEEP:
        timer_sleep_ms(a);
        ret = 0;
        break;

    case SYS_TIME:
        if (uok(a, 24)) {
            int *t = (int *)a;
            rtc_time_t now;
            rtc_read(&now);
            t[0] = now.year;
            t[1] = now.month;
            t[2] = now.day;
            t[3] = now.hour;
            t[4] = now.minute;
            t[5] = now.second;
            ret = 0;
        }
        break;

    case SYS_ARGS:
        /* Copies the argument into the caller's buffer.  Returns 0 on
         * success, matching what existing programs expect; use SYS_ARGS_LEN
         * when the emptiness of the argument matters. */
        if (a == 0) {
            if (uok(b, (u32)strlen(arg0) + 1)) {
                memcpy((void *)b, arg0, strlen(arg0) + 1);
                ret = 0;
            }
        } else {
            if (uok(b, (u32)strlen(arg1) + 1)) {
                memcpy((void *)b, arg1, strlen(arg1) + 1);
                ret = 0;
            }
        }
        break;

    case SYS_ARGS_LEN:
    {
        /* Length of argument `a`, without copying it.  Distinguishes "no
         * argument" from "an argument that happens to be empty". */
        const char *src = (a == 0) ? arg0 : arg1;

        ret = (int)strlen(src);
        break;
    }

    case SYS_PKG_OPEN:
        ret = sys_pkg_open(a);
        break;

    case SYS_PKG_COUNT:
        if (a < UPKG_MAX && upkgs[a].used)
            ret = lst_count(upkgs[a].data);
        break;

    case SYS_PKG_INFO:
        ret = sys_pkg_info(a, b, c, d);
        break;

    case SYS_PKG_EXTRACT:
        ret = sys_pkg_extract(a, b, c);
        break;

    case SYS_WIN_OPEN:
        if (uok(a, 1)) {
            const char *title = (const char *)a;
            if (ustr(a, tmp, sizeof(tmp)) == 0)
                title = tmp;
            ret = uapp_open(title, (int)b, (int)c);
        }
        break;

    case SYS_WIN_CLEAR:
        uapp_clear(a);
        ret = 0;
        break;

    case SYS_GFX_RECT:
        uapp_rect((int)a, (int)b, (int)c, (int)d, r->edi);
        ret = 0;
        break;

    case SYS_WIN_FRAME:
        uapp_frame((int)a, (int)b, (int)c, (int)d, r->edi);
        ret = 0;
        break;

    case SYS_GFX_TEXT:
        if (ustr(c, tmp, sizeof(tmp)) == 0) {
            uapp_text((int)a, (int)b, tmp, d, (int)r->edi);
            ret = 0;
        }
        break;

    case SYS_TEXT_W:
        /* width in pixels of a UTF-8 string, for centring text in user apps */
        if (ustr(a, tmp, sizeof(tmp)) == 0)
            ret = uapp_text_w(tmp, b > 0 ? (int)b : 1);
        break;

    case SYS_APT_OPEN:
    {
        /* a = title, b = width, c = height, d = &pixels (may be 0) */
        char title[64];
        const char *t = "application";
        u32 pixels = 0;

        if (uok(a, 1) && ustr(a, tmp, sizeof(tmp)) == 0)
            strncpy(title, tmp, sizeof(title) - 1);
        title[sizeof(title) - 1] = 0;
        if (title[0])
            t = title;

        if (apt_kernel_open(t, (int)b, (int)c, &pixels) != 0) {
            ret = -1;
            break;
        }
        if (d && uok(d, 4))
            *(u32 *)d = pixels;
        ret = 0;
        break;
    }

    case SYS_APT_PRESENT:
        ret = apt_kernel_present((int)a, (int)b, (int)c, (int)d);
        break;

    case SYS_APT_INFO:
        if (uok(a, 12)) {
            int *info = (int *)a;

            info[0] = apt_kernel_width();
            info[1] = apt_kernel_height();
            info[2] = (int)apt_kernel_user_pixels();
            ret = 0;
        }
        break;

    case SYS_APT_TEXT:
    {
        /* (x, y, str, colour, scale) - render into the shared canvas.  Text
         * is the one drawing call that traps, because the font tables live
         * in the kernel. */
        int x = (int)a, y = (int)b;
        u32 colour = (u32)c;
        int scale = (int)(r->esi);
        u32 *pixels = (u32 *)apt_kernel_pixels();
        int cw = apt_kernel_width(), chh = apt_kernel_height();

        if (!pixels || !uok(d, 1) || ustr(d, tmp, sizeof(tmp)) != 0) {
            ret = -1;
            break;
        }
        if (scale < 1) scale = 1;

        /* Point the compositor's icon at the user's buffer for the duration
         * of the call, then put it back.  gfx_glyph() writes through
         * gfx_pixel(), which targets the backbuffer, so we cheat: render to
         * the backbuffer clipped to nothing and copy the glyph out.  That
         * would be slow; instead the canvas is temporarily adopted. */
        apt_render_text(pixels, cw, chh, x, y, tmp, colour, scale);
        ret = apt_text_width(tmp, scale);
        break;
    }

    case SYS_WIN_CLOSE:
        uapp_close();
        ret = 0;
        break;

    case SYS_MMAP:
        ret = (int)apt_mmap((int)a);
        break;

    case SYS_MUNMAP:
        ret = apt_munmap((u32)a, (u32)b);
        break;

    case SYS_WIN_ALIVE:
        ret = uapp_alive();
        break;

    case SYS_AUDIO_INFO:
        ret = hda_present() ? (int)hda_info()->gain_steps : 0;
        break;

    case SYS_AUDIO_BEEP:
        if (!hda_present()) {
            ret = -1;
            break;
        }
        ret = hda_beep((int)a, (int)b, (int)c);
        break;

    case SYS_AUDIO_PLAY:
        /* a = user PCM pointer, b = bytes, c = volume */
        if (!hda_present()) {
            ret = -1;
            break;
        }
        if (b <= 0 || b > (int)AUDIO_MAX_BYTES || !uok(a, (u32)b)) {
            ret = -2;
            break;
        }
        {
            static u8 *kbuf;
            if (!kbuf) {
                kbuf = (u8 *)kmalloc(AUDIO_MAX_BYTES);
                if (!kbuf) {
                    ret = -3;
                    break;
                }
            }
            memcpy(kbuf, (const void *)a, (u32)b);
            ret = hda_play_start(kbuf, (u32)b, (int)c);
        }
        break;

    case SYS_LANG:
        /* 0 = English, 1 = Chinese; user apps keep their own string tables */
        ret = i18n_lang();
        break;

    case SYS_WIN_FLUSH:
        uapp_flush();
        pump();
        ret = 0;
        break;

    case SYS_MOUSE:
        {
            int mx, my, mb, inside;
            uapp_mouse(&mx, &my, &mb, &inside);
            if (uok(a, 4))
                *(int *)a = mx;
            if (uok(b, 4))
                *(int *)b = my;
            if (uok(c, 4))
                *(int *)c = mb;
            if (uok(d, 4))
                *(int *)d = inside;
            ret = 0;
        }
        break;

    case SYS_WIN_SIZE:
        ret = (uapp_width() << 16) | (uapp_height() & 0xFFFF);
        break;

    case SYS_PUMP:
        gui_cursor_refresh();
        ret = 0;
        break;

    case SYS_TICKS:
        ret = (s32)timer_ticks();
        break;

    case SYS_POWEROFF:
        kprintf("[setup] powering off via ACPI S5\n");
        acpi_shutdown();
        cli();
        for (;;)
            hlt();
        break;

    case SYS_BOOTMODE:
        ret = (s32)BOOTINFO->boot_drive;
        break;

    case SYS_RAWWRITE:
        {
            u32 lba = b;
            u32 count = c;
            if (ustr(a, tmp, sizeof(tmp)) != 0)
                break;
            if (count == 0 || count > 4096)
                break;
            if (!uok(d, count * 512))
                break;
            {
                int dev = blk_find(tmp);
                if (dev < 0)
                    break;
                if (blk_write(dev, lba, count, (const void *)d) != 0)
                    break;
                ret = (s32)count;
            }
        }
        break;

    case SYS_REBOOT:
        kprintf("[setup] rebooting\n");
        gui_poll();
        acpi_reboot();
        cli();
        for (;;)
            hlt();
        break;

    case SYS_DISK_INFO:
        {
            const blk_info_t *bi = blk_info((int)a);
            if (!bi || !bi->present) {
                ret = 0;
                break;
            }
            if (uok(b, 16)) {
                char *nm = (char *)b;
                int k;
                for (k = 0; k < 15 && bi->name[k]; k++)
                    nm[k] = bi->name[k];
                nm[k] = 0;
            }
            if (uok(c, 4))
                *(u32 *)c = bi->sectors;
            if (uok(d, 4))
                *(u32 *)d = (u32)bi->type;
            ret = 1;
        }
        break;

    case SYS_INSTALL:
        if (ustr(a, tmp, sizeof(tmp)) == 0) {
            int dev = blk_find(tmp);
            if (dev < 0) {
                ret = -1;
                break;
            }
            ret = (s32)fs_install(dev, b, "aptos");
            if (ret != 0)
                kprintf("[vfs] install on %s failed: %d (base %d)\n", tmp, ret, (int)b);
        }
        break;

    default:
        break;
    }

    r->eax = (u32)ret;
}
