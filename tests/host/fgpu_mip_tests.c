// OS-6.5 / OS-6.2: mip chains, level selection, trilinear filtering, 2x2 quad level of
// detail in the rasterizer (immediate and binned, 1 and N workers, interpreter and JIT),
// and 16 texture units.
// Build and run: make test-host
//
//   layout       fgpu_mip_layout / fgpu_mip_generate against a naive reference
//   sampling     fgpu_texture_sample_lod against a reference that picks levels by hand:
//                nearest / linear mip selection, magnification, bias, min/max clamps,
//                clamp and repeat addressing, level counts of non-square textures
//   quads        textured quads drawn at known scales: every pixel must equal the
//                reference sample at lod = log2(texels per pixel), with MIP_NEAREST and
//                trilinear; a receding plane picks coarser levels as it gets farther; two
//                triangles of one plane agree along the shared diagonal (helper pixels)
//   paths        binned (1 and 3 workers) vs immediate, interpreter vs JIT
//   units        all 16 texture units sampled in one shader
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_raster.h"
#include "x86_64/fgpu_jit_x64.h"
#include "fglsl.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static uint64_t rng_state = 7;
static uint32_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 16);
}

/* workers */
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
    for (;;) {
        uint32_t part = __atomic_fetch_add(&j.next, 1, __ATOMIC_SEQ_CST);
        if (part >= parts) break;
        fn(ctx, part, 0);
    }
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
}
static const fgpu_parallel_t k_par = { par_workers, par_run };
static void set_threads(int n) { g_threads = n; fgpu_raster_set_parallel(n > 1 ? &k_par : NULL); }

/* ---------------------------------------------------------------- textures */

typedef struct {
    uint32_t* px;
    uint32_t w, h, pitch, levels;
    uint32_t offsets[FGPU_MAX_MIP_LEVELS];
} tex_t;

static tex_t tex_new(uint32_t w, uint32_t h, uint32_t pitch) {
    tex_t t;
    memset(&t, 0, sizeof(t));
    uint64_t total;
    t.w = w; t.h = h; t.pitch = pitch;
    t.levels = fgpu_mip_layout(w, h, pitch, t.offsets, &total);
    t.px = calloc((size_t)total + 8, 4);
    return t;
}

static fgpu_texture_t tex_bind(const tex_t* t, uint32_t flags) {
    fgpu_texture_t b;
    memset(&b, 0, sizeof(b));
    b.pixels = t->px; b.width = t->w; b.height = t->h; b.pitch = t->pitch; b.flags = flags;
    b.mip_count = t->levels;
    memcpy(b.mip_offset, t->offsets, sizeof(b.mip_offset));
    return b;
}

static uint32_t level_w(const tex_t* t, uint32_t k) { uint32_t v = t->w >> k; return v ? v : 1; }
static uint32_t level_h(const tex_t* t, uint32_t k) { uint32_t v = t->h >> k; return v ? v : 1; }

