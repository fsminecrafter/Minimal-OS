# Packages

`.mpkg` archives over `SYS_PKG`. Include `minimalos.h`.

An `.mpkg` holds files and directories, stored or LZSS-compressed. The
terminal's `pkg` command and the `dlr` package client use the same calls.

## `mos_pkg_unzip`

```c
long mos_pkg_unzip(const char* archive_path, const char* target_dir,
                   uint32_t* out_installed, uint32_t* out_failed);
```

Extracts an archive into a directory.

| Parameter | Description |
| --- | --- |
| `archive_path` | The `.mpkg` file |
| `target_dir` | Directory to extract into, for example `"0:/"` |
| `out_installed` | If not NULL, receives the number of entries extracted |
| `out_failed` | If not NULL, receives the number of entries that failed |

**Returns** `SYS_SUCCESS` if the archive was processed, `SYS_ERR_GENERIC`
if it could not be read, `SYS_ERR_INVAL` for a NULL path or target.

**Notes** Success means the archive was read; individual entries can still
fail (invalid names are rejected, for one). Check `out_failed`.

**Example**

```c
uint32_t ok = 0, bad = 0;
if (mos_pkg_unzip("0:/downloads/app.mpkg", "0:/", &ok, &bad) == 0)
    printf("installed %u entries, %u failed\n", ok, bad);
```

## `mos_pkg_zip`

```c
long mos_pkg_zip(const char* source_path, const char* algorithm,
                 char* out_path, uint32_t out_path_size);
```

Archives a file or directory as `<source_path>.mpkg`.

| Parameter | Description |
| --- | --- |
| `source_path` | File or directory to pack |
| `algorithm` | `"lzss"`, `"store"`, or NULL for the default (LZSS) |
| `out_path` | If not NULL, receives the path of the archive written |
| `out_path_size` | Size of `out_path` |

**Returns** `SYS_SUCCESS`, `SYS_ERR_GENERIC` on failure, `SYS_ERR_INVAL`
for a NULL source.

**Example**

```c
char made[128];
if (mos_pkg_zip("0:/projects/game", NULL, made, sizeof made) == 0)
    printf("wrote %s\n", made);
```

## `mos_pkg_info`

```c
long mos_pkg_info(const char* archive_path, uint32_t* out_entry_count);
```

Reads an archive's entry count without extracting it.

**Returns** `SYS_SUCCESS`, `SYS_ERR_GENERIC` if the archive is invalid,
`SYS_ERR_INVAL` for a NULL path.
