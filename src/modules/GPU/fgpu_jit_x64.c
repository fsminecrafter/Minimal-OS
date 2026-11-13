#define _DEFAULT_SOURCE
#include "x86_64/fgpu_jit_x64.h"
#include "x86_64/fgpu_ir.h"
#include <stddef.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cpuid.h>
#else
#include "x86_64/allocator.h"
#include "x86_64/cpu_simd.h"
#include "x86_64/syscall.h"
#include "serial.h"
#endif

/*
 * FGPU shader JIT for x86-64. See docs/gpu.md ("CPU JIT") for the design;
 * the short version:
 *
 *  - Input is the straight-line scalar IR (fgpu_ir.c) lowered with
 *    FGPU_IR_EXACT, so every IR op has exactly the interpreter's meaning.
 *    Each IR value is one SIMD register of W lanes (W = 4 with SSE4.1, 8
 *    with AVX2). The SSE path runs the same code twice, once per half of
 *    the interpreter's 8-lane batch.
 *  - Calling convention (System V): rdi = batch (+16 bytes for the SSE
 *    upper half), rsi = uniforms (64 vec4, never NULL), rdx = textures
 *    (may be NULL); returns eax = lanes that were not discarded (W bits).
 *    The code keeps the pointers in rbx, rbp and r12 (saved), uses rax,
 *    rcx, rdx, rsi, rdi, r8, r9 for helper calls only, and touches no
 *    other general register and no stack beyond its three pushes.
 *  - Memory it may touch, all at offsets known when it is generated: the
 *    batch's input, special and output rows, the batch's temp rows as its
 *    spill area (a compiled shader never needs the interpreter's temps), the
 *    uniforms, and its own constant pool. The only call is the texture
 *    helper, at a fixed address, which clamps every texel access.
 *  - There are no jumps at all: the IR is if-converted.
 *  - Registers: xmm0 = blend mask (SSE blendvps), xmm1..8 hold IR values
 *    (virtual registers 0..7 of fgpu_ir_allocate; higher ones live in
 *    spill slots), xmm9..15 are scratch. SSE uses VEX-free two-operand
 *    forms, AVX2 uses VEX three-operand forms; one emitter layer
 *    (v3/v2/cmp/shift/sel) hides the difference.
 */

/* ---------------------------------------------------------------- hooks */

static uint8_t* mem_alloc_exec(uint32_t bytes, uint32_t* mapped) {
#if __STDC_HOSTED__
    long page = sysconf(_SC_PAGESIZE);
    uint32_t size = (bytes + (uint32_t)page - 1) & ~((uint32_t)page - 1);
    void* m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    *mapped = size;
    return (uint8_t*)m;
#else
    // The kernel heap is inside the low identity map, which is writable
    // and executable today. TODO(W^X): allocate whole pages from the page
    // pool, map them RW, copy, then remap RX once the VMM can do that.
    uint8_t* m = (uint8_t*)kmalloc_aligned(bytes, 64);
    *mapped = bytes;
    return m;
#endif
}

static void mem_make_exec(uint8_t* m, uint32_t mapped) {
#if __STDC_HOSTED__
    mprotect(m, mapped, PROT_READ | PROT_EXEC);
#else
    (void)m; (void)mapped;
    __asm__ volatile("mfence" ::: "memory");
#endif
}

static void mem_free_exec(uint8_t* m, uint32_t mapped) {
#if __STDC_HOSTED__
    munmap(m, mapped);
#else
    (void)mapped;
    kfree_aligned(m);
#endif
}

static void* scratch_alloc(size_t bytes) {
#if __STDC_HOSTED__
    return malloc(bytes);
#else
    return alloc_unzeroed(bytes);
#endif
}

static void scratch_free(void* p) {
#if __STDC_HOSTED__
    free(p);
#else
    free_mem(p);
#endif
}

/* ------------------------------------------------------------ CPU / mode */

uint32_t fgpu_jit_cpu_support(void) {
#if __STDC_HOSTED__
    static int done;
    static uint32_t sup;
    if (!done) {
        unsigned a, b, c, d;
        uint32_t s = 0;
        if (__get_cpuid(1, &a, &b, &c, &d)) {
            if (c & (1u << 19)) s |= FGPU_JIT_CPU_SSE41;
            if ((c & (1u << 27)) && (c & (1u << 28))) {
                unsigned lo, hi;
                __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
                (void)hi;
                if ((lo & 6u) == 6u && __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 5)) &&
                    (s & FGPU_JIT_CPU_SSE41))
                    s |= FGPU_JIT_CPU_AVX2;
            }
        }
        sup = s;
        done = 1;
    }
    return sup;
#else
    if (kernel_boot_nojit) return 0;
    uint64_t f = cpu_simd_features();
    uint32_t s = 0;
    if (f & SYS_CPU_SSE41) s |= FGPU_JIT_CPU_SSE41;
    if ((f & SYS_CPU_AVX2) && (f & SYS_CPU_SSE41)) s |= FGPU_JIT_CPU_AVX2;
    return s;
#endif
}

static fgpu_jit_mode_t g_mode = FGPU_JIT_AUTO;

void fgpu_jit_set_mode(fgpu_jit_mode_t m) { g_mode = m; }
fgpu_jit_mode_t fgpu_jit_get_mode(void) { return g_mode; }

// The path to compile for under the current mode (0 = none).
static fgpu_jit_mode_t resolve_mode(fgpu_jit_mode_t m) {
    uint32_t sup = fgpu_jit_cpu_support();
    switch (m) {
        case FGPU_JIT_AUTO:
            if (sup & FGPU_JIT_CPU_AVX2) return FGPU_JIT_AVX2;
            if (sup & FGPU_JIT_CPU_SSE41) return FGPU_JIT_SSE41;
            return FGPU_JIT_OFF;
        case FGPU_JIT_SSE41: return (sup & FGPU_JIT_CPU_SSE41) ? FGPU_JIT_SSE41 : FGPU_JIT_OFF;
        case FGPU_JIT_AVX2:  return (sup & FGPU_JIT_CPU_AVX2) ? FGPU_JIT_AVX2 : FGPU_JIT_OFF;
        default:             return FGPU_JIT_OFF;
    }
}

/* -------------------------------------------------------------- emitter */