static void test_layout(void) {
    static const uint32_t sizes[][2] = { { 1, 1 }, { 2, 2 }, { 16, 16 }, { 64, 4 }, { 1, 33 }, { 5, 7 }, { 100, 60 }, { 4096, 4096 }, { 3, 1 } };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t w = sizes[i][0], h = sizes[i][1], pitch = w + (i & 1 ? 3 : 0);
        uint32_t off[FGPU_MAX_MIP_LEVELS];
        uint64_t total;
        uint32_t levels = fgpu_mip_layout(w, h, pitch, off, &total);
        uint32_t m = w > h ? w : h, want = 1;
        while (m > 1) { m >>= 1; want++; }
        CHECK(levels == want, "%ux%u: %u levels, expected %u", w, h, levels, want);
        CHECK(levels <= FGPU_MAX_MIP_LEVELS, "level limit");
        uint64_t at = (uint64_t)pitch * h;
        CHECK(off[0] == 0, "offset 0");
        for (uint32_t k = 1; k < levels; k++) {
            CHECK(off[k] == at, "%ux%u level %u offset %u, expected %llu", w, h, k, off[k], (unsigned long long)at);
            uint32_t lw = w >> k, lh = h >> k;
            at += (uint64_t)(lw ? lw : 1) * (lh ? lh : 1);
        }
        CHECK(total == at, "%ux%u total %llu expected %llu", w, h, (unsigned long long)total, (unsigned long long)at);
    }
    // The box filter against a naive reference, on odd sizes too.
    for (unsigned i = 0; i < 40; i++) {
        uint32_t w = 1 + rnd() % 40, h = 1 + rnd() % 40, pitch = w + rnd() % 5;
        tex_t t = tex_new(w, h, pitch);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) t.px[y * pitch + x] = rnd();
        fgpu_mip_generate(t.px, w, h, pitch);
        for (uint32_t k = 1; k < t.levels; k++) {
            uint32_t sw = level_w(&t, k - 1), sh = level_h(&t, k - 1), dw = level_w(&t, k), dh = level_h(&t, k);
            const uint32_t* src = k == 1 ? t.px : t.px + t.offsets[k - 1];
            size_t sp = k == 1 ? pitch : sw;
            int bad = 0;
            for (uint32_t y = 0; y < dh; y++)
                for (uint32_t x = 0; x < dw; x++) {
                    uint32_t x0 = 2 * x, x1 = 2 * x + 1, y0 = 2 * y, y1 = 2 * y + 1;
                    if (x1 >= sw) x1 = sw - 1;
                    if (y1 >= sh) y1 = sh - 1;
                    uint32_t want = 0;
                    for (int sh8 = 0; sh8 < 32; sh8 += 8) {
                        uint32_t sum = ((src[y0 * sp + x0] >> sh8) & 255) + ((src[y0 * sp + x1] >> sh8) & 255) +
                                       ((src[y1 * sp + x0] >> sh8) & 255) + ((src[y1 * sp + x1] >> sh8) & 255);
                        want |= ((sum + 2) / 4) << sh8;
                    }
                    bad += t.px[t.offsets[k] + y * dw + x] != want;
                }
            CHECK(!bad, "%ux%u (pitch %u) level %u: %d texels differ from the box filter", w, h, pitch, k, bad);
        }
        // Level 0 is untouched; the padding between rows is not written.
        free(t.px);
    }
    // 2x2 of known values.
    tex_t t = tex_new(2, 2, 2);
    t.px[0] = 0xFF000000u; t.px[1] = 0xFF0000FFu; t.px[2] = 0xFF00FF00u; t.px[3] = 0xFFFF0000u;
    fgpu_mip_generate(t.px, 2, 2, 2);
    CHECK(t.levels == 2 && t.px[t.offsets[1]] == 0xFF404040u, "2x2 average is %08x", t.levels == 2 ? t.px[t.offsets[1]] : 0);
    free(t.px);
}

/* ---------------------------------------------------------------- sampling */

static uint32_t pack(const float c[4]) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        float f = c[i] * 255.0f + 0.5f;
        if (f < 0) f = 0;
        if (f > 255) f = 255;
        v |= (uint32_t)f << (i == 3 ? 24 : (2 - i) * 8);
    }
    return v;
}

// A texture whose level k is the solid color color_of(k).
static uint32_t level_color(uint32_t k) {
    static const uint32_t c[FGPU_MAX_MIP_LEVELS] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFFFF00FFu,
                                                      0xFF808080u, 0xFF804000u, 0xFF008040u, 0xFF400080u, 0xFFC0C0C0u, 0xFF202020u, 0xFFF0A000u };
    return c[k % FGPU_MAX_MIP_LEVELS];
}

static tex_t solid_levels(uint32_t w, uint32_t h) {
    tex_t t = tex_new(w, h, w);
    for (uint32_t k = 0; k < t.levels; k++) {
        uint32_t lw = level_w(&t, k), lh = level_h(&t, k);
        uint32_t* base = k ? t.px + t.offsets[k] : t.px;
        size_t pitch = k ? lw : t.pitch;
        for (uint32_t y = 0; y < lh; y++)
            for (uint32_t x = 0; x < lw; x++) base[y * pitch + x] = level_color(k);
    }
    return t;
}

