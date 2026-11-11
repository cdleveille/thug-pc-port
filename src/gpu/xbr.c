/**
 * xbr.c -- xBR 2x on RGBA images. See xbr.h.
 *
 * The algorithm is Hyllian's xBR ("scale by rules"), level 2, as he described
 * it: for each corner of each source texel, compare the colour distances
 * along the two diagonals of the 4x4 neighbourhood around that corner. If the
 * edge runs along the corner's diagonal, the corner's subpixel is blended
 * toward the neighbour across the edge; a shallow or steep edge (the
 * neighbouring corner's distances say so) also pulls the subpixel beside it.
 * Written here from that description, for this port.
 *
 * Distances weigh luma most, then chroma, as xBR does, plus alpha as heavily
 * as luma: the game's fonts and icons have their outlines in alpha (black
 * glyph outlines on transparent black), which a colour-only distance cannot
 * see.
 *
 * Neighbourhood of a texel E, for the bottom-right corner; the other three
 * corners are the same rules with the neighbourhood rotated:
 *
 *            B1
 *       A  B  C
 *       D  E  F  F4
 *       G  H  I  I4
 *          H5 I5
 */
#include "xbr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float y, u, v, a;
} Yuva;

static Yuva to_yuva(const uint8_t *p)
{
    float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
    Yuva o;
    o.y = 0.299f * r + 0.587f * g + 0.114f * b;
    o.u = -0.169f * r - 0.331f * g + 0.5f * b;
    o.v = 0.5f * r - 0.419f * g - 0.081f * b;
    o.a = p[3] / 255.0f;
    return o;
}

static float df(const Yuva *p, const Yuva *q)
{
    return 48.0f * fabsf(p->y - q->y) + 7.0f * fabsf(p->u - q->u)
         + 6.0f * fabsf(p->v - q->v) + 48.0f * fabsf(p->a - q->a);
}

/* "Alike": the threshold xBR uses with luma weighted by 48. */
static int eq(const Yuva *p, const Yuva *q) { return df(p, q) < 15.0f; }

static int same(const uint8_t *p, const uint8_t *q) { return memcmp(p, q, 4) == 0; }

/* dst moves toward src by w/256 in every channel. */
static void blend(uint8_t *dst, const uint8_t *src, int w)
{
    int c;
    for (c = 0; c < 4; c++) {
        int d = dst[c], s = src[c];
        dst[c] = (uint8_t)(d + ((s - d) * w + (s >= d ? 128 : -128)) / 256);
    }
}

/* Neighbour offsets in the bottom-right frame, and the subpixels a corner
 * touches: N3 the corner itself, N2 beside it along the bottom, N1 above it
 * along the right. Rotation r maps that frame onto the other corners in xBR's
 * order: bottom-right, top-right, top-left, bottom-left. */
enum { E_, I_, H_, F_, G_, C_, D_, B_, F4, I4, H5, I5, NB };
static const signed char k_off[NB][2] = {
    { 0, 0 }, { 1, 1 }, { 0, 1 }, { 1, 0 }, { -1, 1 }, { 1, -1 }, { -1, 0 },
    { 0, -1 }, { 2, 0 }, { 2, 1 }, { 0, 2 }, { 1, 2 },
};
static const signed char k_sub[3][2] = { { 1, -1 }, { -1, 1 }, { 1, 1 } };  /* N1 N2 N3 */

static void rotate(int r, int dx, int dy, int *ox, int *oy)
{
    switch (r) {
    case 0:  *ox = dx;  *oy = dy;  break;
    case 1:  *ox = dy;  *oy = -dx; break;
    case 2:  *ox = -dx; *oy = -dy; break;
    default: *ox = -dy; *oy = dx;  break;
    }
}

/* Subpixel index in the 2x2 block: 0 top-left, 1 top-right, 2 bottom-left,
 * 3 bottom-right. */
static int sub_index(int r, const signed char d[2])
{
    int x, y;
    rotate(r, d[0], d[1], &x, &y);
    return (y > 0 ? 2 : 0) + (x > 0 ? 1 : 0);
}

/* The image with a 2-texel border repeating its edge texels, so every
 * neighbour is a fixed offset from the texel, with no clamping per lookup. */
#define PAD 2

