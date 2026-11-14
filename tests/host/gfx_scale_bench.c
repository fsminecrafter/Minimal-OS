#define _DEFAULT_SOURCE 1
// Scaled-present benchmark: 960x540 -> 1920x1080 (2x) and 640x360 -> 1920x1080
// (3x), plus a 1x 1920x1080 copy. Host memory is not write-combining, so the
// numbers only indicate the CPU side. Not part of test-host.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>   // src/intf/time.h shadows <time.h> here

#include "gfx_blit.h"

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1e3;
}

static void run(const char* name, uint32_t sw, uint32_t sh, uint32_t k, bool stream) {
    uint32_t dw = 1920, dh = 1080;
    uint32_t* src = malloc((size_t)sw * sh * 4);
    uint32_t* dst = aligned_alloc(64, (size_t)dw * dh * 4);
    for (size_t i = 0; i < (size_t)sw * sh; i++) src[i] = (uint32_t)i * 2654435761u;
    gfx_surface_t ss = { src, (size_t)sw * sh * 4, sw, sh, sw };
    gfx_surface_t ds = { dst, (size_t)dw * dh * 4, dw, dh, dw };
    gfx_rect_t r = { 0, 0, (int32_t)sw, (int32_t)sh };
    for (int i = 0; i < 5; i++) gfx_scale_blit(&ds, &ss, &r, k, 0, 0, stream);
    const int n = 100;
    double best = 1e9, total = 0;
    for (int i = 0; i < n; i++) {
        double t = now_ms();
        gfx_scale_blit(&ds, &ss, &r, k, 0, 0, stream);
        t = now_ms() - t;
        total += t;
        if (t < best) best = t;
    }
    printf("%-26s %s  best %.3f ms  mean %.3f ms\n", name, stream ? "NT " : "std", best, total / n);
    free(src);
    free(dst);
}

int main(void) {
    for (int s = 0; s < 2; s++) {
        run("960x540 -> 1920x1080 2x", 960, 540, 2, s);
        run("640x360 -> 1920x1080 3x", 640, 360, 3, s);
        run("1920x1080 1x", 1920, 1080, 1, s);
    }
    return 0;
}
