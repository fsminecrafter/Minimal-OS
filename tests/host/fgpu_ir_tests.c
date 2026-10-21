// Host tests for the shader IR (fgpu_ir.c): FGSL shaders are compiled,
// lowered to the scalar IR and evaluated by a reference evaluator below;
// the results must match the CPU shader machine (fgpu_shader.c) for
// random inputs. Build and run: make test-host
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_ir.h"
#include "fglsl.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

#define CAP 32768u
static fgpu_ir_inst_t code[CAP];
static uint32_t hash_words[FGPU_IR_HASH_WORDS(CAP)];
static fglsl_program_t comp;
static fgpu_program_t prog;
static float uniforms[FGPU_SHADER_MAX_UNIFORMS][4];
static fgpu_texture_t textures[FGPU_MAX_TEXTURE_UNITS];
static uint32_t texels[2][8 * 8];
static fgpu_shader_batch_t batch;

static uint32_t rng = 12345u;
static float frand(float lo, float hi) {
    rng = rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(rng >> 8) / 16777216.0f;
}

/* ------------------------------------------- reference IR evaluator */

static float vals[CAP];
static float texc[CAP][4];

static float get(const fgpu_ir_src_t* s) {
    float v = vals[s->value];
    if (s->mods & FGPU_IR_ABS) v = fabsf(v);
    if (s->mods & FGPU_IR_NEG) v = -v;
    return v;
}

// Runs one thread. Returns false if it discarded.
static int eval(const fgpu_ir_t* ir, const float in[][4], const float sp[][4], float out[][4]) {
    int kept = 1;
    for (uint32_t i = 0; i < ir->count; i++) {
        const fgpu_ir_inst_t* x = &ir->code[i];
        float a = x->nsrc > 0 ? get(&x->src[0]) : 0, b = x->nsrc > 1 ? get(&x->src[1]) : 0;
        float c = x->nsrc > 2 ? get(&x->src[2]) : 0, r = 0;
        switch (x->op) {
            case FGPU_IR_IMM:     r = x->imm; break;
            case FGPU_IR_INPUT:   r = in[x->aux / 4][x->aux % 4]; break;
            case FGPU_IR_SPECIAL: r = sp[x->aux / 4][x->aux % 4]; break;
            case FGPU_IR_UNIFORM: r = uniforms[x->aux / 4][x->aux % 4]; break;
            case FGPU_IR_ADD:     r = a + b; break;
            case FGPU_IR_MUL:     r = a * b; break;
            case FGPU_IR_FMA:     r = a * b + c; break;
            case FGPU_IR_MIN:     r = a < b ? a : b; break;
            case FGPU_IR_MAX:     r = a > b ? a : b; break;
            case FGPU_IR_FLOOR:   r = floorf(a); break;
            case FGPU_IR_RCP:     r = 1.0f / a; break;
            case FGPU_IR_RSQ:     r = 1.0f / sqrtf(a); break;
            case FGPU_IR_SQRT:    r = sqrtf(a); break;
            case FGPU_IR_EX2:     r = exp2f(a); break;
            case FGPU_IR_LG2:     r = log2f(a); break;
            case FGPU_IR_SIN:     r = sinf(a); break;
            case FGPU_IR_COS:     r = cosf(a); break;
            case FGPU_IR_SLT:     r = a <  b ? 1.0f : 0.0f; break;
            case FGPU_IR_SLE:     r = a <= b ? 1.0f : 0.0f; break;
            case FGPU_IR_SEQ:     r = a == b ? 1.0f : 0.0f; break;
            case FGPU_IR_SNE:     r = a != b ? 1.0f : 0.0f; break;
            case FGPU_IR_SEL:     r = a != 0.0f ? b : c; break;
            case FGPU_IR_TEX:
                fgpu_texture_sample(&textures[x->aux], a, b, texc[i]);
                r = texc[i][0];
                break;
            case FGPU_IR_TEXC:    r = texc[x->src[0].value][x->aux]; break;
            case FGPU_IR_OUTPUT:  out[x->aux / 4][x->aux % 4] = a; break;
            case FGPU_IR_DISCARD: if (a != 0.0f) kept = 0; break;
            default:              printf("bad op %u\n", x->op); failures++; break;
        }
        vals[i] = r;
    }
    return kept;
}

/* ------------------------------------------------------------ helpers */

