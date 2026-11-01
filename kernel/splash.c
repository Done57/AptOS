#include "splash.h"
#include "gfx.h"
#include "timer.h"
#include "util.h"


/* ------------------------------------------------------------------ */
/* palette                                                             */
#define C_BG_TOP    RGB(8, 12, 24)
#define C_BG_BOT    RGB(16, 24, 44)
#define C_RING      RGB(38, 58, 92)
#define C_RING_HI   RGB(70, 110, 170)
#define C_DOT       RGB(90, 190, 255)
#define C_DOT_HI    RGB(200, 235, 255)
#define C_TITLE     RGB(235, 242, 252)
#define C_SUB       RGB(120, 140, 168)
#define C_BAR_BG    RGB(28, 38, 58)
#define C_BAR_FG    RGB(80, 180, 255)
#define C_BAR_TIP   RGB(200, 240, 255)
#define C_LABEL     RGB(150, 172, 200)
#define C_STEP      RGB(96, 118, 148)

static int   sw, sh;
static int   cx, cy;              /* logo centre */
static u32   frame;               /* animation frame counter */
static int   clock_ready;         /* timer_ticks() is meaningful */
static int   cur_step, tot_step;
static char  cur_label[48];
static char  cur_label_zh[48];

/* A 5x7 dot-matrix "aptos" wordmark, drawn as blocks so it scales cleanly
 * and needs no font glyphs beyond what gfx already has. */
static const u8 logo[7][22] = {
    { 0,1,1,1,0,  0,1,1,1,0,  0,1,1,1,0,  0,1,1,1,0,  0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 },
    { 1,0,0,0,1,  1,0,0,0,1,  0,0,1,0,0,  1,0,0,0,1,  1,0,0,0,0, 0,1,1,1,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 },
    { 1,0,0,0,1,  1,0,0,0,1,  0,0,1,0,0,  1,0,0,0,1,  0,1,1,1,0, 1,0,0,0,1, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 },
    { 1,1,1,1,1,  1,1,1,1,1,  0,0,1,0,0,  1,0,0,0,1,  0,0,0,1,1, 1,0,0,0,1, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 },
    { 1,0,0,0,1,  1,0,0,0,0,  0,0,1,0,0,  1,0,0,0,1,  0,0,0,0,1, 1,0,0,0,1, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 },
    { 1,0,0,0,1,  1,0,0,0,0,  0,0,1,0,0,  1,0,0,0,1,  1,1,1,1,0, 0,1,1,1,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 },
    { 1,0,0,0,1,  1,0,0,0,0,  0,0,1,0,0,  1,1,1,1,0,  0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 },
};

/* Integer sine, 0..256 for a full turn.  Keeping it integer avoids pulling
 * floating point into the early boot path. */
static int isin(int deg)
{
    static const short tab[91] = {
          0,   4,   9,  13,  18,  22,  27,  31,  36,  40,
         44,  49,  53,  57,  61,  66,  70,  74,  78,  82,
         86,  90,  94,  98, 102, 106, 109, 113, 117, 120,
        124, 127, 131, 134, 137, 140, 143, 146, 149, 152,
        155, 158, 160, 163, 165, 167, 170, 172, 174, 176,
        178, 180, 182, 184, 185, 187, 188, 190, 191, 192,
        193, 194, 195, 196, 197, 198, 199, 199, 200, 201,
        201, 202, 202, 203, 203, 203, 204, 204, 204, 204,
        204, 204, 204, 204, 204, 203, 203, 203, 202, 202,
        201
    };

    deg %= 360;
    if (deg < 0)
        deg += 360;
    if (deg <= 90)
        return tab[deg];
    if (deg <= 180)
        return tab[180 - deg];
    if (deg <= 270)
        return -tab[deg - 180];
    return -tab[360 - deg];
}

static int icos(int deg)
{
    return isin(deg + 90);
}

/* A full-screen gradient plus grid.  This is the expensive part of a frame,
 * which is why the animation sleeps between frames rather than spinning: the
 * frame rate, not the per-frame cost, is what keeps the boot time sane. */