#define MAX_CONSTS 512u
#define MAX_FIXUPS 4096u
#define IR_CAPACITY 4096u
#define MAX_TEX 16u

typedef struct { uint8_t pp, map, opc; } opd_t;

typedef struct { uint8_t kind; uint8_t reg; int32_t disp; } loc_t;   // 0 reg, 1 [base + disp], 2 constant
enum { L_REG = 0, L_MEM = 1, L_CONST = 2 };

typedef struct { uint32_t pos; uint32_t cidx; uint8_t trail; } fix_t;

typedef struct {
    uint8_t* b;
    uint32_t cap, n;
    bool fail;
    const char* why;
    bool avx;
    uint32_t nconst, nfix;
    uint32_t consts[MAX_CONSTS];
    fix_t fix[MAX_FIXUPS];
} em_t;

static void efail(em_t* e, const char* why) {
    if (!e->fail) { e->fail = true; e->why = why; }
}

static void put8(em_t* e, uint32_t v) {
    if (e->n >= e->cap) { efail(e, "code buffer full"); return; }
    e->b[e->n++] = (uint8_t)v;
}

static void put32(em_t* e, uint32_t v) {
    for (int i = 0; i < 4; i++) put8(e, v >> (8 * i));
}

static void put64(em_t* e, uint64_t v) {
    for (int i = 0; i < 8; i++) put8(e, (uint32_t)(v >> (8 * i)));
}

static inline uint32_t f2u(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }

#define RBX 3
#define RBP 5

static loc_t R(int r) { return (loc_t){ L_REG, (uint8_t)r, 0 }; }
static loc_t MEM(int base, int32_t disp) { return (loc_t){ L_MEM, (uint8_t)base, disp }; }

static loc_t KB(em_t* e, uint32_t bits) {
    for (uint32_t i = 0; i < e->nconst; i++)
        if (e->consts[i] == bits) return (loc_t){ L_CONST, 0, (int32_t)i };
    if (e->nconst >= MAX_CONSTS) { efail(e, "too many constants"); return (loc_t){ L_CONST, 0, 0 }; }
    e->consts[e->nconst] = bits;
    return (loc_t){ L_CONST, 0, (int32_t)e->nconst++ };
}
static loc_t KF(em_t* e, float f) { return KB(e, f2u(f)); }

// One instruction: legacy SSE (prefix, REX, 0F..) or VEX (3-byte form).
// `reg` is the ModRM reg field (a register or an opcode extension),
// `vvvv` the extra VEX source (ignored by the legacy form).
static void emit(em_t* e, opd_t o, int reg, int vvvv, loc_t rm, int imm, bool has_imm) {
    int rmreg = rm.reg;
    bool xr = reg >= 8, xb = rm.kind != L_CONST && rmreg >= 8;
    if (e->avx) {
        put8(e, 0xC4);
        put8(e, ((xr ? 0 : 1) << 7) | (1 << 6) | ((xb ? 0 : 1) << 5) | o.map);
        put8(e, (((~vvvv) & 15) << 3) | (1 << 2) | o.pp);      // W = 0, L = 1 (256 bit)
    } else {
        if (o.pp == 1) put8(e, 0x66);
        else if (o.pp == 2) put8(e, 0xF3);
        else if (o.pp == 3) put8(e, 0xF2);
        if (xr || xb) put8(e, 0x40 | (xr ? 4 : 0) | (xb ? 1 : 0));
        put8(e, 0x0F);
        if (o.map == 2) put8(e, 0x38);
        else if (o.map == 3) put8(e, 0x3A);
    }
    put8(e, o.opc);
    switch (rm.kind) {
        case L_REG:
            put8(e, 0xC0 | ((reg & 7) << 3) | (rmreg & 7));
            break;
        case L_MEM:
            put8(e, 0x80 | ((reg & 7) << 3) | (rmreg & 7));     // [base + disp32], base is rbx or rbp
            put32(e, (uint32_t)rm.disp);
            break;
        default:
            put8(e, ((reg & 7) << 3) | 5);                      // [rip + disp32], patched later
            if (e->nfix >= MAX_FIXUPS) { efail(e, "too many constant references"); break; }
            e->fix[e->nfix++] = (fix_t){ e->n, (uint32_t)rm.disp, has_imm ? 1 : 0 };
            put32(e, 0);
            break;
    }
    if (has_imm) put8(e, (uint32_t)imm);
}

static const opd_t OP_MOVUPS_LD = { 0, 1, 0x10 }, OP_MOVUPS_ST = { 0, 1, 0x11 };
static const opd_t OP_MOVAPS = { 0, 1, 0x28 };
static const opd_t OP_SQRTPS = { 0, 1, 0x51 }, OP_ANDPS = { 0, 1, 0x54 }, OP_ANDNPS = { 0, 1, 0x55 };
static const opd_t OP_ORPS = { 0, 1, 0x56 }, OP_XORPS = { 0, 1, 0x57 }, OP_ADDPS = { 0, 1, 0x58 };
static const opd_t OP_MULPS = { 0, 1, 0x59 }, OP_SUBPS = { 0, 1, 0x5C }, OP_MINPS = { 0, 1, 0x5D };
static const opd_t OP_DIVPS = { 0, 1, 0x5E }, OP_MAXPS = { 0, 1, 0x5F };
static const opd_t OP_CMPPS = { 0, 1, 0xC2 }, OP_SHUFPS = { 0, 1, 0xC6 }, OP_MOVMSKPS = { 0, 1, 0x50 };
static const opd_t OP_CVTTPS2DQ = { 2, 1, 0x5B }, OP_CVTDQ2PS = { 0, 1, 0x5B };
static const opd_t OP_PADDD = { 1, 1, 0xFE }, OP_PSUBD = { 1, 1, 0xFA }, OP_PSHIFT = { 1, 1, 0x72 };
static const opd_t OP_BLENDVPS = { 1, 2, 0x14 }, OP_VBLENDVPS = { 1, 3, 0x4A };
static const opd_t OP_MOVSS_LD = { 2, 1, 0x10 }, OP_VBROADCASTSS = { 1, 2, 0x18 };

enum { P_EQ = 0, P_LT = 1, P_LE = 2, P_NEQ = 4, P_NLE = 6 };

