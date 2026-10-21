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
} fgpu_texture_t;

// A checked program. The pointers point into the blob it was made from.
typedef struct {
    fgpu_shader_header_t h;
    const uint32_t* vs;
    const uint32_t* fs;
    const float (*consts)[4];
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
    const fgpu_texture_t* textures;     // FGPU_MAX_TEXTURE_UNITS, NULL = none
} fgpu_shader_io_t;

/* Batches: FGPU_LANES invocations at once (how the rasterizer runs
 * fragment shaders). Registers are [register][component][lane]. */
#define FGPU_LANES 8

typedef struct {
    float temp[FGPU_SHADER_MAX_TEMPS][4][FGPU_LANES];
    float input[FGPU_MAX_ATTRIBS][4][FGPU_LANES];    // attributes or varyings
    float output[1 + FGPU_SHADER_MAX_VARYINGS][4][FGPU_LANES];
    float special[2][4][FGPU_LANES];                // fragment stage
} fgpu_shader_batch_t;

// Zeroes the first `temps` temporaries. Call it before reusing a batch
// for another program, so a shader never sees older register contents.
void fgpu_shader_batch_reset(fgpu_shader_batch_t* b, uint32_t temps);

// Runs one stage for the lanes in `lanes` (bit l = lane l); their inputs
// (and specials) must be filled in. Returns the lanes that were not
// discarded. Outputs start as (0, 0, 0, 1) for the position/color, 0 else.
uint32_t fgpu_shader_run_batch(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                               uint32_t lanes, const float (*uniforms)[4],
                               const fgpu_texture_t* textures);

/* One invocation at a time (uses a shared batch: not re-entrant). */

// Runs the vertex stage. io->output must have 1 + varying_count registers.
void fgpu_shader_run_vertex(const fgpu_shader_io_t* io);

// Runs the fragment stage. Returns false when the fragment was discarded.
bool fgpu_shader_run_fragment(const fgpu_shader_io_t* io);

// Samples a texture at (u, v): rgba in 0..1. Exposed for tests.
void fgpu_texture_sample(const fgpu_texture_t* t, float u, float v, float out[4]);
