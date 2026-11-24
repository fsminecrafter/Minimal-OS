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
#include <dirent.h>
#include "x86_64/fgpu_ir.h"
#include "x86_64/nv_sass.h"
#include "x86_64/nv_3d.h"
#include "x86_64/nv_nvdec.h"
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
// Storage buffers as the card sees them: constant buffers NV_SASS_CB_STORAGE + b.
static const float* sb_data[FGPU_MAX_STORAGE_BUFFERS];
static uint32_t sb_bytes[FGPU_MAX_STORAGE_BUFFERS];

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
    // Read before written: the card has leftovers there (the simulator's zeros would hide it).
    bool written[256], p_written[8];
    int uninit_reg, uninit_pred;    // first such register / predicate, -1 = none
    uint32_t ip, uninit_ip, uninit_op;
} sim_t;

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
#define F(lo, hi) nv_sass_field(w, lo, hi)

static uint32_t rd(sim_t* s, uint32_t reg) {
    if (reg == 255) return 0;
    if (reg >= s->regs) s->bad = true;
    if (!s->written[reg] && s->uninit_reg < 0) { s->uninit_reg = (int)reg; s->uninit_ip = s->ip; }
    return s->r[reg];
}

static void wr(sim_t* s, uint32_t reg, uint32_t v) {
    if (reg == 255) return;
    if (reg >= s->regs) { s->bad = true; return; }
    s->r[reg] = v;
    s->written[reg] = true;
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
        s->ip = ip / 4;
        if (exited) { s->bad = true; break; }       // code after EXIT
        uint32_t pred = F(12, 15);
        if (pred != 7 && !s->p_written[pred] && s->uninit_pred < 0) s->uninit_pred = (int)pred;
        bool run = pred == 7 ? true : s->p[pred];
        if (F(15, 16)) run = !run;
        // Scheduling as the encoder promises: wait for both barriers
        // from the second instruction on.
        // Fixed-latency results are covered by the stall (no scoreboard): at least the FP32 latency,
        // longer after a predicate write.
        if (F(116, 122) != (ip ? 3u : 0u) || F(105, 109) < (F(0, 9) == 0x00Bu ? NV_SASS_STALL_PRED : NV_SASS_STALL_ALU))
            s->bad = true;
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
            // The card wants register vectors aligned: the coordinate pair
            // on an even register, the four results on a multiple of 4.
            if ((base & 1u) || (dst & 3u)) { s->bad = true; continue; }
            if (unit >= FGPU_MAX_TEXTURE_UNITS || F(61, 64) != 1 || F(72, 76) != 0xF || F(87, 90) != 1) {
                s->bad = true;
                continue;
            }
            fgpu_texture_sample(&textures[unit], u2f(rd(s, base)), u2f(rd(s, base + 1)), out);
            // Like the card: r, g go to Rd, Rd+1 and b, a to the second
            // result register Rd2 (bits 64..71), Rd2+1. RZ throws them away.
            uint32_t dst2 = F(64, 72);
            wr(s, dst, f2u(out[0]));
            wr(s, dst + 1u, f2u(out[1]));
            if (dst2 != 255u) {
                if (dst2 & 1u) { s->bad = true; continue; }
                wr(s, dst2, f2u(out[2]));
                wr(s, dst2 + 1u, f2u(out[3]));
            }
            continue;
        }
        if (full == 0xB82) {                        // LDC: c[bank][R + offset]
            uint32_t bank = F(54, 59), sb = bank - NV_SASS_CB_STORAGE;
            if (F(73, 76) != 4 || F(78, 80) != 0 || bank < NV_SASS_CB_STORAGE || sb >= FGPU_MAX_STORAGE_BUFFERS) {
                s->bad = true;
                continue;
            }
            uint32_t off = rd(s, F(24, 32)) + F(38, 54), v = 0;
            // Past the bound size (a negative index wraps to a huge offset): zero, as on the card.
            if (sb_data[sb] && off < sb_bytes[sb] && off + 4 <= sb_bytes[sb]) memcpy(&v, (const uint8_t*)sb_data[sb] + off, 4);
            wr(s, dst, v);
            continue;
        }
        uint32_t op = F(0, 9), form = F(9, 12);
        bool has0 = op == 0x009 || op == 0x00B || op == 0x020 || op == 0x021 || op == 0x023 || op == 0x007;
        uint32_t a_bits = has0 ? rd(s, F(24, 32)) : 0;      // only read by the ops that have it
        float a = has0 ? mod(a_bits, F(73, 74), F(72, 73)) : 0.0f;
        uint32_t b_bits;
        float b;
        if (form == 4) { b_bits = F(32, 64); b = u2f(b_bits); }
        else if (form == 5) {
            uint32_t off = F(38, 54), bank = F(54, 59);
            if (bank == 0) {
                if (off + 4 > NV_SASS_CB_UNIFORM_BYTES) s->bad = true;
                memcpy(&b_bits, (const uint8_t*)uniforms + off, 4);
            } else {
                uint32_t sb = bank - NV_SASS_CB_STORAGE;
                if (bank < NV_SASS_CB_STORAGE || sb >= FGPU_MAX_STORAGE_BUFFERS) { s->bad = true; continue; }
                b_bits = 0;                         // past the bound size: zero, as on the card
                if (sb_data[sb] && off + 4 <= sb_bytes[sb]) memcpy(&b_bits, (const uint8_t*)sb_data[sb] + off, 4);
            }
            b = u2f(b_bits);
        } else if (form == 1) { b_bits = rd(s, F(32, 40)); b = mod(b_bits, F(62, 63), F(63, 64)); }
        else { s->bad = true; continue; }
        switch (op) {
            case 0x002: wr(s, dst, b_bits); if (F(72, 76) != 0xF) s->bad = true; break;
            case 0x007:
                if (F(87, 90) != 7 && !s->p_written[F(87, 90)] && s->uninit_pred < 0) s->uninit_pred = (int)F(87, 90);
                wr(s, dst, (s->p[F(87, 90)] != (bool)F(90, 91)) ? a_bits : b_bits);
                break;
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
                s->p_written[0] = true;
                break;
            }
            case 0x020: wr(s, dst, f2u(a * b)); if (F(64, 72) != 255 || F(84, 87) != 4) s->bad = true; break;
            case 0x021: wr(s, dst, f2u(a + b)); break;
            case 0x023: wr(s, dst, f2u(a * b + mod(rd(s, F(64, 72)), F(74, 75), F(75, 76)))); break;
            case 0x105:                             // F2I.S32.TRUNC
                if (F(72, 73) != 1 || F(75, 77) != 2 || F(78, 80) != 3 || F(84, 86) != 2) s->bad = true;
                wr(s, dst, (uint32_t)(int32_t)(b != b ? 0.0f : b < -2147483648.0f ? -2147483648.0f : b > 2147483520.0f ? 2147483520.0f : b));
                break;
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

