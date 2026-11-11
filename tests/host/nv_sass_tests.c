// Host tests for the Turing shader encoder (nv_sass.c) and the 3D
// pushbuffers (nv_3d.c).
//
// The machine code cannot run here, so a small simulator below decodes
// the instructions (by the field positions Mesa's NAK documents) and
// executes them; the results must match the CPU shader machine for random
// inputs. That checks everything the encoder decides - registers,
// scratch use, predicates, output moves, headers - against the IR. It
// cannot check that a real card agrees with the field positions.
// Build and run: make test-host
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_ir.h"
#include "x86_64/nv_sass.h"
#include "x86_64/nv_3d.h"
#include "x86_64/nv_builtin_fgsl.h"
#include "fglsl.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

#define CAP 32768u
static fgpu_ir_inst_t code[CAP];
static uint32_t hash_words[FGPU_IR_HASH_WORDS(CAP)];
static uint32_t sass_words[2][CAP * 8u];
static fglsl_program_t comp;
static fgpu_program_t prog;
static float uniforms[FGPU_SHADER_MAX_UNIFORMS][4];
static fgpu_texture_t textures[FGPU_MAX_TEXTURE_UNITS];
static uint32_t texels[2][8 * 8];

static uint32_t rng = 777u;
static float frand(float lo, float hi) {
    rng = rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(rng >> 8) / 16777216.0f;
}

/* ------------------------------------------------------- the simulator */

typedef struct {
    uint32_t r[256];
    bool p[8];
    float attr_in[0x400 / 4];       // a[...] read by ALD / IPA
    float attr_out[0x400 / 4];
    bool attr_written[0x400 / 4];
    bool killed;
    uint32_t regs;                  // registers the header promises
    bool bad;
} sim_t;

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
#define F(lo, hi) nv_sass_field(w, lo, hi)

static uint32_t rd(sim_t* s, uint32_t reg) {
    if (reg == 255) return 0;
    if (reg >= s->regs) s->bad = true;
    return s->r[reg];
}

static void wr(sim_t* s, uint32_t reg, uint32_t v) {
    if (reg == 255) return;
    if (reg >= s->regs) { s->bad = true; return; }
    s->r[reg] = v;
}

static float mod(uint32_t bits, bool abs, bool neg) {
    float v = u2f(bits);
    if (abs) v = fabsf(v);
    return neg ? -v : v;
}

