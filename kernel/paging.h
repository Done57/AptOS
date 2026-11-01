#ifndef APTOS_PAGING_H
#define APTOS_PAGING_H

#include "types.h"

#define PAGE_SIZE 4096

#define USER_BASE  0xC0000000u
#define USER_LIMIT 0xD0000000u

void paging_init(void);
int  paging_map_device(u32 start, u32 size);
u32  paging_get_cr3(void);
int  paging_map_user(u32 vaddr, u32 paddr, u32 size, int writable);
int  paging_unmap_user(u32 vaddr, u32 size);
int  paging_user_range_ok(u32 vaddr, u32 len);
u32  paging_user_base(void);
u32  paging_user_limit(void);
u32  paging_directory(void);
void paging_dump(void);

#endif
