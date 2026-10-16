# Input

Raw keyboard and mouse input for programs that own the screen. Window
apps do not need any of this: mosWM delivers key and mouse events to
their window (see [`mos_window_next_event`](windows.md#mos_window_next_event)).

`mos_input_read` is in `x86_64/user_syscalls.h`; the mouse calls are in
`x86_64/user_mouse.h` and the USB keyboard calls in `x86_64/user_usb.h`.

## `mos_input_read`

```c
uint64_t mos_input_read(syscall_input_event_t* events, uint32_t capacity,
                        uint32_t timeout_ms, uint64_t* dropped_count);
```

Takes queued keyboard and mouse events off the kernel's input queue. Only
the [screen owner](graphics.md#mos_graphics_acquire) may call it; while
nobody owns the screen, hardware input goes to the terminal instead.

| Parameter | Description |
| --- | --- |
| `events` | Array to fill |
| `capacity` | Size of `events`, 1 … `SYSCALL_INPUT_MAX_READ` (256) |
| `timeout_ms` | How long to wait when the queue is empty, at most 5000. `0` returns at once |
| `dropped_count` | If not NULL, receives the total number of events dropped because the queue was full |

**Returns** the number of events written (0 on timeout), `SYS_ERR_PERM` if
the caller does not own the screen, `SYS_ERR_INVAL` for a bad argument.

`syscall_input_event_t` fields:

| Field | Meaning |
| --- | --- |
| `type` | `SYSCALL_INPUT_KEY_DOWN`, `_KEY_UP`, `_MOUSE_MOVE`, `_MOUSE_BUTTON` or `_MOUSE_WHEEL` |
| `key` | USB HID usage code (key events) |
| `mods` | `SYSCALL_INPUT_MOD_SHIFT`, `_CTRL`, `_ALT` bits |
| `buttons` | `SYSCALL_INPUT_BTN_LEFT`, `_RIGHT`, `_MIDDLE` bits: the current button state |
| `dx`, `dy` | Relative motion (`MOUSE_MOVE`) |
| `wheel` | Relative scroll (`MOUSE_WHEEL`) |
| `time_ms` | Uptime when the event was queued |

**Notes** The queue holds `SYSCALL_INPUT_QUEUE_CAPACITY` (256) events. When
it is full the newest event is dropped and the drop counter increases.

**Example**

```c
syscall_input_event_t ev[32];
uint64_t n = mos_input_read(ev, 32, 16, NULL);   /* wait up to one frame */
if (n > 32) n = 0;                               /* an SYS_ERR_* value */
for (uint64_t i = 0; i < n; i++) {
    if (ev[i].type == SYSCALL_INPUT_KEY_DOWN && ev[i].key == 41) quit = 1;   /* Esc */
    if (ev[i].type == SYSCALL_INPUT_MOUSE_MOVE) { mx += ev[i].dx; my += ev[i].dy; }
}
```

## `mos_mouse_get_state`

```c
uint64_t mos_mouse_get_state(syscall_mouse_state_t* out_state);
```

Reads the USB mouse's accumulated motion since the last call, and resets
it. Screen owner only.

| `syscall_mouse_state_t` field | Meaning |
| --- | --- |
| `dx`, `dy` | Motion since the last call |
| `wheel` | Scroll since the last call |
| `buttons` | Current `SYSCALL_MOUSE_BTN_LEFT`, `_RIGHT`, `_MIDDLE` bits |

**Returns** `SYS_SUCCESS`, `SYS_ERR_PERM` if the caller does not own the
screen, `SYS_ERR_INVAL` for a NULL pointer.

## `mos_mouse_poll`

```c
uint64_t mos_mouse_poll(void);
```

Polls the USB controllers once so new mouse reports are picked up.

**Returns** `SYS_SUCCESS`.

## `mos_mouse_has_mouse`

```c
uint64_t mos_mouse_has_mouse(void);
```

**Returns** `1` if a USB mouse is present, `0` if not.

## `mos_mouse_init`

```c
uint64_t mos_mouse_init(void);
```

Initialises the USB mouse driver. Kernel-privileged: a `.run` program gets
`SYS_ERR_PERM`.

## `mos_mouse`

```c
uint64_t mos_mouse(uint64_t operation, uint64_t arg1, uint64_t arg2);
```

Issues `SYS_MOUSE` with a `SYS_MOUSE_*` operation. The functions above
wrap it. `mos_mouse_call` in `minimalos.h` is the same call with `long`
results.

## `mos_mouse_call`

```c
long mos_mouse_call(uint64_t operation, uint64_t arg1, uint64_t arg2);
```

See [`mos_mouse`](#mos_mouse).

## `mos_usb_has_keyboard`

```c
uint64_t mos_usb_has_keyboard(void);
```

**Returns** `1` if a USB keyboard is attached, `0` if not.

## `mos_usb_key_to_ascii`

```c
char mos_usb_key_to_ascii(uint8_t scancode, uint8_t modifiers);
```

Translates a USB HID usage code to a character using the active keyboard
layout.

| Parameter | Description |
| --- | --- |
| `scancode` | HID usage code, as in `syscall_input_event_t.key` or a window key event |
| `modifiers` | HID modifier byte: `0x02`/`0x20` left/right Shift, `0x01`/`0x10` left/right Ctrl, `0x40` AltGr. Caps Lock is applied by the kernel |

**Returns** the character, or `0` if the key has none (arrows, function
keys).

**Example**

```c
char c = mos_usb_key_to_ascii((uint8_t)ev.key, (ev.mods & SYSCALL_INPUT_MOD_SHIFT) ? 0x02 : 0);
```

## `mos_usb_key_is_pressed`

```c
uint64_t mos_usb_key_is_pressed(uint8_t scancode);
```

**Returns** `1` while the key is held down, `0` if not. Screen owner only;
other processes get `SYS_ERR_PERM`.

## `mos_usb_keyboard_info`

```c
uint64_t mos_usb_keyboard_info(syscall_usb_keyboard_info_t* info);
```

Copies details of the attached USB keyboard.

| `syscall_usb_keyboard_info_t` field | Meaning |
| --- | --- |
| `address`, `port` | USB address and root-hub port |
| `state` | Device state in the USB stack |
| `vendor_id`, `product_id` | USB IDs |
| `class_code` | USB class |
| `is_keyboard` | `1` |

**Returns** `SYS_SUCCESS`, `SYS_ERR_NOTFOUND` if there is no keyboard,
`SYS_ERR_INVAL` for a NULL pointer.

## `mos_usb_poll`

```c
uint64_t mos_usb_poll(void);
```

Polls the USB controllers once.

**Returns** `SYS_SUCCESS`.

## `mos_usb_init`

```c
uint64_t mos_usb_init(void);
```

Re-initialises and re-enumerates the USB controllers. Kernel-privileged:
a `.run` program gets `SYS_ERR_PERM`.

## `mos_usb`

```c
uint64_t mos_usb(uint64_t operation, uint64_t arg1, uint64_t arg2);
```

Issues `SYS_USB` with a `SYS_USB_*` operation. The functions above wrap
it.
