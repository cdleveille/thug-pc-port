/**
 * nv2a_pb_gl.c -- an OpenGL render back end for the pushbuffer executor.
 *
 * Provenance: from the otogi1 fork of xboxrecomp (github.com/sigods/xboxrecomp,
 * branch otogi1-linux-v0.1, commit 7d394a5), MIT licence, Copyright (c) 2026
 * sp00nz and contributors. Brought into the THUG PC port as the starting
 * point for a GPU renderer; changes for this project are marked [thug].
 *
 * See nv2a_pb_gl.h for what this is for and which switches it reads.
 *
 * Shape of the thing ([thug]):
 *
 *   main thread      nv2a_pb_gl_init(): SDL video, the game's window (hidden
 *                    when headless), a 3.3 core context, the shader,
 *                    nv2a_backend_register(); then src/host/host_window.c
 *                    pumps the window's events
 *   poll thread      clear/draw: bind the off-screen target for this colour
 *                    surface at the internal resolution, apply the NV2A state
 *                    as GL state, run the register combiners in GLSL
 *   poll thread      flip: blit the flipped surface to the window and swap.
 *                    Nothing is read back into guest memory unless asked
 *                    (THUG_GL_READBACK=1, RECOMP_FB_DUMP*)
 *
 * A texture whose address is a render target binds that target. Decoded
 * textures are cached by identity and content hash (see GlTex).
 *
 * Known gaps: depth textures (shadow maps) are not sampled; each target owns
 * its depth buffer, so two colour surfaces drawn with one zeta surface share
 * its clears (gl_clear) but not its contents.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>                 /* [thug] mkdir, for captures */
#include <time.h>

#include "nv2a_backend.h"
#include "nv2a_pb_gl.h"
#include "../d3d/d3d8_swizzle.h"   /* [thug] swizzled vs linear formats */

#if defined(_WIN32)
/* The Windows build keeps the CPU rasteriser for now: this file exists there
 * only so the symbol resolves, and nv2a_pb_gl_init() reports "not available". */
int nv2a_pb_gl_init(void)
{
    return 0;
}
#else

#include <SDL.h>
#include <epoxy/gl.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── Device ─────────────────────────────────────────────────────────────── */

static SDL_Window   *s_win;
static SDL_GLContext s_ctx;
static int           s_ready;         /* context exists */
static int           s_enabled;       /* RECOMP_PB_GL asked for it */
static int           s_verbose;
static uint32_t      s_zscale_override;
static uint32_t      s_tex_refresh;   /* frames between re-uploads, 0 = once */
static int           s_bind_log;      /* RECOMP_GL_BIND_LOG: one line per bind */

/* [thug] Direct presentation. The fork drew off-screen and read every frame
 * back into guest memory for the toolkit's window to copy again; here the
 * finished frame is blitted to this back end's own window instead, at the
 * internal resolution, and guest memory is only written when something asks
 * for it (a frame dump, or THUG_GL_READBACK=1). */
static int           s_visible;       /* the window is shown and presented to */
static int           s_vsync = 1;     /* THUG_VSYNC=0 swaps without waiting */
static int           s_readback;      /* write frames back to guest memory */
static float         s_aspect = 4.0f / 3.0f;  /* presented aspect ratio */
static volatile int  s_draw_w, s_draw_h;      /* drawable, from the main thread */
static const char   *s_dump_dir;      /* THUG_GL_DUMP=<dir>: write frames */
static uint32_t      s_dump_every = 60;
static uint64_t      s_flip_count;    /* read by the window's title */
static uint32_t      s_res_packed;    /* rendered size of the presented frame */
static float         s_aniso = 1.0f;  /* THUG_ANISO: max anisotropy (default 16) */
static float         s_aniso_max = 1.0f;  /* what the driver allows */
/* [thug] Multisampling: the sample count new targets get (1 = off), what the
 * driver allows, and whether the fragment shader can write its own coverage
 * (smooth alpha-tested edges; GL_ARB_sample_shading's gl_SampleMask). */
static int           s_msaa = 1, s_msaa_max = 1, s_sample_mask_ok;
static volatile int  s_req_msaa = -1;
static int           s_a2c_on;        /* the bound state takes coverage from alpha */
static uint32_t      s_resolves;      /* THUG_GL_STATS: resolves since the last line */

/* 1, 2, 4 or 8 samples, and no more than the driver allows. */
static int msaa_clamp(int n)
{
    int v = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
    while (v > 1 && v > s_msaa_max)
        v >>= 1;
    return v;
}
/* [thug] Settings the overlay changes while the game runs. Written by any
 * thread, applied by the executor's thread at the next flip. */
static volatile float s_req_scale = -1.0f;   /* pending internal scale, <0: none */
static volatile int   s_req_vsync = -1;      /* pending vsync, -1: none */
static volatile int   s_fps_limit;           /* 0: off */
static float          s_native_scale = 1.0f; /* desktop height / 480 */
static void         (*s_overlay)(int dw, int dh);
/* Frame pacing as presented: the last FRAME_HIST flip intervals in ms. */
#define FRAME_HIST 240
static float          s_frame_hist[FRAME_HIST];
static volatile unsigned s_frame_hist_n;
static volatile float s_fps_now, s_ms_now;
static volatile uint32_t s_last_draws, s_last_tris;
static uint32_t       s_cur_draws, s_cur_tris;
static uint32_t       s_cur_fail, s_cur_uploads;   /* [thug] per frame, for the flicker probe */
/* [thug] Set by everything that changes GL state outside the draw path
 * (clears, target allocation, present, readback, dumps): the per-draw
 * memoisation in target_bind/apply_state/rc_upload starts over. */
static int           s_gl_dirty = 1;

/* [thug] What each texture unit holds, so a draw rebinds only what changed:
 * the fork set every stage's texture and five texture parameters on every
 * draw. Forgotten (~0) whenever anything else may have touched the units --
 * texture creation and deletion, the overlay, a present. */
static GLuint s_unit_tex[4] = { ~0u, ~0u, ~0u, ~0u };
static GLuint s_unit_smp[4] = { ~0u, ~0u, ~0u, ~0u };
static int    s_active_unit = -1;

static void unit_cache_reset(void)
{
    int i;
    for (i = 0; i < 4; i++)
        s_unit_tex[i] = s_unit_smp[i] = ~0u;
    s_active_unit = -1;
}

static void bind_unit(uint32_t unit, GLuint tex, GLuint smp)
{
    if (s_unit_tex[unit] != tex) {
        if (s_active_unit != (int)unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            s_active_unit = (int)unit;
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        s_unit_tex[unit] = tex;
    }
    if (s_unit_smp[unit] != smp) {
        glBindSampler(unit, smp);
        s_unit_smp[unit] = smp;
    }
}

static GLuint s_prog;
static GLint  u_texscale;          /* [thug] per-stage coordinate scale */
static GLint  u_surf, u_origin, u_incz, u_tex, u_tex1, u_tex2, u_tex3,
              u_alphatest, u_alphafunc, u_alpharef, u_textured, u_alphaone, u_a2c;
/* The register combiners, as uniforms: uRcCin/uRcAin hold the ICW bytes, the
 * two output registers and their flag halves, and the factors. The GLSL below
 * is nv2a_combiner.c's nv2a_rc_eval written for a fragment shader -- the same
 * arithmetic on the same registers, because "diffuse times stage 0" is a
 * different function (it paints opaque black where a combiner takes its
 * colour from the alpha channel, which is every menu's text). */static GLint  u_rc_on, u_rc_stages, u_rc_muxalpha, u_rc_stpar, u_rc_cin,
              u_rc_ain, u_rc_cout, u_rc_flags, u_rc_c0, u_rc_c1, u_rc_final,
              u_rc_fc0, u_rc_fc1, u_rc_fog, u_rc_clip, u_flat,
              u_rc_bump, u_rc_bump_so;
static GLuint s_white;              /* 1x1 white: an unbound stage's texel */

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 aPos;\n"      /* x, y, z, 1/w */
    "layout(location=1) in uvec2 aCol;\n"     /* diffuse, specular */
    "layout(location=2) in vec4 aSt0;\n"      /* stage 0: s, t, q, - */
    "layout(location=3) in vec4 aSt1;\n"
    "layout(location=4) in vec4 aSt2;\n"
    "layout(location=5) in vec4 aSt3;\n"
    "layout(location=6) in float aFog;\n"     /* interpolated fog factor */
    "uniform vec2 uSurf;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uInvZ;\n"
    "out vec4 vColor;\n"
    "out vec4 vSpec;\n"
    "out vec4 vSt0;\n"
    "out vec4 vSt1;\n"
    "out vec4 vSt2;\n"
    "out vec4 vSt3;\n"
    "out float vFog;\n"
    "vec4 uncol(uint c) {\n"
    "    return vec4(float((c >> 16u) & 255u), float((c >> 8u) & 255u),\n"
    "                float(c & 255u), float((c >> 24u) & 255u)) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    float w = (aPos.w != 0.0) ? 1.0 / aPos.w : 1.0;\n"
    /* [thug] No y flip: a target stores surface row 0 in GL row 0, so a
     * render target bound as a texture lines up with textures decoded from
     * guest memory (row 0 first), and only the present flips. uSurf is the
     * surface's *logical* size, so rendering at any internal resolution is
     * just a bigger target and viewport. */
    "    vec2 ndc = vec2((aPos.x - uOrigin.x) / uSurf.x * 2.0 - 1.0,\n"
    "                    (aPos.y - uOrigin.y) / uSurf.y * 2.0 - 1.0);\n"
    "    float z = clamp(aPos.z * uInvZ, 0.0, 1.0);\n"
    /* Carry the title's own w in gl_Position.w so GL's perspective-correct
     * interpolation weights the varyings the way the CPU rasteriser's
     * 1/w interpolation does. [thug] Depth goes to NDC as 2z - 1, so window
     * depth is the title's z itself: the whole buffer's precision, the same
     * units the clears use, and a polygon-offset unit that is one of the
     * title's (z alone used only half the range and doubled the bias). */
    "    gl_Position = vec4(ndc * w, (z * 2.0 - 1.0) * w, w);\n"
    "    vColor = uncol(aCol.x);\n"
    "    vSpec  = uncol(aCol.y);\n"
    "    vSt0 = aSt0; vSt1 = aSt1; vSt2 = aSt2; vSt3 = aSt3;\n"
    "    vFog = aFog;\n"
    "}\n";

/* The combiner, and the alpha test that reads its result. Register numbers,
 * input mappings and the final combiner are nv2a_combiner.h's. */
static const char *k_fs =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "in vec4 vSpec;\n"
    "in vec4 vSt0;\n"
    "in vec4 vSt1;\n"
    "in vec4 vSt2;\n"
    "in vec4 vSt3;\n"
    "in float vFog;\n"
    "uniform sampler2D uTex;\n"        /* stage 0 */
    "uniform sampler2D uTex1;\n"
    "uniform sampler2D uTex2;\n"
    "uniform sampler2D uTex3;\n"
    /* [thug] Per stage: 1/size for a linear (pitch) texture, whose coordinates
     * the NV2A takes in texels; 1 for a swizzled one (normalised). Logical
     * size, so a render target sampled at a higher internal resolution reads
     * the same place. */
    "uniform vec2 uTexScale[4];\n"
    "uniform int uTextured;\n"
    "uniform int uAlphaTest;\n"
    "uniform int uAlphaFunc;\n"
    "uniform float uAlphaRef;\n"
    "uniform int uRcOn;\n"
    "uniform int uRcStages;\n"
    "uniform int uRcMuxAlpha;\n"
    "uniform uint uRcStPar;\n"
    "uniform uvec4 uRcClip[8];\n"
    "uniform vec4 uRcBump[4];\n"       /* per stage: m00, m01, m10, m11 */
    "uniform vec2 uRcBumpSO[4];\n"     /* per stage: scale, offset */
    "uniform uvec4 uRcCin[8];\n"
    "uniform uvec4 uRcAin[8];\n"
    "uniform uvec2 uRcCout[8];\n"
    "uniform uvec2 uRcFlags[8];\n"
    "uniform vec4 uRcC0[8];\n"
    "uniform vec4 uRcC1[8];\n"
    "uniform uvec2 uRcFinal;\n"
    "uniform vec4 uRcFC0;\n"
    "uniform vec4 uRcFC1;\n"
    "uniform vec4 uRcFog;\n"
    "uniform int uFlat;\n"
    "uniform int uAlphaOne;\n"
    /* [thug] Samples per pixel when an alpha-tested draw should cover the
     * fraction of them its alpha says (multisampled, not blended), else 0. */
    "uniform int uA2C;\n"
    "out vec4 frag;\n"
    /* PS_INPUTMAPPING (nv2a_combiner.c map_in). */
    "float rc_in(float x, uint byte) {\n"
    "    float p = max(x, 0.0);\n"
    "    switch ((byte >> 5) & 7u) {\n"
    "    case 0u: return p;\n"
    "    case 1u: return 1.0 - clamp(x, 0.0, 1.0);\n"
    "    case 2u: return 2.0 * p - 1.0;\n"
    "    case 3u: return -2.0 * p + 1.0;\n"
    "    case 4u: return p - 0.5;\n"
    "    case 5u: return -p + 0.5;\n"
    "    case 6u: return x;\n"
    "    default: return -x;\n"
    "    }\n"
    "}\n"
    "float rc_out(float x, uint m) {\n"
    "    if (m == 0x08u) return x - 0.5;\n"
    "    if (m == 0x10u) return x * 2.0;\n"
    "    if (m == 0x18u) return (x - 0.5) * 2.0;\n"
    "    if (m == 0x20u) return x * 4.0;\n"
    "    if (m == 0x30u) return x * 0.5;\n"
    "    return x;\n"
    "}\n"
    "vec3 rc_rgb(vec4 R[16], uint byte) {\n"
    "    vec4 r = R[byte & 0xFu];\n"
    "    vec3 s = ((byte & 0x10u) != 0u) ? r.aaa : r.rgb;\n"
    "    return vec3(rc_in(s.x, byte), rc_in(s.y, byte), rc_in(s.z, byte));\n"
    "}\n"
    "float rc_a(vec4 R[16], uint byte) {\n"
    "    vec4 r = R[byte & 0xFu];\n"
    "    return rc_in(((byte & 0x10u) != 0u) ? r.a : r.b, byte);\n"
    "}\n"
    /* One stage result, by its SHADER_STAGE_PROGRAM mode, and the same table
     * the CPU rasteriser's rc_stage_fetch() works from (nv2a_pb_exec.c):
     * 0 none, 1/2 a projected 2D sample, 3 a cubemap, 4 the coordinate
     * itself, 5 a clip plane, everything else the bump-env and dot-product
     * modes, which read zero until something depends on one.
     *
     * The clip-plane stage does not shade: it kills the pixel, which is why
     * this can discard from inside a function. A batch using one of these
     * modes used to be refused outright -- uRcOn stayed 0 and the whole
     * batch fell back to "diffuse times stage 0" -- and the title's player
     * model and bamboo are exactly such batches: they came out black, and
     * RECOMP_GL_FLAT showed every one of their triangles arriving and passing
     * every raster gate. Matching the CPU table here is what paints them. */
    /* xemu psh.c's sign3(): an unsigned channel read as a signed bump value. */
    "float sign3(float x) {\n"
    "    float v = x * 255.0;\n"
    "    return v >= 128.0 ? (v - 256.0) / 127.0 : v / 127.0;\n"
    "}\n"
    "vec2 rc_bump_dir(uint st, vec4 cin) {\n"
    "    vec2 d = vec2(sign3(cin.b), sign3(cin.g));\n"
    "    vec4 m = uRcBump[st];\n"
    "    return mat2(m.x, m.z, m.y, m.w) * d;\n"
    "}\n"
    "vec4 rc_stage(uint st, uint mode, vec4 c, sampler2D tx, vec4 cin) {\n"
    "    if (mode == 0u) return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    if (mode == 4u) return clamp(c, 0.0, 1.0);\n"
    "    if (mode == 5u) {\n"
    "        for (int j = 0; j < 4; j++) {\n"
    "            bool ge = uRcClip[st][j] != 0u;\n"
    "            if (ge ? c[j] >= 0.0 : c[j] < 0.0) discard;\n"
    "        }\n"
    "        return vec4(0.0);\n"
    "    }\n"
    /* [thug] The bump offset is in the coordinate's own units -- texels for
     * a linear texture -- so it is added before uTexScale normalises them,
     * as on the NV2A (and xemu's psh.c: texScale * (pT + dsdt)). Added after,
     * an offset of a few texels became several whole textures: THUG's water
     * and rain screen effects (a linear copy of the frame, refracted through
     * a ripple map) drew repeated strips of the screen. */
    "    if (mode == 6u) {\n"            /* BUMPENVMAP */
    "        float q = (c.w != 0.0) ? c.w : 1.0;\n"
    "        return texture(tx, (c.xy / q + rc_bump_dir(st, cin)) * uTexScale[st]);\n"
    "    }\n"
    "    if (mode == 7u) {\n"            /* BUMPENVMAP_LUMINANCE */
    "        float q = (c.w != 0.0) ? c.w : 1.0;\n"
    "        float lum = sign3(cin.r) * 0.5 + 0.5;\n"
    "        vec4 t = texture(tx, (c.xy / q + rc_bump_dir(st, cin)) * uTexScale[st]);\n"
    "        return t * (uRcBumpSO[st].x * lum + uRcBumpSO[st].y);\n"
    "    }\n"
    "    if (mode >= 8u) return vec4(0.0);\n"
    "    float q = (c.w != 0.0) ? c.w : 1.0;\n"
    "    return texture(tx, c.xy * uTexScale[st] / q);\n"
    "}\n"
    "vec4 rc_eval(vec4 t0, vec4 t1, vec4 t2, vec4 t3) {\n"
    "    vec4 R[16];\n"
    "    for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n"
    "    R[3] = vec4(uRcFog.rgb, vFog);\n"
    "    R[4] = vColor;\n"
    "    R[5] = vSpec;\n"
    "    R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
    "    R[12] = vec4(0.0, 0.0, 0.0, ((uRcStPar & 0x1Fu) != 0u) ? t0.a : 1.0);\n"
    "    for (int s = 0; s < uRcStages; s++) {\n"
    "        uint fl = uRcFlags[s].x, afl = uRcFlags[s].y;\n"
    "        vec3 A, B, C, D, ab, cd, ms;\n"
    "        float aA, aB, aC, aD, aab, acd, ams;\n"
    "        bool mux = uRcMuxAlpha != 0 ? (R[12].a >= 0.5)\n"
        "                                   : ((int(R[12].a * 255.0) & 1) != 0);\n"
    "        R[1] = uRcC0[s];\n"
    "        R[2] = uRcC1[s];\n"
    "        A = rc_rgb(R, uRcCin[s].x); B = rc_rgb(R, uRcCin[s].y);\n"
    "        C = rc_rgb(R, uRcCin[s].z); D = rc_rgb(R, uRcCin[s].w);\n"
    "        aA = rc_a(R, uRcAin[s].x); aB = rc_a(R, uRcAin[s].y);\n"
    "        aC = rc_a(R, uRcAin[s].z); aD = rc_a(R, uRcAin[s].w);\n"
    "        for (int i = 0; i < 3; i++) {\n"
    "            float dab = dot(A, B), dcd = dot(C, D);\n"
    "            float pab = ((fl & 2u) != 0u) ? dab : A[i] * B[i];\n"
    "            float pcd = ((fl & 1u) != 0u) ? dcd : C[i] * D[i];\n"
    "            float sum = ((fl & 4u) != 0u) ? (mux ? pcd : pab) : pab + pcd;\n"
    "            ab[i] = clamp(rc_out(pab, fl & 0x38u), -1.0, 1.0);\n"
    "            cd[i] = clamp(rc_out(pcd, fl & 0x38u), -1.0, 1.0);\n"
    "            ms[i] = clamp(rc_out(sum, fl & 0x38u), -1.0, 1.0);\n"
    "        }\n"
    "        {\n"
    "            float pab = aA * aB, pcd = aC * aD;\n"
    "            float sum = ((afl & 4u) != 0u) ? (mux ? pcd : pab) : pab + pcd;\n"
    "            aab = clamp(rc_out(pab, afl & 0x38u), -1.0, 1.0);\n"
    "            acd = clamp(rc_out(pcd, afl & 0x38u), -1.0, 1.0);\n"
    "            ams = clamp(rc_out(sum, afl & 0x38u), -1.0, 1.0);\n"
    "        }\n"
    "        {\n"
    "            uint d = (uRcCout[s].x >> 4) & 0xFu;\n"
    "            if (d != 0u)\n"
    "                R[d] = vec4(ab, ((fl & 0x80u) != 0u) ? ab.z : R[d].a);\n"
    "            d = uRcCout[s].x & 0xFu;\n"
    "            if (d != 0u)\n"
    "                R[d] = vec4(cd, ((fl & 0x40u) != 0u) ? cd.z : R[d].a);\n"
    "            d = (uRcCout[s].x >> 8) & 0xFu;\n"
    "            if (d != 0u)\n"
    "                R[d] = vec4(ms, R[d].a);\n"
    "            d = (uRcCout[s].y >> 4) & 0xFu;\n"
    "            if (d != 0u) R[d].a = aab;\n"
    "            d = uRcCout[s].y & 0xFu;\n"
    "            if (d != 0u) R[d].a = acd;\n"
    "            d = (uRcCout[s].y >> 8) & 0xFu;\n"
    "            if (d != 0u) R[d].a = ams;\n"
    "        }\n"
    "        R[0] = vec4(0.0);\n"
    "    }\n"
    "    vec4 out4;\n"
    "    if (uRcFinal.x != 0u || uRcFinal.y != 0u) {\n"
    "        uint f1 = uRcFinal.y, fflags = f1 & 0xFFu;\n"
    "        R[1] = uRcFC0;\n"
    "        R[2] = uRcFC1;\n"
    "        for (int i = 0; i < 3; i++) {\n"
    "            float a = ((fflags & 0x40u) != 0u) ? 1.0 - R[5][i] : R[5][i];\n"
    "            float b = ((fflags & 0x20u) != 0u) ? 1.0 - R[12][i] : R[12][i];\n"
    "            R[14][i] = ((fflags & 0x80u) != 0u) ? clamp(a + b, 0.0, 1.0)\n"
    "                                               : a + b;\n"
    "        }\n"
    "        R[14].a = 0.0;\n"
    "        R[15] = vec4(rc_rgb(R, f1 >> 24) * rc_rgb(R, f1 >> 16), 0.0);\n"
    "        vec3 A = rc_rgb(R, uRcFinal.x >> 24);\n"
    "        vec3 B = rc_rgb(R, uRcFinal.x >> 16);\n"
    "        vec3 C = rc_rgb(R, uRcFinal.x >> 8);\n"
    "        vec3 D = rc_rgb(R, uRcFinal.x);\n"
    "        out4 = vec4(D + C * (1.0 - A) + B * A, rc_a(R, f1 >> 8));\n"
    "    } else {\n"
    "        out4 = R[12];\n"
    "    }\n"
    "    return clamp(out4, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    /* A shader that writes gl_SampleMask anywhere must write it on every
     * path, or the coverage is undefined (NVIDIA: nothing drawn). */
    "#ifdef HAVE_SAMPLE_MASK\n"
    "    gl_SampleMask[0] = -1;\n"
    "#endif\n"
    "    if (uFlat == 1) { frag = vec4(1.0, 0.0, 1.0, 1.0); return; }\n"
    "    vec4 c;\n"
    "    if (uRcOn == 1) {\n"
    "        vec4 t0 = rc_stage(0u, (uRcStPar >> 0) & 0x1Fu, vSt0, uTex,\n"
    "                          vec4(0.0));\n"
    "        vec4 t1 = rc_stage(1u, (uRcStPar >> 5) & 0x1Fu, vSt1, uTex1, t0);\n"
    "        vec4 t2 = rc_stage(2u, (uRcStPar >> 10) & 0x1Fu, vSt2, uTex2, t1);\n"
    "        vec4 t3 = rc_stage(3u, (uRcStPar >> 15) & 0x1Fu, vSt3, uTex3, t2);\n"
    "        c = rc_eval(t0, t1, t2, t3);\n"
    "    } else {\n"
    "        c = vColor;\n"
    "        if (uTextured == 1)\n"
    "            c *= texture(uTex, vSt0.xy * uTexScale[0]);\n"
    "    }\n"
    /* RECOMP_GL_ALPHA_ONE: the ground is invisible in normal rendering yet
     * survives FLAT's alpha test and NO_BLEND's, which is what a fragment
     * alpha of ~0 looks like under SRC_ALPHA blending: it blends away to the
     * backdrop. Forcing alpha to 1 here answers whether the combiner's alpha
     * output is the gate -- colours are untouched, so a correct scene barely
     * changes. */
    "    if (uAlphaOne == 1) c.a = 1.0;\n"
    "    if (uAlphaTest == 1) {\n"
    "        float a = c.a;\n"
    "        int f = uAlphaFunc;\n"
    /* [thug] Alpha-tested edges under multisampling: the test's cut, put
     * where it falls inside the pixel. (a - ref) over alpha's change across
     * one pixel is how far the threshold is from this pixel's centre, so the
     * covered fraction is that plus a half; the colour keeps its alpha. A
     * hard cutout (fence, foliage, hair) then gets the same smooth edge as a
     * polygon's, and keeps its shape. GREATER and GEQUAL only: the tests
     * that keep the opaque side. */
    "#ifdef HAVE_SAMPLE_MASK\n"
    "        float w = fwidth(a);\n"   /* alpha flat across the pixel: no edge */
    "        if (uA2C > 1 && (f == 0x204 || f == 0x206) && w > 1.0 / 1024.0) {\n"
    "            float cov = clamp((a - uAlphaRef) / w + 0.5, 0.0, 1.0);\n"
    "            int n = int(cov * float(uA2C) + 0.5);\n"
    "            if (n <= 0) discard;\n"
    "            gl_SampleMask[0] = (1 << n) - 1;\n"
    "            frag = c;\n"
    "            return;\n"
    "        }\n"
    "#endif\n"
    "        bool pass = (f == 0x200) ? false :\n"
    "                    (f == 0x201) ? a <  uAlphaRef :\n"
    "                    (f == 0x202) ? a == uAlphaRef :\n"
    "                    (f == 0x203) ? a <= uAlphaRef :\n"
    "                    (f == 0x204) ? a >  uAlphaRef :\n"
    "                    (f == 0x205) ? a != uAlphaRef :\n"
    "                    (f == 0x206) ? a >= uAlphaRef : true;\n"
    "        if (!pass) discard;\n"
    "    }\n"
    "    frag = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[PBGL] shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int program_build(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = 0;
    GLint ok = 0;

    /* [thug] With GL_ARB_sample_shading the fragment shader writes its own
     * coverage (gl_SampleMask) for alpha-tested draws under multisampling:
     * the extension goes in after the #version line. */
    s_sample_mask_ok = epoxy_has_gl_extension("GL_ARB_sample_shading")
                       && !getenv("THUG_GL_NO_A2C");
    if (s_sample_mask_ok) {
        const char *nl = strchr(k_fs, '\n');
        static const char ext[] = "#extension GL_ARB_sample_shading : require\n"
                                  "#define HAVE_SAMPLE_MASK 1\n";
        size_t head = (size_t)(nl - k_fs) + 1, n = strlen(k_fs);
        char *src = (char *)malloc(n + sizeof ext);
        if (src) {
            memcpy(src, k_fs, head);
            memcpy(src + head, ext, sizeof ext - 1);
            memcpy(src + head + sizeof ext - 1, k_fs + head, n - head + 1);
            fs = compile(GL_FRAGMENT_SHADER, src);
            free(src);
        }
        if (!fs) {
            fprintf(stderr, "[PBGL] alpha-tested edges will not be smoothed\n");
            s_sample_mask_ok = 0;
        }
    }
    if (!fs)
        fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs)
        return 0;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(s_prog, sizeof log, NULL, log);
        fprintf(stderr, "[PBGL] program link failed: %s\n", log);
        return 0;
    }
    u_surf      = glGetUniformLocation(s_prog, "uSurf");
    u_origin    = glGetUniformLocation(s_prog, "uOrigin");
    u_incz      = glGetUniformLocation(s_prog, "uInvZ");
    u_tex       = glGetUniformLocation(s_prog, "uTex");
    u_tex1      = glGetUniformLocation(s_prog, "uTex1");
    u_tex2      = glGetUniformLocation(s_prog, "uTex2");
    u_tex3      = glGetUniformLocation(s_prog, "uTex3");
    u_alphatest = glGetUniformLocation(s_prog, "uAlphaTest");
    u_alphafunc = glGetUniformLocation(s_prog, "uAlphaFunc");
    u_alpharef  = glGetUniformLocation(s_prog, "uAlphaRef");
    u_textured  = glGetUniformLocation(s_prog, "uTextured");
    u_rc_on      = glGetUniformLocation(s_prog, "uRcOn");
    u_rc_stages  = glGetUniformLocation(s_prog, "uRcStages");
    u_rc_muxalpha = glGetUniformLocation(s_prog, "uRcMuxAlpha");
    u_rc_stpar   = glGetUniformLocation(s_prog, "uRcStPar");
    u_rc_bump    = glGetUniformLocation(s_prog, "uRcBump");
    u_rc_bump_so = glGetUniformLocation(s_prog, "uRcBumpSO");
    u_rc_cin     = glGetUniformLocation(s_prog, "uRcCin");
    u_rc_ain     = glGetUniformLocation(s_prog, "uRcAin");
    u_rc_cout    = glGetUniformLocation(s_prog, "uRcCout");
    u_rc_flags   = glGetUniformLocation(s_prog, "uRcFlags");
    u_rc_c0      = glGetUniformLocation(s_prog, "uRcC0");
    u_rc_c1      = glGetUniformLocation(s_prog, "uRcC1");
    u_rc_final   = glGetUniformLocation(s_prog, "uRcFinal");
    u_rc_fc0     = glGetUniformLocation(s_prog, "uRcFC0");
    u_rc_fc1     = glGetUniformLocation(s_prog, "uRcFC1");
    u_rc_clip    = glGetUniformLocation(s_prog, "uRcClip");
    u_rc_fog     = glGetUniformLocation(s_prog, "uRcFog");
    u_flat       = glGetUniformLocation(s_prog, "uFlat");
    u_alphaone   = glGetUniformLocation(s_prog, "uAlphaOne");
    u_a2c        = glGetUniformLocation(s_prog, "uA2C");
    u_texscale   = glGetUniformLocation(s_prog, "uTexScale");
    return 1;
}

