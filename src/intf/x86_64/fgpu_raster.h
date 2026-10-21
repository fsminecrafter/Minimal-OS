#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_abi.h"
#include "x86_64/fgpu_shader.h"

/*
 * FGPU CPU backend: the 3D pipeline in software.
 *
 * Vertex stage (a shader program, or transform + per-vertex lighting),
 * clipping against the near plane, perspective-correct triangle
 * rasterization with a 32-bit float depth buffer, and the fragment stage
 * (a shader program, or vertex color / Gouraud / per-pixel Phong), with
 * optional alpha blending. No kernel dependencies, so it also builds on
 * the host (tests/host/fgpu_raster_tests.c).
 */

typedef struct {
    uint32_t* color;            // XRGB8888, may be NULL
    uint32_t color_pitch;       // in pixels
    float* depth;               // may be NULL
    uint32_t depth_pitch;       // in floats
    uint32_t width, height;
} fgpu_target_t;

// Where each attribute sits in a vertex (FGPU_OP_SET_VERTEX_LAYOUT).
typedef struct {
    uint32_t stride;                    // floats per vertex, 1..FGPU_MAX_VERTEX_STRIDE
    uint32_t count;                     // attributes, <= FGPU_MAX_ATTRIBS
    uint32_t attrib[FGPU_MAX_ATTRIBS];  // FGPU_ATTRIB(offset, components); 0 components = absent
} fgpu_layout_t;

typedef struct {
    int32_t vp_x, vp_y, vp_w, vp_h;     // viewport
    uint32_t state;                     // FGPU_STATE_*
    uint32_t program;                   // FGPU_PROGRAM_* (when shader is NULL)
    fgpu_uniforms_t u;                  // built-in programs
    // ABI 2
    const fgpu_program_t* shader;       // NULL = built-in program
    const float (*constants)[4];        // FGPU_SHADER_MAX_UNIFORMS registers
    fgpu_texture_t textures[FGPU_MAX_TEXTURE_UNITS];
} fgpu_pipeline_t;

// The FGPU_VERTEX_P3N3C4 layout.
void fgpu_layout_default(fgpu_layout_t* layout);

// True if every attribute fits inside the stride.
bool fgpu_layout_valid(const fgpu_layout_t* layout);

void fgpu_raster_clear(const fgpu_target_t* t, uint32_t flags, uint32_t color, float depth);

// Draws triangles. vertices: FGPU_VERTEX_P3N3C4 data, vertex_count of
// them. indices: NULL for 0,1,2,...; otherwise index_count uint32 indices
// (each checked against vertex_count). Returns triangles drawn.
uint32_t fgpu_raster_draw(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                          const float* vertices, uint32_t vertex_count,
                          const uint32_t* indices, uint32_t count);

// The same with vertices in `layout` (must be valid).
uint32_t fgpu_raster_draw_layout(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                                 const fgpu_layout_t* layout,
                                 const float* vertices, uint32_t vertex_count,
                                 const uint32_t* indices, uint32_t count);