static bool sim_run(sim_t* s, const nv_sass_t* sh, bool fragment) {
    const uint32_t cb_tex = NV_SASS_CB_TEX_OFFSET;
    bool exited = false;
    for (uint32_t ip = 0; ip + 4 <= sh->count; ip += 4) {
        const uint32_t* w = &sh->code[ip];
        if (exited) { s->bad = true; break; }       // code after EXIT
        uint32_t pred = F(12, 15);
        bool run = pred == 7 ? true : s->p[pred];
        if (F(15, 16)) run = !run;
        // Scheduling as the encoder promises: wait for both barriers
        // from the second instruction on.
        if (F(116, 122) != (ip ? 3u : 0u) || F(105, 109) != 0) s->bad = true;
        uint32_t full = F(0, 12), dst = F(16, 24);
        if (full == 0x94D) { exited = true; continue; }
        if (!run) continue;
        if (full == 0x95B) { s->killed = true; continue; }
        if (full == 0x321 || full == 0x326) {       // ALD / IPA
            uint32_t address = full == 0x321 ? F(40, 50) : F(64, 72) << 2;
            if (full == 0x321 && (F(24, 32) != 255 || F(32, 40) != 255)) s->bad = true;
            if (full == 0x326 && F(32, 40) != 255) s->bad = true;
            wr(s, dst, f2u(s->attr_in[address / 4]));
            continue;
        }
        if (full == 0x322) {                        // AST
            uint32_t address = F(40, 50);
            if (F(24, 32) != 255 || F(64, 72) != 255 || fragment) s->bad = true;
            s->attr_out[address / 4] = u2f(rd(s, F(32, 40)));
            s->attr_written[address / 4] = true;
            continue;
        }
        if (full == 0xB60) {                        // TEX
            uint32_t unit = (F(40, 54) * 4u - cb_tex) / 4u, base = F(24, 32);
            float out[4];
            if (unit >= FGPU_MAX_TEXTURE_UNITS || F(61, 64) != 1 || F(72, 76) != 0xF || F(87, 90) != 1) {
                s->bad = true;
                continue;
            }
            fgpu_texture_sample(&textures[unit], u2f(rd(s, base)), u2f(rd(s, base + 1)), out);
            for (int c = 0; c < 4; c++) wr(s, dst + (uint32_t)c, f2u(out[c]));
            continue;
        }
        uint32_t op = F(0, 9), form = F(9, 12);
        uint32_t a_bits = rd(s, F(24, 32));
        bool has0 = op == 0x009 || op == 0x00B || op == 0x020 || op == 0x021 || op == 0x023 || op == 0x007;
        float a = has0 ? mod(a_bits, F(73, 74), F(72, 73)) : 0.0f;
        uint32_t b_bits;
        float b;
        if (form == 4) { b_bits = F(32, 64); b = u2f(b_bits); }
        else if (form == 5) {
            uint32_t off = F(38, 54);
            if (F(54, 59) != 0 || off + 4 > NV_SASS_CB_UNIFORM_BYTES) s->bad = true;
            memcpy(&b_bits, (const uint8_t*)uniforms + off, 4);
            b = u2f(b_bits);
        } else if (form == 1) { b_bits = rd(s, F(32, 40)); b = mod(b_bits, F(62, 63), F(63, 64)); }
        else { s->bad = true; continue; }
        switch (op) {
            case 0x002: wr(s, dst, b_bits); if (F(72, 76) != 0xF) s->bad = true; break;
            case 0x007: wr(s, dst, (s->p[F(87, 90)] != (bool)F(90, 91)) ? a_bits : b_bits); break;
            case 0x009: wr(s, dst, f2u(F(90, 91) ? (a > b ? a : b) : (a < b ? a : b))); break;
            case 0x00B: {
                bool r;
                switch (F(76, 80)) {
                    case 1: r = a < b; break;
                    case 2: r = a == b; break;
                    case 3: r = a <= b; break;
                    case 0xD: r = !(a == b); break;
                    default: r = false; s->bad = true;
                }
                if (F(81, 84) != 0 || F(84, 87) != 7 || F(87, 90) != 7) s->bad = true;
                s->p[0] = r;
                break;
            }
            case 0x020: wr(s, dst, f2u(a * b)); if (F(64, 72) != 255 || F(84, 87) != 4) s->bad = true; break;
            case 0x021: wr(s, dst, f2u(a + b)); break;
            case 0x023: wr(s, dst, f2u(a * b + mod(rd(s, F(64, 72)), F(74, 75), F(75, 76)))); break;
            case 0x107: wr(s, dst, f2u(floorf(b))); if (F(78, 80) != 1 || F(84, 86) != 2 || F(75, 77) != 2) s->bad = true; break;
            case 0x108:
                switch (F(74, 80)) {
                    case 0: wr(s, dst, f2u(cosf(b * 6.283185307179586f))); break;
                    case 1: wr(s, dst, f2u(sinf(b * 6.283185307179586f))); break;
                    case 2: wr(s, dst, f2u(exp2f(b))); break;
                    case 3: wr(s, dst, f2u(log2f(b))); break;
                    case 4: wr(s, dst, f2u(1.0f / b)); break;
                    case 5: wr(s, dst, f2u(1.0f / sqrtf(b))); break;
                    case 8: wr(s, dst, f2u(sqrtf(b))); break;
                    default: s->bad = true;
                }
                break;
            default: s->bad = true;
        }
    }
    return exited && !s->bad;
}

/* ---------------------------------------------------------------- tests */

static bool build_stage(bool fragment, nv_sass_t* out) {
    fgpu_ir_t ir = { .code = code, .capacity = CAP, .hash = hash_words };
    const char* why = "";
    if (!fgpu_ir_lower(&ir, &prog, fragment, &why)) { printf("  IR: %s\n", why); return false; }
    fgpu_ir_allocate(&ir);
    out->code = sass_words[fragment];
    out->capacity = CAP * 8u;
    if (!nv_sass_encode(ir.code, ir.count, ir.reg_count, fragment, prog.h.varying_count, out, &why)) {
        printf("  SASS: %s\n", why);
        return false;
    }
    return true;
}

