/* Regression helper: opens a window, paints it a recognisable colour, then
 * deliberately faults so the process is killed by the CPU rather than by
 * SYS_EXIT.  That is exactly the path that used to leave a zombie window
 * behind - opening the terminal afterwards would show this window's pixels
 * and its keystrokes went to a dead program. */
#include "aptos.h"

int main(void)
{
    if (sys_win_open("CRASH-TEST", 320, 240) != 0)
        return 1;

    /* bright magenta so the leftover canvas is unmistakable in a screenshot */
    sys_clear(0xFF00FF);
    sys_flush();
    sys_sleep(700);

    /* wander off the end of the mapped image: the CPU raises a page fault
     * and the kernel kills us without running any user cleanup. */
    *(volatile unsigned int *)0xDEADB000 = 0x1234;

    return 0;
}
