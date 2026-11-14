#include "x86_64/fgpu.h"
#include "x86_64/fgpu_raster.h"
#include "x86_64/fgpu_shader.h"
#include "x86_64/fgpu_jit_x64.h"     // OS-4: shader JIT
#include "x86_64/fgpu_backend.h"
#include "x86_64/gpucore.h"
#include "x86_64/gpu.h"
#include "x86_64/allocator.h"
#include "x86_64/syscall.h"
#include "x86_64/proc.h"
#include "x86_64/window_manager.h"
#include "graphics.h"
#include "prochandler.h"
#include "serial.h"
#include "string.h"
#include "x86_64/scheduler.h"
#include "x86_64/clock.h"
#include "x86_64/smp_par.h"

/*
 * FGPU command processor (SYS_GPU). Owns surfaces, buffers and shader
 * programs per process, runs submitted command buffers and presents
 * finished frames to the screen (copy or page flip) or to a window.
 *
 * Objects belong to a program (all its threads share them) and live in
 * per-type tables with per-program quotas; each program has a context
 * (its lock, its counts, the fences of its asynchronous submits). Drawing
 * and presenting share one render lock; a kernel worker process runs
 * asynchronous submits.
 *
 * Clears and draws are offered to the adapter's hardware backend first
 * (fgpu_backend.h); whatever it does not take runs on the CPU backend
 * (fgpu_raster.c, shaders in fgpu_shader.c). Before the CPU touches a
 * surface or buffer, queued GPU work is finished (hw_sync).
 */

typedef struct {
    uint32_t id;                // its handle
    uint64_t owner;             // the program (group owner) it belongs to
    uint32_t width, height, format, flags;
    uint32_t pitch;             // in pixels / floats
    void* mem;
    bool in_vram;
    uint64_t vram_offset;
    gpu_adapter_t* adapter;
    void* shadow;               // system-memory copy for CPU reads of slow VRAM, or NULL
} fgpu_surface_t;

typedef struct {
    uint32_t id;
    uint64_t owner;
    uint64_t size;
    void* mem;
    bool in_vram;
    uint64_t vram_offset;
    gpu_adapter_t* adapter;
} fgpu_buffer_t;

typedef struct {
    uint32_t id;
    uint64_t owner;
    uint32_t* blob;             // kernel copy; `program` points into it
    fgpu_program_t program;
    void* hw;                   // the hardware backend's compiled program, or NULL
} fgpu_shader_program_t;

static uint64_t g_flipped_owner;        // pid whose surface is on screen, 0 = none

/* Hardware backend, chosen once by fgpu_backend_select(). */
static const fgpu_backend_t* g_hw;
static const char* g_hw_why = "not chosen yet";
static bool g_hw_pending;               // GPU work queued since the last finish()

/* CPU draws are recorded in a pass and rendered at flush (fgpu_raster.h,
 * docs/gpu.md "Binned rasterizer"). Flush points, all under the render lock: the end of
 * every SUBMIT (so the reported triangle count is exact and nothing stays
 * pending between syscalls), before the hardware backend gets a clear or
 * draw (CPU draws recorded earlier must land first), and at the start of
 * every other SYS_GPU operation (readback, present, uploads, destroys,
 * program changes) and process exit, which read, write or free what pending
 * draws point at. A target switch inside a SUBMIT is a flush point inside
 * the rasterizer (a pass renders one target at a time). */
bool kernel_boot_fgpuold;
static fgpu_pass_t* g_pass;
static void hw_sync_for_cpu(void);

// Renders the recorded CPU draws; returns the triangles drawn since the last flush.
static uint32_t pass_flush(void) {
    if (!g_pass) return 0;
    if (fgpu_pass_pending(g_pass)) hw_sync_for_cpu();
    return fgpu_pass_flush(g_pass);
}

void fgpu_flush_pending(void) {
    pass_flush();
}

// The CPU backend draws on every core (smp_par.h).
_Static_assert(SMP_PAR_MAX_WORKERS <= FGPU_RASTER_MAX_WORKERS, "worker numbers must fit the rasterizer");
static const fgpu_parallel_t k_parallel = { smp_par_workers, smp_par_run };

void fgpu_backend_select(void) {
    fgpu_raster_set_parallel(&k_parallel);
    fgpu_raster_set_allocator(alloc_unzeroed, free_mem);
    fgpu_raster_set_mode(kernel_boot_fgpuold ? FGPU_RASTER_IMMEDIATE : FGPU_RASTER_BINNED);
    serial_write_str(kernel_boot_fgpuold ? "fgpu: CPU rasterizer: immediate (fgpuold)\n"
                                         : "fgpu: CPU rasterizer: binned\n");
    g_hw = NULL;
    gpu_adapter_t* a = gpu_primary_adapter();
    if (!a) { g_hw_why = "no GPU adapter"; return; }
    if (a->vendor != GPU_VENDOR_NVIDIA) { g_hw_why = "no hardware backend for this adapter"; return; }
    const char* why = "";
    if (fgpu_backend_nvidia.init(a, &why)) {
        g_hw = &fgpu_backend_nvidia;
        g_hw_why = "";
        serial_write_str("fgpu: hardware backend ");
        serial_write_str(g_hw->name);
        serial_write_str("\n");
    } else {
        g_hw_why = why;
        serial_write_str("fgpu: CPU backend: ");
        serial_write_str(why);
        serial_write_str("\n");
    }
}

const fgpu_backend_t* fgpu_hw_backend(const char** why) {
    if (why) *why = g_hw_why;
    return g_hw;
}

// The motion video codec's colour conversion on the GPU (SYS_MEDIA).
bool fgpu_video_convert(const uint8_t* y, const uint8_t* cb, const uint8_t* cr, uint32_t w, uint32_t h,
                        uint32_t cw, uint32_t ch, uint32_t* out, uint32_t width, uint32_t height) {
    const fgpu_backend_t* hw = g_hw;
    return hw && hw->video_convert && hw->video_convert(y, cb, cr, w, h, cw, ch, out, width, height);
}

// One H.264 picture on the card's video decoder (SYS_MEDIA).
bool fgpu_video_h264(const syscall_media_h264_t* pic, uint32_t* out, uint32_t width, uint32_t height) {
    const fgpu_backend_t* hw = g_hw;
    return hw && hw->h264_decode && hw->h264_decode(pic, out, width, height);
}

void fgpu_video_h264_mismatch(void) {
    const fgpu_backend_t* hw = g_hw;
    if (hw && hw->h264_mismatch) hw->h264_mismatch();
}

// Waits for queued GPU work before the CPU reads or writes GPU memory.
static void hw_sync(void) {
    if (g_hw && g_hw_pending) g_hw->finish();
    g_hw_pending = false;
}

static void hw_sync_for_cpu(void) { hw_sync(); }

static fgpu_mem_t surface_mem(const fgpu_surface_t* s) {
    fgpu_mem_t m = { 0 };
    if (!s) return m;
    m.cpu = s->mem;
    m.in_vram = s->in_vram;
    m.vram_offset = s->vram_offset;
    m.bytes = (uint64_t)s->pitch * s->height * 4;
    return m;
}

static fgpu_mem_t buffer_mem(const fgpu_buffer_t* b) {
    fgpu_mem_t m = { 0 };
    if (!b) return m;
    m.cpu = b->mem;
    m.in_vram = b->in_vram;
    m.vram_offset = b->vram_offset;
    m.bytes = b->size;
    return m;
}

static void program_free(fgpu_shader_program_t* sp) {
    if (sp->hw && g_hw && g_hw->program_destroy) g_hw->program_destroy(sp->hw);
    sp->hw = NULL;
    fgpu_program_release(&sp->program);     // OS-4: frees the compiled shader code
    free_mem(sp->blob);
    free_mem(sp);
}

#include "x86_64/user_ptr.h"
#include "x86_64/spinlock.h"
#include "x86_64/waitq.h"
#include "x86_64/clock.h"
#include "time.h"

// The calling thread (window and screen ownership go by it, as before).
static uint64_t caller(void) {
    return current_process ? current_process->pid : 0;
}

// The program GPU objects belong to: the group owner, so that all threads
// of a program share its surfaces, buffers and shaders, and a thread that
// ends frees nothing of them.
static uint64_t caller_group(void) {
    process_t* p = current_process;
    if (!p) return 0;
    process_t* o = proc_mem_owner(p);
    return o ? o->pid : p->pid;
}

