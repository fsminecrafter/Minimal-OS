// Benchmark of the shader JIT against the interpreter, one thread:
//   make bench-fgpu-jit
// Reports lane-instructions per second (FGSL bytecode instructions of the
// stage x 8 lanes x batches / time), through fgpu_shader_run_batch (the
// interface the rasterizer uses), and where the time goes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "x86_64/fgpu_jit_x64.h"
#include "fglsl.h"

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

static uint32_t rng = 1;
static float frand(float lo, float hi) {
    rng = rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(rng >> 8) / 16777216.0f;
}

static fgpu_shader_batch_t batch;
static float uniforms[FGPU_SHADER_MAX_UNIFORMS][4];
static uint32_t texels[64 * 64];
static fgpu_texture_t tex[FGPU_MAX_TEXTURE_UNITS];
static fglsl_program_t comp;
static uint32_t blob[FGLSL_MAX_BLOB_WORDS];

typedef struct { const char* name; const char* vs; const char* fs; } shader_t;

static const char* k_arith_vs =
    "uniform mat4 mvp; uniform mat4 model; uniform vec4 k;\n"
    "attribute vec3 position; attribute vec3 normal; attribute vec4 color; attribute vec2 uv;\n"
    "varying vec3 v_n; varying vec4 v_c; varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec3 n = normalize((model * vec4(normal, 0.0)).xyz);\n"
    "  float d = max(dot(n, normalize(k.xyz)), 0.0);\n"
    "  v_n = n * 0.5 + vec3(0.5);\n"
    "  v_c = color * (0.3 + d * k.w) + vec4(0.02);\n"
    "  v_uv = uv * 2.0 - vec2(0.5);\n"
    "  gl_Position = mvp * vec4(position, 1.0);\n"
    "}\n";
static const char* k_arith_fs =
    "varying vec3 v_n; varying vec4 v_c; varying vec2 v_uv;\n"
    "uniform vec4 k;\n"
    "void main() {\n"
    "  vec3 n = normalize(v_n * 2.0 - vec3(1.0));\n"
    "  float d = max(dot(n, vec3(0.3, 0.8, 0.5)), 0.0);\n"
    "  float s = pow(d, 8.0) * k.x;\n"
    "  gl_FragColor = vec4(v_c.rgb * (0.2 + d) + vec3(s), 1.0);\n"
    "}\n";
static const char* k_tex_fs =
    "varying vec3 v_n; varying vec4 v_c; varying vec2 v_uv;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "  vec4 t = texture(tex, v_uv);\n"
    "  gl_FragColor = vec4(t.rgb * v_c.rgb, t.a);\n"
    "}\n";
static const char* k_cube_vs =
    "uniform mat4 fgl_ModelViewProjection;\nuniform mat4 fgl_Model;\nuniform mat4 fgl_NormalMatrix;\n"
    "attribute vec3 position;\nattribute vec3 normal;\nattribute vec4 color;\nattribute vec2 uv;\n"
    "varying vec3 v_world;\nvarying vec3 v_normal;\nvarying vec4 v_color;\nvarying vec2 v_uv;\n"
    "void main() {\n    v_world = (fgl_Model * vec4(position, 1.0)).xyz;\n"
    "    v_normal = (fgl_NormalMatrix * vec4(normal, 0.0)).xyz;\n    v_color = color;\n    v_uv = uv;\n"
    "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n}\n";
static const char* k_cube_fs =
    "uniform sampler2D tex;\nuniform vec3 fgl_LightDirection, fgl_LightColor, fgl_Ambient, fgl_CameraPosition;\n"
    "uniform float time;\nuniform float textured;\nvarying vec3 v_world;\nvarying vec3 v_normal;\n"
    "varying vec4 v_color;\nvarying vec2 v_uv;\n"
    "vec3 checker(vec3 p) {\n    float c = mod(floor(p.x) + floor(p.z), 2.0);\n    return vec3(mix(0.62, 1.0, c));\n}\n"
    "void main() {\n    vec3 n = normalize(v_normal);\n    vec3 l = -normalize(fgl_LightDirection);\n"
    "    vec3 v = normalize(fgl_CameraPosition - v_world);\n    float diff = max(dot(n, l), 0.0);\n"
    "    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 32.0) * 0.6 * textured;\n"
    "    vec3 base = v_color.rgb;\n    if (textured > 0.5) base *= texture(tex, v_uv).rgb;\n"
    "    else base *= checker(v_world);\n    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);\n"
    "    vec3 glow = vec3(0.3, 0.65, 1.0) * rim * (0.6 + 0.4 * sin(time * 3.0)) * textured;\n"
    "    float fog = smoothstep(4.0, 9.0, length(v_world - fgl_CameraPosition));\n"
    "    vec3 color = base * (fgl_Ambient + fgl_LightColor * diff) + fgl_LightColor * spec + glow;\n"
    "    gl_FragColor = vec4(mix(color, vec3(0.06, 0.08, 0.13), fog), 1.0);\n}\n";

static void fill(bool fragment, uint32_t varyings) {
    uint32_t n = fragment ? varyings : FGPU_MAX_ATTRIBS;
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 8; l++) batch.input[i][c][l] = frand(0.1f, 1.0f);
    for (int c = 0; c < 4; c++)
        for (int l = 0; l < 8; l++) { batch.special[0][c][l] = frand(0, 100); batch.special[1][c][l] = 1; }
}

// Batches per second over about `secs` seconds.
static double rate(const fgpu_program_t* p, bool fragment, double secs, uint32_t* sink) {
    uint64_t n = 0;
    double t0 = now_s(), t;
    uint32_t acc = 0;
    do {
        for (int i = 0; i < 2000; i++) acc += fgpu_shader_run_batch(p, fragment, &batch, 0xFF, (const float (*)[4])uniforms, tex);
        n += 2000;
        t = now_s();
    } while (t - t0 < secs);
    *sink += acc;
    return (double)n / (t - t0);
}

