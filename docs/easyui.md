# easyui

Buttons, sliders, text inputs, radio buttons and more for mosWM apps, in
one header. Include `easyui.h`.

easyui opens one window, keeps a list of widgets, draws them, and turns
mouse and keyboard events into callbacks. Text is smooth, anti-aliased
Inter drawn by the same OpenType renderer mosWM uses. Its colours follow
the MOSdesk theme picked in Settings > Appearance, and they change while
the app is running when the user picks another theme.

```c
#include "easyui.h"

static int label1;

static void clicked(int value) {
    easyui.settext(label1, "Thanks!");
}

int main(void) {
    easyui.init("Hello", 320, 160);
    int button1 = easyui.createbutton(20, 20, 200, 40, "Click me!");
    label1 = easyui.createlabel(20, 80, 280, 24, "Nobody clicked yet");
    easyui.recallfunc(button1, ONCLICK, clicked);
    return easyui.run();
}
```

Build it like any program (`./build.sh hello.c`), or open the **Widgets
(easyui)** sample in Run Studio. `examples/easyui-demo.c` uses every
widget.

## How it works

- **One object.** Every function is called through `easyui`:
  `easyui.createbutton(...)`, `easyui.setvalue(...)`. It is a constant
  structure of function pointers, so it works the same from C and C++.
- **Widgets are ints.** Each `create...` call returns a widget number (1 …
  128), or `0` if there was no room. Keep it in an `int` and pass it to the
  other calls. Calls with an invalid widget do nothing.
- **Coordinates** are pixels inside the window's content area: `(0, 0)` is
  the top-left corner below the title bar. Widgets are drawn in the order
  they were created, so create a panel before the widgets on it.