// Storage buffer loads with constant indices become constant buffer reads;
// a computed index stays on the CPU.
static bool storage_stage(const char* fs, nv_sass_t* sh, const char** why) {
    const char* vs = "attribute vec3 position; varying vec4 v_k;\n"
                     "void main() { v_k = vec4(position, 1.0); gl_Position = vec4(position, 1.0); }";
    if (fglsl_compile(&comp, vs, fs) != 0) { *why = comp.error; return false; }
    if (!fgpu_program_check(comp.words, comp.size, &prog, why)) return false;
    fgpu_ir_t ir = { .code = code, .capacity = CAP, .hash = hash_words };
    if (!fgpu_ir_lower(&ir, &prog, true, why)) return false;
    fgpu_ir_allocate(&ir);
    *sh = (nv_sass_t){ .code = sass_words[1], .capacity = CAP * 8u };
    return nv_sass_encode(ir.code, ir.count, ir.reg_count, true, prog.h.varying_count, sh, why);
}

static void compare(const char* name, const char* vs, const char* fs);

static void test_storage_buffer(void) {
    const char* why = "";
    nv_sass_t sh;
    // Two buffers, an index past the end of the first (reads zero) and a
    // negative one (zero too, like the interpreter).
    const char* fs =
        "layout(binding = 0) buffer vec4 a[];\n"
        "layout(binding = 2) buffer vec4 b[];\n"
        "void main() { gl_FragColor = a[1.0] + b[0.0] * 2.0 + a[7.0] + a[-1.0]; }\n";
    bool ok = storage_stage(fs, &sh, &why);
    if (!ok) printf("  storage: %s\n", why);
    CHECK(ok, "constant-index storage buffer loads encode");
    if (!ok) return;
    CHECK(sh.buffers == 5u, "the stage reports buffers 0 and 2");
    static const float a[3][4] = { { 9, 9, 9, 9 }, { 1, 2, 3, 4 }, { 9, 9, 9, 9 } };
    static const float bb[1][4] = { { 0.5f, -1, 10, 0 } };
    sb_data[0] = &a[0][0]; sb_bytes[0] = sizeof a;
    sb_data[2] = &bb[0][0]; sb_bytes[2] = sizeof bb;
    static sim_t s;
    memset(&s, 0, sizeof s);
    s.regs = sh.regs - NV_SASS_HW_RESERVED_REGS;
    bool ran = sim_run(&s, &sh, true) && !s.bad;
    CHECK(ran, "storage buffer shader runs in the simulator");
    const float want[4] = { 2, 0, 23, 4 };
    for (int c = 0; c < 4 && ran; c++) CHECK(close_enough(u2f(s.r[c]), want[c]), "storage buffer values");
    sb_data[0] = sb_data[2] = NULL;

    // A computed index: LDC with a register offset. compare() runs it against
    // the interpreter with indices in range, past the end and negative.
    const char* dyn_vs = "attribute vec3 position; varying vec4 v_k;\n"
                         "void main() { v_k = vec4(position, 1.0); gl_Position = vec4(position, 1.0); }";
    const char* dyn =
        "layout(binding = 0) buffer vec4 a[];\n"
        "layout(binding = 1) buffer vec4 b[];\n"
        "varying vec4 v_k;\n"
        "void main() { gl_FragColor = a[v_k.x * 4.0 + 3.0] + b[v_k.y * 6.0] * 0.5 + a[14.0 + floor(v_k.z * 2.0) * 10.0]; }\n";
    CHECK(storage_stage(dyn, &sh, &why), "a computed storage buffer index encodes");
    compare("computed buffer index", dyn_vs, dyn);
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
          (fsh.sph[18] & 0xF) == 0xF && (((fsh.sph[0] >> 15) & 1u) != 0) == fsh.kills &&
          (fsh.sph[5] & 0x80000000u), msg);   // IMAP_POSITION_W, or the card traps (SPH11)
    // Every varying the vertex stage writes is one the pixel stage reads.
    for (uint32_t c = 0; c < nvar * 4u; c++) {
        bool wrote = (vsh.sph[(432 + c) / 32] >> ((432 + c) % 32)) & 1u;
        uint32_t read = (fsh.sph[(192 + c * 2) / 32] >> ((192 + c * 2) % 32)) & 3u;
        if (!wrote || read != 2) { snprintf(msg, sizeof msg, "%s: varying maps agree", name); CHECK(false, msg); break; }
    }

    int bad = 0;
    // Storage buffers with random contents, for shaders that read them (the
    // simulator and the interpreter see the same 40 vec4s per buffer).
    static float sb_vals[FGPU_MAX_STORAGE_BUFFERS][40][4];
    static fgpu_storage_buffer_t sbufs[FGPU_MAX_STORAGE_BUFFERS];
    for (int b = 0; b < (int)FGPU_MAX_STORAGE_BUFFERS; b++) {
        for (int i = 0; i < 40; i++)
            for (int c = 0; c < 4; c++) sb_vals[b][i][c] = frand(-2.0f, 2.0f);
        sbufs[b] = (fgpu_storage_buffer_t){ (const float (*)[4])sb_vals[b], 40 };
        sb_data[b] = &sb_vals[b][0][0];
        sb_bytes[b] = sizeof sb_vals[b];
    }
    for (int round = 0; round < 200 && !bad; round++) {
        for (int i = 0; i < (int)FGPU_SHADER_MAX_UNIFORMS; i++)
            for (int c = 0; c < 4; c++) uniforms[i][c] = frand(-2.0f, 2.0f);
        // Vertex stage.
        float in[FGPU_MAX_ATTRIBS][4], out[1 + FGPU_SHADER_MAX_VARYINGS][4];
        static sim_t s;
        memset(&s, 0, sizeof s);
        s.regs = vsh.regs - NV_SASS_HW_RESERVED_REGS;   // the code may not use those
        s.uninit_reg = s.uninit_pred = -1;
        for (int i = 0; i < (int)FGPU_MAX_ATTRIBS; i++)
            for (int c = 0; c < 4; c++) {
                in[i][c] = frand(-1.5f, 1.5f);
                s.attr_in[(NV_SASS_ATTR_GENERIC + 16 * i + 4 * c) / 4] = in[i][c];
            }
        fgpu_shader_io_t io = { &prog, in, out, uniforms, NULL, textures, sbufs };
        fgpu_shader_run_vertex(&io);
        if (!sim_run(&s, &vsh, false)) { bad = 1; break; }
        if (s.uninit_reg >= 0 || s.uninit_pred >= 0) {
            printf("  vertex stage reads R%d / P%d before writing it\n", s.uninit_reg, s.uninit_pred);
            bad = 1;
            break;
        }
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
        s.regs = fsh.regs - NV_SASS_HW_RESERVED_REGS;
        s.uninit_reg = s.uninit_pred = -1;
        for (int i = 0; i < (int)FGPU_MAX_ATTRIBS; i++)
            for (int c = 0; c < 4; c++) {
                in[i][c] = frand(-1.5f, 1.5f);
                s.attr_in[(NV_SASS_ATTR_GENERIC + 16 * i + 4 * c) / 4] = in[i][c];
            }
        for (int c = 0; c < 4; c++) s.attr_in[(NV_SASS_ATTR_POSITION + 4 * c) / 4] = special[0][c];
        // Like the card: perspective attributes come out of the interpolator divided by w, and
        // position.w is 1 / w (gl_FragCoord.w); the shader multiplies by w itself.
        for (int i = 0; i < (int)FGPU_MAX_ATTRIBS; i++)
            for (int c = 0; c < 4; c++)
                s.attr_in[(NV_SASS_ATTR_GENERIC + 16 * i + 4 * c) / 4] = in[i][c] * special[0][3];
        fgpu_shader_io_t fio = { &prog, in, color, uniforms, special, textures, sbufs };
        bool kept = fgpu_shader_run_fragment(&fio);
        if (!sim_run(&s, &fsh, true)) { bad = 1; break; }
        if (s.uninit_reg >= 0 || s.uninit_pred >= 0) {
            printf("  pixel stage reads R%d / P%d before writing it (instruction %u of %u, opcode 0x%03x)\n", s.uninit_reg, s.uninit_pred, s.uninit_ip, fsh.count / 4, nv_sass_field(&fsh.code[s.uninit_ip * 4], 0, 12));
            bad = 1;
            break;
        }
        if (kept == s.killed) { printf("  discard differs\n"); bad = 1; }
        for (int c = 0; c < 4 && kept && !bad; c++)
            if (!close_enough(u2f(s.r[c]), color[0][c])) {
                printf("  color.%d: machine code %g, CPU %g\n", c, u2f(s.r[c]), color[0][c]);
                bad = 1;
            }
    }
    for (int b = 0; b < (int)FGPU_MAX_STORAGE_BUFFERS; b++) sb_data[b] = NULL;
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
    nv3d_push_init(&p, 0x80000000ull, 0x80001000ull, 0x80002000ull);
    CHECK(!p.overflow &&
          find_method(&p, NVC597_SET_SHADER_LOCAL_MEMORY_WINDOW, 0) == (0xFFu << 24),
          "shader local-memory window is initialized");

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
          find_method(&p, 0x116C, 0) == ((1u << 6) | (0x12u << 21) | (7u << 27)), "vertex attributes");
    CHECK(find_method(&p, 0x1C00, 0) == (40u | (1u << 12)) && find_method(&p, 0x0604, 0) == 1440, "vertex stream");
    {
        // A draw with depth goes to a block-linear color target: width and
        // height in pixels (rounded to GOBs), block-linear layout, layer size.
        uint32_t bw, bh, bytes;
        nv3d_color_bl_size(100, 50, &bw, &bh, &bytes);
        CHECK(bw == 112 && bh == 56 && bytes == 112u * 56u * 4u, "block-linear color rounded to GOBs");
        static uint32_t words2[4096];
        nv_push_t p2;
        nv_push_init(&p2, words2, 4096);
        nv3d_draw_t d2 = d;
        d2.color = (nv3d_color_t){ 0x80500000ull, 0, 100, 50, true, bw, bh };
        nv3d_push_draw(&p2, &d2);
        CHECK(find_method(&p2, 0x0E00, 0) == 0, "scissor off by default");
        nv3d_draw_t d3 = d;
        d3.scissor = true;
        d3.sc_x0 = 10; d3.sc_y0 = 5; d3.sc_x1 = 60; d3.sc_y1 = 40;
        nv_push_init(&p2, words2, 4096);
        nv3d_push_draw(&p2, &d3);
        CHECK(find_method(&p2, 0x0E00, 0) == 1 && find_method(&p2, 0x0E04, 0) == (10u | (60u << 16)) &&
              find_method(&p2, 0x0E08, 0) == (5u | (40u << 16)), "draw scissor");
        nv_push_init(&p2, words2, 4096);
        nv3d_push_clear_rect(&p2, &d2.color, &z, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x123456, 1.0f, 2, 3, 30, 20);
        CHECK(find_method(&p2, 0x0E00, 0) == 1 && find_method(&p2, 0x0E04, 0) == (2u | (30u << 16)) &&
              find_method(&p2, 0x10F8, 0) == (1u << 8) && find_method(&p2, 0x19D0, 0) == 0x3Du, "scissored clear");
        nv_push_init(&p2, words2, 4096);
        nv3d_push_draw(&p2, &d2);
        CHECK(!p2.overflow && find_method(&p2, 0x0804, 0) == 0x80500000u && find_method(&p2, 0x0808, 0) == 112 &&
              find_method(&p2, 0x080C, 0) == 56 && find_method(&p2, 0x0814, 0) == 0 &&
              find_method(&p2, 0x081C, 0) == (112u * 56u * 4u) / 4u && find_method(&p2, 0x0FF4, 0) == (100u << 16),
              "block-linear color target");
    }
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
    nv3d_texture_header(th, 0x80500000ull, 1024, 256, 128, NV3D_TEX_ARGB, 0);
    CHECK((th[0] & 0x7F) == 0x08 && ((th[2] >> 21) & 7) == 2 && (th[1] >> 5) == (0x80500000u >> 5) &&
          (th[3] & 0xFFFF) == 1024 / 32 && (th[4] & 0xFFFF) == 255 && (th[5] & 0xFFFF) == 127 &&
          ((th[4] >> 23) & 0xF) == 7 && (th[5] >> 31) == 1, "pitch-linear texture header");
    uint32_t ts[8];
    nv3d_sampler(ts, FGPU_TEXTURE_LINEAR | FGPU_TEXTURE_CLAMP);
    CHECK(ts[0] == (2u | (2u << 3) | (2u << 6)) && ts[1] == (2u | (2u << 4) | (1u << 6)), "sampler");
}

