#include "x86_64/fgpu.h"
#include "x86_64/fgpu_raster.h"
#include "x86_64/fgpu_shader.h"
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
#include "x86_64/smp_par.h"

/*
 * FGPU command processor (SYS_GPU). Owns surfaces, buffers and shader
 * programs per process, runs submitted command buffers and presents
 * finished frames to the screen (copy or page flip) or to a window.
 *
 * Clears and draws are offered to the adapter's hardware backend first
 * (fgpu_backend.h); whatever it does not take runs on the CPU backend
 * (fgpu_raster.c, shaders in fgpu_shader.c). Before the CPU touches a
 * surface or buffer, queued GPU work is finished (hw_sync).
 */

#define FGPU_MAX_SURFACES 64
#define FGPU_MAX_BUFFERS 128
#define FGPU_MAX_PROGRAMS 64

typedef struct {
    bool used;
    uint32_t id;
    uint64_t owner;
    uint32_t width, height, format, flags;
    uint32_t pitch;             // in pixels / floats
    void* mem;
    bool in_vram;
    uint64_t vram_offset;
    gpu_adapter_t* adapter;
} fgpu_surface_t;

typedef struct {
    bool used;
    uint32_t id;
    uint64_t owner;
    uint64_t size;
    void* mem;
    bool in_vram;
    uint64_t vram_offset;
    gpu_adapter_t* adapter;
} fgpu_buffer_t;

typedef struct {
    bool used;
    uint32_t id;
    uint64_t owner;
    uint32_t* blob;             // kernel copy; `program` points into it
    fgpu_program_t program;
    void* hw;                   // the hardware backend's compiled program, or NULL
} fgpu_shader_program_t;

static fgpu_surface_t g_surfaces[FGPU_MAX_SURFACES];
static fgpu_buffer_t g_buffers[FGPU_MAX_BUFFERS];
static fgpu_shader_program_t g_programs[FGPU_MAX_PROGRAMS];
static uint32_t g_next_id = 1;
static volatile int g_busy;
static uint64_t g_flipped_owner;        // pid whose surface is on screen, 0 = none

/* Hardware backend, chosen once by fgpu_backend_select(). */
static const fgpu_backend_t* g_hw;
static const char* g_hw_why = "not chosen yet";
static bool g_hw_pending;               // GPU work queued since the last finish()

// The CPU backend draws on every core (smp_par.h).
_Static_assert(SMP_PAR_MAX_WORKERS <= FGPU_RASTER_MAX_WORKERS, "worker numbers must fit the rasterizer");
static const fgpu_parallel_t k_parallel = { smp_par_workers, smp_par_run };

void fgpu_backend_select(void) {
    fgpu_raster_set_parallel(&k_parallel);
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

// Waits for queued GPU work before the CPU reads or writes GPU memory.
static void hw_sync(void) {
    if (g_hw && g_hw_pending) g_hw->finish();
    g_hw_pending = false;
}

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
    free_mem(sp->blob);
    sp->used = false;
}

extern bool syscall_user_range_ok(const void* ptr, size_t len, bool write);

static uint64_t caller(void) {
    return current_process ? current_process->pid : 0;
}

static volatile uint64_t g_busy_owner;

static void lock(void) {
    while (__sync_lock_test_and_set(&g_busy, 1)) {
        // A process killed while holding the lock must not block everyone.
        uint64_t holder = g_busy_owner;
        if (holder && !findProcessByPID(holder)) {
            g_busy_owner = 0;
            __sync_lock_release(&g_busy);
            continue;
        }
        sleep(1);
    }
    g_busy_owner = caller();
}

static void unlock(void) {
    g_busy_owner = 0;
    __sync_lock_release(&g_busy);
}

static fgpu_surface_t* find_surface(uint32_t id, uint64_t owner) {
    if (!id) return NULL;
    for (int i = 0; i < FGPU_MAX_SURFACES; i++) {
        if (g_surfaces[i].used && g_surfaces[i].id == id && g_surfaces[i].owner == owner)
            return &g_surfaces[i];
    }
    return NULL;
}

static fgpu_shader_program_t* find_program(uint32_t id, uint64_t owner) {
    if (!id) return NULL;
    for (int i = 0; i < FGPU_MAX_PROGRAMS; i++) {
        if (g_programs[i].used && g_programs[i].id == id && g_programs[i].owner == owner)
            return &g_programs[i];
    }
    return NULL;
}

static fgpu_buffer_t* find_buffer(uint32_t id, uint64_t owner) {
    if (!id) return NULL;
    for (int i = 0; i < FGPU_MAX_BUFFERS; i++) {
        if (g_buffers[i].used && g_buffers[i].id == id && g_buffers[i].owner == owner)
            return &g_buffers[i];
    }
    return NULL;
}

