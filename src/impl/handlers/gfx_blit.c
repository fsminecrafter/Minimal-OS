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
