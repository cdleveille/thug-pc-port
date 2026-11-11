/**
 * launch.c -- the log file and the game files at start-up. See launch.h.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L        /* fileno, dup */
#endif

#include "launch.h"
#include "game_data.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>               /* MessageBoxA; no progress window yet */
#else
#include <SDL.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define TITLE "Tony Hawk's Underground"

/* The terminal the player started us from, when the log went to a file; -1
 * otherwise (then stderr is where messages go). */
static int  s_console = -1;
static char s_log_path[1024];
static char s_crash_msg[1200];
static size_t s_crash_len;

/* ── The log ────────────────────────────────────────────────────────────── */

#if !defined(_WIN32)
static int mkdir_p(const char *dir)
{
    char tmp[1024];
    size_t i;

    snprintf(tmp, sizeof tmp, "%s", dir);
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}
#endif

void thug_launch_log_setup(void)
{
#if !defined(_WIN32)
    const char *e = getenv("THUG_LOG");
    char dir[900], prev[1100];
    struct stat st;
    int fd;

    if (e && !strcmp(e, "-"))
        return;
    if (!e || !*e) {
        const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
        /* Someone redirected stderr on purpose: theirs. */
        if (fstat(2, &st) == 0 && (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode)))
            return;
        if (xdg && *xdg)
            snprintf(dir, sizeof dir, "%s/thug-pc-port", xdg);
        else if (home && *home)
            snprintf(dir, sizeof dir, "%s/.local/state/thug-pc-port", home);
        else
            return;
        if (mkdir_p(dir) != 0)
            return;
        snprintf(s_log_path, sizeof s_log_path, "%s/thug.log", dir);
    } else {
        snprintf(s_log_path, sizeof s_log_path, "%s", e);
    }
    /* Keep the previous run's log beside it. */
    snprintf(prev, sizeof prev, "%s", s_log_path);
    {
        char *dot = strrchr(prev, '.'), *slash = strrchr(prev, '/');
        if (dot && (!slash || dot > slash))
            snprintf(dot, sizeof prev - (size_t)(dot - prev), ".prev.log");
        else
            snprintf(prev + strlen(prev), sizeof prev - strlen(prev), ".prev");
    }
    rename(s_log_path, prev);
    fd = open(s_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[thug] cannot write the log %s: %s; logging here\n",
                s_log_path, strerror(errno));
        s_log_path[0] = 0;
        return;
    }
    if (isatty(2)) {
        s_console = dup(2);
        if (s_console >= 0) {
            dprintf(s_console, TITLE ": the log is in %s\n", s_log_path);
            s_crash_len = (size_t)snprintf(s_crash_msg, sizeof s_crash_msg,
                                           TITLE " crashed. The details are at the end of %s\n",
                                           s_log_path);
            if (s_crash_len >= sizeof s_crash_msg)
                s_crash_len = sizeof s_crash_msg - 1;
        }
    }
    fflush(stdout);
    fflush(stderr);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
#endif
}

void thug_launch_crash_notice(void)
{
#if !defined(_WIN32)
    if (s_console >= 0 && s_crash_len) {
        ssize_t r = write(s_console, s_crash_msg, s_crash_len);
        (void)r;
    }
#endif
}

/* A line for the player: the terminal if there is one, and always the log. */
static void say(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[thug] %s", buf);
#if !defined(_WIN32)
    if (s_console >= 0)
        dprintf(s_console, "%s", buf);
#endif
}

/* ── The game files ─────────────────────────────────────────────────────── */

typedef struct {
    int   video;                /* SDL video is up: the progress window */
    void *win;                  /* SDL_Window */
    int   last_pct;
    int   started;              /* the image checked out and copying began */
    const ThugGameDir *gd;
} Ui;

static int headless(void)
{
    const char *h = getenv("RECOMP_HEADLESS");
    return h && *h && *h != '0';
}

static void ui_error(Ui *ui, const char *text)
{
    (void)ui;
    say("%s\n", text);
    if (headless())
        return;
#if defined(_WIN32)
    MessageBoxA(NULL, text, TITLE, MB_OK | MB_ICONERROR);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE, text, NULL);
#endif
}

/* A small window with a bar, the percentage in its title. Drawn into the
 * window's own surface, so no GL driver is loaded before the renderer's. */
