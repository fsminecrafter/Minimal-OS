#pragma GCC optimize("O2")
#include "gfx_blit.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

static inline int64_t gfx_max_i64(int64_t a, int64_t b) {
    return a > b ? a : b;
}

static uint32_t gfx_blend_argb(uint32_t src, uint32_t dst) {
    const uint8_t src_a = (uint8_t)((src >> 24) & 0xFFu);
    const uint8_t src_r = (uint8_t)((src >> 16) & 0xFFu);
    const uint8_t src_g = (uint8_t)((src >> 8) & 0xFFu);
    const uint8_t src_b = (uint8_t)(src & 0xFFu);

    const uint8_t dst_a = (uint8_t)((dst >> 24) & 0xFFu);
    const uint8_t dst_r = (uint8_t)((dst >> 16) & 0xFFu);
    const uint8_t dst_g = (uint8_t)((dst >> 8) & 0xFFu);
    const uint8_t dst_b = (uint8_t)(dst & 0xFFu);

    if (src_a == 0u) {
        return dst;
    }
    if (src_a == 255u) {
        return src;
    }

    const uint32_t inv = 255u - (uint32_t)src_a;
    const uint32_t out_a_scaled = (uint32_t)src_a * 255u +
                                  (uint32_t)dst_a * inv;
    const uint32_t out_a = (out_a_scaled + 127u) / 255u;
    const uint32_t out_r = ((uint32_t)src_r * src_a * 255u +
                            (uint32_t)dst_r * dst_a * inv) / out_a_scaled;
    const uint32_t out_g = ((uint32_t)src_g * src_a * 255u +
                            (uint32_t)dst_g * dst_a * inv) / out_a_scaled;
    const uint32_t out_b = ((uint32_t)src_b * src_a * 255u +
                            (uint32_t)dst_b * dst_a * inv) / out_a_scaled;

    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

bool gfx_surface_valid(const gfx_surface_t* surface) {
    if (!surface || !surface->pixels || !surface->width || !surface->height ||
        surface->stride < surface->width) {
        return false;
    }

    const uint64_t row_count = (uint64_t)surface->height - 1u;
    const uint64_t stride = surface->stride;
    if (row_count && stride > (UINT64_MAX - surface->width) / row_count) {
        return false;
    }
    const uint64_t pixel_count = row_count * stride + surface->width;
    if (pixel_count > SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }

    return surface->byte_length >= (size_t)pixel_count * sizeof(uint32_t);
}

bool gfx_clip_rect(const gfx_rect_t* a, const gfx_rect_t* b, gfx_rect_t* out) {
    if (!a || !b || !out) {
        return false;
    }
    if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0) {
        return false;
    }

    const int64_t left = gfx_max_i64(a->x, b->x);
    const int64_t top = gfx_max_i64(a->y, b->y);
    const int64_t right_a = (int64_t)a->x + a->w;
    const int64_t right_b = (int64_t)b->x + b->w;
    const int64_t bottom_a = (int64_t)a->y + a->h;
    const int64_t bottom_b = (int64_t)b->y + b->h;
    const int64_t right = right_a < right_b ? right_a : right_b;
    const int64_t bottom = bottom_a < bottom_b ? bottom_a : bottom_b;

    if (right <= left || bottom <= top) {
        return false;
    }

    out->x = (int32_t)left;
    out->y = (int32_t)top;
    out->w = (int32_t)(right - left);
    out->h = (int32_t)(bottom - top);
    return true;
}

bool gfx_fill(gfx_surface_t* dst, const gfx_rect_t* rect, uint32_t color) {
    if (!gfx_surface_valid(dst) || !rect) return false;
    if (rect->w <= 0 || rect->h <= 0) {
        return true;
    }

    const int64_t x0 = rect->x > 0 ? rect->x : 0;
    const int64_t y0 = rect->y > 0 ? rect->y : 0;
    const int64_t right = (int64_t)rect->x + rect->w;
    const int64_t bottom = (int64_t)rect->y + rect->h;
    const int64_t x1 = right < dst->width ? right : dst->width;
    const int64_t y1 = bottom < dst->height ? bottom : dst->height;

    if (x1 <= x0 || y1 <= y0) {
        return true;
    }

    for (int64_t y = y0; y < y1; ++y) {
        uint32_t* row = dst->pixels + (size_t)y * dst->stride;
        for (int64_t x = x0; x < x1; ++x) {
            row[x] = color;
        }
    }
    return true;
}