static void mov(em_t* e, int d, int s) {
    if (d != s) emit(e, OP_MOVAPS, d, 0, R(s), 0, false);
}
static void ld(em_t* e, int d, loc_t m) { emit(e, OP_MOVUPS_LD, d, 0, m, 0, false); }
static void st(em_t* e, loc_t m, int r) { emit(e, OP_MOVUPS_ST, r, 0, m, 0, false); }

// d = a op b. Legacy SSE needs d == a, so it copies; if d is also b the
// (non-commutative) operation cannot be done in place: callers avoid that.
static void v3(em_t* e, opd_t o, int d, int a, loc_t b) {
    if (e->avx) { emit(e, o, d, a, b, 0, false); return; }
    if (b.kind == L_REG && b.reg == d && d != a) { efail(e, "internal: operand clash"); return; }
    mov(e, d, a);
    emit(e, o, d, 0, b, 0, false);
}

// d = op b (one source: sqrt, conversions).
static void v2(em_t* e, opd_t o, int d, loc_t b) { emit(e, o, d, 0, b, 0, false); }

static void cmp(em_t* e, int pred, int d, int a, loc_t b) {
    if (e->avx) { emit(e, OP_CMPPS, d, a, b, pred, true); return; }
    if (b.kind == L_REG && b.reg == d && d != a) { efail(e, "internal: operand clash"); return; }
    mov(e, d, a);
    emit(e, OP_CMPPS, d, 0, b, pred, true);
}

static void shift(em_t* e, bool left, int d, int a, int count) {
    if (e->avx) { emit(e, OP_PSHIFT, left ? 6 : 2, d, R(a), count, true); return; }
    mov(e, d, a);
    emit(e, OP_PSHIFT, left ? 6 : 2, 0, R(d), count, true);
}

// b_io = mask ? a : b_io (per lane, mask lanes all ones / all zeros).
static void sel(em_t* e, int b_io, loc_t a, int mask) {
    if (e->avx) { emit(e, OP_VBLENDVPS, b_io, b_io, a, mask << 4, true); return; }
    mov(e, 0, mask);
    emit(e, OP_BLENDVPS, b_io, 0, a, 0, false);
}

static void ldk(em_t* e, int d, loc_t k) { ld(e, d, k); }

/* ------------------------------------------------------- code generator */

#define NSLOT 128u
#define HW_FIRST 1
#define HW_COUNT 8u
#define R_MASK 0
#define R_S1 9
#define R_S2 10
#define R_S3 11
#define R_D 12
#define R_T0 13
#define R_T1 14
#define R_T2 15
#define R_T3 R_S2          // unary sequences have only one source, in S1
#define R_T4 R_S3

#define SLOT_U (NSLOT - 1)
#define SLOT_V (NSLOT - 2)
#define SLOT_SAVE (NSLOT - 2 - HW_COUNT)        // HW_COUNT slots: registers live across a call
#define SLOT_TEX_TOP SLOT_SAVE                  // texture results are allocated downwards from here

typedef struct { uint32_t ir_index; uint32_t base; } texinfo_t;

typedef struct {
    em_t* e;
    const fgpu_ir_t* ir;
    uint32_t* last;
    uint32_t cur[HW_COUNT];
    uint32_t lanes;                 // W
    uint32_t tex_slot_next;         // next free texture slot, counting down
    uint32_t ntex;
    texinfo_t tex[MAX_TEX];
    uint32_t spills, tex_calls;
    int32_t off_temp, off_input, off_special, off_output;
    bool have_discard;
} g_t;

static int32_t slot_off(g_t* g, uint32_t slot) { return g->off_temp + (int32_t)(slot * 32u); }

static loc_t vloc(g_t* g, uint32_t v) {
    const fgpu_ir_inst_t* in = &g->ir->code[v];
    if (in->op == FGPU_IR_IMM) return KF(g->e, in->imm);
    if (in->reg == FGPU_IR_NO_REG) { efail(g->e, "value without register"); return R(R_S1); }
    if (in->reg < HW_COUNT) return R(HW_FIRST + in->reg);
    uint32_t slot = in->reg - HW_COUNT;
    if (slot >= SLOT_TEX_TOP) { efail(g->e, "out of spill slots"); return R(R_S1); }
    return MEM(RBX, slot_off(g, slot));
}

// The register a source lives in with its modifiers applied. `scratch`
// receives it when it has to be loaded or modified.
static int sreg(g_t* g, fgpu_ir_src_t s, int scratch) {
    em_t* e = g->e;
    loc_t l = vloc(g, s.value);
    if (l.kind == L_REG && !s.mods) return l.reg;
    if (l.kind == L_REG) mov(e, scratch, l.reg);
    else ld(e, scratch, l);
    if (s.mods & FGPU_IR_ABS) {
        // The interpreter's m_abs: x < 0 ? -x : x (keeps -0 and NaN as they are).
        cmp(e, P_LT, R_T2, scratch, KF(e, 0.0f));
        v3(e, OP_ANDPS, R_T2, R_T2, KB(e, 0x80000000u));
        v3(e, OP_XORPS, scratch, scratch, R(R_T2));
    }
    if (s.mods & FGPU_IR_NEG) v3(e, OP_XORPS, scratch, scratch, KB(e, 0x80000000u));
    return scratch;
}

// A second operand: a constant stays a memory operand, anything else a register.
static loc_t srm(g_t* g, fgpu_ir_src_t s, int scratch) {
    loc_t l = vloc(g, s.value);
    if (l.kind == L_CONST && !s.mods) return l;
    return R(sreg(g, s, scratch));
}

static int dst_hw(g_t* g, uint32_t i) {
    const fgpu_ir_inst_t* in = &g->ir->code[i];
    return in->reg < HW_COUNT ? HW_FIRST + in->reg : -1;
}

// The register to compute into: the destination itself unless it is `avoid`.
static int target(g_t* g, uint32_t i, int avoid) {
    int d = dst_hw(g, i);
    return (d >= 0 && d != avoid) ? d : R_D;
}

static void finish(g_t* g, uint32_t i, int t) {
    const fgpu_ir_inst_t* in = &g->ir->code[i];
    if (in->reg < HW_COUNT) {
        mov(g->e, HW_FIRST + in->reg, t);
        g->cur[in->reg] = i;
    } else {
        uint32_t slot = in->reg - HW_COUNT;
        if (slot >= SLOT_TEX_TOP) { efail(g->e, "out of spill slots"); return; }
        st(g->e, MEM(RBX, slot_off(g, slot)), t);
        g->spills++;
    }
}

