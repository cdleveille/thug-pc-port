#include "xinput_xbox.h"
#include <SDL.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* keyboard_sdl.h's, which needs the real SDL_Event. */
void xbox_InputSdlRefreshControllers(void);

struct SDL_Joystick {
    SDL_JoystickID instance_id;
};

struct SDL_GameController {
    struct SDL_Joystick joystick;
    int attached;
};

static struct SDL_GameController s_controller;
static int s_device_count;
static SDL_JoystickID s_device_instance;
static int s_open_count;
static int s_close_count;
static int s_update_count;
static pthread_mutex_t s_joystick_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int s_lock_balance;
static int s_fail;

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        s_fail = 1; \
    } \
} while (0)

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *state)
{
    (void)state;
    return 0;
}

int SDL_SetHint(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return 1;
}

Uint32 SDL_WasInit(Uint32 flags)
{
    (void)flags;
    return SDL_INIT_GAMECONTROLLER;
}

int SDL_InitSubSystem(Uint32 flags)
{
    (void)flags;
    return 0;
}

/* Index-based enumeration and opening, and the thread that did them last:
 * with the main pump on, only the pumping thread may (xinput_device.c). */
static int s_enum_count;
static _Atomic int s_enum_off_main;
static pthread_t s_main_thread;
static _Atomic int s_pump_on;

static void note_enum(void)
{
    s_enum_count++;
    if (atomic_load(&s_pump_on) && !pthread_equal(pthread_self(), s_main_thread))
        atomic_fetch_add(&s_enum_off_main, 1);
}

int SDL_NumJoysticks(void)
{
    note_enum();
    return s_device_count;
}

int SDL_IsGameController(int joystick_index)
{
    return joystick_index >= 0 && joystick_index < s_device_count;
}

SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int joystick_index)
{
    return SDL_IsGameController(joystick_index) ? s_device_instance : -1;
}

SDL_GameController *SDL_GameControllerOpen(int joystick_index)
{
    if (!SDL_IsGameController(joystick_index))
        return NULL;
    note_enum();
    s_controller.joystick.instance_id = s_device_instance;
    s_controller.attached = 1;
    s_open_count++;
    return &s_controller;
}

const char *SDL_GameControllerName(SDL_GameController *controller)
{
    return controller ? "Fake Pad" : NULL;
}

void SDL_GameControllerClose(SDL_GameController *controller)
{
    if (controller != &s_controller)
        abort();
    if (controller)
        controller->attached = 0;
    s_close_count++;
}

int SDL_GameControllerGetAttached(SDL_GameController *controller)
{
    return controller && controller->attached;
}

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *controller)
{
    return controller ? &controller->joystick : NULL;
}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick)
{
    return joystick ? joystick->instance_id : -1;
}

void SDL_GameControllerUpdate(void)
{
    s_update_count++;
}

Uint8 SDL_GameControllerGetButton(SDL_GameController *controller, int button)
{
    (void)controller;
    return button == SDL_CONTROLLER_BUTTON_A;
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController *controller, int axis)
{
    (void)controller;
    (void)axis;
    return 0;
}

static int s_rumble_calls;
static uint16_t s_rumble_low, s_rumble_high;
static Uint32 s_rumble_ms;
static Uint32 s_ticks = 1000;               /* SDL_GetTicks, moved by hand */

int SDL_GameControllerRumble(SDL_GameController *controller,
                             uint16_t low_frequency_rumble,
                             uint16_t high_frequency_rumble,
                             Uint32 duration_ms)
{
    (void)controller;
    s_rumble_calls++;
    s_rumble_low = low_frequency_rumble;
    s_rumble_high = high_frequency_rumble;
    s_rumble_ms = duration_ms;
    return 0;
}

Uint32 SDL_GetTicks(void)
{
    return s_ticks;
}

void SDL_LockJoysticks(void)
{
    if (pthread_mutex_lock(&s_joystick_lock) != 0)
        abort();
    atomic_fetch_add_explicit(&s_lock_balance, 1, memory_order_relaxed);
}

