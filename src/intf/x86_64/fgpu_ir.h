#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_shader.h"

/*
 * FGPU shader IR: the first half of a compiler from FGPU bytecode to GPU
 * machine code (SASS on NVIDIA cards).
 *
 * GPU shader cores are scalar per thread: a warp runs 32 threads in step,
 * and each thread works on 32-bit registers, not vec4s. So the vec4
 * bytecode is lowered here into scalar operations that map one to one onto
 * hardware instructions (comments show the Turing SASS each one becomes).
 *
 *  - Scalarizing: every component of every vec4 instruction becomes its
 *    own operation. DP3 becomes MUL + FMA + FMA, MIX becomes ADD + FMA,
 *    and so on; atan2, smoothstep, mod and sign become short sequences.
 *  - The edge cases FGPU defines (x / 0 = 0, sqrt and rsq of x <= 0 = 0,
 *    log2(x <= 0) = -126, pow(x <= 0, y)) are made explicit with selects,
 *    so GPU and CPU agree.
 *  - Control flow is if-converted: since FGPU jumps only go forward, each
 *    thread keeps the instruction it resumes at ("wait"), exactly as the
 *    CPU interpreter does per lane, and writes inside a jumped-over range
 *    become selects. The result is straight-line code with no branches.
 *    A later pass can turn long skipped ranges back into real branches
 *    (BRA with BSSY/BSYNC reconvergence on Volta and later).
 *  - Values are in SSA form (each operation defines one value), so
 *    constant folding, common subexpression elimination and dead code
 *    elimination are simple passes, and registers are allocated with a
 *    linear scan.
 *
 * No kernel or libc dependencies, so it also builds on the host
 * (tests/host/fgpu_ir_tests.c checks it against the CPU interpreter).
 */

typedef enum {
    FGPU_IR_IMM = 0,    // imm                              (MOV32I / immediate operand)
    FGPU_IR_INPUT,      // attribute or varying aux        (ALD in VS, IPA in FS)
    FGPU_IR_SPECIAL,    // fragment special aux            (S2R / IPA)
    FGPU_IR_UNIFORM,    // uniform aux                     (c[0x0][aux * 4] operand)
    FGPU_IR_ADD,        // a + b                           FADD
    FGPU_IR_MUL,        // a * b                           FMUL
    FGPU_IR_FMA,        // a * b + c                       FFMA
    FGPU_IR_MIN,        // a < b ? a : b                   FMNMX
    FGPU_IR_MAX,        // a > b ? a : b                   FMNMX
    FGPU_IR_FLOOR,      //                                 FRND.FLOOR
    FGPU_IR_RCP,        // 1 / a                           MUFU.RCP
    FGPU_IR_RSQ,        // 1 / sqrt(a)                     MUFU.RSQ
    FGPU_IR_SQRT,       //                                 MUFU.SQRT
    FGPU_IR_EX2,        // 2 ^ a                           MUFU.EX2
    FGPU_IR_LG2,        // log2(a)                         MUFU.LG2
    FGPU_IR_SIN,        // radians                         FMUL by 1/2pi + MUFU.SIN
    FGPU_IR_COS,        // radians                         FMUL by 1/2pi + MUFU.COS
    FGPU_IR_SLT,        // a <  b ? 1.0 : 0.0              FSET.BF.LT
    FGPU_IR_SLE,        // a <= b ? 1.0 : 0.0              FSET.BF.LE
    FGPU_IR_SEQ,        // a == b ? 1.0 : 0.0              FSET.BF.EQ
    FGPU_IR_SNE,        // a != b ? 1.0 : 0.0              FSET.BF.NEU
    FGPU_IR_SEL,        // a != 0 ? b : c                  FSETP + FSEL
    FGPU_IR_TEX,        // sample unit aux at (a, b): red   TEX (writes up to 4 registers)
    FGPU_IR_TEXC,       // component aux (1-3) of the TEX in a: no code of its own
    FGPU_IR_OUTPUT,     // output aux = a (no value)       AST in VS, output registers in FS
    FGPU_IR_DISCARD,    // discard the fragment if a != 0  KIL
    FGPU_IR_OP_COUNT
} fgpu_ir_op_t;

// aux for INPUT / SPECIAL / UNIFORM / OUTPUT: register * 4 + component.
#define FGPU_IR_SLOT(reg, comp) ((uint32_t)(reg) * 4u + (uint32_t)(comp))

#define FGPU_IR_NEG 1u      // source modifiers, applied abs first, then neg
#define FGPU_IR_ABS 2u

typedef struct {
    uint32_t value;     // index of the instruction that defines it
    uint32_t mods;      // FGPU_IR_NEG | FGPU_IR_ABS
} fgpu_ir_src_t;

typedef struct {
    uint8_t op;         // fgpu_ir_op_t
    uint8_t nsrc;       // sources used
    uint16_t reg;       // physical register after allocation (FGPU_IR_NO_REG if none)
    uint32_t aux;
    float imm;
    fgpu_ir_src_t src[3];
} fgpu_ir_inst_t;

#define FGPU_IR_NO_REG 0xFFFFu

typedef struct {
    fgpu_ir_inst_t* code;       // caller's storage
    uint32_t capacity;          // instructions code[] holds
    uint32_t count;
    bool fragment;
    // After fgpu_ir_allocate():
    uint32_t reg_count;         // 32-bit registers needed per thread
    // Statistics from fgpu_ir_lower(), for listings and tests.
    uint32_t selects_for_jumps; // SELs added by if-conversion
    // Lowering scratch: CSE hash table, 2 * capacity entries.
    uint32_t* hash;
} fgpu_ir_t;

// Words of scratch fgpu_ir_t.hash needs for `capacity` instructions.
#define FGPU_IR_HASH_WORDS(capacity) (2u * (capacity))

// Lowers one stage of a checked program (fgpu_program_check). code and
// hash must be set; returns false if the stage needs more than capacity
// instructions (`why` then says so).
bool fgpu_ir_lower(fgpu_ir_t* ir, const fgpu_program_t* p, bool fragment, const char** why);

// Assigns registers (linear scan over the straight-line code) and sets
// reg_count. Immediates get no register: they become instruction operands.
void fgpu_ir_allocate(fgpu_ir_t* ir);

// Writes a listing in SASS-style mnemonics, one line per call of `line`.
// It shows what the hardware would run; it is not encoded machine code.
void fgpu_ir_print(const fgpu_ir_t* ir, void (*line)(void* ctx, const char* text), void* ctx);
