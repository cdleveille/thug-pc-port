/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host-side controls over what the title asks of the pad (see
 * xbox_InputSetBlocked and xbox_InputSetRumbleScale below): a host overlay
 * holding the input focus, and the player's vibration strength. One aligned
 * word each, read on every call. */
static volatile int   g_input_blocked;
static volatile float g_rumble_scale = 1.0f;

/* The motor speed the host pad gets for the one the title asked for. */
static WORD rumble_scaled(WORD speed)
{
    float s = g_rumble_scale;
    if (g_input_blocked || s <= 0.0f)
        return 0;
    if (s >= 1.0f)
        return speed;
    return (WORD)((float)speed * s + 0.5f);
}

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* The keyboard on port 0 (RECOMP_KEYBOARD=1) comes from keyboard.c, shared
 * with the SDL backend; the GDI window in fb_present.c records the keys. */

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

static DWORD input_get_state_host(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
                return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    return ERROR_SUCCESS;
}

/* The title's last speeds per port, as asked. XInput holds a pad's motors
 * until told otherwise, as the Xbox did, so these are only for sending again
 * when the player's strength or the overlay's focus changes. */
static WORD g_motor[XBOX_MAX_CONTROLLERS][2];

static DWORD rumble_send(DWORD port)
{
    XINPUT_VIBRATION xi_vib;

    xi_vib.wLeftMotorSpeed = rumble_scaled(g_motor[port][0]);
    xi_vib.wRightMotorSpeed = rumble_scaled(g_motor[port][1]);
    return XInputSetState(port, &xi_vib);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    g_motor[dwPort][0] = pVibration->wLeftMotorSpeed;
    g_motor[dwPort][1] = pVibration->wRightMotorSpeed;
    return rumble_send(dwPort);
}

/* Send every connected pad its held speeds as they now scale. */
static void input_resend_rumble(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_controller_connected[i])
            rumble_send(i);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Set while the framebuffer window's main thread pumps SDL events
 * (src/video/fb_present_sdl.c). The pump already updates the controllers,
 * and SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. */
static SDL_atomic_t g_main_pump;

void xbox_InputSetMainPump(int on)
{
    SDL_AtomicSet(&g_main_pump, on);
}

static void update_controllers(void)
{
    if (!SDL_AtomicGet(&g_main_pump))
        SDL_GameControllerUpdate();
}

static int controller_is_open_locked(SDL_JoystickID instance_id)
{
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot]) {
            SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pads[slot]);
            if (joystick && SDL_JoystickInstanceID(joystick) == instance_id)
                return 1;
        }
    }
    return 0;
}

static void refresh_controllers_locked(void)
{
    int count;

    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
            fprintf(stderr, "  [INPUT] port %d: controller disconnected\n", slot + 1);
        }
    }

    count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        SDL_GameController *controller;
        SDL_JoystickID instance_id;
        int slot;

        if (!SDL_IsGameController(i))
            continue;
        instance_id = SDL_JoystickGetDeviceInstanceID(i);
        if (instance_id < 0 || controller_is_open_locked(instance_id))
            continue;
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
            if (!g_pads[slot])
                break;
        if (slot == XBOX_MAX_CONTROLLERS)
            break;
        controller = SDL_GameControllerOpen(i);
        if (controller) {
            const char *name = SDL_GameControllerName(controller);
            g_pads[slot] = controller;
            g_controller_connected[slot] = TRUE;
            fprintf(stderr, "  [INPUT] port %d: %s connected\n", slot + 1,
                    name ? name : "controller");
        }
    }
}

/* The pad on a port, or NULL. Without the main pump the caller's thread is
 * the only one in SDL, so it opens and closes pads as it finds them. With it,
 * the pumping thread does that (xbox_InputSdlRefreshControllers, on SDL's
 * device events), and this only reads: opening a pad walks SDL's device list
 * by index, which the pumping thread rebuilds during a hotplug, and an open
 * from here at that moment corrupted the heap (sdl2-compat on SDL3, a pad
 * plugged in mid-game). */
static SDL_GameController *controller_for_port_locked(DWORD port)
{
    SDL_GameController *controller = g_pads[port];

    if ((!controller || !SDL_GameControllerGetAttached(controller))
            && !SDL_AtomicGet(&g_main_pump)) {
        refresh_controllers_locked();
        controller = g_pads[port];
    }
    g_controller_connected[port] =
        controller && SDL_GameControllerGetAttached(controller);
    return g_controller_connected[port] ? controller : NULL;
}

/* Vibration. An Xbox pad runs its motors at the speeds it was last sent
 * until it is sent others, and titles send a report only when a speed
 * changes: Tony Hawk's Underground starts a grind's rumble once and stops it
 * when the grind ends. An SDL rumble ends after a duration (SDL caps it at
 * 0xFFFF ms), so each port keeps the title's last request and holds it for
 * that long, and the input poll re-arms it while it is still meant to run.
 * This used to give every request one second, and a grind's rumble died a
 * second in.
 *
 * The speeds are kept as the title asked, so the player's strength and the
 * overlay's focus (rumble_scaled) apply whenever they are sent; both re-send
 * them when they change, and an overlay that closes mid-grind lets the grind
 * rumble again. Everything here is under the joystick lock. */