static int build(const char* vs, const char* fs) {
    if (fglsl_compile(&comp, vs, fs) != 0) {
        printf("  compile error: %s\n", comp.error);
        return 0;
    }
    const char* why = NULL;
    if (!fgpu_program_check(comp.words, comp.size, &prog, &why)) {
        printf("  check error: %s\n", why);
        return 0;
    }
    return 1;
}

static int lower(fgpu_ir_t* ir, int fragment) {
    memset(ir, 0, sizeof(*ir));
    ir->code = code;
    ir->capacity = CAP;
    ir->hash = hash_words;
    const char* why = NULL;
    if (!fgpu_ir_lower(ir, &prog, fragment, &why)) {
        printf("  lower error: %s\n", why);
        return 0;
    }
    fgpu_ir_allocate(ir);
    return 1;
}

static int close_enough(float a, float b) {
    if (isnan(a) && isnan(b)) return 1;
    float m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
    return fabsf(a - b) <= 2e-3f * (m > 1.0f ? m : 1.0f);
}

// Runs one stage of `prog` on 8 random threads through both paths.
// Returns how many outputs disagreed.
static int compare_stage(int fragment, int rounds, const char* name) {
    fgpu_ir_t ir;
    if (!lower(&ir, fragment)) { failures++; return 1; }
    int bad = 0;
    uint32_t outs = fragment ? 1 : 1 + prog.h.varying_count;
    for (int round = 0; round < rounds; round++) {
        for (uint32_t u = 0; u < FGPU_SHADER_MAX_UNIFORMS; u++)
            for (int c = 0; c < 4; c++) uniforms[u][c] = frand(-2.0f, 2.0f);
        fgpu_shader_batch_reset(&batch, prog.h.temp_count);
        float in[FGPU_LANES][FGPU_MAX_ATTRIBS][4], sp[FGPU_LANES][2][4];
        for (int l = 0; l < FGPU_LANES; l++) {
            for (uint32_t i = 0; i < FGPU_MAX_ATTRIBS; i++)
                for (int c = 0; c < 4; c++) {
                    in[l][i][c] = frand(-2.0f, 2.0f);
                    batch.input[i][c][l] = in[l][i][c];
                }
            float ff = frand(0, 1) < 0.5f ? 1.0f : 0.0f;
            float fc[4] = { frand(0, 640), frand(0, 480), frand(0, 1), frand(0.1f, 2.0f) };
            for (int c = 0; c < 4; c++) {
                sp[l][0][c] = fc[c];
                sp[l][1][c] = ff;
                batch.special[0][c][l] = fc[c];
                batch.special[1][c][l] = ff;
            }
        }
        uint32_t kept = fgpu_shader_run_batch(&prog, fragment, &batch, 0xFF,
                                              (const float (*)[4])uniforms, textures);
        for (int l = 0; l < FGPU_LANES; l++) {
            float out[1 + FGPU_SHADER_MAX_VARYINGS][4];
            memset(out, 0, sizeof(out));
            int k = eval(&ir, (const float (*)[4])in[l], (const float (*)[4])sp[l], out);
            if (k != (int)((kept >> l) & 1)) {
                if (bad++ < 3) printf("  %s: lane %d discard differs (ir %d)\n", name, l, k);
                continue;
            }
            if (!k) continue;
            for (uint32_t o = 0; o < outs; o++)
                for (int c = 0; c < 4; c++) {
                    float want = batch.output[o][c][l];
                    if (!close_enough(out[o][c], want)) {
                        if (bad++ < 3)
                            printf("  %s: o%u.%c = %g, interpreter %g\n", name, o, "xyzw"[c],
                                   out[o][c], want);
                    }
                }
        }
    }
    return bad;
}

/* -------------------------------------------------------------- tests */

typedef struct {
    const char* name;
    const char* vs;
    const char* fs;
} shader_case_t;

static const char* k_vs =
    "uniform mat4 mvp;\n"
    "uniform mat4 model;\n"
    "attribute vec3 position;\n"
    "attribute vec3 normal;\n"
    "attribute vec4 color;\n"
    "attribute vec2 uv;\n"
    "varying vec3 v_n;\n"
    "varying vec3 v_p;\n"
    "varying vec4 v_c;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec4 wp = model * vec4(position, 1.0);\n"
    "  v_p = wp.xyz;\n"
    "  v_n = normalize((model * vec4(normal, 0.0)).xyz);\n"
    "  v_c = color;\n"
    "  v_uv = uv;\n"
    "  gl_Position = mvp * vec4(position, 1.0);\n"
    "}\n";

