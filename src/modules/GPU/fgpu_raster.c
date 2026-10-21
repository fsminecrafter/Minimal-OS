#pragma GCC optimize("O2")
#include "x86_64/fgpu_raster.h"
#include <stddef.h>

#define NEAR_EPS 1e-5f
#define FIXED_VARS 10           // built-in programs: world[3], normal[3], color[4]
#define VAR_MAX (FGPU_SHADER_MAX_VARYINGS * 4)

// Shader invocations run FGPU_LANES at a time. One draw runs at a time
// (the kernel serializes SYS_GPU), so these can be shared.
static fgpu_shader_batch_t g_vs_batch, g_fs_batch;

/* ---------------------------------------------------------------- math */

static inline float f_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline float f_clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline float f_floor(float x) {
    int i = (int)x;
    return (float)(x < (float)i ? i - 1 : i);
}

static inline float f_ceil(float x) {
    int i = (int)x;
    return (float)(x > (float)i ? i + 1 : i);
}

// x^n for integer n >= 0 (shininess), by squaring.
static float f_powi(float x, int n) {
    float r = 1.0f;
    while (n > 0) {
        if (n & 1) r *= x;
        x *= x;
        n >>= 1;
    }
    return r;
}

static inline void normalize3(float v[3]) {
    float len = f_sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-20f) {
        float inv = 1.0f / len;
        v[0] *= inv; v[1] *= inv; v[2] *= inv;
    }
}

static inline void mat4_mul_vec(const float m[16], const float v[4], float out[4]) {
    for (int r = 0; r < 4; r++) {
        out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] +
                 m[2 * 4 + r] * v[2] + m[3 * 4 + r] * v[3];
    }
}

/* -------------------------------------------------------------- layout */

void fgpu_layout_default(fgpu_layout_t* l) {
    l->stride = 10;
    l->count = 3;
    l->attrib[0] = FGPU_ATTRIB(0, 3);
    l->attrib[1] = FGPU_ATTRIB(3, 3);
    l->attrib[2] = FGPU_ATTRIB(6, 4);
    for (uint32_t i = 3; i < FGPU_MAX_ATTRIBS; i++) l->attrib[i] = 0;
}

bool fgpu_layout_valid(const fgpu_layout_t* l) {
    if (!l || l->stride == 0 || l->stride > FGPU_MAX_VERTEX_STRIDE || l->count > FGPU_MAX_ATTRIBS)
        return false;
    for (uint32_t i = 0; i < l->count; i++) {
        uint32_t off = l->attrib[i] & 0xFFFF, n = l->attrib[i] >> 16;
        if (n > 4 || (n && off + n > l->stride)) return false;
    }
    return true;
}

// Reads a vertex's attributes; missing components are 0, 0, 0, 1.
static void fetch_vertex(const fgpu_layout_t* l, const float* v, float attr[FGPU_MAX_ATTRIBS][4]) {
    for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++) {
        attr[i][0] = attr[i][1] = attr[i][2] = 0.0f;
        attr[i][3] = 1.0f;
        if (i >= l->count) continue;
        uint32_t off = l->attrib[i] & 0xFFFF, n = l->attrib[i] >> 16;
        for (uint32_t k = 0; k < n; k++) attr[i][k] = v[off + k];
    }
}

/* ------------------------------------------------------------- varyings */

// Per-vertex data after the vertex stage.
typedef struct {
    float clip[4];
    float var[VAR_MAX];
} vtx_t;

// Screen-space vertex for the rasterizer.
typedef struct {
    float x, y, z, inv_w;
    float var[VAR_MAX];         // pre-multiplied by inv_w
} svtx_t;

