// Host tests for vector .img images (version 2) in Minimal-OS-SDK/include/mosmedia.h:
// the rasterizer's coverage, fill rules, strokes, gradients, tinting, scaling, damaged
// files, version 1 compatibility, and (with the converter's path as argv[1]) the
// SVG -> .img -> PNG chain through tools/media/mosmedia.
// Build and run: make test-host (also built with -fsanitize=address,undefined)
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mosmedia.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)
#define PI 3.14159265358979323846

static uint32_t rng = 7;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// ------------------------------------------------------------ helpers

typedef struct { uint8_t* d; size_t n; } img_t;

static img_t finish(mimg_vwriter_t* w, uint8_t* buf) {
    img_t r = { buf, mimg_vw_finish(w) };
    return r;
}

// Renders with exactly the scratch the library asks for (so the sanitizers see an overrun),
// into a buffer of exactly width x height.
typedef struct { size_t allocs, frees; } alloc_stats_t;
static uint32_t* render(const img_t* im, uint32_t w, uint32_t h, const mimg_render_opts_t* o) {
    uint32_t* out = malloc((size_t)w * h * 4);
    mimg_render_opts_t opt;
    if (o) opt = *o; else mimg_render_opts_init(&opt);
    size_t want = mimg_scratch_size(im->d, im->n, w, h);
    void* scratch = malloc(want ? want : 1);
    opt.scratch = scratch; opt.scratch_size = want;
    bool ok = mimg_render_ex(im->d, im->n, out, w, h, w, &opt);
    free(scratch);
    if (!ok) { free(out); return NULL; }
    return out;
}

static double alpha_sum(const uint32_t* px, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)(px[i] >> 24) / 255.0;
    return s;
}

static void circle(mimg_vwriter_t* w, float cx, float cy, float r) {
    float k = 0.5522847498f * r;
    mimg_vw_move(w, cx + r, cy);
    mimg_vw_cubic(w, cx + r, cy + k, cx + k, cy + r, cx, cy + r);
    mimg_vw_cubic(w, cx - k, cy + r, cx - r, cy + k, cx - r, cy);
    mimg_vw_cubic(w, cx - r, cy - k, cx - k, cy - r, cx, cy - r);
    mimg_vw_cubic(w, cx + k, cy - r, cx + r, cy - k, cx + r, cy);
    mimg_vw_close(w);
}

static void rect(mimg_vwriter_t* w, float x0, float y0, float x1, float y1) {
    mimg_vw_move(w, x0, y0); mimg_vw_line(w, x1, y0); mimg_vw_line(w, x1, y1); mimg_vw_line(w, x0, y1); mimg_vw_close(w);
}

// One black fill shape; `build` draws its path.
typedef void (*build_fn)(mimg_vwriter_t*);
static img_t one_shape(uint8_t* buf, size_t cap, float vw, float vh, const mimg_vstyle_t* st, uint32_t argb,
                       build_fn build) {
    mimg_vwriter_t w;
    mimg_vw_begin(&w, buf, cap, mimg_vw_default_shift(vw, vh), 0, 0, vw, vh);
    int p = mimg_vw_paint_solid(&w, argb);
    mimg_vstyle_t s = *st;
    if (s.fill_paint == 0) s.fill_paint = p;
    if (s.stroke_paint == 0) s.stroke_paint = p;
    mimg_vw_shape_begin(&w, &s);
    build(&w);
    mimg_vw_shape_end(&w);
    return finish(&w, buf);
}

static uint8_t g_buf[1 << 16];

// ------------------------------------------------------------ coverage

static void b_half_rect(mimg_vwriter_t* w) { rect(w, 0, 0, 1.5f, 1); }
static void b_half_tri(mimg_vwriter_t* w) {
    mimg_vw_move(w, 0, 0); mimg_vw_line(w, 1, 0); mimg_vw_line(w, 0, 1); mimg_vw_close(w);
}
static void b_quarter_rect(mimg_vwriter_t* w) { rect(w, 0.25f, 0, 1, 0.5f); }

static void test_coverage(void) {
    mimg_vstyle_t fill = mimg_vstyle_fill(0);
    img_t im = one_shape(g_buf, sizeof g_buf, 4, 4, &fill, 0xFF000000u, b_half_rect);
    uint32_t* px = render(&im, 4, 4, NULL);
    CHECK(px != NULL, "renders");
    if (!px) return;
    CHECK((px[0] >> 24) == 255, "a covered pixel is opaque");
    uint32_t a = px[1] >> 24;
    CHECK(a >= 127 && a <= 128, "a half-covered pixel has alpha 127 or 128");
    CHECK((px[2] >> 24) == 0 && (px[4] >> 24) == 0, "pixels outside are clear");
    CHECK((px[1] & 0xFFFFFFu) == 0, "non-premultiplied black stays black");
    free(px);

    im = one_shape(g_buf, sizeof g_buf, 2, 2, &fill, 0xFFFF0000u, b_half_tri);
    px = render(&im, 2, 2, NULL);
    a = px[0] >> 24;
    CHECK(a >= 127 && a <= 128, "a triangle covering half a pixel diagonally: 0.5");
    CHECK((px[1] >> 24) == 0 && (px[2] >> 24) == 0 && (px[3] >> 24) == 0, "the other pixels are clear");
    CHECK((px[0] & 0xFFFFFFu) == 0xFF0000u, "color of a partial pixel");
    free(px);

    im = one_shape(g_buf, sizeof g_buf, 2, 2, &fill, 0xFF000000u, b_quarter_rect);
    px = render(&im, 2, 2, NULL);
    a = px[0] >> 24;
    CHECK(a >= 0.375 * 255 - 1 && a <= 0.375 * 255 + 1, "0.75 x 0.5 of a pixel: 0.375");
    free(px);
}

static float g_cx = 16, g_cy = 16, g_r = 10;
static void b_circle(mimg_vwriter_t* w) { circle(w, g_cx, g_cy, g_r); }

static void test_circle_area(void) {
    mimg_vstyle_t fill = mimg_vstyle_fill(0);
    double worst = 0;
    const float cases[][3] = { { 16, 16, 10 }, { 16.3f, 15.7f, 10 }, { 15.5f, 15.5f, 6.25f }, { 16, 16, 15 }, { 12.1f, 20.9f, 4.4f } };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        g_cx = cases[i][0]; g_cy = cases[i][1]; g_r = cases[i][2];
        img_t im = one_shape(g_buf, sizeof g_buf, 32, 32, &fill, 0xFF000000u, b_circle);
        for (uint32_t size = 32; size <= 128; size *= 2) {
            uint32_t* px = render(&im, size, size, NULL);
            double scale = size / 32.0, expect = PI * g_r * g_r * scale * scale;
            double got = alpha_sum(px, (size_t)size * size), err = fabs(got - expect) / expect;
            if (err > worst) worst = err;
            CHECK(err < 0.006, "circle area within 0.6%");
            free(px);
        }
    }
    printf("  circle area: worst error %.3f%%\n", worst * 100);
    // a circle that is exactly symmetric renders symmetrically
    g_cx = 16; g_cy = 16; g_r = 9.3f;
    img_t im = one_shape(g_buf, sizeof g_buf, 32, 32, &fill, 0xFF000000u, b_circle);
    uint32_t* px = render(&im, 32, 32, NULL);
    int sym = 1;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int d = (int)(px[y * 32 + x] >> 24) - (int)(px[y * 32 + 31 - x] >> 24);
            int e = (int)(px[y * 32 + x] >> 24) - (int)(px[(31 - y) * 32 + x] >> 24);
            if (d > 2 || d < -2 || e > 2 || e < -2) sym = 0;
        }
    CHECK(sym, "a centered circle is symmetric");
    free(px);
}

// ------------------------------------------------------------ fill rules

static double g_star_r = 12;
static void b_star(mimg_vwriter_t* w) {
    for (int i = 0; i < 5; i++) {
        double a = -PI / 2 + i * 4 * PI / 5;
        float x = (float)(16 + g_star_r * cos(a)), y = (float)(16 + g_star_r * sin(a));
        if (i == 0) mimg_vw_move(w, x, y); else mimg_vw_line(w, x, y);
    }
    mimg_vw_close(w);
}

static void test_fill_rules(void) {
    double R = g_star_r, r = R * cos(72 * PI / 180) / cos(36 * PI / 180);
    double star = 10 * 0.5 * R * r * sin(36 * PI / 180);          // the outline, center included
    double pent = 2.5 * r * r * sin(72 * PI / 180);               // the pentagon in the middle (winding 2)
    mimg_vstyle_t nz = mimg_vstyle_fill(0), eo = mimg_vstyle_fill(0);
    eo.even_odd = true;
    uint32_t S = 128;
    double k = S / 32.0;
    img_t a = one_shape(g_buf, sizeof g_buf, 32, 32, &nz, 0xFF000000u, b_star);
    uint32_t* pa = render(&a, S, S, NULL);
    static uint8_t buf2[1 << 14];
    img_t b = one_shape(buf2, sizeof buf2, 32, 32, &eo, 0xFF000000u, b_star);
    uint32_t* pb = render(&b, S, S, NULL);
    double sa = alpha_sum(pa, (size_t)S * S) / (k * k), sb = alpha_sum(pb, (size_t)S * S) / (k * k);
    CHECK(fabs(sa - star) / star < 0.01, "nonzero star area");
    CHECK(fabs(sb - (star - pent)) / (star - pent) < 0.01, "even-odd star area (the pentagon is a hole)");
    CHECK((pa[(S / 2) * S + S / 2] >> 24) == 255, "nonzero fills the middle");
    CHECK((pb[(S / 2) * S + S / 2] >> 24) == 0, "even-odd leaves the middle empty");
    // the tip of the top arm is filled by both
    uint32_t ty = (uint32_t)((16 - R * 0.8) * k), tx = S / 2;
    CHECK((pa[ty * S + tx] >> 24) == 255 && (pb[ty * S + tx] >> 24) == 255, "an arm is filled by both");
    free(pa); free(pb);

    // two nested squares wound the same way and opposite ways
    mimg_vwriter_t w;
    mimg_vw_begin(&w, g_buf, sizeof g_buf, 6, 0, 0, 16, 16);
    int p = mimg_vw_paint_solid(&w, 0xFF000000u);
    mimg_vstyle_t st = mimg_vstyle_fill(p);
    mimg_vw_shape_begin(&w, &st);
    rect(&w, 1, 1, 15, 15);
    mimg_vw_move(&w, 4, 4); mimg_vw_line(&w, 4, 12); mimg_vw_line(&w, 12, 12); mimg_vw_line(&w, 12, 4); mimg_vw_close(&w);
    mimg_vw_shape_end(&w);
    img_t c = finish(&w, g_buf);
    uint32_t* pc = render(&c, 16, 16, NULL);
    CHECK((pc[8 * 16 + 8] >> 24) == 0, "a hole wound the other way is a hole (nonzero)");
    CHECK((pc[2 * 16 + 2] >> 24) == 255, "the ring is filled");
    free(pc);
    mimg_vw_begin(&w, g_buf, sizeof g_buf, 6, 0, 0, 16, 16);
    p = mimg_vw_paint_solid(&w, 0xFF000000u);
    st = mimg_vstyle_fill(p);
    mimg_vw_shape_begin(&w, &st);
    rect(&w, 1, 1, 15, 15);
    rect(&w, 4, 4, 12, 12);      // same direction: nonzero fills it, even-odd does not
    mimg_vw_shape_end(&w);
    c = finish(&w, g_buf);
    pc = render(&c, 16, 16, NULL);
    CHECK((pc[8 * 16 + 8] >> 24) == 255, "nested, same direction: nonzero fills the middle");
    free(pc);
}

