# Build And Debugging

## Prerequisites

The documented Debian/Ubuntu dependencies include GCC/binutils tooling, NASM,
GRUB tools, xorriso, make, and QEMU. The repository's
`build_gccbinutils.sh` creates the cross-toolchain expected by the Makefile at
`$HOME/cross/bin/x86_64-elf-gcc` and `x86_64-elf-ld`.

Create a QEMU disk image before running the full storage workflow:

```sh
fallocate -l 256M sata256.img
```

## Make Targets

```sh
make build-x86_64
make run
make minimaSSL-slib
make clean
```

`build-x86_64` compiles enabled modules, kernel implementation files, minimaSSL
crypto sources, resource packages, and embedded package objects. It links
`dist/x86_64/kernel.bin`, copies it to the ISO input tree, and creates
`dist/x86_64/kernel.iso`.

`run` builds first, then starts QEMU with the ISO, serial console, USB keyboard,
AHCI disk, RTL8139 networking, and audio devices.

`minimaSSL-slib` builds the SDK library and copies the result to the distribution
build. The install1 rule then copies it into `src/resources/install1` before
building the embedded package.

`clean` removes kernel and distribution output, `Minimal-OS-SDK/build`, generated
`.run` files, generated minimaSSL bundles, and package objects. It does not
remove source resources or the disk image.

## Resource Rebuilds

Resource directories are packaged into install1, install2, and install3:

```text
src/resources/install1 -> 0:/Etc
src/resources/install2 -> 0:/programs
src/resources/install3 -> 0:/services
```

Files added to install2/install3 are included as package prerequisites, so a
changed program or service causes the corresponding embedded package to rebuild.
Run `make clean` when removing a generated file should also remove its stale
package object.

## HTTPS Test

Run the focused TLS integration test:

```sh
bash tools/debug/https_test.sh
```

The script:

1. builds the kernel;
2. creates a temporary self-signed certificate;
3. starts `openssl s_server -tls1_3` on the host;
4. starts QEMU with user-mode networking;
5. sends `n` at the disk prompt;
6. runs `dhcp`;
7. runs `wget https://10.0.2.2:8443/`;
8. checks for handshake and download completion.

The test intentionally uses an IP address and defers certificate verification.
It validates the TLS record/key schedule and HTTP exchange, not public CA trust.

The crypto known-answer test is:

```sh
gcc -std=c11 -I Minimal-OS-SDK/libraries/minimaSSL/include \
  tools/debug/mssl_tls_gcm_kat.c \
  Minimal-OS-SDK/libraries/minimaSSL/src/tls_gcm.c \
  -o /tmp/mssl_tls_gcm_kat
/tmp/mssl_tls_gcm_kat
```

## CI And Interactive Tests

```sh
make ci-test
make ci-interact
```

`ci_test.sh` is intended for automated checks. `ci_interact.sh` drives the
terminal over serial and waits for command-specific success patterns. When a
pattern fails, inspect its serial output and the generated QEMU log.

## Logs And Diagnostics

Useful output sources include:

- serial terminal output for initialization, commands, services, and TLS;
- `tools/debug/https_test.log` for the HTTPS test guest console;
- `tools/debug/https_server.log` for OpenSSL trace output;
- `qemu.log` for QEMU guest errors and interrupts.

OpenSSL's `bad record mac` means it rejected an authenticated TLS record before
it could decode the encrypted inner message. Compare it with the guest's TLS
stage message; the server-side error is often the primary failure and the guest
receive error is a follow-on alert/close.

## Debugging Rules

Build the smallest focused test after a change. Preserve existing user changes
in a dirty worktree. Avoid using generated logs as source-of-truth code. For
kernel crashes, inspect process creation, allocator state, interrupt state,
serial output, and the last subsystem initialization message in that order.
