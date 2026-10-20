#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_abi.h"

/*
 * FGPU CPU backend: the 3D pipeline in software.
 *
 * Vertex stage (transform + per-vertex lighting), clipping against the
 * near plane, perspective-correct triangle rasterization with a 32-bit
 * float depth buffer, and the fragment stage (vertex color, Gouraud or
 * per-pixel Phong lighting). No kernel dependencies, so it also builds
 * on the host (tests/host/fgpu_raster_tests.c).
 */

typedef struct {
    uint32_t* color;            // XRGB8888, may be NULL
    uint32_t color_pitch;       // in pixels
    float* depth;               // may be NULL
    uint32_t depth_pitch;       // in floats
    uint32_t width, height;
} fgpu_target_t;

typedef struct {
    int32_t vp_x, vp_y, vp_w, vp_h;     // viewport
    uint32_t state;                     // FGPU_STATE_*
    uint32_t program;                   // FGPU_PROGRAM_*
    fgpu_uniforms_t u;
} fgpu_pipeline_t;

void fgpu_raster_clear(const fgpu_target_t* t, uint32_t flags, uint32_t color, float depth);

// Draws triangles. vertices: FGPU_VERTEX_P3N3C4 data, vertex_count of
// them. indices: NULL for 0,1,2,...; otherwise index_count uint32 indices
// (each checked against vertex_count). Returns triangles drawn.
uint32_t fgpu_raster_draw(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                          const float* vertices, uint32_t vertex_count,
                          const uint32_t* indices, uint32_t count);
