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
 *   program  what runs per vertex and per pixel: one of the built-in
 *            programs (flat color, Gouraud and per-pixel Phong lighting),
 *            or a shader program (ABI version 2): FGPU bytecode for a
 *            vertex and a fragment stage, made by the SDK's FGSL compiler
 *            (fglsl.h) from GLSL-style source. See "Shader programs" below.
 *   texture  any color surface, bound to one of FGPU_MAX_TEXTURE_UNITS
 *            units and sampled by shaders.
 *
 * Command encoding: 32-bit words. Each command starts with a header word
 * (opcode << 16) | total length in words (header included), followed by
 * its payload.
 */

#define SYS_GPU 40
// Version 2 adds shader programs, vertex layouts, textures and blending.
// The kernel still accepts version-1 requests.
#define SYS_GPU_ABI_VERSION 2u
#define SYS_GPU_ABI_VERSION_MIN 1u

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
    // ABI version 2
    SYS_GPU_CREATE_PROGRAM,     // data = shader blob, bytes = its size -> handle
    SYS_GPU_DESTROY_PROGRAM,    // handle
    SYS_GPU_WRITE_SURFACE,      // handle, data = pixels, pitch = bytes per row
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
#define FGPU_MAX_PROGRAM_BYTES (64u * 1024u)

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
#define FGPU_CAP_SHADERS        (1u << 6)   // shader programs (ABI 2)

typedef struct {
    uint32_t size;              // sizeof(syscall_gpu_info_t), set by caller
    uint32_t version;
    char name[64];
    char family[40];
    char driver[16];
    char backend[32];           // what executes commands ("cpu ...")
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
    // ABI version 2
    FGPU_OP_SET_SHADER,         // program handle (0 = back to built-in programs)
    FGPU_OP_SET_CONSTANTS,      // first register, count, then count * 4 floats
    FGPU_OP_SET_VERTEX_LAYOUT,  // stride in floats, count, then count words:
                                //   FGPU_ATTRIB(offset in floats, components)
    FGPU_OP_BIND_TEXTURE,       // unit, color surface (0 = none), FGPU_TEXTURE_* flags
};

#define FGPU_CLEAR_COLOR (1u << 0)
#define FGPU_CLEAR_DEPTH (1u << 1)

#define FGPU_STATE_DEPTH_TEST  (1u << 0)
#define FGPU_STATE_DEPTH_WRITE (1u << 1)
#define FGPU_STATE_CULL_BACK   (1u << 2)
#define FGPU_STATE_CULL_FRONT  (1u << 3)
#define FGPU_STATE_WIREFRAME   (1u << 4)
#define FGPU_STATE_BLEND       (1u << 5)    // color = src * a + dst * (1 - a)

#define FGPU_PROGRAM_COLOR   1u     // vertex colors, no lighting
#define FGPU_PROGRAM_GOURAUD 2u     // lighting per vertex
#define FGPU_PROGRAM_PHONG   3u     // lighting per pixel

// Vertex layout: position xyz, normal xyz, color rgba (floats, 40 bytes).
#define FGPU_VERTEX_P3N3C4 1u
// The layout set with FGPU_OP_SET_VERTEX_LAYOUT (ABI 2). Attribute 0 is the
// position, 1 the normal and 2 the color for the built-in programs.
#define FGPU_VERTEX_LAYOUT 2u

#define FGPU_MAX_ATTRIBS 8u
#define FGPU_MAX_VERTEX_STRIDE 64u          // floats
#define FGPU_ATTRIB(offset, components) ((uint32_t)(offset) | ((uint32_t)(components) << 16))

// Texture flags
#define FGPU_TEXTURE_LINEAR (1u << 0)       // bilinear filtering (else nearest)
#define FGPU_TEXTURE_CLAMP  (1u << 1)       // clamp to the edge (else repeat)
#define FGPU_MAX_TEXTURE_UNITS 4u

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

/* ------------------------------------------------------ shader programs */

/*
 * A shader program is a blob: fgpu_shader_header_t, then the vertex
 * stage's instructions, the fragment stage's instructions and the
 * constant pool (vec4s). Offsets are in 32-bit words from the start.
 *
 * The machine works on vec4 registers in these files:
 *
 *   TEMP     scratch registers, FGPU_SHADER_MAX_TEMPS of them
 *   INPUT    vertex stage: attributes 0..7 (missing components read as
 *            0, 0, 0, 1); fragment stage: varyings 0..varying_count-1
 *   OUTPUT   vertex stage: 0 = clip position, 1.. = varyings;
 *            fragment stage: 0 = color (rgba, 0..1)
 *   UNIFORM  constants set with FGPU_OP_SET_CONSTANTS
 *   CONST    the program's constant pool
 *   SPECIAL  fragment stage: 0 = fragment coordinate (x, y, depth, 1/w),
 *            1 = front facing (1 or 0 in every component)
 *
 * An instruction is five words: opcode, destination, sources a, b, c.
 * Sources pick a register, a swizzle and optional abs / negate
 * (applied in that order). The destination has a write mask. All sources
 * are read before the destination is written. Jumps may only go forward,
 * so every program ends after at most its instruction count; the kernel
 * checks every instruction when the program is created.
 */

#define FGPU_SHADER_MAGIC   0x4C534746u     // "FGSL"
#define FGPU_SHADER_VERSION 1u

