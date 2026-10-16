# Memory and randomness

Heap allocation over `SYS_HEAP`, random bytes over `SYS_RANDOM`, and
`atoi`. Include `stdlib.h`.

Heap blocks come from the kernel allocator and are tracked per process:
`free` and `realloc` only accept blocks this process allocated, and any
block still allocated when the process ends is reclaimed.

## `malloc`

```c
void* malloc(size_t size);
```

Allocates `size` bytes. The memory is not cleared.

| Parameter | Description |
| --- | --- |
| `size` | Number of bytes. Must be greater than 0 |

**Returns** a pointer to the block, or `NULL` if `size` is 0 or there is
not enough memory.

**Example**

```c
uint32_t* pixels = malloc(width * height * sizeof(uint32_t));
if (!pixels) { mos_puts("out of memory\n"); mos_exit(1); }
```

## `calloc`

```c
void* calloc(size_t count, size_t size);
```

Allocates an array of `count` elements of `size` bytes, cleared to zero.

**Returns** a pointer, or `NULL` if `count * size` overflows, is 0, or
memory runs out.

**Example**

```c
int* grid = calloc(rows * cols, sizeof(int));
```

## `realloc`

```c
void* realloc(void* ptr, size_t size);
```

Resizes a block, moving it if needed. The contents are kept up to the
smaller of the two sizes.

| Parameter | Description |
| --- | --- |
| `ptr` | Block from `malloc`, `calloc` or `realloc`, or `NULL` to allocate a new one |
| `size` | New size. `0` frees the block |

**Returns** the (possibly moved) block, or `NULL` on failure. On failure
the original block is unchanged and still yours, so keep the old pointer
until the call succeeds. `realloc(ptr, 0)` frees `ptr` and returns `NULL`.

**Example**

```c
char* bigger = realloc(buf, cap * 2);
if (!bigger) { free(buf); mos_exit(1); }
buf = bigger; cap *= 2;
```

## `free`

```c
void free(void* ptr);
```

Releases a block. `free(NULL)` does nothing.

**Notes** Passing a pointer this process did not allocate is rejected by
the kernel (`SYS_ERR_INVAL`, ignored by `free`) rather than corrupting the
heap. Using a block after freeing it is still a bug.

## `mos_heap_failed`

```c
int mos_heap_failed(long value);
```

Tells whether a raw `SYS_HEAP` result is an error code rather than an
address. `malloc`, `calloc` and `realloc` use it to return `NULL`; you
only need it when calling `SYS_HEAP` through `mos_syscall`.

**Returns** non-zero if `value` is one of the `SYS_ERR_*` codes.

## `mos_random_bytes`

```c
long mos_random_bytes(void* buf, size_t len);
```

Fills `buf` with random bytes from the kernel's entropy source.

| Parameter | Description |
| --- | --- |
| `buf` | Destination |
| `len` | Number of bytes. Must be greater than 0 |

**Returns** `len`, or `SYS_ERR_INVAL` for a NULL buffer or zero length.

**Notes** The kernel uses `RDRAND` when the CPU has it, which is suitable
for keys and nonces. Without it, the bytes are derived from the TSC and
RTC and must not be used for cryptography.

**Example**

```c
uint8_t key[32];
mos_random_bytes(key, sizeof key);
```

## `mos_random_u64`

```c
uint64_t mos_random_u64(void);
```

**Returns** 64 random bits from [`mos_random_bytes`](#mos_random_bytes).

**Example**

```c
int die = (int)(mos_random_u64() % 6) + 1;
```

## `mos_random`

```c
long mos_random(void* buf, uint32_t len);
```

The same call as [`mos_random_bytes`](#mos_random_bytes), declared in
`net.h` with a 32-bit length.

## `atoi`

```c
int atoi(const char* s);
```

Converts a decimal string to an `int`. Skips leading spaces and tabs,
accepts one `+` or `-`, then reads digits until the first non-digit.

**Returns** the value, or 0 if there are no digits. Overflow is not
detected.

**Example**

```c
int port = argc > 1 ? atoi(argv[1]) : 8080;
```