static int rm_reg(loc_t l) { return l.kind == L_REG ? l.reg : -1; }

/* Transcendental sequences: the interpreter's code (fgpu_shader.c), one
 * vector operation per vector operation. x is a register that is not a
 * temporary; the result goes to R_D. Temporaries: T0..T4. */

static void seq_exp2(em_t* e, int x) {
    const int T0 = R_T0, T1 = R_T1, T2 = R_T2, T3 = R_T3, T4 = R_T4;
    ldk(e, T0, KF(e, -126.0f));
    cmp(e, P_NLE, T0, T0, R(x));                    // tiny = !(x >= -126), also NaN
    mov(e, T1, x);
    v3(e, OP_MINPS, T1, T1, KF(e, 127.0f));         // vmin(x, 127)
    mov(e, T2, T0);
    v3(e, OP_ANDNPS, T2, T2, R(T1));                // x = tiny ? 0 : x
    v2(e, OP_CVTTPS2DQ, T3, R(T2));
    v2(e, OP_CVTDQ2PS, T3, R(T3));
    mov(e, T4, T2);
    cmp(e, P_LT, T4, T4, R(T3));                    // x < t
    v3(e, OP_ANDPS, T4, T4, KF(e, 1.0f));
    v3(e, OP_SUBPS, T3, T3, R(T4));                 // fi = floor(x)
    v3(e, OP_SUBPS, T2, T2, R(T3));                 // f = x - fi
    mov(e, T4, T2);
    v3(e, OP_MULPS, T4, T4, KF(e, 0.0001540f));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.0013333f));
    v3(e, OP_MULPS, T4, T4, R(T2));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.0096181f));
    v3(e, OP_MULPS, T4, T4, R(T2));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.0555041f));
    v3(e, OP_MULPS, T4, T4, R(T2));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.2402265f));
    v3(e, OP_MULPS, T4, T4, R(T2));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.6931472f));
    v3(e, OP_MULPS, T4, T4, R(T2));
    v3(e, OP_ADDPS, T4, T4, KF(e, 1.0f));           // p
    v2(e, OP_CVTTPS2DQ, T3, R(T3));
    v3(e, OP_PADDD, T3, T3, KB(e, 127));
    shift(e, true, T3, T3, 23);                     // scale = 2^fi
    v3(e, OP_MULPS, T4, T4, R(T3));
    v3(e, OP_ANDNPS, R_D, T0, R(T4));               // tiny ? 0 : p * scale
}

static void seq_log2(em_t* e, int x) {
    const int T0 = R_T0, T1 = R_T1, T2 = R_T2, T3 = R_T3, T4 = R_T4;
    v3(e, OP_XORPS, T0, T0, R(T0));
    cmp(e, P_LT, T0, T0, R(x));                     // ok = x > 0
    mov(e, T1, x);
    v3(e, OP_ANDPS, T1, T1, R(T0));
    mov(e, T2, T0);
    v3(e, OP_ANDNPS, T2, T2, KF(e, 1.0f));
    v3(e, OP_ORPS, T1, T1, R(T2));                  // u = ok ? x : 1
    shift(e, false, T2, T1, 23);
    v3(e, OP_ANDPS, T2, T2, KB(e, 0xFF));
    v3(e, OP_PSUBD, T2, T2, KB(e, 127));
    v2(e, OP_CVTDQ2PS, T2, R(T2));                  // e
    v3(e, OP_ANDPS, T1, T1, KB(e, 0x007FFFFFu));
    v3(e, OP_ORPS, T1, T1, KB(e, 0x3F800000u));     // m in [1, 2)
    ldk(e, T3, KF(e, 1.41421356f));
    cmp(e, P_LT, T3, T3, R(T1));                    // big = m > sqrt2
    mov(e, T4, T1);
    v3(e, OP_MULPS, T4, T4, KF(e, 0.5f));
    sel(e, T1, R(T4), T3);                          // m = big ? m * 0.5 : m
    mov(e, T4, T3);
    v3(e, OP_ANDPS, T4, T4, KF(e, 1.0f));
    v3(e, OP_ADDPS, T2, T2, R(T4));                 // e += big ? 1 : 0
    mov(e, T3, T1);
    v3(e, OP_SUBPS, T3, T3, KF(e, 1.0f));
    mov(e, T4, T1);
    v3(e, OP_ADDPS, T4, T4, KF(e, 1.0f));
    v3(e, OP_DIVPS, T3, T3, R(T4));                 // t = (m - 1) / (m + 1)
    mov(e, T1, T3);
    v3(e, OP_MULPS, T1, T1, R(T3));                 // t2
    mov(e, T4, T1);
    v3(e, OP_MULPS, T4, T4, KF(e, 0.2222222f));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.2857143f));
    v3(e, OP_MULPS, T4, T4, R(T1));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.4f));
    v3(e, OP_MULPS, T4, T4, R(T1));
    v3(e, OP_ADDPS, T4, T4, KF(e, 0.6666667f));
    v3(e, OP_MULPS, T4, T4, R(T1));
    v3(e, OP_ADDPS, T4, T4, KF(e, 2.0f));
    v3(e, OP_MULPS, T4, T4, R(T3));                 // s
    v3(e, OP_MULPS, T4, T4, KF(e, 1.44269504f));
    v3(e, OP_ADDPS, T4, T4, R(T2));                 // e + s * 1/ln2
    ldk(e, T1, KF(e, -126.0f));
    sel(e, T1, R(T4), T0);                          // ok ? ... : -126
    mov(e, R_D, T1);
}

#define PI_F 3.14159265358979323846f