#define FGPU_SHADER_MAX_INSTRUCTIONS 2048u  // per stage
#define FGPU_SHADER_MAX_TEMPS     32u
#define FGPU_SHADER_MAX_VARYINGS  8u
#define FGPU_SHADER_MAX_UNIFORMS  64u       // vec4 registers (a mat4 takes 4)
#define FGPU_SHADER_MAX_CONSTANTS 256u

enum {
    FGPU_FILE_NONE = 0,
    FGPU_FILE_TEMP,
    FGPU_FILE_INPUT,
    FGPU_FILE_OUTPUT,
    FGPU_FILE_UNIFORM,
    FGPU_FILE_CONST,
    FGPU_FILE_SPECIAL,
};

// Source operand: file | index | swizzle (2 bits per lane, x=0..w=3) | modifiers
#define FGPU_SRC(file, index, swizzle) \
    ((uint32_t)(file) | ((uint32_t)(index) << 4) | ((uint32_t)(swizzle) << 12))
#define FGPU_SRC_NEGATE (1u << 20)
#define FGPU_SRC_ABS    (1u << 21)
#define FGPU_SWIZZLE(x, y, z, w) ((x) | ((y) << 2) | ((z) << 4) | ((w) << 6))
#define FGPU_SWIZZLE_XYZW FGPU_SWIZZLE(0, 1, 2, 3)
// Destination operand: file | index | write mask (bit 0 = x ... bit 3 = w)
#define FGPU_DST(file, index, mask) \
    ((uint32_t)(file) | ((uint32_t)(index) << 4) | ((uint32_t)(mask) << 12))

#define FGPU_OPERAND_FILE(w)    ((w) & 0xFu)
#define FGPU_OPERAND_INDEX(w)   (((w) >> 4) & 0xFFu)
#define FGPU_OPERAND_SWIZZLE(w) (((w) >> 12) & 0xFFu)
#define FGPU_DST_MASK(w)        (((w) >> 12) & 0xFu)

enum {
    FGPU_SOP_NOP = 0,
    FGPU_SOP_MOV,       // d = a
    FGPU_SOP_ADD,       // d = a + b
    FGPU_SOP_SUB,       // d = a - b
    FGPU_SOP_MUL,       // d = a * b
    FGPU_SOP_DIV,       // d = a / b (x / 0 = 0)
    FGPU_SOP_MAD,       // d = a * b + c
    FGPU_SOP_MIN,
    FGPU_SOP_MAX,
    FGPU_SOP_DP2,       // every lane = dot(a.xy, b.xy)
    FGPU_SOP_DP3,
    FGPU_SOP_DP4,
    FGPU_SOP_CROSS,     // d.xyz = cross(a.xyz, b.xyz), d.w = 0
    FGPU_SOP_FLOOR,
    FGPU_SOP_FRACT,     // a - floor(a)
    FGPU_SOP_SQRT,
    FGPU_SOP_RSQ,       // 1 / sqrt(a) (0 for a <= 0)
    FGPU_SOP_EXP2,
    FGPU_SOP_LOG2,
    FGPU_SOP_POW,       // a ^ b (0 for a <= 0)
    FGPU_SOP_SIN,
    FGPU_SOP_COS,
    FGPU_SOP_MIX,       // a + (b - a) * c
    FGPU_SOP_CLAMP,     // min(max(a, b), c)
    FGPU_SOP_STEP,      // a < b ? 0 : 1  (GLSL step(edge = a, x = b))
    FGPU_SOP_SSTEP,     // smoothstep(edge0 = a, edge1 = b, x = c)
    FGPU_SOP_SLT,       // a <  b ? 1 : 0
    FGPU_SOP_SLE,       // a <= b ? 1 : 0
    FGPU_SOP_SEQ,       // a == b ? 1 : 0
    FGPU_SOP_SNE,       // a != b ? 1 : 0
    FGPU_SOP_SEL,       // a != 0 ? b : c
    FGPU_SOP_MOD,       // a - b * floor(a / b)
    FGPU_SOP_SIGN,
    FGPU_SOP_ATAN2,     // atan(a, b) = angle of (b, a)
    FGPU_SOP_TEX,       // d = sample texture unit b (raw word) at a.xy
    FGPU_SOP_KILL,      // fragment stage: discard if a.x != 0
    FGPU_SOP_JMP,       // jump to instruction (destination word)
    FGPU_SOP_JZ,        // jump to instruction (destination word) if a.x == 0
    FGPU_SOP_COUNT
};

#define FGPU_SHADER_FLAG_DISCARD (1u << 0)  // fragment stage may discard

typedef struct {
    uint32_t magic;             // FGPU_SHADER_MAGIC
    uint32_t version;           // FGPU_SHADER_VERSION
    uint32_t size;              // whole blob, bytes
    uint32_t flags;             // FGPU_SHADER_FLAG_*
    uint32_t vs_offset, vs_count;       // words, instructions
    uint32_t fs_offset, fs_count;
    uint32_t const_offset, const_count; // words, vec4s
    uint32_t attrib_mask;       // attributes the vertex stage reads
    uint32_t varying_count;     // vec4 varyings from vertex to fragment stage
    uint32_t uniform_count;     // vec4 uniform registers used
    uint32_t texture_mask;      // texture units the shaders sample
    uint32_t temp_count;        // TEMP registers used (<= FGPU_SHADER_MAX_TEMPS)
    uint32_t reserved;
} fgpu_shader_header_t;

#define FGPU_SHADER_INSTRUCTION_WORDS 5u

#endif // FGPU_ABI_H
