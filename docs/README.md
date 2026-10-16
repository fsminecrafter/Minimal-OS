# Minimal-OS SDK function reference

Every function the SDK headers give a `.run` program, one entry per
function: its signature, what each parameter means, what it returns, the
errors to expect, and a short example. The headers are the source of
truth; these pages describe them as of SDK v1.1 and the kernel's
`syscall.c`.

| Page | Header | What is in it |
| --- | --- | --- |
| [Conventions and errors](conventions.md) | `syscall.h` | Calling convention, result codes, privilege rules, which header to include |
| [Process and terminal](process.md) | `minimalos.h` | Output to the terminal, exit, sleep, time, PIDs, launching programs, cleanup on Ctrl+C |
| [Files](files.md) | `minimalos.h` | MinimaFS: open, create, read, write, seek, list, delete, metadata |
| [Memory and randomness](memory.md) | `stdlib.h` | `malloc`, `calloc`, `realloc`, `free`, random bytes, `atoi` |
| [Strings and memory](strings.md) | `string.h` | `strlen`, `strcmp`, `strstr`, `memcpy`, constant-time compare … |
| [Formatted output](output.md) | `stdio.h`, `iostream` | `printf` and `std::cout` |
| [Windows](windows.md) | `window.h` | mosWM windows: create, draw, present, events |
| [Graphics](graphics.md) | `graphics.h`, `x86_64/user_graphics.h` | Full-screen framebuffer: acquire, blit, present, primitives |
| [Input](input.md) | `x86_64/user_syscalls.h`, `user_mouse.h`, `user_usb.h` | Raw keyboard and mouse events for full-screen programs |
| [Networking](network.md) | `net.h` | DNS, TCP client and server, UDP, the ktls transport |
| [System information](sysinfo.md) | `user_sysinfo.h` | CPU, RAM, heap, cores, uptime, processes |
| [Packages](packages.md) | `minimalos.h` | `.mpkg` archives: unzip, zip, inspect |
| [Device managers](managers.md) | `x86_64/user_managers.h` | GPU, audio, storage and USB manager calls |

## Reading an entry

Each function has the same parts:

- **Signature** - the declaration exactly as the header has it. Every
  wrapper is `static inline`, so including a header never adds a symbol to
  your program.
- **Parameters** - one row per argument.
- **Returns** - the success value and the `SYS_ERR_*` values the kernel
  can return for that call.
- **Notes** - limits, ownership rules and anything surprising.
- **Example** - a minimal fragment. Examples assume the headers named at
  the top of the page are included.

## A complete program

```c
#include "minimalos.h"
#include "stdio.h"

int main(int argc, char** argv) {
    printf("hello from pid %ld, %d argument(s)\n", mos_getpid(), argc);
    for (int i = 0; i < argc; i++) printf("argv[%d] = %s\n", i, argv[i]);
    mos_exit(0);
}
```

Build it with `./build.sh hello.c` and run it inside Minimal-OS with
`run 0:/programs/hello.run`. Returning from `main` also ends the program;
`crt0.c` calls `mos_exit()` with the return value.
