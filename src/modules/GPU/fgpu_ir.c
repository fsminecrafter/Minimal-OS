#include "x86_64/fgpu_ir.h"
#include <stddef.h>

/*
 * FGPU bytecode -> scalar IR (see fgpu_ir.h for the design).
 */

#define PI_F 3.14159265358979323846f

static inline uint32_t f2u(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
static inline float fabs_(float x) { return x < 0.0f ? -x : x; }

// floor() as the CPU interpreter does it: |x| >= 2^23 is already whole.
static float floor_(float x) {
    if (!(fabs_(x) < 8388608.0f)) return x;
    float t = (float)(int32_t)x;
    return t > x ? t - 1.0f : t;
}

/* ------------------------------------------------------------- builder */

typedef fgpu_ir_src_t src_t;

typedef struct {
    fgpu_ir_t* ir;
    const fgpu_program_t* p;
    bool overflow;
    src_t temp[FGPU_SHADER_MAX_TEMPS][4];
    src_t out[1 + FGPU_SHADER_MAX_VARYINGS][4];
    src_t wait;         // instruction this thread resumes at (as a float)
    src_t killed;       // 1.0 once a KILL hit this thread
    src_t active;       // 1.0 if the thread runs the current instruction
    bool active_known;  // active is 1.0 for every thread (no jump spans pc)
} lw_t;

static inline bool is_imm(const lw_t* w, src_t s) {
    return w->ir->code[s.value].op == FGPU_IR_IMM;
}

// The value of an immediate source, with its modifiers applied.
static inline float imm_of(const lw_t* w, src_t s) {
    float v = w->ir->code[s.value].imm;
    if (s.mods & FGPU_IR_ABS) v = fabs_(v);
    if (s.mods & FGPU_IR_NEG) v = -v;
    return v;
}

static inline bool same(src_t a, src_t b) { return a.value == b.value && a.mods == b.mods; }

// Applies modifiers to a source: |(+-x)| = |x|, then the sign.
static inline src_t modify(src_t s, uint32_t m) {
    if (m & FGPU_IR_ABS) s.mods = FGPU_IR_ABS | (m & FGPU_IR_NEG);
    else if (m & FGPU_IR_NEG) s.mods ^= FGPU_IR_NEG;
    return s;
}

static inline src_t neg(src_t s) { return modify(s, FGPU_IR_NEG); }

static uint32_t hash_inst(const fgpu_ir_inst_t* in) {
    uint32_t h = 2166136261u;
    uint32_t k[3 + 2 * 3] = { in->op | ((uint32_t)in->nsrc << 8), in->aux, f2u(in->imm) };
    for (uint32_t i = 0; i < 3; i++) {
        k[3 + 2 * i] = i < in->nsrc ? in->src[i].value : 0xFFFFFFFFu;
        k[4 + 2 * i] = i < in->nsrc ? in->src[i].mods : 0;
    }
    for (uint32_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        h ^= k[i];
        h *= 16777619u;
    }
    return h;
}

static bool equal_inst(const fgpu_ir_inst_t* a, const fgpu_ir_inst_t* b) {
    if (a->op != b->op || a->nsrc != b->nsrc || a->aux != b->aux || f2u(a->imm) != f2u(b->imm))
        return false;
    for (uint32_t i = 0; i < a->nsrc; i++)
        if (!same(a->src[i], b->src[i])) return false;
    return true;
}

static bool cse_allowed(uint32_t op) {
    return op != FGPU_IR_OUTPUT && op != FGPU_IR_DISCARD;
}

// Appends an instruction, or returns an equal one emitted before.
static src_t append(lw_t* w, const fgpu_ir_inst_t* in) {
    fgpu_ir_t* ir = w->ir;
    uint32_t size = FGPU_IR_HASH_WORDS(ir->capacity);
    uint32_t slot = 0;
    bool cse = cse_allowed(in->op);
    if (cse) {
        slot = hash_inst(in) % size;
        for (;;) {
            uint32_t e = ir->hash[slot];
            if (!e) break;
            if (equal_inst(&ir->code[e - 1], in)) return (src_t){ e - 1, 0 };
            slot = (slot + 1) % size;
        }
    }
    if (ir->count >= ir->capacity) {
        w->overflow = true;
        return (src_t){ 0, 0 };
    }
    uint32_t index = ir->count++;
    ir->code[index] = *in;
    ir->code[index].reg = FGPU_IR_NO_REG;
    if (cse) ir->hash[slot] = index + 1;
    return (src_t){ index, 0 };
}

static src_t imm(lw_t* w, float v) {
    fgpu_ir_inst_t in = { .op = FGPU_IR_IMM, .imm = v };
    return append(w, &in);
}

static bool commutative(uint32_t op) {
    return op == FGPU_IR_ADD || op == FGPU_IR_MUL || op == FGPU_IR_MIN || op == FGPU_IR_MAX ||
           op == FGPU_IR_SEQ || op == FGPU_IR_SNE;
}

// Folds an operation whose sources are all immediates. Transcendental
// functions are left to the hardware.
static bool fold(uint32_t op, const float* v, float* r) {
    switch (op) {
        case FGPU_IR_ADD:   *r = v[0] + v[1]; return true;
        case FGPU_IR_MUL:   *r = v[0] * v[1]; return true;
        case FGPU_IR_FMA:   *r = v[0] * v[1] + v[2]; return true;
        case FGPU_IR_MIN:   *r = v[0] < v[1] ? v[0] : v[1]; return true;
        case FGPU_IR_MAX:   *r = v[0] > v[1] ? v[0] : v[1]; return true;
        case FGPU_IR_FLOOR: *r = floor_(v[0]); return true;
        case FGPU_IR_RCP:
            if (v[0] == 0.0f) return false;
            *r = 1.0f / v[0];
            return true;
        case FGPU_IR_SLT:   *r = v[0] <  v[1] ? 1.0f : 0.0f; return true;
        case FGPU_IR_SLE:   *r = v[0] <= v[1] ? 1.0f : 0.0f; return true;
        case FGPU_IR_SEQ:   *r = v[0] == v[1] ? 1.0f : 0.0f; return true;
        case FGPU_IR_SNE:   *r = v[0] != v[1] ? 1.0f : 0.0f; return true;
        case FGPU_IR_SEL:   *r = v[0] != 0.0f ? v[1] : v[2]; return true;
        default:            return false;
    }
}

static src_t op3(lw_t* w, uint32_t op, uint32_t n, src_t a, src_t b, src_t c) {
    src_t s[3] = { a, b, c };
    // Immediates carry no modifiers: fold them into the value.
    bool all_imm = true;
    for (uint32_t i = 0; i < n; i++) {
        if (is_imm(w, s[i])) {
            if (s[i].mods) s[i] = imm(w, imm_of(w, s[i]));
        } else {
            all_imm = false;
        }
    }
    if (w->overflow) return (src_t){ 0, 0 };
    if (all_imm) {
        float v[3] = { 0, 0, 0 }, r;
        for (uint32_t i = 0; i < n; i++) v[i] = imm_of(w, s[i]);
        if (fold(op, v, &r)) return imm(w, r);
    }
    // Simplifications. All are exact except x * 0 = 0 for an infinite or
    // NaN x, which GLSL allows (and GPU compilers do).
    switch (op) {
        case FGPU_IR_SEL:
            if (is_imm(w, s[0])) return imm_of(w, s[0]) != 0.0f ? s[1] : s[2];
            if (same(s[1], s[2])) return s[1];
            break;
        case FGPU_IR_ADD:
            if (is_imm(w, s[1]) && imm_of(w, s[1]) == 0.0f) return s[0];
            if (is_imm(w, s[0]) && imm_of(w, s[0]) == 0.0f) return s[1];
            break;
        case FGPU_IR_MUL:
            for (int i = 0; i < 2; i++) {
                if (!is_imm(w, s[i])) continue;
                float k = imm_of(w, s[i]);
                if (k == 1.0f) return s[1 - i];
                if (k == -1.0f) return neg(s[1 - i]);
                if (k == 0.0f) return imm(w, 0.0f);
            }
            break;
        case FGPU_IR_FMA:
            for (int i = 0; i < 2; i++) {
                if (!is_imm(w, s[i])) continue;
                float k = imm_of(w, s[i]);
                if (k == 1.0f) return op3(w, FGPU_IR_ADD, 2, s[1 - i], s[2], s[2]);
                if (k == 0.0f) return s[2];
            }
            if (is_imm(w, s[2]) && imm_of(w, s[2]) == 0.0f)
                return op3(w, FGPU_IR_MUL, 2, s[0], s[1], s[1]);
            break;
        case FGPU_IR_MIN:
        case FGPU_IR_MAX:
            if (same(s[0], s[1])) return s[0];
            break;
        default:
            break;
    }
    // A canonical order, so a + b and b + a are one value.
    if (commutative(op) &&
        (s[0].value > s[1].value || (s[0].value == s[1].value && s[0].mods > s[1].mods))) {
        src_t t = s[0]; s[0] = s[1]; s[1] = t;
    }
    fgpu_ir_inst_t in = { .op = (uint8_t)op, .nsrc = (uint8_t)n };
    for (uint32_t i = 0; i < n; i++) in.src[i] = s[i];
    return append(w, &in);
}

static inline src_t op1(lw_t* w, uint32_t op, src_t a) { return op3(w, op, 1, a, a, a); }
static inline src_t op2(lw_t* w, uint32_t op, src_t a, src_t b) { return op3(w, op, 2, a, b, b); }

static src_t load(lw_t* w, uint32_t op, uint32_t aux) {
    fgpu_ir_inst_t in = { .op = (uint8_t)op, .aux = aux };
    return append(w, &in);
}

/* ------------------------------------------------------------ operands */

// Component `comp` of a source operand word.
static src_t read(lw_t* w, uint32_t word, uint32_t comp) {
    uint32_t file = FGPU_OPERAND_FILE(word), index = FGPU_OPERAND_INDEX(word);
    uint32_t k = (FGPU_OPERAND_SWIZZLE(word) >> (2 * comp)) & 3;
    src_t s;
    switch (file) {
        case FGPU_FILE_TEMP:    s = w->temp[index][k]; break;
        case FGPU_FILE_OUTPUT:  s = w->out[index][k]; break;
        case FGPU_FILE_INPUT:   s = load(w, FGPU_IR_INPUT, FGPU_IR_SLOT(index, k)); break;
        case FGPU_FILE_SPECIAL: s = load(w, FGPU_IR_SPECIAL, FGPU_IR_SLOT(index, k)); break;
        case FGPU_FILE_UNIFORM: s = load(w, FGPU_IR_UNIFORM, FGPU_IR_SLOT(index, k)); break;
        case FGPU_FILE_CONST:   s = imm(w, w->p->consts[index][k]); break;
        default:                s = imm(w, 0.0f); break;
    }
    uint32_t m = 0;
    if (word & FGPU_SRC_ABS) m |= FGPU_IR_ABS;
    if (word & FGPU_SRC_NEGATE) m |= FGPU_IR_NEG;
    s = modify(s, m);
    if (m && is_imm(w, s)) s = imm(w, imm_of(w, s));
    return s;
}

static void write(lw_t* w, uint32_t d, uint32_t comp, src_t v) {
    src_t* r = FGPU_OPERAND_FILE(d) == FGPU_FILE_OUTPUT ? &w->out[FGPU_OPERAND_INDEX(d)][comp]
                                                        : &w->temp[FGPU_OPERAND_INDEX(d)][comp];
    if (!w->active_known) {
        src_t sel = op3(w, FGPU_IR_SEL, 3, w->active, v, *r);
        if (!same(sel, v)) w->ir->selects_for_jumps++;
        v = sel;
    }
    *r = v;
}

/* --------------------------------------------------- operation lowering */

// a / b with x / 0 = 0, as the CPU backend defines it.
static src_t div_(lw_t* w, src_t a, src_t b) {
    src_t q = op2(w, FGPU_IR_MUL, a, op1(w, FGPU_IR_RCP, b));
    return op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SNE, b, imm(w, 0.0f)), q, imm(w, 0.0f));
}

