/*
 * desk.c —— Aptos/SixOS 解耦桌面环境（R3 用户态）
 *
 * 系统默认纯 CLI，本程序只有安装了桌面包（desktop.lst 把 DESK.APP 放进
 * /APPS）后才会被找到；没装时 shell 在 /APPS 里找不到，直接报
 * "desk: command not found"，与内核零耦合。
 *
 * 所有资源都走 sys_* 申请，退出/停止时归还；进程被内核回收即可，
 * 桌面崩溃绝不影响内核。
 *
 * 状态机：
 *     IDLE --desk---------> INITIALIZED   (申请基础资源，不进主循环)
 *                             --desk -o<模式>>--> READY  (多次 -o 以最后一次为准)
 *                                                  --start--> RUNNING
 *                             RUNNING --desk -o stop--> 释放资源 -> IDLE
 */

#include "aptos.h"

/* ---------------- 状态机 ---------------- */
enum desk_state {
    ST_IDLE = 0,     /* 还没敲过 desk */
    ST_INITIALIZED,  /* 基础资源已申请，未选模式 */
    ST_READY,        /* 已用 -o 选模式，等待 start */
    ST_RUNNING       /* 渲染主循环运行中 */
};

enum desk_mode {
    MODE_NONE = 0,
    MODE_GUI,        /* -o start : 全量 2D/3D + 窗口管理器 */
    MODE_SAFE,       /* -o -i   : 安全模式，保守 2D */
    MODE_TUI         /* -o -s   : 纯文本，不申请画布 */
};

static enum desk_state g_state = ST_IDLE;
static enum desk_mode  g_mode  = MODE_NONE;   /* 多次 -o 以最后一次为准 */
static volatile int    g_exit_req = 0;        /* stop 时置位，让主循环体面退出 */

/* 基础资源（INITIALIZED 时申请；stop/退出时释放） */
static void *g_canvas       = 0;
static int   g_canvas_bytes = 0;
static int   g_win_ok       = 0;

#define GUI_W 800
#define GUI_H 600

/* ---------------- 基础初始化：不带参数的 desk ---------------- */
static int desk_init(void)
{
    if (g_state == ST_RUNNING) {                       /* 防呆 3 */
        puts("[desk] desktop is already running. use 'desk -o stop' first.\n");
        return -1;
    }
    if (g_state != ST_IDLE)                            /* 幂等 */
        return 0;

    /* 只申请最基础的东西：一块共享画布（字库由内核 gfx 提供） */
    g_canvas_bytes = GUI_W * GUI_H * 4;
    g_canvas = sys_mmap(g_canvas_bytes);
    if (!g_canvas) {
        puts("[desk] init failed: cannot allocate canvas.\n");
        return -1;
    }
    g_state = ST_INITIALIZED;
    puts("[desk] initialized. mode='none'.\n"
         "       use 'desk -o start|-i|-s' then 'start'.\n");
    return 0;
}

/* ---------------- 选模式：desk -o <mode> ---------------- */
static int desk_set_mode(enum desk_mode m, const char *tag)
{
    if (g_state == ST_IDLE) {                          /* 防呆 1 */
        puts("[desk] not initialized. type 'desk' first.\n");
        return -1;
    }
    if (g_state == ST_RUNNING) {                       /* 防呆 3 */
        puts("[desk] desktop is already running. use 'desk -o stop' first.\n");
        return -1;
    }
    g_mode  = m;                                       /* 后到覆盖先到 */
    g_state = ST_READY;
    printf("[desk] mode set to '%s'. type 'start' to launch.\n", tag);
    return 0;                                          /* 防呆 2：停在 READY */
}

/* ---------------- 释放资源，回到 IDLE ---------------- */
static void desk_shutdown(void)
{
    if (g_win_ok) { sys_win_close(); g_win_ok = 0; }
    if (g_canvas) { sys_munmap(g_canvas, g_canvas_bytes); g_canvas = 0; }
    g_mode     = MODE_NONE;
    g_exit_req = 0;
    g_state    = ST_IDLE;
    puts("[desk] stopped. resources released.\n");
}

