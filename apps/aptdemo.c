/* aptdemo - a walk through the apt graphics API
 *
 * This doubles as the API's regression test: every primitive, the transform
 * stack, clipping, textures and the quad blitter appear on screen.  Press a
 * key to move through the pages.
 */
#include "apt.h"
#include "aptos.h"

#define W 720
#define H 520

static apt_canvas *c;
static apt_texture *tex_logo;
static apt_texture *tex_check;

/* ------------------------------------------------------------------ */
/* test textures                                                       */

static void build_textures(void)
{
    int x, y;

    tex_logo = apt_texture_new(64, 64);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            int dx = x - 32, dy = y - 32;
            int d2 = dx * dx + dy * dy;
            u32 px;

            if (d2 > 32 * 32)
                px = APT_RGB(24, 28, 40);
            else if (d2 > 24 * 24)
                px = APT_RGB(255, 200, 60);
            else if (d2 > 14 * 14)
                px = APT_RGB(240, 120, 40);
            else
                px = APT_RGB(255, 245, 220);
            tex_logo->pixels[y * 64 + x] = px;
        }
    }

    /* A checkerboard with an alpha mask, to exercise masking + blending. */
    tex_check = apt_texture_new(48, 48);
    tex_check->mask = (u8 *)malloc(48 * 48);
    for (y = 0; y < 48; y++) {
        for (x = 0; x < 48; x++) {
            int on = ((x / 8) + (y / 8)) & 1;
            tex_check->pixels[y * 48 + x] = on ? APT_WHITE : APT_RGB(40, 40, 40);
            /* fade out towards the right edge */
            tex_check->mask[y * 48 + x] = (u8)(255 - (x * 255) / 48);
        }
    }
}

/* ------------------------------------------------------------------ */
/* pages                                                               */

static void page_primitives(void)
{
    apt_clear(c, APT_RGB(22, 26, 36));

    apt_text(c, 16, 12, "apt primitives", APT_WHITE, 2);
    apt_text(c, 16, 32, "line / rect / circle / ellipse / round rect / polygon", APT_GRAY, 1);

    apt_line(c, 16, 56, 220, 120, APT_RED);
    apt_line(c, 220, 56, 16, 120, APT_RGB(120, 40, 40));

    apt_fill_rect(c, 250, 56, 80, 64, APT_BLUE);
    apt_rect(c, 340, 56, 80, 64, APT_CYAN);

    apt_circle(c, 470, 88, 32, APT_YELLOW);
    apt_fill_circle(c, 570, 88, 32, APT_RGB(90, 60, 20));
    apt_fill_circle(c, 570, 88, 22, APT_ORANGE);

    apt_ellipse(c, 90, 200, 60, 34, APT_GREEN);
    apt_fill_ellipse(c, 230, 200, 60, 34, APT_RGB(24, 70, 40));

    apt_round_rect(c, 320, 166, 120, 68, 16, APT_MAGENTA);
    apt_fill_round_rect(c, 460, 166, 120, 68, 16, APT_RGB(70, 30, 70));

    {
        apt_point tri[3];
        tri[0].x = 620; tri[0].y = 166;
        tri[1].x = 700; tri[1].y = 200;
        tri[2].x = 640; tri[2].y = 234;
        apt_fill_poly(c, tri, 3, APT_CYAN);
    }
    {
        apt_point star[10];
        int i;
        for (i = 0; i < 10; i++) {
            /* an octagon-ish spiky polygon, integer only */
            int ang = i * 36;
            int r = (i & 1) ? 22 : 46;
            /* crude integer trig: 36-degree steps */
            static const int sx8[10] = { 100, 95, 59, 31, 0, -31, -59, -95, -100, -95 };
            static const int sy8[10] = { 0, 31, 59, 95, 100, 95, 59, 31, 0, -31 };
            (void)ang;
            star[i].x = 130 + (r * sx8[i]) / 100;
            star[i].y = 330 + (r * sy8[i]) / 100;
        }
        apt_fill_poly(c, star, 10, APT_YELLOW);
    }

    apt_gradient(c, 250, 290, 240, 90, APT_RGB(40, 80, 160), APT_RGB(160, 40, 90));

    apt_shadow(c, 530, 300, 150, 70, 10);
    apt_fill_round_rect(c, 530, 300, 150, 70, 12, APT_RGB(56, 62, 80));
    apt_text_aligned(c, 530, 322, 150, "shadowed panel", APT_WHITE, 1, APT_ALIGN_CENTER);
    apt_text_aligned(c, 530, 338, 150, "apt_shadow()", APT_GRAY, 1, APT_ALIGN_CENTER);

    apt_text(c, 16, 440, "press any key for textures", APT_GRAY, 1);
}

