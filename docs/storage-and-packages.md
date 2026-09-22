# Storage And Packages

## MinimaFS Model

MinimaFS stores a directory tree in 4 KiB blocks on an AHCI-backed disk.
Mounted drives use paths such as `0:/`, `1:/`, and `0:/programs`. Metadata
records file type, format, size, timestamps, runnable state, and block layout.
Directory descriptors contain entries and storage descriptors track the volume
root, block counts, and free space.

The filesystem API is declared in `src/intf/x86_64/minimafs.h`. Commands should
use resolved MinimaFS paths and the API rather than constructing raw disk
structures.

## Disk Workflow

The usual first-use sequence is:

```text
initdisk
format
mount
```

`initdisk` probes storage and creates the kernel disk wrapper. `format` erases
the selected filesystem area, writes MinimaFS metadata, creates standard
folders, and installs embedded resource packages. `mount` attaches an existing
filesystem without erasing it.

The current allocation bitmap supports at most 256 MiB. The QEMU workflow uses
a `sata256.img` disk image. Formatting a larger physical device is clamped to
the supported size.

## Embedded Install Packages

The root Makefile packages these directories as `MPKG0001` archives:

| Source | Destination after `format` |
| --- | --- |
| `src/resources/install1` | `0:/etc` |
| `src/resources/install2` | `0:/programs` |
| `src/resources/install3` | `0:/services` |

The archives are converted to ELF binary objects with `objcopy` and linked into
the kernel. This is intentional: a newly formatted disk has no files from
which the format command could load an installer.

`install1` receives the generated `minimaSSL.slib`. A service file added to
`install3` is therefore present at `0:/services` before service discovery runs.

## MPKG Format

The host builder is `tools/mkpkg/mkpkg.py`. It writes:

- an 8-byte `MPKG0001` magic;
- a version and entry count;
- fixed-size entry records with names, flags, compression method, sizes, and
  payload offsets;
- stored or LZSS-compressed payload data.

Directory entries have no payload. File entries use either `MPKG_METHOD_STORE`
or `MPKG_METHOD_LZSS`. The kernel validates names, table bounds, payload ranges,
and decompression output before writing an entry.

Use the command interface to work with packages already on disk:

```text
pkg zip 0:/source store
pkg unzip 0:/archive.mpkg 0:/target
pkginfo 0:/archive.mpkg
installpkg 0:/archive.mpkg 0:/target
```

User programs use `mos_pkg_zip()`, `mos_pkg_unzip()`, and `mos_pkg_info()` over
`SYS_PKG`.

## File Operations

Kernel code can create, open, read, write, list, delete, and remove files through
MinimaFS. User code uses the corresponding SDK wrappers:

- `mos_open()` and `mos_close()` manage existing files;
- `mos_read()` and `mos_fwrite()` transfer data;
- `mos_seek()`, `mos_tell()`, `mos_size()`, and `mos_feof()` inspect position;
- `mos_create()` creates an empty file before opening it writable;
- `mos_listdir()`, `mos_exists()`, and `mos_is_dir()` inspect paths;
- `mos_get_metadata()` returns stable user-facing metadata.

Filesystem paths are strings, not user-space integer descriptors in the public
SDK. Open file handles are kernel-owned values returned by `SYS_OPEN` and must
be closed.

## Safety Rules

Package extraction must reject absolute paths and `..` traversal. Validate
sizes before allocating or reading payloads. Use heap memory for directory
structures and large file buffers instead of kernel stacks. When adding a new
format or package feature, keep the host encoder and kernel decoder constants
synchronized.