// ------------------------------------------------------------ strokes

typedef struct { float x, y; } pt_t;
static pt_t g_line[8];
static int g_nline;
static bool g_closed;
static void b_polyline(mimg_vwriter_t* w) {
    for (int i = 0; i < g_nline; i++) {
        if (i == 0) mimg_vw_move(w, g_line[i].x, g_line[i].y); else mimg_vw_line(w, g_line[i].x, g_line[i].y);
    }
    if (g_closed) mimg_vw_close(w);
}

static double stroke_area(const pt_t* pts, int n, bool closed, float width, uint32_t cap, uint32_t join, float miter,
                          uint32_t size, uint32_t** keep) {
    memcpy(g_line, pts, (size_t)n * sizeof *pts);
    g_nline = n; g_closed = closed;
    mimg_vstyle_t st = mimg_vstyle_stroke(0, width);
    st.cap = (uint8_t)cap; st.join = (uint8_t)join; st.miter_limit = miter;
    img_t im = one_shape(g_buf, sizeof g_buf, 32, 32, &st, 0xFF000000u, b_polyline);
    uint32_t* px = render(&im, size, size, NULL);
    double k = size / 32.0, a = alpha_sum(px, (size_t)size * size) / (k * k);
    if (keep) *keep = px; else free(px);
    return a;
}

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static void test_strokes(void) {
    const pt_t line[2] = { { 4, 16 }, { 28, 16 } };
    // a straight line of length 24, width 4
    CHECK(NEAR(stroke_area(line, 2, false, 4, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4, 64, NULL), 96, 0.3), "butt caps: area = length x width");
    CHECK(NEAR(stroke_area(line, 2, false, 4, MIMG_CAP_SQUARE, MIMG_JOIN_MITER, 4, 64, NULL), 96 + 16, 0.3), "square caps add half a width at both ends");
    CHECK(NEAR(stroke_area(line, 2, false, 4, MIMG_CAP_ROUND, MIMG_JOIN_MITER, 4, 64, NULL), 96 + PI * 4, 0.4), "round caps add a disc");
    // the same line at an angle: butt caps keep the area, and its edge pixels are exactly partial
    const pt_t diag[2] = { { 6, 6 }, { 24, 22 } };
    double len = sqrt(18.0 * 18 + 16.0 * 16);
    CHECK(NEAR(stroke_area(diag, 2, false, 3, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4, 96, NULL), len * 3, 0.4), "diagonal butt stroke area");
    CHECK(NEAR(stroke_area(diag, 2, false, 3, MIMG_CAP_ROUND, MIMG_JOIN_MITER, 4, 96, NULL), len * 3 + PI * 2.25, 0.5), "diagonal round-capped stroke area");
    // a right angle: two arms of 8 (centerline), width 4
    const pt_t L[3] = { { 4, 4 }, { 12, 4 }, { 12, 12 } };
    CHECK(NEAR(stroke_area(L, 3, false, 4, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4, 64, NULL), 64, 0.3), "miter join of a right angle");
    CHECK(NEAR(stroke_area(L, 3, false, 4, MIMG_CAP_BUTT, MIMG_JOIN_BEVEL, 4, 64, NULL), 62, 0.3), "bevel join of a right angle");
    CHECK(NEAR(stroke_area(L, 3, false, 4, MIMG_CAP_BUTT, MIMG_JOIN_ROUND, 4, 64, NULL), 60 + PI, 0.3), "round join of a right angle");
    // a closed square: miter makes it a square ring of outer 12 and inner 4
    const pt_t sq[4] = { { 8, 8 }, { 24, 8 }, { 24, 24 }, { 8, 24 } };
    CHECK(NEAR(stroke_area(sq, 4, true, 4, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4, 64, NULL), 20.0 * 20 - 12.0 * 12, 0.4), "closed square, miter");
    CHECK(NEAR(stroke_area(sq, 4, true, 4, MIMG_CAP_BUTT, MIMG_JOIN_BEVEL, 4, 64, NULL), 20.0 * 20 - 12.0 * 12 - 4 * 2, 0.4), "closed square, bevel");
    CHECK(NEAR(stroke_area(sq, 4, true, 4, MIMG_CAP_BUTT, MIMG_JOIN_ROUND, 4, 64, NULL), 20.0 * 20 - 12.0 * 12 - 4 * (4 - PI), 0.4), "closed square, round");
    // sharp spike: 2 x atan(1 / 6) wide; miter ratio 1 / sin(half) is about 6.1, beyond the limit of 4: bevel
    const pt_t spike[3] = { { 2, 11 }, { 28, 16 }, { 2, 21 } };
    double half = atan2(5.0, 26.0), ratio = 1 / sin(half);
    CHECK(ratio > 4 && ratio < 8, "spike test setup");
    double lim4 = stroke_area(spike, 3, false, 2, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4, 128, NULL);
    double bev = stroke_area(spike, 3, false, 2, MIMG_CAP_BUTT, MIMG_JOIN_BEVEL, 4, 128, NULL);
    double lim8 = stroke_area(spike, 3, false, 2, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 8, 128, NULL);
    CHECK(NEAR(lim4, bev, 0.05), "beyond the miter limit it is a bevel");
    CHECK(lim8 > bev + 0.5, "within a higher limit it is a miter");
    // a reversal (the line goes back on itself): round join is a half disc, others add nothing
    const pt_t back[3] = { { 4, 16 }, { 20, 16 }, { 8, 16 } };
    double r0 = stroke_area(back, 3, false, 4, MIMG_CAP_BUTT, MIMG_JOIN_ROUND, 4, 64, NULL);
    double m0 = stroke_area(back, 3, false, 4, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4, 64, NULL);
    CHECK(NEAR(m0, 16 * 4, 0.3), "a reversal under a miter join adds nothing");
    CHECK(NEAR(r0, 16 * 4 + PI * 2, 0.4), "a reversal under a round join adds a half disc");
    // zero length: round cap is a dot, square a square, butt nothing
    const pt_t dot[2] = { { 16, 16 }, { 16, 16 } };
    CHECK(NEAR(stroke_area(dot, 2, false, 6, MIMG_CAP_ROUND, MIMG_JOIN_MITER, 4, 64, NULL), PI * 9, 0.3), "zero-length, round cap: a dot");
    CHECK(NEAR(stroke_area(dot, 2, false, 6, MIMG_CAP_SQUARE, MIMG_JOIN_MITER, 4, 64, NULL), 36, 0.3), "zero-length, square cap: a square");
    CHECK(NEAR(stroke_area(dot, 2, false, 6, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4, 64, NULL), 0, 0.01), "zero-length, butt cap: nothing");

    // reference coverage for round joins and caps: a pixel is covered where the distance to the
    // polyline is at most half the width (16 x 16 samples a pixel)
    const pt_t zig[5] = { { 3, 24 }, { 10, 6 }, { 16, 22 }, { 23, 8 }, { 29, 20 } };
    uint32_t* px;
    stroke_area(zig, 5, false, 3.5f, MIMG_CAP_ROUND, MIMG_JOIN_ROUND, 4, 32, &px);
    double maxd = 0, sum = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int in = 0;
            for (int sy = 0; sy < 16; sy++)
                for (int sx = 0; sx < 16; sx++) {
                    double X = x + (sx + 0.5) / 16, Y = y + (sy + 0.5) / 16, best = 1e9;
                    for (int i = 0; i + 1 < 5; i++) {
                        double dx = zig[i + 1].x - zig[i].x, dy = zig[i + 1].y - zig[i].y;
                        double t = ((X - zig[i].x) * dx + (Y - zig[i].y) * dy) / (dx * dx + dy * dy);
                        t = t < 0 ? 0 : t > 1 ? 1 : t;
                        double ex = zig[i].x + t * dx - X, ey = zig[i].y + t * dy - Y;
                        double d = sqrt(ex * ex + ey * ey);
                        if (d < best) best = d;
                    }
                    in += best <= 1.75;
                }
            double d = fabs((double)(px[y * 32 + x] >> 24) - in * 255.0 / 256.0);
            sum += d;
            if (d > maxd) maxd = d;
        }
    printf("  round stroke vs reference: max %.1f, mean %.3f (of 255)\n", maxd, sum / 1024);
    CHECK(maxd <= 17, "round stroke matches the distance reference per pixel");
    CHECK(sum / 1024 < 1.0, "round stroke matches the distance reference on average");
    free(px);
}

