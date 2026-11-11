#include "x86_64/fgpu_backend.h"
#include "x86_64/fgpu_ir.h"
#include "x86_64/fgpu_abi.h"
#include "x86_64/nv_gpu.h"
#include "x86_64/nv_sass.h"
#include "x86_64/nv_3d.h"
#include "x86_64/nv_builtin_fgsl.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "string.h"
#include "fglsl.h"
#include <stdarg.h>

/*
 * FGPU hardware backend for NVIDIA Turing (RTX 20 / GTX 16), and the
 * bring-up report behind 'gpu nvidia'.
 *
 * Status:
 *   1. firmware files: checked (nv_fw.c)                         done
 *   2a. FWSEC found in the VBIOS (nv_vbios.c), reported         done, untested on a card
 *   2b. GSP boot: FWSEC-FRTS (nv_gsp.c, 'gpu nvidia boot')      works on an RTX 2060
 *       booter_load on SEC2, GSP-RM (nv_gsp_rm.c)                works on an RTX 2060
 *   3a. first RPC to GSP-RM (nv_gsp_rm.c)                        works on an RTX 2060
 *   3b. RM objects, page tables, copy engine channel (nv_rm_chan.c) works on an RTX 2060
 *   3c. copy engine fills: test pattern on screen, FGPU clears  works on an RTX 2060
 *   4a. 3D channel, graphics context (nv_rm_chan.c)             written, not yet run on a card
 *   4b. draws: shader machine code (nv_sass.c), 3D state and
 *       draw methods (nv_3d.c), this file                        written, not yet run on a card
 *
 * Step 4b proves itself before it is used: init() draws two triangles
 * into a 64x64 surface and reads pixels back. Only if they are right do
 * FGPU draws go to the 3D engine ("hw-3d"); otherwise the report says
 * what came back and everything stays as before (clears on the copy
 * engine, draws on the CPU). A job that does not finish later switches
 * the 3D path off again.
 *
 * Memory: color surfaces, textures and vertex/index buffers are in VRAM
 * (fgpu.c puts them there when this backend reports FGPU_CAP_HW_3D) and
 * reach the GPU through the 1:1 VRAM mapping. Depth lives in a
 * block-linear buffer of this backend's own, one per FGPU depth surface:
 * the hardware has no pitch-linear depth. So a frame's depth is on the
 * GPU; a draw that has to fall back to the CPU (a shader the encoder
 * refuses, memory outside VRAM) uses the CPU depth buffer, which only
 * knows the clears.
 */

#define IR_CAPACITY 16384u
#define MAX_ZETA 32u

typedef struct {
    uint64_t vram;              // header + code, GPU_VRAM_INVALID if none
    uint32_t regs;
    uint32_t textures;
} nv_stage_t;

typedef struct {
    nv_stage_t vs, fs;
    bool gpu;                   // both stages are on the card
    int32_t builtin_reg[8];     // built-in programs: uniform registers (-1 = unused)
} nv_program_t;

static struct {
    gpu_adapter_t* a;
    bool hw3d;                  // the self-test passed
    bool init_sent;             // nv3d_push_init reached the channel
    bool front_ccw;
    uint64_t pools;             // VRAM: texture headers, samplers, constants, a zero page
    nv_program_t* builtin[4];   // by FGPU_PROGRAM_*
    bool builtin_tried[4];
    struct {
        const void* cpu;        // the FGPU depth surface it shadows
        uint64_t vram;
        nv3d_zeta_t z;
    } zeta[MAX_ZETA];
    float uniforms[FGPU_SHADER_MAX_UNIFORMS * 4];
    uint32_t draws, clears;
} g;

#define POOL_TIC   0u
#define POOL_TSC   (NV3D_POOL_ENTRIES * 32u)
#define POOL_CB    4096u
#define POOL_ZERO  8192u
#define POOL_BYTES 12288u

static void vram_write(uint64_t offset, const void* data, uint32_t bytes) {
    const uint32_t* w = (const uint32_t*)data;
    for (uint32_t i = 0; i < bytes; i += 4)
        *(volatile uint32_t*)(g.a->aperture + offset + i) = w[i / 4];
}

static uint32_t vram_read32(uint64_t offset) { return *(volatile uint32_t*)(g.a->aperture + offset); }

/* ------------------------------------------------------------ programs */

static bool compile_stage(const fgpu_program_t* p, bool fragment, nv_stage_t* out) {
    out->vram = GPU_VRAM_INVALID;
    if (!g.a) return false;
    fgpu_ir_t ir = { 0 };
    ir.capacity = IR_CAPACITY;
    ir.code = (fgpu_ir_inst_t*)alloc_unzeroed(sizeof(fgpu_ir_inst_t) * IR_CAPACITY);
    ir.hash = (uint32_t*)alloc_unzeroed(sizeof(uint32_t) * FGPU_IR_HASH_WORDS(IR_CAPACITY));
    uint32_t* words = NULL;
    bool ok = false;
    const char* why = "out of memory";
    if (ir.code && ir.hash && fgpu_ir_lower(&ir, p, fragment, &why)) {
        fgpu_ir_allocate(&ir);
        // At most a few instructions per IR operation, plus the epilogue.
        uint32_t capacity = (ir.count * 6u + 128u) * 4u;
        words = (uint32_t*)alloc_unzeroed((size_t)capacity * 4u);
        nv_sass_t sh = { .code = words, .capacity = capacity };
        why = "out of memory";
        if (words && nv_sass_encode(ir.code, ir.count, ir.reg_count, fragment, p->h.varying_count, &sh, &why)) {
            uint64_t bytes = (uint64_t)(NV_SASS_SPH_WORDS + sh.count) * 4u;
            uint64_t off = gpu_vram_alloc(g.a, bytes, 4096, 0);
            why = "out of VRAM";
            if (off != GPU_VRAM_INVALID && off + bytes <= nv_rm_vram_mapped(g.a)) {
                vram_write(off, sh.sph, NV_SASS_SPH_WORDS * 4u);
                vram_write(off + NV_SASS_SPH_WORDS * 4u, sh.code, sh.count * 4u);
                out->vram = off;
                out->regs = sh.regs;
                out->textures = sh.textures;
                ok = true;
            } else if (off != GPU_VRAM_INVALID) {
                gpu_vram_free(g.a, off);
            }
        }
    }
    if (!ok) {
        serial_write_str("nv: shader stays on the CPU: ");
        serial_write_str(why);
        serial_write_str("\n");
    }
    if (words) free_mem(words);
    if (ir.code) free_mem(ir.code);
    if (ir.hash) free_mem(ir.hash);
    return ok;
}

static void nv_program_destroy(void* program) {
    nv_program_t* np = (nv_program_t*)program;
    if (!np) return;
    if (g.a && np->vs.vram != GPU_VRAM_INVALID) gpu_vram_free(g.a, np->vs.vram);
    if (g.a && np->fs.vram != GPU_VRAM_INVALID) gpu_vram_free(g.a, np->fs.vram);
    free_mem(np);
}

// NULL when either stage cannot run on the card: the program's draws
// then run on the CPU.
static void* nv_program_create(const fgpu_program_t* p) {
    if (!g.a || !nv_rm_3d_ready(g.a)) return NULL;
    nv_program_t* np = (nv_program_t*)alloc(sizeof(nv_program_t));
    if (!np) return NULL;
    np->vs.vram = np->fs.vram = GPU_VRAM_INVALID;
    np->gpu = compile_stage(p, false, &np->vs) && compile_stage(p, true, &np->fs);
    if (!np->gpu) {
        nv_program_destroy(np);
        return NULL;
    }
    return np;
}

// The built-in programs, compiled from FGSL the first time they are drawn.
static nv_program_t* builtin_program(uint32_t program) {
    if (program < FGPU_PROGRAM_COLOR || program > FGPU_PROGRAM_PHONG) return NULL;
    if (g.builtin_tried[program]) return g.builtin[program];
    g.builtin_tried[program] = true;
    fglsl_program_t* c = (fglsl_program_t*)alloc_unzeroed(sizeof(fglsl_program_t));
    if (!c) return NULL;
    const char* vs = program == FGPU_PROGRAM_GOURAUD ? NV_BUILTIN_VS_GOURAUD : NV_BUILTIN_VS_PLAIN;
    const char* fs = program == FGPU_PROGRAM_PHONG ? NV_BUILTIN_FS_PHONG : NV_BUILTIN_FS_PLAIN;
    fgpu_program_t prog;
    const char* why = "";
    nv_program_t* np = NULL;
    if (fglsl_compile(c, vs, fs) == 0 && fgpu_program_check(c->words, c->size, &prog, &why))
        np = (nv_program_t*)nv_program_create(&prog);
    if (np) {
        for (int i = 0; i < 8; i++) {
            int u = fglsl_find_uniform(c, nv_builtin_uniform_names[i]);
            np->builtin_reg[i] = u >= 0 ? (int32_t)c->uniforms[u].reg : -1;
        }
    } else {
        serial_write_str("nv: a built-in program did not compile for the GPU\n");
    }
    free_mem(c);
    g.builtin[program] = np;
    return np;
}

static const float* builtin_uniforms(const nv_program_t* np, const fgpu_uniforms_t* u) {
    const float* src[8] = { u->mvp, u->model, u->normal, u->light_dir, u->light_color, u->ambient,
                            u->camera_pos, u->material };
    static const uint8_t floats[8] = { 16, 16, 16, 4, 4, 4, 4, 4 };
    memset(g.uniforms, 0, sizeof(g.uniforms));
    for (int i = 0; i < 8; i++) {
        int32_t reg = np->builtin_reg[i];
        if (reg < 0 || (uint32_t)reg * 4u + floats[i] > FGPU_SHADER_MAX_UNIFORMS * 4u) continue;
        memcpy(&g.uniforms[reg * 4], src[i], floats[i] * sizeof(float));
    }
    return g.uniforms;
}

