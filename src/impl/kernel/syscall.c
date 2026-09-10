#include "x86_64/syscall.h"
#include "graphics.h"
#include "serial.h"
#include "prochandler.h"
#include "x86_64/scheduler.h"

static void sys_write_impl(int fd, const char* buf, uint64_t len) {
    if (!buf || len == 0) return;
    if (fd != 1 && fd != 2) return; // only stdout/stderr today

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
}

void syscall_dispatch(syscall_regs_t* regs) {
    if (!regs) return;

    switch (regs->rax) {
        case SYS_WRITE: {
            int fd          = (int)regs->rdi;
            const char* buf = (const char*)regs->rsi;
            uint64_t len    = regs->rdx;
            sys_write_impl(fd, buf, len);
            regs->rax = len;
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

        default: {
            regs->rax = (uint64_t)-1;
            break;
        }
    }
}