/* The vertex the executor hands over, in the layout the vertex shader reads:
 * position and 1/w, the two colours, every stage's texture coordinate (s, t,
 * q -- the combiners read all four stages), and the fog factor. */
typedef struct {
    float    x, y, z, rhw;
    uint32_t color, specular;
    float    st[4][4];
    float    fogf;
    float    pad[3];
} GlVert;

static GLuint s_vao, s_vbo;
/* [thug] The vertex buffer is a ring: each batch is written past the last one
 * with an unsynchronised map, and the buffer is orphaned only when it wraps.
 * The fork re-specified the whole buffer (glBufferData) for every batch, a
 * driver reallocation per draw at thousands of draws a frame. */
#define VBO_SEGS       8u
#define VBO_SEG_BYTES  ((8u << 20) / sizeof(GlVert) * sizeof(GlVert))
#define VBO_RING_BYTES (VBO_SEGS * VBO_SEG_BYTES)
static size_t s_vbo_head;             /* next free byte */
/* [thug] With ARB_buffer_storage the ring is mapped once, persistently and
 * coherently, and batches are written straight into it; each 8 MB segment
 * gets a fence when the head leaves it and is waited on before reuse. */
static uint8_t *s_vbo_ptr;
static GLsync   s_seg_fence[VBO_SEGS];
static int      s_vao_bound;          /* our VAO/VBO are bound (the overlay rebinds) */

static void *vbo_reserve(size_t bytes)
{
    size_t seg = s_vbo_head / VBO_SEG_BYTES;

    if (bytes > VBO_SEG_BYTES)
        return NULL;
    if (s_vbo_head + bytes > (seg + 1) * VBO_SEG_BYTES) {
        if (s_seg_fence[seg])
            glDeleteSync(s_seg_fence[seg]);
        s_seg_fence[seg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        seg = (seg + 1) % VBO_SEGS;
        s_vbo_head = seg * VBO_SEG_BYTES;
        if (s_seg_fence[seg]) {
            glClientWaitSync(s_seg_fence[seg], GL_SYNC_FLUSH_COMMANDS_BIT,
                             2000000000ull);
            glDeleteSync(s_seg_fence[seg]);
            s_seg_fence[seg] = 0;
        }
    }
    return s_vbo_ptr + s_vbo_head;
}
static GlVert *s_verts;               /* staging, grown to the largest batch */
static GLint   s_draw_first;          /* [thug] this batch's first ring vertex */
static size_t  s_verts_cap;

/* The context was created on the main thread; every call below arrives on
 * the executor's poll thread, so each entry point makes it current there
 * first. epoxy resolves its entry points on first use from whatever context
 * is current, and aborts outright if there is none -- so a failed
 * MakeCurrent must skip the work, not fall through into GL. Failures are
 * reported once: the reason matters, the repetition does not. */
static int gl_ready(void)
{
    static int complained;
    /* Per thread: the context is current here already. A draw is not the
     * place to spend a driver round trip, and there are tens of them a
     * frame; the executor's poll thread is the only one that ever gets
     * here, so the flag stays true for the life of the run. */
    static __thread int t_current;

    if (t_current)
        return 1;
    if (SDL_GL_MakeCurrent(s_win, s_ctx) == 0) {
        /* The program stays current for the life of the context: nothing else
         * draws, and every uniform below is set against it. */
        glUseProgram(s_prog);
        /* [thug] The four stage samplers are units 0-3, for good. */
        glUniform1i(u_tex, 0);
        glUniform1i(u_tex1, 1);
        glUniform1i(u_tex2, 2);
        glUniform1i(u_tex3, 3);
        /* [thug] The swap interval belongs to the context, on the thread
         * that swaps: adaptive vsync if the driver has it, else plain. */
        if (s_visible) {
            if (!s_vsync)
                SDL_GL_SetSwapInterval(0);
            else if (SDL_GL_SetSwapInterval(-1) != 0)
                SDL_GL_SetSwapInterval(1);
        }
        t_current = 1;
        return 1;
    }
    if (!complained) {
        fprintf(stderr, "[PBGL] SDL_GL_MakeCurrent failed: %s\n",
                SDL_GetError());
        complained = 1;
    }
    return 0;
}

/* ── Targets: one off-screen colour+depth pair per surface ──────────────── */

/* [thug] Reworked for direct presentation and internal resolution scaling.
 *
 * A target is keyed by its colour surface alone and covers the whole surface
 * from (0,0): its logical size grows to the furthest clip rectangle drawn into
 * it, and the clip itself becomes a scissor. The fork sized a target to each
 * clip, so the same surface drawn with two clips (THUG's 1280x360+0+60
 * letterbox passes and its full-frame ones on the same 1280x480 buffer) became
 * two targets holding half a frame each.
 *
 * Physical size is logical times s_scale: the vertex shader maps surface
 * pixels through the logical size, so the GPU rasterises at the higher
 * resolution with nothing else changing. */

/* [thug] Multisampling (THUG_MSAA, the overlay's antialiasing). A target with
 * samples > 1 is drawn into its multisampled pair (msfbo: colour and
 * depth-stencil renderbuffers) and fbo/tex become its resolved colour. The
 * resolve is lazy: it runs only when something reads the colour -- a draw
 * that samples the target as a texture (THUG's own 2x-wide AA pass, every
 * frame), the present, a dump or a readback -- and only if something was
 * drawn since the last one. */
typedef struct {
    uint32_t color_va, zeta_va;
    uint32_t lw, lh;              /* logical size (surface pixels covered) */
    uint32_t pw, ph;              /* physical size (allocated) */
    GLuint   fbo, tex, rbo;
    GLuint   msfbo, mscol, msdepth;
    uint32_t samples;             /* 1: not multisampled */
    int      ms_dirty;            /* drawn since the last resolve */
    int      alpha_one;           /* tex's alpha swizzle is GL_ONE (see stage_bind) */
    uint64_t used;
} GlTarget;

#define TARGET_MAX 16
static GlTarget s_targets[TARGET_MAX];
static GlTarget *s_cur;
static Nv2aSurface s_cur_surf;        /* the surface s_cur belongs to */
static int s_frame_dirty;             /* something was drawn since the readback */
static GLuint s_bound_fbo;            /* [thug] what target_bind last bound */
static int s_state_valid;             /* [thug] s_last_rs is what GL holds */
static Nv2aRenderState s_last_rs;
static uint64_t s_target_clock;
static float s_scale = 1.0f;          /* internal resolution: physical / logical */
/* [thug] Per axis. Widescreen is anamorphic, as on the Xbox: the title draws
 * a 16:9 view into its 640x480 surfaces, and the presented frame is stretched
 * to 16:9. Scaling x by 4/3 more than y gives that stretch its pixels: at
 * 1080p the 640x480 front buffer is rendered at 1920x1080. */
static float s_scale_x = 1.0f, s_scale_y = 1.0f;

/* Per stage, what the fragment shader multiplies the coordinate by: the
 * NV2A addresses a linear texture in texels and a swizzled one normalised. */
static GLfloat s_stage_sc[4][2];

/* The target the frame is presented from: the biggest surface drawn since the
 * last flip, as the executor itself picks the surface it presents. */
static GlTarget *s_present;
static uint32_t  s_present_area;

static uint32_t z_divisor(const Nv2aRenderState *rs)
{
    uint32_t zf;

    if (s_zscale_override)
        return s_zscale_override;
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. The
     * title's depth lives in that many bits, so one fixed divisor per format
     * keeps every draw and every clear in the same units -- which is the
     * contract: window z is compared as it is, never rescaled per draw. */
    zf = (rs->zeta_format >> 4) & 0xF;
    return zf == 1 ? 65536u : 16777216u;
}

static void target_release(GlTarget *t)
{
    if (t == s_cur) s_cur = NULL;
    if (t == s_present) { s_present = NULL; s_present_area = 0; }
    unit_cache_reset();
    if (t->fbo)  glDeleteFramebuffers(1, &t->fbo);
    if (t->tex)  glDeleteTextures(1, &t->tex);
    if (t->rbo)  glDeleteRenderbuffers(1, &t->rbo);
    if (t->msfbo)   glDeleteFramebuffers(1, &t->msfbo);
    if (t->mscol)   glDeleteRenderbuffers(1, &t->mscol);
    if (t->msdepth) glDeleteRenderbuffers(1, &t->msdepth);
    memset(t, 0, sizeof *t);
}

/* The framebuffer draws and clears go to. */
static GLuint target_draw_fbo(const GlTarget *t)
{
    return t->samples > 1 ? t->msfbo : t->fbo;
}

/* [thug] Bring t's resolved colour (fbo/tex) up to date with its multisampled
 * storage. Disturbs the framebuffer bindings, the scissor and the colour
 * mask; s_gl_dirty and s_bound_fbo make the next draw set them again. With
 * depth, the depth-stencil is resolved too (for carrying a target's contents
 * into new storage). */
static void target_resolve_ex(GlTarget *t, int depth)
{
    if (!t || t->samples <= 1 || (!t->ms_dirty && !depth))
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->msfbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, (GLint)t->pw, (GLint)t->ph, 0, 0, (GLint)t->pw,
                      (GLint)t->ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (depth) {
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glBlitFramebuffer(0, 0, (GLint)t->pw, (GLint)t->ph, 0, 0, (GLint)t->pw,
                          (GLint)t->ph, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                          GL_NEAREST);
    }
    t->ms_dirty = 0;
    s_gl_dirty = 1;
    s_state_valid = 0;
    s_bound_fbo = 0;
    s_resolves++;
}

static void target_resolve(GlTarget *t)
{
    target_resolve_ex(t, 0);
}

static uint32_t scaled_x(uint32_t v)
{
    return (uint32_t)((float)v * s_scale_x + 0.5f);
}

static uint32_t scaled_y(uint32_t v)
{
    return (uint32_t)((float)v * s_scale_y + 0.5f);
}

/* (Re)allocate t's storage at logical lw x lh, keeping what it already holds. */
static int target_alloc(GlTarget *t, uint32_t lw, uint32_t lh)
{
    GLuint fbo = 0, tex = 0, rbo = 0;
    uint32_t pw = scaled_x(lw), ph = scaled_y(lh);

    unit_cache_reset();
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)pw, (GLsizei)ph, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          (GLsizei)pw, (GLsizei)ph);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[PBGL] target %ux%u incomplete\n", pw, ph);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glDeleteRenderbuffers(1, &rbo);
        return 0;
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (t->fbo) {
        /* Growing, or a new internal scale: carry the old contents across,
         * the old logical area to where it lands at the current scale.
         * Depth and stencil can only be copied nearest. A multisampled
         * target is resolved into its single-sample pair first. */
        GLint dw = (GLint)scaled_x(t->lw), dh = (GLint)scaled_y(t->lh);
        int same = dw == (GLint)t->pw && dh == (GLint)t->ph;
        target_resolve_ex(t, 1);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, (GLint)t->pw, (GLint)t->ph, 0, 0, dw, dh,
                          GL_COLOR_BUFFER_BIT, same ? GL_NEAREST : GL_LINEAR);
        glBlitFramebuffer(0, 0, (GLint)t->pw, (GLint)t->ph, 0, 0, dw, dh,
                          GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                          GL_NEAREST);
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->tex);
        glDeleteRenderbuffers(1, &t->rbo);
    }
    if (t->msfbo) {
        glDeleteFramebuffers(1, &t->msfbo);
        glDeleteRenderbuffers(1, &t->mscol);
        glDeleteRenderbuffers(1, &t->msdepth);
        t->msfbo = t->mscol = t->msdepth = 0;
    }
    t->fbo = fbo; t->tex = tex; t->rbo = rbo;
    t->lw = lw; t->lh = lh; t->pw = pw; t->ph = ph;
    t->samples = 1;
    t->ms_dirty = 0;
    t->alpha_one = 0;                 /* a new texture swizzles nothing */
    /* [thug] The multisampled pair, filled from the single-sample one (a blit
     * into a multisampled framebuffer replicates each pixel into every
     * sample, same size only). If the driver refuses, the target simply
     * stays single-sampled. */
    if (s_msaa > 1) {
        GLuint mf = 0, mc = 0, md = 0;
        glGenRenderbuffers(1, &mc);
        glBindRenderbuffer(GL_RENDERBUFFER, mc);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_RGBA8,
                                         (GLsizei)pw, (GLsizei)ph);
        glGenRenderbuffers(1, &md);
        glBindRenderbuffer(GL_RENDERBUFFER, md);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_DEPTH24_STENCIL8,
                                         (GLsizei)pw, (GLsizei)ph);
        glGenFramebuffers(1, &mf);
        glBindFramebuffer(GL_FRAMEBUFFER, mf);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, mc);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, md);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glStencilMask(0xFF);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mf);
            glBlitFramebuffer(0, 0, (GLint)pw, (GLint)ph, 0, 0, (GLint)pw, (GLint)ph,
                              GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
                              | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
            t->msfbo = mf; t->mscol = mc; t->msdepth = md;
            t->samples = (uint32_t)s_msaa;
            {   /* a driver that refuses the replicating blit says so here */
                GLenum err = glGetError();
                if (err != GL_NO_ERROR)
                    fprintf(stderr, "[PBGL] filling a %ux multisampled target:"
                            " GL error %04X\n", t->samples, err);
            }
        } else {
            fprintf(stderr, "[PBGL] %dx multisampled target %ux%u incomplete;"
                    " drawing it without\n", s_msaa, pw, ph);
            glDeleteFramebuffers(1, &mf);
            glDeleteRenderbuffers(1, &mc);
            glDeleteRenderbuffers(1, &md);
        }
    }
    s_gl_dirty = 1;
    s_state_valid = 0;
    s_bound_fbo = 0;
    if (s_verbose)
        fprintf(stderr, "[PBGL] target %08X logical %ux%u physical %ux%u samples %u\n",
                t->color_va, lw, lh, pw, ph, t->samples);
    return 1;
}