static void page_textures(void)
{
    int i;

    apt_clear(c, APT_RGB(22, 26, 36));
    apt_text(c, 16, 12, "apt textures", APT_WHITE, 2);
    apt_text(c, 16, 32, "blit / scaled blit / alpha mask / quad mapping", APT_GRAY, 1);

    apt_blit(c, tex_logo, 20, 60);

    for (i = 1; i <= 4; i++)
        apt_blit_scaled(c, tex_logo, 20 + i * 76, 60, 32 * i / 2, 32 * i / 2);

    for (i = 0; i < 4; i++)
        apt_blit(c, tex_check, 20 + i * 54, 200);

    /* Rotated-ish quads: the texture mapped onto four different shapes. */
    {
        apt_point q[4];

        q[0].x = 300; q[0].y = 200;
        q[1].x = 420; q[1].y = 180;
        q[2].x = 430; q[2].y = 300;
        q[3].x = 310; q[3].y = 320;
        apt_blit_quad(c, tex_logo, q);

        q[0].x = 460; q[0].y = 180;
        q[1].x = 600; q[1].y = 220;
        q[2].x = 570; q[2].y = 330;
        q[3].x = 450; q[3].y = 290;
        apt_blit_quad(c, tex_check, q);

        q[0].x = 300; q[0].y = 350;
        q[1].x = 600; q[1].y = 350;
        q[2].x = 600; q[2].y = 430;
        q[3].x = 300; q[3].y = 430;
        apt_blit_quad(c, tex_logo, q);
    }

    apt_text(c, 16, 440, "press any key for transforms", APT_GRAY, 1);
}

static void page_transforms(void)
{
    int i;

    apt_clear(c, APT_RGB(22, 26, 36));
    apt_text(c, 16, 12, "apt transforms", APT_WHITE, 2);
    apt_text(c, 16, 32, "scale / translate / flip / clip stack", APT_GRAY, 1);

    /* A row of squares drawn at increasing scale from a common origin. */
    apt_fill_rect(c, 40, 300, 40, 40, APT_BLUE);
    for (i = 2; i <= 4; i++) {
        apt_set_transform(c, APT_FX(i), APT_FX(i), APT_FX(40), APT_FX(300));
        apt_fill_rect(c, 0, 0, 40, 40, APT_RGB(40 * i, 60, 160 - 20 * i));
    }
    apt_reset_transform(c);

    /* Translation only. */
    for (i = 0; i < 8; i++) {
        apt_set_transform(c, APT_FX_ONE, APT_FX_ONE, APT_FX(60 + i * 34), APT_FX(60));
        apt_fill_round_rect(c, 0, 0, 28, 28, 6, APT_RGB(60 + i * 20, 180 - i * 12, 90));
    }
    apt_reset_transform(c);

    /* Mirrored drawing: the same call, flipped about the canvas centre. */
    apt_set_transform(c, APT_FX_ONE, APT_FX_ONE, APT_FX(440), APT_FX(120));
    apt_fill_poly(c, (apt_point[]){ { 0, 0 }, { 80, 20 }, { 60, 90 }, { 10, 70 } }, 4, APT_GREEN);

    apt_reset_transform(c);
    apt_set_flip(c, 1, 0);
    apt_set_transform(c, APT_FX_ONE, APT_FX_ONE, APT_FX(440), APT_FX(120));
    apt_fill_poly(c, (apt_point[]){ { 0, 0 }, { 80, 20 }, { 60, 90 }, { 10, 70 } }, 4,
                  APT_RGB(40, 90, 50));
    apt_reset_transform(c);
    apt_set_flip(c, 0, 0);

    /* The clip stack: draw a big shape, clipped to three windows. */
    {
        int k;
        for (k = 0; k < 3; k++) {
            int x = 80 + k * 190;
            apt_push_clip(c, x, 250, 150, 120);
            apt_gradient(c, x, 250, 150, 120, APT_RGB(30, 60, 120), APT_RGB(140, 40, 80));
            apt_fill_circle(c, x + 75, 310, 70, APT_RGB(255, 210, 90));
            apt_text(c, x + 8, 256, "clipped", APT_WHITE, 1);
            apt_pop_clip(c);
            apt_rect(c, x, 250, 150, 120, APT_RGB(90, 96, 110));
        }
    }

    apt_text(c, 16, 440, "press any key to go back", APT_GRAY, 1);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    apt_event ev;
    int page = 0;
    int running = 1;
    int ticks = 0;
    char arg[16];

    /* Optional starting page, so the test harness can jump straight to a
     * page instead of injecting keystrokes. */
    if (sys_args_len(1) > 0 && sys_args(1, arg, sizeof(arg)) == 0 &&
        arg[0] >= '0' && arg[0] <= '2')
        page = arg[0] - '0';

    c = apt_open("apt demo", W, H);
    if (!c) {
        printf("apt_open failed: out of memory?\n");
        return 1;
    }
    build_textures();

    /* Redraw only when something changed.  Repainting the whole page every
     * frame made the program spend all its time in the rasteriser on a slow
     * machine, which is exactly the workload the API is meant to avoid. */
    {
        int drawn = -1;

        while (running) {
            if (page != drawn) {
                switch (page) {
                case 0: page_primitives(); break;
                case 1: page_textures(); break;
                default: page_transforms(); break;
                }
                drawn = page;
                apt_flush(c);
            }

            /* A small live element so it is obvious the loop is alive.  Only
             * its own little square is redrawn. */
            ticks++;
            {
                int r = 8 + (ticks % 4);
                apt_fill_rect(c, W - 40, 8, 32, 32, APT_RGB(22, 26, 36));
                apt_fill_circle(c, W - 24, 24, r, APT_RGB(90, 200, 120));
                apt_flush(c);
            }

            if (apt_poll(c, &ev)) {
                if (ev.quit)
                    running = 0;
                else if (ev.key)
                    page = (page + 1) % 3;
            }
            if (ev.quit)
                running = 0;

            apt_sleep(60);
        }
    }

    apt_texture_free(tex_logo);
    apt_texture_free(tex_check);
    apt_close(c);
    return 0;
}