static bool close_enough(float a, float b) {
    if (a != a && b != b) return true;
    float d = fabsf(a - b), m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
    return d <= 2e-3f + 2e-3f * m;
}

// Compiles a shader pair and runs both stages on random inputs through
// the CPU machine and the simulated machine code.
static void compare(const char* name, const char* vs, const char* fs) {
    char msg[160];
    if (fglsl_compile(&comp, vs, fs) != 0) {
        printf("FAIL: %s does not compile: %s\n", name, comp.error);
        failures++;
        return;
    }
    const char* why = "";
    if (!fgpu_program_check(comp.words, comp.size, &prog, &why)) {
        printf("FAIL: %s rejected: %s\n", name, why);
        failures++;
        return;
    }
    nv_sass_t vsh, fsh;
    snprintf(msg, sizeof msg, "%s: both stages encode", name);
    bool ok = build_stage(false, &vsh) && build_stage(true, &fsh);
    CHECK(ok, msg);
    if (!ok) return;
    uint32_t nvar = prog.h.varying_count;

    // Headers.
    snprintf(msg, sizeof msg, "%s: vertex header", name);
    CHECK((vsh.sph[0] & 0x1F) == 1 && ((vsh.sph[0] >> 5) & 0x1F) == 4 && ((vsh.sph[0] >> 10) & 0xF) == 1 &&
          ((vsh.sph[13] >> 12) & 0xF) == 0xF, msg);
    snprintf(msg, sizeof msg, "%s: pixel header", name);
    CHECK((fsh.sph[0] & 0x1F) == 2 && ((fsh.sph[0] >> 10) & 0xF) == 5 && (fsh.sph[0] & (1u << 14)) &&
          (fsh.sph[18] & 0xF) == 0xF && (((fsh.sph[0] >> 15) & 1u) != 0) == fsh.kills, msg);
    // Every varying the vertex stage writes is one the pixel stage reads.
    for (uint32_t c = 0; c < nvar * 4u; c++) {
        bool wrote = (vsh.sph[(432 + c) / 32] >> ((432 + c) % 32)) & 1u;
        uint32_t read = (fsh.sph[(192 + c * 2) / 32] >> ((192 + c * 2) % 32)) & 3u;
        if (!wrote || read != 2) { snprintf(msg, sizeof msg, "%s: varying maps agree", name); CHECK(false, msg); break; }
    }

    int bad = 0;
    for (int round = 0; round < 200 && !bad; round++) {
        for (int i = 0; i < (int)FGPU_SHADER_MAX_UNIFORMS; i++)
            for (int c = 0; c < 4; c++) uniforms[i][c] = frand(-2.0f, 2.0f);
        // Vertex stage.
        float in[FGPU_MAX_ATTRIBS][4], out[1 + FGPU_SHADER_MAX_VARYINGS][4];
        static sim_t s;
        memset(&s, 0, sizeof s);
        s.regs = vsh.regs;
        for (int i = 0; i < (int)FGPU_MAX_ATTRIBS; i++)
            for (int c = 0; c < 4; c++) {
                in[i][c] = frand(-1.5f, 1.5f);
                s.attr_in[(NV_SASS_ATTR_GENERIC + 16 * i + 4 * c) / 4] = in[i][c];
            }
        fgpu_shader_io_t io = { &prog, in, out, uniforms, NULL, textures };
        fgpu_shader_run_vertex(&io);
        if (!sim_run(&s, &vsh, false)) { bad = 1; break; }
        for (uint32_t r = 0; r <= nvar && !bad; r++)
            for (int c = 0; c < 4; c++) {
                uint32_t address = r == 0 ? NV_SASS_ATTR_POSITION + 4 * c
                                          : NV_SASS_ATTR_GENERIC + 16 * (r - 1) + 4 * c;
                if (!s.attr_written[address / 4] || !close_enough(s.attr_out[address / 4], out[r][c])) {
                    printf("  vertex output %u.%d: machine code %g, CPU %g\n", r, c,
                           s.attr_out[address / 4], out[r][c]);
                    bad = 1;
                }
            }
        // Fragment stage.
        float special[2][4] = { { frand(0, 64), frand(0, 64), frand(0, 1), frand(0.1f, 2) }, { 1, 1, 1, 1 } };
        float color[1][4];
        memset(&s, 0, sizeof s);
        s.regs = fsh.regs;
        for (int i = 0; i < (int)FGPU_MAX_ATTRIBS; i++)
            for (int c = 0; c < 4; c++) {
                in[i][c] = frand(-1.5f, 1.5f);
                s.attr_in[(NV_SASS_ATTR_GENERIC + 16 * i + 4 * c) / 4] = in[i][c];
            }
        for (int c = 0; c < 4; c++) s.attr_in[(NV_SASS_ATTR_POSITION + 4 * c) / 4] = special[0][c];
        fgpu_shader_io_t fio = { &prog, in, color, uniforms, special, textures };
        bool kept = fgpu_shader_run_fragment(&fio);
        if (!sim_run(&s, &fsh, true)) { bad = 1; break; }
        if (kept == s.killed) { printf("  discard differs\n"); bad = 1; }
        for (int c = 0; c < 4 && kept && !bad; c++)
            if (!close_enough(u2f(s.r[c]), color[0][c])) {
                printf("  color.%d: machine code %g, CPU %g\n", c, u2f(s.r[c]), color[0][c]);
                bad = 1;
            }
    }
    snprintf(msg, sizeof msg, "%s: machine code matches the CPU shader machine", name);
    CHECK(!bad, msg);
    printf("  %-22s vertex %4u instructions %3u registers, pixel %4u instructions %3u registers\n", name,
           vsh.count / 4, vsh.regs, fsh.count / 4, fsh.regs);
}