static src_t atan2_(lw_t* w, src_t y, src_t x) {
    // atan(y / x) by range reduction and a polynomial, then the quadrant
    // (the same steps and coefficients as the CPU backend).
    src_t r = op2(w, FGPU_IR_MUL, y, op1(w, FGPU_IR_RCP, x));
    src_t ar = modify(r, FGPU_IR_ABS);
    src_t inv = op2(w, FGPU_IR_SLT, imm(w, 1.0f), ar);
    src_t z = op3(w, FGPU_IR_SEL, 3, inv, op1(w, FGPU_IR_RCP, ar), ar);
    src_t z2 = op2(w, FGPU_IR_MUL, z, z);
    static const float k[] = { 0.05265332f, -0.11643287f, 0.19354346f, -0.33262347f, 0.99997726f };
    src_t p = op3(w, FGPU_IR_FMA, 3, z2, imm(w, -0.01172120f), imm(w, k[0]));
    for (int i = 1; i < 5; i++) p = op3(w, FGPU_IR_FMA, 3, z2, p, imm(w, k[i]));
    p = op2(w, FGPU_IR_MUL, z, p);
    p = op3(w, FGPU_IR_SEL, 3, inv, op2(w, FGPU_IR_ADD, imm(w, PI_F / 2), neg(p)), p);
    src_t at = op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, r, imm(w, 0.0f)), neg(p), p);
    src_t zero = imm(w, 0.0f);
    src_t left = op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLE, zero, y),
                     op2(w, FGPU_IR_ADD, at, imm(w, PI_F)), op2(w, FGPU_IR_ADD, at, imm(w, -PI_F)));
    src_t axis = op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, zero, y), imm(w, PI_F / 2),
                     op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, y, zero), imm(w, -PI_F / 2), zero));
    return op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, zero, x), at,
               op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, x, zero), left, axis));
}

