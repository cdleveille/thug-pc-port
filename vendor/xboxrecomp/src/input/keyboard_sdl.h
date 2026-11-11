/**
 * xbox_input keyboard, SDL side: the SDL window's key, mouse-button and focus
 * events into the key table of keyboard.h. See keyboard_sdl.c. POSIX, and
 * Windows hosts that run an SDL window (XBOXRECOMP_INPUT_SDL_KEYBOARD); there
 * the pads stay on XInput and xbox_InputSdlRefreshControllers does nothing.
 */
#ifndef XBOX_INPUT_KEYBOARD_SDL_H
#define XBOX_INPUT_KEYBOARD_SDL_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An SDL keycode (SDL_Keycode, unmodified) as a Windows VK, or 0 when the
 * key is not one the table records. Every key input_map.h has a name for
 * is recorded; a left or right modifier gives the either-side VK here. */
int  xbox_InputSdlKeyToVk(int sdl_keycode);
/* For a left or right Shift, Ctrl or Alt, its side-specific VK (VK_LSHIFT
 * and so on), which is recorded beside the one above; else 0. */
int  xbox_InputSdlKeyToSideVk(int sdl_keycode);
/* An SDL mouse button (SDL_BUTTON_LEFT ...) as its VK (VK_LBUTTON ...), or 0. */
int  xbox_InputSdlButtonToVk(int sdl_button);
/* Once the window exists, on the thread that pumps its events (after
 * xbox_InputSetMainPump(1)): stop SDL's text input, and open the controllers
 * already connected. */
void xbox_InputSdlInit(void);
/* Every event of the main loop: keys, mouse buttons, and the window events
 * that release them; a controller added or removed opens or closes it
 * (xbox_InputSdlRefreshControllers). Others are ignored. */
void xbox_InputSdlEvent(const SDL_Event *ev);
/* Open the controllers SDL has and close the ones it lost (xinput_device.c).
 * With the main pump on, only the pumping thread may do this: opening walks
 * SDL's device list by index, which that thread rebuilds as it handles a
 * hotplug, so an open from a title thread at that moment can corrupt the
 * heap (seen with sdl2-compat on SDL3). The title's threads then only read
 * the controllers already open. */
void xbox_InputSdlRefreshControllers(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_SDL_H */
