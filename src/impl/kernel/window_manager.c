#include "x86_64/window_manager.h"
#include "x86_64/allocator.h"
#include "graphics.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/spinlock.h"
#include "x86_64/uvm.h"
#include "string.h"

#define WINDOW_MIN_WIDTH 64u
#define WINDOW_MIN_HEIGHT 48u
#define WINDOW_TOTAL_FRAME_LIMIT (64u * 1024u * 1024u)
// Shared frame buffers (two per window) count against their own limit.
#define WINDOW_TOTAL_SHARED_LIMIT (128u * 1024u * 1024u)
#define WINDOW_POSITION_LIMIT 32768u
#define WINDOW_FLAG_MASK (SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_RESIZABLE | \
                          SYSCALL_WINDOW_FULLSCREEN | SYSCALL_WINDOW_MAXIMIZED | \
                          SYSCALL_WINDOW_MINIMIZED | SYSCALL_WINDOW_FOCUSED | \
                          SYSCALL_WINDOW_POINTER_LOCK)
#define WINDOW_CLIENT_FLAG_MASK (SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_RESIZABLE | \
                                 SYSCALL_WINDOW_FULLSCREEN | SYSCALL_WINDOW_MAXIMIZED | \
                                 SYSCALL_WINDOW_MINIMIZED | SYSCALL_WINDOW_POINTER_LOCK)

typedef struct {
    bool active;
    syscall_window_info_t info;
    uint32_t* pixels;               // copy path frame; NULL once the window is shared
    size_t pixel_bytes;
    // Shared frames (SYS_WINDOW_MAP_FRAME): one object with two buffers.
    // The window holds one reference; every mapping holds its own, so a
    // replaced object lives on until the last space unmaps it.
    uvm_shm_t* shm;
    size_t shm_bytes;
    uint32_t buf_stride;            // bytes between the two buffers
    uint32_t front;                 // buffer last presented
    uint32_t generation;            // bumps whenever `shm` is replaced
    // Area changed since the manager last asked (damage_w == 0: none).
    uint32_t damage_x, damage_y, damage_w, damage_h;
    syscall_window_event_t events[SYSCALL_WINDOW_MAX_EVENTS];
    uint32_t event_head;
    uint32_t event_count;
} managed_window_t;

static managed_window_t g_windows[SYSCALL_WINDOW_MAX_WINDOWS];
static uint32_t g_next_window_id = 1;
// Generations are global, so a reused window id never repeats one.
static uint32_t g_shared_generation;
static uint32_t g_focused_window;
static uint64_t g_window_manager_pid;
static size_t g_frame_bytes;
static size_t g_shared_bytes;
static spinlock_t g_window_lock = SPINLOCK_INIT;

static managed_window_t* find_window(uint32_t id) {
    if (!id) return NULL;
    for (uint32_t i = 0; i < SYSCALL_WINDOW_MAX_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].info.id == id)
            return &g_windows[i];
    }
    return NULL;
}

static process_t* find_process(uint64_t pid) {
    for (process_t* process = proc_list_head; process; process = process->next) {
        if (process->pid == pid &&
            process->state != PROCESS_ZOMBIE &&
            process->state != PROCESS_TERMINATED)
            return process;
    }
    return NULL;
}

static bool process_is_descendant(uint64_t candidate_pid,
                                  uint64_t ancestor_pid) {
    if (!candidate_pid || !ancestor_pid || candidate_pid == ancestor_pid)
        return false;

    bool descendant = false;
    uint64_t flags = scheduler_lock();
    process_t* process = find_process(candidate_pid);
    for (uint32_t depth = 0; process && process->parent_pid &&
         depth < 64; depth++) {
        if (process->parent_pid == ancestor_pid) {
            descendant = true;
            break;
        }
        process = find_process(process->parent_pid);
    }
    scheduler_unlock(flags);
    return descendant;
}

