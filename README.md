# Minimal-OS SDK (v1.1)

Minimal-OS SDK is a Software Development Kit for Minimal-OS, Linked here: [Minimal-OS](https://github.com/fsminecrafter/Minimal-OS).

Every function in the SDK headers is documented in [`docs/`](docs/README.md):
its signature, parameters, return values, errors and an example, grouped by
header. The same reference is on the website under Docs → SDK functions, and
in Run Studio's docs panel.


Install the native compiler and linker tools on Debian or Ubuntu:

    ./install-toolchain.sh

Build a C or C++ program and package it as a `.run`:

    ./build.sh examples/hello.c

This creates `hello.run` as a single file containing the program bundle. For
C++, use the same command with a `.cpp` file:

    ./build.sh examples/hello.cpp

You can also pass a project folder. It uses `main.cpp` or `main.c` as the
program entry source, or select another source explicitly:

    ./build.sh programs
    ./build.sh programs --main programs/main.cpp

`-h`/`--help` prints usage. `--output <path>` selects the generated `.run` or
`-h`/`--help` prints usage. `--output <path>` selects the generated `.run` or
`.slib` path.

### Install the MSDK command

From this directory, run:

    ./install.sh

The installer links both `MSDK` and `msdk` into `~/.local/bin` and adds that
directory to `~/.bashrc` once. Apply the PATH update in the current terminal
with `source ~/.bashrc`, or open a new terminal.

Both command spellings accept the SDK build options:

    MSDK --help
    MSDK projectfolder --main entry.cpp
    msdk main.cpp

The same launcher works when invoked directly as `./msdk` from the SDK folder.

The Minimal-OS `dlr`/`deliver` program is placed directly into the OS image's
program package when built from this checkout:

    ./build.sh programs/dlr

This writes `../src/resources/install2/dlr.run`, which is installed as
`0:/programs/dlr.run` by the normal image build. Its HTTPS transport uses the
same kernel-linked minimaSSL implementation as `wget`; the matching library
bundle is installed at `0:/etc/minimaSSL.slib`.

Build a shared library with `--slib`:

    ./build.sh --slib libraries/math

Example library:

    ./build.sh --slib examples/example-slib

The `.slib` bundle contains the linked ELF, an export manifest of global
symbols, and public `.h`/`.hpp` files. When a library has an `include/`
directory, those headers are stored relative to it under the bundle's
`include/` path; otherwise headers are stored relative to the library folder.
Headers are kept as public interface metadata for the runtime loader; `static`
symbols are not exported. The bundle format is `MINISLIB1`; kernel-side loading and
per-process shared/private mapping are provided by the operating system, not
by this packaging script.

The build script compiles `crt0.c`, compiles the selected source, links
`main.elf` with `link.ld`, and packages it with `mkrun.sh`. It uses `gcc`,
`g++`, and `ld` by default. Set `CC`, `CXX`, or `LD` to use a cross-toolchain, for
example `CC=x86_64-elf-gcc CXX=x86_64-elf-g++ LD=x86_64-elf-ld`.

The equivalent manual steps are:

    x86_64-elf-gcc -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c crt0.c -o crt0.o

    x86_64-elf-gcc -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c examples/hello.c -o hello.o

    x86_64-elf-ld -pie --no-dynamic-linker -T link.ld -e _start \
        -o main.elf crt0.o hello.o

    ./mkrun.sh main.elf hello.run

Import the `hello.run` file itself into MinimaFS. Then inside Minimal-OS run:

    run 0:/programs/hello.run

The `.run` file uses the `MINIRUN1` format. Its manifest contains `main.elf`
and every file below a `Resources` directory when those files exist. The
manifest and payload are stored in the same regular file, so copying or
importing the bundle does not require creating a directory tree.

For the C++ example, compile with the same freestanding flags plus the C++
runtime restrictions:

    x86_64-elf-g++ -std=gnu++11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions \
        -fno-rtti -fno-threadsafe-statics -fPIE -Iinclude \
        -c examples/hello.cpp -o hello.o

The C and C++ examples can use `printf` from `include/stdio.h` and
`std::cout` from `include/iostream`. They are header-only and write through
`mos_write`; no libc or C++ standard library is linked. The supported printf
formats are `%s`, `%c`, `%d`, `%i`, `%u`, `%x`, `%X`, `%ld`, `%lu`, and `%%`.

## Requirements on the ELF
- x86_64, ELF64, PIE (`ET_DYN`), linked at base address 0 (link.ld does this).
- No external symbols - everything the program needs must be compiled
  into it. There's no dynamic linker; only `R_X86_64_RELATIVE`
  relocations are processed at load time.

## C++
`-fno-exceptions -fno-rtti -fno-threadsafe-statics`. Global objects with
non-trivial constructors now work: `crt0.c` walks `.init_array` before
calling `main()`, using the same ctor-section-walk pattern
`commandhandler.c` uses for `REGISTER_COMMAND`. Destructors
(`.fini_array`) are still not run - a program ends via `mos_exit()`,
which does not return.

## Syscalls available
- `mos_write(fd, buf, len)` - fd 1/2 only, goes to the terminal + serial.
- `mos_exit(code)`
- `mos_register_cleanup(callback)`
- `mos_getpid()`
- `mos_uptime()` and `mos_sleep(ticks)`
- `mos_open(path, flags)`, `mos_read(fd, buf, len)`, `mos_close(fd)`
- `mos_exists(path)`, `mos_is_dir(path)`, and `mos_mkdir(path)`
- `mos_gettime()`, `mos_seek(fd, offset)`, and `mos_size(fd)`

When the terminal receives Ctrl+C once for a running user program, its
registered cleanup callback is requested. The callback should save any needed
state and call `mos_exit()`. If it does not exit within one second, the kernel
force-terminates the program. A second Ctrl+C within 800 ms force-terminates
it immediately. Programs without a callback are terminated immediately.

Process kill and force-termination controls remain kernel/terminal operations;
the SDK does not expose a syscall for killing arbitrary processes.

The current x86_64 user syscall API is also available through the subsystem
headers under `include/x86_64/`:

- `x86_64/user_syscalls.h` - uint64_t syscall wrappers, graphics dispatch,
    manager dispatch, and USB dispatch.
- `x86_64/user_graphics.h` - primitive drawing, text, and resolution wrappers.
- `x86_64/user_managers.h` - GPU, audio, storage, and USB manager operations.
- `x86_64/user_usb.h` - USB initialization, polling, keyboard translation, and
    copied keyboard metadata.

For a shorter graphics API, include `graphics.h` and use the corresponding
`graphics_*` names. It is usable from both C and C++ without libc or the C++
standard library:

        #include "graphics.h"

        void main(void) {
                graphics_clear(0, 0, 0);
                graphics_fill_rectangle(10, 10, 100, 60, 40, 160, 240);
                graphics_text("hello", 20, 20, 255, 255, 255);
                mos_exit();
        }

The new raw syscall numbers are `SYS_GRAPHICS` (15), `SYS_MANAGER` (16), and
`SYS_USB` (17). The request structures and operation constants are available
from `x86_64/syscall.h`.

The syscall constants, error values, and kernel-side register frame are also
available from `include/syscall.h`. `SYS_O_RDONLY` is the only open flag
currently defined.

## Networking, heap, and randomness (SDK v1.1)

Three syscalls were added so userland can do more than draw and read
files. Include `net.h` for networking, `stdlib.h` for memory, and
`string.h` for the usual string/memory helpers.

- `SYS_NET` (30) - `net.h`. TCP connect/send/recv/close, UDP
  bind/recv/send (broadcast included), DNS resolution, interface
  status, and an explicit poll. Addresses are host-order `uint32_t`
  throughout; `mos_ip_parse()` and `mos_ip_to_string()` convert.
  Connections are integer handles, not kernel pointers.
- `SYS_HEAP` (31) - `stdlib.h`. `malloc`, `calloc`, `realloc`, `free`.
  Backed by the kernel allocator, since every process still shares the
  kernel PML4. Before this there was no heap at all.
- `SYS_RANDOM` (32) - `stdlib.h`. `mos_random_bytes()`. RDRAND where the
  CPU has it, TSC/RTC-derived otherwise.

The interface must already have an address: run `dhcp` in the terminal
first. A `.run` program calling `mos_net_dhcp()` gets `SYS_ERR_PERM` on
purpose - acquiring a lease rewrites the address for every process on
the machine, so it stays an operator action.

`SYS_ERR_BUSY` is new and is not a failure. The network stack builds its
frames in file-static buffers, so only one caller can be inside it at a
time; a caller that gets `BUSY` should retry rather than give up.

`examples/nettest.c` exercises all of it:

    ./build.sh examples/nettest.c
    # then, inside Minimal-OS:
    dhcp
    run 0:/programs/nettest.run                 # DNS + HTTP HEAD
    run 0:/programs/nettest.run --discover      # UDP broadcast discovery

Note for QEMU: under `-netdev user` the guest is NATed and LAN
broadcasts never reach it, so `--discover` will find nothing however
correct your code is. Use a tap or bridge netdev to test discovery.

`crt0.c` now also defines out-of-line `memcpy`/`memmove`/`memset`/
`memcmp` (GCC emits calls to these on its own, even under
`-fno-builtin`) and walks `.init_array`, so C++ globals with non-trivial
constructors work - the restriction the C++ section above describes no
longer applies. Destructors still do not run.

## If you didnt know

This SDK is designed for fsminecrafter/Minimal-OS.

(:
