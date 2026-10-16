# Windows

mosWM windows over `SYS_WINDOW`. Include `window.h`.

A window is a kernel-owned ARGB8888 frame buffer plus a title, bounds,
flags, a style and an event queue. Your program draws into its own pixel
buffer and copies it into the window with
[`mos_window_present`](#mos_window_present); mosWM, the window manager,
composites every window onto the screen and turns mouse and keyboard
input into window events.

Windows belong to the process that created them (and its descendants).
They are closed automatically when that process exits.

## A minimal window app

```c
#include "window.h"
#include "stdlib.h"

static int running = 1;
static void on_close(uint32_t id) { (void)id; running = 0; }

int main(void) {
    const uint32_t W = 320, H = 200;
    uint32_t id = mos_window_create(0, 120, 80, W, H, "Hello");
    if (!id) mos_exit(1);
    uint32_t* px = malloc(W * H * 4);

    mos_window_callbacks_t cb = { .onwindowclose = on_close };
    while (running) {
        for (uint32_t i = 0; i < W * H; i++) px[i] = 0xFF2845F5;   /* opaque blue */
        mos_window_present(id, px, W * 4, 0, 0, W, H);
        mos_window_dispatch_events(id, &cb);
        mos_sleep(16);
    }
    mos_window_close(id);
    mos_exit(0);
}
```

## Limits

| Limit | Value |
| --- | --- |
| Windows in the system | `SYSCALL_WINDOW_MAX_WINDOWS` (64) |
| Minimum size | 64 × 48 |
| Maximum size | 16384 × 16384, and at most 64 MiB of pixels |
| All windows' pixels together | 64 MiB |
| Title | `SYSCALL_WINDOW_MAX_TITLE` (64) bytes including the NUL |
| Queued events per window | `SYSCALL_WINDOW_MAX_EVENTS` (16) |
| Position | 0 … 32768 on each axis |

## Flags

| Flag | Meaning |
| --- | --- |
| `SYSCALL_WINDOW_VISIBLE` | Drawn by mosWM |
| `SYSCALL_WINDOW_RESIZABLE` | The user can resize it, and the program may call `mos_window_set_bounds` |
| `SYSCALL_WINDOW_FULLSCREEN` | Covers the screen; cannot be combined with `MAXIMIZED` |
| `SYSCALL_WINDOW_MAXIMIZED` | Fills the desktop area |
| `SYSCALL_WINDOW_MINIMIZED` | Hidden in the taskbar |
| `SYSCALL_WINDOW_FOCUSED` | Has keyboard focus. Read-only for applications: only mosWM sets it |

## `mos_window_create`

```c
uint32_t mos_window_create(uint32_t parent_id, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, const char* title);
```

Creates a visible, resizable window. Its frame starts out as whatever the
allocator returned, so present a first frame straight away.

| Parameter | Description |
| --- | --- |
| `parent_id` | `0` for a top-level window, or the ID of a window this process controls to create a child (a dialog) |
| `x`, `y` | Position on the desktop |
| `width`, `height` | Content size in pixels, at least 64 × 48 |
| `title` | Title bar text; truncated to 63 bytes. NULL for none |

**Returns** the new window ID (1 … 64), or `0` on failure. The raw call
fails with `SYS_ERR_INVAL` (size out of range, bad flags),
`SYS_ERR_NOTFOUND` / `SYS_ERR_PERM` (bad parent) or `SYS_ERR_GENERIC`
(no free slot, out of frame memory); the wrapper turns all of these into
`0`.

**Notes** To create a window with other flags (not resizable, hidden,
full screen), create it and then call
[`mos_window_set_flags`](#mos_window_set_flags).

## `mos_window_close`

```c
uint64_t mos_window_close(uint32_t id);
```

Destroys a window and frees its frame.

**Returns** `SYS_SUCCESS`, `SYS_ERR_NOTFOUND` for an unknown ID,
`SYS_ERR_PERM` for a window this process does not control.

**Notes** Clicking the close button does not close the window; it queues a
`SYSCALL_WINDOW_EVENT_CLOSE_REQUEST`. The program decides whether to call
`mos_window_close`.

## `mos_window_present`

```c
uint64_t mos_window_present(uint32_t id, const uint32_t* pixels, uint32_t pitch,
                            uint32_t x, uint32_t y, uint32_t width, uint32_t height);
```

Copies a rectangle of your pixel buffer into the window's frame. mosWM
shows the new frame on its next composite.

| Parameter | Description |
| --- | --- |
| `id` | Window |
| `pixels` | Your buffer, the same size as the window, ARGB8888 (`0xAARRGGBB`). Use `0xFF` alpha |
| `pitch` | Bytes per row of `pixels`, at least `window width × 4` |
| `x`, `y`, `width`, `height` | The rectangle to copy, in window coordinates. It must lie inside the window |

**Returns** `SYS_SUCCESS`, `SYS_ERR_INVAL` for a rectangle outside the
window, a short pitch or a NULL buffer, `SYS_ERR_NOTFOUND` /
`SYS_ERR_PERM` for a bad window.

**Notes**

- `pixels` points at the top-left of your whole buffer, not of the
  rectangle; the kernel offsets into it. Presenting only the part that
  changed is cheaper than the whole window.
- After a `SYSCALL_WINDOW_EVENT_RESIZED`, reallocate the buffer at the new
  size before presenting again.
- Each present increments the window's `frame_version`.

**Example**

```c
/* redraw only a 32 x 32 cursor area */
mos_window_present(id, px, W * 4, cx, cy, 32, 32);
```

## `mos_window_next_event`

```c
uint64_t mos_window_next_event(uint32_t id, syscall_window_event_t* event);
```

Takes the oldest event off the window's queue. Never blocks.

| Parameter | Description |
| --- | --- |
| `id` | Window |
| `event` | Filled with the event |

**Returns** `SYS_SUCCESS` with `*event` filled, `SYS_ERR_AGAIN` if the
queue is empty, `SYS_ERR_NOTFOUND` / `SYS_ERR_PERM` for a bad window.

`syscall_window_event_t` fields:

| Field | Meaning |
| --- | --- |
| `id` | Window the event is for |
| `type` | One of the event types below |
| `x`, `y` | Pointer position in window coordinates (mouse events) |
| `width`, `height` | New content size (`RESIZED`) |
| `key` | USB HID usage code (`KEY`) |
| `buttons` | `KEY`: 1 = pressed, 0 = released. `MOUSE`: button mask, bit 0 left, bit 1 right, bit 2 middle |
| `wheel` | Scroll amount (`MOUSE_WHEEL`) |
| `mods` | `SYSCALL_INPUT_MOD_SHIFT`, `_CTRL`, `_ALT` bits |
| `time_ms` | Uptime when the event was queued |

Event types:

| Type | Sent when |
| --- | --- |
| `SYSCALL_WINDOW_EVENT_RESIZED` | The window's size changed |
| `SYSCALL_WINDOW_EVENT_CLOSE_REQUEST` | The user clicked the close button |
| `SYSCALL_WINDOW_EVENT_MINIMIZED` | It was minimised |
| `SYSCALL_WINDOW_EVENT_MAXIMIZED` | It was maximised |
| `SYSCALL_WINDOW_EVENT_RESTORED` | It returned from minimised or maximised |
| `SYSCALL_WINDOW_EVENT_FOCUS_GAINED` | It got keyboard focus |
| `SYSCALL_WINDOW_EVENT_FOCUS_LOST` | It lost keyboard focus |
| `SYSCALL_WINDOW_EVENT_KEY` | A key was pressed or released while it had focus |
| `SYSCALL_WINDOW_EVENT_MOUSE` | The pointer moved over it, or a button changed |
| `SYSCALL_WINDOW_EVENT_MOUSE_WHEEL` | The wheel turned over it |

**Notes**

- The queue holds 16 events. Window-state events of the same type are
  merged (only the newest resize is kept), and consecutive mouse moves with
  the same buttons are merged, so a slow program loses detail but not
  clicks. Drain the queue every frame.
- Common HID codes: `a`-`z` 4-29, `1`-`9` 30-38, `0` 39, Enter 40,
  Escape 41, Backspace 42, Tab 43, Space 44, Right 79, Left 80, Down 81,
  Up 82.

**Example**

```c
syscall_window_event_t ev;
while (mos_window_next_event(id, &ev) == SYS_SUCCESS) {
    if (ev.type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) running = 0;
    else if (ev.type == SYSCALL_WINDOW_EVENT_KEY && ev.buttons && ev.key == 41) running = 0;  /* Esc */
    else if (ev.type == SYSCALL_WINDOW_EVENT_MOUSE && (ev.buttons & 1)) paint(ev.x, ev.y);
    else if (ev.type == SYSCALL_WINDOW_EVENT_RESIZED) resize_buffer(ev.width, ev.height);
}
```

## `mos_window_dispatch_events`

```c
uint64_t mos_window_dispatch_events(uint32_t id, const mos_window_callbacks_t* callbacks);
```

Drains the window's queue and calls a callback for each window-state
event.

| Parameter | Description |
| --- | --- |
| `id` | Window |
| `callbacks` | Structure of function pointers; any may be NULL |

`mos_window_callbacks_t` members:

| Member | Called for |
| --- | --- |
| `onwindowclose(id)` | `CLOSE_REQUEST` |
| `onwindowminimize(id)` | `MINIMIZED` |
| `onwindowmaximise(id)` | `MAXIMIZED` |
| `windowsizechange_recall(id, width, height)` | `RESIZED` |
| `onwindowfocus(id, focused)` | `FOCUS_GAINED` (1) and `FOCUS_LOST` (0) |

**Returns** the number of events taken off the queue, `SYS_ERR_INVAL` if
`callbacks` is NULL, or an error from `mos_window_next_event`.

**Notes** Key, mouse, wheel and `RESTORED` events are consumed and
dropped. A program that needs input should loop over
[`mos_window_next_event`](#mos_window_next_event) itself.

## `mos_window_set_bounds`

```c
uint64_t mos_window_set_bounds(uint32_t id, uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height);
```

Moves and/or resizes a window. Resizing keeps the overlapping part of the
old frame and queues a `RESIZED` event.

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` if the window is not resizable,
`SYS_ERR_INVAL` for a size or position out of range, `SYS_ERR_BUSY` if
its event queue is full, `SYS_ERR_GENERIC` if frame memory runs out.

## `mos_window_set_title`

```c
uint64_t mos_window_set_title(uint32_t id, const char* title);
```

Changes the title bar text (truncated to 63 bytes).

**Returns** `SYS_SUCCESS`, or `SYS_ERR_NOTFOUND` / `SYS_ERR_PERM`.

**Example**

```c
mos_window_set_title(id, dirty ? "notes.txt *" : "notes.txt");
```

## `mos_window_set_flags`

```c
uint64_t mos_window_set_flags(uint32_t id, uint32_t flags);
```

Replaces the window's flags with `flags` (see [Flags](#flags)). The
`FOCUSED` bit is kept as it is.

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` if `flags` has unknown bits,
includes `FOCUSED`, or combines `FULLSCREEN` with `MAXIMIZED`.

**Notes** The helpers below read the current flags with
`mos_window_get_info` and change one bit, which is usually what you want.

**Example**

```c
/* a fixed-size window */
mos_window_set_flags(id, SYSCALL_WINDOW_VISIBLE);
```

## `mos_window_set_fullscreen`

```c
uint64_t mos_window_set_fullscreen(uint32_t id, bool enabled);
```

Turns full screen on or off. Clears `MAXIMIZED` and `MINIMIZED`.

**Returns** the result of `mos_window_set_flags`.

## `mos_window_minimize`

```c
uint64_t mos_window_minimize(uint32_t id);
```

Minimises the window. Clears `FULLSCREEN` and `MAXIMIZED`.

## `mos_window_maximize`

```c
uint64_t mos_window_maximize(uint32_t id);
```

Maximises the window. Clears `FULLSCREEN` and `MINIMIZED`.

## `mos_window_restore`

```c
uint64_t mos_window_restore(uint32_t id);
```

Clears `FULLSCREEN`, `MAXIMIZED` and `MINIMIZED`, returning the window to
its normal bounds.

## `mos_window_set_resizable`

```c
uint64_t mos_window_set_resizable(uint32_t id, bool resizable);
```

Sets or clears `SYSCALL_WINDOW_RESIZABLE`.

## `mos_window_set_visible`

```c
uint64_t mos_window_set_visible(uint32_t id, bool visible);
```

Shows or hides the window by setting or clearing
`SYSCALL_WINDOW_VISIBLE`. A hidden window keeps its frame and events.

## `mos_window_set_style`

```c
uint64_t mos_window_set_style(uint32_t id, syscall_window_style_t style);
```

Changes the colours mosWM uses for this window's decoration. All colours
are ARGB8888.

| `syscall_window_style_t` member | Default |
| --- | --- |
| `title_bar` | `0xFF24343C` |
| `title_text` | `0xFFFFFFFF` |
| `close_button`, `close_icon` | `0xFFE35D6A`, `0xFFFFFFFF` |
| `minimize_button`, `minimize_icon` | `0xFFE8B34C`, `0xFF17262C` |
| `maximize_button`, `maximize_icon` | `0xFF4ABF8A`, `0xFF17262C` |
| `border` | `0xFF49616B` |
| `content_background` | `0xFF17232A` |

**Returns** `SYS_SUCCESS`, or `SYS_ERR_NOTFOUND` / `SYS_ERR_PERM`.

**Example**

```c
syscall_window_info_t info;
mos_window_get_info(id, &info);
syscall_window_style_t s = info.style;
s.title_bar = 0xFF2845F5;
mos_window_set_style(id, s);
```

## `mos_window_get_info`

```c
uint64_t mos_window_get_info(uint32_t id, syscall_window_info_t* info);
```

Copies a window's current state.

| `syscall_window_info_t` field | Meaning |
| --- | --- |
| `id`, `parent_id` | The window and its parent (0 for top level) |
| `x`, `y`, `width`, `height` | Content bounds |
| `flags` | Current [flags](#flags) |
| `frame_version` | Incremented by every present |
| `owner_pid` | Process that created it |
| `title[64]` | Title |
| `style` | Decoration colours |

**Returns** `SYS_SUCCESS`, `SYS_ERR_INVAL` for a NULL `info`, or
`SYS_ERR_NOTFOUND` / `SYS_ERR_PERM`.

**Example**

```c
syscall_window_info_t info;
if (mos_window_get_info(id, &info) == 0 && (info.flags & SYSCALL_WINDOW_FOCUSED))
    draw_caret();
```

## `mos_window_list`

```c
uint64_t mos_window_list(syscall_window_info_t* windows, uint32_t capacity);
```

Lists the windows this process controls (all windows, for the window
manager).

| Parameter | Description |
| --- | --- |
| `windows` | Array to fill; may be NULL when `capacity` is 0 |
| `capacity` | Size of the array, at most 64 |

**Returns** the total number of matching windows, which can be larger than
`capacity` (only `capacity` are copied), or `SYS_ERR_INVAL`.

**Example**

```c
uint64_t count = mos_window_list(NULL, 0);   /* just count */
```

## `mos_window_get_frame`

```c
uint64_t mos_window_get_frame(uint32_t id, syscall_window_frame_t* frame);
```

Window-manager only. Copies a window's frame into `frame->pixels`, which
must match the window's size. mosWM uses it to composite.

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` for any process other than the
registered window manager, `SYS_ERR_INVAL` for a mismatched buffer.

## `mos_window_post_event`

```c
uint64_t mos_window_post_event(uint32_t id, syscall_window_event_t* event);
```

Window-manager only. Queues an event on a window; this is how mosWM
delivers input and close requests.

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` for other processes,
`SYS_ERR_INVAL` for an unknown event type, `SYS_ERR_BUSY` if the queue is
full.

## `mos_window_register_manager`

```c
uint64_t mos_window_register_manager(void);
```

Makes the calling process the window manager. Only a process started as a
window manager that also owns the screen
([`mos_graphics_acquire`](graphics.md#mos_graphics_acquire)) may do this.

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` if the caller is not allowed,
`SYS_ERR_BUSY` if another manager is registered.

## `mos_window_call`

```c
uint64_t mos_window_call(syscall_window_request_t* request);
```

Issues `SYS_WINDOW` with a filled request. Every wrapper above builds a
request with [`mos_window_request`](#mos_window_request) and calls this.

## `mos_window_request`

```c
syscall_window_request_t mos_window_request(uint32_t op, uint32_t id);
```

**Returns** a zeroed request with `size`, `version`, `op` and `id` filled
in, ready for `mos_window_call`.
