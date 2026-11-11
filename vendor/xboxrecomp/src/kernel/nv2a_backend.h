/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * A batch carries the register-combiner state (nv2a_combiner.h) and every
 * texture stage the title programmed, so a back end can evaluate the pixel
 * stage the way the CPU rasteriser does. "Diffuse times stage 0" is not the
 * same function: it paints opaque black where a title's combiner takes the
 * colour from the alpha channel, which is what menu text is made of.
 * (Interface extended to match the otogi1 fork, sigods/xboxrecomp.)
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

#include "nv2a_combiner.h"

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    /* Where the clip rectangle sits inside that surface, in pixels: the
     * rasteriser writes at (x + column, y + row), so a back end that puts
     * pixels back into guest memory needs both. */
    uint32_t x, y;
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    /* Guest address of the 256-entry A8R8G8B8 palette an 8-bit indexed
     * format (0x0B, SZ_I8_A8R8G8B8) reads, from SET_TEXTURE_PALETTE; 0 for
     * none. nv2a_backend_decode_texture uses it. */
    uint32_t palette;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB: the combiners' v0 */
    float    u, v;              /* stage 0 coordinates, in texels */
    /* Colour 1 (D3D's specular) and the fog factor: the combiners' v1 and
     * fog inputs. */
    uint32_t specular;          /* 0xAARRGGBB */
    float    fogf;
    /* Every stage's texture coordinates as the vertex stage produced them
     * (normalised for swizzled textures, texels for linear), w carrying q. */
    float    st[4][4];
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* SURFACE_FORMAT as the title set it: the zeta format in bits 4-7 (1 is
     * Z16, 2 is Z24S8) says how many bits the title's depth lives in. */
    uint32_t zeta_format;
    /* SET_POLY_OFFSET_FILL_ENABLE (0x338) and SET_POLYGON_OFFSET_SCALE_FACTOR
     * / _BIAS (0x384, 0x388): depth bias for filled polygons, as glPolygonOffset takes it
     * (bias in units of the zeta format's least significant bit). Decals
     * drawn on the surface they sit on depend on it. */
    uint32_t poly_offset_fill;
    float    poly_offset_factor, poly_offset_units;
    /* SET_CONTROL0 (0x290): bit 16 Z_PERSPECTIVE_ENABLE (a w-buffer: depth
     * is the clip-space w rather than z), bit 12 a floating-point z format,
     * bit 0 stencil write enable. */
    uint32_t control0;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* stage 0; NULL: untextured */
    /* One entry per stage, NULL where the title has none programmed. */
    const Nv2aTexture *textures[4];
    const Nv2aRenderState *state;
    /* The register-combiner state in force when the batch was submitted, or
     * NULL if the title never programmed any (then modulating by stage 0 is
     * the whole pixel stage, as in the CPU rasteriser's fallback). */
    const Nv2aCombiner   *combiner;
    /* SET_COMBINER_FOG_COLOR as the register holds it (R in the low byte). */
    uint32_t              fog_color;
    /* SET_CLIP_PLANE_MODE: one nibble per stage, the compare the clip-plane
     * stage (SHADER_STAGE_PROGRAM 5) applies to each coordinate. */
    uint32_t              clip_plane_mode;
} Nv2aBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
