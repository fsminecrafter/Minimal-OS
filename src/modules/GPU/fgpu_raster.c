#pragma GCC optimize("O2")
#include "x86_64/fgpu_raster.h"
#include <stddef.h>
#if __STDC_HOSTED__
#include <stdlib.h>
#endif

#define NEAR_EPS 1e-5f
#define FIXED_VARS 10           // built-in programs: world[3], normal[3], color[4]
#define VAR_MAX (FGPU_SHADER_MAX_VARYINGS * 4)

// Triangles per round: shaded and clipped first, then rasterized.
#define CHUNK_TRIS 256
#define GEOMETRY_PART 16        // triangles per geometry part
#define MIN_BAND_ROWS 8

// Shader invocations run FGPU_LANES at a time, with one pair of batches
// per worker. One draw runs at a time (the kernel serializes SYS_GPU).
static fgpu_shader_batch_t g_vs_batch[FGPU_RASTER_MAX_WORKERS], g_fs_batch[FGPU_RASTER_MAX_WORKERS];
static uint32_t g_batch_epoch[FGPU_RASTER_MAX_WORKERS];
static uint32_t g_epoch = 1;            // bumped per draw: batches are reset on first use
static fgpu_parallel_t g_par;

void fgpu_raster_set_parallel(const fgpu_parallel_t* par) {
    if (par) g_par = *par;
    else { g_par.workers = NULL; g_par.run = NULL; }
}

static uint32_t worker_count(void) {
    if (!g_par.run || !g_par.workers) return 1;
    uint32_t n = g_par.workers();
    if (n < 1) n = 1;
    return n < FGPU_RASTER_MAX_WORKERS ? n : FGPU_RASTER_MAX_WORKERS;
}

static void run_parts(fgpu_part_fn fn, void* ctx, uint32_t parts) {
    if (parts > 1 && g_par.run && worker_count() > 1) g_par.run(fn, ctx, parts);
    else for (uint32_t i = 0; i < parts; i++) fn(ctx, i, 0);
}