static bool gfx_blit(gfx_surface_t* dst, const gfx_surface_t* src,
                     const gfx_rect_t* src_rect, int32_t dst_x, int32_t dst_y,
                     bool alpha) {
    if (!gfx_surface_valid(dst) || !gfx_surface_valid(src) || !src_rect) {
        return false;
    }
    if (src_rect->w <= 0 || src_rect->h <= 0) return true;

    const int64_t start_x = gfx_max_i64(0, -((int64_t)src_rect->x));
    const int64_t start_y = gfx_max_i64(0, -((int64_t)src_rect->y));
    const int64_t clip_x = -((int64_t)dst_x);
    const int64_t clip_y = -((int64_t)dst_y);
    const int64_t x0 = start_x > clip_x ? start_x : clip_x;
    const int64_t y0 = start_y > clip_y ? start_y : clip_y;
    const int64_t src_right = (int64_t)src->width - src_rect->x;
    const int64_t dst_right = (int64_t)dst->width - dst_x;
    const int64_t src_bottom = (int64_t)src->height - src_rect->y;
    const int64_t dst_bottom = (int64_t)dst->height - dst_y;
    int64_t x1 = src_rect->w;
    int64_t y1 = src_rect->h;
    if (src_right < x1) x1 = src_right;
    if (dst_right < x1) x1 = dst_right;
    if (src_bottom < y1) y1 = src_bottom;
    if (dst_bottom < y1) y1 = dst_bottom;

    if (x1 <= x0 || y1 <= y0) return true;

    for (int64_t y = y0; y < y1; ++y) {
        const size_t src_row = (size_t)((int64_t)src_rect->y + y) * src->stride;
        const size_t dst_row = (size_t)((int64_t)dst_y + y) * dst->stride;
        if (!alpha) {
            // Opaque rows are whole-frame compositor copies; move them in
            // one go instead of a pixel at a time.
            memcpy(dst->pixels + dst_row + (size_t)((int64_t)dst_x + x0),
                   src->pixels + src_row + (size_t)((int64_t)src_rect->x + x0),
                   (size_t)(x1 - x0) * sizeof(uint32_t));
            continue;
        }
        for (int64_t x = x0; x < x1; ++x) {
            const size_t src_index = src_row + (size_t)((int64_t)src_rect->x + x);
            const size_t dst_index = dst_row + (size_t)((int64_t)dst_x + x);
            dst->pixels[dst_index] = alpha
                ? gfx_blend_argb(src->pixels[src_index], dst->pixels[dst_index])
                : src->pixels[src_index];
        }
    }
    return true;
}

bool gfx_blit_opaque(gfx_surface_t* dst, const gfx_surface_t* src,
                     const gfx_rect_t* src_rect, int32_t dst_x, int32_t dst_y) {
    return gfx_blit(dst, src, src_rect, dst_x, dst_y, false);
}

bool gfx_blit_alpha(gfx_surface_t* dst, const gfx_surface_t* src,
                    const gfx_rect_t* src_rect, int32_t dst_x, int32_t dst_y) {
    return gfx_blit(dst, src, src_rect, dst_x, dst_y, true);
}

bool gfx_present_rect(gfx_surface_t* dst, const gfx_surface_t* src,
                      const gfx_rect_t* rect) {
    if (!gfx_surface_valid(dst) || !gfx_surface_valid(src) || !rect) return false;
    gfx_rect_t clipped;
    const gfx_rect_t bounds = {
        0, 0,
        dst->width > INT32_MAX ? INT32_MAX : (int32_t)dst->width,
        dst->height > INT32_MAX ? INT32_MAX : (int32_t)dst->height
    };
    if (!gfx_clip_rect(&bounds, rect, &clipped)) return true;
    if (clipped.x < 0 || clipped.y < 0 ||
        (uint64_t)(uint32_t)clipped.x + (uint32_t)clipped.w > src->width ||
        (uint64_t)(uint32_t)clipped.y + (uint32_t)clipped.h > src->height) {
        return false;
    }

    for (int32_t y = 0; y < clipped.h; ++y) {
        const size_t src_index = (size_t)(clipped.y + y) * src->stride +
                                 (uint32_t)clipped.x;
        const size_t dst_index = (size_t)(clipped.y + y) * dst->stride +
                                 (uint32_t)clipped.x;
        for (int32_t x = 0; x < clipped.w; ++x) {
            dst->pixels[dst_index + (uint32_t)x] =
                src->pixels[src_index + (uint32_t)x];
        }
    }
    return true;
}

// ---- scaled / streamed present ----
//
// Each source row is expanded into a small stack chunk (stays in L1) and the
// chunk is then written to the destination, once for every destination row
// the source row covers. That keeps the expansion separate from the
// destination's alignment, so arbitrary offsets and clips need no special
// cases, and the destination only sees full 16-byte non-temporal stores.

#if defined(__SSE2__)
// GCC vector types instead of <emmintrin.h>: that header pulls in hosted
// headers the freestanding kernel build does not have.
typedef int gfx_v4si __attribute__((vector_size(16)));
typedef int gfx_v4si_u __attribute__((vector_size(16), aligned(4), may_alias));
typedef long long gfx_v2di __attribute__((vector_size(16)));
#define GFX_HAVE_SSE2 1
#endif

#define GFX_CHUNK 960u      // destination pixels; a multiple of 4, 8 and 12