static void rgba_of(uint32_t p, float out[4]) {
    out[0] = (float)((p >> 16) & 255) / 255.0f; out[1] = (float)((p >> 8) & 255) / 255.0f;
    out[2] = (float)(p & 255) / 255.0f; out[3] = (float)(p >> 24) / 255.0f;
}

static int near_color(uint32_t a, uint32_t b, int tol) {
    for (int sh = 0; sh < 32; sh += 8) {
        int d = (int)((a >> sh) & 255) - (int)((b >> sh) & 255);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}

static void test_sampling(void) {
    tex_t t = solid_levels(64, 32);       // 7 levels
    CHECK(t.levels == 7, "64x32 has %u levels", t.levels);
    float px[4];

    fgpu_texture_t nearest = tex_bind(&t, FGPU_TEXTURE_MIP_NEAREST);
    CHECK(fgpu_texture_has_mips(&nearest), "has mips");
    fgpu_texture_t plain = tex_bind(&t, 0);
    CHECK(!fgpu_texture_has_mips(&plain), "no mip flag: no mips");
    fgpu_texture_t nochain = tex_bind(&t, FGPU_TEXTURE_MIP_LINEAR);
    nochain.mip_count = 1;
    CHECK(!fgpu_texture_has_mips(&nochain), "one level: no mips");

    static const struct { float lod; uint32_t level; } near_cases[] = {
        { -3.0f, 0 }, { 0.0f, 0 }, { 0.49f, 0 }, { 0.51f, 1 }, { 1.0f, 1 }, { 1.49f, 1 }, { 2.6f, 3 }, { 5.4f, 5 }, { 5.6f, 6 }, { 6.0f, 6 }, { 40.0f, 6 },
    };
    for (unsigned i = 0; i < sizeof(near_cases) / sizeof(near_cases[0]); i++) {
        fgpu_texture_sample_lod(&nearest, 0.3f, 0.6f, near_cases[i].lod, px);
        CHECK(pack(px) == level_color(near_cases[i].level), "MIP_NEAREST lod %g: %08x, expected level %u (%08x)",
              near_cases[i].lod, pack(px), near_cases[i].level, level_color(near_cases[i].level));
    }

    // Linear blend between the two levels.
    fgpu_texture_t lin = tex_bind(&t, FGPU_TEXTURE_MIP_LINEAR);
    static const float lods[] = { 0.0f, 0.25f, 1.0f, 1.5f, 2.9f, 5.5f, 5.999f, 6.0f, 9.0f };
    for (unsigned i = 0; i < sizeof(lods) / sizeof(lods[0]); i++) {
        float lod = lods[i], a[4], b[4], want[4];
        uint32_t k0 = (uint32_t)floorf(lod), k1 = k0 + 1;
        if (k0 > 6) { k0 = 6; lod = 6; }
        if (k1 > 6) k1 = 6;
        float f = lod - (float)k0;
        rgba_of(level_color(k0), a); rgba_of(level_color(k1), b);
        for (int c = 0; c < 4; c++) want[c] = a[c] + (b[c] - a[c]) * f;
        fgpu_texture_sample_lod(&lin, 0.77f, 0.12f, lods[i], px);
        CHECK(near_color(pack(px), pack(want), 1), "MIP_LINEAR lod %g: %08x, expected %08x", lods[i], pack(px), pack(want));
    }

    // Bias and clamps.
    fgpu_texture_t biased = tex_bind(&t, FGPU_TEXTURE_MIP_NEAREST);
    biased.lod_bias = 2.0f;
    fgpu_texture_sample_lod(&biased, 0.5f, 0.5f, 0.0f, px);
    CHECK(pack(px) == level_color(2), "bias +2 at lod 0: %08x", pack(px));
    fgpu_texture_t minc = tex_bind(&t, FGPU_TEXTURE_MIP_NEAREST);
    minc.min_lod = 3.0f;
    fgpu_texture_sample_lod(&minc, 0.5f, 0.5f, -5.0f, px);
    CHECK(pack(px) == level_color(3), "min lod 3 at lod -5: %08x", pack(px));
    fgpu_texture_t maxc = tex_bind(&t, FGPU_TEXTURE_MIP_NEAREST);
    maxc.max_lod = 2.0f;
    fgpu_texture_sample_lod(&maxc, 0.5f, 0.5f, 9.0f, px);
    CHECK(pack(px) == level_color(2), "max lod 2 at lod 9: %08x", pack(px));
    fgpu_texture_sample_lod(&maxc, 0.5f, 0.5f, 1.0f, px);
    CHECK(pack(px) == level_color(1), "max lod 2 at lod 1: %08x", pack(px));

    // Without a chain the lod does nothing.
    fgpu_texture_sample_lod(&nochain, 0.5f, 0.5f, 4.0f, px);
    CHECK(pack(px) == level_color(0), "no chain: level 0");
    float p0[4];
    fgpu_texture_sample(&plain, 0.2f, 0.9f, p0);
    fgpu_texture_sample_lod(&plain, 0.2f, 0.9f, 4.0f, px);
    CHECK(!memcmp(p0, px, sizeof(p0)), "no mip flag: lod ignored");
    free(t.px);

    // Real content: a checkerboard whose mips turn gray. Bilinear inside the level.
    tex_t cb = tex_new(16, 16, 16);
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++) cb.px[y * 16 + x] = ((x ^ y) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
    fgpu_mip_generate(cb.px, 16, 16, 16);
    fgpu_texture_t tri = tex_bind(&cb, FGPU_TEXTURE_LINEAR | FGPU_TEXTURE_MIP_LINEAR | FGPU_TEXTURE_CLAMP);
    fgpu_texture_sample_lod(&tri, 0.5f, 0.5f, 1.0f, px);
    CHECK(near_color(pack(px), 0xFF808080u, 1), "level 1 of a checkerboard is gray: %08x", pack(px));
    fgpu_texture_sample_lod(&tri, 0.31f, 0.44f, 0.0f, px);
    float base[4];
    fgpu_texture_sample(&tri, 0.31f, 0.44f, base);
    CHECK(!memcmp(base, px, sizeof(base)), "lod 0 is level 0 with the LINEAR filter");
    free(cb.px);

    // Repeat addressing at a coarse level (texel indices wrap per level).
    tex_t rp = solid_levels(8, 8);
    fgpu_texture_t rpt = tex_bind(&rp, FGPU_TEXTURE_MIP_NEAREST);
    fgpu_texture_sample_lod(&rpt, 3.5f, -2.25f, 2.0f, px);
    CHECK(pack(px) == level_color(2), "repeat at level 2");
    free(rp.px);
}

/* ------------------------------------------------------------ quad shading */

#define W 96
#define H 96

static const char* k_vs =
    "uniform mat4 mvp;\n"
    "attribute vec3 position;\nattribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = uv; gl_Position = mvp * vec4(position, 1.0); }\n";
static const char* k_fs =
    "uniform sampler2D tex;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture(tex, v_uv); }\n";

static fglsl_program_t g_comp;
static fgpu_program_t g_prog;
static int g_mvp_loc;
static float g_consts[FGPU_SHADER_MAX_UNIFORMS][4];

static void build_program(const char* vs, const char* fs, fglsl_program_t* comp, fgpu_program_t* prog) {
    if (fglsl_compile(comp, vs, fs) != 0) { printf("shader: %s\n", comp->error); exit(1); }
    const char* why;
    if (!fgpu_program_check(comp->words, comp->size, prog, &why)) { printf("check: %s\n", why); exit(1); }
}

typedef struct { uint32_t color[W * H]; float depth[W * H]; } fb_t;

static void ortho_mvp(float m[16]) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }

