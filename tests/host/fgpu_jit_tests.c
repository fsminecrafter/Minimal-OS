// Host tests for the shader JIT (fgpu_jit_x64.c): every result must match the
// CPU interpreter (fgpu_shader.c) bit for bit, on the SSE4.1 path and on the
// AVX2 path (each is skipped, with a message, when the host CPU lacks it).
//
//   per-opcode tests with edge-case operands (all pairs/triples of special
//   values, with swizzles, modifiers and write masks), control flow, textures,
//   real FGSL shaders compiled with the userland compiler (including the ones
//   fglcube uses), a differential fuzzer over random valid programs, safety
//   (the code writes only where it may) and negative tests (code buffer too
//   small, too many texture samples, shader too large: clean fallback).
//
// NaN results are compared as "both NaN": the sign and payload of a NaN that
// came out of arithmetic on two NaNs depends on operand order, which FGPU does
// not define. Everything else is compared bit for bit (-0, denormals, inf).
//
//   fgpu_jit_tests                run everything (a few seconds)
//   fgpu_jit_tests --soak SECS    only the fuzzer, for SECS seconds
//   fgpu_jit_tests --seed N       change the fuzzer seed
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "x86_64/fgpu_jit_x64.h"
#include "fglsl.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static uint32_t rndn(uint32_t n) { return rnd() % n; }
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

static float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* Operands that matter: zeros, denormals, huge, infinities, NaNs, powers of two,
 * rounding boundaries of floor/int conversion, the range edges of exp2/sin. */
static const uint32_t k_special_bits[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000, 0x40000000, 0xC0000000,
    0x40600000, 0xC0100000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x80800000,
    0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
    0x4B000000,     // 2^23
    0x4AFFFFFF,     // 2^23 - 0.5
    0xCB000000,
    0x4B800000,     // 2^24
    0x4B189680,     // 1e7
    0x4B18967F, 0x4C189680,
    0x42FC0000,     // 126
    0xC2FC0000,     // -126
    0x42FE0000,     // 127
    0x43000000,     // 128
    0xC2FE0000,
    0x40490FDB, 0xC0490FDB, 0x3FC90FDB, 0xBFC90FDB, 0x40C90FDB,     // pi, pi/2, 2pi
    0x477FFF00, 0x47800000,                                          // 65535, 65536
    0x3A83126F, 0x358637BD, 0x3DCCCCCD, 0x3EAAAAAB, 0x41200000, 0xC1200000, 0x42C80000,
};
#define NSPECIAL (sizeof k_special_bits / sizeof k_special_bits[0])

static float special(uint32_t i) { return bits_f(k_special_bits[i % NSPECIAL]); }

// A mix: mostly special values, otherwise ranges that exercise the functions.
static float any_value(void) {
    switch (rnd() & 7) {
        case 0: case 1: case 2: return special(rnd());
        case 3: return frand(-2.0f, 2.0f);
        case 4: return frand(-10.0f, 10.0f);
        case 5: return frand(-200.0f, 200.0f);
        case 6: return bits_f(rnd());
        default: return frand(-1.0f, 1.0f);
    }
}

/* -------------------------------------------------------- bit comparison */

typedef struct {
    uint64_t compared, nan_bit_diff;
} stats_t;
static stats_t g_stats;

static int same_bits(float a, float b) {
    uint32_t x = f_bits(a), y = f_bits(b);
    if (x == y) return 1;
    if (isnan(a) && isnan(b)) { g_stats.nan_bit_diff++; return 1; }
    return 0;
}

/* ------------------------------------------------------- program builder */

#define BLOB_WORDS 16384
typedef struct {
    uint32_t words[BLOB_WORDS];
    fgpu_program_t prog;        // interpreter only (jit == NULL)
    uint32_t vs[2048 * 5], fs[2048 * 5];
    uint32_t nvs, nfs, nconst;
    float consts[FGPU_SHADER_MAX_CONSTANTS][4];
    uint32_t temps, varyings;
} pb_t;

static pb_t pb;

static void pb_reset(void) {
    memset(&pb, 0, sizeof pb);
    pb.temps = 8;
    pb.varyings = 3;
}

static void pb_op(bool fragment, uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t* p = fragment ? &pb.fs[pb.nfs++ * 5] : &pb.vs[pb.nvs++ * 5];
    p[0] = op; p[1] = d; p[2] = a; p[3] = b; p[4] = c;
}

static bool pb_finish(const char** why) {
    memset(pb.words, 0, sizeof pb.words);
    fgpu_shader_header_t* h = (fgpu_shader_header_t*)pb.words;
    uint32_t hw = sizeof(*h) / 4;
    h->magic = FGPU_SHADER_MAGIC;
    h->version = FGPU_SHADER_VERSION;
    h->flags = FGPU_SHADER_FLAG_DISCARD;
    h->vs_offset = hw;
    h->vs_count = pb.nvs;
    h->fs_offset = hw + pb.nvs * 5;
    h->fs_count = pb.nfs;
    h->const_offset = h->fs_offset + pb.nfs * 5;
    h->const_count = pb.nconst;
    h->varying_count = pb.varyings;
    h->uniform_count = FGPU_SHADER_MAX_UNIFORMS;
    h->temp_count = pb.temps;
    h->texture_mask = 15;
    h->attrib_mask = 0xFF;
    memcpy(pb.words + h->vs_offset, pb.vs, pb.nvs * 20);
    memcpy(pb.words + h->fs_offset, pb.fs, pb.nfs * 20);
    memcpy(pb.words + h->const_offset, pb.consts, pb.nconst * 16);
    h->size = (h->const_offset + pb.nconst * 4) * 4;
    return fgpu_program_check(pb.words, h->size, &pb.prog, why);
}

#define SRC(file, idx, sw) FGPU_SRC(FGPU_FILE_##file, idx, sw)
#define DST(file, idx, mask) FGPU_DST(FGPU_FILE_##file, idx, mask)
#define XYZW FGPU_SWIZZLE_XYZW

/* ------------------------------------------------------------- the runner */

static fgpu_shader_batch_t ref_b, jit_b;
static float g_uniforms[FGPU_SHADER_MAX_UNIFORMS][4];
static fgpu_texture_t g_textures[FGPU_MAX_TEXTURE_UNITS];
static uint32_t g_texels[FGPU_MAX_TEXTURE_UNITS][16 * 16];
static const fgpu_jit_mode_t k_paths[2] = { FGPU_JIT_SSE41, FGPU_JIT_AVX2 };
static const char* const k_path_names[2] = { "SSE4.1", "AVX2" };
static bool g_have[2];
static uint64_t g_batches_run;

static void setup_textures(void) {
    static const uint32_t sizes[4][2] = { { 16, 16 }, { 8, 4 }, { 1, 1 }, { 5, 7 } };
    static const uint32_t flags[4] = { FGPU_TEXTURE_LINEAR, FGPU_TEXTURE_CLAMP, FGPU_TEXTURE_LINEAR | FGPU_TEXTURE_CLAMP, 0 };
    for (int u = 0; u < 4; u++) {
        for (int i = 0; i < 16 * 16; i++) g_texels[u][i] = rnd();
        g_textures[u].pixels = g_texels[u];
        g_textures[u].width = sizes[u][0];
        g_textures[u].height = sizes[u][1];
        g_textures[u].pitch = 16;
        g_textures[u].flags = flags[u];
    }
}

