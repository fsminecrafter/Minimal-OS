#pragma GCC optimize("O2")
#include "x86_64/fgpu_raster.h"
#include <stddef.h>

#define VERTEX_FLOATS 10        // FGPU_VERTEX_P3N3C4
#define NEAR_EPS 1e-5f

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

/* ------------------------------------------------------------- varyings */

// Per-vertex data after the vertex stage.
// clip[4], world[3], normal[3], color[4]
#define VAR_COUNT 10            // world, normal, color
typedef struct {
    float clip[4];
    float var[VAR_COUNT];
} vtx_t;

// Screen-space vertex for the rasterizer.
typedef struct {
    float x, y, z, inv_w;
    float var[VAR_COUNT];       // pre-multiplied by inv_w
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

static void vertex_stage(const fgpu_pipeline_t* p, const float* in, vtx_t* out) {
    float pos[4] = { in[0], in[1], in[2], 1.0f };
    mat4_mul_vec(p->u.mvp, pos, out->clip);
    float world[4];
    mat4_mul_vec(p->u.model, pos, world);
    float n4[4] = { in[3], in[4], in[5], 0.0f };
    float wn[4];
    mat4_mul_vec(p->u.normal, n4, wn);

    out->var[0] = world[0]; out->var[1] = world[1]; out->var[2] = world[2];
    out->var[3] = wn[0]; out->var[4] = wn[1]; out->var[5] = wn[2];
    const float* base = in + 6;
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
    return (r << 16) | (g << 8) | b;
}

static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Top-left rule: pixels exactly on an edge belong to top or left edges.
static inline bool edge_owns(float dx, float dy) {
    return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
}

static void raster_triangle(const fgpu_target_t* t, const fgpu_pipeline_t* p,
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
            if (depth_write) drow[x] = z;
            if (!crow) continue;

            float iw = l0 * a->inv_w + l1 * b->inv_w + l2 * c->inv_w;
            float wpix = iw != 0.0f ? 1.0f / iw : 0.0f;
            float v[VAR_COUNT];
            for (int k = 0; k < VAR_COUNT; k++) {
                v[k] = (l0 * a->var[k] + l1 * b->var[k] + l2 * c->var[k]) * wpix;
            }
            float out[4];
            if (p->program == FGPU_PROGRAM_PHONG) {
                light(&p->u, v + 3, v, v + 6, out);
            } else {
                out[0] = v[6]; out[1] = v[7]; out[2] = v[8]; out[3] = v[9];
            }
            crow[x] = pack_color(out);
        }
        row0 += e0dy; row1 += e1dy; row2 += e2dy;
    }
}

