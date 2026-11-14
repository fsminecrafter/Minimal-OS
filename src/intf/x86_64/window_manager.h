#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "x86_64/syscall.h"

uint64_t window_manager_syscall(const syscall_window_request_t* request,
                                uint64_t caller_pid, bool is_window_manager);
void window_manager_process_exit(uint64_t pid);

// Kernel-side rendering into a window's shared back buffer (FGPU).
typedef struct {
    uint32_t* pixels;      // back buffer, in the kernel's identity map
    uint32_t pitch_px;     // pixels between rows
    uint32_t width, height;
    uint32_t buffer;       // its number (private to the manager)
    void* pin;             // the caller's reference on the frames (private)
} window_present_target_t;
// On true the window `window_id` of `owner_pid` has been switched to shared
// frames if it was not yet, and `target` describes its back buffer; the
// frames stay allocated until window_manager_present_done() whatever
// happens to the window. false means "use the copy path": no such window,
// not the owner, or no contiguous memory.
bool window_manager_present_target(uint32_t window_id, uint64_t owner_pid,
                                   window_present_target_t* target);
// Publishes the target as the window's front buffer with damage rectangle
// x, y, w, h (w == h == 0: abandon it, nothing changes) and releases it.
// Always call it after a successful present_target. SYS_ERR_AGAIN when the
// window was resized meanwhile (nothing presented), SYS_ERR_NOTFOUND when
// it is gone.
uint64_t window_manager_present_done(uint32_t window_id, uint64_t owner_pid,
                                     window_present_target_t* target,
                                     uint32_t x, uint32_t y, uint32_t w,
                                     uint32_t h);

#endif
