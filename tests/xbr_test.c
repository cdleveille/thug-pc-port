/* xbr_test.c -- the xBR magnifier behind "Smooth text and icons"
 * (src/gpu/xbr.c), on small synthetic images:
 *
 *   - a flat image, and straight horizontal and vertical edges, come out as
 *     plain 2x pixel doubling: xBR only redraws diagonal edges;
 *   - a 45-degree staircase that exists only in alpha (an opaque black shape
 *     on transparent black, as the game's font outlines are) is found and
 *     blended along the edge, at least halfway at the corners, while texels
 *     away from the edge are untouched;
 *   - the 4x path doubles twice, and bad arguments are refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbr.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

#define W 8
#define H 8

static void put(uint8_t *img, int w, int x, int y, uint32_t rgba)
{
    uint8_t *p = img + 4 * (y * w + x);
    p[0] = (uint8_t)(rgba >> 24);
    p[1] = (uint8_t)(rgba >> 16);
    p[2] = (uint8_t)(rgba >> 8);
    p[3] = (uint8_t)rgba;
}

/* Is out exactly in doubled pixel by pixel? */
static int is_doubled(const uint8_t *in, const uint8_t *out, int w, int h)
{
    int x, y;
    for (y = 0; y < 2 * h; y++)
        for (x = 0; x < 2 * w; x++)
            if (memcmp(out + 4 * (y * 2 * w + x), in + 4 * ((y / 2) * w + x / 2), 4))
                return 0;
    return 1;
}

int main(void)
{
    uint8_t in[W * H * 4], out[W * H * 16];
    int x, y;

    /* Flat. */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            put(in, W, x, y, 0x4080C0FFu);
    CHECK(thug_xbr2x(in, W, H, out));
    CHECK(is_doubled(in, out, W, H));

    /* Straight edges: white above transparent, then white left of black. */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            put(in, W, x, y, y < H / 2 ? 0xFFFFFFFFu : 0x00000000u);
    CHECK(thug_xbr2x(in, W, H, out));
    CHECK(is_doubled(in, out, W, H));
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            put(in, W, x, y, x < W / 2 ? 0xFFFFFFFFu : 0x000000FFu);
    CHECK(thug_xbr2x(in, W, H, out));
    CHECK(is_doubled(in, out, W, H));

    /* A 45-degree staircase in alpha only: opaque black where x + y <= 7. */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            put(in, W, x, y, x + y <= 7 ? 0x000000FFu : 0x00000000u);
    CHECK(thug_xbr2x(in, W, H, out));
    {
        int partial = 0, strongest = 0, X, Y;
        for (Y = 0; Y < 2 * H; Y++) {
            for (X = 0; X < 2 * W; X++) {
                const uint8_t *p = out + 4 * (Y * 2 * W + X);
                int sx = X / 2, sy = Y / 2, d = sx + sy - 7;
                CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0);     /* colour untouched */
                if (d <= -2)
                    CHECK(p[3] == 255);                        /* inside, away from the edge */
                else if (d >= 3)
                    CHECK(p[3] == 0);                          /* outside, away from it */
                if (p[3] > 0 && p[3] < 255) {
                    partial++;
                    if (d == 0 || d == 1) {                    /* the edge texels */
                        int moved = d == 0 ? 255 - p[3] : p[3];
                        if (moved > strongest)
                            strongest = moved;
                    }
                }
            }
        }
        CHECK(partial >= 8);           /* the edge is redrawn, not doubled */
        CHECK(strongest >= 120);       /* found as an edge (alpha counts), not a 1/4 nudge */
    }

    /* A shallow staircase (slope 1/2: opaque where x + 2y <= 14) and the same
     * transposed (steep). xBR draws a shallow edge as a ramp along each row --
     * the corner's subpixel and the one beside it, 3/4 and 1/4 -- so no
     * column holds two partial values, and a row's partials fall left to
     * right. Blending the subpixel above instead (the wrong neighbour) leaves
     * 2x2 blocks: two partials stacked in a column, equal ones side by side. */
    {
        enum { S = 12 };
        static uint8_t sin[S * S * 4], sout[S * S * 16];
        int t, X, Y;

        for (t = 0; t < 2; t++) {
            int edge_rows = 0;
            for (y = 0; y < S; y++)
                for (x = 0; x < S; x++) {
                    int on = t ? (y + 2 * x <= 14) : (x + 2 * y <= 14);
                    put(sin, S, x, y, on ? 0x000000FFu : 0x00000000u);
                }
            CHECK(thug_xbr2x(sin, S, S, sout));
            /* Along the edge's own direction: rows for shallow, columns for
             * steep (read through the transpose). */
            for (Y = 0; Y < 2 * S; Y++) {
                int last = 256, partials = 0;
                for (X = 0; X < 2 * S; X++) {
                    int a = t ? sout[4 * (X * 2 * S + Y) + 3] : sout[4 * (Y * 2 * S + X) + 3];
                    if (a > 0 && a < 255) {
                        CHECK(a < last);
                        last = a;
                        partials++;
                    }
                }
                edge_rows += partials > 0;
            }
            CHECK(edge_rows >= 8);
            /* Across it: at most one partial value. */
            for (X = 0; X < 2 * S; X++) {
                int partials = 0;
                for (Y = 0; Y < 2 * S; Y++) {
                    int a = t ? sout[4 * (X * 2 * S + Y) + 3] : sout[4 * (Y * 2 * S + X) + 3];
                    partials += a > 0 && a < 255;
                }
                CHECK(partials <= 1);
            }
        }
    }

    /* 4x, and refusals. */
    {
        int ow = 0, oh = 0;
        uint8_t *big = thug_xbr_scale(in, W, H, 4, &ow, &oh);
        CHECK(big && ow == 4 * W && oh == 4 * H);
        free(big);
        CHECK(thug_xbr_scale(in, W, H, 3, &ow, &oh) == NULL);
        CHECK(!thug_xbr2x(NULL, W, H, out));
        CHECK(!thug_xbr2x(in, 0, H, out));
    }

    if (!s_fail)
        printf("xBR: ok\n");
    return s_fail;
}
