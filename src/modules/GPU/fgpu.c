#include "x86_64/fgpu.h"
#include "x86_64/fgpu_raster.h"
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

/*
 * FGPU command processor (SYS_GPU). Owns surfaces and buffers per
 * process, runs submitted command buffers on the CPU backend
 * (fgpu_raster.c) and presents finished frames to the screen (copy or
 * page flip) or to a window.
 */

#define FGPU_MAX_SURFACES 64
#define FGPU_MAX_BUFFERS 128

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

static fgpu_surface_t g_surfaces[FGPU_MAX_SURFACES];
static fgpu_buffer_t g_buffers[FGPU_MAX_BUFFERS];
static uint32_t g_next_id = 1;
static volatile int g_busy;
static uint64_t g_flipped_owner;        // pid whose surface is on screen, 0 = none

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
    unlock();
}

/* ------------------------------------------------------------ commands */

typedef struct {
    fgpu_surface_t* color;
    fgpu_surface_t* depth;
    fgpu_pipeline_t pipe;
    uint64_t owner;
    uint32_t triangles;
} exec_state_t;

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
            case FGPU_OP_DRAW: {
                if (n * 4 < sizeof(fgpu_draw_t)) return SYS_ERR_INVAL;
                fgpu_draw_t d;
                memcpy(&d, arg, sizeof(d));
                if (d.vertex_format != FGPU_VERTEX_P3N3C4) return SYS_ERR_INVAL;
                fgpu_buffer_t* vb = find_buffer(d.vertex_buffer, st->owner);
                if (!vb) return SYS_ERR_NOTFOUND;
                uint64_t stride = 10 * sizeof(float);
                uint64_t total = vb->size / stride;
                if (d.first_vertex > total) return SYS_ERR_INVAL;
                const float* verts = (const float*)vb->mem + (uint64_t)d.first_vertex * 10;
                uint32_t avail = (uint32_t)(total - d.first_vertex);
                fgpu_target_t t;
                if (!make_target(st, &t)) return SYS_ERR_INVAL;
                if (!st->pipe.program) st->pipe.program = FGPU_PROGRAM_COLOR;
                if (d.index_buffer) {
                    fgpu_buffer_t* ib = find_buffer(d.index_buffer, st->owner);
                    if (!ib) return SYS_ERR_NOTFOUND;
                    uint64_t icount = ib->size / 4;
                    if (d.first_index > icount || d.index_count > icount - d.first_index)
                        return SYS_ERR_INVAL;
                    st->triangles += fgpu_raster_draw(&t, &st->pipe, verts, avail,
                                                      (const uint32_t*)ib->mem + d.first_index,
                                                      d.index_count);
                } else {
                    uint32_t count = d.vertex_count < avail ? d.vertex_count : avail;
                    st->triangles += fgpu_raster_draw(&t, &st->pipe, verts, avail, NULL, count);
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
    strcpy(info.backend, "cpu (FGPU software)");
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

uint64_t fgpu_syscall(syscall_gpu_request_t* r) {
    if (!r || !syscall_user_range_ok(r, sizeof(*r), true) || r->size != sizeof(*r) ||
        r->version != SYS_GPU_ABI_VERSION) return SYS_ERR_INVAL;
    uint64_t owner = caller();
    if (r->op == SYS_GPU_INFO) return op_info(r);

    // Rendering can take a while: let the timer and other processes run.
    // The syscall stub saved this process's SSE state.
    __asm__ volatile("sti");
    lock();
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
            if (!cmd) { result = SYS_ERR_NOMEM; break; }
            memcpy(cmd, r->data, (size_t)r->bytes);
            exec_state_t st;
            memset(&st, 0, sizeof(st));
            st.owner = owner;
            st.pipe.program = FGPU_PROGRAM_COLOR;
            result = execute(&st, cmd, (uint32_t)(r->bytes / 4));
            r->handle = st.triangles;
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
        default:
            break;
    }
    unlock();
    __asm__ volatile("cli");
    return result;
}