static void light(const fgpu_uniforms_t* u, const float n_in[3], const float pos[3],
                  const float base[4], float out[4]) {
    float n[3] = { n_in[0], n_in[1], n_in[2] };
    normalize3(n);
    float l[3] = { -u->light_dir[0], -u->light_dir[1], -u->light_dir[2] };
    normalize3(l);
    float diff = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
    if (diff < 0.0f) diff = 0.0f;
    float spec = 0.0f;
    float strength = u->material[0];
    if (strength > 0.0f && diff > 0.0f) {
        float v[3] = { u->camera_pos[0] - pos[0], u->camera_pos[1] - pos[1],
                       u->camera_pos[2] - pos[2] };
        normalize3(v);
        float h[3] = { l[0] + v[0], l[1] + v[1], l[2] + v[2] };
        normalize3(h);
        float nh = n[0] * h[0] + n[1] * h[1] + n[2] * h[2];
        int shin = (int)u->material[1];
        if (shin < 1) shin = 1;
        if (shin > 128) shin = 128;
        if (nh > 0.0f) spec = f_powi(nh, shin) * strength;
    }
    for (int i = 0; i < 3; i++) {
        out[i] = base[i] * (u->ambient[i] + u->light_color[i] * diff) +
                 u->light_color[i] * spec;
    }
    out[3] = base[3];
}

static void vertex_stage(const fgpu_pipeline_t* p, const fgpu_layout_t* layout,
                         const float* in, vtx_t* out) {
    float attr[FGPU_MAX_ATTRIBS][4];
    fetch_vertex(layout, in, attr);

    float pos[4] = { attr[0][0], attr[0][1], attr[0][2], 1.0f };
    mat4_mul_vec(p->u.mvp, pos, out->clip);
    float world[4];
    mat4_mul_vec(p->u.model, pos, world);
    float n4[4] = { attr[1][0], attr[1][1], attr[1][2], 0.0f };
    float wn[4];
    mat4_mul_vec(p->u.normal, n4, wn);

    out->var[0] = world[0]; out->var[1] = world[1]; out->var[2] = world[2];
    out->var[3] = wn[0]; out->var[4] = wn[1]; out->var[5] = wn[2];
    const float* base = attr[2];
    if (p->program == FGPU_PROGRAM_GOURAUD) {
        light(&p->u, wn, world, base, out->var + 6);
    } else {
        out->var[6] = base[0]; out->var[7] = base[1];
        out->var[8] = base[2]; out->var[9] = base[3];
    }
}

/* ------------------------------------------------------------ fragments */