static bool may_control(const managed_window_t* window, uint64_t caller_pid) {
    return window && (caller_pid == window->info.owner_pid ||
        process_is_descendant(window->info.owner_pid, caller_pid));
}

static void copy_window_info(syscall_window_info_t* out,
                             const managed_window_t* window) {
    *out = window->info;
}

// The frame the manager sees: the last presented shared buffer, or the
// copy-path frame.
static uint32_t* front_pixels(const managed_window_t* window) {
    if (window->shm)
        return (uint32_t*)((uint8_t*)uvm_shm_base(window->shm) +
                           (size_t)window->front * window->buf_stride);
    return window->pixels;
}

static void damage_add(managed_window_t* window, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h) {
    if (!window->damage_w || !window->damage_h) {
        window->damage_x = x; window->damage_y = y;
        window->damage_w = w; window->damage_h = h;
        return;
    }
    uint32_t x2 = window->damage_x + window->damage_w;
    uint32_t y2 = window->damage_y + window->damage_h;
    if (x < window->damage_x) window->damage_x = x;
    if (y < window->damage_y) window->damage_y = y;
    if (x + w > x2) x2 = x + w;
    if (y + h > y2) y2 = y + h;
    window->damage_w = x2 - window->damage_x;
    window->damage_h = y2 - window->damage_y;
}

// Drops the window's references on its shared frames (mappings keep theirs).
static void shared_release(managed_window_t* window) {
    if (window->shm) uvm_shm_put(window->shm);
    window->shm = NULL;
    g_shared_bytes -= window->shm_bytes;
    window->shm_bytes = 0;
}

// Frees everything a window holds; the caller clears the slot.
static void window_free(managed_window_t* window) {
    g_frame_bytes -= window->pixel_bytes;
    if (window->pixels) free_mem(window->pixels);
    window->pixels = NULL;
    shared_release(window);
}

// A new object for a width x height window: two buffers, zeroed. Sets
// *stride and *bytes. NULL when over the limit or out of contiguous memory.
static uvm_shm_t* shared_alloc(uint32_t width, uint32_t height, size_t old_bytes,
                               uint32_t* stride, size_t* bytes) {
    size_t row = (size_t)width * sizeof(uint32_t);
    size_t buffer = (row * height + 4095u) & ~(size_t)4095u;
    size_t total = buffer * SYSCALL_WINDOW_SHARED_BUFFERS;
    if (total > WINDOW_TOTAL_SHARED_LIMIT - (g_shared_bytes - old_bytes))
        return NULL;
    uvm_shm_t* shm = uvm_shm_create(total);
    if (!shm) return NULL;
    *stride = (uint32_t)buffer;
    *bytes = total;
    return shm;
}

// Moves a copy-path window to shared frames: its pixels become buffer 0.
static uint64_t enter_shared(managed_window_t* window) {
    uint32_t stride;
    size_t bytes;
    uvm_shm_t* shm = shared_alloc(window->info.width, window->info.height, 0,
                                  &stride, &bytes);
    if (!shm) return SYS_ERR_NOMEM;
    memcpy(uvm_shm_base(shm), window->pixels, window->pixel_bytes);
    g_frame_bytes -= window->pixel_bytes;
    free_mem(window->pixels);
    window->pixels = NULL;
    window->pixel_bytes = 0;
    window->shm = shm;
    window->shm_bytes = bytes;
    g_shared_bytes += bytes;
    window->buf_stride = stride;
    window->front = 0;
    window->generation = ++g_shared_generation;
    damage_add(window, 0, 0, window->info.width, window->info.height);
    return SYS_SUCCESS;
}

