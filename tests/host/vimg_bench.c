// Speed of the vector .img rasterizer: icons at UI sizes and a logo at 1024 px.
// Run: make bench-vimg   (converts docs/examples/sample-*.svg first; built with -O2)
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mosmedia.h"

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s (run from the repository root)\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(*n);
    if (fread(d, 1, *n, f) != *n) exit(1);
    fclose(f);
    return d;
}

static void bench(const char* name, const uint8_t* d, size_t n, uint32_t size, int iters) {
    uint32_t* out = malloc((size_t)size * size * 4);
    size_t want = mimg_scratch_size(d, n, size, size);
    void* scratch = malloc(want);
    mimg_render_opts_t o;
    mimg_render_opts_init(&o);
    o.scratch = scratch; o.scratch_size = want;
    for (int i = 0; i < 3; i++) mimg_render_ex(d, n, out, size, size, size, &o);
    double best = 1e18, total = 0;
    for (int round = 0; round < 7; round++) {
        double t0 = now_us();
        for (int i = 0; i < iters; i++) {
            if (!mimg_render_ex(d, n, out, size, size, size, &o)) { fprintf(stderr, "render failed\n"); exit(1); }
        }
        double t = (now_us() - t0) / iters;
        total += t;
        if (t < best) best = t;
    }
    printf("%-28s %5u px  best %8.1f us  mean %8.1f us   scratch %zu KiB\n", name, size, best, total / 7, want / 1024);
    free(out); free(scratch);
}

int main(void) {
    size_t in, ln;
    uint8_t* icon = slurp("build/tests/host/sample-icon.img", &in);
    uint8_t* logo = slurp("build/tests/host/sample-logo.img", &ln);
    static const uint32_t sizes[] = { 16, 32, 64, 256 };
    for (unsigned i = 0; i < 4; i++) bench("icon (24 units, 3 shapes)", icon, in, sizes[i], sizes[i] <= 64 ? 2000 : 300);
    for (unsigned i = 0; i < 4; i++) bench("logo (256 units, 15 shapes)", logo, ln, sizes[i], sizes[i] <= 64 ? 1000 : 100);
    bench("logo (256 units, 15 shapes)", logo, ln, 512, 30);
    bench("logo (256 units, 15 shapes)", logo, ln, 1024, 10);
    return 0;
}