/* ---------------- 三种模式的主循环 ---------------- */
static void run_gui(void)      /* -o start */
{
    if (sys_win_open("desk", GUI_W, GUI_H) == 0)
        g_win_ok = 1;

    g_state = ST_RUNNING;
    puts("[desk] GUI mode running (wm + 2d/3d). Esc to stop.\n");

    while (g_state == ST_RUNNING && !g_exit_req) {
        int k = sys_getkey();
        if (k == 27)           /* Esc */
            break;
        if (!g_win_ok || !sys_win_alive())
            break;             /* 窗口被关掉，友好退出 */

        /* 每帧：先清屏，再画窗口管理器与 2D/3D 内容（此处示例） */
        sys_rect(0, 0, GUI_W, GUI_H, 0x00203040);
        sys_text(16, 16, "desk / GUI mode", 0x00FFFFFF, 2);
        sys_text(16, 48, "wm + 2d/3d active", 0x00A0C0E0, 1);

        sys_pump();            /* 提交本帧 + 处理输入 */
        sys_sleep(16);         /* ~60fps */
    }
    desk_shutdown();
}

static void run_safe(void)     /* -o -i */
{
    if (sys_win_open("desk (safe)", 640, 480) == 0)
        g_win_ok = 1;

    g_state = ST_RUNNING;
    puts("[desk] SAFE mode running (2d only, no accel). Esc to stop.\n");

    while (g_state == ST_RUNNING && !g_exit_req) {
        int k = sys_getkey();
        if (k == 27)
            break;
        if (!g_win_ok || !sys_win_alive())
            break;

        /* 最保守的 2D：纯色填充 + 文本，不做任何特效 */
        sys_rect(0, 0, 640, 480, 0x00000000);
        sys_text(8, 8, "safe mode / 2d only", 0x00FFFFFF, 1);
        sys_pump();
        sys_sleep(33);
    }
    desk_shutdown();
}

static void run_tui(void)      /* -o -s */
{
    /* 完全不申请图形画布，直接退回纯文本终端 */
    puts("[desk] TUI mode (no canvas requested).\n");
    puts("[desk] press Enter or Esc to leave TUI.\n");

    g_state = ST_RUNNING;
    for (;;) {
        int k = sys_getkey();
        if (k == '\n' || k == '\r' || k == 27 || g_exit_req)
            break;
    }
    /* TUI 没开窗也没额外占画布，退回到 INITIALIZED（保留基础画布） */
    g_mode     = MODE_NONE;
    g_exit_req = 0;
    g_state    = (g_canvas ? ST_INITIALIZED : ST_IDLE);
    puts("[desk] left TUI.\n");
}


/* ---------------- 交互会话：裸 desk 进入，子命令走同一套状态机 ---------------- */
static int desk_start(void);   /* 前置声明 */
static int read_line_into(char *buf, int cap)
{
    int n = 0;
    buf[0] = 0;
    for (;;) {
        int k = sys_getkey();
        if (k == '\n' || k == '\r') {
            printf("\n");
            buf[n] = 0;
            return n;
        }
        if (k == 27) {                 /* Esc：结束会话 */
            printf("\n");
            return -1;
        }
        if (k == 8 || k == 127) {      /* 退格 */
            if (n > 0) { n--; buf[n] = 0; printf("\b \b"); }
            continue;
        }
        if (k >= 32 && k < 127 && n < cap - 1) {
            buf[n++] = (char)k;
            buf[n] = 0;
            char c[2]; c[0] = (char)k; c[1] = 0;
            printf("%s", c);
        }
    }
}

