// Differential and property tests for the binned rasterizer (fgpu_raster.c).
// Build and run: make test-host   (FGPU_FUZZ_ITERS=n, FGPU_FUZZ_SEED=s to change the run)
//
// The binned path (default) is compared with the immediate path (the previous
// rasterizer, fgpu_raster_set_mode(FGPU_RASTER_IMMEDIATE)):
//   exact scenes   screen vertices on a coarse grid, orthographic: both paths
//                  evaluate every edge function exactly, so color, depth and
//                  the triangle count must match bit for bit.
//   general scenes perspective, near clipping, huge and off-screen triangles:
//                  the binned path snaps vertices to 1/256 pixel and evaluates
//                  edges exactly; the immediate path uses float edge functions.
//                  Pixels may differ only on the border of triangles; the number
//                  of differing pixels is bounded (see GENERAL_BOUND).
// Plus: watertight shared-edge meshes (every covered pixel touched exactly
// once, fill rule), snapshot semantics of recorded draws, worker-count
// independence, tiny bin budget and allocation failures.
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_raster.h"
#include "fglsl.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* ------------------------------------------------------------- utilities */

static uint64_t rng_state = 1;
static uint32_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 16);
}
static uint32_t rndn(uint32_t n) { return rnd() % n; }
static float rndf(void) { return (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static float rndr(float a, float b) { return a + (b - a) * rndf(); }

/* A pthread helper standing in for kernel workers. */
static int g_threads = 1;
static uint32_t par_workers(void) { return (uint32_t)g_threads; }
typedef struct { fgpu_part_fn fn; void* ctx; uint32_t parts; uint32_t next; uint32_t worker; } par_job_t;
static void* par_main(void* arg) {
    par_job_t* j = arg;
    uint32_t worker = __atomic_fetch_add(&j->worker, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        uint32_t part = __atomic_fetch_add(&j->next, 1, __ATOMIC_SEQ_CST);
        if (part >= j->parts) break;
        j->fn(j->ctx, part, worker);
    }
    return NULL;
}
static void par_run(fgpu_part_fn fn, void* ctx, uint32_t parts) {
    par_job_t j = { fn, ctx, parts, 0, 1 };
    pthread_t th[16];
    int n = g_threads - 1;
    for (int i = 0; i < n; i++) pthread_create(&th[i], NULL, par_main, &j);
    // The caller is worker 0.
    for (;;) {
        uint32_t part = __atomic_fetch_add(&j.next, 1, __ATOMIC_SEQ_CST);
        if (part >= parts) break;
        fn(ctx, part, 0);
    }
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
}
static const fgpu_parallel_t k_par = { par_workers, par_run };
static void set_threads(int n) {
    g_threads = n;
    fgpu_raster_set_parallel(n > 1 ? &k_par : NULL);
}

/* Allocator with failure injection. */
static int alloc_fail_every;     // 0: never; n: every n-th allocation fails
static unsigned alloc_count;
static void* test_alloc(size_t n) {
    if (alloc_fail_every && ++alloc_count % (unsigned)alloc_fail_every == 0) return NULL;
    return malloc(n);
}

static void mat_identity(float m[16]) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void mat_mul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    memcpy(out, r, sizeof(r));
}
static void mat_perspective(float m[16], float fovy, float aspect, float n, float f) {
    float t = 1.0f / tanf(fovy / 2);
    memset(m, 0, 64);
    m[0] = t / aspect; m[5] = t; m[10] = (f + n) / (n - f); m[11] = -1; m[14] = 2 * f * n / (n - f);
}
static void mat_rotate(float m[16], float angle, float x, float y, float z) {
    float c = cosf(angle), s = sinf(angle), l = sqrtf(x * x + y * y + z * z);
    x /= l; y /= l; z /= l;
    mat_identity(m);
    m[0] = x * x * (1 - c) + c;     m[4] = x * y * (1 - c) - z * s; m[8] = x * z * (1 - c) + y * s;
    m[1] = y * x * (1 - c) + z * s; m[5] = y * y * (1 - c) + c;     m[9] = y * z * (1 - c) - x * s;
    m[2] = x * z * (1 - c) - y * s; m[6] = y * z * (1 - c) + x * s; m[10] = z * z * (1 - c) + c;
}

/* --------------------------------------------------------------- shaders */

#define N_SHADERS 4
static fglsl_program_t g_comp[N_SHADERS];
static fgpu_program_t g_prog[N_SHADERS];
static int g_tint[N_SHADERS], g_params[N_SHADERS], g_mvp[N_SHADERS];

static const char* k_vs =
    "uniform mat4 mvp;\n"
    "attribute vec3 position;\nattribute vec3 normal;\nattribute vec4 color;\nattribute vec2 uv;\n"
    "varying vec4 v_color;\nvarying vec2 v_uv;\n"
    "void main() { v_color = color; v_uv = uv; gl_Position = mvp * vec4(position, 1.0); }\n";
static const char* k_fs[N_SHADERS] = {
    "uniform vec4 tint;\nvarying vec4 v_color;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = v_color * tint; }\n",
    "uniform vec4 params;\nvarying vec4 v_color;\nvarying vec2 v_uv;\n"
    "void main() { if (fract(v_uv.x * 7.0 + v_uv.y * 3.0) < params.x) discard;\n"
    "  gl_FragColor = vec4(v_color.rgb, v_color.a * params.y); }\n",
    "uniform vec4 params;\nvarying vec4 v_color;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = vec4(v_color.rgb * (0.4 + 0.6 * gl_FrontFacing),\n"
    "  fract(gl_FragCoord.x * 0.013 + gl_FragCoord.y * 0.007 + gl_FragCoord.z) * params.y + 0.2); }\n",
    "uniform sampler2D tex;\nuniform vec4 tint;\nvarying vec4 v_color;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture(tex, v_uv) * v_color * tint; }\n",
};
static uint32_t g_texels[16 * 16];
static fgpu_texture_t g_texture;

static void build_shaders(void) {
    for (int i = 0; i < N_SHADERS; i++) {
        if (fglsl_compile(&g_comp[i], k_vs, k_fs[i]) != 0) { printf("shader %d: %s\n", i, g_comp[i].error); exit(1); }
        const char* why;
        if (!fgpu_program_check(g_comp[i].words, g_comp[i].size, &g_prog[i], &why)) { printf("check %d: %s\n", i, why); exit(1); }
        g_mvp[i] = fglsl_find_uniform(&g_comp[i], "mvp");
        g_tint[i] = fglsl_find_uniform(&g_comp[i], "tint");
        g_params[i] = fglsl_find_uniform(&g_comp[i], "params");
    }
    for (int i = 0; i < 256; i++) g_texels[i] = 0xFF000000u | (rnd() & 0xFFFFFF);
    g_texture.pixels = g_texels; g_texture.width = 16; g_texture.height = 16; g_texture.pitch = 16;
}

/* ----------------------------------------------------------------- scenes */

#define MAXOPS 12
typedef struct {
    int is_clear;
    uint32_t cflags, ccolor;
    float cdepth;
    fgpu_pipeline_t p;
    float consts[FGPU_SHADER_MAX_UNIFORMS][4];
    fgpu_layout_t layout;
    float* verts;
    uint32_t nverts;
    uint32_t* idx;
    uint32_t count;
} op_t;

typedef struct {
    uint32_t w, h;
    int nops;
    op_t op[MAXOPS];
    int general;
    int hostile;
} scene_t;

#define STRIDE 12
static void scene_free(scene_t* s) {
    for (int i = 0; i < s->nops; i++) { free(s->op[i].verts); free(s->op[i].idx); }
    s->nops = 0;
}

typedef struct { float x, y, z; } v3_t;

static void put_vertex(float* o, v3_t pos, int alpha_mode) {
    o[0] = pos.x; o[1] = pos.y; o[2] = pos.z;
    float n[3] = { rndr(-1, 1), rndr(-1, 1), rndr(-1, 1) };
    memcpy(o + 3, n, 12);
    o[6] = rndf(); o[7] = rndf(); o[8] = rndf();
    o[9] = alpha_mode == 0 ? 1.0f : (alpha_mode == 1 ? rndf() : (rndn(3) ? 0.5f : 1.0f));
    o[10] = rndr(-0.2f, 1.2f); o[11] = rndr(-0.2f, 1.2f);
}

// A point on the quarter-pixel grid -> orthographic clip coordinates.
typedef struct { int32_t vpx, vpy, vpw, vph; } vp_t;
static float grid_q(void) { return (float)((int)rndn(2000) - 384) * 0.25f; }   // [-96, 404)
static v3_t from_screen(const vp_t* vp, float sx, float sy, float z) {
    v3_t v = { (sx - (float)vp->vpx) / (float)vp->vpw * 2.0f - 1.0f,
               1.0f - 2.0f * (sy - (float)vp->vpy) / (float)vp->vph, z };
    return v;
}
static float grid_z(void) { return (float)((int)rndn(2049) - 1024) / 1024.0f; }   // [-1, 1]

// Triangles for the exact scenes: all vertices on the 1/4 grid.
static void gen_exact_tris(const vp_t* vp, v3_t* out, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t kind = rndn(10);
        v3_t t[3];
        float zbase = grid_z();
        if (kind <= 2) {                         // random
            for (int k = 0; k < 3; k++) t[k] = from_screen(vp, grid_q(), grid_q(), grid_z());
        } else if (kind == 3) {                  // tiny: inside a 2x2 pixel box
            float bx = (float)((int)rndn(260) - 20), by = (float)((int)rndn(160) - 20);
            for (int k = 0; k < 3; k++)
                t[k] = from_screen(vp, bx + (float)rndn(9) * 0.25f, by + (float)rndn(9) * 0.25f, zbase);
        } else if (kind == 4) {                  // sliver
            float ax = grid_q(), ay = grid_q(), bx = ax + (float)(int)rndn(300) - 150, by = ay + (float)(int)rndn(200) - 100;
            t[0] = from_screen(vp, ax, ay, zbase);
            t[1] = from_screen(vp, bx, by, zbase);
            t[2] = from_screen(vp, (ax + bx) * 0.5f + (float)rndn(3) * 0.25f, (ay + by) * 0.5f, zbase);
        } else if (kind == 5) {                  // degenerate
            float ax = grid_q(), ay = grid_q();
            t[0] = from_screen(vp, ax, ay, zbase);
            t[1] = from_screen(vp, ax + 4.0f, ay + 2.0f, zbase);
            t[2] = rndn(2) ? t[1] : from_screen(vp, ax + 8.0f, ay + 4.0f, zbase);
        } else if (kind == 6) {                  // centers exactly on edges: integer + half coordinates
            for (int k = 0; k < 3; k++)
                t[k] = from_screen(vp, (float)((int)rndn(120) - 10) * 0.5f, (float)((int)rndn(90) - 10) * 0.5f, zbase);
        } else if (kind == 7 && i + 1 < n) {     // a quad (two triangles) with a shared diagonal, random depth
            float x0 = grid_q(), y0 = grid_q(), x1 = x0 + (float)rndn(200) * 0.25f + 1, y1 = y0 + (float)rndn(150) * 0.25f + 1;
            v3_t q[4] = { from_screen(vp, x0, y0, zbase), from_screen(vp, x1, y0, zbase),
                          from_screen(vp, x1, y1, zbase), from_screen(vp, x0, y1, zbase) };
            out[i++] = q[0]; out[i++] = q[1]; out[i++] = q[2];
            t[0] = q[0]; t[1] = q[2]; t[2] = q[3];
        } else if (kind == 8) {                  // large, covers whole tiles
            t[0] = from_screen(vp, -60.0f + (float)rndn(40), -40.0f + (float)rndn(40), zbase);
            t[1] = from_screen(vp, 330.0f + (float)rndn(40), -30.0f + (float)rndn(30), zbase);
            t[2] = from_screen(vp, 100.0f + (float)rndn(100), 260.0f + (float)rndn(40), zbase);
            if (rndn(2)) { v3_t x = t[1]; t[1] = t[2]; t[2] = x; }
        } else {                                 // tilted plane: depth differs per vertex
            for (int k = 0; k < 3; k++) t[k] = from_screen(vp, grid_q(), grid_q(), grid_z());
        }
        for (int k = 0; k < 3 && i < n; k++) out[i++] = t[k];
    }
}

