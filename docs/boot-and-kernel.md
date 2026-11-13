# Boot And Kernel

## Boot Chain

GRUB loads the Multiboot2 kernel from the ISO. The boot entry is split between
`src/impl/boot/main.asm` and `src/impl/boot/main64.asm`:

1. Save the Multiboot information pointer.
2. Verify the Multiboot signature, CPUID, and x86_64 long-mode support.
3. Build initial PML4, PDPT, and page-directory tables.
4. Enable PAE, long mode, NX, and paging.
5. Load the 64-bit GDT and jump to `long_mode_start`.
6. Pass the Multiboot information address to `kernel_main()`.

The initial low-memory map uses 2 MiB pages. PML4 slot 511 is reserved for a
high MMIO address range. Later device mappings are made by `src/impl/kernel/mmio.c`.
The Multiboot memory-map tag is parsed by `multiboot2parse.c` to determine the
available RAM reported to the allocator and system information commands.

The GRUB menu provides three entries: `Minimal-OS` for normal startup,
`Minimal-OS Memory Tester x64` for a standalone pattern test of free PMM pages,
and `Minimal-OS (No Disk)` to boot without probing or mounting storage. The
memory test reserves only free PMM pages and halts after reporting PASS or FAIL.

## Startup Order

`kernel_main()` in `src/impl/kernel/main.c` performs the high-level startup:

- serial output, BSP/SMP state, GDT, and command registration;
- memory and startup routines;
- PCI enumeration;
- USB and graphics driver-manager registration;
- network driver registration (RTL8139, RTL8168), Ethernet/IP/UDP/TCP setup, and network syscall state;
- time and random seeding;
- audio and AHCI storage-driver registration;
- AP startup, interrupts, kernel background processes, and the terminal.

The terminal owns the disk interaction. In normal mode it probes and mounts the
disk and starts services; no-disk mode skips that path and still starts the
terminal and keyboard tasks.