// One component of a component-wise operation.
static src_t lower_comp(lw_t* w, uint32_t op, src_t a, src_t b, src_t c) {
    src_t zero = imm(w, 0.0f);
    switch (op) {
        case FGPU_SOP_MOV:   return a;
        case FGPU_SOP_ADD:   return op2(w, FGPU_IR_ADD, a, b);
        case FGPU_SOP_SUB:   return op2(w, FGPU_IR_ADD, a, neg(b));
        case FGPU_SOP_MUL:   return op2(w, FGPU_IR_MUL, a, b);
        case FGPU_SOP_DIV:   return div_(w, a, b);
        case FGPU_SOP_MAD:   return op3(w, FGPU_IR_FMA, 3, a, b, c);
        case FGPU_SOP_MIN:   return op2(w, FGPU_IR_MIN, a, b);
        case FGPU_SOP_MAX:   return op2(w, FGPU_IR_MAX, a, b);
        case FGPU_SOP_FLOOR: return op1(w, FGPU_IR_FLOOR, a);
        case FGPU_SOP_FRACT: return op2(w, FGPU_IR_ADD, a, neg(op1(w, FGPU_IR_FLOOR, a)));
        case FGPU_SOP_SQRT:  return op1(w, FGPU_IR_SQRT, op2(w, FGPU_IR_MAX, a, zero));
        case FGPU_SOP_RSQ:
            return op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, zero, a), op1(w, FGPU_IR_RSQ, a), zero);
        case FGPU_SOP_EXP2:  return op1(w, FGPU_IR_EX2, a);
        case FGPU_SOP_LOG2:
            return op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, zero, a), op1(w, FGPU_IR_LG2, a),
                       imm(w, -126.0f));
        case FGPU_SOP_POW: {
            src_t e = op1(w, FGPU_IR_EX2, op2(w, FGPU_IR_MUL, b, op1(w, FGPU_IR_LG2, a)));
            return op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, zero, a), e,
                       op2(w, FGPU_IR_SEQ, b, zero));
        }
        case FGPU_SOP_SIN:   return op1(w, FGPU_IR_SIN, a);
        case FGPU_SOP_COS:   return op1(w, FGPU_IR_COS, a);
        case FGPU_SOP_MIX:   return op3(w, FGPU_IR_FMA, 3, op2(w, FGPU_IR_ADD, b, neg(a)), c, a);
        case FGPU_SOP_CLAMP: return op2(w, FGPU_IR_MIN, op2(w, FGPU_IR_MAX, a, b), c);
        case FGPU_SOP_STEP:  return op2(w, FGPU_IR_SLE, a, b);
        case FGPU_SOP_SSTEP: {
            src_t d = op2(w, FGPU_IR_ADD, b, neg(a));
            src_t q = op2(w, FGPU_IR_MUL, op2(w, FGPU_IR_ADD, c, neg(a)), op1(w, FGPU_IR_RCP, d));
            src_t t = op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SNE, d, zero), q, op2(w, FGPU_IR_SLE, a, c));
            t = op2(w, FGPU_IR_MIN, op2(w, FGPU_IR_MAX, t, zero), imm(w, 1.0f));
            return op2(w, FGPU_IR_MUL, op2(w, FGPU_IR_MUL, t, t),
                       op3(w, FGPU_IR_FMA, 3, t, imm(w, -2.0f), imm(w, 3.0f)));
        }
        case FGPU_SOP_SLT:   return op2(w, FGPU_IR_SLT, a, b);
        case FGPU_SOP_SLE:   return op2(w, FGPU_IR_SLE, a, b);
        case FGPU_SOP_SEQ:   return op2(w, FGPU_IR_SEQ, a, b);
        case FGPU_SOP_SNE:   return op2(w, FGPU_IR_SNE, a, b);
        case FGPU_SOP_SEL:   return op3(w, FGPU_IR_SEL, 3, a, b, c);
        case FGPU_SOP_MOD: {
            src_t f = op1(w, FGPU_IR_FLOOR, op2(w, FGPU_IR_MUL, a, op1(w, FGPU_IR_RCP, b)));
            return op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SNE, b, zero),
                       op3(w, FGPU_IR_FMA, 3, neg(b), f, a), zero);
        }
        case FGPU_SOP_SIGN:
            return op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, zero, a), imm(w, 1.0f),
                       op3(w, FGPU_IR_SEL, 3, op2(w, FGPU_IR_SLT, a, zero), imm(w, -1.0f), zero));
        case FGPU_SOP_ATAN2: return atan2_(w, a, b);
        default:             return zero;
    }
}

