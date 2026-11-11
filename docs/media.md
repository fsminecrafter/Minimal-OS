# Images (.img) and videos (.vid)

Minimal-OS has its own picture and video formats, in
`Minimal-OS-SDK/include/mosmedia.h` (plain C, no libc, so they run in
programs, in mosWM and on the build PC).

A `.vid` file uses one of three codecs, the way an `.adi` file is PCM16 or
ADPCM:

| Codec | For | How |
| --- | --- | --- |
| `tiles` | screen recordings, drawings, animation, games | lossless (or near-lossless with `--loss`), keeps transparency; only changed tiles are stored |
| `motion` | camera footage, films | lossy and built like H.264: motion vectors, a transform, a range coder. RGB only; tens of times smaller on camera footage |
| `h264` | video that comes from, or goes to, the rest of the world | H.264 (AVC), Constrained Baseline profile: the codec of most video on the web. Smaller than `motion` at the same quality; encoded by x264 on the build PC |

Every player reads all three: the Video app, Mxplorer and the Python
player (which uses ffmpeg for `h264`).

| | .img | .vid |
| --- | --- | --- |
| What | A picture, 8-bit RGB or RGBA (transparency, like PNG) | Frames in 32-bit ARGB8888 (including transparency), a frame rate and an `.adi` soundtrack |
| Compression | Lossless, about PNG's size, faster to decode | `tiles`: lossless, only changed tiles are stored; `motion`: lossy, like H.264; `h264`: H.264 itself |
| Opens with | Photos (`0:/programs/mos_photos.run`), and as a wallpaper in Settings | Video (`0:/programs/mos_video.run`) |
| In the terminal | `img 0:/users/me/Pictures/cat.img` | `video 0:/users/me/Videos/clip.vid` |
| In Mxplorer | `<img src="cat.img">`, or open the file's address | `<video src="clip.vid" controls>`, or open the file's address |
| Make one | `mosmedia img picture.png out.img` | `mosmedia vid movie.mp4 out.vid` (motion), `mosmedia vid screen.mkv out.vid --codec tiles`, `mosmedia vid movie.mp4 out.vid --codec h264` |

## In the terminal

The MOSdesk terminal opens pictures and videos in their apps:

```sh
video 0:/users/me/Videos/clip.vid     # also: vid
img ~/Pictures/cat.img                # also: image, photo
```

Relative paths are inside the current folder.

## On web pages

Mxplorer shows `.img` pictures in `<img>` and plays `.vid` videos in
`<video>`, with sound. It picks files by their extension, so other
pictures (PNG, JPEG) still show a placeholder:

```html
<img src="logo.img" alt="Logo" width="200">
<video src="clip.vid" controls autoplay muted loop poster="still.img"></video>
<video controls><source src="clip.vid"></video>
```

A video's sound plays from a file, so a downloaded video is written to
`0:/data/mxplorer-video.vid` the first time it plays with sound. Downloads
are limited to 32 MiB; local files (`0:/...` in the address bar, or
`run Mxplorer 0:/path/page.html`) have no limit, and local videos are read
as they play. Pages from the network cannot use local files.

## Making files

`make build/tools/mosmedia` builds the converter for the build PC (Linux
or macOS), and `make build/tools/mosmedia.exe` the same tool for Windows
(with MinGW-w64; on Windows, ffmpeg and ffprobe must be in `PATH`). It
uses ffmpeg for everything it reads and writes, and codes the soundtrack
itself (IMA ADPCM, the same bytes as `tools/audioconverter/wavtoadi.py`).

Encoding uses every CPU core: each frame's macroblocks are decided on
several threads as a wavefront (each row two macroblocks behind the one
above), then written in order, so the file is byte-for-byte the same with
any number of threads. ffmpeg decodes the input on its own, a few frames
ahead, with the GPU's video decoder when there is one (NVDEC, VA-API,
Quick Sync, D3D11/DXVA2 on Windows; `-hwaccel auto`). The encoding itself
runs on the CPU.

For viewing and debugging `.img`, `.vid`, and `.adi` files on Linux or
Windows, see the Python player in [tools/media](../tools/media/README.md).

