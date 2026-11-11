/**
 * host_window.c -- the game's window loop when the GPU renderer owns the
 * window (POSIX).
 *
 * Same shape as the toolkit's xbox_HostWindowMain (src/video/fb_present_sdl.c),
 * which this replaces when src/gpu/nv2a_pb_gl.c has opened a visible window:
 *
 *   process main thread   SDL events: the window, the keyboard, the pads
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *   executor poll thread  draws, and presents each flip (nv2a_pb_gl.c)
 *
 * The toolkit's loop presents frames itself, copied out of guest memory; here
 * the renderer swaps the window from the executor's thread, so this loop only
 * pumps events, tells the renderer when the drawable changes size, and keeps
 * the title bar current.
 *
 * Keys: F11 or Alt+Enter toggles borderless fullscreen; F10 captures the
 * next two frames for a glitch report (nv2a_pb_gl_request_capture). Closing
 * the window ends the process.
 */
#if !defined(_WIN32)

#include <SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "host_window.h"
#include "gpu/nv2a_pb_gl.h"
#include "ui/overlay.h"
#include "ui/settings.h"

/* src/input (xinput_xbox.h, keyboard_sdl.h), which the toolkit does not
 * export. While the main pump is on, the input layer reads controller state
 * that this thread's event pumping keeps current, instead of pumping SDL
 * itself from a guest thread. */
void xbox_InputSetMainPump(int on);
void xbox_InputSdlInit(void);
void xbox_InputSdlEvent(const SDL_Event *ev);

static int (*s_guest_main)(void);
static volatile int s_guest_rc;
static Uint32 s_done_event = (Uint32)-1;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* Lifted code recurses on the native stack: at least the main thread's
     * limit, and no less than 64 MB. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

/* The mouse cursor: shown while the overlay is open, and in a window for
 * CURSOR_IDLE_MS after the mouse last moved; hidden otherwise (the game has
 * no use for it). */
#define CURSOR_IDLE_MS 2000
static uint64_t s_mouse_moved_ms;

/* Until when the title says a capture was taken (F10). */
static uint64_t s_capture_note_until;

static void update_cursor(SDL_Window *win)
{
    int want = thug_overlay_is_open()
               || (!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)
                   && now_ms() - s_mouse_moved_ms < CURSOR_IDLE_MS);
    if ((SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE) != want)
        SDL_ShowCursor(want ? SDL_ENABLE : SDL_DISABLE);
}

static void set_fullscreen(SDL_Window *win, int on)
{
    SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    nv2a_pb_gl_drawable_changed();
    thug_overlay_window_resized(win);
    g_thug_settings.fullscreen = on;
    update_cursor(win);
}

static void toggle_fullscreen(SDL_Window *win)
{
    set_fullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
    thug_settings_save();
}

static void update_title(SDL_Window *win, uint64_t *t_last, uint64_t *flips_last)
{
    uint64_t now = now_ms(), flips;
    uint32_t w, h;
    char title[160], res[32] = "";

    if (*t_last && now - *t_last < 1000)
        return;
    nv2a_pb_gl_stats(&flips, &w, &h);
    if (w && h)
        snprintf(res, sizeof res, "%ux%u | ", (unsigned)w, (unsigned)h);
    snprintf(title, sizeof title, "Tony Hawk's Underground | %sFPS: %.1f%s", res,
             *t_last ? (double)(flips - *flips_last) * 1000.0
                       / (double)(now - *t_last)
                     : 0.0,
             now < s_capture_note_until ? " | glitch captured (F10)" : "");
    SDL_SetWindowTitle(win, title);
    *t_last = now;
    *flips_last = flips;
}

int thug_host_window_main(int (*guest_main)(void))
{
    SDL_Window *win = (SDL_Window *)nv2a_pb_gl_window();
    uint64_t t_title = 0, flips_title = 0;
    int quit = 0, done = 0;

    if (!win)
        return guest_main();
    if (SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "[thug] SDL events/controllers: %s\n", SDL_GetError());
    s_done_event = SDL_RegisterEvents(1);
    if (s_done_event == (Uint32)-1) {
        fprintf(stderr, "[thug] SDL_RegisterEvents failed; window not pumped\n");
        return guest_main();
    }
    s_mouse_moved_ms = now_ms();
    update_cursor(win);

    thug_overlay_install(win);
    s_guest_main = guest_main;
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    if (!start_guest()) {
        fprintf(stderr, "[thug] cannot start the guest thread\n");
        xbox_InputSetMainPump(0);
        return guest_main();
    }

    while (!quit && !done) {
        SDL_Event ev;

        /* A short timeout keeps the pads polled at a steady rate (SDL reads
         * joysticks while it pumps) and the title bar ticking. */
        if (SDL_WaitEventTimeout(&ev, 4)) {
            do {
                if (ev.type == SDL_MOUSEMOTION)
                    s_mouse_moved_ms = now_ms();
                /* The settings overlay (F1) sees input first and keeps what
                 * it uses while open. */
                if (thug_overlay_event(&ev))
                    continue;
                xbox_InputSdlEvent(&ev);
                switch (ev.type) {
                case SDL_QUIT:
                    quit = 1;
                    break;
                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                            || ev.window.event == SDL_WINDOWEVENT_RESIZED
                            || ev.window.event == SDL_WINDOWEVENT_SHOWN
                            || ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                        nv2a_pb_gl_drawable_changed();
                        thug_overlay_window_resized(win);
                    }
                    /* "Mute in background" follows the keyboard focus. */
                    if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        thug_settings_set_focused(0);
                    else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                        thug_settings_set_focused(1);
                    break;
                case SDL_KEYDOWN:
                    if (ev.key.repeat)
                        break;
                    if (ev.key.keysym.sym == SDLK_F11
                            || (ev.key.keysym.sym == SDLK_RETURN
                                && (ev.key.keysym.mod & KMOD_ALT)))
                        toggle_fullscreen(win);
                    if (ev.key.keysym.sym == SDLK_F10) {
                        nv2a_pb_gl_request_capture();
                        s_capture_note_until = now_ms() + 4000;
                        update_title(win, &t_title, &flips_title);
                    }
                    break;
                default:
                    if (ev.type == s_done_event)
                        done = 1;
                    break;
                }
            } while (SDL_PollEvent(&ev));
        }
        {
            int fs = thug_overlay_take_fullscreen_request();
            if (fs >= 0)
                set_fullscreen(win, fs);
        }
        update_cursor(win);
        update_title(win, &t_title, &flips_title);
    }

    fprintf(stderr, "[thug] %s\n", quit ? "window closed" : "guest returned");
    if (quit) {
        /* The executor's thread may be inside a swap on this window, and
         * lifted code cannot be stopped mid-flight, so nothing is torn down:
         * flush what is buffered and leave. The title's own file writes have
         * already gone through write(2). */
        fflush(NULL);
        _exit(0);
    }
    return s_guest_rc;
}

#endif /* !_WIN32 */