// General scenes: triangles in world space, seen through a perspective camera.
static int g_hostile;
static void gen_general_tris(v3_t* out, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t kind = rndn(8);
        if (!g_hostile && (kind == 4 || kind == 5 || kind == 7)) kind = rndn(4);
        v3_t t[3];
        float s = kind == 6 ? 40.0f : (kind == 5 ? 0.05f : 3.0f);
        v3_t c = { rndr(-4, 4), rndr(-3, 3), rndr(-8, 3) };
        for (int k = 0; k < 3; k++) {
            t[k].x = c.x + rndr(-s, s); t[k].y = c.y + rndr(-s, s); t[k].z = c.z + rndr(-s, s);
            if (kind == 7) { t[k].z = rndr(-12, 9); }    // crosses the near plane and the camera
        }
        if (kind == 4) { t[2].x = t[1].x + (t[1].x - t[0].x) * 0.5f; t[2].y = t[1].y + (t[1].y - t[0].y) * 0.5f + 1e-3f; t[2].z = t[1].z; }
        for (int k = 0; k < 3 && i < n; k++) out[i++] = t[k];
    }
}

static void gen_scene(scene_t* sc, bool general) {
    static const uint32_t sizes[][2] = { { 200, 150 }, { 64, 64 }, { 130, 70 }, { 257, 65 }, { 300, 200 }, { 17, 100 }, { 128, 128 } };
    memset(sc, 0, sizeof(*sc));
    sc->general = general;
    sc->hostile = g_hostile;
    uint32_t si = rndn(7);
    sc->w = sizes[si][0]; sc->h = sizes[si][1];
    vp_t vp = { (int32_t)rndn(5) * 8 - 8, (int32_t)rndn(5) * 8 - 8, 256, 128 };
    if (rndn(2)) { vp.vpx = 0; vp.vpy = 0; }
    if (general) {
        vp.vpw = (int32_t)sc->w + (int32_t)rndn(40) - 20; vp.vph = (int32_t)sc->h + (int32_t)rndn(40) - 20;
        if (vp.vpw < 4) vp.vpw = 4;
        if (vp.vph < 4) vp.vph = 4;
    }
    float proj[16], view[16], model[16], mvp[16], tmp[16];
    mat_perspective(proj, rndr(0.6f, 1.6f), (float)vp.vpw / (float)vp.vph, 0.1f, 50.0f);
    mat_identity(view); view[14] = -rndr(2, 8);
    mat_rotate(model, rndr(0, 6.28f), rndf(), rndf() + 0.1f, rndf());
    mat_mul(view, model, tmp);
    mat_mul(proj, tmp, mvp);
    float ortho[16];
    mat_identity(ortho);

    int nops = 1 + (int)rndn(MAXOPS - 4);
    for (int o = 0; o < nops; o++) {
        op_t* op = &sc->op[sc->nops++];
        memset(op, 0, sizeof(*op));
        if (rndn(6) == 0) {
            op->is_clear = 1;
            op->cflags = rndn(4);
            if (!op->cflags && rndn(3)) op->cflags = 3;
            op->ccolor = rnd() & 0xFFFFFF;
            op->cdepth = rndn(4) ? 1.0f : rndr(0.0f, 1.0f);
            continue;
        }
        fgpu_pipeline_t* p = &op->p;
        p->vp_x = vp.vpx; p->vp_y = vp.vpy; p->vp_w = vp.vpw; p->vp_h = vp.vph;
        if (rndn(8) == 0) {
            if (general) { p->vp_x += (int32_t)rndn(30); p->vp_w = 20 + (int32_t)rndn(150); p->vp_h = 20 + (int32_t)rndn(90); }
            else { p->vp_x = vp.vpx + 8 * (int32_t)rndn(4); p->vp_w = rndn(2) ? 128 : 256; p->vp_h = rndn(2) ? 64 : 128; }
            vp.vpx = p->vp_x; vp.vpw = p->vp_w; vp.vph = p->vp_h;
        }
        uint32_t st = 0;
        if (rndn(4)) st |= FGPU_STATE_DEPTH_TEST;
        if (rndn(4)) st |= FGPU_STATE_DEPTH_WRITE;
        uint32_t cull = rndn(5);
        if (cull == 0) st |= FGPU_STATE_CULL_BACK; else if (cull == 1) st |= FGPU_STATE_CULL_FRONT;
        if (rndn(3) == 0) st |= FGPU_STATE_BLEND;
        p->state = st;
        int prog = (int)rndn(8);
        int alpha_mode = (st & FGPU_STATE_BLEND) ? 1 + (int)rndn(2) : 0;
        if (prog < 4) {
            int sh = prog;
            p->shader = &g_prog[sh];
            p->constants = (const float (*)[4])op->consts;
            memcpy(op->consts[g_comp[sh].uniforms[g_mvp[sh]].reg], general ? mvp : ortho, 64);
            if (g_tint[sh] >= 0) for (int k = 0; k < 4; k++) op->consts[g_comp[sh].uniforms[g_tint[sh]].reg][k] = rndr(0.5f, 1.2f);
            if (g_params[sh] >= 0) { float* q = op->consts[g_comp[sh].uniforms[g_params[sh]].reg]; q[0] = rndr(0.1f, 0.6f); q[1] = rndr(0.5f, 1.0f); q[2] = q[3] = 1; }
            p->textures[0] = g_texture;
        } else {
            p->program = (uint32_t)(prog - 3);      // COLOR=1, GOURAUD=2, PHONG=3 (checked below)
            if (p->program < FGPU_PROGRAM_COLOR) p->program = FGPU_PROGRAM_COLOR;
            memcpy(p->u.mvp, general ? mvp : ortho, 64);
            memcpy(p->u.model, general ? model : ortho, 64);
            memcpy(p->u.normal, general ? model : ortho, 64);
            for (int k = 0; k < 3; k++) { p->u.light_dir[k] = rndr(-1, 1); p->u.light_color[k] = rndr(0.5f, 1); p->u.ambient[k] = rndr(0, 0.3f); p->u.camera_pos[k] = rndr(-5, 5); }
            p->u.material[0] = rndr(0, 1); p->u.material[1] = (float)rndn(64);
        }
        fgpu_layout_default(&op->layout);
        op->layout.stride = STRIDE;
        op->layout.count = 4;
        op->layout.attrib[0] = FGPU_ATTRIB(0, 3);
        op->layout.attrib[1] = FGPU_ATTRIB(3, 3);
        op->layout.attrib[2] = FGPU_ATTRIB(6, 4);
        op->layout.attrib[3] = FGPU_ATTRIB(10, 2);

        uint32_t ntri = 1 + rndn(rndn(8) ? 24 : 300);
        v3_t* tris = malloc(ntri * 3 * sizeof(v3_t));
        if (general) gen_general_tris(tris, ntri * 3); else gen_exact_tris(&vp, tris, ntri * 3);
        uint32_t nverts = ntri * 3;
        op->verts = malloc((size_t)nverts * STRIDE * 4);
        for (uint32_t v = 0; v < nverts; v++) put_vertex(op->verts + (size_t)v * STRIDE, tris[v], alpha_mode);
        free(tris);
        op->nverts = nverts;
        op->count = nverts;
        if (rndn(3) == 0) {         // indexed, with a few bad indices
            op->idx = malloc(nverts * 4);
            for (uint32_t v = 0; v < nverts; v++) op->idx[v] = v;
            for (uint32_t k = 0; k < nverts / 6; k++) { uint32_t a = rndn(nverts), b = rndn(nverts); uint32_t t2 = op->idx[a]; op->idx[a] = op->idx[b]; op->idx[b] = t2; }
            if (rndn(4) == 0) op->idx[rndn(nverts)] = nverts + rndn(5);
            if (rndn(5) == 0) op->count = nverts - rndn(4);
        }
    }
}