static void fill_inputs(bool fragment, int mode) {
    memset(&ref_b, 0, sizeof ref_b);
    memset(&jit_b, 0, sizeof jit_b);
    uint32_t nin = fragment ? pb.varyings : FGPU_MAX_ATTRIBS;
    for (uint32_t i = 0; i < nin; i++)
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 8; l++) {
                float v;
                if (mode == 1) v = frand(-2.0f, 2.0f);
                else if (mode == 2) v = frand(0.0f, 1.0f);
                else v = any_value();
                ref_b.input[i][c][l] = v;
            }
    if (fragment)
        for (int i = 0; i < 2; i++)
            for (int c = 0; c < 4; c++)
                for (int l = 0; l < 8; l++)
                    ref_b.special[i][c][l] = i == 1 ? (float)(rnd() & 1) : (mode ? frand(0.0f, 600.0f) : any_value());
    jit_b = ref_b;
}

static void fill_uniforms(int mode) {
    for (uint32_t r = 0; r < FGPU_SHADER_MAX_UNIFORMS; r++)
        for (int c = 0; c < 4; c++) g_uniforms[r][c] = mode == 1 ? frand(-2.0f, 2.0f) : (mode == 2 ? frand(0.0f, 1.0f) : any_value());
}

static int g_reported;

// Runs the stage through the interpreter and one JIT path; returns mismatches.
static int run_compare(const fgpu_jit_code_t* code, bool fragment, uint32_t lanes, const char* ctx,
                       const fgpu_program_t* prog) {
    fgpu_shader_batch_reset(&ref_b, prog->h.temp_count);
    fgpu_shader_batch_reset(&jit_b, prog->h.temp_count);
    fgpu_shader_batch_t in_copy = jit_b;
    uint32_t kr = fgpu_shader_run_batch(prog, fragment, &ref_b, lanes, (const float (*)[4])g_uniforms, g_textures);
    uint32_t kj = fgpu_jit_run_code(code, &jit_b, lanes, (const float (*)[4])g_uniforms, g_textures);
    g_batches_run++;
    int bad = 0;
    if (kr != kj) {
        if (g_reported++ < 8) printf("  %s: kept mask %02x, interpreter %02x\n", ctx, kj, kr);
        bad++;
    }
    uint32_t outs = fragment ? 1u : 1u + prog->h.varying_count;
    for (uint32_t o = 0; o < outs; o++)
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 8; l++) {
                bool live = (kr >> l) & 1;
                float want = ref_b.output[o][c][l], got = jit_b.output[o][c][l];
                g_stats.compared++;
                bool ok;
                if (live) ok = same_bits(got, want);
                else if (!((lanes >> l) & 1)) ok = f_bits(got) == f_bits(want);   // not asked for: defaults
                else ok = 1;                                                      // discarded: unspecified
                if (!ok) {
                    if (g_reported++ < 8)
                        printf("  %s: o%u.%c lane %d = %08x (%g), interpreter %08x (%g)\n", ctx, o, "xyzw"[c], l,
                               f_bits(got), got, f_bits(want), want);
                    bad++;
                }
            }
    // The code must leave inputs alone.
    if (memcmp(in_copy.input, jit_b.input, sizeof in_copy.input) || memcmp(in_copy.special, jit_b.special, sizeof in_copy.special)) {
        if (g_reported++ < 8) printf("  %s: JIT modified inputs\n", ctx);
        bad++;
    }
    return bad;
}

typedef struct { fgpu_jit_code_t* c[2][2]; } compiled_t;       // [path][stage]

static void compile_all(compiled_t* k, const fgpu_program_t* prog, const char* name) {
    memset(k, 0, sizeof *k);
    for (int p = 0; p < 2; p++) {
        if (!g_have[p]) continue;
        for (int s = 0; s < 2; s++) {
            const char* why = "";
            k->c[p][s] = fgpu_jit_compile_stage(prog, s == 1, k_paths[p], 0, &why);
            if (!k->c[p][s]) {
                printf("  %s: %s %s stage did not compile: %s\n", name, k_path_names[p], s ? "fragment" : "vertex", why);
            }
        }
    }
}

static void free_all(compiled_t* k) {
    for (int p = 0; p < 2; p++) for (int s = 0; s < 2; s++) fgpu_jit_free(k->c[p][s]);
}

// Both stages, all paths, `rounds` random batches.
static int compare_program(const fgpu_program_t* prog, const char* name, int rounds, bool lane_masks) {
    compiled_t k;
    compile_all(&k, prog, name);
    int bad = 0;
    for (int p = 0; p < 2; p++)
        for (int s = 0; s < 2; s++) {
            if (!g_have[p]) continue;
            if (!k.c[p][s]) { bad++; continue; }
            char ctx[160];
            snprintf(ctx, sizeof ctx, "%s %s %s", name, k_path_names[p], s ? "fragment" : "vertex");
            for (int r = 0; r < rounds; r++) {
                fill_uniforms(r % 4);
                fill_inputs(s == 1, r % 4);
                uint32_t lanes = lane_masks && (r % 5 == 4) ? (rnd() & 0xFF) : 0xFF;
                bad += run_compare(k.c[p][s], s == 1, lanes, ctx, prog);
            }
        }
    free_all(&k);
    return bad;
}

/* ------------------------------------------------------ per-opcode tests */

static const char* const k_op_names[FGPU_SOP_COUNT] = {
    "nop", "mov", "add", "sub", "mul", "div", "mad", "min", "max", "dp2", "dp3", "dp4", "cross", "floor",
    "fract", "sqrt", "rsq", "exp2", "log2", "pow", "sin", "cos", "mix", "clamp", "step", "sstep", "slt",
    "sle", "seq", "sne", "sel", "mod", "sign", "atan2", "tex", "kill", "jmp", "jz",
};

static uint32_t op_nsrc(uint32_t op) {
    switch (op) {
        case FGPU_SOP_MOV: case FGPU_SOP_FLOOR: case FGPU_SOP_FRACT: case FGPU_SOP_SQRT: case FGPU_SOP_RSQ:
        case FGPU_SOP_EXP2: case FGPU_SOP_LOG2: case FGPU_SOP_SIN: case FGPU_SOP_COS: case FGPU_SOP_SIGN:
            return 1;
        case FGPU_SOP_MAD: case FGPU_SOP_MIX: case FGPU_SOP_CLAMP: case FGPU_SOP_SSTEP: case FGPU_SOP_SEL:
            return 3;
        default:
            return 2;
    }
}