static uint32_t sources_of(uint32_t op) {
    switch (op) {
        case FGPU_SOP_NOP: case FGPU_SOP_JMP: return 0;
        case FGPU_SOP_MOV: case FGPU_SOP_FLOOR: case FGPU_SOP_FRACT: case FGPU_SOP_SQRT:
        case FGPU_SOP_RSQ: case FGPU_SOP_EXP2: case FGPU_SOP_LOG2: case FGPU_SOP_SIN:
        case FGPU_SOP_COS: case FGPU_SOP_SIGN: case FGPU_SOP_TEX: case FGPU_SOP_KILL:
        case FGPU_SOP_JZ:
            return 1;
        case FGPU_SOP_MAD: case FGPU_SOP_MIX: case FGPU_SOP_CLAMP: case FGPU_SOP_SSTEP:
        case FGPU_SOP_SEL:
            return 3;
        default:
            return 2;
    }
}

static void lower_inst(lw_t* w, const uint32_t* in) {
    uint32_t op = in[0], d = in[1];
    uint32_t mask = FGPU_DST_MASK(d);
    src_t res[4];
    switch (op) {
        case FGPU_SOP_NOP:
            return;
        case FGPU_SOP_JMP:
        case FGPU_SOP_JZ: {
            src_t take = w->active_known ? imm(w, 1.0f) : w->active;
            if (op == FGPU_SOP_JZ)
                take = op2(w, FGPU_IR_MUL, take, op2(w, FGPU_IR_SEQ, read(w, in[2], 0), imm(w, 0.0f)));
            w->wait = op3(w, FGPU_IR_SEL, 3, take, imm(w, (float)in[1]), w->wait);
            return;
        }
        case FGPU_SOP_KILL: {
            src_t k = op2(w, FGPU_IR_SNE, read(w, in[2], 0), imm(w, 0.0f));
            if (!w->active_known) k = op2(w, FGPU_IR_MUL, k, w->active);
            w->killed = is_imm(w, w->killed) && imm_of(w, w->killed) == 0.0f
                            ? k : op2(w, FGPU_IR_MAX, w->killed, k);
            return;
        }
        case FGPU_SOP_TEX: {
            fgpu_ir_inst_t t = { .op = FGPU_IR_TEX, .nsrc = 2, .aux = in[3] };
            t.src[0] = read(w, in[2], 0);
            t.src[1] = read(w, in[2], 1);
            res[0] = append(w, &t);
            for (uint32_t c = 1; c < 4; c++) {
                fgpu_ir_inst_t tc = { .op = FGPU_IR_TEXC, .nsrc = 1, .aux = c };
                tc.src[0] = res[0];
                res[c] = append(w, &tc);
            }
            break;
        }
        case FGPU_SOP_DP2:
        case FGPU_SOP_DP3:
        case FGPU_SOP_DP4: {
            uint32_t n = op == FGPU_SOP_DP2 ? 2 : (op == FGPU_SOP_DP3 ? 3 : 4);
            src_t s = op2(w, FGPU_IR_MUL, read(w, in[2], 0), read(w, in[3], 0));
            for (uint32_t k = 1; k < n; k++)
                s = op3(w, FGPU_IR_FMA, 3, read(w, in[2], k), read(w, in[3], k), s);
            for (uint32_t c = 0; c < 4; c++) res[c] = s;
            break;
        }
        case FGPU_SOP_CROSS: {
            src_t a[3], b[3];
            for (uint32_t k = 0; k < 3; k++) { a[k] = read(w, in[2], k); b[k] = read(w, in[3], k); }
            for (uint32_t c = 0; c < 3; c++) {
                uint32_t i = (c + 1) % 3, j = (c + 2) % 3;
                res[c] = op3(w, FGPU_IR_FMA, 3, a[i], b[j], neg(op2(w, FGPU_IR_MUL, a[j], b[i])));
            }
            res[3] = imm(w, 0.0f);
            break;
        }
        default: {
            uint32_t used = sources_of(op);
            for (uint32_t c = 0; c < 4; c++) {
                if (!(mask & (1u << c))) continue;
                src_t a = read(w, in[2], c);
                src_t b = used >= 2 ? read(w, in[3], c) : a;
                src_t cc = used >= 3 ? read(w, in[4], c) : a;
                res[c] = lower_comp(w, op, a, b, cc);
            }
            break;
        }
    }
    // All sources were read above, before any component is written.
    for (uint32_t c = 0; c < 4; c++)
        if (mask & (1u << c)) write(w, d, c, res[c]);
}

