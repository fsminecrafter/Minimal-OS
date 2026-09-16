#include "x86_64/syscall.h"
#include "graphics.h"
#include "serial.h"
#include "prochandler.h"
#include "x86_64/scheduler.h"
#include "x86_64/minimafs.h"
#include "x86_64/gpu_manager.h"
#include "x86_64/audio_manager.h"
#include "x86_64/storage_manager.h"
#include "x86_64/usb_manager.h"
#include "usb/usb_stack.h"
#include "keyboard/usbkeyboard.h"
#include "time.h"
#include "x86_64/allocator.h"
#include "x86_64/pkglib.h"
#include "string.h"

#include "x86_64/smp.h"
#include "x86_64/allocator.h"
#include "x86_64/pmm.h"
#include "x86_64/globaldatatable.h"

static uint64_t sys_write_impl(int fd, const char* buf, uint64_t len) {
    if (!buf) return SYS_ERR_INVAL;
    if (fd != 1 && fd != 2) return SYS_ERR_BADFD;

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

static uint64_t sys_graphics_impl(const syscall_graphics_request_t* request) {
    if (!request) return SYS_ERR_INVAL;

    const int64_t* a = request->args;
    switch (request->op) {
        case SYS_GRAPHICS_GET_WIDTH:
            return graphics_get_width();
        case SYS_GRAPHICS_GET_HEIGHT:
            return graphics_get_height();
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
            if (!request->text) return SYS_ERR_INVAL;
            graphics_write_text(request->text, (int32_t)a[0], (int32_t)a[1],
                                 (uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4]);
            return SYS_SUCCESS;
        case SYS_GRAPHICS_MEASURE_TEXT:
            if (!request->text || !request->out_width || !request->out_height) {
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
            if (!info) return SYS_ERR_INVAL;
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

    uint32_t cap = max_entries;
    if (cap > MINIMAFS_MAX_ROOT_ENTRIES) cap = MINIMAFS_MAX_ROOT_ENTRIES;

    minimafs_dir_entry_t* entries =
        (minimafs_dir_entry_t*)alloc(cap * sizeof(minimafs_dir_entry_t));
    if (!entries) return SYS_ERR_GENERIC;

    uint32_t count = minimafs_list_dir(path, entries, cap);
    for (uint32_t i = 0; i < count; i++) {
        strncpy(out[i].name, entries[i].name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = '\0';
        out[i].type = (uint8_t)entries[i].type;
        out[i].hidden = entries[i].hidden ? 1 : 0;
    }

    free_mem(entries);
    return count;
}

static uint64_t sys_get_metadata_impl(const char* path, syscall_file_metadata_t* out) {
    if (!path || !out) return SYS_ERR_INVAL;

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
    if (!request || !request->path) return SYS_ERR_INVAL;

    switch (request->op) {
        case SYS_PKG_UNZIP: {
            if (!request->extra) return SYS_ERR_INVAL;
            uint32_t installed = 0, failed = 0;
            bool ok = pkglib_unzip(request->path, request->extra, &installed, &failed);
            if (request->out_count) *request->out_count = installed;
            if (request->out_failed) *request->out_failed = failed;
            return ok ? SYS_SUCCESS : SYS_ERR_GENERIC;
        }
        case SYS_PKG_ZIP: {
            bool ok = pkglib_zip(request->path, request->extra,
                                 request->out_path, request->out_path_size);
            return ok ? SYS_SUCCESS : SYS_ERR_GENERIC;
        }
        case SYS_PKG_INFO: {
            uint32_t count = 0;
            bool ok = pkglib_info(request->path, &count);
            if (request->out_count) *request->out_count = count;
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
static bool syscall_user_may_call(uint64_t syscall_num, const syscall_regs_t* regs) {
    switch (syscall_num) {
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
                default:
                    // Polling and read-only keyboard queries are
                    // side-effect-free (or self-contained) enough to
                    // allow.
                    return true;
            }
        }

        default:
            // Everything else (SYS_WRITE, file I/O, graphics drawing,
            // process/time queries, MinimaFS extensions, SYS_PKG, etc)
            // has no shared-hardware-state implications and stays open
            // to user processes.
            return true;
    }
}

void syscall_dispatch(syscall_regs_t* regs) {
    if (!regs) return;

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
            process_exit();
            break;
        }

        case SYS_REGISTER_CLEANUP:
            if (!isCurrentProcessUser() || !regs->rdi) {
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
            regs->rax = path ? (minimafs_exists(path) ? 1 : 0) : SYS_ERR_INVAL;
            break;
        }

        case SYS_IS_DIR: {
            const char* path = (const char*)regs->rdi;
            regs->rax = path ? (minimafs_is_dir(path) ? 1 : 0) : SYS_ERR_INVAL;
            break;
        }

        case SYS_MKDIR: {
            const char* path = (const char*)regs->rdi;
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
                regs->rax = SYS_ERR_INVAL;
                break;
            }
            regs->rax = minimafs_write(handle, buf, (uint32_t)len);
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
            regs->rax = path ? (minimafs_delete_file(path) ? SYS_SUCCESS : SYS_ERR_GENERIC)
                             : SYS_ERR_INVAL;
            break;
        }

        case SYS_RMDIR: {
            const char* path = (const char*)regs->rdi;
            regs->rax = path ? (minimafs_rmdir(path) ? SYS_SUCCESS : SYS_ERR_GENERIC)
                             : SYS_ERR_INVAL;
            break;
        }

        case SYS_GET_METADATA: {
            const char* path = (const char*)regs->rdi;
            syscall_file_metadata_t* out = (syscall_file_metadata_t*)regs->rsi;
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

        case SYS_GETTIME: {
            datetime_t* output = (datetime_t*)regs->rdi;
            if (!output) {
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

        default: {
            regs->rax = SYS_ERR_GENERIC;
            break;
        }
    }
}
