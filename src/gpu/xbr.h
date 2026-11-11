/**
 * xbr.h -- xBR, an edge-directed magnifier for low-resolution game art.
 *
 * The renderer runs it over the game's text, icon and logo textures when the
 * player turns on "Smooth text and icons" (nv2a_pb_gl.c): the art is 640x480
 * era, and plain bilinear magnification turns its outlines into soft
 * staircases, where xBR follows the edge and draws it smooth.
 *
 * No GL, no state: testable on its own (tests/xbr_test.c).
 */
#ifndef THUG_XBR_H
#define THUG_XBR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magnify an RGBA image (4 bytes a texel, rows packed, w x h) to 2w x 2h in
 * dst. Returns 0 on bad arguments or no memory, 1 otherwise. */
int thug_xbr2x(const uint8_t *src, int w, int h, uint8_t *dst);

/* Magnify by 2 or 4 (xBR 2x, once or twice) into a new buffer the caller
 * frees; NULL on failure. *ow, *oh get the new size. */
uint8_t *thug_xbr_scale(const uint8_t *src, int w, int h, int factor,
                        int *ow, int *oh);

#ifdef __cplusplus
}
#endif

#endif /* THUG_XBR_H */
