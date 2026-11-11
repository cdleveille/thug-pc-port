/**
 * nv2a_pb_gl.h -- an OpenGL render back end for the pushbuffer executor.
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes a title's NV2A methods and,
 * by default, rasterises them on the CPU into guest memory. With a back end
 * registered it hands the work over already transformed: surface state and
 * clears, triangle lists in surface pixels with per-vertex colour and
 * texel-space UVs, the texture the batch samples, and blend/depth/stencil/cull
 * state in the NV2A's own enum values (see nv2a_backend.h).
 *
 * This back end draws that on the GPU, register combiners included (in GLSL),
 * at an internal resolution, and presents the flipped surface to its own
 * window. [thug] It no longer writes frames back into guest memory unless
 * something asks (see THUG_GL_READBACK below).
 *
 * Switches (read once, at nv2a_pb_gl_init):
 *   RECOMP_PB_GL=0              keep the CPU rasteriser; without it (or with
 *                               it unset) this back end draws
 *   RECOMP_PB_GL_ZSCALE=<n>     depth divisor (default 2^24 for Z24S8, 2^16
 *                               for Z16): the title's z is compared in its own
 *                               units, so one fixed scale keeps every draw's
 *                               depth in the same units, which is what the
 *                               back-end contract asks for
 *   RECOMP_PB_GL_TEXREFRESH=<n> re-upload a texture every n frames (0: once,
 *                               on first use -- the default, and right for
 *                               every static texture)
 *   RECOMP_PB_GL_VERBOSE=1      log the device, targets and texture traffic
 *
 * Call nv2a_pb_gl_init() from the host's main thread before the guest starts:
 * SDL video and the GL context are created there, and the executor's poll
 * thread merely makes the context current when it first draws.
 */
#ifndef XBOXRECOMP_NV2A_PB_GL_H
#define XBOXRECOMP_NV2A_PB_GL_H

/* Create the device and register the back end. Returns 1 if the back end is
 * now drawing, 0 if it is disabled or unavailable (the CPU rasteriser then
 * carries on as before). Safe to call on a host without a display. */
int nv2a_pb_gl_init(void);

/* [thug] Direct presentation (POSIX). The back end draws every surface at an
 * internal resolution and blits the flipped one straight to its own window;
 * nothing goes back to guest memory unless asked.
 *
 * Switches, also read at nv2a_pb_gl_init:
 *   THUG_RES_SCALE=<x>      internal resolution as a multiple of 640x480
 *                           (default: desktop height / 480; 1 when headless)
 *   THUG_VSYNC=0            swap without waiting for the display
 *   THUG_FULLSCREEN=1       start in borderless fullscreen (F11 toggles)
 *   THUG_GL_DUMP=<dir>      write the presented frame as PPM every
 *   THUG_GL_DUMP_EVERY=<n>  n flips (default 60)
 *   THUG_BRIGHTNESS=<b>     gamma on the presented frame, 0.5 to 2 (1:
 *                           as rendered; above 1 lifts the midtones)
 *   THUG_MSAA=2|4|8         multisample every surface (default off); with
 *                           GL_ARB_sample_shading, alpha-tested edges are
 *                           smoothed too (THUG_GL_NO_A2C=1 turns that off)
 *   THUG_GL_READBACK=1      also write each frame into the title's surface
 *                           (on by itself with RECOMP_FB_DUMP,
 *                           RECOMP_FB_DUMP_FLIPS or RECOMP_FB_WINDOW)
 *   RECOMP_HEADLESS=1       keep the window hidden: draw, never present */

/* The visible window (an SDL_Window *), or NULL when headless or not up. */
void *nv2a_pb_gl_window(void);

/* Main thread: the window's drawable size may have changed. */
void nv2a_pb_gl_drawable_changed(void);

/* Flips so far, and the size the presented frame was rendered at. */
#include <stdint.h>
void nv2a_pb_gl_stats(uint64_t *flips, uint32_t *w, uint32_t *h);

/* [thug] Runtime settings, callable from any thread; the executor's thread
 * applies them at the next flip. */
void  nv2a_pb_gl_set_scale(float scale);   /* internal scale; 0 = native */
float nv2a_pb_gl_scale(void);              /* the scale in effect */
float nv2a_pb_gl_native_scale(void);       /* desktop height / 480 */
float nv2a_pb_gl_aspect(void);             /* presented aspect (4:3 or 16:9) */
void  nv2a_pb_gl_set_vsync(int on);
int   nv2a_pb_gl_vsync(void);
void  nv2a_pb_gl_set_fps_limit(int fps);   /* 0 = off */
int   nv2a_pb_gl_fps_limit(void);
void  nv2a_pb_gl_set_aniso(int n);         /* 1 = off */
int   nv2a_pb_gl_aniso(void);
int   nv2a_pb_gl_aniso_max(void);
void  nv2a_pb_gl_set_brightness(float b);  /* gamma on the presented frame:
                                              1 = as rendered, 0.5..2 */
float nv2a_pb_gl_brightness(void);
void  nv2a_pb_gl_set_msaa(int samples);    /* 1 (off), 2, 4 or 8 */
int   nv2a_pb_gl_msaa(void);
int   nv2a_pb_gl_msaa_max(void);           /* what the driver allows, up to 8 */

/* Called on the executor's thread after each frame is blitted to the window
 * and before the swap, with the default framebuffer bound and its size. */
void  nv2a_pb_gl_set_overlay(void (*fn)(int dw, int dh));

typedef struct {
    float    fps, frame_ms;                /* over the last half second */
    uint32_t draws, tris;                  /* last frame */
    uint32_t out_w, out_h;                 /* output resolution */
    float    scale;
    float    hist[240];                    /* flip intervals in ms, oldest first */
    int      hist_n;
} Nv2aGlFrameInfo;
void  nv2a_pb_gl_frame_info(Nv2aGlFrameInfo *out);

#endif /* XBOXRECOMP_NV2A_PB_GL_H */