/* ------------------------------------------------------------- rendering */

typedef struct { uint32_t* color; float* depth; uint32_t w, h; } fb_t;

static fb_t fb_new(uint32_t w, uint32_t h) {
    fb_t f = { calloc((size_t)w * h, 4), calloc((size_t)w * h, 4), w, h };
    return f;
}
static void fb_free(fb_t* f) { free(f->color); free(f->depth); }

// Draws the scene; returns the triangle count. `deferred`: through a pass, with
// random flush points and recorded-state scribbling.
static uint32_t render(const scene_t* sc, fb_t* fb, fgpu_raster_mode_t mode, bool deferred, uint32_t seed) {
    fgpu_raster_set_mode(mode);
    fgpu_target_t t = { fb->color, sc->w, fb->depth, sc->w, sc->w, sc->h };
    // Start from a defined picture so partial clears are compared too.
    for (uint32_t i = 0; i < sc->w * sc->h; i++) { fb->color[i] = 0xFF123456u; fb->depth[i] = 0.75f; }
    uint32_t tris = 0;
    uint64_t saved = rng_state;
    rng_state = seed | 1;
    fgpu_pass_t* pass = deferred ? fgpu_pass_create() : NULL;
    if (deferred && !pass) deferred = false;      // allocation failure: the direct entry points
    for (int i = 0; i < sc->nops; i++) {
        const op_t* op = &sc->op[i];
        if (op->is_clear) {
            if (deferred) fgpu_pass_clear(pass, &t, op->cflags, op->ccolor, op->cdepth);
            else fgpu_raster_clear(&t, op->cflags, op->ccolor, op->cdepth);
            continue;
        }
        if (!deferred) {
            tris += fgpu_raster_draw_layout(&t, &op->p, &op->layout, op->verts, op->nverts, op->idx, op->count);
            continue;
        }
        // Everything the draw depends on is handed over in scratch copies that are
        // destroyed right after the call.
        fgpu_pipeline_t p = op->p;
        float consts[FGPU_SHADER_MAX_UNIFORMS][4];
        memcpy(consts, op->consts, sizeof(consts));
        p.constants = (const float (*)[4])consts;
        fgpu_layout_t layout = op->layout;
        uint32_t flags = rndn(2) ? FGPU_DRAW_COPY : 0;
        float* v = op->verts;
        uint32_t* ix = op->idx;
        if (flags) {
            v = malloc((size_t)op->nverts * STRIDE * 4);
            memcpy(v, op->verts, (size_t)op->nverts * STRIDE * 4);
            if (ix) { ix = malloc(op->count * 4 + 4); memcpy(ix, op->idx, op->count * 4); }
        }
        fgpu_pass_draw_layout(pass, &t, &p, &layout, v, op->nverts, ix, op->count, flags);
        memset(&p, 0xFF, sizeof(p));
        memset(consts, 0xFF, sizeof(consts));
        memset(&layout, 0xFF, sizeof(layout));
        if (flags) {
            memset(v, 0xFF, (size_t)op->nverts * STRIDE * 4);
            free(v);
            if (ix) { memset(ix, 0xFF, op->count * 4); free(ix); }
        }
        if (rndn(5) == 0) tris += fgpu_pass_flush(pass);
    }
    if (deferred) { tris += fgpu_pass_flush(pass); fgpu_pass_destroy(pass); }
    rng_state = saved;
    fgpu_raster_set_mode(FGPU_RASTER_BINNED);
    return tris;
}