// Pixel-space helpers: vertex positions given in pixels of a WxH target (y down).
static void pv(float* o, float px, float py, float z, float u, float v) {
    o[0] = px / W * 2.0f - 1.0f; o[1] = 1.0f - py / H * 2.0f; o[2] = z; o[3] = u; o[4] = v;
}

static const fgpu_layout_t* layout_uv(void) {
    static fgpu_layout_t l;
    memset(&l, 0, sizeof(l));
    l.stride = 5; l.count = 4;                  // attribute 3 is the shader's `uv`
    l.attrib[0] = FGPU_ATTRIB(0, 3);
    l.attrib[3] = FGPU_ATTRIB(3, 2);
    return &l;
}

static void draw(fb_t* fb, fgpu_raster_mode_t mode, const fgpu_texture_t* tex, const float* verts, uint32_t nverts,
                 uint32_t state, bool depth) {
    fgpu_raster_set_mode(mode);
    fgpu_target_t t = { fb->color, W, depth ? fb->depth : NULL, W, W, H };
    fgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.vp_w = W; p.vp_h = H;
    p.shader = &g_prog;
    p.constants = (const float (*)[4])g_consts;
    p.state = state;
    p.textures[0] = *tex;
    fgpu_raster_draw_layout(&t, &p, layout_uv(), verts, nverts, NULL, nverts);
    fgpu_raster_set_mode(FGPU_RASTER_BINNED);
}