/* -------------------------------------------------------------- memory */

static bool is_turing(uint16_t chipset) { return chipset >= 0x160 && chipset < 0x170; }

static bool in_gpu_reach(const fgpu_mem_t* m, uint64_t bytes) {
    return m->cpu && m->in_vram && m->vram_offset + bytes <= nv_rm_vram_mapped(g.a);
}

// A color target the 3D engine can render to: in VRAM, address and pitch
// multiples of 128 bytes.
static bool color_ok(const fgpu_hw_target_t* t, nv3d_color_t* c) {
    uint32_t pitch = t->cpu.color_pitch * 4u;
    if (!t->cpu.color || !in_gpu_reach(&t->color, (uint64_t)pitch * t->cpu.height)) return false;
    if ((t->color.vram_offset & 127u) || (pitch & 127u) || !t->cpu.width || !t->cpu.height) return false;
    *c = (nv3d_color_t){ nv_rm_vram_va(t->color.vram_offset), pitch, t->cpu.width, t->cpu.height };
    return true;
}

// Takes the GPU for a 3D job; other cores may hold it for a moment.
static nv_push_t* begin_job(void) {
    for (uint32_t tries = 0; tries < 2000000u; tries++) {
        nv_push_t* p = nv_rm_3d_begin(g.a);
        if (p) {
            if (!g.init_sent) {
                nv3d_push_init(p, nv_rm_vram_va(g.pools + POOL_TIC), nv_rm_vram_va(g.pools + POOL_TSC),
                               nv_rm_vram_va(g.pools + POOL_ZERO));
            }
            return p;
        }
        if (!nv_rm_3d_ready(g.a)) return NULL;
        __asm__ volatile("pause");
    }
    return NULL;
}

static bool end_job(nv_push_t* p) {
    if (p->overflow) {
        nv_rm_3d_cancel();
        return false;
    }
    if (!nv_rm_3d_submit()) {
        g.hw3d = false;
        serial_write_str("nv: a 3D job failed; rendering goes back to the CPU\n");
        return false;
    }
    g.init_sent = true;
    return true;
}

