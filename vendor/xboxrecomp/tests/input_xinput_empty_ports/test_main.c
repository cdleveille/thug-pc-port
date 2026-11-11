/*
 * input_xinput_empty_ports test: see CMakeLists.txt.
 *
 * XInputGetState on a port with no pad re-enumerates the devices (about a
 * millisecond a call), and a title's USB poll asks for every port each frame
 * or more often. The backend asks about a port it found empty again only
 * after a second. Checked here with a fake XInput that counts its calls:
 *   - an empty port is asked about once in a burst of reads;
 *   - a connected pad is read on every call (no added latency);
 *   - a pad plugged into an empty port is found once the second is up;
 *   - a pad pulled out reads as disconnected at once;
 *   - xbox_InputSetMainPump and xbox_InputSdlRefreshControllers exist on
 *     Windows (an SDL host loop calls them) and change nothing.
 * With the old rule (XInputGetState on every call) the first check fails.
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>

#include "xinput_xbox.h"

/* keyboard_sdl.h's, which needs SDL; the XInput backend defines it. */
void xbox_InputSdlRefreshControllers(void);

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── Fake XInput ──────────────────────────────────────────────────────── */

static int   g_plugged[XBOX_MAX_CONTROLLERS];
static int   g_get_calls[XBOX_MAX_CONTROLLERS];
static DWORD g_packet;

DWORD WINAPI XInputGetState(DWORD port, XINPUT_STATE *st)
{
    if (port >= XBOX_MAX_CONTROLLERS)
        return ERROR_BAD_ARGUMENTS;
    g_get_calls[port]++;
    if (!g_plugged[port])
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(st, 0, sizeof *st);
    st->dwPacketNumber = ++g_packet;
    st->Gamepad.wButtons = XINPUT_GAMEPAD_A;
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(DWORD port, XINPUT_VIBRATION *v)
{
    (void)v;
    return port < XBOX_MAX_CONTROLLERS && g_plugged[port] ? ERROR_SUCCESS
                                                           : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD port, DWORD flags, XINPUT_CAPABILITIES *c)
{
    (void)flags;
    if (port >= XBOX_MAX_CONTROLLERS || !g_plugged[port])
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(c, 0, sizeof *c);
    c->Type = XINPUT_DEVTYPE_GAMEPAD;
    c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static int read_port(DWORD port)
{
    XBOX_INPUT_STATE st;
    return xbox_InputGetState(port, &st) == ERROR_SUCCESS;
}

static void reset_calls(void)
{
    memset(g_get_calls, 0, sizeof g_get_calls);
}

int main(void)
{
    int i, ok;

    /* The SDL host loop's calls: present on Windows, and inert. */
    xbox_InputSetMainPump(1);
    xbox_InputSdlRefreshControllers();

    g_plugged[0] = 1;                   /* a pad on port 1, none on 2..4 */
    xbox_InputInit();
    CHECK(xbox_InputIsConnected(0), "port 1 connected after init");
    CHECK(!xbox_InputIsConnected(1), "port 2 empty after init");

    /* A burst of reads, well inside a second: the empty port is not asked
     * again, the connected one every time. */
    reset_calls();
    for (i = 0, ok = 0; i < 200; i++)
        ok += read_port(1);
    CHECK(ok == 0, "empty port read as connected %d times", ok);
    CHECK(g_get_calls[1] == 0, "empty port asked %d times in 200 reads (want 0: "
          "init asked less than a second ago)", g_get_calls[1]);
    for (i = 0, ok = 0; i < 200; i++)
        ok += read_port(0);
    CHECK(ok == 200, "connected pad read %d of 200 times", ok);
    CHECK(g_get_calls[0] == 200, "connected pad asked %d times (want 200)", g_get_calls[0]);

    /* Plug a pad into port 2: not seen before the second is up, then found,
     * then read on every call. */
    g_plugged[1] = 1;
    reset_calls();
    CHECK(!read_port(1), "a pad just plugged in is found only after the retry interval");
    Sleep(1100);
    CHECK(read_port(1), "the pad on port 2 is found after the retry interval");
    CHECK(g_get_calls[1] == 1, "asked %d times to find it (want 1)", g_get_calls[1]);
    for (i = 0, ok = 0; i < 50; i++)
        ok += read_port(1);
    CHECK(ok == 50 && g_get_calls[1] == 51,
          "found pad read %d of 50 times, asked %d times", ok, g_get_calls[1]);
    CHECK(xbox_InputIsConnected(1), "port 2 reported connected");

    /* Pull it out: disconnected at the next read, then left alone. */
    g_plugged[1] = 0;
    reset_calls();
    CHECK(!read_port(1), "a pad pulled out reads as disconnected at once");
    CHECK(!xbox_InputIsConnected(1), "port 2 reported disconnected");
    for (i = 0; i < 100; i++)
        read_port(1);
    CHECK(g_get_calls[1] == 1, "port emptied: asked %d times in 101 reads (want 1)",
          g_get_calls[1]);

    /* The connected pad's buttons come through. */
    {
        XBOX_INPUT_STATE st;
        memset(&st, 0, sizeof st);
        CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
              && st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255,
              "A on the connected pad (analog %u)", st.Gamepad.bAnalogButtons[XBOX_BUTTON_A]);
    }

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