/* ---------------------------------------------------------------- DCE */

static bool has_effect(uint32_t op) { return op == FGPU_IR_OUTPUT || op == FGPU_IR_DISCARD; }

// Drops instructions no output depends on and renumbers the rest.
static void eliminate(fgpu_ir_t* ir) {
    uint32_t* map = ir->hash;           // reused: new index + 1, 0 = dead
    for (uint32_t i = 0; i < ir->count; i++) map[i] = has_effect(ir->code[i].op) ? 1 : 0;
    for (uint32_t i = ir->count; i-- > 0;) {
        if (!map[i]) continue;
        const fgpu_ir_inst_t* in = &ir->code[i];
        for (uint32_t k = 0; k < in->nsrc; k++) map[in->src[k].value] = 1;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < ir->count; i++) {
        if (!map[i]) continue;
        fgpu_ir_inst_t in = ir->code[i];
        for (uint32_t k = 0; k < in.nsrc; k++) in.src[k].value = map[in.src[k].value] - 1;
        ir->code[n] = in;
        map[i] = ++n;
    }
    ir->count = n;
}

/* ------------------------------------------------------------- lowering */

bool fgpu_ir_lower(fgpu_ir_t* ir, const fgpu_program_t* p, bool fragment, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    if (!ir || !ir->code || !ir->hash || ir->capacity < 16 || !p) { *why = "no storage"; return false; }
    ir->count = 0;
    ir->fragment = fragment;
    ir->reg_count = 0;
    ir->selects_for_jumps = 0;
    for (uint32_t i = 0; i < FGPU_IR_HASH_WORDS(ir->capacity); i++) ir->hash[i] = 0;

    lw_t w;
    __builtin_memset(&w, 0, sizeof(w));
    w.ir = ir;
    w.p = p;
    src_t zero = imm(&w, 0.0f), one = imm(&w, 1.0f);
    for (uint32_t t = 0; t < FGPU_SHADER_MAX_TEMPS; t++)
        for (uint32_t c = 0; c < 4; c++) w.temp[t][c] = zero;
    uint32_t outs = fragment ? 1u : 1u + p->h.varying_count;
    for (uint32_t o = 0; o < outs; o++)
        for (uint32_t c = 0; c < 4; c++) w.out[o][c] = (o == 0 && c == 3) ? one : zero;
    w.wait = zero;
    w.killed = zero;

    const uint32_t* code = fragment ? p->fs : p->vs;
    uint32_t count = fragment ? p->h.fs_count : p->h.vs_count;
    uint32_t reach = 0;     // furthest jump target seen so far
    for (uint32_t pc = 0; pc < count && !w.overflow; pc++) {
        // Threads can only be waiting here if a jump before pc goes past it.
        w.active_known = reach <= pc;
        if (!w.active_known) w.active = op2(&w, FGPU_IR_SLE, w.wait, imm(&w, (float)pc));
        const uint32_t* in = code + (size_t)pc * FGPU_SHADER_INSTRUCTION_WORDS;
        lower_inst(&w, in);
        if ((in[0] == FGPU_SOP_JMP || in[0] == FGPU_SOP_JZ) && in[1] > reach) reach = in[1];
    }
    for (uint32_t o = 0; o < outs && !w.overflow; o++) {
        for (uint32_t c = 0; c < 4; c++) {
            fgpu_ir_inst_t in = { .op = FGPU_IR_OUTPUT, .nsrc = 1, .aux = FGPU_IR_SLOT(o, c) };
            in.src[0] = w.out[o][c];
            append(&w, &in);
        }
    }
    if (fragment && !(is_imm(&w, w.killed) && imm_of(&w, w.killed) == 0.0f) && !w.overflow) {
        fgpu_ir_inst_t in = { .op = FGPU_IR_DISCARD, .nsrc = 1 };
        in.src[0] = w.killed;
        append(&w, &in);
    }
    if (w.overflow) { *why = "shader too large for the IR"; return false; }
    eliminate(ir);
    return true;
}