static void plot_line(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                      const svtx_t* a, const svtx_t* b) {
    if (!t->color) return;
    float color[4];
    float iw = a->inv_w != 0.0f ? 1.0f / a->inv_w : 0.0f;
    for (int k = 0; k < 4; k++) color[k] = a->var[6 + k] * iw;
    uint32_t packed = pack_color(color);
    int32_t x0 = (int32_t)a->x, y0 = (int32_t)a->y, x1 = (int32_t)b->x, y1 = (int32_t)b->y;
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < 16384; guard++) {
        if (x0 >= p->vp_x && y0 >= p->vp_y && x0 < p->vp_x + p->vp_w && y0 < p->vp_y + p->vp_h &&
            x0 >= 0 && y0 >= 0 && x0 < (int32_t)t->width && y0 < (int32_t)t->height) {
            t->color[(size_t)y0 * t->color_pitch + x0] = packed;
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ------------------------------------------------------------- clipping */

static void lerp_vtx(const vtx_t* a, const vtx_t* b, float t, vtx_t* out) {
    for (int i = 0; i < 4; i++) out->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
    for (int i = 0; i < VAR_COUNT; i++) out->var[i] = a->var[i] + (b->var[i] - a->var[i]) * t;
}

// Keeps the part with z >= -w (in front of the near plane) and w > eps.
static int clip_near(const vtx_t* in, int n, vtx_t* out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const vtx_t* a = &in[i];
        const vtx_t* b = &in[(i + 1) % n];
        float da = a->clip[2] + a->clip[3];
        float db = b->clip[2] + b->clip[3];
        bool ina = da >= 0.0f && a->clip[3] > NEAR_EPS;
        bool inb = db >= 0.0f && b->clip[3] > NEAR_EPS;
        if (ina) out[m++] = *a;
        if (ina != inb) {
            float t = da / (da - db);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            lerp_vtx(a, b, t, &out[m]);
            if (out[m].clip[3] <= NEAR_EPS) out[m].clip[3] = NEAR_EPS * 2.0f;
            m++;
        }
    }
    return m;
}

static void to_screen(const fgpu_pipeline_t* p, const vtx_t* v, svtx_t* s) {
    float inv_w = 1.0f / v->clip[3];
    float nx = v->clip[0] * inv_w, ny = v->clip[1] * inv_w, nz = v->clip[2] * inv_w;
    // Keep projected points finite for the rasterizer.
    if (nx > 1e6f) nx = 1e6f;
    if (nx < -1e6f) nx = -1e6f;
    if (ny > 1e6f) ny = 1e6f;
    if (ny < -1e6f) ny = -1e6f;
    s->x = (float)p->vp_x + (nx * 0.5f + 0.5f) * (float)p->vp_w;
    s->y = (float)p->vp_y + (1.0f - (ny * 0.5f + 0.5f)) * (float)p->vp_h;
    s->z = nz * 0.5f + 0.5f;
    s->inv_w = inv_w;
    for (int i = 0; i < VAR_COUNT; i++) s->var[i] = v->var[i] * inv_w;
}

/* ---------------------------------------------------------------- entry */

void fgpu_raster_clear(const fgpu_target_t* t, uint32_t flags, uint32_t color, float depth) {
    if ((flags & FGPU_CLEAR_COLOR) && t->color) {
        for (uint32_t y = 0; y < t->height; y++) {
            uint32_t* row = t->color + (size_t)y * t->color_pitch;
            for (uint32_t x = 0; x < t->width; x++) row[x] = color & 0x00FFFFFFu;
        }
    }
    if ((flags & FGPU_CLEAR_DEPTH) && t->depth) {
        for (uint32_t y = 0; y < t->height; y++) {
            float* row = t->depth + (size_t)y * t->depth_pitch;
            for (uint32_t x = 0; x < t->width; x++) row[x] = depth;
        }
    }
}

uint32_t fgpu_raster_draw(const fgpu_target_t* t, const fgpu_pipeline_t* p,
                          const float* vertices, uint32_t vertex_count,
                          const uint32_t* indices, uint32_t count) {
    if (!t || !p || !vertices || p->vp_w <= 0 || p->vp_h <= 0) return 0;
    uint32_t drawn = 0;
    for (uint32_t i = 0; i + 2 < count; i += 3) {
        vtx_t tri[3];
        bool bad = false;
        for (int k = 0; k < 3; k++) {
            uint32_t index = indices ? indices[i + (uint32_t)k] : i + (uint32_t)k;
            if (index >= vertex_count) { bad = true; break; }
            vertex_stage(p, vertices + (size_t)index * VERTEX_FLOATS, &tri[k]);
        }
        if (bad) continue;

        vtx_t poly[4];
        int n = clip_near(tri, 3, poly);
        if (n < 3) continue;
        svtx_t s[4];
        for (int k = 0; k < n; k++) to_screen(p, &poly[k], &s[k]);

        if (p->state & FGPU_STATE_WIREFRAME) {
            for (int k = 0; k < n; k++) plot_line(t, p, &s[k], &s[(k + 1) % n]);
        } else {
            for (int k = 1; k + 1 < n; k++) raster_triangle(t, p, &s[0], &s[k], &s[k + 1]);
        }
        drawn++;
    }
    return drawn;
}