static void test_encodings(void) {
    // out = a + b; one FADD, checked field by field.
    static fgpu_ir_inst_t ir[4];
    memset(ir, 0, sizeof ir);
    ir[0] = (fgpu_ir_inst_t){ .op = FGPU_IR_INPUT, .reg = 0, .aux = FGPU_IR_SLOT(1, 2) };
    ir[1] = (fgpu_ir_inst_t){ .op = FGPU_IR_UNIFORM, .reg = 1, .aux = FGPU_IR_SLOT(3, 1) };
    ir[2] = (fgpu_ir_inst_t){ .op = FGPU_IR_ADD, .nsrc = 2, .reg = 2,
                              .src = { { 0, FGPU_IR_NEG }, { 1, FGPU_IR_ABS } } };
    ir[3] = (fgpu_ir_inst_t){ .op = FGPU_IR_OUTPUT, .nsrc = 1, .reg = FGPU_IR_NO_REG,
                              .aux = FGPU_IR_SLOT(0, 0), .src = { { 2, 0 } } };
    static uint32_t words[1024];
    nv_sass_t sh = { .code = words, .capacity = 1024 };
    const char* why = "";
    CHECK(nv_sass_encode(ir, 4, 3, false, 0, &sh, &why), "a small vertex stage encodes");
    const uint32_t* w = sh.code;                            // ALD R0, a[0x98]
    CHECK(F(0, 12) == 0x321 && F(16, 24) == 0 && F(40, 50) == 0x98 && F(12, 16) == 7, "ALD");
    CHECK(F(110, 113) == 0 && F(113, 116) == 1 && F(116, 122) == 0, "first instruction waits for nothing");
    w = sh.code + 4;                                        // MOV R1, c[0][0x34]
    CHECK(F(0, 9) == 0x002 && F(9, 12) == 5 && F(16, 24) == 1 && F(38, 54) == 0x34, "MOV from the constant buffer");
    w = sh.code + 8;                                        // FADD R2, -R0, |R1|
    CHECK(F(0, 9) == 0x021 && F(9, 12) == 1 && F(16, 24) == 2 && F(24, 32) == 0 && F(32, 40) == 1, "FADD registers");
    CHECK(F(72, 73) == 1 && F(73, 74) == 0 && F(62, 63) == 1 && F(63, 64) == 0, "FADD modifiers");
    CHECK(F(116, 122) == 3, "later instructions wait for the barriers");
    w = sh.code + 12;                                       // AST a[0x70], R2
    CHECK(F(0, 12) == 0x322 && F(32, 40) == 2 && F(40, 50) == 0x70, "AST");
    w = sh.code + sh.count - 4;
    CHECK(F(0, 12) == 0x94D && F(87, 90) == 7, "EXIT ends the stage");
    CHECK(sh.sph[6] == (1u << 6), "the header lists the attribute component read");

    ir[0].aux = FGPU_IR_SLOT(1, 0);
    ir[0].op = FGPU_IR_SPECIAL;
    nv_sass_t fs = { .code = words, .capacity = 1024 };
    CHECK(!nv_sass_encode(ir, 4, 3, true, 0, &fs, &why), "front-facing is refused, not miscompiled");
}