/* -------------------------------------------------------------- locks
 *
 * Sleeping locks for code that runs with interrupts on and may take long
 * (rendering): a waiter spins briefly, then sleeps on a wait queue. A lock
 * whose holder died holding it (a thread killed in the middle of a GPU
 * call) is taken over by the next waiter, as the old global lock did.
 *
 * Order: context lock, then the render lock, then g_table_lock (a
 * spinlock, held only for table lookups and changes).
 */
typedef struct {
    volatile uint32_t locked;
    volatile uint64_t holder;   // pid of the holding thread
} fgpu_mutex_t;

static bool mutex_should_sleep(void* m) { return ((fgpu_mutex_t*)m)->locked != 0; }

static bool mutex_try(fgpu_mutex_t* m) {
    if (__sync_lock_test_and_set(&m->locked, 1)) return false;
    m->holder = caller();
    return true;
}

static void mutex_lock(fgpu_mutex_t* m) {
    for (uint32_t spins = 0;; spins++) {
        if (mutex_try(m)) return;
        uint64_t h = m->holder;
        if (h && !findProcessByPID(h) &&
            __sync_bool_compare_and_swap(&m->holder, h, caller())) return;
        if (spins < 64) { __asm__ volatile("pause"); continue; }
        // The deadline only bounds how long a dead holder goes unnoticed.
        waitq_wait(m, 0, mutex_should_sleep, m, time_get_uptime_ms() + 10);
    }
}

static void mutex_unlock(fgpu_mutex_t* m) {
    m->holder = 0;
    __sync_lock_release(&m->locked);
    waitq_wake(m, 0, 1);
}

// The CPU rasterizer (one pass, static per-worker batches), the hardware
// backend and the screen are shared by everyone: drawing and presenting
// hold this lock.
static fgpu_mutex_t g_render;

// Whether creating, writing, reading or freeing objects needs the render
// lock too: when queued hardware work may be using them (hw_sync) or VRAM
// is handed out. With the CPU backend alone, programs can upload, read
// back and create while another one renders.
static bool memops_need_render(void) {
    gpu_adapter_t* a = gpu_primary_adapter();
    return g_hw || (a && (a->caps & GPU_CAP_VRAM_HEAP));
}

/* ------------------------------------------------------ object tables
 *
 * One table per object type, shared by all programs, grown on demand up to
 * FGPU_TABLE_MAX slots. A handle is (generation << 20) | (type << 18) |
 * (slot + 1): looking one up is O(1), a stale handle (a freed and reused
 * slot) does not match, and handles stay positive as int32. Every object is
 * owned by one program and only found by it.
 */
enum { OBJ_SURFACE = 1, OBJ_BUFFER = 2, OBJ_PROGRAM = 3, OBJ_TYPES = 4 };

#define FGPU_TABLE_MAX (1u << 16)
// Per-program quotas, so that one program cannot use up the tables.
static const uint32_t k_quota[OBJ_TYPES] = { 0, 1024, 4096, 1024 };

typedef struct {
    void* obj;
    uint64_t owner;
    uint16_t gen;
} fgpu_slot_t;

typedef struct {
    fgpu_slot_t* slots;
    uint32_t cap, used, hint;
} fgpu_table_t;

static fgpu_table_t g_tables[OBJ_TYPES];
static spinlock_t g_table_lock = SPINLOCK_INIT;

static uint32_t table_insert(uint32_t type, void* obj, uint64_t owner) {
    fgpu_table_t* t = &g_tables[type];
    for (;;) {
        uint64_t f = spinlock_acquire(&g_table_lock);
        if (t->used < t->cap) {
            for (uint32_t n = 0; n < t->cap; n++) {
                uint32_t i = (t->hint + n) % t->cap;
                fgpu_slot_t* sl = &t->slots[i];
                if (sl->obj) continue;
                sl->obj = obj;
                sl->owner = owner;
                sl->gen = (uint16_t)((sl->gen + 1) & 0x7FF);
                if (!sl->gen) sl->gen = 1;
                t->used++;
                t->hint = i + 1;
                uint32_t h = ((uint32_t)sl->gen << 20) | (type << 18) | (i + 1);
                spinlock_release(&g_table_lock, f);
                return h;
            }
        }
        uint32_t cap = t->cap;
        spinlock_release(&g_table_lock, f);
        if (cap >= FGPU_TABLE_MAX) return 0;
        // Grow outside the spinlock (the heap may take a while), then swap.
        uint32_t ncap = cap ? cap * 2 : 64;
        fgpu_slot_t* ns = (fgpu_slot_t*)alloc_unzeroed((size_t)ncap * sizeof(*ns));
        if (!ns) return 0;
        memset(ns, 0, (size_t)ncap * sizeof(*ns));
        fgpu_slot_t* old = NULL;
        f = spinlock_acquire(&g_table_lock);
        if (t->cap == cap) {
            if (cap) memcpy(ns, t->slots, (size_t)cap * sizeof(*ns));
            old = t->slots;
            t->slots = ns;
            t->cap = ncap;
            ns = NULL;
        }
        spinlock_release(&g_table_lock, f);
        if (ns) free_mem(ns);
        if (old) free_mem(old);
    }
}

static bool handle_split(uint32_t type, uint32_t h, uint32_t* slot, uint16_t* gen) {
    if (!h || ((h >> 18) & 3u) != type || !(h & 0x3FFFFu)) return false;
    *slot = (h & 0x3FFFFu) - 1;
    *gen = (uint16_t)(h >> 20);
    return true;
}

static void* table_get(uint32_t type, uint32_t h, uint64_t owner) {
    uint32_t i;
    uint16_t gen;
    if (!handle_split(type, h, &i, &gen)) return NULL;
    void* obj = NULL;
    uint64_t f = spinlock_acquire(&g_table_lock);
    fgpu_table_t* t = &g_tables[type];
    if (i < t->cap && t->slots[i].obj && t->slots[i].gen == gen && t->slots[i].owner == owner)
        obj = t->slots[i].obj;
    spinlock_release(&g_table_lock, f);
    return obj;
}

static void table_remove(uint32_t type, uint32_t h) {
    uint32_t i;
    uint16_t gen;
    if (!handle_split(type, h, &i, &gen)) return;
    uint64_t f = spinlock_acquire(&g_table_lock);
    fgpu_table_t* t = &g_tables[type];
    if (i < t->cap && t->slots[i].obj && t->slots[i].gen == gen) {
        t->slots[i].obj = NULL;
        t->slots[i].owner = 0;
        t->used--;
    }
    spinlock_release(&g_table_lock, f);
}

// Takes one object of `owner` out of the table, or NULL when none is left.
static void* table_take_owned(uint32_t type, uint64_t owner) {
    void* obj = NULL;
    uint64_t f = spinlock_acquire(&g_table_lock);
    fgpu_table_t* t = &g_tables[type];
    for (uint32_t i = 0; i < t->cap; i++) {
        if (t->slots[i].obj && t->slots[i].owner == owner) {
            obj = t->slots[i].obj;
            t->slots[i].obj = NULL;
            t->slots[i].owner = 0;
            t->used--;
            break;
        }
    }
    spinlock_release(&g_table_lock, f);
    return obj;
}

static fgpu_surface_t* find_surface(uint32_t id, uint64_t owner) {
    return (fgpu_surface_t*)table_get(OBJ_SURFACE, id, owner);
}

static fgpu_shader_program_t* find_program(uint32_t id, uint64_t owner) {
    return (fgpu_shader_program_t*)table_get(OBJ_PROGRAM, id, owner);
}

static fgpu_buffer_t* find_buffer(uint32_t id, uint64_t owner) {
    return (fgpu_buffer_t*)table_get(OBJ_BUFFER, id, owner);
}

/* ------------------------------------------------------------ contexts
 *
 * One per program that uses SYS_GPU: its lock (the program's GPU calls and
 * its queued command buffers run one at a time, in order), its object
 * counts for the quotas, and the fences of asynchronous submits.
 */
typedef struct fgpu_ctx {
    struct fgpu_ctx* next;
    uint64_t owner;
    fgpu_mutex_t lock;
    uint32_t counts[OBJ_TYPES];
    volatile uint32_t fence_submitted;  // last fence handed out
    volatile uint32_t fence_done;       // last fence whose commands ran
    volatile uint32_t jobs;             // queued or running command buffers
    volatile uint32_t async_triangles;  // drawn by async submits since the last WAIT
    volatile bool async_failed;         // an async command buffer was rejected
    volatile bool dead;                 // the program ended; freed by ctx_gc()
    uint64_t pace_interval_ns;          // FGPU_PRESENT_PACED (0 = 60 Hz)
    uint64_t pace_last_ns;              // the last frame boundary
} fgpu_ctx_t;

