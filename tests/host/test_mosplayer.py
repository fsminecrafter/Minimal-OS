import importlib.util
import pathlib
import struct
import unittest
from array import array


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "mosplayer", ROOT / "tools" / "media" / "mosplayer.py")
mosplayer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mosplayer)


def tile_rgba(red, green, blue, alpha):
    # One row, one RGBA literal. The stored pixel is the residual from opaque black.
    return bytes((0, 0xFF, red, green, blue, (alpha + 1) & 255))


def tile_delta(previous, current):
    channels = [((current >> shift) - (previous >> shift)) & 255
                for shift in (16, 8, 0, 24)]
    return bytes((2, 0xFF, *channels))


def make_image(red, green, blue, alpha):
    tile = tile_rgba(red, green, blue, alpha)
    header = struct.pack("<4sBBBBIIII", b"MIMG", 1, 4, 6, 0, 1, 1, 1, 0)
    return header + struct.pack("<II", 0, len(tile)) + tile


def make_video(colors):
    frames = bytearray()
    entries = []
    previous = None
    for red, green, blue, alpha in colors:
        color = alpha << 24 | red << 16 | green << 8 | blue
        key = previous is None
        tile = tile_rgba(red, green, blue, alpha) if key else tile_delta(previous, color)
        frame = bytes((int(key), 0, 0, 0, 1)) + struct.pack("<I", len(tile)) + tile
        entries.append((64 + len(frames), len(frame), int(key)))
        frames.extend(frame)
        previous = color
    index_offset = 64 + len(frames)
    index = b"".join(struct.pack("<QII", *entry) for entry in entries)
    header = struct.pack(
        "<4sBBBBIIIIIIQQQII", b"MVID", 1, 4, 6, 0, 1, 1, 30, 1,
        len(colors), len(colors) * 1000 // 30, index_offset, 0, 0, 2, 0)
    return header + frames + index


class MediaDecoderTests(unittest.TestCase):
    def test_documented_example_files_decode(self):
        examples = ROOT / "docs" / "examples"
        image = mosplayer.ImageFile((examples / "sample.img").read_bytes())
        self.assertEqual((image.width, image.height), (320, 180))
        self.assertTrue(image.alpha)
        video = mosplayer.VideoFile((examples / "sample.vid").read_bytes())
        self.assertEqual((video.width, video.height, video.frame_count), (160, 90, 24))
        self.assertTrue(video.alpha)
        first = list(video.frame(0))
        last = list(video.frame(video.frame_count - 1))
        self.assertNotEqual(first, last)

    def test_rgba_image_round_trip(self):
        image = mosplayer.ImageFile(make_image(0x12, 0x34, 0x56, 0x78))
        self.assertEqual((image.width, image.height), (1, 1))
        self.assertTrue(image.alpha)
        self.assertEqual(image.frame()[0], 0x78123456)

    def test_run_operation_repeats_rgba_pixels(self):
        tile = bytes((0, 0xFF, 0x12, 0x34, 0x56, 0x79, 0xC0))
        header = struct.pack("<4sBBBBIIII", b"MIMG", 1, 4, 6, 0, 2, 1, 1, 0)
        data = header + struct.pack("<II", 0, len(tile)) + tile
        image = mosplayer.ImageFile(data)
        self.assertEqual(list(image.frame()), [0x78123456, 0x78123456])

    def test_video_keyframes_and_backward_seek(self):
        video = mosplayer.VideoFile(make_video([
            (10, 20, 30, 40),
            (50, 60, 70, 80),
            (90, 100, 110, 120),
        ]))
        self.assertEqual(video.frame(2)[0], 0x785A646E)
        self.assertEqual(video.frame(0)[0], 0x280A141E)
        self.assertEqual(video.frame(1)[0], 0x50323C46)

    def test_motion_codec_video_decodes_close_to_the_source(self):
        examples = ROOT / "docs" / "examples"
        source = mosplayer.VideoFile((examples / "sample.vid").read_bytes())
        video = mosplayer.VideoFile((examples / "sample-motion.vid").read_bytes())
        self.assertEqual((video.codec_name, video.width, video.height, video.frame_count),
                         ("motion", 160, 90, 24))
        self.assertFalse(video.alpha)
        for number in (0, 1, 2, 13, 23, 5):     # in order, then seeking back
            got, want = video.frame(number), source.frame(number)
            error = sum(abs(((g >> s) & 255) - ((w >> s) & 255))
                        for g, w in zip(got, want) for s in (0, 8, 16)) / (len(got) * 3)
            # (Saturated colour bars with hard edges are 4:2:0's worst case.)
            self.assertLess(error, 10, f"frame {number}")
            self.assertTrue(all(p >> 24 == 255 for p in got))

    def test_damaged_motion_frame_is_rejected_or_survived(self):
        data = bytearray((ROOT / "docs" / "examples" / "sample-motion.vid").read_bytes())
        video = mosplayer.VideoFile(bytes(data))
        offset, size, _ = video.index[0]
        data[offset + 4:offset + size] = bytes(size - 4)     # all zero: decodes to garbage, no crash
        try:
            mosplayer.VideoFile(bytes(data)).frame(0)
        except ValueError:
            pass

    def test_truncated_image_is_rejected(self):
        with self.assertRaises(ValueError):
            mosplayer.ImageFile(b"MIMG")

    def test_stereo_ima_adpcm_matches_kernel_sample_order(self):
        adi = (b"AudioFormat=IADPCM\nSampleRate=8000\nChannels=2\n"
               b"AudioDatalen=2\n#DATA\n\x11\x22")
        audio = mosplayer.AudioFile(adi)
        samples = array("h")
        samples.frombytes(audio.pcm)
        self.assertEqual(list(samples), [1, 3, 2, 6])
        self.assertEqual(audio.frames, 2)

    def test_pcm16_adi(self):
        payload = struct.pack("<hhhh", -100, 100, -200, 200)
        adi = (b"AudioFormat=PCM16\nSampleRate=8000\nChannels=2\n"
               + f"AudioDatalen={len(payload)}\n#DATA\n".encode() + payload)
        audio = mosplayer.AudioFile(adi)
        self.assertEqual(audio.pcm, payload)
        self.assertEqual(audio.duration, 2 / 8000)

    def test_unsupported_adi_codec_reports_error(self):
        with self.assertRaisesRegex(ValueError, "unsupported .adi codec"):
            mosplayer.AudioFile(b"AudioFormat=FLAC\n#DATA\npayload")


if __name__ == "__main__":
    unittest.main()