static bool push_event(managed_window_t* window,
                       const syscall_window_event_t* event) {
    if (event->type == SYSCALL_WINDOW_EVENT_RESIZED ||
        event->type == SYSCALL_WINDOW_EVENT_MINIMIZED ||
        event->type == SYSCALL_WINDOW_EVENT_MAXIMIZED ||
        event->type == SYSCALL_WINDOW_EVENT_RESTORED ||
        event->type == SYSCALL_WINDOW_EVENT_FOCUS_GAINED ||
        event->type == SYSCALL_WINDOW_EVENT_FOCUS_LOST ||
        event->type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) {
        for (uint32_t i = 0; i < window->event_count; i++) {
            uint32_t index = (window->event_head + i) %
                             SYSCALL_WINDOW_MAX_EVENTS;
            if (window->events[index].type == event->type) {
                window->events[index] = *event;
                window->events[index].id = window->info.id;
                return true;
            }
        }
    }
    // Pointer motion only replaces the newest queued event, and only when
    // the buttons are unchanged. Merging across a button change would let a
    // quick click (press, then release) reach the application as a release
    // alone.
    if (event->type == SYSCALL_WINDOW_EVENT_MOUSE && window->event_count) {
        uint32_t newest = (window->event_head + window->event_count - 1u) %
                          SYSCALL_WINDOW_MAX_EVENTS;
        if (window->events[newest].type == SYSCALL_WINDOW_EVENT_MOUSE &&
            window->events[newest].buttons == event->buttons) {
            window->events[newest] = *event;
            window->events[newest].id = window->info.id;
            return true;
        }
    }
    if (window->event_count == SYSCALL_WINDOW_MAX_EVENTS) return false;
    uint32_t index = (window->event_head + window->event_count) %
                     SYSCALL_WINDOW_MAX_EVENTS;
    window->events[index] = *event;
    window->events[index].id = window->info.id;
    window->event_count++;
    return true;
}

static bool dimensions_ok(uint32_t width, uint32_t height, size_t* bytes) {
    if (width < WINDOW_MIN_WIDTH || height < WINDOW_MIN_HEIGHT ||
        width > SYSCALL_GFX_MAX_DIMENSION ||
        height > SYSCALL_GFX_MAX_DIMENSION ||
        (size_t)width > SIZE_MAX / height / sizeof(uint32_t))
        return false;
    *bytes = (size_t)width * height * sizeof(uint32_t);
    return *bytes <= SYSCALL_GFX_MAX_BUFFER_BYTES;
}

static bool title_is_terminated(const char* title) {
    for (uint32_t i = 0; i < SYSCALL_WINDOW_MAX_TITLE; i++) {
        if (!title[i]) return true;
    }
    return false;
}

static syscall_window_style_t default_style(void) {
    return (syscall_window_style_t){
        .title_bar = 0xFF24343Cu,
        .title_text = 0xFFFFFFFFu,
        .close_button = 0xFFE35D6Au,
        .minimize_button = 0xFFE8B34Cu,
        .maximize_button = 0xFF4ABF8Au,
        .close_icon = 0xFFFFFFFFu,
        .minimize_icon = 0xFF17262Cu,
        .maximize_icon = 0xFF17262Cu,
        .border = 0xFF49616Bu,
        .content_background = 0xFF17232Au
    };
}

