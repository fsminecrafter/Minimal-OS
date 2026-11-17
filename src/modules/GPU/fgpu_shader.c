// O3: lets GCC turn the per-lane loops (and the math below) into SSE code.
#pragma GCC optimize("O3")
#include "x86_64/fgpu_shader.h"
#include <stddef.h>
#if !__STDC_HOSTED__ && !defined(FGPU_ENABLE_JIT)
#define FGPU_ENABLE_JIT 1       // the kernel always has the JIT (fgpu_jit_x64.c); the host opts in
#endif
#ifdef FGPU_ENABLE_JIT
#include "x86_64/fgpu_jit_x64.h"
#endif

/*
 * FGPU shader machine (CPU backend).
 *
 * fgpu_program_check() validates every instruction once, when a program
 * is created, so the interpreter below can index registers without
 * checks. Jumps only go forward, so a stage runs at most `count`
 * instructions.
 */

#define VS_OUTPUTS (1u + FGPU_SHADER_MAX_VARYINGS)

/* ---------------------------------------------------------------- math */

static inline float m_sqrt(float x) {
    if (!(x > 0.0f)) return 0.0f;
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline float m_abs(float x) { return x < 0.0f ? -x : x; }

static inline float m_floor(float x) {
    // Values this large are already whole (and would overflow the int).
    bool small = m_abs(x) < 8388608.0f;
    float t = (float)(int32_t)(small ? x : 0.0f);
    t = t > x ? t - 1.0f : t;
    return small ? t : x;
}

#define M_PI_F 3.14159265358979323846f

/* The functions below avoid branches (only selects), so loops over the
 * lanes that call them can be vectorized. */

static inline float bits_to_float(uint32_t u) {
    float f;
    __builtin_memcpy(&f, &u, 4);
    return f;
}

static inline uint32_t float_to_bits(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

// floor() for |x| < 2^23 without branches (callers clamp the range).
static inline float floor_small(float x) {
    float t = (float)(int32_t)x;
    return t > x ? t - 1.0f : t;
}

static inline float m_sin(float x) {
    bool ok = m_abs(x) < 1e7f;          // also false for NaN
    x = ok ? x : 0.0f;
    // Reduce to [-pi, pi], then fold to [-pi/2, pi/2].
    float k = floor_small(x * (1.0f / (2.0f * M_PI_F)) + 0.5f);
    x -= k * (2.0f * M_PI_F);
    x = x > M_PI_F / 2 ? M_PI_F - x : x;
    x = x < -M_PI_F / 2 ? -M_PI_F - x : x;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f *
           (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}

static inline float m_cos(float x) { return m_sin(x + M_PI_F / 2); }

static inline float m_exp2(float x) {
    bool tiny = !(x >= -126.0f);        // also catches NaN
    x = x > 127.0f ? 127.0f : x;
    x = tiny ? 0.0f : x;
    float fi = floor_small(x);
    float f = x - fi;                   // [0, 1)
    // 2^f on [0, 1): polynomial, error ~2e-7
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f +
              f * (0.0096181f + f * (0.0013333f + f * 0.0001540f)))));
    float scale = bits_to_float((uint32_t)((int32_t)fi + 127) << 23);
    return tiny ? 0.0f : p * scale;
}

static inline float m_log2(float x) {
    bool ok = x > 0.0f;
    uint32_t u = float_to_bits(ok ? x : 1.0f);
    int32_t e = (int32_t)((u >> 23) & 0xFF) - 127;
    float m = bits_to_float((u & 0x007FFFFFu) | 0x3F800000u);  // [1, 2)
    bool big = m > 1.41421356f;
    m = big ? m * 0.5f : m;                                     // [0.707, 1.414)
    float ef = (float)e + (big ? 1.0f : 0.0f);
    // log2(m) = 2/ln2 * atanh((m - 1) / (m + 1))
    float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    float s = t * (2.0f + t2 * (0.6666667f + t2 * (0.4f + t2 * (0.2857143f + t2 * 0.2222222f))));
    return ok ? ef + s * 1.44269504f : -126.0f;
}

static float m_atan(float x) {
    // atan on [0, 1] after range reduction; error ~1e-5
    bool neg = x < 0.0f;
    if (neg) x = -x;
    bool inv = x > 1.0f;
    if (inv) x = 1.0f / x;
    float x2 = x * x;
    float r = x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f +
              x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
    if (inv) r = M_PI_F / 2 - r;
    return neg ? -r : r;
}

static float m_atan2(float y, float x) {
    if (x > 0.0f) return m_atan(y / x);
    if (x < 0.0f) return y >= 0.0f ? m_atan(y / x) + M_PI_F : m_atan(y / x) - M_PI_F;
    if (y > 0.0f) return M_PI_F / 2;
    if (y < 0.0f) return -M_PI_F / 2;
    return 0.0f;
}

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

/* ------------------------------------------------------------ textures */

// One level of a texture: level 0 is the texture itself, level k > 0 sits at
// mip_offset[k] with tightly packed rows (see fgpu_abi.h, "Mipmaps").
typedef struct {
    const uint32_t* px;
    int32_t w, h;
    size_t pitch;
    bool clamp;
} level_t;

static inline level_t level_of(const fgpu_texture_t* t, uint32_t k) {
    level_t l;
    l.clamp = (t->flags & FGPU_TEXTURE_CLAMP) != 0;
    if (k == 0) {
        l.px = t->pixels; l.w = (int32_t)t->width; l.h = (int32_t)t->height; l.pitch = t->pitch;
    } else {
        int32_t w = (int32_t)(t->width >> k), h = (int32_t)(t->height >> k);
        l.w = w ? w : 1;
        l.h = h ? h : 1;
        l.px = t->pixels + t->mip_offset[k];
        l.pitch = (size_t)l.w;
    }
    return l;
}

