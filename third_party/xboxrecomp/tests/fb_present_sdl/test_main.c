/*
 * fb_present_sdl - does the POSIX framebuffer window publish what the
 * pushbuffer executor hands it at a flip, for every surface shape it can be
 * handed: 32-bit, 16-bit, and the 2x-wide anti-aliased 32-bit surface (pitch
 * 640 * 8), at vertical factor 1 (2x1) and 2 (2x2).
 *
 * The 2x-wide case is the one that matters: the presenter used to reject a
 * pitch / 640 of 8, so a title rendering multisampled -- Tony Hawk's
 * Underground, 1280x480 -- never published a frame and the window stayed
 * black while the executor drew correctly. fb_present.c (GDI) already took
 * every other pixel; this checks the SDL twin does the same.
 *
 * Exercises present_copy() through xbox_FramebufferWindowPresent without
 * opening a window: s_running is set by hand and the published slot is read
 * back directly. No display is needed.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define xbox_GetMemoryOffset present_test_memory_offset
#include "../../src/video/fb_present_sdl.c"
#undef xbox_GetMemoryOffset

#define FRAME_VA     0x1000u
#define FRAME_PIXELS (640u * 480u)

static uint8_t *guest;
static int failures;

ptrdiff_t present_test_memory_offset(void) { return (ptrdiff_t)guest; }

/* The input layer the window loop drives; never reached here. */
void xbox_InputSdlInit(void) {}
void xbox_InputSdlEvent(const SDL_Event *ev) { (void)ev; }
void xbox_InputSetMainPump(int on) { (void)on; }
int  xbox_InputAnyOpen(void) { return 0; }

#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* The frame most recently published: the ready slot. */
static const struct fb_slot *ready(void) { return &s_slot[s_ready]; }

static int ready_is32(uint32_t value)
{
    const uint32_t *p = (const uint32_t *)ready()->px;
    size_t i;
    if (!p || ready()->bpp != 4)
        return 0;
    for (i = 0; i < FRAME_PIXELS; i++)
        if (p[i] != value)
            return 0;
    return 1;
}

int main(void)
{
    uint32_t *g32;
    uint16_t *g16;
    uint64_t seq;
    size_t i;

    /* Room for the largest surface: 2x2 AA, 1280x960 at 4 bytes. */
    guest = calloc(1, FRAME_VA + 1280u * 960u * 4u);
    if (!guest)
        return 1;
    g32 = (uint32_t *)(guest + FRAME_VA);
    g16 = (uint16_t *)(guest + FRAME_VA);
    s_running = 1;

    /* 1. 32-bit, 640 wide. */
    for (i = 0; i < FRAME_PIXELS; i++)
        g32[i] = 0x00112233u;
    seq = s_ready_seq;
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    CHECK("32-bit frame published", s_ready_seq == seq + 1);
    CHECK("32-bit frame content", ready_is32(0x00112233u));

    /* 2. 16-bit: published at 2 bytes per pixel, unconverted (SDL takes
     *    RGB565 directly). */
    for (i = 0; i < FRAME_PIXELS; i++)
        g16[i] = 0xF800u;
    seq = s_ready_seq;
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 2u);
    CHECK("16-bit frame published", s_ready_seq == seq + 1);
    CHECK("16-bit frame is 2 bpp", ready()->bpp == 2
          && ((const uint16_t *)ready()->px)[0] == 0xF800u
          && ((const uint16_t *)ready()->px)[FRAME_PIXELS - 1] == 0xF800u);

    /* 3. 2x1 anti-aliased, 1280x480: even columns carry the picture. */
    for (i = 0; i < 1280u * 480u; i++)
        g32[i] = (i % 2u == 0) ? 0x00010203u : 0x00AABBCCu;
    seq = s_ready_seq;
    xbox_FramebufferWindowSetAA(1);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 8u);
    CHECK("2x1 AA frame published (was dropped: pitch/640 == 8)",
          s_ready_seq == seq + 1);
    CHECK("2x1 AA frame takes every other pixel", ready_is32(0x00010203u));

    /* 4. 2x2 anti-aliased, 1280x960: even rows of even columns. */
    for (i = 0; i < 1280u * 960u; i++)
        g32[i] = ((i / 1280u) % 2u == 0 && i % 2u == 0)
                     ? 0x00040506u : 0x00DDEEFFu;
    seq = s_ready_seq;
    xbox_FramebufferWindowSetAA(2);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 8u);
    xbox_FramebufferWindowSetAA(1);
    CHECK("2x2 AA frame published", s_ready_seq == seq + 1);
    CHECK("2x2 AA frame steps rows by 2", ready_is32(0x00040506u));

    /* 5. A pitch that is none of the shapes is still refused, not guessed. */
    seq = s_ready_seq;
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 3u);
    CHECK("unknown pitch not published", s_ready_seq == seq);

    /* 6. No window: nothing is published. */
    s_running = 0;
    seq = s_ready_seq;
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    CHECK("closed window publishes nothing", s_ready_seq == seq);

    free(guest);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("fb_present_sdl: all checks passed\n");
    return 0;
}
