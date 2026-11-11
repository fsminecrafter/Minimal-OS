# Playing audio (`audio.h`)

Plays `.adi` files through the sound card the kernel found: Intel HD Audio
or AC97, and lets programs play sound they make themselves through audio
streams (below), mixed together with the file. There is one file player
for the whole system. Opening a file replaces
whatever was playing, including a file started with the terminal's `play`
command. MOSdesk's Player app (`0:/programs/mos_player.run`) uses this API.

Every call returns `SYS_SUCCESS` (0) or a negative `SYS_ERR_*` value.

| Call | Does |
| --- | --- |
| `mos_audio_open(path)` | Loads a file and starts playing it. `SYS_ERR_NOTFOUND` if it does not exist, `SYS_ERR_INVAL` if it is not a playable `.adi`, `SYS_ERR_GENERIC` if there is no sound card |
| `mos_audio_pause()` | Pauses |
| `mos_audio_resume()` | Continues; after the end it plays the file again from the start |
| `mos_audio_stop()` | Stops and unloads the file |
| `mos_audio_seek(ms)` | Jumps to a position in milliseconds |
| `mos_audio_volume(v)` | Sets the volume, 0-100. Opening a file resets it to the file's `Globalvol` |
| `mos_audio_status(&st)` | Fills a `syscall_audio_status_t`; works with nothing loaded |

## `syscall_audio_status_t`

| Field | Meaning |
| --- | --- |
| `state` | `SYSCALL_AUDIO_IDLE` (nothing loaded), `_PLAYING`, `_PAUSED`, `_ENDED` |
| `position_ms`, `duration_ms` | Where playback is and how long the file is |
| `sample_rate`, `channels` | From the file's header |
| `volume` | 0-100 |
| `format` | `"IMA-ADPCM"` or `"PCM16"` |
| `driver` | `"Intel HD Audio"`, `"AC97"`, or `""` when there is no sound card |
| `path` | The file that is loaded |

```c
#include "audio.h"

int main(void) {
    if (mos_audio_open("0:/music.adi") != SYS_SUCCESS) return 1;
    syscall_audio_status_t st;
    do {
        mos_sleep(500);
        mos_audio_status(&st);
    } while (st.state == SYSCALL_AUDIO_PLAYING);
    return 0;
}
```

Playback runs in the kernel, so it keeps going after the program exits.
Call `mos_audio_stop()` first if it should not.

## Audio streams: sound a program makes

For games, synthesizers and anything else that makes its own sound. A
program opens a stream and keeps writing 16-bit samples into it; the kernel
mixes up to 8 streams (from any programs) and the file player together,
converting each stream's rate to the sound card's.

| Call | Does |
| --- | --- |
| `mos_audio_stream_open(rate, channels)` | Opens a stream: 8000-96000 Hz, 1 (mono) or 2 (stereo) channels. Returns a handle (1 or more), or `SYS_ERR_BUSY` when all 8 are taken, `SYS_ERR_GENERIC` with no sound card, `SYS_ERR_INVAL` for a bad rate or channel count |
| `mos_audio_stream_write(h, samples, frames)` | Queues `frames` frames (a frame is one sample per channel, stereo interleaved left, right). Returns how many it took: fewer when the queue is full, never blocks |
| `mos_audio_stream_queued(h)` | Frames still waiting to be played |
| `mos_audio_stream_query(h, &info)` | Fills a `syscall_audio_stream_t`: `queued`, `capacity` (16384 frames), `mix_rate` (the card's rate), `underruns` (times the stream ran dry while open) |
| `mos_audio_stream_set_volume(h, v)` | 0-100, default 100 |
| `mos_audio_stream_close(h)` | Closes it; what is still queued is dropped |

A stream belongs to the program that opened it and is closed when that
program exits. The file player's volume does not affect streams, and
opening or stopping a file does not touch them.

Keep a little queued - 50-100 ms is a good amount - and top it up
regularly: too little and the sound breaks up when the program is slow
for a moment, too much and it lags behind the picture.

```c
#include "audio.h"

/* One second of a 440 Hz square wave. */
int main(void) {
    int h = mos_audio_stream_open(22050, 1);
    if (h < 0) return 1;
    static int16_t buf[22050];
    for (int i = 0; i < 22050; i++) buf[i] = (i / 25) % 2 ? 6000 : -6000;
    uint32_t done = 0;
    while (done < 22050) {
        long n = mos_audio_stream_write(h, buf + done, 22050 - done);
        if (n > 0) done += (uint32_t)n;
        else mos_sleep(20);
    }
    while (mos_audio_stream_queued(h) > 0) mos_sleep(20);
    mos_audio_stream_close(h);
    return 0;
}
```

Doom's sound effects (`programs/doom/sound.c`) are a fuller example: up to
16 sounds mixed in the program into one stereo stream.

## Making `.adi` files

```sh
python3 tools/audioconverter/wavtoadi.py --format IADPCM song.wav song.adi
```

Takes 16-bit mono or stereo WAV. Rates from 8 to 48 kHz play as they are;
the HD Audio driver resamples when the codec lacks the file's rate.
