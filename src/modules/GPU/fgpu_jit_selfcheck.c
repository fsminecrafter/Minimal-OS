#include "x86_64/fgpu_jit_x64.h"
#include "x86_64/allocator.h"
#include "x86_64/smp_par.h"
#include "serial.h"
#include "string.h"
#include "fglsl.h"

/*
 * Kernel self-check of the shader JIT ('gpu jit'): the same differential test the host
 * runs (tests/host/fgpu_jit_tests.c), in the real kernel, on the real CPU or under QEMU:
 * shaders compiled with the FGSL compiler, random inputs including special values,
 * interpreter against SSE4.1 JIT against AVX2 JIT, bit for bit (NaN as "both NaN").
 * The AVX2 code also runs on the helper cores of smp_par (inside the IPI handler, where
 * the entry stub saved the vector state), which checks the state handling for real.
 */

typedef struct { uint64_t s; } rng_t;
static uint32_t rnd(rng_t* g) {
    g->s ^= g->s << 13;
    g->s ^= g->s >> 7;
    g->s ^= g->s << 17;
    return (uint32_t)(g->s >> 16);
}
static float fbits(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
static uint32_t bitsf(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }

static const uint32_t k_special[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x40000000, 0x00000001, 0x007FFFFF,
    0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x4B000000, 0x4B189680,
    0x42FC0000, 0xC2FC0000, 0x43000000, 0x40490FDB, 0x3FC90FDB, 0x41200000, 0xC1200000,
};

static float rvalue(rng_t* g, uint32_t mode) {
    uint32_t r = rnd(g);
    if (mode == 0 && (r & 7) < 2) return fbits(k_special[(r >> 3) % (sizeof k_special / sizeof k_special[0])]);
    if (mode == 0 && (r & 7) == 2) return fbits(rnd(g));
    return -2.0f + 4.0f * (float)(rnd(g) & 0xFFFFFF) / 16777216.0f;
}

static const char* k_vs =
    "uniform mat4 mvp; uniform mat4 model;\n"
    "attribute vec3 position; attribute vec3 normal; attribute vec4 color; attribute vec2 uv;\n"
    "varying vec3 v_n; varying vec3 v_p; varying vec4 v_c; varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_p = (model * vec4(position, 1.0)).xyz;\n"
    "  v_n = normalize((model * vec4(normal, 0.0)).xyz);\n"
    "  v_c = color; v_uv = uv;\n"
    "  gl_Position = mvp * vec4(position, 1.0);\n"
    "}\n";
static const char* k_fs_cube =
    "uniform sampler2D tex; uniform vec3 light, eye, amb; uniform float time, textured;\n"
    "varying vec3 v_n; varying vec3 v_p; varying vec4 v_c; varying vec2 v_uv;\n"
    "vec3 checker(vec3 p) { float c = mod(floor(p.x) + floor(p.z), 2.0); return vec3(mix(0.62, 1.0, c)); }\n"
    "void main() {\n"
    "  vec3 n = normalize(v_n); vec3 l = -normalize(light); vec3 v = normalize(eye - v_p);\n"
    "  float diff = max(dot(n, l), 0.0);\n"
    "  float spec = pow(max(dot(n, normalize(l + v)), 0.0), 32.0) * 0.6 * textured;\n"
    "  vec3 base = v_c.rgb;\n"
    "  if (textured > 0.5) base *= texture(tex, v_uv).rgb; else base *= checker(v_p);\n"
    "  float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);\n"
    "  vec3 glow = vec3(0.3, 0.65, 1.0) * rim * (0.6 + 0.4 * sin(time * 3.0)) * textured;\n"
    "  float fog = smoothstep(4.0, 9.0, length(v_p - eye));\n"
    "  vec3 color = base * (amb + vec3(diff)) + vec3(spec) + glow;\n"
    "  gl_FragColor = vec4(mix(color, vec3(0.06, 0.08, 0.13), fog), 1.0);\n"
    "}\n";
static const char* k_fs_misc =
    "uniform vec4 k; uniform sampler2D tex;\n"
    "varying vec3 v_n; varying vec3 v_p; varying vec4 v_c; varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec4 a = vec4(sin(v_p.x * 3.0), cos(v_p.y), atan(v_p.x, v_p.y), exp(v_n.x));\n"
    "  vec4 b = vec4(log(abs(v_p.z) + 0.1), sqrt(abs(v_c.r)), inversesqrt(abs(v_c.g) + 0.01), sign(v_n.y));\n"
    "  vec4 c = vec4(floor(v_p.x * 2.0), fract(v_p.z * 3.0), mod(v_c.b * 5.0, 1.5), smoothstep(-1.0, 1.0, v_n.y));\n"
    "  vec4 t = texture(tex, v_uv * 3.0);\n"
    "  if (t.a < 0.25 && v_n.x > 0.5) discard;\n"
    "  gl_FragColor = a * 0.1 + b * 0.2 + c * 0.1 + t * k;\n"
    "}\n";

#define NPART 16u

typedef struct {
    fgpu_program_t* ref;        // interpreter only
    fgpu_program_t* jit;
    float (*uniforms)[4];
    const fgpu_texture_t* textures;
    uint32_t rounds;
    volatile uint32_t bad;
    volatile uint32_t batches;
} job_t;

static int same(float a, float b) {
    uint32_t x = bitsf(a), y = bitsf(b);
    return x == y || ((x & 0x7FFFFFFFu) > 0x7F800000u && (y & 0x7FFFFFFFu) > 0x7F800000u);
}

// One batch of the stage through both machines; returns mismatching outputs.
static uint32_t one_batch(job_t* j, rng_t* g, bool fragment, fgpu_shader_batch_t* rb, fgpu_shader_batch_t* jb,
                          uint32_t mode, uint32_t lanes) {
    uint32_t nin = fragment ? j->ref->h.varying_count : FGPU_MAX_ATTRIBS;
    for (uint32_t i = 0; i < nin; i++)
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 8; l++) rb->input[i][c][l] = rvalue(g, mode);
    for (int i = 0; i < 2; i++)
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 8; l++) rb->special[i][c][l] = i ? 1.0f : rvalue(g, mode);
    __builtin_memcpy(jb->input, rb->input, sizeof rb->input);
    __builtin_memcpy(jb->special, rb->special, sizeof rb->special);
    fgpu_shader_batch_reset(rb, j->ref->h.temp_count);
    fgpu_shader_batch_reset(jb, j->ref->h.temp_count);
    uint32_t kr = fgpu_shader_run_batch(j->ref, fragment, rb, lanes, (const float (*)[4])j->uniforms, j->textures);
    uint32_t kj = fgpu_shader_run_batch(j->jit, fragment, jb, lanes, (const float (*)[4])j->uniforms, j->textures);
    uint32_t bad = kr != kj;
    uint32_t outs = fragment ? 1u : 1u + j->ref->h.varying_count;
    for (uint32_t o = 0; o < outs; o++)
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 8; l++) {
                bool live = (kr >> l) & 1;
                bool asked = (lanes >> l) & 1;
                if (!asked || live) bad += !same(jb->output[o][c][l], rb->output[o][c][l]);
            }
    return bad;
}

