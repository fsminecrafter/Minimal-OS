#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_ir.h"

/*
 * The second half of the shader compiler: FGPU shader IR (fgpu_ir.h) to
 * machine code for NVIDIA Turing shader cores ("SASS", shader model 7.5),
 * plus the shader program header (SPH) the 3D engine reads in front of it.
 *
 * Encodings follow Mesa's NAK compiler (src/nouveau/compiler/nak/
 * sm70_encode.rs, sph.rs), which is what the NVK Vulkan driver runs on
 * these cards. No kernel dependencies: tests/host/nv_sass_tests.c builds
 * it on the host and checks the encodings field by field.
 *
 * An instruction is 128 bits (4 words, low word first):
 *   0..11   opcode (ALU ops: 9 bits of opcode, 3 bits of operand form)
 *   12..15  predicate (7 = always) and its "not" bit
 *   16..23  destination register
 *   24..31  source 0, 32..39 source 1 (or 32..63 an immediate), 64..71 source 2
 *   105..125 scheduling: stall count, barriers to set and wait for
 *
 * Scheduling: every instruction sets a barrier when it writes and the
 * next one waits for it (NAK's "serial" mode). That is slower than a
 * real schedule and always correct.
 *
 * Registers, per thread:
 *   0 .. ir_regs-1      the IR's own registers
 *   then 5 scratch      immediates, modified operands, texture coordinates
 *   then 4 per TEX      texture results
 *   then 4              fragment stage: the color, moved to R0..R3 at the end
 *
 * Uniforms are constant buffer 0 (c[0][slot * 4]); the texture handle of
 * unit u is the word at NV_SASS_CB_TEX_OFFSET + 4 * u in the same buffer.
 */

#define NV_SASS_SPH_WORDS       32u         // header, in front of the code
#define NV_SASS_MAX_WORDS       (4u * 24576u)
#define NV_SASS_MAX_REGS        240u

#define NV_SASS_CB_UNIFORM_BYTES 1024u      // FGPU_SHADER_MAX_UNIFORMS vec4s
#define NV_SASS_CB_TEX_OFFSET    1024u      // then one handle per texture unit
#define NV_SASS_CB_BYTES         2048u

// Attribute addresses (a[...]) shared by the stages.
#define NV_SASS_ATTR_POSITION   0x070u
#define NV_SASS_ATTR_GENERIC    0x080u      // + 16 * index + 4 * component

typedef struct {
    uint32_t sph[NV_SASS_SPH_WORDS];
    uint32_t* code;             // caller's storage
    uint32_t capacity;          // words in code
    uint32_t count;             // words used (4 per instruction)
    uint32_t regs;              // registers per thread
    bool kills;                 // fragment stage discards
    uint32_t inputs;            // bit per attribute / varying read
    uint32_t outputs;           // vertex stage: bit per varying written
    uint32_t textures;          // bit per texture unit sampled
} nv_sass_t;

// Encodes one stage. `ir` must be lowered and allocated. `varyings` is
// the program's varying count (the vertex stage writes all of them, so
// the fragment stage never reads something undefined). Returns false and
// sets *why when the stage uses something the encoder does not do yet
// (front-facing), needs too many registers, or does not fit.
bool nv_sass_encode(const fgpu_ir_inst_t* code, uint32_t count, uint32_t ir_regs, bool fragment,
                    uint32_t varyings, nv_sass_t* out, const char** why);

// One instruction's fields, for tests and listings.
uint32_t nv_sass_field(const uint32_t inst[4], uint32_t lo, uint32_t hi);