// op t0 = in0, in1, in2 (with `mods` on each source); out0 = t0, for both stages.
static void build_op_program(uint32_t op, uint32_t mods, uint32_t swz, uint32_t dmask) {
    pb_reset();
    for (int fs = 0; fs < 2; fs++) {
        uint32_t a = SRC(INPUT, 0, XYZW) | mods, b = SRC(INPUT, 1, swz) | mods, c = SRC(INPUT, 2, XYZW) | (mods ? mods ^ FGPU_SRC_ABS : 0);
        pb_op(fs, op, DST(TEMP, 0, dmask), a, b, c);
        pb_op(fs, FGPU_SOP_MOV, DST(OUTPUT, 0, 15), SRC(TEMP, 0, XYZW), 0, 0);
        if (!fs) {
            pb_op(0, FGPU_SOP_MOV, DST(OUTPUT, 1, 15), SRC(INPUT, 1, XYZW), 0, 0);
            pb_op(0, FGPU_SOP_MOV, DST(OUTPUT, 2, 15), SRC(TEMP, 0, FGPU_SWIZZLE(3, 2, 1, 0)), 0, 0);
            pb_op(0, FGPU_SOP_MOV, DST(OUTPUT, 3, 15), SRC(INPUT, 2, XYZW), 0, 0);
        }
    }
}

// Every pair (triple for three sources) of special values in lane x, other
// components shifted so swizzles see different values.
static int test_one_op(uint32_t op, uint32_t mods, uint32_t swz, uint32_t dmask) {
    build_op_program(op, mods, swz, dmask);
    const char* why = "";
    if (!pb_finish(&why)) { printf("FAIL: %s: program rejected: %s\n", k_op_names[op], why); return 1; }
    compiled_t k;
    char name[64];
    snprintf(name, sizeof name, "%s mods=%u", k_op_names[op], mods);
    compile_all(&k, &pb.prog, name);
    uint32_t n = NSPECIAL, nsrc = op_nsrc(op);
    uint32_t total = nsrc == 1 ? n : (nsrc == 2 ? n * n : n * n * n);
    int bad = 0;
    for (int p = 0; p < 2; p++)
        for (int s = 0; s < 2; s++) {
            if (!g_have[p]) continue;
            if (!k.c[p][s]) { bad++; continue; }
            char ctx[96];
            snprintf(ctx, sizeof ctx, "%s %s %s", name, k_path_names[p], s ? "fragment" : "vertex");
            fill_uniforms(1);
            for (uint32_t base = 0; base < total; base += 8) {
                memset(&ref_b, 0, sizeof ref_b);
                for (int l = 0; l < 8; l++) {
                    uint32_t idx = (base + (uint32_t)l) % total;
                    uint32_t ia = idx % n, ib = (idx / n) % n, ic = idx / (n * n);
                    if (nsrc == 1) ia = idx;
                    for (int c = 0; c < 4; c++) {
                        ref_b.input[0][c][l] = special(ia + (uint32_t)c * 7);
                        ref_b.input[1][c][l] = special(ib + (uint32_t)c * 3);
                        ref_b.input[2][c][l] = special(ic + (uint32_t)c * 5);
                        ref_b.special[0][c][l] = 0;
                    }
                }
                jit_b = ref_b;
                bad += run_compare(k.c[p][s], s == 1, 0xFF, ctx, &pb.prog);
            }
        }
    free_all(&k);
    return bad;
}

static void test_per_opcode(void) {
    static const uint32_t mods[] = { 0, FGPU_SRC_NEGATE, FGPU_SRC_ABS, FGPU_SRC_ABS | FGPU_SRC_NEGATE };
    for (uint32_t op = FGPU_SOP_MOV; op < FGPU_SOP_TEX; op++) {
        int bad = 0;
        // Plain, with each modifier combination (cheap ops only the plain form for 3-source ops).
        for (uint32_t m = 0; m < 4; m++) {
            if (op_nsrc(op) == 3 && m != 0 && m != 3) continue;
            bad += test_one_op(op, mods[m], FGPU_SWIZZLE(1, 0, 3, 2), 15);
        }
        bad += test_one_op(op, 0, FGPU_SWIZZLE(0, 0, 0, 0), 5);       // write mask xz, broadcast swizzle
        if (bad) { printf("FAIL: opcode %s: %d mismatches\n", k_op_names[op], bad); failures++; }
    }
    printf("per-opcode: %u opcodes x modifiers/swizzles/masks, %llu batches so far\n", FGPU_SOP_TEX - 1,
           (unsigned long long)g_batches_run);
}

/* ------------------------------------------------------------ control flow */

static void test_control_flow(void) {
    // Divergent forward jumps, nested; KILL inside and outside of jumped ranges; JZ on NaN.
    pb_reset();
    for (int fs = 0; fs < 2; fs++) {
        const uint32_t X = FGPU_SWIZZLE(0, 0, 0, 0), Y = FGPU_SWIZZLE(1, 1, 1, 1);
        pb_op(fs, FGPU_SOP_MOV, DST(TEMP, 0, 15), SRC(INPUT, 0, XYZW), 0, 0);               // 0
        pb_op(fs, FGPU_SOP_JZ, 6, SRC(INPUT, 1, X), 0, 0);                                  // 1: if in1.x == 0 skip to 6
        pb_op(fs, FGPU_SOP_ADD, DST(TEMP, 0, 15), SRC(TEMP, 0, XYZW), SRC(INPUT, 2, XYZW), 0); // 2
        pb_op(fs, FGPU_SOP_JZ, 5, SRC(INPUT, 2, Y), 0, 0);                                  // 3: nested
        pb_op(fs, FGPU_SOP_MUL, DST(TEMP, 0, 15), SRC(TEMP, 0, XYZW), SRC(INPUT, 0, XYZW), 0); // 4
        pb_op(fs, FGPU_SOP_SUB, DST(TEMP, 1, 15), SRC(TEMP, 0, XYZW), SRC(INPUT, 1, XYZW), 0); // 5
        if (fs) pb_op(fs, FGPU_SOP_KILL, 0, SRC(INPUT, 0, Y), 0, 0);                        // 6 (fragment): kill if in0.y != 0
        pb_op(fs, FGPU_SOP_JMP, fs ? 10 : 9, 0, 0, 0);
        pb_op(fs, FGPU_SOP_MOV, DST(TEMP, 0, 15), SRC(INPUT, 2, XYZW), 0, 0);
        pb_op(fs, FGPU_SOP_MOV, DST(TEMP, 1, 3), SRC(INPUT, 2, XYZW), 0, 0);
        pb_op(fs, FGPU_SOP_ADD, DST(OUTPUT, 0, 15), SRC(TEMP, 0, XYZW), SRC(TEMP, 1, XYZW), 0);
        if (!fs) pb_op(0, FGPU_SOP_MOV, DST(OUTPUT, 1, 15), SRC(TEMP, 1, XYZW), 0, 0);
    }
    const char* why = "";
    CHECK(pb_finish(&why), "control flow program builds");
    int bad = compare_program(&pb.prog, "control flow", 400, true);
    if (bad) { printf("FAIL: control flow: %d mismatches\n", bad); failures++; }
}