static void part(void* ctx, uint32_t p, uint32_t worker) {
    (void)worker;
    job_t* j = (job_t*)ctx;
    // Each part has its own generator state and batches (heap: helper stacks are small).
    fgpu_shader_batch_t* rb = (fgpu_shader_batch_t*)alloc(sizeof(fgpu_shader_batch_t) * 2);
    if (!rb) { __atomic_fetch_add(&j->bad, 1, __ATOMIC_SEQ_CST); return; }
    fgpu_shader_batch_t* jb = rb + 1;
    uint32_t bad = 0;
    rng_t g = { 0x9E3779B97F4A7C15ull * (p + 1) + 12345 };
    for (uint32_t r = 0; r < j->rounds; r++) {
        uint32_t lanes = (r % 5 == 4) ? (rnd(&g) & 0xFF) : 0xFFu;
        bad += one_batch(j, &g, (r & 1) != 0, rb, jb, r % 3, lanes);
    }
    __atomic_fetch_add(&j->bad, bad, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&j->batches, j->rounds, __ATOMIC_SEQ_CST);
    free_mem(rb);
}

// say(text + decimal n)
static void say_n(void (*say)(const char*), const char* text, uint64_t n) {
    char buf[112];
    uint32_t k = 0;
    for (const char* t = text; *t && k < 90; t++) buf[k++] = *t;
    char d[24];
    int dn = 0;
    do { d[dn++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (dn) buf[k++] = d[--dn];
    buf[k] = 0;
    say(buf);
}
bool fgpu_jit_selfcheck(void (*say)(const char* line)) {
    uint32_t sup = fgpu_jit_cpu_support();
    if (!sup) {
        say("FGPU JIT selfcheck: SKIP (the shader JIT is off: nojit flag, or the CPU has no SSE4.1)");
        serial_write_str("FGPU JIT selfcheck: SKIP\n");
        return true;
    }
    say((sup & FGPU_JIT_CPU_AVX2) ? "FGPU JIT selfcheck: CPU supports SSE4.1 and AVX2"
                                  : "FGPU JIT selfcheck: CPU supports SSE4.1 (no usable AVX2)");
    fglsl_program_t* comp = (fglsl_program_t*)alloc(sizeof(fglsl_program_t));
    uint32_t* blob = (uint32_t*)alloc(FGLSL_MAX_BLOB_WORDS * 4);
    fgpu_program_t* progs = (fgpu_program_t*)alloc(sizeof(fgpu_program_t) * 2);
    float (*uniforms)[4] = (float (*)[4])alloc(sizeof(float) * 4 * FGPU_SHADER_MAX_UNIFORMS);
    uint32_t* texels = (uint32_t*)alloc(4 * 16 * 16);
    fgpu_shader_batch_t* rb = (fgpu_shader_batch_t*)alloc(sizeof(fgpu_shader_batch_t) * 2);
    if (!comp || !blob || !progs || !uniforms || !texels || !rb) {
        say("FGPU JIT selfcheck: FAIL (out of memory)");
        serial_write_str("FGPU JIT selfcheck: FAIL\n");
        return false;
    }
    rng_t g0 = { 0x1234567887654321ull };
    for (int i = 0; i < 16 * 16; i++) texels[i] = rnd(&g0);
    fgpu_texture_t tex[FGPU_MAX_TEXTURE_UNITS];
    for (int u = 0; u < (int)FGPU_MAX_TEXTURE_UNITS; u++) {
        tex[u].pixels = texels;
        tex[u].width = 16;
        tex[u].height = 16;
        tex[u].pitch = 16;
        tex[u].flags = FGPU_TEXTURE_LINEAR;
    }
    for (uint32_t i = 0; i < FGPU_SHADER_MAX_UNIFORMS; i++)
        for (int c = 0; c < 4; c++) uniforms[i][c] = rvalue(&g0, 1);
    static const char* const names[2] = { "cube-like", "misc-builtins" };
    const char* const fss[2] = { k_fs_cube, k_fs_misc };
    fgpu_jit_mode_t saved = fgpu_jit_get_mode();
    uint32_t total_bad = 0;
    uint64_t total_batches = 0;
    bool ok = true;
    for (int s = 0; s < 2 && ok; s++) {
        if (fglsl_compile(comp, k_vs, fss[s]) != 0) {
            say("FGPU JIT selfcheck: FAIL (shader does not compile)");
            ok = false;
            break;
        }
        __builtin_memcpy(blob, comp->words, comp->size);
        fgpu_program_t* ref = &progs[0];
        fgpu_program_t* jit = &progs[1];
        if (!fgpu_program_check(blob, comp->size, ref, NULL)) { say("FGPU JIT selfcheck: FAIL (check)"); ok = false; break; }
        for (int path = 0; path < 2; path++) {
            uint32_t bit = path ? FGPU_JIT_CPU_AVX2 : FGPU_JIT_CPU_SSE41;
            if (!(sup & bit)) continue;
            *jit = *ref;
            jit->jit = NULL;
            fgpu_jit_set_mode(path ? FGPU_JIT_AVX2 : FGPU_JIT_SSE41);
            if (!fgpu_program_jit(jit)) {
                say("FGPU JIT selfcheck: FAIL (shader did not compile)");
                ok = false;
                break;
            }
            fgpu_jit_info_t vi, fi;
            fgpu_program_jit_info(jit, false, &vi);
            fgpu_program_jit_info(jit, true, &fi);
            job_t* job = (job_t*)alloc(sizeof(job_t));
            job->ref = ref; job->jit = jit; job->uniforms = uniforms; job->textures = tex;
            job->rounds = 300; job->bad = 0; job->batches = 0;
            // On this core.
            part(job, 0, 0);
            // On every core that helps (the AVX2 / SSE code inside the IPI handler).
            uint32_t workers = smp_par_workers();
            job->rounds = 150;
            smp_par_run(part, job, NPART);
            say(names[s]);
            say_n(say, path ? "  AVX2: mismatches " : "  SSE4.1: mismatches ", job->bad);
            say_n(say, "    batches compared ", job->batches + 300);
            say_n(say, "    code bytes, vertex ", vi.code_bytes);
            say_n(say, "    code bytes, fragment ", fi.code_bytes);
            say_n(say, "    cores in the parallel part ", workers);
            total_bad += job->bad;
            total_batches += job->batches + 300;
            free_mem(job);
            fgpu_program_release(jit);
        }
    }
    fgpu_jit_set_mode(saved);
    free_mem(comp); free_mem(blob); free_mem(progs); free_mem(uniforms); free_mem(texels); free_mem(rb);
    bool pass = ok && total_bad == 0;
    say_n(say, pass ? "FGPU JIT selfcheck: PASS, batches compared: " : "FGPU JIT selfcheck: FAIL, mismatches: ",
          pass ? total_batches : total_bad);
    serial_write_str(pass ? "FGPU JIT selfcheck: PASS\n" : "FGPU JIT selfcheck: FAIL\n");
    return pass;
}
