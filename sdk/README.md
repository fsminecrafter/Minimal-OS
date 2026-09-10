# MinimalOS SDK

The SDK headers are freestanding and target the existing `int 0x80` syscall ABI.
Build a `.run` entry point with the cross compiler and include `sdk/include`:

```sh
x86_64-elf-gcc -ffreestanding -fno-stack-protector -I sdk/include -c main.c -o main.o
```

Use `#include <minimalos.h>` for the high-level wrappers. File handles are
opaque values returned by `minimalos_open`; the current filesystem interface is
read-only. A successful operation returns a non-negative value. Errors are
represented by `MINIMALOS_ERR_*` values.

The kernel currently runs `.run` programs as trusted kernel processes, so the
SDK does not provide memory protection or handle isolation yet.
