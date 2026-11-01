#ifndef APTOS_HDA_H
#define APTOS_HDA_H

/* Intel HD Audio (Azalia) controller + QEMU's "intel-hda" codec.
 *
 * The driver only does the one thing a toy OS needs: play PCM samples out of
 * the codec's output pins (speaker / line-out).  48 kHz 16 bit stereo.
 * BDL (buffer descriptor list) with two buffers ping-ponging via IRQ 16.
 */

#include "types.h"

#define HDA_FMT_48K_16B_2CH   ((2u << 4) | 1u)   /* base 48kHz, mult x1, div x1, BITS16, CHAN2 */

typedef struct {
    int present;
    int codec_addr;            /* 0..14, (SDI address << 4) | direct ncm(0) */
    u32 gain_steps;            /* output amp steps reported by the pin */
    char name[40];             /* decoded vendor/device, e.g. "Realtek ALC888" */
} hda_info_t;

int  hda_init(void);                       /* 0 on success */
int  hda_present(void);
const hda_info_t *hda_info(void);

/* Play 16 bit stereo PCM at 48 kHz.  Blocks until finished.  volume 0..100
 * maps to the pin's output amplifier range (step 0 = max). */
int  hda_play(const void *pcm, u32 bytes, int volume);

/* Short sine beep.  freq in Hz, ms duration, volume 0..100. */
int  hda_beep(int freq, int ms, int volume);

/* Asynchronous playback: start returns immediately, playing() reports
 * whether the controller is still consuming samples. */
int  hda_play_start(const void *pcm, u32 bytes, int volume);
int  hda_playing(void);
void hda_stop(void);

/* Generate a sine tone into caller-provided 16-bit stereo 48 kHz PCM. */
u32  hda_tone(void *buf, u32 bytes, int freq, int volume);

#endif
