# System-call pointer arguments

Every pointer a program passes to the kernel, and how the kernel checks it.
Programs run in ring 3; the kernel dereferences their pointers directly while
running on the program's page tables. A pointer that is not the program's own
memory must come back as an error (`SYS_ERR_INVAL` unless noted) before the
call has any effect, and never as a kernel-mode page fault.

## The checks

| Helper | Where | Use |
|---|---|---|
| `syscall_user_range_ok(ptr, len, write)` | `syscall.c`, declared in `x86_64/user_ptr.h` | `[ptr, ptr+len)` is the program's (SYS_HEAP block, image, stack, SYS_VM mapping with the right protection); gives uvm pages their frame. NULL and `len == 0` are false. |
| `syscall_user_str_ok(s, max)` | same | NUL-terminated string of at most `max` bytes (NUL included), page by page. |
| `syscall_user_copy_in/out(...)` | same | range check plus `memcpy`; used to take one kernel copy of a request struct. |
| `proc_user_strlen(s, max, &len)` | `spawn.c` | what `syscall_user_str_ok` uses. |
| `syscall_resolve_path(...)` | `syscall.c` | every path syscall goes through it; it checks the string (`MINIMAFS_MAX_PATH`). |

All of them accept anything for kernel-privilege callers (`isCurrentProcessUser()`
false): kernel code calls many syscalls with kernel pointers.

Rules used throughout: output (kernel writes) means `write = true`; where
programs have always been free to pass a capacity larger than their buffer,
only the bytes the kernel really writes are checked (SYS_READ: what is left in
the file; SYS_LISTDIR/SYS_PSLIST: the entries found; SYS_GETCWD: the string;
SYS_NET UDP/TCP send: the slice sent); a request struct holding pointers is
copied into a kernel local first and the nested pointers are checked and used
from the copy (no double fetch). Results are written back to the program only
after the checks.

## Table

"Before" says what was checked before this audit; "now" is the state after it.

