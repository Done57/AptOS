/* splash - animated boot screen
 *
 * Replaces the on-screen kernel log with a graphical boot sequence: a logo
 * that breathes, an orbiting ring of dots, a progress bar driven by the
 * actual init steps, and a rotating status caption.  The textual log still
 * goes to the serial port, so nothing is lost for debugging.
 *
 * The animation advances only when the boot code says so (splash_tick()),
 * which keeps it honest: the bar reflects real progress, not a timer.
 */
#ifndef APTOS_SPLASH_H
#define APTOS_SPLASH_H

#include "types.h"

/* Take over the screen and draw the first frame. */
void splash_begin(void);

/* Advance to step `n` of `total`, showing `label`.  Also pumps the animation
 * by one frame, so every init step that calls this keeps the rings turning. */
void splash_step(int n, int total, const char *label);

/* Same, with a second caption line (Chinese).  Both are drawn, so the boot
 * screen reads correctly regardless of the selected language and before the
 * i18n tables are initialised. */
void splash_step2(int n, int total, const char *label, const char *label_zh);

/* Draw another animation frame without changing the progress.  Call this in
 * a loop while waiting for something slow. */
void splash_animate(void);

/* Final frame: fill the bar, show "starting the desktop", then fade out.
 * Returns after the screen is clear for the window manager. */
void splash_finish(const char *label);

#endif