static const char* k_varyings =
    "varying vec3 v_n;\nvarying vec3 v_p;\nvarying vec4 v_c;\nvarying vec2 v_uv;\n";

static char fs_buf[8][4096];

static const char* fs(int slot, const char* decls, const char* body) {
    snprintf(fs_buf[slot], sizeof fs_buf[slot], "%s%s\nvoid main() {\n%s\n}\n", k_varyings, decls, body);
    return fs_buf[slot];
}

static void test_corpus(void) {
    shader_case_t cases[] = {
        { "phong", k_vs, fs(0,
            "uniform vec3 light; uniform vec3 eye; uniform float shininess;",
            "  vec3 n = normalize(v_n);\n"
            "  vec3 l = normalize(-light);\n"
            "  float d = max(dot(n, l), 0.0);\n"
            "  vec3 r = reflect(-l, n);\n"
            "  float s = pow(max(dot(r, normalize(eye - v_p)), 0.0), 8.0 + abs(shininess) * 16.0);\n"
            "  gl_FragColor = vec4(v_c.rgb * (0.2 + d) + vec3(s), 1.0);") },
        { "builtins", k_vs, fs(1,
            "uniform vec4 k;",
            "  vec4 a = vec4(sin(v_p.x * 3.0), cos(v_p.y), tan(v_n.z * 0.5), atan(v_p.x, v_p.y));\n"
            "  vec4 b = vec4(exp(v_n.x), log(abs(v_p.z) + 0.1), sqrt(abs(v_c.r)), inversesqrt(abs(v_c.g) + 0.01));\n"
            "  vec4 c = vec4(sign(v_p), atan(v_c.a));\n"
            "  vec4 d = vec4(floor(v_p.x * 2.0), ceil(v_p.y), fract(v_p.z * 3.0), mod(v_c.b * 5.0, 1.5));\n"
            "  vec4 e = vec4(clamp(v_c.r, -0.5, 0.5), step(0.0, v_n.x), smoothstep(-1.0, 1.0, v_n.y), mix(v_p.x, v_p.y, 0.25));\n"
            "  vec3 f = cross(v_n, v_p) + vec3(length(v_p), distance(v_p, v_n), min(k.x, k.y));\n"
            "  gl_FragColor = a * 0.1 + b * 0.2 + c * 0.3 + d * 0.05 + e + vec4(f, max(k.z, k.w)) * 0.1;") },
        { "pow-edge", k_vs, fs(2,
            "uniform vec4 k;",
            "  gl_FragColor = vec4(pow(v_p.x, 2.0), pow(k.x, v_p.y), log2(v_p.z), sqrt(v_n.x)) +\n"
            "                 vec4(inversesqrt(v_n.y), v_p.x / v_n.z, mod(v_p.y, 0.0), smoothstep(k.x, k.x, v_p.z));") },
        { "branches", k_vs, fs(3,
            "uniform float cut;",
            "  vec4 c = v_c;\n"
            "  if (v_p.x > 0.0) {\n"
            "    c.r = c.r * 2.0;\n"
            "    if (v_p.y > 0.5) c.g = 1.0; else { c.g = 0.0; c.b = v_n.x; }\n"
            "  } else {\n"
            "    c = vec4(v_n, 1.0);\n"
            "    if (v_n.z < -0.5) discard;\n"
            "  }\n"
            "  if (c.a < cut) discard;\n"
            "  gl_FragColor = c;") },
        { "loops", k_vs, fs(4,
            "uniform vec4 w[4];",
            "  vec3 acc = vec3(0.0);\n"
            "  for (int i = 0; i < 4; i++) {\n"
            "    if (dot(w[i].xyz, v_n) < 0.0) continue;\n"
            "    acc += w[i].xyz * w[i].w;\n"
            "    if (acc.x > 1.0) break;\n"
            "  }\n"
            "  gl_FragColor = vec4(acc, v_p.x > 0.0 ? 1.0 : 0.5);") },
        { "texture", k_vs, fs(5,
            "uniform sampler2D tex; uniform sampler2D tex2;",
            "  vec4 t = texture(tex, v_uv * 3.0);\n"
            "  vec4 u = texture2D(tex2, v_uv.yx + vec2(0.1));\n"
            "  if (t.a < 0.3) discard;\n"
            "  gl_FragColor = vec4(t.rgb * u.a, u.r);") },
        { "fragcoord", k_vs, fs(6,
            "",
            "  vec2 p = gl_FragCoord.xy / vec2(640.0, 480.0);\n"
            "  float checker = mod(floor(gl_FragCoord.x / 8.0) + floor(gl_FragCoord.y / 8.0), 2.0);\n"
            "  gl_FragColor = gl_FrontFacing ? vec4(p, checker, gl_FragCoord.w) : vec4(gl_FragCoord.z);") },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (!build(cases[i].vs, cases[i].fs)) { printf("FAIL: %s does not build\n", cases[i].name); failures++; continue; }
        char name[64];
        snprintf(name, sizeof name, "%s (vertex)", cases[i].name);
        int bad = compare_stage(0, 40, name);
        snprintf(name, sizeof name, "%s (fragment)", cases[i].name);
        bad += compare_stage(1, 200, name);
        if (bad) { printf("FAIL: %s: %d mismatches\n", cases[i].name, bad); failures++; }
    }
}