// A stroke under a transform that is not a similarity is bent with it.
static float g_xf[6];
static void b_hline(mimg_vwriter_t* w) { mimg_vw_move(w, 2, 5); mimg_vw_line(w, 14, 5); }
static void b_vline(mimg_vwriter_t* w) { mimg_vw_move(w, 5, 2); mimg_vw_line(w, 5, 8); }
static void test_stroke_transform(void) {
    mimg_vstyle_t st = mimg_vstyle_stroke(0, 2);
    g_xf[0] = 1; g_xf[1] = 0; g_xf[2] = 0; g_xf[3] = 3; g_xf[4] = 0; g_xf[5] = 0;
    st.transform = g_xf;
    img_t im = one_shape(g_buf, sizeof g_buf, 16, 32, &st, 0xFF000000u, b_hline);
    uint32_t* px = render(&im, 16, 32, NULL);
    CHECK(NEAR(alpha_sum(px, 16 * 32), 12 * 6, 0.3), "scale(1,3): the stroke is 6 thick");
    free(px);
    g_xf[0] = 3; g_xf[3] = 1;
    im = one_shape(g_buf, sizeof g_buf, 32, 16, &st, 0xFF000000u, b_vline);
    px = render(&im, 32, 16, NULL);
    CHECK(NEAR(alpha_sum(px, 32 * 16), 6 * 6, 0.3), "scale(3,1): a vertical stroke is 6 wide");
    free(px);
    // rotation keeps it a similarity: the same area
    float c = (float)cos(0.6), s = (float)sin(0.6);
    g_xf[0] = c; g_xf[1] = s; g_xf[2] = -s; g_xf[3] = c; g_xf[4] = 16; g_xf[5] = 0;
    im = one_shape(g_buf, sizeof g_buf, 32, 32, &st, 0xFF000000u, b_hline);
    px = render(&im, 32, 32, NULL);
    CHECK(NEAR(alpha_sum(px, 32 * 32), 12 * 2, 0.3), "a rotated stroke keeps its area");
    free(px);
}

// ------------------------------------------------------------ gradients

static void grad_rect(uint8_t* buf, img_t* im, bool radial, uint32_t spread, const uint8_t* off, const uint32_t* col,
                      uint32_t n, const float* geom, const float* matrix, float w, float h) {
    mimg_vwriter_t wr;
    mimg_vw_begin(&wr, buf, 4096, 4, 0, 0, w, h);
    int p = mimg_vw_paint_gradient(&wr, radial, spread, off, col, n, geom, matrix);
    mimg_vstyle_t st = mimg_vstyle_fill(p);
    mimg_vw_shape_begin(&wr, &st);
    rect(&wr, 0, 0, w, h);
    mimg_vw_shape_end(&wr);
    *im = finish(&wr, buf);
}

static bool color_near(uint32_t got, uint32_t want, int tol) {
    for (int s = 0; s < 32; s += 8)
        if (abs((int)((got >> s) & 255u) - (int)((want >> s) & 255u)) > tol) return false;
    return true;
}

static void test_gradients(void) {
    static uint8_t buf[4096];
    img_t im;
    const uint8_t off2[2] = { 0, 255 };
    const uint32_t rb[2] = { 0xFFFF0000u, 0xFF0000FFu };
    float lin[4] = { 0, 0, 16, 0 };
    grad_rect(buf, &im, false, MIMG_SPREAD_PAD, off2, rb, 2, lin, NULL, 16, 2);
    uint32_t* px = render(&im, 16, 2, NULL);
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        double t = (i + 0.5) / 16;
        uint32_t want = 0xFF000000u | (uint32_t)(255 * (1 - t) + 0.5) << 16 | (uint32_t)(255 * t + 0.5);
        if (!color_near(px[i], want, 3) || !color_near(px[16 + i], want, 3)) ok = 0;
    }
    CHECK(ok, "linear gradient red to blue, per pixel");
    free(px);
    // the same through a matrix (geometry 0..8, scaled by 2)
    float lin8[4] = { 0, 0, 8, 0 };
    const float m2[6] = { 2, 0, 0, 1, 0, 0 };
    grad_rect(buf, &im, false, MIMG_SPREAD_PAD, off2, rb, 2, lin8, m2, 16, 2);
    uint32_t* pm = render(&im, 16, 2, NULL);
    grad_rect(buf, &im, false, MIMG_SPREAD_PAD, off2, rb, 2, lin, NULL, 16, 2);
    px = render(&im, 16, 2, NULL);
    CHECK(!memcmp(px, pm, 16 * 2 * 4) || color_near(px[5], pm[5], 1), "gradient matrix scales the geometry");
    free(px); free(pm);
    // pad, reflect and repeat past the end (geometry 0..8 on a 16 wide rectangle)
    for (uint32_t spread = 0; spread < 3; spread++) {
        grad_rect(buf, &im, false, spread, off2, rb, 2, lin8, NULL, 16, 1);
        px = render(&im, 16, 1, NULL);
        double t = 12.5 / 8;                    // pixel 12
        t = spread == MIMG_SPREAD_PAD ? 1.0 : spread == MIMG_SPREAD_REFLECT ? 2 - t : t - 1;
        uint32_t want = 0xFF000000u | (uint32_t)(255 * (1 - t) + 0.5) << 16 | (uint32_t)(255 * t + 0.5);
        CHECK(color_near(px[12], want, 4), spread == 0 ? "spread pad" : spread == 1 ? "spread reflect" : "spread repeat");
        free(px);
    }
    // three stops
    const uint8_t off3[3] = { 0, 128, 255 };
    const uint32_t c3[3] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu };
    grad_rect(buf, &im, false, 0, off3, c3, 3, lin, NULL, 16, 1);
    px = render(&im, 16, 1, NULL);
    for (int i = 0; i < 16; i += 5) {
        int idx = (int)((i + 0.5) / 16 * 255 + 0.5);
        double f = idx < 128 ? idx / 128.0 : (idx - 128) / 127.0;
        uint32_t want = idx < 128 ? 0xFF000000u | (uint32_t)(255 * (1 - f) + 0.5) << 16 | (uint32_t)(255 * f + 0.5) << 8
                                  : 0xFF000000u | (uint32_t)(255 * (1 - f) + 0.5) << 8 | (uint32_t)(255 * f + 0.5);
        CHECK(color_near(px[i], want, 4), "three stops");
    }
    free(px);
    // alpha in stops
    const uint32_t ca[2] = { 0x00FF0000u, 0xFFFF0000u };
    grad_rect(buf, &im, false, 0, off2, ca, 2, lin, NULL, 16, 1);
    px = render(&im, 16, 1, NULL);
    CHECK((px[0] >> 24) < 20 && (px[15] >> 24) > 235 && (px[8] >> 24) > 110 && (px[8] >> 24) < 145, "alpha follows the stops");
    CHECK((px[15] & 0xFFFFFFu) == 0xFF0000u, "alpha does not tint the color (non-premultiplied)");
    free(px);
    // radial: centre, half way, beyond
    float rad[5] = { 8, 8, 8, 8, 8 };
    grad_rect(buf, &im, true, 0, off2, rb, 2, rad, NULL, 16, 16);
    px = render(&im, 16, 16, NULL);
    CHECK(color_near(px[7 * 16 + 7], 0xFFF00010u, 24), "radial: red at the centre");
    CHECK(color_near(px[7 * 16 + 11], 0xFF800080u, 24), "radial: half way is a mix");
    CHECK(color_near(px[0], 0xFF0000FFu, 2), "radial: beyond the radius is the last stop");
    free(px);
    // radial with a focal point: the lit side is bigger
    float foc[5] = { 8, 8, 8, 4, 8 };
    grad_rect(buf, &im, true, 0, off2, rb, 2, foc, NULL, 16, 16);
    px = render(&im, 16, 16, NULL);
    CHECK((px[8 * 16 + 4] & 0xFF0000u) > (px[8 * 16 + 12] & 0xFF0000u) + 0x400000u, "radial focal point moves the highlight");
    free(px);
    // a gradient with no extent paints the last stop
    float flat[4] = { 3, 3, 3, 3 };
    grad_rect(buf, &im, false, 0, off2, rb, 2, flat, NULL, 16, 1);
    px = render(&im, 16, 1, NULL);
    CHECK(px[4] == 0xFF0000FFu, "degenerate linear gradient: last stop");
    free(px);
    float zr[5] = { 8, 8, 0, 8, 8 };
    grad_rect(buf, &im, true, 0, off2, rb, 2, zr, NULL, 16, 1);
    px = render(&im, 16, 1, NULL);
    CHECK(px[4] == 0xFF0000FFu, "degenerate radial gradient: last stop");
    free(px);
    // 8 stops are the most
    mimg_vwriter_t wr;
    mimg_vw_begin(&wr, buf, 4096, 4, 0, 0, 4, 4);
    uint8_t o9[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t c9[9] = { 0 };
    CHECK(mimg_vw_paint_gradient(&wr, false, 0, o9, c9, 9, lin, NULL) < 0, "the writer refuses 9 stops");
}

// ------------------------------------------------------------ tinting