typedef struct { uint32_t color_diff, depth_diff, pixels; } diff_t;
static diff_t compare(const fb_t* a, const fb_t* b) {
    diff_t d = { 0, 0, a->w * a->h };
    for (uint32_t i = 0; i < a->w * a->h; i++) {
        d.color_diff += a->color[i] != b->color[i];
        d.depth_diff += memcmp(&a->depth[i], &b->depth[i], 4) != 0;
    }
    return d;
}

// Tolerant comparison for the general scenes: snapping vertices to 1/256 pixel
// moves interpolated values by a few parts in 1e5 (depth) and one color level.
static diff_t compare_tol(const fb_t* a, const fb_t* b) {
    diff_t d = { 0, 0, a->w * a->h };
    for (uint32_t i = 0; i < a->w * a->h; i++) {
        uint32_t x = a->color[i], y = b->color[i];
        int big = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            int dc = (int)((x >> sh) & 255) - (int)((y >> sh) & 255);
            if (dc > 3 || dc < -3) big = 1;
        }
        d.color_diff += big;
        float dz = a->depth[i] - b->depth[i];
        // |z| beyond 1e6 is the ill-conditioned interpolation of a sliver at the near
        // plane; there either value is meaningless.
        int garbage = !(a->depth[i] < 1e6f && a->depth[i] > -1e6f) || !(b->depth[i] < 1e6f && b->depth[i] > -1e6f);
        d.depth_diff += !garbage && !(dz <= 2e-3f && dz >= -2e-3f);
    }
    return d;
}