// The worker's shader batch, with its registers cleared for this draw.
static fgpu_shader_batch_t* batch_for(const fgpu_pipeline_t* p, uint32_t worker, bool fragment) {
    if (g_batch_epoch[worker] != g_epoch) {
        g_batch_epoch[worker] = g_epoch;
        fgpu_shader_batch_reset(&g_vs_batch[worker], p->shader->h.temp_count);
        fgpu_shader_batch_reset(&g_fs_batch[worker], p->shader->h.temp_count);
    }
    return fragment ? &g_fs_batch[worker] : &g_vs_batch[worker];
}

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
static void shade_vertices(const fgpu_pipeline_t* p, const fgpu_layout_t* layout, uint32_t worker,
                           const float* const* in, uint32_t n, vtx_t* out) {
    fgpu_shader_batch_t* b = batch_for(p, worker, false);
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

static bool shade(const fgpu_pipeline_t* p, uint32_t worker, const float* var,
                  const float special[2][4], float out[4]) {
    if (p->shader) {
        // One lane of the worker's batch.
        fgpu_shader_batch_t* b = batch_for(p, worker, true);
        uint32_t n = p->shader->h.varying_count;
        for (uint32_t v = 0; v < n; v++)
            for (int i = 0; i < 4; i++) b->input[v][i][0] = var[v * 4 + i];
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < 4; i++) b->special[k][i][0] = special[k][i];
        if (!(fgpu_shader_run_batch(p->shader, true, b, 1, p->constants, p->textures) & 1))
            return false;
        for (int i = 0; i < 4; i++) out[i] = b->output[0][i][0];
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

static void flush_fragments(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                            fgpu_shader_batch_t* b, frag_batch_t* fb,
                            bool late_depth, bool depth_write, bool blending) {
    if (!fb->n) return;
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

// Draws the rows band_y0 <= y < band_y1 of a triangle.
static void raster_triangle(const fgpu_target_t* t, const fgpu_pipeline_t* p, uint32_t nvar,
                            uint32_t worker, int32_t band_y0, int32_t band_y1,
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
    if (clip_y0 < band_y0) clip_y0 = band_y0;
    if (clip_y1 > band_y1) clip_y1 = band_y1;

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
    fgpu_shader_batch_t* sb = p->shader ? batch_for(p, worker, true) : NULL;
    if (p->shader) {
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < FGPU_LANES; l++) sb->special[1][c][l] = special[1][0];
    }

    // Edge function steps per pixel in x.
    float e0dx = -(c->y - b->y);
    float e1dx = -(a->y - c->y);
    float e2dx = -(b->y - a->y);
    float px0 = (float)x0 + 0.5f;

    for (int32_t y = y0; y < y1; y++) {
        // Evaluated per row, not summed up from the top, so a pixel comes
        // out the same however the rows are split between workers.
        float py = (float)y + 0.5f;
        float w0 = edge(b->x, b->y, c->x, c->y, px0, py);
        float w1 = edge(c->x, c->y, a->x, a->y, px0, py);
        float w2 = edge(a->x, a->y, b->x, b->y, px0, py);
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
                if (fb.n == FGPU_LANES) flush_fragments(t, p, sb, &fb, late_depth, depth_write, blending);
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
            if (!shade(p, worker, v, (const float (*)[4])special, out)) continue;
            if (depth_write && late_depth) drow[x] = z;
            if (!crow) continue;
            crow[x] = blending ? blend(out, crow[x]) : pack_color(out);
        }
    }
    flush_fragments(t, p, sb, &fb, late_depth, depth_write, blending);
}

// Plots the pixels of a line in rows band_y0 <= y < band_y1.
static void plot_line(const fgpu_target_t* t, const fgpu_pipeline_t* p, uint32_t nvar,
                      uint32_t worker, int32_t band_y0, int32_t band_y1,
                      const svtx_t* a, const svtx_t* b) {
    if (!t->color) return;
    int32_t ya = (int32_t)a->y, yb = (int32_t)b->y;
    if ((ya < band_y0 && yb < band_y0) || (ya >= band_y1 && yb >= band_y1)) return;
    // One color per line: the fragment stage at the first end point.
    float v[VAR_MAX];
    float iw = a->inv_w != 0.0f ? 1.0f / a->inv_w : 0.0f;
    for (uint32_t k = 0; k < nvar; k++) v[k] = a->var[k] * iw;
    float special[2][4] = { { a->x, a->y, a->z, a->inv_w }, { 1, 1, 1, 1 } };
    float color[4];
    if (!shade(p, worker, v, (const float (*)[4])special, color)) return;
    bool blending = (p->state & FGPU_STATE_BLEND) != 0;
    uint32_t packed = pack_color(color);
    int32_t x0 = (int32_t)a->x, y0 = (int32_t)a->y, x1 = (int32_t)b->x, y1 = (int32_t)b->y;
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < 16384; guard++) {
        if (y0 >= band_y0 && y0 < band_y1 &&
            x0 >= p->vp_x && y0 >= p->vp_y && x0 < p->vp_x + p->vp_w && y0 < p->vp_y + p->vp_h &&
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

typedef struct {
    const fgpu_target_t* t;
    uint32_t flags, color;
    float depth;
    uint32_t band_h;
} clear_job_t;

static void clear_part(void* ctx, uint32_t part, uint32_t worker) {
    (void)worker;
    const clear_job_t* j = ctx;
    const fgpu_target_t* t = j->t;
    uint32_t y0 = part * j->band_h, y1 = y0 + j->band_h;
    if (y1 > t->height) y1 = t->height;
    uint32_t opaque = j->color | 0xFF000000u;
    for (uint32_t y = y0; y < y1; y++) {
        if ((j->flags & FGPU_CLEAR_COLOR) && t->color) {
            uint32_t* row = t->color + (size_t)y * t->color_pitch;
            for (uint32_t x = 0; x < t->width; x++) row[x] = opaque;
        }
        if ((j->flags & FGPU_CLEAR_DEPTH) && t->depth) {
            float* row = t->depth + (size_t)y * t->depth_pitch;
            for (uint32_t x = 0; x < t->width; x++) row[x] = j->depth;
        }
    }
}

// Rows per band for `rows` rows: about four bands per worker, so a worker
// that finishes early takes over more of the screen.
static uint32_t band_rows(uint32_t rows) {
    uint32_t workers = worker_count();
    if (workers <= 1 || rows == 0) return rows ? rows : 1;
    uint32_t h = (rows + workers * 4 - 1) / (workers * 4);
    return h < MIN_BAND_ROWS ? MIN_BAND_ROWS : h;
}

static void old_clear(const fgpu_target_t* t, uint32_t flags, uint32_t color, float depth) {
    if (!t || !t->height) return;
    clear_job_t j = { t, flags, color, depth, band_rows(t->height) };
    run_parts(clear_part, &j, (t->height + j.band_h - 1) / j.band_h);
}

// One triangle after the vertex stage and clipping: a polygon of 3 or 4
// screen vertices (n = 0: dropped).
typedef struct {
    uint32_t n;
    svtx_t v[4];
} prim_t;

static prim_t g_prims[CHUNK_TRIS];

typedef struct {
    const fgpu_target_t* t;
    const fgpu_pipeline_t* p;
    const fgpu_layout_t* layout;
    const float* vertices;
    uint32_t vertex_count;
    const uint32_t* indices;
    uint32_t nvar;
    uint32_t first, tris;           // this chunk: triangles first .. first + tris - 1
    int32_t y0, y1;                 // rows that can be drawn
    uint32_t band_h;
} draw_job_t;

static void geometry_part(void* ctx, uint32_t part, uint32_t worker) {
    const draw_job_t* j = ctx;
    const fgpu_pipeline_t* p = j->p;
    uint32_t end = (part + 1) * GEOMETRY_PART;
    if (end > j->tris) end = j->tris;
    for (uint32_t i = part * GEOMETRY_PART; i < end; i++) {
        prim_t* prim = &g_prims[i];
        prim->n = 0;
        uint32_t tri_first = (j->first + i) * 3;
        vtx_t tri[3];
        const float* src[3];
        bool bad = false;
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t index = j->indices ? j->indices[tri_first + k] : tri_first + k;
            if (index >= j->vertex_count) { bad = true; break; }
            src[k] = j->vertices + (size_t)index * j->layout->stride;
        }
        if (bad) continue;
        if (p->shader) shade_vertices(p, j->layout, worker, src, 3, tri);
        else for (int k = 0; k < 3; k++) vertex_stage(p, j->layout, src[k], &tri[k]);

        vtx_t poly[4];
        int n = clip_near(tri, 3, j->nvar, poly);
        if (n < 3) continue;
        for (int k = 0; k < n; k++) to_screen(p, &poly[k], j->nvar, &prim->v[k]);
        prim->n = (uint32_t)n;
    }
}

static void raster_part(void* ctx, uint32_t part, uint32_t worker) {
    const draw_job_t* j = ctx;
    int32_t y0 = j->y0 + (int32_t)(part * j->band_h);
    int32_t y1 = y0 + (int32_t)j->band_h;
    if (y1 > j->y1) y1 = j->y1;
    bool wire = (j->p->state & FGPU_STATE_WIREFRAME) != 0;
    // Every band walks the triangles in order, so overlapping triangles
    // land in the same order as on one core.
    for (uint32_t i = 0; i < j->tris; i++) {
        const prim_t* prim = &g_prims[i];
        int n = (int)prim->n;
        if (n < 3) continue;
        const svtx_t* s = prim->v;
        if (wire) {
            for (int k = 0; k < n; k++)
                plot_line(j->t, j->p, j->nvar, worker, y0, y1, &s[k], &s[(k + 1) % n]);
        } else {
            for (int k = 1; k + 1 < n; k++)
                raster_triangle(j->t, j->p, j->nvar, worker, y0, y1, &s[0], &s[k], &s[k + 1]);
        }
    }
}

static uint32_t old_draw_layout(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                                const fgpu_layout_t* layout,
                                const float* vertices, uint32_t vertex_count,
                                const uint32_t* indices, uint32_t count) {
    if (!t || !p || !vertices || p->vp_w <= 0 || p->vp_h <= 0 || !fgpu_layout_valid(layout))
        return 0;
    if (p->shader && !p->constants) return 0;
    if (++g_epoch == 0) {           // wrapped: make every batch stale again
        for (uint32_t w = 0; w < FGPU_RASTER_MAX_WORKERS; w++) g_batch_epoch[w] = 0;
        g_epoch = 1;
    }

    draw_job_t j = {
        .t = t, .p = p, .layout = layout, .vertices = vertices, .vertex_count = vertex_count,
        .indices = indices, .nvar = p->shader ? p->shader->h.varying_count * 4 : FIXED_VARS,
    };
    // Rows the viewport covers on the target. Lines clip the same way.
    j.y0 = p->vp_y < 0 ? 0 : p->vp_y;
    j.y1 = p->vp_y + p->vp_h;
    if (j.y1 > (int32_t)t->height) j.y1 = (int32_t)t->height;
    uint32_t rows = j.y1 > j.y0 ? (uint32_t)(j.y1 - j.y0) : 0;
    j.band_h = band_rows(rows);
    uint32_t bands = rows ? (rows + j.band_h - 1) / j.band_h : 0;

    uint32_t total = count / 3, drawn = 0;
    for (uint32_t first = 0; first < total; first += CHUNK_TRIS) {
        j.first = first;
        j.tris = total - first < CHUNK_TRIS ? total - first : CHUNK_TRIS;
        run_parts(geometry_part, &j, (j.tris + GEOMETRY_PART - 1) / GEOMETRY_PART);
        for (uint32_t i = 0; i < j.tris; i++) drawn += g_prims[i].n >= 3;
        if (bands) run_parts(raster_part, &j, bands);
    }
    return drawn;
}


/* ===========================================================================
 * Binned rasterizer (the default path)
 *
 * Draws are recorded into a pass with a snapshot of their state. At flush:
 *   1. geometry: workers take items (up to ITEM_TRIS triangles of one draw),
 *      shade vertices (8 lanes full), clip, cull, transform, set up the
 *      triangle in 16.8 fixed point and bin it into 64x64 tiles. Records and
 *      bin entries live in per-worker arenas made of chunks the main thread
 *      pre-allocated (nothing allocates inside a parallel phase).
 *   2. tiles: workers take tiles. A tile merges the per-worker bin lists in
 *      item order (= submission order), loads (or clears) color and depth
 *      into a local 64x64 copy, rasterizes with hierarchical Z over 8x8
 *      blocks, and stores the copy back.
 * Rounds bound the bin memory: if a round does not fit, only the items that
 * did are rasterized and the rest goes into the next round.
 * ======================================================================== */

#define TILE 64
#define TILE_BITS 6
#define BLK 8
#define NBLK 64
#define ITEM_TRIS 32
#define GUARD_PX 32768.0f
#define CHUNK_BYTES (64u * 1024u)
#define BIN_N 30
#define STATE_LOOKBACK 8
#define RETAIN_BYTES (1024u * 1024u)
#define MIN_BUDGET (1024u * 1024u)
#define MAX_DRAWS 65536u
#define MAX_ITEMS (1u << 24)
#define W_K (1.0f / 65536.0f)
#define DIRECT_PIXELS 400       // bounding boxes up to this many pixels skip blocks and hi-Z

static fgpu_raster_mode_t g_mode = FGPU_RASTER_BINNED;
static void* (*g_alloc)(size_t);
static void (*g_release)(void*);
static size_t g_budget = 8u << 20;
static fgpu_raster_stats_t g_stats;

void fgpu_raster_set_mode(fgpu_raster_mode_t mode) { g_mode = mode; }
fgpu_raster_mode_t fgpu_raster_get_mode(void) { return g_mode; }

void fgpu_raster_set_allocator(void* (*alloc)(size_t), void (*release)(void*)) {
    g_alloc = alloc;
    g_release = release;
}

void fgpu_raster_set_bin_budget(size_t bytes) { g_budget = bytes < MIN_BUDGET ? MIN_BUDGET : bytes; }

void fgpu_raster_get_stats(fgpu_raster_stats_t* out) { *out = g_stats; }
void fgpu_raster_reset_stats(void) { __builtin_memset(&g_stats, 0, sizeof(g_stats)); }

static void* mem_get(size_t n) {
    if (g_alloc) return g_alloc(n);
#if __STDC_HOSTED__
    return malloc(n);
#else
    return NULL;
#endif
}

static void mem_put(void* p) {
    if (!p) return;
    if (g_release) g_release(p);
#if __STDC_HOSTED__
    else if (!g_alloc) free(p);
#endif
}

/* ---- snapshots and records ---- */

typedef struct {
    fgpu_pipeline_t pipe;       // constants = NULL: see `constants`
    fgpu_layout_t layout;
    float constants[FGPU_SHADER_MAX_UNIFORMS][4];
    int32_t cx0, cy0, cx1, cy1; // viewport clipped to the target
    uint32_t mode;              // 0 shader, 1 color/gouraud, 2 phong
    uint32_t nv;                // floats per vertex kept after the vertex stage
} bstate_t;

typedef struct {
    uint32_t state;
    uint32_t vertex_count, tris, item_base;
    const float* vertices;
    const uint32_t* indices;
} bdraw_t;

typedef struct {
    uint32_t item, state;
    int32_t x0, y0, x1, y1;     // pixels to test: centers inside the bbox, clipped
    int64_t C[3];               // edge i: E = A*X + B*Y + C, X/Y = 256 * pixel + 128
    int32_t A[3], B[3];
    int64_t area;               // > 0, 2^-16 pixel^2
    float inv_area, z[3], inv_w[3];
    uint8_t m[3];               // E >= m[i] is inside (0 for owning edges, 1 else)
    uint8_t small, front;
    uint8_t pad[2];
    float var[];                // 3 * nv, premultiplied by 1/w, order a, b, c
} tri_rec_t;

typedef struct bchunk {
    struct bchunk* next;
    uint32_t n, pad;
    const tri_rec_t* e[BIN_N];
} bchunk_t;

typedef struct { bchunk_t *head, *tail; } blist_t;

typedef struct {
    uint32_t n;
    uint32_t idx[FGPU_LANES];
    float z[FGPU_LANES];
} fbatch_t;

typedef struct {
    uint32_t col[TILE * TILE];
    float dep[TILE * TILE];
    float bmin[NBLK], bmax[NBLK];
    uint64_t stale;
} tile_t;

typedef struct {
    uint32_t index;
    uint8_t* p;                 // arena: bump pointer in the current chunk
    uint8_t* end;
    bool failed;                // the pool ran dry during this item
    uint32_t binned, small;
    uint32_t vs_epoch, fs_epoch;
    const fgpu_program_t *vs_prog, *fs_prog;
    fgpu_shader_batch_t* vsb;
    fgpu_shader_batch_t* fsb;
    vtx_t* tv;                  // ITEM_TRIS * 3
    tile_t* tile;
    void* raw;
} worker_t;

struct fgpu_pass {
    fgpu_target_t t;
    bool have_target;
    uint32_t clear_flags, clear_color;
    float clear_depth;
    bdraw_t* draws;
    uint32_t ndraws, cap_draws, items;
    bstate_t** states;
    uint32_t nstates, cap_states, nv_max;
    void** owned;
    uint32_t nowned, cap_owned;
    bstate_t* cand;
    uint32_t drawn;
    // flush resources, kept between flushes
    worker_t* w[FGPU_RASTER_MAX_WORKERS];
    void** chunks;
    uint32_t nchunks, cap_chunks;
    blist_t* lists;
    size_t lists_cap;
    uint32_t* item_drawn;
    uint32_t item_drawn_cap;
    uint32_t epoch;
};

typedef struct {
    fgpu_pass_t* pass;
    const fgpu_target_t* t;
    uint32_t nw, ntx, nty, ntiles;
    uint32_t r0, r1;
    uint32_t* item_drawn;       // triangles drawn per item of this round
    volatile uint32_t next_item, limit, next_tile, pool_used;
    uint32_t clear_flags, clear_color;
    float clear_depth;
    uint32_t epoch;
    volatile uint32_t tiles_run;
} flush_t;

/* ---- small helpers ---- */

static inline double fabs_d(double x) { return x < 0 ? -x : x; }
static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

static inline int32_t to_fixed(float v) {
    return (int32_t)f_floor(v * 256.0f + 0.5f);
}

static uint32_t mode_of(const fgpu_pipeline_t* p) {
    return p->shader ? 0u : (p->program == FGPU_PROGRAM_PHONG ? 2u : 1u);
}

static uint32_t nv_of(const fgpu_pipeline_t* p) {
    return p->shader ? p->shader->h.varying_count * 4 : (p->program == FGPU_PROGRAM_PHONG ? FIXED_VARS : 4u);
}

/* ---- worker scratch ---- */

static void worker_free(worker_t* w) {
    if (w) mem_put(w->raw);
}

static worker_t* worker_new(void) {
    size_t bytes = sizeof(worker_t) + 2 * sizeof(fgpu_shader_batch_t) + ITEM_TRIS * 3 * sizeof(vtx_t) +
                   sizeof(tile_t) + 4 * 64;
    uint8_t* raw = mem_get(bytes);
    if (!raw) return NULL;
    __builtin_memset(raw, 0, bytes);
    uint8_t* q = (uint8_t*)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
    worker_t* w = (worker_t*)q;
    w->raw = raw;
    q += (sizeof(worker_t) + 63) & ~(size_t)63;
    w->vsb = (fgpu_shader_batch_t*)q; q += (sizeof(fgpu_shader_batch_t) + 63) & ~(size_t)63;
    w->fsb = (fgpu_shader_batch_t*)q; q += (sizeof(fgpu_shader_batch_t) + 63) & ~(size_t)63;
    w->tv = (vtx_t*)q; q += (ITEM_TRIS * 3 * sizeof(vtx_t) + 63) & ~(size_t)63;
    w->tile = (tile_t*)q;
    return w;
}

/* ---- pass lifetime ---- */

fgpu_pass_t* fgpu_pass_create(void) {
    fgpu_pass_t* p = mem_get(sizeof(*p));
    if (!p) return NULL;
    __builtin_memset(p, 0, sizeof(*p));
    p->cand = mem_get(sizeof(bstate_t));
    if (!p->cand) { mem_put(p); return NULL; }
    p->epoch = 1;
    return p;
}

static void pass_release_pending(fgpu_pass_t* p) {
    for (uint32_t i = 0; i < p->nowned; i++) mem_put(p->owned[i]);
    p->nowned = 0;
    for (uint32_t i = 0; i < p->nstates; i++) mem_put(p->states[i]);
    p->nstates = 0;
    p->ndraws = 0;
    p->items = 0;
    p->nv_max = 0;
    p->clear_flags = 0;
    p->have_target = false;
}

static void pass_trim(fgpu_pass_t* p, bool all) {
    if (!all && (uint64_t)p->nchunks * CHUNK_BYTES <= RETAIN_BYTES) return;
    for (uint32_t i = 0; i < p->nchunks; i++) mem_put(p->chunks[i]);
    p->nchunks = 0;
}

void fgpu_pass_discard(fgpu_pass_t* p) {
    if (p) pass_release_pending(p);
}

void fgpu_pass_destroy(fgpu_pass_t* p) {
    if (!p) return;
    pass_release_pending(p);
    pass_trim(p, true);
    for (uint32_t i = 0; i < FGPU_RASTER_MAX_WORKERS; i++) worker_free(p->w[i]);
    mem_put(p->chunks);
    mem_put(p->lists);
    mem_put(p->item_drawn);
    mem_put(p->draws);
    mem_put(p->states);
    mem_put(p->owned);
    mem_put(p->cand);
    mem_put(p);
}

bool fgpu_pass_pending(const fgpu_pass_t* p) {
    return p && (p->ndraws || p->clear_flags);
}

/* ---- geometry stage ---- */

// Vertex stage for the cheap built-in programs: only what the fragment
// stage reads (the color) is kept; the values are the same as vertex_stage's.
static inline void fetch_one(const fgpu_layout_t* l, const float* v, uint32_t i, float a[4]) {
    a[0] = a[1] = a[2] = 0.0f;
    a[3] = 1.0f;
    if (i >= l->count) return;
    uint32_t off = l->attrib[i] & 0xFFFF, n = l->attrib[i] >> 16;
    for (uint32_t k = 0; k < n; k++) a[k] = v[off + k];
}

static void vertex_stage_lite(const bstate_t* s, const float* in, vtx_t* out) {
    const fgpu_pipeline_t* p = &s->pipe;
    float a0[4], a2[4];
    fetch_one(&s->layout, in, 0, a0);
    fetch_one(&s->layout, in, 2, a2);
    float pos[4] = { a0[0], a0[1], a0[2], 1.0f };
    mat4_mul_vec(p->u.mvp, pos, out->clip);
    if (p->program == FGPU_PROGRAM_GOURAUD) {
        float a1[4], world[4], wn[4];
        fetch_one(&s->layout, in, 1, a1);
        mat4_mul_vec(p->u.model, pos, world);
        float n4[4] = { a1[0], a1[1], a1[2], 0.0f };
        mat4_mul_vec(p->u.normal, n4, wn);
        light(&p->u, wn, world, a2, out->var);
    } else {
        for (int i = 0; i < 4; i++) out->var[i] = a2[i];
    }
}

static fgpu_shader_batch_t* worker_batch(fgpu_pass_t* pass, worker_t* w, const bstate_t* s, bool fragment) {
    uint32_t* epoch = fragment ? &w->fs_epoch : &w->vs_epoch;
    const fgpu_program_t** prog = fragment ? &w->fs_prog : &w->vs_prog;
    fgpu_shader_batch_t* b = fragment ? w->fsb : w->vsb;
    if (*epoch != pass->epoch || *prog != s->pipe.shader) {
        *epoch = pass->epoch;
        *prog = s->pipe.shader;
        fgpu_shader_batch_reset(b, s->pipe.shader->h.temp_count);
    }
    return b;
}

// Runs the vertex shader for up to FGPU_LANES vertices at once.
static void shade_vertices_w(fgpu_pass_t* pass, worker_t* w, const bstate_t* s,
                             const float* const* in, uint32_t n, vtx_t* out) {
    fgpu_shader_batch_t* b = worker_batch(pass, w, s, false);
    for (uint32_t l = 0; l < n; l++) {
        float attr[FGPU_MAX_ATTRIBS][4];
        fetch_vertex(&s->layout, in[l], attr);
        for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++)
            for (int c = 0; c < 4; c++) b->input[i][c][l] = attr[i][c];
    }
    fgpu_shader_run_batch(s->pipe.shader, false, b, (1u << n) - 1, (const float (*)[4])s->constants,
                          s->pipe.textures);
    uint32_t nv = s->pipe.shader->h.varying_count;
    for (uint32_t l = 0; l < n; l++) {
        for (int c = 0; c < 4; c++) out[l].clip[c] = b->output[0][c][l];
        for (uint32_t v = 0; v < nv; v++)
            for (int c = 0; c < 4; c++) out[l].var[v * 4 + c] = b->output[1 + v][c][l];
    }
}

static void* pool_alloc(flush_t* F, worker_t* w, size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    if ((size_t)(w->end - w->p) < bytes) {
        uint32_t i = __atomic_fetch_add(&F->pool_used, 1, __ATOMIC_SEQ_CST);
        if (i >= F->pass->nchunks) { w->failed = true; return NULL; }
        w->p = (uint8_t*)(((uintptr_t)F->pass->chunks[i] + 15) & ~(uintptr_t)15);
        w->end = w->p + (CHUNK_BYTES - 16);
    }
    void* r = w->p;
    w->p += bytes;
    return r;
}

static void bin_add(flush_t* F, worker_t* w, uint32_t tile, const tri_rec_t* r) {
    blist_t* l = &F->pass->lists[(size_t)w->index * F->ntiles + tile];
    if (!l->tail || l->tail->n == BIN_N) {
        bchunk_t* c = pool_alloc(F, w, sizeof(bchunk_t));
        if (!c) return;
        c->next = NULL;
        c->n = 0;
        if (l->tail) l->tail->next = c; else l->head = c;
        l->tail = c;
    }
    l->tail->e[l->tail->n++] = r;
}

/* Edge i runs p -> q; E(X, Y) = (qx - px) * (Y - py) - (qy - py) * (X - px). */
static inline void edge_setup(tri_rec_t* r, int i, int32_t px, int32_t py, int32_t qx, int32_t qy) {
    r->A[i] = py - qy;
    r->B[i] = qx - px;
    r->C[i] = -((int64_t)r->B[i] * py + (int64_t)r->A[i] * px);
    int32_t dx = qx - px, dy = qy - py;
    r->m[i] = (dy < 0 || (dy == 0 && dx > 0)) ? 0 : 1;      // top-left rule
}

/* Classifies the pixels px0..px1 x py0..py1 (inclusive, sample = center):
 * 0 none inside, 1 some, 2 all. e[i][k] = edge i at the corners (x0,y0),
 * (x1,y0), (x0,y1), (x1,y1). */
static int classify(const tri_rec_t* r, int32_t px0, int32_t py0, int32_t px1, int32_t py1, int64_t e[3][4]) {
    int64_t X0 = (int64_t)px0 * 256 + 128, Y0 = (int64_t)py0 * 256 + 128;
    int64_t dX = (int64_t)(px1 - px0) * 256, dY = (int64_t)(py1 - py0) * 256;
    int all = 1;
    for (int i = 0; i < 3; i++) {
        int64_t a = r->A[i], b = r->B[i];
        int64_t c0 = r->C[i] + a * X0 + b * Y0;
        e[i][0] = c0;
        e[i][1] = c0 + a * dX;
        e[i][2] = c0 + b * dY;
        e[i][3] = e[i][1] + b * dY;
        int64_t mn = e[i][0], mx = e[i][0];
        for (int k = 1; k < 4; k++) {
            if (e[i][k] < mn) mn = e[i][k];
            if (e[i][k] > mx) mx = e[i][k];
        }
        if (mx < r->m[i]) return 0;
        if (mn < r->m[i]) all = 0;
    }
    return all ? 2 : 1;
}

// Turns the polygon (screen vertices, guard-band clipped) into triangle
// records and bin entries. Returns false when the pool ran dry.
static void setup_tri(flush_t* F, worker_t* w, uint32_t item, uint32_t state_idx, const bstate_t* s,
                      const svtx_t* a, const svtx_t* b, const svtx_t* c) {
    int32_t ax = to_fixed(a->x), ay = to_fixed(a->y);
    int32_t bx = to_fixed(b->x), by = to_fixed(b->y);
    int32_t cx = to_fixed(c->x), cy = to_fixed(c->y);
    int64_t area = (int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax);
    if (area == 0) return;
    bool front = area < 0;
    if ((s->pipe.state & FGPU_STATE_CULL_BACK) && !front) return;
    if ((s->pipe.state & FGPU_STATE_CULL_FRONT) && front) return;
    if (area < 0) {
        const svtx_t* t = b; b = c; c = t;
        int32_t tx = bx, ty = by; bx = cx; by = cy; cx = tx; cy = ty;
        area = -area;
    }
    int32_t minx = imin(ax, imin(bx, cx)), maxx = imax(ax, imax(bx, cx));
    int32_t miny = imin(ay, imin(by, cy)), maxy = imax(ay, imax(by, cy));
    // Pixels whose center lies in the bounding box.
    int32_t x0 = imax((minx - 128 + 255) >> 8, s->cx0), x1 = imin(((maxx - 128) >> 8) + 1, s->cx1);
    int32_t y0 = imax((miny - 128 + 255) >> 8, s->cy0), y1 = imin(((maxy - 128) >> 8) + 1, s->cy1);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t nv = s->nv;
    tri_rec_t* r = pool_alloc(F, w, sizeof(tri_rec_t) + 3 * nv * sizeof(float));
    if (!r) return;
    r->item = item;
    r->state = state_idx;
    r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;
    edge_setup(r, 0, bx, by, cx, cy);
    edge_setup(r, 1, cx, cy, ax, ay);
    edge_setup(r, 2, ax, ay, bx, by);
    r->area = area;
    r->inv_area = 1.0f / ((float)area * W_K);
    r->z[0] = a->z; r->z[1] = b->z; r->z[2] = c->z;
    r->inv_w[0] = a->inv_w; r->inv_w[1] = b->inv_w; r->inv_w[2] = c->inv_w;
    for (uint32_t k = 0; k < nv; k++) {
        r->var[k] = a->var[k];
        r->var[nv + k] = b->var[k];
        r->var[2 * nv + k] = c->var[k];
    }
    r->small = (x1 - x0 <= 2 && y1 - y0 <= 2);
    r->front = front;
    w->binned++;
    w->small += r->small;

    int32_t tx0 = x0 >> TILE_BITS, tx1 = (x1 - 1) >> TILE_BITS;
    int32_t ty0 = y0 >> TILE_BITS, ty1 = (y1 - 1) >> TILE_BITS;
    bool test = (tx1 - tx0 + 1) * (ty1 - ty0 + 1) > 4;
    for (int32_t ty = ty0; ty <= ty1; ty++) {
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            if (test) {
                int64_t e[3][4];
                if (!classify(r, imax(x0, tx << TILE_BITS), imax(y0, ty << TILE_BITS),
                              imin(x1, (tx + 1) << TILE_BITS) - 1, imin(y1, (ty + 1) << TILE_BITS) - 1, e))
                    continue;
            }
            bin_add(F, w, (uint32_t)ty * F->ntx + (uint32_t)tx, r);
            if (w->failed) return;
        }
    }
}

