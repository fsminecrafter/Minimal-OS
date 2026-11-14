#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_blit.h"

static void test_clip_rect(void) {
    gfx_rect_t a = { 0, 0, 10, 10 };
    gfx_rect_t b = { -5, 2, 20, 6 };
    gfx_rect_t out = { 0, 0, 0, 0 };

    assert(gfx_clip_rect(&a, &b, &out));
    assert(out.x == 0);
    assert(out.y == 2);
    assert(out.w == 10);
    assert(out.h == 6);

    gfx_rect_t c = { 0, 0, 0, 0 };
    assert(!gfx_clip_rect(&a, &c, &out));

    gfx_rect_t d = { 100, 100, 10, 10 };
    gfx_rect_t e = { 0, 0, 10, 10 };
    assert(!gfx_clip_rect(&d, &e, &out));

    gfx_rect_t huge = { INT32_MAX - 1, 0, 8, 1 };
    gfx_rect_t overlapping = { INT32_MAX - 2, 0, 8, 1 };
    assert(gfx_clip_rect(&huge, &overlapping, &out));
    assert(out.x == INT32_MAX - 1);
    assert(out.w == 7);
}

static void test_fill_and_blit(void) {
    uint32_t dst[4 * 4] = { 0 };
    uint32_t src[4 * 4];
    gfx_surface_t dst_surface = { dst, sizeof(dst), 4, 4, 4 };
    gfx_surface_t src_surface = { src, sizeof(src), 4, 4, 4 };
    gfx_rect_t rect = { 1, 1, 2, 2 };
    assert(gfx_fill(&dst_surface, &rect, 0x00FF00FFu));
    assert(dst[1 * 4 + 1] == 0x00FF00FFu);

    for (int i = 0; i < 16; ++i) {
        src[i] = (uint32_t)(0x01000000u | (uint32_t)i);
    }

    gfx_rect_t source_rect = { 0, 0, 2, 2 };
    assert(gfx_blit_opaque(&dst_surface, &src_surface, &source_rect, 0, 0));
    assert(dst[0] == 0x01000000u);
    assert(dst[1 * 4 + 1] == ((uint32_t)0x01000000u | 5u));

    uint32_t alpha_dst[4 * 4] = { 0xFFFF0000u };
    uint32_t alpha_src[4 * 4];
    gfx_surface_t alpha_dst_surface = { alpha_dst, sizeof(alpha_dst), 4, 4, 4 };
    gfx_surface_t alpha_src_surface = { alpha_src, sizeof(alpha_src), 4, 4, 4 };
    for (int i = 0; i < 16; ++i) {
        alpha_src[i] = 0x80000000u | (uint32_t)i;
    }

    assert(gfx_blit_alpha(&alpha_dst_surface, &alpha_src_surface,
                          &source_rect, 0, 0));
    assert(alpha_dst[0] != 0xFFFF0000u);
}

static void test_hostile_clip_and_alpha(void) {
    gfx_rect_t a = { INT32_MIN, 0, 4, 2 };
    gfx_rect_t b = { INT32_MIN + 1, 0, 4, 2 };
    gfx_rect_t out = { 0, 0, 0, 0 };
    assert(gfx_clip_rect(&a, &b, &out));
    assert(out.x == INT32_MIN + 1);
    assert(out.w == 3);

    uint32_t fill_dst[4 * 4] = { 0 };
    gfx_surface_t fill_surface = { fill_dst, sizeof(fill_dst), 4, 4, 4 };
    gfx_rect_t hostile_fill = { INT32_MIN, INT32_MIN, 10, 10 };
    assert(gfx_fill(&fill_surface, &hostile_fill, 0x11223344u));
    assert(fill_dst[0] == 0u);

    uint32_t alpha_dst[4] = { 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u };
    uint32_t alpha_src[4] = { 0x00000000u, 0xFFFFFFFFu, 0x80000000u, 0x80008080u };
    gfx_surface_t dst_surface = { alpha_dst, sizeof(alpha_dst), 2, 2, 2 };
    gfx_surface_t src_surface = { alpha_src, sizeof(alpha_src), 2, 2, 2 };
    gfx_rect_t rect = { 0, 0, 2, 2 };
    assert(gfx_blit_alpha(&dst_surface, &src_surface, &rect, -1, 0));
    assert(alpha_dst[0] == 0xFFFFFFFFu);
    assert(alpha_dst[1] == 0xFF000000u);

    uint32_t alpha_dst2[2] = { 0x00000000u, 0x00000000u };
    uint32_t alpha_src2[2] = { 0xFFFFFFFFu, 0x00000000u };
    gfx_surface_t dst_surface2 = { alpha_dst2, sizeof(alpha_dst2), 2, 1, 2 };
    gfx_surface_t src_surface2 = { alpha_src2, sizeof(alpha_src2), 2, 1, 2 };
    gfx_rect_t rect2 = { 0, 0, 2, 1 };
    assert(gfx_blit_alpha(&dst_surface2, &src_surface2, &rect2, 0, 0));
    assert(alpha_dst2[0] == 0xFFFFFFFFu);
    assert(alpha_dst2[1] == 0x00000000u);
}

