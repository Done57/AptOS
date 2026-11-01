#ifndef APTOS_GUI_H
#define APTOS_GUI_H

#include "gfx.h"
#include "mouse.h"

struct window;
typedef struct window window_t;

typedef struct app {
    const char *title;      /* used when title_id is 0 (dynamically named apps) */
    int  title_id;          /* sid_t from i18n.h, or 0 to use .title           */
    int  width, height;
    void (*init)(window_t *w);
    void (*draw)(window_t *w);
    void (*key)(window_t *w, int key, u32 mods);
    void (*mouse)(window_t *w, int x, int y, int buttons, int event);
    void (*tick)(window_t *w);
    void (*close)(window_t *w);
    int  is_user;           /* 1 for the shared ring 3 program window */
} app_t;

struct window {
    const app_t *app;
    int  x, y;
    int  w, h;
    int  cx, cy, cw, ch;
    int  used;
    int  orphan;            /* owner died; do not reuse or draw */
    void *state;
};

#define GUI_MOUSE_DOWN 1
#define GUI_MOUSE_UP   2
#define GUI_MOUSE_MOVE 3

#define GUI_TITLE_H 20
#define GUI_BORDER  2
#define GUI_TASKBAR_H 26

void      gui_init(void);
void      gui_dirty(void);
window_t *gui_open(const app_t *app);
void      gui_resize(window_t *w, int cw, int ch);
void      gui_close(window_t *w);
void      gui_orphan_user_windows(void);
void      gui_reap_orphans(void);
void      gui_focus(window_t *w);
window_t *gui_focused(void);
void      gui_poll(void);
void      gui_set_input(int enabled);
void      gui_cursor_refresh(void);
void      gui_run(void) __attribute__((noreturn));

void ui_panel(int x, int y, int w, int h, u32 bg);
int  ui_button(int x, int y, int w, int h, const char *label, int active);
void ui_text(int x, int y, const char *s, u32 color);
const char *app_title(const app_t *app);

extern const app_t app_terminal;
extern const app_t app_files;
extern const app_t app_setup;
extern const app_t app_editor;
extern const app_t app_paint;
extern const app_t app_about;
extern const app_t app_snake;
extern const app_t app_mines;
extern const app_t app_2048;

void app_editor_load(const char *filename);

#endif