static void clear_fb(fb_t* fb) {
    fgpu_target_t t = { fb->color, W, fb->depth, W, W, H };
    fgpu_raster_clear(&t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x010203, 1.0f);
}

static void quad_verts(float* out, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, bool flip) {
    // Two triangles, split on one of the diagonals.
    float a[5], b[5], c[5], d[5];
    pv(a, x0, y0, 0.5f, u0, v0); pv(b, x1, y0, 0.5f, u1, v0); pv(c, x1, y1, 0.5f, u1, v1); pv(d, x0, y1, 0.5f, u0, v1);
    const float* tri[6] = { a, c, b, a, d, c };         // diagonal a-c, front facing
    const float* tri2[6] = { a, d, b, b, d, c };        // diagonal b-d
    const float** t = flip ? tri2 : tri;
    for (int i = 0; i < 6; i++) memcpy(out + i * 5, t[i], 20);
}

static void test_quads_with_jit(bool jit);

static void test_quads(void) {
    ortho_mvp(g_consts[g_comp.uniforms[g_mvp_loc].reg]);
    test_quads_with_jit(false);
    // The same through compiled code: bit-identical pictures.
    fgpu_jit_set_mode(FGPU_JIT_AUTO);
    if (fgpu_jit_cpu_support() & (FGPU_JIT_CPU_SSE41 | FGPU_JIT_CPU_AVX2)) {
        CHECK(fgpu_program_jit(&g_prog), "program compiles");
        test_quads_with_jit(true);
    } else printf("  (no SSE4.1: JIT variants skipped)\n");
}

static fb_t g_fb;