static void b_square(mimg_vwriter_t* w) { rect(w, 2, 2, 14, 14); }
static void test_current_color(void) {
    mimg_vwriter_t w;
    static uint8_t buf[2048];
    mimg_vw_begin(&w, buf, sizeof buf, 6, 0, 0, 16, 16);
    int cur = mimg_vw_paint_current(&w, 255), half = mimg_vw_paint_current(&w, 128), sol = mimg_vw_paint_solid(&w, 0xFF336699u);
    mimg_vstyle_t st = mimg_vstyle_fill(cur);
    mimg_vw_shape_begin(&w, &st); rect(&w, 0, 0, 4, 4); mimg_vw_shape_end(&w);
    st = mimg_vstyle_fill(half);
    mimg_vw_shape_begin(&w, &st); rect(&w, 4, 0, 8, 4); mimg_vw_shape_end(&w);
    st = mimg_vstyle_fill(sol);
    mimg_vw_shape_begin(&w, &st); rect(&w, 8, 0, 12, 4); mimg_vw_shape_end(&w);
    img_t im = finish(&w, buf);
    mimg_info_t info;
    CHECK(mimg_info(im.d, im.n, &info) && info.vector && info.current_color, "info: vector and tintable");
    uint32_t* px = render(&im, 16, 16, NULL);
    CHECK(px[1 * 16 + 1] == 0xFF000000u, "default current color is opaque black");
    CHECK(px[1 * 16 + 9] == 0xFF336699u, "a solid paint ignores the current color");
    free(px);
    mimg_render_opts_t o;
    mimg_render_opts_init(&o);
    o.current_color = 0xFF40C0FFu;
    px = render(&im, 16, 16, &o);
    CHECK(px[1 * 16 + 1] == 0xFF40C0FFu, "current color tints");
    CHECK((px[1 * 16 + 5] & 0xFFFFFFu) == 0x40C0FFu && (px[1 * 16 + 5] >> 24) >= 127 && (px[1 * 16 + 5] >> 24) <= 128, "the paint's alpha multiplies");
    CHECK(px[1 * 16 + 9] == 0xFF336699u, "solid still untouched");
    free(px);
    o.current_color = 0x80FFFFFFu;
    px = render(&im, 16, 16, &o);
    CHECK(px[1 * 16 + 1] == 0x80FFFFFFu, "the caller's alpha multiplies");
    uint32_t a = px[1 * 16 + 5] >> 24;
    CHECK(a >= 63 && a <= 65, "both alphas multiply (0.5 x 0.5)");
    free(px);
    // the same image through mimg_decode at its natural size is black-tinted
    uint32_t* nat = malloc(16 * 16 * 4);
    CHECK(mimg_decode(im.d, im.n, nat, 16) && nat[1 * 16 + 1] == 0xFF000000u, "mimg_decode draws vectors at the natural size");
    free(nat);
    (void)b_square;
}

// ------------------------------------------------------------ scaling

static void draw_icon(mimg_vwriter_t* w, bool with_transform) {
    int bg = mimg_vw_paint_solid(w, 0xFF2060C0u), fg = mimg_vw_paint_solid(w, 0xFFFFFFFFu), tint = mimg_vw_paint_current(w, 255);
    float lin[4] = { 0, 0, 24, 24 };
    const uint8_t off[2] = { 0, 255 };
    const uint32_t col[2] = { 0xFFFFD040u, 0xC0E02040u };
    int gr = mimg_vw_paint_gradient(w, false, 0, off, col, 2, lin, NULL);
    mimg_vstyle_t st = mimg_vstyle_fill(bg);
    mimg_vw_shape_begin(w, &st);
    mimg_vw_move(w, 4, 2); mimg_vw_line(w, 20, 2);
    mimg_vw_quad(w, 22, 2, 22, 4); mimg_vw_line(w, 22, 20); mimg_vw_quad(w, 22, 22, 20, 22);
    mimg_vw_line(w, 4, 22); mimg_vw_quad(w, 2, 22, 2, 20); mimg_vw_line(w, 2, 4); mimg_vw_quad(w, 2, 2, 4, 2); mimg_vw_close(w);
    mimg_vw_shape_end(w);
    st = mimg_vstyle_fill(gr);
    mimg_vw_shape_begin(w, &st); circle(w, 12, 12, 7.3f); mimg_vw_shape_end(w);
    st = mimg_vstyle_stroke(fg, 1.7f);
    st.cap = MIMG_CAP_ROUND; st.join = MIMG_JOIN_ROUND;
    mimg_vw_shape_begin(w, &st);
    mimg_vw_move(w, 6, 17); mimg_vw_cubic(w, 8, 8, 14, 8, 18, 17);
    mimg_vw_shape_end(w);
    st = mimg_vstyle_stroke(tint, 1.2f);
    st.join = MIMG_JOIN_MITER;
    mimg_vw_shape_begin(w, &st);
    mimg_vw_move(w, 5, 5); mimg_vw_line(w, 9, 5); mimg_vw_line(w, 9, 9); mimg_vw_close(w);
    mimg_vw_shape_end(w);
    (void)with_transform;
}

static void test_scaling(void) {
    static uint8_t buf[8192];
    mimg_vwriter_t w;
    mimg_vw_begin(&w, buf, sizeof buf, mimg_vw_default_shift(24, 24), 0, 0, 24, 24);
    draw_icon(&w, false);
    img_t im = finish(&w, buf);
    CHECK(im.n > 0, "icon encodes");
    printf("  test icon: %zu bytes\n", im.n);
    uint32_t* small = render(&im, 16, 16, NULL);
    uint32_t* big = render(&im, 256, 256, NULL);
    // box-filter the 256 px render (premultiplied) down to 16 and compare
    double maxd = 0, sum = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            double a = 0, r = 0, g = 0, b = 0;
            for (int j = 0; j < 16; j++)
                for (int i = 0; i < 16; i++) {
                    uint32_t p = big[(y * 16 + j) * 256 + x * 16 + i];
                    double pa = (p >> 24) / 255.0;
                    a += pa; r += pa * ((p >> 16) & 255); g += pa * ((p >> 8) & 255); b += pa * (p & 255);
                }
            uint32_t q = small[y * 16 + x];
            double qa = (q >> 24) / 255.0;
            double d[4] = { fabs(a / 256 - qa) * 255, 0, 0, 0 };
            if (a > 0) {
                d[1] = fabs(r / a - ((q >> 16) & 255)) * (qa > 0.2 ? 1 : 0);
                d[2] = fabs(g / a - ((q >> 8) & 255)) * (qa > 0.2 ? 1 : 0);
                d[3] = fabs(b / a - (q & 255)) * (qa > 0.2 ? 1 : 0);
            }
            for (int k = 0; k < 4; k++) { if (d[k] > maxd) maxd = d[k]; sum += d[k]; }
        }
    printf("  16 px vs 256 px box-filtered: max diff %.1f, mean %.3f\n", maxd, sum / (16 * 16 * 4));
    // (pixels where edges of several shapes meet differ the most: coverage-based compositing
    // treats each shape's coverage as independent, as every anti-aliasing rasterizer does)
    CHECK(maxd <= 30.0, "the 16 px render agrees with the 256 px one (max)");
    CHECK(sum / (16 * 16 * 4) < 0.8, "the 16 px render agrees with the 256 px one (mean)");
    free(small); free(big);
    // 1 px, odd sizes, big sizes all draw
    for (uint32_t s = 1; s <= 600; s = s * 2 + 1) {
        uint32_t* p = render(&im, s, s, NULL);
        CHECK(p != NULL && alpha_sum(p, (size_t)s * s) > 0.3 * s * s, "renders at every size");
        free(p);
    }
    // non-square output keeps the aspect ratio, centered
    uint32_t* wide = render(&im, 96, 48, NULL);
    CHECK(wide && (wide[24 * 96 + 5] >> 24) == 0 && (wide[24 * 96 + 90] >> 24) == 0 && (wide[24 * 96 + 48] >> 24) == 255, "contain: centered with clear sides");
    free(wide);
    mimg_render_opts_t o;
    mimg_render_opts_init(&o);
    o.fit = MIMG_FIT_STRETCH;
    wide = render(&im, 96, 48, &o);
    CHECK(wide && (wide[24 * 96 + 12] >> 24) > 200 && (wide[24 * 96 + 84] >> 24) > 200, "stretch: fills the width");
    free(wide);
    o.fit = MIMG_FIT_COVER;
    wide = render(&im, 96, 48, &o);
    CHECK(wide && (wide[2 * 96 + 48] >> 24) > 200 && (wide[24 * 96 + 48] >> 24) > 200, "cover: fills and crops");
    free(wide);
}

// ------------------------------------------------------------ compositing

static void test_compositing(void) {
    mimg_vwriter_t w;
    static uint8_t buf[1024];
    mimg_vw_begin(&w, buf, sizeof buf, 4, 0, 0, 4, 4);
    int red = mimg_vw_paint_solid(&w, 0xFFFF0000u);
    mimg_vstyle_t st = mimg_vstyle_fill(red);
    st.fill_opacity = 128;
    mimg_vw_shape_begin(&w, &st); rect(&w, 0, 0, 4, 4); mimg_vw_shape_end(&w);
    img_t im = finish(&w, buf);
    uint32_t dst[16];
    mimg_render_opts_t o;
    mimg_render_opts_init(&o);
    o.clear = false;
    for (int i = 0; i < 16; i++) dst[i] = i < 4 ? 0xFF0000FFu : i < 8 ? 0x00000000u : i < 12 ? 0x800000FFu : 0xFFFFFFFFu;
    void* scratch = malloc(mimg_scratch_size(im.d, im.n, 4, 4));
    o.scratch = scratch; o.scratch_size = mimg_scratch_size(im.d, im.n, 4, 4);
    CHECK(mimg_render_ex(im.d, im.n, dst, 4, 4, 4, &o), "composite render");
    // over opaque blue: half red + half blue
    CHECK(color_near(dst[0], 0xFF800080u, 1), "50% red over opaque blue");
    CHECK(color_near(dst[4], 0x80FF0000u, 1), "50% red over transparent is 50% red");
    // over 50% blue: alpha 0.5 + 0.5 x 0.502; color weighted by alpha
    double sa = 128 / 255.0, da = 128 / 255.0, oa = sa + da * (1 - sa);
    double rr = (255 * sa) / oa, bb = (255 * da * (1 - sa)) / oa;
    uint32_t want = (uint32_t)(oa * 255 + 0.5) << 24 | (uint32_t)(rr + 0.5) << 16 | (uint32_t)(bb + 0.5);
    CHECK(color_near(dst[8], want, 2), "50% red over 50% blue");
    CHECK(color_near(dst[12], 0xFFFF7F7Fu, 1), "50% red over white");
    free(scratch);
    // clear = true replaces what was there
    for (int i = 0; i < 16; i++) dst[i] = 0xFF123456u;
    uint32_t* px = render(&im, 4, 4, NULL);
    CHECK(px[0] == 0x80FF0000u, "clear: the result does not depend on the buffer");
    free(px);
    // fully transparent opacity draws nothing
    mimg_vw_begin(&w, buf, sizeof buf, 4, 0, 0, 4, 4);
    red = mimg_vw_paint_solid(&w, 0xFFFF0000u);
    st = mimg_vstyle_fill(red); st.fill_opacity = 0;
    mimg_vw_shape_begin(&w, &st); rect(&w, 0, 0, 4, 4); mimg_vw_shape_end(&w);
    im = finish(&w, buf);
    px = render(&im, 4, 4, NULL);
    CHECK(alpha_sum(px, 16) == 0, "opacity 0 draws nothing");
    free(px);
}

