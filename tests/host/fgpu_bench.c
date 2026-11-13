// Host benchmark for the FGPU CPU rasterizer: the immediate path (previous
// rasterizer) against the binned path, on one thread and on N host threads
// standing in for the kernel's workers. Build and run: make bench-fgpu
//   FGPU_BENCH_THREADS=n   threads for the parallel columns (default 4)
//   FGPU_BENCH_REPS=n      repetitions, the best one is reported (default 5)
// Not part of test-host.
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_raster.h"
#include "fglsl.h"

/* ------------------------------------------------ shader time (linker wrap) */

// src/intf has its own time.h, which hides the system one: declare what is used.
int clock_gettime(int clock, struct timespec* ts);
#define CLOCK_MONOTONIC 1

static __thread uint64_t t_shader_ns;
static uint64_t g_shader_ns_all;
uint32_t __real_fgpu_shader_run_batch(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                                      uint32_t lanes, const float (*uniforms)[4], const fgpu_texture_t* textures);
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint32_t __wrap_fgpu_shader_run_batch(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                                      uint32_t lanes, const float (*uniforms)[4], const fgpu_texture_t* textures) {
    uint64_t t0 = now_ns();
    uint32_t r = __real_fgpu_shader_run_batch(p, fragment, b, lanes, uniforms, textures);
    t_shader_ns += now_ns() - t0;
    return r;
}

/* ------------------------------------------------------- pthread helper */

static int g_threads = 1;
static uint32_t par_workers(void) { return (uint32_t)g_threads; }
typedef struct { fgpu_part_fn fn; void* ctx; uint32_t parts, next, worker; } job_t;
static void* par_main(void* arg) {
    job_t* j = arg;
    uint32_t w = __atomic_fetch_add(&j->worker, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        uint32_t p = __atomic_fetch_add(&j->next, 1, __ATOMIC_SEQ_CST);
        if (p >= j->parts) break;
        j->fn(j->ctx, p, w);
    }
    __atomic_fetch_add(&g_shader_ns_all, t_shader_ns, __ATOMIC_SEQ_CST);
    t_shader_ns = 0;
    return NULL;
}
// A persistent pool: threads spin on a generation counter (a kernel worker waits for an IPI,
// so the wake-up cost is not what is measured here; the barrier cost is).
#define MAXT 16
static pthread_t g_pool[MAXT];
static job_t* volatile g_job;
static volatile uint32_t g_gen, g_done, g_quit;
static void* pool_main(void* arg) {
    (void)arg;
    uint32_t seen = 0;
    for (;;) {
        unsigned spins = 0;
        while (__atomic_load_n(&g_gen, __ATOMIC_ACQUIRE) == seen) {
            if (__atomic_load_n(&g_quit, __ATOMIC_RELAXED)) return NULL;
            __builtin_ia32_pause();
            if (++spins > 20000) { sched_yield(); spins = 0; }
        }
        seen = g_gen;
        par_main(g_job);
        __atomic_fetch_add(&g_done, 1, __ATOMIC_SEQ_CST);
    }
}
static void par_run(fgpu_part_fn fn, void* ctx, uint32_t parts) {
    job_t j = { fn, ctx, parts, 0, 1 };
    g_job = &j;
    __atomic_store_n(&g_done, 0, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_gen, 1, __ATOMIC_RELEASE);
    for (;;) {                      // the caller is worker 0
        uint32_t p = __atomic_fetch_add(&j.next, 1, __ATOMIC_SEQ_CST);
        if (p >= parts) break;
        fn(ctx, p, 0);
    }
    while (__atomic_load_n(&g_done, __ATOMIC_SEQ_CST) < (uint32_t)(g_threads - 1)) __builtin_ia32_pause();
}
static const fgpu_parallel_t k_par = { par_workers, par_run };
static void set_threads(int n) {
    g_threads = n;
    fgpu_raster_set_parallel(n > 1 ? &k_par : NULL);
}

/* ------------------------------------------------------------ the scenes */

#define W 640
#define H 480
#define STRIDE 10

