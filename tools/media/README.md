# Minimal-OS media player

`mosplayer.py` is a small Linux and Windows viewer/player for Minimal-OS
`.img`, `.vid`, and `.adi` files. It decodes the native formats directly,
without requiring a kernel build, `ffmpeg`, or the `mosmedia` converter.

## Setup and use

Install Python 3.10 or later, then install the audio dependency:

```sh
python -m pip install -r tools/media/requirements.txt
python tools/media/mosplayer.py
python tools/media/mosplayer.py path/to/file.vid
```

Tkinter is included with most Python distributions. On some Linux
distributions it is a separate package (for example, `python3-tk`).
The pygame-ce dependency supplies cross-platform sound playback.

## Controls and debugging

- **Space**: play or pause; **Left/Right**: seek one second.
- **Comma/Period**: step a paused video by one frame.
- **Ctrl+O**: open a file; seek and volume are adjustable in the window.
- Move the pointer over an image/video to inspect its exact source pixel as
  ARGB hex and individual channel values. Transparency is shown over a
  checkerboard.
- The status line shows format, dimensions, frame count/rate, and whether a
  video contains an embedded soundtrack.

The player supports RGB/RGBA `.img`, `.vid` in all three codecs (tiles,
motion and h264; motion decoding is slow in Python, about 50 ms a frame at
200x112; h264 frames are decoded by ffmpeg, which must be in `PATH` for
those files), PCM16 and IMA-ADPCM `.adi`,
including `.adi` soundtracks embedded in `.vid`. Unsupported audio codecs
are reported instead of being silently misplayed. Large media files and
images have safety limits to avoid accidental excessive memory use.

Run its decoder tests with:

```sh
python -m unittest discover -s tests/host -p 'test_mosplayer.py'
```