static fgpu_ctx_t* g_ctxs;
static spinlock_t g_ctx_lock = SPINLOCK_INIT;
static volatile bool g_gc_pending;

static fgpu_ctx_t* ctx_find(uint64_t owner, bool create) {
    uint64_t f = spinlock_acquire(&g_ctx_lock);
    for (fgpu_ctx_t* c = g_ctxs; c; c = c->next) {
        if (c->owner == owner && !c->dead) {
            spinlock_release(&g_ctx_lock, f);
            return c;
        }
    }
    spinlock_release(&g_ctx_lock, f);
    if (!create) return NULL;
    fgpu_ctx_t* n = (fgpu_ctx_t*)alloc(sizeof(fgpu_ctx_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->owner = owner;
    f = spinlock_acquire(&g_ctx_lock);
    // Another thread of the same program may have made one meanwhile.
    for (fgpu_ctx_t* c = g_ctxs; c; c = c->next) {
        if (c->owner == owner && !c->dead) {
            spinlock_release(&g_ctx_lock, f);
            free_mem(n);
            return c;
        }
    }
    n->next = g_ctxs;
    g_ctxs = n;
    spinlock_release(&g_ctx_lock, f);
    return n;
}

// True when draws run on the graphics card itself: what it renders from
// and into then has to be in VRAM.
static bool hw_3d(void) { return g_hw && (g_hw->caps & FGPU_CAP_HW_3D); }

// VRAM if the adapter has room (and, for memory the CPU reads back, if
// reading VRAM is cheap or the GPU needs it there: `gpu_uses`); otherwise
// the kernel heap.
static void* place(uint64_t bytes, bool cpu_reads, bool allow_vram, bool gpu_uses, uint64_t owner,
                   bool* in_vram, uint64_t* offset, gpu_adapter_t** adapter) {
    *in_vram = false;
    *adapter = NULL;
    gpu_adapter_t* a = gpu_primary_adapter();
    if (allow_vram && a && (a->caps & GPU_CAP_VRAM_HEAP) &&
        (!cpu_reads || (a->caps & GPU_CAP_VRAM_FAST_READ) || (gpu_uses && hw_3d()))) {
        uint64_t off = gpu_vram_alloc(a, bytes, 4096, owner);
        if (off != GPU_VRAM_INVALID) {
            *in_vram = true;
            *offset = off;
            *adapter = a;
            void* p = gpu_vram_ptr(a, off);
            memset(p, 0, (size_t)bytes);
            return p;
        }
    }
    return alloc((size_t)bytes);
}

static void unplace(void* mem, bool in_vram, uint64_t offset, gpu_adapter_t* adapter) {
    if (in_vram) gpu_vram_free(adapter, offset);
    else if (mem) free_mem(mem);
}

// Before a surface's memory is freed.
static void surface_gone(fgpu_surface_t* s) {
    if (g_hw && g_hw->surface_destroy) {
        fgpu_mem_t m = surface_mem(s);
        g_hw->surface_destroy(&m);
    }
    if (s->shadow) free_mem(s->shadow);
    s->shadow = NULL;
}

// The surface's pixels where the CPU can read them quickly: the surface
// itself, or (VRAM behind a slow window) a copy the backend just made.
static void* surface_cpu_view(fgpu_surface_t* s) {
    if (!s->in_vram || !g_hw || !g_hw->read || !s->adapter ||
        (s->adapter->caps & GPU_CAP_VRAM_FAST_READ))
        return s->mem;
    size_t bytes = (size_t)s->pitch * s->height * 4;
    if (!s->shadow) s->shadow = alloc_unzeroed(bytes);
    if (!s->shadow) return s->mem;
    fgpu_mem_t m = surface_mem(s);
    if (!g_hw->read(&m, s->width * 4u, s->height, s->pitch * 4u, s->shadow, s->pitch * 4u))
        return s->mem;
    return s->shadow;
}

static void release_flip_locked(void) {
    if (!g_flipped_owner) return;
    gpu_adapter_t* a = gpu_primary_adapter();
    if (a && a->flip) a->flip(a, a->scanout_offset);
    g_flipped_owner = 0;
    graphics_terminal_redraw();
}

static void surface_free(fgpu_surface_t* s) {
    surface_gone(s);
    unplace(s->mem, s->in_vram, s->vram_offset, s->adapter);
    free_mem(s);
}

static void buffer_free(fgpu_buffer_t* b) {
    unplace(b->mem, b->in_vram, b->vram_offset, b->adapter);
    free_mem(b);
}

/* Asynchronous submits (FGPU_SUBMIT_ASYNC): command buffers queued here
 * are run in order by one kernel worker process (fgpu_worker), each under
 * its program's context lock and the render lock, like a synchronous
 * SUBMIT. */
typedef struct fgpu_job {
    struct fgpu_job* next;
    fgpu_ctx_t* ctx;
    uint32_t* cmd;
    uint32_t words;
    uint32_t fence;
} fgpu_job_t;

static fgpu_job_t* g_job_head;
static fgpu_job_t* g_job_tail;
static spinlock_t g_job_lock = SPINLOCK_INIT;
static volatile uint32_t g_worker_started;

static void wake_worker(void) { waitq_wake(&g_job_head, 0, 1); }

// Frees everything `c` owns. Needs c->lock and, when memops_need_render(),
// the render lock.
static void ctx_free_objects(fgpu_ctx_t* c) {
    void* o;
    while ((o = table_take_owned(OBJ_SURFACE, c->owner))) surface_free((fgpu_surface_t*)o);
    while ((o = table_take_owned(OBJ_BUFFER, c->owner))) buffer_free((fgpu_buffer_t*)o);
    while ((o = table_take_owned(OBJ_PROGRAM, c->owner))) program_free((fgpu_shader_program_t*)o);
    for (int t = 0; t < OBJ_TYPES; t++) c->counts[t] = 0;
}

// Frees `c` and what it owns if nobody holds its locks and nothing of it
// is queued; true when done. Never sleeps (exit paths may have interrupts
// off): lock attempts only.
static bool ctx_try_release(fgpu_ctx_t* c) {
    if (__atomic_load_n(&c->jobs, __ATOMIC_SEQ_CST)) return false;
    if (!mutex_try(&c->lock)) return false;
    bool render = memops_need_render();
    if (render && !mutex_try(&g_render)) {
        mutex_unlock(&c->lock);
        return false;
    }
    if (render) hw_sync();
    ctx_free_objects(c);
    if (render) mutex_unlock(&g_render);
    uint64_t f = spinlock_acquire(&g_ctx_lock);
    for (fgpu_ctx_t** pp = &g_ctxs; *pp; pp = &(*pp)->next) {
        if (*pp == c) { *pp = c->next; break; }
    }
    spinlock_release(&g_ctx_lock, f);
    // No one can find it any more; whoever waits on its lock is a thread of
    // the ended program.
    free_mem(c);
    return true;
}

// Frees contexts of ended programs that could not be freed at exit, and a
// page flip left by a program that is gone. Runs at the start of SYS_GPU
// calls and in the worker.
static void ctx_gc(void) {
    if (!g_gc_pending) return;
    g_gc_pending = false;
    for (;;) {
        fgpu_ctx_t* dead = NULL;
        uint64_t f = spinlock_acquire(&g_ctx_lock);
        for (fgpu_ctx_t* c = g_ctxs; c; c = c->next) if (c->dead) { dead = c; break; }
        spinlock_release(&g_ctx_lock, f);
        if (!dead) break;
        if (!ctx_try_release(dead)) { g_gc_pending = true; break; }
    }
    if (g_flipped_owner && !findProcessByPID(g_flipped_owner) && mutex_try(&g_render)) {
        release_flip_locked();
        mutex_unlock(&g_render);
    }
}

// A thread or a whole program ends. Called on exit paths that may run with
// interrupts off: nothing here sleeps.
void fgpu_process_exit(uint64_t pid) {
    if (!pid) return;
    // A thread killed in the middle of a GPU call: its locks are released
    // and the draws it recorded are dropped, not drawn.
    if (g_render.locked && g_render.holder == pid) {
        if (g_pass) fgpu_pass_discard(g_pass);
        hw_sync();
        if (g_flipped_owner == pid) release_flip_locked();
        mutex_unlock(&g_render);
    } else if (g_flipped_owner == pid && mutex_try(&g_render)) {
        release_flip_locked();
        mutex_unlock(&g_render);
    } else if (g_flipped_owner == pid) {
        g_gc_pending = true;
    }
    uint64_t f = spinlock_acquire(&g_ctx_lock);
    for (fgpu_ctx_t* c = g_ctxs; c; c = c->next) {
        if (c->lock.locked && c->lock.holder == pid) mutex_unlock(&c->lock);
    }
    fgpu_ctx_t* mine = NULL;
    for (fgpu_ctx_t* c = g_ctxs; c; c = c->next) {
        if (c->owner == pid && !c->dead) { mine = c; break; }
    }
    if (mine) mine->dead = true;
    spinlock_release(&g_ctx_lock, f);
    if (!mine) return;          // a thread, or a program that never used the GPU

    // Its queued command buffers are dropped.
    fgpu_job_t* drop = NULL;
    f = spinlock_acquire(&g_job_lock);
    fgpu_job_t** pp = &g_job_head;
    g_job_tail = NULL;
    while (*pp) {
        fgpu_job_t* j = *pp;
        if (j->ctx == mine) {
            *pp = j->next;
            j->next = drop;
            drop = j;
        } else {
            g_job_tail = j;
            pp = &j->next;
        }
    }
    spinlock_release(&g_job_lock, f);
    while (drop) {
        fgpu_job_t* n = drop->next;
        free_mem(drop->cmd);
        free_mem(drop);
        __atomic_fetch_sub(&mine->jobs, 1, __ATOMIC_SEQ_CST);
        drop = n;
    }
    waitq_wake(mine, 1, 0xFFFFFFFFu);
    if (!ctx_try_release(mine)) {
        // Busy (the worker is running one of its buffers, or another CPU
        // holds the render lock): freed by the worker or the next GPU call.
        g_gc_pending = true;
        if (g_worker_started) wake_worker();
    }
}

/* ------------------------------------------------------------ commands */

// State of one SYS_GPU_SUBMIT. Allocated on the heap (it holds the
// shader constants), and reset for every command buffer.
typedef struct {
    fgpu_surface_t* color;
    fgpu_surface_t* depth;
    fgpu_pipeline_t pipe;
    fgpu_layout_t layout;
    float constants[FGPU_SHADER_MAX_UNIFORMS][4];
    void* hw_program;           // compiled form of pipe.shader, NULL if none
    fgpu_surface_t* textures[FGPU_MAX_TEXTURE_UNITS];
    uint64_t owner;
    uint32_t triangles;
} exec_state_t;

static void make_hw_target(exec_state_t* st, const fgpu_target_t* t, fgpu_hw_target_t* hw) {
    hw->cpu = *t;
    hw->color = surface_mem(st->color);
    hw->depth = surface_mem(st->depth);
}

static bool make_target(exec_state_t* st, fgpu_target_t* t) {
    memset(t, 0, sizeof(*t));
    if (!st->color && !st->depth) return false;
    uint32_t w = st->color ? st->color->width : st->depth->width;
    uint32_t h = st->color ? st->color->height : st->depth->height;
    if (st->depth && st->color &&
        (st->depth->width < w || st->depth->height < h)) return false;
    t->width = w;
    t->height = h;
    if (st->color) { t->color = (uint32_t*)st->color->mem; t->color_pitch = st->color->pitch; }
    if (st->depth) { t->depth = (float*)st->depth->mem; t->depth_pitch = st->depth->pitch; }
    return true;
}

static uint64_t execute(exec_state_t* st, const uint32_t* cmd, uint32_t words) {
    uint32_t pos = 0;
    while (pos < words) {
        uint32_t header = cmd[pos];
        uint32_t op = header >> 16;
        uint32_t len = header & 0xFFFF;
        if (len == 0 || pos + len > words) return SYS_ERR_INVAL;
        const uint32_t* arg = cmd + pos + 1;
        uint32_t n = len - 1;
        switch (op) {
            case FGPU_OP_BIND_TARGET: {
                if (n < 2) return SYS_ERR_INVAL;
                fgpu_surface_t* c = arg[0] ? find_surface(arg[0], st->owner) : NULL;
                fgpu_surface_t* d = arg[1] ? find_surface(arg[1], st->owner) : NULL;
                if ((arg[0] && (!c || c->format != FGPU_FORMAT_XRGB8888)) ||
                    (arg[1] && (!d || d->format != FGPU_FORMAT_DEPTH32F)))
                    return SYS_ERR_NOTFOUND;
                st->color = c;
                st->depth = d;
                fgpu_surface_t* any = c ? c : d;
                if (any) {
                    st->pipe.vp_x = 0; st->pipe.vp_y = 0;
                    st->pipe.vp_w = (int32_t)any->width;
                    st->pipe.vp_h = (int32_t)any->height;
                }
                break;
            }
            case FGPU_OP_VIEWPORT:
                if (n < 4) return SYS_ERR_INVAL;
                st->pipe.vp_x = (int32_t)arg[0];
                st->pipe.vp_y = (int32_t)arg[1];
                st->pipe.vp_w = (int32_t)arg[2];
                st->pipe.vp_h = (int32_t)arg[3];
                if (st->pipe.vp_w < 0 || st->pipe.vp_h < 0 ||
                    st->pipe.vp_w > 8192 || st->pipe.vp_h > 8192) return SYS_ERR_INVAL;
                break;
            case FGPU_OP_CLEAR: {
                if (n < 3) return SYS_ERR_INVAL;
                fgpu_target_t t;
                if (!make_target(st, &t)) return SYS_ERR_INVAL;
                float depth;
                memcpy(&depth, &arg[2], sizeof(depth));
                fgpu_hw_target_t ht;
                make_hw_target(st, &t, &ht);
                if (g_hw) {
                    st->triangles += pass_flush();      // CPU draws recorded earlier come first
                    if (g_hw->clear(&ht, arg[0], arg[1], depth)) {
                        g_hw_pending = true;
                        break;
                    }
                }
                if (g_pass) fgpu_pass_clear(g_pass, &t, arg[0], arg[1], depth);
                else { hw_sync(); fgpu_raster_clear(&t, arg[0], arg[1], depth); }
                break;
            }
            case FGPU_OP_SET_STATE:
                if (n < 1) return SYS_ERR_INVAL;
                st->pipe.state = arg[0];
                break;
            case FGPU_OP_SET_PROGRAM:
                if (n < 1 || arg[0] < FGPU_PROGRAM_COLOR || arg[0] > FGPU_PROGRAM_PHONG)
                    return SYS_ERR_INVAL;
                st->pipe.program = arg[0];
                break;
            case FGPU_OP_SET_UNIFORMS:
                if (n * 4 < sizeof(fgpu_uniforms_t)) return SYS_ERR_INVAL;
                memcpy(&st->pipe.u, arg, sizeof(fgpu_uniforms_t));
                break;
            case FGPU_OP_SET_SHADER: {
                if (n < 1) return SYS_ERR_INVAL;
                if (!arg[0]) { st->pipe.shader = NULL; st->hw_program = NULL; break; }
                fgpu_shader_program_t* sp = find_program(arg[0], st->owner);
                if (!sp) return SYS_ERR_NOTFOUND;
                st->pipe.shader = &sp->program;
                st->hw_program = sp->hw;
                break;
            }
            case FGPU_OP_SET_CONSTANTS: {
                if (n < 2) return SYS_ERR_INVAL;
                uint32_t first = arg[0], count = arg[1];
                if (first > FGPU_SHADER_MAX_UNIFORMS || count > FGPU_SHADER_MAX_UNIFORMS - first ||
                    n - 2 < count * 4) return SYS_ERR_INVAL;
                memcpy(st->constants[first], arg + 2, (size_t)count * 16);
                break;
            }
            case FGPU_OP_SET_VERTEX_LAYOUT: {
                if (n < 2 || arg[1] > FGPU_MAX_ATTRIBS || n - 2 < arg[1]) return SYS_ERR_INVAL;
                fgpu_layout_t l;
                memset(&l, 0, sizeof(l));
                l.stride = arg[0];
                l.count = arg[1];
                for (uint32_t i = 0; i < l.count; i++) l.attrib[i] = arg[2 + i];
                if (!fgpu_layout_valid(&l)) return SYS_ERR_INVAL;
                st->layout = l;
                break;
            }
            case FGPU_OP_BIND_TEXTURE: {
                if (n < 3 || arg[0] >= FGPU_MAX_TEXTURE_UNITS) return SYS_ERR_INVAL;
                fgpu_texture_t* tex = &st->pipe.textures[arg[0]];
                memset(tex, 0, sizeof(*tex));
                st->textures[arg[0]] = NULL;
                if (!arg[1]) break;
                fgpu_surface_t* s = find_surface(arg[1], st->owner);
                if (!s || s->format != FGPU_FORMAT_XRGB8888) return SYS_ERR_NOTFOUND;
                st->textures[arg[0]] = s;
                tex->pixels = (const uint32_t*)s->mem;
                tex->width = s->width;
                tex->height = s->height;
                tex->pitch = s->pitch;
                tex->flags = arg[2] & (FGPU_TEXTURE_LINEAR | FGPU_TEXTURE_CLAMP);
                break;
            }
            case FGPU_OP_DRAW: {
                if (n * 4 < sizeof(fgpu_draw_t)) return SYS_ERR_INVAL;
                fgpu_draw_t d;
                memcpy(&d, arg, sizeof(d));
                fgpu_layout_t def;
                const fgpu_layout_t* layout;
                if (d.vertex_format == FGPU_VERTEX_P3N3C4) {
                    fgpu_layout_default(&def);
                    layout = &def;
                } else if (d.vertex_format == FGPU_VERTEX_LAYOUT) {
                    layout = &st->layout;
                } else {
                    return SYS_ERR_INVAL;
                }
                fgpu_buffer_t* vb = find_buffer(d.vertex_buffer, st->owner);
                if (!vb) return SYS_ERR_NOTFOUND;
                uint64_t stride = (uint64_t)layout->stride * sizeof(float);
                uint64_t total = vb->size / stride;
                if (d.first_vertex > total) return SYS_ERR_INVAL;
                const float* verts = (const float*)vb->mem + (uint64_t)d.first_vertex * layout->stride;
                uint32_t avail = (uint32_t)(total - d.first_vertex);
                fgpu_target_t t;
                if (!make_target(st, &t)) return SYS_ERR_INVAL;
                if (!st->pipe.program) st->pipe.program = FGPU_PROGRAM_COLOR;
                fgpu_buffer_t* ib = NULL;
                if (d.index_buffer) {
                    ib = find_buffer(d.index_buffer, st->owner);
                    if (!ib) return SYS_ERR_NOTFOUND;
                    uint64_t icount = ib->size / 4;
                    if (d.first_index > icount || d.index_count > icount - d.first_index)
                        return SYS_ERR_INVAL;
                }
                // A shader the backend could not compile runs on the CPU.
                if (g_hw && (!st->pipe.shader || st->hw_program)) {
                    st->triangles += pass_flush();      // CPU draws recorded earlier come first
                    fgpu_hw_target_t ht;
                    make_hw_target(st, &t, &ht);
                    fgpu_hw_draw_t hd = {
                        .target = &ht, .pipe = &st->pipe, .program = st->hw_program,
                        .layout = layout, .vertices = buffer_mem(vb),
                        .vertex_offset = (uint64_t)d.first_vertex * stride,
                        .vertex_count = avail, .indices = buffer_mem(ib),
                        .index_offset = (uint64_t)d.first_index * 4,
                        .count = ib ? d.index_count : (d.vertex_count < avail ? d.vertex_count : avail),
                    };
                    for (uint32_t u = 0; u < FGPU_MAX_TEXTURE_UNITS; u++)
                        hd.textures[u] = surface_mem(st->textures[u]);
                    uint32_t drawn = 0;
                    if (g_hw->draw(&hd, &drawn)) {
                        g_hw_pending = true;
                        st->triangles += drawn;
                        break;
                    }
                }
                // Recorded now, drawn at the next flush (pipeline state, uniforms and
                // constants are snapshotted; buffers, textures and programs are
                // kept alive by the flush points above).
                const uint32_t* idx = ib ? (const uint32_t*)ib->mem + d.first_index : NULL;
                uint32_t count = ib ? d.index_count : (d.vertex_count < avail ? d.vertex_count : avail);
                if (g_pass) {
                    fgpu_pass_draw_layout(g_pass, &t, &st->pipe, layout, verts, avail, idx, count, 0);
                } else {
                    hw_sync();
                    st->triangles += fgpu_raster_draw_layout(&t, &st->pipe, layout, verts, avail, idx, count);
                }
                break;
            }
            default:
                return SYS_ERR_INVAL;
        }
        pos += len;
    }
    return SYS_SUCCESS;
}

/* ------------------------------------------------------------- syscalls */

static uint64_t op_info(syscall_gpu_request_t* r) {
    syscall_gpu_info_t* out = (syscall_gpu_info_t*)r->out;
    if (!out || !syscall_user_range_ok(out, sizeof(*out), true) ||
        out->size != sizeof(*out)) return SYS_ERR_INVAL;
    syscall_gpu_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = SYS_GPU_ABI_VERSION;
    if (g_hw) {
        strncpy(info.backend, g_hw->name, sizeof(info.backend) - 1);
    } else {
        strcpy(info.backend, "cpu (FGPU software, shaders)");
    }
    gpu_adapter_t* a = gpu_primary_adapter();
    if (a) {
        strncpy(info.name, a->name, sizeof(info.name) - 1);
        strncpy(info.family, a->family, sizeof(info.family) - 1);
        strncpy(info.driver, a->driver, sizeof(info.driver) - 1);
        info.vram_size = a->vram_size;
        info.vram_heap_size = a->heap_end - a->heap_start;
        info.vram_heap_used = gpu_vram_used(a);
        info.caps = a->caps;
    } else {
        strcpy(info.name, "no GPU");
    }
    info.caps |= FGPU_CAP_SHADERS | FGPU_CAP_ASYNC | FGPU_CAP_SCALED_PRESENT | FGPU_CAP_PACED_PRESENT;
    if (g_hw) info.caps |= g_hw->caps;
    info.screen_width = graphics_get_width();
    info.screen_height = graphics_get_height();
    *out = info;
    return SYS_SUCCESS;
}

static uint64_t op_create_surface(syscall_gpu_request_t* r, uint64_t owner) {
    if (!r->width || !r->height || r->width > FGPU_MAX_SURFACE_SIZE ||
        r->height > FGPU_MAX_SURFACE_SIZE ||
        (r->format != FGPU_FORMAT_XRGB8888 && r->format != FGPU_FORMAT_DEPTH32F))
        return SYS_ERR_INVAL;
    bool scanout = (r->flags & FGPU_SURFACE_SCANOUT) != 0;
    gpu_adapter_t* a = gpu_primary_adapter();
    uint32_t pitch = r->width;
    bool color = r->format == FGPU_FORMAT_XRGB8888;
    // The 3D engine renders to pitches that are multiples of 128 bytes.
    if (color && hw_3d()) pitch = (r->width + 31u) & ~31u;
    if (scanout) {
        // Must match the screen so the display can show it directly.
        gpu_device_t* screen = getSystemGPU();
        if (!a || !a->flip || !screen || r->format != FGPU_FORMAT_XRGB8888 ||
            r->width != screen->width || r->height != screen->height) return SYS_ERR_INVAL;
        pitch = screen->pitch / 4;
    }
    fgpu_surface_t* s = (fgpu_surface_t*)alloc(sizeof(fgpu_surface_t));
    if (!s) return SYS_ERR_NOMEM;
    uint64_t bytes = (uint64_t)pitch * r->height * 4;
    // Depth buffers are read for every pixel: keep them in system memory
    // unless VRAM reads are cheap. Scanout surfaces must be in VRAM.
    bool cpu_reads = !scanout;
    bool allow_vram = !(r->flags & FGPU_SURFACE_SYSTEM_MEMORY);
    s->mem = place(bytes, cpu_reads, allow_vram || scanout, color, owner,
                   &s->in_vram, &s->vram_offset, &s->adapter);
    if (!s->mem) { free_mem(s); return SYS_ERR_NOMEM; }
    if (scanout && !s->in_vram) {
        unplace(s->mem, false, 0, NULL);
        free_mem(s);
        return SYS_ERR_NOMEM;
    }
    s->owner = owner;
    s->width = r->width;
    s->height = r->height;
    s->format = r->format;
    s->flags = r->flags;
    s->pitch = pitch;
    s->id = table_insert(OBJ_SURFACE, s, owner);
    if (!s->id) { surface_free(s); return SYS_ERR_NOMEM; }
    r->handle = s->id;
    return SYS_SUCCESS;
}

static uint64_t op_create_buffer(syscall_gpu_request_t* r, uint64_t owner) {
    if (!r->bytes || r->bytes > FGPU_MAX_BUFFER_BYTES) return SYS_ERR_INVAL;
    fgpu_buffer_t* b = (fgpu_buffer_t*)alloc(sizeof(fgpu_buffer_t));
    if (!b) return SYS_ERR_NOMEM;
    b->mem = place(r->bytes, true, true, true, owner, &b->in_vram, &b->vram_offset, &b->adapter);
    if (!b->mem) { free_mem(b); return SYS_ERR_NOMEM; }
    b->owner = owner;
    b->size = r->bytes;
    b->id = table_insert(OBJ_BUFFER, b, owner);
    if (!b->id) { buffer_free(b); return SYS_ERR_NOMEM; }
    r->handle = b->id;
    return SYS_SUCCESS;
}

static uint64_t op_present_to(syscall_gpu_request_t* r, fgpu_surface_t* s, uint64_t owner,
                              uint32_t scale);

// ---- frame pacing (FGPU_PRESENT_PACED) ----
// Per program (its context): frame boundaries are pace_last_ns + n * interval
// on the monotonic clock. A present that arrives early sleeps to the next
// boundary (the scheduler sleeps in whole milliseconds, so the last stretch
// is a short spin with interrupts on). One that is late by less than a frame
// goes straight through and keeps the phase; later than that restarts the
// phase from now. It runs before the render lock is taken, so a paced
// program does not hold up anyone else's GPU calls.
#define FGPU_PACE_DEFAULT_NS 16666667ull

static uint64_t pace_wait(fgpu_ctx_t* c) {
    uint64_t iv = c->pace_interval_ns ? c->pace_interval_ns : FGPU_PACE_DEFAULT_NS;
    uint64_t now = clock_monotonic_ns();
    uint64_t target = c->pace_last_ns ? c->pace_last_ns + iv : now;
    if (now >= target + iv) target = now;
    for (;;) {
        now = clock_monotonic_ns();
        if (now >= target) break;
        uint64_t left = target - now;
        if (left >= 2000000ull) sleep(left / 1000000ull - 1);
        else __asm__ volatile("pause");
    }
    c->pace_last_ns = target;
    return clock_monotonic_ns();
}

static uint64_t op_set_pace(fgpu_ctx_t* c, syscall_gpu_request_t* r) {
    uint64_t ns = r->bytes ? r->bytes : FGPU_PACE_DEFAULT_NS;
    if (ns < FGPU_PACE_MIN_NS || ns > FGPU_PACE_MAX_NS) return SYS_ERR_INVAL;
    c->pace_interval_ns = ns;
    c->pace_last_ns = 0;
    return SYS_SUCCESS;
}

// `group` owns the surface; `owner` is the calling thread, which windows
// and the screen are checked against. `stamp`: the time a paced present
// was let through (pace_wait, already done by the caller).
static uint64_t op_present(syscall_gpu_request_t* r, uint64_t group, uint64_t owner, uint64_t stamp) {
    fgpu_surface_t* s = find_surface(r->handle, group);
    if (!s || s->format != FGPU_FORMAT_XRGB8888) return SYS_ERR_NOTFOUND;
    const uint32_t scale_bits = r->flags & (FGPU_PRESENT_SCALE2 | FGPU_PRESENT_SCALE3);
    if ((r->flags & ~FGPU_PRESENT_KNOWN) ||
        scale_bits == (FGPU_PRESENT_SCALE2 | FGPU_PRESENT_SCALE3) ||
        (scale_bits && (r->window_id || (r->flags & FGPU_PRESENT_FLIP))))
        return SYS_ERR_INVAL;
    const uint32_t scale = (r->flags & FGPU_PRESENT_SCALE3) ? 3u :
                           (r->flags & FGPU_PRESENT_SCALE2) ? 2u : 1u;
    const bool paced = (r->flags & FGPU_PRESENT_PACED) != 0;
    if (paced && r->out && !syscall_user_range_ok(r->out, sizeof(stamp), true))
        return SYS_ERR_INVAL;
    if (!r->window_id && (!graphics_has_screen_owner() || graphics_screen_owner_pid() != owner))
        return SYS_ERR_PERM;
    uint64_t res = op_present_to(r, s, owner, scale);
    if (res == SYS_SUCCESS && paced && r->out &&
        !syscall_user_copy_out(r->out, &stamp, sizeof(stamp)))
        return SYS_ERR_INVAL;
    return res;
}

static uint64_t op_present_to(syscall_gpu_request_t* r, fgpu_surface_t* s, uint64_t owner,
                              uint32_t scale) {

    if (r->window_id) {
        syscall_window_request_t w;
        memset(&w, 0, sizeof(w));
        w.size = sizeof(w);
        w.version = SYS_WINDOW_ABI_VERSION;
        w.op = SYS_WINDOW_GET_INFO;
        w.id = r->window_id;
        syscall_window_info_t info;
        w.info = &info;
        uint64_t res = window_manager_syscall(&w, owner, false);
        if (res != SYS_SUCCESS) return res;
        if (s->width < info.width || s->height < info.height) return SYS_ERR_INVAL;
        // Shared frames: render straight into the window's back buffer.
        window_present_target_t target;
        if (window_manager_present_target(r->window_id, owner, &target)) {
            if (s->width < target.width || s->height < target.height) {
                // The window grew since it was checked: abandon the target.
                window_manager_present_done(r->window_id, owner, &target, 0, 0, 0, 0);
                return SYS_ERR_INVAL;
            }
            hw_sync();
            const uint32_t* src_px = (const uint32_t*)surface_cpu_view(s);
            for (uint32_t row = 0; row < target.height; row++)
                memcpy(target.pixels + (size_t)row * target.pitch_px,
                       src_px + (size_t)row * s->pitch,
                       (size_t)target.width * sizeof(uint32_t));
            return window_manager_present_done(r->window_id, owner, &target,
                                               0, 0, target.width, target.height);
        }
        memset(&w, 0, sizeof(w));
        w.size = sizeof(w);
        w.version = SYS_WINDOW_ABI_VERSION;
        w.op = SYS_WINDOW_PRESENT;
        w.id = r->window_id;
        hw_sync();
        w.pixels = (const uint32_t*)surface_cpu_view(s);
        w.pitch = s->pitch * 4;
        w.width = info.width;
        w.height = info.height;
        return window_manager_syscall(&w, owner, false);
    }

    // Screen (the owner check is in op_present).
    if ((r->flags & FGPU_PRESENT_FLIP) && (s->flags & FGPU_SURFACE_SCANOUT) &&
        s->in_vram && s->adapter && s->adapter->flip) {
        if (!s->adapter->flip(s->adapter, s->vram_offset)) return SYS_ERR_INVAL;
        g_flipped_owner = owner;
        return SYS_SUCCESS;
    }
    if (g_flipped_owner == owner) release_flip_locked();
    hw_sync();
    // Straight from the surface into the framebuffer (scaled if asked), no
    // back buffer copy: see graphics_present_scaled().
    gfx_surface_t src = { (uint32_t*)surface_cpu_view(s), (size_t)s->pitch * s->height * 4,
                          s->width, s->height, s->pitch };
    return graphics_present_scaled(&src, scale, r->x, r->y) ? SYS_SUCCESS : SYS_ERR_INVAL;
}

static uint64_t op_create_program(syscall_gpu_request_t* r, uint64_t owner) {
    if (!r->data || r->bytes < sizeof(fgpu_shader_header_t) || r->bytes > FGPU_MAX_PROGRAM_BYTES ||
        (r->bytes & 3) || !syscall_user_range_ok(r->data, (size_t)r->bytes, false))
        return SYS_ERR_INVAL;
    // Check the kernel's own copy, so the program cannot change afterwards.
    uint32_t* blob = (uint32_t*)alloc_unzeroed((size_t)r->bytes);
    if (!blob) return SYS_ERR_NOMEM;
    memcpy(blob, r->data, (size_t)r->bytes);
    fgpu_shader_program_t* sp = (fgpu_shader_program_t*)alloc(sizeof(fgpu_shader_program_t));
    if (!sp) { free_mem(blob); return SYS_ERR_NOMEM; }
    const char* why = "";
    if (!fgpu_program_check(blob, (uint32_t)r->bytes, &sp->program, &why)) {
        serial_write_str("fgpu: shader program rejected: ");
        serial_write_str(why);
        serial_write_str("\n");
        free_mem(blob);
        free_mem(sp);
        return SYS_ERR_INVAL;
    }
    fgpu_program_jit(&sp->program);         // OS-4: compile now (here we may allocate); falls back silently
    sp->owner = owner;
    sp->blob = blob;
    sp->hw = g_hw && g_hw->program_create ? g_hw->program_create(&sp->program) : NULL;
    sp->id = table_insert(OBJ_PROGRAM, sp, owner);
    if (!sp->id) { program_free(sp); return SYS_ERR_NOMEM; }
    r->handle = sp->id;
    return SYS_SUCCESS;
}

static uint64_t op_write_surface(syscall_gpu_request_t* r, uint64_t owner) {
    fgpu_surface_t* s = find_surface(r->handle, owner);
    if (!s) return SYS_ERR_NOTFOUND;
    uint64_t row = (uint64_t)s->width * 4;
    if (!r->data || r->pitch < row ||
        !syscall_user_range_ok(r->data, (size_t)r->pitch * (s->height - 1) + row, false))
        return SYS_ERR_INVAL;
    for (uint32_t y = 0; y < s->height; y++) {
        memcpy((uint8_t*)s->mem + (size_t)y * s->pitch * 4,
               (const uint8_t*)r->data + (size_t)y * r->pitch, (size_t)row);
    }
    return SYS_SUCCESS;
}

static uint64_t fgpu_syscall_checked(syscall_gpu_request_t* r);

// The request is copied into the kernel first: sizes and pointers are
// validated and then used from the copy (the work below runs with
// interrupts on, so a second thread of the program could otherwise change
// `bytes` between a check and the memcpy). Only `handle` is written back.
// Nothing here keeps a program pointer past the call: data is copied (or
// copied out) before it returns, and the parallel workers only touch
// kernel memory.
uint64_t fgpu_syscall(syscall_gpu_request_t* user) {
    syscall_gpu_request_t req;
    if (!user || !syscall_user_range_ok(user, sizeof(req), true)) return SYS_ERR_INVAL;
    memcpy(&req, user, sizeof(req));
    uint32_t handle = req.handle;
    uint64_t result = fgpu_syscall_checked(&req);
    if (req.handle != handle) user->handle = req.handle;
    return result;
}

// Runs one command buffer (kernel copy) for `owner`: needs its context
// lock and the render lock. Returns the result; *triangles gets the count.
static uint64_t run_commands(uint64_t owner, const uint32_t* cmd, uint32_t words, uint32_t* triangles) {
    *triangles = 0;
    exec_state_t* st = (exec_state_t*)alloc(sizeof(exec_state_t));
    if (!st) return SYS_ERR_NOMEM;
    memset(st, 0, sizeof(*st));
    st->owner = owner;
    st->pipe.program = FGPU_PROGRAM_COLOR;
    st->pipe.constants = (const float (*)[4])st->constants;
    fgpu_layout_default(&st->layout);
    if (!g_pass) g_pass = fgpu_pass_create();     // NULL: draws run immediately
    uint64_t result = execute(st, cmd, words);
    st->triangles += pass_flush();      // also when the buffer failed half way
    *triangles = st->triangles;
    free_mem(st);
    return result;
}

/* ------------------------------------------------- async submit worker */

#define FGPU_MAX_QUEUED_PER_CTX 4u

static bool worker_should_sleep(void* unused) {
    (void)unused;
    return !g_job_head && !g_gc_pending;
}

static fgpu_job_t* job_pop(void) {
    uint64_t f = spinlock_acquire(&g_job_lock);
    fgpu_job_t* j = g_job_head;
    if (j) {
        g_job_head = j->next;
        if (!g_job_head) g_job_tail = NULL;
    }
    spinlock_release(&g_job_lock, f);
    return j;
}

// A kernel process: preemptible like any other, so long command buffers
// do not hold up the program that queued them or anyone else.
static void fgpu_worker(void) {
    __asm__ volatile("sti");
    for (;;) {
        ctx_gc();
        fgpu_job_t* j = job_pop();
        if (!j) {
            waitq_wait(&g_job_head, 0, worker_should_sleep, NULL, 0);
            continue;
        }
        fgpu_ctx_t* c = j->ctx;
        mutex_lock(&c->lock);
        if (!c->dead) {
            uint32_t tris = 0;
            mutex_lock(&g_render);
            uint64_t res = run_commands(c->owner, j->cmd, j->words, &tris);
            mutex_unlock(&g_render);
            __atomic_fetch_add(&c->async_triangles, tris, __ATOMIC_SEQ_CST);
            if (res != SYS_SUCCESS) c->async_failed = true;
        }
        __atomic_store_n(&c->fence_done, j->fence, __ATOMIC_SEQ_CST);
        mutex_unlock(&c->lock);
        free_mem(j->cmd);
        free_mem(j);
        // The context may be freed (ctx_gc) as soon as this reaches 0.
        __atomic_fetch_sub(&c->jobs, 1, __ATOMIC_SEQ_CST);
        waitq_wake(c, 1, 0xFFFFFFFFu);
    }
}

static bool start_worker(void) {
    if (__atomic_load_n(&g_worker_started, __ATOMIC_ACQUIRE) == 2) return true;
    uint32_t expect = 0;
    if (__atomic_compare_exchange_n(&g_worker_started, &expect, 1, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        if (!proc_create("fgpu-worker", fgpu_worker)) {
            __atomic_store_n(&g_worker_started, 0, __ATOMIC_RELEASE);
            return false;
        }
        __atomic_store_n(&g_worker_started, 2, __ATOMIC_RELEASE);
        return true;
    }
    // Another thread is starting it.
    while (__atomic_load_n(&g_worker_started, __ATOMIC_ACQUIRE) == 1) __asm__ volatile("pause");
    return __atomic_load_n(&g_worker_started, __ATOMIC_ACQUIRE) == 2;
}

typedef struct {
    fgpu_ctx_t* ctx;
    uint32_t fence;
    uint32_t backlog;           // wait until at most this many are queued (fence = 0)
} fence_wait_t;

static bool fence_pending(const fence_wait_t* w) {
    if (w->ctx->dead) return false;
    if (!w->fence) return __atomic_load_n(&w->ctx->jobs, __ATOMIC_SEQ_CST) > w->backlog;
    return (int32_t)(__atomic_load_n(&w->ctx->fence_done, __ATOMIC_SEQ_CST) - w->fence) < 0;
}

static bool fence_should_sleep(void* p) { return fence_pending((const fence_wait_t*)p); }

// Waits until `fence` has run (fence 0: until no more than `backlog`
// command buffers are queued). timeout_ns: 0 = just check, ~0 = forever.
static uint64_t fence_wait(fgpu_ctx_t* c, uint32_t fence, uint32_t backlog, uint64_t timeout_ns) {
    fence_wait_t w = { c, fence, backlog };
    uint64_t start = clock_monotonic_ns();
    for (;;) {
        if (!fence_pending(&w)) return SYS_SUCCESS;
        uint64_t now = clock_monotonic_ns();
        if (timeout_ns != ~0ull && now - start >= timeout_ns) return SYS_ERR_TIMEDOUT;
        uint64_t deadline = time_get_uptime_ms() + 10;
        if (timeout_ns != ~0ull) {
            uint64_t left_ms = (timeout_ns - (now - start)) / 1000000ull + 1;
            if (left_ms < 10) deadline = time_get_uptime_ms() + left_ms;
        }
        waitq_wait(c, 1, fence_should_sleep, &w, deadline);
    }
}

// Waits until every command buffer the program queued has run, so that
// what it does next sees their results.
static void ctx_drain(fgpu_ctx_t* c) {
    if (__atomic_load_n(&c->jobs, __ATOMIC_SEQ_CST)) fence_wait(c, 0, 0, ~0ull);
}

static uint64_t op_submit_async(fgpu_ctx_t* c, syscall_gpu_request_t* r, uint32_t* cmd) {
    if (!start_worker()) return SYS_ERR_NOMEM;
    // Back-pressure: a program that queues faster than the GPU draws waits.
    if (__atomic_load_n(&c->jobs, __ATOMIC_SEQ_CST) >= FGPU_MAX_QUEUED_PER_CTX)
        fence_wait(c, 0, FGPU_MAX_QUEUED_PER_CTX - 1, ~0ull);
    fgpu_job_t* j = (fgpu_job_t*)alloc(sizeof(fgpu_job_t));
    if (!j) return SYS_ERR_NOMEM;
    j->ctx = c;
    j->cmd = cmd;
    j->words = (uint32_t)(r->bytes / 4);
    __atomic_fetch_add(&c->jobs, 1, __ATOMIC_SEQ_CST);
    uint64_t f = spinlock_acquire(&g_job_lock);
    // Fences are handed out in queue order (under the queue lock), so the
    // worker completes them in increasing order.
    uint32_t fence = c->fence_submitted + 1;
    if (!fence) fence = 1;      // 0 means "all"
    c->fence_submitted = fence;
    j->fence = fence;
    j->next = NULL;
    if (g_job_tail) g_job_tail->next = j;
    else g_job_head = j;
    g_job_tail = j;
    spinlock_release(&g_job_lock, f);
    wake_worker();
    r->handle = fence;
    return SYS_SUCCESS;
}

static uint64_t op_wait(fgpu_ctx_t* c, syscall_gpu_request_t* r) {
    uint32_t fence = r->handle;
    uint64_t timeout = r->bytes;
    uint64_t res = fence ? fence_wait(c, fence, 0, timeout) : fence_wait(c, 0, 0, timeout);
    if (res != SYS_SUCCESS) return res;
    r->handle = __atomic_exchange_n(&c->async_triangles, 0, __ATOMIC_SEQ_CST);
    if (c->async_failed) {
        c->async_failed = false;
        return SYS_ERR_INVAL;
    }
    return SYS_SUCCESS;
}

static uint64_t fgpu_syscall_checked(syscall_gpu_request_t* r) {
    if (r->size != sizeof(*r) ||
        r->version < SYS_GPU_ABI_VERSION_MIN || r->version > SYS_GPU_ABI_VERSION)
        return SYS_ERR_INVAL;
    if (r->op == SYS_GPU_INFO) return op_info(r);
    uint64_t owner = caller();
    uint64_t group = caller_group();

    // Rendering can take a while: let the timer and other processes run.
    // The syscall stub saved this process's SSE state.
    __asm__ volatile("sti");
    ctx_gc();
    uint64_t result = SYS_ERR_INVAL;
    // SUBMIT copies the command buffer before taking any lock.
    uint32_t* cmd = NULL;
    if (r->op == SYS_GPU_SUBMIT) {
        if (!r->data || r->bytes < 4 || r->bytes > FGPU_MAX_COMMAND_BYTES || (r->bytes & 3) ||
            !syscall_user_range_ok(r->data, (size_t)r->bytes, false)) goto out;
        cmd = (uint32_t*)alloc_unzeroed((size_t)r->bytes);
        if (!cmd) { result = SYS_ERR_NOMEM; goto out; }
        memcpy(cmd, r->data, (size_t)r->bytes);
    }
    fgpu_ctx_t* c = ctx_find(group, true);
    if (!c) { result = SYS_ERR_NOMEM; goto out; }

    if (r->op == SYS_GPU_WAIT) { result = op_wait(c, r); goto out; }
    bool async = r->op == SYS_GPU_SUBMIT && r->version >= 3 && (r->flags & FGPU_SUBMIT_ASYNC);
    if (async) {
        result = op_submit_async(c, r, cmd);
        if (result == SYS_SUCCESS) cmd = NULL;      // the worker frees it
        goto out;
    }
    // Everything else sees the results of the program's queued submits.
    ctx_drain(c);
    mutex_lock(&c->lock);
    uint64_t stamp = 0;
    if (r->op == SYS_GPU_PRESENT && (r->flags & FGPU_PRESENT_PACED) && !(r->flags & ~FGPU_PRESENT_KNOWN))
        stamp = pace_wait(c);
    bool render = r->op == SYS_GPU_SUBMIT || r->op == SYS_GPU_PRESENT ||
                  r->op == SYS_GPU_RELEASE_DISPLAY || memops_need_render();
    if (render) {
        mutex_lock(&g_render);
        // Everything but SUBMIT has the CPU read or write GPU memory (or
        // free it). Nothing is pending between calls (SUBMIT flushes at its
        // end); this keeps that true if a later change defers work.
        if (r->op != SYS_GPU_SUBMIT) { fgpu_flush_pending(); hw_sync(); }
    }
    switch (r->op) {
        case SYS_GPU_CREATE_SURFACE:
            if (c->counts[OBJ_SURFACE] >= k_quota[OBJ_SURFACE]) { result = SYS_ERR_NOMEM; break; }
            result = op_create_surface(r, group);
            if (result == SYS_SUCCESS) c->counts[OBJ_SURFACE]++;
            break;
        case SYS_GPU_DESTROY_SURFACE: {
            fgpu_surface_t* s = find_surface(r->handle, group);
            if (!s) { result = SYS_ERR_NOTFOUND; break; }
            if (g_flipped_owner == owner) {
                if (!render) mutex_lock(&g_render);
                release_flip_locked();
                if (!render) mutex_unlock(&g_render);
            }
            table_remove(OBJ_SURFACE, s->id);
            surface_free(s);
            c->counts[OBJ_SURFACE]--;
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_CREATE_BUFFER:
            if (c->counts[OBJ_BUFFER] >= k_quota[OBJ_BUFFER]) { result = SYS_ERR_NOMEM; break; }
            result = op_create_buffer(r, group);
            if (result == SYS_SUCCESS) c->counts[OBJ_BUFFER]++;
            break;
        case SYS_GPU_UPLOAD_BUFFER: {
            fgpu_buffer_t* b = find_buffer(r->handle, group);
            if (!b) { result = SYS_ERR_NOTFOUND; break; }
            if (!r->data || r->offset > b->size || r->bytes > b->size - r->offset ||
                !syscall_user_range_ok(r->data, (size_t)r->bytes, false)) break;
            memcpy((uint8_t*)b->mem + r->offset, r->data, (size_t)r->bytes);
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_DESTROY_BUFFER: {
            fgpu_buffer_t* b = find_buffer(r->handle, group);
            if (!b) { result = SYS_ERR_NOTFOUND; break; }
            table_remove(OBJ_BUFFER, b->id);
            buffer_free(b);
            c->counts[OBJ_BUFFER]--;
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_SUBMIT: {
            uint32_t tris = 0;
            result = run_commands(group, cmd, (uint32_t)(r->bytes / 4), &tris);
            r->handle = tris;
            break;
        }
        case SYS_GPU_READBACK: {
            fgpu_surface_t* s = find_surface(r->handle, group);
            if (!s) { result = SYS_ERR_NOTFOUND; break; }
            uint64_t row = (uint64_t)s->width * 4;
            if (!r->out || r->pitch < row ||
                !syscall_user_range_ok(r->out, (size_t)r->pitch * (s->height - 1) + row, true)) break;
            const uint8_t* view = (const uint8_t*)surface_cpu_view(s);
            for (uint32_t y = 0; y < s->height; y++) {
                memcpy((uint8_t*)r->out + (size_t)y * r->pitch,
                       view + (size_t)y * s->pitch * 4, (size_t)row);
            }
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_PRESENT:
            result = op_present(r, group, owner, stamp);
            break;
        case SYS_GPU_SET_PACE:
            result = op_set_pace(c, r);
            break;
        case SYS_GPU_RELEASE_DISPLAY:
            if (g_flipped_owner == owner) release_flip_locked();
            result = SYS_SUCCESS;
            break;
        case SYS_GPU_CREATE_PROGRAM:
            if (c->counts[OBJ_PROGRAM] >= k_quota[OBJ_PROGRAM]) { result = SYS_ERR_NOMEM; break; }
            result = op_create_program(r, group);
            if (result == SYS_SUCCESS) c->counts[OBJ_PROGRAM]++;
            break;
        case SYS_GPU_DESTROY_PROGRAM: {
            fgpu_shader_program_t* sp = find_program(r->handle, group);
            if (!sp) { result = SYS_ERR_NOTFOUND; break; }
            table_remove(OBJ_PROGRAM, sp->id);
            program_free(sp);
            c->counts[OBJ_PROGRAM]--;
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_WRITE_SURFACE:
            result = op_write_surface(r, group);
            break;
        default:
            break;
    }
    if (render) mutex_unlock(&g_render);
    mutex_unlock(&c->lock);
out:
    if (cmd) free_mem(cmd);
    __asm__ volatile("cli");
    return result;
}