// VRAM if the adapter has room (and, for memory the CPU reads back, if
// reading VRAM is cheap); otherwise the kernel heap.
static void* place(uint64_t bytes, bool cpu_reads, bool allow_vram, uint64_t owner,
                   bool* in_vram, uint64_t* offset, gpu_adapter_t** adapter) {
    *in_vram = false;
    *adapter = NULL;
    gpu_adapter_t* a = gpu_primary_adapter();
    if (allow_vram && a && (a->caps & GPU_CAP_VRAM_HEAP) &&
        (!cpu_reads || (a->caps & GPU_CAP_VRAM_FAST_READ))) {
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

static void release_flip_locked(void) {
    if (!g_flipped_owner) return;
    gpu_adapter_t* a = gpu_primary_adapter();
    if (a && a->flip) a->flip(a, a->scanout_offset);
    g_flipped_owner = 0;
    graphics_terminal_redraw();
}

void fgpu_process_exit(uint64_t pid) {
    if (!pid) return;
    if (g_busy && g_busy_owner == pid) {
        g_busy_owner = 0;
        __sync_lock_release(&g_busy);
    }
    // Called on exit paths that may run with interrupts off; never sleep
    // there waiting for the lock.
    if (__sync_lock_test_and_set(&g_busy, 1)) {
        serial_write_str("fgpu: busy at process exit, resources kept\n");
        return;
    }
    g_busy_owner = pid;
    hw_sync();
    if (g_flipped_owner == pid) release_flip_locked();
    for (int i = 0; i < FGPU_MAX_SURFACES; i++) {
        fgpu_surface_t* s = &g_surfaces[i];
        if (s->used && s->owner == pid) {
            unplace(s->mem, s->in_vram, s->vram_offset, s->adapter);
            s->used = false;
        }
    }
    for (int i = 0; i < FGPU_MAX_BUFFERS; i++) {
        fgpu_buffer_t* b = &g_buffers[i];
        if (b->used && b->owner == pid) {
            unplace(b->mem, b->in_vram, b->vram_offset, b->adapter);
            b->used = false;
        }
    }
    for (int i = 0; i < FGPU_MAX_PROGRAMS; i++) {
        fgpu_shader_program_t* sp = &g_programs[i];
        if (sp->used && sp->owner == pid) program_free(sp);
    }
    unlock();
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
                if (g_hw && g_hw->clear(&ht, arg[0], arg[1], depth)) {
                    g_hw_pending = true;
                    break;
                }
                hw_sync();
                fgpu_raster_clear(&t, arg[0], arg[1], depth);
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
                hw_sync();
                if (ib) {
                    st->triangles += fgpu_raster_draw_layout(&t, &st->pipe, layout, verts, avail,
                                                             (const uint32_t*)ib->mem + d.first_index,
                                                             d.index_count);
                } else {
                    uint32_t count = d.vertex_count < avail ? d.vertex_count : avail;
                    st->triangles += fgpu_raster_draw_layout(&t, &st->pipe, layout, verts, avail,
                                                             NULL, count);
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
    info.caps |= FGPU_CAP_SHADERS;
    if (g_hw) info.caps |= FGPU_CAP_HW_3D;
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
    if (scanout) {
        // Must match the screen so the display can show it directly.
        gpu_device_t* screen = getSystemGPU();
        if (!a || !a->flip || !screen || r->format != FGPU_FORMAT_XRGB8888 ||
            r->width != screen->width || r->height != screen->height) return SYS_ERR_INVAL;
        pitch = screen->pitch / 4;
    }
    int slot = -1;
    for (int i = 0; i < FGPU_MAX_SURFACES; i++) if (!g_surfaces[i].used) { slot = i; break; }
    if (slot < 0) return SYS_ERR_NOMEM;
    fgpu_surface_t* s = &g_surfaces[slot];
    memset(s, 0, sizeof(*s));
    uint64_t bytes = (uint64_t)pitch * r->height * 4;
    // Depth buffers are read for every pixel: keep them in system memory
    // unless VRAM reads are cheap. Scanout surfaces must be in VRAM.
    bool cpu_reads = !scanout;
    bool allow_vram = !(r->flags & FGPU_SURFACE_SYSTEM_MEMORY);
    s->mem = place(bytes, cpu_reads, allow_vram || scanout, owner,
                   &s->in_vram, &s->vram_offset, &s->adapter);
    if (!s->mem) return SYS_ERR_NOMEM;
    if (scanout && !s->in_vram) {
        unplace(s->mem, false, 0, NULL);
        return SYS_ERR_NOMEM;
    }
    s->used = true;
    s->id = g_next_id++;
    s->owner = owner;
    s->width = r->width;
    s->height = r->height;
    s->format = r->format;
    s->flags = r->flags;
    s->pitch = pitch;
    r->handle = s->id;
    return SYS_SUCCESS;
}

static uint64_t op_create_buffer(syscall_gpu_request_t* r, uint64_t owner) {
    if (!r->bytes || r->bytes > FGPU_MAX_BUFFER_BYTES) return SYS_ERR_INVAL;
    int slot = -1;
    for (int i = 0; i < FGPU_MAX_BUFFERS; i++) if (!g_buffers[i].used) { slot = i; break; }
    if (slot < 0) return SYS_ERR_NOMEM;
    fgpu_buffer_t* b = &g_buffers[slot];
    memset(b, 0, sizeof(*b));
    b->mem = place(r->bytes, true, true, owner, &b->in_vram, &b->vram_offset, &b->adapter);
    if (!b->mem) return SYS_ERR_NOMEM;
    b->used = true;
    b->id = g_next_id++;
    b->owner = owner;
    b->size = r->bytes;
    r->handle = b->id;
    return SYS_SUCCESS;
}

static uint64_t op_present(syscall_gpu_request_t* r, uint64_t owner) {
    fgpu_surface_t* s = find_surface(r->handle, owner);
    if (!s || s->format != FGPU_FORMAT_XRGB8888) return SYS_ERR_NOTFOUND;

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
        memset(&w, 0, sizeof(w));
        w.size = sizeof(w);
        w.version = SYS_WINDOW_ABI_VERSION;
        w.op = SYS_WINDOW_PRESENT;
        w.id = r->window_id;
        w.pixels = (const uint32_t*)s->mem;
        w.pitch = s->pitch * 4;
        w.width = info.width;
        w.height = info.height;
        return window_manager_syscall(&w, owner, false);
    }

    // Screen: only for the process that owns it.
    if (!graphics_has_screen_owner() || graphics_screen_owner_pid() != owner)
        return SYS_ERR_PERM;
    if ((r->flags & FGPU_PRESENT_FLIP) && (s->flags & FGPU_SURFACE_SCANOUT) &&
        s->in_vram && s->adapter && s->adapter->flip) {
        if (!s->adapter->flip(s->adapter, s->vram_offset)) return SYS_ERR_INVAL;
        g_flipped_owner = owner;
        return SYS_SUCCESS;
    }
    if (g_flipped_owner == owner) release_flip_locked();
    gfx_surface_t src = { (uint32_t*)s->mem, (size_t)s->pitch * s->height * 4,
                          s->width, s->height, s->pitch };
    gfx_rect_t rect = { 0, 0, (int32_t)s->width, (int32_t)s->height };
    if (!graphics_backbuffer_blit(&src, &rect, r->x, r->y, false)) return SYS_ERR_INVAL;
    gfx_rect_t dst = { r->x, r->y, (int32_t)s->width, (int32_t)s->height };
    return graphics_backbuffer_present(&dst) ? SYS_SUCCESS : SYS_ERR_INVAL;
}

static uint64_t op_create_program(syscall_gpu_request_t* r, uint64_t owner) {
    if (!r->data || r->bytes < sizeof(fgpu_shader_header_t) || r->bytes > FGPU_MAX_PROGRAM_BYTES ||
        (r->bytes & 3) || !syscall_user_range_ok(r->data, (size_t)r->bytes, false))
        return SYS_ERR_INVAL;
    int slot = -1;
    for (int i = 0; i < FGPU_MAX_PROGRAMS; i++) if (!g_programs[i].used) { slot = i; break; }
    if (slot < 0) return SYS_ERR_NOMEM;
    // Check the kernel's own copy, so the program cannot change afterwards.
    uint32_t* blob = (uint32_t*)alloc_unzeroed((size_t)r->bytes);
    if (!blob) return SYS_ERR_NOMEM;
    memcpy(blob, r->data, (size_t)r->bytes);
    fgpu_shader_program_t* sp = &g_programs[slot];
    const char* why = "";
    if (!fgpu_program_check(blob, (uint32_t)r->bytes, &sp->program, &why)) {
        serial_write_str("fgpu: shader program rejected: ");
        serial_write_str(why);
        serial_write_str("\n");
        free_mem(blob);
        return SYS_ERR_INVAL;
    }
    sp->used = true;
    sp->id = g_next_id++;
    sp->owner = owner;
    sp->blob = blob;
    sp->hw = g_hw && g_hw->program_create ? g_hw->program_create(&sp->program) : NULL;
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

uint64_t fgpu_syscall(syscall_gpu_request_t* r) {
    if (!r || !syscall_user_range_ok(r, sizeof(*r), true) || r->size != sizeof(*r) ||
        r->version < SYS_GPU_ABI_VERSION_MIN || r->version > SYS_GPU_ABI_VERSION)
        return SYS_ERR_INVAL;
    uint64_t owner = caller();
    if (r->op == SYS_GPU_INFO) return op_info(r);

    // Rendering can take a while: let the timer and other processes run.
    // The syscall stub saved this process's SSE state.
    __asm__ volatile("sti");
    lock();
    // Everything but SUBMIT has the CPU read or write GPU memory (or free it).
    if (r->op != SYS_GPU_SUBMIT) hw_sync();
    uint64_t result = SYS_ERR_INVAL;
    switch (r->op) {
        case SYS_GPU_CREATE_SURFACE:
            result = op_create_surface(r, owner);
            break;
        case SYS_GPU_DESTROY_SURFACE: {
            fgpu_surface_t* s = find_surface(r->handle, owner);
            if (!s) { result = SYS_ERR_NOTFOUND; break; }
            if (g_flipped_owner == owner) release_flip_locked();
            unplace(s->mem, s->in_vram, s->vram_offset, s->adapter);
            s->used = false;
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_CREATE_BUFFER:
            result = op_create_buffer(r, owner);
            break;
        case SYS_GPU_UPLOAD_BUFFER: {
            fgpu_buffer_t* b = find_buffer(r->handle, owner);
            if (!b) { result = SYS_ERR_NOTFOUND; break; }
            if (!r->data || r->offset > b->size || r->bytes > b->size - r->offset ||
                !syscall_user_range_ok(r->data, (size_t)r->bytes, false)) break;
            memcpy((uint8_t*)b->mem + r->offset, r->data, (size_t)r->bytes);
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_DESTROY_BUFFER: {
            fgpu_buffer_t* b = find_buffer(r->handle, owner);
            if (!b) { result = SYS_ERR_NOTFOUND; break; }
            unplace(b->mem, b->in_vram, b->vram_offset, b->adapter);
            b->used = false;
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_SUBMIT: {
            if (!r->data || r->bytes < 4 || r->bytes > FGPU_MAX_COMMAND_BYTES || (r->bytes & 3) ||
                !syscall_user_range_ok(r->data, (size_t)r->bytes, false)) break;
            uint32_t* cmd = (uint32_t*)alloc_unzeroed((size_t)r->bytes);
            exec_state_t* st = (exec_state_t*)alloc(sizeof(exec_state_t));
            if (!cmd || !st) {
                if (cmd) free_mem(cmd);
                if (st) free_mem(st);
                result = SYS_ERR_NOMEM;
                break;
            }
            memcpy(cmd, r->data, (size_t)r->bytes);
            memset(st, 0, sizeof(*st));
            st->owner = owner;
            st->pipe.program = FGPU_PROGRAM_COLOR;
            st->pipe.constants = (const float (*)[4])st->constants;
            fgpu_layout_default(&st->layout);
            result = execute(st, cmd, (uint32_t)(r->bytes / 4));
            r->handle = st->triangles;
            free_mem(st);
            free_mem(cmd);
            break;
        }
        case SYS_GPU_READBACK: {
            fgpu_surface_t* s = find_surface(r->handle, owner);
            if (!s) { result = SYS_ERR_NOTFOUND; break; }
            uint64_t row = (uint64_t)s->width * 4;
            if (!r->out || r->pitch < row ||
                !syscall_user_range_ok(r->out, (size_t)r->pitch * (s->height - 1) + row, true)) break;
            for (uint32_t y = 0; y < s->height; y++) {
                memcpy((uint8_t*)r->out + (size_t)y * r->pitch,
                       (uint8_t*)s->mem + (size_t)y * s->pitch * 4, (size_t)row);
            }
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_PRESENT:
            result = op_present(r, owner);
            break;
        case SYS_GPU_RELEASE_DISPLAY:
            if (g_flipped_owner == owner) release_flip_locked();
            result = SYS_SUCCESS;
            break;
        case SYS_GPU_CREATE_PROGRAM:
            result = op_create_program(r, owner);
            break;
        case SYS_GPU_DESTROY_PROGRAM: {
            fgpu_shader_program_t* sp = find_program(r->handle, owner);
            if (!sp) { result = SYS_ERR_NOTFOUND; break; }
            program_free(sp);
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_WRITE_SURFACE:
            result = op_write_surface(r, owner);
            break;
        default:
            break;
    }
    unlock();
    __asm__ volatile("cli");
    return result;
}
