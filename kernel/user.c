#include "user.h"
#include "uapp.h"
#include "apt_kernel.h"
#include "elf.h"
#include "gdt.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "gui.h"
#include "syscall.h"
#include "util.h"

extern void user_enter(u32 entry, u32 stack);
extern void user_set_kernel_stack(u32 top);
extern void user_exit_asm(int status);
extern u32 user_exit_status;

static int active;
static int last_status;
static u8 *kstack;

/* The kernel stack used while a ring 3 program runs.  It has to absorb the
 * deepest syscall chain (sys_open -> fs_read -> mfs_read -> blk_read, plus
 * the per-syscall 160 byte scratch buffer) *and* any timer interrupt that
 * lands in the middle of it.  Reading a large file keeps interrupts enabled
 * for a long time, so 32 KiB was not enough: the stack walked off its top
 * and corrupted the interrupt frame, which showed up as a page fault at
 * eip=0xffffffff.  128 KiB leaves plenty of headroom. */
#define KSTACK_SIZE (128 * 1024)

int user_active(void)
{
    return active;
}

int user_last_status(void)
{
    return last_status;
}

void user_exit(int status)
{
    /* Tear down whatever the program left on screen *before* unwinding.
     * This used to live in the SYS_EXIT handler only, which meant a program
     * killed by an exception (or by the user) left its window behind: the
     * window stayed in gui's table with used=1 and its canvas still holding
     * the dead program's pixels.  The next program to ask for a window was
     * handed that zombie, so e.g. the terminal drew the spider solitaire
     * board and its keystrokes went nowhere.  Doing it here covers every
     * way a process can end. */
    uapp_close();
    /* Drop the canvas mapping too.  A program killed by an exception never
     * gets to call apt_close(), and leaving the mapping in place would hand
     * a stale pointer to whatever runs next. */
    apt_kernel_reset();

    /* Belt and braces: if the program somehow left a window behind (it never
     * called uapp_close, or the window was opened outside the uapp layer),
     * mark it so gui_open() will not hand it to the next program and the
     * window manager will reap it. */
    gui_orphan_user_windows();

    active = 0;
    last_status = status;
    user_exit_asm(status);
    kprintf("[user] WARNING: exit returned\n");
}

int user_exec_image(const void *elf, u32 size, const char *arg0, const char *arg1)
{
    elf_image_t img;
    u32 stack_phys;
    int r;

    if (active)
        return -1;
    if (elf_load(elf, size, &img) != 0)
        return -2;

    if (!kstack)
        kstack = (u8 *)kmalloc(KSTACK_SIZE);
    if (!kstack) {
        elf_unload(&img);
        return -3;
    }
    tss_set_stack((u32)kstack + KSTACK_SIZE - 16);
    /* Ring 3 code runs on its own kernel stack, so a deep syscall inside a
     * program cannot collide with whatever the caller was doing. */
    user_set_kernel_stack((u32)kstack + KSTACK_SIZE - 64);

    stack_phys = (u32)kmalloc_a(USER_STACK_SIZE, PAGE_SIZE);
    if (!stack_phys) {
        elf_unload(&img);
        return -4;
    }
    memset((void *)stack_phys, 0, USER_STACK_SIZE);
    if (paging_map_user(USER_STACK_TOP - USER_STACK_SIZE, stack_phys,
                        USER_STACK_SIZE, 1) != 0) {
        elf_unload(&img);
        return -5;
    }

    syscall_set_args(arg0, arg1);

    active = 1;
    last_status = 0;


    user_enter(img.entry, USER_STACK_TOP - 16);

    active = 0;
    last_status = (int)user_exit_status;
    gui_dirty();
    gui_poll();
    elf_unload(&img);
    paging_unmap_user(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE);
    r = last_status;
    if (r != 0)
        kprintf("[user] process exited with status %d\n", r);
    return r;
}
