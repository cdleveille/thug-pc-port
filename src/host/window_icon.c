/**
 * window_icon.c -- the game's window icon and application ID. See
 * window_icon.h.
 */
#include "window_icon.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

/* Per-monitor DPI awareness for the whole process, from Windows itself:
 * SDL2's hint below is not passed on by sdl2-compat (MSYS2's SDL2 since
 * 2026, over SDL3), and without it the window was DPI-unaware, rendered at
 * the desktop's scaled size and stretched by Windows (blurry at 175%).
 * Before any window exists; a second call changes nothing. Windows 10 1703
 * and later, looked up so older systems still start. */
static void dpi_aware(void)
{
    typedef BOOL (WINAPI *set_ctx_fn)(HANDLE);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    set_ctx_fn set_ctx = user32 ? (set_ctx_fn)(void (*)(void))
        GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;

    if (set_ctx)
        set_ctx((HANDLE)(INT_PTR)-4);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
}
#endif

/* Generated from thug.png by scripts/embed_icon.py (CMakeLists.txt). */
extern const int thug_icon_w, thug_icon_h;
extern const unsigned char thug_icon_rgba[];

void thug_window_hints(void)
{
    /* SDL3's name (sdl2-compat passes it through), then SDL2's own. */
    SDL_SetHint("SDL_APP_ID", "thug-pc-port");
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "thug-pc-port");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "thug-pc-port");
#if defined(__linux__)
    {
        /* Wayland first (SDL2 itself tries X11 first), unless the player
         * chose X11 (THUG_X11, exported from the settings file before this
         * runs): NVIDIA's threaded optimisations only work there
         * (nv2a_pb_gl_driver_thread). Either falls back to the other, and
         * SDL_VIDEODRIVER still overrides both. */
        const char *x11 = getenv("THUG_X11");
        SDL_SetHint(SDL_HINT_VIDEODRIVER,
                    x11 && atoi(x11) != 0 ? "x11,wayland" : "wayland,x11");
    }
#endif
#if defined(_WIN32)
    /* A scaled desktop (a 4K display at 150%) would otherwise get a
     * 2560x1440 window that Windows stretches, blurring every frame: the
     * window works in real pixels and follows each monitor's scale. */
    dpi_aware();
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
#endif
}

void thug_window_set_icon(SDL_Window *win)
{
    SDL_Surface *s;

    if (!win)
        return;
    /* RGBA32 is R, G, B, A in memory order on any host, as the bytes are. */
    s = SDL_CreateRGBSurfaceWithFormatFrom((void *)thug_icon_rgba, thug_icon_w,
                                           thug_icon_h, 32, thug_icon_w * 4,
                                           SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return;
    SDL_SetWindowIcon(win, s);
    SDL_FreeSurface(s);
}
