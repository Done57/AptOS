/* The allocator behind apt's malloc()/free().
 *
 * This lives in its own translation unit, and the arena is file-static, so a
 * program that links libapt but never allocates anything does not get the
 * arena in its BSS.  That matters here: the ELF loader allocates a program's
 * BSS in one block and never reclaims it, so an arena that everybody paid for
 * would show up as a 128 KiB floor on even the smallest program.
 */
#include "apt_alloc.h"
#include "aptos.h"



/* The arena sits in the program's BSS.  The ELF loader allocates BSS in one
 * block that is never reclaimed, so this size is paid by every program that
 * links libapt AND calls malloc().  128 KiB comfortably covers a UI's
 * textures without inflating the load footprint. */
#define APT_ARENA_SIZE (128 * 1024)

typedef struct apt_block {
    unsigned int      size;      /* payload size in bytes                */
    int               free;
    struct apt_block *next;
    struct apt_block *prev;
} apt_block;

#define APT_ALIGN8(n) (((n) + 7u) & ~7u)

/* How much of the arena a single request may take.  Anything larger goes
 * straight to the kernel, which keeps the arena free for the many small
 * allocations a UI makes. */
#define APT_ARENA_MAX_ALLOC (16 * 1024)

/* ------------------------------------------------------------------ */

static unsigned char apt_arena[APT_ARENA_SIZE] __attribute__((aligned(8)));
static apt_block    *apt_head;
static int           apt_ready;

static void apt_alloc_init(void)
{
    if (apt_ready)
        return;
    apt_head = (apt_block *)apt_arena;
    apt_head->size = APT_ARENA_SIZE - sizeof(apt_block);
    apt_head->free = 1;
    apt_head->next = NULL;
    apt_head->prev = NULL;
    apt_ready = 1;
}

/* ------------------------------------------------------------------ */
/* kernel-backed fallback                                             */

#define APT_MAGIC 0xA9C0DE01u

typedef struct {
    u32 magic;
    u32 size;
    u32 raw;         /* the pointer the kernel gave us */
} apt_big_hdr;

void *malloc(int n)
{
    apt_block *b;
    unsigned int want;

    if (n <= 0)
        return NULL;
    apt_alloc_init();

    want = APT_ALIGN8((unsigned int)n);

    if (want <= APT_ARENA_MAX_ALLOC) {
        for (b = apt_head; b; b = b->next) {
            if (!b->free || b->size < want)
                continue;

            /* Split when the remainder can hold a header plus something
             * useful; otherwise hand over the whole block. */
            if (b->size >= want + sizeof(apt_block) + 16) {
                apt_block *nb =
                    (apt_block *)((unsigned char *)b + sizeof(apt_block) + want);

                nb->size = b->size - want - sizeof(apt_block);
                nb->free = 1;
                nb->next = b->next;
                nb->prev = b;
                if (nb->next)
                    nb->next->prev = nb;
                b->next = nb;
                b->size = want;
            }
            b->free = 0;
            return (void *)((unsigned char *)b + sizeof(apt_block));
        }
    }

    /* Arena could not serve it: ask the kernel. */
    {
        unsigned int total = want + sizeof(apt_big_hdr);
        void *p = sys_mmap((int)total);

        if (!p)
            return NULL;
        {
            apt_big_hdr *h = (apt_big_hdr *)p;
            h->magic = APT_MAGIC;
            h->size = want;
            h->raw = (u32)p;
            return (unsigned char *)p + sizeof(apt_big_hdr);
        }
    }
}

void free(void *p)
{
    apt_block *b;

    if (!p)
        return;

    /* A pointer outside the arena must be one of the kernel-backed blocks.
     * The bounds check comes first: an arena pointer could coincidentally
     * look like a big block, but the reverse cannot happen. */
    if ((unsigned char *)p < apt_arena ||
        (unsigned char *)p >= apt_arena + APT_ARENA_SIZE) {
        apt_big_hdr *h = (apt_big_hdr *)((unsigned char *)p - sizeof(apt_big_hdr));

        if (h->magic == APT_MAGIC) {
            h->magic = 0;
            sys_munmap((void *)h->raw, (int)(h->size + sizeof(apt_big_hdr)));
            return;
        }
        return;   /* not ours; leaking is better than corrupting */
    }

    b = (apt_block *)((unsigned char *)p - sizeof(apt_block));
    b->free = 1;

    /* Coalesce forwards then backwards.  Doing both is what stops an
     * allocate/free loop from grinding the arena into 8-byte crumbs. */
    if (b->next && b->next->free) {
        apt_block *n = b->next;
        b->size += sizeof(apt_block) + n->size;
        b->next = n->next;
        if (n->next)
            n->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        apt_block *pr = b->prev;
        pr->size += sizeof(apt_block) + b->size;
        pr->next = b->next;
        if (b->next)
            b->next->prev = pr;
    }
}

/* Bytes still available in the arena, for diagnostics in demo programs. */
int apt_mem_free(void)
{
    apt_block *b;
    int total = 0;

    apt_alloc_init();
    for (b = apt_head; b; b = b->next)
        if (b->free)
            total += (int)b->size;
    return total;
}

/* The arena is one contiguous block, which is exactly what the kernel's
 * mmap gives us (it hands back page-aligned, physically contiguous memory).
 * Requesting it lazily means a program that links libapt but never allocates
 * has no arena at all. */
apt_block *apt_arena_head(void)
{
    return apt_head;
}

