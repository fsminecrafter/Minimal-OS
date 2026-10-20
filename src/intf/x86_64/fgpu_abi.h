#ifndef FGPU_ABI_H
#define FGPU_ABI_H

#include <stdint.h>

/*
 * FGPU: the Farcade GPU interface (SYS_GPU).
 *
 * This header is shared word for word by the kernel
 * (src/intf/x86_64/fgpu_abi.h) and the SDK (include/fgpu_abi.h).
 * Programs normally use FarcadeGL (farcadegl.h) instead of this directly.
 *
 * Model
 *   surface  an image the GPU draws into: color (XRGB8888) or depth
 *            (32-bit float). Placed in VRAM when the adapter has room.
 *   buffer   raw memory for vertices / indices.
 *   command buffer
 *            a list of commands (bind target, clear, set state, set
 *            uniforms, draw...) submitted in one SYS_GPU_SUBMIT call and
 *            executed in order.
 *   program  what runs per vertex and per pixel. Version 1 has built-in
 *            programs (flat color, Gouraud and per-pixel Phong lighting);
 *            the command format leaves room for user shader code later.
 *
 * Command encoding: 32-bit words. Each command starts with a header word
 * (opcode << 16) | total length in words (header included), followed by
 * its payload.
 */

#define SYS_GPU 40
#define SYS_GPU_ABI_VERSION 1u

enum {
    SYS_GPU_INFO = 1,           // out = syscall_gpu_info_t*
    SYS_GPU_CREATE_SURFACE,     // width, height, format, flags -> handle
    SYS_GPU_DESTROY_SURFACE,    // handle
    SYS_GPU_CREATE_BUFFER,      // bytes, flags -> handle
    SYS_GPU_UPLOAD_BUFFER,      // handle, offset, bytes, data
    SYS_GPU_DESTROY_BUFFER,     // handle
    SYS_GPU_SUBMIT,             // data = command words, bytes = size in bytes
    SYS_GPU_READBACK,           // handle -> out (pitch bytes per row)
    SYS_GPU_PRESENT,            // handle -> screen (window_id 0) or window
    SYS_GPU_RELEASE_DISPLAY,    // undo a page-flipped present
};

// Surface formats
#define FGPU_FORMAT_XRGB8888  1u
#define FGPU_FORMAT_DEPTH32F  2u

// Surface flags
#define FGPU_SURFACE_SYSTEM_MEMORY (1u << 0)   // never place in VRAM
#define FGPU_SURFACE_SCANOUT       (1u << 1)   // screen-sized, can be flipped to

// Present flags
#define FGPU_PRESENT_FLIP  (1u << 0)   // page flip if possible (screen only)

#define FGPU_MAX_SURFACE_SIZE 4096u
#define FGPU_MAX_BUFFER_BYTES (16u * 1024u * 1024u)
#define FGPU_MAX_COMMAND_BYTES (1024u * 1024u)

typedef struct {
    uint32_t size;              // sizeof(syscall_gpu_request_t)
    uint32_t version;           // SYS_GPU_ABI_VERSION
    uint32_t op;
    uint32_t handle;            // surface/buffer (in), created handle (out)
    uint32_t width, height;
    uint32_t format;
    uint32_t flags;
    uint64_t bytes;
    uint64_t offset;
    const void* data;
    void* out;
    uint32_t pitch;             // READBACK: bytes per row of out
    uint32_t window_id;         // PRESENT: 0 = screen
    int32_t x, y;               // PRESENT: position on screen
} syscall_gpu_request_t;

#define FGPU_CAP_SCANOUT        (1u << 0)
#define FGPU_CAP_VRAM_HEAP      (1u << 1)
#define FGPU_CAP_PAGE_FLIP      (1u << 2)
#define FGPU_CAP_MODESET        (1u << 3)
#define FGPU_CAP_VRAM_FAST_READ (1u << 4)
#define FGPU_CAP_HW_3D          (1u << 5)

typedef struct {
    uint32_t size;              // sizeof(syscall_gpu_info_t), set by caller
    uint32_t version;
    char name[64];
    char family[40];
    char driver[16];
    char backend[32];           // what executes commands ("cpu")
    uint64_t vram_size;
    uint64_t vram_heap_size;
    uint64_t vram_heap_used;
    uint32_t caps;
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t reserved;
} syscall_gpu_info_t;

/* ------------------------------------------------------------ commands */

#define FGPU_CMD(op, words) ((uint32_t)(((uint32_t)(op) << 16) | (uint32_t)(words)))

enum {
    FGPU_OP_BIND_TARGET = 1,    // color surface, depth surface (0 = none)
    FGPU_OP_VIEWPORT,           // x, y, width, height (int32)
    FGPU_OP_CLEAR,              // flags, color (0xRRGGBB), depth (float bits)
    FGPU_OP_SET_STATE,          // FGPU_STATE_* flags
    FGPU_OP_SET_PROGRAM,        // FGPU_PROGRAM_*
    FGPU_OP_SET_UNIFORMS,       // fgpu_uniforms_t, as words
    FGPU_OP_DRAW,               // fgpu_draw_t, as words
};

#define FGPU_CLEAR_COLOR (1u << 0)
#define FGPU_CLEAR_DEPTH (1u << 1)

#define FGPU_STATE_DEPTH_TEST  (1u << 0)
#define FGPU_STATE_DEPTH_WRITE (1u << 1)
#define FGPU_STATE_CULL_BACK   (1u << 2)
#define FGPU_STATE_CULL_FRONT  (1u << 3)
#define FGPU_STATE_WIREFRAME   (1u << 4)

#define FGPU_PROGRAM_COLOR   1u     // vertex colors, no lighting
#define FGPU_PROGRAM_GOURAUD 2u     // lighting per vertex
#define FGPU_PROGRAM_PHONG   3u     // lighting per pixel

// Vertex layout: position xyz, normal xyz, color rgba (floats, 40 bytes).
#define FGPU_VERTEX_P3N3C4 1u

// Matrices are column-major (OpenGL style): m[col * 4 + row].
typedef struct {
    float mvp[16];              // clip = mvp * position
    float model[16];            // world position
    float normal[16];           // world normal (upper 3x3 used)
    float light_dir[4];         // direction the light travels (world)
    float light_color[4];
    float ambient[4];
    float camera_pos[4];        // world
    float material[4];          // x: specular strength, y: shininess (1-128)
} fgpu_uniforms_t;

typedef struct {
    uint32_t vertex_buffer;
    uint32_t vertex_format;     // FGPU_VERTEX_*
    uint32_t first_vertex;
    uint32_t vertex_count;      // triangles: multiple of 3 (without indices)
    uint32_t index_buffer;      // 0 = none; uint32 indices
    uint32_t first_index;
    uint32_t index_count;
} fgpu_draw_t;

#endif // FGPU_ABI_H