// Clips a screen-space polygon to |x|, |y| <= GUARD_PX. Everything the
// rasterizer interpolates is affine in screen space, so lerping is exact.
static uint32_t clip_guard(const svtx_t* in, uint32_t n, uint32_t nv, svtx_t* out, svtx_t* tmp) {
    const svtx_t* src = in;
    svtx_t* dst = tmp;
    for (int plane = 0; plane < 4; plane++) {
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; i++) {
            const svtx_t* p = &src[i];
            const svtx_t* q = &src[(i + 1) % n];
            float vp = plane < 2 ? p->x : p->y, vq = plane < 2 ? q->x : q->y;
            float lim = (plane & 1) ? -GUARD_PX : GUARD_PX;
            // distance inside: plane 0: G - x, 1: x + G, 2: G - y, 3: y + G
            float dp = (plane & 1) ? vp - lim : lim - vp;
            float dq = (plane & 1) ? vq - lim : lim - vq;
            bool ip = dp >= 0.0f, iq = dq >= 0.0f;
            if (ip) dst[m++] = *p;
            if (ip != iq) {
                float t = dp / (dp - dq);
                svtx_t* o = &dst[m++];
                o->x = p->x + (q->x - p->x) * t;
                o->y = p->y + (q->y - p->y) * t;
                o->z = p->z + (q->z - p->z) * t;
                o->inv_w = p->inv_w + (q->inv_w - p->inv_w) * t;
                for (uint32_t k = 0; k < nv; k++) o->var[k] = p->var[k] + (q->var[k] - p->var[k]) * t;
                if (plane < 2) o->x = lim; else o->y = lim;
            }
        }
        n = m;
        if (n < 3) return 0;
        const svtx_t* s2 = dst;
        dst = (dst == tmp) ? out : tmp;
        src = s2;
    }
    if (src != out) for (uint32_t i = 0; i < n; i++) out[i] = src[i];
    return n;
}

