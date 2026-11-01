/* Declarations for apt's small allocator.  The implementation lives in
 * apt_alloc.c so that programs which never allocate do not carry the arena
 * in their BSS. */
#ifndef APT_ALLOC_H
#define APT_ALLOC_H

#include "apt.h"

void *malloc(int n);
void  free(void *p);

/* Bytes still free in the arena, for diagnostics. */
int   apt_mem_free(void);

#endif /* APT_ALLOC_H */