/* ---------------------------------------------------------------- textures */

static void test_textures(void) {
    pb_reset();
    for (int fs = 0; fs < 2; fs++) {
        for (uint32_t u = 0; u < 4; u++) {
            pb_op(fs, FGPU_SOP_TEX, DST(TEMP, u, 15), SRC(INPUT, u % 3, XYZW), u, 0);
            // A dependent sample, with modifiers.
            pb_op(fs, FGPU_SOP_TEX, DST(TEMP, 4 + u, 15), SRC(TEMP, u, FGPU_SWIZZLE(3, 1, 0, 0)) | FGPU_SRC_NEGATE, (u + 1) & 3, 0);
        }
        for (uint32_t u = 0; u < 4; u++)
            pb_op(fs, FGPU_SOP_ADD, DST(TEMP, u, 15), SRC(TEMP, u, XYZW), SRC(TEMP, 4 + u, XYZW), 0);
        pb_op(fs, FGPU_SOP_ADD, DST(TEMP, 0, 15), SRC(TEMP, 0, XYZW), SRC(TEMP, 1, XYZW), 0);
        pb_op(fs, FGPU_SOP_ADD, DST(TEMP, 2, 15), SRC(TEMP, 2, XYZW), SRC(TEMP, 3, XYZW), 0);
        pb_op(fs, FGPU_SOP_ADD, DST(OUTPUT, 0, 15), SRC(TEMP, 0, XYZW), SRC(TEMP, 2, XYZW), 0);
        if (!fs) pb_op(0, FGPU_SOP_MOV, DST(OUTPUT, 1, 15), SRC(TEMP, 5, XYZW), 0, 0);
    }
    const char* why = "";
    CHECK(pb_finish(&why), "texture program builds");
    int bad = compare_program(&pb.prog, "textures", 300, true);
    // No textures bound: the interpreter returns (0, 0, 0, 1).
    compiled_t k;
    compile_all(&k, &pb.prog, "textures-none");
    for (int p = 0; p < 2; p++) {
        if (!g_have[p] || !k.c[p][1]) continue;
        fill_uniforms(1);
        fill_inputs(true, 1);
        fgpu_shader_batch_reset(&ref_b, 8);
        fgpu_shader_batch_reset(&jit_b, 8);
        uint32_t kr = fgpu_shader_run_batch(&pb.prog, true, &ref_b, 0xFF, (const float (*)[4])g_uniforms, NULL);
        uint32_t kj = fgpu_jit_run_code(k.c[p][1], &jit_b, 0xFF, (const float (*)[4])g_uniforms, NULL);
        CHECK(kr == kj, "no textures: same kept mask");
        for (int c = 0; c < 4; c++) for (int l = 0; l < 8; l++) bad += !same_bits(jit_b.output[0][c][l], ref_b.output[0][c][l]);
    }
    free_all(&k);
    if (bad) { printf("FAIL: textures: %d mismatches\n", bad); failures++; }
}

/* ------------------------------------------------------- FGSL shaders */

static const char* k_vs =
    "uniform mat4 mvp;\nuniform mat4 model;\nattribute vec3 position;\nattribute vec3 normal;\n"
    "attribute vec4 color;\nattribute vec2 uv;\nvarying vec3 v_n;\nvarying vec3 v_p;\nvarying vec4 v_c;\n"
    "varying vec2 v_uv;\nvoid main() {\n  vec4 wp = model * vec4(position, 1.0);\n  v_p = wp.xyz;\n"
    "  v_n = normalize((model * vec4(normal, 0.0)).xyz);\n  v_c = color;\n  v_uv = uv;\n"
    "  gl_Position = mvp * vec4(position, 1.0);\n}\n";
static const char* k_varyings = "varying vec3 v_n;\nvarying vec3 v_p;\nvarying vec4 v_c;\nvarying vec2 v_uv;\n";
static char fs_buf[10][4096];
static const char* mkfs(int slot, const char* decls, const char* body) {
    snprintf(fs_buf[slot], sizeof fs_buf[slot], "%s%s\nvoid main() {\n%s\n}\n", k_varyings, decls, body);
    return fs_buf[slot];
}

// The shaders fglcube uses (Minimal-OS-SDK/programs/fglcube/main.c).
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

typedef struct { const char* name; const char* vs; const char* fs; } shader_case_t;
static fglsl_program_t comp;

static int compare_source(const shader_case_t* sc, int rounds) {
    if (fglsl_compile(&comp, sc->vs, sc->fs) != 0) { printf("FAIL: %s: compile error: %s\n", sc->name, comp.error); return 1; }
    static uint32_t blob[FGLSL_MAX_BLOB_WORDS];
    memcpy(blob, comp.words, comp.size);
    static fgpu_program_t prog;
    const char* why = "";
    if (!fgpu_program_check(blob, comp.size, &prog, &why)) { printf("FAIL: %s: check: %s\n", sc->name, why); return 1; }
    pb.varyings = prog.h.varying_count;
    return compare_program(&prog, sc->name, rounds, true);
}

