# Images (.img) and videos (.vid)

Minimal-OS has its own lossless picture and video formats. Both use one
codec, in `Minimal-OS-SDK/include/mosmedia.h` (plain C, no libc, so it
runs in programs, in mosWM and on the build PC).

| | .img | .vid |
| --- | --- | --- |
| What | A picture, 8-bit RGB or RGBA (transparency, like PNG) | Frames in 32-bit ARGB8888 (including transparency), a frame rate and an `.adi` soundtrack |
| Compression | Lossless, about PNG's size, faster to decode | Lossless, only changed tiles are stored |
| Opens with | Photos (`0:/programs/mos_photos.run`), and as a wallpaper in Settings | Video (`0:/programs/mos_video.run`) |
| Make one | `mosmedia img picture.png out.img` | `mosmedia vid movie.mp4 out.vid` |

## Making files

`make build/tools/mosmedia` builds the converter for the build PC. It uses
ffmpeg for everything it reads and writes, and
`tools/audioconverter/wavtoadi.py` for the soundtrack.

For viewing and debugging `.img`, `.vid`, and `.adi` files on Linux or
Windows, see the Python player in [tools/media](../tools/media/README.md).

Small sample files are included: [sample.img](examples/sample.img) is a
320x180 RGBA gradient with transparent corners, and
[sample.vid](examples/sample.vid) is a 2-second, 12 fps color test video.
Open either in the [Python player](../tools/media/README.md), for example:

```sh
python tools/media/mosplayer.py docs/examples/sample.img
python tools/media/mosplayer.py docs/examples/sample.vid
```

```sh
build/tools/mosmedia img logo.png logo.img        # any picture ffmpeg reads; keeps transparency
build/tools/mosmedia png logo.img logo.png        # and back
build/tools/mosmedia vid clip.mp4 clip.vid --width 640 --fps 30
build/tools/mosmedia unvid clip.vid check.mp4     # to look at the result (check.raw: exact pixels)
build/tools/mosmedia info clip.vid
```

The codec preserves all 8 bits per color channel. `.img` files may store
opaque pictures as RGB or pictures with transparency as RGBA; `.vid` files
may store RGB or RGBA frames. The `mosmedia vid` converter writes RGBA and
preserves any source alpha channel.