static GlTarget *target_by_va(uint32_t va)
{
    uint32_t i;
    for (i = 0; i < TARGET_MAX; i++)
        if (s_targets[i].fbo && s_targets[i].color_va == va)
            return &s_targets[i];
    return NULL;
}

static GlTarget *target_for(const Nv2aSurface *s, const Nv2aRenderState *rs)
{
    GlTarget *t, *victim = &s_targets[0];
    uint32_t i, need_w, need_h;

    if (!s || !s->width || !s->height)
        return NULL;
    need_w = s->x + s->width;
    need_h = s->y + s->height;
    t = target_by_va(s->color_va);
    if (t) {
        t->used = ++s_target_clock;
        t->zeta_va = rs ? rs->zeta_va : 0;
        if (need_w > t->lw || need_h > t->lh) {
            if (!target_alloc(t, need_w > t->lw ? need_w : t->lw,
                              need_h > t->lh ? need_h : t->lh))
                return NULL;
        }
        return t;
    }
    for (i = 0; i < TARGET_MAX; i++) {
        t = &s_targets[i];
        if (!t->fbo) { victim = t; break; }
        if (t->used < victim->used)
            victim = t;
    }
    if (victim->fbo)
        target_release(victim);
    t = victim;
    memset(t, 0, sizeof *t);
    t->color_va = s->color_va;
    t->zeta_va  = rs ? rs->zeta_va : 0;
    if (!target_alloc(t, need_w, need_h)) {
        memset(t, 0, sizeof *t);
        return NULL;
    }
    t->used = ++s_target_clock;
    return t;
}

/* Bind t, with the surface's clip rectangle as the scissor. */
static void target_bind(GlTarget *t, const Nv2aSurface *s)
{
    uint32_t area;

    if (!t)
        return;
    if (s_gl_dirty || t != s_cur || target_draw_fbo(t) != s_bound_fbo
            || s->x != s_cur_surf.x || s->y != s_cur_surf.y
            || s->width != s_cur_surf.width || s->height != s_cur_surf.height) {
        glBindFramebuffer(GL_FRAMEBUFFER, target_draw_fbo(t));
        glViewport(0, 0, (GLsizei)t->pw, (GLsizei)t->ph);
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)scaled_x(s->x), (GLint)scaled_y(s->y),
                  (GLsizei)scaled_x(s->width), (GLsizei)scaled_y(s->height));
        s_bound_fbo = target_draw_fbo(t);
        s_state_valid = 0;            /* apply_state's memo is per target */
    }
    s_cur = t;
    s_cur_surf = *s;
    area = t->lw * t->lh;
    if (area >= s_present_area) {
        s_present = t;
        s_present_area = area;
    }
}

/* ── Textures ───────────────────────────────────────────────────────────── */

typedef struct {
    /* `pitch` is part of the identity, not decoration: for a linear format it
     * is the row stride the decoder walks, so the same offset and size bound
     * with a different pitch is a different image. Otogi binds its 640x480
     * A8R8G8B8 surfaces this way (pitch 2560), and a cache that ignored the
     * field would keep the first decode of an offset that a later bind reads
     * at another stride. */
    uint32_t offset, w, h, fmt, levels, pitch;
    uint32_t palette;     /* [thug] CLUT address for indexed formats */
    int      failed;      /* [thug] decode failed: bind white until the bytes change */
    int      mipped;      /* [thug] has a generated mip chain */
    GLuint   tex;
    int32_t  next;        /* [thug] next entry in the same hash bucket, -1 */
    int32_t  bucket;      /* [thug] the bucket it is linked into, -1 */
    uint64_t stamp;
    uint64_t hash;        /* source bytes at last upload, 0 = unchecked */
    /* Content checking is once per texture per frame, not once per bind.
     * A 640x480 surface is 1.2 MB with its pitch, the movie binds one several
     * hundred times a frame, and hashing on every bind dropped the title from
     * 24 fps to 0.33 fps once this check was added (drw1 vs stage2,
     * docs/BOOT_LOG.md 2026-10-30). Frame granularity is also the right one:
     * a buffer the title rewrites -- a decoded video frame, a render surface
     * copied back -- changes between frames, not between binds. */
    uint32_t hash_frame;  /* frame + 1 whose bytes `live` describes (0 = never) */
    uint64_t live;        /* hash sampled this frame */
    uint32_t upload_frame;/* frame + 1 of the last upload (streamed sources) */
} GlTex;

/* [thug] 4096 entries behind a hash on the identity. A level binds well
 * over 512 distinct textures, and the fork's 512-entry table, scanned
 * linearly on every bind, both thrashed (every eviction is a full decode on
 * the next bind) and cost a scan per bind. */
#define TEX_MAX     4096
#define TEX_BUCKETS 8192
static GlTex s_tex[TEX_MAX];
static int32_t s_tex_bucket[TEX_BUCKETS];
static int s_tex_init;
static uint64_t s_tex_clock;

/* Per-frame counters for THUG_GL_STATS. */
static struct {
    uint32_t draws, tris, uploads, evictions, binds, aliases, switches, polyoff, a2c, resolves;
    uint64_t upload_texels, hash_bytes;
} s_st;
static int s_stats;

static uint32_t tex_key_bucket(uint32_t offset, uint32_t w, uint32_t h,
                               uint32_t fmt)
{
    uint32_t k = offset * 2654435761u ^ (w << 16 | h) * 40503u ^ fmt * 97u;
    return (k ^ (k >> 15)) & (TEX_BUCKETS - 1);
}

static void tex_cache_init(void)
{
    uint32_t i;
    if (s_tex_init)
        return;
    for (i = 0; i < TEX_BUCKETS; i++)
        s_tex_bucket[i] = -1;
    for (i = 0; i < TEX_MAX; i++) {
        s_tex[i].next = -1;
        s_tex[i].bucket = -1;
    }
    s_tex_init = 1;
}

static void tex_unlink(GlTex *e)
{
    int32_t idx = (int32_t)(e - s_tex), *pp;

    if (e->bucket < 0)
        return;
    pp = &s_tex_bucket[e->bucket];
    while (*pp >= 0) {
        if (*pp == idx) {
            *pp = e->next;
            break;
        }
        pp = &s_tex[*pp].next;
    }
    e->next = -1;
    e->bucket = -1;
}

/* Link e under its current identity (offset, w, h, fmt). */
static void tex_link(GlTex *e)
{
    uint32_t b = tex_key_bucket(e->offset, e->w, e->h, e->fmt);

    tex_unlink(e);
    e->bucket = (int32_t)b;
    e->next = s_tex_bucket[b];
    s_tex_bucket[b] = (int32_t)(e - s_tex);
}

static GlTex *tex_find(const Nv2aTexture *t)
{
    int32_t i;

    tex_cache_init();
    for (i = s_tex_bucket[tex_key_bucket(t->offset, t->width, t->height, t->color)];
         i >= 0; i = s_tex[i].next) {
        GlTex *e = &s_tex[i];
        if ((e->tex || e->failed) && e->offset == t->offset && e->w == t->width
            && e->h == t->height && e->fmt == t->color
            && e->levels == t->levels && e->pitch == t->pitch
            && e->palette == t->palette)
            return e;
    }
    return NULL;
}

/* A free entry, or the least recently bound one, emptied and unlinked. */
static GlTex *tex_slot(void)
{
    static uint32_t s_used;           /* entries handed out so far */
    GlTex *victim;
    uint32_t i;

    tex_cache_init();
    if (s_used < TEX_MAX) {
        victim = &s_tex[s_used++];
    } else {
        victim = &s_tex[0];
        for (i = 1; i < TEX_MAX; i++)
            if (s_tex[i].stamp < victim->stamp)
                victim = &s_tex[i];
        s_st.evictions++;
    }
    tex_unlink(victim);
    if (victim->tex) {
        glDeleteTextures(1, &victim->tex);
        unit_cache_reset();
    }
    memset(victim, 0, sizeof *victim);
    victim->next = -1;
    victim->bucket = -1;
    return victim;
}

/* Every texture whose source has a pitch is hashed: the bytes live in one
 * contiguous pitch*height run whatever the addressing (a Morton index
 * permutes the same run), and a title may rewrite any of them while bound --
 * a streamed video frame, a render surface written back into a texture. The
 * terrain's 32x64 stage-0 texture is swizzled A8R8G8B8 and was first decoded
 * with A=0 in every texel; if a later write fills it, an upload-once cache
 * keeps the empty alpha and the alpha test discards the whole ground. */

/* FNV-1a over the texture's source bytes, word at a time. A title that
 * rewrites a buffer while it is bound -- a decoded video frame, a render
 * surface copied into a linear texture, a streamed mip -- is invisible to a
 * cache keyed on offset/size/format alone: the first decode wins and every
 * later frame shows that stale image. The XMV promo drew over itself in
 * black every frame this way (its X8R8G8B8 quad uploaded once before the
 * decoder's first write). Returns 0 for "nothing to hash" (no pitch, which
 * cannot describe a linear row range) -- and 0 never matches a stored hash,
 * because stored hashes are forced non-zero below. */
/* [thug] Bytes per texel of the uncompressed formats THUG binds; 4 for
 * anything not listed, which only over-hashes. */
static uint32_t tex_bpp(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B:
    case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x16: case 0x17: case 0x18: case 0x1A: case 0x1D: case 0x20:
    case 0x24: case 0x25: case 0x27: case 0x28: case 0x29: case 0x2C:
    case 0x2D: case 0x30: case 0x31: case 0x35: case 0x37:
        return 2;
    default:
        return 4;
    }
}

/* [thug] Level 0's size as the format lays it out. The fork hashed
 * pitch * height for everything, but the pitch register is only meaningful
 * for linear formats: a swizzled or DXT texture inherits whatever the last
 * linear one set (THUG's 128x128 DXT1 arrives with pitch 5120), so the hash
 * walked 80 times the texture and every write to the memory after it forced a
 * re-upload. */
static size_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color);

    if (bb)
        return (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * bb;
    if (!d3d8_format_is_swizzled(t->color) && t->pitch)
        return (size_t)t->pitch * t->height;
    return (size_t)t->width * t->height * tex_bpp(t->color);
}

/* FNV-1a over the texture's source bytes, so a texture rewritten in place
 * (a decoded video frame, a streamed mip, a re-filled palette) is uploaded
 * again. [thug] Up to 32 KB is hashed whole; above that, 4096 words spread
 * across it plus the last one -- a full walk of every bound texture every
 * frame cost more than drawing them. The palette of an indexed texture is
 * part of its content. Never returns 0. */
static uint64_t tex_source_hash(const Nv2aTexture *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    size_t len = tex_bytes(t), i, step;
    uint64_t h = 0xcbf29ce484222325ULL, v;

    if (!len || len > (64u << 20))
        return 1;
    s_st.hash_bytes += len <= 32768 ? len : 32768;
    p = mem + t->offset;
    if (len <= 32768) {
        for (i = 0; i + 8 <= len; i += 8) {
            memcpy(&v, p + i, 8);
            h = (h ^ v) * 0x100000001b3ULL;
        }
        for (; i < len; i++)
            h = (h ^ p[i]) * 0x100000001b3ULL;
    } else {
        step = (len / 4096) & ~(size_t)7;
        if (step < 8)
            step = 8;
        for (i = 0; i + 8 <= len; i += step) {
            memcpy(&v, p + i, 8);
            h = (h ^ v) * 0x100000001b3ULL;
        }
        memcpy(&v, p + len - 8, 8);
        h = (h ^ v) * 0x100000001b3ULL;
    }
    if (t->color == 0x0B && t->palette) {
        const uint8_t *pal = mem + t->palette;
        for (i = 0; i < 1024; i += 8) {
            memcpy(&v, pal + i, 8);
            h = (h ^ v) * 0x100000001b3ULL;
        }
    }
    return h ? h : 1;
}

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

static int tex_upload(GlTex *e, const Nv2aTexture *t)
{
    uint32_t *argb;
    size_t n = (size_t)t->width * t->height;
    uint8_t *rgba, *dst;
    size_t i;

    if (!n || n > (64u << 20))
        return 0;
    argb = (uint32_t *)malloc(n * 4);
    rgba = (uint8_t *)malloc(n * 4);
    if (!argb || !rgba) {
        free(argb);
        free(rgba);
        return 0;
    }
    if (!nv2a_backend_decode_texture(t, argb)) {
        free(argb);
        free(rgba);
        return 0;
    }
    /* [thug] THUG_GL_TEXDUMP=<guest address>: write that texture's decode as
     * a PPM (and its alpha as a second one) on every upload, to tell a bad
     * decode from a bad draw. */
    {
        static uint32_t want = 1;
        if (want == 1) {
            const char *td = getenv("THUG_GL_TEXDUMP");
            want = td ? (uint32_t)strtoul(td, NULL, 16) : 0;
        }
        if (want && want == t->offset) {
            static unsigned seq;
            char path[256];
            FILE *f;
            int pass;
            for (pass = 0; pass < 2; pass++) {
                snprintf(path, sizeof path, "texdump_%08X_%u%s.ppm", t->offset,
                         seq, pass ? "_a" : "");
                f = fopen(path, "wb");
                if (!f)
                    break;
                fprintf(f, "P6\n%u %u\n255\n", t->width, t->height);
                for (i = 0; i < n; i++) {
                    uint8_t px[3];
                    uint32_t c = argb[i];
                    if (pass) {
                        px[0] = px[1] = px[2] = (uint8_t)(c >> 24);
                    } else {
                        px[0] = (uint8_t)(c >> 16);
                        px[1] = (uint8_t)(c >> 8);
                        px[2] = (uint8_t)c;
                    }
                    fwrite(px, 1, 3, f);
                }
                fclose(f);
            }
            seq++;
        }
    }
    /* AARRGGBB -> RGBA bytes, row-major from the decoder. */
    dst = rgba;
    for (i = 0; i < n; i++) {
        uint32_t c = argb[i];
        dst[0] = (uint8_t)(c >> 16);
        dst[1] = (uint8_t)(c >> 8);
        dst[2] = (uint8_t)c;
        dst[3] = (uint8_t)(c >> 24);
        dst += 4;
    }
    if (!e->tex)
        glGenTextures(1, &e->tex);
    {
        int au = s_active_unit;
        unit_cache_reset();
        s_active_unit = au;           /* unchanged by this bind */
    }
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)t->width,
                 (GLsizei)t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    /* [thug] A mip chain for every swizzled or compressed (power-of-two)
     * texture, generated from level 0: the decoder reads level 0 only, and
     * at several times the Xbox's resolution an unmipped texture shimmers.
     * tex_params uses it when the title asked for mip filtering. */
    e->mipped = d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color);
    e->mipped = e->mipped && is_pow2(t->width) && is_pow2(t->height)
                && (t->width > 1 || t->height > 1)
                && !getenv("THUG_GL_NO_MIPS");          /* A/B switch */
    if (e->mipped) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
        glGenerateMipmap(GL_TEXTURE_2D);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    if (s_bind_log)
        fprintf(stderr, "[PBGLU] off %08X %ux%u fmt %02X mid %08X\n",
                t->offset, t->width, t->height, t->color, argb[n / 2]);
    free(argb);
    free(rgba);

    e->offset = t->offset;
    e->palette = t->palette;
    e->failed = 0;
    s_cur_uploads++;
    s_st.uploads++;
    s_st.upload_texels += n;
    e->w = t->width;
    e->h = t->height;
    e->fmt = t->color;
    e->levels = t->levels;
    e->pitch = t->pitch;
    e->stamp = ++s_tex_clock;
    tex_link(e);
    if (s_verbose)
        fprintf(stderr, "[PBGL] upload %08X %ux%u fmt %02X\n",
                t->offset, t->width, t->height, t->color);
    return 1;
}

/* Address mode and filter, as the CPU sampler applies them: the NV2A's 1 is
 * wrap, 2 mirror, everything else clamps; MAG 2 is tent, MIN 2/4/6 is tent at
 * level 0, everything else nearest. [thug] As sampler objects, made once per
 * combination and bound per unit, instead of five texture parameters set on
 * every bind. MIN 3-6 are the mip-filtered modes: with a chain to use, filter
 * trilinearly with anisotropy; without one, level 0 as the fork did. */