static void test_nvdec(void) {
    _Static_assert(sizeof(nvdec_h264_pic_s) == 764, "nvdec_h264_pic_s as in NVIDIA's nvdec_drv.h");
    nvdec_layout_t l;
    nvdec_layout(80, 45, &l);           // 1280x720
    CHECK(l.width == 1280 && l.height == 720 && l.pitch == 1280 && l.block_log2 == 4 &&
          l.luma_bytes >= 1280u * 720u && l.chroma_bytes >= 1280u * 360u && !(l.luma_bytes & 4095u),
          "NV12 picture layout");
    nvdec_layout(1, 1, &l);
    CHECK(l.block_log2 == 1 && l.pitch == 64, "small pictures use small blocks");

    static syscall_media_h264_t pic;
    memset(&pic, 0, sizeof pic);
    pic.mb_w = 10; pic.mb_h = 6; pic.max_refs = 3; pic.log2_max_frame_num_minus4 = 2;
    pic.num_ref_l0_default_minus1 = 2; pic.pic_init_qp_minus26 = -3; pic.chroma_qp_offset = 2;
    pic.second_chroma_qp_offset = 2; pic.deblocking_control = 1; pic.is_reference = 1;
    pic.frame_num = 5; pic.poc = 10; pic.cur_index = 3; pic.ref_count = 2; pic.slice_count = 2;
    pic.refs[0] = (syscall_media_h264_ref_t){ 1, 0, 0, 4, 8 };
    pic.refs[1] = (syscall_media_h264_ref_t){ 2, 1, 0, 0, 2 };
    nvdec_layout(10, 6, &l);
    static nvdec_h264_pic_s st;
    nvdec_h264_setup(&st, &pic, &l, 1000);
    CHECK(st.stream_len == 1016 && st.slice_count == 2 && st.explicitEOSPresentFlag == 1 &&
          !memcmp(st.eos, nvdec_h264_eos, 16), "stream length and end-of-stream marker");
    CHECK(st.PicWidthInMbs == 10 && st.FrameHeightInMbs == 6 && st.frame_mbs_only_flag == 1 &&
          st.pitch_luma == 192 && st.chroma_format_idc == 1 && st.tileFormat == 1 &&
          st.gob_height == l.block_log2 - 1, "picture size and layout");
    CHECK(st.pic_init_qp_minus26 == -3 && st.chroma_qp_index_offset == 2 && st.log2_max_frame_num_minus4 == 2 &&
          st.num_ref_idx_l0_active_minus1 == 2 && st.deblocking_filter_control_present_flag == 1 &&
          st.ref_pic_flag == 1 && st.frame_num == 5 && st.CurrFieldOrderCnt[0] == 10 && st.CurrPicIdx == 3,
          "parameter sets and the picture's numbers");
    CHECK(st.dpb[0].index == 1 && st.dpb[0].state == 3 && st.dpb[0].top_field_marking == 1 &&
          st.dpb[0].FrameIdx == 4 && st.dpb[0].FieldOrderCnt[0] == 8 && st.dpb[1].is_long_term == 1 &&
          st.dpb[1].bottom_field_marking == 2 && st.dpb[2].state == 0, "reference pictures");
    CHECK(st.WeightScale[5][3][3] == 16 && st.WeightScale8x8[1][7][7] == 16, "flat scaling matrices");

    static uint32_t words[256];
    nv_push_t p;
    nv_push_init(&p, words, 256);
    nvdec_job_t job = { .setup = 0x80100000ull, .stream = 0x80200000ull, .slice_offsets = 0x80101000ull,
                        .coloc = 0x80300000ull, .history = 0x80400000ull, .mbhist = 0x80500000ull,
                        .status = 0x80102000ull, .picture_number = 7, .max_refs = 3 };
    job.luma[3] = 0x80600000ull;
    job.chroma[3] = 0x80700000ull;
    nvdec_push_h264(&p, &job);
    CHECK(!p.overflow, "a decode job fits");
    CHECK(find_method(&p, 0x0000, 0) == NV_CLASS_NVDEC_TURING && find_method(&p, NVDEC_SET_APPLICATION_ID, 0) == 3,
          "decoder class, H.264");
    CHECK((find_method(&p, NVDEC_SET_CONTROL_PARAMS, 0) & 15u) == 3 &&
          ((find_method(&p, NVDEC_SET_CONTROL_PARAMS, 0) >> 7) & 63u) == 1, "control parameters");
    CHECK(find_method(&p, NVDEC_SET_DRV_PIC_SETUP_OFFSET, 0) == 0x801000u &&
          find_method(&p, NVDEC_SET_IN_BUF_BASE_OFFSET, 0) == 0x802000u &&
          find_method(&p, NVDEC_SET_PICTURE_LUMA_OFFSET(3), 0) == 0x806000u &&
          find_method(&p, NVDEC_SET_PICTURE_CHROMA_OFFSET(3), 0) == 0x807000u &&
          find_method(&p, NVDEC_SET_PICTURE_LUMA_OFFSET(0), 0) == 0 &&
          find_method(&p, NVDEC_H264_SET_MBHIST_BUF_OFFSET, 0) == 0x805000u, "buffers in units of 256 bytes");
    CHECK(find_method(&p, NVDEC_SET_PICTURE_INDEX, 0) == 7 && p.words[p.count - 1] == 0 &&
          ((p.words[p.count - 2] & 0xFFFu) << 2) == NVDEC_EXECUTE, "the job ends with EXECUTE");

    uint32_t th[8];
    nv3d_texture_header(th, 0x80600000ull, 192, 160, 96, NV3D_TEX_R8_BL, 2);
    CHECK((th[0] & 0x7F) == 0x1D && ((th[2] >> 21) & 7) == 3 && (th[1] >> 9) == (0x80600000u >> 9) &&
          ((th[3] >> 3) & 7) == 2 && ((th[4] >> 23) & 0xF) == 1, "block-linear luma texture header");
    nv3d_texture_header(th, 0x80700000ull, 192, 80, 48, NV3D_TEX_RG8_BL, 2);
    CHECK((th[0] & 0x7F) == 0x18 && ((th[0] >> 22) & 7) == 3, "block-linear colour texture header");
}

