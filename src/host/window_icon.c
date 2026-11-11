/**
 * window_icon.c -- the game's window icon and application ID. See
 * window_icon.h.
 */
#include "window_icon.h"

/* Generated from thug.png by scripts/embed_icon.py (CMakeLists.txt). */
extern const int thug_icon_w, thug_icon_h;
extern const unsigned char thug_icon_rgba[];

void thug_window_hints(void)
{
    /* SDL3's name (sdl2-compat passes it through), then SDL2's own. */
    SDL_SetHint("SDL_APP_ID", "thug-pc-port");
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "thug-pc-port");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "thug-pc-port");
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