void SDL_UnlockJoysticks(void)
{
    if (atomic_fetch_sub_explicit(&s_lock_balance, 1, memory_order_relaxed) != 1)
        abort();
    if (pthread_mutex_unlock(&s_joystick_lock) != 0)
        abort();
}

int SDL_AtomicGet(SDL_atomic_t *value)
{
    return atomic_load_explicit(&value->value, memory_order_seq_cst);
}

int SDL_AtomicSet(SDL_atomic_t *value, int desired)
{
    return atomic_exchange_explicit(&value->value, desired, memory_order_seq_cst);
}

static void *poll_controller(void *unused)
{
    XBOX_INPUT_STATE state;
    (void)unused;
    for (int i = 0; i < 2000; i++)
        (void)xbox_InputGetState(0, &state);
    return NULL;
}

/* The pumping thread: devices come and go, and with the main pump on it is
 * the one that opens and closes them, as the window loop does on SDL's
 * device events. */
static void *hotplug_controller(void *unused)
{
    (void)unused;
    s_main_thread = pthread_self();
    for (int i = 0; i < 2000; i++) {
        SDL_LockJoysticks();
        if (i & 1) {
            s_device_count = 1;
            s_device_instance++;
        } else {
            s_controller.attached = 0;
            s_device_count = 0;
        }
        SDL_UnlockJoysticks();
        /* A poll reads the pump flag under the joystick lock, so flipping it
         * under the lock means no poll is mid-way through a decision made
         * with the old value when the counting starts. */
        SDL_LockJoysticks();
        xbox_InputSetMainPump(1);
        atomic_store(&s_pump_on, 1);
        SDL_UnlockJoysticks();
        xbox_InputSdlRefreshControllers();
        if (!(i & 2)) {
            SDL_LockJoysticks();
            atomic_store(&s_pump_on, 0);
            xbox_InputSetMainPump(0);
            SDL_UnlockJoysticks();
        }
    }
    atomic_store(&s_pump_on, 0);
    return NULL;
}

