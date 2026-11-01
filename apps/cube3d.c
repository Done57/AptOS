/* cube3d - a rotating textured cube, built to exercise the apt graphics API.
 *
 * This is the API's stress test.  Every frame it does six textured quad
 * blits plus six shading overlays, all through the shared-canvas path, so
 * the frame rate it reports is a direct read on how well the
 * "draw without trapping" design actually holds up.
 *
 * Controls:
 *   space      pause / resume the rotation
 *   + / -      faster / slower
 *   w          toggle the wireframe overlay
 *   b          toggle drawing the back faces (darkened)
 *   r          reset the view angles
 *   esc        quit
 */
#include "apt.h"
#include "aptos.h"
#include "cube3d.h"

#define W 700
#define H 460

#define TEX_SIZE 128

static apt_canvas *c;
static cube_mesh   mesh;
static cube_view   view;

static int paused;
static int speed = 3;          /* angle steps per frame */
static int show_stats = 1;
static int last_faces;   /* how many faces the last frame rasterised */

/* ------------------------------------------------------------------ */
/* frame rate                                                          */

typedef struct {
    int frames;
    int last_ms;
    int fps;
    int worst_ms;
    int best_ms;
} fps_t;

static fps_t fps;

static void fps_tick(fps_t *f)
{
    int now = apt_ticks();

    f->frames++;
    if (f->last_ms == 0) {
        f->last_ms = now;
        return;
    }
    if (now - f->last_ms >= 1000) {
        int elapsed = now - f->last_ms;

        f->fps = (f->frames * 1000) / elapsed;
        f->frames = 0;
        f->last_ms = now;
    }
}

/* ------------------------------------------------------------------ */

static void draw_hud(void)
{
    char buf[96];
    int y = 10;

    apt_fill_round_rect(c, 8, 8, 250, show_stats ? 118 : 30, 8, APT_RGB(12, 14, 20));
    apt_rect(c, 8, 8, 250, show_stats ? 118 : 30, APT_RGB(60, 66, 84));

    apt_printf(c, 18, y, APT_RGB(150, 200, 255), 1, "apt 3D cube test");
    y += 18;

    if (!show_stats)
        return;

    apt_printf(c, 18, y, APT_WHITE, 1, "fps    %d", fps.fps);
    y += 14;
    apt_printf(c, 18, y, APT_GRAY, 1, "faces  %d", last_faces);
    y += 14;
    apt_printf(c, 18, y, APT_GRAY, 1, "mem    %d KiB free", apt_mem_free() / 1024);
    y += 14;
    apt_printf(c, 18, y, paused ? APT_YELLOW : APT_RGB(120, 220, 140), 1,
               "%s   speed %d", paused ? "PAUSED" : "running", speed);
    y += 14;
    apt_printf(c, 18, y, APT_GRAY, 1, "ax%3d ay%3d az%3d", view.ax, view.ay, view.az);

    /* The controls are the kind of thing that should be visible, not
     * something you have to read the source to discover. */
    apt_fill_round_rect(c, 8, H - 44, 420, 36, 8, APT_RGB(12, 14, 20));
    apt_rect(c, 8, H - 44, 420, 36, APT_RGB(60, 66, 84));
    apt_text(c, 18, H - 36, "space pause   +/- speed   w wire   b back   r reset",
             APT_GRAY, 1);
    apt_text(c, 18, H - 22, "esc quit", APT_GRAY, 1);
}

int main(void)
{
    apt_event ev;
    int running = 1;

    c = apt_open("apt 3D cube", W, H);
    if (!c) {
        printf("cube3d: could not open a canvas\n");
        return 1;
    }

    cube_init(&mesh, TEX_SIZE);

    view.c = c;
    view.cx = W / 2;
    view.cy = H / 2 + 20;
    view.scale = 210;
    view.dist = 4;
    view.ax = 20;
    view.ay = 30;
    view.az = 0;
    view.wireframe = 0;
    view.backface_dark = 0;
    view.debug_print = 0;
    view.light = vec3_norm(vec3_make(1, 2, -2));

    while (running) {
        apt_clear(c, APT_RGB(18, 20, 30));

        /* A subtle backdrop so the cube's edges have something to read
         * against, without competing with it. */
        apt_gradient(c, 0, 0, W, H / 2, APT_RGB(22, 26, 40), APT_RGB(18, 20, 30));

        last_faces = cube_draw(&view, &mesh);

        fps_tick(&fps);
        draw_hud();
        apt_flush(c);

        /* Input.  apt_poll reports a key only on the frame it is pressed,
         * so the rotation will keep going while nothing is held. */
        if (apt_poll(c, &ev)) {
            if (ev.quit)
                running = 0;

            switch (ev.key) {
            case 27:                       /* esc */
                running = 0;
                break;
            case ' ':
                paused = !paused;
                break;
            case '+':
            case '=':
                if (speed < 24)
                    speed++;
                break;
            case '-':
            case '_':
                if (speed > 0)
                    speed--;
                break;
            case 'w':
            case 'W':
                view.wireframe = !view.wireframe;
                break;
            case 'b':
            case 'B':
                view.backface_dark = !view.backface_dark;
                break;
            case 'r':
            case 'R':
                view.ax = 20;
                view.ay = 30;
                view.az = 0;
                break;
            case 's':
            case 'S':
                show_stats = !show_stats;
                break;
            default:
                break;
            }
        }
        if (ev.quit)
            running = 0;

        if (!paused) {
            /* Different rates per axis so the rotation never looks like a
             * simple spin about one axis. */
            view.ax = (view.ax + speed) & 255;
            view.ay = (view.ay + speed + (speed >> 1)) & 255;
            view.az = (view.az + (speed >> 1)) & 255;
        }

        apt_sleep(8);
    }

    cube_free(&mesh);
    apt_close(c);
    return 0;
}