static void draw_background(void)
{
    gfx_reset_clip();
    gfx_gradient(0, 0, sw, sh, C_BG_TOP, C_BG_BOT);

    for (int x = 0; x < sw; x += 48)
        gfx_vline(x, 0, sh, RGB(18, 26, 44));
    for (int y = 0; y < sh; y += 48)
        gfx_hline(0, y, sw, RGB(18, 26, 44));
}

static void draw_emblem(void)
{
    int breathe = 12 + (isin((int)(frame * 3) % 360) + 204) / 40;
    int r_out = 58;
    int r_mid = 44;
    int r_in = 30;

    /* soft glow behind the mark */
    for (int i = 6; i >= 1; i--)
        gfx_fill_circle(cx, cy, r_out + breathe / 2 + i * 3,
                        gfx_blend(C_RING_HI, C_BG_TOP, 12 + i * 4));

    /* outer ring: dashed, slow */
    for (int a = 0; a < 360; a += 6) {
        int on = ((a / 6) + (int)(frame / 3)) % 3;
        if (!on)
            continue;
        int x = cx + (r_out * icos(a)) / 204;
        int y = cy + (r_out * isin(a)) / 204;
        gfx_fill_rect(x - 1, y - 1, 3, 3, C_RING_HI);
    }

    /* middle ring: solid */
    for (int a = 0; a < 360; a += 2) {
        int x = cx + (r_mid * icos(a)) / 204;
        int y = cy + (r_mid * isin(a)) / 204;
        gfx_pixel(x, y, C_RING);
    }

    /* inner ring: dashed, fast, opposite direction */
    for (int a = 0; a < 360; a += 6) {
        int on = ((a / 6) - (int)(frame / 2)) % 4;
        if (on && on != 2)
            continue;
        int x = cx + (r_in * icos(a)) / 204;
        int y = cy + (r_in * isin(a)) / 204;
        gfx_fill_rect(x - 1, y - 1, 3, 3, C_DOT);
    }

    /* orbiting dot with a trail */
    {
        int deg = (int)(frame * 6) % 360;
        for (int t = 0; t < 10; t++) {
            int a = deg - t * 7;
            int x = cx + (r_mid * icos(a)) / 204;
            int y = cy + (r_mid * isin(a)) / 204;
            u32 c = (t == 0) ? C_DOT_HI : gfx_blend(C_DOT, C_RING, 200 - t * 20);
            int s = (t == 0) ? 3 : 2;
            gfx_fill_rect(x - s / 2, y - s / 2, s, s, c);
        }
    }
}

/* Draw the wordmark scaled inside the ring. */
static void draw_wordmark(void)
{
    const int scale = 3;
    const int w = 22 * scale;
    const int h = 7 * scale;
    int ox = cx - w / 2;
    int oy = cy - h / 2;
    /* subtle vertical bob so the logo feels alive */
    oy += isin((int)(frame * 2) % 360) / 60;

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 22; col++) {
            if (!logo[row][col])
                continue;
            /* letter "o" and the dot of nothing: every filled cell is white,
             * with a soft pulse on the diagonal for a little shimmer */
            int pulse = ((row + col + (int)(frame / 2)) % 12 == 0);
            gfx_fill_rect(ox + col * scale, oy + row * scale, scale, scale,
                          pulse ? C_DOT_HI : C_TITLE);
        }
    }
}

