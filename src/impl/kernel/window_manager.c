#include "x86_64/window_manager.h"
#include "x86_64/allocator.h"
#include "graphics.h"
#include "x86_64/proc.h"
#include "x86_64/scheduler.h"
#include "x86_64/spinlock.h"
#include "string.h"

#define WINDOW_MIN_WIDTH 64u
#define WINDOW_MIN_HEIGHT 48u
#define WINDOW_TOTAL_FRAME_LIMIT (64u * 1024u * 1024u)
#define WINDOW_POSITION_LIMIT 32768u
#define WINDOW_FLAG_MASK (SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_RESIZABLE | \
                          SYSCALL_WINDOW_FULLSCREEN | SYSCALL_WINDOW_MAXIMIZED | \
                          SYSCALL_WINDOW_MINIMIZED | SYSCALL_WINDOW_FOCUSED)
#define WINDOW_CLIENT_FLAG_MASK (SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_RESIZABLE | \
                                 SYSCALL_WINDOW_FULLSCREEN | SYSCALL_WINDOW_MAXIMIZED | \
                                 SYSCALL_WINDOW_MINIMIZED)

typedef struct {
    bool active;
    syscall_window_info_t info;
    uint32_t* pixels;
    size_t pixel_bytes;
    syscall_window_event_t events[SYSCALL_WINDOW_MAX_EVENTS];
    uint32_t event_head;
    uint32_t event_count;
} managed_window_t;

static managed_window_t g_windows[SYSCALL_WINDOW_MAX_WINDOWS];
static uint32_t g_next_window_id = 1;
static uint32_t g_focused_window;
static uint64_t g_window_manager_pid;
static size_t g_frame_bytes;
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
    if (new_bytes > WINDOW_TOTAL_FRAME_LIMIT -
                    (g_frame_bytes - window->pixel_bytes))
        return SYS_ERR_GENERIC;

    uint32_t* pixels = alloc(new_bytes);
    if (!pixels) return SYS_ERR_GENERIC;
    uint32_t copy_width = width < window->info.width ? width : window->info.width;
    uint32_t copy_height = height < window->info.height ? height : window->info.height;
    for (uint32_t y = 0; y < copy_height; y++) {
        memcpy(pixels + (size_t)y * width,
               window->pixels + (size_t)y * window->info.width,
               (size_t)copy_width * sizeof(uint32_t));
    }

    free_mem(window->pixels);
    g_frame_bytes = g_frame_bytes - window->pixel_bytes + new_bytes;
    window->pixels = pixels;
    window->pixel_bytes = new_bytes;
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
            g_frame_bytes -= window->pixel_bytes;
            free_mem(window->pixels);
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
            for (uint32_t y = request->y;
                 y < request->y + request->height; y++) {
                memcpy(window->pixels + (size_t)y * window->info.width +
                           request->x,
                       (const uint8_t*)source + (size_t)y * request->pitch +
                           (size_t)request->x * sizeof(uint32_t),
                       (size_t)request->width * sizeof(uint32_t));
            }
            window->info.frame_version++;
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
            for (uint32_t y = 0; y < frame->height; y++) {
                memcpy((uint8_t*)frame->pixels + (size_t)y * frame->pitch,
                       window->pixels + (size_t)y * window->info.width,
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
                request->event->type > SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
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
        g_frame_bytes -= window->pixel_bytes;
        free_mem(window->pixels);
        if (g_focused_window == window->info.id) g_focused_window = 0;
        memset(window, 0, sizeof(*window));
    }
    spinlock_release(&g_window_lock, flags);
}