static void seq_sin(em_t* e, int x) {
    const int T0 = R_T0, T1 = R_T1, T2 = R_T2, T3 = R_T3, T4 = R_T4;
    mov(e, T0, x);
    v3(e, OP_ANDPS, T0, T0, KB(e, 0x7FFFFFFFu));
    cmp(e, P_LT, T0, T0, KF(e, 1e7f));              // |x| < 1e7 (false for NaN)
    mov(e, T1, x);
    v3(e, OP_ANDPS, T1, T1, R(T0));                 // x = ok ? x : 0
    mov(e, T2, T1);
    v3(e, OP_MULPS, T2, T2, KF(e, 1.0f / (2.0f * PI_F)));
    v3(e, OP_ADDPS, T2, T2, KF(e, 0.5f));
    v2(e, OP_CVTTPS2DQ, T3, R(T2));
    v2(e, OP_CVTDQ2PS, T3, R(T3));
    mov(e, T4, T2);
    cmp(e, P_LT, T4, T4, R(T3));
    v3(e, OP_ANDPS, T4, T4, KF(e, 1.0f));
    v3(e, OP_SUBPS, T3, T3, R(T4));                 // k = floor(x / 2pi + 0.5)
    v3(e, OP_MULPS, T3, T3, KF(e, 2.0f * PI_F));
    v3(e, OP_SUBPS, T1, T1, R(T3));                 // x -= k * 2pi
    ldk(e, T2, KF(e, PI_F / 2));
    cmp(e, P_LT, T2, T2, R(T1));                    // x > pi/2
    ldk(e, T3, KF(e, PI_F));
    v3(e, OP_SUBPS, T3, T3, R(T1));
    sel(e, T1, R(T3), T2);
    mov(e, T2, T1);
    cmp(e, P_LT, T2, T2, KF(e, -PI_F / 2));         // x < -pi/2
    ldk(e, T3, KF(e, -PI_F));
    v3(e, OP_SUBPS, T3, T3, R(T1));
    sel(e, T1, R(T3), T2);
    mov(e, T2, T1);
    v3(e, OP_MULPS, T2, T2, R(T1));                 // x2
    mov(e, T3, T2);
    v3(e, OP_DIVPS, T3, T3, KF(e, 110.0f));
    ldk(e, T4, KF(e, 1.0f));
    v3(e, OP_SUBPS, T4, T4, R(T3));
    static const float k_div[4] = { 72.0f, 42.0f, 20.0f, 6.0f };
    for (int i = 0; i < 4; i++) {
        mov(e, T3, T2);
        v3(e, OP_DIVPS, T3, T3, KF(e, k_div[i]));
        v3(e, OP_MULPS, T3, T3, R(T4));
        ldk(e, T4, KF(e, 1.0f));
        v3(e, OP_SUBPS, T4, T4, R(T3));
    }
    v3(e, OP_MULPS, R_D, T1, R(T4));
}

static void seq_floor(em_t* e, int x) {
    const int T0 = R_T0, T1 = R_T1, T2 = R_T2, T3 = R_T3;
    mov(e, T0, x);
    v3(e, OP_ANDPS, T0, T0, KB(e, 0x7FFFFFFFu));
    cmp(e, P_LT, T0, T0, KF(e, 8388608.0f));        // |x| < 2^23
    mov(e, T1, x);
    v3(e, OP_ANDPS, T1, T1, R(T0));
    v2(e, OP_CVTTPS2DQ, T1, R(T1));
    v2(e, OP_CVTDQ2PS, T1, R(T1));                  // t = (float)(int)x
    mov(e, T2, x);
    cmp(e, P_LT, T2, T2, R(T1));                    // t > x
    v3(e, OP_ANDPS, T2, T2, KF(e, 1.0f));
    v3(e, OP_SUBPS, T1, T1, R(T2));
    mov(e, T3, x);
    sel(e, T3, R(T1), T0);                          // small ? t : x
    mov(e, R_D, T3);
}

/* One IR instruction. */

static void emit_call_prologue_live(g_t* g, uint32_t i, bool save) {
    em_t* e = g->e;
    for (uint32_t r = 0; r < HW_COUNT; r++) {
        uint32_t v = g->cur[r];
        if (v == 0xFFFFFFFFu || g->last[v] <= i) continue;
        loc_t m = MEM(RBX, slot_off(g, SLOT_SAVE + r));
        if (save) st(e, m, HW_FIRST + (int)r);
        else ld(e, HW_FIRST + (int)r, m);
    }
}

// The texture helper: samples the unit for `n` lanes, out is [4][8] floats.
static void jit_tex_helper(const fgpu_texture_t* textures, uint32_t unit, const float* u,
                           const float* v, float* out, uint32_t n) {
    const fgpu_texture_t* t = textures ? &textures[unit] : NULL;
    for (uint32_t l = 0; l < n; l++) {
        float px[4];
        fgpu_texture_sample(t, u[l], v[l], px);
        for (int c = 0; c < 4; c++) out[c * 8 + l] = px[c];
    }
}

static void emit_lea(em_t* e, int rexb, int modrm_reg, int32_t disp) {
    // lea reg64, [rbx + disp32]
    put8(e, 0x48 | (rexb ? 4 : 0));
    put8(e, 0x8D);
    put8(e, 0x80 | ((modrm_reg & 7) << 3) | RBX);
    put32(e, (uint32_t)disp);
}

