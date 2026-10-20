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

## Disks, partitions and drives

```text
disks                                  list disks, partitions, filesystems
part sata1                             show sata1's partition table
part sata1 init gpt --yes              new empty GPT (or mbr)
part sata1 add 500G minimafs data      add a partition (sizes: 512M, 20G, 2T, rest)
part sata1 del 2                       delete partition 2
format sata1p1 1 data                  MinimaFS on sata1p1, mounted as 1:
mount sata1p2 2                        mount an existing volume as 2:
umount 2
drives                                 mounted drives and their usage
format                                 (no arguments) the boot volume as 0:, with system files
```

At boot the system volume is mounted as `0:` (the one named by
`root=<partition GUID or device>` on the kernel command line, else the
first MinimaFS volume found), and every other MinimaFS volume as `1:`,
`2:`, ... in disk order.

A MinimaFS volume can hold up to 2^32 - 2 blocks of 4 KiB, just under
16 TiB. Free space is tracked as a list of used extents rebuilt at mount
time, so memory use does not grow with the volume size. MBR can address
disks up to 2 TiB; use GPT beyond that (the default).

MinimaFS partitions use GPT type `4D696E69-6D61-4653-B0A7-4D4F534D4653`
or MBR type `0x7F`.

To make a disk boot by itself, use `install` (see
[Installing](installing.md)).

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