/* -------------------------------------------------- register allocation */

#define MAX_REGS 1024u

static bool needs_reg(uint32_t op) {
    return op != FGPU_IR_IMM && op != FGPU_IR_OUTPUT && op != FGPU_IR_DISCARD;
}

void fgpu_ir_allocate(fgpu_ir_t* ir) {
    uint32_t* last = ir->hash;          // last use of each value
    for (uint32_t i = 0; i < ir->count; i++) last[i] = i;
    for (uint32_t i = 0; i < ir->count; i++) {
        const fgpu_ir_inst_t* in = &ir->code[i];
        for (uint32_t k = 0; k < in->nsrc; k++) last[in->src[k].value] = i;
    }
    uint32_t used[MAX_REGS / 32];
    __builtin_memset(used, 0, sizeof(used));
    uint32_t high = 0;
    for (uint32_t i = 0; i < ir->count; i++) {
        fgpu_ir_inst_t* in = &ir->code[i];
        // Sources whose last use is here are free for the result: GPU
        // instructions read their operands before writing.
        for (uint32_t k = 0; k < in->nsrc; k++) {
            uint32_t v = in->src[k].value;
            uint16_t r = ir->code[v].reg;
            if (last[v] == i && r != FGPU_IR_NO_REG) used[r / 32] &= ~(1u << (r % 32));
        }
        in->reg = FGPU_IR_NO_REG;
        if (!needs_reg(in->op)) continue;
        for (uint32_t r = 0; r < MAX_REGS; r++) {
            if (used[r / 32] & (1u << (r % 32))) continue;
            in->reg = (uint16_t)r;
            if (last[i] != i) used[r / 32] |= 1u << (r % 32);
            if (r + 1 > high) high = r + 1;
            break;
        }
    }
    ir->reg_count = high;
}