`vid` options: `--width N` scales (keeping the shape), `--fps N` sets the
frame rate (the source's, at most 60), `--key N` puts a keyframe every N
frames (default every 2 seconds), `--no-audio` drops the sound and
`--fast` encodes quicker but larger.

Lossless video is ideal for screen recordings, animations, games and
drawings, which shrink to a few percent of their raw size. Camera footage
does not: a photo-like frame stays near half its raw size (a 1600x1067
photo: 45%, about what PNG manages), which is many times an MP4. Scale such
videos down (`--width 480`) or keep them short. MinimaFS files stop at
4 GiB.

## How the codec works

A picture is cut into 64x64 tiles. Each tile is compressed on its own, so
tiles can be decoded in any order or in parallel. Inside a tile:

1. Each row picks a filter: the row as it is, minus the row above (in the
   same tile), or in a video minus the same row of the previous frame.
   The encoder tries each and keeps the smallest.
2. The filtered values (per channel, modulo 256) are coded with QOI's
   operations: a run of the previous value, one of 64 recently seen
   values, a small difference, or the value written out. Runs carry on
   into the next row.

Without a filter a row is coded against opaque black, so an opaque
picture never spends bits on alpha.

## .img layout

All numbers are little-endian.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `MIMG` |
| 4 | 1 | version, 1 |
| 5 | 1 | channels: 3 (RGB) or 4 (RGBA) |
| 6 | 1 | tile size as a power of two: 6 (64 pixels) |
| 7 | 1 | flags, 0 |
| 8 | 4 | width (1 to 16384) |
| 12 | 4 | height (1 to 16384) |
| 16 | 4 | tile count: ceil(width / 64) x ceil(height / 64) |
| 20 | 4 | reserved, 0 |
| 24 | 4 x (count + 1) | where each tile starts, counted from the end of this table; the last entry is where the data ends |

Then the tiles, left to right and top to bottom. A tile with `rows` rows:

- `ceil(rows / 4)` bytes of filters, 2 bits a row, row 0 in the low bits:
  0 none, 1 the row above (never on row 0), 2 the previous frame (videos
  only);
- the operations for all its pixels, row by row:

| Byte | Meaning |
| --- | --- |
| `00iiiiii` | the value at index `i` (index = (r x 3 + g x 5 + b x 7 + a x 11) mod 64) |
| `01rrggbb` | the previous value plus (r - 2, g - 2, b - 2), same alpha |
| `10gggggg`, `rrrrbbbb` | green changes by g - 32, red by that plus r - 8, blue by that plus b - 8 |
| `11nnnnnn` (n < 62) | the previous value n + 1 times |
| `0xFE` r g b | that colour, same alpha |
| `0xFF` r g b a | that colour and alpha |

The pixel is the decoded value plus the filter's reference pixel. The
previous value starts at 0 and the index at all zeros in every tile.

## .vid layout

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `MVID` |
| 4 | 1 | version, 1 |
| 5 | 1 | channels, 3 or 4 |
| 6 | 1 | tile size as a power of two, 6 |
| 7 | 1 | flags: bit 0 = has a soundtrack |
| 8 | 4 | width |
| 12 | 4 | height |
| 16 | 4 | frame rate numerator |
| 20 | 4 | frame rate denominator |
| 24 | 4 | frame count |
| 28 | 4 | duration in milliseconds |
| 32 | 8 | where the index starts |
| 40 | 8 | where the soundtrack starts |
| 48 | 8 | soundtrack size |
| 56 | 4 | keyframe interval (information only) |
| 60 | 4 | reserved, 0 |

Frames follow the header. Each frame has:

- a flags byte (bit 0: keyframe) and 3 zero bytes;
- a bitmap with one bit per tile: set means the tile is stored, clear
  means it is unchanged from the previous frame (a keyframe stores every
  tile);
- a 4-byte size for each stored tile, then the tiles (coded as in .img;
  keyframes never use the previous-frame filter).

The index has 16 bytes per frame: 8-byte offset, 4-byte size and 4-byte
flags (bit 0: keyframe). To show frame n, a player decodes from the
keyframe at or before it. The next frame decodes over the last one in
place, so one frame buffer is enough.

The soundtrack is an ordinary `.adi` file stored unchanged. The kernel
plays it from inside the video: `SYS_AUDIO_OPEN` takes the soundtrack's
offset in `value` (`mos_audio_open_at(path, offset)`). The player uses the
sound's position as its clock, so picture and sound stay in step.

## In programs

```c
#include "mosmedia.h"

// Pictures
mimg_info_t info;
if (mimg_info(data, size, &info)) {
    uint32_t* px = malloc(info.width * info.height * 4);
    mimg_decode(data, size, px, info.width);          // 0xAARRGGBB
}
size_t n = mimg_encode(px, w, h, w, true, out, mimg_encode_bound(w, h));

// Videos: give it a read function (offset, buffer, length)
mvid_reader_t r;
mvid_open(&r, my_read, &fd, file_size, malloc);
mvid_decode(&r, mvid_frame_at(&r.info, ms), frame);    // frame: width x height
mos_audio_open_at(path, (uint32_t)r.info.audio_offset);

// Recording: frames in, one at a time (`fast` for live capture)
mvid_writer_t v;
mvid_writer_begin(&v, w, h, 30, 1, false, 60, true, my_write, &fd, malloc, free);
mvid_writer_frame(&v, pixels, stride);
mvid_writer_finish(&v, adi_bytes, adi_size);
```

`apps/video.c` and `apps/photos.c` in MOSdesk are complete examples.
`make test-host` runs `tests/host/media_tests.c`: exact round trips of
many picture and video shapes, seeking, and damaged files, which must
fail cleanly.

## Hardware decoding

Everything runs on the CPU for now. The GPU's video engines (NVDEC and
NVENC on NVIDIA cards) only run NVIDIA-signed firmware, which needs the
GSP boot that `docs/gpu.md` describes and which is not finished. The
codec avoids work that a CPU does badly: there is no transform or motion
search, unchanged tiles cost nothing, and tiles are independent. That
also makes it a good fit for decoding in parallel later, on several CPU
cores or as a GPU compute shader.