// The GPU depth buffer behind an FGPU depth surface; made (and cleared
// to the far plane) the first time it is needed.
static const nv3d_zeta_t* zeta_for(const fgpu_hw_target_t* t) {
    if (!t->cpu.depth) return NULL;
    int free_slot = -1;
    for (uint32_t i = 0; i < MAX_ZETA; i++) {
        if (g.zeta[i].cpu == t->depth.cpu) return &g.zeta[i].z;
        if (!g.zeta[i].cpu && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot < 0) return NULL;
    nv3d_zeta_t z;
    nv3d_zeta_size(t->cpu.width, t->cpu.height, &z);
    uint64_t off = gpu_vram_alloc(g.a, z.bytes, 4096, 0);
    if (off == GPU_VRAM_INVALID) return NULL;
    if (off + z.bytes > nv_rm_vram_mapped(g.a)) {
        gpu_vram_free(g.a, off);
        return NULL;
    }
    z.va = nv_rm_vram_va(off);
    nv_push_t* p = begin_job();
    if (!p) { gpu_vram_free(g.a, off); return NULL; }
    nv3d_push_clear(p, NULL, &z, FGPU_CLEAR_DEPTH, 0, 1.0f);
    if (!end_job(p)) { gpu_vram_free(g.a, off); return NULL; }
    g.zeta[free_slot].cpu = t->depth.cpu;
    g.zeta[free_slot].vram = off;
    g.zeta[free_slot].z = z;
    return &g.zeta[free_slot].z;
}

static void nv_surface_destroy(const fgpu_mem_t* mem) {
    for (uint32_t i = 0; i < MAX_ZETA; i++) {
        if (!g.zeta[i].cpu || g.zeta[i].cpu != mem->cpu) continue;
        gpu_vram_free(g.a, g.zeta[i].vram);
        g.zeta[i].cpu = NULL;
    }
}

static bool nv_read(const fgpu_mem_t* mem, uint32_t row_bytes, uint32_t rows, uint32_t src_pitch,
                    void* dst, uint32_t dst_pitch) {
    if (!g.a || !mem->in_vram) return false;
    return nv_rm_ce_read(g.a, mem->vram_offset, row_bytes, rows, src_pitch, dst, dst_pitch);
}

/* ------------------------------------------------------ clears and draws */

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Clears of surfaces in VRAM: on the 3D engine when it is up (color and
// the GPU depth buffer together), else on the copy engine. Synchronous,
// so finish() has nothing to wait for.
static bool nv_clear(const fgpu_hw_target_t* t, uint32_t flags, uint32_t color, float depth) {
    bool want_color = (flags & FGPU_CLEAR_COLOR) && t->cpu.color;
    bool want_depth = (flags & FGPU_CLEAR_DEPTH) && t->cpu.depth;
    if (!want_color && !want_depth) return false;
    nv3d_color_t c;
    if (g.hw3d && color_ok(t, &c)) {
        const nv3d_zeta_t* z = want_depth ? zeta_for(t) : NULL;
        if (!g.hw3d) return false;
        if (want_color || z) {
            nv_push_t* p = begin_job();
            if (!p) return false;
            nv3d_push_clear(p, &c, z, flags, color, depth);
            if (!end_job(p)) return false;
        }
        // The CPU's depth buffer follows the clears, for draws that fall back.
        if (want_depth) fgpu_raster_clear(&t->cpu, FGPU_CLEAR_DEPTH, 0, depth);
        g.clears++;
        return true;
    }
    if ((want_color && !t->color.in_vram) || (want_depth && !t->depth.in_vram)) return false;
    if (want_color && !nv_rm_ce_fill(g.a, t->color.vram_offset, t->cpu.width, t->cpu.height,
                                     t->cpu.color_pitch * 4u, color | 0xFF000000u))
        return false;
    if (want_depth && !nv_rm_ce_fill(g.a, t->depth.vram_offset, t->cpu.width, t->cpu.height,
                                     t->cpu.depth_pitch * 4u, float_bits(depth))) {
        // The color part is done; the CPU redoing it is harmless.
        return false;
    }
    return true;
}

static bool nv_draw(const fgpu_hw_draw_t* d, uint32_t* triangles) {
    if (!g.hw3d) return false;
    const fgpu_pipeline_t* pipe = d->pipe;
    nv3d_draw_t nd;
    memset(&nd, 0, sizeof(nd));
    if (!color_ok(d->target, &nd.color)) return false;

    const nv_program_t* np = pipe->shader ? (const nv_program_t*)d->program : builtin_program(pipe->program);
    if (!np || !np->gpu) return false;

    uint64_t stride = (uint64_t)d->layout->stride * 4u;
    uint64_t vb_bytes = (uint64_t)d->vertex_count * stride;
    if (!in_gpu_reach(&d->vertices, d->vertex_offset + vb_bytes) || !vb_bytes) return false;
    uint32_t count = d->count - d->count % 3u;
    if (d->indices.cpu && !in_gpu_reach(&d->indices, d->index_offset + (uint64_t)count * 4u)) return false;

    // Textures the program samples: a header and a sampler per unit.
    uint32_t used = np->vs.textures | np->fs.textures;
    for (uint32_t u = 0; u < FGPU_MAX_TEXTURE_UNITS; u++) {
        if (!(used & (1u << u))) continue;
        const fgpu_texture_t* tex = &pipe->textures[u];
        const fgpu_mem_t* mem = &d->textures[u];
        uint32_t pitch = tex->pitch * 4u;
        if (!tex->pixels || !tex->width || !tex->height ||
            !in_gpu_reach(mem, (uint64_t)pitch * tex->height) || (mem->vram_offset & 31u) || (pitch & 31u) ||
            mem->cpu == d->target->color.cpu)       // sampling the target it draws to
            return false;
        uint32_t words[8];
        nv3d_texture_header(words, nv_rm_vram_va(mem->vram_offset), pitch, tex->width, tex->height);
        vram_write(g.pools + POOL_TIC + (u + 1u) * 32u, words, 32);
        nv3d_sampler(words, tex->flags);
        vram_write(g.pools + POOL_TSC + (u + 1u) * 32u, words, 32);
        nd.tex_handle[u] = (u + 1u) | ((u + 1u) << 20);
        nd.textures_changed = true;
    }

    bool depth = d->target->cpu.depth && (pipe->state & (FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE));
    if (depth) {
        nd.zeta = zeta_for(d->target);
        if (!nd.zeta) return false;
    }
    if (count == 0) { *triangles = 0; return true; }

    nd.vp_x = pipe->vp_x; nd.vp_y = pipe->vp_y; nd.vp_w = pipe->vp_w; nd.vp_h = pipe->vp_h;
    nd.state = pipe->state;
    nd.front_ccw = g.front_ccw;
    nd.vs = (nv3d_stage_t){ nv_rm_vram_va(np->vs.vram), np->vs.regs };
    nd.fs = (nv3d_stage_t){ nv_rm_vram_va(np->fs.vram), np->fs.regs };
    nd.cb_va = nv_rm_vram_va(g.pools + POOL_CB);
    nd.uniforms = pipe->shader ? (const float*)pipe->constants : builtin_uniforms(np, &pipe->u);
    nd.stride_bytes = (uint32_t)stride;
    nd.attrib_count = d->layout->count;
    nd.attrib = d->layout->attrib;
    nd.vb_va = nv_rm_vram_va(d->vertices.vram_offset + d->vertex_offset);
    nd.vb_bytes = vb_bytes;
    if (d->indices.cpu) {
        nd.indexed = true;
        nd.ib_va = nv_rm_vram_va(d->indices.vram_offset + d->index_offset);
        nd.ib_bytes = (uint64_t)count * 4u;
    }
    nd.count = count;

    nv_push_t* p = begin_job();
    if (!p) return false;
    nv3d_push_draw(p, &nd);
    if (!end_job(p)) return false;
    *triangles = count / 3u;
    g.draws++;
    return true;
}

static void nv_finish(void) {}

/* ------------------------------------------------------------ self-test */

static void say(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    nv_rm_say(line);
}

static bool near_color(uint32_t got, uint32_t want) {
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((got >> s) & 255u) - (int)((want >> s) & 255u);
        if (d < -3 || d > 3) return false;
    }
    return true;
}

// Step 4b: two triangles into a 64x64 surface of our own. The first
// (blue-ish, near) must cover the centre and leave the corner alone; the
// second (red, behind it, larger) must lose the depth test at the centre
// and win outside the first. Also finds which winding the hardware calls
// front. Returns true if the 3D engine draws what FGPU expects.
static bool self_test(void) {
    enum { N = 64, PITCH = N * 4 };
    say("\nStep 4b: FarcadeGL draws on the 3D engine\n");
    nv_program_t* np = builtin_program(FGPU_PROGRAM_COLOR);
    if (!np) { say("  the built-in shader did not compile to machine code\n"); return false; }
    say("  shaders: vertex and pixel stage compiled (%u + %u registers)\n", np->vs.regs, np->fs.regs);

    uint64_t surf = gpu_vram_alloc(g.a, PITCH * N, 4096, 0);
    uint64_t vbo = gpu_vram_alloc(g.a, 4096, 4096, 0);
    nv3d_zeta_t z;
    nv3d_zeta_size(N, N, &z);
    uint64_t zoff = gpu_vram_alloc(g.a, z.bytes, 4096, 0);
    bool ok = false;
    if (surf == GPU_VRAM_INVALID || vbo == GPU_VRAM_INVALID || zoff == GPU_VRAM_INVALID ||
        zoff + z.bytes > nv_rm_vram_mapped(g.a) || surf + PITCH * N > nv_rm_vram_mapped(g.a)) {
        say("  out of VRAM for the test\n");
        goto out;
    }
    z.va = nv_rm_vram_va(zoff);
    // Counter-clockwise (front) triangles, P3N3C4: near and blue-ish
    // around the centre, then far and red over the left half.
    static const float verts[6][10] = {
        { -0.5f, -0.5f, 0.0f, 0, 0, 1, 0.25f, 0.5f, 0.75f, 1 },
        {  0.5f, -0.5f, 0.0f, 0, 0, 1, 0.25f, 0.5f, 0.75f, 1 },
        {  0.0f,  0.6f, 0.0f, 0, 0, 1, 0.25f, 0.5f, 0.75f, 1 },
        { -0.9f, -0.9f, 0.5f, 0, 0, 1, 1.0f, 0.0f, 0.0f, 1 },
        {  0.3f, -0.9f, 0.5f, 0, 0, 1, 1.0f, 0.0f, 0.0f, 1 },
        { -0.9f,  0.9f, 0.5f, 0, 0, 1, 1.0f, 0.0f, 0.0f, 1 },
    };
    vram_write(vbo, verts, sizeof(verts));
    fgpu_uniforms_t u;
    memset(&u, 0, sizeof(u));
    u.mvp[0] = u.mvp[5] = u.mvp[10] = u.mvp[15] = 1.0f;
    u.model[0] = u.model[5] = u.model[10] = u.model[15] = 1.0f;
    u.normal[0] = u.normal[5] = u.normal[10] = u.normal[15] = 1.0f;
    static const uint32_t attrib[3] = { FGPU_ATTRIB(0, 3), FGPU_ATTRIB(3, 3), FGPU_ATTRIB(6, 4) };
    nv3d_color_t color = { nv_rm_vram_va(surf), PITCH, N, N };

    for (int pass = 0; pass < 2 && !ok; pass++) {
        nv_push_t* p = begin_job();
        if (!p) { say("  the 3D channel is not available\n"); goto out; }
        nv3d_push_clear(p, &color, &z, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x00102030u, 1.0f);
        nv3d_draw_t d = {
            .color = color, .zeta = &z, .vp_w = N, .vp_h = N,
            .state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE | FGPU_STATE_CULL_BACK,
            .front_ccw = pass == 1,
            .vs = { nv_rm_vram_va(np->vs.vram), np->vs.regs }, .fs = { nv_rm_vram_va(np->fs.vram), np->fs.regs },
            .cb_va = nv_rm_vram_va(g.pools + POOL_CB), .uniforms = builtin_uniforms(np, &u),
            .stride_bytes = 40, .attrib_count = 3, .attrib = attrib,
            .vb_va = nv_rm_vram_va(vbo), .vb_bytes = sizeof(verts), .count = 6,
        };
        nv3d_push_draw(p, &d);
        if (!end_job(p)) { say("  the test draw did not finish (see the lines above)\n"); goto out; }
        uint32_t centre = vram_read32(surf + 36u * PITCH + 32u * 4u);      // inside both triangles
        uint32_t left = vram_read32(surf + 50u * PITCH + 8u * 4u);         // only the red one
        uint32_t corner = vram_read32(surf + 2u * PITCH + 61u * 4u);       // neither
        say("  %s front: centre 0x%08x, left 0x%08x, corner 0x%08x\n", pass ? "counter-clockwise" : "clockwise",
            centre, left, corner);
        if (near_color(centre, 0x4080BFu) && near_color(left, 0xFF0000u) && near_color(corner, 0x102030u)) {
            g.front_ccw = pass == 1;
            ok = true;
        } else if (near_color(centre, 0xFF0000u)) {
            say("  the far triangle won at the centre: the depth test is not working as expected\n");
        } else if (pass == 1) {
            say("  expected 0x..4080bf, 0x..ff0000, 0x..102030\n");
        }
    }
    if (ok) say("  ok: triangles, shaders, depth test and culling work on the 3D engine.\n");
out:
    if (surf != GPU_VRAM_INVALID) gpu_vram_free(g.a, surf);
    if (vbo != GPU_VRAM_INVALID) gpu_vram_free(g.a, vbo);
    if (zoff != GPU_VRAM_INVALID) gpu_vram_free(g.a, zoff);
    return ok;
}

static bool nv_init(gpu_adapter_t* a, const char** why) {
    if (!a || a->vendor != GPU_VENDOR_NVIDIA || !a->mmio) { *why = "not an NVIDIA adapter"; return false; }
    if (!is_turing(nv_chipset(a))) { *why = "hardware rendering is planned for Turing first"; return false; }
    if (!nv_gsp_rm_running(a)) { *why = "GSP-RM is not running (see 'gpu nvidia')"; return false; }
    if (!nv_rm_chan_ready(a)) { *why = "GSP-RM runs, but the copy engine channel is not up (step 3b)"; return false; }
    g.a = a;
    if (nv_rm_3d_ready(a) && !g.hw3d) {
        if (!g.pools) {
            uint64_t off = gpu_vram_alloc(a, POOL_BYTES, 4096, 0);
            if (off != GPU_VRAM_INVALID && off + POOL_BYTES <= nv_rm_vram_mapped(a)) {
                for (uint32_t i = 0; i < POOL_BYTES; i += 4) *(volatile uint32_t*)(a->aperture + off + i) = 0;
                g.pools = off;
            }
        }
        if (g.pools) {
            g.hw3d = true;              // so the test's own jobs run
            g.hw3d = self_test();
        }
        if (!g.hw3d) say("  FarcadeGL stays on the CPU; clears use the copy engine.\n");
    }
    if (!nv_rm_3d_ready(a)) g.hw3d = false;
    fgpu_backend_nvidia.name = g.hw3d ? "nvidia 3D engine" : "nvidia copy engine";
    fgpu_backend_nvidia.caps = g.hw3d ? FGPU_CAP_HW_3D : 0;
    if (g.hw3d) a->caps |= GPU_CAP_HW_3D;
    else a->caps &= ~GPU_CAP_HW_3D;
    return true;
}

fgpu_backend_t fgpu_backend_nvidia = {
    .name = "nvidia copy engine",
    .caps = 0,
    .init = nv_init,
    .program_create = nv_program_create,
    .program_destroy = nv_program_destroy,
    .clear = nv_clear,
    .draw = nv_draw,
    .finish = nv_finish,
    .read = nv_read,
    .surface_destroy = nv_surface_destroy,
};

/* ------------------------------------------------------- bring-up report */

static bool fs_read(void* ctx, uint64_t offset, void* buf, uint32_t len) {
    minimafs_file_handle_t* h = (minimafs_file_handle_t*)ctx;
    if (offset > 0xFFFFFFFFull || !minimafs_seek(h, (uint32_t)offset)) return false;
    return minimafs_read(h, buf, len) == len;
}

// Checks one firmware file; returns true if it is usable.
static bool check_file(const char* chip, const nv_fw_entry_t* e, char* result, size_t result_size) {
    char path[160];
    snprintf(path, sizeof(path), NV_FW_DIR "%s/%s", chip, e->file);
    minimafs_file_handle_t* h = minimafs_open(path, true);
    if (!h) {
        snprintf(result, result_size, "missing");
        return false;
    }
    nv_fw_file_t f = { fs_read, h, minimafs_size(h) };
    const char* why = "";
    bool ok = true;
    if (e->kind == NV_FW_KIND_BIN) {
        nv_fw_bin_hdr_t hdr;
        ok = nv_fw_parse_bin(&f, &hdr, &why);
        if (ok) snprintf(result, result_size, "ok, %u bytes of ucode", (unsigned)hdr.data_size);
    } else {
        uint64_t size = 0;
        for (int i = 0; ok && i < 2 && e->sections[i]; i++) {
            ok = nv_fw_elf_section(&f, e->sections[i], NULL, i == 0 ? &size : NULL, &why);
            if (!ok) snprintf(result, result_size, "bad: %s %s", e->sections[i], why);
        }
        if (ok) snprintf(result, result_size, "ok, image %llu KiB", (unsigned long long)(size >> 10));
        why = NULL;
    }
    if (!ok && why) snprintf(result, result_size, "bad: %s", why);
    minimafs_close(h);
    return ok;
}

// Reads the VBIOS through the PROM window, a 32-bit register at a time.
bool nv_prom_read(void* ctx, uint64_t offset, void* buf, uint32_t len) {
    gpu_adapter_t* a = (gpu_adapter_t*)ctx;
    if (offset + len > NV_VBIOS_PROM_SIZE) return false;
    uint8_t* out = (uint8_t*)buf;
    uint32_t at = (uint32_t)offset;
    while (len) {
        uint32_t word = nv_rd32(a, NV_VBIOS_PROM + (at & ~3u));
        uint32_t skip = at & 3u, n = 4u - skip;
        if (n > len) n = len;
        for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)(word >> (8u * (skip + i)));
        out += n;
        at += n;
        len -= n;
    }
    return true;
}

