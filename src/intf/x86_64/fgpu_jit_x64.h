#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/fgpu_shader.h"

/*
 * FGPU shader JIT for x86-64 (SSE4.1 and AVX2).
 *
 * Compiles one stage of a checked FGPU program (fgpu_program_check) into
 * straight-line machine code through the scalar IR (fgpu_ir.c, lowered with
 * FGPU_IR_EXACT), producing bit-identical results to the interpreter in
 * fgpu_shader.c (NaN sign/payload excepted, see docs/gpu.md). The
 * interpreter stays the reference and the fallback: every failure here
 * (no CPU support, a program too large, any emitter problem) leaves the
 * program running on it.
 *
 * Design and safety rules: docs/gpu.md, "CPU JIT". No kernel dependencies
 * beyond the memory hooks in fgpu_jit_x64.c, so it also builds on the host.
 */

typedef enum {
    FGPU_JIT_OFF = 0,       // never compile (the `nojit` boot flag)
    FGPU_JIT_AUTO,          // AVX2 if usable, else SSE4.1, else off
    FGPU_JIT_SSE41,         // force the 4-lane path (tests, benchmarks)
    FGPU_JIT_AVX2,          // force the 8-lane path (tests, benchmarks)
} fgpu_jit_mode_t;

// Compiled code of one stage. Opaque; owned by the program it came from.
typedef struct fgpu_jit_code fgpu_jit_code_t;

// Selects what fgpu_program_jit() compiles from now on (default AUTO).
// Modes the CPU cannot run behave like OFF.
void fgpu_jit_set_mode(fgpu_jit_mode_t mode);
fgpu_jit_mode_t fgpu_jit_get_mode(void);

// What this CPU can run: bit 0 = SSE4.1, bit 1 = AVX2 (usable: the OS
// enabled the state). In the kernel this comes from cpu_simd_features().
#define FGPU_JIT_CPU_SSE41 1u
#define FGPU_JIT_CPU_AVX2  2u
uint32_t fgpu_jit_cpu_support(void);

// Compiles both stages of a checked program and stores the result in
// p->jit (replacing nothing: call once). Returns true if at least one
// stage compiled; stages that failed keep using the interpreter. Safe to
// call with the mode OFF (does nothing). Not safe to call concurrently on
// one program, nor from interrupt context (it allocates).
bool fgpu_program_jit(fgpu_program_t* p);

// Frees what fgpu_program_jit() allocated. Call before the program's
// memory is reused. No other CPU may be running the program.
void fgpu_program_release(fgpu_program_t* p);

// Runs compiled code for a batch. Returns false if there is none for this
// stage (caller runs the interpreter); otherwise *kept is the lanes that
// were not discarded, exactly like fgpu_shader_run_batch.
bool fgpu_jit_run_batch(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                        uint32_t lanes, const float (*uniforms)[4],
                        const fgpu_texture_t* textures, uint32_t* kept);

/* Introspection for tests and the benchmark. */

typedef struct {
    uint32_t path;          // FGPU_JIT_SSE41 or FGPU_JIT_AVX2 (0 = no code)
    uint32_t code_bytes;    // machine code + constants
    uint32_t ir_ops;
    uint32_t spills;        // virtual registers that live in memory
    uint32_t tex_calls;
} fgpu_jit_info_t;

bool fgpu_program_jit_info(const fgpu_program_t* p, bool fragment, fgpu_jit_info_t* out);

/* Test hooks. */

// Compiles one stage with an explicit path and a code buffer limit (bytes
// of machine code; tests use tiny limits to check clean failure). Returns
// NULL on any failure, with `why` set. The result is freed with
// fgpu_jit_free().
fgpu_jit_code_t* fgpu_jit_compile_stage(const fgpu_program_t* p, bool fragment, fgpu_jit_mode_t path,
                                        uint32_t code_limit, const char** why);
void fgpu_jit_free(fgpu_jit_code_t* c);
uint32_t fgpu_jit_run_code(const fgpu_jit_code_t* c, fgpu_shader_batch_t* b, uint32_t lanes,
                           const float (*uniforms)[4], const fgpu_texture_t* textures);

/* Kernel self-check (fgpu_jit_selfcheck.c, 'gpu jit' in the terminal): compiles a few
 * FGSL shaders, runs random batches through the interpreter and the JIT (SSE4.1 and AVX2
 * when usable), also on the other cores through smp_par, and compares bit for bit.
 * `say` receives one line at a time. Prints "FGPU JIT selfcheck: PASS" or "...: FAIL" at
 * the end (also on the serial port). Returns true on PASS and on SKIP (JIT unusable). */
bool fgpu_jit_selfcheck(void (*say)(const char* line));