static inline void texel(const level_t* t, int32_t x, int32_t y, float out[4]) {
    if (t->clamp) {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= t->w) x = t->w - 1;
        if (y >= t->h) y = t->h - 1;
    } else {
        x %= t->w;
        y %= t->h;
        if (x < 0) x += t->w;
        if (y < 0) y += t->h;
    }
    uint32_t p = t->px[(size_t)y * t->pitch + (size_t)x];
    const float k = 1.0f / 255.0f;
    out[0] = (float)((p >> 16) & 255u) * k;
    out[1] = (float)((p >> 8) & 255u) * k;
    out[2] = (float)(p & 255u) * k;
    out[3] = (float)(p >> 24) * k;
}

// u, v are already range checked.
static inline void sample_level(const level_t* t, float u, float v, bool linear, float out[4]) {
    float x = u * (float)t->w, y = v * (float)t->h;
    if (!linear) {
        texel(t, (int32_t)m_floor(x), (int32_t)m_floor(y), out);
        return;
    }
    x -= 0.5f;
    y -= 0.5f;
    float fx0 = m_floor(x), fy0 = m_floor(y);
    float ax = x - fx0, ay = y - fy0;
    int32_t x0 = (int32_t)fx0, y0 = (int32_t)fy0;
    float a[4], b[4], c[4], d[4];
    texel(t, x0, y0, a);
    texel(t, x0 + 1, y0, b);
    texel(t, x0, y0 + 1, c);
    texel(t, x0 + 1, y0 + 1, d);
    for (int i = 0; i < 4; i++) {
        float top = a[i] + (b[i] - a[i]) * ax;
        float bottom = c[i] + (d[i] - c[i]) * ax;
        out[i] = top + (bottom - top) * ay;
    }
}

void fgpu_texture_sample(const fgpu_texture_t* t, float u, float v, float out[4]) {
    if (!t || !t->pixels || !t->width || !t->height) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    // Keep coordinates in a range the int conversions below can hold.
    if (!(m_abs(u) < 65536.0f)) u = 0.0f;
    if (!(m_abs(v) < 65536.0f)) v = 0.0f;
    level_t l = level_of(t, 0);
    sample_level(&l, u, v, (t->flags & FGPU_TEXTURE_LINEAR) != 0, out);
}

// Does sampling this texture depend on the level of detail?
static inline bool texture_uses_lod(const fgpu_texture_t* t) {
    return t && t->pixels && t->mip_count > 1 &&
           (t->flags & (FGPU_TEXTURE_MIP_NEAREST | FGPU_TEXTURE_MIP_LINEAR));
}

bool fgpu_texture_has_mips(const fgpu_texture_t* t) { return texture_uses_lod(t); }

void fgpu_texture_sample_lod(const fgpu_texture_t* t, float u, float v, float lod, float out[4]) {
    if (!texture_uses_lod(t) || t->width == 0 || t->height == 0) {
        fgpu_texture_sample(t, u, v, out);
        return;
    }
    if (!(m_abs(u) < 65536.0f)) u = 0.0f;
    if (!(m_abs(v) < 65536.0f)) v = 0.0f;
    bool linear = (t->flags & FGPU_TEXTURE_LINEAR) != 0;
    lod += t->lod_bias;
    if (!(lod == lod)) lod = 0.0f;                  // NaN
    if (lod < t->min_lod) lod = t->min_lod;
    if (t->max_lod > 0.0f && lod > t->max_lod) lod = t->max_lod;
    float last = (float)(t->mip_count - 1);
    if (!(lod > 0.0f)) {                            // magnification: the base level
        level_t l = level_of(t, 0);
        sample_level(&l, u, v, linear, out);
        return;
    }
    if (lod > last) lod = last;
    if (t->flags & FGPU_TEXTURE_MIP_LINEAR) {
        uint32_t k0 = (uint32_t)m_floor(lod);
        if (k0 >= t->mip_count - 1) {
            level_t l = level_of(t, t->mip_count - 1);
            sample_level(&l, u, v, linear, out);
            return;
        }
        float f = lod - (float)k0;
        level_t l0 = level_of(t, k0), l1 = level_of(t, k0 + 1);
        float a[4], b[4];
        sample_level(&l0, u, v, linear, a);
        sample_level(&l1, u, v, linear, b);
        for (int i = 0; i < 4; i++) out[i] = a[i] + (b[i] - a[i]) * f;
        return;
    }
    uint32_t k = (uint32_t)m_floor(lod + 0.5f);
    if (k > t->mip_count - 1) k = t->mip_count - 1;
    level_t l = level_of(t, k);
    sample_level(&l, u, v, linear, out);
}

// lod = log2 of the longest side of the pixel's footprint in texels, from the
// coordinate differences to the neighbours in the 2x2 quad (lane = 2 * row + column).
static inline float quad_lod(const fgpu_texture_t* t, const float* u, const float* v, uint32_t l) {
    uint32_t row = l & 2u, col = l & 1u, base = l & ~3u;
    float dudx = u[base + row + 1] - u[base + row], dvdx = v[base + row + 1] - v[base + row];
    float dudy = u[base + 2 + col] - u[base + col], dvdy = v[base + 2 + col] - v[base + col];
    float w = (float)t->width, h = (float)t->height;
    float ax = dudx * w, ay = dvdx * h, bx = dudy * w, by = dvdy * h;
    float rx = ax * ax + ay * ay, ry = bx * bx + by * by;
    float r2 = rx > ry ? rx : ry;
    if (!(r2 > 0.0f)) return -126.0f;
    if (!(r2 < 3.0e38f)) return 126.0f;
    return 0.5f * m_log2(r2);
}