static void gen_inst(g_t* g, uint32_t i) {
    em_t* e = g->e;
    const fgpu_ir_inst_t* in = &g->ir->code[i];
    switch (in->op) {
        case FGPU_IR_IMM:
            return;
        case FGPU_IR_INPUT:
        case FGPU_IR_SPECIAL: {
            bool in_file = in->op == FGPU_IR_INPUT;
            uint32_t limit = in_file ? FGPU_MAX_ATTRIBS * 4u : 2u * 4u;
            if (in->aux >= limit) { efail(e, "input slot out of range"); return; }
            int t = target(g, i, -1);
            int32_t base = in_file ? g->off_input : g->off_special;
            ld(e, t, MEM(RBX, base + (int32_t)(in->aux * 32u)));
            finish(g, i, t);
            return;
        }
        case FGPU_IR_UNIFORM: {
            if (in->aux >= FGPU_SHADER_MAX_UNIFORMS * 4u) { efail(e, "uniform out of range"); return; }
            int t = target(g, i, -1);
            loc_t m = MEM(RBP, (int32_t)(in->aux * 4u));
            if (e->avx) {
                emit(e, OP_VBROADCASTSS, t, 0, m, 0, false);
            } else {
                emit(e, OP_MOVSS_LD, t, 0, m, 0, false);
                emit(e, OP_SHUFPS, t, 0, R(t), 0, true);
            }
            finish(g, i, t);
            return;
        }
        case FGPU_IR_ADD: case FGPU_IR_SUB: case FGPU_IR_MUL: case FGPU_IR_DIV:
        case FGPU_IR_MIN: case FGPU_IR_MAX: {
            opd_t o = in->op == FGPU_IR_ADD ? OP_ADDPS : in->op == FGPU_IR_SUB ? OP_SUBPS :
                      in->op == FGPU_IR_MUL ? OP_MULPS : in->op == FGPU_IR_DIV ? OP_DIVPS :
                      in->op == FGPU_IR_MIN ? OP_MINPS : OP_MAXPS;
            int ra = sreg(g, in->src[0], R_S1);
            loc_t b = srm(g, in->src[1], R_S2);
            int t = target(g, i, rm_reg(b));
            v3(e, o, t, ra, b);
            finish(g, i, t);
            return;
        }
        case FGPU_IR_FMA: {
            int ra = sreg(g, in->src[0], R_S1);
            loc_t b = srm(g, in->src[1], R_S2);
            v3(e, OP_MULPS, R_T0, ra, b);
            loc_t c = srm(g, in->src[2], R_S3);
            int t = target(g, i, rm_reg(c));
            v3(e, OP_ADDPS, t, R_T0, c);
            finish(g, i, t);
            return;
        }
        case FGPU_IR_SLT: case FGPU_IR_SLE: case FGPU_IR_SEQ: case FGPU_IR_SNE: {
            int pred = in->op == FGPU_IR_SLT ? P_LT : in->op == FGPU_IR_SLE ? P_LE :
                       in->op == FGPU_IR_SEQ ? P_EQ : P_NEQ;
            int ra = sreg(g, in->src[0], R_S1);
            loc_t b = srm(g, in->src[1], R_S2);
            int t = target(g, i, rm_reg(b));
            cmp(e, pred, t, ra, b);
            v3(e, OP_ANDPS, t, t, KF(e, 1.0f));
            finish(g, i, t);
            return;
        }
        case FGPU_IR_SEL: {
            int ra = sreg(g, in->src[0], R_S1);
            int m = e->avx ? R_T0 : R_MASK;
            cmp(e, P_NEQ, m, ra, KF(e, 0.0f));
            loc_t b = srm(g, in->src[1], R_S2);
            int rc = sreg(g, in->src[2], R_S3);
            int t = target(g, i, rm_reg(b));
            if (e->avx) {
                emit(e, OP_VBLENDVPS, t, rc, b, m << 4, true);
            } else {
                mov(e, t, rc);
                emit(e, OP_BLENDVPS, t, 0, b, 0, false);
            }
            finish(g, i, t);
            return;
        }
        case FGPU_IR_SQRT: {
            int x = sreg(g, in->src[0], R_S1);
            int t = target(g, i, -1);
            v2(e, OP_SQRTPS, t, R(x));
            finish(g, i, t);
            return;
        }
        case FGPU_IR_RSQ: {
            int x = sreg(g, in->src[0], R_S1);
            v2(e, OP_SQRTPS, R_T0, R(x));
            ldk(e, R_D, KF(e, 1.0f));
            v3(e, OP_DIVPS, R_D, R_D, R(R_T0));
            finish(g, i, R_D);
            return;
        }
        case FGPU_IR_FLOOR:
            seq_floor(e, sreg(g, in->src[0], R_S1));
            finish(g, i, R_D);
            return;
        case FGPU_IR_EX2:
            seq_exp2(e, sreg(g, in->src[0], R_S1));
            finish(g, i, R_D);
            return;
        case FGPU_IR_LG2:
            seq_log2(e, sreg(g, in->src[0], R_S1));
            finish(g, i, R_D);
            return;
        case FGPU_IR_SIN:
            seq_sin(e, sreg(g, in->src[0], R_S1));
            finish(g, i, R_D);
            return;
        case FGPU_IR_COS: {
            int x = sreg(g, in->src[0], R_S1);
            mov(e, R_S1, x);
            v3(e, OP_ADDPS, R_S1, R_S1, KF(e, PI_F / 2));
            seq_sin(e, R_S1);
            finish(g, i, R_D);
            return;
        }
        case FGPU_IR_TEX: {
            if (in->aux >= FGPU_MAX_TEXTURE_UNITS) { efail(e, "texture unit out of range"); return; }
            if (g->ntex >= MAX_TEX || g->tex_slot_next < 4 + 1) { efail(e, "too many texture samples"); return; }
            uint32_t base = g->tex_slot_next - 4;
            g->tex_slot_next = base;
            g->tex[g->ntex++] = (texinfo_t){ i, base };
            g->tex_calls++;
            int ru = sreg(g, in->src[0], R_S1);
            st(e, MEM(RBX, slot_off(g, SLOT_U)), ru);
            int rv = sreg(g, in->src[1], R_S2);
            st(e, MEM(RBX, slot_off(g, SLOT_V)), rv);
            emit_call_prologue_live(g, i, true);
            if (e->avx) { put8(e, 0xC5); put8(e, 0xF8); put8(e, 0x77); }        // vzeroupper
            put8(e, 0x4C); put8(e, 0x89); put8(e, 0xE7);                        // mov rdi, r12
            put8(e, 0xBE); put32(e, in->aux);                                   // mov esi, unit
            emit_lea(e, 0, 2, slot_off(g, SLOT_U));                             // lea rdx, [rbx + u]
            emit_lea(e, 0, 1, slot_off(g, SLOT_V));                             // lea rcx, [rbx + v]
            put8(e, 0x4C); put8(e, 0x8D); put8(e, 0x83);                        // lea r8, [rbx + out]
            put32(e, (uint32_t)slot_off(g, base));
            put8(e, 0x41); put8(e, 0xB9); put32(e, g->lanes);                   // mov r9d, W
            put8(e, 0x48); put8(e, 0xB8);                                       // mov rax, helper
            put64(e, (uint64_t)(uintptr_t)&jit_tex_helper);
            put8(e, 0xFF); put8(e, 0xD0);                                       // call rax
            emit_call_prologue_live(g, i, false);
            int t = target(g, i, -1);
            ld(e, t, MEM(RBX, slot_off(g, base)));
            finish(g, i, t);
            return;
        }
        case FGPU_IR_TEXC: {
            uint32_t tex = in->src[0].value;
            uint32_t base = 0xFFFFFFFFu;
            for (uint32_t k = 0; k < g->ntex; k++) if (g->tex[k].ir_index == tex) base = g->tex[k].base;
            if (base == 0xFFFFFFFFu || in->aux < 1 || in->aux > 3) { efail(e, "texture component without sample"); return; }
            int t = target(g, i, -1);
            ld(e, t, MEM(RBX, slot_off(g, base + in->aux)));
            finish(g, i, t);
            return;
        }
        case FGPU_IR_OUTPUT: {
            if (in->aux >= (1u + FGPU_SHADER_MAX_VARYINGS) * 4u) { efail(e, "output out of range"); return; }
            int r = sreg(g, in->src[0], R_S1);
            st(e, MEM(RBX, g->off_output + (int32_t)(in->aux * 32u)), r);
            return;
        }
        case FGPU_IR_DISCARD: {
            if (i + 1 != g->ir->count) { efail(e, "discard is not last"); return; }
            int ra = sreg(g, in->src[0], R_S1);
            cmp(e, P_NEQ, R_T0, ra, KF(e, 0.0f));                  // lanes that were killed
            emit(e, OP_MOVMSKPS, 0, 0, R(R_T0), 0, false);          // eax
            put8(e, 0x35); put32(e, (1u << g->lanes) - 1u);         // xor eax, all lanes
            g->have_discard = true;
            return;
        }
        default:
            efail(e, "unsupported IR instruction");
            return;
    }
}

