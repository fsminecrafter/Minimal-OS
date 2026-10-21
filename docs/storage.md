# Disks and root commands (`storage.h`)

## `mos_storage_list`

```c
long mos_storage_list(syscall_storage_device_t* devices, uint32_t capacity);
```

Fills `devices` with every disk, each followed by its partitions, and
returns how many there are (which can be more than `capacity`). Read-only;
any program may call it.

| Field | Meaning |
| --- | --- |
| `name`, `parent` | `sata0`, `nvme0p2`; partitions name their disk |
| `model`, `label` | Drive model; GPT partition name |
| `kind` | `SYSCALL_STORAGE_KIND_DISK` or `_PARTITION` |
| `table` | Disks: `SYSCALL_STORAGE_TABLE_NONE`, `_MBR`, `_GPT` |
| `fs` | 0 unknown, 1 empty, 2 MinimaFS, 3 FAT, 4 partitioned |
| `part_kind` | 0 MinimaFS, 1 EFI System, 2 BIOS boot, 3 FAT, 4 other |
| `mounted_drive` | Drive number, or -1 |
| `flags` | `SYSCALL_STORAGE_FLAG_BOOT` (holds 0:), `_IN_USE` (something on the disk is mounted) |
| `size_bytes`, `sector_size` | Size |

## `mos_storage_status`

```c
long mos_storage_status(int* install_payload, char boot_device[16]);
```

`install_payload` is 1 when the kernel can run `install` (GRUB loaded the
install payload, as the ISO and installed systems do). `boot_device` is the
device mounted as `0:`.

## Running kernel commands as root

```c
long mos_root_auth(const char* username, const char* password);
long mos_root_run(const char* command);
long mos_root_read(char* out, size_t out_size);
```

`mos_root_auth` checks the password of the MOSdesk account and gives the
process a ticket for a few minutes (`SYS_ERR_PERM` when wrong).
`mos_root_run` starts a kernel terminal command such as
`install sata1 --yes`; one command runs at a time. `mos_root_read`
appends the command's output to `out` and returns 1 while it still runs,
0 once it has finished. The MOSdesk Installer is built this way.