static void test_fgsl(void) {
    shader_case_t cases[] = {
        { "fglcube", k_cube_vs, k_cube_fs },
        { "phong", k_vs, mkfs(0,
            "uniform vec3 light; uniform vec3 eye; uniform float shininess;",
            "  vec3 n = normalize(v_n);\n  vec3 l = normalize(-light);\n  float d = max(dot(n, l), 0.0);\n"
            "  vec3 r = reflect(-l, n);\n"
            "  float s = pow(max(dot(r, normalize(eye - v_p)), 0.0), 8.0 + abs(shininess) * 16.0);\n"
            "  gl_FragColor = vec4(v_c.rgb * (0.2 + d) + vec3(s), 1.0);") },
        { "builtins", k_vs, mkfs(1,
            "uniform vec4 k;",
            "  vec4 a = vec4(sin(v_p.x * 3.0), cos(v_p.y), tan(v_n.z * 0.5), atan(v_p.x, v_p.y));\n"
            "  vec4 b = vec4(exp(v_n.x), log(abs(v_p.z) + 0.1), sqrt(abs(v_c.r)), inversesqrt(abs(v_c.g) + 0.01));\n"
            "  vec4 c = vec4(sign(v_p), atan(v_c.a));\n"
            "  vec4 d = vec4(floor(v_p.x * 2.0), ceil(v_p.y), fract(v_p.z * 3.0), mod(v_c.b * 5.0, 1.5));\n"
            "  vec4 e = vec4(clamp(v_c.r, -0.5, 0.5), step(0.0, v_n.x), smoothstep(-1.0, 1.0, v_n.y), mix(v_p.x, v_p.y, 0.25));\n"
            "  vec3 f = cross(v_n, v_p) + vec3(length(v_p), distance(v_p, v_n), min(k.x, k.y));\n"
            "  gl_FragColor = a * 0.1 + b * 0.2 + c * 0.3 + d * 0.05 + e + vec4(f, max(k.z, k.w)) * 0.1;") },
        { "pow-edge", k_vs, mkfs(2,
            "uniform vec4 k;",
            "  gl_FragColor = vec4(pow(v_p.x, 2.0), pow(k.x, v_p.y), log2(v_p.z), sqrt(v_n.x)) +\n"
            "                 vec4(inversesqrt(v_n.y), v_p.x / v_n.z, mod(v_p.y, 0.0), smoothstep(k.x, k.x, v_p.z));") },
        { "branches", k_vs, mkfs(3,
            "uniform float cut;",
            "  vec4 c = v_c;\n  if (v_p.x > 0.0) {\n    c.r = c.r * 2.0;\n"
            "    if (v_p.y > 0.5) c.g = 1.0; else { c.g = 0.0; c.b = v_n.x; }\n  } else {\n"
            "    c = vec4(v_n, 1.0);\n    if (v_n.z < -0.5) discard;\n  }\n  if (c.a < cut) discard;\n  gl_FragColor = c;") },
        { "loops", k_vs, mkfs(4,
            "uniform vec4 w[4];",
            "  vec3 acc = vec3(0.0);\n  for (int i = 0; i < 4; i++) {\n    if (dot(w[i].xyz, v_n) < 0.0) continue;\n"
            "    acc += w[i].xyz * w[i].w;\n    if (acc.x > 1.0) break;\n  }\n"
            "  gl_FragColor = vec4(acc, v_p.x > 0.0 ? 1.0 : 0.5);") },
        { "texture", k_vs, mkfs(5,
            "uniform sampler2D tex; uniform sampler2D tex2;",
            "  vec4 t = texture(tex, v_uv * 3.0);\n  vec4 u = texture2D(tex2, v_uv.yx + vec2(0.1));\n"
            "  if (t.a < 0.3) discard;\n  gl_FragColor = vec4(t.rgb * u.a, u.r);") },
        { "fragcoord", k_vs, mkfs(6,
            "",
            "  vec2 p = gl_FragCoord.xy / vec2(640.0, 480.0);\n"
            "  float checker = mod(floor(gl_FragCoord.x / 8.0) + floor(gl_FragCoord.y / 8.0), 2.0);\n"
            "  gl_FragColor = gl_FrontFacing ? vec4(p, checker, gl_FragCoord.w) : vec4(gl_FragCoord.z);") },
        { "bilinear-blur", k_vs, mkfs(7,
            "uniform sampler2D tex; uniform vec2 texel;",
            "  vec4 c = texture(tex, v_uv) * 0.4 + texture(tex, v_uv + vec2(texel.x, 0.0)) * 0.2 +\n"
            "           texture(tex, v_uv - vec2(texel.x, 0.0)) * 0.2 + texture(tex, v_uv + vec2(0.0, texel.y)) * 0.2;\n"
            "  gl_FragColor = vec4(c.rgb * v_c.rgb, 1.0);") },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int bad = compare_source(&cases[i], 400);
        if (bad) { printf("FAIL: FGSL %s: %d mismatches\n", cases[i].name, bad); failures++; }
    }
    printf("FGSL: %u real shaders compared, %llu batches so far\n",
           (unsigned)(sizeof cases / sizeof cases[0]), (unsigned long long)g_batches_run);
}

/* ------------------------------------------------------------- the fuzzer */

static uint32_t rnd_src(bool fragment, uint32_t nsrc_class) {
    (void)nsrc_class;
    uint32_t file, index;
    uint32_t r = rndn(100);
    if (r < 38) { file = FGPU_FILE_TEMP; index = rndn(pb.temps); }
    else if (r < 62) { file = FGPU_FILE_INPUT; index = fragment ? rndn(pb.varyings) : rndn(FGPU_MAX_ATTRIBS); }
    else if (r < 77) { file = FGPU_FILE_UNIFORM; index = rndn(12); }
    else if (r < 90) { file = FGPU_FILE_CONST; index = rndn(pb.nconst); }
    else if (r < 95 && fragment) { file = FGPU_FILE_SPECIAL; index = rndn(2); }
    else { file = FGPU_FILE_OUTPUT; index = fragment ? 0 : rndn(1 + pb.varyings); }
    uint32_t sw = rndn(3) ? XYZW : (rnd() & 0xFF);
    if (rndn(5) == 0) sw = FGPU_SWIZZLE(rndn(4), rndn(4), rndn(4), rndn(4));
    uint32_t w = FGPU_SRC(file, index, sw);
    if (rndn(6) == 0) w |= FGPU_SRC_ABS;
    if (rndn(6) == 0) w |= FGPU_SRC_NEGATE;
    return w;
}

static void gen_stage(bool fragment, uint32_t count, bool allow_tex) {
    uint32_t outs = fragment ? 1u : 1u + pb.varyings;
    uint32_t body = count > outs ? count - outs : count;
    uint32_t start = fragment ? pb.nfs : pb.nvs;
    for (uint32_t pc = 0; pc < body; pc++) {
        uint32_t cur = start + pc;
        uint32_t r = rndn(100), op;
        if (r < 4 && cur + 2 < start + body) op = FGPU_SOP_JMP;
        else if (r < 9 && cur + 2 < start + body) op = FGPU_SOP_JZ;
        else if (r < 11 && fragment) op = FGPU_SOP_KILL;
        else if (r < 16 && allow_tex) op = FGPU_SOP_TEX;
        else op = 1 + rndn(FGPU_SOP_ATAN2);
        uint32_t file = rndn(4) ? FGPU_FILE_TEMP : FGPU_FILE_OUTPUT;
        uint32_t index = file == FGPU_FILE_TEMP ? rndn(pb.temps) : (fragment ? 0 : rndn(outs));
        uint32_t mask = rndn(3) ? 15 : 1 + rndn(15);
        uint32_t d = FGPU_DST(file, index, mask);
        if (op == FGPU_SOP_JMP || op == FGPU_SOP_JZ) {
            // Targets are instruction numbers within the stage; forward only, may reach the end.
            uint32_t target = pc + 1 + rndn(body - pc);
            d = target + (start - start);
            pb_op(fragment, op, d, op == FGPU_SOP_JZ ? rnd_src(fragment, 1) : 0, 0, 0);
        } else if (op == FGPU_SOP_KILL) {
            pb_op(fragment, op, 0, rnd_src(fragment, 1), 0, 0);
        } else if (op == FGPU_SOP_TEX) {
            pb_op(fragment, op, d, rnd_src(fragment, 1), rndn(4), 0);
        } else {
            pb_op(fragment, op, d, rnd_src(fragment, 1), rnd_src(fragment, 2), rnd_src(fragment, 3));
        }
    }
    // Make the outputs depend on the temporaries.
    for (uint32_t o = 0; o < outs; o++)
        pb_op(fragment, FGPU_SOP_MOV, FGPU_DST(FGPU_FILE_OUTPUT, o, 15),
              rndn(3) ? FGPU_SRC(FGPU_FILE_TEMP, rndn(pb.temps), XYZW) : rnd_src(fragment, 1), 0, 0);
}