// ------------------------------------------------------------ memory

static size_t g_allocs, g_frees, g_last;
static void* t_alloc(size_t n) { g_allocs++; g_last = n; return malloc(n); }
static void t_free(void* p) { g_frees++; free(p); }
static void* t_alloc_fail(size_t n) { (void)n; g_allocs++; return NULL; }

// Banding changes where float sums start, so pixels may differ by a level.
static bool same_pixels(const uint32_t* a, const uint32_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) if (!color_near(a[i], b[i], 1)) return false;
    return true;
}

static void test_scratch(void) {
    static uint8_t buf[8192];
    mimg_vwriter_t w;
    mimg_vw_begin(&w, buf, sizeof buf, mimg_vw_default_shift(24, 24), 0, 0, 24, 24);
    draw_icon(&w, false);
    img_t im = finish(&w, buf);
    for (uint32_t s = 8; s <= 512; s *= 4) {
        size_t want = mimg_scratch_size(im.d, im.n, s, s), min = mimg_scratch_min(im.d, im.n, s, s);
        CHECK(want >= min && min > 0 && want < 1500u * 1024u, "scratch sizes make sense");
        uint32_t* ref = render(&im, s, s, NULL);
        // exactly the minimum: banded in single rows, the same pixels
        uint32_t* out = malloc((size_t)s * s * 4);
        void* sc = malloc(min);
        mimg_render_opts_t o;
        mimg_render_opts_init(&o);
        o.scratch = sc; o.scratch_size = min;
        CHECK(mimg_render_ex(im.d, im.n, out, s, s, s, &o), "renders with the minimum scratch");
        CHECK(same_pixels(out, ref, (size_t)s * s), "the minimum scratch gives the same pixels (within 1)");
        // a little more than the minimum
        free(sc);
        size_t mid = min + (want - min) / 3;
        sc = malloc(mid);
        o.scratch = sc; o.scratch_size = mid;
        CHECK(mimg_render_ex(im.d, im.n, out, s, s, s, &o) && same_pixels(out, ref, (size_t)s * s), "any size in between gives the same pixels");
        free(sc);
        free(out); free(ref);
    }
    // too little: falls back to alloc, then to the built-in buffer
    uint32_t out[32 * 32];
    mimg_render_opts_t o;
    mimg_render_opts_init(&o);
    char tiny[64];
    o.scratch = tiny; o.scratch_size = sizeof tiny;
    o.alloc = t_alloc; o.free = t_free;
    g_allocs = g_frees = 0;
    CHECK(mimg_render_ex(im.d, im.n, out, 32, 32, 32, &o), "too small a scratch: alloc is used");
    CHECK(g_allocs == 1 && g_frees == 1 && g_last == mimg_scratch_size(im.d, im.n, 32, 32), "alloc once, free once, the recommended size");
    o.alloc = t_alloc_fail;
    CHECK(!mimg_render_ex(im.d, im.n, out, 32, 32, 32, &o), "alloc failure is reported");
    o.alloc = NULL; o.free = NULL;
    CHECK(mimg_render_ex(im.d, im.n, out, 32, 32, 32, &o), "no scratch at all: the built-in buffer");
    CHECK(mimg_render(im.d, im.n, out, 32, 32, 32), "mimg_render uses the built-in buffer");
    // bad arguments
    CHECK(!mimg_render(im.d, im.n, out, 0, 32, 32) && !mimg_render(im.d, im.n, out, 32, 32, 31) &&
          !mimg_render(im.d, im.n, out, 20000, 32, 20000) && !mimg_render(NULL, 0, out, 8, 8, 8), "bad sizes are refused");
    mimg_render_opts_init(&o);
    o.fit = 9;
    CHECK(!mimg_render_ex(im.d, im.n, out, 8, 8, 8, &o), "unknown fit is refused");
    // stride larger than the width: the padding is not touched
    uint32_t padded[40 * 20];
    for (int i = 0; i < 40 * 20; i++) padded[i] = 0xDEADBEEFu;
    CHECK(mimg_render(im.d, im.n, padded, 32, 20, 40), "stride");
    int untouched = 1;
    for (int y = 0; y < 20; y++) for (int x = 32; x < 40; x++) if (padded[y * 40 + x] != 0xDEADBEEFu) untouched = 0;
    CHECK(untouched, "pixels past the width are not written");
}

// ------------------------------------------------------------ format

static img_t make_icon(uint8_t* buf, size_t cap) {
    mimg_vwriter_t w;
    mimg_vw_begin(&w, buf, cap, mimg_vw_default_shift(24, 24), 0, 0, 24, 24);
    draw_icon(&w, false);
    return finish(&w, buf);
}

static void test_format(void) {
    static uint8_t buf[8192];
    img_t im = make_icon(buf, sizeof buf);
    CHECK(!memcmp(im.d, "MIMG", 4) && im.d[4] == 2 && im.d[5] == 4, "magic MIMG, version 2");
    CHECK(mm_get32(im.d + 8) == 24 && mm_get32(im.d + 12) == 24, "natural size is the view box");
    CHECK(mm_get32(im.d + 16) == im.n - MIMG_HEADER_SIZE && mm_get32(im.d + 20) == 0, "payload size and reserved field");
    CHECK(im.d[7] == 1, "flag: uses current color");
    mimg_info_t info;
    CHECK(mimg_info(im.d, im.n, &info) && info.vector && info.width == 24 && info.height == 24 && info.alpha &&
              info.vb_w == 24 && info.vb_h == 24 && info.vb_x == 0, "mimg_info of a vector");
    // fractional view box rounds the natural size up
    mimg_vwriter_t w;
    static uint8_t b2[512];
    mimg_vw_begin(&w, b2, sizeof b2, 6, -3.5f, 2, 20.25f, 10.5f);
    int p = mimg_vw_paint_solid(&w, 0xFF000000u);
    mimg_vstyle_t st = mimg_vstyle_fill(p);
    mimg_vw_shape_begin(&w, &st); rect(&w, -3, 3, 5, 9); mimg_vw_shape_end(&w);
    img_t f = finish(&w, b2);
    CHECK(mimg_info(f.d, f.n, &info) && info.width == 21 && info.height == 11 && info.vb_x == -3.5f && info.vb_y == 2 &&
              info.vb_w == 20.25f && info.vb_h == 10.5f, "fractional view box: size rounded up");
    // a view box that does not start at 0 is honored
    uint32_t* px = render(&f, 21, 11, NULL);
    CHECK(px && (px[(4) * 21 + 1] >> 24) > 0 && (px[4 * 21 + 12] >> 24) == 0, "view box origin");
    free(px);
    // the raster format stays version 1 and an old reader's checks reject version 2
    uint32_t pix[16];
    for (int i = 0; i < 16; i++) pix[i] = 0xFF000000u | (uint32_t)i * 0x010203u;
    uint8_t rast[4096];
    size_t n = mimg_encode(pix, 4, 4, 4, false, rast, sizeof rast);
    CHECK(n > 0 && rast[4] == 1, "raster encoder still writes version 1");
    CHECK(mimg_info(rast, n, &info) && !info.vector && !info.current_color && info.width == 4 && info.vb_w == 4, "info of a raster");
    // versions other than 1 and 2, and mixed-up headers, are refused
    uint8_t cp[8192];
    memcpy(cp, im.d, im.n);
    cp[4] = 3;
    CHECK(!mimg_info(cp, im.n, &info), "version 3 is refused");
    cp[4] = 0;
    CHECK(!mimg_info(cp, im.n, &info), "version 0 is refused");
    memcpy(cp, im.d, im.n);
    cp[5] = 3;
    CHECK(!mimg_info(cp, im.n, &info), "vector with 3 channels is refused");
    memcpy(cp, im.d, im.n);
    cp[6] = 13;
    CHECK(!mimg_info(cp, im.n, &info), "shift above 12 is refused");
    memcpy(cp, im.d, im.n);
    cp[7] = 2;
    CHECK(!mimg_info(cp, im.n, &info), "unknown flags are refused");
    memcpy(cp, im.d, im.n);
    mm_put32(cp + 16, (uint32_t)im.n);   // payload larger than the file
    CHECK(!mimg_info(cp, im.n, &info), "payload size beyond the file is refused");
    memcpy(cp, im.d, im.n);
    mm_put32(cp + 20, 1);
    CHECK(!mimg_info(cp, im.n, &info), "reserved field must be 0");
    memcpy(cp, rast, n);
    cp[4] = 2;
    CHECK(!mimg_info(cp, n, &info), "a raster header with version 2 is refused");
    // writer limits and misuse never produce a file
    CHECK(!mimg_vw_begin(&w, b2, sizeof b2, 13, 0, 0, 10, 10), "writer: shift too big");
    CHECK(!mimg_vw_begin(&w, b2, sizeof b2, 6, 0, 0, 0, 10), "writer: empty view box");
    mimg_vw_begin(&w, b2, sizeof b2, 6, 0, 0, 10, 10);
    CHECK(mimg_vw_paint_solid(&w, 0xFF000000u) == 0, "writer: first paint is 0");
    mimg_vw_move(&w, 1, 1);
    CHECK(mimg_vw_finish(&w) == 0, "writer: line outside a shape fails");
    mimg_vw_begin(&w, b2, 64, 6, 0, 0, 10, 10);
    p = mimg_vw_paint_solid(&w, 0xFF000000u);
    st = mimg_vstyle_fill(p);
    mimg_vw_shape_begin(&w, &st);
    for (int i = 0; i < 100; i++) mimg_vw_line(&w, (float)i, 1);
    CHECK(mimg_vw_finish(&w) == 0, "writer: overflow of the buffer is reported");
    // compact: the icon is small
    CHECK(im.n < 400, "the icon is compact");
}

