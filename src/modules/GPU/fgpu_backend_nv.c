#include "x86_64/fgpu_backend.h"
#include "x86_64/fgpu_ir.h"
#include "x86_64/fgpu_abi.h"
#include "x86_64/nv_gpu.h"
#include "x86_64/nv_sass.h"
#include "x86_64/nv_3d.h"
#include "x86_64/nv_nvdec.h"
#include "x86_64/nv_builtin_fgsl.h"
#include "x86_64/minimafs.h"
#include "x86_64/allocator.h"
#include "serial.h"
#include "string.h"
#include "fglsl.h"
#include <stdarg.h>
#include "x86_64/clock.h"

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
 * the hardware has no pitch-linear depth.
 *
 * The hardware also refuses a pitch-linear color target next to a depth
 * buffer (DATA_ERROR RT_LINEAR_WITH_ZETA, reported by GSP-RM as a
 * "graphics class error", Xid 69). So a color surface that is drawn with
 * depth gets a block-linear copy of its own ("tiled"), as NVK does for
 * linear images: clears and draws go to that copy, and the copy engine
 * brings it back to the FGPU surface when the CPU needs the pixels
 * (finish, read), when a draw samples the surface as a texture, or when
 * a draw falls back to the CPU. Draws without depth on a surface that has
 * no such copy still go straight to the surface. So a frame's depth is on the
 * GPU; a draw that has to fall back to the CPU (a shader the encoder
 * refuses, memory outside VRAM) uses the CPU depth buffer, which only
 * knows the clears.
 */

#define IR_CAPACITY 16384u
#define MAX_ZETA 32u
#define MAX_TILED 32u

typedef struct {
    uint64_t vram;              // header + code, GPU_VRAM_INVALID if none
    uint32_t regs;
    uint32_t textures;
    uint32_t buffers;           // storage buffer bindings read
} nv_stage_t;

typedef struct {
    nv_stage_t vs, fs;
    uint32_t uniform_words;     // words of uniforms the program reads (pushed with each draw)
    bool gpu;                   // both stages are on the card
    int32_t builtin_reg[8];     // built-in programs: uniform registers (-1 = unused)
} nv_program_t;

static struct {
    gpu_adapter_t* a;
    bool hw3d;                  // the self-test passed
    bool tex_tested, tex_ok;    // step 4c: texture sampling on the card (draws that sample go to the CPU if not ok)
    uint32_t tex_wrong;         // pixels the sampling test got wrong (of 4096)
    uint32_t tex_first_x, tex_first_y, tex_got, tex_want;
    // Step 4c, second part: sampling a draw target's block-linear copy directly. When it passes, a pass
    // that samples what an earlier pass drew reads that copy instead of copying it back to the surface.
    bool tex_bl_tested, tex_bl_ok;
    uint32_t tex_bl_wrong, bl_samples;
    bool init_sent;             // nv3d_push_init reached the channel
    bool front_ccw;
    uint64_t pools;             // VRAM: texture headers, samplers, constants, a zero page
    nv_program_t* builtin[4];   // by FGPU_PROGRAM_*
    bool builtin_tried[4];
    struct {
        const void* cpu;        // the FGPU depth surface it shadows
        uint64_t vram;
        nv3d_zeta_t z;
        // The FGPU depth surface (float32, pitch-linear): kept in step with the GPU buffer, so draws that
        // fall back to the CPU test against what the card drew and the card against what the CPU drew.
        bool in_vram;
        uint64_t linear;
        uint32_t pitch, width, height;
        bool gpu_newer;         // the card wrote depth the surface does not have yet
        bool stale;             // the surface may hold depth the card does not have (load before use)
    } zeta[MAX_ZETA];
    struct {
        const void* cpu;        // the FGPU color surface it stands in for, NULL = free
        uint64_t linear;        // that surface: VRAM offset, pitch in bytes, size
        uint32_t pitch, width, height;
        uint64_t vram;          // the block-linear copy
        uint32_t bl_width, bl_height, bytes;
        bool valid;             // holds the surface's current pixels
        bool dirty;             // newer than the surface: the GPU drew into it
    } tiled[MAX_TILED];
    float uniforms[FGPU_SHADER_MAX_UNIFORMS * 4];
    uint32_t draws, clears, resolves, uploads;
    // For the 'gpu' report: why draws went back to the CPU, and where the
    // time of the GPU path goes (jobs wait for the engine; copies are the
    // block-linear loads and resolves).
    uint32_t refused[8];
    uint64_t jobs, job_ns, copy_ns;
    uint32_t depth_loads, depth_stores;     // depth copied between the GPU buffers and the surfaces
} g;

static bool batch_flush(void);
static bool flush_wait(void);
static uint32_t g_compile_failures;         // shaders kept on the CPU ('gpu perf')
static const char* g_last_failure;

enum { R_TARGET, R_PROGRAM, R_BUFFERS, R_TEXTURE, R_STORAGE, R_DEPTH, R_TILED, R_JOB };
static const char* const k_refused[8] = {
    "target not in VRAM", "shader not on the card", "vertex/index buffer not in VRAM",
    "texture not usable", "storage buffer not usable", "no depth buffer", "no block-linear copy", "job failed",
};
#define REFUSE(r) do { g.refused[r]++; return false; } while (0)

#define POOL_TIC   0u
#define POOL_TSC   (NV3D_POOL_ENTRIES * 32u)
#define POOL_CB    (2u * NV3D_POOL_ENTRIES * 32u)
#define POOL_ZERO  (POOL_CB + 4096u)    // POOL_CB..: two constant buffers (draws, video)
#define POOL_BYTES (POOL_ZERO + 4096u)
// Texture header / sampler entries: 1..15 for the synchronous jobs (self-tests, video, H.264), 16.. for
// the draws of a batch, each draw its own (a later draw must not change what an earlier one samples).
#define TEX_FIRST_DRAW 16u

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
                out->buffers = sh.buffers;
                ok = true;
            } else if (off != GPU_VRAM_INVALID) {
                gpu_vram_free(g.a, off);
            }
        }
    }
    if (!ok) {
        g_compile_failures++;
        g_last_failure = why;
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
    if (g.a && g.hw3d) flush_wait();        // queued batches may still run its code
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
    uint32_t uc = p->h.uniform_count;
    np->uniform_words = (uc > FGPU_SHADER_MAX_UNIFORMS ? FGPU_SHADER_MAX_UNIFORMS : uc) * 4u;
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
    *c = (nv3d_color_t){ nv_rm_vram_va(t->color.vram_offset), pitch, t->cpu.width, t->cpu.height, false, 0, 0 };
    return true;
}

static bool end_job(nv_push_t* p);
static nv_push_t* job_begin_raw(void);

/* Batches. Draws and clears are recorded here and submitted together (one job, one wait) when the batch is
 * full, before anything else touches what they draw (copy engine copies, CPU access: finish(), reads,
 * surface frees) and before any synchronous job. One draw used to be one job and one wait: ~0.3-0.5 ms
 * each on the RTX 2060, so a frame of a few hundred draws spent most of its time waiting. */
#define BATCH_MAX_WORDS (12u * 1024u)
static uint32_t g_batch_buf[BATCH_MAX_WORDS];
static nv_push_t g_batch;
static bool g_batch_ready;
static uint32_t g_batch_draws, g_tic_next = TEX_FIRST_DRAW;
static uint32_t g_batches, g_barriers;

/* Texture headers and samplers stay in the pool across batches (a "bindless" table: draws name an entry by
 * its index through a constant, nothing is bound per draw). An entry is kept for a texture as long as it is
 * used, so most draws write no header at all; one is reused for another texture only when the batches that
 * read it are done (asynchronous batches may still run). */
#define TIC_CACHE (NV3D_POOL_ENTRIES - TEX_FIRST_DRAW)
static struct {
    uint64_t va;
    uint32_t pitch, width, height, kind, flags;
    uint32_t fence;             // the newest submitted batch that reads it (0 = none)
    uint32_t epoch;             // the batch being recorded reads it: g_batch_epoch
    uint64_t lru;
    bool used;
} g_tic[TIC_CACHE];
static uint32_t g_batch_epoch = 1;
static uint64_t g_tic_clock;
static uint32_t g_tic_written, g_tic_hits;

// Fence of the newest asynchronous batch (0 = none, or it went the synchronous way).
static uint32_t g_fence;
static uint32_t g_async_batches, g_waits;
static uint64_t g_wait_ns, g_depth_cpu_ns;

// Surfaces the 3D engine drew or cleared since the last barrier (or wait): a draw that samples one needs a
// barrier first. More than fit: every sampling draw gets one until the next.
#define WRITTEN_MAX 16u
static const void* g_written[WRITTEN_MAX];
static uint32_t g_written_count;
static bool g_written_all;

static void written_add(const void* cpu) {
    if (!cpu) return;
    for (uint32_t i = 0; i < g_written_count; i++) if (g_written[i] == cpu) return;
    if (g_written_count < WRITTEN_MAX) g_written[g_written_count++] = cpu;
    else g_written_all = true;
}

static bool written_since_barrier(const void* cpu) {
    if (g_written_all) return true;
    for (uint32_t i = 0; i < g_written_count; i++) if (g_written[i] == cpu) return true;
    return false;
}

// Buffers and textures the batches not yet waited for read: the CPU may overwrite anything else at once
// (mem_busy). More than fit: everything counts as read until the next wait.
#define READ_MAX 64u
static const void* g_read[READ_MAX];
static uint32_t g_read_count;
static bool g_read_all;

static void read_add(const void* cpu) {
    if (!cpu || g_read_all) return;
    for (uint32_t i = 0; i < g_read_count; i++) if (g_read[i] == cpu) return;
    if (g_read_count < READ_MAX) g_read[g_read_count++] = cpu;
    else g_read_all = true;
}

static bool gpu_wait(void) {
    g_written_count = 0;                    // done: whatever was drawn has landed
    g_written_all = false;
    g_read_count = 0;
    g_read_all = false;
    if (!g_fence) return true;
    uint64_t t0 = 0;
    if (!nv_rm_3d_done(g_fence)) { g_waits++; t0 = clock_monotonic_ns(); }
    bool done = nv_rm_3d_wait(g_fence);
    if (t0) g_wait_ns += clock_monotonic_ns() - t0;
    if (!done) {
        g.hw3d = false;
        serial_write_str("nv: a 3D batch did not finish; rendering goes back to the CPU\n");
        return false;
    }
    g_fence = 0;
    return true;
}

// The batch submitted and done: before the copy engine or the CPU touches what it draws or reads.
static bool flush_wait(void) { return batch_flush() && gpu_wait(); }

