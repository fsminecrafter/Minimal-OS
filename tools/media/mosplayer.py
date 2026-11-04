#!/usr/bin/env python3
"""Cross-platform viewer and audio player for Minimal-OS .img, .vid, and .adi files."""

from __future__ import annotations

import argparse
import base64
import os
import struct
import sys
import time
import tkinter as tk
from array import array
from tkinter import filedialog, messagebox, ttk

MAX_SIDE = 16384
MAX_PIXELS = 64 * 1024 * 1024
MAX_FILE_SIZE = 1024 * 1024 * 1024
MAX_PCM_SIZE = 512 * 1024 * 1024
IMA_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
IMA_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _check_dimensions(width: int, height: int) -> None:
    if not 0 < width <= MAX_SIDE or not 0 < height <= MAX_SIDE:
        raise ValueError(f"invalid image dimensions: {width} x {height}")
    if width * height > MAX_PIXELS:
        raise ValueError(f"image is too large to display safely: {width} x {height}")


def _tile_decode(
    data: bytes,
    width: int,
    height: int,
    pixels: array,
    tx: int,
    ty: int,
    has_previous: bool,
) -> None:
    x0, y0 = tx * 64, ty * 64
    tw, th = min(64, width - x0), min(64, height - y0)
    if tw <= 0 or th <= 0:
        raise ValueError("tile is outside the image")
    filter_bytes = (th + 3) // 4
    if len(data) < filter_bytes:
        raise ValueError("truncated tile filters")
    indices = [0] * 64
    previous = run = 0
    pos = filter_bytes

    def hash_pixel(value: int) -> int:
        return ((((value >> 16) & 255) * 3 + ((value >> 8) & 255) * 5 +
                 (value & 255) * 7 + (value >> 24) * 11) & 63)

    for y in range(th):
        filt = (data[y // 4] >> (2 * (y % 4))) & 3
        if filt > 2 or (filt == 1 and y == 0) or (filt == 2 and not has_previous):
            raise ValueError("invalid tile row filter")
        for x in range(tw):
            if run:
                run -= 1
            else:
                if pos >= len(data):
                    raise ValueError("truncated tile pixel data")
                op = data[pos]
                pos += 1
                if op == 0xFE:
                    if pos + 3 > len(data):
                        raise ValueError("truncated RGB pixel")
                    previous = ((previous & 0xFF000000) | data[pos] << 16 |
                                data[pos + 1] << 8 | data[pos + 2])
                    pos += 3
                    indices[hash_pixel(previous)] = previous
                elif op == 0xFF:
                    if pos + 4 > len(data):
                        raise ValueError("truncated RGBA pixel")
                    previous = (data[pos + 3] << 24 | data[pos] << 16 |
                                data[pos + 1] << 8 | data[pos + 2])
                    pos += 4
                    indices[hash_pixel(previous)] = previous
                elif op & 0xC0 == 0x00:
                    previous = indices[op]
                elif op & 0xC0 == 0x40:
                    r = (((previous >> 16) + ((op >> 4) & 3) - 2) & 255)
                    g = (((previous >> 8) + ((op >> 2) & 3) - 2) & 255)
                    b = ((previous + (op & 3) - 2) & 255)
                    previous = previous & 0xFF000000 | r << 16 | g << 8 | b
                    indices[hash_pixel(previous)] = previous
                elif op & 0xC0 == 0x80:
                    if pos >= len(data):
                        raise ValueError("truncated luma pixel")
                    op2 = data[pos]
                    pos += 1
                    dg = (op & 63) - 32
                    dr, db = dg + (op2 >> 4) - 8, dg + (op2 & 15) - 8
                    r = ((previous >> 16) + dr) & 255
                    g = ((previous >> 8) + dg) & 255
                    b = (previous + db) & 255
                    previous = previous & 0xFF000000 | r << 16 | g << 8 | b
                    indices[hash_pixel(previous)] = previous
                else:
                    run = op & 63

            index = (y0 + y) * width + x0 + x
            if filt == 0:
                reference = 0xFF000000
            elif filt == 1:
                reference = pixels[index - width]
            else:
                reference = pixels[index]
            value = 0
            for shift in (0, 8, 16, 24):
                value |= ((((previous >> shift) & 255) +
                           ((reference >> shift) & 255)) & 255) << shift
            pixels[index] = value
    if run:
        raise ValueError("tile run exceeds its pixel count")


class ImageFile:
    def __init__(self, data: bytes):
        if (len(data) < 32 or data[:4] != b"MIMG" or data[4] != 1 or
                data[5] not in (3, 4) or data[6] != 6):
            raise ValueError("not a supported .img file")
        self.width, self.height = struct.unpack_from("<II", data, 8)
        _check_dimensions(self.width, self.height)
        across = (self.width + 63) // 64
        down = (self.height + 63) // 64
        tiles = across * down
        if _u32(data, 16) != tiles or 24 + (tiles + 1) * 4 > len(data):
            raise ValueError("invalid .img tile table")
        table_at = 24
        payload_at = table_at + (tiles + 1) * 4
        payload_size = len(data) - payload_at
        self.pixels = array("I", [0]) * (self.width * self.height)
        for tile in range(tiles):
            start, end = _u32(data, table_at + tile * 4), _u32(data, table_at + (tile + 1) * 4)
            if start > end or end > payload_size:
                raise ValueError(f"invalid .img tile {tile} offsets")
            _tile_decode(data[payload_at + start:payload_at + end],
                         self.width, self.height, self.pixels,
                         tile % across, tile // across, False)
        self.alpha = data[5] == 4
        self.frame_count = 1
        self.fps = 0.0
        self.duration = 0.0

    def frame(self, _number: int = 0) -> array:
        return self.pixels


class VideoFile:
    def __init__(self, data: bytes):
        if (len(data) < 64 or data[:4] != b"MVID" or data[4] != 1 or
                data[5] not in (3, 4) or data[6] != 6):
            raise ValueError("not a supported .vid file")
        self.data = data
        (self.width, self.height, fps_num, fps_den, self.frame_count,
         self.duration_ms) = struct.unpack_from("<IIIIII", data, 8)
        _check_dimensions(self.width, self.height)
        if not fps_num or not fps_den or not self.frame_count or self.frame_count > 100_000:
            raise ValueError("invalid .vid timing or frame count")
        self.fps = fps_num / fps_den
        if self.fps > 1000:
            raise ValueError("invalid .vid frame rate")
        self.duration = self.duration_ms / 1000
        self.alpha = data[5] == 4
        self.index_offset = struct.unpack_from("<Q", data, 32)[0]
        self.audio_offset, self.audio_size = struct.unpack_from("<QQ", data, 40)
        index_end = self.index_offset + self.frame_count * 16
        if self.index_offset < 64 or index_end > len(data):
            raise ValueError("invalid .vid frame index")
        self.index = []
        for frame in range(self.frame_count):
            offset, size, flags = struct.unpack_from("<QII", data, self.index_offset + frame * 16)
            if offset < 64 or offset + size > self.index_offset:
                raise ValueError(f"invalid .vid frame {frame} range")
            self.index.append((offset, size, flags))
        if self.audio_size and (
                self.audio_offset < index_end or self.audio_offset + self.audio_size > len(data)):
            raise ValueError("invalid embedded .adi range")
        self.audio = data[self.audio_offset:self.audio_offset + self.audio_size] if self.audio_size else None
        self._pixels = array("I", [0]) * (self.width * self.height)
        self._decoded = -1

    def _decode_one(self, number: int) -> None:
        offset, size, _ = self.index[number]
        frame = self.data[offset:offset + size]
        across = (self.width + 63) // 64
        tiles = across * ((self.height + 63) // 64)
        bitmap_size = (tiles + 7) // 8
        if len(frame) < 4 + bitmap_size:
            raise ValueError(f"truncated .vid frame {number}")
        key = bool(frame[0] & 1)
        if not key and self._decoded != number - 1:
            raise ValueError(f".vid frame {number} requires its previous frame")
        bitmap = frame[4:4 + bitmap_size]
        sizes_at = 4 + bitmap_size
        coded = sum((bitmap[tile // 8] >> (tile % 8)) & 1 for tile in range(tiles))
        if sizes_at + coded * 4 > len(frame):
            raise ValueError(f"truncated .vid frame {number} tile sizes")
        data_at = sizes_at + coded * 4
        pos = data_at
        size_index = 0
        for tile in range(tiles):
            if not (bitmap[tile // 8] & (1 << (tile % 8))):
                if key:
                    raise ValueError(f".vid keyframe {number} omits a tile")
                continue
            tile_size = _u32(frame, sizes_at + size_index * 4)
            size_index += 1
            end = pos + tile_size
            if end > len(frame):
                raise ValueError(f"truncated .vid tile in frame {number}")
            _tile_decode(frame[pos:end], self.width, self.height, self._pixels,
                         tile % across, tile // across, not key)
            pos = end
        self._decoded = number

    def frame(self, number: int) -> array:
        if not 0 <= number < self.frame_count:
            raise ValueError("frame number is outside the video")
        if number == self._decoded:
            return self._pixels
        if self._decoded >= 0 and number > self._decoded:
            start = self._decoded + 1
            for candidate in range(number, start, -1):
                if self.index[candidate][2] & 1:
                    start = candidate
                    break
        else:
            start = number
            while start and not (self.index[start][2] & 1):
                start -= 1
        if self.index[start][2] & 1:
            self._pixels = array("I", [0]) * (self.width * self.height)
            self._decoded = -1
        for current in range(start, number + 1):
            self._decode_one(current)
        return self._pixels


def _decode_nibble(code: int, predictor: int, step_index: int) -> tuple[int, int, int]:
    step = IMA_STEP[step_index]
    delta = step >> 3
    if code & 1:
        delta += step >> 2
    if code & 2:
        delta += step >> 1
    if code & 4:
        delta += step
    predictor = max(-32768, min(32767, predictor + (-delta if code & 8 else delta)))
    step_index = max(0, min(88, step_index + IMA_INDEX[code & 7]))
    return predictor, step_index, predictor


class AudioFile:
    def __init__(self, data: bytes):
        marker = data.find(b"#DATA")
        if marker < 0:
            marker = data.find(b"DATA")
        if marker < 0 or marker > 2048:
            raise ValueError("missing .adi #DATA marker")
        header = {}
        for line in data[:marker].decode("ascii", "replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                header[key.strip()] = value.strip()
        try:
            self.format = header.get("AudioFormat", "IADPCM").upper()
            self.sample_rate = int(header.get("SampleRate", "48000"))
            self.channels = int(header.get("Channels", "2"))
            volume = max(0, min(100, int(header.get("Globalvol", "80"))))
        except ValueError as exc:
            raise ValueError("invalid .adi header value") from exc
        if not 8000 <= self.sample_rate <= 192000 or self.channels not in (1, 2):
            raise ValueError("unsupported .adi sample rate or channel count")
        line_end = data.find(b"\n", marker)
        if line_end < 0:
            raise ValueError(".adi #DATA marker is missing its line ending")
        payload_at = line_end + 1
        payload = data[payload_at:]
        data_length = header.get("AudioDatalen")
        if data_length is not None:
            try:
                expected = int(data_length)
            except ValueError as exc:
                raise ValueError("invalid .adi AudioDatalen") from exc
            if expected < 0 or expected > len(payload):
                raise ValueError(".adi payload is shorter than AudioDatalen")
            payload = payload[:expected]
        if self.format == "PCM16":
            frame_size = self.channels * 2
            payload = payload[:len(payload) - len(payload) % frame_size]
            if len(payload) > MAX_PCM_SIZE:
                raise ValueError(".adi PCM data exceeds the 512 MiB playback limit")
            self.pcm = payload
        elif "IMA" in self.format or self.format == "IADPCM":
            if len(payload) * 4 > MAX_PCM_SIZE:
                raise ValueError(".adi decoded audio would exceed the 512 MiB playback limit")
            self.pcm = self._decode_ima(payload)
        else:
            raise ValueError(f"unsupported .adi codec: {self.format}")
        self.volume = volume / 100.0
        self.frames = len(self.pcm) // (self.channels * 2)
        if not self.frames:
            raise ValueError(".adi file contains no audio samples")
        self.duration = self.frames / self.sample_rate

    def _decode_ima(self, payload: bytes) -> bytes:
        decoded = array("h")
        if self.channels == 1:
            predictor = step_index = 0
            for byte in payload:
                for code in (byte & 15, byte >> 4):
                    predictor, step_index, sample = _decode_nibble(code, predictor, step_index)
                    decoded.append(sample)
        else:
            predictor_l = predictor_r = step_l = step_r = 0
            usable = len(payload) & ~1
            for pos in range(0, usable, 2):
                left, right = payload[pos], payload[pos + 1]
                predictor_l, step_l, l0 = _decode_nibble(left & 15, predictor_l, step_l)
                predictor_r, step_r, r0 = _decode_nibble(right & 15, predictor_r, step_r)
                predictor_l, step_l, l1 = _decode_nibble(left >> 4, predictor_l, step_l)
                predictor_r, step_r, r1 = _decode_nibble(right >> 4, predictor_r, step_r)
                decoded.extend((l0, r0, l1, r1))
        if sys.byteorder != "little":
            decoded.byteswap()
        return decoded.tobytes()


class MediaPlayer:
    def __init__(self, root: tk.Tk, initial_path: str | None = None):
        self.root = root
        self.root.title("Minimal-OS Media Player")
        self.root.geometry("1000x720")
        self.root.minsize(480, 360)
        try:
            import pygame
        except ImportError as exc:
            raise RuntimeError("pygame is required for audio. Install it with: python -m pip install pygame") from exc
        self.pygame = pygame
        self.media: ImageFile | VideoFile | AudioFile | None = None
        self.path = ""
        self.playing = False
        self.anchor_time = 0.0
        self.anchor_position = 0.0
        self.audio_channel = None
        self.audio_sound = None
        self.audio_data: AudioFile | None = None
        self.photo = None
        self.dragging_seek = False
        self.last_frame = -1
        self.current_position = 0.0
        self.pixel_text = "Move pointer over video/image for pixel values"
        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.bind("<space>", lambda _event: self.toggle_play())
        self.root.bind("<Left>", lambda _event: self.seek_by(-1.0))
        self.root.bind("<Right>", lambda _event: self.seek_by(1.0))
        self.root.bind("<comma>", lambda _event: self.step_frame(-1))
        self.root.bind("<period>", lambda _event: self.step_frame(1))
        self.root.bind("<Control-o>", lambda _event: self.open_dialog())
        self.root.after(20, self._tick)
        if initial_path:
            self.root.after(100, lambda: self.open_path(initial_path))

    def _build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)
        menu = tk.Menu(self.root)
        file_menu = tk.Menu(menu, tearoff=False)
        file_menu.add_command(label="Open…", command=self.open_dialog, accelerator="Ctrl+O")
        file_menu.add_command(label="Quit", command=self.root.destroy)
        menu.add_cascade(label="File", menu=file_menu)
        self.root.config(menu=menu)

        self.status = tk.StringVar(value="Open an .img, .vid, or .adi file")
        ttk.Label(self.root, textvariable=self.status, anchor="w", padding=(8, 5)).grid(
            row=0, column=0, sticky="ew")
        self.canvas = tk.Canvas(self.root, background="#202020", highlightthickness=0)
        self.canvas.grid(row=1, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _event: self._draw())
        self.canvas.bind("<Motion>", self._inspect_pixel)
        self.canvas.bind("<Leave>", self._pointer_left)

        controls = ttk.Frame(self.root, padding=6)
        controls.grid(row=2, column=0, sticky="ew")
        controls.columnconfigure(1, weight=1)
        self.play_button = ttk.Button(controls, text="Play", command=self.toggle_play, width=8)
        self.play_button.grid(row=0, column=0, padx=(0, 6))
        self.seek = ttk.Scale(controls, from_=0, to=1, command=None)
        self.seek.grid(row=0, column=1, sticky="ew")
        self.seek.bind("<ButtonPress-1>", lambda _event: setattr(self, "dragging_seek", True))
        self.seek.bind("<ButtonRelease-1>", self._seek_released)
        self.time_label = ttk.Label(controls, text="00:00 / 00:00", width=16, anchor="e")
        self.time_label.grid(row=0, column=2, padx=(6, 0))
        self.volume = tk.DoubleVar(value=80)
        ttk.Label(controls, text="Vol").grid(row=0, column=3, padx=(10, 2))
        ttk.Scale(controls, from_=0, to=100, variable=self.volume,
                  command=self._set_volume, length=100).grid(row=0, column=4)
        self.debug = tk.StringVar(value="Pixel: move pointer over the image")
        ttk.Label(self.root, textvariable=self.debug, anchor="w", padding=(8, 3)).grid(
            row=3, column=0, sticky="ew")

    def open_dialog(self) -> None:
        path = filedialog.askopenfilename(
            filetypes=[("Minimal-OS media", "*.img *.vid *.adi"),
                       ("All files", "*.*")])
        if path:
            self.open_path(path)

    def open_path(self, path: str) -> None:
        try:
            size = os.path.getsize(path)
            if not 0 < size <= MAX_FILE_SIZE:
                raise ValueError("file is empty or larger than the 1 GiB safety limit")
            with open(path, "rb") as stream:
                data = stream.read()
            extension = os.path.splitext(path)[1].lower()
            if extension == ".img":
                media = ImageFile(data)
            elif extension == ".vid":
                media = VideoFile(data)
            elif extension == ".adi":
                media = AudioFile(data)
            else:
                raise ValueError("choose a file ending in .img, .vid, or .adi")
        except (OSError, ValueError, OverflowError) as exc:
            messagebox.showerror("Could not open media", str(exc), parent=self.root)
            return
        self._stop_audio()
        self.playing = False
        self.media, self.path = media, path
        self.audio_data = media if isinstance(media, AudioFile) else None
        if isinstance(media, VideoFile) and media.audio:
            try:
                self.audio_data = AudioFile(media.audio)
            except ValueError as exc:
                self.audio_data = None
                messagebox.showwarning("Embedded soundtrack", str(exc), parent=self.root)
        self.root.title(f"{os.path.basename(path)} - Minimal-OS Media Player")
        self.current_position = 0.0
        self.anchor_position = 0.0
        self.last_frame = -1
        self.pixel_text = "Move pointer over video/image for pixel values"
        self.seek.configure(to=max(0.001, self._duration()))
        self.play_button.configure(state="normal" if not isinstance(media, ImageFile) else "disabled")
        self.play_button.configure(text="Play")
        if isinstance(media, VideoFile):
            self._show_frame(0)
        else:
            self._draw()
        self._update_status()

    def close(self) -> None:
        self._stop_audio()
        self.pygame.mixer.quit()
        self.root.destroy()

    def _duration(self) -> float:
        return self.media.duration if self.media else 0.0

    def _audio_start(self, position: float) -> None:
        if not self.audio_data or self.audio_data.sample_rate <= 0:
            return
        pcm = self.audio_data.pcm
        frame_offset = min(self.audio_data.frames, int(position * self.audio_data.sample_rate))
        byte_offset = frame_offset * self.audio_data.channels * 2
        pcm = pcm[byte_offset:]
        if not pcm:
            return
        try:
            mixer = self.pygame.mixer
            if not mixer.get_init():
                mixer.init(frequency=self.audio_data.sample_rate, size=-16,
                           channels=self.audio_data.channels, buffer=1024)
            elif mixer.get_init() != (self.audio_data.sample_rate, -16, self.audio_data.channels):
                mixer.quit()
                mixer.init(frequency=self.audio_data.sample_rate, size=-16,
                           channels=self.audio_data.channels, buffer=1024)
            sound = mixer.Sound(buffer=pcm)
            sound.set_volume(self.audio_data.volume * float(self.volume.get()) / 80.0)
            self.audio_channel = mixer.Channel(0)
            self.audio_channel.play(sound)
            self.audio_sound = sound
        except self.pygame.error as exc:
            self.audio_channel = None
            self.audio_sound = None
            messagebox.showerror("Audio playback failed", str(exc), parent=self.root)

    def _stop_audio(self) -> None:
        if self.audio_channel is not None:
            self.audio_channel.stop()
            self.audio_channel = None
            self.audio_sound = None

    def toggle_play(self) -> None:
        if not self.media or isinstance(self.media, ImageFile):
            return
        if self.playing:
            self.current_position = self._position()
            self.playing = False
            if self.audio_channel is not None:
                self.audio_channel.pause()
            self.play_button.configure(text="Play")
        else:
            if self.current_position >= self._duration():
                self.current_position = 0.0
            self.anchor_position = self.current_position
            self.anchor_time = time.monotonic()
            self.playing = True
            if self.audio_data:
                self._audio_start(self.current_position)
            elif isinstance(self.media, VideoFile) and self.media.audio:
                self.status.set("Embedded soundtrack could not be decoded; playing video silently")
            self.play_button.configure(text="Pause")
        self._tick()

    def _position(self) -> float:
        if self.playing:
            return min(self._duration(), self.anchor_position + time.monotonic() - self.anchor_time)
        return self.current_position

    def seek_by(self, seconds: float) -> None:
        if self.media and not isinstance(self.media, ImageFile):
            self._seek_to(self._position() + seconds)

    def step_frame(self, amount: int) -> None:
        if not isinstance(self.media, VideoFile):
            return
        if self.playing:
            self.toggle_play()
        number = max(0, min(self.media.frame_count - 1, self.last_frame + amount))
        self.current_position = number / self.media.fps
        self._show_frame(number)
        self._update_status()

    def _seek_released(self, _event: tk.Event) -> None:
        self.dragging_seek = False
        self._seek_to(float(self.seek.get()))

    def _seek_to(self, position: float) -> None:
        if not self.media or isinstance(self.media, ImageFile):
            return
        was_playing = self.playing
        self._stop_audio()
        self.current_position = max(0.0, min(self._duration(), position))
        if was_playing:
            self.anchor_position = self.current_position
            self.anchor_time = time.monotonic()
            if self.audio_data:
                self._audio_start(self.current_position)
        self._show_current_frame()
        self._update_status()

    def _set_volume(self, _value: str) -> None:
        if self.audio_sound is not None and self.audio_data:
            self.audio_sound.set_volume(self.audio_data.volume * float(self.volume.get()) / 80.0)

    def _show_current_frame(self) -> None:
        if isinstance(self.media, VideoFile):
            number = min(self.media.frame_count - 1,
                         int(self.current_position * self.media.fps))
            self._show_frame(number)
        elif isinstance(self.media, ImageFile):
            self._draw()

    def _show_frame(self, number: int) -> None:
        if not isinstance(self.media, VideoFile):
            return
        try:
            self.media.frame(number)
        except ValueError as exc:
            self.playing = False
            self._stop_audio()
            messagebox.showerror("Damaged video", str(exc), parent=self.root)
            return
        self.last_frame = number
        self._draw()

    def _tick(self) -> None:
        if self.media:
            if self.playing:
                self.current_position = self._position()
                if self.current_position >= self._duration():
                    self.current_position = self._duration()
                    self.playing = False
                    self._stop_audio()
                    self.play_button.configure(text="Play")
                self._show_current_frame()
            if not self.dragging_seek:
                self.seek.set(self.current_position)
            self.time_label.configure(
                text=f"{self._format_time(self.current_position)} / {self._format_time(self._duration())}")
            self._update_status()
        self.root.after(33, self._tick)

    @staticmethod
    def _format_time(seconds: float) -> str:
        total = max(0, int(seconds))
        return f"{total // 60:02}:{total % 60:02}"

    def _draw(self) -> None:
        self.canvas.delete("all")
        media = self.media
        if not isinstance(media, (ImageFile, VideoFile)):
            if isinstance(media, AudioFile):
                self.canvas.create_text(
                    max(10, self.canvas.winfo_width() // 2),
                    max(10, self.canvas.winfo_height() // 2),
                    text=f"ADI audio\n{media.sample_rate} Hz · {media.channels} channel(s)\n{media.format}",
                    fill="white", font=("TkDefaultFont", 18), justify="center")
            return
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        if width < 2 or height < 2:
            return
        scale = min(width / media.width, height / media.height)
        out_w = max(1, int(media.width * scale))
        out_h = max(1, int(media.height * scale))
        pixels = media.frame(max(0, self.last_frame) if isinstance(media, VideoFile) else 0)
        ppm = bytearray(f"P6\n{out_w} {out_h}\n255\n".encode("ascii"))
        for y in range(out_h):
            sy = min(media.height - 1, int(y * media.height / out_h))
            checker_y = (sy // 12) & 1
            for x in range(out_w):
                sx = min(media.width - 1, int(x * media.width / out_w))
                color = pixels[sy * media.width + sx]
                alpha = color >> 24
                bg = 0xB8 if ((sx // 12 + checker_y) & 1) else 0xE0
                inverse = 255 - alpha
                ppm.extend((
                    ((((color >> 16) & 255) * alpha + bg * inverse) // 255),
                    ((((color >> 8) & 255) * alpha + bg * inverse) // 255),
                    (((color & 255) * alpha + bg * inverse) // 255),
                ))
        self.photo = tk.PhotoImage(data=base64.b64encode(ppm), format="PPM")
        self.canvas.create_image(width // 2, height // 2, image=self.photo, anchor="center")

    def _inspect_pixel(self, event: tk.Event) -> None:
        media = self.media
        if not isinstance(media, (ImageFile, VideoFile)):
            return
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        scale = min(width / media.width, height / media.height)
        shown_w, shown_h = media.width * scale, media.height * scale
        x0, y0 = (width - shown_w) / 2, (height - shown_h) / 2
        x, y = int((event.x - x0) / scale), int((event.y - y0) / scale)
        if 0 <= x < media.width and 0 <= y < media.height:
            pixels = media.frame(max(0, self.last_frame) if isinstance(media, VideoFile) else 0)
            color = pixels[y * media.width + x]
            self.pixel_text = (
                f"Pixel ({x}, {y})  ARGB #{color:08X}  "
                f"A={color >> 24:3} R={(color >> 16) & 255:3} "
                f"G={(color >> 8) & 255:3} B={color & 255:3}")
            self.debug.set(self.pixel_text)
        else:
            self._update_status()

    def _pointer_left(self, _event: tk.Event) -> None:
        self.pixel_text = "Move pointer over video/image for pixel values"
        self._update_status()

    def _update_status(self) -> None:
        media = self.media
        if media is None:
            self.status.set("Open an .img, .vid, or .adi file")
        elif isinstance(media, ImageFile):
            self.status.set(f"{self.path}  |  IMG {media.width} x {media.height}  |  "
                            f"{'RGBA' if media.alpha else 'RGB'}")
        elif isinstance(media, VideoFile):
            self.status.set(f"{self.path}  |  VID {media.width} x {media.height}  |  "
                            f"{self.last_frame + 1}/{media.frame_count}  |  {media.fps:.3g} fps  |  "
                            f"{'RGBA' if media.alpha else 'RGB'}  |  "
                            f"{'audio' if media.audio else 'silent'}")
        elif isinstance(media, AudioFile):
            self.status.set(f"{self.path}  |  ADI {media.format}  |  "
                            f"{media.sample_rate} Hz  |  {media.channels} ch")
        if not self.pixel_text.startswith("Pixel "):
            self.pixel_text = (f"Frame {self.last_frame + 1 if isinstance(media, VideoFile) else '-'}"
                               f"  |  {self._format_time(self.current_position)}"
                               "  |  Move pointer over video/image for pixel values")
        self.debug.set(self.pixel_text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", nargs="?", help="optional .img, .vid, or .adi file to open")
    args = parser.parse_args()
    root = tk.Tk()
    try:
        MediaPlayer(root, args.file)
    except RuntimeError as exc:
        root.destroy()
        parser.error(str(exc))
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
