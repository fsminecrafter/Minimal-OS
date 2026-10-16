# Formatted output

`printf` from `stdio.h` and `std::cout` from `iostream`. Both are
header-only and write to stdout through [`mos_write`](process.md#mos_write).

## `printf`

```c
long printf(const char* format, ...);
```

Formats its arguments and writes the result to stdout.

| Parameter | Description |
| --- | --- |
| `format` | Format string |
| `...` | One argument per conversion |

Supported conversions:

| Conversion | Argument | Output |
| --- | --- | --- |
| `%d`, `%i` | `int` | Signed decimal |
| `%u` | `unsigned int` | Unsigned decimal |
| `%x`, `%X` | `unsigned int` | Hexadecimal, lower or upper case |
| `%ld` | `long` | Signed decimal |
| `%lu` | `unsigned long` | Unsigned decimal |
| `%c` | `int` | One character |
| `%s` | `const char*` | String; NULL prints `(null)` |
| `%%` | none | A `%` |

**Returns** the number of bytes written.

**Notes**

- There are no flags, widths or precisions (`%5d`, `%-10s`, `%02x`), no
  floating point and no `%p` or `%lx`. An unsupported conversion is
  printed as is.
- To print a 64-bit value in hex, split it:
  `printf("%x%x", (unsigned)(v >> 32), (unsigned)v)` (pad the low half
  yourself), or format it into a buffer by hand.
- There is no `sprintf`. Build strings by hand or with `string.h`.
- Output is buffered in 128-byte chunks inside one call, so a single
  `printf` is one or a few `mos_write` calls.

**Example**

```c
printf("%s: %d of %u done (%ld ms)\n", name, done, total, mos_uptime() - start);
```

## `mos_vprintf`

```c
long mos_vprintf(const char* format, va_list args);
```

The `va_list` form of [`printf`](#printf), for writing your own
logging functions.

**Example**

```c
static void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mos_puts("[app] ");
    mos_vprintf(fmt, ap);
    va_end(ap);
}
```

## `std::cout`

```cpp
namespace std { class ostream; static ostream cout; }
ostream& operator<<(const char*);
ostream& operator<<(char);
ostream& operator<<(int);
ostream& operator<<(unsigned int);
ostream& operator<<(long);
ostream& operator<<(unsigned long);
ostream& operator<<(bool);
```

A minimal `std::cout` for C++ programs. Include `<iostream>`.

**Notes**

- `bool` prints `true` or `false`; a NULL `const char*` prints `(null)`.
- There is no `std::endl`, manipulators, `std::string` or `std::cin`.
  Write `"\n"` for a newline.

**Example**

```cpp
#include <iostream>

int main() {
    std::cout << "2 + 2 = " << 2 + 2 << "\n";
    return 0;
}
```
