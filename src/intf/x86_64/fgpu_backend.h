#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_raster.h"
#include "x86_64/gpucore.h"

/*
 * FGPU hardware backends.
 *
 * The command processor (fgpu.c) offers every clear and draw to the
 * hardware backend of the primary adapter first; anything the backend
 * does not handle (returns false) runs on the CPU backend (fgpu_raster.c).
 * So a backend can start small (just clears, or just built-in programs)
 * and take over more over time, and programs never notice which one ran.
 *
 * Memory is described both ways: the CPU pointer (for the CPU backend)
 * and, when it is in VRAM, the offset the GPU uses.
 */

typedef struct {
    void* cpu;                  // NULL = none
    bool in_vram;
    uint64_t vram_offset;       // valid when in_vram
    uint64_t bytes;
} fgpu_mem_t;

typedef struct {
    fgpu_target_t cpu;          // the same target as CPU pointers
    fgpu_mem_t color;           // XRGB8888, pitch cpu.color_pitch pixels
    fgpu_mem_t depth;           // float32, pitch cpu.depth_pitch floats
} fgpu_hw_target_t;

typedef struct {
    const fgpu_hw_target_t* target;
    const fgpu_pipeline_t* pipe;
    void* program;              // from program_create; NULL = built-in program or none made
    const fgpu_layout_t* layout;
    fgpu_mem_t vertices;
    uint64_t vertex_offset;     // bytes into vertices
    uint32_t vertex_count;      // vertices available from there
    fgpu_mem_t indices;         // cpu NULL = not indexed
    uint64_t index_offset;      // bytes into indices
    uint32_t count;             // indices, or vertices when not indexed
    fgpu_mem_t textures[FGPU_MAX_TEXTURE_UNITS];    // bound units (cpu NULL = none);
                                // size and flags are in pipe->textures
} fgpu_hw_draw_t;

typedef struct {
    const char* name;           // reported as syscall_gpu_info_t.backend
    uint32_t caps;              // FGPU_CAP_* capabilities actually provided; set by init()

    // Takes the adapter, or returns false and sets *why.
    bool (*init)(gpu_adapter_t* adapter, const char** why);

    // Compiles a checked shader program for the GPU. NULL = the program
    // runs on the CPU (its draws are then not offered to draw()).
    void* (*program_create)(const fgpu_program_t* program);
    void (*program_destroy)(void* program);

    // Return true if the work was queued on the GPU.
    bool (*clear)(const fgpu_hw_target_t* target, uint32_t flags, uint32_t color, float depth);
    bool (*draw)(const fgpu_hw_draw_t* draw, uint32_t* triangles);

    // Waits until the GPU finished everything queued, before the CPU
    // touches a surface or buffer.
    void (*finish)(void);

    // Optional. Copies rows of a surface or buffer in VRAM to system
    // memory faster than the CPU can read VRAM; false = read it directly.
    bool (*read)(const fgpu_mem_t* mem, uint32_t row_bytes, uint32_t rows, uint32_t src_pitch,
                 void* dst, uint32_t dst_pitch);
    // Optional. A surface is going away: drop what the backend keeps for it.
    void (*surface_destroy)(const fgpu_mem_t* mem);
    // Optional. The motion video codec's last stage on the GPU: 4:2:0
    // YCbCr planes (rows of w and cw bytes) to width x height 0xFFRRGGBB
    // pixels (rows of `width`), exactly as the CPU decoder converts them.
    // False = not now; the caller converts on the CPU.
    bool (*video_convert)(const uint8_t* y, const uint8_t* cb, const uint8_t* cr, uint32_t w, uint32_t h,
                          uint32_t cw, uint32_t ch, uint32_t* out, uint32_t width, uint32_t height);
} fgpu_backend_t;

// NVIDIA Turing (fgpu_backend_nv.c). init() fails until GSP-RM and the
// copy engine channel are up ('gpu nvidia boot'); it reports
// FGPU_CAP_HW_3D once the 3D engine passed its draw self-test.
extern fgpu_backend_t fgpu_backend_nvidia;

// The hardware backend in use (NULL = CPU only) and, if none, why.
const fgpu_backend_t* fgpu_hw_backend(const char** why);
