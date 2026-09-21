# Minimal-OS Documentation Index

This directory contains the system documentation. Start with the guide that
matches the layer you are changing.

## Guides

| Path | Responsibility |
| --- | --- |
| [Boot and kernel](boot-and-kernel.md) | Boot transition, memory, processes, SMP, scheduling, and syscalls |
| [Hardware](hardware-and-drivers.md) | PCI, graphics, USB, audio, storage, and driver managers |
| [Storage and packages](storage-and-packages.md) | MinimaFS, disk commands, `.mpkg`, install directories, and format |
| [Networking](networking.md) | Ethernet, IPv4, ARP, UDP, DHCP, DNS, TCP, and TLS |
| [Services](services.md) | `.service` files, startup commands, supervision, and registration |
| [User programs](user-programs.md) | SDK, `.run`, `.slib`, ELF loading, and user/kernel boundaries |
| [Build and debugging](build-and-debugging.md) | Make targets, ISO generation, QEMU, tests, and logs |
| [SDK syscalls](user-sdk-syscalls.md) | Detailed user-space syscall API reference |
| [Install and services](install-and-services.md) | Short installation and service quick-start |

## Source Map

| `src/impl/boot` | Multiboot2 entry, page tables, long-mode transition |
| `src/impl/kernel` | Startup, memory, scheduler, syscalls, services |
| `src/impl/proc` | Processes, threads, context switching, run queues |
| `src/impl/filesystem` | MinimaFS and AHCI-backed disk operations |
| `src/impl/net` | Network stack and TLS client |
| `src/modules` | Hardware drivers and device managers |
| `src/impl/commands` | Terminal commands |
| `src/intf` | Kernel interfaces and shared headers |
| `Minimal-OS-SDK` | User programs, libraries, headers, and SDK tools |
| `tools` | Host builders, tests, QEMU scripts, and utilities |
