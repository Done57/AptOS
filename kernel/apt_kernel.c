/* apt_kernel - the kernel half of the apt graphics API
 *
 * The point of apt is that drawing does not trap.  A program asks for a
 * canvas once; the kernel allocates a physically contiguous block, keeps a
 * pointer to it for compositing, and maps the same pages into the program's
 * address space.  From then on the program writes pixels directly.
 *
 * The kernel only has to do two things afterwards:
 *
 *   - remember which region changed, so the window manager can blit just
 *     that part of the canvas to the screen instead of the whole thing
 *   - answer input queries
 *
 * A canvas is bound to the single ring 3 window (see uapp.c), because a
 * process only ever has one window in this system.
 */
#include "apt_kernel.h"
#include "paging.h"
#include "heap.h"
#include "gfx.h"
#include "uapp.h"
#include "klog.h"
#include "util.h"

/* Where the canvas is mapped in every program's address space.  Well clear of
 * the program image (loaded at 0xC0000000) and the user stack (0xC8000000). */
#define APT_MAP_BASE   0xC4000000u
#define APT_MAP_LIMIT  0xC6000000u          /* 32 MiB of address space */
#define APT_MAX_CANVAS (8u * 1024u * 1024u) /* 8 MiB, so 1004x700 fits   */

/* A bump-allocated heap region for programs.  libapt uses this when its own
 * arena cannot serve a request, which is how a program gets a texture bigger
 * than the arena without every program paying for the address space upfront.
 * Placed after the canvas window so the two cannot collide. */
#define APT_HEAP_BASE  0xC4800000u
#define APT_HEAP_LIMIT 0xC4C00000u          /* 4 MiB */
static u32 apt_heap_ptr = APT_HEAP_BASE;

/* Pages handed out by apt_mmap, tracked so munmap can find the physical
 * address again.  A small fixed table is plenty: these are big, rare
 * allocations. */
#define APT_MAP_SLOTS 32
static struct { u32 vaddr; u32 paddr; u32 size; } apt_maps[APT_MAP_SLOTS];

