/**
 * controls.c -- the keyboard's bindings. See controls.h.
 */
#include "controls.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "input/input_map.h"   /* the toolkit's bindings (vendor/xboxrecomp/src) */
#include "input/keyboard.h"

/* Where the game keeps the state Escape follows (USA retail only, AGENTS.md
 * rule 4; docs/NOTES.md, "Title-side facts"). Both are what its own script
 * functions read:
 *   LevelIs (sub_0011E870)       [THUG_SKATE_MODULE] + 0x398, the current
 *                                level's checksum
 *   GameIsPaused (sub_0011D430)  [THUG_FRONTEND_MODULE] + 0x40, non-zero while
 *                                the game is paused (PauseGame)
 * The main menu and its screens are a level of their own, load_skateshop. */
#define THUG_SKATE_MODULE     0x002FCD68u
#define THUG_SKATE_LEVEL      0x398u
#define THUG_FRONTEND_MODULE  0x002FCD7Cu
#define THUG_FRONTEND_PAUSED  0x40u
#define THUG_LEVEL_SKATESHOP  0x9F2BAFB7u   /* checksum of "load_skateshop" */

enum { LAYER_PLAY, LAYER_MENU };

/* g_xbox_mem_offset, once the guest's memory is mapped; until then Escape is
 * START. */
static volatile ptrdiff_t s_mem;
static volatile int       s_mapped;

static void bind(InputBindings *b, int control, const char *keys)
{
    if (xbox_InputMap_BindingsParse(b, control, keys) != 0)
        fprintf(stderr, "[thug] keyboard: could not bind \"%s\"\n", keys);
}

void thug_keyboard_controls_install(void)
{
    /* Key names as input_map.h reads them: logical keys (the key labelled
     * W), as the toolkit's keyboard layer records them. WASD is the left
     * stick (steering, as on a pad), the arrows the d-pad. */
    static const struct {
        int         control;
        const char *keys;
    } k_bindings[] = {
        { INPUT_A,            "enter, space" },
        { INPUT_LSTICK_UP,    "w" },
        { INPUT_LSTICK_DOWN,  "s" },
        { INPUT_LSTICK_LEFT,  "a" },
        { INPUT_LSTICK_RIGHT, "d" },
        { INPUT_DPAD_UP,      "up" },
        { INPUT_DPAD_DOWN,    "down" },
        { INPUT_DPAD_LEFT,    "left" },
        { INPUT_DPAD_RIGHT,   "right" },
    };
    InputBindings play = { 0 }, menu;
    size_t i;

    for (i = 0; i < sizeof k_bindings / sizeof k_bindings[0]; i++)
        bind(&play, k_bindings[i].control, k_bindings[i].keys);
    /* Two keys at once (W+A) push the stick to the edge of its circle, as a
     * real stick goes, not full on both axes: no faster on a diagonal. */
    play.diagonal = INPUT_DIAGONAL_ROUND;
    menu = play;
    bind(&play, INPUT_B, "backspace, tab");
    bind(&play, INPUT_START, "escape");
    bind(&menu, INPUT_B, "backspace, tab, escape");

    xbox_InputKeyboardSetBindings(&play);
    xbox_InputKeyboardSetLayer(LAYER_MENU, &menu);
}

void thug_keyboard_controls_guest_mapped(ptrdiff_t mem_offset)
{
    s_mem = mem_offset;
    s_mapped = 1;
}

/* A guest word, read from this thread while the game runs: 0 for an address
 * outside RAM (64 MB) and the contiguous window (0x80000000, 64 MB), where the
 * game's modules live. The pointers followed are the game's own, so a torn or
 * stale read only gives a wrong answer for one pass, never a fault. */
static uint32_t guest32(uint32_t va)
{
    const volatile uint8_t *p;

    if (!((va >= 0x00010000u && va <= 0x04000000u - 4)
          || (va >= 0x80000000u && va <= 0x84000000u - 4)))
        return 0;
    p = (const volatile uint8_t *)((uintptr_t)s_mem + va);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

/* In the front end, or paused in a level: the menus, where Escape goes back. */
static int in_menus(void)
{
    uint32_t skate = guest32(THUG_SKATE_MODULE);
    uint32_t fe = guest32(THUG_FRONTEND_MODULE);

    if (skate && guest32(skate + THUG_SKATE_LEVEL) == THUG_LEVEL_SKATESHOP)
        return 1;
    return fe && (guest32(fe + THUG_FRONTEND_PAUSED) & 0xFFu) != 0;
}

void thug_keyboard_controls_update(void)
{
    int want;

    if (!s_mapped)
        return;
    want = in_menus() ? LAYER_MENU : LAYER_PLAY;
    /* Not while Escape is held: the press that pauses would otherwise turn
     * into a B in the pause menu, which resumes at once. The switch waits
     * for the key to come up. */
    if (want == xbox_InputKeyboardLayer() || xbox_FramebufferKeyDown(XBOX_VK_ESCAPE))
        return;
    xbox_InputKeyboardUseLayer(want);
    fprintf(stderr, "[thug] keyboard: Escape is %s\n", want == LAYER_MENU ? "B" : "START");
}