// Step 2, first part: find FWSEC in the VBIOS. Only reads, apart from
// turning ROM shadowing off while it reads (as nouveau does).
static void report_vbios(gpu_adapter_t* a, void (*print)(const char* text)) {
    char line[160];
    print("\nStep 2a VBIOS: FWSEC (sets up the protected memory GSP boots in)\n");
    uint32_t shadow = nv_rd32(a, NV_PCI_ROM_SHADOW);
    if (nv_pri_error(shadow)) {
        print("  The ROM shadow register does not answer.\n");
        return;
    }
    nv_wr32(a, NV_PCI_ROM_SHADOW, shadow & ~1u);
    nv_fw_file_t rom = { nv_prom_read, a, NV_VBIOS_PROM_SIZE };
    nv_vbios_info_t v;
    const char* why = "";
    bool ok = nv_vbios_parse(&rom, &v, &why);
    nv_wr32(a, NV_PCI_ROM_SHADOW, shadow);

    for (uint32_t i = 0; i < v.image_count; i++) {
        snprintf(line, sizeof(line), "  image %u at 0x%05x, %3u KiB, type 0x%02x %s\n", (unsigned)i,
                 (unsigned)v.images[i].base, (unsigned)(v.images[i].size >> 10),
                 (unsigned)v.images[i].type, nv_vbios_image_name(v.images[i].type));
        print(line);
    }
    if (v.bit_offset) {
        snprintf(line, sizeof(line), "  BIT table at 0x%05x", (unsigned)v.bit_offset);
        print(line);
        if (v.pmu_table) {
            snprintf(line, sizeof(line), ", falcon ucode table at 0x%05x (%u entries)",
                     (unsigned)v.pmu_table, (unsigned)v.pmu_entries);
            print(line);
        }
        print("\n");
    }
    if (v.fwsec_desc && v.desc_version == 2) {
        snprintf(line, sizeof(line),
                 "  FWSEC at 0x%05x: descriptor v2, IMEM %u (secure %u) + DMEM %u bytes\n",
                 (unsigned)v.fwsec_desc, (unsigned)v.imem_load, (unsigned)v.imem_sec_size,
                 (unsigned)v.dmem_load);
        print(line);
    } else if (v.fwsec_desc) {
        snprintf(line, sizeof(line),
                 "  FWSEC at 0x%05x: descriptor v%u, %u signature(s), IMEM %u + DMEM %u bytes\n",
                 (unsigned)v.fwsec_desc, (unsigned)v.desc_version, (unsigned)v.signature_count,
                 (unsigned)v.imem_load, (unsigned)v.dmem_load);
        print(line);
    }
    if (v.mapper_offset) {
        snprintf(line, sizeof(line),
                 "  DMEM mapper: command buffer at DMEM 0x%x, init command 0x%x, mask 0x%08x\n",
                 (unsigned)v.mapper_cmd_in, (unsigned)v.mapper_init_cmd,
                 (unsigned)v.mapper_cmd_mask0);
        print(line);
    }
    snprintf(line, sizeof(line), "  %s%s\n", ok ? "ok: FWSEC can be patched with the FRTS command"
                                                : "not usable: ", ok ? "" : why);
    print(line);
    serial_write_str(ok ? "nvidia: FWSEC found in the VBIOS\n" : "nvidia: VBIOS: ");
    if (!ok) { serial_write_str(why); serial_write_str("\n"); }
}