// Straight-line code needs no selects; branches do; folding removes
// everything that does not reach an output.
static void test_optimizer(void) {
    fgpu_ir_t ir;
    CHECK(build(k_vs, fs(0, "", "  gl_FragColor = vec4(v_c.rgb * 1.0 + vec3(0.0), 1.0);")), "builds");
    CHECK(lower(&ir, 1), "lowers");
    CHECK(ir.selects_for_jumps == 0, "no selects without jumps");
    uint32_t math = 0;
    for (uint32_t i = 0; i < ir.count; i++) {
        uint32_t op = ir.code[i].op;
        if (op != FGPU_IR_IMM && op != FGPU_IR_INPUT && op != FGPU_IR_OUTPUT) math++;
    }
    CHECK(math == 0, "x * 1 + 0 folds to x");
    CHECK(ir.reg_count == 3, "three registers for three varyings");

    CHECK(build(k_vs, fs(1, "", "  vec4 c = v_c; if (v_p.x > 0.0) c.r = 1.0; gl_FragColor = c;")), "builds");
    CHECK(lower(&ir, 1), "lowers");
    CHECK(ir.selects_for_jumps > 0, "if-conversion adds selects");

    // Vertex stage: model * vec4(position, 1) is 3 FMAs + 1 MUL per row,
    // with the w = 1 product folded away.
    CHECK(build("uniform mat4 m; attribute vec3 position;\n"
                "void main() { gl_Position = m * vec4(position, 1.0); }\n",
                "void main() { gl_FragColor = vec4(1.0); }\n"), "builds");
    CHECK(lower(&ir, 0), "lowers");
    uint32_t fma = 0, mul = 0;
    for (uint32_t i = 0; i < ir.count; i++) {
        if (ir.code[i].op == FGPU_IR_FMA) fma++;
        if (ir.code[i].op == FGPU_IR_MUL) mul++;
    }
    CHECK(mul == 4 && fma == 8, "mat4 * vec4(p, 1): 4 FMUL + 8 FFMA + 4 FADD");
}

// Hand-written bytecode: divergent nested jumps, and a jump that skips
// a KILL.
static uint32_t blob[256];

static void emit5(uint32_t* p, uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c) {
    p[0] = op; p[1] = d; p[2] = a; p[3] = b; p[4] = c;
}

