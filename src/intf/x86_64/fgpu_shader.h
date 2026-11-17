#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_abi.h"

/*
 * FGPU shader machine: checks shader program blobs (fgpu_abi.h, "Shader
 * programs") and runs their vertex and fragment stages on the CPU.
 * No kernel dependencies, so it also builds on the host.
 */

typedef struct {
    const uint32_t* pixels;     // ARGB8888 (XRGB surfaces read as opaque)
    uint32_t width, height;
    uint32_t pitch;             // in pixels
    uint32_t flags;             // FGPU_TEXTURE_*
    // Mip chain (ABI 4; all zero = none). Level k > 0 starts mip_offset[k]
    // pixels after `pixels` and is max(1, width >> k) x max(1, height >> k)
    // texels with packed rows (mip_offset[0] is unused).
    uint32_t mip_count;         // levels including level 0; 0 or 1 = no chain
    uint32_t mip_offset[FGPU_MAX_MIP_LEVELS];
    float lod_bias;             // added to the computed level of detail
    float min_lod;              // clamp (0 = none)
    float max_lod;              // clamp (<= 0 = none)
} fgpu_texture_t;

typedef struct {
    const float (*data)[4];
    uint32_t vec4_count;
} fgpu_storage_buffer_t;

// A checked program. The pointers point into the blob it was made from.
typedef struct {
    fgpu_shader_header_t h;
    const uint32_t* vs;
    const uint32_t* fs;
    const float (*consts)[4];
    void* jit;                  // compiled code (fgpu_jit_x64.h), NULL = interpreter only
} fgpu_program_t;

// Checks a blob and fills *out. On failure returns false and, if `why`
// is not NULL, sets it to a short reason.
bool fgpu_program_check(const void* blob, uint32_t bytes, fgpu_program_t* out, const char** why);

typedef struct {
    const fgpu_program_t* program;
    const float (*input)[4];    // attributes or varyings
    float (*output)[4];         // vertex: 1 + varying_count, fragment: 1
    const float (*uniforms)[4]; // FGPU_SHADER_MAX_UNIFORMS registers
    const float (*special)[4];  // fragment stage: 2 registers, else NULL
    const fgpu_texture_t* textures;     // FGPU_TEXTURE_UNITS, NULL = none
} fgpu_shader_io_t;

/* Batches: FGPU_LANES invocations at once (how the rasterizer runs
 * fragment shaders). Registers are [register][component][lane]. */
#define FGPU_LANES 8

typedef struct {
    float temp[FGPU_SHADER_MAX_TEMPS][4][FGPU_LANES];
    float input[FGPU_MAX_ATTRIBS][4][FGPU_LANES];    // attributes or varyings
    float output[1 + FGPU_SHADER_MAX_VARYINGS][4][FGPU_LANES];
    float special[2][4][FGPU_LANES];                // fragment stage
    // Fragment stage: set by the rasterizer when the lanes are 2x2 pixel
    // quads (lanes 4q .. 4q + 3 = (x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1),
    // helper pixels outside the triangle included), so texture sampling can
    // take the level of detail from differences between neighbours. 0 = lanes
    // are unrelated; mipmapped textures then sample level 0. Must be set
    // before every run (nothing resets it).
    uint32_t quad;
} fgpu_shader_batch_t;

// Zeroes the first `temps` temporaries. Call it before reusing a batch
// for another program, so a shader never sees older register contents.
void fgpu_shader_batch_reset(fgpu_shader_batch_t* b, uint32_t temps);

// Runs one stage for the lanes in `lanes` (bit l = lane l); their inputs
// (and specials) must be filled in. Returns the lanes that were not
// discarded. Outputs start as (0, 0, 0, 1) for the position/color, 0 else.
uint32_t fgpu_shader_run_batch(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                               uint32_t lanes, const float (*uniforms)[4], const fgpu_texture_t* textures);
uint32_t fgpu_shader_run_batch_buffers(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                               uint32_t lanes, const float (*uniforms)[4], const fgpu_texture_t* textures,
                               const fgpu_storage_buffer_t* storage_buffers);

/* One invocation at a time (uses a shared batch: not re-entrant). */

// Runs the vertex stage. io->output must have 1 + varying_count registers.
void fgpu_shader_run_vertex(const fgpu_shader_io_t* io);

// Runs the fragment stage. Returns false when the fragment was discarded.
bool fgpu_shader_run_fragment(const fgpu_shader_io_t* io);

// Samples a texture at (u, v): rgba in 0..1, level 0 only. Exposed for tests.
void fgpu_texture_sample(const fgpu_texture_t* t, float u, float v, float out[4]);

// Mip chain layout of a surface of `width` x `height` pixels whose level 0 has `pitch`
// pixels per row (fgpu_abi.h, "Mipmaps"): returns the level count (<= FGPU_MAX_MIP_LEVELS),
// fills offsets[k] (pixels from level 0; offsets[0] = 0) and the total pixel count.
uint32_t fgpu_mip_layout(uint32_t width, uint32_t height, uint32_t pitch,
                         uint32_t offsets[FGPU_MAX_MIP_LEVELS], uint64_t* total_pixels);

// Rebuilds levels 1.. of a chain from level 0 with a 2x2 box filter (per channel, rounded).
void fgpu_mip_generate(uint32_t* pixels, uint32_t width, uint32_t height, uint32_t pitch);

// True when sampling `t` depends on the level of detail (a chain and a MIP flag).
bool fgpu_texture_has_mips(const fgpu_texture_t* t);

// Samples with an explicit level of detail (before the texture's bias and
// clamps): see fgpu_abi.h, "Mipmaps". Textures without a chain: level 0.
void fgpu_texture_sample_lod(const fgpu_texture_t* t, float u, float v, float lod, float out[4]);

// The JIT's and the tests' view of the TEX instruction: fills out[c * FGPU_LANES + lane]
// for lanes 0 .. lanes - 1 from u[], v[]. With `quads` (and a mip chain) the level of
// detail of each lane comes from its 2x2 quad (lane groups of 4, in order).
void fgpu_texture_sample_batch(const fgpu_texture_t* t, const float* u, const float* v,
                               uint32_t lanes, bool quads, float* out);