static uint32_t find_method(const nv_push_t* p, uint32_t method, uint32_t index) {
    for (uint32_t i = 0; i < p->count;) {
        uint32_t h = p->words[i], type = h >> 29, n = (h >> 16) & 0x1FFF, m = (h & 0xFFF) << 2;
        if (type == NV_PUSH_TYPE_IMMD) { if (m == method) return n; i++; continue; }
        for (uint32_t k = 0; k < n; k++) {
            uint32_t at = type == NV_PUSH_TYPE_INC ? m + 4 * k : type == NV_PUSH_TYPE_ONEINC ? m + (k ? 4 : 0) : m;
            if (at == method) { if (!index) return p->words[i + 1 + k]; index--; }
        }
        i += 1 + n;
    }
    return 0xDEADBEEFu;
}

static void test_pushbuffers(void) {
    static uint32_t words[4096];
    nv_push_t p;
    nv_push_init(&p, words, 4096);
    nv3d_zeta_t z;
    nv3d_zeta_size(100, 50, &z);
    z.va = 0x80200000ull;
    CHECK(z.width == 112 && z.height == 56 && z.bytes == 112 * 56 * 4, "depth buffer rounded to GOBs");
    static float u[FGPU_SHADER_MAX_UNIFORMS * 4];
    u[5] = 2.5f;
    uint32_t attrib[3] = { FGPU_ATTRIB(0, 3), FGPU_ATTRIB(3, 3), FGPU_ATTRIB(6, 4) };
    nv3d_draw_t d = {
        .color = { 0x80100000ull, 512, 100, 50 }, .zeta = &z, .vp_w = 100, .vp_h = 50,
        .state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE | FGPU_STATE_CULL_BACK,
        .vs = { 0x80300000ull, 12 }, .fs = { 0x80301000ull, 20 }, .cb_va = 0x80302000ull, .uniforms = u,
        .tex_handle = { 1u | (1u << 20) }, .stride_bytes = 40, .attrib_count = 3, .attrib = attrib,
        .vb_va = 0x80400000ull, .vb_bytes = 1440, .count = 36,
    };
    nv3d_push_draw(&p, &d);
    CHECK(!p.overflow, "a draw fits the pushbuffer");
    CHECK(find_method(&p, 0x0804, 0) == 0x80100000u && find_method(&p, 0x0808, 0) == 512 &&
          find_method(&p, 0x0810, 0) == 0xCF && find_method(&p, 0x0814, 0) == 0x1000, "color target");
    CHECK(find_method(&p, 0x0FE4, 0) == 0x80200000u && find_method(&p, 0x0FE8, 0) == 0x0A &&
          find_method(&p, 0x1228, 0) == 112 && find_method(&p, 0x1538, 0) == 1, "depth target");
    float sy;
    uint32_t bitsy = find_method(&p, 0x0A04, 0);
    memcpy(&sy, &bitsy, 4);
    CHECK(sy == -25.0f, "the viewport flips y");
    CHECK(find_method(&p, 0x1160, 0) == ((0u << 7) | (0x02u << 21) | (7u << 27)) &&
          find_method(&p, 0x1168, 0) == ((24u << 7) | (0x01u << 21) | (7u << 27)) &&
          find_method(&p, 0x116C, 0) == (1u << 6), "vertex attributes");
    CHECK(find_method(&p, 0x1C00, 0) == (40u | (1u << 12)) && find_method(&p, 0x0604, 0) == 1440, "vertex stream");
    CHECK(find_method(&p, 0x2040, 0) == 0x11 && find_method(&p, 0x2058, 0) == 0x80300000u &&
          find_method(&p, 0x204C, 0) == 12 && find_method(&p, 0x2050, 0) == 0, "vertex stage bound");
    CHECK(find_method(&p, 0x2140, 0) == 0x51 && find_method(&p, 0x2158, 0) == 0x80301000u &&
          find_method(&p, 0x214C, 0) == 20 && find_method(&p, 0x2150, 0) == 4, "pixel stage bound");
    uint32_t want;
    memcpy(&want, &u[5], 4);
    CHECK(find_method(&p, 0x238C, 0) == 0 && find_method(&p, 0x2390, 5) == want, "uniforms loaded");
    CHECK(find_method(&p, 0x238C, 1) == NV_SASS_CB_TEX_OFFSET, "texture handles loaded after them");
    CHECK(find_method(&p, 0x1618, 0) == 4 && find_method(&p, 0x0D78, 0) == 36 &&
          find_method(&p, 0x1614, 0) == 0, "begin, draw, end");
    CHECK(find_method(&p, 0x1918, 0) == 1 && find_method(&p, 0x1920, 0) == 0x405, "back faces culled");

    nv_push_init(&p, words, 4096);
    nv3d_push_clear(&p, &d.color, &z, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x00FF8000u, 1.0f);
    uint32_t one;
    float f1 = 1.0f;
    memcpy(&one, &f1, 4);
    CHECK(find_method(&p, 0x0D80, 0) == one && find_method(&p, 0x0D90, 0) == one &&
          find_method(&p, 0x19D0, 0) == 0x3D, "clear of color and depth");

    uint32_t th[8];
    nv3d_texture_header(th, 0x80500000ull, 1024, 256, 128);
    CHECK((th[0] & 0x7F) == 0x08 && ((th[2] >> 21) & 7) == 2 && (th[1] >> 5) == (0x80500000u >> 5) &&
          (th[3] & 0xFFFF) == 1024 / 32 && (th[4] & 0xFFFF) == 255 && (th[5] & 0xFFFF) == 127 &&
          ((th[4] >> 23) & 0xF) == 7 && (th[5] >> 31) == 1, "pitch-linear texture header");
    uint32_t ts[8];
    nv3d_sampler(ts, FGPU_TEXTURE_LINEAR | FGPU_TEXTURE_CLAMP);
    CHECK(ts[0] == (2u | (2u << 3) | (2u << 6)) && ts[1] == (2u | (2u << 4) | (1u << 6)), "sampler");
}