int main(void)
{
    XBOX_INPUT_STATE state;
    pthread_t poll_thread, hotplug_thread;

    xbox_InputInit();
    memset(&state, 0, sizeof state);
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(s_open_count == 0);

    s_device_count = 1;
    s_device_instance = 10;
    memset(&state, 0, sizeof state);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(s_open_count == 1);
    CHECK(s_update_count == 2);
    CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(xbox_InputIsConnected(0));

    /* Input focus taken by a host overlay: still connected, A at rest, a new
     * packet; given back, A reads held again. */
    {
        DWORD packet = state.dwPacketNumber;
        xbox_InputSetBlocked(1);
        CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
        CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
        CHECK(state.dwPacketNumber != packet);
        CHECK(xbox_InputIsConnected(0));
        xbox_InputSetBlocked(0);
        CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
        CHECK(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    }

    /* Vibration: as the title asks, then at the player's strength; the
     * overlay's focus stops the motors at once and keeps them still. */
    {
        XBOX_VIBRATION v;
        int calls;
        v.wLeftMotorSpeed = 40000;
        v.wRightMotorSpeed = 20000;
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 40000 && s_rumble_high == 20000);
        xbox_InputSetRumbleScale(0.5f);
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 20000 && s_rumble_high == 10000);
        calls = s_rumble_calls;
        xbox_InputSetBlocked(1);
        CHECK(s_rumble_calls == calls + 1);
        CHECK(s_rumble_low == 0 && s_rumble_high == 0);
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 0 && s_rumble_high == 0);
        xbox_InputSetBlocked(0);
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 20000);
        xbox_InputSetRumbleScale(0.0f);
        CHECK(s_rumble_low == 0 && s_rumble_high == 0);
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 0 && s_rumble_high == 0);
        xbox_InputSetRumbleScale(2.0f);       /* clamped to as-asked */
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 40000 && s_rumble_high == 20000);
    }

    /* An Xbox pad holds its motors until told otherwise, and a title sends
     * only changes (Tony Hawk's Underground: once when a grind starts, once
     * when it ends). So a request is held as long as SDL allows and re-armed
     * by the poll while it should still run; a stop is not; the overlay's
     * focus given back resumes a rumble still held. Each request used to last
     * one second, and a grind's rumble died a second in. */
    {
        XBOX_VIBRATION v;
        int calls;

        v.wLeftMotorSpeed = 30000;
        v.wRightMotorSpeed = 0;
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 30000 && s_rumble_ms >= 60000);
        calls = s_rumble_calls;
        s_ticks += 20000;                             /* 20 s in: still running */
        CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
        CHECK(s_rumble_calls == calls);
        s_ticks += 20000;                             /* 40 s in: re-armed */
        CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
        CHECK(s_rumble_calls == calls + 1);
        CHECK(s_rumble_low == 30000 && s_rumble_ms >= 60000);
        xbox_InputSetBlocked(1);
        CHECK(s_rumble_low == 0);
        xbox_InputSetBlocked(0);
        CHECK(s_rumble_low == 30000 && s_rumble_ms >= 60000);
        v.wLeftMotorSpeed = 0;
        CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS);
        CHECK(s_rumble_low == 0 && s_rumble_high == 0 && s_rumble_ms == 0);
        calls = s_rumble_calls;
        s_ticks += 60000;                             /* a stop stays stopped */
        CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
        CHECK(s_rumble_calls == calls);
    }

    s_controller.attached = 0;
    s_device_count = 0;
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(s_close_count == 1);
    CHECK(!xbox_InputIsConnected(0));

    s_device_count = 1;
    s_device_instance = 11;
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(s_open_count == 2);

    /* The main pump: polls only read the pads already open. A pad pulled
     * out reads as disconnected but stays for the pumping thread to close;
     * a pad plugged in is not opened by a poll, only by that thread's
     * refresh (a hotplug open from a title thread corrupted SDL3's heap). */
    xbox_InputSetMainPump(1);
    int updates_before = s_update_count, enum_before = s_enum_count;
    s_controller.attached = 0;
    s_device_count = 0;
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(!xbox_InputIsConnected(0));
    CHECK(s_update_count == updates_before);
    CHECK(s_close_count == 1);
    CHECK(s_enum_count == enum_before);
    CHECK(!xbox_InputAnyOpen());
    CHECK(s_close_count == 1);
    xbox_InputSdlRefreshControllers();
    CHECK(s_close_count == 2);

    s_device_count = 1;
    s_device_instance = 12;
    enum_before = s_enum_count;
    CHECK(xbox_InputGetState(0, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xbox_InputGetState(1, &state) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(s_open_count == 2);
    CHECK(s_enum_count == enum_before);
    xbox_InputSdlRefreshControllers();
    CHECK(s_open_count == 3);
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(s_update_count == updates_before);
    CHECK(xbox_InputAnyOpen());

    CHECK(pthread_create(&hotplug_thread, NULL, hotplug_controller, NULL) == 0);
    CHECK(pthread_create(&poll_thread, NULL, poll_controller, NULL) == 0);
    CHECK(pthread_join(poll_thread, NULL) == 0);
    CHECK(pthread_join(hotplug_thread, NULL) == 0);
    CHECK(atomic_load(&s_enum_off_main) == 0);

    SDL_LockJoysticks();
    s_device_count = 1;
    s_device_instance++;
    s_controller.attached = 0;
    SDL_UnlockJoysticks();
    xbox_InputSetMainPump(1);
    xbox_InputSdlRefreshControllers();
    CHECK(xbox_InputGetState(0, &state) == ERROR_SUCCESS);
    CHECK(atomic_load_explicit(&s_lock_balance, memory_order_relaxed) == 0);

    if (!s_fail)
        puts("SDL controller hotplug: ok");
    return s_fail;
}
