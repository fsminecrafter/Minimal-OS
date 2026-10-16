# Process and terminal

Terminal output, ending the program, time, sleeping, process IDs,
launching other programs and the Ctrl+C cleanup hook. Include
`minimalos.h`.

## `mos_write`

```c
long mos_write(int fd, const void* buf, size_t len);   /* form 1 */
long mos_write(const char* text, uint64_t len);        /* form 2 */
```

Writes bytes to the terminal. `mos_write` is a macro that picks a form by
argument count: three arguments call `mos_write_fd`, two call
`mos_write_text` (always stdout).

| Parameter | Description |
| --- | --- |
| `fd` | `1` (stdout) or `2` (stderr). Both go to the terminal and the serial log |
| `buf` / `text` | Bytes to write. Not required to be NUL-terminated |
| `len` | Number of bytes |

**Returns** `len` on success, `SYS_ERR_BADFD` for any other `fd`,
`SYS_ERR_INVAL` if `buf` is NULL.

**Notes**

- To write to a file use [`mos_fwrite`](files.md#mos_fwrite); `mos_write`
  only reaches the terminal.
- Inside mosWM the terminal is the Terminal window that launched the
  program.

**Example**

```c
mos_write(1, "ready\n", 6);
mos_write("also stdout\n", 12);
```

## `mos_write_fd`

```c
long mos_write_fd(int fd, const void* buf, size_t len);
```

The three-argument form of [`mos_write`](#mos_write), as a function.
Useful where a macro cannot go, such as taking its address.

**Returns** the same values as `mos_write`.

## `mos_write_text`

```c
long mos_write_text(const char* text, uint64_t length);
```

Writes `length` bytes of `text` to stdout. The two-argument form of
[`mos_write`](#mos_write).

## `mos_puts`

```c
long mos_puts(const char* str);
```

Writes a NUL-terminated string to stdout. No newline is added, unlike the
C library's `puts`.

| Parameter | Description |
| --- | --- |
| `str` | NUL-terminated string. Must not be NULL |

**Returns** the number of bytes written.

**Example**

```c
mos_puts("Hello, Minimal-OS!\n");
```

## `mos_exit`

```c
void mos_exit(int code);   /* noreturn */
void mos_exit(void);       /* exit code 0 */
```

Ends the calling process. Never returns. A macro over
[`mos_exit_with`](#mos_exit_with): `mos_exit()` with no argument exits with
code 0.

| Parameter | Description |
| --- | --- |
| `code` | Exit status. Only the low 8 bits are reported; the terminal prints `[exited with code N]` for a non-zero code |

**Notes**

- Global destructors (`.fini_array`) do not run. Release resources
  yourself, or from a [cleanup callback](#mos_register_cleanup).
- The kernel closes the process's windows, network handles and heap
  blocks when it reaps it, but files should be closed explicitly so
  their data is flushed.
- Returning from `main` calls `mos_exit` with the return value.

**Example**

```c
if (!buffer) {
    mos_puts("out of memory\n");
    mos_exit(1);
}
```

## `mos_exit_with`

```c
__attribute__((noreturn)) void mos_exit_with(int code);
```

The function behind [`mos_exit`](#mos_exit). Issues `SYS_EXIT` and never
returns.

## `mos_exit_code`

```c
void mos_exit_code(uint8_t code);
```

Older spelling from `x86_64/user_syscalls.h`. Same effect as
`mos_exit(code)`.

## `mos_getpid`

```c
long mos_getpid(void);
```

**Returns** the PID of the calling process.

**Notes** A server passes this, or a child's PID from
[`mos_exec`](#mos_exec), to [`mos_tcp_handoff`](network.md#mos_tcp_handoff).

## `mos_uptime`

```c
long mos_uptime(void);
```

**Returns** the time since boot in milliseconds.

**Notes** Use it to measure intervals and frame times. For wall-clock time
see [`mos_gettime`](#mos_gettime).

**Example**

```c
long start = mos_uptime();
do_work();
printf("took %ld ms\n", mos_uptime() - start);
```

## `mos_sleep`

```c
long mos_sleep(long ms);
```

Puts the process to sleep for at least `ms` milliseconds and lets other
processes run.

| Parameter | Description |
| --- | --- |
| `ms` | Milliseconds to sleep. `0` returns at once |

**Returns** `SYS_SUCCESS`.

**Notes**

- Call it in every polling loop (window events, network receive, accept)
  so the program does not spin a core at 100%.
- A frame loop typically sleeps 10-16 ms per frame.

**Example**

```c
for (;;) {
    handle_events();
    draw_frame();
    mos_sleep(16);   /* about 60 frames per second */
}
```

## `mos_gettime`

```c
long mos_gettime(void);
```

Wrapper for `SYS_GETTIME`, the real-time clock.

**Returns** `SYS_ERR_INVAL` with the current kernel: `SYS_GETTIME` expects
a pointer to a `datetime_t` in `rdi`, and this wrapper passes none.

**Notes** Until the wrapper takes a buffer, call the syscall directly with
a structure laid out like the kernel's `datetime_t`:

```c
typedef struct {
    uint16_t year;
    uint8_t month, day;           /* 1-12, 1-31 */
    uint8_t hour, minute, second; /* 0-23, 0-59, 0-59 */
} mos_datetime_t;

mos_datetime_t now;
if (mos_syscall(SYS_GETTIME, (long)(uintptr_t)&now, 0, 0) == 0)
    printf("%d-%d-%d %d:%d\n", now.year, now.month, now.day, now.hour, now.minute);
```

## `mos_register_cleanup`

```c
long mos_register_cleanup(void (*cleanup)(void));
```

Registers a function the kernel calls when the user presses Ctrl+C in the
terminal running this program.

| Parameter | Description |
| --- | --- |
| `cleanup` | Function to call. Must not be NULL |

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` for a NULL callback.

**Notes**

- The callback runs on the program's next syscall after Ctrl+C. It should
  save what it needs and call `mos_exit()`.
- If the program has not exited one second after Ctrl+C, the kernel
  terminates it. A second Ctrl+C within 800 ms terminates it at once.
- Without a callback, Ctrl+C terminates the program immediately.
- Registering again replaces the previous callback.

**Example**

```c
static long log_fd;

static void on_ctrl_c(void) {
    mos_fwrite(log_fd, "interrupted\n", 12);
    mos_close(log_fd);
    mos_exit(130);
}

int main(void) {
    log_fd = mos_open("0:/tmp/log.txt", SYS_O_RDWR);
    mos_register_cleanup(on_ctrl_c);
    for (;;) mos_sleep(100);
}
```

## `mos_exec`

```c
long mos_exec(const char* path, const char** argv, long argc);
```

Starts another `.run` bundle as a new process, the way the terminal's
`run` command does. Does not wait for it.

| Parameter | Description |
| --- | --- |
| `path` | Path of the `.run` file |
| `argv` | Extra arguments. The child sees `path` as `argv[0]` and these from `argv[1]` on. May be NULL when `argc` is 0 |
| `argc` | Number of entries in `argv`, at most 16 |

**Returns** the new process's PID, `SYS_ERR_INVAL` for a NULL or invalid
path or more than 16 arguments, `SYS_ERR_GENERIC` if the program could not
be loaded.

**Example**

```c
const char* args[] = { "--port", "8080" };
long pid = mos_exec("0:/programs/server.run", args, 2);
if (pid < 0) printf("could not start server (%ld)\n", pid);
```

## `mos_pslist`

```c
long mos_pslist(syscall_process_info_t* entries, size_t max_entries);
```

Copies a snapshot of the process list into a caller buffer.

| Parameter | Description |
| --- | --- |
| `entries` | Array to fill |
| `max_entries` | Capacity of `entries` |

**Returns** the number of entries written (0 if `entries` is NULL or
`max_entries` is 0).

Each `syscall_process_info_t` holds:

| Field | Meaning |
| --- | --- |
| `pid` | Process ID |
| `name[128]` | Process name, NUL-terminated |
| `state` | `SYSCALL_PROC_STATE_READY`, `_RUNNING`, `_WAITING`, `_PAUSED`, `_ZOMBIE` or `_TERMINATED` |
| `privilege` | `SYSCALL_PROC_PRIV_KERNEL` or `SYSCALL_PROC_PRIV_USER` |
| `sched_cpu` | Core the scheduler assigned |
| `sched_level` | Scheduler queue level |

**Example**

```c
syscall_process_info_t procs[32];
long n = mos_pslist(procs, 32);
for (long i = 0; i < n; i++)
    printf("%lu %s\n", (unsigned long)procs[i].pid, procs[i].name);
```

## `mos_getcwd`

```c
long mos_getcwd(char* path, size_t path_size);
```

Copies the terminal's current directory, which relative paths are resolved
against.

| Parameter | Description |
| --- | --- |
| `path` | Output buffer |
| `path_size` | Size of `path` in bytes, including room for the NUL |

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` if the buffer is NULL or too
small.

**Example**

```c
char cwd[256];
if (mos_getcwd(cwd, sizeof cwd) == 0) printf("in %s\n", cwd);
```

## `mos_syscall`

```c
long mos_syscall(long num, long a1, long a2, long a3);
```

Issues syscall `num` with up to three arguments. Every other wrapper is
built on this (or on `mos_syscall3`). Use it for calls that have no
wrapper yet.

| Parameter | Description |
| --- | --- |
| `num` | A `SYS_*` number from `syscall.h` |
| `a1`, `a2`, `a3` | Arguments for `rdi`, `rsi`, `rdx`; pass 0 for unused ones |

**Returns** the raw `rax` value.

## `mos_syscall3`

```c
uint64_t mos_syscall3(uint64_t number, uint64_t arg1, uint64_t arg2, uint64_t arg3);
```

Same as [`mos_syscall`](#mos_syscall) with unsigned types, from
`x86_64/user_syscalls.h`.

## `mos_syscall1`

```c
uint64_t mos_syscall1(uint64_t number, uint64_t arg1);
```

`mos_syscall3(number, arg1, 0, 0)`.
