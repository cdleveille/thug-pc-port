/**
 * controls.h -- the keyboard as player one's pad, for the menus.
 *
 *   Enter, Space          A
 *   Backspace, Tab        B (back)
 *   Escape                START while skating, B (back) in the menus
 *   W A S D, arrows       D-pad
 *
 * Escape pauses, and in the main menu's screens and the pause menu goes
 * back, as on most PC games. It follows the game's own state (controls.c:
 * the current level is the main menu's, or GameIsPaused) through two of the
 * toolkit's binding layers, and changes meaning only while it is up, so the
 * press that pauses is not also read as a B in the pause menu.
 *
 * The keys reach the game through the toolkit's keyboard layer
 * (RECOMP_KEYBOARD, a default here): merged on top of a pad on port 0, or on
 * their own when no pad is connected, read only while the window has the
 * focus. host_window.c keeps Alt+Enter and Alt+Tab (window shortcuts) from
 * the game, and the F1 overlay releases every held key when it opens.
 */
#ifndef THUG_CONTROLS_H
#define THUG_CONTROLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the bindings above. Call once, before the guest starts. */
void thug_keyboard_controls_install(void);

/* The guest's memory is mapped at mem_offset (g_xbox_mem_offset): from now
 * on, thug_keyboard_controls_update reads the game's state. Any thread. */
void thug_keyboard_controls_guest_mapped(ptrdiff_t mem_offset);

/* Give Escape the meaning the game's state calls for. Call often (every
 * pass of the window loop), on the thread that records key events. */
void thug_keyboard_controls_update(void);

#ifdef __cplusplus
}
#endif

#endif /* THUG_CONTROLS_H */