static void gen_program(bool allow_tex) {
    pb_reset();
    pb.nconst = 1 + rndn(8);
    for (uint32_t i = 0; i < pb.nconst; i++)
        for (int c = 0; c < 4; c++) pb.consts[i][c] = rndn(2) ? special(rnd()) : frand(-3.0f, 3.0f);
    pb.temps = 2 + rndn(10);
    pb.varyings = 1 + rndn(4);
    gen_stage(false, 4 + rndn(40), allow_tex);
    gen_stage(true, 4 + rndn(40), allow_tex);
}

static uint64_t fuzz_programs, fuzz_failed;

static void dump_stage(const char* title, const uint32_t* code, uint32_t n) {
    static const char* const files[] = { "-", "r", "v", "o", "u", "c", "s" };
    printf("  %s:\n", title);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t* w = code + i * 5;
        printf("   %2u %-5s", i, k_op_names[w[0]]);
        if (w[0] == FGPU_SOP_JMP || w[0] == FGPU_SOP_JZ) printf(" ->%u", w[1]);
        else if (w[0] != FGPU_SOP_KILL) printf(" %s%u.%x", files[FGPU_OPERAND_FILE(w[1])], FGPU_OPERAND_INDEX(w[1]), FGPU_DST_MASK(w[1]));
        for (int k = 0; k < 3; k++) {
            if (w[0] == FGPU_SOP_TEX && k == 1) { printf("  unit%u", w[3]); continue; }
            if (w[2 + k] == 0 && k) continue;
            uint32_t x = w[2 + k];
            printf("  %s%s%s%u.", (x & FGPU_SRC_NEGATE) ? "-" : "", (x & FGPU_SRC_ABS) ? "|" : "", files[FGPU_OPERAND_FILE(x) % 7], FGPU_OPERAND_INDEX(x));
            for (int c = 0; c < 4; c++) printf("%c", "xyzw"[(FGPU_OPERAND_SWIZZLE(x) >> (2 * c)) & 3]);
        }
        printf("\n");
    }
}

// Greedy shrinking of a failing program (FGPU_JIT_DUMP): drops instructions
// while the mismatch stays. Jump targets are adjusted.
static int try_program(int rounds, uint64_t seed) {
    uint64_t save = rng_state;
    rng_state = seed;
    const char* why = "";
    int bad = 0;
    int rep = g_reported;
    g_reported = 100;
    if (pb_finish(&why)) bad = compare_program(&pb.prog, "shrink", rounds, true);
    g_reported = rep;
    rng_state = save;
    return bad;
}

static void drop(uint32_t* code, uint32_t* n, uint32_t j) {
    for (uint32_t pc = 0; pc < *n; pc++) {
        uint32_t* w = code + pc * 5;
        if ((w[0] == FGPU_SOP_JMP || w[0] == FGPU_SOP_JZ) && pc != j && w[1] > j) w[1]--;
    }
    memmove(code + j * 5, code + (j + 1) * 5, (*n - j - 1) * 5 * sizeof(uint32_t));
    (*n)--;
}

static void shrink(int rounds, uint64_t seed) {
    for (int fs = 0; fs < 2; fs++) {
        uint32_t* code = fs ? pb.fs : pb.vs;
        uint32_t* n = fs ? &pb.nfs : &pb.nvs;
        for (uint32_t j = 0; j < *n;) {
            uint32_t saved[2048 * 5], sn = *n;
            memcpy(saved, code, sn * 20);
            drop(code, n, j);
            // jumps must still be forward and inside
            bool ok = *n > 0;
            for (uint32_t pc = 0; pc < *n && ok; pc++) {
                uint32_t* w = code + pc * 5;
                if ((w[0] == FGPU_SOP_JMP || w[0] == FGPU_SOP_JZ) && (w[1] <= pc || w[1] > *n)) ok = false;
            }
            if (ok && try_program(rounds, seed)) continue;
            memcpy(code, saved, sn * 20);
            *n = sn;
            j++;
        }
    }
}

static int fuzz_one(uint32_t id, int rounds) {
    gen_program(rndn(4) == 0);
    const char* why = "";
    if (!pb_finish(&why)) { printf("fuzz %u: generated program rejected: %s\n", id, why); return 1; }
    char name[48];
    snprintf(name, sizeof name, "fuzz#%u", id);
    fuzz_programs++;
    g_reported = 0;
    uint64_t seed = rng_state;
    int bad = compare_program(&pb.prog, name, rounds, true);
    if (bad && getenv("FGPU_JIT_DUMP")) {
        if (try_program(rounds, seed)) {
            shrink(rounds, seed);
            uint64_t sv = rng_state;
            rng_state = seed;
            g_reported = 0;
            if (pb_finish(&why)) compare_program(&pb.prog, "minimal", rounds, true);
            rng_state = sv;
        } else printf("  (does not reproduce with a replayed seed)\n");
        dump_stage("vertex", pb.vs, pb.nvs);
        dump_stage("fragment", pb.fs, pb.nfs);
        for (uint32_t i = 0; i < pb.nconst; i++) printf("  const%u = %g %g %g %g\n", i, pb.consts[i][0], pb.consts[i][1], pb.consts[i][2], pb.consts[i][3]);
    }
    return bad;
}

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

static void test_fuzz(uint32_t iterations, double seconds) {
    setup_textures();
    double t0 = now_s();
    uint32_t i = 0;
    int bad = 0;
    for (;;) {
        if (seconds > 0) { if (now_s() - t0 >= seconds) break; }
        else if (i >= iterations) break;
        int b = fuzz_one(i++, 6);
        if (b) { fuzz_failed++; bad += b; if (fuzz_failed >= 5) break; }
    }
    if (bad) { printf("FAIL: fuzzer: %d mismatches in %llu programs\n", bad, (unsigned long long)fuzz_failed); failures++; }
    printf("fuzz: %llu random programs, %llu batches in total, %llu outputs compared (%llu NaN with other bits), %llu failing programs\n",
           (unsigned long long)fuzz_programs, (unsigned long long)g_batches_run, (unsigned long long)g_stats.compared,
           (unsigned long long)g_stats.nan_bit_diff, (unsigned long long)fuzz_failed);
}

/* ----------------------------------------------------------------- safety */