static void draw_progress(void)
{
    int bar_w = sw / 3;
    int bar_h = 6;
    int bx = (sw - bar_w) / 2;
    int by = cy + 92;
    int pct;

    if (tot_step <= 0)
        pct = 0;
    else
        pct = (cur_step * 100) / tot_step;
    if (pct > 100)
        pct = 100;

    gfx_text((sw - gfx_text_w("aptos 2.0", 2)) / 2, cy - 132, "aptos 2.0",
             C_TITLE, 2);
    {
        const char *sub = "32-bit x86  -  booting";
        gfx_text((sw - gfx_text_w(sub, 1)) / 2, cy - 106, sub, C_SUB, 1);
    }

    /* track */
    gfx_fill_rect(bx, by, bar_w, bar_h, C_BAR_BG);
    gfx_rect(bx - 1, by - 1, bar_w + 2, bar_h + 2, C_RING);

    /* fill with a moving highlight so even a stalled bar looks alive */
    if (pct > 0) {
        int fw = (bar_w * pct) / 100;
        if (fw < 2)
            fw = 2;
        gfx_fill_rect(bx, by, fw, bar_h, C_BAR_FG);
        if (fw > 3)
            gfx_fill_rect(bx + fw - 3, by, 3, bar_h, C_BAR_TIP);
    }

    /* sweeping sheen across the track */
    {
        int sweep = (int)(frame * 7) % (bar_w + 60) - 30;
        for (int i = 0; i < 30; i++) {
            int x = bx + sweep - i;
            if (x < bx || x >= bx + bar_w)
                continue;
            int a = 22 - (i * 22) / 30;
            if (a < 0)
                a = 0;
            if (x < bx + (pct > 0 ? (bar_w * pct) / 100 : 0))
                gfx_pixel(x, by + 1, gfx_blend(0xFFFFFF, C_BAR_FG, a));
        }
    }

    /* percentage, right aligned under the bar */
    {
        char buf[8];
        int i = 0;
        if (pct >= 100) { buf[i++] = '1'; buf[i++] = '0'; buf[i++] = '0'; }
        else if (pct >= 10) { buf[i++] = (char)('0' + pct / 10); buf[i++] = (char)('0' + pct % 10); }
        else { buf[i++] = (char)('0' + pct); }
        buf[i++] = '%';
        buf[i] = 0;
        gfx_text(bx + bar_w + 10, by - 1, buf, C_BAR_TIP, 1);
    }

    /* current step label, centred beneath.  Both languages are shown so the
     * caption works before i18n_init() has run. */
    if (cur_label[0]) {
        int tw = gfx_text_w(cur_label, 1);
        gfx_text((sw - tw) / 2, by + 20, cur_label, C_LABEL, 1);
    }
    if (cur_label_zh[0]) {
        int tw = gfx_text_w(cur_label_zh, 1);
        gfx_text((sw - tw) / 2, by + 34, cur_label_zh, C_STEP, 1);
    }

    /* a row of dots that light up as steps complete */
    if (tot_step > 0 && tot_step <= 24) {
        int gap = 14;
        int total_w = (tot_step - 1) * gap;
        int dx = (sw - total_w) / 2;
        int dy = by + 40;
        for (int i = 0; i < tot_step; i++) {
            u32 c;
            if (i < cur_step)
                c = C_DOT;
            else if (i == cur_step)
                c = (frame & 4) ? C_DOT_HI : C_RING_HI;
            else
                c = C_RING;
            gfx_fill_rect(dx + i * gap, dy, 4, 4, c);
        }
    }
}

static void draw_frame(void)
{
    draw_background();
    draw_emblem();
    draw_wordmark();
    draw_progress();
    gfx_present();
    frame++;
}

/* ---- 文本模式实现：不再绘制任何图形，仅把启动进度打到串口/VGA 文本 ---- */
#include "klog.h"

static void splash_text(const char *tag, int n, int total, const char *a, const char *b)
{
    if (n > 0 && total > 0)
        kprintf("[boot] (%d/%d) %s%s%s\n", n, total, a ? a : "",
                (b && b[0]) ? " / " : "", (b && b[0]) ? b : "");
    else if (a)
        kprintf("[boot] %s%s%s\n", a, (b && b[0]) ? " / " : "", (b && b[0]) ? b : "");
}

void splash_begin(void) { }
void splash_animate(void) { }
void splash_step(int n, int total, const char *label)
{
    splash_text("step", n, total, label, 0);
}
void splash_step2(int n, int total, const char *label, const char *label_zh)
{
    splash_text("step", n, total, label, label_zh);
}
void splash_finish(const char *label)
{
    splash_text("finish", 0, 0, label, 0);
}
