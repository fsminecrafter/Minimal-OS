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

## Startup Order

`kernel_main()` in `src/impl/kernel/main.c` performs the high-level startup:

- serial output, BSP/SMP state, GDT, and command registration;
- memory and startup routines;
- PCI enumeration;
- USB and graphics driver-manager registration;
- RTL8139 network registration, Ethernet/IP/UDP/TCP setup, and network syscall state;
- time and random seeding;
- audio and AHCI storage-driver registration;
- AP startup, interrupts, kernel background processes, and the terminal.

The terminal owns the disk interaction prompt. It asks whether to mount a disk,
starts keyboard update tasks, and calls `service_manager_init()` after the boot
disk is mounted.

## Memory

The physical memory manager (`src/impl/pmm.c`) allocates 4 KiB pages. The heap
allocator (`src/impl/kernel/allocator.c`) provides variable-size allocations
for kernel structures, file buffers, process objects, and driver state.

Use page allocation for DMA-aligned or page-owned memory. Use `alloc()` or
`alloc_unzeroed()` for ordinary objects. Large structures such as directory
descriptors are deliberately heap allocated because kernel stacks are small.

## Processes And Threads

Kernel processes are created with `createProcess(name, entry)`. A process entry
must either loop, call `sleep()`, or terminate through `process_exit()`/`kill()`;
returning from a raw kernel entry is not a normal completion mechanism.

User `.run` programs are loaded as user processes. The loader creates an image,
user stack, page mappings, and argument block, then enters ring 3 through the
process trampoline. See [user programs](user-programs.md).

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
