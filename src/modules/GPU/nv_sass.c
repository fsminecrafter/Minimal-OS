#include "x86_64/nv_sass.h"
#include <stddef.h>
#include "x86_64/fgpu_abi.h"

/*
 * FGPU shader IR -> Turing machine code (nv_sass.h has the overview).
 * Opcodes and field positions are those of Mesa's NAK (sm70_encode.rs).
 */

/* ------------------------------------------------------------ opcodes */

#define OP_MOV      0x002u
#define OP_SEL      0x007u
#define OP_FMNMX    0x009u
#define OP_FSETP    0x00Bu
#define OP_FMUL     0x020u
#define OP_FADD     0x021u
#define OP_FFMA     0x023u
#define OP_FRND     0x107u
#define OP_MUFU     0x108u
#define OP_ALD      0x321u      // whole 12-bit opcodes from here on
#define OP_AST      0x322u
#define OP_IPA      0x326u
#define OP_TEX      0xB60u      // texture handle from a constant buffer
#define OP_EXIT     0x94Du
#define OP_KILL     0x95Bu

#define FORM_REG    1u          // source 1 is a register
#define FORM_IMM    4u          // source 1 is a 32-bit immediate
#define FORM_CBUF   5u          // source 1 is c[buffer][offset]

#define REG_ZERO    255u
#define PRED_TRUE   7u

#define MUFU_COS 0u
#define MUFU_SIN 1u
#define MUFU_EX2 2u
#define MUFU_LG2 3u
#define MUFU_RCP 4u
#define MUFU_RSQ 5u
#define MUFU_SQRT 8u

#define CMP_LT  0x1u            // ordered
#define CMP_EQ  0x2u
#define CMP_LE  0x3u
#define CMP_NEU 0xDu            // not equal, or unordered

#define SCRATCH_REGS 5u         // 0..2 operands, 3..4 texture coordinates

/* ------------------------------------------------------------- fields */

static void set(uint32_t w[4], uint32_t lo, uint32_t hi, uint32_t v) {
    for (uint32_t b = lo; b < hi; b++, v >>= 1) {
        if (v & 1u) w[b / 32u] |= 1u << (b % 32u);
        else w[b / 32u] &= ~(1u << (b % 32u));
    }
}

uint32_t nv_sass_field(const uint32_t inst[4], uint32_t lo, uint32_t hi) {
    uint32_t v = 0;
    for (uint32_t b = hi; b-- > lo;) v = (v << 1) | ((inst[b / 32u] >> (b % 32u)) & 1u);
    return v;
}