static GLuint sampler_for(const Nv2aTexture *t, int mipped)
{
    static struct { uint32_t key; GLuint smp; } cache[64];
    static int n;
    GLint u = GL_REPEAT, v = GL_REPEAT, f, minf;
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    uint32_t key, aniso = 1;
    GLuint smp;
    int i, mip;

    if (t->addr_u == 2) u = GL_MIRRORED_REPEAT;
    else if (t->addr_u != 1) u = GL_CLAMP_TO_EDGE;
    if (t->addr_v == 2) v = GL_MIRRORED_REPEAT;
    else if (t->addr_v != 1) v = GL_CLAMP_TO_EDGE;
    f = (mag == 2 || min == 2 || min == 4 || min == 6) ? GL_LINEAR : GL_NEAREST;
    mip = mipped && min >= 3 && min <= 6;
    minf = mip ? GL_LINEAR_MIPMAP_LINEAR : f;
    if (mip && s_aniso > 1.0f)
        aniso = (uint32_t)s_aniso;
    key = (uint32_t)(u == GL_REPEAT ? 0 : u == GL_MIRRORED_REPEAT ? 1 : 2)
        | (uint32_t)(v == GL_REPEAT ? 0 : v == GL_MIRRORED_REPEAT ? 1 : 2) << 2
        | (uint32_t)(f == GL_LINEAR) << 4 | (uint32_t)mip << 5 | aniso << 8;
    for (i = 0; i < n; i++)
        if (cache[i].key == key)
            return cache[i].smp;
    glGenSamplers(1, &smp);
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, u);
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, v);
    glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, f);
    glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, minf);
    if (s_aniso_max > 1.0f)
        glSamplerParameterf(smp, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)aniso);
    if (n < 64) {
        cache[n].key = key;
        cache[n].smp = smp;
        n++;
    }
    return smp;
}

/* Which stages a draw has a texture bound on: the combiner shader samples
 * them, and a stage with none reads as the CPU sampler's failure colour (white)
 * rather than an unbound GL texture. */
static int s_stage_ok[4];

/* A texture whose content the title rewrites while it is bound, so a cached
 * decode is wrong the moment it is made. YUY2/UYVY is the decoded video frame:
 * the movie is drawn as a quad whose texture is refilled every frame (often
 * from a double buffer, alternating two offsets). Upload-once showed the first
 * frame forever -- the promo stood still on the GL back end while the CPU
 * rasteriser, which samples live memory per pixel, played it. */
static int tex_streamed(const Nv2aTexture *t)
{
    return t->color == 0x24 || t->color == 0x25;   /* YUY2 / UYVY */
}

/* Bind one texture stage, uploading the texture the first time it is seen.
 * Returns 0 when there is nothing to bind (or the format cannot be decoded),
 * which the caller reports to the shader as "stage off". */
/* [thug] Does an NV2A texture format carry alpha? The colour formats
 * without it read alpha 1 (nv2a_pb_exec.c's decoder ORs in 0xFF000000). */
static int fmt_without_alpha(uint32_t fmt)
{
    switch (fmt) {
    case 0x03: case 0x1C:             /* X1R5G5B5, LIN_X1R5G5B5 */
    case 0x05: case 0x11:             /* R5G6B5, LIN_R5G6B5 */
    case 0x07: case 0x1E:             /* X8R8G8B8, LIN_X8R8G8B8 */
        return 1;
    default:
        return 0;
    }
}

static int tex_format_has_alpha(uint32_t fmt)
{
    static int raw = -1;              /* THUG_GL_RT_RAW_ALPHA=1: the A/B switch */
    if (raw < 0)
        raw = getenv("THUG_GL_RT_RAW_ALPHA") != NULL;
    return raw || !fmt_without_alpha(fmt);
}

static int stage_bind(uint32_t unit, const Nv2aTexture *t, uint32_t frame)
{
    GlTex *e;
    uint32_t stamp = frame + 1;
    int linear = t && !d3d8_format_is_swizzled(t->color)
                   && !d3d8_format_dxt_block_bytes(t->color);

    s_stage_sc[unit][0] = s_stage_sc[unit][1] = 1.0f;
    if (linear && t->width && t->height) {
        s_stage_sc[unit][0] = 1.0f / (float)t->width;
        s_stage_sc[unit][1] = 1.0f / (float)t->height;
    }

    /* [thug] A texture whose address is a surface this back end renders is
     * that target: bind its GL texture rather than decode guest memory, which
     * only a readback would ever update. THUG resolves its 1280x480
     * anti-aliased buffer into the front buffer this way every frame. */
    if (t) {
        GlTarget *rt = target_by_va(t->offset);
        if (rt && rt != s_cur) {
            s_st.aliases++;
            /* The target may cover more than this texture's extent (it grows
             * to the largest clip drawn into it): normalise to the target. */
            float tw = (float)(t->width ? t->width : rt->lw);
            float th = (float)(t->height ? t->height : rt->lh);
            if (linear) {
                s_stage_sc[unit][0] = 1.0f / (float)rt->lw;
                s_stage_sc[unit][1] = 1.0f / (float)rt->lh;
            } else {
                s_stage_sc[unit][0] = tw / (float)rt->lw;
                s_stage_sc[unit][1] = th / (float)rt->lh;
            }
            bind_unit(unit, rt->tex, sampler_for(t, 0));
            /* [thug] A format without alpha (X8R8G8B8, R5G6B5, X1R5G5B5)
             * reads alpha 1 on the NV2A, whatever byte the surface holds; the
             * decoder does the same for textures from guest memory. The GL
             * texture holds whatever alpha the target's draws left. THUG's
             * reset transition (bailing into water, going out of bounds)
             * renders the scene into an X8R8G8B8 target and lays it over the
             * last frames at vertex alpha x texture alpha: with the real alpha
             * mostly near 0, old frames stayed on screen as sharp repeated
             * copies instead of fading into a blur. */
            {
                int one = !tex_format_has_alpha(t->color);
                /* THUG_GL_CAPTURE_ON_X8=1: capture the first frames that
                 * blend an alpha-less target over SRC_ALPHA (the reset
                 * transition's composite; diagnosis of this path). */
                static int on_x8 = -1;
                if (on_x8 < 0)
                    on_x8 = getenv("THUG_GL_CAPTURE_ON_X8") != NULL;
                if (on_x8 == 1 && fmt_without_alpha(t->color)
                        && s_last_rs.blend_enable && s_last_rs.blend_src == 0x0302) {
                    on_x8 = 2;
                    nv2a_pb_gl_request_capture();
                }
                if (one != rt->alpha_one) {
                    if (s_active_unit != (int)unit) {
                        glActiveTexture(GL_TEXTURE0 + unit);
                        s_active_unit = (int)unit;
                    }
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
                                    one ? GL_ONE : GL_ALPHA);
                    rt->alpha_one = one;
                }
            }
            if (s_bind_log)
                fprintf(stderr, "[PBGLB] f%u u%u off %08X target alias\n",
                        frame, unit, t->offset);
            return 1;
        }
    }
    e = t ? tex_find(t) : NULL;

    /* Streamed source: re-check the bytes behind a cached decode, so a
     * buffer rewritten while bound is re-uploaded instead of frozen. The
     * hash is taken at most once per texture per frame (see GlTex), and a
     * streamed source is re-uploaded at most once per frame for the same
     * reason: both walk pitch*height bytes. */
    if (t && e) {
        if (e->hash_frame != stamp) {
            e->hash_frame = stamp;
            e->live = tex_source_hash(t);
            if (!e->live)
                e->live = 1;             /* no pitch: nothing to compare */
        }
        if (e->live != e->hash)
            e = NULL;                    /* content changed: re-upload */
    }
    if (s_bind_log && t)
        fprintf(stderr, "[PBGLB] f%u u%u off %08X %ux%u fmt %02X pitch %u %s\n",
                frame, unit, t->offset, t->width, t->height, t->color,
                t->pitch, e ? "hit" : "miss");
    if (!t)
        return 0;                      /* the caller binds white */
    if (e && e->failed) {
        s_cur_fail++;
        /* [thug] Known not to decode, and the bytes have not changed: do not
         * retry (and evict a good texture for it) on every bind. */
        e->stamp = ++s_tex_clock;
        return 0;
    }
    /* An upload binds on the active unit: make that this stage's own, so it
     * cannot displace a texture an earlier stage of this draw just bound. */
    if (s_active_unit != (int)unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        s_active_unit = (int)unit;
    }
    if (!e) {
        GlTex *old = tex_find(t);
        e = old ? old : tex_slot();
        if (!tex_upload(e, t)) {
            static uint32_t complained;
            s_cur_fail++;
            if (complained++ < 8)
                fprintf(stderr, "[PBGL] texture %08X %ux%u fmt %02X will not "
                                "decode; drawn untextured\n", t->offset,
                        t->width, t->height, t->color);
            if (e->tex) {
                glDeleteTextures(1, &e->tex);
                e->tex = 0;
                unit_cache_reset();
            }
            e->offset = t->offset;
            e->w = t->width;
            e->h = t->height;
            e->fmt = t->color;
            e->levels = t->levels;
            e->pitch = t->pitch;
            e->palette = t->palette;
            e->failed = 1;
            e->hash = e->live = tex_source_hash(t);
            e->hash_frame = stamp;
            e->stamp = ++s_tex_clock;
            tex_link(e);
            return 0;
        }
        e->hash = tex_source_hash(t);
        if (!e->hash)
            e->hash = 1;
        e->live = e->hash;
        e->hash_frame = stamp;
        e->upload_frame = stamp;
    } else if ((tex_streamed(t) && e->upload_frame != stamp)
               || (s_tex_refresh && frame && (frame % s_tex_refresh) == 0)) {
        tex_upload(e, t);
        e->hash = tex_source_hash(t);
        if (!e->hash)
            e->hash = 1;
        e->live = e->hash;
        e->hash_frame = stamp;
        e->upload_frame = stamp;
    }
    e->stamp = ++s_tex_clock;
    bind_unit(unit, e->tex, sampler_for(t, e->mipped));
    return 1;
}

/* ── State ──────────────────────────────────────────────────────────────── */

/* Does this shader evaluate the stage's mode? Every mode the CPU rasteriser
 * implements is now handled by rc_stage(), which follows nv2a_pb_exec.c's
 * rc_stage_fetch() table; there is nothing left to refuse, so the combiner
 * runs for every batch that has one. */
/* Which stage modes the title uses, counted across every batch of a run and
 * printed once, so "a mode this shader samples as 2D" can be told from "a mode
 * that reads zero" rather than guessed from one wrong-looking model. */
static uint32_t s_mode_hist[32];

/* Per-batch combiner state -> uniforms. nv2a_rc_eval's inputs, decoded the
 * same way the CPU rasteriser decodes them: the ICW bytes go up as they are
 * (the shader's rc_in/rc_rgb apply the mapping), the output registers packed
 * three nibbles at a time, and the factors resolved per stage here so the
 * shader needs no decision about COMBINERCOUNT. */
static Nv2aCombiner s_rc_last;        /* [thug] last uploaded combiner */
static int s_u_rc_on = -1, s_u_textured = -1;
static void set_rc_on(int v) { if (v != s_u_rc_on) { glUniform1i(u_rc_on, v); s_u_rc_on = v; } }
static void set_textured(int v) { if (v != s_u_textured) { glUniform1i(u_textured, v); s_u_textured = v; } }
static uint32_t s_rc_last_clip, s_rc_last_fog;
static int s_rc_valid, s_no_comb = -1;

/* [thug] Bump-env stages are rare (THUG: water and rain screen effects, a
 * second at a time), so the first few distinct set-ups go to the log
 * unasked: the record to check their coordinate space against. */
static uint32_t s_frame;               /* defined with the flip code below */

static void bump_log(const Nv2aBatch *b, uint32_t st, uint32_t mode)
{
    static uint32_t seen[12];
    static int n;
    const Nv2aCombiner *rc = b->combiner;
    const Nv2aTexture *t = b->textures[st];
    uint32_t key, k;
    int linear;

    if (n >= (int)(sizeof seen / sizeof seen[0]))
        return;
    key = (t ? t->offset : 0) ^ (st << 28) ^ (mode << 24);
    for (k = 0; k < (uint32_t)n; k++)
        if (seen[k] == key)
            return;
    seen[n++] = key;
    linear = t && !d3d8_format_is_swizzled(t->color)
             && !d3d8_format_dxt_block_bytes(t->color);
    fprintf(stderr, "[PBGL] bump-env: flip %u stage %u mode %u tex %08X %ux%u fmt %02X %s%s"
            " addr %u/%u matrix %g %g %g %g lum %g %g st0 %g %g q %g\n",
            (unsigned)s_frame, st, mode, t ? t->offset : 0, t ? t->width : 0,
            t ? t->height : 0, t ? t->color : 0, linear ? "linear" : "swizzled",
            t && target_by_va(t->offset) ? ", a render target" : "",
            t ? t->addr_u : 0, t ? t->addr_v : 0,
            (double)rc->bump_mat[st][0], (double)rc->bump_mat[st][1],
            (double)rc->bump_mat[st][2], (double)rc->bump_mat[st][3],
            (double)rc->bump_scale[st], (double)rc->bump_offset[st],
            (double)b->vertices[0].st[st][0], (double)b->vertices[0].st[st][1],
            (double)b->vertices[0].st[st][3]);
}

static int rc_upload(const Nv2aBatch *b, const Nv2aVertex *v)
{
    const Nv2aCombiner *rc = b->combiner;
    uint32_t cin[8][4], ain[8][4], cout[8][2], flags[8][2];
    float c0[8][4], c1[8][4], f0[4], f1[4];
    int on = 0, i;
    uint32_t n, ctl, per_stage_c0, per_stage_c1;

    (void)v;
    /* The CPU rasteriser evaluates the combiner whenever the title has
     * programmed one (rc_seen), and falls back to modulate only otherwise.
     * Matching that exactly is the point: a batch whose combiner reads (0,0,0,1)
     * for an off stage is a different draw from "diffuse times texture". */
    if (!rc)
        goto off;
    for (i = 0; i < 4; i++) {
        uint32_t m = (rc->stage_program >> (i * 5)) & 0x1F;
        s_mode_hist[m]++;
        if (m == 6 || m == 7)
            bump_log(b, (uint32_t)i, m);
    }
    if (s_no_comb < 0)
        s_no_comb = getenv("RECOMP_GL_NO_COMB") != NULL;
    /* [thug] The combiner uniforms are a few hundred bytes of state that
     * rarely changes between neighbouring draws: upload only on change. */
    if (s_rc_valid && s_rc_last_clip == b->clip_plane_mode
            && s_rc_last_fog == b->fog_color
            && memcmp(&s_rc_last, rc, sizeof *rc) == 0) {
        set_rc_on(s_no_comb ? 0 : 1);
        set_textured(b->textures[0] && s_stage_ok[0] ? 1 : 0);
        return 1;
    }
    s_rc_last = *rc;
    s_rc_last_clip = b->clip_plane_mode;
    s_rc_last_fog = b->fog_color;
    s_rc_valid = 1;
    /* Every stage mode runs the combiner. Refusing the batch to the fallback
     * (the old "mode not implemented" exit) cost the title its player model
     * and its bamboo: those batches use modes this shader samples as a plain
     * 2D lookup, and disabling the combiner painted them v0 times a texel,
     * which is black. rc_stage() now implements the same table as the CPU
     * rasteriser's rc_stage_fetch(), so the modes that read zero and the one
     * that kills the pixel do so here too. */
    ctl = rc->control;
    n = ctl & 0xFF;
    if (n > 8)
        n = 8;
    per_stage_c0 = (ctl >> 8) & 0x10;    /* COMBINERCOUNT bit for c0 */
    per_stage_c1 = (ctl >> 8) & 0x100;   /* ... and for c1 */
    for (i = 0; i < 8; i++) {
        uint32_t s = (uint32_t)i;
        nv2a_rc_unpack(rc->factor0[per_stage_c0 ? s : 0], c0[i]);
        nv2a_rc_unpack(rc->factor1[per_stage_c1 ? s : 0], c1[i]);
        cin[i][0] = (rc->color_icw[i] >> 24) & 0xFF;
        cin[i][1] = (rc->color_icw[i] >> 16) & 0xFF;
        cin[i][2] = (rc->color_icw[i] >> 8) & 0xFF;
        cin[i][3] = rc->color_icw[i] & 0xFF;
        ain[i][0] = (rc->alpha_icw[i] >> 24) & 0xFF;
        ain[i][1] = (rc->alpha_icw[i] >> 16) & 0xFF;
        ain[i][2] = (rc->alpha_icw[i] >> 8) & 0xFF;
        ain[i][3] = rc->alpha_icw[i] & 0xFF;
        cout[i][0] = rc->color_ocw[i];
        cout[i][1] = rc->alpha_ocw[i];
        flags[i][0] = rc->color_ocw[i] >> 12;
        flags[i][1] = rc->alpha_ocw[i] >> 12;
    }
    nv2a_rc_unpack(rc->final_c0, f0);
    nv2a_rc_unpack(rc->final_c1, f1);
    {
        /* SET_CLIP_PLANE_MODE: one bit per stage per coordinate channel. */
        uint32_t clip[8][4];
        int j;
        for (i = 0; i < 8; i++)
            for (j = 0; j < 4; j++)
                clip[i][j] = (b->clip_plane_mode >> (i * 4 + j)) & 1u;
        glUniform4uiv(u_rc_clip, 8, (const GLuint *)clip);
    }
    on = 1;

    glUniform1i(u_rc_stages, (int)n);
    glUniform1i(u_rc_muxalpha, (int)((ctl >> 8) & 1));
    glUniform1ui(u_rc_stpar, rc->stage_program);
    {
        /* The bump-env matrix, or identity for a stage the title never
         * programmed: the shader always multiplies by it, and an all-zero
         * matrix would collapse the barycentric offsets a stage-6 sample
         * needs. Scale/offset only matter for BUMPENVMAP_LUMINANCE. */
        float bm[4][4], so[4][2];
        unsigned k;
        for (k = 0; k < 4; k++) {
            if (rc->bump_valid & (1u << k)) {
                bm[k][0] = rc->bump_mat[k][0];
                bm[k][1] = rc->bump_mat[k][1];
                bm[k][2] = rc->bump_mat[k][2];
                bm[k][3] = rc->bump_mat[k][3];
                so[k][0] = rc->bump_scale[k];
                so[k][1] = rc->bump_offset[k];
            } else {
                bm[k][0] = 1.0f; bm[k][1] = 0.0f;
                bm[k][2] = 0.0f; bm[k][3] = 1.0f;
                so[k][0] = 1.0f; so[k][1] = 0.0f;
            }
        }
        glUniform4fv(u_rc_bump, 4, (const GLfloat *)bm);
        glUniform2fv(u_rc_bump_so, 4, (const GLfloat *)so);
    }
    glUniform4uiv(u_rc_cin, 8, (const GLuint *)cin);
    glUniform4uiv(u_rc_ain, 8, (const GLuint *)ain);
    glUniform2uiv(u_rc_cout, 8, (const GLuint *)cout);
    glUniform2uiv(u_rc_flags, 8, (const GLuint *)flags);
    glUniform4fv(u_rc_c0, 8, (const GLfloat *)c0);
    glUniform4fv(u_rc_c1, 8, (const GLfloat *)c1);
    glUniform2ui(u_rc_final, rc->final0, rc->final1);
    glUniform4fv(u_rc_fc0, 1, f0);
    glUniform4fv(u_rc_fc1, 1, f1);
    {
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        float fogc[4];
        nv2a_rc_unpack(b->fog_color, fogc);
        { float r = fogc[2]; fogc[2] = fogc[0]; fogc[0] = r; }
        glUniform4fv(u_rc_fog, 1, fogc);
    }
    /* RECOMP_GL_NO_COMB: skip the register combiners and modulate stage 0 by
     * the vertex colour, which is the back end's original pixel stage. A
     * scene that paints black with the combiner on and lit without it has a
     * combiner problem; one that is black either way has a colour or texture
     * problem. */
    set_rc_on(s_no_comb ? 0 : on);
    /* Stage 0's texture is also the one the fallback path samples. */
    set_textured(b->textures[0] && s_stage_ok[0] ? 1 : 0);
    return on;

  off:
    /* No combiner (or one whose stage modes this shader does not evaluate):
     * the batch's pixel stage is the old modulate, which is what the CPU
     * rasteriser does for the same batch. The texture has to stay bound for
     * it -- leaving uTextured at apply_state's 0 drew every such batch in flat
     * vertex colour, and a stage-0 texture with it. */
    set_rc_on(0);
    set_textured(b->textures[0] && s_stage_ok[0] ? 1 : 0);
    return 0;
}

/* Diagnostic overrides, all off unless the environment asks for them: each
 * one forces a raster state open so a scene that draws too little can be
 * bisected (is it the cull, the depth test, the alpha test?). They are how
 * the 2026-10-27 stage capture was narrowed down and are kept for the next
 * title whose world comes out half drawn. */
static int s_no_cull, s_no_depth, s_no_blend, s_no_alpha, s_allchan;
static int s_no_polyoff = -1;         /* [thug] THUG_GL_NO_POLYOFF: A/B switch */
static int s_front_mirror;