// The compiled code may write the batch's output and temp rows and nothing else.
static void test_safety(void) {
    static const shader_case_t sc = { "fglcube", NULL, NULL };
    (void)sc;
    if (fglsl_compile(&comp, k_cube_vs, k_cube_fs) != 0) { printf("FAIL: cube compile\n"); failures++; return; }
    static uint32_t blob[FGLSL_MAX_BLOB_WORDS];
    memcpy(blob, comp.words, comp.size);
    static fgpu_program_t prog;
    const char* why = "";
    CHECK(fgpu_program_check(blob, comp.size, &prog, &why), "cube checks");
    for (int p = 0; p < 2; p++) {
        if (!g_have[p]) continue;
        for (int s = 0; s < 2; s++) {
            fgpu_jit_code_t* c = fgpu_jit_compile_stage(&prog, s == 1, k_paths[p], 0, &why);
            CHECK(c != NULL, "cube compiles");
            if (!c) continue;
            // Guards before and after the batch and the uniforms.
            struct { uint8_t pre[256]; fgpu_shader_batch_t b; uint8_t post[256]; } *w = malloc(sizeof *w);
            struct { uint8_t pre[256]; float u[FGPU_SHADER_MAX_UNIFORMS][4]; uint8_t post[256]; } *wu = malloc(sizeof *wu);
            memset(w, 0xA5, sizeof *w);
            memset(wu, 0x5A, sizeof *wu);
            for (int r = 0; r < 64; r++) for (int cc = 0; cc < 4; cc++) wu->u[r][cc] = frand(-2, 2);
            fgpu_shader_batch_t before;
            for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++) for (int cc = 0; cc < 4; cc++) for (int l = 0; l < 8; l++) w->b.input[i][cc][l] = frand(-2, 2);
            before = w->b;
            uint8_t pre[256], post[256], upre[256], upost[256];
            float ucopy[FGPU_SHADER_MAX_UNIFORMS][4];
            memcpy(pre, w->pre, 256); memcpy(post, w->post, 256);
            memcpy(upre, wu->pre, 256); memcpy(upost, wu->post, 256);
            memcpy(ucopy, wu->u, sizeof ucopy);
            fgpu_jit_run_code(c, &w->b, 0xFF, (const float (*)[4])wu->u, g_textures);
            CHECK(!memcmp(pre, w->pre, 256) && !memcmp(post, w->post, 256), "no write outside the batch");
            CHECK(!memcmp(upre, wu->pre, 256) && !memcmp(upost, wu->post, 256) && !memcmp(ucopy, wu->u, sizeof ucopy), "uniforms untouched");
            CHECK(!memcmp(before.input, w->b.input, sizeof before.input) && !memcmp(before.special, w->b.special, sizeof before.special),
                  "inputs untouched");
            free(w);
            free(wu);
            fgpu_jit_free(c);
        }
    }
}

/* --------------------------------------------------------- negative tests */

static void test_negative(void) {
    const char* why = "";
    // 1. Code buffer too small: clean failure.
    if (fglsl_compile(&comp, k_cube_vs, k_cube_fs) != 0) { printf("FAIL: cube compile\n"); failures++; return; }
    static uint32_t blob[FGLSL_MAX_BLOB_WORDS];
    memcpy(blob, comp.words, comp.size);
    static fgpu_program_t prog;
    CHECK(fgpu_program_check(blob, comp.size, &prog, &why), "cube checks");
    for (int p = 0; p < 2; p++) {
        if (!g_have[p]) continue;
        why = "";
        fgpu_jit_code_t* c = fgpu_jit_compile_stage(&prog, true, k_paths[p], 64, &why);
        CHECK(c == NULL, "64 byte code buffer: compile fails");
        CHECK(strstr(why, "code buffer") != NULL, "failure reason names the code buffer");
        for (uint32_t lim = 1; lim < 2000; lim += 37) {          // every size: fails cleanly or works
            c = fgpu_jit_compile_stage(&prog, true, k_paths[p], lim, &why);
            if (c) { fgpu_jit_free(c); break; }
        }
    }
    // 2. A program with a program-level handle: jit stays NULL when compile fails; the interpreter runs.
    fgpu_jit_mode_t saved = fgpu_jit_get_mode();
    fgpu_jit_set_mode(FGPU_JIT_OFF);
    fgpu_program_t off = prog;
    CHECK(!fgpu_program_jit(&off) && off.jit == NULL, "mode OFF compiles nothing");
    uint32_t kept = 0;
    fgpu_shader_batch_t* b = &ref_b;
    CHECK(!fgpu_jit_run_batch(&off, true, b, 0xFF, NULL, NULL, &kept), "no code: run_batch returns false");
    fgpu_jit_set_mode(saved);

    // 3. Too many dependent texture samples: the stage does not compile, the program falls back.
    why = "";
    {
        pb_reset();
        pb.nconst = 1;
        pb_op(0, FGPU_SOP_MOV, DST(OUTPUT, 0, 15), SRC(INPUT, 0, XYZW), 0, 0);
        for (uint32_t i = 0; i < 20; i++) {
            pb.consts[0][0] = 0.1f;
            pb_op(1, FGPU_SOP_ADD, DST(TEMP, 1, 15), SRC(INPUT, 0, XYZW), SRC(CONST, 0, XYZW), 0);
            pb_op(1, FGPU_SOP_MUL, DST(TEMP, 1, 15), SRC(TEMP, 1, XYZW), SRC(TEMP, 3, XYZW), 0);
            pb_op(1, FGPU_SOP_MOV, DST(TEMP, 3, 15), SRC(TEMP, 2, XYZW), 0, 0);
            pb_op(1, FGPU_SOP_TEX, DST(TEMP, 2, 15), SRC(TEMP, 1, XYZW), i & 3, 0);
            pb_op(1, FGPU_SOP_ADD, DST(OUTPUT, 0, 15), SRC(OUTPUT, 0, XYZW), SRC(TEMP, 2, XYZW), 0);
        }
        CHECK(pb_finish(&why), "chained texture program builds");
        for (int p = 0; p < 2; p++) {
            if (!g_have[p]) continue;
            fgpu_jit_code_t* c = fgpu_jit_compile_stage(&pb.prog, true, k_paths[p], 0, &why);
            CHECK(c == NULL && strstr(why, "texture"), "20 dependent texture samples: clean failure");
            fgpu_jit_free(c);
        }
        // Through the program interface: the failed stage keeps the interpreter, results stay right.
        if (g_have[0] || g_have[1]) {
            fgpu_jit_set_mode(g_have[1] ? FGPU_JIT_AVX2 : FGPU_JIT_SSE41);
            fgpu_program_t jp = pb.prog;
            bool any = fgpu_program_jit(&jp);
            CHECK(any, "vertex stage of the texture-heavy program still compiles");
            fgpu_jit_info_t info;
            CHECK(fgpu_program_jit_info(&jp, false, &info) && !fgpu_program_jit_info(&jp, true, &info), "only the vertex stage has code");
            setup_textures();
            fill_uniforms(1);
            fill_inputs(true, 1);
            fgpu_shader_batch_t* ba = &ref_b;
            fgpu_shader_batch_reset(ba, 8);
            uint32_t k1 = fgpu_shader_run_batch(&pb.prog, true, ba, 0xFF, (const float (*)[4])g_uniforms, g_textures);
            fgpu_shader_batch_t other = jit_b;
            fgpu_shader_batch_reset(&other, 8);
            uint32_t k2 = fgpu_shader_run_batch(&jp, true, &other, 0xFF, (const float (*)[4])g_uniforms, g_textures);
            CHECK(k1 == k2 && !memcmp(ba->output, other.output, sizeof ba->output), "fallback stage gives the interpreter's result");
            fgpu_program_release(&jp);
            CHECK(jp.jit == NULL, "release clears the handle");
            fgpu_jit_set_mode(saved);
        }
    }

    // 4. Shader too large for the IR: clean failure.
    pb_reset();
    pb.nconst = 1;
    pb.consts[0][0] = 1.0f;
    for (int fs = 0; fs < 2; fs++) {
        for (uint32_t i = 0; i < 2000; i++)
            pb_op(fs, FGPU_SOP_ATAN2, DST(TEMP, i % 8, 15), SRC(TEMP, (i + 1) % 8, XYZW), SRC(INPUT, 0, XYZW), 0);
        pb_op(fs, FGPU_SOP_MOV, DST(OUTPUT, 0, 15), SRC(TEMP, 0, XYZW), 0, 0);
        if (!fs) for (uint32_t o = 1; o <= pb.varyings; o++) pb_op(0, FGPU_SOP_MOV, DST(OUTPUT, o, 15), SRC(TEMP, 0, XYZW), 0, 0);
    }
    CHECK(pb_finish(&why), "huge program builds");
    for (int p = 0; p < 2; p++) {
        if (!g_have[p]) continue;
        fgpu_jit_code_t* c = fgpu_jit_compile_stage(&pb.prog, true, k_paths[p], 0, &why);
        CHECK(c == NULL, "huge program: clean failure");
        fgpu_jit_free(c);
    }
    printf("negative tests done\n");
}