void fgpu_texture_sample_batch(const fgpu_texture_t* t, const float* u, const float* v,
                               uint32_t lanes, bool quads, float* out) {
    // out is [4][FGPU_LANES]; lanes = how many lanes to fill (from 0).
    bool lod = quads && texture_uses_lod(t);
    for (uint32_t l = 0; l < lanes; l++) {
        float px[4];
        if (lod) fgpu_texture_sample_lod(t, u[l], v[l], quad_lod(t, u, v, l), px);
        else fgpu_texture_sample(t, u[l], v[l], px);
        for (int c = 0; c < 4; c++) out[c * FGPU_LANES + l] = px[c];
    }
}

/* ------------------------------------------------------- mip chains */

uint32_t fgpu_mip_layout(uint32_t width, uint32_t height, uint32_t pitch,
                         uint32_t offsets[FGPU_MAX_MIP_LEVELS], uint64_t* total_pixels) {
    uint32_t levels = 1;
    uint32_t m = width > height ? width : height;
    while ((m >> levels) && levels < FGPU_MAX_MIP_LEVELS) levels++;
    uint64_t at = (uint64_t)pitch * height;
    if (offsets) offsets[0] = 0;
    for (uint32_t k = 1; k < levels; k++) {
        uint32_t w = width >> k, h = height >> k;
        if (!w) w = 1;
        if (!h) h = 1;
        if (offsets) offsets[k] = (uint32_t)at;
        at += (uint64_t)w * h;
    }
    if (total_pixels) *total_pixels = at;
    return levels;
}

static inline uint32_t box4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= ((((a >> shift) & 255u) + ((b >> shift) & 255u) + ((c >> shift) & 255u) +
                 ((d >> shift) & 255u) + 2u) >> 2) << shift;
    return out;
}

void fgpu_mip_generate(uint32_t* pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    uint32_t offsets[FGPU_MAX_MIP_LEVELS];
    uint32_t levels = fgpu_mip_layout(width, height, pitch, offsets, NULL);
    const uint32_t* src = pixels;
    uint32_t sw = width, sh = height;
    size_t spitch = pitch;
    for (uint32_t k = 1; k < levels; k++) {
        uint32_t dw = sw >> 1, dh = sh >> 1;
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        uint32_t* dst = pixels + offsets[k];
        for (uint32_t y = 0; y < dh; y++) {
            uint32_t y0 = 2 * y, y1 = 2 * y + 1;
            if (y0 > sh - 1) y0 = sh - 1;
            if (y1 > sh - 1) y1 = sh - 1;
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t x0 = 2 * x, x1 = 2 * x + 1;
                if (x0 > sw - 1) x0 = sw - 1;
                if (x1 > sw - 1) x1 = sw - 1;
                dst[(size_t)y * dw + x] = box4(src[y0 * spitch + x0], src[y0 * spitch + x1],
                                               src[y1 * spitch + x0], src[y1 * spitch + x1]);
            }
        }
        src = dst;
        sw = dw;
        sh = dh;
        spitch = dw;
    }
}

/* ---------------------------------------------------------- validation */

static bool op_has_dst(uint32_t op) {
    return op != FGPU_SOP_NOP && op != FGPU_SOP_KILL && op != FGPU_SOP_JMP && op != FGPU_SOP_JZ;
}

// Which of a, b, c each opcode reads (bit 0 = a).
static const uint8_t k_op_sources[FGPU_SOP_COUNT] = {
    [FGPU_SOP_NOP] = 0, [FGPU_SOP_MOV] = 1, [FGPU_SOP_ADD] = 3, [FGPU_SOP_SUB] = 3,
    [FGPU_SOP_MUL] = 3, [FGPU_SOP_DIV] = 3, [FGPU_SOP_MAD] = 7, [FGPU_SOP_MIN] = 3,
    [FGPU_SOP_MAX] = 3, [FGPU_SOP_DP2] = 3, [FGPU_SOP_DP3] = 3, [FGPU_SOP_DP4] = 3,
    [FGPU_SOP_CROSS] = 3, [FGPU_SOP_FLOOR] = 1, [FGPU_SOP_FRACT] = 1, [FGPU_SOP_SQRT] = 1,
    [FGPU_SOP_RSQ] = 1, [FGPU_SOP_EXP2] = 1, [FGPU_SOP_LOG2] = 1, [FGPU_SOP_POW] = 3,
    [FGPU_SOP_SIN] = 1, [FGPU_SOP_COS] = 1, [FGPU_SOP_MIX] = 7, [FGPU_SOP_CLAMP] = 7,
    [FGPU_SOP_STEP] = 3, [FGPU_SOP_SSTEP] = 7, [FGPU_SOP_SLT] = 3, [FGPU_SOP_SLE] = 3,
    [FGPU_SOP_SEQ] = 3, [FGPU_SOP_SNE] = 3, [FGPU_SOP_SEL] = 7, [FGPU_SOP_MOD] = 3,
    [FGPU_SOP_SIGN] = 1, [FGPU_SOP_ATAN2] = 3, [FGPU_SOP_TEX] = 1, [FGPU_SOP_KILL] = 1,
    [FGPU_SOP_JMP] = 0, [FGPU_SOP_JZ] = 1, [FGPU_SOP_BUFFER_LOAD] = 1,
};

static inline uint32_t op_sources(uint32_t op) { return k_op_sources[op]; }