static void state_env_init(void)
{
    static time_t last;
    time_t now = time(NULL);

    if (now == last)
        return;
    last = now;
    s_no_cull  = getenv("RECOMP_GL_NO_CULL") && *getenv("RECOMP_GL_NO_CULL")
                 && *getenv("RECOMP_GL_NO_CULL") != '0';   /* [thug] 0 = cull */
    s_no_depth = getenv("RECOMP_GL_NO_DEPTH") != NULL;
    s_no_blend = getenv("RECOMP_GL_NO_BLEND") != NULL;
    s_no_alpha = getenv("RECOMP_GL_NO_ALPHA") != NULL;
    s_allchan  = getenv("RECOMP_GL_ALLCHAN")  != NULL;
    /* RECOMP_GL_FRONT_MIRROR: the opposite front-face convention. The title's
     * SET_FRONT_FACE is carried through unchanged because window-space
     * winding is a picture property: the vertex shader's y flip puts a vertex
     * on the same pixel the NV2A puts it on, so the name means the same thing
     * on both. This exists to prove that on a capture -- a title whose culled
     * geometry comes out inside-out under one convention and right under the
     * other says which. */
    s_front_mirror = getenv("RECOMP_GL_FRONT_MIRROR") != NULL;
    /* RECOMP_GL_FLAT: paint every surviving fragment one colour. Depth, cull
     * and alpha test still apply, so what is magenta is geometry that is
     * drawn and passes every gate -- the answer to "is the missing thing not
     * submitted, or not painted". */
    glUniform1i(u_flat, getenv("RECOMP_GL_FLAT") != NULL ? 1 : 0);
    glUniform1i(u_alphaone, getenv("RECOMP_GL_ALPHA_ONE") != NULL ? 1 : 0);
}

static void apply_state(const Nv2aRenderState *rs, const Nv2aSurface *s,
                        uint32_t frame)
{
    static const uint32_t none = 0;

    state_env_init();
    {   /* THUG_GL_POLYOFF_LOG also lists each SET_CONTROL0 value once */
        static int lg = -1, n;
        static uint32_t seen[16];
        int k;
        if (lg < 0) lg = getenv("THUG_GL_POLYOFF_LOG") != NULL;
        if (lg) {
            for (k = 0; k < n && seen[k] != rs->control0; k++)
                ;
            if (k == n && n < 16) {
                seen[n++] = rs->control0;
                fprintf(stderr, "[CONTROL0] %08X (zeta fmt %X)\n", rs->control0,
                        rs->zeta_format);
            }
        }
    }
    /* [thug] Same state on the same target as the last draw: GL already
     * holds all of it. Only the stage-0 reset at the end is per draw. */
    (void)frame;
    if (!s_gl_dirty && s_state_valid
            && memcmp(&s_last_rs, rs, sizeof *rs) == 0)
        return;
    s_last_rs = *rs;
    s_state_valid = 1;
    s_gl_dirty = 0;
    /* [thug] The target covers the surface from (0,0) at its logical size. */
    glUniform2f(u_surf, (float)(s_cur ? s_cur->lw : s->width),
                (float)(s_cur ? s_cur->lh : s->height));
    glUniform2f(u_origin, 0.0f, 0.0f);
    glUniform1f(u_incz, 1.0f / (float)z_divisor(rs));

    if (rs->blend_enable && !s_no_blend) {
        glEnable(GL_BLEND);
        glBlendFunc(rs->blend_src, rs->blend_dst);   /* NV2A carries GL's
                                                      * own factor enums */
        glBlendEquation(rs->blend_eq);
        {
            uint32_t c = rs->blend_color;
            glBlendColor((float)((c >> 16) & 0xFF) / 255.0f,
                         (float)((c >>  8) & 0xFF) / 255.0f,
                         (float)( c        & 0xFF) / 255.0f,
                         (float)((c >> 24) & 0xFF) / 255.0f);
        }
    } else {
        glDisable(GL_BLEND);
    }

    if (s_no_depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
    } else {
        if (rs->depth_test_enable) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        glDepthFunc(rs->depth_func ? rs->depth_func : 0x207);
        glDepthMask(rs->depth_write ? GL_TRUE : GL_FALSE);
    }

    glUniform1i(u_alphatest,
                (rs->alpha_test_enable && !s_no_alpha) ? 1 : 0);
    glUniform1i(u_alphafunc, (int)rs->alpha_func);
    glUniform1f(u_alpharef, (float)(rs->alpha_ref & 0xFF) / 255.0f);
    /* [thug] Coverage from alpha for an alpha-tested draw into a
     * multisampled target (the shader's uA2C). */
    /* THUG never turns blending off: opaque surfaces blend ONE, ZERO, and its
     * cutouts (fences, foliage, hair) blend SRC_ALPHA, ONE_MINUS_SRC_ALPHA
     * under an alpha test whose reference cuts the edge. Those two take
     * coverage from alpha; additive, modulating and constant-alpha blends
     * keep the plain test. */
    {
        int opaque = !rs->blend_enable || s_no_blend
                     || (rs->blend_src == 0x0001 && rs->blend_dst == 0x0000);
        int over = rs->blend_src == 0x0302 && rs->blend_dst == 0x0303;
        int cuts = rs->alpha_func == 0x204                      /* GREATER */
                   || (rs->alpha_func == 0x206 && (rs->alpha_ref & 0xFF));  /* GEQUAL > 0 */
        s_a2c_on = s_sample_mask_ok && s_cur && s_cur->samples > 1
                   && rs->alpha_test_enable && !s_no_alpha && (opaque || over) && cuts;
    }
    glUniform1i(u_a2c, s_a2c_on ? (GLint)s_cur->samples : 0);

    if (s_allchan) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    } else {
        uint32_t m = rs->color_mask;
        glColorMask((m >> 16) & 1, (m >> 8) & 1, m & 1, (m >> 24) & 1);
    }

    if (rs->cull_enable && !s_no_cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(rs->cull_face ? rs->cull_face : 0x0405);
        /* SET_FRONT_FACE names the winding the title calls front, and the
         * rasteriser has to keep that set. It used to be written mirrored on
         * the theory that the vertex shader's y flip (which does invert the
         * winding) had to be paid back here; that is the wrong end of the
         * pipeline -- the flip makes window-space winding match the NV2A's
         * screen-space one, so the name carries over unchanged. Mirrored, the
         * stage drew the inside of every closed mesh: the bamboo trunks were
         * hollow black wedges, the player was his own back faces, and the
         * ground (a single up-facing sheet) was culled away completely. That
         * was exactly what the 2026-10-28 capture with RECOMP_GL_NO_CULL=1
         * proved: culling off restored the trunks and the ground. */
        /* [thug] Without the vertex shader's y flip, window-space winding is
         * the mirror of the NV2A's screen-space winding, so the name flips. */
        glFrontFace(((rs->front_face == 0x0900) == s_front_mirror)
                    ? 0x0900 : 0x0901);
    } else {
        glDisable(GL_CULL_FACE);
    }

    /* [thug] Depth bias. The bias is in the zeta format's units and this
     * depth buffer is always 24-bit, so a Z16 bias is 256 of ours; the slope
     * term is per surface pixel, and a pixel here is 1/s_scale of one. */
    if (s_no_polyoff < 0)
        s_no_polyoff = getenv("THUG_GL_NO_POLYOFF") != NULL;
    if (rs->poly_offset_fill && !s_no_polyoff && (rs->poly_offset_factor != 0.0f
                                 || rs->poly_offset_units != 0.0f)) {
        float units = rs->poly_offset_units * (16777216.0f / (float)z_divisor(rs));
        float sc = s_scale_x > s_scale_y ? s_scale_x : s_scale_y;
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(rs->poly_offset_factor * sc, units);
        s_st.polyoff++;
        {   /* THUG_GL_POLYOFF_LOG=1: each distinct offset the title uses, once */
            static int lg = -1;
            static uint32_t seen[32][3];
            static int ns;
            if (lg < 0) lg = getenv("THUG_GL_POLYOFF_LOG") != NULL;
            if (lg) {
                uint32_t fb, ub, k;
                memcpy(&fb, &rs->poly_offset_factor, 4);
                memcpy(&ub, &rs->poly_offset_units, 4);
                for (k = 0; k < (uint32_t)ns; k++)
                    if (seen[k][0] == fb && seen[k][1] == ub && seen[k][2] == rs->zeta_format)
                        break;
                if (k == (uint32_t)ns && ns < 32) {
                    seen[ns][0] = fb; seen[ns][1] = ub; seen[ns][2] = rs->zeta_format;
                    ns++;
                    fprintf(stderr, "[POLYOFF] factor %g units %g zeta fmt %X depth func %X"
                            " write %u -> GL %g %g\n", rs->poly_offset_factor,
                            rs->poly_offset_units, rs->zeta_format, rs->depth_func,
                            rs->depth_write, rs->poly_offset_factor * sc, units);
                }
            }
        }
        if (s_verbose) {
            static int said;
            if (!said++)
                fprintf(stderr, "[PBGL] polygon offset in use: factor %g bias %g\n",
                        rs->poly_offset_factor, rs->poly_offset_units);
        }
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (rs->stencil_enable) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(rs->stencil_func, (int)rs->stencil_ref,
                      rs->stencil_func_mask);
        glStencilOp(rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass);
        glStencilMask(rs->stencil_write_mask);
    } else {
        glDisable(GL_STENCIL_TEST);
    }

    (void)none;
}

/* ── The back end ───────────────────────────────────────────────────────── */

static uint32_t s_frame;
static int s_zeta_log;                /* THUG_GL_ZETA_LOG=1: shared-zeta clears */
static int s_capture_left;            /* the glitch capture (below) */
static unsigned s_capture_draw;