// version 1 files decode exactly as before this change (hashes taken with the previous header)
static uint64_t fnv64(const void* p, size_t n) {
    const uint8_t* b = p; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static void golden_pic(uint32_t* px, uint32_t w, uint32_t h, int kind, bool alpha) {
    uint32_t s = 12345u + (uint32_t)kind * 7919u;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            uint32_t c;
            if (kind == 0) c = (x * 255 / w) << 16 | (y * 255 / h) << 8 | ((s >> 24) & 15u);
            else if (kind == 1) c = ((x / 9 + y / 7) % 3 == 0) ? 0x3366CC : (x > w / 2 ? 0xF0F0F0 : 0x202020);
            else c = s & 0xFFFFFFu;
            uint32_t a = alpha ? (kind == 0 ? (x * 3 + y * 2) & 255u : 255u - ((x ^ y) & 63u)) : 255u;
            px[(size_t)y * w + x] = a << 24 | c;
        }
}

static void test_version1(void) {
    static const struct { uint32_t w, h; int kind; bool alpha; size_t n; uint64_t enc, dec; } gold[] = {
        {100,70,0,true,9433,0x92f58ac44532b35full,0x9918e6d2acb3bc12ull},
        {130,65,1,false,554,0xd94ee50d0ca2285full,0xa8d7ecc5d8ed78afull},
        {64,64,2,true,20525,0xa6d94e6c5ffe7d1eull,0x2dd45c16e7fa1637ull},
        {3,200,1,true,1034,0xa17172848f836dc0ull,0xfb8173ce6597d1e7ull},
    };
    for (unsigned i = 0; i < sizeof gold / sizeof *gold; i++) {
        uint32_t w = gold[i].w, h = gold[i].h;
        uint32_t* px = malloc((size_t)w * h * 4);
        golden_pic(px, w, h, gold[i].kind, gold[i].alpha);
        size_t cap = mimg_encode_bound(w, h);
        uint8_t* enc = malloc(cap);
        size_t n = mimg_encode(px, w, h, w, gold[i].alpha, enc, cap);
        CHECK(n == gold[i].n && fnv64(enc, n) == gold[i].enc, "version 1 encoding is byte-identical to before");
        uint32_t* out = malloc((size_t)w * h * 4);
        CHECK(mimg_decode(enc, n, out, w) && fnv64(out, (size_t)w * h * 4) == gold[i].dec, "version 1 decodes bit-identically to before");
        // and through the new calls
        uint32_t* out2 = malloc((size_t)w * h * 4);
        mimg_info_t info;
        CHECK(mimg_info(enc, n, &info) && !info.vector && info.width == w && info.height == h && info.alpha == gold[i].alpha, "v1 info");
        CHECK(mimg_render(enc, n, out2, w, h, w) && !memcmp(out, out2, (size_t)w * h * 4), "mimg_render at the natural size is mimg_decode");
        // a different size: scaled; constant areas stay constant
        uint32_t* sm = malloc(37 * 29 * 4);
        CHECK(mimg_render(enc, n, sm, 37, 29, 37), "raster scaled down");
        free(sm);
        free(px); free(enc); free(out); free(out2);
    }
    // scaling a flat picture gives a flat picture, and enlarging is smooth (bilinear)
    uint32_t flat[8 * 8];
    for (int i = 0; i < 64; i++) flat[i] = 0xFF804020u;
    uint8_t enc[4096];
    size_t n = mimg_encode(flat, 8, 8, 8, false, enc, sizeof enc);
    uint32_t big[21 * 21], small[3 * 3];
    CHECK(mimg_render(enc, n, big, 21, 21, 21) && mimg_render(enc, n, small, 3, 3, 3), "raster rendering");
    int same = 1;
    for (int i = 0; i < 21 * 21; i++) same &= big[i] == 0xFF804020u;
    for (int i = 0; i < 9; i++) same &= small[i] == 0xFF804020u;
    CHECK(same, "flat stays flat when scaled");
    uint32_t ramp[2] = { 0xFF000000u, 0xFFFFFFFFu };
    n = mimg_encode(ramp, 2, 1, 2, false, enc, sizeof enc);
    uint32_t row[8];
    uint32_t row4[8 * 4];
    CHECK(mimg_render(enc, n, row4, 8, 4, 8), "raster ramp");
    memcpy(row, row4, sizeof row);
    int mono = 1;
    for (int i = 1; i < 8; i++) mono &= (row[i] & 255u) >= (row[i - 1] & 255u);
    CHECK(mono && (row[0] & 255u) < 20 && (row[7] & 255u) > 235 && (row[3] & 255u) > 60 && (row[3] & 255u) < 120, "enlarging is bilinear");
    // 2x1 raster into a square: aspect kept, centered (contain)
    uint32_t sq[16 * 16];
    CHECK(mimg_render(enc, n, sq, 16, 16, 16), "raster into a square");
    CHECK((sq[0] >> 24) == 0 && (sq[8 * 16 + 8] >> 24) == 255 && (sq[15 * 16] >> 24) == 0, "raster keeps its aspect ratio (contain)");
    // averaging down: a checkerboard 2x2 becomes grey
    uint32_t chk[16 * 16];
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) chk[y * 16 + x] = (x + y) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
    uint8_t* e2 = malloc(mimg_encode_bound(16, 16));
    size_t n2 = mimg_encode(chk, 16, 16, 16, false, e2, mimg_encode_bound(16, 16));
    uint32_t g4[4 * 4];
    CHECK(mimg_render(e2, n2, g4, 4, 4, 4), "checkerboard down");
    CHECK(color_near(g4[5], 0xFF808080u, 2), "shrinking averages (box filter)");
    free(e2);
    // a raster that needs more scratch than the built-in buffer
    uint32_t* bigpic = malloc(520 * 520 * 4);
    for (int i = 0; i < 520 * 520; i++) bigpic[i] = 0xFF204060u;
    uint8_t* e3 = malloc(mimg_encode_bound(520, 520));
    size_t n3 = mimg_encode(bigpic, 520, 520, 520, false, e3, mimg_encode_bound(520, 520));
    uint32_t o100[100 * 100];
    CHECK(!mimg_render(e3, n3, o100, 100, 100, 100), "raster too big for the built-in scratch is refused without alloc");
    mimg_render_opts_t o;
    mimg_render_opts_init(&o);
    o.alloc = malloc; o.free = free;
    CHECK(mimg_render_ex(e3, n3, o100, 100, 100, 100, &o) && o100[5050] == 0xFF204060u, "with alloc it renders");
    CHECK(mimg_scratch_size(e3, n3, 100, 100) >= 520u * 520 * 4, "raster scratch size");
    free(bigpic); free(e3);
}

// ------------------------------------------------------------ damaged files

static int g_rendered;
static void try_file(const uint8_t* d, size_t n) {
    // exact-size copy so the sanitizers see any read past the end
    uint8_t* c = malloc(n ? n : 1);
    memcpy(c, d, n);
    mimg_info_t info;
    if (mimg_info(c, n, &info)) {
        uint32_t w = 20, h = 17;
        uint32_t* out = malloc((size_t)w * h * 4);
        size_t want = mimg_scratch_size(c, n, w, h), min = mimg_scratch_min(c, n, w, h);
        void* sc = malloc(want ? want : 1);
        mimg_render_opts_t o;
        mimg_render_opts_init(&o);
        o.scratch = sc; o.scratch_size = want;
        g_rendered += mimg_render_ex(c, n, out, w, h, w, &o);
        // the minimum scratch must not overrun either
        void* sc2 = malloc(min ? min : 1);
        o.scratch = sc2; o.scratch_size = min;
        (void)mimg_render_ex(c, n, out, w, h, w, &o);
        free(sc2);
        free(sc);
        free(out);
        if ((uint64_t)info.width * info.height <= 256u * 256u) {
            uint32_t* nat = malloc((size_t)info.width * info.height * 4);
            (void)mimg_decode(c, n, nat, info.width);
            free(nat);
        }
    }
    free(c);
}

