#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
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

/* Several cores. The rasterizer splits a draw into parts (groups of
 * triangles for the vertex stage, bands of rows for rasterization; each
 * pixel is drawn by one part, in the order the triangles were given, so
 * the picture is the same as on one core) and hands them to run(). */
#define FGPU_RASTER_MAX_WORKERS 16u

typedef void (*fgpu_part_fn)(void* ctx, uint32_t part, uint32_t worker);

typedef struct {
    // Workers available now (>= 1).
    uint32_t (*workers)(void);
    // Calls fn(ctx, part, worker) once for every part < parts and returns
    // when all have finished. Calls running at the same time get
    // different worker numbers, each < FGPU_RASTER_MAX_WORKERS.
    void (*run)(fgpu_part_fn fn, void* ctx, uint32_t parts);
} fgpu_parallel_t;

// NULL: one core (the default).
void fgpu_raster_set_parallel(const fgpu_parallel_t* par);

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

/* ---------------------------------------------------------------- binning
 *
 * The default path records draws and renders them at flush time (see
 * docs/gpu.md, "Binned rasterizer"): a geometry stage (vertex shading,
 * clipping, culling, 16.8 fixed-point setup) bins triangles into 64x64
 * tiles, then workers take tiles and rasterize them into a tile-local copy
 * of color and depth with hierarchical Z.
 *
 * fgpu_raster_draw*() and fgpu_raster_clear() keep their meaning (the result
 * is in the target when they return). A pass keeps draws pending instead,
 * until fgpu_pass_flush(): the caller must flush before anything reads or
 * writes the target, a texture bound by a pending draw, or the memory
 * behind a pending draw's vertices/indices/shader program (unless the draw
 * was recorded with FGPU_DRAW_COPY). Everything else the draws depend on
 * (pipeline state, uniforms, shader constants, layout) is snapshotted. */

typedef enum {
    FGPU_RASTER_BINNED = 0,     // default
    FGPU_RASTER_IMMEDIATE = 1,  // the previous rasterizer: one barrier per draw, scalar
} fgpu_raster_mode_t;

// Selects the path for everything that follows (flush pending work first).
void fgpu_raster_set_mode(fgpu_raster_mode_t mode);
fgpu_raster_mode_t fgpu_raster_get_mode(void);

// Memory for bins and records. Hosted builds default to malloc/free; the
// kernel passes its heap. A NULL result is never fatal: the pass flushes
// early or draws immediately.
void fgpu_raster_set_allocator(void* (*alloc)(size_t), void (*release)(void*));
// Upper bound for bin memory in bytes (default 8 MiB, minimum 1 MiB).
void fgpu_raster_set_bin_budget(size_t bytes);

typedef struct fgpu_pass fgpu_pass_t;

#define FGPU_DRAW_COPY 1u       // copy vertices and indices at record time

fgpu_pass_t* fgpu_pass_create(void);
// Drops pending work without drawing it.
void fgpu_pass_destroy(fgpu_pass_t* pass);
void fgpu_pass_discard(fgpu_pass_t* pass);
bool fgpu_pass_pending(const fgpu_pass_t* pass);

void fgpu_pass_clear(fgpu_pass_t* pass, const fgpu_target_t* t, uint32_t flags, uint32_t color,
                     float depth);
void fgpu_pass_draw_layout(fgpu_pass_t* pass, const fgpu_target_t* t, const fgpu_pipeline_t* p,
                           const fgpu_layout_t* layout, const float* vertices,
                           uint32_t vertex_count, const uint32_t* indices, uint32_t count,
                           uint32_t flags);
// Renders what is pending. Returns the triangles drawn (the value
// fgpu_raster_draw_layout() returns) by everything since the last flush.
uint32_t fgpu_pass_flush(fgpu_pass_t* pass);

// Counters of the last flushes, for tests and the benchmark.
typedef struct {
    uint32_t flushes, rounds, fallbacks;   // fallbacks: items drawn by the immediate path
    uint32_t tris_binned, small_tris, tiles_run;
    uint64_t bin_bytes_peak;
    uint64_t geometry_cycles, tile_cycles;      // TSC cycles spent in the two phases (wall clock of the flush)
} fgpu_raster_stats_t;
void fgpu_raster_get_stats(fgpu_raster_stats_t* out);
void fgpu_raster_reset_stats(void);