static void gl_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    GlTarget *t;
    GLbitfield bits = 0;
    uint32_t zf;

    if (!s_ready)
        return;
    if (!gl_ready())
        return;
    t = target_for(s, rs);
    if (!t)
        return;
    target_bind(t, s);
    apply_state(rs, s, s_frame);
    /* [thug] CLEAR_SURFACE writes what its own flags name (bit 0 Z, bit 1
     * stencil, bits 4-7 R, G, B, A), whatever the draw state's write masks
     * say -- and glClear honours those masks. THUG clears with depth writes
     * off, so its depth buffer was never cleared and every 3D draw failed
     * the depth test against stale values. */
    glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0,
                (flags & 0x40) != 0, (flags & 0x80) != 0);
    glDepthMask((flags & 0x01) ? GL_TRUE : GL_FALSE);
    glStencilMask((flags & 0x02) ? 0xFF : 0x00);
    s_state_valid = 0;

    if (flags & 0xF0) {                      /* colour */
        glClearColor((float)((argb >> 16) & 0xFF) / 255.0f,
                     (float)((argb >>  8) & 0xFF) / 255.0f,
                     (float)( argb        & 0xFF) / 255.0f,
                     (float)((argb >> 24) & 0xFF) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    zf = (rs->zeta_format >> 4) & 0xF;
    if (flags & 0x01) {                      /* depth */
        double z = (zf == 1 ? (double)(zstencil & 0xFFFF)
                            : (double)(zstencil >> 8)) / (double)z_divisor(rs);
        /* Print on change: the depth clear's VALUE decides what every later
         * comparison can pass against, and "wrong units" lives right here. */
        {
            static int seen = -1;
            int key = (int)(z * 1e6) ^ (flags << 8) ^ (zf << 4)
                    ^ (rs->zeta_format << 1);
            if (seen < 0) {
                const char *e = getenv("RECOMP_GL_STATE");
                seen = e && *e ? 1 : 0;
            }
            if (seen && key != seen) {
                seen = key ? key : 1;
                fprintf(stderr, "[GLST] clear flags %X zstencil %08X -> z %f"
                        " (zf %u div %u) argb %08X zfmt %X\n",
                        flags, zstencil, z, zf, z_divisor(rs), argb,
                        rs->zeta_format);
            }
        }
        glClearDepth(z < 0.0 ? 0.0 : (z > 1.0 ? 1.0 : z));
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 0x02) {                      /* stencil */
        glClearStencil((int)(zstencil & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits) {
        glClear(bits);
        s_frame_dirty = 1;
        t->ms_dirty = 1;
    }
    /* [thug] The depth-stencil surface (zeta) is one surface on the NV2A,
     * shared by every colour surface drawn with it; here each target has a
     * depth buffer of its own. So a clear of the zeta clears the depth of
     * every target that last drew with that zeta, not only the bound one.
     * THUG's reset transition clears zeta 0x80864000 with the 2x-wide screen
     * buffer bound, then draws the scene into a 640x480 surface with the same
     * zeta: that surface kept the previous frame's depth, and everything that
     * moved left copies behind (a stack of skateboards, doubled limbs). */
    if ((bits & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && rs->zeta_va) {
        GLbitfield zbits = bits & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        uint32_t i, n = 0;
        for (i = 0; i < TARGET_MAX; i++) {
            GlTarget *u = &s_targets[i];
            if (!u->fbo || u == t || u->zeta_va != rs->zeta_va)
                continue;
            if (!n) {
                glDisable(GL_SCISSOR_TEST);    /* the whole of each: the zeta is */
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, target_draw_fbo(u));
            glClear(zbits);
            n++;
        }
        if (n) {
            s_gl_dirty = 1;                    /* the next draw rebinds and re-applies */
            s_state_valid = 0;
            s_bound_fbo = 0;
            if (s_zeta_log)
                fprintf(stderr, "[PBGL] flip %u: zeta %08X cleared on %u more target(s)\n",
                        (unsigned)s_frame, rs->zeta_va, n);
        }
    }
    if (s_capture_left > 0)
        fprintf(stderr, "[CAPTURE] flip %u clear target %08X %ux%u zeta %08X flags %02X"
                " argb %08X zstencil %08X (after draw %u)\n", (unsigned)s_frame,
                t->color_va, t->lw, t->lh, rs->zeta_va, flags, argb, zstencil,
                s_capture_draw);
}

static uint64_t s_dropped_tris;

/* A vertex no screen could hold: non-finite, or absurdly far out. [thug] The
 * bound was a million pixels, which dropped about a hundred valid triangles a
 * frame in a level: a sky dome clipped at the eye's plane (w = 1e-3) puts
 * vertices tens of millions of pixels out, and losing those triangles left
 * the clear colour showing across the top of the sky. GL clips such
 * triangles itself; only non-finite or truly absurd positions are dropped
 * now (the garbage "wedges" this guarded against came from the pushbuffer
 * scan replaying stale commands, fixed since). */
static int vert_usable(const Nv2aVertex *v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z)
        && isfinite(v->rhw) && fabsf(v->x) < 1e12f && fabsf(v->y) < 1e12f;
}

/* RECOMP_GL_COMB=<n>: the pixel stage the title asked for, once per distinct
 * configuration, for the 2D batches (z = 0, rhw = 1) a back end gets wrong
 * most visibly -- a menu's text. "Diffuse times stage 0" is not the combiner:
 * where a title takes the colour from the alpha channel it paints opaque
 * black instead. What this prints is the register set the CPU rasteriser
 * evaluates (nv2a_combiner.h), which is the one to match. */
static void comb_report(const Nv2aBatch *b, const Nv2aVertex *v)
{
    static int left = -1, left3 = -1, min3 = -1;
    static uint32_t seen[256];
    static int nseen;
    const Nv2aCombiner *rc = b->combiner;
    uint32_t h, i;
    int s, dup = 0, two_d;
    float x0, y0, x1, y1;

    if (left < 0) {
        const char *e = getenv("RECOMP_GL_COMB");
        const char *e3 = getenv("RECOMP_GL_COMB3");
        const char *m3 = getenv("RECOMP_GL_COMB3_MIN");
        left = e ? atoi(e) : 0;
        left3 = e3 ? atoi(e3) : 0;
        /* The world's geometry is the big 3D batches; a dump that fills up on
         * sprites never reaches them. */
        min3 = m3 ? atoi(m3) : 300;
    }
    if ((!left && !left3) || !rc || !b->count)
        return;
    /* RECOMP_GL_COMB3 is the same record for 3D batches, where the question
     * is the fog: the first vertex's factor is what the fragment shader's
     * fog register holds, and a stage that is supposed to be textured but is
     * painted flat is either that factor or the final combiner's A input. */
    two_d = v->z == 0.0f && v->rhw == 1.0f;
    if (two_d ? left <= 0 : (left3 <= 0 || b->count < (uint32_t)min3))
        return;
    /* The batch's screen box, computed before the duplicate check so two
     * sprites that share a combiner but sit in different places are reported
     * apart: "the white rectangle at the lower right" has to be matchable,
     * and this is the only place the batch's vertices are in hand. */
    x0 = y0 = 1e30f;
    x1 = y1 = -1e30f;
    for (i = 0; i < b->count; i++) {
        const Nv2aVertex *p = &b->vertices[i];
        if (p->x != p->x || p->y != p->y)
            continue;                           /* NaN: no box from this one */
        if (p->x < x0) x0 = p->x;
        if (p->y < y0) y0 = p->y;
        if (p->x > x1) x1 = p->x;
        if (p->y > y1) y1 = p->y;
    }
    if (x0 > x1) { x0 = x1 = y0 = y1 = 0.0f; }
    h = 2166136261u;
    for (i = 0; i < 8; i++)
        h = (h ^ rc->color_icw[i]) * 16777619u;
    for (i = 0; i < 8; i++)
        h = (h ^ (rc->alpha_icw[i] * 3 + rc->color_ocw[i])) * 16777619u;
    h = (h ^ rc->stage_program) * 16777619u;
    h = (h ^ rc->control) * 16777619u;
    h = (h ^ rc->final0) * 16777619u;
    h = (h ^ rc->final1) * 16777619u;
    h = (h ^ rc->factor0[0]) * 16777619u;
    h = (h ^ rc->factor1[0]) * 16777619u;
    h = (h ^ (two_d ? 1u : 2u)) * 16777619u;   /* the two are reported apart */
    h = (h ^ (uint32_t)(x0 / 32.0f) * 73856093u) * 16777619u;
    h = (h ^ (uint32_t)(y0 / 32.0f) * 19349663u) * 16777619u;
    for (i = 0; i < (uint32_t)nseen; i++)
        if (seen[i] == h) dup = 1;
    if (dup)
        return;
    if (nseen < (int)(sizeof seen / sizeof seen[0]))
        seen[nseen++] = h;
    if (two_d)
        left--;
    else
        left3--;
    fprintf(stderr, "[GLCOMB] batch n %u col %08X spec %08X uv %g %g"
                    " fogf %g stpar %08X ctl %08X final %08X/%08X"
                    " f0 %08X f1 %08X fog %08X box %.0f..%.0f %.0f..%.0f\n",
            b->count, v->diffuse, v->specular, v->u, v->v, v->fogf,
            rc->stage_program, rc->control, rc->final0, rc->final1,
            rc->final_c0, rc->final_c1, b->fog_color, x0, x1, y0, y1);
    for (s = 0; s < 8; s++)
        fprintf(stderr, "[GLCOMB]   stage %d c %08X/%08X a %08X/%08X"
                        " fac %08X %08X\n", s, rc->color_icw[s],
                rc->color_ocw[s], rc->alpha_icw[s], rc->alpha_ocw[s],
                rc->factor0[s], rc->factor1[s]);
    for (i = 0; i < 4; i++) {
        const Nv2aTexture *tex = b->textures[i];
        fprintf(stderr, "[GLCOMB]   tex%u %s", i,
                tex ? "" : "(none)\n");
        if (tex)
            fprintf(stderr, " off %08X %ux%u fmt %02X pitch %u\n",
                    tex->offset, tex->width, tex->height, tex->color,
                    tex->pitch);
    }
}

/* RECOMP_GL_PROBE=x,y[,n]: the draws that actually cover one pixel.
 *
 * The census and the combiner records say what is submitted and with what
 * state, but a batch's screen box is not a promise about one pixel: the
 * ground is a sheet seen at a grazing angle and its box is mostly sky. This
 * is a point-in-triangle test against the batch's own triangles, so a pixel
 * that reads as the clear colour gets a list of the draws that could have
 * written it, in order, with every gate that can drop a fragment. n is how
 * many records to print (default 40). */
/* [thug] RECOMP_GL_PROBE_TEX=<hex address>[,n]: the same record for the first
 * n draws (default 6) that bind that texture in any stage, whatever pixels
 * they cover -- a deterministic way to find one object's draw. */
static int s_probe_force;

static int probe_report(const Nv2aSurface *s, const Nv2aBatch *b)
{
    static int px = -1, py, left, seen, minv, minflip;
    uint32_t i;

    if (px == -1) {
        const char *e = getenv("RECOMP_GL_PROBE");
        px = 0;
        if (e && *e) {
            int n = 40;
            minv = 1;
            minflip = 0;
            if (sscanf(e, "%d,%d,%d,%d,%d", &px, &py, &n, &minv,
                       &minflip) >= 2)
                left = n;
            else
                px = 0;
        }
    }
    if (!s_probe_force && (px <= 0 || left <= 0 || b->count < (uint32_t)minv
        || (int)s_frame < minflip))
        return 0;
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *a = &b->vertices[i];
        const Nv2aVertex *c = &b->vertices[i + 1];
        const Nv2aVertex *d = &b->vertices[i + 2];
        float x = (float)px, y = (float)py;   /* [thug] surface pixels */
        float d1, d2, d3;
        int neg, pos;

        if (!s_probe_force) {
            d1 = (x - c->x) * (a->y - c->y) - (a->x - c->x) * (y - c->y);
            d2 = (x - d->x) * (c->y - d->y) - (c->x - d->x) * (y - d->y);
            d3 = (x - a->x) * (d->y - a->y) - (d->x - a->x) * (y - a->y);
            if (!isfinite(d1) || !isfinite(d2) || !isfinite(d3))
                continue;
            neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
            pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
            if (neg && pos)
                continue;
            left--;
        }
        seen++;
        fprintf(stderr, "[GLPROBE] #%d batch n %u tri %u flip %u"
                        " depth %u func %X wmask %u polyoff %u %g %g | blend %u %X/%X"
                        " alpha %u %X ref %u | cull %u face %X front %X"
                        " | mask %08X | stpar %08X final %08X/%08X\n",
                seen, b->count, i, (unsigned)s_frame,
                b->state->depth_test_enable, b->state->depth_func,
                b->state->depth_write, b->state->poly_offset_fill,
                b->state->poly_offset_factor, b->state->poly_offset_units,
                b->state->blend_enable, b->state->blend_src,
                b->state->blend_dst,
                b->state->alpha_test_enable, b->state->alpha_func,
                (unsigned)b->state->alpha_ref, b->state->cull_enable,
                b->state->cull_face, b->state->front_face,
                b->state->color_mask,
                b->combiner ? b->combiner->stage_program : 0,
                b->combiner ? b->combiner->final0 : 0,
                b->combiner ? b->combiner->final1 : 0);
        if (b->combiner) {
            const Nv2aCombiner *rc = b->combiner;
            fprintf(stderr, "[GLPROBE]   rc ctl %08X icw0 %08X/%08X"
                            " ocw0 %08X/%08X f0 %08X f1 %08X"
                            " fcf %08X/%08X bump %04X\n",
                    rc->control, rc->color_icw[0], rc->alpha_icw[0],
                    rc->color_ocw[0], rc->alpha_ocw[0],
                    rc->factor0[0], rc->factor1[0],
                    rc->final_c0, rc->final_c1, rc->bump_valid);
        }
        {
            uint32_t zb;
            memcpy(&zb, &a->z, sizeof zb);
            fprintf(stderr, "[GLPROBE]   v0 %g %g z %g (%08X) rhw %g"
                            " fogf %g col %08X\n",
                    a->x, a->y, a->z, zb, a->rhw, a->fogf, a->diffuse);
        }
        fprintf(stderr, "[GLPROBE]   v1 %g %g z %g rhw %g | v2 %g %g z %g rhw %g\n",
                c->x, c->y, c->z, c->rhw, d->x, d->y, d->z, d->rhw);
        {   /* the depth this triangle has at the probed pixel's centre, in
             * depth-buffer units (screen-linear, as the z buffer sees it) */
            double X = px + 0.5, Y = py + 0.5;
            double den = ((double)c->y - d->y) * ((double)a->x - d->x)
                       + ((double)d->x - c->x) * ((double)a->y - d->y);
            if (den != 0.0) {
                double l0 = (((double)c->y - d->y) * (X - d->x)
                           + ((double)d->x - c->x) * (Y - d->y)) / den;
                double l1 = (((double)d->y - a->y) * (X - d->x)
                           + ((double)a->x - d->x) * (Y - d->y)) / den;
                fprintf(stderr, "[GLPROBE]   z at pixel %.3f\n",
                        l0 * a->z + l1 * c->z + (1.0 - l0 - l1) * d->z);
            }
        }
        for (int st = 0; st < 4; st++) {
            const Nv2aTexture *t = b->textures[st];
            if (t)
                fprintf(stderr, "[GLPROBE]   tex%d off %08X %ux%u fmt %02X"
                                " pitch %u addr %u/%u filter %08X levels %u"
                                " | st v0 %g %g %g %g v1 %g %g %g %g\n",
                        st, t->offset, t->width, t->height, t->color,
                        t->pitch, t->addr_u, t->addr_v, t->filter, t->levels,
                        a->st[st][0], a->st[st][1], a->st[st][2], a->st[st][3],
                        c->st[st][0], c->st[st][1], c->st[st][2], c->st[st][3]);
        }
        {
            /* Winding census: signed area in surface pixels (y down). */
            uint32_t q, npos = 0, nneg = 0;
            for (q = 0; q + 2 < b->count; q += 3) {
                const Nv2aVertex *p0 = &b->vertices[q], *p1 = &b->vertices[q + 1],
                                 *p2 = &b->vertices[q + 2];
                float ar = (p1->x - p0->x) * (p2->y - p0->y)
                         - (p1->y - p0->y) * (p2->x - p0->x);
                if (ar > 0.0f) npos++; else if (ar < 0.0f) nneg++;
            }
            fprintf(stderr, "[GLPROBE]   winding: %u positive, %u negative\n", npos, nneg);
        }
        if (b->combiner) {
            const Nv2aCombiner *rc = b->combiner;
            int k, ns = (int)(rc->control & 0xFF);
            for (k = 0; k < ns && k < 8; k++)
                fprintf(stderr, "[GLPROBE]   stage %d icw %08X/%08X ocw %08X/%08X"
                        " f0 %08X f1 %08X\n", k, rc->color_icw[k],
                        rc->alpha_icw[k], rc->color_ocw[k], rc->alpha_ocw[k],
                        rc->factor0[k], rc->factor1[k]);
        }
        return 1;
    }
    return 0;
}

/* The colour the probe pixel holds now, as the readback would see it. */
static void probe_pixel(const char *when, const Nv2aSurface *s)
{
    static int px = -1, py;
    unsigned char p[4] = {0, 0, 0, 0};

    if (px < 0) {
        const char *e = getenv("RECOMP_GL_PROBE");
        px = 0;
        if (e)
            sscanf(e, "%d,%d", &px, &py);
        if (px < 0) px = 0;
    }
    if (px <= 0)
        return;
    /* [thug] Targets hold the surface from (0,0), row 0 at the top, at
     * s_scale. A multisampled one is read through its resolve, and the
     * draw's framebuffer, scissor and colour mask are put back after. */
    (void)s;
    if (s_cur && s_cur->samples > 1) {
        GlTarget *t = s_cur;
        uint32_t m = s_last_rs.color_mask;
        target_resolve(t);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
        glReadPixels((GLint)scaled_x((uint32_t)px), (GLint)scaled_y((uint32_t)py),
                     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        glBindFramebuffer(GL_FRAMEBUFFER, t->msfbo);
        s_bound_fbo = t->msfbo;
        glEnable(GL_SCISSOR_TEST);
        if (s_allchan)
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        else
            glColorMask((m >> 16) & 1, (m >> 8) & 1, m & 1, (m >> 24) & 1);
    } else {
        glReadPixels((GLint)scaled_x((uint32_t)px), (GLint)scaled_y((uint32_t)py),
                     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    }
    fprintf(stderr, "[GLPROBE]   %s %02X%02X%02X%02X\n", when,
            p[0], p[1], p[2], p[3]);
}

/* Did this batch cover the probe at all? probe_report() prints when it did;
 * the caller brackets the draw with the pixel readbacks so the record says
 * what the batch actually changed, not only what state it was given. */
static int probe_hit(const Nv2aSurface *s, const Nv2aBatch *b)
{
    static int px = -1, py;
    uint32_t i;

    if (px < 0) {
        const char *e = getenv("RECOMP_GL_PROBE");
        px = 0;
        if (e)
            sscanf(e, "%d,%d", &px, &py);
        if (px < 0) px = 0;
    }
    if (px <= 0 || !b || !b->count)
        return 0;
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *a = &b->vertices[i];
        const Nv2aVertex *c = &b->vertices[i + 1];
        const Nv2aVertex *d = &b->vertices[i + 2];
        float x = (float)px, y = (float)py;   /* [thug] surface pixels */
        float d1, d2, d3;
        int neg, pos;

        d1 = (x - c->x) * (a->y - c->y) - (a->x - c->x) * (y - c->y);
        d2 = (x - d->x) * (c->y - d->y) - (c->x - d->x) * (y - d->y);
        d3 = (x - a->x) * (d->y - a->y) - (d->x - a->x) * (y - a->y);
        if (!isfinite(d1) || !isfinite(d2) || !isfinite(d3))
            continue;
        neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
        pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
        if (!neg || !pos)
            return 1;
    }
    return 0;
}

static void gl_vert_fill(GlVert *o, const Nv2aVertex *v)
{
    int st, j;

    o->x = v->x; o->y = v->y; o->z = v->z; o->rhw = v->rhw;
    o->color = v->diffuse;
    o->specular = v->specular;
    o->fogf = v->fogf;
    /* Normalised coordinates, one vec4 per stage: s, t, q and the sampler's
     * spare. The combiners' stage fetch divides by q exactly as the CPU
     * rasteriser does (nv2a_pb_exec.c's rc_stage_fetch), which is why nothing
     * here scales by the texture size -- the sampler's own normalisation is
     * the same thing, and it keeps a passthrough stage's coordinates
     * comparable instead of multiplied by a size that only this stage's
     * texture has. */
    for (st = 0; st < 4; st++)
        for (j = 0; j < 4; j++)
            o->st[st][j] = v->st[st][j];
}

/* [thug] THUG's reset transition (bailing into water, leaving the play
 * area) renders each frame into an alpha-less offscreen surface and lays it
 * over the screen buffer, never cleared, at vertex alpha 0.15 with
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA: for about a second every frame keeps 85%
 * of the last, so anything moving leaves a trail of fading copies. That is
 * the game's own effect, faithfully drawn, and the owner asked for it gone:
 * this draw is recognised (one full-surface quad sampling another render
 * target in an alpha-less format, alpha below 1, alpha blending, depth
 * ALWAYS without writes) and drawn unblended, so each frame replaces the
 * last and the transition looks like play. THUG_GL_KEEP_TRAILS=1 keeps the
 * game's blend. */
static int is_trail_composite(const Nv2aBatch *b, const GlTarget *t)
{
    static int keep = -1;
    static uint32_t last_flip = ~0u;
    const Nv2aRenderState *rs = b->state;
    const Nv2aTexture *tx = b->textures[0];
    const GlTarget *rt;
    float x0, x1, y0, y1;
    uint32_t i;

    if (keep < 0)
        keep = getenv("THUG_GL_KEEP_TRAILS") != NULL;
    if (keep || !tx || b->count > 6 || !rs->blend_enable
            || rs->blend_src != 0x0302 || rs->blend_dst != 0x0303
            || rs->depth_write || (rs->depth_test_enable && rs->depth_func != 0x0207)
            || !fmt_without_alpha(tx->color)
            || (b->vertices[0].diffuse >> 24) == 0xFF)
        return 0;
    rt = target_by_va(tx->offset);
    if (!rt || rt == t)
        return 0;
    x0 = x1 = b->vertices[0].x;
    y0 = y1 = b->vertices[0].y;
    for (i = 1; i < b->count; i++) {
        const Nv2aVertex *v = &b->vertices[i];
        if (v->x < x0) x0 = v->x;
        if (v->x > x1) x1 = v->x;
        if (v->y < y0) y0 = v->y;
        if (v->y > y1) y1 = v->y;
    }
    if (x0 > 1.0f || y0 > 1.0f || x1 < (float)t->lw - 1.0f || y1 < (float)t->lh - 1.0f)
        return 0;
    if (s_frame - last_flip > 30)
        fprintf(stderr, "[PBGL] flip %u: reset transition, its trail skipped"
                " (THUG_GL_KEEP_TRAILS=1 keeps it)\n", (unsigned)s_frame);
    last_flip = s_frame;
    return 1;
}

/* The glitch capture, defined with the frame dumps below. */
static void capture_draw(const Nv2aSurface *s, const Nv2aBatch *b, const GlTarget *t);

static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    GlTarget *t;
    size_t need, out;
    uint32_t i, dropped = 0;
    int probe_drew = 0;
    GlVert *dstv;

    if (!s_ready || !b || !b->count)
        return;
    if (!gl_ready())
        return;
    t = target_for(s, b->state);
    if (!t)
        return;
    /* [thug] A stage that samples another target reads its resolved colour:
     * resolve it now, before this draw's framebuffer and state are set. */
    if (s_msaa > 1) {                 /* every target matches s_msaa */
        for (i = 0; i < 4; i++) {
            const Nv2aTexture *tx = b->textures[i];
            GlTarget *rt = tx ? target_by_va(tx->offset) : NULL;
            if (rt && rt != t && rt->ms_dirty && rt->samples > 1)
                target_resolve(rt);
        }
    }
    target_bind(t, s);
    {
        /* [thug] The reset transition's trail, skipped (owner's call). */
        Nv2aRenderState solid;
        if (is_trail_composite(b, t)) {
            solid = *b->state;
            solid.blend_enable = 0;
            apply_state(&solid, s, s_frame);
        } else {
            apply_state(b->state, s, s_frame);
        }
    }
    comb_report(b, &b->vertices[0]);
    probe_drew = probe_hit(s, b) && probe_report(s, b);
    {
        static uint32_t want = 1;
        static int left;
        if (want == 1) {
            const char *e = getenv("RECOMP_GL_PROBE_TEX");
            want = 0;
            left = 6;
            if (e && *e && sscanf(e, "%x,%d", &want, &left) < 1)
                want = 0;
        }
        if (want && left > 0 && !probe_drew) {
            int k, hit = 0;
            for (k = 0; k < 4; k++)
                if (b->textures[k] && b->textures[k]->offset == want)
                    hit = 1;
            if (hit) {
                left--;
                s_probe_force = 1;
                probe_report(s, b);
                s_probe_force = 0;
            }
        }
    }
    if (s_capture_left > 0)
        capture_draw(s, b, t);
    if (probe_drew)
        probe_pixel("pre", s);

    need = b->count;
    if (s_vbo_ptr) {
        /* [thug] Straight into the mapped ring. */
        dstv = (GlVert *)vbo_reserve(need * sizeof(GlVert));
        if (!dstv)
            return;
    } else {
        if (need > s_verts_cap) {
            GlVert *grown = (GlVert *)realloc(s_verts, need * sizeof *grown);
            if (!grown)
                return;
            s_verts = grown;
            s_verts_cap = need;
        }
        dstv = s_verts;
    }
    for (i = 0, out = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *v0 = &b->vertices[i];
        const Nv2aVertex *v1 = &b->vertices[i + 1];
        const Nv2aVertex *v2 = &b->vertices[i + 2];
        GlVert *o = &dstv[out];

        if (!vert_usable(v0) || !vert_usable(v1) || !vert_usable(v2)) {
            dropped++;
            continue;
        }
        gl_vert_fill(&o[0], v0);
        gl_vert_fill(&o[1], v1);
        gl_vert_fill(&o[2], v2);
        o[0].pad[0] = o[1].pad[0] = o[2].pad[0] = 0.0f;
        out += 3;
    }
    if (dropped) {
        s_dropped_tris += dropped;
        if (s_verbose && s_dropped_tris == dropped)
            fprintf(stderr, "[PBGL] dropping unusable triangles "
                            "(non-finite or off-scale vertices)\n");
    }
    if (!out)
        return;

    if (!s_vao_bound) {
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        s_vao_bound = 1;
    }
    if (s_vbo_ptr) {
        s_draw_first = (GLint)(s_vbo_head / sizeof(GlVert));
        s_vbo_head += out * sizeof(GlVert);
    } else {
        size_t bytes = out * sizeof(GlVert);
        void *dst;
        GLint first;

        if (bytes > VBO_RING_BYTES)
            return;
        if (s_vbo_head + bytes > VBO_RING_BYTES) {
            glBufferData(GL_ARRAY_BUFFER, VBO_RING_BYTES, NULL, GL_STREAM_DRAW);
            s_vbo_head = 0;
        }
        dst = glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)s_vbo_head,
                               (GLsizeiptr)bytes,
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT
                               | GL_MAP_INVALIDATE_RANGE_BIT);
        if (!dst)
            return;
        memcpy(dst, s_verts, bytes);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        first = (GLint)(s_vbo_head / sizeof(GlVert));
        s_vbo_head += bytes;
        s_draw_first = first;
    }

    /* Every stage the title programmed, then the pixel stage itself. A stage
     * with nothing to bind gets a white texel, which is what the CPU
     * rasteriser's sampler returns when it cannot read one. */
    for (i = 0; i < 4; i++) {
        s_stage_ok[i] = stage_bind(i, b->textures[i], s_frame);
        if (!s_stage_ok[i])
            bind_unit(i, s_white, 0);
    }
    /* [thug] Each stage's coordinate scale, as stage_bind worked it out,
     * sent only when it changed. */
    {
        static GLfloat last[4][2] = { { -1, -1 } };
        if (memcmp(last, s_stage_sc, sizeof last) != 0) {
            glUniform2fv(u_texscale, 4, &s_stage_sc[0][0]);
            memcpy(last, s_stage_sc, sizeof last);
        }
    }
    rc_upload(b, &b->vertices[0]);

    glDrawArrays(GL_TRIANGLES, s_draw_first, (GLsizei)out);
    s_cur_draws++;
    s_cur_tris += (uint32_t)(out / 3);
    s_st.draws++;
    s_st.tris += (uint32_t)(out / 3);
    if (probe_drew)
        probe_pixel("post", s);
    s_frame_dirty = 1;
    t->ms_dirty = 1;
    if (s_a2c_on)
        s_st.a2c++;
}

/* Put the frame where the rest of the runtime already looks for it: the
 * title's colour surface, in the layout the window present reads.
 * [thug] Only on request now (see s_readback). The target holds the surface
 * from (0,0) with surface row 0 in GL row 0, at s_scale: the clip rectangle is
 * scaled back down through a scratch framebuffer first when s_scale is not 1. */
static void readback(void)
{
    const Nv2aSurface *s = &s_cur_surf;
    GlTarget *t = s_cur;
    uint8_t *mem, *dst;
    uint32_t y, w, h, bpp, pitch;
    GLint rx, ry;

    if (!t || !s->width || !s->height || !s->bytes_per_pixel || !s->color_va || !s->pitch)
        return;
    bpp = s->bytes_per_pixel;
    if (bpp != 2 && bpp != 4)
        return;                       /* the present path handles 1 and 8 */
    w = s->width;
    h = s->height;
    pitch = s->pitch;

    target_resolve(t);
    s_gl_dirty = 1;
    s_bound_fbo = 0;
    glDisable(GL_SCISSOR_TEST);
    if (s_scale_x != 1.0f || s_scale_y != 1.0f) {
        static GLuint fbo, tex;
        static uint32_t cw, ch;
        if (cw != w || ch != h) {
            if (!fbo) {
                glGenFramebuffers(1, &fbo);
                glGenTextures(1, &tex);
            }
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex, 0);
            cw = w;
            ch = h;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer((GLint)scaled_x(s->x), (GLint)scaled_y(s->y),
                          (GLint)scaled_x(s->x + w), (GLint)scaled_y(s->y + h),
                          0, 0, (GLint)w, (GLint)h,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        rx = ry = 0;
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
        rx = (GLint)s->x;
        ry = (GLint)s->y;
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    {
        static uint8_t *buf;
        static size_t cap;
        size_t need = (size_t)w * h * 4;
        if (need > cap) {
            uint8_t *grown = (uint8_t *)realloc(buf, need);
            if (!grown)
                return;
            buf = grown;
            cap = need;
        }
        glReadPixels(rx, ry, (GLsizei)w, (GLsizei)h, GL_RGBA,
                     GL_UNSIGNED_BYTE, buf);
        mem = (uint8_t *)xbox_GetMemoryOffset();
        dst = mem + s->color_va + (size_t)s->y * pitch + (size_t)s->x * bpp;
        for (y = 0; y < h; y++) {
            const uint8_t *src = buf + (size_t)y * w * 4;
            uint8_t *d = dst + (size_t)y * pitch;
            uint32_t x;
            if (bpp == 4) {
                for (x = 0; x < w; x++) {   /* RGBA bytes -> AARRGGBB dword */
                    d[0] = src[2];
                    d[1] = src[1];
                    d[2] = src[0];
                    d[3] = src[3];
                    d += 4;
                    src += 4;
                }
            } else {
                for (x = 0; x < w; x++) {
                    uint32_t r = src[0], g = src[1], bl = src[2];
                    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5)
                                            | (bl >> 3));
                    d[0] = (uint8_t)v;
                    d[1] = (uint8_t)(v >> 8);
                    d += 2;
                    src += 4;
                }
            }
        }
    }
}

