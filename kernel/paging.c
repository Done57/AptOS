#include "paging.h"
#include "bootinfo.h"
#include "heap.h"
#include "klog.h"
#include "util.h"

#define PDE_PRESENT 0x01
#define PDE_RW      0x02
#define PDE_USER    0x04

static u32 *pd;
static u32 enabled;
static u32 fb_mapped_kb;

static void flush_tlb(void)
{
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
}

static u32 *pt_for(u32 vaddr, int user, int create)
{
    u32 idx = vaddr >> 22;
    u32 entry = pd[idx];

    if (entry & PDE_PRESENT)
        return (u32 *)(entry & ~0xFFFu);
    if (!create)
        return NULL;
    {
        u32 *pt = (u32 *)kmalloc_a(PAGE_SIZE, PAGE_SIZE);
        if (!pt)
            return NULL;
        memset(pt, 0, PAGE_SIZE);
        pd[idx] = (u32)pt | PDE_PRESENT | PDE_RW | (user ? PDE_USER : 0);
        return pt;
    }
}

static int map_identity(u32 start, u32 size)
{
    u32 a;

    start &= ~0xFFFu;
    size = (size + 0xFFFu) & ~0xFFFu;

    for (a = start; a < start + size; a += PAGE_SIZE) {
        u32 *pt = pt_for(a, 0, 1);
        if (!pt)
            return -1;
        pt[(a >> 12) & 0x3FF] = a | PDE_PRESENT | PDE_RW;
    }
    return 0;
}

static void map_framebuffer(void)
{
    u32 fb = BOOTINFO->fb_addr;
    u32 bytes;

    fb_mapped_kb = 0;
    if (!fb)
        return;

    bytes = BOOTINFO->fb_pitch * BOOTINFO->fb_height;
    if (bytes < 1024 * 1024)
        bytes = 1024 * 1024;
    bytes += 1024 * 1024;

    if (map_identity(fb, bytes) == 0)
        fb_mapped_kb = bytes / 1024;
}

/* Identity-map a physical MMIO region (PCI BARs live above the RAM identity
 * window).  Call it once, before any driver touches the device. */
u32 paging_get_cr3(void)
{
    return (u32)pd;
}

int paging_map_device(u32 start, u32 size)
{
    return map_identity(start, size);
}

void paging_init(void)
{
    u32 top = mem_top_addr();
    u32 map_mb = (top + 0xFFFFFu) / 0x100000u;
    u32 addr;

    if (map_mb < 16)
        map_mb = 16;
    if (map_mb > 3072)
        map_mb = 3072;
    map_mb = (map_mb + 3) & ~3u;

    pd = (u32 *)kmalloc_a(PAGE_SIZE, PAGE_SIZE);
    if (!pd) {
        kprintf("[mmu] cannot allocate a page directory - paging left off\n");
        return;
    }
    memset(pd, 0, PAGE_SIZE);

    for (addr = 0; addr < map_mb * 0x100000u; addr += 0x400000u) {
        u32 *pt;

        if (addr >= USER_BASE && addr < USER_LIMIT) {
            kprintf("[mmu] WARNING: RAM reaches the user window at 0x%x\n", addr);
            break;
        }
        pt = (u32 *)kmalloc_a(PAGE_SIZE, PAGE_SIZE);
        if (!pt) {
            kprintf("[mmu] out of memory building page tables\n");
            return;
        }
        memset(pt, 0, PAGE_SIZE);
        pd[addr >> 22] = (u32)pt | PDE_PRESENT | PDE_RW;
        for (u32 i = 0; i < 1024; i++)
            pt[i] = (addr + i * PAGE_SIZE) | PDE_PRESENT | PDE_RW;
    }

    map_framebuffer();

    __asm__ volatile("mov %%eax, %%cr3" ::"a"(pd));

    {
        u32 cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= 0x80000000u;
        __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
    }

    enabled = 1;
    flush_tlb();
    kprintf("[mmu] paging on: 4 KiB pages, identity 0..%d MiB, framebuffer 0x%x (%d KiB)\n",
            map_mb, BOOTINFO->fb_addr, fb_mapped_kb);
}

int paging_map_user(u32 vaddr, u32 paddr, u32 size, int writable)
{
    u32 off = 0;
    (void)writable;

    if (!enabled)
        return -1;
    if (vaddr & 0xFFF)
        return -1;
    if (vaddr < paging_user_base() || vaddr + size > paging_user_limit())
        return -1;

    while (off < size) {
        u32 *pt = pt_for(vaddr + off, 1, 1);
        if (!pt)
            return -1;
        pt[((vaddr + off) >> 12) & 0x3FF] =
            (paddr + off) | PDE_PRESENT | PDE_RW | PDE_USER;
        off += PAGE_SIZE;
    }
    flush_tlb();
    return 0;
}

int paging_unmap_user(u32 vaddr, u32 size)
{
    u32 off = 0;
    while (off < size) {
        u32 *pt = pt_for(vaddr + off, 1, 0);
        if (pt)
            pt[((vaddr + off) >> 12) & 0x3FF] = 0;
        off += PAGE_SIZE;
    }
    flush_tlb();
    return 0;
}

int paging_user_range_ok(u32 vaddr, u32 len)
{
    u32 a, last;

    if (!enabled || len == 0)
        return 0;
    if (vaddr < paging_user_base())
        return 0;
    if (vaddr + len < vaddr)
        return 0;
    if (vaddr + len > paging_user_limit())
        return 0;

    last = vaddr + len - 1;
    for (a = vaddr & ~0xFFFu; a <= last; a += PAGE_SIZE) {
        u32 *pt = pt_for(a, 1, 0);
        u32 e;

        if (!pt)
            return 0;
        e = pt[(a >> 12) & 0x3FF];
        if (!(e & 1) || !(e & PDE_USER))
            return 0;
        if (a + PAGE_SIZE < a)
            break;
    }
    return 1;
}

u32 paging_user_base(void) { return USER_BASE; }
u32 paging_user_limit(void) { return USER_LIMIT; }
u32 paging_directory(void) { return (u32)pd; }

void paging_dump(void)
{
    kprintf("[mmu] page directory at 0x%x, user window 0x%x..0x%x\n", (u32)pd,
            paging_user_base(), paging_user_limit());
}