typedef struct {
    const char* name;
    uint32_t draws;             // draw calls
    uint32_t tris_per_draw;
    float* verts;               // all draws (draws * tris_per_draw * 3 vertices)
    double pixels;              // pixels covered (estimated), per frame
    uint32_t state;
} scene_t;

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static float rndf(void) { return (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

static void put(float* o, float sx, float sy, float z, const float* c) {
    o[0] = sx / (W / 2.0f) - 1.0f; o[1] = 1.0f - sy / (H / 2.0f); o[2] = z;
    o[3] = 0; o[4] = 0; o[5] = 1;
    o[6] = c[0]; o[7] = c[1]; o[8] = c[2]; o[9] = 1.0f;
}

static scene_t make_small_draws(void) {
    scene_t s = { "2000 draws x 12 triangles", 2000, 12, NULL, 0, FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE };
    s.verts = malloc((size_t)s.draws * 36 * STRIDE * 4);
    for (uint32_t d = 0; d < s.draws; d++) {
        float cx = rndf() * W, cy = rndf() * H, z = rndf(), sz = 6 + rndf() * 14;
        float col[3] = { rndf(), rndf(), rndf() };
        float* o = s.verts + (size_t)d * 36 * STRIDE;
        for (int t = 0; t < 12; t++) {
            float a = (float)t * 0.52f;
            put(o, cx, cy, z, col); o += STRIDE;
            put(o, cx + sz * __builtin_cosf(a), cy + sz * __builtin_sinf(a), z, col); o += STRIDE;
            put(o, cx + sz * __builtin_cosf(a + 0.55f), cy + sz * __builtin_sinf(a + 0.55f), z, col); o += STRIDE;
            s.pixels += 0.5 * sz * sz * 0.55;
        }
    }
    return s;
}

static scene_t make_huge(void) {
    scene_t s = { "8 huge triangles (fill rate)", 1, 8, NULL, 0, FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE };
    s.verts = malloc(8 * 3 * STRIDE * 4);
    for (int t = 0; t < 4; t++) {                 // 4 full-screen quads = 8 triangles, near to far
        float z = 0.1f + 0.2f * (float)t, c[3] = { 0.2f * (float)t, 0.5f, 0.9f - 0.2f * (float)t };
        float* o = s.verts + (size_t)t * 6 * STRIDE;
        put(o, 0, 0, z, c); put(o + STRIDE, W, 0, z, c); put(o + 2 * STRIDE, W, H, z, c);
        put(o + 3 * STRIDE, 0, 0, z, c); put(o + 4 * STRIDE, W, H, z, c); put(o + 5 * STRIDE, 0, H, z, c);
    }
    s.pixels = 4.0 * W * H;
    return s;
}

static scene_t make_dense(void) {
    scene_t s = { "200k small triangles (~1.2x1.2 px)", 1, 200000, NULL, 0, FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE };
    s.verts = malloc((size_t)200000 * 3 * STRIDE * 4);
    float cell = 1.55f;
    int nx = (int)(W / cell), tris = 0;
    float* o = s.verts;
    for (int j = 0; tris < 200000; j++)
        for (int i = 0; i < nx && tris < 200000; i++) {
            float x = (float)i * cell, y = (float)(j % (int)(H / cell)) * cell, z = 0.2f + 0.5f * (float)(j / (int)(H / cell)) * 0.1f;
            float c[3] = { rndf(), rndf(), rndf() };
            put(o, x, y, z, c); put(o + STRIDE, x + cell, y, z, c); put(o + 2 * STRIDE, x + cell, y + cell, z, c);
            o += 3 * STRIDE; tris++;
            if (tris < 200000) {
                put(o, x, y, z, c); put(o + STRIDE, x + cell, y + cell, z, c); put(o + 2 * STRIDE, x, y + cell, z, c);
                o += 3 * STRIDE; tris++;
            }
        }
    s.pixels = 200000.0 * cell * cell / 2;
    return s;
}

static scene_t make_overdraw(int front_to_back) {
    scene_t s = { front_to_back ? "overdraw x32, front to back (hi-Z)" : "overdraw x32, back to front",
                  1, 64, NULL, 0, FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE };
    s.verts = malloc(64 * 3 * STRIDE * 4);
    for (int l = 0; l < 32; l++) {
        float z = front_to_back ? 0.05f + 0.9f * (float)l / 32 : 0.95f - 0.9f * (float)l / 32;
        float c[3] = { rndf(), rndf(), rndf() };
        float* o = s.verts + (size_t)l * 6 * STRIDE;
        put(o, 0, 0, z, c); put(o + STRIDE, W, 0, z, c); put(o + 2 * STRIDE, W, H, z, c);
        put(o + 3 * STRIDE, 0, 0, z, c); put(o + 4 * STRIDE, W, H, z, c); put(o + 5 * STRIDE, 0, H, z, c);
    }
    s.pixels = 32.0 * W * H;
    return s;
}

/* -------------------------------------------------------------- running */

static fglsl_program_t g_comp;
static fgpu_program_t g_prog;
static float g_consts[FGPU_SHADER_MAX_UNIFORMS][4];

static void mat_identity(float m[16]) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }

static void setup_pipe(fgpu_pipeline_t* p, uint32_t state, int use_shader) {
    memset(p, 0, sizeof(*p));
    p->vp_w = W; p->vp_h = H;
    p->state = state;
    p->program = FGPU_PROGRAM_COLOR;
    mat_identity(p->u.mvp); mat_identity(p->u.model); mat_identity(p->u.normal);
    if (use_shader) {
        p->shader = &g_prog;
        p->constants = (const float (*)[4])g_consts;
        memcpy(g_consts[g_comp.uniforms[fglsl_find_uniform(&g_comp, "mvp")].reg], p->u.mvp, 64);
    }
}

static uint32_t* g_color;
static float* g_depth;

// One frame: clear, then the scene's draws.
static uint32_t frame(const scene_t* s, int use_shader, fgpu_pass_t* pass) {
    fgpu_target_t t = { g_color, W, g_depth, W, W, H };
    fgpu_pipeline_t p;
    setup_pipe(&p, s->state, use_shader);
    fgpu_layout_t layout;
    fgpu_layout_default(&layout);
    uint32_t tris = 0;
    if (pass) fgpu_pass_clear(pass, &t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x202020, 1.0f);
    else fgpu_raster_clear(&t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x202020, 1.0f);
    for (uint32_t d = 0; d < s->draws; d++) {
        const float* v = s->verts + (size_t)d * s->tris_per_draw * 3 * STRIDE;
        uint32_t n = s->tris_per_draw * 3;
        if (pass) fgpu_pass_draw_layout(pass, &t, &p, &layout, v, n, NULL, n, 0);
        else tris += fgpu_raster_draw_layout(&t, &p, &layout, v, n, NULL, n);
    }
    if (pass) tris += fgpu_pass_flush(pass);
    return tris;
}

typedef struct { double ms; double shader_ms; uint32_t tris; } result_t;

static result_t run(const scene_t* s, int use_shader, fgpu_raster_mode_t mode, int threads, int reps) {
    set_threads(threads);
    fgpu_raster_set_mode(mode);
    fgpu_pass_t* pass = mode == FGPU_RASTER_BINNED ? fgpu_pass_create() : NULL;
    result_t best = { 1e30, 0, 0 };
    for (int r = -1; r < reps; r++) {            // the first run only warms up
        g_shader_ns_all = 0;
        t_shader_ns = 0;
        uint64_t t0 = now_ns();
        uint32_t tris = frame(s, use_shader, pass);
        double ms = (double)(now_ns() - t0) / 1e6;
        g_shader_ns_all += t_shader_ns;
        t_shader_ns = 0;
        if (r >= 0 && ms < best.ms) { best.ms = ms; best.tris = tris; best.shader_ms = (double)g_shader_ns_all / 1e6; }
    }
    fgpu_pass_destroy(pass);
    fgpu_raster_set_mode(FGPU_RASTER_BINNED);
    return best;
}

int main(void) {
    int N = 4, reps = 5;
    const char* e = getenv("FGPU_BENCH_THREADS");
    if (e) N = atoi(e);
    e = getenv("FGPU_BENCH_REPS");
    if (e) reps = atoi(e);
    if (N < 2) N = 2;
    if (N > MAXT) N = MAXT;
    for (int i = 0; i < N - 1; i++) pthread_create(&g_pool[i], NULL, pool_main, NULL);

    static const char* vs =
        "uniform mat4 mvp;\nattribute vec3 position;\nattribute vec3 normal;\nattribute vec4 color;\n"
        "varying vec4 v_color;\nvoid main() { v_color = color; gl_Position = mvp * vec4(position, 1.0); }\n";
    static const char* fs = "varying vec4 v_color;\nvoid main() { gl_FragColor = v_color; }\n";
    if (fglsl_compile(&g_comp, vs, fs) != 0) { printf("shader: %s\n", g_comp.error); return 1; }
    const char* why;
    if (!fgpu_program_check(g_comp.words, g_comp.size, &g_prog, &why)) { printf("check: %s\n", why); return 1; }

    g_color = calloc((size_t)W * H, 4);
    g_depth = calloc((size_t)W * H, 4);
    scene_t scenes[] = { make_small_draws(), make_huge(), make_dense(), make_overdraw(1), make_overdraw(0) };
    int nscenes = (int)(sizeof(scenes) / sizeof(scenes[0]));

    printf("FGPU rasterizer benchmark, %dx%d target, best of %d, 1 and %d threads\n", W, H, reps, N);
    printf("| scene | program | path | 1 thread ms | %d threads ms | Mtri/s (1) | Mtri/s (%d) | Mpx/s (1) | Mpx/s (%d) | shader share (1) |\n", N, N, N);
    printf("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
    for (int i = 0; i < nscenes; i++) {
        for (int sh = 0; sh < 2; sh++) {
            result_t r[2][2];
            for (int m = 0; m < 2; m++) {
                fgpu_raster_mode_t mode = m ? FGPU_RASTER_BINNED : FGPU_RASTER_IMMEDIATE;
                r[m][0] = run(&scenes[i], sh, mode, 1, reps);
                r[m][1] = run(&scenes[i], sh, mode, N, reps);
            }
            double tris = (double)scenes[i].draws * scenes[i].tris_per_draw;
            for (int m = 0; m < 2; m++) {
                printf("| %s | %s | %s | %.2f | %.2f | %.2f | %.2f | %.0f | %.0f | %.0f%% |\n",
                       scenes[i].name, sh ? "FGSL color shader" : "built-in color", m ? "binned" : "immediate",
                       r[m][0].ms, r[m][1].ms, tris / r[m][0].ms / 1e3, tris / r[m][1].ms / 1e3,
                       scenes[i].pixels / r[m][0].ms / 1e3, scenes[i].pixels / r[m][1].ms / 1e3,
                       r[m][0].ms > 0 ? 100.0 * r[m][0].shader_ms / r[m][0].ms : 0.0);
            }
            if (scenes[i].draws > 1)
                printf("|  | per-draw overhead (1 thread / %d threads), us | immediate %.2f / %.2f | binned %.2f / %.2f |||||||\n",
                       N, r[0][0].ms * 1e3 / scenes[i].draws, r[0][1].ms * 1e3 / scenes[i].draws,
                       r[1][0].ms * 1e3 / scenes[i].draws, r[1][1].ms * 1e3 / scenes[i].draws);
        }
    }
    if (getenv("FGPU_BENCH_PHASES")) {
        // Where the binned path spends its time (one thread), cycles per frame.
        for (int i = 0; i < nscenes; i++) {
            set_threads(1);
            fgpu_pass_t* pass = fgpu_pass_create();
            frame(&scenes[i], 0, pass);
            fgpu_raster_reset_stats();
            frame(&scenes[i], 0, pass);
            fgpu_raster_stats_t st;
            fgpu_raster_get_stats(&st);
            printf("phases: %-40s geometry %.2f Mcycles, tiles %.2f Mcycles\n", scenes[i].name,
                   (double)st.geometry_cycles / 1e6, (double)st.tile_cycles / 1e6);
            fgpu_pass_destroy(pass);
        }
    }
    __atomic_store_n(&g_quit, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < N - 1; i++) pthread_join(g_pool[i], NULL);
    return 0;
}