static uint64_t resize_window(managed_window_t* window, uint32_t width,
                              uint32_t height) {
    size_t new_bytes;
    if (!dimensions_ok(width, height, &new_bytes)) return SYS_ERR_INVAL;
    if (window->event_count == SYSCALL_WINDOW_MAX_EVENTS)
        return SYS_ERR_BUSY;
    uint32_t copy_width = width < window->info.width ? width : window->info.width;
    uint32_t copy_height = height < window->info.height ? height : window->info.height;

    if (window->shm) {
        // Shared frames are replaced, never resized in place: mappings of
        // the old object stay valid (and keep their old size) until their
        // owners unmap them; MAP_FRAME again gives the new ones. The old
        // front buffer is copied into the new buffer 0.
        uint32_t stride;
        size_t bytes;
        uvm_shm_t* shm = shared_alloc(width, height, window->shm_bytes, &stride, &bytes);
        if (!shm) return SYS_ERR_NOMEM;
        for (uint32_t y = 0; y < copy_height; y++) {
            memcpy((uint8_t*)uvm_shm_base(shm) + (size_t)y * width * sizeof(uint32_t),
                   front_pixels(window) + (size_t)y * window->info.width,
                   (size_t)copy_width * sizeof(uint32_t));
        }
        shared_release(window);
        window->shm = shm;
        window->shm_bytes = bytes;
        g_shared_bytes += bytes;
        window->buf_stride = stride;
        window->front = 0;
        window->generation = ++g_shared_generation;
        window->damage_w = window->damage_h = 0;
        damage_add(window, 0, 0, width, height);
    } else {
        if (new_bytes > WINDOW_TOTAL_FRAME_LIMIT -
                        (g_frame_bytes - window->pixel_bytes))
            return SYS_ERR_GENERIC;
        uint32_t* pixels = alloc(new_bytes);
        if (!pixels) return SYS_ERR_GENERIC;
        for (uint32_t y = 0; y < copy_height; y++) {
            memcpy(pixels + (size_t)y * width,
                   window->pixels + (size_t)y * window->info.width,
                   (size_t)copy_width * sizeof(uint32_t));
        }
        free_mem(window->pixels);
        g_frame_bytes = g_frame_bytes - window->pixel_bytes + new_bytes;
        window->pixels = pixels;
        window->pixel_bytes = new_bytes;
        window->damage_w = window->damage_h = 0;
        damage_add(window, 0, 0, width, height);
    }
    window->info.width = width;
    window->info.height = height;

    syscall_window_event_t event = {
        .type = SYSCALL_WINDOW_EVENT_RESIZED,
        .width = (int32_t)width,
        .height = (int32_t)height
    };
    if (!push_event(window, &event)) return SYS_ERR_BUSY;
    return SYS_SUCCESS;
}