/* ------------------------------------------------------------- listing */

typedef struct {
    char s[128];
    uint32_t n;
} buf_t;

static void put(buf_t* b, const char* t) {
    while (*t && b->n + 1 < sizeof(b->s)) b->s[b->n++] = *t++;
    b->s[b->n] = 0;
}

static void put_uint(buf_t* b, uint32_t v, uint32_t base, uint32_t min_digits) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v || n < (int)min_digits);
    char out[13];
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    put(b, out);
}

static void put_float(buf_t* b, float v) {
    if (v != v || fabs_(v) >= 1e9f) {           // NaN, inf or huge: show the bits
        put(b, "0x");
        put_uint(b, f2u(v), 16, 8);
        return;
    }
    if (v < 0.0f || (v == 0.0f && (f2u(v) >> 31))) { put(b, "-"); v = -v; }
    uint32_t whole = (uint32_t)v;
    uint32_t frac = (uint32_t)((v - (float)whole) * 1000000.0f + 0.5f);
    if (frac >= 1000000u) { whole++; frac -= 1000000u; }
    put_uint(b, whole, 10, 1);
    if (frac) {
        uint32_t digits = 6;
        while (frac % 10 == 0) { frac /= 10; digits--; }
        put(b, ".");
        put_uint(b, frac, 10, digits);
    }
}

static const char* const k_comp = "xyzw";

static void put_slot(buf_t* b, const char* file, uint32_t slot) {
    put(b, file);
    put_uint(b, slot / 4, 10, 1);
    char c[3] = { '.', k_comp[slot % 4], 0 };
    put(b, c);
}

static void put_src(buf_t* b, const fgpu_ir_t* ir, fgpu_ir_src_t s) {
    const fgpu_ir_inst_t* v = &ir->code[s.value];
    if (s.mods & FGPU_IR_NEG) put(b, "-");
    if (s.mods & FGPU_IR_ABS) put(b, "|");
    if (v->op == FGPU_IR_IMM) {
        put_float(b, v->imm);
    } else if (v->reg != FGPU_IR_NO_REG) {
        put(b, "R");
        put_uint(b, v->reg, 10, 1);
    } else {
        put(b, "%");
        put_uint(b, s.value, 10, 1);
    }
    if (s.mods & FGPU_IR_ABS) put(b, "|");
}