static bool batch_flush(void) {
    g_tic_next = TEX_FIRST_DRAW;
    if (!g_batch_ready || !g_batch.count) return true;
    uint32_t n = g_batch.count, draws = g_batch_draws;
    bool overflow = g_batch.overflow;
    g_batch.count = 0;
    g_batch.overflow = false;
    g_batch_draws = 0;
    if (overflow) { serial_write_str("nv: a batch overflowed; its draws are lost\n"); return false; }
    // Asynchronous: queued, and the CPU goes on recording the next batch. The first job sets the channel up
    // (nv3d_push_init), synchronously.
    if (g.init_sent) {
        uint32_t fence = nv_rm_3d_submit_async(g.a, g_batch_buf, n);
        if (fence) {
            g_fence = fence;
            for (uint32_t i = 0; i < TIC_CACHE; i++)
                if (g_tic[i].used && g_tic[i].epoch == g_batch_epoch) g_tic[i].fence = fence;
            g_batch_epoch++;
            g_batches++;
            g_async_batches++;
            g.jobs++;
            return true;
        }
        if (!nv_rm_3d_ready(g.a)) { g.hw3d = false; return false; }
    }
    if (!gpu_wait()) return false;
    nv_push_t* p = job_begin_raw();
    if (!p) return false;
    if (p->capacity - p->count < n) {
        nv_rm_3d_cancel();
        serial_write_str("nv: a batch did not fit the pushbuffer\n");
        return false;
    }
    memcpy(p->words + p->count, g_batch_buf, (size_t)n * 4u);
    p->count += n;
    g_batches++;
    if (!end_job(p)) {
        char line[96];
        snprintf(line, sizeof(line), "nv: a batch of %u draws did not finish\n", (unsigned)draws);
        nv_rm_say(line);
        return false;
    }
    for (uint32_t i = 0; i < TIC_CACHE; i++)
        if (g_tic[i].used && g_tic[i].epoch == g_batch_epoch) g_tic[i].fence = 0;     // done already
    g_batch_epoch++;
    return true;
}

// The pool entry for a texture (pitch-linear or a target's block-linear copy) with these sampler flags,
// written if it is not there yet (*wrote). -1: every entry is read by the batch being recorded (flush it).
static int tic_entry(uint64_t va, uint32_t pitch, uint32_t width, uint32_t height, uint32_t kind,
                     uint32_t flags, bool* wrote) {
    int victim = -1;
    for (uint32_t i = 0; i < TIC_CACHE; i++) {
        if (g_tic[i].used && g_tic[i].va == va && g_tic[i].pitch == pitch && g_tic[i].width == width &&
            g_tic[i].height == height && g_tic[i].kind == kind && g_tic[i].flags == flags) {
            g_tic[i].epoch = g_batch_epoch;
            g_tic[i].lru = ++g_tic_clock;
            g_tic_hits++;
            return (int)(i + TEX_FIRST_DRAW);
        }
        if (!g_tic[i].used) { if (victim < 0 || g_tic[victim].used) victim = (int)i; }
        else if (g_tic[i].epoch != g_batch_epoch && (victim < 0 || (g_tic[victim].used && g_tic[i].lru < g_tic[victim].lru)))
            victim = (int)i;
    }
    if (victim < 0) return -1;
    // A batch still running may read the old entry.
    if (g_tic[victim].used && g_tic[victim].fence && !nv_rm_3d_done(g_tic[victim].fence)) {
        g_waits++;
        if (!nv_rm_3d_wait(g_tic[victim].fence)) { g.hw3d = false; return -1; }
    }
    uint32_t slot = (uint32_t)victim + TEX_FIRST_DRAW;
    uint32_t words[8];
    nv3d_texture_header(words, va, pitch, width, height, (int)kind, 0);
    vram_write(g.pools + POOL_TIC + slot * 32u, words, 32);
    nv3d_sampler(words, flags);
    vram_write(g.pools + POOL_TSC + slot * 32u, words, 32);
    g_tic[victim] = (typeof(g_tic[0])){ va, pitch, width, height, kind, flags, 0, g_batch_epoch, ++g_tic_clock, true };
    g_tic_written++;
    *wrote = true;
    return (int)slot;
}

// Room for `words` more in the batch (flushing it first if needed); NULL if the GPU cannot take it.
static nv_push_t* batch_room(uint32_t words) {
    if (!g_batch_ready) {
        uint32_t cap = nv_rm_3d_push_capacity();
        cap = cap > 1024u ? cap - 1024u : cap / 2u;          // the first job also carries the init methods
        if (cap > BATCH_MAX_WORDS) cap = BATCH_MAX_WORDS;
        nv_push_init(&g_batch, g_batch_buf, cap);
        g_batch_ready = true;
    }
    if (g_batch.capacity - g_batch.count < words && !batch_flush()) return NULL;
    if (g_batch.capacity - g_batch.count < words) return NULL;
    return &g_batch;
}

// A synchronous job (self-tests, video, zeta setup): the batch goes first.
static nv_push_t* begin_job(void) {
    if (!batch_flush()) return NULL;
    return job_begin_raw();
}

// Takes the GPU for a 3D job; other cores may hold it for a moment.
static nv_push_t* job_begin_raw(void) {
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
    uint64_t t0 = clock_monotonic_ns();
    bool ok = nv_rm_3d_submit();
    g.job_ns += clock_monotonic_ns() - t0;
    g.jobs++;
    if (!ok) {
        g.hw3d = false;
        serial_write_str("nv: a 3D job failed; rendering goes back to the CPU\n");
        return false;
    }
    g.init_sent = true;
    return true;
}

// The GPU depth buffer behind an FGPU depth surface; made (and cleared
// to the far plane) the first time it is needed.
// The surface's depth into the GPU buffer, when the CPU may have changed it (block-linear, GOBs like the
// color copies: D32F is 4 bytes a pixel).
static bool zeta_load(uint32_t i) {
    if (!g.zeta[i].stale) return true;
    if (!flush_wait()) return false;
    uint64_t t0 = clock_monotonic_ns();
    // A depth surface in system memory goes through the staging pages, one in VRAM directly.
    bool ok = g.zeta[i].in_vram
        ? nv_rm_ce_copy_surface(g.a, g.zeta[i].linear, g.zeta[i].pitch, g.zeta[i].vram, 0, g.zeta[i].z.width * 4u,
                                g.zeta[i].z.height, g.zeta[i].width * 4u, g.zeta[i].height)
        : nv_rm_ce_cpu_to_bl(g.a, g.zeta[i].vram, g.zeta[i].z.width * 4u, g.zeta[i].z.height, g.zeta[i].width * 4u,
                             g.zeta[i].height, g.zeta[i].cpu, g.zeta[i].pitch);
    g.copy_ns += clock_monotonic_ns() - t0;
    if (ok) { g.zeta[i].stale = false; g.depth_loads++; }
    return ok;
}

// The GPU buffer's depth back into the surface (the CPU is about to read or draw).
static void zeta_store(uint32_t i) {
    if (!g.zeta[i].gpu_newer) return;
    if (!flush_wait()) return;
    uint64_t t0 = clock_monotonic_ns();
    bool ok = g.zeta[i].in_vram
        ? nv_rm_ce_copy_surface(g.a, g.zeta[i].vram, 0, g.zeta[i].linear, g.zeta[i].pitch, g.zeta[i].z.width * 4u,
                                g.zeta[i].z.height, g.zeta[i].width * 4u, g.zeta[i].height)
        : nv_rm_ce_bl_to_cpu(g.a, g.zeta[i].vram, g.zeta[i].z.width * 4u, g.zeta[i].z.height, g.zeta[i].width * 4u,
                             g.zeta[i].height, (void*)g.zeta[i].cpu, g.zeta[i].pitch);
    if (ok) {
        g.zeta[i].gpu_newer = false;
        g.depth_stores++;
    }
    g.copy_ns += clock_monotonic_ns() - t0;
}

static int zeta_index(const nv3d_zeta_t* z) {
    for (uint32_t i = 0; i < MAX_ZETA; i++) if (g.zeta[i].cpu && &g.zeta[i].z == z) return (int)i;
    return -1;
}

