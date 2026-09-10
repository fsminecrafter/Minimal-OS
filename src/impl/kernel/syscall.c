#include "x86_64/syscall.h"
#include "graphics.h"
#include "serial.h"
#include "prochandler.h"
#include "x86_64/scheduler.h"
#include "x86_64/minimafs.h"
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