static uint64_t create_window(const syscall_window_request_t* request,
                              uint64_t caller_pid) {
    size_t bytes;
    if (!caller_pid || request->x > WINDOW_POSITION_LIMIT ||
        request->y > WINDOW_POSITION_LIMIT ||
        !dimensions_ok(request->width, request->height, &bytes) ||
        request->flags & ~WINDOW_CLIENT_FLAG_MASK ||
        ((request->flags & SYSCALL_WINDOW_FULLSCREEN) &&
         (request->flags & SYSCALL_WINDOW_MAXIMIZED)) ||
        !title_is_terminated(request->title))
        return SYS_ERR_INVAL;

    uint64_t flags = spinlock_acquire(&g_window_lock);
    managed_window_t* parent = request->parent_id
        ? find_window(request->parent_id) : NULL;
    if (request->parent_id &&
        (!parent || !may_control(parent, caller_pid))) {
        spinlock_release(&g_window_lock, flags);
        return parent ? SYS_ERR_PERM : SYS_ERR_NOTFOUND;
    }

    managed_window_t* slot = NULL;
    for (uint32_t i = 0; i < SYSCALL_WINDOW_MAX_WINDOWS; i++) {
        if (!g_windows[i].active) {
            slot = &g_windows[i];
            break;
        }
    }
    if (!slot || bytes > WINDOW_TOTAL_FRAME_LIMIT - g_frame_bytes) {
        spinlock_release(&g_window_lock, flags);
        return SYS_ERR_GENERIC;
    }

    uint32_t* pixels = alloc(bytes);
    if (!pixels) {
        spinlock_release(&g_window_lock, flags);
        return SYS_ERR_GENERIC;
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->pixels = pixels;
    slot->pixel_bytes = bytes;
    slot->generation = 0;
    while (find_window(g_next_window_id)) {
        g_next_window_id = (g_next_window_id % SYSCALL_WINDOW_MAX_WINDOWS) + 1;
    }
    slot->info.id = g_next_window_id;
    g_next_window_id = (g_next_window_id % SYSCALL_WINDOW_MAX_WINDOWS) + 1;
    slot->info.parent_id = request->parent_id;
    slot->info.x = request->x;
    slot->info.y = request->y;
    slot->info.width = request->width;
    slot->info.height = request->height;
    slot->info.flags = request->flags;
    slot->info.owner_pid = caller_pid;
    slot->info.style = default_style();
    if (request->title[0]) {
        memcpy(slot->info.title, request->title, sizeof(slot->info.title));
    }
    g_frame_bytes += bytes;
    uint32_t id = slot->info.id;
    spinlock_release(&g_window_lock, flags);
    return id;
}

static bool set_focus(managed_window_t* window) {
    if (g_focused_window == window->info.id) return true;
    managed_window_t* old = find_window(g_focused_window);
    syscall_window_event_t lost = {
        .type = SYSCALL_WINDOW_EVENT_FOCUS_LOST
    };
    syscall_window_event_t gained = {
        .type = SYSCALL_WINDOW_EVENT_FOCUS_GAINED
    };
    if (old && old->event_count == SYSCALL_WINDOW_MAX_EVENTS) return false;
    if (window->event_count == SYSCALL_WINDOW_MAX_EVENTS) return false;
    if (old && !push_event(old, &lost)) return false;
    if (!push_event(window, &gained)) {
        if (old && old->event_count) old->event_count--;
        return false;
    }
    if (old) old->info.flags &= ~SYSCALL_WINDOW_FOCUSED;
    window->info.flags |= SYSCALL_WINDOW_FOCUSED;
    g_focused_window = window->info.id;
    return true;
}

uint64_t window_manager_syscall(const syscall_window_request_t* request,
                                uint64_t caller_pid, bool is_window_manager) {
    if (!request || request->size != sizeof(*request) ||
        request->version != SYS_WINDOW_ABI_VERSION)
        return SYS_ERR_INVAL;

    if (request->op == SYS_WINDOW_REGISTER_MANAGER) {
        if (!is_window_manager ||
            graphics_screen_owner_pid() != caller_pid)
            return SYS_ERR_PERM;
        uint64_t flags = spinlock_acquire(&g_window_lock);
        if (g_window_manager_pid && g_window_manager_pid != caller_pid) {
            spinlock_release(&g_window_lock, flags);
            return SYS_ERR_BUSY;
        }
        g_window_manager_pid = caller_pid;
        spinlock_release(&g_window_lock, flags);
        return SYS_SUCCESS;
    }

    uint64_t lock_flags = spinlock_acquire(&g_window_lock);
    bool is_manager = caller_pid && caller_pid == g_window_manager_pid &&
                      is_window_manager;

    if (request->op == SYS_WINDOW_CREATE) {
        spinlock_release(&g_window_lock, lock_flags);
        return create_window(request, caller_pid);
    }

    if (request->op == SYS_WINDOW_LIST) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < SYSCALL_WINDOW_MAX_WINDOWS; i++) {
            if (!g_windows[i].active) continue;
            if (!is_manager && !may_control(&g_windows[i], caller_pid))
                continue;
            if (count < request->capacity)
                copy_window_info(&request->windows[count], &g_windows[i]);
            count++;
        }
        spinlock_release(&g_window_lock, lock_flags);
        return count;
    }

    managed_window_t* window = find_window(request->id);
    if (!window) {
        spinlock_release(&g_window_lock, lock_flags);
        return SYS_ERR_NOTFOUND;
    }
    if (!is_manager && !may_control(window, caller_pid)) {
        spinlock_release(&g_window_lock, lock_flags);
        return SYS_ERR_PERM;
    }

    uint64_t result = SYS_SUCCESS;
    switch (request->op) {
        case SYS_WINDOW_CLOSE:
            window_free(window);
            if (g_focused_window == window->info.id) g_focused_window = 0;
            memset(window, 0, sizeof(*window));
            break;
        case SYS_WINDOW_SET_BOUNDS:
            if (!is_manager && !(window->info.flags & SYSCALL_WINDOW_RESIZABLE)) {
                result = SYS_ERR_PERM;
                break;
            }
            if (request->x > WINDOW_POSITION_LIMIT ||
                request->y > WINDOW_POSITION_LIMIT) {
                result = SYS_ERR_INVAL;
                break;
            }
            if (request->width != window->info.width ||
                request->height != window->info.height) {
                result = resize_window(window, request->width,
                                       request->height);
                if (result != SYS_SUCCESS) break;
            }
            window->info.x = request->x;
            window->info.y = request->y;
            break;
        case SYS_WINDOW_SET_TITLE:
            if (!title_is_terminated(request->title)) {
                result = SYS_ERR_INVAL;
                break;
            }
            memcpy(window->info.title, request->title,
                   sizeof(window->info.title));
            break;
        case SYS_WINDOW_SET_FLAGS: {
            if (request->flags & ~WINDOW_FLAG_MASK ||
                (!is_manager && (request->flags &
                                 SYSCALL_WINDOW_FOCUSED)) ||
                ((request->flags & SYSCALL_WINDOW_FULLSCREEN) &&
                 (request->flags & SYSCALL_WINDOW_MAXIMIZED))) {
                result = SYS_ERR_INVAL;
                break;
            }
            if (is_manager) {
                if (request->flags & SYSCALL_WINDOW_FOCUSED) {
                    if (!set_focus(window)) {
                        result = SYS_ERR_BUSY;
                        break;
                    }
                } else if (g_focused_window == window->info.id) {
                    window->info.flags &= ~SYSCALL_WINDOW_FOCUSED;
                    g_focused_window = 0;
                }
            }
            window->info.flags =
                (request->flags & ~SYSCALL_WINDOW_FOCUSED) |
                (window->info.flags & SYSCALL_WINDOW_FOCUSED);
            break;
        }
        case SYS_WINDOW_SET_STYLE:
            window->info.style = request->style;
            break;
        case SYS_WINDOW_PRESENT: {
            // The manager composites other processes' windows but must not
            // draw into them; it may still present its own (File Browser).
            if ((is_manager && window->info.owner_pid != caller_pid) ||
                !request->pixels ||
                !request->width || !request->height ||
                request->x > window->info.width ||
                request->y > window->info.height ||
                request->width > window->info.width - request->x ||
                request->height > window->info.height - request->y ||
                request->pitch < window->info.width * sizeof(uint32_t) ||
                request->pitch > SYSCALL_GFX_MAX_BUFFER_BYTES) {
                result = SYS_ERR_INVAL;
                break;
            }
            const uint32_t* source = request->pixels;
            uint32_t* frame_pixels = front_pixels(window);
            for (uint32_t y = request->y;
                 y < request->y + request->height; y++) {
                memcpy(frame_pixels + (size_t)y * window->info.width +
                           request->x,
                       (const uint8_t*)source + (size_t)y * request->pitch +
                           (size_t)request->x * sizeof(uint32_t),
                       (size_t)request->width * sizeof(uint32_t));
            }
            window->info.frame_version++;
            damage_add(window, request->x, request->y, request->width,
                       request->height);
            break;
        }
        case SYS_WINDOW_MAP_FRAME: {
            syscall_window_shared_t* out = request->shared;
            bool owner = caller_pid == window->info.owner_pid;
            process_t* me = current_process;
            if (!out || !me || me->privilege != PROC_PRIVILEGE_USER) {
                result = SYS_ERR_INVAL;
                break;
            }
            // The owner draws (RW); the manager reads (RO). Nobody else.
            if (!owner && !is_manager) {
                result = SYS_ERR_PERM;
                break;
            }
            if (!window->shm) {
                // The manager can not turn another program's window into a
                // shared one; it keeps using GET_FRAME for it.
                result = owner ? enter_shared(window) : SYS_ERR_AGAIN;
                if (result != SYS_SUCCESS) break;
            }
            memset(out, 0, sizeof(*out));
            out->buffer_stride = window->buf_stride;
            out->width = window->info.width;
            out->height = window->info.height;
            out->pitch = window->info.width * sizeof(uint32_t);
            out->buffer_count = SYSCALL_WINDOW_SHARED_BUFFERS;
            out->generation = window->generation;
            out->front = window->front;
            out->frame_version = window->info.frame_version;
            // Map outside the window lock (it may touch many pages); our
            // reference keeps the object alive even if the window is
            // resized or closed meanwhile.
            uvm_shm_t* shm = uvm_shm_get(window->shm);
            spinlock_release(&g_window_lock, lock_flags);
            uint64_t address = uvm_map_shm(me, shm, 0,
                owner ? (UVM_PROT_READ | UVM_PROT_WRITE) : UVM_PROT_READ);
            uvm_shm_put(shm);
            if (address >= (uint64_t)-16) return address;
            out->address = address;
            return SYS_SUCCESS;
        }
        case SYS_WINDOW_PRESENT_SHARED: {
            if (caller_pid != window->info.owner_pid) {
                result = SYS_ERR_PERM;
                break;
            }
            uint32_t w = request->width, h = request->height;
            uint32_t x = request->x, y = request->y;
            if (!w && !h) {              // no rectangle: the whole window
                x = y = 0;
                w = window->info.width;
                h = window->info.height;
            }
            if (!window->shm || request->flags >= SYSCALL_WINDOW_SHARED_BUFFERS ||
                !w || !h || x > window->info.width || y > window->info.height ||
                w > window->info.width - x || h > window->info.height - y) {
                result = SYS_ERR_INVAL;
                break;
            }
            window->front = request->flags;
            window->info.frame_version++;
            damage_add(window, x, y, w, h);
            break;
        }
        case SYS_WINDOW_TAKE_DAMAGE: {
            syscall_window_shared_t* out = request->shared;
            if (!is_manager) {
                result = SYS_ERR_PERM;
                break;
            }
            if (!out) {
                result = SYS_ERR_INVAL;
                break;
            }
            memset(out, 0, sizeof(*out));
            out->buffer_stride = window->buf_stride;
            out->width = window->info.width;
            out->height = window->info.height;
            out->pitch = window->info.width * sizeof(uint32_t);
            out->buffer_count = window->shm ? SYSCALL_WINDOW_SHARED_BUFFERS : 0;
            out->generation = window->generation;
            out->front = window->front;
            out->frame_version = window->info.frame_version;
            out->damage_x = window->damage_x;
            out->damage_y = window->damage_y;
            out->damage_width = window->damage_w;
            out->damage_height = window->damage_h;
            window->damage_w = window->damage_h = 0;
            break;
        }
        case SYS_WINDOW_GET_INFO:
            if (!request->info) result = SYS_ERR_INVAL;
            else copy_window_info(request->info, window);
            break;
        case SYS_WINDOW_GET_FRAME: {
            syscall_window_frame_t* frame = request->frame;
            if (!is_manager || !frame || !frame->pixels ||
                frame->width != window->info.width ||
                frame->height != window->info.height ||
                frame->pitch < window->info.width * sizeof(uint32_t) ||
                frame->pitch > SYSCALL_GFX_MAX_BUFFER_BYTES) {
                result = is_manager ? SYS_ERR_INVAL : SYS_ERR_PERM;
                break;
            }
            const uint32_t* source_pixels = front_pixels(window);
            for (uint32_t y = 0; y < frame->height; y++) {
                memcpy((uint8_t*)frame->pixels + (size_t)y * frame->pitch,
                       source_pixels + (size_t)y * window->info.width,
                       (size_t)window->info.width * sizeof(uint32_t));
            }
            break;
        }
        case SYS_WINDOW_NEXT_EVENT:
            if (!request->event) {
                result = SYS_ERR_INVAL;
            } else if (!window->event_count) {
                result = SYS_ERR_AGAIN;
            } else {
                *request->event = window->events[window->event_head];
                window->event_head =
                    (window->event_head + 1u) % SYSCALL_WINDOW_MAX_EVENTS;
                window->event_count--;
            }
            break;
        case SYS_WINDOW_POST_EVENT: {
            if (!is_manager || !request->event ||
                request->event->type < SYSCALL_WINDOW_EVENT_RESIZED ||
                request->event->type > SYSCALL_WINDOW_EVENT_MOUSE_RELATIVE) {
                result = is_manager ? SYS_ERR_INVAL : SYS_ERR_PERM;
                break;
            }
            result = push_event(window, request->event)
                ? SYS_SUCCESS : SYS_ERR_BUSY;
            break;
        }
        default:
            result = SYS_ERR_INVAL;
            break;
    }
    spinlock_release(&g_window_lock, lock_flags);
    return result;
}