/* [thug] The executor's present_sync: before it snapshots or dumps the
 * frame, put the frame in guest memory. Registered only when wanted. */
static void gl_present_sync(void)
{
    if (!s_ready || !gl_ready())
        return;
    if (s_frame_dirty && s_cur) {
        readback();
        s_frame_dirty = 0;
    }
}

/* The surface the frame is presented from: the one drawn last, which is the
 * one the title flips (the executor presents its drawn_offset, the same
 * thing). Falls back to the largest surface of the frame. */
static GlTarget *present_target(void)
{
    if (s_cur && s_cur->fbo)
        return s_cur;
    return s_present;
}

/* [thug] RECOMP-style frame dump from the GPU: THUG_GL_DUMP=<dir> writes the
 * presented surface at its physical size as a PPM every THUG_GL_DUMP_EVERY
 * flips (default 60). Row 0 of the target is the top of the surface. */
/* [thug] THUG_GL_FLICKER=<dir>: find one-frame glitches. Every presented
 * frame is read back (keep this to diagnosis runs) and compared, shrunk to
 * 64x36, with its neighbours. A frame unlike both neighbours while they are
 * alike is a flicker: the three frames are written to <dir> as PPM and a
 * line compares their draw counts, texture uploads, failed texture binds and
 * dropped triangles. */
typedef struct {
    uint8_t *px;            /* full frame, RGB, row 0 = top */
    uint32_t w, h, frame;
    uint8_t  small[64 * 36 * 3];
    uint32_t draws, tris, uploads, fails, target;
    uint64_t dropped;
} FlickerFrame;

static void flicker_shrink(const FlickerFrame *f, uint8_t *out)
{
    uint32_t x, y, c;
    for (y = 0; y < 36; y++)
        for (x = 0; x < 64; x++) {
            uint32_t x0 = x * f->w / 64, x1 = (x + 1) * f->w / 64;
            uint32_t y0 = y * f->h / 36, y1 = (y + 1) * f->h / 36;
            uint32_t sum[3] = { 0, 0, 0 }, n = 0, xx, yy;
            for (yy = y0; yy < y1; yy += 2)
                for (xx = x0; xx < x1; xx += 2) {
                    const uint8_t *p = f->px + ((size_t)yy * f->w + xx) * 3;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; n++;
                }
            for (c = 0; c < 3; c++)
                out[(y * 64 + x) * 3 + c] = (uint8_t)(n ? sum[c] / n : 0);
        }
}

static double flicker_diff(const uint8_t *a, const uint8_t *b)
{
    uint32_t i, sum = 0;
    for (i = 0; i < 64 * 36 * 3; i++)
        sum += (uint32_t)abs((int)a[i] - (int)b[i]);
    return (double)sum / (64.0 * 36.0 * 3.0);
}

static void flicker_write(const char *dir, const FlickerFrame *f, const char *tag)
{
    char path[512];
    FILE *fp;
    snprintf(path, sizeof path, "%s/flicker_%06u_%s.ppm", dir, f->frame, tag);
    fp = fopen(path, "wb");
    if (!fp)
        return;
    fprintf(fp, "P6\n%u %u\n255\n", f->w, f->h);
    fwrite(f->px, 1, (size_t)f->w * f->h * 3, fp);
    fclose(fp);
}

static void flicker_check(GlTarget *t)
{
    static const char *dir = (const char *)-1;
    static FlickerFrame ring[3];
    static uint32_t n, found;
    FlickerFrame *f;

    if (dir == (const char *)-1) {
        dir = getenv("THUG_GL_FLICKER");
        if (dir && !*dir)
            dir = NULL;
    }
    if (!dir || !t || !t->pw || !t->ph)
        return;
    f = &ring[n % 3];
    if (f->w != t->pw || f->h != t->ph || !f->px) {
        free(f->px);
        f->px = (uint8_t *)malloc((size_t)t->pw * t->ph * 3);
        if (!f->px)
            return;
        f->w = t->pw;
        f->h = t->ph;
    }
    target_resolve(t);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)t->pw, (GLsizei)t->ph, GL_RGB, GL_UNSIGNED_BYTE, f->px);
    s_gl_dirty = 1;
    f->frame = s_frame;
    f->draws = s_cur_draws;
    f->tris = s_cur_tris;
    f->uploads = s_cur_uploads;
    f->fails = s_cur_fail;
    f->dropped = s_dropped_tris;
    f->target = t->color_va;
    flicker_shrink(f, f->small);
    n++;
    if (n >= 3) {
        const FlickerFrame *a = &ring[(n - 3) % 3], *b = &ring[(n - 2) % 3],
                           *c = &ring[(n - 1) % 3];
        double dab = flicker_diff(a->small, b->small);
        double dbc = flicker_diff(b->small, c->small);
        double dac = flicker_diff(a->small, c->small);
        if (a->w == b->w && b->w == c->w && dab > 3.0 && dbc > 3.0
                && dac < 0.35 * (dab < dbc ? dab : dbc)) {
            found++;
            fprintf(stderr, "[FLICKER] #%u frame %u: diff %.1f / %.1f (neighbours %.1f)"
                    " | draws %u %u %u | tris %u %u %u | uploads %u %u %u"
                    " | failed binds %u %u %u | dropped tris %llu %llu"
                    " | target %08X %08X %08X\n", found, b->frame, dab, dbc, dac,
                    a->draws, b->draws, c->draws, a->tris, b->tris, c->tris,
                    a->uploads, b->uploads, c->uploads, a->fails, b->fails, c->fails,
                    (unsigned long long)(b->dropped - a->dropped),
                    (unsigned long long)(c->dropped - b->dropped),
                    a->target, b->target, c->target);
            if (found <= 12) {
                flicker_write(dir, a, "a");
                flicker_write(dir, b, "b_flicker");
                flicker_write(dir, c, "c");
            }
        }
    }
}

static void dump_target_to(GlTarget *t, const char *path);

/* [thug] t's alpha channel as a grey PPM (captures). */
static void dump_target_alpha_to(GlTarget *t, const char *path)
{
    uint8_t *buf;
    size_t n;
    FILE *f;
    uint32_t i;

    if (!t || !t->pw || !t->ph)
        return;
    n = (size_t)t->pw * t->ph;
    buf = (uint8_t *)malloc(n * 3);
    if (!buf)
        return;
    s_gl_dirty = 1;
    target_resolve(t);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)t->pw, (GLsizei)t->ph, GL_ALPHA, GL_UNSIGNED_BYTE, buf);
    for (i = (uint32_t)n; i-- > 0;)
        buf[3 * i] = buf[3 * i + 1] = buf[3 * i + 2] = buf[i];
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", t->pw, t->ph);
        fwrite(buf, 1, n * 3, f);
        fclose(f);
    }
    free(buf);
}

static void dump_target_to(GlTarget *t, const char *path)
{
    static uint8_t *buf;
    static size_t cap;
    size_t need;
    FILE *f;
    uint32_t y;

    if (!t || !t->pw || !t->ph)
        return;
    s_gl_dirty = 1;
    need = (size_t)t->pw * t->ph * 3;
    if (need > cap) {
        uint8_t *g = (uint8_t *)realloc(buf, need);
        if (!g)
            return;
        buf = g;
        cap = need;
    }
    target_resolve(t);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)t->pw, (GLsizei)t->ph, GL_RGB,
                 GL_UNSIGNED_BYTE, buf);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", t->pw, t->ph);
    for (y = 0; y < t->ph; y++)
        fwrite(buf + (size_t)y * t->pw * 3, 1, (size_t)t->pw * 3, f);
    fclose(f);
}

static void dump_target(GlTarget *t)
{
    char path[512];
    snprintf(path, sizeof path, "%s/gl%06u.ppm", s_dump_dir, (unsigned)s_frame);
    dump_target_to(t, path);
}

/* [thug] Glitch capture (F10; nv2a_pb_gl_request_capture): the next two
 * frames in full, for a report of something that is on screen only briefly.
 * Every draw goes to the log as a [CAPTURE] line (index, target, each stage's
 * texture: linear or swizzled, and whether it is a render target) followed by
 * the probe's full record ([GLPROBE] lines: state, vertices, coordinates,
 * combiner); at each flip every render target is written as PPM to
 * <state dir>/captures/<time>/ ($XDG_STATE_HOME/thug-pc-port, else
 * ~/.local/state/thug-pc-port). */
static volatile int s_capture_req;
static char     s_capture_dir[1024];

void nv2a_pb_gl_request_capture(void) { s_capture_req = 1; }

static int capture_begin(void)
{
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char stamp[32];
    time_t now = time(NULL);
    struct tm tmv;
    size_t i;

    if (xdg && *xdg)
        snprintf(s_capture_dir, sizeof s_capture_dir, "%s/thug-pc-port/captures", xdg);
    else if (home && *home)
        snprintf(s_capture_dir, sizeof s_capture_dir, "%s/.local/state/thug-pc-port/captures", home);
    else
        snprintf(s_capture_dir, sizeof s_capture_dir, "captures");
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "/%Y%m%d-%H%M%S", &tmv);
    strncat(s_capture_dir, stamp, sizeof s_capture_dir - strlen(s_capture_dir) - 1);
    for (i = 1; s_capture_dir[i]; i++) {               /* mkdir -p */
        if (s_capture_dir[i] == '/') {
            s_capture_dir[i] = 0;
            mkdir(s_capture_dir, 0755);
            s_capture_dir[i] = '/';
        }
    }
    if (mkdir(s_capture_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[CAPTURE] cannot create %s: %s\n", s_capture_dir, strerror(errno));
        return 0;
    }
    fprintf(stderr, "[CAPTURE] capturing flips %u-%u into %s\n", (unsigned)s_frame,
            (unsigned)s_frame + 1, s_capture_dir);
    s_capture_draw = 0;
    return 1;
}

static void capture_draw(const Nv2aSurface *s, const Nv2aBatch *b, const GlTarget *t)
{
    int k;
    fprintf(stderr, "[CAPTURE] flip %u draw %u target %08X %ux%u zeta %08X", (unsigned)s_frame,
            s_capture_draw++, t->color_va, t->lw, t->lh, b->state->zeta_va);
    for (k = 0; k < 4; k++) {
        const Nv2aTexture *x = b->textures[k];
        if (x)
            fprintf(stderr, " | t%d %08X %ux%u fmt %02X %s%s", k, x->offset, x->width,
                    x->height, x->color,
                    d3d8_format_is_swizzled(x->color) ? "swz"
                    : d3d8_format_dxt_block_bytes(x->color) ? "dxt" : "lin",
                    target_by_va(x->offset) ? " RT" : "");
    }
    fprintf(stderr, " | stages %05X\n", b->combiner ? b->combiner->stage_program : 0);
    s_probe_force = 1;
    probe_report(s, b);
    s_probe_force = 0;
}

static void capture_flip(void)
{
    uint32_t i;
    int n = 0;
    for (i = 0; i < TARGET_MAX; i++) {
        GlTarget *t = &s_targets[i];
        char path[1200];
        if (!t->fbo)
            continue;
        snprintf(path, sizeof path, "%s/flip%06u_target%08X_%ux%u.ppm", s_capture_dir,
                 (unsigned)s_frame, t->color_va, t->lw, t->lh);
        dump_target_to(t, path);
        snprintf(path, sizeof path, "%s/flip%06u_target%08X_%ux%u_alpha.ppm", s_capture_dir,
                 (unsigned)s_frame, t->color_va, t->lw, t->lh);
        dump_target_alpha_to(t, path);
        n++;
    }
    fprintf(stderr, "[CAPTURE] flip %u: %u draws so far, %d render targets written\n",
            (unsigned)s_frame, s_capture_draw, n);
}

/* [thug] Brightness (THUG_BRIGHTNESS, the overlay's slider): the presented
 * frame through a gamma curve, out = in^(1/b), which lifts or lowers the
 * midtones and keeps black and white where they are. At 1 the frame is
 * blitted as before and this pass never runs. */
static volatile float s_brightness = 1.0f;
static GLuint s_pp_prog, s_pp_vao;
static GLint  u_pp_gamma;

static const char *k_pp_vs =
    "#version 330 core\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    /* One triangle over the viewport; row 0 of the target is the top. */
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *k_pp_fs =
    "#version 330 core\n"
    "in vec2 vUv;\n"
    "uniform sampler2D uFrame;\n"
    "uniform float uGamma;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec3 c = texture(uFrame, vUv).rgb;\n"
    "    frag = vec4(pow(c, vec3(uGamma)), 1.0);\n"
    "}\n";

static int present_pass_ready(void)
{
    static int failed;
    GLuint vs, fs;
    GLint ok = 0;

    if (s_pp_prog || failed)
        return s_pp_prog != 0;
    vs = compile(GL_VERTEX_SHADER, k_pp_vs);
    fs = compile(GL_FRAGMENT_SHADER, k_pp_fs);
    if (vs && fs) {
        s_pp_prog = glCreateProgram();
        glAttachShader(s_pp_prog, vs);
        glAttachShader(s_pp_prog, fs);
        glLinkProgram(s_pp_prog);
        glGetProgramiv(s_pp_prog, GL_LINK_STATUS, &ok);
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (!ok) {
        fprintf(stderr, "[PBGL] brightness pass unavailable; presenting without it\n");
        if (s_pp_prog) glDeleteProgram(s_pp_prog);
        s_pp_prog = 0;
        failed = 1;
        return 0;
    }
    glUseProgram(s_pp_prog);
    glUniform1i(glGetUniformLocation(s_pp_prog, "uFrame"), 0);
    u_pp_gamma = glGetUniformLocation(s_pp_prog, "uGamma");
    glGenVertexArrays(1, &s_pp_vao);
    glUseProgram(s_prog);
    return 1;
}

/* Draw t's colour into the bound default framebuffer's (x, y, w, h) through
 * the brightness curve. Leaves the title's program current. */
static void present_with_brightness(GlTarget *t, float b, int x, int y, int w, int h)
{
    glViewport(x, y, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glUseProgram(s_pp_prog);
    glUniform1f(u_pp_gamma, 1.0f / b);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glBindSampler(0, 0);
    glBindVertexArray(s_pp_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(s_prog);
    glViewport(0, 0, s_draw_w, s_draw_h);
}

/* [thug] Blit the frame to the window, fitted to the presented aspect ratio
 * with black bars, and swap. The target's row 0 is the surface's top, and the
 * window's row 0 is its bottom, hence the reversed destination rows. */
static void present(GlTarget *t)
{
    int dw = s_draw_w, dh = s_draw_h;
    int w, h, x, y;

    if (!t || dw <= 0 || dh <= 0)
        return;
    s_gl_dirty = 1;
    s_vao_bound = 0;
    unit_cache_reset();
    if ((float)dw > (float)dh * s_aspect) {
        h = dh;
        w = (int)((float)dh * s_aspect + 0.5f);
    } else {
        w = dw;
        h = (int)((float)dw / s_aspect + 0.5f);
    }
    x = (dw - w) / 2;
    y = (dh - h) / 2;

    target_resolve(t);
    s_bound_fbo = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, dw, dh);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        float b = s_brightness;
        if ((b < 0.995f || b > 1.005f) && present_pass_ready())
            present_with_brightness(t, b, x, y, w, h);
        else
            glBlitFramebuffer(0, 0, (GLint)t->pw, (GLint)t->ph,
                              x, y + h, x + w, y,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    /* [thug] The settings overlay draws over the finished frame. */
    if (s_overlay) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        s_overlay(dw, dh);
    }
    SDL_GL_SwapWindow(s_win);
    /* [thug] Frame limiter: hold this thread -- and through the pushbuffer,
     * the game -- to the chosen rate. Game time is real time (see
     * sub_00173F80), so a limit changes smoothness, never speed. */
    {
        static struct timespec next;
        int lim = s_fps_limit;
        if (lim > 0) {
            struct timespec now;
            long long step = 1000000000LL / lim, ahead;
            clock_gettime(CLOCK_MONOTONIC, &now);
            ahead = (long long)(next.tv_sec - now.tv_sec) * 1000000000LL
                  + (next.tv_nsec - now.tv_nsec);
            if (ahead > 0 && ahead <= step)
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
            else
                next = now;           /* late, or the first frame: restart */
            next.tv_nsec += step;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec++;
            }
        }
    }
}

/* [thug] Settings changed from another thread, applied here. */
static void apply_requests(void)
{
    int v = s_req_vsync;
    float sc = s_req_scale;
    uint32_t i;

    if (v >= 0) {
        s_req_vsync = -1;
        s_vsync = v;
        if (s_visible) {
            if (!v)
                SDL_GL_SetSwapInterval(0);
            else if (SDL_GL_SetSwapInterval(-1) != 0)
                SDL_GL_SetSwapInterval(1);
        }
    }
    {
        int realloc_all = 0;
        int ms = s_req_msaa;
        if (ms >= 0) {
            s_req_msaa = -1;
            ms = msaa_clamp(ms);
            if (ms != s_msaa) {
                s_msaa = ms;
                realloc_all = 1;
                fprintf(stderr, "[PBGL] antialiasing %s\n",
                        ms > 1 ? (ms == 2 ? "2x MSAA" : ms == 4 ? "4x MSAA" : "8x MSAA")
                               : "off");
            }
        }
        if (sc >= 0.0f) {
            s_req_scale = -1.0f;
            if (sc == 0.0f)
                sc = s_native_scale;
            if (sc < 1.0f) sc = 1.0f;
            if (sc > 8.0f) sc = 8.0f;
            if (sc != s_scale) {
                s_scale = sc;
                s_scale_y = sc;
                s_scale_x = sc * s_aspect / (4.0f / 3.0f);
                realloc_all = 1;
                fprintf(stderr, "[PBGL] internal scale %.2f (%ux%u)\n", (double)sc,
                        scaled_x(640), scaled_y(480));
            }
        }
        /* Every target at the new scale and sample count, its contents
         * carried across. */
        if (realloc_all) {
            for (i = 0; i < TARGET_MAX; i++)
                if (s_targets[i].fbo)
                    target_alloc(&s_targets[i], s_targets[i].lw, s_targets[i].lh);
            s_gl_dirty = 1;
            s_bound_fbo = 0;
        }
    }
}

/* [thug] Frame pacing, measured at each flip. */
static void frame_timing(void)
{
    static struct timespec last, win_start;
    static unsigned win_frames;
    struct timespec now;
    double ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec) {
        ms = (double)(now.tv_sec - last.tv_sec) * 1000.0
           + (double)(now.tv_nsec - last.tv_nsec) / 1e6;
        s_frame_hist[s_frame_hist_n % FRAME_HIST] = (float)ms;
        s_frame_hist_n = s_frame_hist_n + 1;
    } else {
        win_start = now;
    }
    last = now;
    win_frames++;
    ms = (double)(now.tv_sec - win_start.tv_sec) * 1000.0
       + (double)(now.tv_nsec - win_start.tv_nsec) / 1e6;
    if (ms >= 500.0) {
        s_fps_now = (float)(win_frames * 1000.0 / ms);
        s_ms_now = (float)(ms / win_frames);
        win_frames = 0;
        win_start = now;
    }
    s_last_draws = s_cur_draws;
    s_last_tris = s_cur_tris;
}

