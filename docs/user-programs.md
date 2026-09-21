# User Programs And SDK

## Program Model

The SDK produces self-contained x86_64 PIE programs with no dynamic linker.
The kernel loads these programs as user processes and enters them in ring 3.
The current process model still shares the kernel PML4, so the syscall ABI is a
capability boundary but not yet a fully isolated address-space boundary.

Build a program with:

```sh
Minimal-OS-SDK/build.sh Minimal-OS-SDK/examples/hello.c
```

The output is a `.run` bundle. Import it into MinimaFS, normally under
`0:/programs`, and run it with:

```text
run 0:/programs/hello.run
```

## `.run` Bundles

A `.run` contains a `MINIRUN1` header, an entry table, `main.elf`, and optional
resource files. The kernel extracts `main.elf`, loads its PT_LOAD segments, and
applies supported `R_X86_64_RELATIVE` relocations.

The loader validates:

- ELF64 little-endian format;
- x86_64 machine type;
- `ET_EXEC` or `ET_DYN` image type;
- program-header bounds;
- segment file-size and memory-size relationships;
- maximum file and image sizes.

The SDK linker script keeps the image base-relative. External symbols are not
available because there is no dynamic linker or libc at runtime.

## Arguments And Exit

The first argument is the program path. Extra arguments follow it. The SDK
runtime calls `mos_exit()` to terminate; returning from `main()` is not the
normal exit path in the freestanding environment.

Use `mos_register_cleanup()` when a user program needs a chance to save state
before Ctrl+C or a requested termination. The kernel gives the cleanup callback
a bounded period before force termination.

## SDK Runtime

The SDK provides freestanding headers and small wrappers rather than a normal
libc. Common facilities include:

- `stdio.h`: terminal output helpers;
- `stdlib.h`: heap, random bytes, and integer conversion;
- `string.h`: memory and string operations;
- `minimalos.h`: core syscalls and package/file wrappers;
- `net.h`: TCP, UDP, DNS, and polling;
- `graphics.h`: graphics operations;
- `x86_64/user_sysinfo.h`: system and memory statistics.

The runtime is built with freestanding flags, no stack protector, no exceptions,
no RTTI, and no dynamic linker. See [SDK syscalls](user-sdk-syscalls.md) for
API contracts and examples.

## `.slib` Bundles

The SDK can build a shared-library bundle:

```sh
Minimal-OS-SDK/build.sh --slib Minimal-OS-SDK/libraries/minimaSSL
```

A `.slib` contains:

- `library.elf`;
- `exports.txt` with global symbols;
- public headers under `include/`.

The bundle format is `MINISLIB1`. It is package metadata for a future/runtime
loader; Minimal-OS does not currently dynamically load `.slib` code. The current
kernel TLS path statically links the freestanding minimaSSL implementation and
also installs the `.slib` at `0:/Etc/minimaSSL.slib` for distribution.

## Writing A Program

A minimal program should include the SDK headers, perform work, and terminate:

```c
#include "minimalos.h"
#include "stdio.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    puts("hello from user space");
    mos_exit(0);
    return 0;
}
```

For long-running programs, sleep or poll rather than busy-looping. Network
programs must call `mos_net_poll()` or use the deadline helpers so the kernel
network stack can process incoming frames.

## Security And Capability Notes

User programs can access only syscall operations permitted by the kernel's
privilege table. DHCP is privileged. File, network, process, graphics, USB,
heap, random, and package operations have individual validation paths.

Do not pass kernel pointers through the ABI. Use the public request structures,
user buffers, and returned handles. Always close file, TCP, UDP, and TLS handles.