static const bdraw_t* find_draw(const fgpu_pass_t* pass, uint32_t item) {
    uint32_t lo = 0, hi = pass->ndraws;      // last draw with item_base <= item
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (pass->draws[mid].item_base <= item) lo = mid; else hi = mid;
    }
    return &pass->draws[lo];
}

static uint32_t geom_item(flush_t* F, worker_t* w, uint32_t item) {
    fgpu_pass_t* pass = F->pass;
    const bdraw_t* d = find_draw(pass, item);
    const bstate_t* s = pass->states[d->state];
    uint32_t first = (item - d->item_base) * ITEM_TRIS;
    uint32_t n = d->tris - first < ITEM_TRIS ? d->tris - first : ITEM_TRIS;
    uint32_t stride = s->layout.stride, nv = s->nv;

    const float* src[ITEM_TRIS * 3];
    bool valid[ITEM_TRIS];
    uint32_t nsrc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tri = (first + i) * 3;
        const float* v[3];
        bool ok = true;
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t idx = d->indices ? d->indices[tri + k] : tri + k;
            if (idx >= d->vertex_count) { ok = false; break; }
            v[k] = d->vertices + (size_t)idx * stride;
        }
        valid[i] = ok;
        if (ok) for (uint32_t k = 0; k < 3; k++) src[nsrc++] = v[k];
    }
    vtx_t* tv = w->tv;
    if (s->mode == 0) {
        for (uint32_t v0 = 0; v0 < nsrc; v0 += FGPU_LANES)
            shade_vertices_w(pass, w, s, &src[v0], nsrc - v0 < FGPU_LANES ? nsrc - v0 : FGPU_LANES, &tv[v0]);
    } else if (s->mode == 1) {
        for (uint32_t v = 0; v < nsrc; v++) vertex_stage_lite(s, src[v], &tv[v]);
    } else {
        for (uint32_t v = 0; v < nsrc; v++) vertex_stage(&s->pipe, &s->layout, src[v], &tv[v]);
    }

    uint32_t vi = 0, drawn = 0;
    for (uint32_t i = 0; i < n && !w->failed; i++) {
        if (!valid[i]) continue;
        vtx_t* tri = &tv[vi];
        vi += 3;
        vtx_t poly[4];
        const vtx_t* pv = poly;
        int np;
        bool all_in = true;
        for (int k = 0; k < 3; k++)
            if (!(tri[k].clip[2] + tri[k].clip[3] >= 0.0f && tri[k].clip[3] > NEAR_EPS)) all_in = false;
        if (all_in) { pv = tri; np = 3; }       // what clip_near would copy unchanged
        else np = clip_near(tri, 3, nv, poly);
        if (np < 3) continue;
        drawn++;
        svtx_t sv[8];
        bool wide = false;
        for (int k = 0; k < np; k++) {
            to_screen(&s->pipe, &pv[k], nv, &sv[k]);
            if (!(sv[k].x <= GUARD_PX && sv[k].x >= -GUARD_PX && sv[k].y <= GUARD_PX && sv[k].y >= -GUARD_PX))
                wide = true;
        }
        uint32_t cnt = (uint32_t)np;
        if (wide) {
            svtx_t g[8], tmp[8];
            cnt = clip_guard(sv, cnt, nv, g, tmp);
            for (uint32_t k = 0; k < cnt; k++) sv[k] = g[k];
        }
        for (uint32_t k = 1; k + 1 < cnt && !w->failed; k++)
            setup_tri(F, w, item, d->state, s, &sv[0], &sv[k], &sv[k + 1]);
    }
    return drawn;
}

