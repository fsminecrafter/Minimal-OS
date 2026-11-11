// Host test for the GPU stage of the motion video codec: the YCbCr -> RGB
// shader (nv_builtin_fgsl.h) must give exactly mmc_to_argb's pixels.
// The shader runs here on the CPU rasterizer; the same bytecode is what
// the NVIDIA backend turns into machine code (also encoded below).
// Build and run: make test-host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_raster.h"
#include "x86_64/fgpu_ir.h"
#include "x86_64/nv_sass.h"
#include "x86_64/nv_builtin_fgsl.h"
#include "fglsl.h"
#include "mosmedia.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static fglsl_program_t comp;
static uint32_t rng = 99u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

#define IRCAP 16384u
static fgpu_ir_inst_t ir_code[IRCAP];
static uint32_t ir_hash[FGPU_IR_HASH_WORDS(IRCAP)];
static uint32_t sass_words[IRCAP * 8u];

static void run(uint32_t width, uint32_t height, int kind) {
    uint32_t w = (width + 15u) & ~15u, h = (height + 15u) & ~15u, cw = w / 2, ch = h / 2;
    uint8_t* planes[3] = { malloc(w * h), malloc(cw * ch), malloc(cw * ch) };
    uint32_t* tex[3] = { malloc(w * h * 4), malloc(cw * ch * 4), malloc(cw * ch * 4) };
    for (int p = 0; p < 3; p++) {
        uint32_t n = p ? cw * ch : w * h;
        for (uint32_t i = 0; i < n; i++) {
            // Random, extremes (clipping), or smooth.
            uint8_t v = kind == 0 ? (uint8_t)rnd() : kind == 1 ? ((rnd() & 1u) ? 255 : 0) : (uint8_t)(i * 3u + p * 40u);
            planes[p][i] = v;
            tex[p][i] = 0xFF000000u | ((uint32_t)v << 16);     // the byte as red: what .r reads
        }
    }
    mmc_t m;
    memset(&m, 0, sizeof m);
    m.w = w; m.h = h; m.cw = cw; m.ch = ch; m.width = width; m.height = height;
    m.ref[0] = planes[0]; m.ref[1] = planes[1]; m.ref[2] = planes[2];
    uint32_t* want = malloc(width * height * 4);
    mmc_to_argb(&m, want, width, height, width);

    fgpu_program_t prog;
    const char* why = "";
    CHECK(fgpu_program_check(comp.words, comp.size, &prog, &why), "video shader accepted");
    static float uniforms[FGPU_SHADER_MAX_UNIFORMS][4];
    memset(uniforms, 0, sizeof uniforms);
    int ui = fglsl_find_uniform(&comp, "u_inv"), uc = fglsl_find_uniform(&comp, "u_cmax");
    CHECK(ui >= 0 && uc >= 0, "video shader uniforms");
    float inv[4] = { 1.0f / w, 1.0f / h, 1.0f / cw, 1.0f / ch };
    memcpy(uniforms[comp.uniforms[ui].reg], inv, sizeof inv);
    uniforms[comp.uniforms[uc].reg][0] = (float)(cw - 1);
    uniforms[comp.uniforms[uc].reg][1] = (float)(ch - 1);

    fgpu_pipeline_t p;
    memset(&p, 0, sizeof p);
    p.vp_w = (int32_t)width; p.vp_h = (int32_t)height;
    p.shader = &prog;
    p.constants = uniforms;
    static const char* const names[3] = { "ty", "tu", "tv" };
    for (int t = 0; t < 3; t++) {
        int u = fglsl_find_uniform(&comp, names[t]);
        CHECK(u >= 0 && comp.uniforms[u].reg < FGPU_MAX_TEXTURE_UNITS, "video shader sampler");
        p.textures[comp.uniforms[u].reg] =
            (fgpu_texture_t){ tex[t], t ? cw : w, t ? ch : h, t ? cw : w, FGPU_TEXTURE_CLAMP };
    }
    // One triangle over the whole target: clip position, pixel position.
    float W = (float)width, H = (float)height;
    float tri[3][4] = { { -1, 1, 0, 0 }, { -1, -3, 0, 2 * H }, { 3, 1, 2 * W, 0 } };
    fgpu_layout_t layout = { 4, 2, { FGPU_ATTRIB(0, 2), FGPU_ATTRIB(2, 2) } };
    uint32_t* got = calloc(width * height, 4);
    fgpu_target_t t = { got, width, NULL, 0, width, height };
    fgpu_raster_draw_layout(&t, &p, &layout, &tri[0][0], 3, NULL, 3);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < width * height; i++)
        if ((got[i] | 0xFF000000u) != want[i]) {
            if (!bad) printf("  %ux%u kind %d: pixel %u is 0x%08x, CPU decoder 0x%08x\n", width, height, kind, i,
                             got[i], want[i]);
            bad++;
        }
    CHECK(bad == 0, "the shader gives exactly the CPU decoder's pixels");
    for (int k = 0; k < 3; k++) { free(planes[k]); free(tex[k]); }
    free(want); free(got);
}

int main(void) {
    if (fglsl_compile(&comp, NV_VIDEO_VS, NV_VIDEO_FS) != 0) {
        printf("FAIL: the video shader does not compile: %s\n", comp.error);
        return 1;
    }
    for (int kind = 0; kind < 3; kind++) {
        run(64, 48, kind);
        run(45, 30, kind);      // odd sizes: the last column and row
        run(200, 112, kind);
    }
    // And it becomes machine code for the card.
    fgpu_program_t prog;
    const char* why = "";
    fgpu_program_check(comp.words, comp.size, &prog, &why);
    for (int fragment = 0; fragment < 2; fragment++) {
        fgpu_ir_t ir = { .code = ir_code, .capacity = IRCAP, .hash = ir_hash };
        bool ok = fgpu_ir_lower(&ir, &prog, fragment, &why);
        if (ok) fgpu_ir_allocate(&ir);
        nv_sass_t sh = { .code = sass_words, .capacity = IRCAP * 8u };
        ok = ok && nv_sass_encode(ir.code, ir.count, ir.reg_count, fragment, prog.h.varying_count, &sh, &why);
        if (!ok) printf("  %s\n", why);
        CHECK(ok, "the video shader encodes for the GPU");
        if (fragment) CHECK(sh.textures == 7u, "it samples the three planes");
    }
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("video_gpu_tests: all passed\n");
    return 0;
}