static void ui_draw(Ui *ui, int pct)
{
#if !defined(_WIN32)
    SDL_Surface *s;
    SDL_Rect r;
    int w, h, pad, bh;

    if (!ui->win || !(s = SDL_GetWindowSurface((SDL_Window *)ui->win)))
        return;
    w = s->w;
    h = s->h;
    pad = w / 16;
    bh = h / 4;
    SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0x1C, 0x1C, 0x1E));
    r.x = pad; r.y = (h - bh) / 2; r.w = w - 2 * pad; r.h = bh;
    SDL_FillRect(s, &r, SDL_MapRGB(s->format, 0x45, 0x45, 0x4A));
    r.x += 2; r.y += 2; r.h -= 4;
    r.w = (int)((long)(w - 2 * pad - 4) * pct / 100);
    SDL_FillRect(s, &r, SDL_MapRGB(s->format, 0xE8, 0x6A, 0x17));
    SDL_UpdateWindowSurface((SDL_Window *)ui->win);
#else
    (void)ui;
    (void)pct;
#endif
}

static int ui_progress(uint64_t done, uint64_t total, void *user)
{
    Ui *ui = (Ui *)user;
    int pct = total ? (int)(done * 100 / total) : 100;

    /* The first call comes once the image has been checked: only now is
     * there something to show. */
    if (!ui->started) {
        ui->started = 1;
        say("%s the game files from %s into %s (%.1f GB, once).\n",
            ui->gd->has_xbe ? "Unpacking again" : "First launch: unpacking",
            ui->gd->iso, ui->gd->dir, (double)total / 1e9);
#if !defined(_WIN32)
        if (ui->video) {
            ui->win = SDL_CreateWindow(TITLE ": unpacking the game files",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       520, 90, 0);
            ui_draw(ui, 0);
        }
#endif
    }

    if (pct != ui->last_pct) {
        ui->last_pct = pct;
#if !defined(_WIN32)
        if (s_console >= 0)        /* the terminal: one rewritten line */
            dprintf(s_console, "\r  unpacking: %3d%%  (%.1f of %.1f GB)%s", pct,
                    (double)done / 1e9, (double)total / 1e9, pct == 100 ? "\n" : "");
        else
#endif
        if (pct % 10 == 0)
            fprintf(stderr, "[thug] unpacking: %d%% (%.1f of %.1f GB)\n", pct,
                    (double)done / 1e9, (double)total / 1e9);
#if !defined(_WIN32)
        if (ui->win) {
            char title[128];
            snprintf(title, sizeof title, TITLE ": unpacking the game files, %d%%", pct);
            SDL_SetWindowTitle((SDL_Window *)ui->win, title);
        }
#endif
    }
#if !defined(_WIN32)
    if (ui->win) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT
                    || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE))
                return 1;                      /* the player closed it: stop */
        }
        ui_draw(ui, pct);
    }
#endif
    return 0;
}

int thug_launch_prepare(char *game_dir, size_t n)
{
    ThugGameDir gd;
    Ui ui;
    char searched[1200], err[2048], text[3600];
    ThugGameStatus st;
    int found;

    memset(&ui, 0, sizeof ui);
    ui.last_pct = -1;
    found = thug_game_dir_locate(&gd, searched, sizeof searched);
    if (found && gd.xbe_ok) {
        snprintf(game_dir, n, "%s", gd.dir);
        fprintf(stderr, "[thug] game files: %s (default.xbe verified)\n", gd.dir);
        return 1;
    }

    /* Anything else may need a window: SDL video, unless headless. Asked
     * before SDL_Init, as the renderer does, so Ctrl+C and `timeout` still
     * end the process. */
#if !defined(_WIN32)
    if (!headless()) {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        ui.video = SDL_WasInit(SDL_INIT_VIDEO) || SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
    }
#endif

    if (!found && !gd.has_xbe && !gd.iso[0]) {
        snprintf(text, sizeof text,
                 "Tony Hawk's Underground needs your game files.\n\n"
                 "Put your disc image (a .iso of the USA Xbox release) or the files "
                 "unpacked from it (default.xbe and the data folder) in:\n\n  %s\n\n"
                 "Looked in:\n%s", gd.dir, searched);
        ui_error(&ui, text);
        return 0;
    }

    ui.gd = &gd;
    st = thug_game_prepare(&gd, ui_progress, &ui, err, sizeof err);
#if !defined(_WIN32)
    if (ui.win) {
        SDL_DestroyWindow((SDL_Window *)ui.win);
        ui.win = NULL;
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    }
#endif
    switch (st) {
    case THUG_GAME_READY:
    case THUG_GAME_EXTRACTED:
        if (st == THUG_GAME_EXTRACTED)
            say("Unpacked and verified (default.xbe is the USA retail build).\n");
        snprintf(game_dir, n, "%s", gd.dir);
        fprintf(stderr, "[thug] game files: %s (default.xbe verified)\n", gd.dir);
        return 1;
    case THUG_GAME_CANCELLED:
        say("%s\n", err);
        return 0;
    default:
        ui_error(&ui, err);
        return 0;
    }
}