static const char* const k_names[FGPU_IR_OP_COUNT] = {
    [FGPU_IR_IMM] = "MOV32I",       [FGPU_IR_INPUT] = "",           [FGPU_IR_SPECIAL] = "",
    [FGPU_IR_UNIFORM] = "MOV",      [FGPU_IR_ADD] = "FADD",         [FGPU_IR_MUL] = "FMUL",
    [FGPU_IR_FMA] = "FFMA",         [FGPU_IR_MIN] = "FMNMX.MIN",    [FGPU_IR_MAX] = "FMNMX.MAX",
    [FGPU_IR_FLOOR] = "FRND.FLOOR", [FGPU_IR_RCP] = "MUFU.RCP",     [FGPU_IR_RSQ] = "MUFU.RSQ",
    [FGPU_IR_SQRT] = "MUFU.SQRT",   [FGPU_IR_EX2] = "MUFU.EX2",     [FGPU_IR_LG2] = "MUFU.LG2",
    [FGPU_IR_SIN] = "MUFU.SIN",     [FGPU_IR_COS] = "MUFU.COS",     [FGPU_IR_SLT] = "FSET.BF.LT",
    [FGPU_IR_SLE] = "FSET.BF.LE",   [FGPU_IR_SEQ] = "FSET.BF.EQ",   [FGPU_IR_SNE] = "FSET.BF.NEU",
    [FGPU_IR_SEL] = "FSEL",         [FGPU_IR_TEX] = "TEX",          [FGPU_IR_TEXC] = "",
    [FGPU_IR_OUTPUT] = "",          [FGPU_IR_DISCARD] = "KIL",
};

void fgpu_ir_print(const fgpu_ir_t* ir, void (*line)(void* ctx, const char* text), void* ctx) {
    buf_t b;
    b.n = 0; b.s[0] = 0;
    put(&b, ir->fragment ? "// fragment stage: " : "// vertex stage: ");
    put_uint(&b, ir->count, 10, 1);
    put(&b, " ops, ");
    put_uint(&b, ir->reg_count, 10, 1);
    put(&b, " registers");
    line(ctx, b.s);
    for (uint32_t i = 0; i < ir->count; i++) {
        const fgpu_ir_inst_t* in = &ir->code[i];
        if (in->op == FGPU_IR_IMM || in->op == FGPU_IR_TEXC) continue;     // operands / part of TEX
        b.n = 0; b.s[0] = 0;
        put(&b, "  ");
        switch (in->op) {
            case FGPU_IR_INPUT:
                put(&b, ir->fragment ? "IPA " : "ALD ");
                put_src(&b, ir, (fgpu_ir_src_t){ i, 0 });
                put(&b, ", ");
                put_slot(&b, ir->fragment ? "v" : "a", in->aux);
                break;
            case FGPU_IR_SPECIAL:
                put(&b, "S2R ");
                put_src(&b, ir, (fgpu_ir_src_t){ i, 0 });
                put(&b, in->aux / 4 == 0 ? ", SR_FRAGCOORD." : ", SR_FRONTFACE.");
                { char c[2] = { k_comp[in->aux % 4], 0 }; put(&b, c); }
                break;
            case FGPU_IR_UNIFORM:
                put(&b, "MOV ");
                put_src(&b, ir, (fgpu_ir_src_t){ i, 0 });
                put(&b, ", c[0x0][0x");
                put_uint(&b, in->aux * 4, 16, 1);
                put(&b, "]");
                break;
            case FGPU_IR_OUTPUT:
                put(&b, ir->fragment ? "OUT o" : "AST o");
                put_uint(&b, in->aux / 4, 10, 1);
                { char c[3] = { '.', k_comp[in->aux % 4], 0 }; put(&b, c); }
                put(&b, ", ");
                put_src(&b, ir, in->src[0]);
                break;
            case FGPU_IR_DISCARD:
                put(&b, "KIL if ");
                put_src(&b, ir, in->src[0]);
                put(&b, " != 0");
                break;
            case FGPU_IR_TEX:
                put(&b, "TEX.2D ");
                put_src(&b, ir, (fgpu_ir_src_t){ i, 0 });
                put(&b, "+, ");
                put_src(&b, ir, in->src[0]);
                put(&b, ", ");
                put_src(&b, ir, in->src[1]);
                put(&b, ", tex");
                put_uint(&b, in->aux, 10, 1);
                break;
            default:
                put(&b, k_names[in->op]);
                put(&b, " ");
                put_src(&b, ir, (fgpu_ir_src_t){ i, 0 });
                for (uint32_t k = 0; k < in->nsrc; k++) {
                    put(&b, ", ");
                    put_src(&b, ir, in->src[k]);
                }
                break;
        }
        line(ctx, b.s);
    }
}
