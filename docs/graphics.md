# Graphics

Full-screen framebuffer access over `SYS_GRAPHICS`, for programs that
take over the whole display instead of opening a mosWM window. The
ownership, info, blit and present calls are in `minimalos.h` (from
`x86_64/user_common.h`); the size queries and drawing primitives are in
`x86_64/user_graphics.h`. Include `graphics.h` to get all of them plus the
short `graphics_*` names.

Most apps should use [windows](windows.md) instead. Under MOSdesk, mosWM
already owns the screen and a second program cannot acquire it.

## How drawing works

The kernel keeps a back buffer the size of the screen. A program:

1. takes ownership of the screen with
   [`mos_graphics_acquire`](#mos_graphics_acquire);
2. draws into its own ARGB8888 surface (a `malloc`ed `uint32_t` array);
3. copies the surface into the back buffer with
   [`mos_graphics_blit_surface`](#mos_graphics_blit_surface);
4. shows the changed rectangle with
   [`mos_graphics_present`](#mos_graphics_present);
5. gives the screen back with
   [`mos_graphics_release`](#mos_graphics_release), which redraws the
   terminal.

```c
#include "graphics.h"
#include "stdlib.h"

int main(void) {
    if (mos_graphics_acquire() != SYS_SUCCESS) { mos_puts("screen busy\n"); return 1; }
    uint32_t w = mos_graphics_get_width(), h = mos_graphics_get_height();
    uint32_t* surf = malloc((size_t)w * h * 4);
    for (uint32_t f = 0; f < 300; f++) {
        for (uint32_t i = 0; i < w * h; i++) surf[i] = 0xFF000000 | ((i + f * 4) & 0xFF);
        mos_graphics_blit_surface(surf, w, h, w * 4, 0, 0, (int32_t)w, (int32_t)h, 0, 0,
                                  SYS_GRAPHICS_BLIT_OPAQUE);
        mos_graphics_present(0, 0, (int32_t)w, (int32_t)h);
        mos_sleep(16);
    }
    mos_graphics_release();
    return 0;
}
```

## Privilege

On Minimal-OS a `.run` program is a user process, and the kernel only
lets user processes call these graphics operations: get width, get
height, owner, acquire, release, info, blit and present. The drawing
primitives - clear, pixel, line, rectangle, circle, triangle, ellipse,
text, measure text and set resolution - return `SYS_ERR_PERM` to a user
process; they exist for kernel-privileged callers. Draw shapes and text
into your own surface and blit it.

The Run Studio preview runs the primitives for any program, so code that
uses them works there but not on the real system.

## `mos_graphics_acquire`

```c
long mos_graphics_acquire(void);
```

Makes the calling process the owner of the screen. Only the owner may
blit, present, draw or read raw input.

**Returns** `SYS_SUCCESS`, or `SYS_ERR_PERM` if another process owns the
screen.

**Notes** While a program owns the screen, keyboard and mouse input goes
to the raw input queue ([`mos_input_read`](input.md#mos_input_read))
instead of the terminal.

## `mos_graphics_release`

```c
long mos_graphics_release(void);
```

Gives up ownership and redraws the terminal.

**Returns** `SYS_SUCCESS`, or `SYS_ERR_PERM` if the caller does not own
the screen.

## `mos_graphics_owner`

```c
uint64_t mos_graphics_owner(void);
```

**Returns** the PID that owns the screen, or `SYS_ERR_NOTFOUND` if nobody
does.

**Example**

```c
if (mos_graphics_owner() != SYS_ERR_NOTFOUND) mos_puts("a desktop is running; open a window instead\n");
```

## `mos_graphics_info`

```c
long mos_graphics_info(syscall_graphics_info_t* info);
```

Describes the display. The wrapper sets `info->size`, `info->version` and
`info->reserved` before the call.

| `syscall_graphics_info_t` field | Meaning |
| --- | --- |
| `width`, `height` | Screen size in pixels |
| `pitch` | Framebuffer bytes per row |
| `pixel_format` | Back-buffer format; always `SYS_GRAPHICS_PIXEL_FORMAT_ARGB8888` |
| `bytes_per_pixel` | Framebuffer bytes per pixel |
| `red_position`, `red_mask_size`, and green/blue | Framebuffer channel layout |

**Returns** `SYS_SUCCESS`, `SYS_ERR_INVAL` for a NULL or malformed
structure, `SYS_ERR_GENERIC` if there is no framebuffer.

**Notes** Surfaces you blit are always ARGB8888; the channel layout is
only informative.

## `mos_graphics_blit_surface`

```c
long mos_graphics_blit_surface(const uint32_t* src, uint32_t width, uint32_t height,
                               uint32_t pitch, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                               int32_t dx, int32_t dy, uint32_t flags);
```

Copies a rectangle of an ARGB8888 surface into the kernel back buffer.
Nothing appears until [`mos_graphics_present`](#mos_graphics_present).

| Parameter | Description |
| --- | --- |
| `src` | Surface pixels, 4-byte aligned. The whole surface must be one live heap allocation of this process |
| `width`, `height` | Surface size, 1 … 16384 each |
| `pitch` | Surface bytes per row: a multiple of 4, at least `width × 4`. `pitch × height` at most 64 MiB |
| `sx`, `sy`, `sw`, `sh` | Source rectangle; must lie inside the surface and have positive size |
| `dx`, `dy` | Where its top-left lands on screen; parts off screen are clipped |
| `flags` | `SYS_GRAPHICS_BLIT_OPAQUE` copies pixels as they are; `SYS_GRAPHICS_BLIT_ALPHA` blends them using their alpha byte |

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` if the caller does not own the
screen, `SYS_ERR_INVAL` for any argument outside the rules above,
`SYS_ERR_GENERIC` if the blit failed.

**Example**

```c
/* draw a 32x32 sprite with transparency at (x, y) */
mos_graphics_blit_surface(sprite, 32, 32, 32 * 4, 0, 0, 32, 32, x, y, SYS_GRAPHICS_BLIT_ALPHA);
mos_graphics_present(x, y, 32, 32);
```

## `mos_graphics_present`

```c
long mos_graphics_present(int32_t x, int32_t y, int32_t width, int32_t height);
```

Copies a rectangle of the back buffer to the visible framebuffer. The
rectangle is clipped to the screen.

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` if the caller does not own the
screen, `SYS_ERR_INVAL` for an empty rectangle or no framebuffer.

**Notes** Present only what changed: a small dirty rectangle is much
cheaper than the whole screen.

## `mos_graphics_get_width`

```c
uint32_t mos_graphics_get_width(void);
```

**Returns** the screen width in pixels. Allowed for any process.

## `mos_graphics_get_height`

```c
uint32_t mos_graphics_get_height(void);
```

**Returns** the screen height in pixels. Allowed for any process.

## `mos_graphics_clear`

```c
void mos_graphics_clear(uint8_t r, uint8_t g, uint8_t b);
```

Fills the screen with one colour. Kernel-privileged; see
[Privilege](#privilege).

## `mos_graphics_pixel`

```c
void mos_graphics_pixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b);
```

Sets one pixel. Kernel-privileged.

## `mos_graphics_line`

```c
void mos_graphics_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       uint8_t r, uint8_t g, uint8_t b);
```

Draws a one-pixel line from `(x1, y1)` to `(x2, y2)`. Kernel-privileged.

## `mos_graphics_rectangle`

```c
void mos_graphics_rectangle(int32_t x, int32_t y, uint32_t width, uint32_t height,
                            uint8_t r, uint8_t g, uint8_t b);
```

Draws a rectangle outline with its top-left corner at `(x, y)`.
Kernel-privileged.

## `mos_graphics_fill_rectangle`

```c
void mos_graphics_fill_rectangle(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                 uint8_t r, uint8_t g, uint8_t b);
```

Fills a rectangle. Kernel-privileged.

## `mos_graphics_triangle`

```c
void mos_graphics_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                           int32_t x3, int32_t y3, uint8_t r, uint8_t g, uint8_t b);
```

Draws the outline of the triangle through three points. Kernel-privileged.

## `mos_graphics_fill_triangle`

```c
void mos_graphics_fill_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                int32_t x3, int32_t y3, uint8_t r, uint8_t g, uint8_t b);
```

Fills a triangle. Kernel-privileged.

## `mos_graphics_circle`

```c
void mos_graphics_circle(int32_t x, int32_t y, uint32_t radius,
                         uint8_t r, uint8_t g, uint8_t b);
```

Draws a circle outline centred on `(x, y)`. Kernel-privileged.

## `mos_graphics_fill_circle`

```c
void mos_graphics_fill_circle(int32_t x, int32_t y, uint32_t radius,
                              uint8_t r, uint8_t g, uint8_t b);
```

Fills a circle. Kernel-privileged.

## `mos_graphics_ellipse`

```c
void mos_graphics_ellipse(int32_t x, int32_t y, uint32_t radius_x, uint32_t radius_y,
                          uint8_t r, uint8_t g, uint8_t b);
```

Draws an ellipse outline centred on `(x, y)` with horizontal radius
`radius_x` and vertical radius `radius_y`. Kernel-privileged.

## `mos_graphics_text`

```c
void mos_graphics_text(const char* text, int32_t x, int32_t y,
                       uint8_t r, uint8_t g, uint8_t b);
```

Draws `text` in the kernel's terminal font with its top-left at
`(x, y)`. Kernel-privileged.

## `mos_graphics_measure_text`

```c
void mos_graphics_measure_text(const char* text, uint32_t* out_width, uint32_t* out_height);
```

Reports the size `mos_graphics_text` would draw `text` at. Both output
pointers are required. Kernel-privileged.

## `mos_graphics_set_resolution`

```c
void mos_graphics_set_resolution(uint32_t columns, uint32_t rows);
```

Changes the terminal's text grid to `columns × rows`. Kernel-privileged.

## `mos_graphics_call`

```c
long mos_graphics_call(syscall_graphics_request_t* request);
```

Issues `SYS_GRAPHICS` with a filled `syscall_graphics_request_t`. The
request has an `op` (a `SYS_GRAPHICS_*` value), an `args[10]` array of
integers, and pointers for text, output sizes, blit and info. Every
wrapper on this page builds one and calls this (or `mos_graphics`, its
`uint64_t` twin in `x86_64/user_syscalls.h`).

## `mos_graphics`

```c
uint64_t mos_graphics(syscall_graphics_request_t* request);
```

Same as [`mos_graphics_call`](#mos_graphics_call), returning `uint64_t`.

## Short names

`graphics.h` defines these aliases:

| Short name | Function |
| --- | --- |
| `graphics_get_width`, `graphics_get_height` | `mos_graphics_get_width`, `mos_graphics_get_height` |
| `graphics_acquire`, `graphics_release` | `mos_graphics_acquire`, `mos_graphics_release` |
| `graphics_info` | `mos_graphics_info` |
| `graphics_blit_surface`, `graphics_present` | `mos_graphics_blit_surface`, `mos_graphics_present` |
| `graphics_clear`, `graphics_pixel`, `graphics_line` | `mos_graphics_clear`, `mos_graphics_pixel`, `mos_graphics_line` |
| `graphics_rectangle`, `graphics_fill_rectangle` | `mos_graphics_rectangle`, `mos_graphics_fill_rectangle` |
| `graphics_triangle`, `graphics_fill_triangle` | `mos_graphics_triangle`, `mos_graphics_fill_triangle` |
| `graphics_circle`, `graphics_fill_circle`, `graphics_ellipse` | `mos_graphics_circle`, `mos_graphics_fill_circle`, `mos_graphics_ellipse` |
| `graphics_text`, `graphics_measure_text` | `mos_graphics_text`, `mos_graphics_measure_text` |
| `graphics_set_resolution` | `mos_graphics_set_resolution` |