- **Callbacks** take one `int`, the event's value (see [Events](#events)).
  Register them with [`easyui.recallfunc`](#easyui-recallfunc). Inside a
  callback, [`easyui.current`](#easyui-current) says which widget fired, so
  one function can serve several widgets.
- **The main loop** is [`easyui.run`](#easyui-run). It handles events,
  runs timers, redraws only when something changed and sleeps between
  frames. Callbacks can change any widget; the window is repainted
  afterwards.
- **Memory.** Widgets live in a fixed table (`EASYUI_MAX_WIDGETS`, 128).
  Widget text holds up to 127 bytes. List and dropdown items are copied to
  the heap and freed by `clearitems` and `remove`.

## Several source files

The implementation is inside the header. In a program with more than one
`.c` file, include `easyui.h` normally in one file, and in every other file
write:

```c
#define EASYUI_NO_IMPLEMENTATION
#include "easyui.h"
```

## Events

Pass one of these to `recallfunc`. The short names (`ONCLICK`) are macros
for `EASYUI_ONCLICK` and friends; define `EASYUI_NO_SHORT_NAMES` before the
include if they clash with your own names.

| Event | When | Value passed to the callback |
| --- | --- | --- |
| `ONPRESS` | The left mouse button goes down on the widget, or Space/Enter is pressed while it has focus | `1` |
| `ONRELEASE` | The button comes up again after an `ONPRESS`, wherever the pointer is | `0` |
| `ONCLICK` | Pressed and released on the same widget | `0` |
| `ONCE` | Like `ONCLICK`, but only the first time; the callback is then removed | `0` |
| `ONCHANGE` | The widget's value changed, by the user or by `setvalue` | the new value (see below) |
| `ONSUBMIT` | Enter in a text input; Enter or a double-click in a list box | text length, or the item index |
| `ONHOVER` | The pointer moved onto the widget | `0` |
| `ONLEAVE` | The pointer moved off it | `0` |
| `ONFOCUS` | It got keyboard focus | `0` |
| `ONBLUR` | It lost keyboard focus | `0` |
| `ONKEY` | A key went down while it had focus (on `EASYUI_WINDOW`: any key) | `key \| (modifiers << 8)` |
| `ONDRAW` | Canvas only: draw your content now | `0` |
| `ONFRAME` | Window only: once per frame, about 60 times a second | uptime in ms |
| `ONCLOSE` | Window only: the close button was clicked | `0` |
| `ONRESIZE` | Window only: the window changed size | the new width |

`ONCHANGE` values by widget:

| Widget | Value |
| --- | --- |
| Slider, progress bar | The new value |
| Checkbox, toggle | `1` checked / on, `0` not |
| Radio button | `1` when it becomes selected, `0` when another in its group does |
| Text input | The text's new length (read the text with `gettext`) |
| Dropdown, list box | Index of the selected item |

Window events use `EASYUI_WINDOW` (`0`) as the widget:

```c
static void closing(int value) {
    easyui.messagebox("Not yet", "Save your work first.");   /* or easyui.quit() */
}
easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, closing);
```

Without an `ONCLOSE` callback, clicking the close button ends `run()`.

## Keyboard

Click a widget or press Tab (Shift+Tab backwards) to give it focus; a ring
in the accent colour shows which one has it.

| Widget | Keys |
| --- | --- |
| Button, checkbox, toggle | Space or Enter clicks it |
| Radio button | Space selects; arrow keys move to the previous/next button of the same group and select it |
| Slider | Arrows step by `setstep` (default 1); Home and End jump to the ends |
| Text input | Typing, Backspace, Delete, Left, Right, Home, End; Enter submits; Escape leaves the field |
| Dropdown | Space or Enter opens it; Up and Down change the selection |
| List box | Up and Down move the selection; Enter submits |

Key codes are USB HID usage codes. `easyui.h` defines the common ones:
`EASYUI_KEY_ENTER` (40), `_ESCAPE` (41), `_BACKSPACE` (42), `_TAB` (43),
`_SPACE` (44), `_HOME` (74), `_DELETE` (76), `_END` (77), `_RIGHT` (79),
`_LEFT` (80), `_DOWN` (81), `_UP` (82). Letters are 4 (`a`) … 29 (`z`).

## Theme

MOSdesk stores the chosen theme as an accent colour in
`0:/programs/mosdesk/settings.dat`. easyui reads it when the window opens
and again twice a second, and repaints when it changes. Without MOSdesk
(no settings file), the accent is MOSdesk's default blue, `0xFF3DAEE9`.

The other colours match mosWM: the light palette of its dialogs and the
Settings app by default, or the dark palette of its menus and Files window
after [`easyui.setdarkmode(1)`](#easyui-setdarkmode).

| Theme | Accent |
| --- | --- |
| Blue | `0xFF59A6D8` |
| Teal | `0xFF48B8A5` |
| Copper | `0xFFD28C54` |
| Forest | `0xFF69A86A` |
| Rose | `0xFFD66B83` |
| Gold | `0xFFE1B64C` |

## Text

easyui draws text the way mosWM does:

- **Smooth text (normal).** `easyui_text.h` is a bundled copy of
  ttrend_outline, the anti-aliased OpenType/TrueType renderer mosWM uses
  (integer maths only, no libc). When the window opens, easyui loads the
  Inter fonts mosWM installs, `0:/programs/mosWM/assets/fonts/Inter-Regular.otf`
  and `Inter-Medium.otf`. Text size 1 is 12 px Inter Regular; size 2 and up
  is `8 × size` px Inter Medium, the same sizes mosWM uses for its own text.
  Text is proportional: wrapping, alignment and the text-input caret are
  measured with the real glyph widths.
- **Bitmap fallback.** If neither font can be read (mosWM is not installed,
  or the files are missing), or after
  [`easyui.setsmoothtext(0)`](#easyui-setsmoothtext), text uses the 8 × 8
  bitmap font mosWM falls back to, scaled by the text size.
- **Your own font.** [`easyui.loadfont`](#easyui-loadfont) loads any
  `.otf` or `.ttf` file instead.

Run Studio's preview has the same Inter files at the same path, so smooth
text looks the same there as on MOSdesk.

`include/easyui_text.h` is generated from MOSdesk's
`mosdesk/libraries/ttrend_outline` by `tools/bundle-ttrend.py`; do not edit
it by hand.

## Limits

| Limit | Value |
| --- | --- |
| Widgets | `EASYUI_MAX_WIDGETS` (128) |
| Widget text | `EASYUI_TEXT_MAX` - 1 (127) bytes |
| Timers | `EASYUI_MAX_TIMERS` (8) |
| Windows | One per program |
| Text | Inter via the OpenType renderer (8 × 8 bitmap fallback); ASCII from the keyboard. No kerning or right-to-left text |

## `easyui.init`

```c
int easyui.init(const char* title, int width, int height);
```

Opens the app's window and loads the MOSdesk theme. Call it before
creating any widget.

| Parameter | Description |
| --- | --- |
| `title` | Title bar text (63 bytes at most) |
| `width`, `height` | Content size in pixels, at least 64 × 48 |

**Returns** `1`, or `0` if the window could not be created (no window
manager running, or out of memory). Calling it again does nothing and
returns `1`.

**Example**

```c
if (!easyui.init("Notes", 480, 320)) {
    mos_puts("Start MOSdesk first\n");
    return 1;
}
```

## `easyui.run`

```c
int easyui.run(void);
```

Runs the app until the window is closed or [`easyui.quit`](#easyui-quit)
is called, then closes the window.

**Returns** `0`, so `return easyui.run();` ends `main` with exit code 0.

**Notes** Each frame handles all pending window events, polls the theme,
runs due timers, fires `ONFRAME`, repaints if anything changed and sleeps
16 ms.

## `easyui.update`

```c
int easyui.update(void);
```

One frame of [`easyui.run`](#easyui-run) without the sleep, for programs
with their own loop.

**Returns** `1` while the app should keep running, `0` once it is closing.

**Example**

```c
while (easyui.update()) {
    simulate_step();
    mos_sleep(10);
}
```

## `easyui.quit`

```c
void easyui.quit(void);
```

Makes `run()` return (and `update()` return 0) after the current frame.

## `easyui.settitle`

```c
void easyui.settitle(const char* title);
```

Changes the window's title.

## `easyui.width`

```c
int easyui.width(void);
```

**Returns** the current content width. It changes when the user resizes
the window.

## `easyui.height`

```c
int easyui.height(void);
```

**Returns** the current content height.

## `easyui.window`

```c
uint32_t easyui.window(void);
```

**Returns** the mosWM window ID, for calls from `window.h` such as
[`mos_window_set_flags`](windows.md#mos_window_set_flags).

**Example**

```c
mos_window_set_resizable(easyui.window(), false);   /* fixed-size window */
```

## `easyui.redraw`

```c
void easyui.redraw(void);
```

Asks for a repaint on the next frame. Only needed after changing something
easyui cannot see, such as the pixels behind an image widget or the state a
canvas draws.

## `easyui.setdarkmode`

```c
void easyui.setdarkmode(int on);
```

Switches between the light palette (default) and the dark one. The accent
colour is unchanged.

**Example**

```c
easyui.recallfunc(dark_toggle, ONCHANGE, easyui.setdarkmode);
```

## `easyui.setaccent`

```c
void easyui.setaccent(uint32_t argb);
```

Uses `argb` as the accent colour instead of MOSdesk's. Pass `0` to follow
MOSdesk again.

## `easyui.accent`

```c
uint32_t easyui.accent(void);
```

**Returns** the accent colour in use, as `0xFFRRGGBB`. Use it to draw your
own content in the theme colour.

## `easyui.theme`

```c
const easyui_theme_t* easyui.theme(void);
```

**Returns** every colour easyui is using. The structure is read-only.

| Field | Used for |
| --- | --- |
| `background` | Window content |
| `surface` | Text inputs, lists, panels, slider knobs |
| `border` | Outlines and separators |
| `text`, `muted` | Normal text; placeholders and panel titles |
| `button`, `button_hover`, `button_pressed` | Secondary buttons and dropdowns |
| `accent`, `accent_hover`, `accent_pressed`, `accent_text` | Primary buttons, selection, focus rings; text on accent |
| `track` | Slider, progress and toggle tracks |
| `disabled`, `disabled_text` | Disabled widgets |
| `danger` | `EASYUI_DANGER` buttons |

**Example**

```c
easyui.fillrect(x, y, w, h, easyui.theme()->surface);
```

## `easyui.loadfont`

```c
int easyui.loadfont(const char* path, int bold);
```

Loads an OpenType (`.otf`, CFF outlines) or TrueType (`.ttf`) font file
and uses it for every widget.

| Parameter | Description |
| --- | --- |
| `path` | Font file, for example a resource of your app: `"0:/programs/myapp/Resources/Font.ttf"` |
| `bold` | `0` for size-1 text, `1` for size 2 and larger. Load the same file into both to use one face everywhere |

**Returns** `1`, or `0` if the file could not be read or is not a font the
renderer understands (CID-keyed CFF, variable or colour fonts). The font
already loaded stays in use when it fails.

**Notes** Call it after `init`, which has already loaded Inter. The file
is read into the heap and kept for the life of the program.

**Example**

```c
if (!easyui.loadfont("0:/programs/notes/Resources/Mono.ttf", 0))
    mos_puts("using Inter\n");
```

## `easyui.setsmoothtext`

```c
void easyui.setsmoothtext(int on);
```

`0` draws every text with the 8 × 8 bitmap font even when a font is
loaded (for a retro look, or pixel-exact layouts); `1` (the default) goes
back to smooth text.

## `easyui.smoothtext`

```c
int easyui.smoothtext(void);
```

**Returns** `1` if text is being drawn with an outline font, `0` if it is
using the bitmap fallback.

**Example**

```c
if (!easyui.smoothtext()) easyui.settext(status, "Install mosWM for nicer text");
```

## `easyui.createbutton`

```c
int easyui.createbutton(int x, int y, int w, int h, const char* text);
```

A push button. By default it is filled with the accent colour; see
[`easyui.setstyle`](#easyui-setstyle) for the other styles.

| Parameter | Description |
| --- | --- |
| `x`, `y`, `w`, `h` | Bounds |
| `text` | Label, centred; wraps onto several lines if it is too wide |

**Returns** the widget, or `0`.

**Events** `ONPRESS`, `ONRELEASE`, `ONCLICK`, `ONCE`, `ONHOVER`,
`ONLEAVE`, `ONFOCUS`, `ONBLUR`, `ONKEY`.

**Example**

```c
int save = easyui.createbutton(20, 20, 120, 36, "Save");
easyui.recallfunc(save, ONCLICK, save_file);
```

## `easyui.createlabel`

```c
int easyui.createlabel(int x, int y, int w, int h, const char* text);
```

Text. It wraps at word boundaries to fit `w`, and `\n` starts a new line;
the block is centred vertically in `h`. Change it with `settext`, its
colour with `setcolor`, its size with `settextsize` and its alignment with
`setalign`.

**Returns** the widget, or `0`.

**Notes** Labels get `ONCLICK` too, which makes them usable as links.

## `easyui.createslider`

```c
int easyui.createslider(int x, int y, int w, int h, int min, int max, int value);
```

A slider. It is horizontal, or vertical when `h` is greater than `w`
(the top is `max`).

| Parameter | Description |
| --- | --- |
| `min`, `max` | Range |
| `value` | Starting value, clamped to the range |

**Returns** the widget, or `0`.

**Events** `ONCHANGE` with the new value whenever it moves (dragging,
clicking the track, the mouse wheel, arrow keys or `setvalue`), plus
`ONPRESS` / `ONRELEASE` around a drag.

**Example**

```c
static void volume_changed(int value) { set_volume(value); }

int vol = easyui.createslider(20, 60, 200, 24, 0, 100, 50);
easyui.recallfunc(vol, ONCHANGE, volume_changed);
easyui.setstep(vol, 5);   /* arrow keys and wheel move by 5 */
```

## `easyui.createcheckbox`

```c
int easyui.createcheckbox(int x, int y, int w, int h, const char* text, int checked);
```

A checkbox with a label to its right. Clicking anywhere on the widget
toggles it.

**Returns** the widget, or `0`. Read the state with `getvalue` (`1` or
`0`).

**Events** `ONCHANGE` (`1` / `0`), `ONCLICK`.

## `easyui.createradio`

```c
int easyui.createradio(int x, int y, int w, int h, const char* text, int group);
```

A radio button. Radio buttons with the same `group` number exclude each
other: selecting one clears the others. The first one created in a group
starts selected.

| Parameter | Description |
| --- | --- |
| `group` | Any number you choose; one per set of choices |

**Returns** the widget, or `0`.

**Events** `ONCHANGE` with `1` on the button that became selected and `0`
on the one that was cleared.

**Example**

```c
static int small, medium, large;

static void size_picked(int selected) {
    if (!selected) return;                  /* ignore the one being cleared */
    int w = easyui.current();
    set_size(w == small ? 1 : w == medium ? 2 : 3);
}

small  = easyui.createradio(20, 20, 120, 24, "Small", 1);
medium = easyui.createradio(20, 48, 120, 24, "Medium", 1);
large  = easyui.createradio(20, 76, 120, 24, "Large", 1);
easyui.recallfunc(small, ONCHANGE, size_picked);
easyui.recallfunc(medium, ONCHANGE, size_picked);
easyui.recallfunc(large, ONCHANGE, size_picked);
```

## `easyui.createtoggle`

```c
int easyui.createtoggle(int x, int y, int w, int h, const char* text, int on);
```

An on/off switch with a label. Behaves like a checkbox.

**Returns** the widget, or `0`.

**Events** `ONCHANGE` (`1` / `0`), `ONCLICK`.

## `easyui.createtextinput`

```c
int easyui.createtextinput(int x, int y, int w, int h, const char* placeholder);
```

A one-line text field. `placeholder` is shown in grey while the field is
empty and not focused.

**Returns** the widget, or `0`.

**Events** `ONCHANGE` after every edit (value: the new length),
`ONSUBMIT` when Enter is pressed (value: the length), `ONFOCUS`, `ONBLUR`,
`ONKEY`.

**Notes**

- Typing uses a US keyboard layout. Text is limited to 127 bytes, or less
  with [`easyui.setmaxlength`](#easyui-setmaxlength).
- [`easyui.setpassword`](#easyui-setpassword) shows `*` instead of the
  characters.
- Read the text with `gettext`, change it with `settext`.

**Example**

```c
static int search;

static void run_search(int length) {
    if (length) find(easyui.gettext(search));
}

search = easyui.createtextinput(20, 20, 260, 32, "Search");
easyui.recallfunc(search, ONSUBMIT, run_search);
```

## `easyui.createprogress`

```c
int easyui.createprogress(int x, int y, int w, int h, int max);
```

A progress bar from 0 to `max`. Set it with `setvalue`; change the range
with `setrange`.

**Returns** the widget, or `0`.

**Example**

```c
int bar = easyui.createprogress(20, 20, 300, 10, total_files);
...
easyui.setvalue(bar, files_done);
```

## `easyui.createdropdown`

```c
int easyui.createdropdown(int x, int y, int w, int h);
```

A drop-down list. Add choices with `additem`. Until one is selected it
shows its text (`"Choose..."`; change it with `settext`). Clicking opens a
list of up to 8 rows below it (above, if there is no room), which scrolls
with the wheel.

**Returns** the widget, or `0`. `getvalue` is the selected index (`-1` for
none) and `gettext` the selected item's text.

**Events** `ONCHANGE` with the selected index.

**Example**

```c
int lang = easyui.createdropdown(20, 20, 200, 30);
easyui.additem(lang, "English");
easyui.additem(lang, "Svenska");
easyui.setvalue(lang, 0);
easyui.recallfunc(lang, ONCHANGE, language_changed);
```

## `easyui.createlistbox`

```c
int easyui.createlistbox(int x, int y, int w, int h);
```

A scrolling list. Add rows with `additem`. Clicking a row selects it; the
wheel scrolls.

**Returns** the widget, or `0`. `getvalue` is the selected index (`-1` for
none).

**Events** `ONCHANGE` with the selected index, `ONSUBMIT` with the index
on a double-click or Enter.

**Example**

```c
static int files;

static void open_file(int index) { open(easyui.getitem(files, index)); }

files = easyui.createlistbox(20, 20, 240, 200);
for (long i = 0; i < n; i++) easyui.additem(files, entries[i].name);
easyui.recallfunc(files, ONSUBMIT, open_file);
```

## `easyui.createpanel`

```c
int easyui.createpanel(int x, int y, int w, int h, const char* title);
```

A rounded box with an optional title in its top-left corner, for grouping
widgets. Create it before the widgets that sit on it. `setcolor` changes
its fill.

**Returns** the widget, or `0`.

## `easyui.createimage`

```c
int easyui.createimage(int x, int y, int w, int h, const uint32_t* pixels,
                       int image_w, int image_h);
```

Shows an ARGB8888 image, scaled to `w × h`. Pixels with alpha below 255
are blended.

| Parameter | Description |
| --- | --- |
| `pixels` | `image_w × image_h` pixels, row by row. Not copied: keep the buffer alive while the widget exists |
| `image_w`, `image_h` | Image size |

**Returns** the widget, or `0`.

**Notes** After changing the pixels, call `easyui.redraw()`. Images get
`ONCLICK`, `ONPRESS` and the hover events.

## `easyui.createcanvas`

```c
int easyui.createcanvas(int x, int y, int w, int h);
```

An area you draw yourself. Its `ONDRAW` callback runs whenever the window
is repainted, with drawing clipped to the canvas. Use the drawing calls
below, in window coordinates.

**Returns** the widget, or `0`.

**Example**

```c
static int chart;

static void draw_chart(int value) {
    int x = easyui.getx(chart), y = easyui.gety(chart), h = easyui.getheight(chart);
    for (int i = 0; i < 10; i++)
        easyui.fillrect(x + i * 20, y + h - data[i], 16, data[i], easyui.accent());
}

chart = easyui.createcanvas(20, 20, 200, 120);
easyui.recallfunc(chart, ONDRAW, draw_chart);
```

## `easyui.createseparator`

```c
int easyui.createseparator(int x, int y, int w);
```

A one-pixel horizontal line in the border colour.

**Returns** the widget, or `0`.

## `easyui.remove`

```c
void easyui.remove(int widget);
```

Deletes a widget and frees its items. Its number may be reused by the next
`create...` call.

## `easyui.recallfunc`

```c
int easyui.recallfunc(int widget, int event, easyui_callback fn);
```

Sets the function called when `event` happens on `widget`. Each widget has
one callback per event; registering again replaces it.

| Parameter | Description |
| --- | --- |
| `widget` | A widget, or `EASYUI_WINDOW` (0) for window events |
| `event` | One of the [events](#events) |
| `fn` | `void fn(int value)`, or `NULL` to remove the callback |

**Returns** `1`, or `0` for an unknown widget or event.

**Example**

```c
static void pressed(int v)  { easyui.settext(status, "Holding..."); }
static void released(int v) { easyui.settext(status, "Let go"); }

int button1 = easyui.createbutton(0, 0, 200, 100, "Click me!");
easyui.recallfunc(button1, ONPRESS, pressed);
easyui.recallfunc(button1, ONRELEASE, released);
```

## `easyui.current`

```c
int easyui.current(void);
```

**Returns** the widget whose callback is running (`0` for window events
and timers).

**Example**

```c
static void digit(int v) { append(easyui.gettext(easyui.current())); }

for (int i = 0; i < 10; i++) {
    char t[2] = { (char)('0' + i), 0 };
    easyui.recallfunc(easyui.createbutton(20 + i * 44, 20, 40, 40, t), ONCLICK, digit);
}
```

## `easyui.settext`

```c
void easyui.settext(int widget, const char* text);
```

Changes the text of a button, label, checkbox, radio button, toggle, text
input (the cursor moves to the end), panel title or the dropdown's
"nothing selected" text. Longer text is cut at 127 bytes. Does not fire
`ONCHANGE`.

## `easyui.gettext`

```c
const char* easyui.gettext(int widget);
```

**Returns** the widget's text; for a dropdown or list box, the selected
item's text. The pointer stays valid until the text changes. An invalid
widget returns `""`.

## `easyui.setvalue`

```c
void easyui.setvalue(int widget, int value);
```

Sets the value: a slider or progress bar position (clamped to the range),
a checkbox or toggle (`0` / `1`), a radio button (`1` selects it), or the
selected index of a dropdown or list box (`-1` for none). Fires `ONCHANGE`
if the value changed.

## `easyui.getvalue`

```c
int easyui.getvalue(int widget);
```

**Returns** the value described under `setvalue`.

## `easyui.setrange`

```c
void easyui.setrange(int widget, int min, int max);
```

Changes a slider's or progress bar's range and clamps its value into it.

## `easyui.setstep`

```c
void easyui.setstep(int widget, int step);
```

How far a slider moves per arrow key or wheel notch, and what dragging
snaps to. Default `1`.

## `easyui.additem`

```c
int easyui.additem(int widget, const char* text);
```

Adds a row to a dropdown or list box. The text is copied.

**Returns** the new item's index, or `-1` if memory ran out.

## `easyui.clearitems`

```c
void easyui.clearitems(int widget);
```

Removes every item and clears the selection (to `-1`).

## `easyui.getitem`

```c
const char* easyui.getitem(int widget, int index);
```

**Returns** an item's text, or `""` for an index out of range.

## `easyui.itemcount`

```c
int easyui.itemcount(int widget);
```

**Returns** the number of items.

## `easyui.setenabled`

```c
void easyui.setenabled(int widget, int enabled);
```

Disables (`0`) or enables (`1`) a widget. A disabled widget is drawn grey,
cannot get focus and ignores clicks and keys.

## `easyui.setvisible`

```c
void easyui.setvisible(int widget, int visible);
```

Hides (`0`) or shows (`1`) a widget. Hidden widgets get no events.

**Example**

```c
/* simple tabs: show one page's widgets at a time */
for (int i = 0; i < page1_count; i++) easyui.setvisible(page1[i], tab == 0);
```

## `easyui.setposition`

```c
void easyui.setposition(int widget, int x, int y);
```

Moves a widget.

## `easyui.setsize`

```c
void easyui.setsize(int widget, int width, int height);
```

Resizes a widget.

## `easyui.setanchor`

```c
void easyui.setanchor(int widget, uint32_t anchors);
```

Says which window edges the widget keeps its distance to when the window
is resized. The default is `EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_TOP`: it
stays put.

| Anchors | Effect when the window grows |
| --- | --- |
| `RIGHT` | Moves right with the right edge |
| `LEFT \| RIGHT` | Stretches horizontally |
| `BOTTOM` | Moves down with the bottom edge |
| `TOP \| BOTTOM` | Stretches vertically |

(Each name is prefixed with `EASYUI_ANCHOR_`.) The distances are measured
when `setanchor` is called.

**Example**

```c
int ok = easyui.createbutton(easyui.width() - 110, easyui.height() - 46, 90, 32, "OK");
easyui.setanchor(ok, EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_BOTTOM);   /* stays in the corner */
```

## `easyui.setcolor`

```c
void easyui.setcolor(int widget, uint32_t argb);
```

Gives one widget its own colour: the accent of a button, slider, checkbox,
radio, toggle, progress bar, list or dropdown; the text colour of a label;
the fill of a panel. Pass `0` to go back to the theme.

## `easyui.settextsize`

```c
void easyui.settextsize(int widget, int size);
```

Text size, 1 … 8. With smooth text, size 1 is 12 px Inter Regular and
size `n` (2 and up) is `8 × n` px Inter Medium, as in mosWM. With the
bitmap font, each size is `8 × n` pixels tall. Checkbox, radio and toggle
boxes and list rows grow with it.

**Example**

```c
int title = easyui.createlabel(20, 10, 300, 30, "Settings");
easyui.settextsize(title, 2);
```

## `easyui.setalign`

```c
void easyui.setalign(int widget, int align);
```

`EASYUI_ALIGN_LEFT`, `EASYUI_ALIGN_CENTER` or `EASYUI_ALIGN_RIGHT`, for
labels and buttons. Buttons are centred by default, labels left-aligned.

## `easyui.setstyle`

```c
void easyui.setstyle(int widget, int style);
```

A button's look:

| Style | Look |
| --- | --- |
| `EASYUI_PRIMARY` | Filled with the accent colour (default) |
| `EASYUI_SECONDARY` | Grey, like mosWM's Cancel buttons |
| `EASYUI_DANGER` | Red, for destructive actions |
| `EASYUI_FLAT` | No background until hovered; accent-coloured text |

**Example**

```c
easyui.setstyle(cancel, EASYUI_SECONDARY);
easyui.setstyle(delete_button, EASYUI_DANGER);
```

## `easyui.setpassword`

```c
void easyui.setpassword(int widget, int hidden);
```

Shows a text input's characters as `*` when `hidden` is `1`.

## `easyui.setmaxlength`

```c
void easyui.setmaxlength(int widget, int length);
```

The most characters a text input accepts, at most 127.

## `easyui.setimage`

```c
void easyui.setimage(int widget, const uint32_t* pixels, int image_w, int image_h);
```

Changes an image widget's pixels.

## `easyui.setfocus`

```c
void easyui.setfocus(int widget);
```

Gives a widget keyboard focus (`0` removes focus). Useful to put the cursor
in a text input when the window opens.

## `easyui.focused`

```c
int easyui.focused(void);
```

**Returns** the widget with keyboard focus, or `0`.

## `easyui.type`

```c
int easyui.type(int widget);
```

**Returns** the kind of widget: `EASYUI_BUTTON`, `EASYUI_LABEL`,
`EASYUI_SLIDER`, `EASYUI_CHECKBOX`, `EASYUI_RADIO`, `EASYUI_TOGGLE`,
`EASYUI_TEXTINPUT`, `EASYUI_PROGRESS`, `EASYUI_DROPDOWN`,
`EASYUI_LISTBOX`, `EASYUI_PANEL`, `EASYUI_IMAGE`, `EASYUI_CANVAS`,
`EASYUI_SEPARATOR`, or `EASYUI_NONE` for an invalid one.

## `easyui.getx`

```c
int easyui.getx(int widget);
```

**Returns** the widget's left edge. `gety`, `getwidth` and `getheight`
return the rest of its bounds, which change when anchors move it.

## `easyui.gety`

```c
int easyui.gety(int widget);
```

**Returns** the widget's top edge.

## `easyui.getwidth`

```c
int easyui.getwidth(int widget);
```

**Returns** the widget's width.

## `easyui.getheight`

```c
int easyui.getheight(int widget);
```

**Returns** the widget's height.

## `easyui.settimer`

```c
int easyui.settimer(int interval_ms, easyui_callback fn);
```

Calls `fn` every `interval_ms` milliseconds while `run()` (or `update()`)
is running. The value passed is the timer number.

**Returns** a timer number (1 … 8), or `0` if all timers are in use.

**Notes** Timers run between frames, so their resolution is about 16 ms.
To run something once, stop the timer in its own callback.

**Example**

```c
static int clock_label;

static void tick(int timer) {
    char t[16];
    format_time(t, mos_uptime() / 1000);
    easyui.settext(clock_label, t);
}

easyui.settimer(1000, tick);
```

## `easyui.stoptimer`

```c
void easyui.stoptimer(int timer);
```

Stops a timer from `settimer`.

**Example**

```c
static void once_later(int timer) {
    easyui.stoptimer(timer);
    easyui.settext(status, "Ready");
}
easyui.settimer(2000, once_later);
```

## `easyui.messagebox`

```c
void easyui.messagebox(const char* title, const char* text);
```

Shows a message over the window with an OK button. It returns at once;
while the message is up, the window's widgets ignore input. OK, Enter,
Space or Escape closes it.

| Parameter | Description |
| --- | --- |
| `title` | Heading, up to 63 bytes |
| `text` | Message, up to 255 bytes, word-wrapped |

## `easyui.pixels`

```c
uint32_t* easyui.pixels(void);
```

**Returns** the window's ARGB8888 pixel buffer, `easyui.width()` pixels
per row. easyui repaints it completely each time something changes, so
draw from a canvas's `ONDRAW` rather than writing here directly.

## `easyui.fillrect`

```c
void easyui.fillrect(int x, int y, int w, int h, uint32_t argb);
```

Fills a rectangle. Colours with alpha below 255 are blended. Like all the
drawing calls, it is clipped to the window (and to the canvas inside
`ONDRAW`).

## `easyui.drawrect`

```c
void easyui.drawrect(int x, int y, int w, int h, uint32_t argb);
```

Draws a one-pixel rectangle outline.

## `easyui.fillroundrect`

```c
void easyui.fillroundrect(int x, int y, int w, int h, int radius, uint32_t argb);
```

Fills a rectangle with rounded corners.

## `easyui.fillcircle`

```c
void easyui.fillcircle(int cx, int cy, int radius, uint32_t argb);
```

Fills a circle centred on `(cx, cy)`.

## `easyui.drawline`

```c
void easyui.drawline(int x1, int y1, int x2, int y2, uint32_t argb);
```

Draws a one-pixel line.

## `easyui.drawtext`

```c
void easyui.drawtext(int x, int y, const char* text, uint32_t argb, int size);
```

Draws text with its top-left corner at `(x, y)`, in the same font as the
widgets (blended onto what is underneath). `\n` starts a new line one line
height lower.

## `easyui.textwidth`

```c
int easyui.textwidth(const char* text, int size);
```

**Returns** the width in pixels of the longest line of `text` at `size`.

**Example**

```c
int w = easyui.textwidth(msg, 1);
easyui.drawtext(x + (cw - w) / 2, y + 10, msg, easyui.theme()->text, 1);
```

## `easyui.drawpixels`

```c
void easyui.drawpixels(int x, int y, const uint32_t* pixels, int w, int h);
```

Copies a `w × h` ARGB8888 image with its top-left at `(x, y)`, blending
pixels that are not opaque.
