/**
 * launch.c -- the log file and the game files at start-up. See launch.h.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L        /* fileno, dup */
#endif

#include "launch.h"
#include "game_data.h"
#include "window_icon.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#if defined(_WIN32)
#include <windows.h>               /* MessageBoxA, the console's handle */
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir((p), 0755)
#endif

#define TITLE "Tony Hawk's Underground"

/* The terminal the player started us from, when the log went to a file; -1
 * (Windows: NULL) otherwise (then stderr is where messages go). */
#if defined(_WIN32)
static HANDLE s_console;
/* stdout and stderr as the game was started with, for a restart. */
static HANDLE s_orig_out, s_orig_err;
static int    s_have_orig;
#else
static int  s_console = -1;
/* stdout and stderr as the game was started with, while the log stands in
 * for them; -1 when it does not. A restart hands them back. */
static int  s_orig_out = -1, s_orig_err = -1;
#endif
static char s_log_path[1024];
static char s_crash_msg[1200];
static size_t s_crash_len;

/* Text for the terminal the player started us from, if the log took stderr. */
static int console_open(void)
{
#if defined(_WIN32)
    return s_console != NULL;
#else
    return s_console >= 0;
#endif
}

static void console_write(const char *text, size_t len)
{
#if defined(_WIN32)
    DWORD wrote;
    if (s_console)
        WriteFile(s_console, text, (DWORD)len, &wrote, NULL);
#else
    if (s_console >= 0) {
        ssize_t r = write(s_console, text, len);
        (void)r;
    }
#endif
}