static bool check_src(const fgpu_shader_header_t* h, uint32_t w, bool fragment) {
    uint32_t file = FGPU_OPERAND_FILE(w), index = FGPU_OPERAND_INDEX(w);
    if (w & ~0x3FFFFFu) return false;           // unknown modifier bits
    switch (file) {
        case FGPU_FILE_TEMP:    return index < h->temp_count;
        case FGPU_FILE_INPUT:   return fragment ? index < h->varying_count : index < FGPU_MAX_ATTRIBS;
        case FGPU_FILE_OUTPUT:  return fragment ? index < 1 : index < 1 + h->varying_count;
        case FGPU_FILE_UNIFORM: return index < FGPU_SHADER_MAX_UNIFORMS;
        case FGPU_FILE_CONST:   return index < h->const_count;
        case FGPU_FILE_SPECIAL: return fragment && index < 2;
        default:                return false;
    }
}

static bool check_stage(const fgpu_shader_header_t* h, const uint32_t* code, uint32_t count,
                        bool fragment, const char** why) {
    for (uint32_t pc = 0; pc < count; pc++) {
        const uint32_t* in = code + (size_t)pc * FGPU_SHADER_INSTRUCTION_WORDS;
        uint32_t op = in[0] & 0xFF;
        if ((in[0] & ~0xFFu) || op >= FGPU_SOP_COUNT) { *why = "unknown opcode"; return false; }
        if (op == FGPU_SOP_KILL && !fragment) { *why = "discard in vertex stage"; return false; }
        if (op == FGPU_SOP_JMP || op == FGPU_SOP_JZ) {
            // Forward only, so every program terminates.
            if (in[1] <= pc || in[1] > count) { *why = "bad jump target"; return false; }
        } else if (op_has_dst(op)) {
            uint32_t d = in[1], file = FGPU_OPERAND_FILE(d), index = FGPU_OPERAND_INDEX(d);
            if ((d & ~0xFFFFu) || !FGPU_DST_MASK(d)) { *why = "bad destination"; return false; }
            bool ok = (file == FGPU_FILE_TEMP && index < h->temp_count) ||
                      (file == FGPU_FILE_OUTPUT && index < (fragment ? 1u : 1u + h->varying_count));
            if (!ok) { *why = "bad destination register"; return false; }
        }
        uint32_t used = op_sources(op);
        for (int k = 0; k < 3; k++) {
            if (!(used & (1u << k))) continue;
            if (!check_src(h, in[2 + k], fragment)) { *why = "bad source register"; return false; }
        }
        if (op == FGPU_SOP_TEX && in[3] >= FGPU_TEXTURE_UNITS) { *why = "bad texture unit"; return false; }
        if (op == FGPU_SOP_BUFFER_LOAD &&
            (in[3] >= FGPU_MAX_STORAGE_BUFFERS || !(h->storage_buffer_mask & (1u << in[3])))) {
            *why = "bad storage buffer binding";
            return false;
        }
    }
    return true;
}

bool fgpu_program_check(const void* blob, uint32_t bytes, fgpu_program_t* out, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    if (!blob || bytes < sizeof(fgpu_shader_header_t) || (bytes & 3) ||
        ((uintptr_t)blob & 3)) { *why = "too small"; return false; }
    const fgpu_shader_header_t* h = (const fgpu_shader_header_t*)blob;
    uint32_t words = bytes / 4;
    if (h->magic != FGPU_SHADER_MAGIC || h->version != FGPU_SHADER_VERSION) { *why = "not an FGSL program"; return false; }
    if (h->size != bytes) { *why = "size mismatch"; return false; }
    if (h->vs_count == 0 || h->fs_count == 0 ||
        h->vs_count > FGPU_SHADER_MAX_INSTRUCTIONS || h->fs_count > FGPU_SHADER_MAX_INSTRUCTIONS) {
        *why = "bad instruction count"; return false;
    }
    if (h->temp_count > FGPU_SHADER_MAX_TEMPS || h->varying_count > FGPU_SHADER_MAX_VARYINGS ||
        h->uniform_count > FGPU_SHADER_MAX_UNIFORMS || h->const_count > FGPU_SHADER_MAX_CONSTANTS ||
        (h->attrib_mask >> FGPU_MAX_ATTRIBS) || (h->texture_mask >> FGPU_TEXTURE_UNITS) ||
        (h->storage_buffer_mask >> FGPU_MAX_STORAGE_BUFFERS)) {
        *why = "limits exceeded"; return false;
    }
    uint32_t header_words = sizeof(fgpu_shader_header_t) / 4;
    // Each section must lie inside the blob (counts are small, no overflow).
    if (h->vs_offset < header_words || h->vs_offset > words ||
        h->vs_count * FGPU_SHADER_INSTRUCTION_WORDS > words - h->vs_offset ||
        h->fs_offset < header_words || h->fs_offset > words ||
        h->fs_count * FGPU_SHADER_INSTRUCTION_WORDS > words - h->fs_offset ||
        h->const_offset < header_words || h->const_offset > words ||
        h->const_count * 4 > words - h->const_offset) {
        *why = "section outside the program"; return false;
    }
    const uint32_t* base = (const uint32_t*)blob;
    if (!check_stage(h, base + h->vs_offset, h->vs_count, false, why)) return false;
    if (!check_stage(h, base + h->fs_offset, h->fs_count, true, why)) return false;
    if (out) {
        out->h = *h;
        out->vs = base + h->vs_offset;
        out->fs = base + h->fs_offset;
        out->consts = (const float (*)[4])(const void*)(base + h->const_offset);
        out->jit = NULL;
    }
    return true;
}

/* ----------------------------------------------------------- execution */

/*
 * The machine runs FGPU_LANES invocations (pixels or vertices) together,
 * like a GPU warp. Registers are stored as [register][component][lane], so
 * each instruction is decoded once and then applied with short loops over
 * the lanes, which the compiler turns into SSE code.
 *
 * Branches: every lane has a "resume at" instruction. A lane that jumps
 * waits until execution reaches its target; since jumps only go forward,
 * the lanes that did not jump run the skipped code first, then all lanes
 * continue together. A discarded lane stops for good.
 */