RamFS mode (GRUB entry "Minimal-OS (RamFS)", kernel option `ramfs` or
`ramfs=<MiB>`) runs the whole system from RAM, which makes it safe for
testing on a real PC. Real disks (AHCI, NVMe) are never probed, so nothing
reads or writes them. Instead a zeroed RAM disk, `ram0` (64 MiB by default,
less if memory is short, at least 16 MiB), is formatted as `0:`, the system
files are unpacked into it as `format` would, and startup goes on as usual
(services, desktop). Every change is lost at the next boot. A boot module
named `mosfirmware` (an `.mpkg`) is unpacked into `0:/firmware` (by `format`
and `install` too); `make gpu-setup` puts one on the ISO with NVIDIA's
firmware files (see [gpu.md](gpu.md#firmware-files)), and the test VM uses
it the same way.

The kernel option `serial-echo` copies everything the terminal prints to
COM1, so a headless VM's serial log contains command output as well as the
kernel's own messages (`tools/hyperv/mosvm.sh` boots with it).

## SIMD State (SSE, AVX, AVX-512)

The kernel is compiled without AVX; only user programs use the wide
registers. `cpu_simd_init_bsp()` (`src/impl/cpu_simd.c`) runs first thing in
`kernel_main`, before the IDT is loaded, and every AP runs `cpu_simd_init_ap()`
at the top of `ap_entry_c`, before it can take an interrupt. When CPUID reports
XSAVE and AVX they set `CR4.OSXSAVE` and `XCR0` to x87|SSE|AVX, plus opmask,
ZMM_Hi256 and Hi16_ZMM when the CPU has AVX-512F. The save area size is
`CPUID.(0xD,0).EBX` for that XCR0, rounded up to 64 bytes (AVX2: 832, AVX-512:
2752). Without XSAVE or AVX, or with the `noavx` boot flag, nothing changes:
`CR4.OSFXSR` only, `fxsave64`, 512 bytes.

Nothing in `context_switch.asm` touches SIMD registers. Every kernel entry (the
exception stub in `idt_isr.asm`, the IRQ stubs in `idt_.asm`, `int 0x80` in
`syscall_isr.asm`) saves the interrupted code's state in a 64-byte aligned
area on the kernel stack and restores it before `iretq`, through the
`SIMD_SAVE`/`SIMD_RESTORE` macros in `src/impl/simd_save.inc`. A context
switch happens inside a timer IRQ or a blocking syscall, deeper than one of
those stubs, so a sleeping process's state is already on its own kernel stack
and comes back when it unwinds. The macros pick `fxsave64` or `xsave64`
(header zeroed first, `EDX:EAX` = `simd_xcr0`) from the global
`simd_use_xsave`, which is final before interrupts are first enabled, so one
kernel runs on both kinds of CPU. `xsaveopt` is not used: its init/modified
tracking is tied to the linear address of the last `xrstor`, and stack
addresses are reused between processes.

Kernel stacks grew because of this. Entries nest: a syscall, a timer IRQ while
it sleeps, a page fault in the kernel, each with up to ~2.7 KiB of state plus
its frame. At 16 KiB that left about 7 KiB for C code (14 KiB before), so a
process kernel stack is now `KERNEL_STACK_SIZE` = 32 KiB, AP boot stacks are 32
KiB, and the per-CPU TSS stack (the double fault IST stack) is 16 KiB instead
of 4 KiB. The boot CPU's stack is 256 KiB. A new process starts with whatever
the previous code left in the registers (not cleared: a leak, not corruption).

`SYS_SYSINFO` returns the `SYS_CPU_*` feature mask (`SYS_SYSINFO_CPU_FEATURES`)
and the enabled XCR0 (`SYS_SYSINFO_XCR0`); the AVX-family bits are set only
when the state is enabled, so a set bit means usable.

**Boot flag `noavx`** keeps the SSE-only behaviour (a fallback for hardware
trouble). It goes on the multiboot command line like `ramfs`; the GRUB entry
"Minimal-OS (RamFS, noavx)" sets it. `tools/debug/ci_os0.sh` can boot it
(see [testing.md](testing.md#os0-self-test-in-qemu)).

## Memory

The physical memory manager (`src/impl/pmm.c`) allocates 4 KiB pages. The heap
allocator (`src/impl/kernel/allocator.c`) provides variable-size allocations
for kernel structures, file buffers, process objects, and driver state.

Use page allocation for DMA-aligned or page-owned memory. Use `alloc()` or
`alloc_unzeroed()` for ordinary objects. Large structures such as directory
descriptors are deliberately heap allocated because kernel stacks are small.

At boot the kernel takes the largest free block of RAM below 4 GiB. The heap
gets the lower half and the page pool the upper half (at least 64 MiB). The
kernel reaches all of it through an identity mapping of the first 4 GiB, which
every process shares.

Program memory above 4 GiB is managed by `src/impl/proc/uvm.c`: a sorted list
of mapped areas per program, with a protection each. A page gets a frame from
the page pool on its first use, in the page-fault handler, and reads as zero
until written. `SYS_VM` maps, unmaps, protects and decommits such areas. Kernel
code copies into or out of another program's memory with `uvm_copy_in()` and
`uvm_copy_out()`, which work through the frames whatever page tables are loaded.

## Processes And Threads

Kernel processes are created with `createProcess(name, entry)`. A process entry
must either loop, call `sleep()`, or terminate through `process_exit()`/`kill()`;
returning from a raw kernel entry is not a normal completion mechanism.

User `.run` programs are loaded as user processes. The loader maps the image,
the main stack and the argument block into the new program's address space, then
enters ring 3 through the process trampoline. See [user programs](user-programs.md).

Threads are lightweight wrappers around kernel processes. `thread_join()` polls
for process termination while yielding through `sleep()`.

## Scheduling And SMP

Each online CPU owns a run queue with three levels:

- interactive: terminal input and short blocking work;
- normal: ordinary processes;
- background: CPU-heavy or lower-priority work.

A process that consumes its quantum is demoted. A process that blocks is
promoted on wake. Periodic boosting prevents starvation, and periodic
rebalancing moves queued processes between CPUs when the load difference is
large enough.

Run-queue locks are SMP-safe spinlocks. `cli` only disables interrupts on the
current CPU; it is not a replacement for an atomic lock shared by CPUs.

`process_t` contains fields consumed by context-switch assembly. Fields before
the documented ABI boundary must not move without updating the assembly and
verification code.

## Syscall Boundary

User code invokes `int 0x80`. The syscall ISR saves registers into the packed
`syscall_regs_t` layout, and `syscall_dispatch()` validates the requested
operation and user pointers before calling a subsystem implementation.

The user privilege table is separate from the syscall number table. For
example, a user program may use TCP and filesystem calls, while DHCP remains a
privileged operation because it changes the machine-wide network configuration.
See [user SDK syscalls](user-sdk-syscalls.md) for the complete user-facing API.

## Kernel Interfaces

Public kernel contracts live in `src/intf`:

- `x86_64/proc.h` and `x86_64/scheduler.h` define process/scheduler state.
- `x86_64/allocator.h` and `x86_64/pmm.h` define memory allocation.
- `x86_64/minimafs.h` defines filesystem objects and operations.
- `x86_64/syscall.h` defines the syscall ABI structures and constants.
- subsystem manager headers define driver registration and lifecycle methods.

Implementations can grow internal structures, but shared headers and assembly
offset contracts must remain synchronized.