| Syscall / op | Pointer arguments | Dir | Length | Before | Now |
|---|---|---|---|---|---|
| SYS_WRITE (fd 1, 2, 3) | `buf` | read | `len` | redirected stdout only (`proc_stdio_write`) | checked on every path |
| SYS_OPEN, EXISTS, IS_DIR, MKDIR, DELETE, RMDIR, GET_METADATA path, LISTDIR path, STAT path, RENAME from/to, CHDIR, GETCWD | path string | read | NUL, `MINIMAFS_MAX_PATH` | STAT, RENAME, EXEC, CHDIR only | all, in `syscall_resolve_path` |
| SYS_CREATE | `path`; `type`, `format` strings (optional) | read | NUL | none | path, type, format |
| SYS_READ | `buf` | write | `min(len, bytes left in file)` | none | checked (position unchanged on error) |
| SYS_FWRITE | `buf` | read | `(uint32_t)len` | none | checked |
| SYS_LISTDIR | `out` | write | entries found (`<= max_entries`, `<= MINIMAFS_MAX_ROOT_ENTRIES`) | none | checked after listing, before writing |
| SYS_STAT | `out` | write | `sizeof(syscall_stat_t)` | yes | yes |
| SYS_GET_METADATA | `out` | write | `sizeof(syscall_file_metadata_t)` | none | checked |
| SYS_GETCWD | `out` | write | strlen(cwd)+1 (capacity may be larger) | yes | yes |
| SYS_GETTIME | `out` | write | `sizeof(datetime_t)` | none | checked |
| SYS_RANDOM | `buf` | write | `len` | none | checked |
| SYS_PSLIST | `out` | write | entries written (`<= max_entries`) | none | checked |
| SYS_PKG | request; `path`, `extra` strings; `out_path` (size `out_path_size`); `out_count`, `out_failed` | read; write | NUL / given | none | request copied, all checked before the op |
| SYS_EXEC | `path`; `argv[argc]` (argc <= 16); each argv string | read | NUL / `argc` | yes | yes; argv pointer array copied once |
| SYS_SPAWN | request; `path`, `argv[]` + strings, `envp[]` + strings, `cwd`, `stdout_path`, `stderr_path` | read | NUL / `argc` | yes (copied into the kernel) | unchanged |
| SYS_WAIT | `out` | write | `sizeof(syscall_wait_t)` | yes | yes |
| SYS_VM map | request | read | `sizeof` | yes | yes (copy) |
| SYS_VM unmap/protect/decommit | address range | - | `len` | by uvm itself (not dereferenced) | unchanged |
| SYS_THREAD create / create_ex | request (entry, stack, exit futex are checked in `proc_create_thread_ex`) | read | `sizeof` | yes | yes (copy) |
| SYS_FUTEX | `uint32_t*` | read (wait) / key only (wake) | 4, aligned | yes | unchanged |
| SYS_REGISTER_CLEANUP | `entry` (code pointer) | exec | - | non-NULL only | must be executable program code; see below |
| SYS_HEAP free / resize | `ptr` | - | tracked blocks | yes (looked up in the process's allocation list) | unchanged |
| SYS_GRAPHICS | request; INFO `info` (write); BLIT `blit` (read) and `blit->src` (read); TEXT / MEASURE_TEXT `text` (<= 64 KiB), `out_width`, `out_height` (write) | | `sizeof`, `src_pitch * src_h`, NUL | request, INFO, BLIT src | request and BLIT copied; TEXT, MEASURE_TEXT added |
| SYS_USB KEYBOARD_INFO | `arg1` | write | `sizeof(syscall_usb_keyboard_info_t)` | none | checked |
| SYS_MOUSE GET_STATE | `arg1` | write | `sizeof(syscall_mouse_state_t)` | none | checked |
| SYS_INPUT_READ | request; `events` (write); `dropped_count` (write, optional) | | `capacity * sizeof(event)`, 8 | yes | yes; request copied |
| SYS_WINDOW | request; PRESENT `pixels` (read); GET_INFO `info`, LIST `windows`, NEXT_EVENT `event` (write); GET_FRAME `frame` struct (read) and `frame->pixels` (write); POST_EVENT `event` (read) | | formulas in `sys_window_impl` | yes | yes; request, frame struct and posted event copied |
| SYS_GPU | request; INFO `out`; UPLOAD_BUFFER, CREATE_PROGRAM, SUBMIT, WRITE_SURFACE `data` (read); READBACK `out` (write) | | `bytes`, `pitch*(h-1)+row` | yes | yes; request copied once, only `handle` is written back |
| SYS_MEDIA | request; DECODE_TILES `frame` (read), `pixels` (write); CONVERT_YCBCR planes struct and planes; DECODE_H264 `frame` struct, `data`, `pixels` | | `frame_bytes`, `w*h*4`, plane sizes | yes | yes; request copied; tile frame copied into the kernel (see FGPU/media below) |
| SYS_STORAGE | request; LIST `devices` | write | `capacity * sizeof` | yes | yes; fields read once |
| SYS_AUDIO | request (path inline, status out) | in/out | `sizeof` | yes (gate) | yes; kernel copy, copied back |
| SYS_AUDIO_MIXER | request | in/out | `sizeof` | yes (gate) | yes; kernel copy, copied back |
| SYS_AUDIO_STREAM | request; WRITE `samples` | read | `frames * channels * 2` | yes | yes; request copied (frames validated = frames used) |
| SYS_NET | request; per op: `out_status`, `out_handle`, `out_ip`, `out_from_ip`, `out_from_port` (write), `buf` send (read) / recv (write), `host` (string <= 256) | | slice sizes, `NET_UDP_DGRAM_MAX`, `sizeof(syscall_net_adapters_t)` | none (the gate even read `req->op` unchecked) | request copied, every op's pointers checked before it runs |
| SYS_ROOT | request (inline strings, output buffer) | in/out | `sizeof` | yes | yes; kernel copy (heap), copied back |
| SYS_TIMESYNC | request (inline url, error) | in/out | `sizeof` | yes | yes; kernel copy (heap), copied back |
| SYS_MANAGER, SYS_SYSINFO, SYS_CLOCK, SYS_SLEEP, SYS_UPTIME, SYS_GETPID, SYS_EXIT, SYS_POWER, SYS_SEEK/SIZE/TELL/EOF/CLOSE | no pointers (file handles: see below) | | | | |

## Not fixed, and why

* **File handles are kernel pointers.** `SYS_OPEN` returns a `minimafs_file_handle_t*`
  and `SYS_READ`, `SYS_FWRITE`, `SYS_CLOSE`, `SYS_SEEK`, `SYS_SIZE`, `SYS_TELL`,
  `SYS_EOF` take it back unchecked. A program can forge one and make the kernel
  read or write there. Closing it needs a handle table (small integers owned by
  the process, like the SYS_NET handles); that changes the ABI value programs
  see, so it is a redesign, not part of this audit.
* **SYS_REGISTER_CLEANUP runs program code in ring 0.** `syscall_dispatch_inner`
  calls `cleanup_entry()` on the kernel stack. The entry is now required to be
  executable program memory (so it cannot be an unmapped address that would
  fault), but the call itself still gives the program ring-0 execution. The
  fix is to deliver the cleanup as an upcall into ring 3.
* **Races with a sibling thread.** Checks happen once, then the kernel uses the
  memory. A second thread of the same program can `SYS_VM_UNMAP` or
  `SYS_VM_PROTECT` a checked range between the check and the access. Requests
  and nested pointer values are copied, so *values* cannot change, but memory
  can disappear. Closing this needs fault-tolerant copies (an exception table
  for kernel-mode faults inside a syscall) or the uvm lock held across the
  call. Windows where the kernel holds a spinlock while touching program memory
  (window manager) are the worst case.
* **Strings are checked in place**, not copied (paths, TEXT, host names, argv
  strings): the length is validated but the bytes are read again later.
  An unterminated rewrite by a sibling thread could make a kernel read run on;
  all consumers are bounded by their own buffers except `graphics_write_text`
  and `graphics_measure_text`.
* **SYS_EXEC**: argv strings are checked, but `proc_exec_inherit` reads them
  from the program's memory again.
* **SYS_NET TCP/UDP handles** are small integers with owner checks only for
  TLS; `tcp_slot_get` does not compare owners for send/recv/close. Not a
  pointer issue.

## FGPU and SYS_MEDIA: do they keep program pointers?

* **FGPU (`fgpu.c`)** copies at call time: command streams (`SUBMIT`) and shader
  blobs (`CREATE_PROGRAM`) go into kernel buffers, uploads are `memcpy`ed into
  surface/buffer memory, readback is written before the call returns. Surfaces,
  buffers and programs live in kernel memory (or VRAM). The workers started by
  `smp_par_run` only touch those kernel copies. **No program pointer is kept
  beyond the call.** What was wrong: the request struct was read from program
  memory throughout a call that runs with interrupts enabled, so `bytes` could
  change between its check and the `memcpy`. The request is now copied once.
* **SYS_MEDIA DECODE_TILES** did hand program memory to the parallel workers:
  each tile job pointed into the program's `frame`, and the workers run on the
  kernel's page tables, where memory above 4 GiB does not exist. The frame is
  now copied into a kernel buffer first (pixels already went through one). The
  CONVERT_YCBCR and DECODE_H264 paths (NVIDIA hardware only, not run in QEMU)
  read the planes / slice data on the calling core, with the program's page
  tables, and are unchanged apart from the request copy.

## Cost of the checks

`syscall_user_range_ok` on heap memory walks the process's allocation list; on
uvm memory it walks the area list and the page tables per page. Per call:

* SYS_GPU, SYS_WINDOW, SYS_INPUT_READ, SYS_MEDIA, SYS_STORAGE, SYS_AUDIO*: one
  request check (gates that used to check the same struct a second time were
  removed) plus one per nested buffer. Image/blit buffers are large (a frame):
  that is O(pages) once per call, as before.
* SYS_GRAPHICS: the gate still checks the request (it needs `op`) and
  `sys_graphics_impl` copies it again: two small checks.
* SYS_READ / SYS_FWRITE / SYS_WRITE: one check of the buffer; SYS_READ also
  asks the handle for size and position (two field reads).
* SYS_NET: one for the request, one or two for buffers.
* Strings walk byte by byte for NUL but check the range once per page.
* Largest added cost: SYS_NET (new), SYS_LISTDIR (after the listing, under the
  listing lock), SYS_ROOT / SYS_TIMESYNC / SYS_AUDIO* (a heap copy, not hot).