// NV_SASS_SHADER_DIR=<dir>: every <dir>/NAME.vs + NAME.fs pair (Farcade's shader_compile_tests writes
// them with FE_DUMP_DIR) through the encoder and the simulator.
static char* read_text(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = (char*)malloc((size_t)n + 1);
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) { free(t); t = NULL; }
    if (t) t[n] = 0;
    fclose(f);
    return t;
}

static int name_cmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

static void test_shader_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) { printf("FAIL: cannot open %s\n", dir); failures++; return; }
    char* names[4096];
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < 4096) {
        size_t l = strlen(e->d_name);
        if (l > 3 && !strcmp(e->d_name + l - 3, ".vs")) { names[n] = (char*)malloc(l + 1); memcpy(names[n++], e->d_name, l + 1); }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], name_cmp);
    int before = failures;
    for (int i = 0; i < n; i++) {
        char path[1024], label[160];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        char* vs = read_text(path);
        size_t l = strlen(path);
        path[l - 2] = 'f';
        char* fs = read_text(path);
        memcpy(path + l - 2, "name", 5);
        char* what = read_text(path);
        snprintf(label, sizeof label, "%.*s %s", (int)(strlen(names[i]) - 3), names[i], what ? what : "");
        if (vs && fs) compare(label, vs, fs);
        free(vs); free(fs); free(what); free(names[i]);
    }
    printf("shader dir: %d programs, %d failing checks\n", n, failures - before);
}

