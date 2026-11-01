#include "hda.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "paging.h"
#include "klog.h"
#include "pci.h"
#include "timer.h"
#include "util.h"

/* ---- controller registers (MMIO, 32 bit unless noted) ----------------- */
#define HDA_GCAP   0x00
#define HDA_VMAJ   0x02      /* u16 */
#define HDA_OUTPAY 0x0C
#define HDA_INPAY  0x0E
#define HDA_GCTL   0x08
#define HDA_WAKEEN 0x0C
#define HDA_STATESTS 0x0E    /* u16: codec presence bits */
#define HDA_ICW    0x60
#define HDA_IRR    0x64
#define HDA_ICS    0x68
#define HDA_CORBWP 0x48     /* u16 */
#define HDA_CORBRP 0x4A     /* u16 */
#define HDA_CORBCTL 0x4C    /* u8 */
#define HDA_CORBSTS 0x4D    /* u8 */
#define HDA_CORBLUB 0x4E    /* u8 */
#define HDA_RIRBWP 0x58     /* u16 */
#define HDA_RIRBCTL 0x5C    /* u8 */
#define HDA_RIRBSTS 0x5D    /* u8 */
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_CORBSIZE  0x4E     /* u16: 2, 16 or 256 entries */
#define HDA_RIRBSIZE  0x5E     /* u16: 2, 16 or 256 entries */
#define HDA_DPIBLB 0x70

/* one DMA stream: base + n*0x20 */
#define SD_CTL    0x00
#define SD_STS    0x03      /* u8 */
#define SD_LPIB   0x05      /* u24 via 0x05..0x07 */
#define SD_CBL    0x08
#define SD_LVI    0x0C      /* u16 */
#define SD_FIFOW  0x0E      /* u16 */
#define SD_BDLPL  0x18
#define SD_BDLPU  0x1C

/* SD_CTL bits */
#define SD_CTL_DMA_EN   (1u << 1)
#define SD_CTL_IOCE     (1u << 2)
#define SD_CTL_FEIE     (1u << 3)
#define SD_CTL_DEIE     (1u << 4)
#define SD_CTL_STRM(x)  (((u32)(x) & 0xF) << 20)
#define SD_CTL_STRM_MASK SD_CTL_STRM(0xF)

/* SD_STS bits (status byte) */
#define SD_STS_BCIS (1u << 2)   /* buffer completion */
#define SD_STS_DESE (1u << 3)

/* GCTL bits */
#define GCTL_CRST   (1u << 0)
#define GCTL_FCNTRL (1u << 1)
#define GCTL_UNSOL  (1u << 8)

/* ICS bits */
#define ICS_IRBUSY (1u << 0)
#define ICS_IRR    (1u << 1)   /* not needed: read-on-ready */
#define ICS_ICI    (1u << 1)

/* ---- verb construction ------------------------------------------------ */
#define VERB(codecid, nid, verb, payload) \
    ((((u32)(codecid)) << 28) | (((u32)(nid)) << 20) | (((u32)(verb)) << 8) | (u32)(payload))

#define VERB_GET_CONN_LIST   0xF02
#define VERB_GET_PIN_CAPS    0xF0C
#define VERB_GET_AMP_CAPS    0xF12
#define VERB_GET_CFG_DEFAULT 0xF1C
#define VERB_SET_AMP         0x300
#define VERB_SET_STREAM_CH   0x200
#define VERB_GET_SUBNODE     0xF04
#define VERB_GET_FUNCTION    0xF05   /* type 5: get converter/pin state */
#define VERB_SET_CONVERTER   0x200   /* same as stream format; see below */
#define HDA_CMD_SET_FMT      0x2     /* SET_CONVERTER_FORMAT payload id */

/* response flag: bit 31 set = invalid */
#define RESP_VALID(r) (!((r) & 0x80000000u))

/* widget types (from GET_PARAMS) */
#define WTYPE_AUDIO_OUT 0x0
#define WTYPE_PIN       0x4

/* BDL: two 128-byte aligned entries; each covers up to 4 buffers of 128 BDDs */
typedef struct {
    u32 addr_lo;
    u32 addr_hi;
    u32 length;      /* bytes in this buffer */
    u32 flags;       /* IOC in bit 0 */
} hda_bdl_entry_t;

#define BDL_ENTRIES 4
#define STREAM_TAG  1

#define PCM_BUF_BYTES (48 * 1024)      /* per BDL entry: 0.5 s at 48k/16/stereo */

