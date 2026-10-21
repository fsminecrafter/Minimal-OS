# Text in your own pixels (`font.h`)

`font.h` draws text into any ARGB8888 pixel buffer, for programs that
render their own window contents (games, canvases, clocks) instead of using
easyui widgets. It uses the same OpenType renderer and Inter fonts as mosWM
and easyui, so text looks like the rest of MOSdesk: smooth and
anti-aliased. When the fonts cannot be read (mosWM is not installed), it
falls back to the 8 × 8 bitmap font, scaled by the text size.

Header only; it can be included together with `easyui.h`.

```c
#include "window.h"
#include "font.h"

static uint32_t pixels[320 * 120];

draw_text(pixels, 320, 120, 10, 10, "Score 120", 0xFFEAF2F8, 1);
int w = text_width("Game over", 2);
draw_text(pixels, 320, 120, (320 - w) / 2, 50, "Game over", 0xFFFFD166, 2);
```

## Sizes

| Size | Smooth text | Bitmap fallback |
| --- | --- | --- |
| 1 | 12 px Inter Regular | 8 × 8 |
| 2 and up | 8 × size px Inter Medium | (8 × size) square glyphs |

These match easyui's text sizes and mosWM's own text.

## Functions

### `draw_text`

```c
void draw_text(uint32_t* pixels, uint32_t width, uint32_t height,
               int x, int y, const char* text, uint32_t color, int size);
```

Draws `text` with its top-left corner at `(x, y)` into a `width` × `height`
buffer whose rows are `width` pixels long. Clipped to the buffer. `'\n'`
starts a new line. The color is `0xAARRGGBB`; text is blended with the
pixels underneath, so a lower alpha draws translucent text. Text is
UTF-8; the bundled Inter files are a subset that may lack some characters, and the bitmap fallback
has ASCII only.

### `text_width`

```c
int text_width(const char* text, int size);
```

Width in pixels of the widest line, for centring and right alignment.

### `text_height`

```c
int text_height(int size);
```

Height of one line (ascent + descent).

### `font_load`

```c
int font_load(const char* path, int bold);
```

Uses another `.otf` / `.ttf` file instead of Inter: `bold = 0` for size-1
text, `1` for size 2 and up. Returns 1 on success. Call it before the first
`draw_text`.

### `font_set_smooth`, `font_is_smooth`

```c
void font_set_smooth(int on);
int font_is_smooth(void);
```

`font_set_smooth(0)` always uses the bitmap font. `font_is_smooth()` says
whether the OpenType fonts were found.

## Notes

- The fonts are read from `0:/programs/mosWM/assets/fonts/` the first time
  text is drawn or measured (about 50 KiB of memory for both faces).
- Glyphs are cached, so redrawing the same text every frame is cheap.
- MOSdesk's Snake, Clock and Paint use it.
