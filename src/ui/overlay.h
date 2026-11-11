/**
 * overlay.h -- the in-game settings overlay (F1), drawn with Dear ImGui.
 *
 * Threads: SDL events arrive on the process main thread (host_window.c) and
 * are queued here; the overlay is drawn on the executor's thread, which owns
 * the GL context, from the renderer's present (nv2a_pb_gl_set_overlay).
 */
#ifndef THUG_OVERLAY_H
#define THUG_OVERLAY_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register with the renderer. Call once the renderer's window exists. */
void thug_overlay_install(SDL_Window *win);

/* Main thread, for every SDL event. Returns 1 when the overlay took the
 * event (F1, L3+R3 on a pad, and all keyboard and mouse input while it
 * is open); the caller then does not pass it to the game. */
int thug_overlay_event(const SDL_Event *ev);

/* Main thread, once per loop: fullscreen asked for from the overlay, as
 * 0 or 1, or -1 for no request. */
int thug_overlay_take_fullscreen_request(void);

/* Main thread, once per loop: 1 once after the player confirmed "Restart to
 * apply changes" (the caller saves the settings and restarts, restart.h). */
int thug_overlay_take_restart_request(void);

/* Main thread: the window's size changed (mouse coordinates are in window
 * units, drawing is in drawable pixels). */
void thug_overlay_window_resized(SDL_Window *win);

/* Whether the overlay is open (it owns the mouse cursor while it is). */
int thug_overlay_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* THUG_OVERLAY_H */