int main(void) {
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 64; i++) {
            rng = rng * 1664525u + 1013904223u;
            texels[t][i] = rng;
        }
        textures[t] = (fgpu_texture_t){ texels[t], 8, 8, 8, t ? FGPU_TEXTURE_LINEAR : 0 };
    }
    test_encodings();
    test_pushbuffers();
    compare("built-in color", NV_BUILTIN_VS_PLAIN, NV_BUILTIN_FS_PLAIN);
    compare("built-in gouraud", NV_BUILTIN_VS_GOURAUD, NV_BUILTIN_FS_PLAIN);
    compare("built-in phong", NV_BUILTIN_VS_PLAIN, NV_BUILTIN_FS_PHONG);
    compare("textures and discard",
            "uniform mat4 m; attribute vec3 position; attribute vec2 uv; varying vec2 v_uv; varying float v_k;\n"
            "void main() { v_uv = uv; v_k = position.z; gl_Position = m * vec4(position, 1.0); }",
            "uniform sampler2D a; uniform sampler2D b; uniform float cut; varying vec2 v_uv; varying float v_k;\n"
            "void main() { vec4 x = texture(a, v_uv); vec4 y = texture(b, v_uv * 0.5);\n"
            "  if (x.a < cut) discard;\n"
            "  gl_FragColor = vec4(mix(x.rgb, y.bgr, clamp(v_k, 0.0, 1.0)), abs(x.a - y.a)); }");
    compare("math and branches",
            "uniform vec4 k; attribute vec3 position; varying vec3 v;\n"
            "void main() { v = sin(position * k.x) + cos(position.zxy); gl_Position = vec4(position, 1.0 + k.y * 0.1); }",
            "uniform vec4 k; varying vec3 v;\n"
            "void main() { vec3 c = fract(abs(v) * 3.0);\n"
            "  if (v.x > k.z) c = sqrt(c + 0.5); else c = c * c - min(v.yzx, 0.25);\n"
            "  float s = smoothstep(0.2, 0.8, c.x) + exp2(-abs(v.y)) + floor(v.z * 2.0) * 0.1;\n"
            "  gl_FragColor = vec4(c * s, gl_FragCoord.x * 0.01 + gl_FragCoord.y); }");
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("nv_sass_tests: all passed\n");
    return 0;
}