/* ------------------------------------------------------------- program API */

static void test_program_api(void) {
    // fgpu_program_jit + fgpu_shader_run_batch (the interface the rasterizer uses).
    if (fglsl_compile(&comp, k_cube_vs, k_cube_fs) != 0) { failures++; return; }
    static uint32_t blob[FGLSL_MAX_BLOB_WORDS];
    memcpy(blob, comp.words, comp.size);
    static fgpu_program_t pj, pi;
    const char* why = "";
    CHECK(fgpu_program_check(blob, comp.size, &pj, &why), "checks");
    pi = pj;
    fgpu_jit_mode_t saved = fgpu_jit_get_mode();
    for (int p = 0; p < 2; p++) {
        if (!g_have[p]) continue;
        fgpu_jit_set_mode(k_paths[p]);
        pj.jit = NULL;
        CHECK(fgpu_program_jit(&pj), "program compiles");
        CHECK(!fgpu_program_jit(&pj), "second compile call does nothing");
        fgpu_jit_info_t vi, fi;
        CHECK(fgpu_program_jit_info(&pj, false, &vi) && fgpu_program_jit_info(&pj, true, &fi), "info");
        printf("  cube on %s: vertex %u ops %u bytes, fragment %u ops %u bytes (%u tex, %u spilled regs)\n", k_path_names[p],
               vi.ir_ops, vi.code_bytes, fi.ir_ops, fi.code_bytes, fi.tex_calls, fi.spills);
        int bad = 0;
        setup_textures();
        for (int r = 0; r < 200; r++) {
            for (int s = 0; s < 2; s++) {
                pb.varyings = pj.h.varying_count;
                fill_uniforms(r % 3);
                fill_inputs(s == 1, r % 3);
                fgpu_shader_batch_reset(&ref_b, pj.h.temp_count);
                fgpu_shader_batch_reset(&jit_b, pj.h.temp_count);
                uint32_t lanes = r % 4 == 3 ? (rnd() & 0xFF) : 0xFF;
                uint32_t k1 = fgpu_shader_run_batch(&pi, s == 1, &ref_b, lanes, (const float (*)[4])g_uniforms, g_textures);
                uint32_t k2 = fgpu_shader_run_batch(&pj, s == 1, &jit_b, lanes, (const float (*)[4])g_uniforms, g_textures);
                if (k1 != k2) bad++;
                uint32_t outs = s ? 1u : 1u + pj.h.varying_count;
                for (uint32_t o = 0; o < outs; o++) for (int c = 0; c < 4; c++) for (int l = 0; l < 8; l++) {
                    if (!((k1 >> l) & 1) && ((lanes >> l) & 1)) continue;
                    bad += !same_bits(jit_b.output[o][c][l], ref_b.output[o][c][l]);
                }
            }
        }
        CHECK(bad == 0, "run_batch through the program matches the interpreter");
        // Single-lane interfaces.
        float in[FGPU_MAX_ATTRIBS][4], out_a[1 + FGPU_SHADER_MAX_VARYINGS][4], out_b[1 + FGPU_SHADER_MAX_VARYINGS][4];
        for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++) for (int c = 0; c < 4; c++) in[i][c] = frand(-1, 1);
        fgpu_shader_io_t io = { &pi, (const float (*)[4])in, out_a, (const float (*)[4])g_uniforms, NULL, g_textures };
        fgpu_shader_run_vertex(&io);
        io.program = &pj;
        io.output = out_b;
        fgpu_shader_run_vertex(&io);
        CHECK(!memcmp(out_a, out_b, sizeof(float) * 4 * (1 + pj.h.varying_count)), "run_vertex matches");
        float fsp[2][4] = { { 100, 100, 0.5f, 1 }, { 1, 1, 1, 1 } };
        io.special = (const float (*)[4])fsp;
        io.program = &pi; io.output = out_a;
        bool ka = fgpu_shader_run_fragment(&io);
        io.program = &pj; io.output = out_b;
        bool kb = fgpu_shader_run_fragment(&io);
        CHECK(ka == kb && !memcmp(out_a[0], out_b[0], 16), "run_fragment matches");
        fgpu_program_release(&pj);
    }
    fgpu_jit_set_mode(saved);
}

/* ------------------------------------------------------------------- main */

int main(int argc, char** argv) {
    double soak = 0;
    uint32_t iterations = 6000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--soak") && i + 1 < argc) soak = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) rng_state ^= (uint64_t)atoll(argv[++i]) * 0x2545F4914F6CDD1Dull;
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = (uint32_t)atoi(argv[++i]);
    }
    uint32_t sup = fgpu_jit_cpu_support();
    g_have[0] = (sup & FGPU_JIT_CPU_SSE41) != 0;
    g_have[1] = (sup & FGPU_JIT_CPU_AVX2) != 0;
    printf("fgpu_jit_tests: host CPU: SSE4.1 %s, AVX2 %s\n", g_have[0] ? "yes" : "NO (path skipped)",
           g_have[1] ? "yes" : "NO (path skipped)");
    if (!g_have[0] && !g_have[1]) { printf("SKIP: no JIT-capable path on this CPU; nothing to test\n"); return 0; }
    setup_textures();
    if (soak <= 0) {
        test_per_opcode();
        test_control_flow();
        test_textures();
        test_fgsl();
        test_program_api();
        test_safety();
        test_negative();
    }
    test_fuzz(iterations, soak);
    if (failures) { printf("fgpu_jit_tests: %d FAILURES\n", failures); return 1; }
    printf("fgpu_jit_tests: all passed (%llu batches compared)\n", (unsigned long long)g_batches_run);
    return 0;
}