void window_manager_process_exit(uint64_t pid) {
    uint64_t flags = spinlock_acquire(&g_window_lock);
    if (g_window_manager_pid == pid) g_window_manager_pid = 0;
    for (uint32_t i = 0; i < SYSCALL_WINDOW_MAX_WINDOWS; i++) {
        managed_window_t* window = &g_windows[i];
        if (!window->active || window->info.owner_pid != pid) continue;
        window_free(window);
        if (g_focused_window == window->info.id) g_focused_window = 0;
        memset(window, 0, sizeof(*window));
    }
    spinlock_release(&g_window_lock, flags);
}

bool window_manager_present_target(uint32_t window_id, uint64_t owner_pid,
                                   window_present_target_t* target) {
    uint64_t flags = spinlock_acquire(&g_window_lock);
    managed_window_t* window = find_window(window_id);
    bool ok = false;
    if (window && window->info.owner_pid == owner_pid) {
        // A copy-path window becomes a shared one here (its pixels move to
        // buffer 0); when that is not possible the caller copies instead.
        if (window->shm || enter_shared(window) == SYS_SUCCESS) {
            target->buffer = window->front ^ 1u;
            // The caller's own reference: a resize or close meanwhile
            // drops the window's, not this one.
            target->pin = uvm_shm_get(window->shm);
            target->pixels = (uint32_t*)((uint8_t*)uvm_shm_base(window->shm) +
                                         (size_t)target->buffer * window->buf_stride);
            target->pitch_px = window->info.width;
            target->width = window->info.width;
            target->height = window->info.height;
            ok = true;
        }
    }
    spinlock_release(&g_window_lock, flags);
    return ok;
}