// `text` padded with spaces to `width` (the kernel's snprintf has no %-Ns).
static const char* padded(const char* text, uint32_t width, char* buf, size_t size) {
    size_t n = 0;
    while (text[n] && n + 1 < size) { buf[n] = text[n]; n++; }
    while (n < width && n + 1 < size) buf[n++] = ' ';
    buf[n] = 0;
    return buf;
}

void nv_bringup_report(gpu_adapter_t* a, void (*print)(const char* text)) {
    char pad[48];
    char line[160];
    uint16_t chip = nv_chipset(a);
    snprintf(line, sizeof(line), "%s, %s (chipset 0x%x)\n", a->name, a->family, (unsigned)chip);
    print(line);
    if (!chip) {
        print("  The registers (BAR0) do not answer; nothing else to check.\n");
        return;
    }

    const char* name = nv_chip_name(chip);
    uint32_t count = 0;
    const nv_fw_entry_t* files = nv_fw_manifest(chip, &count);
    print("\nStep 1  firmware files\n");
    uint32_t good = 0;
    if (!files) {
        print("  No firmware list for this chip yet (Turing comes first).\n");
    } else {
        snprintf(line, sizeof(line), "  From linux-firmware nvidia/%s/, copied to " NV_FW_DIR "%s/\n",
                 name, name);
        print(line);
        for (uint32_t i = 0; i < count; i++) {
            char result[96];
            if (check_file(name, &files[i], result, sizeof(result))) good++;
            snprintf(line, sizeof(line), "  %s %s\n", padded(files[i].file, 36, pad, sizeof(pad)), result);
            print(line);
        }
        snprintf(line, sizeof(line), "  %u of %u present and well-formed\n", (unsigned)good,
                 (unsigned)count);
        print(line);
    }

    print("\nMicrocontrollers (read only)\n");
    const nv_falcon_t* fl = nv_falcons(chip);
    for (; fl && fl->name; fl++) {
        nv_falcon_state_t st;
        if (!nv_falcon_read_state(a, fl, &st)) {
            snprintf(line, sizeof(line), "  %s 0x%06x  not reachable (powered off?)\n",
                     padded(fl->name, 6, pad, sizeof(pad)), (unsigned)fl->base);
        } else {
            char state[12];
            snprintf(line, sizeof(line), "  %s 0x%06x  IMEM %3u KiB  DMEM %3u KiB  %s mailbox 0x%x\n",
                     padded(fl->name, 6, pad, sizeof(pad)), (unsigned)fl->base,
                     (unsigned)(st.imem_bytes >> 10), (unsigned)(st.dmem_bytes >> 10),
                     padded(st.halted ? "halted" : (st.stopped ? "stopped" : "running"), 8,
                            state, sizeof(state)),
                     (unsigned)st.mailbox0);
        }
        print(line);
    }

    if (is_turing(chip)) report_vbios(a, print);
    uint64_t wlo, whi;
    if (is_turing(chip) && nv_gsp_rm_running(a)) {
        print("\nStep 2b GSP-RM running (FWSEC-FRTS, booter_load, GSP_INIT_DONE)\n");
    } else if (is_turing(chip) && nv_gsp_wpr2(a, &wlo, &whi)) {
        snprintf(line, sizeof(line),
                 "\nStep 2b FWSEC-FRTS done (WPR2 0x%llx - 0x%llx); run 'gpu nvidia boot' for GSP-RM\n",
                 (unsigned long long)wlo, (unsigned long long)whi);
        print(line);
    } else {
        print("\nStep 2b GSP boot: run 'gpu nvidia boot' (log in 0:/gsp_boot.log)\n");
    }
    if (nv_rm_chan_ready(a))
        print("Step 3  RPC, copy engine channel and fills work (clears of VRAM surfaces on the GPU)\n");
    else if (nv_gsp_rm_rpc_ok(a))
        print("Step 3a RPC to GSP-RM works; 3b copy engine channel not up (run 'gpu nvidia boot')\n");
    else
        print("Step 3  RPC, channel + TURING_A, first clear   waits for step 2b\n");
    if (g.hw3d) {
        snprintf(line, sizeof(line), "Step 4  FarcadeGL draws run on the 3D engine (%u draws, %u clears so far)\n",
                 (unsigned)g.draws, (unsigned)g.clears);
        print(line);
    } else if (nv_rm_3d_ready(a)) {
        print("Step 4  3D channel is up, but the draw self-test failed (run 'gpu nvidia boot' to see it)\n");
    } else {
        print("Step 4  draws on the 3D engine: wait for the 3D channel (step 4a, 'gpu nvidia boot')\n");
    }
    const char* why = "";
    const fgpu_backend_t* hw = fgpu_hw_backend(&why);
    snprintf(line, sizeof(line), "\nRendering: %s%s%s\n", hw ? hw->name : "FGPU CPU backend",
             hw ? "" : " - ", hw ? "" : why);
    print(line);
}
