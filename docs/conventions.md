# Conventions and errors

How SDK calls reach the kernel, what they return, and which calls an
ordinary `.run` program is allowed to make.

## Calling convention

Every wrapper ends in one `int 0x80`:

| Register | In | Out |
| --- | --- | --- |
| `rax` | syscall number (`SYS_*`) | result |
| `rdi` | first argument | |
| `rsi` | second argument | |
| `rdx` | third argument | |

Calls with more data than three registers (graphics, windows, network,
packages, input) pass one pointer to a request structure in `rdi`. Keep
the structure and every buffer it points to alive until the call returns.
Versioned structures (`syscall_graphics_info_t`, `syscall_blit_request_t`,
`syscall_input_read_request_t`, `syscall_window_request_t`) must have their
`size` and `version` fields set and their `reserved` fields zero; the SDK
wrappers do this for you.

## Result codes

Results come back in `rax` as a 64-bit value. Failures are small negative
numbers, which as unsigned values sit at the very top of the range:

| Constant | Value | Meaning |
| --- | --- | --- |
| `SYS_SUCCESS` | `0` | The operation succeeded |
| `SYS_ERR_GENERIC` | `-1` | Unspecified failure (out of memory, the driver refused, the file system said no) |
| `SYS_ERR_BADFD` | `-2` | Invalid or closed handle; `mos_write` to an fd other than 1 or 2 |
| `SYS_ERR_NOTFOUND` | `-3` | Path, window, peer or resource not found, or a connection that has closed |
| `SYS_ERR_INVAL` | `-4` | Invalid argument: NULL pointer, out-of-range value, bad structure size or version |
| `SYS_ERR_PERM` | `-5` | The calling process may not do this (see [privilege](#privilege)) |
| `SYS_ERR_BUSY` | `-6` | A shared subsystem (the network stack, a full event queue) is in use: retry |
| `SYS_ERR_AGAIN` | `-7` | Non-blocking call with nothing to report yet: call again later |

The constants are defined as `((uint64_t)-N)`. Compare against them
directly, or cast to `long` and test for a negative value:

```c
long r = mos_mkdir("0:/data");
if (r == (long)SYS_ERR_INVAL) printf("bad path\n");
else if (r < 0)               printf("mkdir failed: %ld\n", r);
```

Some wrappers fold errors into a friendlier value (`malloc` returns
`NULL`, `mos_window_create` returns `0`, `mos_tcp_accept` returns `0` for
"nobody waiting"); each entry says so.

## Privilege

`.run` programs start as `PROC_PRIVILEGE_USER` processes. Before
dispatching, the kernel checks the call against a deny list
(`syscall_user_may_call()` in `syscall.c`); a denied call returns
`SYS_ERR_PERM` and does nothing.

| Denied to `.run` programs | Why |
| --- | --- |
| `SYS_GRAPHICS` drawing primitives (clear, pixel, line, rectangle, circle, triangle, ellipse, text, measure text, set resolution, terminal clear) | User programs draw into their own surface and use `mos_graphics_blit_surface` + `mos_graphics_present` |
| `SYS_MANAGER` init, update, start, set sample rate | They reconfigure hardware every process shares |
| `SYS_USB_INIT`, `SYS_MOUSE_INIT` | They re-enumerate devices the whole system uses |
| `SYS_NET_DHCP` | It rewrites the address of the shared interface |
| `SYS_USB_KEY_IS_PRESSED`, `SYS_MOUSE_GET_STATE`, `SYS_INPUT_READ` | Allowed only to the process that owns the screen |

Everything else - files, heap, network connections, windows, packages,
sysinfo, process list - is open to user programs. The table is not a
memory-protection boundary; it closes the specific hole of a program
reconfiguring shared hardware through the syscall gate.

## Which header

| Include | You get |
| --- | --- |
| `minimalos.h` | Core calls: terminal output, exit, time, files, packages, exec, process list, framebuffer ownership and blit |
| `stdio.h` | `printf` (includes `minimalos.h`) |
| `stdlib.h` | `malloc` and friends, random bytes, `atoi` (includes `minimalos.h`) |
| `string.h` | String and memory helpers (no syscalls) |
| `window.h` | mosWM windows and their events |
| `graphics.h` | Short `graphics_*` names for the framebuffer API |
| `net.h` | Networking (includes `minimalos.h`) |
| `user_sysinfo.h` | System statistics |
| `iostream` | `std::cout` for C++ |
| `x86_64/user_syscalls.h` | Raw `uint64_t` wrappers: `mos_syscall3`, `mos_graphics`, `mos_mouse`, `mos_usb`, `mos_manager`, `mos_sysinfo`, `mos_input_read` |
| `x86_64/user_mouse.h`, `x86_64/user_usb.h`, `x86_64/user_managers.h` | Mouse, USB keyboard and manager wrappers |

Both header families include `x86_64/user_common.h`, so every wrapper is
defined exactly once and any combination of headers can be included
together.

## Units

- Time: milliseconds. `mos_uptime()` and `mos_sleep()` both use
  milliseconds; so do every `timeout_ms` argument.
- Colours: `window.h` and `mos_graphics_blit_surface` take ARGB8888
  pixels, `0xAARRGGBB`. Use `0xFF` alpha for opaque pixels. The
  `mos_graphics_*` primitives take separate `r`, `g`, `b` bytes.
- IPv4 addresses and ports in `net.h` are host order: `192.168.1.10` is
  `0xC0A8010A`.
- Paths are MinimaFS paths such as `0:/programs/app.run`. Relative paths
  are resolved against the terminal's current directory.

## Memory

There is no libc and no dynamic linker. Everything a program uses is
either an inline wrapper from these headers or code compiled into the
program. `crt0.c` supplies `_start`, runs global constructors
(`.init_array`), calls `main(argc, argv)` and then `mos_exit()` with its
return value. It also defines out-of-line `memcpy`, `memmove`, `memset`
and `memcmp`, which the compiler may call on its own.