/* The general scenes: pixels that may differ because the paths round edges
 * differently. One pixel of a triangle border is touched by either path
 * depending on 1/256-pixel snapping; with random triangles of up to a few
 * hundred pixels that is a small share of the picture. */
#define GENERAL_BOUND 0.004     // differing pixels / pixels, per scene
// Hostile scenes add slivers, a triangle a few hundredths of a pixel wide, and
// triangles crossing the camera. Snapping a vertex by 1/512 pixel moves the
// plane of a sliver a lot (interpolated depth and color change along its length).
#define HOSTILE_BOUND 0.02

static void dump_diff(const scene_t* scp, const fb_t* ref, const fb_t* got, diff_t d) {
    const scene_t sc = *scp;
    if (!getenv("FGPU_FUZZ_DEBUG") || !(d.color_diff || d.depth_diff)) return;
    int shown = 0;
    for (uint32_t i = 0; i < sc.w * sc.h && shown < 4; i++)
        if ((ref->color[i] != got->color[i] || ref->depth[i] != got->depth[i]) &&
            (!scp->general || (((int)(ref->color[i] & 255) - (int)(got->color[i] & 255) > 3 || (int)(ref->color[i] & 255) - (int)(got->color[i] & 255) < -3 ||
              (int)((ref->color[i] >> 8) & 255) - (int)((got->color[i] >> 8) & 255) > 3 || (int)((ref->color[i] >> 8) & 255) - (int)((got->color[i] >> 8) & 255) < -3 ||
              (int)((ref->color[i] >> 16) & 255) - (int)((got->color[i] >> 16) & 255) > 3 || (int)((ref->color[i] >> 16) & 255) - (int)((got->color[i] >> 16) & 255) < -3) ||
              (fabsf(ref->depth[i] - got->depth[i]) > 2e-3f && fabsf(ref->depth[i]) < 1e6f && fabsf(got->depth[i]) < 1e6f)))) {
            printf("    px (%u,%u): color %08x vs %08x depth %.9g vs %.9g\n", i % sc.w, i / sc.w, ref->color[i], got->color[i], ref->depth[i], got->depth[i]);
            shown++;
        }
    for (int o = 0; o < sc.nops; o++) {
        const op_t* op = &sc.op[o];
        if (op->is_clear) printf("    op %d clear flags %u depth %g\n", o, op->cflags, op->cdepth);
        else printf("    op %d draw state %x shader %d prog %u tris %u idx %d vp %d,%d %dx%d\n", o, op->p.state, op->p.shader != NULL, op->p.program, op->count / 3, op->idx != NULL, op->p.vp_x, op->p.vp_y, op->p.vp_w, op->p.vp_h);
    }
}

