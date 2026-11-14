#include "x86_64/syscall.h"
#include "x86_64/cpu_simd.h"
#include "x86_64/wm_watchdog.h"
#include "x86_64/acpi.h"
#include "graphics.h"
#include "serial.h"
#include "prochandler.h"
#include "x86_64/scheduler.h"
#include "x86_64/minimafs.h"
#include "x86_64/gpu_manager.h"
#include "x86_64/fgpu.h"
#include "x86_64/audio_manager.h"
#include "x86_64/storage_manager.h"
#include "x86_64/usb_manager.h"
#include "usb/usb_stack.h"
#include "keyboard/usbkeyboard.h"
#include "time.h"
#include "x86_64/allocator.h"
#include "x86_64/pkglib.h"
#include "usb/usb_stack.h"
#include "keyboard/usbkeyboard.h"
#include "mouse/usbmouse.h"
#include "string.h"

#include "x86_64/smp.h"
#include "x86_64/boot_options.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "x86_64/globaldatatable.h"
#include "x86_64/net_syscall.h"
#include "net/timesync.h"
#include "x86_64/root.h"
#include "x86_64/random.h"
#include "x86_64/runcommand.h"
#include "x86_64/proc.h"
#include "fspaths.h"
#include "input.h"
#include "x86_64/window_manager.h"
#include "x86_64/spawn.h"
#include "x86_64/uvm.h"
#include "x86_64/waitq.h"
#include "x86_64/clock.h"
#include "timeconv.h"
#include "x86_64/user_ptr.h"

// spawn.c: length of a NUL-terminated string entirely inside the calling
// program's memory (at most `max` bytes, NUL included).
bool proc_user_strlen(const char* s, size_t max, size_t* out_len);

static minimafs_dir_entry_t g_listdir_entries[MINIMAFS_MAX_ROOT_ENTRIES];
static volatile int     g_listdir_entries_lock;

// Programs resolve relative paths against their own working directory
// (SYS_CHDIR, inherited through SYS_SPAWN), or the terminal's when they
// never set one; kernel callers always use the terminal's.
static const char* syscall_resolve_path(const char* input, char* resolved) {
    if (!input || !resolved) return NULL;
    // Every path syscall goes through here: the string must be the
    // program's own and fit a path.
    if (isCurrentProcessUser() && !proc_user_strlen(input, MINIMAFS_MAX_PATH, NULL))
        return NULL;
    bool ok = isCurrentProcessUser() ? proc_resolve_path(current_process, input, resolved)
                                     : fs_resolve_path(input, resolved);
    return ok ? resolved : NULL;
}

static bool user_range_ok(const void* ptr, size_t len, bool write) {
    if (!ptr || len == 0) return false;

    uintptr_t addr = (uintptr_t)ptr;
    if (addr < 0x1000ULL) return false;

    if (len > SIZE_MAX - addr) return false;
    if (addr > UINTPTR_MAX - len) return false;

    if (current_process && current_process->privilege == PROC_PRIVILEGE_USER) {
        if (!proc_user_range_ok(current_process, ptr, len, write)) {
            return false;
        }
    }

    return true;
}

bool syscall_user_range_ok(const void* ptr, size_t len, bool write) {
    return user_range_ok(ptr, len, write);
}

bool syscall_user_str_ok(const char* s, size_t max) {
    if (!s) return false;
    if (!isCurrentProcessUser()) return true;
    return proc_user_strlen(s, max, NULL);
}

bool syscall_user_copy_in(void* dst, const void* src, size_t len) {
    if (!src || !dst) return false;
    if (len && !user_range_ok(src, len, false)) return false;
    memcpy(dst, src, len);
    return true;
}

bool syscall_user_copy_out(void* dst, const void* src, size_t len) {
    if (!src || !dst) return false;
    if (len && !user_range_ok(dst, len, true)) return false;
    memcpy(dst, src, len);
    return true;
}

// Longest string SYS_GRAPHICS_TEXT / MEASURE_TEXT take.
#define SYS_GRAPHICS_TEXT_MAX (64u * 1024u)

static uint64_t sys_write_impl(int fd, const char* buf, uint64_t len) {
    if (!buf) return SYS_ERR_INVAL;
    // fd 3: the kernel log only (serial and the network log), not the
    // screen - for program diagnostics such as Mxplorer's console.
    if (fd == 3) {
        if (len && !user_range_ok(buf, len, false)) return SYS_ERR_INVAL;
        serial_write_str("[");
        serial_write_str(current_process ? current_process->name : "app");
        serial_write_str("] ");
        for (uint64_t i = 0; i < len; i++) serial_write(buf[i]);
        if (len && buf[len - 1] != '\n') serial_write('\n');
        return len;
    }
    if (fd != 1 && fd != 2) return SYS_ERR_BADFD;

    // Output redirected into a file by SYS_SPAWN (stdout_path/stderr_path).
    // Anything not redirected goes to the terminal exactly as before.
    if (isCurrentProcessUser()) {
        int redirected = proc_stdio_write(current_process, fd, buf, len);
        if (redirected == 0) return SYS_ERR_INVAL;
        if (redirected > 0) return len;
    }
    if (len && !user_range_ok(buf, len, false)) return SYS_ERR_INVAL;

    // No memory protection between processes exists yet (every
    // process_t shares the kernel's own PML4 - see proc_create()), so
    // there is no way to validate `buf` beyond a NULL check. Every
    // .run program is trusted for now, same trust level as any other
    // kernel process.
    for (uint64_t i = 0; i < len; i++) {
        graphics_write_textr_char(buf[i]);
    }
    serial_write_str("[run] ");
    for (uint64_t i = 0; i < len; i++) {
        serial_write(buf[i]);
    }
    return len;
}