Small sample files are included: [sample.img](examples/sample.img) is a
320x180 RGBA gradient with transparent corners,
[sample.vid](examples/sample.vid) is a 2-second, 12 fps color test video
(tiles codec), and [sample-motion.vid](examples/sample-motion.vid) and
[sample-h264.vid](examples/sample-h264.vid) are the same video in the
motion and h264 codecs.
Open either in the [Python player](../tools/media/README.md), for example:

```sh
python tools/media/mosplayer.py docs/examples/sample.img
python tools/media/mosplayer.py docs/examples/sample.vid
```

```sh
build/tools/mosmedia img logo.png logo.img        # any picture ffmpeg reads; keeps transparency
build/tools/mosmedia png logo.img logo.png        # and back
build/tools/mosmedia vid film.mp4 film.vid --width 640 --q 26   # motion codec
build/tools/mosmedia vid screen.mkv screen.vid --codec tiles      # lossless
build/tools/mosmedia vid film.mp4 film.vid --codec h264 --width 640   # H.264 (needs ffmpeg with libx264)
build/tools/mosmedia unvid clip.vid check.mp4     # to look at the result (check.raw: exact pixels)
build/tools/mosmedia info clip.vid
```

The picture codec and the tiles codec preserve all 8 bits per color
channel. `.img` files may store opaque pictures as RGB or pictures with
transparency as RGBA; tiles `.vid` files may store RGB or RGBA frames (the
`mosmedia vid` converter writes RGBA and preserves any source alpha
channel). Motion `.vid` files are RGB. The converter writes motion unless
`--codec tiles` is given; programs using `mvid_writer_*` get tiles unless
they set `codec`.

`vid` options:

| Option | Meaning |
| --- | --- |
| `--codec motion` / `--codec tiles` / `--codec h264` | the codec (default `motion`; use `tiles` for screen recordings, drawings and anything with transparency) |
| `--q N` | motion: quality, 0 (best) to 51 (smallest), default 26; 6 steps double or halve the quantizer step. h264: x264's quality setting (`-crf`), the same scale |
| `--loss N` | tiles: each colour may end up to N away from the source (alpha stays exact); 0, lossless, is the default |
| `--fast` | quicker, a little larger (motion: about 25% quicker and 3% larger; tiles: delta frames only use the last frame; h264: x264's `veryfast` preset instead of `medium`) |
| `--threads N` | motion: encoder threads (default: one per CPU core) |
| `--no-hwaccel` | decode the input on the CPU only |
| `--width N` | scale (keeping the shape) |
| `--fps N` | the frame rate (the source's, at most 60) |
| `--key N` | a keyframe every N frames (default every 2 seconds) |
| `--no-audio` | drop the sound |

What to expect, on a 3-minute camera clip at 200x112 (PSNR against the
source; the original MP4 is 854x480 at 0.74 Mbit/s; measured with the
first version of the encoder):

| Encoding | Picture data | PSNR |
| --- | --- | --- |
| tiles, lossless | 131 MB | exact |
| tiles, `--loss 8` | 75 MB | 39 dB |
| motion, `--q 22` | 13.2 MB | 36.4 dB |
| motion, `--q 26` | 8.6 MB | 34.2 dB |
| motion, `--q 30` | 5.3 MB | 32.1 dB |
| x264 `-crf 23`, for comparison | 3.9 MB | 33.4 dB |

So motion is around 20 times smaller than tiles on footage like this, and
about 1.7 times the size of x264 at the same quality. Its sharp colour
edges are limited by 4:2:0 colour (half-resolution colour, like nearly all
video): lossless x264 in 4:2:0 reaches 38 dB on the same clip. The
soundtrack (IMA ADPCM, 44.1 kHz stereo) adds 2.6 MB a minute either way.
Decoding a 480p motion video takes under 10 ms a frame on one core of the
build PC. Encoding 480p takes about 40 ms a frame on one core (about 30 ms
with `--fast`) and scales with the cores. MinimaFS files stop at 4 GiB.

## How the tiles codec works

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

## How the motion codec works

The code is in `Minimal-OS-SDK/include/mosmedia_motion.h`. All of it is
integer arithmetic, so every decoder (C or Python) shows exactly the
pixels the encoder used as its reference.

1. **Colour.** Each frame becomes YCbCr 4:2:0 (full range, BT.601):
   Y per pixel, Cb and Cr per 2x2 pixels. The coded size is rounded up
   to 16 by repeating the last column and row.
2. **Macroblocks.** The picture is cut into 16x16 macroblocks (with 8x8
   of each colour plane), coded left to right, top to bottom.
3. **Prediction.** In a keyframe each 4x4 luma block is predicted from
   the pixels above and to its left (DC, vertical, horizontal or
   TrueMotion: left + above - corner), and each 8x8 colour block the
   same way. In other frames a macroblock is either *skipped* (moved by
   the predicted vector, nothing else stored), *moved* by a vector in
   quarter pixels from the last frame (bilinear, edges repeated) plus a
   residual, or predicted like a keyframe block. The predicted vector is
   the median of the left, above and above-right macroblocks' vectors.
4. **Transform.** The residual goes through H.264's 4x4 integer
   transform and quantizer. QP (0-51) picks the step size; colour uses
   H.264's lower colour QP above 29. Keyframes use QP - 2.
5. **Deblocking.** After a frame is decoded, a filter smooths 4x4 block
   edges whose two sides differ by less than the QP allows: first all
   vertical edges, then all horizontal ones. The filtered frame is the
   next frame's reference.
6. **Entropy coding.** Everything is coded with a binary range coder
   (LZMA's: 11-bit probabilities, adapting by 1/32 a bit). Probabilities
   start at 1/2 in every frame. Coefficients are coded like CABAC: a
   coded-block flag, then a significance flag and a last flag for each
   position in zigzag order, then the levels from the last one back, as
   "more than 1", an adaptive unary part and Exp-Golomb for the rest,
   then the sign.

The encoder searches motion with candidate vectors (zero, the predicted
one, the neighbours' and the same macroblock's in the last frame), then a
shrinking square pattern down to quarter pixels. For each macroblock it
compares skip, moved and intra by distortion plus lambda times estimated
bits (lambda as in H.264's reference encoder). If a frame does not fit
its buffer, the encoder raises QP for that frame.

The bitstream is exactly what `mmc_decode_frame` reads; for each
macroblock:

- P frames: `skip` (context: skipped neighbours), and if not skipped
  `intra` (context: intra neighbours);
- intra: 16 luma modes in raster order (a flag "not the most probable
  mode", which is the smaller of the left and above block's modes, then
  the remaining mode in 1 or 2 bits), the colour mode (2 bits), the 16
  luma blocks, then 4 Cb and 4 Cr blocks;
- moved: the vector minus the predicted one (x then y: zero flag, sign,
  unary/Exp-Golomb size), then the 16 luma, 4 Cb and 4 Cr blocks;
- each block: a coded flag (context: whether the left and above blocks
  have coefficients) and its coefficients.

## The H.264 codec

`Minimal-OS-SDK/include/mosmedia_h264.h` is an H.264 decoder written for
Minimal-OS: plain C without libc, so it runs in programs and in mosWM
like the other codecs. Videos made elsewhere are almost always H.264, so
this is the codec to carry them in, and the first piece a web video
player needs.

**What it decodes: the Constrained Baseline profile.**

| Part | State |
| --- | --- |
| I and P frames, several slices per picture | yes |
| CAVLC entropy coding | yes |
| Intra prediction: 4x4 (9 modes), 16x16, colour, I_PCM; constrained intra prediction | yes |
| Inter prediction: 16x16 down to 4x4 blocks, quarter-pixel motion (6-tap filter), up to 16 reference frames, reference list reordering and memory management commands | yes |
| In-loop deblocking filter, with per-slice offsets | yes |
| 4:2:0, 8 bits, progressive, cropping to any size; BT.601 and BT.709, full and limited range | yes |
| CABAC, B frames, the 8x8 transform (Main and High profile) | **no** - refused with a message, not garbled |
| Interlaced video, slice groups (FMO/ASO), weighted prediction, 4:2:2 / 4:4:4, more than 8 bits | no |

Main and High profile are what most downloaded films and the higher
resolutions of video sites use, so they are the next step for this
decoder (CABAC and B frames first). `mosmedia vid --codec h264` always
writes Constrained Baseline, so every `.vid` it makes plays.

**It is exact.** H.264 defines the decoded pictures bit for bit.
`make test-h264` (`tools/media/h264_check.sh`, needs ffmpeg with libx264)
lets x264 encode 23 test videos with very different settings - all block
sizes, 1 to 16 reference frames, several slices, the best and the worst
quality, deblocking off and at its extremes, constrained intra, noise,
odd sizes from 16x16 to 1280x720 - and compares every byte of every frame
with ffmpeg's decoder. `make test-host` checks the sample file against a
checksum made the same way, seeks in it, and decodes 400 damaged copies,
which must fail cleanly. Parts no test stream uses yet: I_PCM blocks,
long-term reference frames and the memory management commands (x264 does
not emit them in this profile).

**Speed.** 854x480 decodes in about 4 ms a frame on one core of the build
PC, without any SIMD.

**In a `.vid`.** A frame is the 4-byte frame header (bit 0 of the first
byte: keyframe) and one access unit in Annex B form (NAL units behind
`00 00 01` start codes). A keyframe is an IDR picture and carries its
parameter sets (SPS, PPS), so decoding can start at any keyframe. There
are no B frames, so frames are stored in the order they are shown. The
converter writes full-range BT.601 colour, like the motion codec, which
lets the same colour conversion - on the CPU or on the graphics card -
serve both; streams from elsewhere are converted by their own matrix and
range.

**In programs.** `mvid_open` / `mvid_decode` handle it like the other
codecs. The decoder on its own:

```c
#include "mosmedia_h264.h"

h264_t d;
h264_init(&d, malloc, largest_access_unit);
if (h264_decode(&d, unit, unit_size))                   // one frame's NAL units
    h264_to_argb(&d, pixels, h264_width(&d), h264_height(&d), stride);
else if (h264_error(&d)[0]) puts(h264_error(&d));       // e.g. "CABAC (Main and High profile) is not supported"
h264_free(&d, free);
```

Recording (`mvid_writer_frame`) stays tiles or motion: there is no H.264
encoder in the OS. `mvid_writer_h264_frame` stores frames something else
encoded.

**What a web video player still needs.** Sites deliver H.264 inside MP4
(or fragmented MP4 over DASH/HLS) with AAC sound, usually in Main or High
profile. This decoder is the video half for the Baseline streams; an MP4
demuxer, an AAC decoder and the missing profiles are separate work.

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
| 4 | 1 | version: 1 (always the tiles codec) or 2 (the codec is at offset 60) |
| 5 | 1 | channels, 3 or 4 (motion: 3) |
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
| 60 | 4 | version 2: the codec, 0 tiles, 1 motion or 2 h264; version 1: 0 |

The tiles codec writes version 1, so players from before the motion codec
still read those files; a motion file is version 2, which they reject
cleanly.

Frames follow the header. A tiles frame has:

- a flags byte (bit 0: keyframe) and 3 zero bytes;
- a bitmap with one bit per tile: set means the tile is stored, clear
  means it is unchanged from the previous frame (a keyframe stores every
  tile);
- a 4-byte size for each stored tile, then the tiles (coded as in .img;
  keyframes never use the previous-frame filter).

A motion frame has a flags byte (bit 0: keyframe), its QP, 2 zero bytes,
then the range-coded frame.

An h264 frame has a flags byte (bit 0: keyframe), 3 zero bytes, then one
H.264 access unit in Annex B form.

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
v.codec = MVID_CODEC_MOTION;   // optional, before the first frame (then v.qp)
mvid_writer_frame(&v, pixels, stride);
mvid_writer_finish(&v, adi_bytes, adi_size);
```

`apps/video.c` and `apps/photos.c` in MOSdesk are complete examples.
`make test-host` runs `tests/host/media_tests.c`: exact round trips of
many picture and video shapes, lossy limits, motion videos that must
decode to exactly the encoder's reference frames, seeking, and damaged
files, which must fail cleanly.

## Hardware decoding

What runs where:

| Step | tiles | motion | h264 |
| --- | --- | --- | --- |
| Entropy decoding, prediction, transform, deblocking | CPU, changed tiles in parallel on the kernel's SMP workers | CPU, one core | **GPU** (NVDEC) once it has proven itself on the file, else CPU |
| Colour conversion (4:2:0 YCbCr to RGB) | - | **GPU** when FarcadeGL runs on the card (`hw-3d`), else CPU | the same, for full-range BT.601 streams (what the converter writes) |

`.vid` uses its own codecs, so the card's fixed-function video decoder
(NVDEC), which only knows H.264, HEVC, VP9 and AV1 bitstreams, cannot read
it. What a GPU can take over is what a 3D engine is good at: the same
small computation for every pixel.

**Motion codec, on the GPU.** The decoder's result is three planes (Y,
Cb, Cr). `mvid_decode` hands them to the kernel
(`SYS_MEDIA_CONVERT_YCBCR`); the NVIDIA backend uploads them as
one-byte textures, draws one triangle with a shader that upsamples the
colour planes and converts to RGB, and copies the picture back with the
copy engine. The shader does `mmc_to_argb`'s arithmetic on whole numbers,
so the pixels are exactly the CPU's (`tests/host/video_gpu_tests.c` runs
the same shader bytecode on the CPU rasterizer and compares every pixel).
Without `hw-3d` the call answers "not now" once and the file is converted
on the CPU as before. Every player gets this without changes: the Video
app, Mxplorer, anything using `mvid_decode`. `gpu nvidia` counts the
frames converted on the GPU. Like the 3D path it rides on, this has not
run on a card yet.

**Why not more of the motion codec.** The range coder carries its state
from one macroblock to the next, intra prediction reads the pixels of the
blocks before it, and the deblocking filter's output is the next frame's
reference, bit for bit. Those steps are sequential by design; a
rasterizer cannot run them, and hardware decoders do them in dedicated
circuits for the standard codecs only. Decoding has no motion search, so
this part is cheap.

**H.264 on the card's video decoder (NVDEC).** H.264 is one of the
formats NVIDIA's NVDEC decodes completely in hardware. On an RTX 20 /
GTX 16 card where `gpu nvidia boot` brought up the video decoder's
channel (step 5a) and the 3D engine (step 4), `h264` videos are decoded
like this:

1. `mosmedia_h264.h` still reads every frame's headers: the parameter
   sets, the picture order count, the reference picture list. It
   describes the picture to the kernel (`SYS_MEDIA_DECODE_H264`,
   `syscall_media_h264_t`): parameter sets, the picture's numbers, its
   reference pictures, and the slice data as it is in the file.
2. The NVIDIA backend fills NVIDIA's picture setup structure
   (`nvdec_h264_pic_s`, from `nvdec_drv.h` in open-gpu-doc) and sends one
   decode job to NVDEC (`nv_nvdec.c`). NVDEC writes an NV12 picture into
   VRAM. The 17 picture buffers stay in VRAM, so reference pictures never
   travel.
3. The 3D engine converts NV12 to RGB with a shader doing the CPU
   decoder's arithmetic (`NV_VIDEO_NV12_FS`), and the copy engine brings
   the RGB picture back.

**It has to prove itself first.** H.264 output is exact, so the first
pictures of every file are decoded on both: from the first keyframe, the
CPU decoder and NVDEC each decode every picture and the two are compared
pixel for pixel. After 12 equal pictures NVDEC takes over alone at the
next keyframe, and the CPU only reads headers (cheap). One different
pixel and the file stays on the CPU, and the kernel is told
(`SYS_MEDIA_H264_MISMATCH`): it stops offering NVDEC for every file and
`gpu nvidia` says so. If NVDEC fails later (a job that does not finish),
the CPU decoder starts again from the last keyframe and the video goes
on. Streams NVDEC is not used for: anything not full-range BT.601 or
with cropping at the top or left (the converter never writes those), and
anything the CPU decoder refuses.

Only full pictures from the CPU decoder are ever shown before NVDEC has
passed the comparison, so a mistake in this path costs speed, never a
wrong picture. **It has not run on a card yet.** The NVDEC interface is
taken from NVIDIA's published `nvdec_drv.h` / `clc5b0.h` and from Mesa's
NVK, which decodes H.264 on NVDEC the same way; the host tests check the
setup structure, the methods, the hand-over and fallback logic (with a
stand-in decoder), and that the NV12 shader gives exactly the CPU's
pixels.

**Tiles codec.** Each changed tile is independent, so the kernel decodes
the tiles of a frame in parallel on its CPU cores
(`SYS_MEDIA_DECODE_TILES`). A tile itself is a byte-by-byte code (runs,
an index of recent values), which a GPU cannot split up; this stays CPU
work.