static void test_bytecode_jumps(void) {
    memset(blob, 0, sizeof blob);
    fgpu_shader_header_t* h = (fgpu_shader_header_t*)blob;
    uint32_t hw = sizeof(*h) / 4;
    const uint32_t X = FGPU_SWIZZLE(0, 0, 0, 0), Y = FGPU_SWIZZLE(1, 1, 1, 1), XYZW = FGPU_SWIZZLE_XYZW;
    uint32_t vs_n = 1, fs_n = 8;
    h->magic = FGPU_SHADER_MAGIC;
    h->version = FGPU_SHADER_VERSION;
    h->flags = FGPU_SHADER_FLAG_DISCARD;
    h->vs_offset = hw;
    h->vs_count = vs_n;
    h->fs_offset = hw + vs_n * 5;
    h->fs_count = fs_n;
    h->const_offset = h->fs_offset + fs_n * 5;
    h->const_count = 1;
    h->varying_count = 1;
    h->temp_count = 1;
    h->size = (h->const_offset + 4) * 4;
    uint32_t* v = blob + h->vs_offset;
    emit5(v, FGPU_SOP_MOV, FGPU_DST(FGPU_FILE_OUTPUT, 0, 0xF), FGPU_SRC(FGPU_FILE_INPUT, 0, XYZW), 0, 0);
    uint32_t* f = blob + h->fs_offset;
    uint32_t in_x = FGPU_SRC(FGPU_FILE_INPUT, 0, X), in_y = FGPU_SRC(FGPU_FILE_INPUT, 0, Y);
    uint32_t t0 = FGPU_DST(FGPU_FILE_TEMP, 0, 0xF), out = FGPU_DST(FGPU_FILE_OUTPUT, 0, 0xF);
    // 0: t0 = v0            1: SLT t0.x = v0.x < 0      2: JZ t0.x -> 6
    // 3: SLT t0.y = v0.y < 0  4: JZ t0.y -> 7 (skips the KILL at 6)
    // 5: t0 = c0 (inner)    6: KILL v0.z (skipped by threads from 2 or 4? no: only 4)
    // 7: out = t0
    emit5(f + 0, FGPU_SOP_MOV, t0, FGPU_SRC(FGPU_FILE_INPUT, 0, XYZW), 0, 0);
    emit5(f + 5, FGPU_SOP_SLT, FGPU_DST(FGPU_FILE_TEMP, 0, 1), in_x, FGPU_SRC(FGPU_FILE_CONST, 0, X), 0);
    emit5(f + 10, FGPU_SOP_JZ, 6, FGPU_SRC(FGPU_FILE_TEMP, 0, X), 0, 0);
    emit5(f + 15, FGPU_SOP_SLT, FGPU_DST(FGPU_FILE_TEMP, 0, 2), in_y, FGPU_SRC(FGPU_FILE_CONST, 0, X), 0);
    emit5(f + 20, FGPU_SOP_JZ, 7, FGPU_SRC(FGPU_FILE_TEMP, 0, FGPU_SWIZZLE(1, 1, 1, 1)), 0, 0);
    emit5(f + 25, FGPU_SOP_MOV, t0, FGPU_SRC(FGPU_FILE_CONST, 0, XYZW) | FGPU_SRC_NEGATE, 0, 0);
    emit5(f + 30, FGPU_SOP_KILL, 0, FGPU_SRC(FGPU_FILE_INPUT, 0, FGPU_SWIZZLE(2, 2, 2, 2)), 0, 0);
    emit5(f + 35, FGPU_SOP_MOV, out, FGPU_SRC(FGPU_FILE_TEMP, 0, XYZW) | FGPU_SRC_ABS, 0, 0);
    float* k = (float*)(blob + h->const_offset);
    k[0] = 0.0f; k[1] = 0.25f; k[2] = 0.5f; k[3] = 0.75f;
    const char* why = NULL;
    CHECK(fgpu_program_check(blob, h->size, &prog, &why), "hand-written program checks");
    // v0.z is 0 for about a third of the threads, so some KILLs do nothing.
    int bad = 0;
    for (int round = 0; round < 50; round++) bad += compare_stage(1, 1, "jumps");
    CHECK(bad == 0, "divergent jumps match the interpreter");
}

static void line_count(void* ctx, const char* text) {
    (void)text;
    (*(int*)ctx)++;
}

static void line_print(void* ctx, const char* text) {
    (void)ctx;
    printf("%s\n", text);
}

static void test_listing(void) {
    fgpu_ir_t ir;
    CHECK(build(k_vs, fs(0, "uniform sampler2D tex;",
                         "  vec4 t = texture(tex, v_uv);\n"
                         "  if (t.a < 0.5) discard;\n"
                         "  gl_FragColor = vec4(t.rgb * max(dot(normalize(v_n), vec3(0.0, 0.0, 1.0)), 0.2), 1.0);")),
          "builds");
    CHECK(lower(&ir, 1), "lowers");
    int lines = 0;
    fgpu_ir_print(&ir, line_count, &lines);
    CHECK(lines > 5, "listing has lines");
    if (getenv("FGPU_IR_LIST")) fgpu_ir_print(&ir, line_print, NULL);
}

static void setup_textures(void) {
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++) {
            rng = rng * 1664525u + 1013904223u;
            texels[t][i] = rng;
        }
    textures[0] = (fgpu_texture_t){ texels[0], 8, 8, 8, 0 };
    textures[1] = (fgpu_texture_t){ texels[1], 8, 8, 8, FGPU_TEXTURE_LINEAR | FGPU_TEXTURE_CLAMP };
}

int main(void) {
    setup_textures();
    test_corpus();
    test_optimizer();
    test_bytecode_jumps();
    test_listing();
    if (failures) {
        printf("fgpu ir tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("fgpu ir tests passed\n");
    return 0;
}