static uint64_t sys_graphics_impl(const syscall_graphics_request_t* user_request) {
    // One kernel copy: the nested pointers below are checked and used from
    // it, not re-read from the program's memory.
    syscall_graphics_request_t request_copy;
    if (!syscall_user_copy_in(&request_copy, user_request, sizeof(request_copy)))
        return SYS_ERR_INVAL;
    const syscall_graphics_request_t* request = &request_copy;

    const int64_t* a = request->args;
    const bool requires_screen_owner =
        request->op == SYS_GRAPHICS_CLEAR ||
        request->op == SYS_GRAPHICS_PIXEL ||
        request->op == SYS_GRAPHICS_LINE ||
        request->op == SYS_GRAPHICS_CIRCLE ||
        request->op == SYS_GRAPHICS_FILL_CIRCLE ||
        request->op == SYS_GRAPHICS_RECTANGLE ||
        request->op == SYS_GRAPHICS_FILL_RECTANGLE ||
        request->op == SYS_GRAPHICS_TRIANGLE ||
        request->op == SYS_GRAPHICS_FILL_TRIANGLE ||
        request->op == SYS_GRAPHICS_ELLIPSE ||
        request->op == SYS_GRAPHICS_TEXT ||
        request->op == SYS_GRAPHICS_SET_RESOLUTION ||
        request->op == SYS_GRAPHICS_TERMINAL_CLEAR ||
        request->op == SYS_GRAPHICS_BLIT ||
        request->op == SYS_GRAPHICS_PRESENT;

    if (requires_screen_owner &&
        (!graphics_has_screen_owner() || graphics_screen_owner_pid() != getCurrentPID())) {
        return SYS_ERR_PERM;
    }

    switch (request->op) {
        case SYS_GRAPHICS_GET_WIDTH:
            return graphics_get_width();
        case SYS_GRAPHICS_GET_HEIGHT:
            return graphics_get_height();
        case SYS_GRAPHICS_INFO: {
            syscall_graphics_info_t* output = request->info;
            if (!output || !user_range_ok(output, sizeof(*output), true) ||
                output->size != sizeof(*output) ||
                output->version != SYS_GRAPHICS_ABI_VERSION ||
                output->reserved != 0) {
                serial_write_str("SYS_GRAPHICS_INFO: invalid output structure\n");
                return SYS_ERR_INVAL;
            }
            gfx_display_info_t display;
            if (!graphics_get_display_info(&display)) return SYS_ERR_GENERIC;
            output->width = display.width;
            output->height = display.height;
            output->pitch = display.pitch;
            output->pixel_format = SYS_GRAPHICS_PIXEL_FORMAT_ARGB8888;
            output->bytes_per_pixel = display.bytes_per_pixel;
            output->red_position = display.red_position;
            output->red_mask_size = display.red_mask_size;
            output->green_position = display.green_position;
            output->green_mask_size = display.green_mask_size;
            output->blue_position = display.blue_position;
            output->blue_mask_size = display.blue_mask_size;
            return SYS_SUCCESS;
        }
        case SYS_GRAPHICS_ACQUIRE:
            return graphics_claim_screen() ? SYS_SUCCESS : SYS_ERR_PERM;
        case SYS_GRAPHICS_RELEASE:
            if (graphics_release_screen()) {
                graphics_terminal_redraw();
                return SYS_SUCCESS;
            }
            return SYS_ERR_PERM;
        case SYS_GRAPHICS_OWNER:
            return graphics_has_screen_owner() ? graphics_screen_owner_pid() : SYS_ERR_NOTFOUND;
        case SYS_GRAPHICS_BLIT: {
            syscall_blit_request_t blit_copy;
            if (!syscall_user_copy_in(&blit_copy, request->blit, sizeof(blit_copy))) {
                serial_write_str("SYS_GRAPHICS_BLIT: invalid request pointer\n");
                return SYS_ERR_INVAL;
            }
            const syscall_blit_request_t* blit = &blit_copy;
            if (!blit || blit->size != sizeof(*blit) ||
                blit->version != SYS_GRAPHICS_ABI_VERSION ||
                (blit->flags != SYS_GRAPHICS_BLIT_OPAQUE &&
                 blit->flags != SYS_GRAPHICS_BLIT_ALPHA) ||
                !blit->src || ((uintptr_t)blit->src & 3u) != 0u ||
                !blit->src_w || !blit->src_h ||
                blit->src_w > SYSCALL_GFX_MAX_DIMENSION ||
                blit->src_h > SYSCALL_GFX_MAX_DIMENSION ||
                blit->sx < 0 || blit->sy < 0 || blit->sw <= 0 || blit->sh <= 0 ||
                (uint64_t)(uint32_t)blit->sx + (uint32_t)blit->sw > blit->src_w ||
                (uint64_t)(uint32_t)blit->sy + (uint32_t)blit->sh > blit->src_h) {
                serial_write_str("SYS_GRAPHICS_BLIT: invalid request\n");
                return SYS_ERR_INVAL;
            }
            for (size_t i = 0; i < 4; ++i) {
                if (blit->reserved[i] != 0) {
                    serial_write_str("SYS_GRAPHICS_BLIT: reserved field set\n");
                    return SYS_ERR_INVAL;
                }
            }

            const uint64_t min_pitch = (uint64_t)blit->src_w * sizeof(uint32_t);
            const uint64_t source_bytes = (uint64_t)blit->src_pitch * blit->src_h;
            if ((blit->src_pitch & 3u) != 0u || blit->src_pitch < min_pitch ||
                source_bytes > SYSCALL_GFX_MAX_BUFFER_BYTES ||
                source_bytes > SIZE_MAX ||
                !user_range_ok(blit->src, (size_t)source_bytes, false)) {
                serial_write_str("SYS_GRAPHICS_BLIT: invalid source range or pitch\n");
                return SYS_ERR_INVAL;
            }

            // The source range was validated above and stays mapped for the
            // length of this syscall, so blit straight out of it. This used
            // to go through a kernel staging buffer first: one more
            // full-size copy per frame, plus free_mem() zeroing it after.
            gfx_surface_t source = {
                (uint32_t*)blit->src, (size_t)source_bytes, blit->src_w,
                blit->src_h, blit->src_pitch / (uint32_t)sizeof(uint32_t)
            };
            gfx_rect_t source_rect = {
                blit->sx, blit->sy, blit->sw, blit->sh
            };
            bool ok = graphics_backbuffer_blit(&source, &source_rect,
                blit->dx, blit->dy, blit->flags == SYS_GRAPHICS_BLIT_ALPHA);
            if (!ok) {
                serial_write_str("SYS_GRAPHICS_BLIT: backbuffer blit failed\n");
                return SYS_ERR_GENERIC;
            }
            return SYS_SUCCESS;
        }
        case SYS_GRAPHICS_PRESENT: {
            gfx_rect_t rect = {
                (int32_t)a[0], (int32_t)a[1],
                (int32_t)a[2], (int32_t)a[3]
            };
            if (!graphics_backbuffer_present(&rect)) {
                serial_write_str("SYS_GRAPHICS_PRESENT: invalid rectangle or framebuffer\n");
                return SYS_ERR_INVAL;
            }
            return SYS_SUCCESS;
        }
        case SYS_GRAPHICS_CLEAR:
            graphics_clear((uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_PIXEL:
            graphics_write_pixel((int32_t)a[0], (int32_t)a[1],
                                 (uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_LINE:
            graphics_write_line((int32_t)a[0], (int32_t)a[1],
                                (int32_t)a[2], (int32_t)a[3],
                                (uint8_t)a[4], (uint8_t)a[5], (uint8_t)a[6]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_CIRCLE:
            graphics_write_circle((int32_t)a[0], (int32_t)a[1], (uint32_t)a[2],
                                  (uint8_t)a[3], (uint8_t)a[4], (uint8_t)a[5]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_FILL_CIRCLE:
            graphics_fill_circle((int32_t)a[0], (int32_t)a[1], (uint32_t)a[2],
                                 (uint8_t)a[3], (uint8_t)a[4], (uint8_t)a[5]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_RECTANGLE:
            graphics_write_rectangle((int32_t)a[0], (int32_t)a[1],
                                     (uint32_t)a[2], (uint32_t)a[3],
                                     (uint8_t)a[4], (uint8_t)a[5], (uint8_t)a[6]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_FILL_RECTANGLE:
            graphics_fill_rectangle((int32_t)a[0], (int32_t)a[1],
                                    (uint32_t)a[2], (uint32_t)a[3],
                                    (uint8_t)a[4], (uint8_t)a[5], (uint8_t)a[6]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_TRIANGLE:
            graphics_write_triangle((int32_t)a[0], (int32_t)a[1],
                                    (int32_t)a[2], (int32_t)a[3],
                                    (int32_t)a[4], (int32_t)a[5],
                                    (uint8_t)a[6], (uint8_t)a[7],
                                    (uint8_t)a[8]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_FILL_TRIANGLE:
            graphics_fill_triangle((int32_t)a[0], (int32_t)a[1],
                                   (int32_t)a[2], (int32_t)a[3],
                                   (int32_t)a[4], (int32_t)a[5],
                                   (uint8_t)a[6], (uint8_t)a[7],
                                   (uint8_t)a[8]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_ELLIPSE:
            graphics_write_ellipse((int32_t)a[0], (int32_t)a[1],
                                   (uint32_t)a[2], (uint32_t)a[3],
                                   (uint8_t)a[4], (uint8_t)a[5], (uint8_t)a[6]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_TEXT:
            if (!syscall_user_str_ok(request->text, SYS_GRAPHICS_TEXT_MAX))
                return SYS_ERR_INVAL;
            graphics_write_text(request->text, (int32_t)a[0], (int32_t)a[1],
                                 (uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_MEASURE_TEXT:
            if (!syscall_user_str_ok(request->text, SYS_GRAPHICS_TEXT_MAX) ||
                !request->out_width || !request->out_height ||
                !user_range_ok(request->out_width, sizeof(uint32_t), true) ||
                !user_range_ok(request->out_height, sizeof(uint32_t), true)) {
                return SYS_ERR_INVAL;
            }
            graphics_measure_text(request->text, request->out_width,
                                  request->out_height);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_SET_RESOLUTION:
            graphics_set_resolution((uint32_t)a[0], (uint32_t)a[1]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_TERMINAL_CLEAR:
            graphics_terminal_clear();
            return SYS_SUCCESS;
        default:
            return SYS_ERR_INVAL;
    }
}

static uint64_t sys_manager_impl(uint64_t manager, uint64_t operation,
                                 uint64_t value) {
    switch (manager) {
        case SYS_MANAGER_GPU:
            return operation == SYS_MANAGER_INIT && gpu_manager_init()
                ? SYS_SUCCESS : SYS_ERR_GENERIC;
        case SYS_MANAGER_AUDIO:
            switch (operation) {
                case SYS_MANAGER_INIT: return audio_manager_init()
                    ? SYS_SUCCESS : SYS_ERR_GENERIC;
                case SYS_MANAGER_UPDATE: audio_manager_update(); return SYS_SUCCESS;
                case SYS_MANAGER_START: audio_manager_start(); return SYS_SUCCESS;
                case SYS_MANAGER_HAS_DATA: return audio_manager_has_data() ? 1 : 0;
                case SYS_MANAGER_SET_SAMPLE_RATE:
                    audio_manager_set_sample_rate((uint32_t)value);
                    return SYS_SUCCESS;
                default: return SYS_ERR_INVAL;
            }
        case SYS_MANAGER_STORAGE:
            return operation == SYS_MANAGER_INIT && storage_manager_init()
                ? SYS_SUCCESS : SYS_ERR_GENERIC;
        case SYS_MANAGER_USB:
            return operation == SYS_MANAGER_INIT && usb_manager_init()
                ? SYS_SUCCESS : SYS_ERR_GENERIC;
        default:
            return SYS_ERR_INVAL;
    }
}

static uint64_t sys_sysinfo_impl(uint64_t op) {
    // Per-core usage lives in the open-ended tail of the op space, so
    // check that range first.
    if (op >= SYS_SYSINFO_CORE_USAGE_BASE) {
        uint64_t core_id = op - SYS_SYSINFO_CORE_USAGE_BASE;
        if (core_id >= MAX_CPUS || !g_cpus[core_id].online) {
            return SYS_ERR_INVAL;
        }
        uint32_t average = 0, usage = 0;
        smp_get_cpu_usage((uint32_t)core_id, &average, &usage);
        return usage;
    }

    switch (op) {
        case SYS_SYSINFO_CPU_USAGE: {
            uint32_t online = smp_online_cpu_count();
            if (online > MAX_CPUS) online = MAX_CPUS;

            uint64_t sum = 0;
            uint32_t counted = 0;
            for (uint32_t i = 0; i < online; i++) {
                if (!g_cpus[i].online) continue;
                uint32_t average = 0, usage = 0;
                smp_get_cpu_usage(i, &average, &usage);
                sum += usage;
                counted++;
            }
            return counted ? (sum / counted) : 0;
        }

        case SYS_SYSINFO_RAM_TOTAL:
            return findvar("totalrambytes");

        case SYS_SYSINFO_RAM_USED: {
            uint64_t heap_used = (uint64_t)allocator_used_bytes();
            uint64_t pmm_used  = (uint64_t)pmm_used_pages() * 4096ULL;
            return heap_used + pmm_used;
        }

        case SYS_SYSINFO_RAM_FREE: {
            uint64_t total = findvar("totalrambytes");
            uint64_t used  = (uint64_t)allocator_used_bytes() +
                              (uint64_t)pmm_used_pages() * 4096ULL;
            // Heap + PMM usage is an approximation of total RAM in use
            // (both pools sit inside total RAM but neither one alone
            // accounts for kernel/BIOS-reserved regions), so guard
            // against underflow rather than trust it can't exceed total.
            return (used < total) ? (total - used) : 0;
        }

        case SYS_SYSINFO_UPTIME_MS:
            return time_get_uptime_ms();

        case SYS_SYSINFO_PROCESS_COUNT:
            return (uint64_t)getProcessCount();

        case SYS_SYSINFO_CORE_COUNT:
            return (uint64_t)smp_online_cpu_count();

        case SYS_SYSINFO_HEAP_USED:
            return (uint64_t)allocator_used_bytes();

        case SYS_SYSINFO_HEAP_FREE:
            return (uint64_t)allocator_free_bytes();

        case SYS_SYSINFO_PMM_USED_PAGES:
            return (uint64_t)pmm_used_pages();

        case SYS_SYSINFO_PMM_FREE_PAGES:
            return (uint64_t)pmm_free_pages();

        case SYS_SYSINFO_CPU_FEATURES:
            return cpu_simd_features();

        case SYS_SYSINFO_XCR0:
            return simd_xcr0;

        default:
            return SYS_ERR_INVAL;
    }
}

static uint64_t sys_usb_impl(uint64_t operation, uint64_t arg1, uint64_t arg2) {
    switch (operation) {
        case SYS_USB_INIT:
            return usb_init() ? SYS_SUCCESS : SYS_ERR_GENERIC;
        case SYS_USB_POLL:
            usb_poll();
            return SYS_SUCCESS;
        case SYS_USB_HAS_KEYBOARD:
            return usb_get_keyboard() ? 1 : 0;
        case SYS_USB_KEY_TO_ASCII:
            return (uint8_t)usb_keyboard_translate((uint8_t)arg1, (uint8_t)arg2);
        case SYS_USB_KEY_IS_PRESSED:
            return usb_keyboard_is_pressed((uint8_t)arg1) ? 1 : 0;
        case SYS_USB_KEYBOARD_INFO: {
            syscall_usb_keyboard_info_t* info =
                (syscall_usb_keyboard_info_t*)arg1;
            usb_device_t* keyboard = usb_get_keyboard();
            if (!info || !user_range_ok(info, sizeof(*info), true)) return SYS_ERR_INVAL;
            if (!keyboard) return SYS_ERR_NOTFOUND;
            info->address = keyboard->address;
            info->port = keyboard->port;
            info->state = (uint8_t)keyboard->state;
            info->vendor_id = keyboard->vendor_id;
            info->product_id = keyboard->product_id;
            info->class_code = keyboard->class_code;
            info->is_keyboard = keyboard->is_keyboard ? 1 : 0;
            return SYS_SUCCESS;
        }
        default:
            return SYS_ERR_INVAL;
    }
}

static uint64_t sys_mouse_impl(uint64_t operation, uint64_t arg1, uint64_t arg2) {
    switch (operation) {
        case SYS_MOUSE_INIT:
            usb_mouse_init();
            return SYS_SUCCESS;
        case SYS_MOUSE_POLL:
            usb_poll();
            return SYS_SUCCESS;
        case SYS_MOUSE_HAS_MOUSE:
            return usb_mouse_is_present() ? 1 : 0;
        case SYS_MOUSE_GET_STATE: {
            syscall_mouse_state_t* out = (syscall_mouse_state_t*)arg1;
            if (!out || !user_range_ok(out, sizeof(*out), true)) return SYS_ERR_INVAL;
            int32_t dx, dy, wheel;
            uint8_t buttons;
            usb_mouse_consume_delta(&dx, &dy, &wheel, &buttons);
            out->dx = dx;
            out->dy = dy;
            out->wheel = wheel;
            out->buttons = buttons;
            return SYS_SUCCESS;
        }
        default:
            return SYS_ERR_INVAL;
    }
}

static uint64_t sys_input_read_impl(const syscall_input_read_request_t* user_request) {
    syscall_input_read_request_t request_copy;
    const syscall_input_read_request_t* request = &request_copy;
    if (!syscall_user_copy_in(&request_copy, user_request, sizeof(request_copy)) ||
        request->size != sizeof(*request) ||
        request->version != SYS_INPUT_ABI_VERSION ||
        !request->events || request->capacity == 0 ||
        request->capacity > SYSCALL_INPUT_MAX_READ ||
        request->timeout_ms > SYSCALL_INPUT_MAX_TIMEOUT_MS) {
        serial_write_str("SYS_INPUT_READ: invalid request\n");
        return SYS_ERR_INVAL;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (request->reserved[i] != 0) {
            serial_write_str("SYS_INPUT_READ: reserved field set\n");
            return SYS_ERR_INVAL;
        }
    }
    const size_t events_bytes =
        (size_t)request->capacity * sizeof(syscall_input_event_t);
    if (!user_range_ok(request->events, events_bytes, true) ||
        (request->dropped_count &&
         !user_range_ok(request->dropped_count,
                        sizeof(*request->dropped_count), true))) {
        serial_write_str("SYS_INPUT_READ: invalid output range\n");
        return SYS_ERR_INVAL;
    }
    if (!graphics_has_screen_owner() ||
        graphics_screen_owner_pid() != getCurrentPID()) {
        return SYS_ERR_PERM;
    }

    syscall_input_event_t events[SYSCALL_INPUT_MAX_READ];
    uint64_t start = time_get_uptime_ms();
    uint32_t count = 0;
    do {
        count = input_event_drain(events, request->capacity);
        if (count || request->timeout_ms == 0 ||
            time_get_uptime_ms() - start >= request->timeout_ms) {
            break;
        }
        sleep(1);
    } while (true);

    if (count) memcpy(request->events, events,
                      (size_t)count * sizeof(syscall_input_event_t));
    if (request->dropped_count) {
        *request->dropped_count = input_event_dropped_count();
    }
    return count;
}

static uint64_t sys_window_impl(const syscall_window_request_t* user_request) {
    // The window manager works on this kernel copy; the frame and event
    // structs a request points to are copied below as well, so nothing the
    // checks looked at can change before it is used.
    syscall_window_request_t request_copy;
    syscall_window_frame_t frame_copy;
    syscall_window_event_t event_copy;
    syscall_window_request_t* request = &request_copy;
    if (!syscall_user_copy_in(&request_copy, user_request, sizeof(request_copy)) ||
        request->size != sizeof(*request) ||
        request->version != SYS_WINDOW_ABI_VERSION)
        return SYS_ERR_INVAL;

    switch (request->op) {
        case SYS_WINDOW_PRESENT: {
            if (!request->pixels || !request->width || !request->height ||
                request->x > SYSCALL_GFX_MAX_DIMENSION ||
                request->y > SYSCALL_GFX_MAX_DIMENSION ||
                request->width > SYSCALL_GFX_MAX_DIMENSION - request->x ||
                request->height > SYSCALL_GFX_MAX_DIMENSION - request->y ||
                request->pitch < (request->x + request->width) *
                                     sizeof(uint32_t) ||
                request->pitch > SYSCALL_GFX_MAX_BUFFER_BYTES)
                return SYS_ERR_INVAL;
            uint64_t bytes = (uint64_t)(request->y + request->height - 1u) *
                             request->pitch +
                             (uint64_t)(request->x + request->width) *
                             sizeof(uint32_t);
            if (bytes > SYSCALL_GFX_MAX_BUFFER_BYTES ||
                !user_range_ok(request->pixels, (size_t)bytes, false))
                return SYS_ERR_INVAL;
            break;
        }
        case SYS_WINDOW_GET_INFO:
            if (!request->info ||
                !user_range_ok(request->info, sizeof(*request->info), true))
                return SYS_ERR_INVAL;
            break;
        case SYS_WINDOW_LIST:
            if (request->capacity > SYSCALL_WINDOW_MAX_WINDOWS ||
                (request->capacity &&
                 (!request->windows ||
                  !user_range_ok(request->windows,
                      (size_t)request->capacity * sizeof(*request->windows),
                      true))))
                return SYS_ERR_INVAL;
            break;
        case SYS_WINDOW_GET_FRAME: {
            if (!syscall_user_copy_in(&frame_copy, request->frame, sizeof(frame_copy)))
                return SYS_ERR_INVAL;
            syscall_window_frame_t* frame = &frame_copy;
            request->frame = frame;
            if (!frame->pixels || !frame->width || !frame->height ||
                frame->width > SYSCALL_GFX_MAX_DIMENSION ||
                frame->height > SYSCALL_GFX_MAX_DIMENSION ||
                frame->pitch < frame->width * sizeof(uint32_t) ||
                frame->pitch > SYSCALL_GFX_MAX_BUFFER_BYTES ||
                (uint64_t)frame->pitch * frame->height >
                    SYSCALL_GFX_MAX_BUFFER_BYTES ||
                !user_range_ok(frame->pixels,
                               (size_t)frame->pitch * frame->height, true))
                return SYS_ERR_INVAL;
            break;
        }
        case SYS_WINDOW_NEXT_EVENT:
        case SYS_WINDOW_POST_EVENT:
            if (!request->event ||
                !user_range_ok(request->event, sizeof(*request->event),
                    request->op == SYS_WINDOW_NEXT_EVENT))
                return SYS_ERR_INVAL;
            if (request->op == SYS_WINDOW_POST_EVENT) {
                // Only read: validate and queue the same copy.
                memcpy(&event_copy, request->event, sizeof(event_copy));
                request->event = &event_copy;
            }
            break;
        default:
            break;
    }

    return window_manager_syscall(&request_copy, getCurrentPID(),
        current_process && current_process->is_window_manager);
}

/*
 * SYS_LISTDIR helper: converts kernel-internal minimafs_dir_entry_t
 * entries into the stable syscall_dirent_t ABI (see syscall.h) and
 * writes them into the caller-supplied buffer. Caps at both the
 * caller's requested max_entries and MINIMAFS_MAX_ROOT_ENTRIES (the
 * most minimafs_list_dir() can ever produce for one directory), so a
 * caller passing an unreasonably large max_entries can't turn this
 * into an unbounded allocation.
 */
static uint64_t sys_listdir_impl(const char* path, syscall_dirent_t* out,
                                 uint32_t max_entries) {
    if (!path || !out || max_entries == 0) return SYS_ERR_INVAL;

    char resolved[MINIMAFS_MAX_PATH];
    path = syscall_resolve_path(path, resolved);
    if (!path) return SYS_ERR_INVAL;

    uint32_t cap = max_entries;
    if (cap > MINIMAFS_MAX_ROOT_ENTRIES) cap = MINIMAFS_MAX_ROOT_ENTRIES;

    while (__sync_lock_test_and_set(&g_listdir_entries_lock, 1)) { }
    minimafs_dir_entry_t* entries = (minimafs_dir_entry_t*)g_listdir_entries;

    uint32_t count = minimafs_list_dir(path, entries, cap);
    // Only the entries that exist are written: callers may pass a larger
    // capacity than their buffer needs.
    if (count && !user_range_ok(out, (size_t)count * sizeof(*out), true)) {
        __sync_lock_release(&g_listdir_entries_lock);
        return SYS_ERR_INVAL;
    }
    for (uint32_t i = 0; i < count; i++) {
        strncpy(out[i].name, entries[i].name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = '\0';
        out[i].type = (uint8_t)entries[i].type;
        out[i].hidden = entries[i].hidden ? 1 : 0;
    }

    __sync_lock_release(&g_listdir_entries_lock);
    return count;
}

static uint64_t sys_get_metadata_impl(const char* path, syscall_file_metadata_t* out) {
    if (!path || !out || !user_range_ok(out, sizeof(*out), true)) return SYS_ERR_INVAL;

    minimafs_file_metadata_t meta;
    if (!minimafs_get_metadata(path, &meta)) return SYS_ERR_NOTFOUND;

    strncpy(out->filetype, meta.filetype, sizeof(out->filetype) - 1);
    out->filetype[sizeof(out->filetype) - 1] = '\0';
    strncpy(out->fileformat, meta.fileformat, sizeof(out->fileformat) - 1);
    out->fileformat[sizeof(out->fileformat) - 1] = '\0';
    out->data_length = meta.data_length;
    out->runnable = meta.runnable ? 1 : 0;
    out->hidden = meta.hidden ? 1 : 0;
    out->entrypoint = meta.entrypoint;
    strncpy(out->created_date, meta.created_date, sizeof(out->created_date) - 1);
    out->created_date[sizeof(out->created_date) - 1] = '\0';
    strncpy(out->last_changed, meta.last_changed, sizeof(out->last_changed) - 1);
    out->last_changed[sizeof(out->last_changed) - 1] = '\0';
    return SYS_SUCCESS;
}

static uint64_t sys_pkg_impl(const syscall_pkg_request_t* request) {
    serial_write_str("SYS_PKG: dispatch\n");
    syscall_pkg_request_t local;
    if (!syscall_user_copy_in(&local, request, sizeof(local)) || !local.path)
        return SYS_ERR_INVAL;

    // Everything the request points to is checked before anything runs.
    if (!syscall_user_str_ok(local.path, MINIMAFS_MAX_PATH) ||
        (local.extra && !syscall_user_str_ok(local.extra, MINIMAFS_MAX_PATH)) ||
        (local.out_count && !user_range_ok(local.out_count, sizeof(uint32_t), true)) ||
        (local.out_failed && !user_range_ok(local.out_failed, sizeof(uint32_t), true)) ||
        (local.out_path && local.out_path_size &&
         !user_range_ok(local.out_path, local.out_path_size, true)))
        return SYS_ERR_INVAL;

    switch (local.op) {
        case SYS_PKG_UNZIP: {
            if (!local.extra) return SYS_ERR_INVAL;
            uint32_t installed = 0, failed = 0;
            bool ok = pkglib_unzip(local.path, local.extra, &installed, &failed);
            if (local.out_count) *local.out_count = installed;
            if (local.out_failed) *local.out_failed = failed;
            return ok ? SYS_SUCCESS : SYS_ERR_GENERIC;
        }
        case SYS_PKG_ZIP: {
            serial_write_str("SYS_PKG: zip\n");
            bool ok = pkglib_zip(local.path, local.extra,
                                 local.out_path, local.out_path_size);
            return ok ? SYS_SUCCESS : SYS_ERR_GENERIC;
        }
        case SYS_PKG_INFO: {
            uint32_t count = 0;
            bool ok = pkglib_info(local.path, &count);
            if (local.out_count) *local.out_count = count;
            return ok ? SYS_SUCCESS : SYS_ERR_GENERIC;
        }
        default:
            return SYS_ERR_INVAL;
    }
}

/*
 * SYS_PSLIST helper: snapshots the current process list into the
 * stable syscall_process_info_t ABI (see syscall.h). Uses get_procs()
 * (proc.h) rather than walking proc_list_head directly so this stays
 * consistent with how every other process-listing consumer in the
 * kernel (listAllProcesses(), getprocslist()) already does it.
 */
static uint64_t sys_pslist_impl(syscall_process_info_t* out, uint32_t max_entries) {
    if (!out || max_entries == 0) return 0;

    size_t count = 0;
    process_t** procs = get_procs(&count);
    if (!procs || count == 0) return 0;

    uint32_t n = (uint32_t)count;
    if (n > max_entries) n = max_entries;
    // Only the entries written have to fit (max_entries may be generous).
    if (n && !user_range_ok(out, (size_t)n * sizeof(*out), true)) {
        free_mem(procs);
        return SYS_ERR_INVAL;
    }

    for (uint32_t i = 0; i < n; i++) {
        process_t* p = procs[i];
        strncpy(out[i].name, p->name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = '\0';
        out[i].pid         = p->pid;
        out[i].state       = (uint8_t)p->state;
        out[i].privilege   = (uint8_t)p->privilege;
        out[i].sched_cpu   = p->sched_cpu;
        out[i].sched_level = p->sched_level;
    }

    free_mem(procs);
    return n;
}

/* ============================================================
 * SECURITY: user/kernel syscall privilege boundary
 *
 * This kernel does not yet have per-process address spaces, user-mode
 * GDT segments, or a real CPL3 transition path (see process_privilege_t
 * in proc.h for the full breakdown of what's missing and why a
 * PROC_PRIVILEGE_USER process still technically runs with full kernel
 * memory access today). Because of that, this check is NOT a hard
 * security boundary the way a real ring3/ring0 split would be - a
 * sufficiently malicious .run program could still corrupt kernel state
 * directly rather than going through a syscall at all.
 *
 * What this DOES do, cheaply and without any of that machinery: it
 * closes the specific, concrete hole where any loaded .run program
 * (createUserProcess() in runcommand.c is the only thing that ever
 * creates a PROC_PRIVILEGE_USER process today) can call through the
 * syscall gate to reinitialize or reconfigure shared hardware/driver
 * state - GPU mode, audio DMA start, USB controller re-enumeration,
 * storage re-probe - that the whole system, including the kernel's
 * own drivers and every OTHER process, depends on staying consistent.
 * A user program has no legitimate reason to call SYS_MANAGER_INIT or
 * SYS_USB_INIT; only kernel-owned code (main.c's boot sequence, the
 * terminal's disk-mount prompt, etc) should ever do that.
 *
 * Read-only status queries (e.g. "is there audio data ready", polling
 * for keyboard input, translating a scancode) are left available to
 * user processes, since a .run program legitimately needs those to
 * draw its own UI or react to input - only privilege-escalating /
 * global-state-mutating operations are denied.
 *
 * The MinimaFS extension syscalls (SYS_FWRITE/SYS_LISTDIR/SYS_DELETE/
 * SYS_RMDIR/SYS_GET_METADATA/SYS_TELL/SYS_EOF) and SYS_PKG are all
 * per-file, self-contained operations with no shared-hardware-state
 * implications - same trust level as the existing SYS_OPEN/SYS_READ/
 * SYS_MKDIR/etc, so they fall through to "allowed" below without
 * needing an entry here.
 *
 * To extend this: add a case for the new syscall number (or, for a
 * multiplexed syscall like SYS_MANAGER/SYS_USB, a case for the new
 * sub-operation) and return false for anything a user process should
 * never be allowed to trigger. Default is "allowed" - only list what
 * needs denying, so adding a new syscall doesn't require remembering
 * to update this table or it silently becomes forbidden.
 * ============================================================ */
/* User allocations use the kernel heap, but ownership is tracked per
 * process so free/resize can be validated and leaked blocks reclaimed
 * when the scheduler reaps that process. */
static bool sys_heap_make_user_accessible(void* ptr, uint64_t size) {
	if (!isCurrentProcessUser()) return true;
	uintptr_t address = (uintptr_t)ptr;
	if (!current_process || !ptr || address < 64 || size > SIZE_MAX - 128) return false;
	return proc_map_user_heap_range(current_process, (void*)(address - 64),
					(size_t)size + 128);
}

static uint64_t sys_heap_impl(uint64_t op, void* ptr, uint64_t size) {
    switch (op) {
        case SYS_HEAP_ALLOC: {
            if (size == 0) return SYS_ERR_INVAL;
            void* mem = alloc_unzeroed((size_t)size);
            if (mem && isCurrentProcessUser()) {
                if (!proc_user_heap_track(current_process, mem, (size_t)size)) {
                    free_mem(mem);
                    return SYS_ERR_GENERIC;
                }
                if (!sys_heap_make_user_accessible(mem, size)) {
                    proc_user_heap_untrack(current_process, mem);
                    free_mem(mem);
                    return SYS_ERR_GENERIC;
                }
            }
            return mem ? (uint64_t)(uintptr_t)mem : SYS_ERR_GENERIC;
        }
        case SYS_HEAP_ALLOC_ZEROED: {
            if (size == 0) return SYS_ERR_INVAL;
            void* mem = alloc((size_t)size);
            if (mem && isCurrentProcessUser()) {
                if (!proc_user_heap_track(current_process, mem, (size_t)size)) {
                    free_mem(mem);
                    return SYS_ERR_GENERIC;
                }
                if (!sys_heap_make_user_accessible(mem, size)) {
                    proc_user_heap_untrack(current_process, mem);
                    free_mem(mem);
                    return SYS_ERR_GENERIC;
                }
            }
            return mem ? (uint64_t)(uintptr_t)mem : SYS_ERR_GENERIC;
        }
        case SYS_HEAP_FREE:
            if (isCurrentProcessUser()) {
                if (!ptr) return SYS_SUCCESS;
                if (!proc_user_heap_contains(current_process, ptr)) {
                    return SYS_ERR_INVAL;
                }
                if (!proc_user_heap_untrack(current_process, ptr)) {
                    return SYS_ERR_GENERIC;
                }
            }
            free_mem(ptr);
            return SYS_SUCCESS;
        case SYS_HEAP_RESIZE: {
            bool user_process = isCurrentProcessUser();
            if (user_process && ptr &&
                !proc_user_heap_contains(current_process, ptr)) {
                return SYS_ERR_INVAL;
            }
            if (size == 0) {
                if (user_process && ptr &&
                    !proc_user_heap_untrack(current_process, ptr)) {
                    return SYS_ERR_GENERIC;
                }
                alloc_resize(ptr, 0);
                return SYS_SUCCESS;
            }
            void* mem = alloc_resize(ptr, (size_t)size);
            if (!mem) return SYS_ERR_GENERIC;
            if (user_process) {
                if (!proc_user_heap_resize(current_process, ptr, mem,
                                           (size_t)size)) {
                    free_mem(mem);
                    return SYS_ERR_GENERIC;
                }
                if (!sys_heap_make_user_accessible(mem, size)) {
                    proc_user_heap_untrack(current_process, mem);
                    free_mem(mem);
                    return SYS_ERR_GENERIC;
                }
            }
            return (uint64_t)(uintptr_t)mem;
        }
        default:
            return SYS_ERR_INVAL;
    }
}

static uint64_t sys_random_impl(void* buf, uint64_t len) {
    if (!buf || len == 0 || !user_range_ok(buf, (size_t)len, true)) return SYS_ERR_INVAL;
    random_bytes(buf, (size_t)len);
    return len;
}

// Biggest single argv string SYS_EXEC accepts from a program.
#define SYS_EXEC_MAX_ARG_LEN (64u * 1024u)

static uint64_t sys_exec_impl(const char* path, const char** argv, uint64_t argc) {
    if (!path) { serial_write_str("SYS_EXEC: missing path\n"); return SYS_ERR_INVAL; }
    if (argc > 16) { serial_write_str("SYS_EXEC: too many args\n"); return SYS_ERR_INVAL; }

    // A program's path, argv array and every argv string must be its own
    // memory (the kernel reads them below and in the launch).
    // The pointer array is copied once, so the strings checked are the
    // strings used.
    const char* argv_copy[16];
    if (isCurrentProcessUser()) {
        if (!proc_user_strlen(path, MINIMAFS_MAX_PATH, NULL)) return SYS_ERR_INVAL;
        if (argc) {
            if (!argv || !syscall_user_copy_in(argv_copy, argv, argc * sizeof(char*)))
                return SYS_ERR_INVAL;
            for (uint64_t i = 0; i < argc; i++) {
                const char* a = argv_copy[i];
                if (a && !proc_user_strlen(a, SYS_EXEC_MAX_ARG_LEN, NULL)) return SYS_ERR_INVAL;
            }
            argv = argv_copy;
        }
    }

    serial_write_str("SYS_EXEC: request path=");
    serial_write_str(path);
    serial_write_str(" argc=");
    serial_write_dec(argc);
    serial_write_str("\n");

    char normalized[MINIMAFS_MAX_PATH];
    if (isCurrentProcessUser()) {
        // Relative paths are the program's own (its working directory), like
        // SYS_SPAWN. A program started from the root (what run_normalize_path
        // always assumed) is still found: when nothing is at the resolved
        // path and the input has no drive, fall back to "0:/<path>".
        if (!proc_resolve_path(current_process, path, normalized)) {
            serial_write_str("SYS_EXEC: invalid path\n");
            return SYS_ERR_INVAL;
        }
        if ((!minimafs_exists(normalized) || minimafs_is_dir(normalized)) && !strchr(path, ':')) {
            char fallback[MINIMAFS_MAX_PATH];
            if (run_normalize_path(path, fallback, sizeof(fallback)) &&
                minimafs_exists(fallback) && !minimafs_is_dir(fallback))
                strcpy(normalized, fallback);
        }
    } else if (!run_normalize_path(path, normalized, sizeof(normalized))) {
        serial_write_str("SYS_EXEC: invalid path\n");
        return SYS_ERR_INVAL;
    }

    serial_write_str("SYS_EXEC: normalized path=");
    serial_write_str(normalized);
    serial_write_str(" exists=");
    serial_write_dec(minimafs_exists(normalized) ? 1 : 0);
    serial_write_str(" dir=");
    serial_write_dec(minimafs_is_dir(normalized) ? 1 : 0);
    serial_write_str("\n");

    if (isCurrentProcessUser()) {
        // From a program: the child inherits its working directory,
        // environment and output, and the program may SYS_WAIT for it.
        uint64_t pid = proc_exec_inherit(normalized, (int)argc, argv);
        if ((int64_t)pid < 0) serial_write_str("SYS_EXEC: launch failed\n");
        return pid;
    }
    uint64_t started_pid = 0;
    process_t* proc = run_launch_file_pid(normalized, (int)argc, argv, &started_pid);
    if (!proc) {
        serial_write_str("SYS_EXEC: launch failed\n");
        return SYS_ERR_GENERIC;
    }
    return started_pid;
}

static uint64_t sys_stat_impl(const char* path, syscall_stat_t* out) {
    if (!path || !out) return SYS_ERR_INVAL;
    // (the path string is checked by syscall_resolve_path)
    if (!user_range_ok(out, sizeof(*out), true)) return SYS_ERR_INVAL;
    char resolved[MINIMAFS_MAX_PATH];
    if (!syscall_resolve_path(path, resolved)) return SYS_ERR_INVAL;
    minimafs_stat_t st;
    memset(&st, 0, sizeof(st));
    if (!minimafs_stat(resolved, &st)) return SYS_ERR_NOTFOUND;
    syscall_stat_t r;
    memset(&r, 0, sizeof(r));
    r.type = st.is_dir ? SYS_STAT_DIR : SYS_STAT_FILE;
    r.size = st.is_dir ? 0 : st.size;
    r.flags = (st.hidden ? SYS_STAT_HIDDEN : 0u) | (st.runnable ? SYS_STAT_RUNNABLE : 0u);
    int64_t when = 0;
    if (timeconv_parse_minimafs_date(st.created_date, &when)) r.created = when;
    if (timeconv_parse_minimafs_date(st.last_changed, &when)) r.modified = when;
    *out = r;
    return SYS_SUCCESS;
}

static uint64_t sys_rename_impl(const char* from, const char* to) {
    if (!from || !to) return SYS_ERR_INVAL;
    // Both strings are checked by syscall_resolve_path, before anything changes.
    char* paths = (char*)alloc_unzeroed(2u * MINIMAFS_MAX_PATH);
    if (!paths) return SYS_ERR_NOMEM;
    char* a = paths;
    char* b = paths + MINIMAFS_MAX_PATH;
    uint64_t result;
    if (!syscall_resolve_path(from, a) || !syscall_resolve_path(to, b)) result = SYS_ERR_INVAL;
    else if (!minimafs_exists(a)) result = SYS_ERR_NOTFOUND;
    else result = minimafs_rename(a, b) ? SYS_SUCCESS : SYS_ERR_GENERIC;
    free_mem(paths);
    return result;
}

static bool syscall_user_may_call(uint64_t syscall_num, const syscall_regs_t* regs) {
    switch (syscall_num) {
        case SYS_GRAPHICS: {
            if (!user_range_ok((const void*)regs->rdi,
                               sizeof(syscall_graphics_request_t), false)) {
                return false;
            }
            const syscall_graphics_request_t* req =
                (const syscall_graphics_request_t*)regs->rdi;
            if (!req) return true;
            switch (req->op) {
                case SYS_GRAPHICS_GET_WIDTH:
                case SYS_GRAPHICS_GET_HEIGHT:
                case SYS_GRAPHICS_OWNER:
                    return true;
                case SYS_GRAPHICS_ACQUIRE:
                case SYS_GRAPHICS_RELEASE:
                case SYS_GRAPHICS_BLIT:
                case SYS_GRAPHICS_PRESENT:
                case SYS_GRAPHICS_INFO:
                    return true;
                default:
                    return false;
            }
        }

        case SYS_MANAGER: {
            // arg layout matches mos_manager(manager, operation, value)
            // -> mos_syscall3(SYS_MANAGER, manager, operation, value)
            // -> rdi=manager, rsi=operation, rdx=value (see
            // sys_manager_impl()'s call site below for the same
            // mapping).
            uint64_t operation = regs->rsi;
            switch (operation) {
                case SYS_MANAGER_HAS_DATA:
                    // Pure read - safe for any process.
                    return true;
                case SYS_MANAGER_INIT:
                case SYS_MANAGER_UPDATE:
                case SYS_MANAGER_START:
                case SYS_MANAGER_SET_SAMPLE_RATE:
                default:
                    // Everything else mutates shared driver/hardware
                    // state - kernel only.
                    return false;
            }
        }

        case SYS_NET: {
            // rdi = syscall_net_request_t*. Read the op out of the
            // request rather than a register, since this syscall is a
            // dispatch like SYS_GRAPHICS/SYS_PKG.
            const syscall_net_request_t* req =
                (const syscall_net_request_t*)regs->rdi;
            // Not readable: sys_net_impl() rejects it.
            if (!req || !user_range_ok(req, sizeof(*req), false)) return true;

            switch (req->op) {
                case SYS_NET_DHCP:
                    // Rewrites the interface's address, netmask,
                    // gateway and DNS for EVERY process on the
                    // machine, and blocks the net stack for up to ten
                    // seconds doing it. Same reasoning as
                    // SYS_USB_INIT below: bringing the shared
                    // interface up is an operator action, so it stays
                    // with the terminal's 'dhcp' command.
                    return false;
                default:
                    // Connect/send/recv/bind are per-connection and
                    // bounded; a user program doing them is the whole
                    // point of the syscall.
                    return true;
            }
        }

        case SYS_USB: {
            // arg layout matches mos_usb(operation, arg1, arg2) ->
            // mos_syscall3(SYS_USB, operation, arg1, arg2) ->
            // rdi=operation (see sys_usb_impl()'s call site below).
            uint64_t operation = regs->rdi;
            switch (operation) {
                case SYS_USB_INIT:
                    // Full controller (re-)init/enumeration - never
                    // safe for an arbitrary user process to trigger;
                    // it would tear down and re-probe every USB device
                    // in the system, including the keyboard everyone
                    // else is relying on.
                    return false;
                case SYS_USB_KEY_IS_PRESSED:
                    return graphics_has_screen_owner() &&
                           graphics_screen_owner_pid() == getCurrentPID();
                default:
                    // Polling and keyboard metadata queries are safe to allow.
                    return true;
            }
        }
        case SYS_MOUSE: {
            uint64_t operation = regs->rdi;
            switch (operation) {
                case SYS_MOUSE_INIT:
                    return false;
                case SYS_MOUSE_GET_STATE:
                    // Raw pointer state belongs to the active display server;
                    // applications receive pointer events through their own
                    // window event queues instead.
                    return graphics_has_screen_owner() &&
                           graphics_screen_owner_pid() == getCurrentPID();
                default:
                    return true;
            }
        }

        // SYS_INPUT_READ, SYS_WINDOW, SYS_GPU, SYS_MEDIA, SYS_STORAGE: the
        // handler checks (and copies) its own request, once per call.
        case SYS_INPUT_READ:
        case SYS_WINDOW:
        case SYS_GPU:
        case SYS_MEDIA:
        case SYS_STORAGE:
            return true;

        // The audio handlers check (and copy) their request themselves.
        case SYS_AUDIO:
        case SYS_AUDIO_MIXER:
        case SYS_AUDIO_STREAM:
            return true;

        default:
            // Everything else (SYS_WRITE, file I/O, graphics drawing,
            // process/time queries, MinimaFS extensions, SYS_PKG, etc)
            // has no shared-hardware-state implications and stays open
            // to user processes.
            return true;
    }
}

// A request struct without nested pointers that the handler reads and
// writes in place (text buffers it validates and then uses): the handler
// works on a kernel copy, so a second thread cannot change it between the
// handler's check and use; the result is copied back. Heap, not stack: the
// biggest is SYS_ROOT's (2.4 KB) and the kernel stack is small.
typedef uint64_t (*syscall_struct_fn)(void* request);
static uint64_t syscall_inout_struct(void* user, size_t size, syscall_struct_fn fn) {
    if (!isCurrentProcessUser()) return user ? fn(user) : SYS_ERR_INVAL;
    if (!user_range_ok(user, size, true)) return SYS_ERR_INVAL;
    void* copy = alloc_unzeroed(size);
    if (!copy) return SYS_ERR_NOMEM;
    memcpy(copy, user, size);
    uint64_t result = fn(copy);
    // The range was verified (and its pages given frames) above.
    memcpy(user, copy, size);
    memset(copy, 0, size);      // SYS_ROOT held a password
    free_mem(copy);
    return result;
}

// SYS_THREAD affinity / priority / CPU queries (see syscall.h).
static uint64_t sys_thread_online_mask(void) {
    uint64_t mask = 0;
    for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS; i++)
        if (g_cpus[i].online) mask |= 1ull << i;
    return mask ? mask : 1;
}

static uint64_t sys_thread_sched_op(process_t* self, uint64_t op, uint64_t tid, uint64_t arg) {
    if (op == SYS_THREAD_GET_CPU) return smp_current_cpu_id();
    if (op == SYS_THREAD_INFO) return kernel_boot_threads_multicore ? SYS_THREAD_INFO_MULTICORE : 0;
    if (op == SYS_THREAD_SET_AFFINITY) {
        if (!kernel_boot_threads_multicore) return SYS_ERR_PERM;
        uint64_t online = sys_thread_online_mask();
        if (arg && !(arg & online)) return SYS_ERR_INVAL;
        arg &= online;
        if (arg == online) arg = 0;             // everything: unrestricted
    } else if (op == SYS_THREAD_SET_PRIORITY) {
        if (arg > SYS_THREAD_PRIO_LOW) return SYS_ERR_INVAL;
    }
    // Looked up and changed under the scheduler lock (CPUs reap from the list).
    process_t* owner = proc_mem_owner(self);
    uint64_t result = SYS_ERR_NOTFOUND;
    uint64_t flags = scheduler_lock();
    process_t* p = NULL;
    if (!tid || tid == self->pid) p = self;
    else for (process_t* q = proc_list_head; q; q = q->next)
        if (q->pid == tid) { p = q; break; }
    if (p && proc_mem_owner(p) == owner &&
        p->state != PROCESS_ZOMBIE && p->state != PROCESS_TERMINATED) {
        switch (op) {
            case SYS_THREAD_SET_AFFINITY: scheduler_set_affinity(p, arg); result = SYS_SUCCESS; break;
            case SYS_THREAD_GET_AFFINITY: result = p->affinity ? p->affinity : sys_thread_online_mask(); break;
            case SYS_THREAD_SET_PRIORITY: scheduler_set_priority(p, (uint32_t)arg); result = SYS_SUCCESS; break;
            case SYS_THREAD_GET_PRIORITY: result = p->sched_prio; break;
            default: result = SYS_ERR_INVAL; break;
        }
        if (result == SYS_SUCCESS && op == SYS_THREAD_SET_AFFINITY && p != self) {
            // nothing more: scheduler_set_affinity moved or kicked it
        }
    } else if (p && proc_mem_owner(p) != owner) {
        result = SYS_ERR_INVAL;
    }
    scheduler_unlock(flags);
    // The caller itself on a CPU outside its new mask: off it (a short
    // sleep wakes on an allowed CPU).
    if (result == SYS_SUCCESS && op == SYS_THREAD_SET_AFFINITY && p == self)
        for (int i = 0; i < 100 && !scheduler_cpu_allowed(self, smp_current_cpu_id()); i++) sleep(1);
    return result;
}

static void syscall_dispatch_inner(syscall_regs_t* regs);

void syscall_dispatch(syscall_regs_t* regs) {
    if (!regs) return;
    // The page-fault handler looks at in_syscall: a fault inside a system
    // call on an address the program passed ends the program, not the
    // system.
    process_t* self = current_process;
    if (self) self->in_syscall++;
    wm_watchdog_syscall_enter(regs->rax);
    syscall_dispatch_inner(regs);
    wm_watchdog_syscall_exit();
    // The same process comes back here, whatever ran in between.
    if (self) self->in_syscall--;
}

static void syscall_dispatch_inner(syscall_regs_t* regs) {

    if (isCurrentProcessUser() && current_process->cleanup_requested &&
        !current_process->cleanup_invoked && current_process->cleanup_entry &&
        regs->rax != SYS_REGISTER_CLEANUP) {
        current_process->cleanup_invoked = true;
        current_process->cleanup_entry();
        // A callback may return without calling mos_exit(); give the
        // process the remainder of its grace period in that case.
    }

    if (isCurrentProcessUser() && !syscall_user_may_call(regs->rax, regs)) {
        serial_write_str("[syscall] denied: user process attempted privileged "
                          "syscall/op (num=");
        serial_write_dec(regs->rax);
        serial_write_str(")\n");
        regs->rax = SYS_ERR_PERM;
        return;
    }

    switch (regs->rax) {
        case SYS_GPU:
            regs->rax = fgpu_syscall((syscall_gpu_request_t*)regs->rdi);
            return;
        case SYS_MEDIA: {
            extern uint64_t media_syscall(syscall_media_request_t* request) __attribute__((weak));
            regs->rax = media_syscall
                ? media_syscall((syscall_media_request_t*)regs->rdi)
                : SYS_ERR_AGAIN;
            return;
        }
        case SYS_AUDIO_STREAM: {
            extern uint64_t audio_stream_syscall(syscall_audio_stream_t* r, uint64_t pid) __attribute__((weak));
            uint64_t pid = current_process ? current_process->pid : 0;
            if (!audio_stream_syscall) { regs->rax = SYS_ERR_INVAL; return; }
            if (!isCurrentProcessUser()) {
                regs->rax = audio_stream_syscall((syscall_audio_stream_t*)regs->rdi, pid);
                return;
            }
            // Hot (a call per audio buffer): one check, the handler works on
            // a copy so the frame count it validated is the one it uses.
            syscall_audio_stream_t sc;
            if (!user_range_ok((void*)regs->rdi, sizeof(sc), true)) { regs->rax = SYS_ERR_INVAL; return; }
            memcpy(&sc, (void*)regs->rdi, sizeof(sc));
            regs->rax = audio_stream_syscall(&sc, pid);
            memcpy((void*)regs->rdi, &sc, sizeof(sc));
            return;
        }
        case SYS_AUDIO: {
            // Weak: a build without the audio module answers "invalid".
            extern uint64_t audio_syscall(syscall_audio_request_t* r) __attribute__((weak));
            regs->rax = audio_syscall
                ? syscall_inout_struct((void*)regs->rdi, sizeof(syscall_audio_request_t),
                                       (syscall_struct_fn)audio_syscall)
                : SYS_ERR_INVAL;
            return;
        }
        case SYS_THREAD: {
            process_t* self = current_process;
            if (!self || self->privilege != PROC_PRIVILEGE_USER) {
                regs->rax = SYS_ERR_PERM;
                return;
            }
            if (regs->rdi == SYS_THREAD_CREATE) {
                syscall_thread_t tc;
                if (!syscall_user_copy_in(&tc, (const void*)regs->rsi, sizeof(tc))) { regs->rax = SYS_ERR_INVAL; return; }
                const syscall_thread_t* t = &tc;
                uint64_t tid = proc_create_thread(self, t->entry, t->stack_top, t->arg);
                regs->rax = tid ? tid : SYS_ERR_INVAL;
                return;
            }
            if (regs->rdi == SYS_THREAD_ALIVE) {
                // Under the scheduler lock: other CPUs reap from the list.
                uint64_t lf = scheduler_lock();
                process_t* thread = findProcessByPID(regs->rsi);
                bool alive = thread && thread->mem_owner == proc_mem_owner(self) &&
                             thread->state != PROCESS_ZOMBIE && thread->state != PROCESS_TERMINATED;
                scheduler_unlock(lf);
                regs->rax = alive ? SYS_SUCCESS : SYS_ERR_NOTFOUND;
                return;
            }
            if (regs->rdi == SYS_THREAD_SET_FS) {
                // WRMSR faults on a non-canonical address: lower half only.
                if (regs->rsi >= 0x0000800000000000ULL) { regs->rax = SYS_ERR_INVAL; return; }
                scheduler_set_fs_base(self, regs->rsi);
                regs->rax = SYS_SUCCESS;
                return;
            }
            if (regs->rdi == SYS_THREAD_GET_FS) {
                regs->rax = self->fs_base;
                return;
            }
            if (regs->rdi >= SYS_THREAD_SET_AFFINITY && regs->rdi <= SYS_THREAD_INFO) {
                regs->rax = sys_thread_sched_op(self, regs->rdi, regs->rsi, regs->rdx);
                return;
            }
            if (regs->rdi == SYS_THREAD_CREATE_EX) {
                syscall_thread_ex_t req;
                if (!syscall_user_copy_in(&req, (const void*)regs->rsi, sizeof(req))) { regs->rax = SYS_ERR_INVAL; return; }
                uint64_t tid = proc_create_thread_ex(self, req.entry, req.stack_top, req.arg,
                                                     req.fs_base, req.exit_futex);
                regs->rax = tid ? tid : SYS_ERR_INVAL;
                return;
            }
            regs->rax = SYS_ERR_INVAL;
            return;
        }
        case SYS_FUTEX: {
            if (regs->rdi == SYS_FUTEX_WAIT)
                regs->rax = futex_wait((uint32_t*)regs->rsi, (uint32_t)regs->rdx, regs->r10);
            else if (regs->rdi == SYS_FUTEX_WAKE)
                regs->rax = futex_wake((uint32_t*)regs->rsi,
                                       regs->rdx > UINT32_MAX ? UINT32_MAX : (uint32_t)regs->rdx);
            else
                regs->rax = SYS_ERR_INVAL;
            return;
        }
        case SYS_CLOCK: {
            switch (regs->rdi) {
                case SYS_CLOCK_MONOTONIC: regs->rax = clock_monotonic_ns(); break;
                case SYS_CLOCK_REALTIME:  regs->rax = clock_realtime_ns(); break;
                case SYS_CLOCK_TSC_HZ:    regs->rax = clock_tsc_hz(); break;
                default:                  regs->rax = SYS_ERR_INVAL; break;
            }
            return;
        }
        case SYS_VM: {
            process_t* self = current_process;
            if (!self || self->privilege != PROC_PRIVILEGE_USER) { regs->rax = SYS_ERR_PERM; return; }
            switch (regs->rdi) {
                case SYS_VM_MAP: {
                    syscall_vm_map_t req;
                    if (!syscall_user_copy_in(&req, (const void*)regs->rsi, sizeof(req)) ||
                        req.size < sizeof(req)) {
                        regs->rax = SYS_ERR_INVAL;
                        return;
                    }
                    regs->rax = uvm_map(self, req.address, req.length, req.prot, req.flags);
                    return;
                }
                case SYS_VM_UNMAP:    regs->rax = uvm_unmap(self, regs->rsi, regs->rdx); return;
                case SYS_VM_PROTECT:  regs->rax = uvm_protect(self, regs->rsi, regs->rdx, (uint32_t)regs->r10); return;
                case SYS_VM_DECOMMIT: regs->rax = uvm_decommit(self, regs->rsi, regs->rdx); return;
                default:              regs->rax = SYS_ERR_INVAL; return;
            }
        }
        case SYS_STAT:
            regs->rax = sys_stat_impl((const char*)regs->rdi, (syscall_stat_t*)regs->rsi);
            return;
        case SYS_RENAME:
            regs->rax = sys_rename_impl((const char*)regs->rdi, (const char*)regs->rsi);
            return;
        case SYS_CHDIR:
            regs->rax = proc_chdir((const char*)regs->rdi);
            return;
        case SYS_SPAWN:
            regs->rax = proc_spawn((const syscall_spawn_t*)regs->rdi);
            return;
        case SYS_WAIT:
            regs->rax = proc_wait_child(regs->rdi, regs->rsi, (syscall_wait_t*)regs->rdx);
            return;
        case SYS_POWER: {
            uint64_t op = regs->rdi;
            if (op != SYS_POWER_SHUTDOWN && op != SYS_POWER_REBOOT) {
                regs->rax = SYS_ERR_INVAL;
                return;
            }
            serial_write_str(op == SYS_POWER_SHUTDOWN ? "Power: shutdown requested by "
                                                      : "Power: reboot requested by ");
            serial_write_str(current_process ? current_process->name : "?");
            serial_write_str("\n");
            bool done = op == SYS_POWER_SHUTDOWN ? acpi_shutdown() : acpi_reboot();
            (void)done;
            regs->rax = SYS_ERR_GENERIC;    // still here: it did not work
            return;
        }
        case SYS_AUDIO_MIXER: {
            extern uint64_t audio_mixer_syscall(syscall_audio_mixer_t* m) __attribute__((weak));
            regs->rax = audio_mixer_syscall
                ? syscall_inout_struct((void*)regs->rdi, sizeof(syscall_audio_mixer_t),
                                       (syscall_struct_fn)audio_mixer_syscall)
                : SYS_ERR_INVAL;
            return;
        }
        case SYS_STORAGE: {
            extern uint64_t storage_syscall(syscall_storage_request_t* r);
            regs->rax = storage_syscall((syscall_storage_request_t*)regs->rdi);
            return;
        }
        case SYS_WRITE: {
            int fd          = (int)regs->rdi;
            const char* buf = (const char*)regs->rsi;
            uint64_t len    = regs->rdx;
            regs->rax = sys_write_impl(fd, buf, len);
            break;
        }

        case SYS_EXIT: {
            // noreturn - tears this process down and context-switches
            // away for good. Nothing after this ever runs, including
            // isr_syscall_wrapped's iretq.
            process_exit_with_status((uint8_t)regs->rdi, false, 0);
            break;
        }

        case SYS_REGISTER_CLEANUP:
            // The kernel calls this later: it has to be code of the program.
            if (!isCurrentProcessUser() || !regs->rdi ||
                !uvm_is_executable(proc_mem_owner(current_process), regs->rdi)) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            current_process->cleanup_entry = (void (*)())regs->rdi;
            current_process->cleanup_requested = false;
            current_process->cleanup_invoked = false;
            regs->rax = SYS_SUCCESS;
            break;

        case SYS_GETPID: {
            regs->rax = getCurrentPID();
            break;
        }

        case SYS_UPTIME: {
            regs->rax = time_get_uptime_ms();
            break;
        }

        case SYS_SLEEP: {
            sleep(regs->rdi);
            regs->rax = SYS_SUCCESS;
            break;
        }

        case SYS_OPEN: {
            const char* path = (const char*)regs->rdi;
            if (!path) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            char resolved[MINIMAFS_MAX_PATH];
            path = syscall_resolve_path(path, resolved);
            if (!path) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            minimafs_file_handle_t* handle =
                minimafs_open(path, regs->rsi == SYS_O_RDONLY);
            regs->rax = handle ? (uint64_t)handle : SYS_ERR_NOTFOUND;
            break;
        }

        case SYS_READ: {
            minimafs_file_handle_t* handle =
                (minimafs_file_handle_t*)regs->rdi;
            if (!handle || !regs->rsi) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            // Only the bytes that will be written must fit the buffer: a
            // program may pass a capacity larger than what is left.
            uint32_t want = (uint32_t)regs->rdx;
            uint32_t total = minimafs_size(handle), at = minimafs_tell(handle);
            uint32_t left = total > at ? total - at : 0;
            if (want > left) want = left;
            if (want && !user_range_ok((const void*)regs->rsi, want, true)) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            regs->rax = minimafs_read(handle, (void*)regs->rsi, regs->rdx);
            break;
        }

        case SYS_CLOSE: {
            minimafs_file_handle_t* handle =
                (minimafs_file_handle_t*)regs->rdi;
            if (!handle) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            minimafs_close(handle);
            regs->rax = SYS_SUCCESS;
            break;
        }

        case SYS_SEEK: {
            minimafs_file_handle_t* handle =
                (minimafs_file_handle_t*)regs->rdi;
            regs->rax = (handle && minimafs_seek(handle, regs->rsi))
                ? SYS_SUCCESS : SYS_ERR_INVAL;
            break;
        }

        case SYS_SIZE: {
            minimafs_file_handle_t* handle =
                (minimafs_file_handle_t*)regs->rdi;
            regs->rax = handle ? minimafs_size(handle) : SYS_ERR_INVAL;
            break;
        }

        case SYS_EXISTS: {
            const char* path = (const char*)regs->rdi;
            char resolved[MINIMAFS_MAX_PATH];
            path = syscall_resolve_path(path, resolved);
            regs->rax = path ? (minimafs_exists(path) ? 1 : 0) : SYS_ERR_INVAL;
            break;
        }

        case SYS_IS_DIR: {
            const char* path = (const char*)regs->rdi;
            char resolved[MINIMAFS_MAX_PATH];
            path = syscall_resolve_path(path, resolved);
            regs->rax = path ? (minimafs_is_dir(path) ? 1 : 0) : SYS_ERR_INVAL;
            break;
        }

        case SYS_MKDIR: {
            const char* path = (const char*)regs->rdi;
            char resolved[MINIMAFS_MAX_PATH];
            path = syscall_resolve_path(path, resolved);
            regs->rax = path ? (minimafs_mkdir(path) ? SYS_SUCCESS : SYS_ERR_GENERIC)
                             : SYS_ERR_INVAL;
            break;
        }

        case SYS_FWRITE: {
            minimafs_file_handle_t* handle =
                (minimafs_file_handle_t*)regs->rdi;
            const void* buf = (const void*)regs->rsi;
            uint64_t len = regs->rdx;
            if (!handle || !buf) {
                serial_write_str("SYS_FWRITE: invalid handle or buffer\n");
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            if ((uint32_t)len && !user_range_ok(buf, (uint32_t)len, false)) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            regs->rax = minimafs_write(handle, buf, (uint32_t)len);
            // Logged only when short: a line per write made every small
            // write cost a few ms of serial output.
            if (regs->rax != len) {
                serial_write_str("SYS_FWRITE: requested=");
                serial_write_dec((uint32_t)len);
                serial_write_str(" wrote=");
                serial_write_dec((uint32_t)regs->rax);
                serial_write_str("\n");
            }
            break;
        }

        case SYS_LISTDIR: {
            const char* path = (const char*)regs->rdi;
            syscall_dirent_t* out = (syscall_dirent_t*)regs->rsi;
            uint32_t max_entries = (uint32_t)regs->rdx;
            regs->rax = sys_listdir_impl(path, out, max_entries);
            break;
        }

        case SYS_DELETE: {
            const char* path = (const char*)regs->rdi;
            char resolved[MINIMAFS_MAX_PATH];
            path = syscall_resolve_path(path, resolved);
            regs->rax = path ? (minimafs_delete_file(path) ? SYS_SUCCESS : SYS_ERR_GENERIC)
                             : SYS_ERR_INVAL;
            break;
        }

        case SYS_RMDIR: {
            const char* path = (const char*)regs->rdi;
            char resolved[MINIMAFS_MAX_PATH];
            path = syscall_resolve_path(path, resolved);
            regs->rax = path ? (minimafs_rmdir(path) ? SYS_SUCCESS : SYS_ERR_GENERIC)
                             : SYS_ERR_INVAL;
            break;
        }

        case SYS_GET_METADATA: {
            const char* path = (const char*)regs->rdi;
            syscall_file_metadata_t* out = (syscall_file_metadata_t*)regs->rsi;
            char resolved[MINIMAFS_MAX_PATH];
            path = syscall_resolve_path(path, resolved);
            regs->rax = sys_get_metadata_impl(path, out);
            break;
        }

        case SYS_TELL: {
            minimafs_file_handle_t* handle =
                (minimafs_file_handle_t*)regs->rdi;
            regs->rax = handle ? minimafs_tell(handle) : SYS_ERR_INVAL;
            break;
        }

        case SYS_EOF: {
            minimafs_file_handle_t* handle =
                (minimafs_file_handle_t*)regs->rdi;
            regs->rax = handle ? (minimafs_eof(handle) ? 1 : 0) : SYS_ERR_INVAL;
            break;
        }

        case SYS_PKG: {
            regs->rax = sys_pkg_impl((const syscall_pkg_request_t*)regs->rdi);
            break;
        }

        case SYS_NET: {
            regs->rax = sys_net_impl((syscall_net_request_t*)regs->rdi);
            break;
        }

        case SYS_CREATE: {
            const char* path   = (const char*)regs->rdi;
            const char* type   = (const char*)regs->rsi;
            const char* format = (const char*)regs->rdx;
            if (!path) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            char resolved[MINIMAFS_MAX_PATH];
            if ((type && !syscall_user_str_ok(type, MINIMAFS_MAX_PATH)) ||
                (format && !syscall_user_str_ok(format, MINIMAFS_MAX_PATH))) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            path = syscall_resolve_path(path, resolved);
            if (!path) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            regs->rax = minimafs_create_file(path,
                                             type   ? type   : "binary",
                                             format ? format : "bin")
                        ? SYS_SUCCESS : SYS_ERR_GENERIC;
            break;
        }

        case SYS_GETCWD:
            // The program's own working directory, or the terminal's.
            regs->rax = proc_getcwd((char*)regs->rdi, regs->rsi);
            break;

        case SYS_EXEC: {
            const char* path   = (const char*)regs->rdi;
            const char** argv  = (const char**)regs->rsi;
            uint64_t argc      = regs->rdx;
            regs->rax = sys_exec_impl(path, argv, argc);
            break;
        }

        case SYS_HEAP: {
            uint64_t op   = regs->rdi;
            void* ptr     = (void*)regs->rsi;
            uint64_t size = regs->rdx;
            regs->rax = sys_heap_impl(op, ptr, size);
            break;
        }

        case SYS_RANDOM: {
            void* buf    = (void*)regs->rdi;
            uint64_t len = regs->rsi;
            regs->rax = sys_random_impl(buf, len);
            break;
        }

        case SYS_PSLIST: {
            syscall_process_info_t* out = (syscall_process_info_t*)regs->rdi;
            uint32_t max_entries = (uint32_t)regs->rsi;
            regs->rax = sys_pslist_impl(out, max_entries);
            break;
        }

        case SYS_GRAPHICS:
            regs->rax = sys_graphics_impl(
                (const syscall_graphics_request_t*)regs->rdi);
            break;

        case SYS_MANAGER:
            regs->rax = sys_manager_impl(regs->rdi, regs->rsi, regs->rdx);
            break;

        case SYS_USB:
            regs->rax = sys_usb_impl(regs->rdi, regs->rsi, regs->rdx);
            break;

        case SYS_MOUSE:
            regs->rax = sys_mouse_impl(regs->rdi, regs->rsi, regs->rdx);
            break;

        case SYS_INPUT_READ:
            regs->rax = sys_input_read_impl(
                (const syscall_input_read_request_t*)regs->rdi);
            break;

        case SYS_WINDOW:
            regs->rax = sys_window_impl(
                (const syscall_window_request_t*)regs->rdi);
            break;

        case SYS_GETTIME: {
            datetime_t* output = (datetime_t*)regs->rdi;
            if (!output || !user_range_ok(output, sizeof(*output), true)) {
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            *output = time_get_datetime();
            regs->rax = SYS_SUCCESS;
            break;
        }
        case SYS_SYSINFO:
            regs->rax = sys_sysinfo_impl(regs->rdi);
            break;

        case SYS_ROOT:
            regs->rax = syscall_inout_struct((void*)regs->rdi, sizeof(syscall_root_t),
                                             (syscall_struct_fn)root_syscall);
            break;

        case SYS_TIMESYNC:
            regs->rax = syscall_inout_struct((void*)regs->rdi, sizeof(syscall_timesync_t),
                                             (syscall_struct_fn)timesync_syscall);
            break;

        default: {
            regs->rax = SYS_ERR_GENERIC;
            break;
        }
    }
}