/* 把一行按空格切成 argc/argv，交给同一套分发逻辑 */
/* 把一组 argv 词交给状态机分发（会话与命令行两条入口共用） */
static void dispatch_argv(char **argv, int argc)
{
    if (argc <= 0) return;

    /* 单发车指令：start（不带 -o） */
    if (argc == 1 && strcmp(argv[0], "start") == 0) { desk_start(); return; }

    /* 会话内退出 */
    if (argc == 1 && (strcmp(argv[0], "exit") == 0 ||
                      strcmp(argv[0], "quit") == 0)) {
        g_exit_req = 1;
        return;
    }

    /* 所有模式必须以 -o 开头 */
    if (strcmp(argv[0], "-o") == 0) {
        const char *m = (argc >= 2) ? argv[1] : "";
        if (strcmp(m, "start") == 0)       desk_set_mode(MODE_GUI,  "start");
        else if (strcmp(m, "-i") == 0)     desk_set_mode(MODE_SAFE, "-i");
        else if (strcmp(m, "-s") == 0)     desk_set_mode(MODE_TUI,  "-s");
        else if (strcmp(m, "stop") == 0) {
            if (g_state == ST_RUNNING) g_exit_req = 1;
            else puts("[desk] desktop is not running.\n");
        } else if (m[0] == 0) {
            puts("[desk] '-o' needs a mode\n");
        } else {
            printf("[desk] unknown mode '%s'\n", m);
        }
        return;
    }

    printf("[desk] unknown command '%s' (use '-o <mode>', 'start', 'exit')\n",
           argv[0]);
}

/* 把一行按空格切成 argv，再分发 */
static void dispatch_line(char *line)
{
    char *argv[16];
    int argc = 0;
    char *p = line;

    while (*p == ' ') p++;
    if (!*p) return;

    while (*p && argc < 16) {
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
        while (*p == ' ') p++;
    }
    dispatch_argv(argv, argc);
}

static void desk_session(void)
{
    char line[128];

    if (desk_init() != 0)
        return;

    puts("[desk] session started. commands: '-o start|-i|-s', 'start', "
         "'-o stop', 'exit'\n");

    while (!g_exit_req) {
        if (g_state == ST_INITIALIZED || g_state == ST_READY)
            printf("desk> ");
        else
            printf("desk> ");
        int n = read_line_into(line, sizeof(line));
        if (n < 0) break;           /* Esc 结束会话 */
        dispatch_line(line);
        if (g_state == ST_RUNNING && g_exit_req) {
            /* stop 已在主循环外请求：这里补一次收尾 */
            break;
        }
    }

    /* 会话结束时，若仍持有资源则释放 */
    if (g_state != ST_IDLE)
        desk_shutdown();
    puts("[desk] session ended.\n");
}

/* ---------------- 发车：start ---------------- */
static int desk_start(void)
{
    if (g_state == ST_IDLE) {                          /* 防呆 1 */
        puts("[desk] not initialized. type 'desk' first.\n");
        return -1;
    }
    if (g_state == ST_RUNNING) {                       /* 防呆 3 */
        puts("[desk] desktop is already running. use 'desk -o stop' first.\n");
        return -1;
    }
    if (g_state != ST_READY) {                         /* 防呆 2：没选模式 */
        puts("[desk] no mode selected. use 'desk -o start|-i|-s' first.\n");
        return -1;
    }
    switch (g_mode) {
    case MODE_GUI:  run_gui();  break;
    case MODE_SAFE: run_safe(); break;
    case MODE_TUI:  run_tui();  break;
    default:
        puts("[desk] unknown mode.\n");
        return -1;
    }
    return 0;
}

/* ---------------- 参数解析（模拟 argc/argv） ----------------
 * 本内核的用户进程通过 sys_args() 拿参数，而不是栈上的 argc/argv：
 *   index 0 -> arg0 程序路径（如 /APPS/DESK.APP）
 *   index 1 -> arg1 命令行参数串（如 "-o stop"）
 * 裸 desk（无 arg1）进入交互会话；带参数则把参数串切成 argv 后分发。
 */
int main(void)
{
    char params[128];

    /* 有没有后续参数？ */
    if (sys_args_len(1) <= 0) {
        desk_session();         /* 裸 desk：初始化 + 交互会话 */
        return 0;
    }

    params[0] = 0;
    if (sys_args(1, params, sizeof(params)) != 0)
        params[0] = 0;

    dispatch_line(params);      /* 一次性命令行：-o start|-i|-s / start / -o stop */
    return 0;
}