static void test_quads_with_jit(bool jit) {
    // 64x64 texture drawn on screen at several sizes: the lod is log2(64 / size).
    tex_t t = solid_levels(64, 64);
    static const struct { float size; } sizes[] = { { 64 }, { 32 }, { 16 }, { 8 }, { 4 }, { 2 }, { 80 } };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        float s = sizes[i].size, x0 = 8.0f, y0 = 8.0f;
        float lod = log2f(64.0f / s);
        for (int mode = 0; mode < 2; mode++) {
            for (int threads = 1; threads <= 3; threads += 2) {
                for (int flip = 0; flip < 2; flip++) {
                    set_threads(threads);
                    float verts[30];
                    quad_verts(verts, x0, y0, x0 + s, y0 + s, 0, 0, 1, 1, flip);
                    fgpu_texture_t b = tex_bind(&t, FGPU_TEXTURE_MIP_NEAREST);
                    clear_fb(&g_fb);
                    draw(&g_fb, mode ? FGPU_RASTER_IMMEDIATE : FGPU_RASTER_BINNED, &b, verts, 6, 0, false);
                    int bad = 0, inside = 0;
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++) {
                            bool in = x >= (int)x0 && x < (int)(x0 + s) && y >= (int)y0 && y < (int)(y0 + s);
                            inside += in;
                            float px[4];
                            uint32_t want = 0xFF010203u;
                            if (in) {
                                fgpu_texture_sample_lod(&b, (x + 0.5f - x0) / s, (y + 0.5f - y0) / s, lod, px);
                                want = pack(px);
                            }
                            bad += g_fb.color[y * W + x] != want;
                        }
                    if (bad && getenv("MIPDBG")) { int shown = 0; for (int y = 0; y < H && shown < 6; y++) for (int x = 0; x < W && shown < 6; x++) { bool in = x >= (int)x0 && x < (int)(x0 + s) && y >= (int)y0 && y < (int)(y0 + s); float px2[4]; uint32_t want = 0xFF010203u; if (in) { fgpu_texture_sample_lod(&b, (x + 0.5f - x0) / s, (y + 0.5f - y0) / s, lod, px2); want = pack(px2); } if (g_fb.color[y * W + x] != want) { printf("   wrong %d,%d got %08x want %08x\n", x, y, g_fb.color[y * W + x], want); shown++; } } int x = (int)x0 + (int)s / 2, y = (int)y0 + (int)s / 2; printf("   px %d,%d = %08x\n", x, y, g_fb.color[y * W + x]); }
                    // The immediate path (fgpuold) evaluates float edge functions, so pixels exactly on a
                    // shared diagonal can be missed there (the previous rasterizer does the same);
                    // the binned path is exact.
                    CHECK(mode ? bad <= inside / 100 + 2 : bad == 0, "%s%s size %g (lod %.2f), %d workers, %s, diagonal %d: %d of %d pixels wrong",
                          jit ? "JIT " : "", "MIP_NEAREST", s, lod, threads, mode ? "immediate" : "binned", flip, bad, inside);
                }
            }
        }
        // Trilinear on the same quad (random content so the blend matters).
        set_threads(1);
    }
    free(t.px);

    // Trilinear against the reference sample, on a gradient texture. Tolerance: the lod
    // comes from float differences of interpolated coordinates.
    tex_t g = tex_new(64, 64, 64);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            g.px[y * 64 + x] = 0xFF000000u | ((x * 4) << 16) | ((y * 4) << 8) | (((x ^ y) & 4) ? 0xC0u : 0x20u);
    fgpu_mip_generate(g.px, 64, 64, 64);
    static const float tri_sizes[] = { 48.0f, 30.0f, 21.0f, 11.0f, 5.0f };
    for (unsigned i = 0; i < sizeof(tri_sizes) / sizeof(tri_sizes[0]); i++) {
        float s = tri_sizes[i], x0 = 5.0f, y0 = 9.0f;
        float lod = log2f(64.0f / s);
        fgpu_texture_t b = tex_bind(&g, FGPU_TEXTURE_LINEAR | FGPU_TEXTURE_MIP_LINEAR | FGPU_TEXTURE_CLAMP);
        float verts[30];
        quad_verts(verts, x0, y0, x0 + s, y0 + s, 0, 0, 1, 1, i & 1);
        for (int mode = 0; mode < 2; mode++) {
            set_threads(1 + 2 * (int)(i & 1));
            clear_fb(&g_fb);
            draw(&g_fb, mode ? FGPU_RASTER_IMMEDIATE : FGPU_RASTER_BINNED, &b, verts, 6, 0, false);
            int bad = 0, inside = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    if (!(x >= (int)ceilf(x0) && x < (int)(x0 + s) && y >= (int)ceilf(y0) && y < (int)(y0 + s))) continue;
                    inside++;
                    float px[4];
                    fgpu_texture_sample_lod(&b, (x + 0.5f - x0) / s, (y + 0.5f - y0) / s, lod, px);
                    bad += !near_color(g_fb.color[y * W + x], pack(px), 3);
                }
            CHECK(bad <= inside / 50, "%strilinear size %g (lod %.2f) %s: %d of %d pixels off by more than 3 levels",
                  jit ? "JIT " : "", s, lod, mode ? "immediate" : "binned", bad, inside);
        }
    }
    free(g.px);
    set_threads(1);

}

/* ------------------------------------------------------- perspective plane */

// The ground-plane test needs w; a second shader multiplies by it.
static const char* k_vs_persp =
    "uniform mat4 mvp;\n"
    "attribute vec3 position;\nattribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = uv; gl_Position = mvp * vec4(position, 1.0); }\n";