#define L FGPU_LANES
typedef float row_t[L];

typedef struct {
    fgpu_shader_batch_t* b;
    const float (*uniforms)[4];
    const float (*consts)[4];
    const fgpu_texture_t* textures;
    const fgpu_storage_buffer_t* storage_buffers;
    row_t ops[3][4];            // materialized sources
    row_t res[4];               // result
} vm_t;

static const float k_zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// Points rows[c] at source component c (for the lanes) for every c in
// `need`, copying into `tmp` when the value is the same for all lanes or
// has modifiers.
static inline __attribute__((always_inline))
void operand(vm_t* vm, uint32_t w, uint32_t need, row_t tmp[4], const float* rows[4]) {
    uint32_t file = FGPU_OPERAND_FILE(w), index = FGPU_OPERAND_INDEX(w);
    uint32_t sw = FGPU_OPERAND_SWIZZLE(w);
    bool mods = (w & (FGPU_SRC_ABS | FGPU_SRC_NEGATE)) != 0;
    const float (*lanes)[L] = NULL;             // per-lane register, [comp][lane]
    const float* uni = NULL;                    // same for every lane, [comp]
    switch (file) {
        case FGPU_FILE_TEMP:    lanes = (const float (*)[L])vm->b->temp[index]; break;
        case FGPU_FILE_INPUT:   lanes = (const float (*)[L])vm->b->input[index]; break;
        case FGPU_FILE_OUTPUT:  lanes = (const float (*)[L])vm->b->output[index]; break;
        case FGPU_FILE_SPECIAL: lanes = (const float (*)[L])vm->b->special[index]; break;
        case FGPU_FILE_UNIFORM: uni = vm->uniforms ? vm->uniforms[index] : k_zero; break;
        case FGPU_FILE_CONST:   uni = vm->consts[index]; break;
        default:                uni = k_zero; break;
    }
    for (int c = 0; c < 4; c++) {
        if (!(need & (1u << c))) continue;
        uint32_t k = (sw >> (2 * c)) & 3;
        if (lanes && !mods) { rows[c] = lanes[k]; continue; }
        float* t = tmp[c];
        if (lanes) {
            for (int l = 0; l < L; l++) t[l] = lanes[k][l];
        } else {
            float u = uni[k];
            for (int l = 0; l < L; l++) t[l] = u;
        }
        if (w & FGPU_SRC_ABS) for (int l = 0; l < L; l++) t[l] = m_abs(t[l]);
        if (w & FGPU_SRC_NEGATE) for (int l = 0; l < L; l++) t[l] = -t[l];
        rows[c] = t;
    }
}

static inline float smooth(float e0, float e1, float x) {
    float d = e1 - e0;
    float t = d != 0.0f ? clamp01((x - e0) / d) : (x < e0 ? 0.0f : 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* SSE math on 4 lanes at a time (GCC vector extensions). Comparisons give
 * all-ones / all-zeros masks; vsel picks per lane without branches. */

typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));

static inline v4f vld(const float* p) { v4f v; __builtin_memcpy(&v, p, 16); return v; }
static inline void vst(float* p, v4f v) { __builtin_memcpy(p, &v, 16); }
static inline v4f vsplat(float x) { return (v4f){ x, x, x, x }; }
static inline v4f vsel(v4i m, v4f a, v4f b) { return (v4f)((m & (v4i)a) | (~m & (v4i)b)); }
static inline v4f vabs(v4f x) { return (v4f)((v4i)x & 0x7FFFFFFF); }
static inline v4f vmin(v4f a, v4f b) { return vsel(a < b, a, b); }
static inline v4f vmax(v4f a, v4f b) { return vsel(a > b, a, b); }
static inline v4f vsqrt(v4f x) { return __builtin_ia32_sqrtps(vmax(x, vsplat(0.0f))); }
static inline v4f vbool(v4i m) { return vsel(m, vsplat(1.0f), vsplat(0.0f)); }

static inline v4f vfloor(v4f x) {
    v4i small = vabs(x) < vsplat(8388608.0f);
    v4f t = __builtin_convertvector(__builtin_convertvector(vsel(small, x, vsplat(0.0f)), v4i), v4f);
    t = vsel(t > x, t - 1.0f, t);
    return vsel(small, t, x);
}

static inline v4f vdiv(v4f a, v4f b) {
    v4i nz = b != vsplat(0.0f);
    return vsel(nz, a / vsel(nz, b, vsplat(1.0f)), vsplat(0.0f));
}

static inline v4f vexp2(v4f x) {
    v4i tiny = ~(x >= vsplat(-126.0f));                 // also NaN
    x = vmin(x, vsplat(127.0f));
    x = vsel(tiny, vsplat(0.0f), x);
    v4f fi = vfloor(x);
    v4f f = x - fi;
    v4f p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f +
            f * (0.0096181f + f * (0.0013333f + f * 0.0001540f)))));
    v4i e = (__builtin_convertvector(fi, v4i) + 127) << 23;
    return vsel(tiny, vsplat(0.0f), p * (v4f)e);
}

static inline v4f vlog2(v4f x) {
    v4i ok = x > vsplat(0.0f);
    v4i u = (v4i)vsel(ok, x, vsplat(1.0f));
    v4f e = __builtin_convertvector(((u >> 23) & 0xFF) - 127, v4f);
    v4f m = (v4f)((u & 0x007FFFFF) | 0x3F800000);
    v4i big = m > vsplat(1.41421356f);
    m = vsel(big, m * 0.5f, m);
    e = vsel(big, e + 1.0f, e);
    v4f t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    v4f s = t * (2.0f + t2 * (0.6666667f + t2 * (0.4f + t2 * (0.2857143f + t2 * 0.2222222f))));
    return vsel(ok, e + s * 1.44269504f, vsplat(-126.0f));
}