u32 apt_mmap(int bytes)
{
    u32 need, phys, vaddr;
    int i, slot = -1;

    if (bytes <= 0)
        return 0;
    need = ((u32)bytes + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);

    if (apt_heap_ptr + need > APT_HEAP_LIMIT) {
        kprintf("[apt] user heap exhausted (%u bytes requested)\n", need);
        return 0;
    }
    for (i = 0; i < APT_MAP_SLOTS; i++) {
        if (apt_maps[i].vaddr == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kprintf("[apt] no free mmap slot\n");
        return 0;
    }

    phys = (u32)kmalloc_a(need, PAGE_SIZE);
    if (!phys)
        return 0;
    memset((void *)phys, 0, need);

    vaddr = apt_heap_ptr;
    if (paging_map_user(vaddr, phys, need, 1) != 0) {
        kprintf("[apt] could not map %u bytes for the program\n", need);
        return 0;
    }
    apt_heap_ptr += need;

    apt_maps[slot].vaddr = vaddr;
    apt_maps[slot].paddr = phys;
    apt_maps[slot].size = need;
    return vaddr;
}

int apt_munmap(u32 vaddr, u32 size)
{
    int i;

    if (!vaddr)
        return -1;
    for (i = 0; i < APT_MAP_SLOTS; i++) {
        if (apt_maps[i].vaddr == vaddr) {
            u32 sz = apt_maps[i].size;

            paging_unmap_user(vaddr, sz);
            apt_maps[i].vaddr = 0;
            apt_maps[i].paddr = 0;
            apt_maps[i].size = 0;

            /* Only give the bump pointer back when this was the newest block,
             * which is the common case for a program that allocates one big
             * texture, uses it, and frees it. */
            if (vaddr + sz == apt_heap_ptr)
                apt_heap_ptr = vaddr;
            (void)size;
            return 0;
        }
    }
    return -1;
}

/* Called when a program exits: drop every mapping and rewind the heap. */
void apt_heap_reset(void)
{
    int i;

    for (i = 0; i < APT_MAP_SLOTS; i++) {
        if (apt_maps[i].vaddr) {
            paging_unmap_user(apt_maps[i].vaddr, apt_maps[i].size);
            apt_maps[i].vaddr = 0;
            apt_maps[i].paddr = 0;
            apt_maps[i].size = 0;
        }
    }
    apt_heap_ptr = APT_HEAP_BASE;
}

static u32 *apt_pixels;
static u32  apt_bytes;
static int  apt_w, apt_h;
static int  apt_mapped;            /* mapped into a program right now */
static u32  apt_map_base;

/* Dirty bookkeeping.  The kernel tracks it as well as the library so a
 * canvas that is written without ever calling flush still composites
 * correctly the first time. */
static int apt_dirty;

static int rows_bytes(int w)
{
    return w * 4;
}

int apt_kernel_open(const char *title, int w, int h, u32 *out_pixels)
{
    u32 need;
    u32 map;

    if (w < 64 || h < 64)
        return -1;
    if (w > UAPP_MAX_W || h > UAPP_MAX_H)
        return -1;

    need = (u32)w * (u32)h * 4u;
    if (need > APT_MAX_CANVAS)
        return -1;

    /* The canvas has to be physically contiguous because it is mapped page by
     * page, so it cannot be reallocated piecemeal.  The heap is a linear
     * bump allocator, which gives us that guarantee for free. */
    apt_pixels = (u32 *)kmalloc_a(need, PAGE_SIZE);
    if (!apt_pixels) {
        kprintf("[apt] no memory for a %dx%d canvas (%u bytes)\n", w, h, need);
        return -1;
    }

    memset(apt_pixels, 0, need);
    apt_bytes = need;
    apt_w = w;
    apt_h = h;
    apt_dirty = 0;

    map = APT_MAP_BASE;
    if (!apt_mapped) {
        if (paging_map_user(map, (u32)apt_pixels, need, 1) != 0) {
            kprintf("[apt] could not map the canvas into user space\n");
            apt_pixels = NULL;
            return -1;
        }
        apt_mapped = 1;
        apt_map_base = map;
    } else if (apt_map_base != map) {
        /* A previous canvas left the region mapped somewhere else; drop it. */
        paging_unmap_user(apt_map_base, apt_bytes);
        if (paging_map_user(map, (u32)apt_pixels, need, 1) != 0)
            return -1;
        apt_map_base = map;
    }

    /* Hand the window manager the pixels so it can composite them. */
    if (uapp_attach_canvas(apt_pixels, w, h, title) != 0) {
        kprintf("[apt] could not open a window for the canvas\n");
        paging_unmap_user(map, need);
        apt_mapped = 0;
        apt_pixels = NULL;
        return -1;
    }

    /* The program wants the address *it* can write to, not the physical one
     * the kernel composites from. */
    if (out_pixels)
        *out_pixels = map;
    kprintf("[apt] canvas %dx%d, %u KiB at 0x%x, mapped at 0x%x\n",
            w, h, need / 1024, (u32)apt_pixels, map);
    return 0;
}

int apt_kernel_active(void)
{
    return apt_pixels != NULL && apt_mapped;
}

/* The kernel's own view of the pixels - used for compositing. */
u32 apt_kernel_pixels(void)
{
    return (u32)apt_pixels;
}

/* The address the *program* should use.  This is the whole point of the
 * design: one block of memory, two addresses.  Handing out the kernel's
 * physical pointer here would fault the moment the program drew. */
u32 apt_kernel_user_pixels(void)
{
    return apt_mapped ? apt_map_base : 0;
}

int apt_kernel_width(void)  { return apt_w; }
int apt_kernel_height(void) { return apt_h; }

int apt_kernel_present(int x, int y, int w, int h)
{
    if (!apt_pixels || !apt_mapped)
        return -1;
    if (w <= 0 || h <= 0)
        return -1;

    /* Clamp to the canvas so a buggy program cannot ask the compositor to
     * read outside its own buffer. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > apt_w) w = apt_w - x;
    if (y + h > apt_h) h = apt_h - y;
    if (w <= 0 || h <= 0)
        return -1;

    apt_dirty = 0;
    uapp_present(x, y, w, h);
    return 0;
}

/* Render text straight into the shared canvas.  Rather than duplicating the
 * font tables into every program (the CJK set alone is large), the glyph
 * renderer is pointed at the canvas for the duration of the call. */
int apt_render_text(u32 *pixels, int w, int h, int x, int y, const char *s,
                    u32 colour, int scale)
{
    rect_t r;
    int start = x;

    if (!pixels || !s)
        return 0;

    r.x0 = 0;
    r.y0 = 0;
    r.x1 = w - 1;
    r.y1 = h - 1;

    gfx_push_target(pixels, w, h, r);
    while (*s) {
        int adv = 0;
        u32 cp = gfx_utf8_decode(s, &adv);
        if (adv <= 0)
            break;
        gfx_glyph(x, y, cp, colour, scale);
        x += gfx_glyph_w(cp, scale);
        s += adv;
    }
    gfx_pop_target();
    return x - start;
}

int apt_text_width(const char *s, int scale)
{
    return gfx_text_w(s, scale);
}

void apt_kernel_close(void)
{
    if (apt_mapped) {
        paging_unmap_user(apt_map_base, apt_bytes);
        apt_mapped = 0;
    }
    apt_pixels = NULL;
    apt_bytes = 0;
    apt_w = apt_h = 0;
    apt_dirty = 0;
}

void apt_kernel_reset(void)
{
    apt_heap_reset();
    /* Called when a program dies.  The pixels are handed back to the heap in
     * the sense that nobody references them any more; the mapping is dropped
     * so the next program gets a clean address space. */
    apt_kernel_close();
}
