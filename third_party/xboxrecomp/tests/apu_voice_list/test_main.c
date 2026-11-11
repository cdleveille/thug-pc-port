/* The voice processor's list walk, against a small fake guest RAM.
 *
 * apu_vp.c is compiled into this file (as tests/apu_vp does) so a frame can
 * be run on a hand-built voice list. The check fails on the code it was
 * written for: mcpx_apu_vp_frame bounded the *number* of voices it walked
 * but not the handle, and voice_process's `assert(v < MCPX_HW_MAX_VOICES)` is
 * compiled out in a release build. A list naming handle 0x0400 then indexed
 * g_dbg.vp.v[], d->vp.voice_spin[] and the resampler state 1024 voices into
 * 256-entry tables -- Tony Hawk's Underground segfaulted the VP thread that
 * way while reloading sound for its main menu.
 *
 * Built with AddressSanitizer, so the out-of-bounds write is a hard failure
 * as well as the observable checks below. POSIX: tests/apu_vp covers the
 * same file on Windows. */
#include "../../src/apu/apu_vp.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Symbols the VP takes from the rest of the APU (as in tests/apu_vp). */
MCPXAPUState *g_state;
struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
uint8_t *g_apu_ram_ptr;

#define RAM_SIZE   0x100000u
#define VOICE_BASE 0x10000u   /* NV_PAPU_VPVADDR */

static uint8_t *s_ram;
static int s_failures;

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    if (addr >= RAM_SIZE) {
        fprintf(stderr, "FAIL: guest access at 0x%llX is outside the fake RAM\n",
                (unsigned long long)addr);
        exit(1);
    }
    return s_ram + addr;
}

const char *mcpx_apu_phys_class(uint64_t addr) { (void)addr; return "fake"; }
int apu_env_on(const char *name) { (void)name; return 0; }
ApuIrqStats apu_irq_stats;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

static void set_voice_reg(uint16_t v, uint32_t reg, uint32_t value)
{
    memcpy(s_ram + VOICE_BASE + (uint32_t)v * NV_PAVS_SIZE + reg, &value, 4);
}

static uint32_t get_voice_reg(uint16_t v, uint32_t reg)
{
    uint32_t value;
    memcpy(&value, s_ram + VOICE_BASE + (uint32_t)v * NV_PAVS_SIZE + reg, 4);
    return value;
}

/* A voice that would be processed if reached: active, paused (so
 * voice_process stops right after touching g_dbg), linked to `next`. */
static void make_voice(uint16_t v, uint16_t next)
{
    set_voice_reg(v, NV_PAVS_VOICE_PAR_STATE,
                  NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE
                  | NV_PAVS_VOICE_PAR_STATE_PAUSED);
    set_voice_reg(v, NV_PAVS_VOICE_TAR_PITCH_LINK, next);
}

static void reset(MCPXAPUState *d)
{
    memset(s_ram, 0, RAM_SIZE);
    memset(d, 0, sizeof(*d));
    memset(&g_dbg, 0, sizeof(g_dbg));
    d->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    /* Every list empty unless a case says otherwise. */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
}

/* A list whose head is out of range: nothing is walked or processed. */
static void bad_head_ends_the_list(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    const uint16_t bad = 0x0400;
    uint32_t before;

    reset(&d);
    make_voice(bad, 0xFFFF);          /* inside the fake RAM, outside 0..255 */
    before = get_voice_reg(bad, NV_PAVS_VOICE_PAR_STATE);
    d.regs[NV_PAPU_TVL2D] = bad;

    mcpx_apu_vp_frame(&d, mixbins);

    CHECK(get_voice_reg(bad, NV_PAVS_VOICE_PAR_STATE) == before,
          "voice 0x%04X was processed", bad);
    CHECK(d.vp.voice_spin[MCPX_HW_MAX_VOICES - 1] == 0,
          "voice lock table touched");
}

/* A valid voice whose link is out of range: the valid one is still walked
 * (and recorded in g_dbg), the list stops at the bad link. */
static void bad_link_ends_the_list(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

    reset(&d);
    make_voice(7, 0x0300);
    make_voice(0x0300, 0xFFFF);
    d.regs[NV_PAPU_TVL2D] = 7;

    mcpx_apu_vp_frame(&d, mixbins);

    CHECK(g_dbg.vp.v[7].active, "valid voice 7 was not walked");
    CHECK(d.regs[NV_PAPU_CVL2D] == 0x0300,
          "walk did not stop at the bad link (current 0x%X)",
          (unsigned)d.regs[NV_PAPU_CVL2D]);
}

/* The terminator alone is still an empty list, and a normal two-voice list
 * walks both. */
static void good_lists_still_walk(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

    reset(&d);
    make_voice(3, 200);
    make_voice(200, 0xFFFF);
    d.regs[NV_PAPU_TVL3D] = 3;

    mcpx_apu_vp_frame(&d, mixbins);

    CHECK(g_dbg.vp.v[3].active && g_dbg.vp.v[200].active,
          "a valid two-voice list was not walked to its end");
    CHECK(d.regs[NV_PAPU_CVL3D] == 0xFFFF, "list did not end at 0xFFFF");
}

int main(void)
{
    s_ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!s_ram) return 2;

    good_lists_still_walk();
    bad_head_ends_the_list();
    bad_link_ends_the_list();

    free(s_ram);
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: voice lists end at an out-of-range handle; valid lists walk.");
    return 0;
}