static void gl_flip(void)
{
    GlTarget *t;

    if (!s_ready || !gl_ready())
        return;
    frame_timing();
    if (s_verbose && (s_frame == 300 || s_frame == 3000 || s_frame == 6000)) {
        int k, any = 0;
        fprintf(stderr, "[PBGL] stage modes at frame %u (stage uses): ",
                (unsigned)s_frame);
        for (k = 0; k < 32; k++)
            if (s_mode_hist[k]) {
                fprintf(stderr, "%u x%u ", k, s_mode_hist[k]);
                any = 1;
            }
        if (!any)
            fprintf(stderr, "none");
        fprintf(stderr, "\n");
    }
    t = present_target();
    if (t) {
        static uint32_t last_va;
        if (last_va && t->color_va != last_va) {
            s_st.switches++;
            if (s_stats)
                fprintf(stderr, "[GLSTAT] f%u presented %08X (%ux%u), last flip %08X\n",
                        s_frame, t->color_va, t->lw, t->lh, last_va);
        }
        last_va = t->color_va;
        /* The output resolution: the presented height at the presented
         * aspect. The target itself may be anti-aliased (THUG's is 2x wide). */
        uint32_t rw = (uint32_t)((float)t->ph * s_aspect + 0.5f);
        __atomic_store_n(&s_res_packed, ((rw & 0xFFFFu) << 16) | (t->ph & 0xFFFFu),
                         __ATOMIC_RELAXED);
        if (s_dump_dir && s_dump_every && (s_frame % s_dump_every) == 0)
            dump_target(t);
        flicker_check(t);
        if (s_visible)
            present(t);
    }
    if (s_stats) {
        static struct timespec t0;
        static uint32_t f0;
        /* GPU time per frame: a TIME_ELAPSED query from flip to flip, read
         * back four frames later so the read never waits on the GPU. */
        static GLuint q[4];
        static int q_live[4], q_open;
        static double gpu_ms_sum;
        static uint32_t gpu_n;
        struct timespec now;
        {
            uint32_t k = s_frame & 3;
            if (!q[0])
                glGenQueries(4, q);
            if (q_open)
                glEndQuery(GL_TIME_ELAPSED);
            if (q_live[k]) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(q[k], GL_QUERY_RESULT, &ns);
                gpu_ms_sum += (double)ns / 1e6;
                gpu_n++;
            }
            glBeginQuery(GL_TIME_ELAPSED, q[k]);
            q_live[k] = 1;
            q_open = 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!t0.tv_sec) {
            t0 = now;
            f0 = s_frame;
        } else if (s_frame - f0 >= 120) {
            double dt = (double)(now.tv_sec - t0.tv_sec)
                      + (double)(now.tv_nsec - t0.tv_nsec) * 1e-9;
            uint32_t n = s_frame - f0;
            fprintf(stderr, "[GLSTAT] f%u %.1f fps | per frame: draws %u tris %u"
                    " gpu %.2f ms uploads %.1f (%.0f Ktexels) evict %.1f alias %.1f"
                    " hashKB %.0f polyoff %u | msaa %d: a2c %u resolves %.1f"
                    " | flipped-surface changes %u, dropped tris %llu (total)\n",
                    s_frame, n / dt, s_st.draws / n,
                    s_st.tris / n, gpu_n ? gpu_ms_sum / gpu_n : 0.0,
                    (double)s_st.uploads / n,
                    (double)s_st.upload_texels / n / 1000.0,
                    (double)s_st.evictions / n, (double)s_st.aliases / n,
                    (double)s_st.hash_bytes / n / 1024.0, s_st.polyoff / n,
                    s_msaa, s_st.a2c / n, (double)s_resolves / n, s_st.switches,
                    (unsigned long long)s_dropped_tris);
            memset(&s_st, 0, sizeof s_st);
            s_resolves = 0;
            gpu_ms_sum = 0.0;
            gpu_n = 0;
            t0 = now;
            f0 = s_frame;
        }
    }
    /* [thug] THUG_BENCH_SCALES=4.5,2.25,1 (THUG_BENCH_PERIOD flips each,
     * default 600): cycle internal scales in the running scene and print the
     * frame rate each one held, for a like-for-like resolution comparison. */
    /* THUG_BENCH_MSAA=1,4,8 does the same for antialiasing (both lists step
     * together when both are given). */
    {
        static int init, n, nm, cur, period = 600;
        static float scales[8];
        static int msaa[8];
        static uint32_t seg_start;
        static struct timespec t0;
        if (!init) {
            const char *e = getenv("THUG_BENCH_SCALES");
            init = 1;
            while (e && *e && n < 8) {
                scales[n++] = (float)atof(e);
                e = strchr(e, ',');
                if (e) e++;
            }
            e = getenv("THUG_BENCH_MSAA");
            while (e && *e && nm < 8) {
                msaa[nm++] = atoi(e);
                e = strchr(e, ',');
                if (e) e++;
            }
            e = getenv("THUG_BENCH_PERIOD");
            if (e && atoi(e) > 0)
                period = atoi(e);
        }
        if ((n || nm) && s_frame - seg_start >= (uint32_t)period) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (t0.tv_sec) {
                double dt = (double)(now.tv_sec - t0.tv_sec)
                          + (double)(now.tv_nsec - t0.tv_nsec) * 1e-9;
                fprintf(stderr, "[BENCH] scale %.2f (%ux%u) msaa %d: %.1f fps over"
                        " flips %u-%u, %u draws/frame\n", (double)s_scale,
                        scaled_x(640), scaled_y(480), s_msaa,
                        (s_frame - seg_start) / dt, seg_start, s_frame,
                        s_last_draws);
            }
            if (n)
                s_req_scale = scales[cur % n];
            if (nm)
                s_req_msaa = msaa[cur % nm];
            cur++;
            seg_start = s_frame + 1;
            t0 = now;
        }
    }
    {   /* THUG_GL_CAPTURE_AT=<flip>: the same capture, for scripted runs */
        static long at = -2;
        if (at == -2) {
            const char *e = getenv("THUG_GL_CAPTURE_AT");
            at = e && *e ? atol(e) : -1;
        }
        if (at >= 0 && (long)s_frame == at)
            s_capture_req = 1;
    }
    if (s_capture_left > 0) {
        capture_flip();
        if (--s_capture_left == 0)
            fprintf(stderr, "[CAPTURE] done: %s\n", s_capture_dir);
    } else if (s_capture_req) {
        s_capture_req = 0;
        if (capture_begin())
            s_capture_left = 2;
    }
    apply_requests();
    s_cur_draws = s_cur_tris = s_cur_fail = s_cur_uploads = 0;
    __atomic_add_fetch(&s_flip_count, 1, __ATOMIC_RELAXED);
    s_present = NULL;
    s_present_area = 0;
    s_frame++;
}

/* ── [thug] The window, for the host's event loop ─────────────────────────── */

void *nv2a_pb_gl_window(void)
{
    return s_visible ? (void *)s_win : NULL;
}

void nv2a_pb_gl_drawable_changed(void)
{
    int w = 0, h = 0;

    if (!s_win)
        return;
    SDL_GL_GetDrawableSize(s_win, &w, &h);
    s_draw_w = w;
    s_draw_h = h;
}

void nv2a_pb_gl_set_scale(float sc) { s_req_scale = sc < 0.0f ? 0.0f : sc; }
float nv2a_pb_gl_scale(void) { return s_scale; }
float nv2a_pb_gl_native_scale(void) { return s_native_scale; }
float nv2a_pb_gl_aspect(void) { return s_aspect; }
void nv2a_pb_gl_set_vsync(int on) { s_req_vsync = on ? 1 : 0; }
int  nv2a_pb_gl_vsync(void) { return s_req_vsync >= 0 ? s_req_vsync : s_vsync; }
void nv2a_pb_gl_set_fps_limit(int fps) { s_fps_limit = fps > 0 ? fps : 0; }
int  nv2a_pb_gl_fps_limit(void) { return s_fps_limit; }
void nv2a_pb_gl_set_aniso(int n)
{
    float a = (float)(n < 1 ? 1 : n);
    s_aniso = a > s_aniso_max ? s_aniso_max : a;
}
int  nv2a_pb_gl_aniso(void) { return (int)s_aniso; }
int  nv2a_pb_gl_aniso_max(void) { return (int)s_aniso_max; }
void nv2a_pb_gl_set_brightness(float b)
{
    s_brightness = b < 0.5f ? 0.5f : b > 2.0f ? 2.0f : b;
}
float nv2a_pb_gl_brightness(void) { return s_brightness; }
void nv2a_pb_gl_set_msaa(int samples) { s_req_msaa = samples < 1 ? 1 : samples; }
int  nv2a_pb_gl_msaa(void) { return s_req_msaa >= 0 ? msaa_clamp(s_req_msaa) : s_msaa; }
int  nv2a_pb_gl_msaa_max(void) { return s_msaa_max; }
void nv2a_pb_gl_set_overlay(void (*fn)(int dw, int dh)) { s_overlay = fn; }

void nv2a_pb_gl_frame_info(Nv2aGlFrameInfo *fi)
{
    unsigned n = s_frame_hist_n, i, have = n < FRAME_HIST ? n : FRAME_HIST;
    uint32_t r = __atomic_load_n(&s_res_packed, __ATOMIC_RELAXED);

    memset(fi, 0, sizeof *fi);
    fi->fps = s_fps_now;
    fi->frame_ms = s_ms_now;
    fi->draws = s_last_draws;
    fi->tris = s_last_tris;
    fi->out_w = r >> 16;
    fi->out_h = r & 0xFFFFu;
    fi->scale = s_scale;
    /* Oldest first. */
    for (i = 0; i < have; i++)
        fi->hist[i] = s_frame_hist[(n - have + i) % FRAME_HIST];
    fi->hist_n = (int)have;
}

void nv2a_pb_gl_stats(uint64_t *flips, uint32_t *w, uint32_t *h)
{
    uint32_t r = __atomic_load_n(&s_res_packed, __ATOMIC_RELAXED);
    if (flips)
        *flips = __atomic_load_n(&s_flip_count, __ATOMIC_RELAXED);
    if (w)
        *w = r >> 16;
    if (h)
        *h = r & 0xFFFFu;
}

static const Nv2aBackend s_backend = { gl_clear, gl_draw, gl_flip };

static void APIENTRY gl_debug_message(GLenum source, GLenum type, GLuint id,
                                      GLenum severity, GLsizei length,
                                      const GLchar *msg, const void *user)
{
    static unsigned n;
    (void)source; (void)length; (void)user;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || n >= 200)
        return;
    n++;
    fprintf(stderr, "[GLDBG] type %04X id %u severity %04X frame %u: %s\n",
            type, id, severity, (unsigned)s_frame, msg);
}

/* ── Bring-up ───────────────────────────────────────────────────────────── */

int nv2a_pb_gl_init(void)
{
    const char *e;
    int major = 3, minor = 3;

    /* On by default: the point of this back end is that the GPU draws and
     * the CPU does not. RECOMP_PB_GL=0 keeps the CPU rasteriser, which is
     * still the fallback for titles whose look needs combiners or fog. */
    e = getenv("RECOMP_PB_GL");
    if (e && e[0] == '0' && !e[1])
        return 0;
    s_enabled = 1;
    s_verbose = getenv("RECOMP_PB_GL_VERBOSE") != NULL;
    e = getenv("RECOMP_PB_GL_ZSCALE");
    if (e && *e)
        s_zscale_override = (uint32_t)strtoul(e, NULL, 0);
    s_bind_log = getenv("RECOMP_GL_BIND_LOG") != NULL;
    e = getenv("RECOMP_PB_GL_TEXREFRESH");
    if (e && *e)
        s_tex_refresh = (uint32_t)strtoul(e, NULL, 0);

    /* [thug] Presentation switches. */
    s_visible = !(getenv("RECOMP_HEADLESS") && *getenv("RECOMP_HEADLESS")
                  && *getenv("RECOMP_HEADLESS") != '0');
    e = getenv("THUG_VSYNC");
    s_vsync = !(e && e[0] == '0' && !e[1]);
    s_readback = (getenv("THUG_GL_READBACK") && *getenv("THUG_GL_READBACK") == '1')
              || getenv("RECOMP_FB_DUMP") || getenv("RECOMP_FB_DUMP_FLIPS")
              || (getenv("RECOMP_FB_WINDOW") && *getenv("RECOMP_FB_WINDOW") == '1');
    s_stats = getenv("THUG_GL_STATS") != NULL;
    e = getenv("THUG_FPS_LIMIT");
    if (e && *e)
        s_fps_limit = atoi(e) > 0 ? atoi(e) : 0;
    s_zeta_log = getenv("THUG_GL_ZETA_LOG") != NULL;
    e = getenv("THUG_BRIGHTNESS");
    if (e && *e && atof(e) > 0.0)
        nv2a_pb_gl_set_brightness((float)atof(e));
    s_dump_dir = getenv("THUG_GL_DUMP");
    if (s_dump_dir && !*s_dump_dir)
        s_dump_dir = NULL;
    e = getenv("THUG_GL_DUMP_EVERY");
    if (e && *e)
        s_dump_every = (uint32_t)strtoul(e, NULL, 0);

    /* SDL installs SIGINT/SIGTERM handlers that push an SDL_QUIT event, and
     * nothing here pumps events for the hidden window -- so a headless run
     * that initialized SDL video could no longer be stopped with Ctrl+C or
     * `timeout`, which is how every script in tools/ ends a run. Asked for
     * before SDL_Init, as the input layer already does (xinput_device.c). */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* [thug] Let the driver run its own work on a thread of its own: the
     * executor's thread both decodes the pushbuffer and submits every draw,
     * and the driver's share of that is large (NVIDIA's threaded
     * optimisations; Mesa's glthread). Explicit settings win. */
    if (!getenv("THUG_GL_NO_DRIVER_THREAD")) {
        setenv("__GL_THREADED_OPTIMIZATIONS", "1", 0);
        setenv("mesa_glthread", "true", 0);
    }
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[PBGL] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* [thug] The window only ever receives a blit: no depth, no alpha (an
     * alpha channel would let a compositor show the desktop through it). */
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);

    /* [thug] Internal resolution: THUG_RES_SCALE=<x> times the game's
     * 640x480, or by default whatever fills the desktop's height, so a
     * fullscreen window shows the frame 1:1. Every surface scales with it. */
    {
        SDL_DisplayMode dm;
        float sc = 1.0f;
        if (s_visible && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.h > 0)
            s_native_scale = (float)dm.h / 480.0f;
        e = getenv("THUG_RES_SCALE");
        if (e && *e && atof(e) > 0.0)
            sc = (float)atof(e);
        else
            sc = s_native_scale;      /* 1 when headless */
        if (!(sc >= 1.0f))
            sc = 1.0f;
        if (sc > 8.0f)
            sc = 8.0f;
        s_scale = sc;
        /* [thug] Widescreen: the title renders anamorphic 16:9 when the
         * kernel reports the dashboard's widescreen setting (main.c turns
         * RECOMP_WIDESCREEN on unless THUG_WIDESCREEN=0). */
        e = getenv("RECOMP_WIDESCREEN");
        if (e && *e && *e != '0')
            s_aspect = 16.0f / 9.0f;
        s_scale_y = sc;
        s_scale_x = sc * s_aspect / (4.0f / 3.0f);
    }

    {
        /* [thug] The game's window: visible and resizable unless headless.
         * Sized to the largest multiple of 640x480 that fits in 80% of the
         * desktop; THUG_FULLSCREEN=1 (or RECOMP_FULLSCREEN=1) starts it in
         * borderless fullscreen. */
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
        int ww = 640, wh = 480;
        SDL_Rect ub;
        if (s_visible) {
            flags |= SDL_WINDOW_RESIZABLE;
            /* Window sizes are in the desktop's scaled units (a 4K desktop at
             * 150% is 2560x1440 of them), so size from the usable bounds,
             * not from the mode's pixels. */
            if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.h > 0) {
                int k = (int)((float)ub.h * 0.85f) / 480;
                if (k < 1) k = 1;
                wh = 480 * k;
                ww = (int)((float)wh * s_aspect + 0.5f);
                if (ww > ub.w) {
                    ww = ub.w;
                    wh = (int)((float)ww / s_aspect + 0.5f);
                }
            }
            e = getenv("THUG_FULLSCREEN");
            if (!e) e = getenv("RECOMP_FULLSCREEN");
            if (e && *e && *e != '0')
                flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        } else {
            flags |= SDL_WINDOW_HIDDEN;
        }
        s_win = SDL_CreateWindow("Tony Hawk's Underground",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 ww, wh, flags);
    }
    if (!s_win) {
        fprintf(stderr, "[PBGL] SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        fprintf(stderr, "[PBGL] SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(s_win, s_ctx);
    nv2a_pb_gl_drawable_changed();

    if (!program_build())
        return 0;

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* [thug] The ring's storage, a whole number of vertices: mapped once and
     * for good with ARB_buffer_storage, else re-specified when it wraps. */
    if (epoxy_has_gl_extension("GL_ARB_buffer_storage")
            && !getenv("THUG_GL_NO_PERSISTENT")) {
        GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glBufferStorage(GL_ARRAY_BUFFER, VBO_RING_BYTES, NULL, fl);
        s_vbo_ptr = (uint8_t *)glMapBufferRange(GL_ARRAY_BUFFER, 0, VBO_RING_BYTES, fl);
    }
    if (!s_vbo_ptr)
        glBufferData(GL_ARRAY_BUFFER, VBO_RING_BYTES, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 2, GL_UNSIGNED_INT, sizeof(GlVert),
                           (const void *)16);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)24);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)40);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)56);
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)72);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)88);

    /* A texture unit nothing is bound to samples as black, and the CPU
     * rasteriser's failed sample is white (rc_texel); a stage the title
     * programmed but this back end has no texture for reads the same on both
     * sides only if the unit holds white. */
    {
        static const uint8_t white[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &s_white);
        glBindTexture(GL_TEXTURE_2D, s_white);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    glDisable(GL_DITHER);
    glEnable(GL_MULTISAMPLE);

    /* [thug] THUG_GL_DEBUG=1: the driver's error and warning messages
     * (KHR_debug), synchronously, so a breakpoint on gl_debug_message
     * lands at the offending call. Slow; diagnosis only. */
    if (getenv("THUG_GL_DEBUG") && epoxy_has_gl_extension("GL_KHR_debug")) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(gl_debug_message, NULL);
    }

    /* [thug] Multisampling: THUG_MSAA=2|4|8 (the overlay's antialiasing),
     * capped by what the driver allows for colour and depth renderbuffers. */
    {
        GLint mx = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &mx);
        s_msaa_max = mx >= 8 ? 8 : mx >= 4 ? 4 : mx >= 2 ? 2 : 1;
        e = getenv("THUG_MSAA");
        s_msaa = msaa_clamp(e && *e ? atoi(e) : 1);
        fprintf(stderr, "[PBGL] antialiasing: %s (driver allows up to %dx;"
                " alpha-tested edges %s)\n",
                s_msaa > 1 ? (s_msaa == 2 ? "2x MSAA" : s_msaa == 4 ? "4x MSAA"
                                                                    : "8x MSAA") : "off",
                s_msaa_max, s_sample_mask_ok ? "smoothed" : "not smoothed");
    }

    /* [thug] Anisotropic filtering for mip-filtered textures: core in 4.6,
     * an extension everywhere that matters before it. */
    if (epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic")
            || epoxy_has_gl_extension("GL_ARB_texture_filter_anisotropic")) {
        GLfloat mx = 1.0f;
        float want = 16.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &mx);
        s_aniso_max = mx >= 1.0f ? mx : 1.0f;
        e = getenv("THUG_ANISO");
        if (e && *e)
            want = (float)atof(e);
        if (want > mx)
            want = mx;
        s_aniso = want >= 1.0f ? want : 1.0f;
    }

    s_ready = 1;
    /* [thug] With a present_sync only when something reads the frame from
     * guest memory: otherwise the frame never leaves the GPU. */
    nv2a_backend_register_with_present_sync(&s_backend,
                                            s_readback ? gl_present_sync : NULL);
    fprintf(stderr, "[PBGL] OpenGL %s, %s; internal scale %.2f (%ux%u%s), %s,"
            " vsync %s%s\n",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER), (double)s_scale,
            scaled_x(640), scaled_y(480), s_aspect > 1.5f ? ", 16:9" : "",
            s_visible ? "window" : "headless",
            s_vsync ? "on" : "off", s_readback ? ", readback on" : "");
    /* Release it here: GLX lets a context be current on one thread only, and
     * the executor's poll thread is the one that will use it. */
    SDL_GL_MakeCurrent(s_win, NULL);
    return 1;
}

#endif /* !_WIN32 */