/* --------------------------------------------------------- compilation */

typedef uint32_t (*jit_fn_t)(char* batch, const float* uniforms, const fgpu_texture_t* textures);

struct fgpu_jit_code {
    uint8_t* mem;
    uint32_t mapped;
    jit_fn_t fn;
    uint32_t width;         // 4 (SSE4.1) or 8 (AVX2) lanes per call
    uint32_t outs;          // output registers the stage writes
    bool fragment;
    fgpu_jit_info_t info;
};

#define CODE_LIMIT_DEFAULT (128u * 1024u)

fgpu_jit_code_t* fgpu_jit_compile_stage(const fgpu_program_t* p, bool fragment, fgpu_jit_mode_t path,
                                        uint32_t code_limit, const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    if (path != FGPU_JIT_SSE41 && path != FGPU_JIT_AVX2) { *why = "no path"; return NULL; }
    if (resolve_mode(path) != path) { *why = "CPU lacks the instruction set"; return NULL; }
    if (!code_limit || code_limit > CODE_LIMIT_DEFAULT) code_limit = CODE_LIMIT_DEFAULT;

    fgpu_ir_t ir;
    __builtin_memset(&ir, 0, sizeof(ir));
    ir.code = (fgpu_ir_inst_t*)scratch_alloc(sizeof(fgpu_ir_inst_t) * IR_CAPACITY);
    ir.capacity = IR_CAPACITY;
    ir.hash = (uint32_t*)scratch_alloc(sizeof(uint32_t) * FGPU_IR_HASH_WORDS(IR_CAPACITY));
    em_t* e = (em_t*)scratch_alloc(sizeof(em_t));
    uint8_t* buf = (uint8_t*)scratch_alloc(code_limit);
    g_t* g = (g_t*)scratch_alloc(sizeof(g_t));
    fgpu_jit_code_t* result = NULL;
    if (!ir.code || !ir.hash || !e || !buf || !g) { *why = "out of memory"; goto done; }

    if (!fgpu_ir_lower_ex(&ir, p, fragment, FGPU_IR_EXACT, why)) goto done;
    fgpu_ir_allocate(&ir);
    // Last use of every value (the allocator's scratch is gone by now).
    uint32_t* last = ir.hash;
    for (uint32_t i = 0; i < ir.count; i++) last[i] = i;
    for (uint32_t i = 0; i < ir.count; i++)
        for (uint32_t k = 0; k < ir.code[i].nsrc; k++) last[ir.code[i].src[k].value] = i;

    __builtin_memset(e, 0, offsetof(em_t, consts));
    e->b = buf;
    e->cap = code_limit;
    e->avx = path == FGPU_JIT_AVX2;
    __builtin_memset(g, 0, sizeof(*g));
    g->e = e;
    g->ir = &ir;
    g->last = last;
    for (uint32_t r = 0; r < HW_COUNT; r++) g->cur[r] = 0xFFFFFFFFu;
    g->lanes = e->avx ? 8 : 4;
    g->tex_slot_next = SLOT_TEX_TOP;
    g->off_temp = (int32_t)offsetof(fgpu_shader_batch_t, temp);
    g->off_input = (int32_t)offsetof(fgpu_shader_batch_t, input);
    g->off_special = (int32_t)offsetof(fgpu_shader_batch_t, special);
    g->off_output = (int32_t)offsetof(fgpu_shader_batch_t, output);
    // The spill area is the batch's temp registers.
    if (NSLOT * 32u > sizeof(((fgpu_shader_batch_t*)0)->temp)) { *why = "batch too small"; goto done; }

    // push rbx; push rbp; push r12; mov rbx, rdi; mov rbp, rsi; mov r12, rdx
    put8(e, 0x53); put8(e, 0x55); put8(e, 0x41); put8(e, 0x54);
    put8(e, 0x48); put8(e, 0x89); put8(e, 0xFB);
    put8(e, 0x48); put8(e, 0x89); put8(e, 0xF5);
    put8(e, 0x49); put8(e, 0x89); put8(e, 0xD4);
    for (uint32_t i = 0; i < ir.count && !e->fail; i++) gen_inst(g, i);
    // Spill slots count up from 0, texture results down from SLOT_TEX_TOP: they must not meet.
    if (!e->fail && ir.reg_count > HW_COUNT && ir.reg_count - HW_COUNT > g->tex_slot_next)
        efail(e, "spill slots collide with texture results");
    if (!g->have_discard) { put8(e, 0xB8); put32(e, (1u << g->lanes) - 1u); }   // mov eax, all lanes
    if (e->avx) { put8(e, 0xC5); put8(e, 0xF8); put8(e, 0x77); }                // vzeroupper
    put8(e, 0x41); put8(e, 0x5C); put8(e, 0x5D); put8(e, 0x5B); put8(e, 0xC3);   // pop r12; pop rbp; pop rbx; ret
    if (e->fail) { *why = e->why; goto done; }

    // Constant pool after the code, 32-byte entries (every lane the same).
    uint32_t pool = (e->n + 31u) & ~31u;
    uint32_t total = pool + e->nconst * 32u;
    if (total > code_limit + MAX_CONSTS * 32u) { *why = "code too large"; goto done; }
    uint8_t* image = (uint8_t*)scratch_alloc(total ? total : 1);
    if (!image) { *why = "out of memory"; goto done; }
    for (uint32_t i = 0; i < total; i++) image[i] = i < e->n ? buf[i] : 0xCC;
    for (uint32_t k = 0; k < e->nconst; k++)
        for (uint32_t l = 0; l < 8; l++)
            for (uint32_t b = 0; b < 4; b++)
                image[pool + k * 32 + l * 4 + b] = (uint8_t)(e->consts[k] >> (8 * b));
    for (uint32_t k = 0; k < e->nfix; k++) {
        const fix_t* f = &e->fix[k];
        int32_t disp = (int32_t)(pool + f->cidx * 32u) - (int32_t)(f->pos + 4u + f->trail);
        for (int b = 0; b < 4; b++) image[f->pos + b] = (uint8_t)((uint32_t)disp >> (8 * b));
    }
    result = (fgpu_jit_code_t*)scratch_alloc(sizeof(*result));
    uint32_t mapped = 0;
    uint8_t* mem = result ? mem_alloc_exec(total, &mapped) : NULL;
    if (!result || !mem) {
        scratch_free(image);
        scratch_free(result);
        result = NULL;
        *why = "out of memory";
        goto done;
    }
    for (uint32_t i = 0; i < total; i++) mem[i] = image[i];
    scratch_free(image);
    mem_make_exec(mem, mapped);
    result->mem = mem;
    result->mapped = mapped;
    result->fn = (jit_fn_t)(uintptr_t)mem;
    result->width = g->lanes;
    result->outs = fragment ? 1u : 1u + p->h.varying_count;
    result->fragment = fragment;
    result->info.path = (uint32_t)path;
    result->info.code_bytes = total;
    result->info.ir_ops = ir.count;
    result->info.spills = ir.reg_count > HW_COUNT ? ir.reg_count - HW_COUNT : 0;
    result->info.tex_calls = g->tex_calls;
done:
    scratch_free(ir.code);
    scratch_free(ir.hash);
    scratch_free(e);
    scratch_free(buf);
    scratch_free(g);
    return result;
}

