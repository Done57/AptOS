#include "timer.h"
#include "idt.h"
#include "io.h"
#include "klog.h"

#define PIT_FREQ 1193182u
#define PIT_CH0  0x40
#define PIT_CMD  0x43

static volatile u32 ticks;
static u32 tick_hz = 100;

static void timer_irq(regs_t *r)
{
    (void)r;
    ticks++;
}

void timer_init(u32 hz)
{
    if (hz == 0)
        hz = 100;
    tick_hz = hz;
    ticks = 0;

    u32 divisor = PIT_FREQ / hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(divisor & 0xFF));
    outb(PIT_CH0, (u8)((divisor >> 8) & 0xFF));

    irq_register(0, timer_irq);
    irq_unmask(0);
    kprintf("[pit] timer at %d Hz (divisor %d)\n", hz, divisor);
}

u32 timer_hz(void) { return tick_hz; }
u32 timer_ticks(void) { return ticks; }

u32 timer_uptime_ms(void)
{
    return (u32)((u64)ticks * 1000ull / (u64)tick_hz);
}

void timer_sleep_ms(u32 ms)
{
    u32 target = ticks + (ms * tick_hz) / 1000u + 1u;

    while (ticks < target)
        hlt();
}

/* Same wait, but it also pumps a callback while it waits.  Used by the
 * window manager so the screen keeps refreshing during a sleep. */
void timer_sleep_ms_pump(u32 ms, void (*pump)(void))
{
    u32 target = ticks + (ms * tick_hz) / 1000u + 1u;

    while (ticks < target) {
        if (pump)
            pump();
        hlt();
    }
}
