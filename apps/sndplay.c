/* sndplay - audio player for aptos
 *
 * Two modes:
 *   run /APPS/SNDPLAY.APP <file.wav>   play a 16 bit PCM WAV file
 *   run /APPS/SNDPLAY.APP              built-in tone player (no file needed)
 *
 * Playback goes through the HDA driver via the SYS_AUDIO_PLAY syscall, which
 * starts the DMA and returns immediately so the window stays responsive.
 */
typedef unsigned long long u64;

#include "aptos.h"
#include "i18n.h"

#define WIN_W 520
#define WIN_H 300
/* Keep the ring 3 BSS small: the ELF loader allocates and zeroes the whole
 * BSS in one contiguous block, and memory is never reclaimed.  256 KiB is
 * a few seconds of 48 kHz 16 bit stereo. */
#define MAX_WAV (256 * 1024)

/* The eight tones of a major scale, as a fallback instrument. */
static const int scale[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
static const char *note_names[8] = {
    "C", "D", "E", "F", "G", "A", "B", "C'"
};

static u8 wav[MAX_WAV];
static u32 wav_len;                       /* PCM byte count */
static const u8 *wav_pcm;
static int wav_ok;
static char wav_path[160];

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 rd16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

/* Parse a canonical RIFF/WAVE file.  Only uncompressed PCM is accepted; the
 * HDA driver plays 16 bit stereo 48 kHz, so anything else is rejected with a
 * clear reason rather than played as noise. */
static int wav_parse(int len, int *rate, int *channels, int *bits)
{
    int pos;
    int fmt_seen = 0;

    if (len < 44)
        return -1;
    if (rd32(wav) != 0x46464952u)          /* "RIFF" */
        return -1;
    if (rd32(wav + 8) != 0x45564157u)      /* "WAVE" */
        return -1;

    *rate = 0; *channels = 0; *bits = 0;
    wav_pcm = NULL; wav_len = 0;

    pos = 12;
    while (pos + 8 <= len) {
        u32 id = rd32(wav + pos);
        u32 sz = rd32(wav + pos + 4);
        const u8 *body = wav + pos + 8;

        if (id == 0x20746D66u) {           /* "fmt " */
            if (sz < 16)
                return -2;
            if (rd16(body) != 1)           /* format 1 = PCM */
                return -3;
            *channels = (int)rd16(body + 2);
            *rate = (int)rd32(body + 4);
            *bits = (int)rd16(body + 14);
            fmt_seen = 1;
        } else if (id == 0x61746164u) {    /* "data" */
            u32 avail = (u32)(len - (pos + 8));
            if (sz > avail)
                sz = avail;                /* tolerate a wrong chunk size */
            wav_pcm = body;
            wav_len = sz;
        }

        pos += 8 + (int)sz + ((int)sz & 1);   /* chunks are word aligned */
        if (sz > (u32)(len - pos + 8))
            break;
    }

    if (!fmt_seen)
        return -4;
    if (!wav_pcm || !wav_len)
        return -5;
    return 0;
}

static const char *wav_err(int e)
{
    switch (e) {
    case -1: return T("not a RIFF/WAVE file", "不是 RIFF/WAVE 文件");
    case -2: return T("broken fmt chunk", "fmt 块损坏");
    case -3: return T("compressed WAV is not supported",
                      "不支持压缩的 WAV");
    case -4: return T("missing fmt chunk", "缺少 fmt 块");
    case -5: return T("missing audio data", "缺少音频数据");
    default: return T("cannot read this file", "无法读取该文件");
    }
}

static void draw(const char *title, const char *line1, const char *line2,
                 int color, int playing_note, int progress)
{
    sys_clear(0x101820);

    sys_text(12, 10, T("Audio Player", "音频播放器"), 0xFFFFFF, 2);
    sys_text(12, 44, title, 0x80D0FF, 1);
    if (line1) sys_text(12, 72, line1, color, 1);
    if (line2) sys_text(12, 92, line2, 0x9098A0, 1);

    /* Keyboard: tone buttons across the bottom */
    sys_text(12, 150, T("Tones - press 1..8 or click:",
                        "音阶 —— 按 1..8 或点击："), 0xA0B0C0, 1);
    {
        int x = 12, y = 176;
        for (int i = 0; i < 8; i++) {
            int on = (playing_note == i);
            sys_rect(x, y, 54, 44, on ? 0x3070C0 : 0x203040);
            sys_frame(x, y, 54, 44, on ? 0x80D0FF : 0x405060);
            sys_text(x + 8, y + 6, note_names[i], on ? 0xFFFFFF : 0xC0C8D0, 1);
            {
                char hz[16];
                snprintf(hz, sizeof(hz), "%d", scale[i]);
                sys_text(x + 6, y + 26, hz, 0x9098A0, 1);
            }
            x += 58;
        }
    }

    if (progress >= 0) {
        sys_text(12, 236, T("Playing...", "正在播放..."), 0x60FF90, 1);
        sys_rect(12, 256, 480, 10, 0x203040);
        sys_rect(12, 256, progress > 480 ? 480 : progress, 10, 0x60C0FF);
    } else {
        sys_text(12, 236, T("Q/ESC close   R replay",
                            "Q/ESC 关闭   R 重播"), 0x808890, 1);
    }
    sys_flush();
}

int main(void)
{
    char path[160];
    int have_wav = 0;
    int rate = 0, channels = 0, bits = 0;
    int last_note = -1;
    u32 start_ticks = 0;
    u32 clip_ms = 0;
    int prev_btn = 0;
    int len;

    if (sys_win_open("Audio Player", WIN_W, WIN_H) != 0) {
        printf("sndplay: cannot open a window\n");
        return 1;
    }

    if (sys_audio_info() == 0) {
        draw(T("no sound card found", "未找到声卡"),
             T("This machine has no Intel HDA codec.",
               "这台机器上没有 Intel HDA 编解码器。"),
             T("Playback is unavailable.", "无法播放。"),
             0xFF8080, -1, -1);
        while (sys_getkey() == 0)
            sys_sleep(30);
        return 1;
    }

    /* arg0 is the program path; arg1 is the .wav to play, if given. */
    if (sys_args(1, path, sizeof(path)) == 0 && path[0]) {
        int e;

        len = 0;
        {
            int fd = sys_open(path, 0);
            if (fd >= 0) {
                len = sys_read(fd, wav, sizeof(wav));
                sys_close(fd);
            } else {
                len = -1;
            }
        }
        if (len < 0) {
            draw(T("cannot open file", "无法打开文件"), path, NULL,
                 0xFF8080, -1, -1);
            while (sys_getkey() == 0)
                sys_sleep(30);
            return 1;
        }
        e = wav_parse(len, &rate, &channels, &bits);
        if (e != 0) {
            draw(T("unsupported audio file", "不支持的音频文件"), path,
                 wav_err(e), 0xFF8080, -1, -1);
            while (sys_getkey() == 0)
                sys_sleep(30);
            return 1;
        }
        /* The HDA path is fixed at 16 bit stereo 48 kHz. */
        if (bits != 16 || channels != 2 || rate != 48000) {
            char why[100];
            snprintf(why, sizeof(why),
                     T("needs 16 bit stereo 48 kHz (this is %d bit %d ch %d Hz)",
                       "需要 16 位立体声 48 kHz（当前 %d 位 %d 声道 %d Hz）"),
                     bits, channels, rate);
            draw(T("unsupported format", "格式不支持"), path, why,
                 0xFFB060, -1, -1);
            while (sys_getkey() == 0)
                sys_sleep(30);
            return 1;
        }
        have_wav = 1;
        wav_ok = 1;
        {
            int i = 0;
            while (path[i] && i < (int)sizeof(wav_path) - 1) {
                wav_path[i] = path[i];
                i++;
            }
            wav_path[i] = 0;
        }
        clip_ms = wav_len / 192u;   /* 48000 * 4 bytes per second */
    }

    if (!have_wav) {
        draw(T("built-in tone player", "内置音调播放器"),
             T("Press 1..8 to play a note.", "按 1..8 播放一个音。"),
             T("Or run with a .wav path to play a file.",
               "也可以带 .wav 路径运行来播放文件。"),
             0xC0C8D0, -1, -1);
    }

    for (;;) {
        int k = sys_getkey();
        int mx, my, btn, inside;

        if (k == 27 || k == 'q' || k == 'Q')
            break;

        if (have_wav && (k == 'r' || k == 'R')) {
            sys_audio_play(wav_pcm, (int)wav_len, 70);
            start_ticks = (u32)sys_ticks();
        }

        if (k >= '1' && k <= '8') {
            int n = k - '1';
            char buf[64];

            sys_audio_beep(scale[n], 400, 70);
            last_note = n;
            start_ticks = (u32)sys_ticks();
            clip_ms = 400;
            snprintf(buf, sizeof(buf),
                     T("playing note %s (%d Hz)", "正在播放 %s（%d Hz）"),
                     note_names[n], scale[n]);
            draw(have_wav ? wav_path : T("built-in tone player",
                                         "内置音调播放器"),
                 buf, NULL, 0x60FF90, last_note, 0);
        }

        /* Click a tone button */
        sys_mouse(&mx, &my, &btn, &inside);
        if ((btn & MOUSE_BTN_LEFT) && !(prev_btn & MOUSE_BTN_LEFT)) {
            int y = 176;
            if (my >= y && my <= y + 44) {
                int idx = (mx - 12) / 58;
                if (mx >= 12 && idx >= 0 && idx < 8 && (mx - 12) % 58 <= 54) {
                    char buf[64];

                    sys_audio_beep(scale[idx], 400, 70);
                    last_note = idx;
                    start_ticks = (u32)sys_ticks();
                    clip_ms = 400;
                    snprintf(buf, sizeof(buf),
                             T("playing note %s (%d Hz)",
                               "正在播放 %s（%d Hz）"),
                             note_names[idx], scale[idx]);
                    draw(have_wav ? wav_path
                                  : T("built-in tone player",
                                      "内置音调播放器"),
                         buf, NULL, 0x60FF90, last_note, 0);
                }
            }
        }
        prev_btn = btn;

        /* Progress: redraw only while something is playing */
        if (clip_ms) {
            u32 now = (u32)sys_ticks();
            u32 elapsed = now - start_ticks;    /* ticks are 100 Hz */
            u32 ms_done = elapsed * 10;

            if (ms_done >= clip_ms) {
                clip_ms = 0;
                last_note = -1;
                draw(have_wav ? wav_path
                              : T("built-in tone player", "内置音调播放器"),
                     T("ready", "就绪"),
                     have_wav ? T("Press R to replay, Q to close.",
                                  "按 R 重播，Q 关闭。")
                              : T("Press 1..8 to play a note.",
                                  "按 1..8 播放一个音。"),
                     0xC0C8D0, -1, -1);
            } else {
                int p;
                if (clip_ms >= 1000) p = (int)((ms_done / (clip_ms / 480u + 1u)));
                else p = (int)(ms_done * 480u / (clip_ms ? clip_ms : 1u));
                if (p > 480) p = 480;
                draw(have_wav ? wav_path
                              : T("built-in tone player", "内置音调播放器"),
                     T("playing...", "正在播放..."), NULL,
                     0x60FF90, last_note, p);
            }
        }

        /* Honour a finished file playback too */
        sys_sleep(30);
    }

    return 0;
}
