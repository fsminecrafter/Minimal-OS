#include "x86_64/nv_3d.h"
#include <stddef.h>
#include "x86_64/nv_sass.h"

/* Pushbuffers for the Turing 3D class; nv_3d.h has the overview. */

#define S 0u        // the 3D class's subchannel

static uint32_t fbits(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

static void m1(nv_push_t* p, uint32_t method, uint32_t value) { nv_push_1(p, S, method, value); }

static void m2(nv_push_t* p, uint32_t method, uint32_t a, uint32_t b) {
    uint32_t v[2] = { a, b };
    nv_push_method(p, S, method, v, 2);
}

static void address(nv_push_t* p, uint32_t method, uint64_t va) {
    m2(p, method, (uint32_t)(va >> 32) & 0xFFu, (uint32_t)va);
}

static void bits(uint32_t* w, uint32_t lo, uint32_t hi, uint32_t v) {
    for (uint32_t b = lo; b < hi; b++, v >>= 1)
        if (v & 1u) w[b / 32u] |= 1u << (b % 32u);
}

void nv3d_zeta_size(uint32_t width, uint32_t height, nv3d_zeta_t* z) {
    // Blocks of one GOB: 64 bytes (16 pixels) by 8 rows.
    z->width = (width + 15u) & ~15u;
    z->height = (height + 7u) & ~7u;
    z->bytes = z->width * z->height * 4u;
}

void nv3d_texture_header(uint32_t out[8], uint64_t va, uint32_t pitch_bytes, uint32_t width,
                         uint32_t height, bool r8) {
    for (int i = 0; i < 8; i++) out[i] = 0;
    if (r8) {
        // One byte per texel (a video plane): x = the byte, y = z = 0, w = 1.
        bits(out, 0, 7, 0x1D);              // R8
        bits(out, 7, 10, 2);                // unsigned normalized
        bits(out, 19, 22, 2);               // x = R
        bits(out, 28, 31, 7);               // w = 1.0
    } else {
        // 0xAARRGGBB words are bytes B, G, R, A: the hardware's A8B8G8R8
        // with red and blue swapped.
        bits(out, 0, 7, 0x08);              // A8B8G8R8
        bits(out, 7, 10, 2);                // r, g, b, a: unsigned normalized
        bits(out, 10, 13, 2);
        bits(out, 13, 16, 2);
        bits(out, 16, 19, 2);
        bits(out, 19, 22, 4);               // x = B
        bits(out, 22, 25, 3);               // y = G
        bits(out, 25, 28, 2);               // z = R
        bits(out, 28, 31, 5);               // w = A
    }
    bits(out, 37, 64, (uint32_t)(va >> 5) & 0x7FFFFFFu);
    bits(out, 64, 80, (uint32_t)(va >> 32) & 0xFFFFu);
    bits(out, 85, 88, 2);                   // pitch-linear header
    bits(out, 96, 112, (pitch_bytes >> 5) & 0xFFFFu);
    bits(out, 112, 113, 1);
    bits(out, 128, 144, width - 1u);
    bits(out, 151, 155, 7);                 // 2D, one level
    bits(out, 157, 160, 7);                 // border color from the sampler
    bits(out, 160, 176, height - 1u);
    bits(out, 191, 192, 1);                 // coordinates 0..1
}

void nv3d_sampler(uint32_t out[8], uint32_t flags) {
    for (int i = 0; i < 8; i++) out[i] = 0;
    uint32_t wrap = (flags & FGPU_TEXTURE_CLAMP) ? 2u : 0u;     // clamp to edge / repeat
    uint32_t filter = (flags & FGPU_TEXTURE_LINEAR) ? 2u : 1u;  // linear / nearest
    out[0] = wrap | (wrap << 3) | (wrap << 6);
    out[1] = filter | (filter << 4) | (1u << 6);                // no mip mapping
}

void nv3d_push_init(nv_push_t* p, uint64_t tic_va, uint64_t tsc_va, uint64_t zero_va) {
    m1(p, NVC597_SET_OBJECT, NV_CLASS_TURING_A);
    m1(p, NVC597_SET_RENDER_ENABLE_C, 1);           // always render
    m1(p, NVC597_SET_Z_COMPRESSION, 0);             // our buffers have no compression tags
    m1(p, NVC597_SET_COLOR_COMPRESSION(0), 0);
    m1(p, NVC597_SET_CT_SELECT, (076543210u << 4) | 1u);
    m1(p, NVC597_SET_CT_MRT_ENABLE, 1);
    m1(p, NVC597_SET_SINGLE_CT_WRITE_CONTROL, 1);
    m1(p, NVC597_SET_SHADE_MODE, 0x1D01);           // smooth
    m1(p, NVC597_CHECK_SPH_VERSION, 3u | (3u << 16));
    m1(p, NVC597_CHECK_AAM_VERSION, 2u | (2u << 16));
    // Attribute components a vertex does not supply read as (0, 0, 0, 1).
    m1(p, NVC597_SET_ATTRIBUTE_DEFAULT, (1u << 1) | (1u << 2));
    m1(p, NVC597_SET_EDGE_FLAG, 1);
    m1(p, NVC597_SET_SAMPLER_BINDING, 0);           // samplers chosen by the handle
    m1(p, NVC597_SET_PRIMITIVE_TOPOLOGY_CONTROL, 0);    // topology comes with BEGIN
    m1(p, NVC597_SET_WINDOW_ORIGIN, 0);             // row 0 is the top
    m2(p, NVC597_SET_WINDOW_OFFSET_X, 0, 0);
    m1(p, NVC597_SET_ANTI_ALIAS, 0);                // one sample per pixel
    m1(p, NVC597_SET_ANTI_ALIAS_ENABLE, 0);
    m1(p, NVC597_SET_SRGB_WRITE, 0);
    m1(p, NVC597_SET_SCISSOR_ENABLE(0), 0);
    m1(p, NVC597_SET_CLEAR_SURFACE_CONTROL, 0);     // clears cover the whole surface clip
    m1(p, NVC597_SET_STENCIL_TEST, 0);
    m1(p, NVC597_SET_ALPHA_TEST, 0);
    m1(p, NVC597_SET_DEPTH_BOUNDS_TEST, 0);
    m1(p, NVC597_SET_API_MANDATED_EARLY_Z, 0);
    m1(p, NVC597_SET_VIEWPORT_Z_CLIP, 0);           // clip z to -w..w
    // Depth is clamped to the viewport's range instead of clipped at the
    // far plane; triangles are clipped where w crosses zero.
    m1(p, NVC597_SET_VIEWPORT_CLIP_CONTROL, (1u << 3) | (1u << 4));
    m1(p, NVC597_SET_DEPTH_FUNC, 0x201);            // less
    m1(p, NVC597_SET_BLEND_STATE_PER_TARGET, 0);
    m1(p, NVC597_SET_BLEND_SEPARATE_FOR_ALPHA, 1);
    // color = src * a + dst * (1 - a); alpha is replaced.
    uint32_t blend[7] = { 0x8006, 0x4302, 0x4303, 0x8006, 0x4001, 0, 0x4000 };
    nv_push_method(p, S, NVC597_SET_BLEND_COLOR_OP, blend, 5);
    m1(p, NVC597_SET_BLEND_COLOR_OP + 0x18u, blend[6]);     // alpha destination factor
    m1(p, NVC597_SET_CT_WRITE(0), 0x1111);
    address(p, NVC597_SET_VERTEX_STREAM_SUBSTITUTE_A, zero_va);
    for (uint32_t s = 1; s < 32; s++) m1(p, NVC597_SET_VERTEX_STREAM_A_FORMAT(s), 0);
    // Only the vertex and pixel stages run.
    for (uint32_t s = 0; s < 6; s++)
        if (s != NV3D_STAGE_VERTEX && s != NV3D_STAGE_PIXEL)
            m1(p, NVC597_SET_PIPELINE_SHADER(s), s << 4);
    uint32_t tic[3] = { (uint32_t)(tic_va >> 32) & 0xFFu, (uint32_t)tic_va, NV3D_POOL_ENTRIES - 1u };
    nv_push_method(p, S, NVC597_SET_TEX_HEADER_POOL_A, tic, 3);
    uint32_t tsc[3] = { (uint32_t)(tsc_va >> 32) & 0xFFu, (uint32_t)tsc_va, NV3D_POOL_ENTRIES - 1u };
    nv_push_method(p, S, NVC597_SET_TEX_SAMPLER_POOL_A, tsc, 3);
}

static void color_target(nv_push_t* p, const nv3d_color_t* c) {
    // Address, pitch (the "width" of a pitch-linear target), height,
    // format, pitch layout, one layer.
    uint32_t rt[9] = {
        (uint32_t)(c->va >> 32) & 0xFFu, (uint32_t)c->va, c->pitch_bytes, c->height,
        NV_RT_FORMAT_BGRA8_UNORM, 1u << 12, 1, 0, 0,
    };
    nv_push_method(p, S, NVC597_SET_COLOR_TARGET_A(0), rt, 9);
    m1(p, NVC597_SET_CT_SELECT, (076543210u << 4) | 1u);
}

static void depth_target(nv_push_t* p, const nv3d_zeta_t* z) {
    if (!z) {
        m1(p, NVC597_SET_ZT_SELECT, 0);
        return;
    }
    // Address, ZF32, blocks of one GOB, bytes per layer / 4.
    uint32_t zt[5] = { (uint32_t)(z->va >> 32) & 0xFFu, (uint32_t)z->va, 0x0A, 0, z->bytes >> 2 };
    nv_push_method(p, S, NVC597_SET_ZT_A, zt, 5);
    uint32_t size[3] = { z->width, z->height, 1 };
    nv_push_method(p, S, NVC597_SET_ZT_SIZE_A, size, 3);
    m1(p, NVC597_SET_ZT_SELECT, 1);
}

static void surface_clip(nv_push_t* p, uint32_t width, uint32_t height) {
    m2(p, NVC597_SET_SURFACE_CLIP_HORIZONTAL, width << 16, height << 16);
}

void nv3d_push_clear(nv_push_t* p, const nv3d_color_t* color, const nv3d_zeta_t* zeta,
                     uint32_t flags, uint32_t xrgb, float depth) {
    uint32_t what = 0;
    if (color) {
        color_target(p, color);
        surface_clip(p, color->width, color->height);
    } else {
        m1(p, NVC597_SET_CT_SELECT, 076543210u << 4);
        if (zeta) surface_clip(p, zeta->width, zeta->height);
    }
    depth_target(p, zeta);
    if ((flags & FGPU_CLEAR_COLOR) && color) {
        const float k = 1.0f / 255.0f;
        uint32_t v[4] = {
            fbits((float)((xrgb >> 16) & 255u) * k), fbits((float)((xrgb >> 8) & 255u) * k),
            fbits((float)(xrgb & 255u) * k), fbits(1.0f),
        };
        nv_push_method(p, S, NVC597_SET_COLOR_CLEAR_VALUE(0), v, 4);
        what |= 0x3Cu;                              // r, g, b, a of target 0
    }
    if ((flags & FGPU_CLEAR_DEPTH) && zeta) {
        m1(p, NVC597_SET_Z_CLEAR_VALUE, fbits(depth));
        what |= 1u;
    }
    if (what) m1(p, NVC597_CLEAR_SURFACE, what);
}

static void stage(nv_push_t* p, uint32_t slot, uint32_t group, const nv3d_stage_t* s) {
    m1(p, NVC597_SET_PIPELINE_SHADER(slot), 1u | (slot << 4));
    address(p, NVC597_SET_PIPELINE_PROGRAM_ADDRESS_A(slot), s->va);
    m2(p, NVC597_SET_PIPELINE_REGISTER_COUNT(slot), s->regs, group);
}

// LOAD_CONSTANT_BUFFER: the first word is the offset, the rest is data.
static void load_constants(nv_push_t* p, uint32_t offset, const uint32_t* words, uint32_t count) {
    if (p->overflow || p->capacity - p->count < 2u + count) { p->overflow = true; return; }
    p->words[p->count++] = nv_push_header(NV_PUSH_TYPE_ONEINC, S, NVC597_LOAD_CONSTANT_BUFFER_OFFSET,
                                          1u + count);
    p->words[p->count++] = offset;
    for (uint32_t i = 0; i < count; i++) p->words[p->count++] = words[i];
}

void nv3d_push_draw(nv_push_t* p, const nv3d_draw_t* d) {
    color_target(p, &d->color);
    surface_clip(p, d->color.width, d->color.height);
    bool depth = d->zeta && (d->state & (FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE));
    depth_target(p, depth ? d->zeta : NULL);

    // Clip space to pixels: y up becomes rows down; z from -1..1 to 0..1.
    float w = (float)d->vp_w, h = (float)d->vp_h;
    uint32_t vp[6] = {
        fbits(w * 0.5f), fbits(-h * 0.5f), fbits(0.5f),
        fbits((float)d->vp_x + w * 0.5f), fbits((float)d->vp_y + h * 0.5f), fbits(0.5f),
    };
    nv_push_method(p, S, NVC597_SET_VIEWPORT_SCALE_X(0), vp, 6);
    int32_t x0 = d->vp_x < 0 ? 0 : d->vp_x, y0 = d->vp_y < 0 ? 0 : d->vp_y;
    int32_t x1 = d->vp_x + d->vp_w, y1 = d->vp_y + d->vp_h;
    if (x1 > (int32_t)d->color.width) x1 = (int32_t)d->color.width;
    if (y1 > (int32_t)d->color.height) y1 = (int32_t)d->color.height;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    uint32_t clip[4] = {
        (uint32_t)x0 | ((uint32_t)(x1 - x0) << 16), (uint32_t)y0 | ((uint32_t)(y1 - y0) << 16),
        fbits(0.0f), fbits(1.0f),
    };
    nv_push_method(p, S, NVC597_SET_VIEWPORT_CLIP_HORIZONTAL(0), clip, 4);

    m1(p, NVC597_SET_DEPTH_TEST, depth ? 1 : 0);
    // Without the test every fragment passes but may still write depth.
    m1(p, NVC597_SET_DEPTH_FUNC, (d->state & FGPU_STATE_DEPTH_TEST) ? 0x201 : 0x207);
    m1(p, NVC597_SET_DEPTH_WRITE, (depth && (d->state & FGPU_STATE_DEPTH_WRITE)) ? 1 : 0);
    bool cull_back = (d->state & FGPU_STATE_CULL_BACK) != 0;
    bool cull_front = (d->state & FGPU_STATE_CULL_FRONT) != 0;
    m1(p, NVC597_OGL_SET_CULL, (cull_back || cull_front) ? 1 : 0);
    m1(p, NVC597_OGL_SET_FRONT_FACE, d->front_ccw ? 0x901 : 0x900);
    m1(p, NVC597_OGL_SET_CULL_FACE, cull_back && cull_front ? 0x408 : cull_front ? 0x404 : 0x405);
    uint32_t mode = (d->state & FGPU_STATE_WIREFRAME) ? 0x1B01u : 0x1B02u;
    m1(p, NVC597_SET_FRONT_POLYGON_MODE, mode);
    m1(p, NVC597_SET_BACK_POLYGON_MODE, mode);
    m1(p, NVC597_SET_BLEND(0), (d->state & FGPU_STATE_BLEND) ? 1 : 0);

    // Vertex stream 0 and where each attribute location sits in it.
    m1(p, NVC597_SET_VERTEX_STREAM_A_FORMAT(0), (d->stride_bytes & 0xFFFu) | (1u << 12));
    address(p, NVC597_SET_VERTEX_STREAM_A_LOCATION_A(0), d->vb_va);
    m2(p, NVC597_SET_VERTEX_STREAM_SIZE_A(0), (uint32_t)(d->vb_bytes >> 32) & 0xFFu, (uint32_t)d->vb_bytes);
    uint32_t attr[FGPU_MAX_ATTRIBS];
    for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++) {
        static const uint8_t format[5] = { 0, 0x12, 0x04, 0x02, 0x01 };    // R32 .. R32G32B32A32
        uint32_t a = i < d->attrib_count ? d->attrib[i] : 0;
        uint32_t comps = (a >> 16) & 0xFFFFu, offset = (a & 0xFFFFu) * 4u;
        if (comps == 0 || comps > 4) attr[i] = 1u << 6;                    // not from a stream
        else attr[i] = (offset << 7) | ((uint32_t)format[comps] << 21) | (7u << 27);    // float
    }
    nv_push_method(p, S, NVC597_SET_VERTEX_ATTRIBUTE_A(0), attr, FGPU_MAX_ATTRIBS);

    // Constants: one buffer for both stages, loaded through the engine so
    // it sees every change.
    uint32_t sel[3] = { NV_SASS_CB_BYTES, (uint32_t)(d->cb_va >> 32) & 0xFFu, (uint32_t)d->cb_va };
    nv_push_method(p, S, NVC597_SET_CONSTANT_BUFFER_SELECTOR_A, sel, 3);
    load_constants(p, 0, (const uint32_t*)d->uniforms, NV_SASS_CB_UNIFORM_BYTES / 4u);
    load_constants(p, NV_SASS_CB_TEX_OFFSET, d->tex_handle, FGPU_MAX_TEXTURE_UNITS);
    m1(p, NVC597_BIND_GROUP_CONSTANT_BUFFER(NV3D_GROUP_VERTEX), 1);        // valid, slot 0
    m1(p, NVC597_BIND_GROUP_CONSTANT_BUFFER(NV3D_GROUP_PIXEL), 1);
    if (d->textures_changed) {
        m1(p, NVC597_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
        m1(p, NVC597_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
    }

    stage(p, NV3D_STAGE_VERTEX, NV3D_GROUP_VERTEX, &d->vs);
    stage(p, NV3D_STAGE_PIXEL, NV3D_GROUP_PIXEL, &d->fs);

    m1(p, NVC597_BEGIN, 4);                         // triangles
    if (d->indexed) {
        address(p, NVC597_SET_INDEX_BUFFER_A, d->ib_va);
        m2(p, NVC597_SET_INDEX_BUFFER_SIZE_A, (uint32_t)(d->ib_bytes >> 32) & 0xFFu, (uint32_t)d->ib_bytes);
        m2(p, NVC597_SET_INDEX_BUFFER_E, 2, 0);     // 32-bit indices, first index 0
        m1(p, NVC597_DRAW_INDEX_BUFFER, d->count);
    } else {
        m2(p, NVC597_SET_VERTEX_ARRAY_START, 0, d->count);     // then DRAW_VERTEX_ARRAY
    }
    m1(p, NVC597_END, 0);
}