// `overwrite`: the caller replaces all of it (a whole clear), so the surface's depth need not be loaded.
static const nv3d_zeta_t* zeta_for_ex(const fgpu_hw_target_t* t, bool overwrite) {
    if (!t->cpu.depth) return NULL;
    int free_slot = -1;
    for (uint32_t i = 0; i < MAX_ZETA; i++) {
        if (g.zeta[i].cpu == t->depth.cpu) {
            if (overwrite) g.zeta[i].stale = false;
            else if (!zeta_load(i)) return NULL;
            return &g.zeta[i].z;
        }
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
    g.zeta[free_slot].in_vram = t->depth.in_vram;
    g.zeta[free_slot].linear = t->depth.vram_offset;
    g.zeta[free_slot].pitch = t->cpu.depth_pitch * 4u;
    g.zeta[free_slot].width = t->cpu.width;
    g.zeta[free_slot].height = t->cpu.height;
    g.zeta[free_slot].gpu_newer = false;
    // A new buffer starts from what the surface holds (the CPU's clears and draws so far).
    g.zeta[free_slot].stale = !overwrite;
    if (!zeta_load((uint32_t)free_slot)) { g.zeta[free_slot].cpu = NULL; gpu_vram_free(g.a, off); return NULL; }
    return &g.zeta[free_slot].z;
}

static const nv3d_zeta_t* zeta_for(const fgpu_hw_target_t* t) { return zeta_for_ex(t, false); }

/* Block-linear color copies (see the top of the file). */

static int tiled_find(const void* cpu) {
    if (!cpu) return -1;
    for (uint32_t i = 0; i < MAX_TILED; i++)
        if (g.tiled[i].cpu == cpu) return (int)i;
    return -1;
}

static void tiled_drop(int i) {
    if (i < 0 || !g.tiled[i].cpu) return;
    gpu_vram_free(g.a, g.tiled[i].vram);
    g.tiled[i].cpu = NULL;
}

// The block-linear copy back to the FGPU surface, if it is newer.
static bool tiled_resolve(int i) {
    if (i < 0 || !g.tiled[i].dirty) return true;
    if (!flush_wait()) return false;
    uint64_t t0 = clock_monotonic_ns();
    bool ok = nv_rm_ce_copy_surface(g.a, g.tiled[i].vram, 0, g.tiled[i].linear, g.tiled[i].pitch,
                               g.tiled[i].bl_width * 4u, g.tiled[i].bl_height, g.tiled[i].width * 4u,
                               g.tiled[i].height);
    g.copy_ns += clock_monotonic_ns() - t0;
    if (!ok) return false;
    g.tiled[i].dirty = false;
    g.resolves++;
    return true;
}

// The CPU may write the surface next (a draw or clear it does instead of
// the GPU): bring the pixels back and forget the copy's contents.
static void tiled_release(const void* cpu) {
    int i = tiled_find(cpu);
    if (i < 0) return;
    if (!tiled_resolve(i)) { tiled_drop(i); return; }
    g.tiled[i].valid = false;
}

// The block-linear copy of a target's color surface, made the first time
// and loaded with the surface's pixels unless the caller overwrites all
// of them anyway (a clear). -1 if it cannot be had.
static int tiled_for(const fgpu_hw_target_t* t, bool overwrite) {
    uint32_t pitch = t->cpu.color_pitch * 4u;
    int i = tiled_find(t->color.cpu);
    if (i >= 0 && (g.tiled[i].linear != t->color.vram_offset || g.tiled[i].pitch != pitch ||
                   g.tiled[i].width != t->cpu.width || g.tiled[i].height != t->cpu.height)) {
        tiled_resolve(i);
        tiled_drop(i);
        i = -1;
    }
    if (i < 0) {
        for (uint32_t k = 0; k < MAX_TILED && i < 0; k++)
            if (!g.tiled[k].cpu) i = (int)k;
        if (i < 0) return -1;
        uint32_t bw, bh, bytes;
        nv3d_color_bl_size(t->cpu.width, t->cpu.height, &bw, &bh, &bytes);
        uint64_t off = gpu_vram_alloc(g.a, bytes, 4096, 0);
        if (off == GPU_VRAM_INVALID) return -1;
        if (off + bytes > nv_rm_vram_mapped(g.a)) { gpu_vram_free(g.a, off); return -1; }
        g.tiled[i].cpu = t->color.cpu;
        g.tiled[i].linear = t->color.vram_offset;
        g.tiled[i].pitch = pitch;
        g.tiled[i].width = t->cpu.width;
        g.tiled[i].height = t->cpu.height;
        g.tiled[i].vram = off;
        g.tiled[i].bl_width = bw;
        g.tiled[i].bl_height = bh;
        g.tiled[i].bytes = bytes;
        g.tiled[i].valid = g.tiled[i].dirty = false;
    }
    if (!g.tiled[i].valid && !overwrite) {
        if (!flush_wait()) return -1;
        uint64_t t0 = clock_monotonic_ns();
        bool ok = nv_rm_ce_copy_surface(g.a, g.tiled[i].linear, g.tiled[i].pitch, g.tiled[i].vram, 0,
                                        g.tiled[i].bl_width * 4u, g.tiled[i].bl_height, g.tiled[i].width * 4u,
                                        g.tiled[i].height);
        g.copy_ns += clock_monotonic_ns() - t0;
        if (!ok) return -1;
        g.tiled[i].valid = true;
        g.uploads++;
    }
    return i;
}

static void tiled_color(int i, nv3d_color_t* c) {
    *c = (nv3d_color_t){ nv_rm_vram_va(g.tiled[i].vram), 0, g.tiled[i].width, g.tiled[i].height,
                         true, g.tiled[i].bl_width, g.tiled[i].bl_height };
}

static void nv_surface_destroy(const fgpu_mem_t* mem) {
    flush_wait();                          // pending draws may use it
    for (uint32_t i = 0; i < MAX_ZETA; i++) {
        if (!g.zeta[i].cpu || g.zeta[i].cpu != mem->cpu) continue;
        gpu_vram_free(g.a, g.zeta[i].vram);
        g.zeta[i].cpu = NULL;
    }
    tiled_drop(tiled_find(mem->cpu));
}

static bool nv_read(const fgpu_mem_t* mem, uint32_t row_bytes, uint32_t rows, uint32_t src_pitch,
                    void* dst, uint32_t dst_pitch) {
    if (!g.a || !mem->in_vram) return false;
    if (!flush_wait()) return false;
    if (!tiled_resolve(tiled_find(mem->cpu))) return false;
    return nv_rm_ce_read(g.a, mem->vram_offset, row_bytes, rows, src_pitch, dst, dst_pitch);
}

/* ------------------------------------------------------ clears and draws */

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// The FGPU scissor rectangle clamped to a width x height target:
// pixels x0 <= x < x1, y0 <= y < y1. False when nothing is left.
static bool clamp_scissor(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t width, uint32_t height,
                          uint32_t* x0, uint32_t* y0, uint32_t* x1, uint32_t* y1) {
    int64_t a = x < 0 ? 0 : x, b = y < 0 ? 0 : y;
    int64_t c = (int64_t)x + (w < 0 ? 0 : w), d = (int64_t)y + (h < 0 ? 0 : h);
    if (c > (int64_t)width) c = width;
    if (d > (int64_t)height) d = height;
    if (c <= a || d <= b) return false;
    *x0 = (uint32_t)a; *y0 = (uint32_t)b; *x1 = (uint32_t)c; *y1 = (uint32_t)d;
    return true;
}

// Clears of surfaces in VRAM: on the 3D engine when it is up (color and
// the GPU depth buffer together), else on the copy engine. Synchronous,
// so finish() has nothing to wait for. With the scissor on, only the
// rectangle (x0, y0)-(x1, y1) is cleared.
static bool nv_clear_3d(const fgpu_hw_target_t* t, uint32_t flags, uint32_t color, float depth,
                        bool want_color, bool want_depth, bool rect, uint32_t x0, uint32_t y0, uint32_t x1,
                        uint32_t y1) {
    nv3d_color_t c;
    if (!color_ok(t, &c)) return false;
    bool whole_area = !rect || (x0 == 0 && y0 == 0 && x1 == t->cpu.width && y1 == t->cpu.height);
    const nv3d_zeta_t* z = want_depth ? zeta_for_ex(t, whole_area) : NULL;
    if (!g.hw3d || (want_depth && !z)) return false;
    int ti = tiled_find(t->color.cpu);
    // Color cleared together with depth, or a surface whose pixels live in
    // its block-linear copy: clear that copy (a pitch-linear target cannot
    // sit next to a depth buffer). Depth alone binds no color target. A
    // partial clear keeps the rest of the copy, so it must hold the pixels.
    bool whole = !rect || (x0 == 0 && y0 == 0 && x1 == t->cpu.width && y1 == t->cpu.height);
    if (want_color && (z || ti >= 0)) {
        ti = tiled_for(t, whole);
        if (ti < 0) return false;
        tiled_color(ti, &c);
    }
    nv_push_t* p = batch_room(256u);
    if (!p) return false;
    uint32_t mark = p->count;
    if (rect) nv3d_push_clear_rect(p, want_color ? &c : NULL, z, flags, color, depth, x0, y0, x1, y1);
    else nv3d_push_clear(p, want_color ? &c : NULL, z, flags, color, depth);
    if (p->overflow) { p->count = mark; p->overflow = false; return false; }
    if (want_color && c.block_linear) g.tiled[ti].valid = g.tiled[ti].dirty = true;
    if (want_color) written_add(t->color.cpu);
    // The surface's own depth is not cleared: the card's buffer holds the newest depth, and it is copied
    // into the surface only when the CPU needs it (nv_cpu_draw, cpu_access). Clearing it here as well cost
    // a CPU write of the whole buffer per clear (64 MiB for a 4096x4096 shadow map).
    if (want_depth) {
        int zi = zeta_index(z);
        if (zi >= 0) g.zeta[zi].gpu_newer = true;
    }
    g.clears++;
    return true;
}

static bool nv_clear(const fgpu_hw_target_t* t, uint32_t flags, uint32_t color, float depth) {
    bool want_color = (flags & FGPU_CLEAR_COLOR) && t->cpu.color;
    bool want_depth = (flags & FGPU_CLEAR_DEPTH) && t->cpu.depth;
    if (!want_color && !want_depth) return false;
    uint32_t x0 = 0, y0 = 0, x1 = t->cpu.width, y1 = t->cpu.height;
    bool rect = t->scissor;
    if (rect && !clamp_scissor(t->sc_x, t->sc_y, t->sc_w, t->sc_h, t->cpu.width, t->cpu.height, &x0, &y0, &x1, &y1))
        return true;                            // an empty rectangle: nothing to clear
    if (g.hw3d) {
        if (nv_clear_3d(t, flags, color, depth, want_color, want_depth, rect, x0, y0, x1, y1)) return true;
        // The copy engine or the CPU clears instead: the surface itself
        // must hold the pixels then.
        if (want_color) tiled_release(t->color.cpu);
        if (!g.hw3d) return false;
        nv3d_color_t c;
        if (color_ok(t, &c)) return false;      // 3D could have, but did not: the CPU does it
    }
    if (want_color) tiled_release(t->color.cpu);
    if ((want_color && !t->color.in_vram) || (want_depth && !t->depth.in_vram)) return false;
    if (!flush_wait()) return false;
    uint64_t cpitch = (uint64_t)t->cpu.color_pitch * 4u, dpitch = (uint64_t)t->cpu.depth_pitch * 4u;
    if (want_color && !nv_rm_ce_fill(g.a, t->color.vram_offset + y0 * cpitch + x0 * 4u, x1 - x0, y1 - y0,
                                     (uint32_t)cpitch, color | 0xFF000000u))
        return false;
    if (want_depth && !nv_rm_ce_fill(g.a, t->depth.vram_offset + y0 * dpitch + x0 * 4u, x1 - x0, y1 - y0,
                                     (uint32_t)dpitch, float_bits(depth))) {
        // The color part is done; the CPU redoing it is harmless.
        return false;
    }
    return true;
}

static bool nv_draw_3d(const fgpu_hw_draw_t* d, uint32_t* triangles);

// The CPU draws into `t` with its depth buffer next: it gets the card's depth, and the card reloads the
// surface's depth before it draws there again. (Only then: copying depth back at every finish() cost a
// readback per frame.)
static void nv_cpu_draw(const fgpu_hw_target_t* t) {
    if (!g.a || !t->cpu.depth) return;
    for (uint32_t i = 0; i < MAX_ZETA; i++) {
        if (g.zeta[i].cpu != t->depth.cpu) continue;
        zeta_store(i);
        g.zeta[i].stale = true;
    }
}

static bool nv_draw(const fgpu_hw_draw_t* d, uint32_t* triangles) {
    if (!g.hw3d) return false;
    if (nv_draw_3d(d, triangles)) return true;
    // The CPU draws it into the surface itself.
    tiled_release(d->target->color.cpu);
    return false;
}

static bool nv_draw_3d(const fgpu_hw_draw_t* d, uint32_t* triangles) {
    const fgpu_pipeline_t* pipe = d->pipe;
    // Texture units >= FGPU_MAX_TEXTURE_UNITS (none: it is all 16) would run on the CPU backend
    // (fgpu.c does not offer such draws either). Mipmapped textures are sampled at level 0 only: the
    // header describes a pitch-linear surface, which the hardware cannot mip. Drawing
    // them here keeps the frame on the card; going to the CPU for them meant copying the
    // render target back and forth for every such draw.
    for (uint32_t u = FGPU_MAX_TEXTURE_UNITS; u < FGPU_TEXTURE_UNITS; u++)
        if (pipe->textures[u].pixels) return false;
    nv3d_draw_t nd;
    memset(&nd, 0, sizeof(nd));
    if (!color_ok(d->target, &nd.color)) REFUSE(R_TARGET);

    const nv_program_t* np = pipe->shader ? (const nv_program_t*)d->program : builtin_program(pipe->program);
    if (!np || !np->gpu) REFUSE(R_PROGRAM);

    uint64_t stride = (uint64_t)d->layout->stride * 4u;
    uint64_t vb_bytes = (uint64_t)d->vertex_count * stride;
    if (!in_gpu_reach(&d->vertices, d->vertex_offset + vb_bytes) || !vb_bytes) REFUSE(R_BUFFERS);
    uint32_t count = d->count - d->count % 3u;
    if (d->indices.cpu && !in_gpu_reach(&d->indices, d->index_offset + (uint64_t)count * 4u)) REFUSE(R_BUFFERS);

    // Textures the program samples: a header and a sampler per unit.
    uint32_t used = np->vs.textures | np->fs.textures;
    int bl_src[FGPU_MAX_TEXTURE_UNITS];
    bool need_barrier = false;
    if (used && g.tex_tested && !g.tex_ok) REFUSE(R_TEXTURE);    // step 4c failed: the CPU samples correctly
    for (uint32_t u = 0; u < FGPU_MAX_TEXTURE_UNITS; u++) {
        if (!(used & (1u << u))) continue;
        const fgpu_texture_t* tex = &pipe->textures[u];
        const fgpu_mem_t* mem = &d->textures[u];
        uint32_t pitch = tex->pitch * 4u;
        if (!tex->pixels || !tex->width || !tex->height ||
            !in_gpu_reach(mem, (uint64_t)pitch * tex->height) || (mem->vram_offset & 31u) || (pitch & 31u) ||
            mem->cpu == d->target->color.cpu)       // sampling the target it draws to
            REFUSE(R_TEXTURE);
        // A surface drawn earlier through its block-linear copy: sample that
        // copy (step 4c showed the card reads it right), or bring the pixels
        // back first (the texture header reads the surface).
        // Drawn by the 3D engine since the last barrier (this batch or one still running): barrier first.
        if (written_since_barrier(mem->cpu)) need_barrier = true;
        int si = tiled_find(mem->cpu);
        bl_src[u] = -1;
        if (si >= 0 && g.tiled[si].valid && g.tiled[si].dirty && g.tex_bl_ok &&
            g.tiled[si].width == tex->width && g.tiled[si].height == tex->height) {
            // Drawn into by this batch: a barrier in the batch makes those draws land before this one reads.
            bl_src[u] = si;
        } else if (!tiled_resolve(si)) {
            REFUSE(R_TEXTURE);
        }
    }
    // Storage buffers the program reads: constant buffers on their memory.
    uint32_t sbs = np->vs.buffers | np->fs.buffers;
    for (uint32_t b = 0; b < FGPU_MAX_STORAGE_BUFFERS; b++) {
        if (!(sbs & (1u << b))) continue;
        const fgpu_mem_t* mem = &d->storage_buffers[b];
        uint64_t bytes = mem->bytes & ~15ull;
        if (bytes > NV_SASS_CB_STORAGE_BYTES) bytes = NV_SASS_CB_STORAGE_BYTES;
        if (!bytes || !in_gpu_reach(mem, bytes) || (mem->vram_offset & 255u)) REFUSE(R_STORAGE);
        nd.sb_mask |= 1u << b;
        nd.sb_va[b] = nv_rm_vram_va(mem->vram_offset);
        nd.sb_bytes[b] = (uint32_t)bytes;
    }
    bool depth = d->target->cpu.depth && (pipe->state & (FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE));
    if (depth) {
        nd.zeta = zeta_for(d->target);
        if (!nd.zeta) REFUSE(R_DEPTH);
    }
    if (count == 0) { *triangles = 0; return true; }
    if (pipe->scissor) {
        nd.scissor = true;
        if (!clamp_scissor(pipe->sc_x, pipe->sc_y, pipe->sc_w, pipe->sc_h, d->target->cpu.width,
                           d->target->cpu.height, &nd.sc_x0, &nd.sc_y0, &nd.sc_x1, &nd.sc_y1)) {
            *triangles = 0;                     // everything is cut away
            return true;
        }
    }
    // With depth, or when the surface's pixels already live there: draw
    // into the block-linear copy.
    int ti = tiled_find(d->target->color.cpu);
    if (depth || ti >= 0) {
        ti = tiled_for(d->target, false);
        if (ti < 0) REFUSE(R_TILED);
        tiled_color(ti, &nd.color);
    }

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

    nd.uniform_words = np->uniform_words;

    // Recorded into the batch. Nothing after the room check may flush it: the texture entries taken
    // below belong to this batch.
    if (!batch_room(640u + nd.uniform_words)) REFUSE(R_JOB);
    // Pool entries for the textures (kept across draws and batches: see tic_entry).
    if (used) {
        bool wrote = false;
        for (int attempt = 0; attempt < 2; attempt++) {
            bool full = false;
            for (uint32_t u = 0; u < FGPU_MAX_TEXTURE_UNITS && !full; u++) {
                if (!(used & (1u << u))) continue;
                const fgpu_texture_t* tex = &pipe->textures[u];
                const fgpu_mem_t* mem = &d->textures[u];
                int slot = bl_src[u] >= 0
                    ? tic_entry(nv_rm_vram_va(g.tiled[bl_src[u]].vram), 0, tex->width, tex->height, NV3D_TEX_ARGB_BL,
                                tex->flags, &wrote)
                    : tic_entry(nv_rm_vram_va(mem->vram_offset), tex->pitch * 4u, tex->width, tex->height, NV3D_TEX_ARGB,
                                tex->flags, &wrote);
                if (slot < 0) { full = true; break; }
                if (bl_src[u] >= 0) g.bl_samples++;
                nd.tex_handle[u] = (uint32_t)slot | ((uint32_t)slot << 20);
            }
            if (!full) break;
            // Every entry is read by this batch: submit it and start a new one.
            if (attempt || !g.hw3d || !batch_flush() || !batch_room(640u + nd.uniform_words)) REFUSE(R_JOB);
        }
        nd.textures_changed = wrote;
    }
    if (need_barrier) {
        nv3d_push_barrier(&g_batch);
        g_written_count = 0;
        g_written_all = false;
        g_barriers++;
    }
    uint32_t mark = g_batch.count;
    nv3d_push_draw(&g_batch, &nd);
    if (g_batch.overflow) {                 // did not fit after all: take it back out, the CPU draws it
        g_batch.count = mark;
        g_batch.overflow = false;
        REFUSE(R_JOB);
    }
    g_batch_draws++;
    read_add(d->vertices.cpu);
    read_add(d->indices.cpu);
    for (uint32_t b = 0; b < FGPU_MAX_STORAGE_BUFFERS; b++) if (nd.sb_mask & (1u << b)) read_add(d->storage_buffers[b].cpu);
    for (uint32_t u = 0; u < FGPU_MAX_TEXTURE_UNITS; u++) if (used & (1u << u)) read_add(d->textures[u].cpu);
    if (ti >= 0) g.tiled[ti].dirty = true;
    written_add(d->target->color.cpu);
    if (nd.zeta && (pipe->state & FGPU_STATE_DEPTH_WRITE)) {
        int zi = zeta_index(nd.zeta);
        if (zi >= 0) g.zeta[zi].gpu_newer = true;
    }
    *triangles = count / 3u;
    g.draws++;
    return true;
}

// Jobs are synchronous; what remains is bringing the block-linear copies
// back to their surfaces before the CPU reads or writes them. The CPU may
// change a surface after this, so a copy is reloaded before its next use
// (unless a clear overwrites it).
static void nv_finish(void) {
    flush_wait();
    for (uint32_t i = 0; i < MAX_TILED; i++) {
        if (!g.tiled[i].cpu) continue;
        if (!tiled_resolve((int)i)) { tiled_drop((int)i); continue; }
        g.tiled[i].valid = false;
    }
}

static void nv_wait_idle(void) { flush_wait(); }
static void nv_kick(void) { if (g.hw3d) batch_flush(); }

// May queued or running card work read `mem`? (False: the CPU can overwrite it without waiting.)
static bool nv_mem_busy(const fgpu_mem_t* mem) {
    if (!g.a || !mem->cpu) return false;
    if (g_read_all || g_written_all) return true;
    for (uint32_t i = 0; i < g_read_count; i++) if (g_read[i] == mem->cpu) return true;
    for (uint32_t i = 0; i < g_written_count; i++) if (g_written[i] == mem->cpu) return true;
    return false;
}

static void nv_perf(fgpu_backend_perf_t* o) {
    memset(o, 0, sizeof(*o));
    o->wait_ns = g_wait_ns; o->waits = g_waits;
    o->copy_ns = g.copy_ns; o->depth_cpu_ns = g_depth_cpu_ns;
    o->batches = g_batches; o->draws = g.draws; o->clears = g.clears; o->barriers = g_barriers;
    o->resolves = g.resolves; o->uploads = g.uploads; o->depth_loads = g.depth_loads; o->depth_stores = g.depth_stores;
    for (int r = 0; r < 8; r++) { o->refused[r] = g.refused[r]; o->refused_names[r] = k_refused[r]; }
    o->compile_failures = g_compile_failures;
    o->last_failure = g_last_failure;
}

// One surface for the CPU: the frame it presents or reads back, pixels it writes. The other block-linear
// copies stay valid: before, every present and buffer upload resolved all of them (the shadow map, the
// HDR and post targets...) and the next frame's first draw into each loaded it again.
static void nv_cpu_access(const fgpu_mem_t* mem, bool write) {
    if (!g.a || !mem->cpu) return;
    flush_wait();
    int i = tiled_find(mem->cpu);
    if (i >= 0) {
        if (!tiled_resolve(i)) tiled_drop(i);
        else if (write) g.tiled[i].valid = false;
    }
    for (uint32_t z = 0; z < MAX_ZETA; z++) {
        if (g.zeta[z].cpu != mem->cpu) continue;
        zeta_store(z);
        if (write) g.zeta[z].stale = true;
    }
}

/* ---------------------------------------------------------------- video */

/* The motion codec's colour conversion as a draw: the three planes become
 * one-byte textures, a shader (NV_VIDEO_FS) does mmc_to_argb's arithmetic
 * per pixel, and the copy engine brings the picture back to system
 * memory. Entropy decoding, prediction and deblocking stay on the CPU:
 * they are sequential by design (docs/media.md). */

#define TEX_FIRST_VIDEO 5u          // texture header / sampler entries 5, 6, 7
#define POOL_CB_VIDEO   (POOL_CB + NV_SASS_CB_BYTES)

static struct {
    volatile int busy;
    nv_program_t* program;
    bool tried;
    int32_t reg_inv, reg_cmax;
    uint32_t unit[3];               // texture units of the y, cb, cr samplers
    uint32_t w, h, cw, ch, width, height;       // what the memory below is for
    uint64_t plane[3], target, vbo;             // VRAM, GPU_VRAM_INVALID = none
    uint32_t pitch[3], target_pitch;
    float uniforms[FGPU_SHADER_MAX_UNIFORMS * 4];
    uint32_t frames;
} v = { .plane = { GPU_VRAM_INVALID, GPU_VRAM_INVALID, GPU_VRAM_INVALID },
        .target = GPU_VRAM_INVALID, .vbo = GPU_VRAM_INVALID };

static void video_free(void) {
    uint64_t* all[5] = { &v.plane[0], &v.plane[1], &v.plane[2], &v.target, &v.vbo };
    for (int i = 0; i < 5; i++) {
        if (*all[i] != GPU_VRAM_INVALID) gpu_vram_free(g.a, *all[i]);
        *all[i] = GPU_VRAM_INVALID;
    }
    v.w = 0;
}

static bool video_program(void) {
    if (v.tried) return v.program != NULL;
    v.tried = true;
    fglsl_program_t* c = (fglsl_program_t*)alloc_unzeroed(sizeof(fglsl_program_t));
    if (!c) return false;
    fgpu_program_t prog;
    const char* why = "";
    if (fglsl_compile(c, NV_VIDEO_VS, NV_VIDEO_FS) == 0 && fgpu_program_check(c->words, c->size, &prog, &why))
        v.program = (nv_program_t*)nv_program_create(&prog);
    if (v.program) {
        static const char* const names[3] = { "ty", "tu", "tv" };
        int inv = fglsl_find_uniform(c, "u_inv"), cmax = fglsl_find_uniform(c, "u_cmax");
        v.reg_inv = inv >= 0 ? (int32_t)c->uniforms[inv].reg : -1;
        v.reg_cmax = cmax >= 0 ? (int32_t)c->uniforms[cmax].reg : -1;
        bool ok = v.reg_inv >= 0 && v.reg_cmax >= 0;
        for (int i = 0; i < 3; i++) {
            int u = fglsl_find_uniform(c, names[i]);
            if (u < 0 || c->uniforms[u].reg >= FGPU_MAX_TEXTURE_UNITS) ok = false;
            else v.unit[i] = c->uniforms[u].reg;
        }
        if (!ok) { nv_program_destroy(v.program); v.program = NULL; }
    }
    free_mem(c);
    return v.program != NULL;
}

static bool video_memory(uint32_t w, uint32_t h, uint32_t cw, uint32_t ch, uint32_t width, uint32_t height) {
    if (v.w == w && v.h == h && v.cw == cw && v.ch == ch && v.width == width && v.height == height) return true;
    video_free();
    uint64_t limit = nv_rm_vram_mapped(g.a);
    for (int p = 0; p < 3; p++) {
        v.pitch[p] = ((p ? cw : w) + 31u) & ~31u;
        uint64_t bytes = (uint64_t)v.pitch[p] * (p ? ch : h);
        v.plane[p] = gpu_vram_alloc(g.a, bytes, 4096, 0);
        if (v.plane[p] == GPU_VRAM_INVALID || v.plane[p] + bytes > limit) { video_free(); return false; }
    }
    v.target_pitch = ((width + 31u) & ~31u) * 4u;
    v.target = gpu_vram_alloc(g.a, (uint64_t)v.target_pitch * height, 4096, 0);
    v.vbo = gpu_vram_alloc(g.a, 4096, 4096, 0);
    if (v.target == GPU_VRAM_INVALID || v.vbo == GPU_VRAM_INVALID ||
        v.target + (uint64_t)v.target_pitch * height > limit || v.vbo + 4096u > limit) {
        video_free();
        return false;
    }
    // One triangle over the whole picture: clip position, pixel position.
    float fw = (float)width, fh = (float)height;
    float tri[3][4] = { { -1, 1, 0, 0 }, { -1, -3, 0, 2 * fh }, { 3, 1, 2 * fw, 0 } };
    vram_write(v.vbo, tri, sizeof(tri));
    v.w = w; v.h = h; v.cw = cw; v.ch = ch; v.width = width; v.height = height;
    return true;
}

static bool nv_video_convert(const uint8_t* y, const uint8_t* cb, const uint8_t* cr, uint32_t w, uint32_t h,
                             uint32_t cw, uint32_t ch, uint32_t* out, uint32_t width, uint32_t height) {
    if (!g.hw3d || !width || !height || width > w || height > h || (width + 1u) / 2u > cw ||
        (height + 1u) / 2u > ch || w > 8192u || h > 8192u)
        return false;
    if (__atomic_exchange_n(&v.busy, 1, __ATOMIC_ACQUIRE)) return false;
    bool ok = false;
    if (!video_program() || !video_memory(w, h, cw, ch, width, height)) goto out;

    const uint8_t* src[3] = { y, cb, cr };
    nv3d_draw_t d;
    memset(&d, 0, sizeof(d));
    for (int p = 0; p < 3; p++) {
        uint32_t pw = p ? cw : w, ph = p ? ch : h;
        for (uint32_t row = 0; row < ph; row++)
            memcpy((void*)(g.a->aperture + v.plane[p] + (uint64_t)row * v.pitch[p]), src[p] + (size_t)row * pw, pw);
        uint32_t words[8], slot = TEX_FIRST_VIDEO + (uint32_t)p;
        nv3d_texture_header(words, nv_rm_vram_va(v.plane[p]), v.pitch[p], pw, ph, NV3D_TEX_R8, 0);
        vram_write(g.pools + POOL_TIC + slot * 32u, words, 32);
        nv3d_sampler(words, FGPU_TEXTURE_CLAMP);                // nearest texel
        vram_write(g.pools + POOL_TSC + slot * 32u, words, 32);
        d.tex_handle[v.unit[p]] = slot | (slot << 20);
    }
    memset(v.uniforms, 0, sizeof(v.uniforms));
    float inv[4] = { 1.0f / (float)w, 1.0f / (float)h, 1.0f / (float)cw, 1.0f / (float)ch };
    memcpy(&v.uniforms[v.reg_inv * 4], inv, sizeof(inv));
    v.uniforms[v.reg_cmax * 4] = (float)(cw - 1u);
    v.uniforms[v.reg_cmax * 4 + 1] = (float)(ch - 1u);

    static const uint32_t attrib[2] = { FGPU_ATTRIB(0, 2), FGPU_ATTRIB(2, 2) };
    d.color = (nv3d_color_t){ nv_rm_vram_va(v.target), v.target_pitch, width, height, false, 0, 0 };
    d.vp_w = (int32_t)width;
    d.vp_h = (int32_t)height;
    d.front_ccw = g.front_ccw;
    d.vs = (nv3d_stage_t){ nv_rm_vram_va(v.program->vs.vram), v.program->vs.regs };
    d.fs = (nv3d_stage_t){ nv_rm_vram_va(v.program->fs.vram), v.program->fs.regs };
    d.cb_va = nv_rm_vram_va(g.pools + POOL_CB_VIDEO);
    d.uniforms = v.uniforms;
    d.textures_changed = true;
    d.stride_bytes = 16;
    d.attrib_count = 2;
    d.attrib = attrib;
    d.vb_va = nv_rm_vram_va(v.vbo);
    d.vb_bytes = 48;
    d.count = 3;
    nv_push_t* p = begin_job();
    if (!p) goto out;
    nv3d_push_draw(p, &d);
    if (!end_job(p)) goto out;
    ok = nv_rm_ce_read(g.a, v.target, width * 4u, height, v.target_pitch, out, width * 4u);
    if (ok) v.frames++;
out:
    __atomic_store_n(&v.busy, 0, __ATOMIC_RELEASE);
    return ok;
}

/* ---------------------------------------------------- H.264 on NVDEC */

/* A whole H.264 picture on the card: NVDEC decodes the slices into an
 * NV12 picture in VRAM, the 3D engine converts that to RGB (the same
 * arithmetic as above, NV_VIDEO_NV12_FS), the copy engine brings it back.
 * The caller (mosmedia.h) parsed the headers and keeps the list of
 * reference pictures; it also decodes the first pictures of a file on the
 * CPU and compares, so a decoder that is wrong here is switched off
 * (nv_h264_mismatch) before anyone sees its pictures. */

#define TEX_H264_LUMA   8u
#define TEX_H264_CHROMA 9u

static struct {
    volatile int busy;
    bool init, bad;
    nv_program_t* program;
    bool tried;
    int32_t reg_inv, reg_cmax;
    uint32_t unit[2];
    uint32_t mb_w, mb_h, max_refs, width, height;   // what the memory below is for
    nvdec_layout_t l;
    uint64_t luma[SYS_MEDIA_H264_SURFACES], chroma[SYS_MEDIA_H264_SURFACES];
    uint64_t coloc, mbhist, history, pages, stream, target, vbo, cb;
    uint32_t stream_cap, target_pitch;
    float uniforms[FGPU_SHADER_MAX_UNIFORMS * 4];
    uint32_t pictures;
} hd;

#define H264_PAGE_SETUP  0u
#define H264_PAGE_SLICES 4096u
#define H264_PAGE_STATUS 8192u
#define H264_PAGES       12288u

static void h264_release(uint64_t* off) {
    if (*off != GPU_VRAM_INVALID) gpu_vram_free(g.a, *off);
    *off = GPU_VRAM_INVALID;
}

static void h264_free_all(void) {
    for (uint32_t i = 0; i < SYS_MEDIA_H264_SURFACES; i++) {
        if (hd.init) { h264_release(&hd.luma[i]); h264_release(&hd.chroma[i]); }
        hd.luma[i] = hd.chroma[i] = GPU_VRAM_INVALID;
    }
    uint64_t* all[8] = { &hd.coloc, &hd.mbhist, &hd.history, &hd.pages, &hd.stream, &hd.target, &hd.vbo, &hd.cb };
    for (int i = 0; i < 8; i++) {
        if (hd.init) h264_release(all[i]);
        *all[i] = GPU_VRAM_INVALID;
    }
    hd.init = true;
    hd.mb_w = 0;
    hd.stream_cap = 0;
}

static bool h264_alloc(uint64_t* off, uint64_t bytes) {
    if (*off != GPU_VRAM_INVALID) return true;
    uint64_t o = gpu_vram_alloc(g.a, bytes, 4096, 0);
    if (o == GPU_VRAM_INVALID) return false;
    if (o + bytes > nv_rm_vram_mapped(g.a)) { gpu_vram_free(g.a, o); return false; }
    *off = o;
    return true;
}

static bool h264_program(void) {
    if (hd.tried) return hd.program != NULL;
    hd.tried = true;
    fglsl_program_t* c = (fglsl_program_t*)alloc_unzeroed(sizeof(fglsl_program_t));
    if (!c) return false;
    fgpu_program_t prog;
    const char* why = "";
    if (fglsl_compile(c, NV_VIDEO_VS, NV_VIDEO_NV12_FS) == 0 && fgpu_program_check(c->words, c->size, &prog, &why))
        hd.program = (nv_program_t*)nv_program_create(&prog);
    if (hd.program) {
        int inv = fglsl_find_uniform(c, "u_inv"), cmax = fglsl_find_uniform(c, "u_cmax");
        int ty = fglsl_find_uniform(c, "ty"), tc = fglsl_find_uniform(c, "tc");
        bool ok = inv >= 0 && cmax >= 0 && ty >= 0 && tc >= 0 && c->uniforms[ty].reg < FGPU_MAX_TEXTURE_UNITS &&
                  c->uniforms[tc].reg < FGPU_MAX_TEXTURE_UNITS;
        if (ok) {
            hd.reg_inv = (int32_t)c->uniforms[inv].reg;
            hd.reg_cmax = (int32_t)c->uniforms[cmax].reg;
            hd.unit[0] = c->uniforms[ty].reg;
            hd.unit[1] = c->uniforms[tc].reg;
        } else {
            nv_program_destroy(hd.program);
            hd.program = NULL;
        }
    }
    free_mem(c);
    return hd.program != NULL;
}

// The stream's buffers; picture buffers come as pictures name them.
static bool h264_memory(const syscall_media_h264_t* pic, uint32_t width, uint32_t height) {
    if (!hd.init) h264_free_all();
    if (hd.mb_w != pic->mb_w || hd.mb_h != pic->mb_h || hd.max_refs != pic->max_refs || hd.width != width ||
        hd.height != height) {
        h264_free_all();
        nvdec_layout(pic->mb_w, pic->mb_h, &hd.l);
        uint32_t coloc, mbhist, history;
        nvdec_buffer_sizes(pic->mb_w, pic->mb_h, pic->max_refs, &coloc, &mbhist, &history);
        hd.target_pitch = ((width + 31u) & ~31u) * 4u;
        if (!h264_alloc(&hd.coloc, coloc) || !h264_alloc(&hd.mbhist, mbhist) || !h264_alloc(&hd.history, history) ||
            !h264_alloc(&hd.pages, H264_PAGES) || !h264_alloc(&hd.target, (uint64_t)hd.target_pitch * height) ||
            !h264_alloc(&hd.vbo, 4096) || !h264_alloc(&hd.cb, 4096)) {
            h264_free_all();
            return false;
        }
        float fw = (float)width, fh = (float)height;
        float tri[3][4] = { { -1, 1, 0, 0 }, { -1, -3, 0, 2 * fh }, { 3, 1, 2 * fw, 0 } };
        vram_write(hd.vbo, tri, sizeof(tri));
        hd.mb_w = pic->mb_w; hd.mb_h = pic->mb_h; hd.max_refs = pic->max_refs;
        hd.width = width; hd.height = height;
    }
    uint32_t need = (pic->data_bytes + (uint32_t)sizeof(nvdec_h264_eos) + 0xFFFFFu) & ~0xFFFFFu;
    if (need > hd.stream_cap) {
        h264_release(&hd.stream);
        hd.stream_cap = 0;
        if (!h264_alloc(&hd.stream, need)) return false;
        hd.stream_cap = need;
    }
    return true;
}

static void nv_h264_mismatch(void) {
    hd.bad = true;
    serial_write_str("nv: NVDEC pictures differ from the CPU decoder's; H.264 stays on the CPU\n");
}

static bool nv_h264_decode(const syscall_media_h264_t* pic, uint32_t* out, uint32_t width, uint32_t height) {
    if (!g.hw3d || hd.bad || !nv_rm_dec_ready(g.a)) return false;
    if (!pic->mb_w || !pic->mb_h || pic->mb_w > 256u || pic->mb_h > 256u || !width || !height ||
        width > (uint32_t)pic->mb_w * 16u || height > (uint32_t)pic->mb_h * 16u || pic->max_refs > 16u ||
        pic->cur_index >= SYS_MEDIA_H264_SURFACES || pic->ref_count > 16u || !pic->slice_count ||
        pic->slice_count > SYS_MEDIA_H264_MAX_SLICES || !pic->data_bytes || pic->data_bytes > (32u << 20))
        return false;
    if (__atomic_exchange_n(&hd.busy, 1, __ATOMIC_ACQUIRE)) return false;
    bool ok = false;
    if (!h264_program() || !h264_memory(pic, width, height)) goto out;

    // Picture buffers: the references must be pictures decoded here before.
    nvdec_job_t job;
    memset(&job, 0, sizeof(job));
    for (uint32_t i = 0; i < pic->ref_count; i++) {
        uint32_t idx = pic->refs[i].index;
        if (idx >= SYS_MEDIA_H264_SURFACES || idx == pic->cur_index || hd.luma[idx] == GPU_VRAM_INVALID) goto out;
    }
    if (!h264_alloc(&hd.luma[pic->cur_index], hd.l.luma_bytes) ||
        !h264_alloc(&hd.chroma[pic->cur_index], hd.l.chroma_bytes))
        goto out;
    for (uint32_t i = 0; i < SYS_MEDIA_H264_SURFACES; i++) {
        if (hd.luma[i] == GPU_VRAM_INVALID) continue;
        job.luma[i] = nv_rm_vram_va(hd.luma[i]);
        job.chroma[i] = nv_rm_vram_va(hd.chroma[i]);
    }

    // The slices and the end-of-stream marker, the slice table, the setup.
    memcpy((void*)(g.a->aperture + hd.stream), pic->data, pic->data_bytes);
    memcpy((void*)(g.a->aperture + hd.stream + pic->data_bytes), nvdec_h264_eos, sizeof(nvdec_h264_eos));
    static uint32_t offsets[256];
    memset(offsets, 0, sizeof(offsets));
    for (uint32_t i = 0; i < pic->slice_count; i++) {
        if (pic->slice_offset[i] >= pic->data_bytes) goto out;
        offsets[i] = pic->slice_offset[i];
    }
    offsets[pic->slice_count] = pic->data_bytes;
    vram_write(hd.pages + H264_PAGE_SLICES, offsets, sizeof(offsets));
    static nvdec_h264_pic_s setup;
    static uint32_t setup_words[(sizeof(nvdec_h264_pic_s) + 3) / 4];
    nvdec_h264_setup(&setup, pic, &hd.l, pic->data_bytes);
    memcpy(setup_words, &setup, sizeof(setup));
    vram_write(hd.pages + H264_PAGE_SETUP, setup_words, sizeof(setup_words));

    job.setup = nv_rm_vram_va(hd.pages + H264_PAGE_SETUP);
    job.slice_offsets = nv_rm_vram_va(hd.pages + H264_PAGE_SLICES);
    job.status = nv_rm_vram_va(hd.pages + H264_PAGE_STATUS);
    job.stream = nv_rm_vram_va(hd.stream);
    job.coloc = nv_rm_vram_va(hd.coloc);
    job.history = nv_rm_vram_va(hd.history);
    job.mbhist = nv_rm_vram_va(hd.mbhist);
    job.picture_number = hd.pictures;
    job.max_refs = pic->max_refs;
    nv_push_t* p = NULL;
    for (uint32_t tries = 0; tries < 2000000u && !p && nv_rm_dec_ready(g.a); tries++) {
        p = nv_rm_dec_begin(g.a);
        if (!p) __asm__ volatile("pause");
    }
    if (!p) goto out;
    nvdec_push_h264(p, &job);
    if (!nv_rm_dec_submit()) {
        serial_write_str("nv: a video decoder job failed; H.264 goes back to the CPU\n");
        goto out;
    }
    hd.pictures++;

    // NV12 to RGB on the 3D engine.
    nv3d_draw_t d;
    memset(&d, 0, sizeof(d));
    uint32_t words[8];
    nv3d_texture_header(words, nv_rm_vram_va(hd.luma[pic->cur_index]), hd.l.pitch, hd.l.width, hd.l.height,
                        NV3D_TEX_R8_BL, hd.l.block_log2);
    vram_write(g.pools + POOL_TIC + TEX_H264_LUMA * 32u, words, 32);
    nv3d_texture_header(words, nv_rm_vram_va(hd.chroma[pic->cur_index]), hd.l.pitch, hd.l.width / 2u,
                        hd.l.height / 2u, NV3D_TEX_RG8_BL, hd.l.block_log2);
    vram_write(g.pools + POOL_TIC + TEX_H264_CHROMA * 32u, words, 32);
    nv3d_sampler(words, FGPU_TEXTURE_CLAMP);
    vram_write(g.pools + POOL_TSC + TEX_H264_LUMA * 32u, words, 32);
    d.tex_handle[hd.unit[0]] = TEX_H264_LUMA | (TEX_H264_LUMA << 20);
    d.tex_handle[hd.unit[1]] = TEX_H264_CHROMA | (TEX_H264_LUMA << 20);
    memset(hd.uniforms, 0, sizeof(hd.uniforms));
    float inv[4] = { 1.0f / (float)hd.l.width, 1.0f / (float)hd.l.height, 2.0f / (float)hd.l.width,
                     2.0f / (float)hd.l.height };
    memcpy(&hd.uniforms[hd.reg_inv * 4], inv, sizeof(inv));
    hd.uniforms[hd.reg_cmax * 4] = (float)(hd.l.width / 2u - 1u);
    hd.uniforms[hd.reg_cmax * 4 + 1] = (float)(hd.l.height / 2u - 1u);
    static const uint32_t attrib[2] = { FGPU_ATTRIB(0, 2), FGPU_ATTRIB(2, 2) };
    d.color = (nv3d_color_t){ nv_rm_vram_va(hd.target), hd.target_pitch, width, height, false, 0, 0 };
    d.vp_w = (int32_t)width;
    d.vp_h = (int32_t)height;
    d.front_ccw = g.front_ccw;
    d.vs = (nv3d_stage_t){ nv_rm_vram_va(hd.program->vs.vram), hd.program->vs.regs };
    d.fs = (nv3d_stage_t){ nv_rm_vram_va(hd.program->fs.vram), hd.program->fs.regs };
    d.cb_va = nv_rm_vram_va(hd.cb);
    d.uniforms = hd.uniforms;
    d.textures_changed = true;
    d.stride_bytes = 16;
    d.attrib_count = 2;
    d.attrib = attrib;
    d.vb_va = nv_rm_vram_va(hd.vbo);
    d.vb_bytes = 48;
    d.count = 3;
    p = begin_job();
    if (!p) goto out;
    nv3d_push_draw(p, &d);
    if (!end_job(p)) goto out;
    ok = nv_rm_ce_read(g.a, hd.target, width * 4u, height, hd.target_pitch, out, width * 4u);
out:
    __atomic_store_n(&hd.busy, 0, __ATOMIC_RELEASE);
    return ok;
}

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

// Step 4b: two triangles into a 64x64 surface of our own. First the
// near one alone, without depth, straight into a pitch-linear target
// (shaders, vertex fetch, viewport). Then both with the depth test into a
// block-linear target, as every draw with depth runs (the hardware has no
// pitch-linear color next to a depth buffer), copied back by the copy
// engine: the first (blue-ish, near) must cover the centre and leave the
// corner alone; the second (red, behind it, larger) must lose the depth
// test at the centre and win outside the first. Also finds which winding
// the hardware calls front. Returns true if the 3D engine draws what FGPU
// expects.
static bool self_test(void) {
    enum { N = 64, PITCH = N * 4 };
    say("\nStep 4b: FarcadeGL draws on the 3D engine\n");
    nv_program_t* np = builtin_program(FGPU_PROGRAM_COLOR);
    if (!np) { say("  the built-in shader did not compile to machine code\n"); return false; }
    say("  shaders: vertex and pixel stage compiled (%u + %u registers)\n", np->vs.regs, np->fs.regs);

    uint32_t bw, bh, bl_bytes;
    nv3d_color_bl_size(N, N, &bw, &bh, &bl_bytes);
    uint64_t surf = gpu_vram_alloc(g.a, PITCH * N, 4096, 0);
    uint64_t tsurf = gpu_vram_alloc(g.a, bl_bytes, 4096, 0);
    uint64_t vbo = gpu_vram_alloc(g.a, 4096, 4096, 0);
    nv3d_zeta_t z;
    nv3d_zeta_size(N, N, &z);
    uint64_t zoff = gpu_vram_alloc(g.a, z.bytes, 4096, 0);
    bool ok = false;
    uint64_t mapped = nv_rm_vram_mapped(g.a);
    if (surf == GPU_VRAM_INVALID || tsurf == GPU_VRAM_INVALID || vbo == GPU_VRAM_INVALID ||
        zoff == GPU_VRAM_INVALID || zoff + z.bytes > mapped || surf + PITCH * N > mapped ||
        tsurf + bl_bytes > mapped || vbo + 4096 > mapped) {
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
    nv3d_color_t color = { nv_rm_vram_va(surf), PITCH, N, N, false, 0, 0 };
    nv3d_color_t tiled = { nv_rm_vram_va(tsurf), 0, N, N, true, bw, bh };
    nv3d_draw_t base = {
        .vp_w = N, .vp_h = N,
        .vs = { nv_rm_vram_va(np->vs.vram), np->vs.regs }, .fs = { nv_rm_vram_va(np->fs.vram), np->fs.regs },
        .cb_va = nv_rm_vram_va(g.pools + POOL_CB), .uniforms = builtin_uniforms(np, &u),
        .stride_bytes = 40, .attrib_count = 3, .attrib = attrib,
        .vb_va = nv_rm_vram_va(vbo), .vb_bytes = sizeof(verts),
    };

    // Stage 1: no depth, no culling, pitch-linear target.
    {
        nv_push_t* p = begin_job();
        if (!p) { say("  the 3D channel is not available\n"); goto out; }
        nv3d_push_clear(p, &color, NULL, FGPU_CLEAR_COLOR, 0x00102030u, 1.0f);
        nv3d_draw_t d = base;
        d.color = color;
        d.count = 3;
        nv3d_push_draw(p, &d);
        if (!end_job(p)) { say("  the draw without depth did not finish (see the lines above)\n"); goto out; }
        uint32_t centre = vram_read32(surf + 36u * PITCH + 32u * 4u), corner = vram_read32(surf + 2u * PITCH + 61u * 4u);
        bool good = near_color(centre, 0x4080BFu) && near_color(corner, 0x102030u);
        say("  without depth: centre 0x%08x, corner 0x%08x (%s)\n", centre, corner, good ? "ok" : "WRONG");
        if (!good) goto out;
    }

    // Stage 2: depth test and back-face culling, block-linear target.
    for (int pass = 0; pass < 2 && !ok; pass++) {
        nv_push_t* p = begin_job();
        if (!p) { say("  the 3D channel is not available\n"); goto out; }
        nv3d_push_clear(p, &tiled, &z, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x00102030u, 1.0f);
        nv3d_draw_t d = base;
        d.color = tiled;
        d.zeta = &z;
        d.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE | FGPU_STATE_CULL_BACK;
        d.front_ccw = pass == 1;
        d.count = 6;
        nv3d_push_draw(p, &d);
        if (!end_job(p)) { say("  the test draw did not finish (see the lines above)\n"); goto out; }
        if (!nv_rm_ce_copy_surface(g.a, tsurf, 0, surf, PITCH, bw * 4u, bh, N * 4u, N)) {
            say("  the copy engine did not copy the block-linear target back\n");
            goto out;
        }
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
    if (tsurf != GPU_VRAM_INVALID) gpu_vram_free(g.a, tsurf);
    if (vbo != GPU_VRAM_INVALID) gpu_vram_free(g.a, vbo);
    if (zoff != GPU_VRAM_INVALID) gpu_vram_free(g.a, zoff);
    return ok;
}

// Step 4c: texture sampling. A 64x64 pattern in VRAM is sampled (nearest) by a full screen triangle into a
// pitch-linear target and read back: every pixel must be its texel (either way up). If not, draws that sample
// textures run on the CPU, which keeps pictures right (the report says what the card got).
#define TEX_TEST_VS "attribute vec3 position; varying vec2 v;\n" \
    "void main() { v = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }\n"
#define TEX_TEST_FS "uniform sampler2D t; varying vec2 v;\nvoid main() { gl_FragColor = texture(t, v); }\n"

static uint32_t tex_test_texel(uint32_t x, uint32_t y) {
    return 0xFF000000u | (x * 4u) << 16 | (y * 4u) << 8 | (((x ^ y) & 63u) * 4u);
}

// One sampling draw of the test: a full-screen triangle into surf, the texture described by tic.
static bool tex_test_draw(const nv_program_t* np, uint64_t surf, uint32_t pitch, uint32_t n, uint64_t vbo,
                          const uint32_t tic[8]) {
    uint32_t words[8];
    vram_write(g.pools + POOL_TIC + 32u, tic, 32);
    nv3d_sampler(words, FGPU_TEXTURE_CLAMP);
    vram_write(g.pools + POOL_TSC + 32u, words, 32);
    static float zero_uniforms[FGPU_SHADER_MAX_UNIFORMS * 4];
    static const uint32_t attrib[1] = { FGPU_ATTRIB(0, 3) };
    nv3d_draw_t d;
    memset(&d, 0, sizeof(d));
    d.color = (nv3d_color_t){ nv_rm_vram_va(surf), pitch, n, n, false, 0, 0 };
    d.vp_w = n; d.vp_h = n;
    d.front_ccw = g.front_ccw;
    d.vs = (nv3d_stage_t){ nv_rm_vram_va(np->vs.vram), np->vs.regs };
    d.fs = (nv3d_stage_t){ nv_rm_vram_va(np->fs.vram), np->fs.regs };
    d.cb_va = nv_rm_vram_va(g.pools + POOL_CB);
    d.uniforms = zero_uniforms;
    d.tex_handle[0] = 1u | (1u << 20);
    d.textures_changed = true;
    d.stride_bytes = 12; d.attrib_count = 1; d.attrib = attrib;
    d.vb_va = nv_rm_vram_va(vbo); d.vb_bytes = 36;
    d.count = 3;
    nv_push_t* p = begin_job();
    if (!p) { say("  the 3D channel is not available\n"); return false; }
    nv3d_push_clear(p, &d.color, NULL, FGPU_CLEAR_COLOR, 0x00FF00FFu, 1.0f);
    nv3d_push_draw(p, &d);
    if (!end_job(p)) { say("  the sampling draw did not finish (see the lines above)\n"); return false; }
    return true;
}

// Pixels of surf that are not their texel; orientation 0 upright, 1 rows flipped, -1 the better one
// (written back). The first wrong pixel goes to *fx, *fy, *fg, *fw.
static uint32_t tex_test_compare(uint64_t surf, uint32_t pitch, uint32_t n, int* orientation, uint32_t* fx,
                                 uint32_t* fy, uint32_t* fg, uint32_t* fw) {
    uint32_t wrong[2] = { 0, 0 }, x0[2] = { 0, 0 }, y0[2] = { 0, 0 }, g0[2] = { 0, 0 }, w0[2] = { 0, 0 };
    for (uint32_t y = 0; y < n; y++)
        for (uint32_t x = 0; x < n; x++) {
            uint32_t got = vram_read32(surf + y * pitch + x * 4u) & 0xFFFFFFu;
            for (int o = 0; o < 2; o++) {
                uint32_t want = tex_test_texel(x, o ? n - 1u - y : y) & 0xFFFFFFu;
                if (got != want && wrong[o]++ == 0) { x0[o] = x; y0[o] = y; g0[o] = got; w0[o] = want; }
            }
        }
    int best = *orientation >= 0 ? *orientation : (wrong[1] < wrong[0] ? 1 : 0);
    *orientation = best;
    *fx = x0[best]; *fy = y0[best]; *fg = g0[best]; *fw = w0[best];
    return wrong[best];
}

static void tex_self_test(void) {
    enum { N = 64, PITCH = N * 4 };
    g.tex_tested = true;
    g.tex_ok = false;
    g.tex_wrong = N * N;
    say("\nStep 4c: texture sampling on the 3D engine\n");
    fglsl_program_t* c = (fglsl_program_t*)alloc_unzeroed(sizeof(fglsl_program_t));
    nv_program_t* np = NULL;
    fgpu_program_t prog;
    const char* why = "";
    if (c && fglsl_compile(c, TEX_TEST_VS, TEX_TEST_FS) == 0 && fgpu_program_check(c->words, c->size, &prog, &why))
        np = (nv_program_t*)nv_program_create(&prog);
    if (c) free_mem(c);
    if (!np) {
        say("  the sampling shader did not compile to machine code: textures stay on the CPU\n");
        return;
    }
    uint32_t bw, bh, bl_bytes;
    nv3d_color_bl_size(N, N, &bw, &bh, &bl_bytes);
    uint64_t tex = gpu_vram_alloc(g.a, PITCH * N, 4096, 0);
    uint64_t surf = gpu_vram_alloc(g.a, PITCH * N, 4096, 0);
    uint64_t vbo = gpu_vram_alloc(g.a, 4096, 4096, 0);
    uint64_t bl = gpu_vram_alloc(g.a, bl_bytes, 4096, 0);
    uint64_t mapped = nv_rm_vram_mapped(g.a);
    if (tex == GPU_VRAM_INVALID || surf == GPU_VRAM_INVALID || vbo == GPU_VRAM_INVALID || bl == GPU_VRAM_INVALID ||
        tex + PITCH * N > mapped || surf + PITCH * N > mapped || vbo + 4096 > mapped || bl + bl_bytes > mapped) {
        say("  out of VRAM for the test\n");
        goto out;
    }
    for (uint32_t y = 0; y < N; y++)
        for (uint32_t x = 0; x < N; x++) {
            uint32_t v = tex_test_texel(x, y);
            vram_write(tex + y * PITCH + x * 4u, &v, 4);
        }
    {
        static const float tri[3][3] = { { -1, -1, 0 }, { 3, -1, 0 }, { -1, 3, 0 } };
        vram_write(vbo, tri, sizeof(tri));
    }
    int orientation = -1;
    {
        uint32_t tic[8];
        nv3d_texture_header(tic, nv_rm_vram_va(tex), PITCH, N, N, NV3D_TEX_ARGB, 0);
        if (!tex_test_draw(np, surf, PITCH, N, vbo, tic)) goto out;
        uint32_t fx, fy, fg, fw;
        uint32_t wrong = tex_test_compare(surf, PITCH, N, &orientation, &fx, &fy, &fg, &fw);
        g.tex_wrong = wrong;
        g.tex_first_x = fx; g.tex_first_y = fy; g.tex_got = fg; g.tex_want = fw;
        g.tex_ok = wrong <= (N * N) / 100u;        // a few edge texels may round either way
        if (g.tex_ok) say("  ok: %u of %u pixels are their texel (%s)\n", N * N - wrong, N * N, orientation ? "rows flipped" : "upright");
        else say("  WRONG: %u of %u pixels differ; first at %u,%u: got 0x%06x, want 0x%06x. Draws that sample textures run on the CPU.\n",
                 wrong, N * N, fx, fy, fg, fw);
    }
    // The same texels in a draw target's block-linear layout (made by the copy engine, as for targets).
    if (g.tex_ok) {
        g.tex_bl_tested = true;
        if (!nv_rm_ce_copy_surface(g.a, tex, PITCH, bl, 0, bw * 4u, bh, N * 4u, N)) {
            say("  block-linear: the copy engine did not make the test copy; passes copy targets back before sampling\n");
            goto out;
        }
        uint32_t tic[8];
        nv3d_texture_header(tic, nv_rm_vram_va(bl), 0, N, N, NV3D_TEX_ARGB_BL, 0);
        if (!tex_test_draw(np, surf, PITCH, N, vbo, tic)) goto out;
        uint32_t fx, fy, fg, fw;
        uint32_t wrong = tex_test_compare(surf, PITCH, N, &orientation, &fx, &fy, &fg, &fw);
        g.tex_bl_wrong = wrong;
        g.tex_bl_ok = wrong <= (N * N) / 100u;
        if (g.tex_bl_ok) say("  ok: block-linear targets are sampled directly (%u of %u pixels right)\n", N * N - wrong, N * N);
        else say("  block-linear sampling WRONG (%u of %u differ; first %u,%u got 0x%06x want 0x%06x): targets are copied back before sampling\n",
                 wrong, N * N, fx, fy, fg, fw);
    }
out:
    if (tex != GPU_VRAM_INVALID) gpu_vram_free(g.a, tex);
    if (surf != GPU_VRAM_INVALID) gpu_vram_free(g.a, surf);
    if (vbo != GPU_VRAM_INVALID) gpu_vram_free(g.a, vbo);
    if (bl != GPU_VRAM_INVALID) gpu_vram_free(g.a, bl);
    nv_program_destroy(np);
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
            if (g.hw3d) tex_self_test();
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
    .cpu_draw = nv_cpu_draw,
    .finish = nv_finish,
    .wait_idle = nv_wait_idle,
    .cpu_access = nv_cpu_access,
    .mem_busy = nv_mem_busy,
    .kick = nv_kick,
    .read = nv_read,
    .surface_destroy = nv_surface_destroy,
    .video_convert = nv_video_convert,
    .h264_decode = nv_h264_decode,
    .h264_mismatch = nv_h264_mismatch,
    .engine_stats = nv_rm_engine_stats,
    .perf = nv_perf,
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
            // With GSP-RM running, it power-gates engines while they are idle: their registers do not
            // answer the CPU, though the engine works (NVDEC: see step 5).
            bool gated = nv_rm_chan_ready(a);
            snprintf(line, sizeof(line), "  %s 0x%06x  %s\n", padded(fl->name, 6, pad, sizeof(pad)), (unsigned)fl->base,
                     gated ? "power-gated by GSP-RM while idle (normal)" : "not reachable (powered off?)");
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
    // The counters also after the 3D engine was switched off: they say
    // what ran before that.
    bool was_up = g.hw3d || g.draws || g.refused[R_JOB];
    if (was_up) {
        snprintf(line, sizeof(line), g.hw3d ? "Step 4  FarcadeGL draws run on the 3D engine (%u draws, %u clears so far)\n"
                                            : "Step 4  3D engine switched off after a failed job (%u draws, %u clears before; see the errors below)\n",
                 (unsigned)g.draws, (unsigned)g.clears);
        print(line);
        snprintf(line, sizeof(line), "        depth draws use block-linear color copies (%u copied back, %u loaded); depth synced %u / %u times (to the CPU / to the card)\n",
                 (unsigned)g.resolves, (unsigned)g.uploads, (unsigned)g.depth_stores, (unsigned)g.depth_loads);
        print(line);
        // Where the GPU path's time goes, and why draws went back to the CPU
        // (counts since boot; run 'gpu' twice around a few seconds of work).
        fgpu_hw_counts_t hc;
        fgpu_hw_counts(&hc);
        snprintf(line, sizeof(line), "        batches: %u (draws and clears submitted together), %u queued without waiting; "
                 "the CPU waited for the card %u times; %u render-to-texture barriers\n",
                 (unsigned)g_batches, (unsigned)g_async_batches, (unsigned)g_waits, (unsigned)g_barriers);
        print(line);
        snprintf(line, sizeof(line), "        texture table: %u entries written, %u reused\n", (unsigned)g_tic_written,
                 (unsigned)g_tic_hits);
        print(line);
        snprintf(line, sizeof(line), "        jobs: %llu, %llu us each on average; copies took %llu ms in all\n",
                 (unsigned long long)g.jobs, (unsigned long long)(g.jobs ? g.job_ns / g.jobs / 1000u : 0),
                 (unsigned long long)(g.copy_ns / 1000000u));
        print(line);
        snprintf(line, sizeof(line), "        draws offered to the card %u; on the CPU: shader %u, texture units %u, refused %u; CPU syncs %u\n",
                 (unsigned)hc.offered, (unsigned)hc.shader_on_cpu, (unsigned)hc.texture_units, (unsigned)hc.refused,
                 (unsigned)hc.syncs);
        print(line);
        if (g.tex_tested) {
            if (g.tex_ok && g.tex_bl_ok)
                snprintf(line, sizeof(line), "        texture sampling test: ok; block-linear targets sampled directly (%u times)\n", (unsigned)g.bl_samples);
            else if (g.tex_ok && g.tex_bl_tested)
                snprintf(line, sizeof(line), "        texture sampling test: ok; block-linear sampling WRONG (%u of 4096): targets copied back first\n", (unsigned)g.tex_bl_wrong);
            else if (g.tex_ok) snprintf(line, sizeof(line), "        texture sampling test: ok\n");
            else snprintf(line, sizeof(line), "        texture sampling test: WRONG (%u of 4096 pixels; first %u,%u got 0x%06x want 0x%06x): sampling draws run on the CPU\n",
                          (unsigned)g.tex_wrong, (unsigned)g.tex_first_x, (unsigned)g.tex_first_y, (unsigned)g.tex_got, (unsigned)g.tex_want);
            print(line);
        }
        snprintf(line, sizeof(line), "        shader programs: %u compiled for the card, %u made again and reused\n",
                 (unsigned)hc.programs_compiled, (unsigned)hc.programs_reused);
        print(line);
        for (uint32_t r = 0; r < 8; r++) {
            if (!g.refused[r]) continue;
            snprintf(line, sizeof(line), "          refused: %s %u\n", k_refused[r], (unsigned)g.refused[r]);
            print(line);
        }
        snprintf(line, sizeof(line), "        video: colour conversion of motion .vid frames on the GPU (%u frames so far)\n",
                 (unsigned)v.frames);
        print(line);
    }
    if (nv_rm_log_count()) {
        print("\nGPU errors since boot (the last ones):\n");
        for (uint32_t i = 0; i < nv_rm_log_count(); i++) {
            snprintf(line, sizeof(line), "  %s\n", nv_rm_log_line(i));
            print(line);
        }
    }
    if (was_up) {
        /* reported above */
    } else if (nv_rm_3d_ready(a)) {
        print("Step 4  3D channel is up, but the draw self-test failed (run 'gpu nvidia boot' to see it)\n");
    } else {
        print("Step 4  draws on the 3D engine: wait for the 3D channel (step 4a, 'gpu nvidia boot')\n");
    }
    if (hd.bad) {
        print("Step 5  H.264 on NVDEC: switched off, its pictures differed from the CPU decoder's\n");
    } else if (nv_rm_dec_ready(a)) {
        snprintf(line, sizeof(line), "Step 5  H.264 .vid on the video decoder (NVDEC): channel up, %u pictures decoded%s\n",
                 (unsigned)hd.pictures, g.hw3d ? (hd.pictures ? "" : " (motion .vid files do not use it: only H.264 ones)")
                                                : " (needs step 4 for the colour conversion)");
        print(line);
    } else {
        print("Step 5  H.264 on NVDEC: no decoder channel (step 5a, 'gpu nvidia boot')\n");
    }
    const char* why = "";
    const fgpu_backend_t* hw = fgpu_hw_backend(&why);
    snprintf(line, sizeof(line), "\nRendering: %s%s%s\n", hw ? hw->name : "FGPU CPU backend",
             hw ? "" : " - ", hw ? "" : why);
    print(line);
}