int main(void) {
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 64; i++) {
            rng = rng * 1664525u + 1013904223u;
            texels[t][i] = rng;
        }
        textures[t] = (fgpu_texture_t){ .pixels = texels[t], .width = 8, .height = 8, .pitch = 8, .flags = t ? FGPU_TEXTURE_LINEAR : 0 };
    }
    if (getenv("NV_SASS_SHADER_DIR")) {
        test_shader_dir(getenv("NV_SASS_SHADER_DIR"));
        return failures ? 1 : 0;
    }
    test_encodings();
    test_pushbuffers();
    test_nvdec();
    test_storage_buffer();
    compare("built-in color", NV_BUILTIN_VS_PLAIN, NV_BUILTIN_FS_PLAIN);
    compare("video NV12 to RGB", NV_VIDEO_VS, NV_VIDEO_NV12_FS);
    compare("built-in gouraud", NV_BUILTIN_VS_GOURAUD, NV_BUILTIN_FS_PLAIN);
    compare("built-in phong", NV_BUILTIN_VS_PLAIN, NV_BUILTIN_FS_PHONG);
    compare("textures and discard",
            "uniform mat4 m; attribute vec3 position; attribute vec2 uv; varying vec2 v_uv; varying float v_k;\n"
            "void main() { v_uv = uv; v_k = position.z; gl_Position = m * vec4(position, 1.0); }",
            "uniform sampler2D a; uniform sampler2D b; uniform float cut; varying vec2 v_uv; varying float v_k;\n"
            "void main() { vec4 x = texture(a, v_uv); vec4 y = texture(b, v_uv * 0.5);\n"
            "  if (x.a < cut) discard;\n"
            "  gl_FragColor = vec4(mix(x.rgb, y.bgr, clamp(v_k, 0.0, 1.0)), abs(x.a - y.a)); }");
    // Varyings read after a texture lookup: the TEX coordinates must not land on the register that
    // holds w for perspective (they did when the first scratch register was odd). Two shaders with
    // different register counts, so both parities are covered.
    for (int extra = 0; extra < 4; extra++) {
        char fs[1024];
        snprintf(fs, sizeof fs,
                 "uniform sampler2D a; uniform vec4 k; varying vec2 v_uv; varying vec3 v_n; varying vec3 v_c;\n"
                 "void main() { vec4 t = texture(a, v_uv);\n"
                 "  vec3 q = vec3(k.x, k.y, k.z)%s;\n"
                 "  gl_FragColor = vec4(t.rgb * normalize(v_n) + v_c * q, t.a); }",
                 extra == 0 ? "" : extra == 1 ? " * k.w" : extra == 2 ? " * fract(k.w * 3.0) + k.xyz" :
                 " + sin(k.yzx) * cos(k.zxy)");
        char name[64];
        snprintf(name, sizeof name, "varyings after a lookup %d", extra);
        compare(name,
                "uniform mat4 m; attribute vec3 position; attribute vec2 uv; attribute vec3 normal;\n"
                "varying vec2 v_uv; varying vec3 v_n; varying vec3 v_c;\n"
                "void main() { v_uv = uv; v_n = normal; v_c = position * 0.5; gl_Position = m * vec4(position, 1.0); }",
                fs);
    }
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