int thug_xbr2x(const uint8_t *src, int w, int h, uint8_t *dst)
{
    Yuva *yv;
    uint8_t *pp;
    int x, y, r, k, ow = 2 * w, pw = w + 2 * PAD, ph = h + 2 * PAD;
    int sub[4][3], delta[4][NB];

    if (!src || !dst || w <= 0 || h <= 0)
        return 0;
    yv = (Yuva *)malloc((size_t)pw * ph * sizeof *yv);
    pp = (uint8_t *)malloc((size_t)pw * ph * 4);
    if (!yv || !pp) {
        free(yv);
        free(pp);
        return 0;
    }
    for (y = 0; y < ph; y++) {
        int sy = y - PAD < 0 ? 0 : y - PAD >= h ? h - 1 : y - PAD;
        for (x = 0; x < pw; x++) {
            int sx = x - PAD < 0 ? 0 : x - PAD >= w ? w - 1 : x - PAD;
            const uint8_t *s = src + 4 * ((size_t)sy * w + sx);
            memcpy(pp + 4 * ((size_t)y * pw + x), s, 4);
            yv[(size_t)y * pw + x] = to_yuva(s);
        }
    }
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 3; k++)
            sub[r][k] = sub_index(r, k_sub[k]);
        for (k = 0; k < NB; k++) {
            int dx, dy;
            rotate(r, k_off[k][0], k_off[k][1], &dx, &dy);
            delta[r][k] = dy * pw + dx;
        }
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t out[4][4];
            int base = (y + PAD) * pw + (x + PAD);
            const uint8_t *pe = pp + 4 * (size_t)base;

            for (k = 0; k < 4; k++)
                memcpy(out[k], pe, 4);
            for (r = 0; r < 4; r++) {
                const Yuva *Y[NB];
                const uint8_t *P[NB];
                float de, di;

                /* A texel matching the neighbour below or beside it is no
                 * corner of an edge: the common case, decided first. */
                if (same(pe, pp + 4 * (size_t)(base + delta[r][H_]))
                        || same(pe, pp + 4 * (size_t)(base + delta[r][F_])))
                    continue;
                for (k = 0; k < NB; k++) {
                    Y[k] = &yv[base + delta[r][k]];
                    P[k] = pp + 4 * (size_t)(base + delta[r][k]);
                }
                /* Distances across the corner's diagonal (H-F) and along it. */
                de = df(Y[E_], Y[C_]) + df(Y[E_], Y[G_]) + df(Y[I_], Y[H5])
                   + df(Y[I_], Y[F4]) + 4.0f * df(Y[H_], Y[F_]);
                di = df(Y[H_], Y[D_]) + df(Y[H_], Y[I5]) + df(Y[F_], Y[I4])
                   + df(Y[F_], Y[B_]) + 4.0f * df(Y[E_], Y[I_]);
                if (de < di
                    && ((!eq(Y[F_], Y[B_]) && !eq(Y[H_], Y[D_]))
                        || (eq(Y[E_], Y[I_]) && !eq(Y[F_], Y[I4]) && !eq(Y[H_], Y[I5]))
                        || eq(Y[E_], Y[G_]) || eq(Y[E_], Y[C_]))) {
                    float ke = df(Y[F_], Y[G_]), ki = df(Y[H_], Y[C_]);
                    int ex2 = !same(P[E_], P[C_]) && !same(P[B_], P[C_]);
                    int ex3 = !same(P[E_], P[G_]) && !same(P[D_], P[G_]);
                    const uint8_t *px = df(Y[E_], Y[F_]) <= df(Y[E_], Y[H_]) ? P[F_] : P[H_];
                    uint8_t *n1 = out[sub[r][0]], *n2 = out[sub[r][1]], *n3 = out[sub[r][2]];
                    int shallow = 2.0f * ke <= ki && ex3, steep = ke >= 2.0f * ki && ex2;

                    if (shallow && steep) {
                        blend(n3, px, 224);
                        blend(n2, px, 64);
                        memcpy(n1, n2, 4);
                    } else if (shallow) {
                        blend(n3, px, 192);
                        blend(n2, px, 64);
                    } else if (steep) {
                        blend(n3, px, 192);
                        blend(n1, px, 64);
                    } else {
                        blend(n3, px, 128);
                    }
                } else if (de <= di) {
                    blend(out[sub[r][2]],
                          df(Y[E_], Y[F_]) <= df(Y[E_], Y[H_]) ? P[F_] : P[H_], 64);
                }
            }
            memcpy(dst + 4 * ((size_t)(2 * y) * ow + 2 * x), out[0], 4);
            memcpy(dst + 4 * ((size_t)(2 * y) * ow + 2 * x + 1), out[1], 4);
            memcpy(dst + 4 * ((size_t)(2 * y + 1) * ow + 2 * x), out[2], 4);
            memcpy(dst + 4 * ((size_t)(2 * y + 1) * ow + 2 * x + 1), out[3], 4);
        }
    }
    free(yv);
    free(pp);
    return 1;
}

uint8_t *thug_xbr_scale(const uint8_t *src, int w, int h, int factor,
                        int *ow, int *oh)
{
    uint8_t *a, *b;

    if (!src || w <= 0 || h <= 0 || (factor != 2 && factor != 4))
        return NULL;
    a = (uint8_t *)malloc((size_t)w * h * 16);
    if (!a || !thug_xbr2x(src, w, h, a)) {
        free(a);
        return NULL;
    }
    if (factor == 4) {
        b = (uint8_t *)malloc((size_t)w * h * 64);
        if (!b || !thug_xbr2x(a, 2 * w, 2 * h, b)) {
            free(a);
            free(b);
            return NULL;
        }
        free(a);
        a = b;
    }
    if (ow) *ow = w * factor;
    if (oh) *oh = h * factor;
    return a;
}
