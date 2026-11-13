#ifndef USER_PTR_H
#define USER_PTR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Checks for pointers that come from a program (syscall.c implements them).
 *
 * All of them are no-ops for kernel-privilege callers (kernel threads call
 * many syscalls with kernel pointers): the range/string check passes, and
 * the copy helpers are plain memcpy. For a user caller they verify that the
 * memory belongs to the calling program (SYS_HEAP block, image, stack or
 * SYS_VM mapping, with the right protection) and give uvm pages their frame.
 *
 * The checks cannot stop a sibling thread from unmapping the memory after
 * the check; see docs/syscall-pointers.md.
 */

// [ptr, ptr+len) is usable; len 0 and NULL are false. `write`: the kernel
// will store into it.
bool syscall_user_range_ok(const void* ptr, size_t len, bool write);
// NUL-terminated string of at most `max` bytes including the NUL, entirely
// the program's. Kernel callers: only s != NULL.
bool syscall_user_str_ok(const char* s, size_t max);
// Copies a struct in / out. False (nothing copied) when the range is bad.
bool syscall_user_copy_in(void* kernel_dst, const void* user_src, size_t len);
bool syscall_user_copy_out(void* user_dst, const void* kernel_src, size_t len);

#endif
