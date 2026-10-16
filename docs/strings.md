# Strings and memory

Freestanding string and memory helpers. Include `string.h`. None of these
make a syscall.

`memcpy`, `memmove`, `memset` and `memcmp` are real functions defined in
`crt0.c`, because the compiler emits calls to them on its own (struct
assignment, `= {0}` initialisers); the rest are `static inline`.

## `strlen`

```c
size_t strlen(const char* s);
```

**Returns** the number of bytes before the terminating NUL.

## `strcmp`

```c
int strcmp(const char* a, const char* b);
```

Compares two strings byte by byte as `unsigned char`.

**Returns** `0` if they are equal, a negative value if `a` sorts first, a
positive value if `b` does.

**Example**

```c
if (argc > 1 && strcmp(argv[1], "--help") == 0) usage();
```

## `strncmp`

```c
int strncmp(const char* a, const char* b, size_t n);
```

Like [`strcmp`](#strcmp) but compares at most `n` bytes.

**Example**

```c
if (strncmp(line, "GET ", 4) == 0) handle_get(line + 4);
```

## `strcpy`

```c
char* strcpy(char* dst, const char* src);
```

Copies `src`, including its NUL, into `dst`.

**Returns** `dst`.

**Notes** Does not check the size of `dst`. Prefer
[`strlcpy_safe`](#strlcpy_safe).

## `strlcpy_safe`

```c
char* strlcpy_safe(char* dst, const char* src, size_t size);
```

Copies at most `size - 1` bytes of `src` and always NUL-terminates,
truncating if `src` does not fit. Unlike the C library's `strncpy`, it
never leaves `dst` unterminated and never pads with zeros.

| Parameter | Description |
| --- | --- |
| `dst` | Destination buffer |
| `src` | NUL-terminated source |
| `size` | Size of `dst` in bytes. `0` copies nothing |

**Returns** `dst`.

**Example**

```c
char name[16];
strlcpy_safe(name, argv[1], sizeof name);
```

## `strchr`

```c
char* strchr(const char* s, int c);
```

**Returns** a pointer to the first `c` in `s`, or `NULL`. Searching for
`'\0'` returns a pointer to the terminator.

## `strrchr`

```c
char* strrchr(const char* s, int c);
```

**Returns** a pointer to the last `c` in `s`, or `NULL`.

**Example**

```c
const char* slash = strrchr(path, '/');
const char* file = slash ? slash + 1 : path;
```

## `strstr`

```c
char* strstr(const char* haystack, const char* needle);
```

**Returns** a pointer to the first occurrence of `needle` in `haystack`,
`haystack` itself if `needle` is empty, or `NULL`.

## `memcpy`

```c
void* memcpy(void* dst, const void* src, size_t n);
```

Copies `n` bytes. The ranges must not overlap; use
[`memmove`](#memmove) if they might.

**Returns** `dst`.

## `memmove`

```c
void* memmove(void* dst, const void* src, size_t n);
```

Copies `n` bytes; the ranges may overlap.

**Returns** `dst`.

**Example**

```c
/* shift an array one slot right */
memmove(&items[1], &items[0], (count - 1) * sizeof items[0]);
```

## `memset`

```c
void* memset(void* dst, int value, size_t n);
```

Sets `n` bytes to `(unsigned char)value`.

**Returns** `dst`.

## `memcmp`

```c
int memcmp(const void* a, const void* b, size_t n);
```

Compares `n` bytes.

**Returns** `0` if equal, otherwise the difference of the first differing
bytes.

**Notes** Stops at the first difference, so its timing reveals where the
inputs differ. For secrets use [`mos_memeq_ct`](#mos_memeq_ct).

## `mos_memcmp`

```c
int mos_memcmp(const void* a, const void* b, size_t n);
```

An inline copy of [`memcmp`](#memcmp) with the same result.

## `mos_memeq_ct`

```c
int mos_memeq_ct(const void* a, const void* b, size_t n);
```

Compares `n` bytes in constant time: it always reads every byte, whatever
the contents.

**Returns** `1` if the ranges are equal, `0` if not. Note the sense is the
opposite of `memcmp`.

**Notes** Use it for authentication tags, MACs and password hashes. An
early-exit comparison leaks how many leading bytes matched, which is
enough to forge a tag one byte at a time.

**Example**

```c
if (!mos_memeq_ct(tag, expected_tag, 16)) reject();
```