static uint32_t f2u(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

/* ------------------------------------------------------------ emitter */

typedef struct {
    nv_sass_t* out;
    const fgpu_ir_inst_t* code;
    bool fragment;
    bool first;                 // nothing emitted yet
    bool overflow;
    uint32_t scratch;           // first scratch register
    uint32_t tex_base;          // first texture result register
    uint32_t out_base;          // fragment color registers
} enc_t;

// An operand ready to encode: a register with modifiers, or an immediate.
typedef struct {
    bool imm;
    uint32_t reg, bits;
    bool neg, abs;
} opnd_t;

// Finishes an instruction: predicate and scheduling, then stores it.
// `pred` is a predicate register number, PRED_TRUE for always.
static void emit(enc_t* e, uint32_t w[4], uint32_t pred, bool writes, bool reads) {
    set(w, 12, 15, pred);
    set(w, 105, 109, 0);                        // no extra stall
    set(w, 110, 113, writes ? 0u : 7u);         // barrier set when the result is written
    set(w, 113, 116, reads ? 1u : 7u);          // barrier set when the operands were read
    set(w, 116, 122, e->first ? 0u : 3u);       // wait for the previous instruction's barriers
    e->first = false;
    nv_sass_t* o = e->out;
    if (o->count + 4u > o->capacity) { e->overflow = true; return; }
    for (int i = 0; i < 4; i++) o->code[o->count++] = w[i];
}

// ALU instruction: opcode, destination, sources 0..2. s1 may be an
// immediate; s0 and s2 must be registers (NULL = unused).
static void alu(uint32_t w[4], uint32_t opcode, uint32_t dst, const opnd_t* s0, const opnd_t* s1,
                const opnd_t* s2) {
    uint32_t form = FORM_REG;
    set(w, 16, 24, dst);
    if (s0) {
        set(w, 24, 32, s0->reg);
        set(w, 73, 74, s0->abs);
        set(w, 72, 73, s0->neg);
    }
    if (s1 && s1->imm) {
        set(w, 32, 64, s1->bits);
        form = FORM_IMM;
    } else if (s1) {
        set(w, 32, 40, s1->reg);
        set(w, 62, 63, s1->abs);
        set(w, 63, 64, s1->neg);
    }
    if (s2) {
        set(w, 64, 72, s2->reg);
        set(w, 74, 75, s2->abs);
        set(w, 75, 76, s2->neg);
    }
    set(w, 0, 9, opcode);
    set(w, 9, 12, form);
}

static opnd_t reg_opnd(uint32_t reg) { return (opnd_t){ false, reg, 0, false, false }; }
static opnd_t imm_opnd(uint32_t bits) { return (opnd_t){ true, 0, bits, false, false }; }

static void mov(enc_t* e, uint32_t dst, const opnd_t* src, uint32_t pred) {
    uint32_t w[4] = { 0 };
    alu(w, OP_MOV, dst, NULL, src, NULL);
    set(w, 72, 76, 0xF);                        // all four lanes of a pixel quad
    emit(e, w, pred, true, true);
}

static void mov_cbuf(enc_t* e, uint32_t dst, uint32_t offset) {
    uint32_t w[4] = { 0 };
    set(w, 16, 24, dst);
    set(w, 38, 54, offset);                     // byte offset
    set(w, 54, 59, 0);                          // constant buffer 0
    set(w, 0, 9, OP_MOV);
    set(w, 9, 12, FORM_CBUF);
    set(w, 72, 76, 0xF);
    emit(e, w, PRED_TRUE, true, true);
}

// P0 = a <cmp> b.
static void fsetp(enc_t* e, const opnd_t* a, const opnd_t* b, uint32_t cmp) {
    uint32_t w[4] = { 0 };
    alu(w, OP_FSETP, 0, a, b, NULL);
    set(w, 16, 24, 0);
    set(w, 74, 76, 0);                          // combine with the accumulator: AND
    set(w, 76, 80, cmp);
    set(w, 81, 84, 0);                          // result: P0
    set(w, 84, 87, PRED_TRUE);                  // no second result
    set(w, 87, 90, PRED_TRUE);                  // accumulator: true
    emit(e, w, PRED_TRUE, true, true);
}

// The register holding source k of `in`, loading immediates into scratch
// register k. With `plain`, modifiers are applied into the scratch
// register too (for instructions that cannot negate or take |x|).
static opnd_t source(enc_t* e, const fgpu_ir_inst_t* in, uint32_t k, bool plain) {
    const fgpu_ir_src_t* s = &in->src[k];
    const fgpu_ir_inst_t* def = &e->code[s->value];
    uint32_t tmp = e->scratch + k;
    if (def->op == FGPU_IR_IMM) {
        float v = def->imm;
        if (s->mods & FGPU_IR_ABS) v = v < 0.0f ? -v : v;
        uint32_t bits = f2u(v);
        if (s->mods & FGPU_IR_ABS) bits &= 0x7FFFFFFFu;
        if (s->mods & FGPU_IR_NEG) bits ^= 0x80000000u;
        opnd_t i = imm_opnd(bits);
        mov(e, tmp, &i, PRED_TRUE);
        return reg_opnd(tmp);
    }
    opnd_t o = { false, def->reg, 0, (s->mods & FGPU_IR_NEG) != 0, (s->mods & FGPU_IR_ABS) != 0 };
    if (plain && (o.neg || o.abs)) {
        // tmp = (modified x) + -0.0, which is exact.
        uint32_t w[4] = { 0 };
        opnd_t zero = imm_opnd(0x80000000u);
        alu(w, OP_FADD, tmp, &o, &zero, NULL);
        emit(e, w, PRED_TRUE, true, true);
        return reg_opnd(tmp);
    }
    return o;
}

static void binary(enc_t* e, const fgpu_ir_inst_t* in, uint32_t opcode) {
    opnd_t a = source(e, in, 0, false), b = source(e, in, 1, false);
    uint32_t w[4] = { 0 };
    if (opcode == OP_FMUL) {
        opnd_t z = reg_opnd(REG_ZERO);
        alu(w, opcode, in->reg, &a, &b, &z);
        set(w, 84, 87, 4);
    } else {
        alu(w, opcode, in->reg, &a, &b, NULL);
    }
    if (opcode == OP_FMNMX) {
        set(w, 87, 90, PRED_TRUE);
        set(w, 90, 91, in->op == FGPU_IR_MAX);  // not true = max
    }
    emit(e, w, PRED_TRUE, true, true);
}

static void mufu(enc_t* e, uint32_t dst, const opnd_t* src, uint32_t fn) {
    uint32_t w[4] = { 0 };
    alu(w, OP_MUFU, dst, NULL, src, NULL);
    set(w, 74, 80, fn);
    emit(e, w, PRED_TRUE, true, true);
}

// dst = P0 ? 1.0 : 0.0
static void bool_from_p0(enc_t* e, uint32_t dst) {
    opnd_t zero = imm_opnd(0), one = imm_opnd(0x3F800000u);
    mov(e, dst, &zero, PRED_TRUE);
    mov(e, dst, &one, 0);
}

static uint32_t attr_address(uint32_t slot) { return NV_SASS_ATTR_GENERIC + slot * 4u; }

static void store_attr(enc_t* e, uint32_t address, uint32_t reg) {
    uint32_t w[4] = { 0 };
    set(w, 0, 12, OP_AST);
    set(w, 24, 32, REG_ZERO);                   // no offset register
    set(w, 32, 40, reg);
    set(w, 64, 72, REG_ZERO);                   // not per vertex of a primitive
    set(w, 40, 50, address);
    set(w, 74, 76, 0);                          // one component
    emit(e, w, PRED_TRUE, false, true);
}

static void sph_bit(uint32_t* sph, uint32_t bit) { sph[bit / 32u] |= 1u << (bit % 32u); }

static void sph_field(uint32_t* sph, uint32_t lo, uint32_t hi, uint32_t v) {
    for (uint32_t b = lo; b < hi; b++, v >>= 1)
        if (v & 1u) sph_bit(sph, b);
}

/* -------------------------------------------------------------- stages */

bool nv_sass_encode(const fgpu_ir_inst_t* code, uint32_t count, uint32_t ir_regs, bool fragment,
                    uint32_t varyings, nv_sass_t* out, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    uint32_t* storage = out->code;
    uint32_t capacity = out->capacity;
    __builtin_memset(out, 0, sizeof(*out));
    out->code = storage;
    out->capacity = capacity;
    if (varyings > FGPU_SHADER_MAX_VARYINGS) { *why = "too many varyings"; return false; }

    uint32_t tex_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (code[i].op == FGPU_IR_TEX) tex_count++;
        if (code[i].op == FGPU_IR_SPECIAL && code[i].aux >= 4u) {
            *why = "front-facing is not encoded yet";
            return false;
        }
    }
    enc_t e = { .out = out, .code = code, .fragment = fragment, .first = true };
    // The fragment color ends up in R0..R3 whatever the IR used them for.
    uint32_t base = fragment && ir_regs < 4u ? 4u : ir_regs;
    e.scratch = base;
    e.tex_base = e.scratch + SCRATCH_REGS;
    e.out_base = e.tex_base + 4u * tex_count;
    out->regs = e.out_base + (fragment ? 4u : 0u);
    if (out->regs > NV_SASS_MAX_REGS) { *why = "too many registers"; return false; }

    uint32_t* sph = out->sph;
    uint32_t written[1 + FGPU_SHADER_MAX_VARYINGS] = { 0 };    // vertex outputs, bit per component
    bool reads_position = false;

    if (fragment) {
        // Color starts as (0, 0, 0, 1), like the CPU shader machine.
        for (uint32_t c = 0; c < 4; c++) {
            opnd_t v = imm_opnd(c == 3 ? 0x3F800000u : 0u);
            mov(&e, e.out_base + c, &v, PRED_TRUE);
        }
    }

    uint32_t tex_index = 0;
    for (uint32_t i = 0; i < count; i++) {
        const fgpu_ir_inst_t* in = &code[i];
        switch (in->op) {
            case FGPU_IR_IMM:
                break;
            case FGPU_IR_INPUT: {
                uint32_t address = attr_address(in->aux);
                uint32_t w[4] = { 0 };
                if (in->aux >= 4u * FGPU_MAX_ATTRIBS) { *why = "input out of range"; return false; }
                out->inputs |= 1u << (in->aux / 4u);
                if (fragment) {
                    set(w, 0, 12, OP_IPA);
                    set(w, 16, 24, in->reg);
                    set(w, 32, 40, REG_ZERO);           // no offset
                    set(w, 64, 72, address >> 2);
                    set(w, 76, 78, 0);                  // at the pixel centre
                    set(w, 78, 80, 0);                  // every pass; perspective comes from the header
                    set(w, 81, 84, PRED_TRUE);
                    sph_field(sph, 192u + in->aux * 2u, 194u + in->aux * 2u, 2u);   // perspective
                } else {
                    set(w, 0, 12, OP_ALD);
                    set(w, 16, 24, in->reg);
                    set(w, 24, 32, REG_ZERO);
                    set(w, 32, 40, REG_ZERO);
                    set(w, 40, 50, address);
                    set(w, 74, 76, 0);
                    sph_bit(sph, 192u + in->aux);
                }
                emit(&e, w, PRED_TRUE, true, true);
                break;
            }
            case FGPU_IR_SPECIAL: {
                // The fragment coordinate: position x, y, z and w.
                uint32_t w[4] = { 0 };
                uint32_t address = NV_SASS_ATTR_POSITION + in->aux * 4u;
                set(w, 0, 12, OP_IPA);
                set(w, 16, 24, in->reg);
                set(w, 32, 40, REG_ZERO);
                set(w, 64, 72, address >> 2);
                set(w, 81, 84, PRED_TRUE);
                emit(&e, w, PRED_TRUE, true, true);
                sph_bit(sph, 160u + address / 4u);
                reads_position = true;
                break;
            }
            case FGPU_IR_UNIFORM:
                if (in->aux * 4u >= NV_SASS_CB_UNIFORM_BYTES) { *why = "uniform out of range"; return false; }
                mov_cbuf(&e, in->reg, in->aux * 4u);
                break;
            case FGPU_IR_ADD: binary(&e, in, OP_FADD); break;
            case FGPU_IR_MUL: binary(&e, in, OP_FMUL); break;
            case FGPU_IR_MIN:
            case FGPU_IR_MAX: binary(&e, in, OP_FMNMX); break;
            case FGPU_IR_FMA: {
                opnd_t a = source(&e, in, 0, false), b = source(&e, in, 1, false);
                opnd_t c = source(&e, in, 2, false);
                uint32_t w[4] = { 0 };
                alu(w, OP_FFMA, in->reg, &a, &b, &c);
                emit(&e, w, PRED_TRUE, true, true);
                break;
            }
            case FGPU_IR_FLOOR: {
                opnd_t a = source(&e, in, 0, false);
                uint32_t w[4] = { 0 };
                alu(w, OP_FRND, in->reg, NULL, &a, NULL);
                set(w, 75, 77, 2);                      // 32-bit result
                set(w, 78, 80, 1);                      // towards minus infinity
                set(w, 84, 86, 2);                      // 32-bit source
                emit(&e, w, PRED_TRUE, true, true);
                break;
            }
            case FGPU_IR_RCP:
            case FGPU_IR_RSQ:
            case FGPU_IR_SQRT:
            case FGPU_IR_EX2:
            case FGPU_IR_LG2: {
                static const uint8_t fn[] = {
                    [FGPU_IR_RCP] = MUFU_RCP, [FGPU_IR_RSQ] = MUFU_RSQ, [FGPU_IR_SQRT] = MUFU_SQRT,
                    [FGPU_IR_EX2] = MUFU_EX2, [FGPU_IR_LG2] = MUFU_LG2,
                };
                opnd_t a = source(&e, in, 0, false);
                mufu(&e, in->reg, &a, fn[in->op]);
                break;
            }
            case FGPU_IR_SIN:
            case FGPU_IR_COS: {
                // The hardware functions take turns, not radians.
                opnd_t a = source(&e, in, 0, false);
                opnd_t k = imm_opnd(f2u(0.15915494309189535f)), z = reg_opnd(REG_ZERO);
                uint32_t tmp = e.scratch + 1u;
                uint32_t w[4] = { 0 };
                alu(w, OP_FMUL, tmp, &a, &k, &z);
                set(w, 84, 87, 4);
                emit(&e, w, PRED_TRUE, true, true);
                opnd_t t = reg_opnd(tmp);
                mufu(&e, in->reg, &t, in->op == FGPU_IR_SIN ? MUFU_SIN : MUFU_COS);
                break;
            }
            case FGPU_IR_SLT:
            case FGPU_IR_SLE:
            case FGPU_IR_SEQ:
            case FGPU_IR_SNE: {
                opnd_t a = source(&e, in, 0, false), b = source(&e, in, 1, false);
                fsetp(&e, &a, &b, in->op == FGPU_IR_SLT ? CMP_LT : in->op == FGPU_IR_SLE ? CMP_LE :
                                  in->op == FGPU_IR_SEQ ? CMP_EQ : CMP_NEU);
                bool_from_p0(&e, in->reg);
                break;
            }
            case FGPU_IR_SEL: {
                // SEL moves bits: modifiers are applied first.
                opnd_t a = source(&e, in, 0, false);
                opnd_t b = source(&e, in, 1, true), c = source(&e, in, 2, true);
                opnd_t zero = imm_opnd(0);
                fsetp(&e, &a, &zero, CMP_NEU);
                uint32_t w[4] = { 0 };
                alu(w, OP_SEL, in->reg, &b, &c, NULL);
                set(w, 87, 90, 0);                      // P0 picks source 0
                emit(&e, w, PRED_TRUE, true, true);
                break;
            }
            case FGPU_IR_TEX: {
                if (in->aux >= FGPU_MAX_TEXTURE_UNITS) { *why = "texture unit out of range"; return false; }
                // The coordinates go in two registers in a row.
                for (uint32_t k = 0; k < 2; k++) {
                    opnd_t s = source(&e, in, k, true);
                    mov(&e, e.scratch + 3u + k, &s, PRED_TRUE);
                }
                uint32_t result = e.tex_base + 4u * tex_index++;
                uint32_t w[4] = { 0 };
                set(w, 0, 12, OP_TEX);
                set(w, 16, 24, result);
                set(w, 24, 32, e.scratch + 3u);
                set(w, 32, 40, REG_ZERO);               // no array index, bias or offset
                set(w, 40, 54, (NV_SASS_CB_TEX_OFFSET + 4u * in->aux) / 4u);
                set(w, 54, 59, 0);                      // handle from constant buffer 0
                set(w, 61, 64, 1);                      // 2D
                set(w, 64, 72, REG_ZERO);               // no second result register
                set(w, 72, 76, 0xF);                    // r, g, b, a
                set(w, 81, 84, PRED_TRUE);
                set(w, 87, 90, 1);                      // level 0 (one level; also valid in the vertex stage)
                emit(&e, w, PRED_TRUE, true, true);
                opnd_t r = reg_opnd(result);
                mov(&e, in->reg, &r, PRED_TRUE);
                out->textures |= 1u << in->aux;
                // Remember where this TEX's results are for its TEXCs.
                break;
            }
            case FGPU_IR_TEXC: {
                // Component aux of the TEX in src[0]: find its result registers.
                uint32_t n = 0;
                for (uint32_t j = 0; j < in->src[0].value; j++)
                    if (code[j].op == FGPU_IR_TEX) n++;
                if (code[in->src[0].value].op != FGPU_IR_TEX || in->aux > 3u) {
                    *why = "bad texture component";
                    return false;
                }
                opnd_t r = reg_opnd(e.tex_base + 4u * n + in->aux);
                mov(&e, in->reg, &r, PRED_TRUE);
                break;
            }
            case FGPU_IR_OUTPUT: {
                opnd_t v = source(&e, in, 0, true);
                uint32_t reg = in->aux / 4u, comp = in->aux % 4u;
                if (fragment) {
                    if (reg != 0) { *why = "fragment output out of range"; return false; }
                    mov(&e, e.out_base + comp, &v, PRED_TRUE);
                } else {
                    if (reg > varyings) { *why = "vertex output out of range"; return false; }
                    store_attr(&e, reg == 0 ? NV_SASS_ATTR_POSITION + comp * 4u
                                            : attr_address((reg - 1u) * 4u + comp), v.reg);
                    written[reg] |= 1u << comp;
                }
                break;
            }
            case FGPU_IR_DISCARD: {
                opnd_t a = source(&e, in, 0, false), zero = imm_opnd(0);
                fsetp(&e, &a, &zero, CMP_NEU);
                uint32_t w[4] = { 0 };
                set(w, 0, 12, OP_KILL);
                set(w, 87, 90, PRED_TRUE);
                emit(&e, w, 0, false, true);
                out->kills = true;
                break;
            }
            default:
                *why = "unknown IR operation";
                return false;
        }
    }

    if (fragment) {
        for (uint32_t c = 0; c < 4; c++) {
            opnd_t v = reg_opnd(e.out_base + c);
            mov(&e, c, &v, PRED_TRUE);
        }
    } else {
        // Whatever the shader left out: position (0, 0, 0, 1), varyings 0.
        for (uint32_t reg = 0; reg <= varyings; reg++) {
            for (uint32_t comp = 0; comp < 4; comp++) {
                if (written[reg] & (1u << comp)) continue;
                opnd_t v = imm_opnd(reg == 0 && comp == 3 ? 0x3F800000u : 0u);
                mov(&e, e.scratch, &v, PRED_TRUE);
                store_attr(&e, reg == 0 ? NV_SASS_ATTR_POSITION + comp * 4u
                                        : attr_address((reg - 1u) * 4u + comp), e.scratch);
            }
        }
        out->outputs = varyings >= 32u ? 0xFFFFFFFFu : (1u << varyings) - 1u;
    }
    {
        uint32_t w[4] = { 0 };
        set(w, 0, 12, OP_EXIT);
        set(w, 87, 90, PRED_TRUE);
        emit(&e, w, PRED_TRUE, false, false);
    }
    if (e.overflow) { *why = "the machine code does not fit"; return false; }

    // The header.
    sph_field(sph, 0, 5, fragment ? 2u : 1u);       // header type: pixel, or vertex/tess/geometry
    sph_field(sph, 5, 10, 4u);                      // version 4 (shader model 7.3 and later)
    sph_field(sph, 10, 14, fragment ? 5u : 1u);     // shader type
    sph_field(sph, 17, 21, 1u);                     // SASS version
    if (fragment) {
        sph_bit(sph, 14);                           // per-target outputs
        if (out->kills) sph_bit(sph, 15);
        sph_field(sph, 576, 580, 0xFu);             // writes target 0's r, g, b, a
        // Every varying counts as read (perspective), also the unused
        // ones: the vertex stage writes them all, and outputs nobody
        // reads raise an exception on the card.
        for (uint32_t c = 0; c < varyings * 4u; c++) {
            sph[(192u + c * 2u) / 32u] &= ~(3u << ((192u + c * 2u) % 32u));
            sph_field(sph, 192u + c * 2u, 194u + c * 2u, 2u);
        }
    } else {
        sph_field(sph, 140, 148, 0xFFu);            // no attributes that must be stored
        sph_field(sph, 400 + 28, 400 + 32, 0xFu);   // writes the position
        for (uint32_t v = 0; v < varyings; v++) sph_field(sph, 432u + v * 4u, 436u + v * 4u, 0xFu);
    }
    (void)reads_position;
    return true;
}
