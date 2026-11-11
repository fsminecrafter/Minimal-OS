#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/nv_hw.h"
#include "x86_64/fgpu_abi.h"

/*
 * Pushbuffers for the Turing 3D class (TURING_A, 0xC597): what turns an
 * FGPU clear or draw into methods for the graphics engine. Method
 * addresses and fields are from NVIDIA's clc597.h (open-gpu-doc); the
 * order and the values follow Mesa's NVK driver (nvk_cmd_draw.c).
 *
 * No kernel dependencies (tests/host/nv_sass_tests.c checks the streams).
 * The kernel side - memory, shader upload, submitting - is in
 * fgpu_backend_nv.c.
 *
 * Addresses are GPU virtual addresses. Color targets and textures are
 * pitch-linear XRGB8888 / ARGB8888 (address and pitch multiples of 128
 * bytes for a target, 32 for a texture). Depth is a block-linear ZF32
 * buffer of the backend's own: the hardware has no pitch-linear depth.
 */

#define NV3D_POOL_ENTRIES 16u       // texture headers / samplers (32 bytes each)

typedef struct {
    uint64_t va;
    uint32_t pitch_bytes;
    uint32_t width, height;
} nv3d_color_t;

typedef struct {
    uint64_t va;
    uint32_t width, height;         // as allocated: nv3d_zeta_size
    uint32_t bytes;
} nv3d_zeta_t;

// Size of the depth buffer for a width x height target.
void nv3d_zeta_size(uint32_t width, uint32_t height, nv3d_zeta_t* z);

typedef struct {
    uint64_t va;                    // header (NV_SASS_SPH_WORDS words), then the code
    uint32_t regs;
} nv3d_stage_t;

typedef struct {
    nv3d_color_t color;
    const nv3d_zeta_t* zeta;        // NULL = no depth buffer
    int32_t vp_x, vp_y, vp_w, vp_h;
    uint32_t state;                 // FGPU_STATE_*
    bool front_ccw;                 // what the hardware calls front (found by the self-test)
    nv3d_stage_t vs, fs;
    uint64_t cb_va;                 // constant buffer, NV_SASS_CB_BYTES
    const float* uniforms;          // FGPU_SHADER_MAX_UNIFORMS * 4 floats
    uint32_t tex_handle[FGPU_MAX_TEXTURE_UNITS];    // header index | sampler index << 20
    bool textures_changed;          // headers or samplers were rewritten
    uint32_t stride_bytes;
    uint32_t attrib_count;
    const uint32_t* attrib;         // FGPU_ATTRIB(offset in floats, components)
    uint64_t vb_va, vb_bytes;
    bool indexed;
    uint64_t ib_va, ib_bytes;       // uint32 indices
    uint32_t count;                 // vertices or indices, a multiple of 3
} nv3d_draw_t;

// State that never changes: once per channel, before anything else.
void nv3d_push_init(nv_push_t* p, uint64_t tic_va, uint64_t tsc_va, uint64_t zero_va);
// Clears (FGPU_CLEAR_*) the whole color target and/or depth buffer.
void nv3d_push_clear(nv_push_t* p, const nv3d_color_t* color, const nv3d_zeta_t* zeta,
                     uint32_t flags, uint32_t xrgb, float depth);
void nv3d_push_draw(nv_push_t* p, const nv3d_draw_t* d);

// A texture header for a pitch-linear image - ARGB8888, or with `r8` one
// byte per texel (read as x) - and a sampler for FGPU_TEXTURE_* flags
// (8 words each).
void nv3d_texture_header(uint32_t out[8], uint64_t va, uint32_t pitch_bytes, uint32_t width,
                         uint32_t height, bool r8);
void nv3d_sampler(uint32_t out[8], uint32_t flags);