void fgpu_jit_free(fgpu_jit_code_t* c) {
    if (!c) return;
    mem_free_exec(c->mem, c->mapped);
    scratch_free(c);
}

/* ------------------------------------------------------------ execution */

static const float g_zero_uniforms[FGPU_SHADER_MAX_UNIFORMS][4];

uint32_t fgpu_jit_run_code(const fgpu_jit_code_t* c, fgpu_shader_batch_t* b, uint32_t lanes,
                           const float (*uniforms)[4], const fgpu_texture_t* textures) {
    lanes &= 0xFFu;
    if (!uniforms) uniforms = g_zero_uniforms;
    uint32_t alive = 0;
    if (c->width == 8) {
        if (lanes) alive = c->fn((char*)b, &uniforms[0][0], textures);
    } else {
        if (lanes & 0x0Fu) alive = c->fn((char*)b, &uniforms[0][0], textures);
        if (lanes & 0xF0u) alive |= c->fn((char*)b + 16, &uniforms[0][0], textures) << 4;
    }
    alive &= lanes;
    if (lanes != 0xFFu) {
        // Lanes that were not asked for read as the interpreter leaves them.
        for (uint32_t o = 0; o < c->outs; o++)
            for (int ch = 0; ch < 4; ch++) {
                float v = (ch == 3 && (c->fragment || o == 0)) ? 1.0f : 0.0f;
                for (uint32_t l = 0; l < 8; l++)
                    if (!(lanes & (1u << l))) b->output[o][ch][l] = v;
            }
    }
    return alive;
}

/* ------------------------------------------------------- program glue */

typedef struct {
    fgpu_jit_code_t* vs;
    fgpu_jit_code_t* fs;
} prog_jit_t;

bool fgpu_program_jit(fgpu_program_t* p) {
    fgpu_jit_mode_t path = resolve_mode(g_mode);
    if (path == FGPU_JIT_OFF || p->jit) return false;
    prog_jit_t* j = (prog_jit_t*)scratch_alloc(sizeof(*j));
    if (!j) return false;
    j->vs = fgpu_jit_compile_stage(p, false, path, 0, NULL);
    j->fs = fgpu_jit_compile_stage(p, true, path, 0, NULL);
#if !__STDC_HOSTED__
    // One line per program: which path, and whether each stage compiled.
    serial_write_str("fgpu: shader JIT ");
    serial_write_str(path == FGPU_JIT_AVX2 ? "(AVX2): " : "(SSE4.1): ");
    serial_write_str(j->vs ? "vertex stage compiled, " : "vertex stage stays on the interpreter, ");
    serial_write_str(j->fs ? "fragment stage compiled\n" : "fragment stage stays on the interpreter\n");
#endif
    if (!j->vs && !j->fs) { scratch_free(j); return false; }
    p->jit = j;
    return true;
}

void fgpu_program_release(fgpu_program_t* p) {
    prog_jit_t* j = (prog_jit_t*)p->jit;
    p->jit = NULL;
    if (!j) return;
    fgpu_jit_free(j->vs);
    fgpu_jit_free(j->fs);
    scratch_free(j);
}

bool fgpu_jit_run_batch(const fgpu_program_t* p, bool fragment, fgpu_shader_batch_t* b,
                        uint32_t lanes, const float (*uniforms)[4],
                        const fgpu_texture_t* textures, uint32_t* kept) {
    const prog_jit_t* j = (const prog_jit_t*)p->jit;
    if (!j) return false;
    const fgpu_jit_code_t* c = fragment ? j->fs : j->vs;
    if (!c) return false;
    *kept = fgpu_jit_run_code(c, b, lanes, uniforms, textures);
    return true;
}

bool fgpu_program_jit_info(const fgpu_program_t* p, bool fragment, fgpu_jit_info_t* out) {
    const prog_jit_t* j = (const prog_jit_t*)p->jit;
    const fgpu_jit_code_t* c = j ? (fragment ? j->fs : j->vs) : NULL;
    if (!c) return false;
    *out = c->info;
    return true;
}