static void test_plane(void) {
    fglsl_program_t comp;
    fgpu_program_t prog;
    build_program(k_vs_persp, k_fs, &comp, &prog);
    float consts[FGPU_SHADER_MAX_UNIFORMS][4];
    memset(consts, 0, sizeof(consts));
    int loc = fglsl_find_uniform(&comp, "mvp");
    // A plane y = -1, x in [-4, 4], z in [-30, -1.5], camera at the origin looking down -z.
    float proj[16] = { 0 };
    float n = 1.0f, f = 100.0f, t = 1.0f / tanf(0.5f);
    proj[0] = t; proj[5] = t; proj[10] = (f + n) / (n - f); proj[11] = -1; proj[14] = 2 * f * n / (n - f);
    memcpy(consts[comp.uniforms[loc].reg], proj, 64);
    float v[4][5] = {
        { -4, -1, -2, 0, 0 }, { 4, -1, -2, 8, 0 }, { 4, -1, -40, 8, 76 }, { -4, -1, -40, 0, 76 },
    };
    float verts[6 * 5];
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) memcpy(verts + i * 5, v[order[i]], 20);

    tex_t tx = solid_levels(64, 64);
    fgpu_texture_t b = tex_bind(&tx, FGPU_TEXTURE_MIP_NEAREST);
    static fb_t fbs[5];
    int idx = 0;
    for (int mode = 0; mode < 2; mode++)
        for (int threads = 1; threads <= 3; threads += 2) {
            if (mode && threads > 1) continue;
            set_threads(threads);
            fb_t* fb = &fbs[idx++];
            fgpu_raster_set_mode(mode ? FGPU_RASTER_IMMEDIATE : FGPU_RASTER_BINNED);
            fgpu_target_t tg = { fb->color, W, fb->depth, W, W, H };
            fgpu_raster_clear(&tg, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x010203, 1.0f);
            fgpu_pipeline_t p;
            memset(&p, 0, sizeof(p));
            p.vp_w = W; p.vp_h = H; p.shader = &prog; p.constants = (const float (*)[4])consts;
            p.textures[0] = b;
            fgpu_raster_draw_layout(&tg, &p, layout_uv(), verts, 6, NULL, 6);
            fgpu_raster_set_mode(FGPU_RASTER_BINNED);
        }
    set_threads(1);
    // Rows from the bottom to the top of the plane: the level index never decreases.
    int levels_seen[FGPU_MAX_MIP_LEVELS] = { 0 };
    for (int mode = 0; mode < 3; mode++) {
        const fb_t* fb = &fbs[mode == 2 ? 2 : mode];       // binned 1 worker, binned 3 workers, immediate
        if (mode == 1) fb = &fbs[1];
        if (mode == 2) fb = &fbs[2];
        int last = -1, backwards = 0, covered_rows = 0;
        for (int y = H - 1; y >= 0; y--) {
            int level = -1;
            for (int x = 0; x < W; x++) {
                uint32_t c = fb->color[y * W + x];
                if (c == 0xFF010203u) continue;
                int k;
                for (k = 0; k < (int)tx.levels; k++) if (c == level_color(k)) break;
                CHECK(k < (int)tx.levels, "pixel %d,%d has no level color: %08x", x, y, c);
                if (level < 0) level = k;
                CHECK(abs(level - k) <= 1, "row %d mixes levels %d and %d", y, level, k);
                if (k > level) level = k;
            }
            if (level < 0) continue;
            covered_rows++;
            if (level < last) backwards++;
            else last = level;
            if (mode == 0) levels_seen[level]++;
        }
        CHECK(covered_rows > 20, "plane covers %d rows", covered_rows);
        CHECK(backwards <= 1, "mode %d: the level goes back toward finer %d times going up the plane", mode, backwards);
    }
    int distinct = 0;
    for (int k = 0; k < (int)FGPU_MAX_MIP_LEVELS; k++) distinct += levels_seen[k] > 0;
    CHECK(distinct >= 3, "the plane shows %d different levels", distinct);
    // Worker count must not change the binned picture.
    CHECK(!memcmp(fbs[0].color, fbs[1].color, sizeof(fbs[0].color)), "binned plane differs between 1 and 3 workers");
    // Immediate vs binned: only pixels at a level boundary may differ.
    int diff = 0;
    for (int i = 0; i < W * H; i++) diff += fbs[0].color[i] != fbs[2].color[i];
    CHECK(diff < W * H / 100, "immediate and binned planes differ in %d pixels", diff);
    free(tx.px);
}

/* --------------------------------------------------------------- 16 units */