static void fuzz(int iters, uint64_t seed) {
    uint32_t exact_scenes = 0, general_scenes = 0, hostile_scenes = 0, worst_ppm = 0, total_diff = 0, worst_hostile_ppm = 0;
    double total_px = 0;
    for (int it = 0; it < iters; it++) {
        bool general = (it % 3) == 2;
        g_hostile = general && ((it / 3) & 1);
        rng_state = seed * 0x9E3779B97F4A7C15ull + (uint64_t)it * 7919 + 1;
        scene_t sc;
        gen_scene(&sc, general);
        fb_t ref = fb_new(sc.w, sc.h), got = fb_new(sc.w, sc.h), got2 = fb_new(sc.w, sc.h);
        set_threads(1);
        uint32_t tref = render(&sc, &ref, FGPU_RASTER_IMMEDIATE, false, 0);
        fgpu_raster_reset_stats();
        uint32_t tnew = render(&sc, &got, FGPU_RASTER_BINNED, (it & 1) != 0, (uint32_t)it);
        if (getenv("FGPU_FUZZ_DEBUG")) { fgpu_raster_stats_t st; fgpu_raster_get_stats(&st); printf("scene %d stats: flushes %u rounds %u fallbacks %u binned %u small %u tiles %u (deferred %d)\n", it, st.flushes, st.rounds, st.fallbacks, st.tris_binned, st.small_tris, st.tiles_run, it & 1); }
        // The same scene on several workers must give exactly the same picture.
        set_threads(it % 4 == 1 ? 3 : 2);
        uint32_t tnew2 = render(&sc, &got2, FGPU_RASTER_BINNED, (it & 1) == 0, (uint32_t)it * 3);
        set_threads(1);
        diff_t dn = compare(&got, &got2);
        CHECK(dn.color_diff == 0 && dn.depth_diff == 0 && tnew == tnew2,
              "scene %d (seed %llu): binned result depends on worker count / record style (%u color, %u depth)",
              it, (unsigned long long)seed, dn.color_diff, dn.depth_diff);
        diff_t d = general ? compare_tol(&ref, &got) : compare(&ref, &got);
        if (!general) {
            exact_scenes++;
            CHECK(d.color_diff == 0 && d.depth_diff == 0,
                  "exact scene %d (seed %llu, %ux%u, %d ops): %u color and %u depth pixels differ",
                  it, (unsigned long long)seed, sc.w, sc.h, sc.nops, d.color_diff, d.depth_diff);
            CHECK(tref == tnew, "exact scene %d: triangle count %u vs %u", it, tref, tnew);
            dump_diff(&sc, &ref, &got, d);
        } else {
            general_scenes++;
            hostile_scenes += g_hostile;
            uint32_t bad = d.color_diff > d.depth_diff ? d.color_diff : d.depth_diff;
            uint32_t ppm = (uint32_t)((uint64_t)bad * 1000000 / d.pixels);
            if (g_hostile) { if (ppm > worst_hostile_ppm) worst_hostile_ppm = ppm; }
            else if (ppm > worst_ppm) worst_ppm = ppm;
            total_diff += bad;
            total_px += d.pixels;
            if ((double)bad > GENERAL_BOUND * d.pixels) dump_diff(&sc, &ref, &got, d);
            CHECK((double)bad <= (g_hostile ? HOSTILE_BOUND : GENERAL_BOUND) * d.pixels,
                  "general scene %d (seed %llu): %u of %u pixels differ (bound %.2f%%)",
                  it, (unsigned long long)seed, bad, d.pixels, GENERAL_BOUND * 100);
            CHECK(tref == tnew, "general scene %d: triangle count %u vs %u", it, tref, tnew);
        }
        fb_free(&ref); fb_free(&got); fb_free(&got2);
        scene_free(&sc);
    }
    printf("  fuzz: %d scenes: %u exact (bit-identical), %u general (%u hostile); pixels differing: worst %u ppm"
           " (hostile %u ppm), mean %.0f ppm\n",
           iters, exact_scenes, general_scenes, hostile_scenes, worst_ppm, worst_hostile_ppm,
           total_px ? total_diff * 1e6 / total_px : 0.0);
}

/* ---------------------------------------------------------- watertightness */

