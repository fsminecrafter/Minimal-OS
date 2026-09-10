# MinimalOS SDK (v1)

Build a program, package it as a `.run`:

    x86_64-elf-gcc -std=c11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c crt0.c -o crt0.o

    x86_64-elf-gcc -std=c11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c examples/hello.c -o hello.o

    x86_64-elf-ld -pie --no-dynamic-linker -T link.ld -e _start \
        -o main.elf crt0.o hello.o

    ./mkrun.sh main.elf hello.run

Then inside MinimalOS: `run 0:/programs/hello.run`

## Requirements on the ELF
- x86_64, ELF64, PIE (`ET_DYN`), linked at base address 0 (link.ld does this).
- No external symbols - everything the program needs must be compiled
  into it. There's no dynamic linker; only `R_X86_64_RELATIVE`
  relocations are processed at load time.

## C++
`-fno-exceptions -fno-rtti -fno-threadsafe-statics` and skip global
objects with non-trivial constructors for now - `crt0.c` doesn't walk
`.init_array` yet (it could, using the same ctor-section-walk pattern
`commandhandler.c` already uses for `REGISTER_COMMAND`, if that's
wanted next).

## Syscalls available today
- `mos_write(fd, buf, len)` - fd 1/2 only, goes to the terminal + serial.
- `mos_exit(code)`
- `mos_getpid()`