int main(void) {
    for (uint32_t r = 0; r < FGPU_SHADER_MAX_UNIFORMS; r++) for (int c = 0; c < 4; c++) uniforms[r][c] = frand(-1, 1);
    for (int i = 0; i < 64 * 64; i++) texels[i] = (uint32_t)frand(0, 4e9f);
    for (int u = 0; u < 4; u++) { tex[u].pixels = texels; tex[u].width = 64; tex[u].height = 64; tex[u].pitch = 64; tex[u].flags = FGPU_TEXTURE_LINEAR; }
    uint32_t sup = fgpu_jit_cpu_support();
    printf("host: SSE4.1 %s, AVX2 %s\n", (sup & 1) ? "yes" : "no", (sup & 2) ? "yes" : "no");
    printf("%-28s %6s %10s %10s %10s %8s %8s\n", "shader (stage)", "instr", "interp", "SSE4.1", "AVX2", "SSE x", "AVX2 x");
    printf("%-28s %6s %10s %10s %10s\n", "", "", "G lane-i/s", "G lane-i/s", "G lane-i/s");
    shader_t sh[] = {
        { "arith vertex (~25 instr)", k_arith_vs, k_arith_fs },
        { "arith fragment", k_arith_vs, k_arith_fs },
        { "fragment + bilinear tex", k_arith_vs, k_tex_fs },
        { "fglcube vertex", k_cube_vs, k_cube_fs },
        { "fglcube fragment", k_cube_vs, k_cube_fs },
    };
    static const bool is_frag[] = { false, true, true, false, true };
    uint32_t sink = 0;
    for (unsigned i = 0; i < sizeof sh / sizeof sh[0]; i++) {
        if (fglsl_compile(&comp, sh[i].vs, sh[i].fs) != 0) { printf("compile error: %s\n", comp.error); return 1; }
        memcpy(blob, comp.words, comp.size);
        static fgpu_program_t pi, pj;
        const char* why;
        if (!fgpu_program_check(blob, comp.size, &pi, &why)) { printf("check: %s\n", why); return 1; }
        bool fr = is_frag[i];
        uint32_t instr = fr ? pi.h.fs_count : pi.h.vs_count;
        fill(fr, pi.h.varying_count);
        fgpu_shader_batch_reset(&batch, pi.h.temp_count);
        double lanes = 8.0 * instr / 1e9;
        double r_int = rate(&pi, fr, 0.4, &sink) * lanes;
        double r_sse = 0, r_avx = 0;
        for (int path = 0; path < 2; path++) {
            if (!(sup & (1u << path))) continue;
            pj = pi;
            pj.jit = NULL;
            fgpu_jit_set_mode(path ? FGPU_JIT_AVX2 : FGPU_JIT_SSE41);
            if (!fgpu_program_jit(&pj)) { printf("jit failed\n"); return 1; }
            double r = rate(&pj, fr, 0.4, &sink) * lanes;
            if (path) r_avx = r; else r_sse = r;
            fgpu_program_release(&pj);
        }
        printf("%-28s %6u %10.3f %10.3f %10.3f %7.1fx %7.1fx\n", sh[i].name, instr, r_int, r_sse, r_avx,
               r_sse / r_int, r_avx / r_int);
    }
    // Where the time goes: the batch interface (AoS -> SoA marshalling like the rasterizer's) and the texture helper.
    {
        fglsl_compile(&comp, k_arith_vs, k_arith_fs);
        memcpy(blob, comp.words, comp.size);
        static fgpu_program_t pj;
        fgpu_program_check(blob, comp.size, &pj, NULL);
        fgpu_jit_set_mode((sup & 2) ? FGPU_JIT_AVX2 : FGPU_JIT_SSE41);
        fgpu_program_jit(&pj);
        fill(true, pj.h.varying_count);
        double base = rate(&pj, true, 0.3, &sink);
        // Marshalling: 8 lanes x varyings scattered in, outputs gathered out (what raster does around each batch).
        float aos[8][8][4], out[8][4];
        uint64_t n = 0;
        double t0 = now_s(), t;
        do {
            for (int i = 0; i < 2000; i++) {
                for (int l = 0; l < 8; l++)
                    for (uint32_t v = 0; v < pj.h.varying_count; v++)
                        for (int c = 0; c < 4; c++) batch.input[v][c][l] = aos[l][v][c] + (float)i;
                sink += fgpu_shader_run_batch(&pj, true, &batch, 0xFF, (const float (*)[4])uniforms, tex);
                for (int l = 0; l < 8; l++)
                    for (int c = 0; c < 4; c++) out[l][c] = batch.output[0][c][l];
                sink += (uint32_t)out[3][2];
            }
            n += 2000;
            t = now_s();
        } while (t - t0 < 0.3);
        double with = (double)n / (t - t0);
        printf("\nmarshalling (arith fragment, %s): bare batch %.1f ns, with AoS<->SoA in/out %.1f ns (%.0f%% of the time is the interface)\n",
               (sup & 2) ? "AVX2" : "SSE4.1", 1e9 / base, 1e9 / with, 100.0 * (1.0 / with - 1.0 / base) / (1.0 / with));
        fgpu_program_release(&pj);
    }
    printf("\nplan target (docs/farcade-engine-plan.md C4.7): >= 16 G lane-instr/s AVX2, ~0.6x of that with SSE4.1 (per the plan's reference machine, all cores)\n");
    return sink == 0xdeadbeef;
}