static void geom_loop(void* ctx, uint32_t part, uint32_t worker) {
    (void)part;
    flush_t* F = ctx;
    if (worker >= F->nw) return;
    worker_t* w = F->pass->w[worker];
    for (;;) {
        uint32_t item = __atomic_fetch_add(&F->next_item, 1, __ATOMIC_SEQ_CST);
        if (item >= F->r1 || item >= __atomic_load_n(&F->limit, __ATOMIC_SEQ_CST)) break;
        w->failed = false;
        uint32_t drawn = geom_item(F, w, item);
        F->item_drawn[item - F->r0] = drawn;
        if (w->failed) {
            // This item may be half binned: everything from here on waits
            // for the next round. (Its entries are skipped by the tiles.)
            uint32_t cur = __atomic_load_n(&F->limit, __ATOMIC_SEQ_CST);
            while (item < cur && !__atomic_compare_exchange_n(&F->limit, &cur, item, false,
                                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
            w->failed = false;
            break;
        }
    }
}

/* ---- tile stage ---- */

typedef struct {
    flush_t* F;
    worker_t* W;
    tile_t* T;
    const fgpu_target_t* t;
    int32_t tx0, ty0;
    uint32_t state_idx;
    const bstate_t* s;
    bool depth_test, depth_write, blending, late, color_on;
    uint32_t mode;
    fgpu_shader_batch_t* fsb;
    fbatch_t fb;
} tctx_t;

static inline uint32_t blk_of(uint32_t idx) {
    return ((idx >> 9) << 3) | ((idx & 63) >> 3);
}

// A depth value was written at a pixel: keep the bounds valid (the max may
// be too high until the block is recomputed).
static inline void hiz_note(tile_t* T, uint32_t idx, float z) {
    uint32_t b = blk_of(idx);
    if (z < T->bmin[b]) T->bmin[b] = z;
    if (z > T->bmax[b]) T->bmax[b] = z;
    T->stale |= 1ull << b;
}

static void hiz_recompute(tile_t* T, uint32_t b) {
    const float* d = T->dep + ((b >> 3) * BLK) * TILE + (b & 7) * BLK;
    float mn = d[0], mx = d[0];
    for (int y = 0; y < BLK; y++)
        for (int x = 0; x < BLK; x++) {
            float v = d[y * TILE + x];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    T->bmin[b] = mn;
    T->bmax[b] = mx;
    T->stale &= ~(1ull << b);
}

static void flush_frags(tctx_t* c) {
    fbatch_t* fb = &c->fb;
    if (!fb->n) return;
    const bstate_t* s = c->s;
    uint32_t kept = fgpu_shader_run_batch(s->pipe.shader, true, c->fsb, (1u << fb->n) - 1,
                                          (const float (*)[4])s->constants, s->pipe.textures);
    for (uint32_t l = 0; l < fb->n; l++) {
        if (!(kept & (1u << l))) continue;
        uint32_t idx = fb->idx[l];
        if (c->late && c->depth_write) {
            c->T->dep[idx] = fb->z[l];
            hiz_note(c->T, idx, fb->z[l]);
        }
        if (!c->color_on) continue;
        float out[4] = { c->fsb->output[0][0][l], c->fsb->output[0][1][l], c->fsb->output[0][2][l],
                         c->fsb->output[0][3][l] };
        uint32_t* px = &c->T->col[idx];
        *px = c->blending ? blend(out, *px) : pack_color(out);
    }
    fb->n = 0;
}

static void set_state(tctx_t* c, uint32_t idx) {
    flush_frags(c);
    c->state_idx = idx;
    const bstate_t* s = c->F->pass->states[idx];
    c->s = s;
    bool has_depth = c->t->depth != NULL;
    c->depth_test = has_depth && (s->pipe.state & FGPU_STATE_DEPTH_TEST);
    c->depth_write = has_depth && (s->pipe.state & FGPU_STATE_DEPTH_WRITE);
    c->blending = (s->pipe.state & FGPU_STATE_BLEND) != 0;
    c->late = s->pipe.shader && (s->pipe.shader->h.flags & FGPU_SHADER_FLAG_DISCARD);
    c->color_on = c->t->color != NULL;
    c->mode = s->mode;
    if (s->mode == 0) c->fsb = worker_batch(c->F->pass, c->W, s, true);
}

// The fragment stage for one covered pixel (e0..e2: exact edge values).
static inline void frag(tctx_t* c, const tri_rec_t* r, int32_t lx, int32_t ly,
                        int64_t e0, int64_t e1, int64_t e2, bool depth_ok, bool note) {
    float w0 = (float)e0 * W_K, w1 = (float)e1 * W_K, w2 = (float)e2 * W_K;
    float l0 = w0 * r->inv_area, l1 = w1 * r->inv_area, l2 = w2 * r->inv_area;
    float z = l0 * r->z[0] + l1 * r->z[1] + l2 * r->z[2];
    uint32_t idx = (uint32_t)ly * TILE + (uint32_t)lx;
    tile_t* T = c->T;
    if (c->depth_test && !depth_ok && z >= T->dep[idx]) return;
    if (c->depth_write && !c->late) {
        T->dep[idx] = z;
        if (note) hiz_note(T, idx, z);
    }
    if (!c->color_on && !c->late) return;

    float iw = l0 * r->inv_w[0] + l1 * r->inv_w[1] + l2 * r->inv_w[2];
    float wpix = iw != 0.0f ? 1.0f / iw : 0.0f;
    uint32_t nv = c->s->nv;
    const float* va = r->var;
    const float* vb = va + nv;
    const float* vc = vb + nv;
    if (c->mode == 0) {
        fgpu_shader_batch_t* sb = c->fsb;
        fbatch_t* fb = &c->fb;
        uint32_t lane = fb->n++;
        for (uint32_t k = 0; k < nv; k++)
            sb->input[k >> 2][k & 3][lane] = (l0 * va[k] + l1 * vb[k] + l2 * vc[k]) * wpix;
        sb->special[0][0][lane] = (float)(c->tx0 + lx) + 0.5f;
        sb->special[0][1][lane] = (float)(c->ty0 + ly) + 0.5f;
        sb->special[0][2][lane] = z;
        sb->special[0][3][lane] = iw;
        for (int k = 0; k < 4; k++) sb->special[1][k][lane] = r->front ? 1.0f : 0.0f;
        fb->idx[lane] = idx;
        fb->z[lane] = z;
        if (fb->n == FGPU_LANES) flush_frags(c);
        return;
    }
    if (c->mode == 1) {         // built-in color / Gouraud: the interpolated color
        float out1[4] = { (l0 * va[0] + l1 * vb[0] + l2 * vc[0]) * wpix,
                          (l0 * va[1] + l1 * vb[1] + l2 * vc[1]) * wpix,
                          (l0 * va[2] + l1 * vb[2] + l2 * vc[2]) * wpix,
                          (l0 * va[3] + l1 * vb[3] + l2 * vc[3]) * wpix };
        uint32_t* px1 = &T->col[idx];
        *px1 = c->blending ? blend(out1, *px1) : pack_color(out1);
        return;
    }
    float out[4];
    float v[VAR_MAX];
    uint32_t n = nv;
    for (uint32_t k = 0; k < n; k++) v[k] = (l0 * va[k] + l1 * vb[k] + l2 * vc[k]) * wpix;
    if (c->mode == 2) {
        light(&c->s->pipe.u, v + 3, v, v + 6, out);
    } else {
        out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; out[3] = v[3];
    }
    uint32_t* px = &T->col[idx];
    *px = c->blending ? blend(out, *px) : pack_color(out);
}

// Pixels of a small triangle (rect up to DIRECT_PIXELS): edge functions at
// each sample, no blocks and no hierarchical Z.
static void tri_direct(tctx_t* c, const tri_rec_t* r) {
    int32_t x0 = imax(r->x0, c->tx0), x1 = imin(r->x1, c->tx0 + TILE);
    int32_t y0 = imax(r->y0, c->ty0), y1 = imin(r->y1, c->ty0 + TILE);
    if (x0 >= x1 || y0 >= y1) return;
    int64_t X0 = (int64_t)x0 * 256 + 128;
    int64_t dA0 = (int64_t)r->A[0] * 256, dA1 = (int64_t)r->A[1] * 256, dA2 = (int64_t)r->A[2] * 256;
    int64_t m0 = r->m[0], m1 = r->m[1], m2 = r->m[2];
    for (int32_t y = y0; y < y1; y++) {
        int64_t Y = (int64_t)y * 256 + 128;
        int64_t e0 = r->A[0] * X0 + r->B[0] * Y + r->C[0];
        int64_t e1 = r->A[1] * X0 + r->B[1] * Y + r->C[1];
        int64_t e2 = r->A[2] * X0 + r->B[2] * Y + r->C[2];
        for (int32_t x = x0; x < x1; x++, e0 += dA0, e1 += dA1, e2 += dA2) {
            if (((e0 - m0) | (e1 - m1) | (e2 - m2)) < 0) continue;
            frag(c, r, x - c->tx0, y - c->ty0, e0, e1, e2, false, true);
        }
    }
}

static void tri_big(tctx_t* c, const tri_rec_t* r) {
    tile_t* T = c->T;
    int32_t rx0 = imax(r->x0, c->tx0), rx1 = imin(r->x1, c->tx0 + TILE);
    int32_t ry0 = imax(r->y0, c->ty0), ry1 = imin(r->y1, c->ty0 + TILE);
    if (rx0 >= rx1 || ry0 >= ry1) return;
    int64_t e[3][4];
    int tcls = classify(r, rx0, ry0, rx1 - 1, ry1 - 1, e);
    if (tcls == 0) return;

    double inv_area_d = 1.0 / (double)r->area;
    double margin = 1e-5 * (1.0 + fabs_d(r->z[0]) + fabs_d(r->z[1]) + fabs_d(r->z[2]));
    if (c->depth_test) {
        // Whole tile: the plane's range over the rectangle against the
        // largest depth in the tile.
        double zmin = 1e300, zmax = -1e300;
        for (int k = 0; k < 4; k++) {
            double z = ((double)e[0][k] * r->z[0] + (double)e[1][k] * r->z[1] + (double)e[2][k] * r->z[2]) * inv_area_d;
            if (z < zmin) zmin = z;
            if (z > zmax) zmax = z;
        }
        float tmax = T->bmax[0];
        for (int b = 1; b < NBLK; b++) if (T->bmax[b] > tmax) tmax = T->bmax[b];
        if ((float)(zmin - margin) >= tmax) return;
    }
    int32_t bx_lo = (rx0 - c->tx0) >> 3, bx_hi = (rx1 - 1 - c->tx0) >> 3;
    int32_t by_lo = (ry0 - c->ty0) >> 3, by_hi = (ry1 - 1 - c->ty0) >> 3;
    for (int32_t by = by_lo; by <= by_hi; by++) {
        for (int32_t bx = bx_lo; bx <= bx_hi; bx++) {
            int32_t px0 = imax(rx0, c->tx0 + bx * BLK), px1 = imin(rx1, c->tx0 + bx * BLK + BLK) - 1;
            int32_t py0 = imax(ry0, c->ty0 + by * BLK), py1 = imin(ry1, c->ty0 + by * BLK + BLK) - 1;
            uint32_t b = (uint32_t)(by * 8 + bx);
            int64_t eb[3][4];
            int cls;
            bool need_corners = c->depth_test;
            if (tcls == 2 && !need_corners) {
                cls = 2;
                int64_t X0 = (int64_t)px0 * 256 + 128, Y0 = (int64_t)py0 * 256 + 128;
                for (int i = 0; i < 3; i++) eb[i][0] = r->A[i] * X0 + r->B[i] * Y0 + r->C[i];
            } else {
                cls = classify(r, px0, py0, px1, py1, eb);
                if (cls == 0) continue;
            }
            bool depth_ok = false;
            if (c->depth_test) {
                double zmin = 1e300, zmax = -1e300;
                for (int k = 0; k < 4; k++) {
                    double z = ((double)eb[0][k] * r->z[0] + (double)eb[1][k] * r->z[1] +
                                (double)eb[2][k] * r->z[2]) * inv_area_d;
                    if (z < zmin) zmin = z;
                    if (z > zmax) zmax = z;
                }
                float zlo = (float)(zmin - margin), zhi = (float)(zmax + margin);
                if (zlo >= T->bmax[b]) continue;
                if (((T->stale >> b) & 1) && zlo >= T->bmin[b]) {
                    hiz_recompute(T, b);
                    if (zlo >= T->bmax[b]) continue;
                }
                depth_ok = zhi < T->bmin[b];
            }
            int64_t dA0 = (int64_t)r->A[0] * 256, dA1 = (int64_t)r->A[1] * 256, dA2 = (int64_t)r->A[2] * 256;
            int64_t m0 = r->m[0], m1 = r->m[1], m2 = r->m[2];
            // A block this triangle overwrites completely gets exact bounds afterwards,
            // so its per-pixel writes need not update them.
            bool exact_after = cls == 2 && c->depth_write && !c->late && px1 - px0 == BLK - 1 &&
                               py1 - py0 == BLK - 1 && (depth_ok || !c->depth_test);
            bool note = !exact_after;
            for (int32_t y = py0; y <= py1; y++) {
                int64_t dy = (int64_t)(y - py0) * 256;
                int64_t w0 = eb[0][0] + (int64_t)r->B[0] * dy;
                int64_t w1 = eb[1][0] + (int64_t)r->B[1] * dy;
                int64_t w2 = eb[2][0] + (int64_t)r->B[2] * dy;
                int32_t ly = y - c->ty0;
                if (cls == 2) {
                    for (int32_t x = px0; x <= px1; x++, w0 += dA0, w1 += dA1, w2 += dA2)
                        frag(c, r, x - c->tx0, ly, w0, w1, w2, depth_ok, note);
                } else {
                    for (int32_t x = px0; x <= px1; x++, w0 += dA0, w1 += dA1, w2 += dA2) {
                        if (((w0 - m0) | (w1 - m1) | (w2 - m2)) < 0) continue;
                        frag(c, r, x - c->tx0, ly, w0, w1, w2, depth_ok, note);
                    }
                }
            }
            if (exact_after) hiz_recompute(T, b);
        }
    }
}

static void tri_draw(tctx_t* c, const tri_rec_t* r) {
    if (r->state != c->state_idx) set_state(c, r->state);
    if ((r->x1 - r->x0) * (r->y1 - r->y0) <= DIRECT_PIXELS) tri_direct(c, r); else tri_big(c, r);
    if (c->late) flush_frags(c);
}

static void tile_run(flush_t* F, worker_t* W, uint32_t tile) {
    const fgpu_target_t* t = F->t;
    fgpu_pass_t* pass = F->pass;
    uint32_t tx = tile % F->ntx, ty = tile / F->ntx;
    int32_t ox = (int32_t)(tx * TILE), oy = (int32_t)(ty * TILE);
    int32_t w = imin(TILE, (int32_t)t->width - ox), h = imin(TILE, (int32_t)t->height - oy);
    if (w <= 0 || h <= 0) return;
    bool any = false;
    for (uint32_t i = 0; i < F->nw; i++)
        if (pass->lists[(size_t)i * F->ntiles + tile].head) { any = true; break; }
    bool clr_c = (F->clear_flags & FGPU_CLEAR_COLOR) && t->color;
    bool clr_d = (F->clear_flags & FGPU_CLEAR_DEPTH) && t->depth;
    if (!any && !clr_c && !clr_d) return;
    uint32_t opaque = F->clear_color | 0xFF000000u;

    if (!any) {      // only a clear: write straight to the target
        for (int32_t y = 0; y < h; y++) {
            if (clr_c) {
                uint32_t* row = t->color + (size_t)(oy + y) * t->color_pitch + ox;
                for (int32_t x = 0; x < w; x++) row[x] = opaque;
            }
            if (clr_d) {
                float* row = t->depth + (size_t)(oy + y) * t->depth_pitch + ox;
                for (int32_t x = 0; x < w; x++) row[x] = F->clear_depth;
            }
        }
        return;
    }

    tile_t* T = W->tile;
    if (t->color) {
        if (clr_c) {
            for (int i = 0; i < TILE * TILE; i++) T->col[i] = opaque;
        } else {
            for (int32_t y = 0; y < h; y++)
                __builtin_memcpy(&T->col[y * TILE], t->color + (size_t)(oy + y) * t->color_pitch + ox, (size_t)w * 4);
        }
    }
    if (t->depth) {
        if (clr_d) {
            for (int i = 0; i < TILE * TILE; i++) T->dep[i] = F->clear_depth;
            for (int b = 0; b < NBLK; b++) { T->bmin[b] = F->clear_depth; T->bmax[b] = F->clear_depth; }
            T->stale = 0;
        } else {
            if (w < TILE || h < TILE) for (int i = 0; i < TILE * TILE; i++) T->dep[i] = 1.0f;
            for (int32_t y = 0; y < h; y++)
                __builtin_memcpy(&T->dep[y * TILE], t->depth + (size_t)(oy + y) * t->depth_pitch + ox, (size_t)w * 4);
            for (uint32_t b = 0; b < NBLK; b++) hiz_recompute(T, b);
        }
    }

    tctx_t c;
    __builtin_memset(&c, 0, sizeof(c));
    c.F = F; c.W = W; c.T = T; c.t = t;
    c.tx0 = ox; c.ty0 = oy;
    c.state_idx = 0xFFFFFFFFu;

    const bchunk_t* ch[FGPU_RASTER_MAX_WORKERS];
    uint32_t ci[FGPU_RASTER_MAX_WORKERS], key[FGPU_RASTER_MAX_WORKERS];
    uint32_t limit = F->limit;
    for (uint32_t i = 0; i < F->nw; i++) {
        ch[i] = pass->lists[(size_t)i * F->ntiles + tile].head;
        ci[i] = 0;
        key[i] = ch[i] && ch[i]->n ? ch[i]->e[0]->item : 0xFFFFFFFFu;
        if (key[i] >= limit) key[i] = 0xFFFFFFFFu;
    }
    for (;;) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < F->nw; i++) if (key[i] < key[best]) best = i;
        uint32_t item = key[best];
        if (item == 0xFFFFFFFFu) break;
        while (key[best] == item) {
            tri_draw(&c, ch[best]->e[ci[best]]);
            if (++ci[best] == ch[best]->n) { ch[best] = ch[best]->next; ci[best] = 0; }
            key[best] = ch[best] ? ch[best]->e[ci[best]]->item : 0xFFFFFFFFu;
            if (key[best] >= limit) key[best] = 0xFFFFFFFFu;
        }
    }
    flush_frags(&c);

    if (t->color)
        for (int32_t y = 0; y < h; y++)
            __builtin_memcpy(t->color + (size_t)(oy + y) * t->color_pitch + ox, &T->col[y * TILE], (size_t)w * 4);
    if (t->depth)
        for (int32_t y = 0; y < h; y++)
            __builtin_memcpy(t->depth + (size_t)(oy + y) * t->depth_pitch + ox, &T->dep[y * TILE], (size_t)w * 4);
}

static void tile_loop(void* ctx, uint32_t part, uint32_t worker) {
    (void)part;
    flush_t* F = ctx;
    if (worker >= F->nw) return;
    worker_t* w = F->pass->w[worker];
    uint32_t done = 0;
    for (;;) {
        uint32_t tile = __atomic_fetch_add(&F->next_tile, 1, __ATOMIC_SEQ_CST);
        if (tile >= F->ntiles) break;
        tile_run(F, w, tile);
        done++;
    }
    __atomic_fetch_add(&F->tiles_run, done, __ATOMIC_SEQ_CST);
}

/* ---- flush ---- */

static void replay_item(fgpu_pass_t* pass, uint32_t item) {
    const bdraw_t* d = find_draw(pass, item);
    const bstate_t* s = pass->states[d->state];
    uint32_t first = (item - d->item_base) * ITEM_TRIS;
    uint32_t n = d->tris - first < ITEM_TRIS ? d->tris - first : ITEM_TRIS;
    fgpu_pipeline_t p = s->pipe;
    p.constants = (const float (*)[4])s->constants;
    if (d->indices)
        pass->drawn += old_draw_layout(&pass->t, &p, &s->layout, d->vertices, d->vertex_count,
                                       d->indices + (size_t)first * 3, n * 3);
    else
        pass->drawn += old_draw_layout(&pass->t, &p, &s->layout,
                                       d->vertices + (size_t)first * 3 * s->layout.stride,
                                       d->vertex_count - first * 3, NULL, n * 3);
    g_stats.fallbacks++;
}

static uint32_t chunks_for(uint32_t bytes) { return (bytes + CHUNK_BYTES - 1) / CHUNK_BYTES; }

static bool pool_ensure(fgpu_pass_t* p, uint32_t want, uint32_t cap) {
    if (want > cap) want = cap;
    bool grew = false;
    while (p->nchunks < want) {
        if (p->nchunks == p->cap_chunks) {
            uint32_t nc = p->cap_chunks ? p->cap_chunks * 2 : 64;
            void** n = mem_get(nc * sizeof(void*));
            if (!n) return grew;
            if (p->nchunks) __builtin_memcpy(n, p->chunks, p->nchunks * sizeof(void*));
            mem_put(p->chunks);
            p->chunks = n;
            p->cap_chunks = nc;
        }
        void* c = mem_get(CHUNK_BYTES);
        if (!c) return grew;
        p->chunks[p->nchunks++] = c;
        grew = true;
    }
    return grew;
}

static void pass_flush_impl(fgpu_pass_t* pass);

uint32_t fgpu_pass_flush(fgpu_pass_t* pass) {
    if (!pass) return 0;
    pass_flush_impl(pass);
    uint32_t d = pass->drawn;
    pass->drawn = 0;
    return d;
}

static void pass_flush_impl(fgpu_pass_t* pass) {
    if (!pass->ndraws && !pass->clear_flags) { pass->have_target = false; return; }
    g_stats.flushes++;
    const fgpu_target_t* t = &pass->t;
    flush_t F;
    __builtin_memset(&F, 0, sizeof(F));
    F.pass = pass;
    F.t = t;
    F.clear_flags = pass->clear_flags;
    F.clear_color = pass->clear_color;
    F.clear_depth = pass->clear_depth;
    uint32_t total = pass->items;
    uint32_t r0 = 0;
    bool clear_pending = pass->clear_flags != 0;
    bool ok = t->width && t->height;

    F.nw = worker_count();
    F.ntx = (t->width + TILE - 1) / TILE;
    F.nty = (t->height + TILE - 1) / TILE;
    F.ntiles = F.ntx * F.nty;
    if (ok) {
        for (uint32_t i = 0; i < F.nw && ok; i++) {
            if (!pass->w[i]) pass->w[i] = worker_new();
            if (!pass->w[i]) ok = false;
            else pass->w[i]->index = i;
        }
        size_t need = (size_t)F.ntiles * F.nw;
        if (ok && pass->lists_cap < need) {
            blist_t* l = mem_get(need * sizeof(blist_t));
            if (l) { mem_put(pass->lists); pass->lists = l; pass->lists_cap = need; }
            else ok = false;
        }
    }
    uint32_t cap_chunks = (uint32_t)(g_budget / CHUNK_BYTES);
    uint32_t nv_max = pass->nv_max ? pass->nv_max : 4;
    uint32_t est_tri = (uint32_t)sizeof(tri_rec_t) + 12 * nv_max + 48;
    uint32_t items_round = (uint32_t)((g_budget / 4 * 3) / ((uint64_t)est_tri * ITEM_TRIS));
    if (items_round < 1) items_round = 1;

    while (ok && (r0 < total || clear_pending)) {
        uint32_t n_items = total - r0 < items_round ? total - r0 : items_round;
        uint32_t want = chunks_for(n_items * ITEM_TRIS * est_tri) + F.nw + 2 +
                        chunks_for((uint32_t)(F.ntiles * F.nw * sizeof(bchunk_t)) / 2);
        if (n_items) {
            pool_ensure(pass, want, cap_chunks);
            if (!pass->nchunks) break;
        }
        if (n_items > pass->item_drawn_cap) {
            uint32_t* a = mem_get((size_t)n_items * sizeof(uint32_t));
            if (!a) { ok = false; break; }
            mem_put(pass->item_drawn);
            pass->item_drawn = a;
            pass->item_drawn_cap = n_items;
        }
        F.item_drawn = pass->item_drawn;
        g_stats.rounds++;
        F.r0 = r0;
        F.r1 = r0 + n_items;
        F.next_item = r0;
        F.limit = F.r1;
        F.pool_used = 0;
        F.next_tile = 0;
        F.epoch = ++pass->epoch;
        if (pass->epoch == 0) pass->epoch = F.epoch = 1;
        __builtin_memset(pass->lists, 0, (size_t)F.ntiles * F.nw * sizeof(blist_t));
        for (uint32_t i = 0; i < F.nw; i++) {
            worker_t* w = pass->w[i];
            w->p = w->end = NULL;
            w->failed = false;
        }
        uint64_t c0 = __builtin_ia32_rdtsc();
        if (n_items) run_parts(geom_loop, &F, F.nw);
        g_stats.geometry_cycles += __builtin_ia32_rdtsc() - c0;
        uint32_t limit = F.limit;
        if (n_items && limit == r0) {
            // Nothing fit: more memory if the budget allows, else the old path.
            uint32_t before = pass->nchunks;
            if (pass->nchunks < cap_chunks) pool_ensure(pass, pass->nchunks * 2, cap_chunks);
            if (pass->nchunks > before) continue;
            if (clear_pending) {
                old_clear(t, F.clear_flags, F.clear_color, F.clear_depth);
                clear_pending = false;
                F.clear_flags = 0;
            }
            replay_item(pass, r0);
            r0++;
            continue;
        }
        for (uint32_t i = r0; i < limit; i++) pass->drawn += F.item_drawn[i - r0];
        F.limit = limit;
        F.next_tile = 0;
        c0 = __builtin_ia32_rdtsc();
        run_parts(tile_loop, &F, F.nw);
        g_stats.tile_cycles += __builtin_ia32_rdtsc() - c0;
        clear_pending = false;
        F.clear_flags = 0;
        if (limit < F.r1) {
            items_round = limit - r0 > 1 ? limit - r0 : 1;
            g_stats.rounds += 0;
        }
        r0 = limit;
    }
    for (uint32_t i = 0; i < F.nw; i++) {
        worker_t* w = pass->w[i];
        if (!w) continue;
        g_stats.tris_binned += w->binned;
        g_stats.small_tris += w->small;
        w->binned = w->small = 0;
    }
    g_stats.tiles_run += F.tiles_run;
    {
        uint64_t used = (uint64_t)pass->nchunks * CHUNK_BYTES;
        if (used > g_stats.bin_bytes_peak) g_stats.bin_bytes_peak = used;
    }
    if (!ok || r0 < total || clear_pending) {      // could not set up: draw the rest the old way
        if (clear_pending) {
            old_clear(t, F.clear_flags, F.clear_color, F.clear_depth);
            clear_pending = false;
        }
        for (; r0 < total; r0++) replay_item(pass, r0);
    }
    pass_release_pending(pass);
    pass_trim(pass, false);
}

/* ---- recording ---- */

static bool same_target(const fgpu_target_t* a, const fgpu_target_t* b) {
    return a->color == b->color && a->color_pitch == b->color_pitch && a->depth == b->depth &&
           a->depth_pitch == b->depth_pitch && a->width == b->width && a->height == b->height;
}

static void* grow(void* old, uint32_t count, uint32_t* cap, size_t elem) {
    uint32_t nc = *cap ? *cap * 2 : 64;
    void* n = mem_get((size_t)nc * elem);
    if (!n) return NULL;
    if (count) __builtin_memcpy(n, old, (size_t)count * elem);
    mem_put(old);
    *cap = nc;
    return n;
}

void fgpu_pass_clear(fgpu_pass_t* pass, const fgpu_target_t* t, uint32_t flags, uint32_t color, float depth) {
    if (!t || !t->height) return;
    if (g_mode == FGPU_RASTER_IMMEDIATE || !pass) { old_clear(t, flags, color, depth); return; }
    if ((pass->have_target && !same_target(&pass->t, t)) || pass->ndraws) pass_flush_impl(pass);
    pass->t = *t;
    pass->have_target = true;
    flags &= FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH;
    if (flags & FGPU_CLEAR_COLOR) pass->clear_color = color;
    if (flags & FGPU_CLEAR_DEPTH) pass->clear_depth = depth;
    pass->clear_flags |= flags;
}

void fgpu_pass_draw_layout(fgpu_pass_t* pass, const fgpu_target_t* t, const fgpu_pipeline_t* p,
                           const fgpu_layout_t* layout, const float* vertices, uint32_t vertex_count,
                           const uint32_t* indices, uint32_t count, uint32_t flags) {
    if (!t || !p || !vertices || p->vp_w <= 0 || p->vp_h <= 0 || !fgpu_layout_valid(layout)) return;
    if (p->shader && !p->constants) return;
    if (g_mode == FGPU_RASTER_IMMEDIATE || !pass) {
        if (pass) pass->drawn += old_draw_layout(t, p, layout, vertices, vertex_count, indices, count);
        else old_draw_layout(t, p, layout, vertices, vertex_count, indices, count);
        return;
    }
    uint32_t tris = count / 3;
    if (!tris) return;
    bool feedback = false;
    if (p->shader)
        for (uint32_t u = 0; u < FGPU_MAX_TEXTURE_UNITS; u++)
            if (p->textures[u].pixels && (p->textures[u].pixels == t->color)) feedback = true;
    if ((p->state & FGPU_STATE_WIREFRAME) || feedback) {
        // Lines, and draws that read what they write, run in submission order.
        pass_flush_impl(pass);
        pass->drawn += old_draw_layout(t, p, layout, vertices, vertex_count, indices, count);
        return;
    }
    if (pass->have_target && !same_target(&pass->t, t)) pass_flush_impl(pass);
    uint32_t nitems = (tris + ITEM_TRIS - 1) / ITEM_TRIS;
    if (pass->ndraws >= MAX_DRAWS || pass->items + nitems > MAX_ITEMS || pass->items + nitems < pass->items)
        pass_flush_impl(pass);

    for (int attempt = 0; attempt < 2; attempt++) {
        bool ok = true;
        // State snapshot (deduplicated against the recent ones).
        bstate_t* c = pass->cand;
        __builtin_memset(c, 0, sizeof(*c));
        c->pipe = *p;
        c->pipe.constants = NULL;
        if (p->shader) __builtin_memcpy(c->constants, p->constants, sizeof(c->constants));
        c->layout.stride = layout->stride;
        c->layout.count = layout->count;
        for (uint32_t i = 0; i < layout->count; i++) c->layout.attrib[i] = layout->attrib[i];
        int64_t cx0 = p->vp_x < 0 ? 0 : p->vp_x, cy0 = p->vp_y < 0 ? 0 : p->vp_y;
        int64_t cx1 = (int64_t)p->vp_x + p->vp_w, cy1 = (int64_t)p->vp_y + p->vp_h;
        if (cx1 > (int64_t)t->width) cx1 = t->width;
        if (cy1 > (int64_t)t->height) cy1 = t->height;
        if (cx0 > cx1) cx0 = cx1;
        if (cy0 > cy1) cy0 = cy1;
        c->cx0 = (int32_t)cx0; c->cy0 = (int32_t)cy0; c->cx1 = (int32_t)cx1; c->cy1 = (int32_t)cy1;
        c->mode = mode_of(p);
        c->nv = nv_of(p);
        int32_t found = -1;
        for (uint32_t k = 0; k < STATE_LOOKBACK && k < pass->nstates; k++) {
            uint32_t i = pass->nstates - 1 - k;
            if (!__builtin_memcmp(pass->states[i], c, sizeof(*c))) { found = (int32_t)i; break; }
        }
        if (found < 0) {
            if (pass->nstates == pass->cap_states) {
                void* n = grow(pass->states, pass->nstates, &pass->cap_states, sizeof(void*));
                if (!n) ok = false; else pass->states = n;
            }
            bstate_t* ns = ok ? mem_get(sizeof(bstate_t)) : NULL;
            if (!ns) ok = false;
            else {
                __builtin_memcpy(ns, c, sizeof(*c));
                pass->states[pass->nstates] = ns;
                found = (int32_t)pass->nstates++;
            }
        }
        if (ok && pass->ndraws == pass->cap_draws) {
            void* n = grow(pass->draws, pass->ndraws, &pass->cap_draws, sizeof(bdraw_t));
            if (!n) ok = false; else pass->draws = n;
        }
        const float* vsrc = vertices;
        const uint32_t* isrc = indices;
        if (ok && (flags & FGPU_DRAW_COPY)) {
            if (pass->nowned + 2 > pass->cap_owned) {
                void* n = grow(pass->owned, pass->nowned, &pass->cap_owned, sizeof(void*));
                if (!n) ok = false; else pass->owned = n;
            }
            if (ok) {
                float* vc = mem_get((size_t)vertex_count * layout->stride * sizeof(float) + 16);
                uint32_t* ic = indices ? mem_get((size_t)count * sizeof(uint32_t) + 16) : NULL;
                if (!vc || (indices && !ic)) { mem_put(vc); mem_put(ic); ok = false; }
                else {
                    __builtin_memcpy(vc, vertices, (size_t)vertex_count * layout->stride * sizeof(float));
                    pass->owned[pass->nowned++] = vc;
                    vsrc = vc;
                    if (ic) {
                        __builtin_memcpy(ic, indices, (size_t)count * sizeof(uint32_t));
                        pass->owned[pass->nowned++] = ic;
                        isrc = ic;
                    }
                }
            }
        }
        if (ok) {
            bdraw_t* d = &pass->draws[pass->ndraws++];
            d->state = (uint32_t)found;
            d->vertices = vsrc;
            d->vertex_count = vertex_count;
            d->indices = isrc;
            d->tris = tris;
            d->item_base = pass->items;
            pass->items += nitems;
            if (c->nv > pass->nv_max) pass->nv_max = c->nv;
            pass->t = *t;
            pass->have_target = true;
            return;
        }
        // Out of memory for the record: render what is pending and retry; then draw at once.
        if (attempt == 0) { pass_flush_impl(pass); pass->t = *t; pass->have_target = true; }
    }
    pass->drawn += old_draw_layout(t, p, layout, vertices, vertex_count, indices, count);
}

/* ---- entry points ---- */

static fgpu_pass_t* g_default_pass;

static fgpu_pass_t* default_pass(void) {
    if (!g_default_pass) g_default_pass = fgpu_pass_create();
    return g_default_pass;
}

void fgpu_raster_clear(const fgpu_target_t* t, uint32_t flags, uint32_t color, float depth) {
    fgpu_pass_t* pass = g_mode == FGPU_RASTER_IMMEDIATE ? NULL : default_pass();
    if (!pass) { if (t && t->height) old_clear(t, flags, color, depth); return; }
    fgpu_pass_clear(pass, t, flags, color, depth);
    fgpu_pass_flush(pass);
}

uint32_t fgpu_raster_draw_layout(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                                 const fgpu_layout_t* layout,
                                 const float* vertices, uint32_t vertex_count,
                                 const uint32_t* indices, uint32_t count) {
    fgpu_pass_t* pass = g_mode == FGPU_RASTER_IMMEDIATE ? NULL : default_pass();
    if (!pass) {
        if (!t || !p || !vertices || p->vp_w <= 0 || p->vp_h <= 0 || !fgpu_layout_valid(layout)) return 0;
        return old_draw_layout(t, p, layout, vertices, vertex_count, indices, count);
    }
    fgpu_pass_draw_layout(pass, t, p, layout, vertices, vertex_count, indices, count, 0);
    return fgpu_pass_flush(pass);
}

uint32_t fgpu_raster_draw(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                          const float* vertices, uint32_t vertex_count,
                          const uint32_t* indices, uint32_t count) {
    fgpu_layout_t layout;
    fgpu_layout_default(&layout);
    return fgpu_raster_draw_layout(t, p, &layout, vertices, vertex_count, indices, count);
}