static void test_surface_bounds_and_present(void) {
    uint32_t dst[6] = { 0 };
    uint32_t src[6] = { 1, 2, 3, 4, 5, 6 };
    gfx_surface_t dst_surface = { dst, sizeof(dst), 2, 2, 3 };
    gfx_surface_t src_surface = { src, sizeof(src), 2, 2, 3 };
    gfx_rect_t rect = { 0, 0, 2, 2 };

    assert(gfx_surface_valid(&dst_surface));
    assert(gfx_present_rect(&dst_surface, &src_surface, &rect));
    assert(dst[0] == 1 && dst[1] == 2);
    assert(dst[3] == 4 && dst[4] == 5);
    assert(dst[2] == 0 && dst[5] == 0);

    dst_surface.byte_length = 4 * sizeof(uint32_t);
    assert(!gfx_surface_valid(&dst_surface));

    gfx_surface_t overflow = {
        dst, SIZE_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    assert(!gfx_surface_valid(&overflow));
}

// Reference: every destination pixel looked up one by one.
static void scale_reference(uint32_t* dst, uint32_t dw, uint32_t dh, uint32_t dstride,
                            const uint32_t* src, uint32_t sstride, const gfx_rect_t* r,
                            uint32_t k, int32_t dx, int32_t dy) {
    for (int32_t y = 0; y < r->h * (int32_t)k; y++) {
        for (int32_t x = 0; x < r->w * (int32_t)k; x++) {
            int64_t px = (int64_t)dx + x, py = (int64_t)dy + y;
            if (px < 0 || py < 0 || px >= dw || py >= dh) continue;
            dst[(size_t)py * dstride + px] = src[(size_t)(r->y + y / (int32_t)k) * sstride +
                                                  r->x + x / (int32_t)k];
        }
    }
}

static uint32_t rng_state = 12345;
static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void test_scale_blit(void) {
    // Odd sizes, strides larger than widths, offsets and clipping on every side.
    for (int iter = 0; iter < 600; iter++) {
        uint32_t k = 1 + (uint32_t)iter % 3;
        uint32_t sw = 1 + rnd() % 70, sh = 1 + rnd() % 20;
        uint32_t sstride = sw + rnd() % 5;
        uint32_t dw = 1 + rnd() % 200, dh = 1 + rnd() % 60;
        uint32_t dstride = dw + rnd() % 7;
        uint32_t* src = malloc((size_t)sstride * sh * 4);
        uint32_t* a = malloc((size_t)dstride * dh * 4);
        uint32_t* b = malloc((size_t)dstride * dh * 4);
        uint32_t* c = malloc((size_t)dstride * dh * 4);
        assert(src && a && b && c);
        for (size_t i = 0; i < (size_t)sstride * sh; i++) src[i] = rnd() | 0x01000000u;
        for (size_t i = 0; i < (size_t)dstride * dh; i++) a[i] = b[i] = c[i] = 0xDEAD0000u + (uint32_t)i;
        gfx_rect_t r = { 0, 0, (int32_t)sw, (int32_t)sh };
        if (iter & 1) {     // a sub-rectangle, sometimes sticking out of the source
            r.x = (int32_t)(rnd() % sw) - 2; r.y = (int32_t)(rnd() % sh) - 1;
            r.w = 1 + (int32_t)(rnd() % sw); r.h = 1 + (int32_t)(rnd() % sh);
        }
        int32_t dx = (int32_t)(rnd() % (dw + 80)) - 40, dy = (int32_t)(rnd() % (dh + 40)) - 20;
        // Reference works on the part of the rect that lies inside the source.
        gfx_rect_t in = r;
        if (in.x < 0) { in.w += in.x; dx += -r.x * (int32_t)k; in.x = 0; }
        if (in.y < 0) { in.h += in.y; dy += -r.y * (int32_t)k; in.y = 0; }
        if (in.x + in.w > (int32_t)sw) in.w = (int32_t)sw - in.x;
        if (in.y + in.h > (int32_t)sh) in.h = (int32_t)sh - in.y;
        if (in.w > 0 && in.h > 0) scale_reference(a, dw, dh, dstride, src, sstride, &in, k, dx, dy);
        gfx_surface_t ss = { src, (size_t)sstride * sh * 4, sw, sh, sstride };
        gfx_surface_t ds = { b, (size_t)dstride * dh * 4, dw, dh, dstride };
        gfx_surface_t dt = { c, (size_t)dstride * dh * 4, dw, dh, dstride };
        int32_t odx = (iter & 1) ? dx - ((r.x < 0) ? -r.x * (int32_t)k : 0) : dx;
        int32_t ody = (iter & 1) ? dy - ((r.y < 0) ? -r.y * (int32_t)k : 0) : dy;
        assert(gfx_scale_blit(&ds, &ss, &r, k, odx, ody, false));
        assert(gfx_scale_blit(&dt, &ss, &r, k, odx, ody, true));
        assert(memcmp(a, b, (size_t)dstride * dh * 4) == 0);
        assert(memcmp(a, c, (size_t)dstride * dh * 4) == 0);
        free(src); free(a); free(b); free(c);
    }
    uint32_t px[4] = {0}, one = 1;
    gfx_surface_t d = { px, sizeof(px), 2, 2, 2 }, s1 = { &one, 4, 1, 1, 1 };
    gfx_rect_t r1 = { 0, 0, 1, 1 };
    assert(!gfx_scale_blit(&d, &s1, &r1, 0, 0, 0, false));
    assert(!gfx_scale_blit(&d, &s1, &r1, 4, 0, 0, false));
    assert(gfx_scale_blit(&d, &s1, &r1, 3, INT32_MAX, INT32_MIN, true));   // far off screen
    assert(gfx_scale_blit(&d, &s1, &r1, 2, 0, 0, false) && px[0] == 1 && px[3] == 1);
}

int main(void) {
    test_clip_rect();
    test_fill_and_blit();
    test_hostile_clip_and_alpha();
    test_surface_bounds_and_present();
    test_scale_blit();
    puts("gfx_blit_tests: PASS");
    return 0;
}