// A mesh with shared edges over a rectangle; each pixel whose center is in the
// rectangle (top-left rule) must be blended exactly once.
static void test_watertight(int rounds) {
    const uint32_t W = 160, H = 120;
    for (int round = 0; round < rounds; round++) {
        rng_state = 0xABCDEF + (uint64_t)round * 31337;
        // The rectangle in 1/256 pixel units; half the rounds put the edges on pixel
        // centers (half integers), where only the top-left rule decides.
        bool on_centers = round % 2 == 0;
        float L, T, R, B;
        if (on_centers) {
            L = (float)rndn(30) + 0.5f; T = (float)rndn(20) + 0.5f;
            R = L + 20 + (float)rndn(100); B = T + 15 + (float)rndn(80);
        } else {
            L = (float)rndn(30 * 256) / 256.0f; T = (float)rndn(20 * 256) / 256.0f;
            R = L + 20 + (float)rndn(100 * 256) / 256.0f; B = T + 15 + (float)rndn(80 * 256) / 256.0f;
        }
        int nx = 2 + (int)rndn(14), ny = 2 + (int)rndn(10);
        float* px = malloc((size_t)(nx + 1) * (ny + 1) * 8);
        float step = on_centers ? 0.5f : 1.0f / 256.0f;
        float mincell = (R - L) / (float)nx < (B - T) / (float)ny ? (R - L) / (float)nx : (B - T) / (float)ny;
        int jm = (int)(0.3f * mincell / step);
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                float x = L + (R - L) * (float)i / (float)nx, y = T + (B - T) * (float)j / (float)ny;
                if (i > 0 && i < nx && j > 0 && j < ny) {   // interior vertices move freely (1/256 grid or half-integers)
                    x += step * (float)((int)rndn((uint32_t)(2 * jm + 1)) - jm);
                    y += step * (float)((int)rndn((uint32_t)(2 * jm + 1)) - jm);
                }
                px[(j * (nx + 1) + i) * 2] = x; px[(j * (nx + 1) + i) * 2 + 1] = y;
            }
        vp_t vp = { 0, 0, 256, 128 };
        uint32_t ntri = (uint32_t)nx * ny * 2;
        float* verts = malloc((size_t)ntri * 3 * 10 * 4);
        uint32_t n = 0;
        int flipped = 0;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                const float* a = &px[(j * (nx + 1) + i) * 2];
                const float* b = &px[(j * (nx + 1) + i + 1) * 2];
                const float* c = &px[((j + 1) * (nx + 1) + i + 1) * 2];
                const float* d = &px[((j + 1) * (nx + 1) + i) * 2];
                const float* tri[2][3] = { { a, b, c }, { a, c, d } };
                if ((i + j) & 1) { tri[0][0] = a; tri[0][1] = b; tri[0][2] = d; tri[1][0] = b; tri[1][1] = c; tri[1][2] = d; }
                for (int k = 0; k < 2; k++) {
                    float ar = (tri[k][1][0] - tri[k][0][0]) * (tri[k][2][1] - tri[k][0][1]) -
                               (tri[k][1][1] - tri[k][0][1]) * (tri[k][2][0] - tri[k][0][0]);
                    if (ar <= 0) flipped = 1;
                    for (int q = 0; q < 3; q++) {
                        float* o = verts + (size_t)n * 10;
                        v3_t v = from_screen(&vp, tri[k][q][0], tri[k][q][1], 0.5f);
                        o[0] = v.x; o[1] = v.y; o[2] = v.z;
                        o[3] = 0; o[4] = 0; o[5] = 1;
                        o[6] = o[7] = o[8] = 1.0f; o[9] = 0.5f;
                        n++;
                    }
                }
            }
        CHECK(!flipped, "watertight mesh generator produced a flipped cell");
        if (flipped) { free(px); free(verts); continue; }
        uint32_t* color = calloc((size_t)W * H, 4);
        fgpu_target_t t = { color, W, NULL, 0, W, H };
        fgpu_pipeline_t p;
        memset(&p, 0, sizeof(p));
        p.vp_w = (int32_t)vp.vpw; p.vp_h = (int32_t)vp.vph;
        p.vp_w = 256; p.vp_h = 128;
        p.state = FGPU_STATE_BLEND;
        p.program = FGPU_PROGRAM_COLOR;
        mat_identity(p.u.mvp); mat_identity(p.u.model); mat_identity(p.u.normal);
        for (int mode = 0; mode < 2; mode++) {
            for (uint32_t i = 0; i < W * H; i++) color[i] = 0xFF000000u;
            fgpu_raster_set_mode(mode ? FGPU_RASTER_IMMEDIATE : FGPU_RASTER_BINNED);
            set_threads(mode ? 1 : 1 + round % 3);
            fgpu_layout_t layout;
            fgpu_layout_default(&layout);
            fgpu_raster_draw_layout(&t, &p, &layout, verts, n, NULL, n);
            uint32_t bad_once = 0, bad_outside = 0, expect = 0;
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
                    bool inside = cx >= L && cx < R && cy >= T && cy < B;
                    // white at alpha 1/2 over black: one touch = 128, two = 191, none = 0
                    uint32_t v = color[y * W + x] & 0xFF;
                    expect += inside;
                    if (inside && v != 128) bad_once++;
                    if (!inside && v != 0) bad_outside++;
                }
            if (mode == 0) {
                CHECK(bad_once == 0 && bad_outside == 0,
                      "watertight round %d (%s, %dx%d cells): %u covered pixels not touched exactly once, %u outside touched (of %u)",
                      round, on_centers ? "edges on pixel centers" : "1/256 grid", nx, ny, bad_once, bad_outside, expect);
            }
        }
        fgpu_raster_set_mode(FGPU_RASTER_BINNED);
        set_threads(1);
        free(px); free(verts); free(color);
    }
    printf("  watertight: %d shared-edge meshes (fill rule, no gaps, no double hits)\n", rounds);
}

/* --------------------------------------------------------- budget, failures */

static void test_resources(void) {
    // Dense scene: 30000 small triangles, so a 1 MB budget needs several rounds.
    rng_state = 99;
    scene_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.w = 200; sc.h = 150;
    vp_t vp = { 0, 0, 256, 128 };
    op_t* op = &sc.op[sc.nops++];
    op->p.vp_w = 256; op->p.vp_h = 128;
    op->p.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE | FGPU_STATE_BLEND;
    op->p.program = FGPU_PROGRAM_COLOR;
    mat_identity(op->p.u.mvp); mat_identity(op->p.u.model); mat_identity(op->p.u.normal);
    fgpu_layout_default(&op->layout);
    uint32_t ntri = 30000;
    v3_t* tris = malloc(ntri * 3 * sizeof(v3_t));
    for (uint32_t i = 0; i < ntri; i++) {
        float bx = (float)rndn(200), by = (float)rndn(128), z = grid_z();
        for (int k = 0; k < 3; k++) tris[i * 3 + k] = from_screen(&vp, bx + (float)rndn(13) * 0.25f, by + (float)rndn(13) * 0.25f, z);
    }
    op->verts = malloc((size_t)ntri * 3 * STRIDE * 4);
    op->nverts = ntri * 3;
    op->count = ntri * 3;
    op->layout.stride = STRIDE;
    for (uint32_t v = 0; v < ntri * 3; v++) put_vertex(op->verts + (size_t)v * STRIDE, tris[v], 1);
    free(tris);
    // The layout must read the 12-float vertices.
    op->layout.stride = STRIDE; op->layout.count = 3;
    op->layout.attrib[0] = FGPU_ATTRIB(0, 3); op->layout.attrib[1] = FGPU_ATTRIB(3, 3); op->layout.attrib[2] = FGPU_ATTRIB(6, 4);

    fb_t ref = fb_new(sc.w, sc.h), got = fb_new(sc.w, sc.h);
    uint32_t tref = render(&sc, &ref, FGPU_RASTER_IMMEDIATE, false, 0);

    fgpu_raster_set_bin_budget(1u << 20);
    fgpu_raster_reset_stats();
    uint32_t tnew = render(&sc, &got, FGPU_RASTER_BINNED, true, 5);
    fgpu_raster_stats_t st;
    fgpu_raster_get_stats(&st);
    diff_t d = compare(&ref, &got);
    CHECK(d.color_diff == 0 && d.depth_diff == 0 && tref == tnew,
          "1 MB budget: %u color %u depth pixels differ (tris %u vs %u)", d.color_diff, d.depth_diff, tref, tnew);
    CHECK(st.rounds > 1, "a 1 MB budget needs several rounds for 30000 triangles (rounds=%u)", st.rounds);
    CHECK(st.bin_bytes_peak <= (2u << 20), "bin memory stayed near the budget (%llu bytes)", (unsigned long long)st.bin_bytes_peak);
    printf("  budget: 30000 triangles in %u rounds, bins peak %llu KB\n", st.rounds, (unsigned long long)st.bin_bytes_peak / 1024);

    // Allocation failures: the result must not change, the draw must not fail.
    for (int every = 2; every <= 7; every += 5) {
        fgpu_raster_set_allocator(test_alloc, free);
        alloc_fail_every = every;
        alloc_count = 0;
        fgpu_raster_reset_stats();
        for (int th = 1; th <= 2; th++) {
            set_threads(th);
            uint32_t t3 = render(&sc, &got, FGPU_RASTER_BINNED, true, 77);
            diff_t e = compare(&ref, &got);
            CHECK(e.color_diff == 0 && e.depth_diff == 0 && t3 == tref,
                  "allocation failing every %d: %u color %u depth pixels differ (tris %u vs %u)",
                  every, e.color_diff, e.depth_diff, t3, tref);
        }
        alloc_fail_every = 0;
        fgpu_raster_get_stats(&st);
        printf("  alloc failure every %d: identical (%u items drawn by the immediate path)\n", every, st.fallbacks);
    }
    fgpu_raster_set_allocator(NULL, NULL);
    set_threads(1);
    fgpu_raster_set_bin_budget(8u << 20);
    fb_free(&ref); fb_free(&got);
    scene_free(&sc);
}

