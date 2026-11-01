/*
 * x86-64 四级页表实现。
 *
 * 设计取舍
 * --------
 * 1. **恒等映射内核**。内核代码在 1MB，页表也放在低内存；这一段用 2MB
 *    大页恒等映射，省掉大量页表项，也让"物理地址就是虚拟地址"成立，
 *    于是页表操作本身不需要额外的映射技巧。
 *
 * 2. **页表页来自一个极简的 bump 分配器**。真正的物理内存管理器
 *    （paging.c 里那套）还没迁过来，而页表必须先能用。所以这里先用
 *    一个"从 _kernel_end 之后往上要页"的 bump 分配器，等物理内存
 *    管理就绪后再换成正式的。
 *
 * 3. **不支持超 512GB 物理内存**。PML4 一层只能覆盖 512 个 PDPT，
 *    每个 PDPT 覆盖 1GB。够用了。
 */

#include "paging64.h"
#include "klog.h"
#include "util.h"

/* 页表页的大小 */
#define PT_PAGES 512

/* 内核页表自己用的物理内存池。放在 16MB 起，避开内核映像与
 * stage2 留在低内存里的引导数据结构。 */
#define PT_POOL_BASE   0x01000000ull
#define PT_POOL_LIMIT  0x02000000ull        /* 16MB 池，够建 4096 张页表 */

static u64  *pml4;
static u64   pool_next = PT_POOL_BASE;
static u64   mapped_bytes;
static u64   pool_pages_used;

/* ------------------------------------------------------------------ */
/* 极简物理页分配：从池里顺序切页                                     */
/* ------------------------------------------------------------------ */

static u64 pool_alloc_page(void)
{
    u64 p = pool_next;

    if (p + P64_PAGE_SIZE > PT_POOL_LIMIT) {
        kprintf("[p64] 页表池耗尽 (base=0x%x limit=0x%x)\n",
                PT_POOL_BASE, PT_POOL_LIMIT);
        return 0;
    }
    pool_next += P64_PAGE_SIZE;
    pool_pages_used++;

    /* 必须清空：页表项里的残留位会被当成 present / user 等标志，
     * 后果是莫名其妙地能访问不该访问的地址。*/
    memset((void *)(uptr)p, 0, P64_PAGE_SIZE);
    return p;
}

/* 取出表项里的物理地址部分 */
static inline u64 entry_addr(u64 e)
{
    return e & P64_PAGE_MASK;
}

static inline int entry_present(u64 e)
{
    return (e & P64_PRESENT) != 0;
}

/* 按索引逐级向下走，必要时创建中间层。
 * 返回最底层（PT）的指针，失败返回 NULL。*/
static u64 *walk_create(u64 vaddr)
{
    u64  i4 = (vaddr >> 39) & 0x1FF;
    u64  i3 = (vaddr >> 30) & 0x1FF;
    u64  i2 = (vaddr >> 21) & 0x1FF;
    u64 *pdpt, *pd, *pt;

    if (!entry_present(pml4[i4])) {
        u64 page = pool_alloc_page();
        if (!page)
            return 0;
        /* 中间层带 RW 但不带 USER：内核页表，用户态不该直接改 */
        pml4[i4] = page | P64_PRESENT | P64_RW;
    }
    pdpt = (u64 *)(uptr)entry_addr(pml4[i4]);

    if (!entry_present(pdpt[i3])) {
        u64 page = pool_alloc_page();
        if (!page)
            return 0;
        pdpt[i3] = page | P64_PRESENT | P64_RW;
    }
    pd = (u64 *)(uptr)entry_addr(pdpt[i3]);

    if (!entry_present(pd[i2])) {
        u64 page = pool_alloc_page();
        if (!page)
            return 0;
        pd[i2] = page | P64_PRESENT | P64_RW;
    }
    pt = (u64 *)(uptr)entry_addr(pd[i2]);

    return pt;
}

/* 只查不建，返回最底层表项指针（可能是 PD 层的 2MB 大页） */
static u64 *walk_find(u64 vaddr)
{
    u64  i4 = (vaddr >> 39) & 0x1FF;
    u64  i3 = (vaddr >> 30) & 0x1FF;
    u64  i2 = (vaddr >> 21) & 0x1FF;
    u64 *pdpt, *pd, *pt;

    if (!entry_present(pml4[i4]))
        return 0;
    pdpt = (u64 *)(uptr)entry_addr(pml4[i4]);
    if (!entry_present(pdpt[i3]))
        return 0;
    /* PDPT 层的 1GB 大页 */
    if (pdpt[i3] & P64_HUGE)
        return &pdpt[i3];
    pd = (u64 *)(uptr)entry_addr(pdpt[i3]);
    if (!entry_present(pd[i2]))
        return 0;
    /* PD 层的 2MB 大页 */
    if (pd[i2] & P64_HUGE)
        return &pd[i2];
    pt = (u64 *)(uptr)entry_addr(pd[i2]);
    return &pt[vaddr & 0x1FF];
}