// tmp[0 .. o1-o0) = pixels o0 .. o1-1 of the source row scaled by k.
static void gfx_expand_row(uint32_t* tmp, const uint32_t* s, uint32_t k,
                           uint32_t o0, uint32_t o1) {
    uint32_t o = o0;
    if (k == 1) {
        memcpy(tmp, s + o0, (size_t)(o1 - o0) * sizeof(uint32_t));
        return;
    }
#ifdef GFX_HAVE_SSE2
    const uint32_t group = 4u * k;
    while (o < o1 && (o % group) != 0u) { tmp[o - o0] = s[o / k]; o++; }
    for (; o + group <= o1; o += group) {
        gfx_v4si v = *(const gfx_v4si_u*)(s + o / k);
        gfx_v4si_u* d = (gfx_v4si_u*)(tmp + (o - o0));
        if (k == 2) {
            d[0] = __builtin_shufflevector(v, v, 0, 0, 1, 1);
            d[1] = __builtin_shufflevector(v, v, 2, 2, 3, 3);
        } else {
            d[0] = __builtin_shufflevector(v, v, 0, 0, 0, 1);
            d[1] = __builtin_shufflevector(v, v, 1, 1, 2, 2);
            d[2] = __builtin_shufflevector(v, v, 2, 3, 3, 3);
        }
    }
#endif
    for (; o < o1; o++) tmp[o - o0] = s[o / k];
}

// Copies n pixels to d; with `stream`, 16-byte aligned stretches use
// non-temporal stores.
static void gfx_store_row(uint32_t* d, const uint32_t* t, size_t n, bool stream) {
#ifdef GFX_HAVE_SSE2
    if (stream) {
        while (n && ((uintptr_t)d & 15u)) { *d++ = *t++; n--; }
        for (; n >= 4; n -= 4, d += 4, t += 4) {
            gfx_v4si v = *(const gfx_v4si_u*)t;
            __builtin_ia32_movntdq((gfx_v2di*)d, (gfx_v2di)v);
        }
    }
#else
    (void)stream;
#endif
    while (n) { *d++ = *t++; n--; }
}

bool gfx_scale_blit(gfx_surface_t* dst, const gfx_surface_t* src,
                    const gfx_rect_t* src_rect, uint32_t scale,
                    int32_t dst_x, int32_t dst_y, bool stream) {
    if (!gfx_surface_valid(dst) || !gfx_surface_valid(src) || !src_rect ||
        scale < 1u || scale > 3u) {
        return false;
    }
    gfx_rect_t bounds = { 0, 0, (int32_t)src->width, (int32_t)src->height };
    gfx_rect_t sr;
    if (src_rect->w <= 0 || src_rect->h <= 0) return true;
    if (!gfx_clip_rect(src_rect, &bounds, &sr)) return true;

    // The scaled image, relative to its own origin, spans [0, sw) x [0, sh)
    // pixels from the clipped source rect; its origin lands at (ox, oy).
    const int64_t k = scale;
    const int64_t ox = (int64_t)dst_x + (sr.x - (int64_t)src_rect->x) * k;
    const int64_t oy = (int64_t)dst_y + (sr.y - (int64_t)src_rect->y) * k;
    const int64_t sw = (int64_t)sr.w * k;
    const int64_t sh = (int64_t)sr.h * k;
    // Visible part in destination coordinates.
    int64_t vx0 = ox > 0 ? ox : 0;
    int64_t vy0 = oy > 0 ? oy : 0;
    int64_t vx1 = ox + sw < (int64_t)dst->width ? ox + sw : (int64_t)dst->width;
    int64_t vy1 = oy + sh < (int64_t)dst->height ? oy + sh : (int64_t)dst->height;
    if (vx1 <= vx0 || vy1 <= vy0) return true;

    uint32_t tmp[GFX_CHUNK];
    const int64_t first_sy = (vy0 - oy) / k;
    const int64_t last_sy = (vy1 - 1 - oy) / k;
    for (int64_t sy = first_sy; sy <= last_sy; sy++) {
        const uint32_t* srow = src->pixels + (size_t)(sr.y + sy) * src->stride + sr.x;
        int64_t r0 = oy + sy * k;
        int64_t r1 = r0 + k;
        if (r0 < vy0) r0 = vy0;
        if (r1 > vy1) r1 = vy1;
        for (int64_t cx = vx0; cx < vx1; cx += GFX_CHUNK) {
            int64_t ce = cx + GFX_CHUNK;
            if (ce > vx1) ce = vx1;
            gfx_expand_row(tmp, srow, scale, (uint32_t)(cx - ox), (uint32_t)(ce - ox));
            for (int64_t r = r0; r < r1; r++) {
                gfx_store_row(dst->pixels + (size_t)r * dst->stride + cx, tmp,
                              (size_t)(ce - cx), stream);
            }
        }
    }
#ifdef GFX_HAVE_SSE2
    if (stream) __builtin_ia32_sfence();
#endif
    return true;
}