uint64_t window_manager_present_done(uint32_t window_id, uint64_t owner_pid,
                                     window_present_target_t* target,
                                     uint32_t x, uint32_t y, uint32_t w,
                                     uint32_t h) {
    uint64_t flags = spinlock_acquire(&g_window_lock);
    managed_window_t* window = find_window(window_id);
    uint64_t result;
    if (!window || window->info.owner_pid != owner_pid) {
        result = SYS_ERR_NOTFOUND;
    } else if (window->shm != (uvm_shm_t*)target->pin) {
        // Resized while the caller was drawing: the buffer it filled
        // belongs to the old object, nothing is presented.
        result = SYS_ERR_AGAIN;
    } else if (!w && !h) {
        result = SYS_SUCCESS;                   // abandoned
    } else if (!w || !h || x > window->info.width || y > window->info.height ||
               w > window->info.width - x || h > window->info.height - y) {
        result = SYS_ERR_INVAL;
    } else {
        window->front = target->buffer;
        window->info.frame_version++;
        damage_add(window, x, y, w, h);
        result = SYS_SUCCESS;
    }
    spinlock_release(&g_window_lock, flags);
    uvm_shm_put((uvm_shm_t*)target->pin);       // the caller's reference, always
    target->pin = NULL;
    return result;
}