/* Methods (clc597.h). */
#define NVC597_SET_OBJECT                   0x0000u
#define NVC597_SET_API_MANDATED_EARLY_Z     0x0210u
#define NVC597_SET_INDEX_BUFFER_SIZE_A      0x0238u
#define NVC597_SET_VERTEX_STREAM_SIZE_A(j)  (0x0600u + (j) * 8u)
#define NVC597_SET_COLOR_TARGET_A(j)        (0x0800u + (j) * 64u)
#define NVC597_SET_VIEWPORT_SCALE_X(j)      (0x0A00u + (j) * 32u)
#define NVC597_SET_VIEWPORT_CLIP_HORIZONTAL(j) (0x0C00u + (j) * 16u)
#define NVC597_SET_VERTEX_ARRAY_START       0x0D74u
#define NVC597_DRAW_VERTEX_ARRAY            0x0D78u
#define NVC597_SET_VIEWPORT_Z_CLIP          0x0D7Cu
#define NVC597_SET_COLOR_CLEAR_VALUE(i)     (0x0D80u + (i) * 4u)
#define NVC597_SET_Z_CLEAR_VALUE            0x0D90u
#define NVC597_SET_FRONT_POLYGON_MODE       0x0DACu
#define NVC597_SET_BACK_POLYGON_MODE        0x0DB0u
#define NVC597_SET_WINDOW_OFFSET_X          0x0DF8u
#define NVC597_SET_SCISSOR_ENABLE(j)        (0x0E00u + (j) * 16u)
#define NVC597_SET_VERTEX_STREAM_SUBSTITUTE_A 0x0F84u
#define NVC597_SET_SINGLE_CT_WRITE_CONTROL  0x0F90u
#define NVC597_SET_CT_MRT_ENABLE            0x0FACu
#define NVC597_SET_ZT_A                     0x0FE0u
#define NVC597_SET_SURFACE_CLIP_HORIZONTAL  0x0FF4u
#define NVC597_SET_CLEAR_SURFACE_CONTROL    0x10F8u
#define NVC597_SET_VERTEX_ATTRIBUTE_A(i)    (0x1160u + (i) * 4u)
#define NVC597_SET_CT_SELECT                0x121Cu
#define NVC597_SET_ZT_SIZE_A                0x1228u
#define NVC597_SET_SAMPLER_BINDING          0x1234u
#define NVC597_SET_DEPTH_TEST               0x12CCu
#define NVC597_SET_SHADE_MODE               0x12D4u
#define NVC597_SET_BLEND_STATE_PER_TARGET   0x12E4u
#define NVC597_SET_DEPTH_WRITE              0x12E8u
#define NVC597_SET_ALPHA_TEST               0x12ECu
#define NVC597_SET_DEPTH_FUNC               0x130Cu
#define NVC597_SET_BLEND_SEPARATE_FOR_ALPHA 0x133Cu
#define NVC597_SET_BLEND_COLOR_OP           0x1340u
#define NVC597_SET_BLEND(i)                 (0x1360u + (i) * 4u)
#define NVC597_SET_STENCIL_TEST             0x1380u
#define NVC597_SET_WINDOW_ORIGIN            0x13ACu
#define NVC597_INVALIDATE_SAMPLER_CACHE_NO_WFI 0x1424u
#define NVC597_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI 0x1428u
#define NVC597_SET_ANTI_ALIAS_ENABLE        0x1534u
#define NVC597_SET_ZT_SELECT                0x1538u
#define NVC597_SET_RENDER_ENABLE_C          0x1558u
#define NVC597_SET_TEX_SAMPLER_POOL_A       0x155Cu
#define NVC597_SET_TEX_HEADER_POOL_A        0x1574u
#define NVC597_SET_SRGB_WRITE               0x15B8u
#define NVC597_SET_ANTI_ALIAS               0x15D0u
#define NVC597_SET_EDGE_FLAG                0x15E4u
#define NVC597_SET_ATTRIBUTE_DEFAULT        0x1610u
#define NVC597_END                          0x1614u
#define NVC597_BEGIN                        0x1618u
#define NVC597_CHECK_SPH_VERSION            0x16A8u
#define NVC597_CHECK_AAM_VERSION            0x1794u
#define NVC597_SET_INDEX_BUFFER_A           0x17C8u
#define NVC597_SET_INDEX_BUFFER_E           0x17D8u
#define NVC597_DRAW_INDEX_BUFFER            0x17E0u
#define NVC597_OGL_SET_CULL                 0x1918u
#define NVC597_OGL_SET_FRONT_FACE           0x191Cu
#define NVC597_OGL_SET_CULL_FACE            0x1920u
#define NVC597_SET_VIEWPORT_CLIP_CONTROL    0x193Cu
#define NVC597_SET_PRIMITIVE_TOPOLOGY_CONTROL 0x1948u
#define NVC597_SET_DEPTH_BOUNDS_TEST        0x19BCu
#define NVC597_SET_Z_COMPRESSION            0x19CCu
#define NVC597_CLEAR_SURFACE                0x19D0u
#define NVC597_SET_COLOR_COMPRESSION(i)     (0x19E0u + (i) * 4u)
#define NVC597_SET_CT_WRITE(i)              (0x1A00u + (i) * 4u)
#define NVC597_SET_VERTEX_STREAM_A_FORMAT(j) (0x1C00u + (j) * 16u)
#define NVC597_SET_VERTEX_STREAM_A_LOCATION_A(j) (0x1C04u + (j) * 16u)
#define NVC597_SET_PIPELINE_SHADER(j)       (0x2000u + (j) * 64u)
#define NVC597_SET_PIPELINE_REGISTER_COUNT(j) (0x200Cu + (j) * 64u)
#define NVC597_SET_PIPELINE_PROGRAM_ADDRESS_A(j) (0x2014u + (j) * 64u)
#define NVC597_SET_CONSTANT_BUFFER_SELECTOR_A 0x2380u
#define NVC597_LOAD_CONSTANT_BUFFER_OFFSET  0x238Cu
#define NVC597_BIND_GROUP_CONSTANT_BUFFER(j) (0x2410u + (j) * 32u)

// Pipeline stage slots (the slot number is the shader type) and the
// constant-buffer group each stage reads.
#define NV3D_STAGE_VERTEX   1u
#define NV3D_STAGE_PIXEL    5u
#define NV3D_GROUP_VERTEX   0u
#define NV3D_GROUP_PIXEL    4u
