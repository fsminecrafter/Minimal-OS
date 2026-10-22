# Playing audio (`audio.h`)

Plays `.adi` files through the sound card the kernel found: Intel HD Audio
or AC97. There is one player for the whole system. Opening a file replaces
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

## Making `.adi` files

```sh
python3 tools/audioconverter/wavtoadi.py --format IADPCM song.wav song.adi
```

Takes 16-bit mono or stereo WAV. Rates from 8 to 48 kHz play as they are;
the HD Audio driver resamples when the codec lacks the file's rate.
