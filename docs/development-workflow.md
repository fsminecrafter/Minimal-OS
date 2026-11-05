# Development Workflow

This guide helps you choose where a change belongs, wire it into a build, and
select a useful validation command. For subsystem behavior, start with the
matching guide in the [documentation index](README.md).

## Find the right layer

| Change | Start here |
| --- | --- |
| Boot protocol, page tables, interrupts, scheduler, syscalls | `src/impl/boot`, `src/impl/kernel`, `src/impl/proc`; shared contracts in `src/intf/x86_64` |
| Shell command behavior | `src/impl/commands` and its declarations in `src/intf/commands` |
| MinimaFS, block devices, partitioning, installation | `src/impl/filesystem`, `src/impl/storage`, `src/impl/install` |
| Ethernet and network protocols | `src/impl/net` and `src/modules/Network` |
| Optional hardware modules | `src/modules`; module selection is generated into `config.mk` |
| Built-in program, service, or configuration file | `src/resources/install1`, `install2`, or `install3` |
| User program, SDK header, or user library | `Minimal-OS-SDK` |
| Host-side packaging, test, or developer utility | `tools` and, for host tests, `tests/host` |

The high-level boot and component relationships are in
[Boot and Kernel](boot-and-kernel.md). Driver registration and hardware
fallbacks are described in [Hardware](hardware-and-drivers.md).

## Preserve public contracts

Before changing a shared interface, check who consumes it. Kernel interfaces
and data structures are declared in `src/intf`; user programs call the syscall
ABI through the SDK. The syscall register frame and process fields also have
assembly consumers, so a layout change may require synchronized updates to
assembly and ABI checks. See [SDK conventions](conventions.md) and
[Boot and Kernel](boot-and-kernel.md#syscall-boundary).

Keep kernel-only functionality in the kernel. User syscall access is governed
by the privilege table as well as by the syscall-number table; an operation
that changes machine-wide state may intentionally remain unavailable to
ordinary user programs.

## Add or configure a hardware module

`config.sh` discovers C and assembly files under `src/modules` and edits the
generated `config.mk` interactively:

```sh
./config.sh
```

The interface groups source files by directory. Use arrow keys or `j`/`k` to
navigate, Space to toggle an item, and Enter to save. `q` exits without saving.
Known dependencies are enabled or disabled together; review the resulting
`config.mk` before building. Add dependency relationships to `config.sh` when a
new module requires other optional modules.

The module key is derived from the source path, for example
`src/modules/Audio/audio.c` becomes `MODULE_Audio_audio_c`. Newly discovered
modules default to enabled. Use `make build-x86_64` to check the selected
configuration.

When adding a driver, keep device detection in the driver and expose
subsystem-level operations through the existing manager pattern. Define a
degraded behavior for absent or unsupported devices rather than making an
optional device a boot requirement. See [Hardware](hardware-and-drivers.md#adding-a-driver).

## Add a built-in resource

The build embeds three resource trees in the kernel image:

| Source directory | Installed path after `format` |
| --- | --- |
| `src/resources/install1` | `0:/etc` |
| `src/resources/install2` | `0:/programs` |
| `src/resources/install3` | `0:/services` |

Put a service definition in `install3` and its `.run` program in `install2`.
Services are discovered after the system volume has been mounted; no C
registration call is needed. See [Services](services.md) and
[Storage and Packages](storage-and-packages.md#embedded-install-packages).

## Build and validate

For a kernel change, a useful baseline is:

```sh
make build-x86_64
make test-host
```

`build-x86_64` also produces the BIOS/UEFI ISO. `test-host` runs focused tests
without booting a VM. For boot-time or command behavior, use `make ci-test` or
`make ci-interact`; use the smallest applicable test first. See
[Build Targets](build-targets.md) and [Testing](testing.md).

For syscall or user-program changes, build the relevant SDK program as well as
the kernel. Keep the kernel and user SDK headers in sync: MOSdesk programs are
built against this checkout's `Minimal-OS-SDK` headers.

## Generated files and cleanup

Build outputs go under `build/` and `dist/`; the ISO input tree is under
`targets/x86_64/iso`. Package inputs in `src/resources` are source files and
should be edited directly.

Review the `clean` target before running it: it deletes every `*.run` file it
finds under the repository, not only generated bundles. Preserve any `.run`
files you need. See [Build Targets](build-targets.md#cleanup).

Keep local logs and generated images out of source changes unless they are
intentional deliverables. Before handing off a change, inspect `git status` and
`git diff --check`.