#define RUMBLE_HOLD_MS  0xFFFFu                  /* SDL's longest rumble */
#define RUMBLE_REARM_MS 30000u

static WORD   g_motor[XBOX_MAX_CONTROLLERS][2];  /* left, right, as asked */
static Uint32 g_rumble_armed[XBOX_MAX_CONTROLLERS]; /* SDL_GetTicks when sent; 0 = still */

static void rumble_send_locked(DWORD port, SDL_GameController *c)
{
    WORD left = rumble_scaled(g_motor[port][0]);
    WORD right = rumble_scaled(g_motor[port][1]);
    int on = left || right;

    SDL_GameControllerRumble(c, left, right, on ? RUMBLE_HOLD_MS : 0);
    g_rumble_armed[port] = on ? (SDL_GetTicks() | 1u) : 0;
}

/* Send every open pad its held speeds as they now scale: after the overlay
 * takes or gives back the input focus, or the strength changes. */
static void input_resend_rumble(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        return;                         /* set before start-up: nothing open */
    SDL_LockJoysticks();
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i] && SDL_GameControllerGetAttached(g_pads[i]))
            rumble_send_locked((DWORD)i, g_pads[i]);
    SDL_UnlockJoysticks();
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    int any = 0;

    update_controllers();
    SDL_LockJoysticks();
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (g_pads[i] && !SDL_GameControllerGetAttached(g_pads[i])) {
            if (SDL_AtomicGet(&g_main_pump))
                continue;               /* the pumping thread closes it */
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = NULL;
            g_controller_connected[i] = FALSE;
        }
        any |= g_pads[i] != NULL;
    }
    SDL_UnlockJoysticks();
    return any;
}

/* Declared in keyboard_sdl.h (which needs SDL_Event, so not included here). */
void xbox_InputSdlRefreshControllers(void)
{
    SDL_LockJoysticks();
    refresh_controllers_locked();
    SDL_UnlockJoysticks();
}

void xbox_InputInit(void)
{
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_LockJoysticks();
    refresh_controllers_locked();
    SDL_UnlockJoysticks();
}

static DWORD input_get_state_host(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    update_controllers();
    SDL_LockJoysticks();
    SDL_GameController *c = controller_for_port_locked(dwPort);
    if (!c) {
        SDL_UnlockJoysticks();
        if (dwPort == 0 && xbox_InputKeyboardPoll(pState))
            return ERROR_SUCCESS;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    /* A rumble still meant to run, re-armed well before SDL ends it. */
    if (g_rumble_armed[dwPort]
            && SDL_GetTicks() - g_rumble_armed[dwPort] >= RUMBLE_REARM_MS)
        rumble_send_locked(dwPort, c);
    SDL_UnlockJoysticks();

    if (dwPort == 0)
        keyboard_merge(pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    update_controllers();
    SDL_LockJoysticks();
    SDL_GameController *c = controller_for_port_locked(dwPort);
    if (!c) {
        SDL_UnlockJoysticks();
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    /* Held until the title sends other speeds (see rumble_send_locked). */
    g_motor[dwPort][0] = pVibration->wLeftMotorSpeed;
    g_motor[dwPort][1] = pVibration->wRightMotorSpeed;
    rumble_send_locked(dwPort, c);
    SDL_UnlockJoysticks();
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    BOOL connected;

    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    update_controllers();
    SDL_LockJoysticks();
    connected = controller_for_port_locked(dwPort) != NULL;
    SDL_UnlockJoysticks();
    return connected;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    update_controllers();
    SDL_LockJoysticks();
    if (!controller_for_port_locked(dwPort)) {
        SDL_UnlockJoysticks();
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    SDL_UnlockJoysticks();
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */

/* ======================================================================== */
/* Input focus: a host overlay (a settings menu drawn over the game) takes
 * the pad while it is open. The title keeps seeing a connected controller --
 * a pad that vanished would pause most games, or worse -- with every button
 * and stick at rest, and a fresh packet number so a button held when the
 * overlay opened reads as released. */

void xbox_InputSetBlocked(int on)
{
    g_input_blocked = on ? 1 : 0;      /* one aligned word: a flag, no ordering needed */
    input_resend_rumble();             /* taken: the motors stop; given back: a rumble
                                        * the title still holds resumes */
}

/* The player's vibration strength, 0 (off) to 1 (as the title asks), applied
 * at once to a rumble in progress. */
void xbox_InputSetRumbleScale(float scale)
{
    g_rumble_scale = scale < 0.0f ? 0.0f : scale > 1.0f ? 1.0f : scale;
    input_resend_rumble();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD rc = input_get_state_host(dwPort, pState);

    if (rc == ERROR_SUCCESS && g_input_blocked)
        memset(&pState->Gamepad, 0, sizeof pState->Gamepad);
    return rc;
}