static inline uint32_t pack_color(const float c[4]) {
    uint32_t r = (uint32_t)(f_clamp01(c[0]) * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(f_clamp01(c[1]) * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(f_clamp01(c[2]) * 255.0f + 0.5f);
    // Opaque: window compositors read the top byte as alpha.
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// src over dst with src alpha.
static inline uint32_t blend(const float c[4], uint32_t dst) {
    float a = f_clamp01(c[3]);
    const float k = 1.0f / 255.0f;
    float d[3] = { (float)((dst >> 16) & 255u) * k, (float)((dst >> 8) & 255u) * k,
                   (float)(dst & 255u) * k };
    float o[4];
    for (int i = 0; i < 3; i++) o[i] = f_clamp01(c[i]) * a + d[i] * (1.0f - a);
    o[3] = 1.0f;
    return pack_color(o);
}

static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Top-left rule: pixels exactly on an edge belong to top or left edges.
static inline bool edge_owns(float dx, float dy) {
    return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
}

// Runs the fragment stage for interpolated (already divided) varyings.
// Returns false when the fragment is discarded.
// Runs the vertex shader for up to FGPU_LANES vertices at once.
static void shade_vertices(const fgpu_pipeline_t* p, const fgpu_layout_t* layout,
                           const float* const* in, uint32_t n, vtx_t* out) {
    fgpu_shader_batch_t* b = &g_vs_batch;
    for (uint32_t l = 0; l < n; l++) {
        float attr[FGPU_MAX_ATTRIBS][4];
        fetch_vertex(layout, in[l], attr);
        for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++)
            for (int c = 0; c < 4; c++) b->input[i][c][l] = attr[i][c];
    }
    fgpu_shader_run_batch(p->shader, false, b, (1u << n) - 1, p->constants, p->textures);
    uint32_t nv = p->shader->h.varying_count;
    for (uint32_t l = 0; l < n; l++) {
        for (int c = 0; c < 4; c++) out[l].clip[c] = b->output[0][c][l];
        for (uint32_t v = 0; v < nv; v++)
            for (int c = 0; c < 4; c++) out[l].var[v * 4 + c] = b->output[1 + v][c][l];
    }
}

static bool shade(const fgpu_pipeline_t* p, const float* var, const float special[2][4],
                  float out[4]) {
    if (p->shader) {
        float in[FGPU_SHADER_MAX_VARYINGS][4];
        uint32_t n = p->shader->h.varying_count;
        for (uint32_t v = 0; v < n; v++)
            for (int i = 0; i < 4; i++) in[v][i] = var[v * 4 + i];
        float o[1][4];
        fgpu_shader_io_t io = { p->shader, (const float (*)[4])in, o, p->constants, special,
                                p->textures };
        if (!fgpu_shader_run_fragment(&io)) return false;
        for (int i = 0; i < 4; i++) out[i] = o[0][i];
        return true;
    }
    if (p->program == FGPU_PROGRAM_PHONG) {
        light(&p->u, var + 3, var, var + 6, out);
    } else {
        out[0] = var[6]; out[1] = var[7]; out[2] = var[8]; out[3] = var[9];
    }
    return true;
}

// Pixels waiting for the fragment shader.
typedef struct {
    uint32_t n;
    int32_t x[FGPU_LANES], y[FGPU_LANES];
    float z[FGPU_LANES];
} frag_batch_t;

static void flush_fragments(const fgpu_target_t* t, const fgpu_pipeline_t* p, frag_batch_t* fb,
                            bool late_depth, bool depth_write, bool blending) {
    if (!fb->n) return;
    fgpu_shader_batch_t* b = &g_fs_batch;
    uint32_t kept = fgpu_shader_run_batch(p->shader, true, b, (1u << fb->n) - 1, p->constants,
                                          p->textures);
    for (uint32_t l = 0; l < fb->n; l++) {
        if (!(kept & (1u << l))) continue;
        int32_t x = fb->x[l], y = fb->y[l];
        if (late_depth && depth_write) t->depth[(size_t)y * t->depth_pitch + x] = fb->z[l];
        if (!t->color) continue;
        float out[4] = { b->output[0][0][l], b->output[0][1][l], b->output[0][2][l], b->output[0][3][l] };
        uint32_t* px = &t->color[(size_t)y * t->color_pitch + x];
        *px = blending ? blend(out, *px) : pack_color(out);
    }
    fb->n = 0;
}

static void raster_triangle(const fgpu_target_t* t, const fgpu_pipeline_t* p, uint32_t nvar,
                            const svtx_t* a, const svtx_t* b, const svtx_t* c) {
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (area == 0.0f) return;
    // Screen y points down, so counter-clockwise in GL terms has area < 0.
    bool front = area < 0.0f;
    if ((p->state & FGPU_STATE_CULL_BACK) && !front) return;
    if ((p->state & FGPU_STATE_CULL_FRONT) && front) return;
    if (area < 0.0f) {          // make it positive: swap b and c
        const svtx_t* tmp = b; b = c; c = tmp;
        area = -area;
    }

    float minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    float maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    float miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    float maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);

    int32_t clip_x0 = p->vp_x < 0 ? 0 : p->vp_x;
    int32_t clip_y0 = p->vp_y < 0 ? 0 : p->vp_y;
    int32_t clip_x1 = p->vp_x + p->vp_w;
    int32_t clip_y1 = p->vp_y + p->vp_h;
    if (clip_x1 > (int32_t)t->width) clip_x1 = (int32_t)t->width;
    if (clip_y1 > (int32_t)t->height) clip_y1 = (int32_t)t->height;

    float fx0 = f_floor(minx), fx1 = f_ceil(maxx), fy0 = f_floor(miny), fy1 = f_ceil(maxy);
    if (fx1 < (float)clip_x0 || fy1 < (float)clip_y0 ||
        fx0 > (float)clip_x1 || fy0 > (float)clip_y1) return;
    int32_t x0 = fx0 < (float)clip_x0 ? clip_x0 : (int32_t)fx0;
    int32_t y0 = fy0 < (float)clip_y0 ? clip_y0 : (int32_t)fy0;
    int32_t x1 = fx1 > (float)clip_x1 ? clip_x1 : (int32_t)fx1;
    int32_t y1 = fy1 > (float)clip_y1 ? clip_y1 : (int32_t)fy1;
    if (x0 >= x1 || y0 >= y1) return;

    bool own0 = edge_owns(c->x - b->x, c->y - b->y);
    bool own1 = edge_owns(a->x - c->x, a->y - c->y);
    bool own2 = edge_owns(b->x - a->x, b->y - a->y);

    float inv_area = 1.0f / area;
    bool depth_test = t->depth && (p->state & FGPU_STATE_DEPTH_TEST);
    bool depth_write = t->depth && (p->state & FGPU_STATE_DEPTH_WRITE);
    bool blending = (p->state & FGPU_STATE_BLEND) != 0;
    // A shader may discard: write depth only for fragments it keeps.
    bool late_depth = p->shader && (p->shader->h.flags & FGPU_SHADER_FLAG_DISCARD);

    float special[2][4];
    special[1][0] = special[1][1] = special[1][2] = special[1][3] = front ? 1.0f : 0.0f;
    frag_batch_t fb;
    fb.n = 0;
    fgpu_shader_batch_t* sb = &g_fs_batch;
    if (p->shader) {
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < FGPU_LANES; l++) sb->special[1][c][l] = special[1][0];
    }

    // Edge function steps per pixel in x and y.
    float e0dx = -(c->y - b->y), e0dy = (c->x - b->x);
    float e1dx = -(a->y - c->y), e1dy = (a->x - c->x);
    float e2dx = -(b->y - a->y), e2dy = (b->x - a->x);
    float px0 = (float)x0 + 0.5f, py0 = (float)y0 + 0.5f;
    float row0 = edge(b->x, b->y, c->x, c->y, px0, py0);
    float row1 = edge(c->x, c->y, a->x, a->y, px0, py0);
    float row2 = edge(a->x, a->y, b->x, b->y, px0, py0);

    for (int32_t y = y0; y < y1; y++) {
        float w0 = row0, w1 = row1, w2 = row2;
        uint32_t* crow = t->color ? t->color + (size_t)y * t->color_pitch : NULL;
        float* drow = t->depth ? t->depth + (size_t)y * t->depth_pitch : NULL;
        for (int32_t x = x0; x < x1; x++, w0 += e0dx, w1 += e1dx, w2 += e2dx) {
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            if ((w0 == 0.0f && !own0) || (w1 == 0.0f && !own1) || (w2 == 0.0f && !own2)) continue;
            float l0 = w0 * inv_area, l1 = w1 * inv_area, l2 = w2 * inv_area;
            float z = l0 * a->z + l1 * b->z + l2 * c->z;
            if (depth_test && z >= drow[x]) continue;
            if (depth_write && !late_depth) drow[x] = z;
            if (!crow && !late_depth) continue;

            float iw = l0 * a->inv_w + l1 * b->inv_w + l2 * c->inv_w;
            float wpix = iw != 0.0f ? 1.0f / iw : 0.0f;
            if (p->shader) {
                // Queue the pixel; shade FGPU_LANES of them at once.
                uint32_t lane = fb.n++;
                for (uint32_t k = 0; k < nvar; k++)
                    sb->input[k >> 2][k & 3][lane] = (l0 * a->var[k] + l1 * b->var[k] + l2 * c->var[k]) * wpix;
                sb->special[0][0][lane] = (float)x + 0.5f;
                sb->special[0][1][lane] = (float)y + 0.5f;
                sb->special[0][2][lane] = z;
                sb->special[0][3][lane] = iw;
                fb.x[lane] = x;
                fb.y[lane] = y;
                fb.z[lane] = z;
                if (fb.n == FGPU_LANES) flush_fragments(t, p, &fb, late_depth, depth_write, blending);
                continue;
            }
            float v[VAR_MAX];
            for (uint32_t k = 0; k < nvar; k++) {
                v[k] = (l0 * a->var[k] + l1 * b->var[k] + l2 * c->var[k]) * wpix;
            }
            special[0][0] = (float)x + 0.5f;
            special[0][1] = (float)y + 0.5f;
            special[0][2] = z;
            special[0][3] = iw;
            float out[4];
            if (!shade(p, v, (const float (*)[4])special, out)) continue;
            if (depth_write && late_depth) drow[x] = z;
            if (!crow) continue;
            crow[x] = blending ? blend(out, crow[x]) : pack_color(out);
        }
        row0 += e0dy; row1 += e1dy; row2 += e2dy;
    }
    flush_fragments(t, p, &fb, late_depth, depth_write, blending);
}