#ifndef FUZZ_ROUNDS
#define FUZZ_ROUNDS 6000
#endif
static void test_damaged(void) {
    static uint8_t buf[8192];
    img_t im = make_icon(buf, sizeof buf);
    // valid first
    uint32_t* ok = render(&im, 20, 17, NULL);
    CHECK(ok != NULL, "the base file renders");
    free(ok);
    // every truncation
    for (size_t n = 0; n < im.n; n++) {
        try_file(im.d, n);
        uint32_t out[20 * 17];
        mimg_info_t info;
        if (n < im.n) CHECK(!(mimg_info(im.d, n, &info) && mimg_render(im.d, n, out, 20, 17, 20)), "a truncated vector file is refused");
    }
    // random mutations of the vector file, of an icon with a transform and a radial gradient, and of a raster
    static uint8_t buf2[4096];
    mimg_vwriter_t w;
    mimg_vw_begin(&w, buf2, sizeof buf2, 5, -4, -4, 40, 30);
    const uint8_t off[3] = { 0, 100, 255 };
    const uint32_t col[3] = { 0xFFFF0000u, 0x8000FF00u, 0xFF0000FFu };
    float rg[5] = { 10, 10, 12, 8, 9 }, mt[6] = { 1, 0.2f, 0, 1, 1, 1 };
    int g = mimg_vw_paint_gradient(&w, true, MIMG_SPREAD_REFLECT, off, col, 3, rg, mt);
    int s = mimg_vw_paint_solid(&w, 0xFF202020u);
    float xf[6] = { 1.5f, 0.3f, -0.2f, 0.8f, 3, 2 };
    mimg_vstyle_t st = mimg_vstyle_fill(g);
    st.stroke_paint = s; st.stroke_width = 2; st.cap = MIMG_CAP_SQUARE; st.join = MIMG_JOIN_ROUND; st.transform = xf; st.even_odd = true;
    mimg_vw_shape_begin(&w, &st);
    circle(&w, 12, 12, 8); rect(&w, 6, 6, 20, 20);
    mimg_vw_shape_end(&w);
    img_t grad = finish(&w, buf2);
    uint32_t px[16 * 16];
    for (int i = 0; i < 256; i++) px[i] = 0xFF000000u | (uint32_t)i * 0x010101u;
    static uint8_t rast[4096];
    img_t ras = { rast, mimg_encode(px, 16, 16, 16, false, rast, sizeof rast) };
    const img_t* bases[3] = { &im, &grad, &ras };
    int mutations = 0, accepted = 0;
    for (int round = 0; round < FUZZ_ROUNDS; round++) {
        const img_t* base = bases[round % 3];
        uint8_t m[8192];
        memcpy(m, base->d, base->n);
        size_t len = base->n;
        int edits = 1 + (int)(rnd() % 4);
        for (int e = 0; e < edits; e++) {
            size_t at = rnd() % len;
            if (round % 5 == 0) at = rnd() % (len < 48 ? len : 48);     // header and the first paints
            switch (rnd() % 5) {
            case 0: m[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: m[at] = (uint8_t)rnd(); break;
            case 2: m[at] = 0xFF; break;
            case 3: m[at] = 0; break;
            default: if (len > 1 && at + 1 < len) { memmove(m + at, m + at + 1, len - at - 1); len--; }
            }
        }
        // keep the payload size field truthful half of the time so the mutation reaches the parser
        if (round % 2 == 0 && m[4] == 2 && len >= MIMG_HEADER_SIZE) mm_put32(m + 16, (uint32_t)(len - MIMG_HEADER_SIZE));
        try_file(m, len);
        mutations++;
        mimg_info_t info;
        accepted += mimg_info(m, len, &info);
    }
    printf("  %d mutated files, %d with a valid header, %d renders succeeded\n", mutations, accepted, g_rendered);
    CHECK(accepted > 500, "the mutations reach the parser");
    CHECK(g_rendered > 1000, "many mutations are still valid pictures and are drawn");
    // hand-made nasty payloads
    uint8_t h[MIMG_HEADER_SIZE + 64];
    memcpy(h, im.d, MIMG_HEADER_SIZE);
    for (int round = 0; round < 2000; round++) {
        size_t n = MIMG_HEADER_SIZE + 8 + rnd() % 56;
        for (size_t i = MIMG_HEADER_SIZE; i < n; i++) h[i] = (uint8_t)(rnd() % 3 == 0 ? 0xFF : rnd());
        mm_put32(h + 16, (uint32_t)(n - MIMG_HEADER_SIZE));
        h[6] = (uint8_t)(rnd() % 13);
        try_file(h, n);
    }
    // a path that keeps asking for more than the file has, and coordinates at the extremes
    uint8_t ex[256];
    mimg_vw_begin(&w, ex, sizeof ex, 12, 0, 0, 100000, 100000);
    int p = mimg_vw_paint_solid(&w, 0xFF000000u);
    st = mimg_vstyle_stroke(p, 50000);
    st.cap = MIMG_CAP_ROUND; st.join = MIMG_JOIN_ROUND;
    mimg_vw_shape_begin(&w, &st);
    mimg_vw_move(&w, -90000, -90000); mimg_vw_line(&w, 90000, 90000); mimg_vw_cubic(&w, 1e5f, 0, -1e5f, 1e5f, 0, 0);
    mimg_vw_shape_end(&w);
    img_t huge = finish(&w, ex);
    CHECK(huge.n > 0, "extreme coordinates encode");
    uint32_t* hx = render(&huge, 64, 64, NULL);
    CHECK(hx != NULL, "extreme coordinates and a huge stroke render without trouble");
    free(hx);
}

// ------------------------------------------------------------ end to end

static int run(const char* fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc((size_t)len + 1);
    if (fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[len] = 0;
    *n = (size_t)len;
    return d;
}

static void spit(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

// A PNG written with stored deflate blocks (what mosmedia writes): returns RGBA as 0xAARRGGBB.
static uint32_t* read_png(const char* path, uint32_t* w, uint32_t* h) {
    size_t n;
    uint8_t* d = slurp(path, &n);
    if (!d || n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    uint8_t* z = malloc(n);
    size_t zn = 0, at = 8;
    *w = *h = 0;
    while (at + 12 <= n) {
        uint32_t len = (uint32_t)d[at] << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
        const uint8_t* body = d + at + 8;
        if (!memcmp(d + at + 4, "IHDR", 4)) {
            *w = (uint32_t)body[0] << 24 | body[1] << 16 | body[2] << 8 | body[3];
            *h = (uint32_t)body[4] << 24 | body[5] << 16 | body[6] << 8 | body[7];
            if (body[8] != 8 || body[9] != 6) return NULL;
        } else if (!memcmp(d + at + 4, "IDAT", 4)) {
            memcpy(z + zn, body, len);
            zn += len;
        }
        at += 12 + len;
    }
    // zlib: 2 bytes, stored blocks, 4 bytes of adler
    size_t row = (size_t)*w * 4 + 1;
    uint8_t* raw = malloc(row * *h + 1);
    size_t rn = 0, p = 2;
    for (;;) {
        if (p + 5 > zn) return NULL;
        int last = z[p] & 1;
        if ((z[p] >> 1) & 3) return NULL;
        size_t len = z[p + 1] | (size_t)z[p + 2] << 8;
        p += 5;
        if (p + len > zn || rn + len > row * *h) return NULL;
        memcpy(raw + rn, z + p, len);
        rn += len; p += len;
        if (last) break;
    }
    if (rn != row * *h) return NULL;
    uint32_t* px = malloc((size_t)*w * *h * 4);
    for (uint32_t y = 0; y < *h; y++) {
        if (raw[y * row] != 0) return NULL;
        for (uint32_t x = 0; x < *w; x++) {
            const uint8_t* q = raw + y * row + 1 + x * 4;
            px[(size_t)y * *w + x] = (uint32_t)q[3] << 24 | (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
        }
    }
    free(d); free(z); free(raw);
    return px;
}

#define TMP "build/tests/host/vimg_e2e_"

static const char* SVG_BASIC =
    "<?xml version=\"1.0\"?>\n<!-- a comment -->\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\" width=\"64\" height=\"64\">\n"
    "  <title>test</title>\n"
    "  <rect width=\"32\" height=\"32\" fill=\"#00f\"/>\n"
    "  <g transform=\"translate(16 16)\" fill=\"rgb(255, 0, 0)\"><circle r=\"8\"/></g>\n"
    "  <path d=\"M4 28 a6 6 0 0 1 12 0\" stroke=\"#0f0\" fill=\"none\" stroke-width=\"2\"/>\n"
    "  <text x=\"1\" y=\"1\">ignored</text>\n"
    "</svg>\n";

static void test_end_to_end(const char* tool) {
    spit(TMP "basic.svg", SVG_BASIC);
    int rc = run("%s vimg " TMP "basic.svg " TMP "basic.img 2>" TMP "warn.txt >" TMP "out.txt", tool);
    CHECK(rc == 0, "mosmedia vimg converts an SVG");
    size_t n = 0;
    uint8_t* img = slurp(TMP "basic.img", &n);
    CHECK(img != NULL && n > 24 && img[4] == 2, "the output is a version 2 .img");
    if (!img) return;
    mimg_info_t info;
    CHECK(mimg_info(img, n, &info) && info.vector && info.width == 32 && info.height == 32 && info.vb_w == 32, "its size is the view box");
    size_t wn;
    uint8_t* warn = slurp(TMP "warn.txt", &wn);
    CHECK(warn && strstr((char*)warn, "<text>") != NULL, "unsupported <text> is warned about, not fatal");
    free(warn);
    // png at the natural size and at 128
    CHECK(run("%s png " TMP "basic.img " TMP "n.png >" TMP "out.txt", tool) == 0, "mosmedia png on a vector file");
    CHECK(run("%s png " TMP "basic.img " TMP "s.png --size 128 >" TMP "out.txt", tool) == 0, "mosmedia png --size");
    uint32_t w, h;
    uint32_t* png = read_png(TMP "n.png", &w, &h);
    CHECK(png && w == 32 && h == 32, "natural size PNG is 32 x 32");
    if (png) {
        CHECK(png[16 * 32 + 16] == 0xFFFF0000u, "the circle is red (translate, rgb())");
        CHECK(png[1 * 32 + 1] == 0xFF0000FFu, "the background is blue (#00f)");
        uint32_t* ref = malloc(32 * 32 * 4);
        mimg_decode(img, n, ref, 32);
        CHECK(!memcmp(ref, png, 32 * 32 * 4), "the PNG is exactly what mimg_decode draws");
        free(ref);
        free(png);
    }
    png = read_png(TMP "s.png", &w, &h);
    CHECK(png && w == 128 && h == 128, "--size 128 gives 128 x 128");
    if (png) {
        uint32_t* ref = malloc(128 * 128 * 4);
        mimg_render(img, n, ref, 128, 128, 128);
        CHECK(!memcmp(ref, png, 128 * 128 * 4), "the 128 px PNG is exactly mimg_render's");
        // the arc: apex at (10, 22) in drawing units is on the 2-wide green stroke
        uint32_t ap = png[(22 * 4 - 2) * 128 + 10 * 4];
        CHECK(color_near(ap, 0xFF00FF00u, 2), "the arc (A command) is where it should be, green");
        CHECK(png[(26 * 4) * 128 + (10 * 4)] == 0xFF0000FFu, "inside the arc is background");
        free(ref);
        free(png);
    }
    // an independent reader accepts the PNG, when ffmpeg is there
    if (run("command -v ffmpeg >/dev/null 2>&1") == 0) {
        rc = run("ffmpeg -v error -y -i " TMP "s.png -f rawvideo -pix_fmt bgra " TMP "s.raw");
        size_t rn;
        uint8_t* raw = slurp(TMP "s.raw", &rn);
        uint32_t* ref = malloc(128 * 128 * 4);
        mimg_render(img, n, ref, 128, 128, 128);
        CHECK(rc == 0 && raw && rn == 128 * 128 * 4 && !memcmp(raw, ref, rn), "ffmpeg reads the PNG and gets the same pixels");
        free(raw); free(ref);
    }
    // tinting from the command line
    spit(TMP "tint.svg", "<svg viewBox=\"0 0 8 8\"><path d=\"M0 0h8v8H0z\" fill=\"currentColor\"/></svg>");
    CHECK(run("%s vimg " TMP "tint.svg " TMP "tint.img 2>/dev/null >" TMP "out.txt", tool) == 0, "currentColor converts");
    size_t tn;
    uint8_t* tint = slurp(TMP "tint.img", &tn);
    CHECK(tint && mimg_info(tint, tn, &info) && info.current_color, "currentColor becomes a tintable paint");
    CHECK(run("%s png " TMP "tint.img " TMP "t.png --color 336699 >" TMP "out.txt", tool) == 0, "mosmedia png --color");
    png = read_png(TMP "t.png", &w, &h);
    CHECK(png && png[3 * 8 + 3] == 0xFF336699u, "--color tints current color");
    free(png);
    CHECK(run("%s info " TMP "tint.img >" TMP "out.txt", tool) == 0, "mosmedia info on a vector");
    size_t on;
    uint8_t* outtxt = slurp(TMP "out.txt", &on);
    CHECK(outtxt && strstr((char*)outtxt, "vector") && strstr((char*)outtxt, "tintable"), "info says vector and tintable");
    free(outtxt);

    // the SVG subset: the same drawing written two ways must render the same
    static const char* pairs[][2] = {
        { "<svg viewBox='0 0 24 24'><rect x='2' y='3' width='20' height='18' rx='5' fill='red'/></svg>",
          "<svg viewBox='0 0 24 24'><path d='M7 3H17A5 5 0 0 1 22 8V16A5 5 0 0 1 17 21H7A5 5 0 0 1 2 16V8A5 5 0 0 1 7 3Z' fill='red'/></svg>" },
        { "<svg viewBox='0 0 24 24'><circle cx='12' cy='12' r='9' fill='#369'/></svg>",
          "<svg viewBox='0 0 24 24'><ellipse cx='12' cy='12' rx='9' ry='9' style='fill:#336699'/></svg>" },
        { "<svg viewBox='0 0 24 24'><path d='M4 4L20 4L20 20Z' fill='black'/></svg>",
          "<svg viewBox='0 0 24 24'><path d='m4 4 16 0 0 16z' fill='black'/></svg>" },
        { "<svg viewBox='0 0 24 24'><polygon points='4,4 20,4 12,20' fill='black'/></svg>",
          "<svg viewBox='0 0 24 24'><path d='M4,4H20L12,20z'/></svg>" },
        { "<svg viewBox='0 0 24 24'><path d='M4 12h16' stroke='#000' stroke-width='3' stroke-linecap='round'/></svg>",
          "<svg viewBox='0 0 24 24'><line x1='4' y1='12' x2='20' y2='12' stroke='#000' stroke-width='3' stroke-linecap='round'/></svg>" },
        { "<svg viewBox='0 0 24 24'><g transform='translate(12 12) rotate(90) scale(2)'><rect x='-3' y='-5' width='6' height='10' fill='green'/></g></svg>",
          "<svg viewBox='0 0 24 24'><rect x='2' y='6' width='20' height='12' fill='green'/></svg>" },
        { "<svg viewBox='0 0 24 24'><g fill='none' stroke='blue' stroke-width='2' opacity='.5'><path d='M3 3l18 18'/></g></svg>",
          "<svg viewBox='0 0 24 24'><path d='M3 3L21 21' stroke='blue' stroke-width='2' stroke-opacity='.5' fill='none'/></svg>" },
        { "<svg viewBox='0 0 24 24' xmlns:xlink='http://www.w3.org/1999/xlink'><defs><path id='p' d='M2 2h8v8h-8z'/></defs><use xlink:href='#p' x='4' y='4' fill='orange'/></svg>",
          "<svg viewBox='0 0 24 24'><path d='M6 6h8v8h-8z' fill='orange'/></svg>" },
        { "<svg viewBox='0 0 24 24'><defs><linearGradient id='g' x1='0' x2='1'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect width='24' height='24' fill='url(#g)'/></svg>",
          "<svg viewBox='0 0 24 24'><defs><linearGradient id='g' gradientUnits='userSpaceOnUse' x1='0' y1='0' x2='24' y2='0'><stop offset='0%' style='stop-color:#f00'/><stop offset='100%' style='stop-color:#00f'/></linearGradient></defs><rect width='24' height='24' fill='url(#g)'/></svg>" },
        { "<svg viewBox='0 0 24 24'><path d='M12 4C18 4 20 12 12 20S6 4 12 4Z' fill='black'/></svg>",
          "<svg viewBox='0 0 24 24'><path d='M12 4c6 0 8 8 0 16s-6-16 0-16z' fill='black'/></svg>" },
    };
    for (unsigned i = 0; i < sizeof pairs / sizeof *pairs; i++) {
        uint32_t* im[2];
        for (int k = 0; k < 2; k++) {
            spit(TMP "pair.svg", pairs[i][k]);
            rc = run("%s vimg " TMP "pair.svg " TMP "pair.img 2>/dev/null >" TMP "out.txt", tool);
            size_t pn;
            uint8_t* pd = slurp(TMP "pair.img", &pn);
            im[k] = malloc(48 * 48 * 4);
            CHECK(rc == 0 && pd && mimg_render(pd, pn, im[k], 48, 48, 48), "the pair converts and renders");
            free(pd);
        }
        int worst = 0;
        for (int p = 0; p < 48 * 48; p++)
            for (int s = 0; s < 32; s += 8) {
                int d = abs((int)((im[0][p] >> s) & 255u) - (int)((im[1][p] >> s) & 255u));
                if (d > worst) worst = d;
            }
        char msg[96];
        snprintf(msg, sizeof msg, "SVG pair %u renders the same (worst channel difference %d)", i, worst);
        CHECK(worst <= 6, msg);
        free(im[0]); free(im[1]);
    }
    // arcs: the four flag combinations draw four different shapes that all touch both end points
    for (int fa = 0; fa < 2; fa++)
        for (int fs = 0; fs < 2; fs++) {
            char svg[256];
            snprintf(svg, sizeof svg, "<svg viewBox='0 0 24 24'><path d='M6 12A8 5 20 %d %d 18 12' fill='none' stroke='black' stroke-width='1'/></svg>", fa, fs);
            spit(TMP "arc.svg", svg);
            CHECK(run("%s vimg " TMP "arc.svg " TMP "arc.img 2>/dev/null >" TMP "out.txt", tool) == 0, "arc converts");
            size_t an;
            uint8_t* ad = slurp(TMP "arc.img", &an);
            uint32_t* a = malloc(96 * 96 * 4);
            CHECK(ad && mimg_render(ad, an, a, 96, 96, 96), "arc renders");
            int near_a = 0, near_b = 0;
            for (int dy = -3; dy <= 3; dy++)
                for (int dx = -3; dx <= 3; dx++) {
                    if ((a[(48 + dy) * 96 + 24 + dx] >> 24) > 60) near_a = 1;
                    if ((a[(48 + dy) * 96 + 72 + dx] >> 24) > 60) near_b = 1;
                }
            CHECK(near_a && near_b, "the arc reaches both end points");
            free(a); free(ad);
        }
    // the samples in docs/examples convert and draw
    static const char* samples[] = { "docs/examples/sample-icon.svg", "docs/examples/sample-logo.svg" };
    for (int i = 0; i < 2; i++) {
        rc = run("%s vimg %s " TMP "sample.img 2>/dev/null >" TMP "out.txt", tool, samples[i]);
        size_t sn;
        uint8_t* sd = slurp(TMP "sample.img", &sn);
        uint32_t* sp = malloc(96 * 96 * 4);
        CHECK(rc == 0 && sd && mimg_render(sd, sn, sp, 96, 96, 96) && alpha_sum(sp, 96 * 96) > 500, "a docs/examples sample converts and draws");
        free(sp); free(sd);
    }
    // damaged and odd SVG: clean errors, no crash
    spit(TMP "bad.svg", "this is not xml");
    CHECK(run("%s vimg " TMP "bad.svg " TMP "bad.img 2>/dev/null >/dev/null", tool) != 0, "not an SVG: an error");
    spit(TMP "bad.svg", "<svg viewBox='0 0 10 10'><path d='M1 1 L 5 5 L 9 q q q' stroke='red'/><rect width='-5' height='3'/><foo/></svg>");
    CHECK(run("%s vimg " TMP "bad.svg " TMP "bad.img 2>/dev/null >/dev/null", tool) == 0, "a damaged path is drawn as far as it goes, with a warning");
    spit(TMP "bad.svg", "<svg><g><g></svg>");
    run("%s vimg " TMP "bad.svg " TMP "bad.img 2>/dev/null >/dev/null", tool);
    CHECK(run("%s vimg /nonexistent.svg " TMP "bad.img 2>/dev/null >/dev/null", tool) != 0, "a missing file is an error");
    CHECK(run("%s png /nonexistent.img " TMP "bad.png 2>/dev/null >/dev/null", tool) != 0, "png of a missing file is an error");
    free(img); free(tint);
    run("rm -f " TMP "*");
}

int main(int argc, char** argv) {
    test_coverage();
    test_circle_area();
    test_fill_rules();
    test_strokes();
    test_stroke_transform();
    test_gradients();
    test_current_color();
    test_scaling();
    test_compositing();
    test_scratch();
    test_format();
    test_version1();
    test_damaged();
    if (argc > 1) test_end_to_end(argv[1]);
    else printf("  (no converter given: end-to-end checks skipped)\n");
    if (failures) {
        printf("vimg_tests: %d FAILED\n", failures);
        return 1;
    }
    printf("vimg_tests: all passed\n");
    return 0;
}