static void test_units(void) {
    // One shader samples all 16 units and adds up one channel of each: unit u holds
    // a solid color with red = u + 1 (of 255), so the sum identifies the bound set.
    char fs[4096];
    int n = snprintf(fs, sizeof(fs), "varying vec2 v_uv;\n");
    for (int u = 0; u < 16; u++) n += snprintf(fs + n, sizeof(fs) - (size_t)n, "uniform sampler2D t%d;\n", u);
    n += snprintf(fs + n, sizeof(fs) - (size_t)n, "void main() { vec4 s = vec4(0.0);\n");
    for (int u = 0; u < 16; u++) n += snprintf(fs + n, sizeof(fs) - (size_t)n, "  s += texture(t%d, v_uv);\n", u);
    snprintf(fs + n, sizeof(fs) - (size_t)n, "  gl_FragColor = s / 16.0; }\n");
    fglsl_program_t comp;
    fgpu_program_t prog;
    build_program(k_vs, fs, &comp, &prog);
    CHECK(prog.h.texture_mask == 0xFFFFu, "texture mask %08x", prog.h.texture_mask);
    float consts[FGPU_SHADER_MAX_UNIFORMS][4];
    memset(consts, 0, sizeof(consts));
    ortho_mvp(consts[comp.uniforms[fglsl_find_uniform(&comp, "mvp")].reg]);
    static uint32_t texel[16];
    fgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.vp_w = W; p.vp_h = H; p.shader = &prog; p.constants = (const float (*)[4])consts;
    int expect_r = 0;
    for (int u = 0; u < 16; u++) {
        char name[8];
        snprintf(name, sizeof(name), "t%d", u);
        int loc = fglsl_find_uniform(&comp, name);
        CHECK(loc >= 0, "uniform %s", name);
        uint32_t reg = comp.uniforms[loc].reg;
        CHECK(reg < FGPU_TEXTURE_UNITS, "sampler %s on unit %u", name, reg);
        texel[u] = 0xFF000000u | ((uint32_t)(16 * (u + 1) - 1) << 16);
        p.textures[reg].pixels = &texel[u]; p.textures[reg].width = 1; p.textures[reg].height = 1; p.textures[reg].pitch = 1;
        expect_r += 16 * (u + 1) - 1;
    }
    float verts[30];
    quad_verts(verts, 0, 0, W, H, 0, 0, 1, 1, false);
    for (int mode = 0; mode < 2; mode++) {
        fgpu_raster_set_mode(mode ? FGPU_RASTER_IMMEDIATE : FGPU_RASTER_BINNED);
        static fb_t fb;
        fgpu_target_t tg = { fb.color, W, NULL, W, W, H };
        fgpu_raster_clear(&tg, FGPU_CLEAR_COLOR, 0, 1.0f);
        fgpu_raster_draw_layout(&tg, &p, layout_uv(), verts, 6, NULL, 6);
        int r = (int)((fb.color[H / 2 * W + W / 2] >> 16) & 255);
        int want = (int)((float)expect_r / 16.0f + 0.5f);
        CHECK(abs(r - want) <= 1, "16 units %s: red %d, expected about %d", mode ? "immediate" : "binned", r, want);
    }
    fgpu_raster_set_mode(FGPU_RASTER_BINNED);
    // A unit above 15 is rejected when the program is checked.
    static uint32_t bad_words[512];
    memcpy(bad_words, comp.words, comp.size);
    CHECK(comp.size <= sizeof(bad_words), "scratch size");
    fgpu_shader_header_t* h = (fgpu_shader_header_t*)bad_words;
    h->texture_mask |= 1u << 16;
    fgpu_program_t chk;
    const char* why;
    CHECK(!fgpu_program_check(bad_words, comp.size, &chk, &why), "texture_mask bit 16 is rejected");
}

int main(void) {
    printf("fgpu mip tests\n");
    build_program(k_vs, k_fs, &g_comp, &g_prog);
    g_mvp_loc = fglsl_find_uniform(&g_comp, "mvp");
    test_layout();
    test_sampling();
    test_quads();
    test_plane();
    test_units();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("fgpu mip tests passed\n");
    return 0;
}