static void flush_tlb_page(u64 vaddr)
{
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

int p64_map_page(u64 vaddr, u64 paddr, u64 flags)
{
    u64 *pt;

    if (vaddr & (P64_PAGE_SIZE - 1))
        return -1;
    if (paddr & (P64_PAGE_SIZE - 1))
        return -1;
    if (paddr > P64_PHYS_MAX)
        return -1;

    pt = walk_create(vaddr);
    if (!pt)
        return -1;

    pt[vaddr & 0x1FF] = (paddr & P64_PAGE_MASK) | flags | P64_PRESENT;
    flush_tlb_page(vaddr);
    return 0;
}

int p64_map_range(u64 vaddr, u64 paddr, u64 size, u64 flags)
{
    u64 off;

    if (size == 0)
        return 0;
    if ((vaddr & (P64_PAGE_SIZE - 1)) || (paddr & (P64_PAGE_SIZE - 1)))
        return -1;

    for (off = 0; off < size; off += P64_PAGE_SIZE) {
        if (p64_map_page(vaddr + off, paddr + off, flags) != 0)
            return -1;
    }
    return 0;
}

int p64_unmap_page(u64 vaddr)
{
    u64 *e = walk_find(vaddr);

    if (!e || !entry_present(*e))
        return -1;
    *e = 0;
    flush_tlb_page(vaddr);
    return 0;
}

int p64_unmap_range(u64 vaddr, u64 size)
{
    u64 off;

    for (off = 0; off < size; off += P64_PAGE_SIZE)
        p64_unmap_page(vaddr + off);
    return 0;
}

u64 p64_translate(u64 vaddr)
{
    u64 *e = walk_find(vaddr);

    if (!e || !entry_present(*e))
        return 0;
    if (*e & P64_HUGE) {
        /* 大页：低 21 位（2MB）或低 30 位（1GB）保留自地址 */
        if (*e & P64_HUGE)
            return entry_addr(*e) | (vaddr & (P64_HUGE_2M - 1));
    }
    return entry_addr(*e) | (vaddr & (P64_PAGE_SIZE - 1));
}

int p64_map_identity_2m(u64 start, u64 size, u64 flags)
{
    u64 addr;

    start &= ~(P64_HUGE_2M - 1);
    size = (size + P64_HUGE_2M - 1) & ~(P64_HUGE_2M - 1);

    for (addr = start; addr < start + size; addr += P64_HUGE_2M) {
        u64  i4 = (addr >> 39) & 0x1FF;
        u64  i3 = (addr >> 30) & 0x1FF;
        u64  i2 = (addr >> 21) & 0x1FF;
        u64 *pdpt, *pd;

        if (!entry_present(pml4[i4])) {
            u64 page = pool_alloc_page();
            if (!page)
                return -1;
            pml4[i4] = page | P64_PRESENT | P64_RW;
        }
        pdpt = (u64 *)(uptr)entry_addr(pml4[i4]);

        if (!entry_present(pdpt[i3])) {
            u64 page = pool_alloc_page();
            if (!page)
                return -1;
            pdpt[i3] = page | P64_PRESENT | P64_RW;
        }
        pd = (u64 *)(uptr)entry_addr(pdpt[i3]);

        pd[i2] = (addr & P64_PAGE_MASK) | flags | P64_PRESENT | P64_HUGE;
        flush_tlb_page(addr);
    }
    return 0;
}

void p64_init(u64 initial_map_bytes)
{
    u64 mapped;

    /* PML4 本身也从池里要一页 */
    pool_next = PT_POOL_BASE;
    pml4 = (u64 *)(uptr)pool_alloc_page();
    if (!pml4) {
        kprintf("[p64] 无法分配 PML4\n");
        return;
    }

    /* 至少映射内核所在的低 1GB（内核在 1MB，页表池在 16~32MB） */
    mapped = initial_map_bytes;
    if (mapped < P64_HUGE_1G)
        mapped = P64_HUGE_1G;
    mapped = (mapped + P64_HUGE_1G - 1) & ~(P64_HUGE_1G - 1);

    if (p64_map_identity_2m(0, mapped, P64_RW) != 0) {
        kprintf("[p64] 恒等映射失败\n");
        return;
    }

    mapped_bytes = mapped;

    __asm__ volatile("mov %0, %%cr3" :: "r"((u64)(uptr)pml4) : "memory");

    kprintf("[p64] 四级页表就绪: PML4=0x%lx 恒等映射=%d MB 用了 %d 张页表\n",
            (u64)(uptr)pml4, (u32)(mapped >> 20), (u32)pool_pages_used);
}

u64 p64_get_cr3(void)
{
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

u64 *p64_root(void)
{
    return pml4;
}

void p64_dump_stats(void)
{
    kprintf("[p64] CR3=0x%lx 池指针=0x%lx 已用页表=%d 恒等映射=%d MB\n",
            p64_get_cr3(), pool_next, (u32)pool_pages_used,
            (u32)(mapped_bytes >> 20));
}
