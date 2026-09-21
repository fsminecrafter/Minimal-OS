# User SDK Syscalls

This is the user-space ABI reference for `.run` programs. The canonical
numbers and request structures are in `Minimal-OS-SDK/include/syscall.h`; the
inline wrappers are in `minimalos.h`, `net.h`, `stdlib.h`,
`x86_64/user_syscalls.h`, and related headers.

## Calling Convention

The SDK invokes the kernel with `int 0x80`:

- `rax`: syscall number;
- `rdi`, `rsi`, `rdx`: the first three arguments;
- `rax` on return: result value.

Pointer arguments must point to user-readable or user-writable memory as
required by the operation. Request-structure syscalls pass a pointer in `rdi`.
Do not pass kernel-internal structures or pointers through the ABI.

Most wrappers return `long`. Success is usually non-negative; failures are
negative values represented in the unsigned syscall type. Use the constants
rather than comparing against a raw magic number.

## Result Codes

| Constant | Meaning |
| --- | --- |
| `SYS_SUCCESS` | operation succeeded |
| `SYS_ERR_GENERIC` | unspecified failure |
| `SYS_ERR_BADFD` | invalid or closed handle |
| `SYS_ERR_NOTFOUND` | path, peer, or resource not found/closed |
| `SYS_ERR_INVAL` | invalid argument or request |
| `SYS_ERR_PERM` | operation is not allowed for this process |
| `SYS_ERR_BUSY` | shared non-reentrant subsystem is busy; retry |
| `SYS_ERR_AGAIN` | non-blocking operation has no result yet |

## Core Process And Console Calls

| Wrapper | Syscall | Contract |
| --- | --- | --- |
| `mos_write(fd, buf, len)` | `SYS_WRITE` | writes to terminal output; SDK programs normally use fd 1 or 2 |
| `mos_puts(text)` | `SYS_WRITE` | convenience wrapper for a NUL-terminated string |
| `mos_getpid()` | `SYS_GETPID` | returns the current process ID |
| `mos_exit(code)` | `SYS_EXIT` | terminates and does not return |
| `mos_sleep(ms)` | `SYS_SLEEP` | yields/sleeps for approximately the requested interval |
| `mos_uptime()` | `SYS_UPTIME` | returns uptime in milliseconds |
| `mos_gettime()` | `SYS_GETTIME` | returns the current kernel time representation |
| `mos_register_cleanup(fn)` | `SYS_REGISTER_CLEANUP` | registers a bounded cleanup callback for termination |
| `mos_exec(path, argv, argc)` | `SYS_EXEC` | launches another `.run`, returning its PID |
| `mos_pslist(entries, max)` | `SYS_PSLIST` | lists visible process information |

`mos_exec()` treats `argv` as extra arguments. The launched program receives
the path as `argv[0]` and the supplied values starting at `argv[1]`.

## Filesystem Calls

MinimaFS paths are strings such as `0:/Etc/System.conf`. `SYS_OPEN` opens an
existing file; use `mos_create()` before opening a new file for writing.

| Wrapper | Syscall | Contract |
| --- | --- | --- |
| `mos_open(path, flags)` | `SYS_OPEN` | returns an open handle; `SYS_O_RDONLY` is read-only and `SYS_O_RDWR` is writable |
| `mos_create(path, type, format)` | `SYS_CREATE` | creates an empty file; NULL type/format defaults to binary/bin |
| `mos_read(fd, buf, len)` | `SYS_READ` | reads from an open handle |
| `mos_fwrite(fd, buf, len)` | `SYS_FWRITE` | writes to an open writable handle |
| `mos_close(fd)` | `SYS_CLOSE` | closes the handle |
| `mos_seek(fd, offset)` | `SYS_SEEK` | changes the current file position |
| `mos_tell(fd)` | `SYS_TELL` | returns the current position |
| `mos_size(fd)` | `SYS_SIZE` | returns file data size |
| `mos_feof(fd)` | `SYS_EOF` | reports whether the handle is at EOF |
| `mos_exists(path)` | `SYS_EXISTS` | tests path existence |
| `mos_is_dir(path)` | `SYS_IS_DIR` | tests whether a path is a directory |
| `mos_mkdir(path)` | `SYS_MKDIR` | creates a directory |
| `mos_listdir(path, entries, max)` | `SYS_LISTDIR` | writes directory entries to a caller buffer |
| `mos_delete(path)` | `SYS_DELETE` | deletes a file |
| `mos_rmdir(path)` | `SYS_RMDIR` | removes an empty directory |
| `mos_get_metadata(path, out)` | `SYS_GET_METADATA` | fills stable file metadata |

Example:

```c
int fd = mos_open("0:/Etc/config.txt", SYS_O_RDONLY);
if (fd >= 0) {
    char buf[128];
    long got = mos_read(fd, buf, sizeof buf - 1);
    if (got > 0) {
        buf[got] = '\0';
        mos_puts(buf);
    }
    mos_close(fd);
}
```

Open handles belong to the process and should always be closed. The public
metadata structure is intentionally separate from the kernel's internal
`minimafs_file_metadata_t` so kernel structure growth does not change the ABI.

## Heap And Randomness

`stdlib.h` maps the standard allocation names to `SYS_HEAP`:

