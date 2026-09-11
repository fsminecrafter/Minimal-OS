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

void syscall_dispatch(syscall_regs_t* regs) {
    if (!regs) return;

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

        default: {
            regs->rax = SYS_ERR_GENERIC;
            break;
        }
    }
}