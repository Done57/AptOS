#ifndef APTOS_PAGING64_H
#define APTOS_PAGING64_H

/*
 * x86-64 四级页表。
 *
 * 与 32 位版本的根本差别不只是"多两级"，而是**地址不再能塞进 32 位**：
 * 32 位版本里几乎所有地址参数都是 u32，映射表项本身也是 u32。到了 64 位，
 * 任何一个漏改的 u32 都会在超过 4GB 时静默截断。所以这个模块的所有
 * 地址参数都用 u64 / uptr。
 *
 * 长模式下的地址还有一个额外约束：**规范地址**。x86-64 目前只用 48 位
 * 虚拟地址，第 48..63 位必须全部等于第 47 位（符号扩展）。不合规的
 * 地址一旦被解引用就 #GP，而不是页错误 —— 调试时这点很容易看错。
 */

#include "types.h"

#define P64_PAGE_SIZE   4096ull
#define P64_PAGE_MASK   0x000FFFFFFFFFF000ull

/* 页表项标志位 */
#define P64_PRESENT     (1ull << 0)
#define P64_RW          (1ull << 1)
#define P64_USER        (1ull << 2)
#define P64_PWT         (1ull << 3)
#define P64_PCD         (1ull << 4)
#define P64_ACCESSED    (1ull << 5)
#define P64_DIRTY       (1ull << 6)
#define P64_HUGE        (1ull << 7)     /* PS：PD/PDPT 层表示大页 */
#define P64_GLOBAL      (1ull << 8)
#define P64_NX          (1ull << 63)

/* 2MB 大页与 1GB 大页 */
#define P64_HUGE_2M     (2ull * 1024 * 1024)
#define P64_HUGE_1G     (1024ull * 1024 * 1024)

/* 规范地址检查：第 48..63 位必须是第 47 位的符号扩展 */
#define P64_CANONICAL(a) \
    ((((u64)(a) >> 47) == 0) || (((u64)(a) >> 47) == 0x1FFFFull))

/* 物理内存上限：一级页表项只能放 52 位物理地址 */
#define P64_PHYS_MAX    0x000FFFFFFFFFF000ull

/* 初始化：建立内核的四级页表并切换过去。
 * initial_map_bytes 指定要恒等映射多少物理内存（会被向上取整到 1GB）。*/
void     p64_init(u64 initial_map_bytes);

/* 取当前 CR3 / PML4 物理地址 */
u64      p64_get_cr3(void);
u64     *p64_root(void);

/* 映射一个 4KB 页。vaddr 与 paddr 都必须页对齐。
 * 返回 0 成功，-1 失败。*/
int      p64_map_page(u64 vaddr, u64 paddr, u64 flags);

/* 映射一段内存，用 4KB 页。*/
int      p64_map_range(u64 vaddr, u64 paddr, u64 size, u64 flags);

/* 解除映射，并释放沿途的空页表（可选）。*/
int      p64_unmap_page(u64 vaddr);
int      p64_unmap_range(u64 vaddr, u64 size);

/* 查询 vaddr 对应的物理地址；未映射返回 0。*/
u64      p64_translate(u64 vaddr);

/* 用 2MB 大页恒等映射一段物理内存（用于内核区，省表项）。*/
int      p64_map_identity_2m(u64 start, u64 size, u64 flags);

/* 诊断 */
void     p64_dump_stats(void);

#endif