/* ------------------------------------------------------------- pass rules */

static void test_pass_rules(void) {
    const uint32_t W = 96, H = 80;
    uint32_t* color = calloc(W * H, 4);
    float* depth = calloc(W * H, 4);
    fgpu_target_t t = { color, W, depth, W, W, H };
    fgpu_pass_t* pass = fgpu_pass_create();
    fgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.vp_w = (int32_t)W; p.vp_h = (int32_t)H;
    p.program = FGPU_PROGRAM_COLOR;
    p.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE;
    mat_identity(p.u.mvp); mat_identity(p.u.model); mat_identity(p.u.normal);
    float tri[30] = { -1,-1,0, 0,0,1, 1,0,0,1,  1,-1,0, 0,0,1, 1,0,0,1,  0,1,0, 0,0,1, 1,0,0,1 };
    fgpu_layout_t layout;
    fgpu_layout_default(&layout);

    fgpu_pass_clear(pass, &t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x102030, 1.0f);
    CHECK(fgpu_pass_pending(pass), "a clear is pending");
    CHECK(color[0] == 0, "nothing is written before the flush");
    fgpu_pass_draw_layout(pass, &t, &p, &layout, tri, 3, NULL, 3, FGPU_DRAW_COPY);
    memset(tri, 0, sizeof(tri));     // the caller reuses its memory
    p.state = 0;                      // and its state
    CHECK(fgpu_pass_flush(pass) == 1, "one triangle drawn");
    CHECK(!fgpu_pass_pending(pass), "nothing pending after the flush");
    CHECK(color[0] == 0xFF102030 && color[(H / 2) * W + W / 2] == 0xFFFF0000, "recorded clear and draw rendered");
    CHECK(depth[(H / 2) * W + W / 2] < 1.0f && depth[0] == 1.0f, "depth written from the snapshot state");

    // A clear after draws flushes them first; a partial clear keeps the other plane.
    float tri2[30] = { -1,-1,0.2f, 0,0,1, 0,1,0,1,  1,-1,0.2f, 0,0,1, 0,1,0,1,  0,1,0.2f, 0,0,1, 0,1,0,1 };
    p.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE;
    fgpu_pass_draw_layout(pass, &t, &p, &layout, tri2, 3, NULL, 3, 0);
    fgpu_pass_clear(pass, &t, FGPU_CLEAR_COLOR, 0x0000FF, 1.0f);
    fgpu_pass_flush(pass);
    CHECK(color[(H / 2) * W + W / 2] == 0xFF0000FF && depth[(H / 2) * W + W / 2] < 0.6f, "color-only clear keeps depth");

    // Switching the target renders the old pass first.
    uint32_t* color2 = calloc(W * H, 4);
    fgpu_target_t t2 = { color2, W, NULL, 0, W, H };
    p.state = 0;
    fgpu_pass_draw_layout(pass, &t, &p, &layout, tri2, 3, NULL, 3, 0);
    fgpu_pass_draw_layout(pass, &t2, &p, &layout, tri2, 3, NULL, 3, 0);
    CHECK(color[(H / 2) * W + W / 2] == 0xFF00FF00, "target switch flushed the first target");
    CHECK(color2[(H / 2) * W + W / 2] == 0, "second target still pending");
    fgpu_pass_flush(pass);
    CHECK(color2[(H / 2) * W + W / 2] == 0xFF00FF00, "second target rendered by the flush");
    // Discard drops pending work.
    fgpu_pass_draw_layout(pass, &t2, &p, &layout, tri, 3, NULL, 3, 0);
    fgpu_pass_discard(pass);
    CHECK(!fgpu_pass_pending(pass), "discard empties the pass");
    fgpu_pass_destroy(pass);
    free(color); free(color2); free(depth);
    printf("  pass: record/flush rules\n");
}

int main(int argc, char** argv) {
    int iters = 600;
    uint64_t seed = 1;
    const char* e = getenv("FGPU_FUZZ_ITERS");
    if (e) iters = atoi(e);
    e = getenv("FGPU_FUZZ_SEED");
    if (e) seed = strtoull(e, NULL, 10);
    (void)argc; (void)argv;
    build_shaders();
    printf("fgpu binning tests (seed %llu)\n", (unsigned long long)seed);
    test_pass_rules();
    test_watertight(24);
    test_resources();
    fuzz(iters, seed);
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("fgpu binning tests passed\n");
    return 0;
}