static void console_printf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int n;

    if (!console_open())
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        console_write(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

/* ── The log ────────────────────────────────────────────────────────────── */

static int mkdir_p(const char *dir)
{
    char tmp[1024];
    size_t i;

    snprintf(tmp, sizeof tmp, "%s", dir);
    for (i = 1; tmp[i]; i++) {
        if ((tmp[i] == '/' || tmp[i] == '\\') && tmp[i - 1] != ':') {
            char c = tmp[i];
            tmp[i] = 0;
            if (mkdir_one(tmp) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = c;
        }
    }
    return mkdir_one(tmp) != 0 && errno != EEXIST ? -1 : 0;
}

int thug_launch_state_dir(char *out, size_t n)
{
#if defined(_WIN32)
    const char *base = getenv("LOCALAPPDATA");

    if (!base || !*base)
        return 0;
    snprintf(out, n, "%s\\thug-pc-port", base);
#else
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");

    if (xdg && *xdg)
        snprintf(out, n, "%s/thug-pc-port", xdg);
    else if (home && *home)
        snprintf(out, n, "%s/.local/state/thug-pc-port", home);
    else
        return 0;
#endif
    return mkdir_p(out) == 0;
}

/* Keep the previous run's log beside it: thug.log -> thug.prev.log. */
static void rotate_log(void)
{
    char prev[1100];
    char *dot, *slash;

    snprintf(prev, sizeof prev, "%s", s_log_path);
    dot = strrchr(prev, '.');
    slash = strrchr(prev, '/');
#if defined(_WIN32)
    if (strrchr(prev, '\\') && (!slash || strrchr(prev, '\\') > slash))
        slash = strrchr(prev, '\\');
#endif
    if (dot && (!slash || dot > slash))
        snprintf(dot, sizeof prev - (size_t)(dot - prev), ".prev.log");
    else
        snprintf(prev + strlen(prev), sizeof prev - strlen(prev), ".prev");
#if defined(_WIN32)
    /* rename() does not replace a file on Windows. A run started by "Restart
     * to apply changes" can get here while the run before it still holds the
     * log: it is on its way out, so wait a moment for it. */
    {
        int tries;
        remove(prev);
        for (tries = 0; tries < 50 && rename(s_log_path, prev) != 0 && errno == EACCES; tries++)
            Sleep(20);
    }
#else
    rename(s_log_path, prev);
#endif
}

void thug_launch_log_setup(void)
{
    const char *e = getenv("THUG_LOG");
    char dir[900];

    if (e && !strcmp(e, "-"))
        return;
#if defined(_WIN32)
    {
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        DWORD type = (err && err != INVALID_HANDLE_VALUE) ? GetFileType(err)
                                                          : FILE_TYPE_UNKNOWN;
        FILE *f;

        s_orig_out = GetStdHandle(STD_OUTPUT_HANDLE);
        s_orig_err = err;
        s_have_orig = 1;
        if (!e || !*e) {
            /* Someone redirected stderr on purpose (a file, or a pipe: an
             * MSYS2 terminal is one too): theirs. */
            if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE)
                return;
            if (!thug_launch_state_dir(dir, sizeof dir))
                return;
            snprintf(s_log_path, sizeof s_log_path, "%s\\thug.log", dir);
        } else {
            snprintf(s_log_path, sizeof s_log_path, "%s", e);
        }
        rotate_log();
        /* Truncate, then both streams append: they share the file without
         * writing over each other. "N": not inherited by a restarted run. */
        f = fopen(s_log_path, "wN");
        if (!f) {
            fprintf(stderr, "[thug] cannot write the log %s: %s; logging here\n",
                    s_log_path, strerror(errno));
            s_log_path[0] = 0;
            return;
        }
        fclose(f);
        /* A console for the player's messages: ours (a console build), or,
         * for the windowed build, the one of the terminal that started us. */
        if (type == FILE_TYPE_CHAR) {
            s_console = err;
        } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            s_console = CreateFileA("CONOUT$", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    OPEN_EXISTING, 0, NULL);
            if (s_console == INVALID_HANDLE_VALUE)
                s_console = NULL;
            else
                console_write("\n", 1);    /* the shell's prompt is already out */
        }
        if (s_console) {
            console_printf(TITLE ": the log is in %s\n", s_log_path);
            s_crash_len = (size_t)snprintf(s_crash_msg, sizeof s_crash_msg,
                                           TITLE " crashed. The details are at the end of %s\n",
                                           s_log_path);
            if (s_crash_len >= sizeof s_crash_msg)
                s_crash_len = sizeof s_crash_msg - 1;
        }
        fflush(stdout);
        fflush(stderr);
        /* freopen, not dup2: a program started without a console has no
         * descriptor behind stdout and stderr to replace. */
        if (!freopen(s_log_path, "aN", stderr) || !freopen(s_log_path, "aN", stdout)) {
            s_console = NULL;
            s_log_path[0] = 0;
            return;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    }
#else
    {
        struct stat st;
        int fd;

        if (!e || !*e) {
            /* Someone redirected stderr on purpose: theirs. */
            if (fstat(2, &st) == 0 && (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode)))
                return;
            if (!thug_launch_state_dir(dir, sizeof dir))
                return;
            snprintf(s_log_path, sizeof s_log_path, "%s/thug.log", dir);
        } else {
            snprintf(s_log_path, sizeof s_log_path, "%s", e);
        }
        rotate_log();
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
        s_orig_out = fcntl(1, F_DUPFD_CLOEXEC, 3);
        s_orig_err = fcntl(2, F_DUPFD_CLOEXEC, 3);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
#endif
}

void thug_launch_restore_stdio(void)
{
    fflush(NULL);
#if defined(_WIN32)
    /* The handles a restarted run is given (restart.c); the log stays open
     * for this run's last lines. */
    if (s_have_orig) {
        SetStdHandle(STD_OUTPUT_HANDLE, s_orig_out);
        SetStdHandle(STD_ERROR_HANDLE, s_orig_err);
    }
#else
    if (s_orig_out >= 0)
        dup2(s_orig_out, 1);
    if (s_orig_err >= 0)
        dup2(s_orig_err, 2);
#endif
}

void thug_launch_crash_notice(void)
{
    if (console_open() && s_crash_len) {
        console_write(s_crash_msg, s_crash_len);
        return;
    }
#if defined(_WIN32)
    /* Started without a console (Explorer, a shortcut): say so in a box, or
     * the game just vanishes. */
    if (s_log_path[0] && !getenv("RECOMP_HEADLESS")) {
        char text[1300];
        snprintf(text, sizeof text, TITLE " crashed.\n\nThe details are at the end of\n%s",
                 s_log_path);
        MessageBoxA(NULL, text, TITLE, MB_OK | MB_ICONERROR | MB_TOPMOST);
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
    console_printf("%s", buf);
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
        if (ui->video) {
            float k = 1.0f;
#if defined(_WIN32)
            /* The window is in real pixels (DPI aware, window_icon.c): as
             * large on a scaled desktop as on a 100% one. Windows reports the
             * desktop's scale as a DPI over 96. */
            float ddpi = 0.0f;
            if (SDL_GetDisplayDPI(0, &ddpi, NULL, NULL) == 0 && ddpi > 96.0f && ddpi < 400.0f)
                k = ddpi / 96.0f;
#endif
            ui->win = SDL_CreateWindow(TITLE ": unpacking the game files",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       (int)(520 * k), (int)(90 * k), 0);
            thug_window_set_icon((SDL_Window *)ui->win);
            ui_draw(ui, 0);
        }
    }

    if (pct != ui->last_pct) {
        ui->last_pct = pct;
        if (console_open())        /* the terminal: one rewritten line */
            console_printf("\r  unpacking: %3d%%  (%.1f of %.1f GB)%s", pct,
                           (double)done / 1e9, (double)total / 1e9, pct == 100 ? "\n" : "");
        else if (pct % 10 == 0)
            fprintf(stderr, "[thug] unpacking: %d%% (%.1f of %.1f GB)\n", pct,
                    (double)done / 1e9, (double)total / 1e9);
        if (ui->win) {
            char title[128];
            snprintf(title, sizeof title, TITLE ": unpacking the game files, %d%%", pct);
            SDL_SetWindowTitle((SDL_Window *)ui->win, title);
        }
    }
    if (ui->win) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT
                    || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE))
                return 1;                      /* the player closed it: stop */
        }
        ui_draw(ui, pct);
    }
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
    if (!headless()) {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        thug_window_hints();
        ui.video = SDL_WasInit(SDL_INIT_VIDEO) || SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
    }

    if (!found && !gd.has_xbe && !gd.iso[0]) {
        char exe[1024];
        thug_exe_dir(exe, sizeof exe);
        snprintf(text, sizeof text,
                 "Tony Hawk's Underground needs your game files.\n\n"
                 "Put your disc image (a .iso of the USA Xbox release) beside the "
                 "game, in:\n\n  %s\n\n"
                 "It is unpacked into a game folder there on the first launch. Or put "
                 "the files already unpacked from it (default.xbe and the data folder) "
                 "in:\n\n  %s\n\n"
                 "Looked in:\n%s", exe, gd.dir, searched);
        ui_error(&ui, text);
        return 0;
    }

    ui.gd = &gd;
    st = thug_game_prepare(&gd, ui_progress, &ui, err, sizeof err);
    if (ui.win) {
        SDL_DestroyWindow((SDL_Window *)ui.win);
        ui.win = NULL;
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    }
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