static volatile u8  *mmio;
static int           out_sd;             /* output stream descriptor index */
static const pci_dev_t *hda_dev;
static hda_info_t    info;
static volatile int  bd_done;
static volatile u32 *dpib;

/* Full compiler barrier: every store before it must be globally visible
 * before any store after it (used around DMA descriptor programming). */
#define dma_barrier() asm volatile("" ::: "memory")
static int hda_dac_nid = 2;
static int hda_pin_nid = 3;

static u8  *pcm_pool;                    /* BDL_ENTRIES * PCM_BUF_BYTES */
static hda_bdl_entry_t *bdl;             /* 128-byte aligned */

/* ------------------------------------------------------------------ */
static inline u32 rdi(int off)
{
    return *(volatile u32 *)(mmio + off);
}

static inline void wri(int off, u32 v)
{
    *(volatile u32 *)(mmio + off) = v;
}

static inline u16 rdi16(int off)
{
    return *(volatile u16 *)(mmio + off);
}

static inline void wri16(int off, u16 v)
{
    *(volatile u16 *)(mmio + off) = v;
}

static inline u32 sd_reg(int sd, int off)
{
    return 0x80 + (u32)sd * 0x20 + (u32)off;
}

/* ------------------------------------------------------------------ */
/* Immediate command interface.  QEMU's intel-hda wires it up: setting
 * ICS.IRS_BUSY makes the controller deliver ICW to the codec; the codec's
 * answer lands in IRR and ICS turns into IRS_VALID | (cad << 4).
 * (The CORB/RIRB rings also work, but this is far less setup.) */
#define ICH6_IRS_BUSY  0x1
#define ICH6_IRS_VALID 0x2

static int hda_cmd(u32 verb, u32 *response)
{
    int tries;

    /* wait for any previous transaction to finish */
    for (tries = 0; tries < 200000; tries++) {
        if (!(rdi16(HDA_ICS) & ICH6_IRS_BUSY))
            break;
    }
    if (tries >= 200000)
        return -1;

    wri(HDA_ICW, verb);
    wri16(HDA_ICS, ICH6_IRS_BUSY);          /* start the transfer */

    for (tries = 0; tries < 200000; tries++) {
        u16 ics = rdi16(HDA_ICS);
        if (!(ics & ICH6_IRS_BUSY)) {
            u32 r = rdi(HDA_IRR);
            if (response)
                *response = r;
            return (ics & ICH6_IRS_VALID) ? 0 : -2;
        }
    }
    return -3;
}

/* GET_PARAMS: verb 0xF00, parameter id in the 8-bit payload.  Note the
 * encoding split QEMU applies: 0xF00|param uses the 12/8 form. */
static u32 get_param(int nid, int param)
{
    u32 resp = 0;

    hda_cmd(VERB(info.codec_addr, nid, 0xF00, param), &resp);
    return resp;
}

#define AC_AMP_MUTE        (1u << 7)
#define AC_AMP_SET_INDEX_S 8
#define AC_AMP_SET_RIGHT   (1u << 12)
#define AC_AMP_SET_LEFT    (1u << 13)
#define AC_AMP_SET_OUTPUT  (1u << 15)

/* Set the output amplifier on both channels; unmuted unless gain=0+mute. */
static int set_amp_gain(int nid, int gain)
{
    u32 base = AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT |
               ((0u & 0xF) << AC_AMP_SET_INDEX_S) | ((u32)gain & 0x7F);
    int r = hda_cmd(VERB(info.codec_addr, nid, 0x300, base), NULL);
    return r;
}

static int set_converter_format(int nid, u32 fmt)
{
    return hda_cmd(VERB(info.codec_addr, nid, 0x2, fmt), NULL);
}

static int set_stream_channel(int nid, int stream, int channel)
{
    u32 payload = ((u32)(stream & 0xF) << 4) | (u32)(channel & 0xF);
    return hda_cmd(VERB(info.codec_addr, nid, 0x706, payload), NULL);
}

/* ------------------------------------------------------------------ */
static void codec_name(u32 vid_dev)
{
    u32 vend = vid_dev >> 16;
    const char *n;

    switch (vend) {
    case 0x10EC: n = "Realtek"; break;
    case 0x8384: n = "SigmaTel"; break;
    case 0x11D4: n = "Analog Devices"; break;
    case 0x111D: n = "IDT"; break;
    case 0x1013: n = "Cirrus"; break;
    case 0x1002: n = "ATI"; break;
    case 0x8086: n = "Intel"; break;
    default:     n = "Codec"; break;
    }
    snprintf(info.name, sizeof(info.name), "%s %04x", n, vid_dev & 0xFFFF);
}