static void plot_line(const fgpu_target_t* t, const fgpu_pipeline_t* p, uint32_t nvar,
                      const svtx_t* a, const svtx_t* b) {
    if (!t->color) return;
    // One color per line: the fragment stage at the first end point.
    float v[VAR_MAX];
    float iw = a->inv_w != 0.0f ? 1.0f / a->inv_w : 0.0f;
    for (uint32_t k = 0; k < nvar; k++) v[k] = a->var[k] * iw;
    float special[2][4] = { { a->x, a->y, a->z, a->inv_w }, { 1, 1, 1, 1 } };
    float color[4];
    if (!shade(p, v, (const float (*)[4])special, color)) return;
    bool blending = (p->state & FGPU_STATE_BLEND) != 0;
    uint32_t packed = pack_color(color);
    int32_t x0 = (int32_t)a->x, y0 = (int32_t)a->y, x1 = (int32_t)b->x, y1 = (int32_t)b->y;
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < 16384; guard++) {
        if (x0 >= p->vp_x && y0 >= p->vp_y && x0 < p->vp_x + p->vp_w && y0 < p->vp_y + p->vp_h &&
            x0 >= 0 && y0 >= 0 && x0 < (int32_t)t->width && y0 < (int32_t)t->height) {
            uint32_t* px = &t->color[(size_t)y0 * t->color_pitch + x0];
            *px = blending ? blend(color, *px) : packed;
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ------------------------------------------------------------- clipping */

static void lerp_vtx(const vtx_t* a, const vtx_t* b, float t, uint32_t nvar, vtx_t* out) {
    for (int i = 0; i < 4; i++) out->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
    for (uint32_t i = 0; i < nvar; i++) out->var[i] = a->var[i] + (b->var[i] - a->var[i]) * t;
}

static void copy_vtx(const vtx_t* a, uint32_t nvar, vtx_t* out) {
    for (int i = 0; i < 4; i++) out->clip[i] = a->clip[i];
    for (uint32_t i = 0; i < nvar; i++) out->var[i] = a->var[i];
}

// Keeps the part with z >= -w (in front of the near plane) and w > eps.
static int clip_near(const vtx_t* in, int n, uint32_t nvar, vtx_t* out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const vtx_t* a = &in[i];
        const vtx_t* b = &in[(i + 1) % n];
        float da = a->clip[2] + a->clip[3];
        float db = b->clip[2] + b->clip[3];
        bool ina = da >= 0.0f && a->clip[3] > NEAR_EPS;
        bool inb = db >= 0.0f && b->clip[3] > NEAR_EPS;
        if (ina) copy_vtx(a, nvar, &out[m++]);
        if (ina != inb) {
            float t = da / (da - db);
            if (!(t >= 0.0f)) t = 0.0f;     // also catches NaN
            if (t > 1.0f) t = 1.0f;
            lerp_vtx(a, b, t, nvar, &out[m]);
            if (!(out[m].clip[3] > NEAR_EPS)) out[m].clip[3] = NEAR_EPS * 2.0f;
            m++;
        }
    }
    return m;
}

static void to_screen(const fgpu_pipeline_t* p, const vtx_t* v, uint32_t nvar, svtx_t* s) {
    float inv_w = 1.0f / v->clip[3];
    float nx = v->clip[0] * inv_w, ny = v->clip[1] * inv_w, nz = v->clip[2] * inv_w;
    // Keep projected points finite for the rasterizer.
    if (!(nx < 1e6f)) nx = 1e6f;
    if (!(nx > -1e6f)) nx = -1e6f;
    if (!(ny < 1e6f)) ny = 1e6f;
    if (!(ny > -1e6f)) ny = -1e6f;
    s->x = (float)p->vp_x + (nx * 0.5f + 0.5f) * (float)p->vp_w;
    s->y = (float)p->vp_y + (1.0f - (ny * 0.5f + 0.5f)) * (float)p->vp_h;
    s->z = nz * 0.5f + 0.5f;
    s->inv_w = inv_w;
    for (uint32_t i = 0; i < nvar; i++) s->var[i] = v->var[i] * inv_w;
}

/* ---------------------------------------------------------------- entry */

void fgpu_raster_clear(const fgpu_target_t* t, uint32_t flags, uint32_t color, float depth) {
    if ((flags & FGPU_CLEAR_COLOR) && t->color) {
        for (uint32_t y = 0; y < t->height; y++) {
            uint32_t* row = t->color + (size_t)y * t->color_pitch;
            uint32_t opaque = color | 0xFF000000u;
            for (uint32_t x = 0; x < t->width; x++) row[x] = opaque;
        }
    }
    if ((flags & FGPU_CLEAR_DEPTH) && t->depth) {
        for (uint32_t y = 0; y < t->height; y++) {
            float* row = t->depth + (size_t)y * t->depth_pitch;
            for (uint32_t x = 0; x < t->width; x++) row[x] = depth;
        }
    }
}

uint32_t fgpu_raster_draw_layout(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                                 const fgpu_layout_t* layout,
                                 const float* vertices, uint32_t vertex_count,
                                 const uint32_t* indices, uint32_t count) {
    if (!t || !p || !vertices || p->vp_w <= 0 || p->vp_h <= 0 || !fgpu_layout_valid(layout))
        return 0;
    if (p->shader && !p->constants) return 0;
    uint32_t nvar = p->shader ? p->shader->h.varying_count * 4 : FIXED_VARS;
    if (p->shader) {
        fgpu_shader_batch_reset(&g_vs_batch, p->shader->h.temp_count);
        fgpu_shader_batch_reset(&g_fs_batch, p->shader->h.temp_count);
    }
    uint32_t drawn = 0;
    for (uint32_t i = 0; i + 2 < count; i += 3) {
        vtx_t tri[3];
        const float* src[3];
        bool bad = false;
        for (int k = 0; k < 3; k++) {
            uint32_t index = indices ? indices[i + (uint32_t)k] : i + (uint32_t)k;
            if (index >= vertex_count) { bad = true; break; }
            src[k] = vertices + (size_t)index * layout->stride;
        }
        if (bad) continue;
        if (p->shader) shade_vertices(p, layout, src, 3, tri);
        else for (int k = 0; k < 3; k++) vertex_stage(p, layout, src[k], &tri[k]);

        vtx_t poly[4];
        int n = clip_near(tri, 3, nvar, poly);
        if (n < 3) continue;
        svtx_t s[4];
        for (int k = 0; k < n; k++) to_screen(p, &poly[k], nvar, &s[k]);

        if (p->state & FGPU_STATE_WIREFRAME) {
            for (int k = 0; k < n; k++) plot_line(t, p, nvar, &s[k], &s[(k + 1) % n]);
        } else {
            for (int k = 1; k + 1 < n; k++) raster_triangle(t, p, nvar, &s[0], &s[k], &s[k + 1]);
        }
        drawn++;
    }
    return drawn;
}

uint32_t fgpu_raster_draw(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                          const float* vertices, uint32_t vertex_count,
                          const uint32_t* indices, uint32_t count) {
    fgpu_layout_t layout;
    fgpu_layout_default(&layout);
    return fgpu_raster_draw_layout(t, p, &layout, vertices, vertex_count, indices, count);
}
