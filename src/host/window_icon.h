/**
 * window_icon.h -- the game's window icon and application ID.
 *
 * The icon is thug.png (repository root), embedded at build time as RGBA by
 * scripts/embed_icon.py. On Wayland, SDL hands it to the compositor through
 * the xdg-toplevel-icon protocol (KDE Plasma 6, GNOME 47+), which is what puts
 * it on the title bar and the taskbar; on X11 and Windows it is the usual
 * window icon.
 */
#ifndef THUG_WINDOW_ICON_H
#define THUG_WINDOW_ICON_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Before SDL video starts: the application ID ("thug-pc-port") the desktop
 * groups the windows by -- Wayland app_id, X11 WM_CLASS. */
void thug_window_hints(void);

/* Give a window the game's icon. */
void thug_window_set_icon(SDL_Window *win);

#ifdef __cplusplus
}
#endif

#endif /* THUG_WINDOW_ICON_H */