/* Walk the codec function group and find an output-capable pin widget plus
 * the DAC that feeds it.  QEMU's codec has DAC nid=2 and pin nid=3. */
static int find_output_path(int *dac_nid, int *pin_nid)
{
    u32 r;
    int fg;

    /* root node 0 -> first function group (AC_PAR_NODE_COUNT = verb 0xF04) */
    fg = 1;
    {
        u32 start, count;
        r = get_param(0, 0x04);        /* AC_PAR_NODE_COUNT of the root */
        start = (r >> 16) & 0xFF;
        count = r & 0xFF;
        if (count > 0)
            fg = (int)start;            /* the audio function group */
    }

    /* children of the function group (AC_PAR_NODE_COUNT) */
    r = get_param(fg, 0x04);
    {
        u32 start = (r >> 16) & 0xFF;
        u32 count = r & 0xFF;
        int dac = -1, pin = -1;

        for (u32 i = 0; i < count && start + i <= 0x7F; i++) {
            int nid = (int)(start + i);
            u32 t = (get_param(nid, 0x09) >> 20) & 0xF;

            if (t == WTYPE_AUDIO_OUT && dac < 0)
                dac = nid;
            if (t == WTYPE_PIN) {
                u32 pcaps = get_param(nid, 0x0C);
                if ((pcaps & 0x10) && pin < 0)    /* AC_PINCAP_OUT */
                    pin = nid;
            }
        }
        if (dac < 0 || pin < 0)
            return -1;
        *dac_nid = dac;
        *pin_nid = pin;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
static void hda_irq(regs_t *r)
{
    (void)r;
    if (mmio) {
        u8 st = *(volatile u8 *)(mmio + sd_reg(out_sd, SD_STS));
        if (st & SD_STS_BCIS) {
            *(volatile u8 *)(mmio + sd_reg(out_sd, SD_STS)) = SD_STS_BCIS;
            bd_done = 1;
        }
    }
}

int hda_init(void)
{
    const pci_dev_t *d = pci_find_class(PCI_CLASS_MMEDIA, PCI_SUBCL_AUDIO);
    u32 maj;
    int dac, pin;
    u32 amp_caps;

    if (!d)
        return -1;
    if ((d->bar[0] & 1) || d->bar[0] == 0)
        return -1;
    hda_dev = d;
    if (paging_map_device(d->bar[0] & ~0xFu, 128 * 1024) != 0) {
        kprintf("[hda] cannot map BAR0 0x%x\n", d->bar[0] & ~0xFu);
        return -1;
    }
    mmio = (volatile u8 *)(unsigned long)(d->bar[0] & ~0xFu);

    memset(&info, 0, sizeof(info));

    /* GCAP bits 3:0 = major, 7:4 = minor version (there is no separate
     * 16-bit VMAJ register; that was a misreading of the spec). */
    maj = rdi(HDA_GCAP) & 0xF;
    kprintf("[hda] BAR0 0x%x gcaps 0x%x (spec %d.%d)\n",
            d->bar[0], rdi(HDA_GCAP), (int)maj, (int)((rdi(HDA_GCAP) >> 4) & 0xF));

    /* controller reset: clear CRST, wait, set again */
    wri(HDA_GCTL, 0);
    for (int i = 0; i < 100000 && (rdi(HDA_GCTL) & GCTL_CRST); i++)
        ;
    wri(HDA_GCTL, GCTL_CRST);
    for (int i = 0; i < 100000 && !(rdi(HDA_GCTL) & GCTL_CRST); i++)
        ;
    for (volatile int i = 0; i < 20000; i++)
        ;

    /* Intel: wait 25 frames of the 24 MHz codec clock (~1 us) after reset
     * before reading STATESTS.  Poll generously - QEMU settles quickly. */
    for (volatile int i = 0; i < 200000; i++)
        ;
    {
        u16 st = rdi16(HDA_STATESTS);
        kprintf("[hda] STATESTS 0x%x\n", st);
        int found = -1;
        for (int a = 0; a < 15; a++)
            if (st & (1u << a)) {
                found = a;
                break;
            }
        if (found < 0) {
            kprintf("[hda] no codecs responded (STATESTS 0x%x)\n", st);
            return -1;
        }
        info.codec_addr = found;
    }

    {
        u32 resp = 0;
        int r = hda_cmd(VERB(info.codec_addr, 0, 0xF00, 0), &resp);
        if (r != 0)
            return -1;
        codec_name(resp);
    }

    if (find_output_path(&dac, &pin) != 0) {
        kprintf("[hda] %s: no output path found\n", info.name);
        return -1;
    }

    /* QEMU maps descriptors 0..3 to input, 4..7 to output.  Output
     * stream #1 = descriptor 4 with stream tag 1 in SD_CTL bits 23:20. */
    out_sd = 4;

    amp_caps = get_param(dac, 0x12);        /* AC_PAR_AMP_OUT_CAP */
    info.gain_steps = (amp_caps >> 8) & 0x7F;   /* NUM_STEPS */

    /* DAC amp: max gain (HDA gain 0 = softest!).  Pin amp: unmuted. */
    set_amp_gain(dac, (int)(info.gain_steps ? info.gain_steps : 0x4A));
    set_amp_gain(pin, 0);

    hda_dac_nid = dac;
    hda_pin_nid = pin;

    /* route the pin to the DAC */
    hda_cmd(VERB(info.codec_addr, pin, 0x701, (u32)dac), NULL);   /* SET_CONN_SELECT */
    set_converter_format(dac, HDA_FMT_48K_16B_2CH);
    set_stream_channel(dac, STREAM_TAG, 0);

    if (d->irq_line < 16 && d->irq_pin)
        irq_register(d->irq_line, hda_irq);

    info.present = 1;
    kprintf("[hda] %s codec %d, DAC nid %d -> pin nid %d (%d gain steps), mmio 0x%x irq %d\n",
            info.name, info.codec_addr, dac, pin, info.gain_steps,
            (u32)(unsigned long)mmio, d->irq_line);
    return 0;
}

int hda_present(void)
{
    return info.present;
}

const hda_info_t *hda_info(void)
{
    return &info;
}

/* ------------------------------------------------------------------ */
/* Walk the current page tables: virtual -> physical (0 if unmapped). */
static int bdl_prepare(const u8 *pcm, u32 bytes)
{
    u32 per = PCM_BUF_BYTES;
    u32 done = 0;

    if (!pcm_pool) {
        pcm_pool = (u8 *)kmalloc_a(BDL_ENTRIES * PCM_BUF_BYTES, 128);
        bdl = (hda_bdl_entry_t *)kmalloc_a(BDL_ENTRIES * sizeof(hda_bdl_entry_t), 128);
        if (!pcm_pool || !bdl)
            return -1;
        memset(bdl, 0, BDL_ENTRIES * sizeof(*bdl));
        for (int i = 0; i < BDL_ENTRIES; i++) {
            bdl[i].addr_lo = (u32)(unsigned long)(pcm_pool + i * PCM_BUF_BYTES);
            bdl[i].addr_hi = 0;
        }
    }

    for (int i = 0; i < BDL_ENTRIES; i++) {
        u32 n = bytes - done;
        if (n > per)
            n = per;
        if (n == 0)
            n = per;                      /* pad silent buffers: keep DMA legal */
        bdl[i].length = n;
        bdl[i].flags = (i == BDL_ENTRIES - 1) ? 1u : 0u;   /* IOC on last */
        if (bytes - done) {
            u32 c = bytes - done;
            if (c > per)
                c = per;
            memcpy(pcm_pool + i * PCM_BUF_BYTES, pcm + done, c);
            if (c < per)
                memset(pcm_pool + i * PCM_BUF_BYTES + c, 0, per - c);
            done += c;
        } else {
            memset(pcm_pool + i * PCM_BUF_BYTES, 0, per);
        }
    }
    dma_barrier();          /* samples + descriptors visible before DMA */
    return 0;
}

/* Begin playback and return immediately; hda_playing() reports progress.
 * Split out from hda_play() so ring 3 callers can keep their window alive
 * instead of freezing for the whole clip. */
static u32 play_start_ms;
static u32 play_expect_ms;

int hda_play_start(const void *pcm, u32 bytes, int volume)
{
    u32 total = bytes;

    if (!info.present)
        return -1;

    /* apply volume on the DAC's output amp.  HDA gain 0 = softest, max
     * steps = loudest; the MUTE bit is separate (bit 7 of the payload). */
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    {
        u32 steps = info.gain_steps ? info.gain_steps : 0x4A;
        u32 g = ((u32)volume * steps) / 100u;
        set_amp_gain(hda_dac_nid, (int)g);
    }

    if (bdl_prepare((const u8 *)pcm, total) != 0)
        return -2;

    /* DMA position buffer so we can see how much the controller ate */
    if (!dpib) {
        dpib = (volatile u32 *)kmalloc_a(8 * 8, 8);
        memset((void *)dpib, 0, 8 * 8);
        wri(0x70, ((u32)(unsigned long)dpib) | 1u);   /* DPLBASE */
    }

    bd_done = 0;

    /* stop, reprogram, start */
    *(volatile u8 *)(mmio + sd_reg(out_sd, SD_CTL)) = 0;
    wri(sd_reg(out_sd, SD_CBL), PCM_BUF_BYTES * BDL_ENTRIES);
    wri16(sd_reg(out_sd, SD_LVI), BDL_ENTRIES - 1);
    wri(sd_reg(out_sd, SD_BDLPL), (u32)(unsigned long)bdl);
    wri(sd_reg(out_sd, SD_BDLPU), 0);
    wri16(sd_reg(out_sd, 0x12), HDA_FMT_48K_16B_2CH);   /* SD_FORMAT */
    *(volatile u8 *)(mmio + sd_reg(out_sd, SD_STS)) = 0xFF;   /* clear status */

    play_start_ms = timer_uptime_ms();
    play_expect_ms = 1000uL * total / (48000 * 4) + 400;

    *(volatile u32 *)(mmio + sd_reg(out_sd, SD_CTL)) =
        SD_CTL_DMA_EN | SD_CTL_IOCE | SD_CTL_STRM(STREAM_TAG);
    return 0;
}

int hda_playing(void)
{
    if (!info.present)
        return 0;
    if (bd_done)
        return 0;
    if (timer_uptime_ms() - play_start_ms > play_expect_ms)
        return 0;
    return 1;
}

void hda_stop(void)
{
    if (!info.present)
        return;
    *(volatile u8 *)(mmio + sd_reg(out_sd, SD_CTL)) = 0;
}

int hda_play(const void *pcm, u32 bytes, int volume)
{
    if (hda_play_start(pcm, bytes, volume) != 0)
        return -1;
    while (hda_playing())
        for (volatile int i = 0; i < 2000; i++)
            ;
    hda_stop();
    return 0;
}

int hda_beep(int freq, int ms, int volume)
{
    /* 16 bit stereo 48 kHz: 2 ch * 2 bytes = 4 bytes per sample frame */
    u32 frames = (u32)(48000 * ms / 1000);
    static u8 *buf;
    u32 nbytes;

    if (frames < 8)
        frames = 8;
    if (frames > 48000 * 4)
        frames = 48000 * 4;               /* 4 s cap */
    nbytes = frames * 4;

    if (!buf || nbytes > PCM_BUF_BYTES * BDL_ENTRIES) {
        buf = (u8 *)kmalloc_a(PCM_BUF_BYTES * BDL_ENTRIES, 16);
        if (!buf)
            return -1;
    }
    for (u32 i = 0; i < frames; i++) {
        /* sin(x) with the quadratic Padé approximation on a 32k table-free
         * fixed point angle: cheap, no libm, good enough for a beep. */
        u32 frac = (u32)(((u64)(i % frames) * (u64)freq * 65536u / 48000u) % 65536u);
        u32 x = (frac < 32768u) ? frac : (65536u - frac);       /* half wave */
        int s = (int)(((u64)x * (u64)(27300000u - ((u64)x * x >> 4) * 643u)) >> 27);
        s16 v = (s16)(s * 3);

        buf[i * 4] = (u8)((u16)v & 0xFF);
        buf[i * 4 + 1] = (u8)((u16)v >> 8);
        buf[i * 4 + 2] = (u8)((u16)v & 0xFF);
        buf[i * 4 + 3] = (u8)((u16)v >> 8);
    }
    return hda_play(buf, nbytes, volume);
}

/* Fill `buf` with `bytes` of 16 bit stereo 48 kHz samples of a sine wave.
 * Exposed to ring 3 so an app can synthesise audio without a sound file. */
u32 hda_tone(void *buf, u32 bytes, int freq, int volume)
{
    s16 *out = (s16 *)buf;
    u32 frames = bytes / 4;
    int amp;

    if (!frames)
        return 0;
    if (freq < 20)
        freq = 20;
    if (freq > 20000)
        freq = 20000;
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    amp = (volume * 22000) / 100;

    for (u32 i = 0; i < frames; i++) {
        u32 frac = (u32)(((u64)i * (u64)freq * 65536u / 48000u) % 65536u);
        u32 x = (frac < 32768u) ? frac : (65536u - frac);
        int s = (int)(((u64)x * (u64)(27300000u - ((u64)x * x >> 4) * 643u)) >> 27);
        s16 v = (s16)((s * amp) / 32768);

        out[i * 2] = v;
        out[i * 2 + 1] = v;
    }
    return frames * 4;
}