static inline v4f vpow(v4f a, v4f b) {
    v4f r = vexp2(b * vlog2(a));
    return vsel(a > vsplat(0.0f), r, vsel(b == vsplat(0.0f), vsplat(1.0f), vsplat(0.0f)));
}

static inline v4f vsin(v4f x) {
    x = vsel(vabs(x) < vsplat(1e7f), x, vsplat(0.0f));
    v4f k = vfloor(x * (1.0f / (2.0f * M_PI_F)) + 0.5f);
    x -= k * (2.0f * M_PI_F);
    x = vsel(x > vsplat(M_PI_F / 2), M_PI_F - x, x);
    x = vsel(x < vsplat(-M_PI_F / 2), -M_PI_F - x, x);
    v4f x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f *
           (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}

static inline v4f vsmooth(v4f e0, v4f e1, v4f x) {
    v4f d = e1 - e0;
    v4f t = vsel(d != vsplat(0.0f), vdiv(x - e0, d), vbool(x >= e0));
    t = vmin(vmax(t, vsplat(0.0f)), vsplat(1.0f));
    return t * t * (3.0f - 2.0f * t);
}

// SSE loops: 4 lanes per step, for the components in `need`.
#define VEACH(expr) \
    for (int c = 0; c < 4; c++) { \
        if (!(need & (1u << c))) continue; \
        for (int h = 0; h < L; h += 4) { \
            v4f A = vld(a[c] + h), B = vld(b[c] + h), C = vld(cc[c] + h); \
            (void)A; (void)B; (void)C; \
            vst(out[c] + h, (expr)); \
        } \
    }

// Component-wise loops over the lanes, for the components in `need`.
#define EACH(expr) \
    for (int c = 0; c < 4; c++) { \
        if (!(need & (1u << c))) continue; \
        const float* __restrict A = a[c]; const float* __restrict B = b[c]; \
        const float* __restrict C = cc[c]; float* __restrict R = out[c]; \
        (void)A; (void)B; (void)C; \
        for (int l = 0; l < L; l++) R[l] = (expr); \
    }
#define EACH_SLOW(expr) \
    for (int c = 0; c < 4; c++) { \
        if (!(need & (1u << c))) continue; \
        const float* __restrict A = a[c]; const float* __restrict B = b[c]; \
        const float* __restrict C = cc[c]; float* __restrict R = out[c]; \
        (void)A; (void)B; (void)C; \
        for (int l = 0; l < L; l++) if (active & (1u << l)) R[l] = (expr); \
    }

static inline void dot(vm_t* vm, const float* const a[4], const float* const b[4], int n) {
    for (int l = 0; l < L; l++) {
        float s = a[0][l] * b[0][l];
        for (int k = 1; k < n; k++) s += a[k][l] * b[k][l];
        vm->res[0][l] = s;
    }
    for (int c = 1; c < 4; c++)
        for (int l = 0; l < L; l++) vm->res[c][l] = vm->res[0][l];
}

static inline __attribute__((always_inline)) void store(vm_t* vm, uint32_t d, uint32_t active) {
    uint32_t index = FGPU_OPERAND_INDEX(d), mask = FGPU_DST_MASK(d);
    float (*r)[L] = FGPU_OPERAND_FILE(d) == FGPU_FILE_OUTPUT ? vm->b->output[index] : vm->b->temp[index];
    for (int c = 0; c < 4; c++) {
        if (!(mask & (1u << c))) continue;
        if (active == (1u << L) - 1) {
            for (int l = 0; l < L; l++) r[c][l] = vm->res[c][l];
        } else {
            for (int l = 0; l < L; l++) if (active & (1u << l)) r[c][l] = vm->res[c][l];
        }
    }
}

// Lanes in `live` that run at instruction `pc`.
static inline uint32_t running(const uint32_t* wait, uint32_t live, uint32_t pc) {
    uint32_t a = 0;
    for (int l = 0; l < L; l++) if ((live & (1u << l)) && wait[l] <= pc) a |= 1u << l;
    return a;
}

// The destination register as [comp][lane].
static inline float (*dst_reg(vm_t* vm, uint32_t d))[L] {
    uint32_t index = FGPU_OPERAND_INDEX(d);
    return FGPU_OPERAND_FILE(d) == FGPU_FILE_OUTPUT ? vm->b->output[index] : vm->b->temp[index];
}

// Does source word `w` read the destination register `d`?
static inline bool reads(uint32_t w, uint32_t d) {
    return (w & 0xFFFu) == (d & 0xFFFu);
}

static uint32_t run(vm_t* vm, const uint32_t* code, uint32_t count, uint32_t live) {
    uint32_t wait[L];
    for (int l = 0; l < L; l++) wait[l] = 0;
    uint32_t pc = 0, active = live;
    uint32_t wake = count;      // the next instruction a waiting lane resumes at
    const uint32_t all = (1u << L) - 1;
    while (pc < count && live) {
        if (!active) {
            // Everyone left is waiting: go to the nearest target.
            uint32_t next = count;
            for (int l = 0; l < L; l++) if ((live & (1u << l)) && wait[l] < next) next = wait[l];
            pc = next;
            if (pc >= count) break;
            active = running(wait, live, pc);
            wake = count;
            for (int l = 0; l < L; l++) if ((live & (1u << l)) && wait[l] > pc && wait[l] < wake) wake = wait[l];
            continue;
        }
        const uint32_t* in = code + (size_t)pc * FGPU_SHADER_INSTRUCTION_WORDS;
        uint32_t op = in[0];
        pc++;
        const float* a[4] = { k_zero, k_zero, k_zero, k_zero };
        const float* b[4] = { k_zero, k_zero, k_zero, k_zero };
        const float* cc[4] = { k_zero, k_zero, k_zero, k_zero };
        bool control = false;
        switch (op) {
            case FGPU_SOP_NOP:
                break;
            case FGPU_SOP_JMP:
                for (int l = 0; l < L; l++) if (active & (1u << l)) wait[l] = in[1];
                control = true;
                break;
            case FGPU_SOP_JZ:
                operand(vm, in[2], 1, vm->ops[0], a);
                for (int l = 0; l < L; l++)
                    if ((active & (1u << l)) && a[0][l] == 0.0f) wait[l] = in[1];
                control = true;
                break;
            case FGPU_SOP_KILL:
                operand(vm, in[2], 1, vm->ops[0], a);
                for (int l = 0; l < L; l++)
                    if ((active & (1u << l)) && a[0][l] != 0.0f) live &= ~(1u << l);
                control = true;
                break;
            case FGPU_SOP_TEX: {
                operand(vm, in[2], 3, vm->ops[0], a);
                const fgpu_texture_t* t = vm->textures ? &vm->textures[in[3]] : NULL;
                if (vm->b->quad && texture_uses_lod(t)) {
                    // Level of detail from the 2x2 quad: every lane's coordinate counts.
                    float px[4];
                    for (int l = 0; l < L; l++) {
                        if (!(active & (1u << l))) continue;
                        fgpu_texture_sample_lod(t, a[0][l], a[1][l], quad_lod(t, a[0], a[1], (uint32_t)l), px);
                        for (int c = 0; c < 4; c++) vm->res[c][l] = px[c];
                    }
                    store(vm, in[1], active);
                    break;
                }
                for (int l = 0; l < L; l++) {
                    if (!(active & (1u << l))) continue;
                    float px[4];
                    fgpu_texture_sample(t, a[0][l], a[1][l], px);
                    for (int c = 0; c < 4; c++) vm->res[c][l] = px[c];
                }
                store(vm, in[1], active);
                break;
            }
            case FGPU_SOP_BUFFER_LOAD: {
                operand(vm, in[2], 1, vm->ops[0], a);
                const fgpu_storage_buffer_t* buffer = vm->storage_buffers ? &vm->storage_buffers[in[3]] : NULL;
                for (int l = 0; l < L; l++) {
                    if (!(active & (1u << l))) continue;
                    float index = a[0][l];
                    for (int c = 0; c < 4; c++) vm->res[c][l] = 0.0f;
                    if (buffer && buffer->data && index >= 0.0f &&
                        index < (float)buffer->vec4_count && index < 16777216.0f) {
                        const float* value = buffer->data[(uint32_t)index];
                        for (int c = 0; c < 4; c++) vm->res[c][l] = value[c];
                    }
                }
                store(vm, in[1], active);
                break;
            }
            default: {
                uint32_t used = op_sources(op);
                uint32_t mask = FGPU_DST_MASK(in[1]);
                // Dot products and cross read other components than they write.
                bool across = op == FGPU_SOP_DP2 || op == FGPU_SOP_DP3 || op == FGPU_SOP_DP4 || op == FGPU_SOP_CROSS;
                uint32_t need = across ? 0xF : mask;
                operand(vm, in[2], need, vm->ops[0], a);
                if (used & 2) operand(vm, in[3], need, vm->ops[1], b);
                if (used & 4) operand(vm, in[4], need, vm->ops[2], cc);
                // Write straight into the destination when every lane runs
                // and no source reads it; otherwise compute, then copy.
                bool direct = active == all && !across && !reads(in[2], in[1]) &&
                              !((used & 2) && reads(in[3], in[1])) && !((used & 4) && reads(in[4], in[1]));
                float (*out)[L] = direct ? dst_reg(vm, in[1]) : vm->res;
                switch (op) {
                    case FGPU_SOP_MOV:   VEACH(A); break;
                    case FGPU_SOP_ADD:   VEACH(A + B); break;
                    case FGPU_SOP_SUB:   VEACH(A - B); break;
                    case FGPU_SOP_MUL:   VEACH(A * B); break;
                    case FGPU_SOP_DIV:   VEACH(vdiv(A, B)); break;
                    case FGPU_SOP_MAD:   VEACH(A * B + C); break;
                    case FGPU_SOP_MIN:   VEACH(vmin(A, B)); break;
                    case FGPU_SOP_MAX:   VEACH(vmax(A, B)); break;
                    case FGPU_SOP_DP2:   dot(vm, a, b, 2); break;
                    case FGPU_SOP_DP3:   dot(vm, a, b, 3); break;
                    case FGPU_SOP_DP4:   dot(vm, a, b, 4); break;
                    case FGPU_SOP_CROSS:
                        for (int l = 0; l < L; l++) {
                            vm->res[0][l] = a[1][l] * b[2][l] - a[2][l] * b[1][l];
                            vm->res[1][l] = a[2][l] * b[0][l] - a[0][l] * b[2][l];
                            vm->res[2][l] = a[0][l] * b[1][l] - a[1][l] * b[0][l];
                            vm->res[3][l] = 0.0f;
                        }
                        break;
                    case FGPU_SOP_FLOOR: VEACH(vfloor(A)); break;
                    case FGPU_SOP_FRACT: VEACH(A - vfloor(A)); break;
                    case FGPU_SOP_SQRT:  VEACH(vsqrt(A)); break;
                    case FGPU_SOP_RSQ:   VEACH(vsel(A > vsplat(0.0f), 1.0f / vsqrt(vsel(A > vsplat(0.0f), A, vsplat(1.0f))), vsplat(0.0f))); break;
                    case FGPU_SOP_EXP2:  VEACH(vexp2(A)); break;
                    case FGPU_SOP_LOG2:  VEACH(vlog2(A)); break;
                    case FGPU_SOP_POW:   VEACH(vpow(A, B)); break;
                    case FGPU_SOP_SIN:   VEACH(vsin(A)); break;
                    case FGPU_SOP_COS:   VEACH(vsin(A + M_PI_F / 2)); break;
                    case FGPU_SOP_MIX:   VEACH(A + (B - A) * C); break;
                    case FGPU_SOP_CLAMP: VEACH(vmin(vmax(A, B), C)); break;
                    case FGPU_SOP_STEP:  VEACH(vsel(B < A, vsplat(0.0f), vsplat(1.0f))); break;
                    case FGPU_SOP_SSTEP: VEACH(vsmooth(A, B, C)); break;
                    case FGPU_SOP_SLT:   VEACH(vbool(A < B)); break;
                    case FGPU_SOP_SLE:   VEACH(vbool(A <= B)); break;
                    case FGPU_SOP_SEQ:   VEACH(vbool(A == B)); break;
                    case FGPU_SOP_SNE:   VEACH(vbool(A != B)); break;
                    case FGPU_SOP_SEL:   VEACH(vsel(A != vsplat(0.0f), B, C)); break;
                    case FGPU_SOP_MOD:   VEACH(vsel(B != vsplat(0.0f), A - B * vfloor(vdiv(A, B)), vsplat(0.0f))); break;
                    case FGPU_SOP_SIGN:  VEACH(vsel(A > vsplat(0.0f), vsplat(1.0f), vsel(A < vsplat(0.0f), vsplat(-1.0f), vsplat(0.0f)))); break;
                    case FGPU_SOP_ATAN2: EACH_SLOW(m_atan2(A[l], B[l])); break;
                    default:             VEACH(vsplat(0.0f)); break;
                }
                if (!direct) store(vm, in[1], active);
                break;
            }
        }
        // Lanes that jumped wait; lanes whose target is here join again.
        if (control || pc >= wake) {
            active = running(wait, live, pc);
            wake = count;
            for (int l = 0; l < L; l++) if ((live & (1u << l)) && wait[l] > pc && wait[l] < wake) wake = wait[l];
        }
    }
    return live;
}

#undef EACH
#undef EACH_SLOW

void fgpu_shader_batch_reset(fgpu_shader_batch_t* b, uint32_t temps) {
    // Registers start at zero: a shader must never see old data.
    if (temps > FGPU_SHADER_MAX_TEMPS) temps = FGPU_SHADER_MAX_TEMPS;
    float* t = &b->temp[0][0][0];
    for (size_t i = 0; i < (size_t)temps * 4 * L; i++) t[i] = 0.0f;
}

uint32_t fgpu_shader_run_batch_buffers(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                               uint32_t lanes, const float (*uniforms)[4],
                               const fgpu_texture_t* textures,
                               const fgpu_storage_buffer_t* storage_buffers) {
#ifdef FGPU_ENABLE_JIT
    // Compiled code (bit-identical results), if the program has any.
    if (p->jit && !p->h.storage_buffer_mask) {
        uint32_t kept;
        if (fgpu_jit_run_batch(p, fragment, b, lanes, uniforms, textures, &kept)) return kept;
    }
#endif
    vm_t vm;
    vm.b = b;
    vm.uniforms = uniforms;
    vm.consts = p->consts;
    vm.textures = textures;
    vm.storage_buffers = storage_buffers;
    uint32_t outs = fragment ? 1u : 1u + p->h.varying_count;
    for (uint32_t o = 0; o < outs; o++) {
        for (int c = 0; c < 4; c++) {
            float v = (c == 3 && (fragment || o == 0)) ? 1.0f : 0.0f;
            for (int l = 0; l < L; l++) b->output[o][c][l] = v;
        }
    }
    lanes &= (1u << L) - 1;
    return fragment ? run(&vm, p->fs, p->h.fs_count, lanes) : run(&vm, p->vs, p->h.vs_count, lanes);
}

uint32_t fgpu_shader_run_batch(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                               uint32_t lanes, const float (*uniforms)[4], const fgpu_texture_t* textures) {
    return fgpu_shader_run_batch_buffers(p, fragment, b, lanes, uniforms, textures, NULL);
}

/* One invocation at a time (tests, wireframe lines). */

static fgpu_shader_batch_t g_single;

void fgpu_shader_run_vertex(const fgpu_shader_io_t* io) {
    const fgpu_program_t* p = io->program;
    fgpu_shader_batch_reset(&g_single, p->h.temp_count);
    for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++)
        for (int c = 0; c < 4; c++) g_single.input[i][c][0] = io->input[i][c];
    fgpu_shader_run_batch(p, false, &g_single, 1, io->uniforms, io->textures);
    for (uint32_t o = 0; o < 1 + p->h.varying_count; o++)
        for (int c = 0; c < 4; c++) io->output[o][c] = g_single.output[o][c][0];
}

bool fgpu_shader_run_fragment(const fgpu_shader_io_t* io) {
    const fgpu_program_t* p = io->program;
    fgpu_shader_batch_reset(&g_single, p->h.temp_count);
    for (uint32_t i = 0; i < p->h.varying_count; i++)
        for (int c = 0; c < 4; c++) g_single.input[i][c][0] = io->input[i][c];
    for (int i = 0; i < 2; i++)
        for (int c = 0; c < 4; c++) g_single.special[i][c][0] = io->special ? io->special[i][c] : 0.0f;
    uint32_t kept = fgpu_shader_run_batch(p, true, &g_single, 1, io->uniforms, io->textures);
    for (int c = 0; c < 4; c++) io->output[0][c] = g_single.output[0][c][0];
    return kept != 0;
}