```c
char* data = malloc(4096);
if (!data) mos_exit(1);
/* use data */
free(data);
```

Available operations are `malloc`, `calloc`, `realloc`, and `free`. Allocation
failure becomes NULL. `realloc()` leaves the original allocation valid when it
fails. `mos_random_bytes()` uses the kernel random source and returns a byte
count; `mos_random_u64()` is a convenience wrapper.

The current implementation backs user heap allocations with the kernel heap.
That is an implementation detail and should not be used to infer unrestricted
kernel access.

## Graphics

`SYS_GRAPHICS` receives a `syscall_graphics_request_t`. The SDK headers provide
wrappers for:

- width and height queries;
- clear, pixel, line, rectangle, circle, triangle, and ellipse drawing;
- text output and text measurement;
- resolution changes;
- terminal clear.

Use `graphics.h` for the shorter API or `x86_64/user_graphics.h` for raw-style
wrappers. Coordinates and sizes are passed in the request's integer argument
array; strings and output dimensions use validated pointers.

## Device Managers And USB

`SYS_MANAGER` selects a manager and operation. The SDK exposes manager IDs for
GPU, audio, storage, and USB, with operations such as init, update, start,
has-data, and sample-rate configuration.

`SYS_USB` supports:

- `mos_usb_init()`;
- `mos_usb_poll()`;
- keyboard presence checks;
- scancode/modifier to ASCII translation;
- pressed-key queries;
- keyboard device metadata.

`SYS_MOUSE` provides mouse initialization, polling, presence, and accumulated
relative state (`dx`, `dy`, wheel, and button bitmask).

## Packages

`SYS_PKG` uses a `syscall_pkg_request_t` structure:

```c
uint32_t installed = 0, failed = 0;
long rc = mos_pkg_unzip("0:/packages/app.mpkg", "0:/programs",
                        &installed, &failed);
```

Wrappers:

- `mos_pkg_unzip(archive, target, installed, failed)`;
- `mos_pkg_zip(source, algorithm, output, output_size)`;
- `mos_pkg_info(archive, entry_count)`.

Algorithms are `"lzss"` or `"store"`; NULL selects the default. Package
extraction validates archive names and reports individual entry failures
separately from whole-archive failures.

## System Information And Processes

`user_sysinfo.h` exposes statistics through `SYS_SYSINFO`:

- CPU usage;
- total, used, and free RAM;
- uptime;
- process and online-core counts;
- heap used/free bytes;
- physical page used/free counts;
- per-core usage using `SYS_SYSINFO_CORE_USAGE_BASE + core_id`.

`mos_pslist()` writes process records containing PID, name, state, privilege,
CPU assignment, and scheduler level. Its buffer belongs to the caller.

## Networking

`SYS_NET` uses a `syscall_net_request_t` pointer. IPv4 addresses and ports are
host-order values. `net.h` provides wrappers for status, resolve, polling, TCP,
UDP, and the experimental TLS handle namespace.

### Status And DNS

```c
syscall_net_status_t status;
mos_net_status(&status);
uint32_t ip;
mos_net_resolve("example.com", &ip, 5000);
```

`mos_net_dhcp()` is privileged and returns `SYS_ERR_PERM` from ordinary user
programs. Run `dhcp` in the terminal first.

### TCP Client

```c
long conn = mos_tcp_connect(ip, 80, 5000);
if (conn > 0) {
    mos_tcp_send(conn, request, request_len);
    while (mos_net_poll() == 0) mos_sleep(10);
    long got = mos_tcp_recv(conn, buffer, sizeof buffer);
    mos_tcp_close(conn);
}
```

`mos_tcp_recv()` returns zero when no data is ready and a negative error when
the handle is invalid or the peer is closed. Deadline helpers in `net.h` pump
polling and sleep between attempts.

### TCP Server And Ownership

`mos_tcp_listen()` creates a listener handle. `mos_tcp_accept()` is
non-blocking and returns zero when no client is queued. After starting a child,
`mos_tcp_handoff(connection, child_pid)` transfers ownership so the child can
use the accepted handle. The handle number is normally passed as an argument.

### UDP

`mos_udp_bind()` creates a handle. `mos_udp_recv()` returns a datagram and fills
source address/port outputs. `mos_udp_send()` sends to a host-order destination;
`SYSCALL_NET_IP_BROADCAST` is the broadcast address.

### TLS Handles

The ABI defines separate TLS connect, accept, state, send, receive, and close
operations. TLS handshakes advance when `SYS_NET_TLS_STATE` or other TLS calls
pump the session. `SYS_ERR_AGAIN` means the session is still progressing.
The terminal `wget` command currently uses the kernel's dedicated TLS client;
user TLS handles and certificate policy should be treated as evolving APIs.

## ABI Rules And Lifecycle

- Check every return value, especially handle allocation and network operations.
- Retry `SYS_ERR_BUSY` and non-blocking `SYS_ERR_AGAIN` where documented.
- Close every file, TCP, UDP, and TLS handle before exit.
- Keep request structures alive until the syscall returns.
- Keep input/output buffers valid for the entire call.
- Do not call privileged operations from an ordinary `.run` process.
- Use `mos_sleep()` between polling attempts to yield CPU time.
- Do not assume kernel structure layouts match SDK structures.
