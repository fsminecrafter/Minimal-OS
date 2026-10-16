# Files

MinimaFS file and directory calls. Include `minimalos.h`.

Paths look like `0:/etc/System.conf`: a drive number, a colon, then an
absolute path. A path without a drive prefix is resolved against the
terminal's current directory (see [`mos_getcwd`](process.md#mos_getcwd)).

A file handle is a `long`. Keep it in a `long`: it is a 64-bit value, and
truncating it to `int` can corrupt it. Valid handles are positive;
errors are negative.

## `mos_open`

```c
long mos_open(const char* path, long flags);
```

Opens an existing file.

| Parameter | Description |
| --- | --- |
| `path` | File to open |
| `flags` | `SYS_O_RDONLY` (0) for read-only; `SYS_O_RDWR` (or any non-zero value) for read and write |

**Returns** a handle (positive), `SYS_ERR_NOTFOUND` if the file does not
exist or cannot be opened, `SYS_ERR_INVAL` for a NULL or invalid path.

**Notes**

- `mos_open` never creates a file. Call [`mos_create`](#mos_create) first.
- The position starts at 0. Writing at position 0 overwrites the start of
  the file; to append, [`mos_seek`](#mos_seek) to [`mos_size`](#mos_size)
  first.
- Writes are kept in memory and saved to disk by [`mos_close`](#mos_close).
  Always close a handle you wrote to.

**Example**

```c
long fd = mos_open("0:/etc/motd.txt", SYS_O_RDONLY);
if (fd < 0) { printf("no motd (%ld)\n", fd); mos_exit(1); }
char buf[256];
long n = mos_read(fd, buf, sizeof buf - 1);
if (n > 0) { buf[n] = '\0'; mos_puts(buf); }
mos_close(fd);
```

## `mos_create`

```c
long mos_create(const char* path, const char* filetype, const char* fileformat);
```

Creates a new, empty file. Open it afterwards with `SYS_O_RDWR` to write
to it.

| Parameter | Description |
| --- | --- |
| `path` | File to create. The parent directory must exist |
| `filetype` | MinimaFS file type such as `"text"` or `"binary"`. NULL means `"binary"` |
| `fileformat` | Format tag such as `"txt"` or `"bin"`. NULL means `"bin"` |

**Returns** `SYS_SUCCESS`, `SYS_ERR_INVAL` for a NULL or invalid path,
`SYS_ERR_GENERIC` if the file system refused (missing parent, no space).

**Example**

```c
const char* path = "0:/data/notes.txt";
if (!mos_exists(path)) mos_create(path, "text", "txt");
long fd = mos_open(path, SYS_O_RDWR);
mos_seek(fd, mos_size(fd));          /* append */
mos_fwrite(fd, "new line\n", 9);
mos_close(fd);
```

## `mos_read`

```c
long mos_read(long fd, void* buf, size_t len);
```

Reads up to `len` bytes from the current position and advances it.

| Parameter | Description |
| --- | --- |
| `fd` | Handle from `mos_open` |
| `buf` | Destination buffer |
| `len` | Maximum number of bytes |

**Returns** the number of bytes read; `0` at end of file. `SYS_ERR_INVAL`
for a NULL handle or buffer.

**Notes** The result is not NUL-terminated. Leave a byte free when reading
text.

**Example**

```c
/* read a whole file into the heap */
long fd = mos_open(path, SYS_O_RDONLY);
long size = mos_size(fd);
char* data = malloc((size_t)size + 1);
long got = 0, n;
while ((n = mos_read(fd, data + got, (size_t)(size - got))) > 0) got += n;
data[got] = '\0';
mos_close(fd);
```

## `mos_fwrite`

```c
long mos_fwrite(long fd, const void* buf, size_t len);
```

Writes `len` bytes at the current position and advances it, growing the
file if needed.

| Parameter | Description |
| --- | --- |
| `fd` | Handle opened with `SYS_O_RDWR` |
| `buf` | Bytes to write |
| `len` | Number of bytes |

**Returns** the number of bytes written, `0` if the handle is read-only or
memory ran out, `SYS_ERR_INVAL` for a NULL handle or buffer.

**Notes**

- Writing never shortens a file. To replace a file's contents, delete it,
  create it again and write.
- The data reaches disk when the handle is closed.

## `mos_close`

```c
long mos_close(long fd);
```

Closes a handle and saves anything written to it.

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` for a NULL handle.

**Notes** Do not use the handle afterwards. Closing twice is undefined.

## `mos_seek`

```c
long mos_seek(long fd, long offset);
```

Moves the position to `offset` bytes from the start of the file.

| Parameter | Description |
| --- | --- |
| `fd` | Open handle |
| `offset` | New position. Values past the end are clamped to the file size |

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` for a NULL handle.

## `mos_tell`

```c
long mos_tell(long fd);
```

**Returns** the current position in bytes, or `SYS_ERR_INVAL` for a NULL
handle.

## `mos_size`

```c
long mos_size(long fd);
```

**Returns** the size of the open file's data in bytes, including anything
written but not yet closed. `SYS_ERR_INVAL` for a NULL handle.

## `mos_feof`

```c
long mos_feof(long fd);
```

**Returns** `1` if the position is at the end of the file, `0` if not,
`SYS_ERR_INVAL` for a NULL handle.

**Example**

```c
char c;
while (!mos_feof(fd) && mos_read(fd, &c, 1) == 1) count++;
```

## `mos_exists`

```c
long mos_exists(const char* path);
```

**Returns** `1` if a file or directory exists at `path`, `0` if not,
`SYS_ERR_INVAL` for an invalid path.

## `mos_is_dir`

```c
long mos_is_dir(const char* path);
```

**Returns** `1` if `path` is a directory, `0` if it is a file or does not
exist, `SYS_ERR_INVAL` for an invalid path.

## `mos_mkdir`

```c
long mos_mkdir(const char* path);
```

Creates one directory. The parent must already exist.

**Returns** `SYS_SUCCESS`, `SYS_ERR_GENERIC` if it could not be created
(it already exists, or the parent is missing), `SYS_ERR_INVAL` for an
invalid path.

**Example**

```c
if (!mos_is_dir("0:/data")) mos_mkdir("0:/data");
```

## `mos_listdir`

```c
long mos_listdir(const char* path, syscall_dirent_t* entries, size_t max_entries);
```

Lists a directory into a caller buffer.

| Parameter | Description |
| --- | --- |
| `path` | Directory to list |
| `entries` | Array to fill |
| `max_entries` | Capacity of `entries`. The kernel also caps it at the most entries one directory can hold |

**Returns** the number of entries written, or `SYS_ERR_INVAL` for a NULL
path or buffer, an invalid path, or `max_entries` of 0.

Each `syscall_dirent_t` holds:

| Field | Meaning |
| --- | --- |
| `name[64]` | Entry name, NUL-terminated |
| `type` | `SYSCALL_DIRENT_TYPE_FILE`, `_DIR`, `_EXECUTABLE` or `_SYMLINK` |
| `hidden` | `1` for hidden entries |

**Example**

```c
syscall_dirent_t items[64];
long n = mos_listdir("0:/programs", items, 64);
for (long i = 0; i < n; i++)
    printf("%s%s\n", items[i].name, items[i].type == SYSCALL_DIRENT_TYPE_DIR ? "/" : "");
```

## `mos_delete`

```c
long mos_delete(const char* path);
```

Deletes a file.

**Returns** `SYS_SUCCESS`, `SYS_ERR_GENERIC` if it does not exist or could
not be deleted, `SYS_ERR_INVAL` for an invalid path.

**Notes** Use [`mos_rmdir`](#mos_rmdir) for directories.

## `mos_rmdir`

```c
long mos_rmdir(const char* path);
```

Removes an empty directory.

**Returns** `SYS_SUCCESS`, `SYS_ERR_GENERIC` if it is not empty, does not
exist or is the drive root, `SYS_ERR_INVAL` for an invalid path.

## `mos_get_metadata`

```c
long mos_get_metadata(const char* path, syscall_file_metadata_t* out);
```

Reads a file's MinimaFS metadata without opening it.

| Parameter | Description |
| --- | --- |
| `path` | File to inspect |
| `out` | Structure to fill |

**Returns** `SYS_SUCCESS`, `SYS_ERR_NOTFOUND` if there is no such file,
`SYS_ERR_INVAL` for a NULL argument.

`syscall_file_metadata_t` fields:

| Field | Meaning |
| --- | --- |
| `filetype[64]` | File type, such as `text` or `binary` |
| `fileformat[16]` | Format tag, such as `txt` or `run` |
| `data_length` | Size of the data in bytes |
| `runnable` | `1` for executables |
| `hidden` | `1` for hidden files |
| `entrypoint` | Entry point recorded for executables |
| `created_date[32]` | Creation date as text |
| `last_changed[32]` | Last-modified date as text |

**Example**

```c
syscall_file_metadata_t m;
if (mos_get_metadata("0:/programs/hello.run", &m) == 0)
    printf("%s/%s, %u bytes, changed %s\n", m.filetype, m.fileformat, m.data_length, m.last_changed);
```
