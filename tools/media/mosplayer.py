#!/usr/bin/env python3
"""Cross-platform viewer and audio player for Minimal-OS .img, .vid, and .adi files."""

from __future__ import annotations

import argparse
import base64
import os
import struct
import subprocess
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


# --- The "motion" .vid codec (Minimal-OS-SDK/include/mosmedia_motion.h) ---
# A bit-exact port of the C decoder. It is slow in Python (fine for
# checking files, not for smooth playback of large videos).

_MMC_ZIGZAG = (0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15)
_MMC_V = ((10, 16, 13), (11, 18, 14), (13, 20, 16), (14, 23, 18), (16, 25, 20), (18, 29, 23))
_MMC_CLASS = tuple(0 if not (i >> 2) & 1 and not i & 1 else 1 if (i >> 2) & 1 and i & 1 else 2
                   for i in range(16))
_MMC_CHROMA_QP = (
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39)
_MMC_ALPHA = (
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 7, 8, 9, 10, 12, 13,
    15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255)
_MMC_BETA = (
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18)
_MMC_TC0 = (
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 13)
_MMC_UBINS = 14
_MMC_MV_MAX = 16383


def _clip(v: int) -> int:
    return 0 if v < 0 else 255 if v > 255 else v


class _RangeDecoder:
    __slots__ = ("data", "pos", "range", "code", "over")

    def __init__(self, data: bytes):
        self.data, self.pos, self.range, self.code, self.over = data, 0, 0xFFFFFFFF, 0, 0
        for _ in range(5):
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF

    def _byte(self) -> int:
        if self.pos < len(self.data):
            self.pos += 1
            return self.data[self.pos - 1]
        self.over += 1
        return 0

    def bit(self, probs: list, i: int) -> int:
        p = probs[i]
        bound = (self.range >> 11) * p
        if self.code < bound:
            self.range = bound
            probs[i] = p + ((2048 - p) >> 5)
            b = 0
        else:
            self.code -= bound
            self.range -= bound
            probs[i] = p - (p >> 5)
            b = 1
        while self.range < 0x1000000:
            self.range <<= 8
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
        return b

    def direct(self) -> int:
        self.range >>= 1
        b = 0
        if self.code >= self.range:
            self.code -= self.range
            b = 1
        while self.range < 0x1000000:
            self.range <<= 8
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
        return b

    def eg(self) -> int:
        n = 0
        while self.direct():
            n += 1
            if n > 24:
                self.over += 1000
                return 0
        v = 1
        for _ in range(n):
            v = v << 1 | self.direct()
        return v - 1

    def uint(self, bins: list) -> int:
        i = 0
        while i < _MMC_UBINS and self.bit(bins, i):
            i += 1
        return i if i < _MMC_UBINS else _MMC_UBINS + self.eg()


class _MotionDecoder:
    def __init__(self, width: int, height: int):
        self.w, self.h = (width + 15) & ~15, (height + 15) & ~15
        self.mbw, self.mbh = self.w // 16, self.h // 16
        self.cw, self.ch = self.w // 2, self.h // 2
        size_y, size_c = self.w * self.h, self.cw * self.ch
        self.cur = [bytearray(size_y), bytearray(size_c), bytearray(size_c)]
        self.ref = [bytearray(size_y), bytearray(size_c), bytearray(size_c)]
        mbs = self.mbw * self.mbh
        self.nz_y = bytearray(size_y // 16)
        self.mode = bytearray(size_y // 16)
        self.nz_c = [bytearray(size_c // 16), bytearray(size_c // 16)]
        self.mb_intra = bytearray(mbs)
        self.mb_skip = bytearray(mbs)
        self.mv = [0] * (mbs * 2)
        self.qp = 26

    # Prediction and reconstruction -----------------------------------
    @staticmethod
    def _intra_pred(plane, stride, x, y, n, mode):
        ha, hl = y > 0, x > 0
        above = [plane[(y - 1) * stride + x + i] if ha else 128 for i in range(n)]
        left = [plane[(y + i) * stride + x - 1] if hl else 128 for i in range(n)]
        corner = plane[(y - 1) * stride + x - 1] if ha and hl else 128
        shift = 2 if n == 4 else 3
        dc = 128
        if ha and hl:
            dc = (sum(above) + sum(left) + n) >> (shift + 1)
        elif ha or hl:
            dc = (sum(above if ha else left) + n // 2) >> shift
        if mode == 0:
            return [dc] * (n * n)
        if mode == 1:
            return above * n
        if mode == 2:
            return [left[j] for j in range(n) for _ in range(n)]
        return [_clip(left[j] + above[i] - corner) for j in range(n) for i in range(n)]

    @staticmethod
    def _mc(plane, pw, ph, x, y, n, mvx, mvy, frac):
        one = 1 << frac
        ix0, iy0 = x + (mvx >> frac), y + (mvy >> frac)
        fx, fy = mvx & (one - 1), mvy & (one - 1)
        w00, w01 = (one - fx) * (one - fy), fx * (one - fy)
        w10, w11 = (one - fx) * fy, fx * fy
        sh = 2 * frac
        rnd = 1 << (sh - 1)
        maxx, maxy = pw - 1, ph - 1
        out = [0] * (n * n)
        xs0 = [min(max(ix0 + i, 0), maxx) for i in range(n)]
        xs1 = [min(max(ix0 + i + 1, 0), maxx) for i in range(n)]
        for j in range(n):
            r0 = min(max(iy0 + j, 0), maxy) * pw
            r1 = min(max(iy0 + j + 1, 0), maxy) * pw
            base = j * n
            if not fx and not fy:
                for i in range(n):
                    out[base + i] = plane[r0 + xs0[i]]
            else:
                for i in range(n):
                    a, b = xs0[i], xs1[i]
                    out[base + i] = (w00 * plane[r0 + a] + w01 * plane[r0 + b] +
                                     w10 * plane[r1 + a] + w11 * plane[r1 + b] + rnd) >> sh
        return out

    @staticmethod
    def _put(plane, stride, x, y, n, block):
        for j in range(n):
            at = (y + j) * stride + x
            plane[at:at + n] = bytes(block[j * n:(j + 1) * n])

    @staticmethod
    def _add_residual(z, qp, plane, at, stride):
        scale = 1 << (qp // 6)
        row_v = _MMC_V[qp % 6]
        c = [0] * 16
        for i in range(16):
            if z[i]:
                v = z[i] * row_v[_MMC_CLASS[i]] * scale
                c[i] = -(1 << 20) if v < -(1 << 20) else (1 << 20) if v > (1 << 20) else v
        t = [0] * 16
        for i in range(4):
            d0, d1, d2, d3 = c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]
            e0, e1, e2, e3 = d0 + d2, d0 - d2, (d1 >> 1) - d3, d1 + (d3 >> 1)
            t[i * 4:i * 4 + 4] = (e0 + e3, e1 + e2, e1 - e2, e0 - e3)
        for i in range(4):
            d0, d1, d2, d3 = t[i], t[4 + i], t[8 + i], t[12 + i]
            e0, e1, e2, e3 = d0 + d2, d0 - d2, (d1 >> 1) - d3, d1 + (d3 >> 1)
            for y, r in enumerate((e0 + e3, e1 + e2, e1 - e2, e0 - e3)):
                q = at + y * stride + i
                plane[q] = _clip(plane[q] + ((r + 32) >> 6))

    def _mvp(self, mx, my):
        w, mv, intra = self.mbw, self.mv, self.mb_intra
        a = b = c = (0, 0)
        if mx > 0:
            i = my * w + mx - 1
            if not intra[i]:
                a = (mv[i * 2], mv[i * 2 + 1])
        if my == 0:
            return a
        bi = (my - 1) * w + mx
        if not intra[bi]:
            b = (mv[bi * 2], mv[bi * 2 + 1])
        ci = bi + 1 if mx + 1 < w else bi - 1 if mx > 0 else bi
        if ci != bi and not intra[ci]:
            c = (mv[ci * 2], mv[ci * 2 + 1])
        res = []
        for k in range(2):
            lo, hi = min(a[k], b[k]), max(a[k], b[k])
            res.append(lo if c[k] < lo else hi if c[k] > hi else c[k])
        return res[0], res[1]

    # Entropy decoding ----------------------------------------------------
    def _coeffs(self, d, c, typ, ctx):
        z = [0] * 16
        if not d.bit(c["cbf"][typ], ctx):
            return None
        sig, last_p = c["sig"][typ], c["last"][typ]
        pos = []
        last = 15
        for i in range(15):
            if d.bit(sig, i):
                pos.append(i)
                if d.bit(last_p, i):
                    last = i
                    break
        if last == 15:
            pos.append(15)
        gt1 = eq1 = 0
        g, lev = c["gt1"][typ], c["lev"][typ]
        for k in range(len(pos) - 1, -1, -1):
            inc = 0 if gt1 else min(eq1 + 1, 4)
            a = 1
            if d.bit(g, inc):
                a = min(2 + d.uint(lev), 65535)
                gt1 += 1
            else:
                eq1 += 1
            z[_MMC_ZIGZAG[pos[k]]] = -a if d.direct() else a
        return z

    def _mvd(self, d, c, k):
        if not d.bit(c["mv_zero"], k):
            return 0
        s = d.direct()
        a = min(1 + d.uint(c["mv_bins"][k]), _MMC_MV_MAX * 2)
        return -a if s else a

    def _mpm(self, bx, by):
        bw = self.w // 4
        a = self.mode[by * bw + bx - 1] if bx > 0 else 0
        b = self.mode[(by - 1) * bw + bx] if by > 0 else 0
        return min(a, b)

    def _chroma(self, d, c, mx, my):
        qpc = _MMC_CHROMA_QP[self.qp]
        cw, cbw = self.cw, self.w // 8
        for pl in range(2):
            plane, nz = self.cur[1 + pl], self.nz_c[pl]
            for k in range(4):
                bx, by = mx * 2 + (k & 1), my * 2 + (k >> 1)
                ctx = (bx > 0 and nz[by * cbw + bx - 1]) + (by > 0 and nz[(by - 1) * cbw + bx])
                z = self._coeffs(d, c, 1, ctx)
                nz[by * cbw + bx] = z is not None
                if z is not None:
                    self._add_residual(z, qpc, plane, by * 4 * cw + bx * 4, cw)

    def decode(self, data: bytes, key: bool, qp: int) -> None:
        if not 0 <= qp <= 51:
            raise ValueError("invalid motion frame QP")
        self.qp = qp
        d = _RangeDecoder(data)
        c = {"skip": [1024] * 3, "intra": [1024] * 3, "mpm": [1024], "rem": [1024] * 2,
             "cmode": [1024] * 3, "mv_zero": [1024] * 2,
             "mv_bins": [[1024] * _MMC_UBINS for _ in range(2)],
             "cbf": [[1024] * 3 for _ in range(2)], "sig": [[1024] * 15 for _ in range(2)],
             "last": [[1024] * 15 for _ in range(2)], "gt1": [[1024] * 5 for _ in range(2)],
             "lev": [[1024] * _MMC_UBINS for _ in range(2)]}
        w, cw, bw, cbw = self.w, self.cw, self.w // 4, self.w // 8
        Y, U, V = self.cur
        for my in range(self.mbh):
            for mx in range(self.mbw):
                mb = my * self.mbw + mx
                nskip = (mx > 0 and self.mb_skip[mb - 1]) + (my > 0 and self.mb_skip[mb - self.mbw])
                nintra = (mx > 0 and self.mb_intra[mb - 1]) + (my > 0 and self.mb_intra[mb - self.mbw])
                skip = not key and d.bit(c["skip"], nskip)
                intra = key or (not skip and d.bit(c["intra"], nintra))
                self.mb_skip[mb], self.mb_intra[mb] = skip, intra
                if intra:
                    self.mv[mb * 2] = self.mv[mb * 2 + 1] = 0
                    modes = []
                    for k in range(16):
                        bx, by = mx * 4 + (k & 3), my * 4 + (k >> 2)
                        mpm = mode = self._mpm(bx, by)
                        if d.bit(c["mpm"], 0):
                            rem = 1 + d.bit(c["rem"], 1) if d.bit(c["rem"], 0) else 0
                            mode = rem if rem < mpm else rem + 1
                        self.mode[by * bw + bx] = mode
                        modes.append(mode)
                    b0 = d.bit(c["cmode"], 0)
                    cmode = b0 * 2 + d.bit(c["cmode"], 1 + b0)
                    for k in range(16):
                        bx, by = mx * 4 + (k & 3), my * 4 + (k >> 2)
                        self._put(Y, w, bx * 4, by * 4, 4, self._intra_pred(Y, w, bx * 4, by * 4, 4, modes[k]))
                        ctx = (bx > 0 and self.nz_y[by * bw + bx - 1]) + (by > 0 and self.nz_y[(by - 1) * bw + bx])
                        z = self._coeffs(d, c, 0, ctx)
                        self.nz_y[by * bw + bx] = z is not None
                        if z is not None:
                            self._add_residual(z, qp, Y, by * 4 * w + bx * 4, w)
                    for plane in (U, V):
                        self._put(plane, cw, mx * 8, my * 8, 8, self._intra_pred(plane, cw, mx * 8, my * 8, 8, cmode))
                    self._chroma(d, c, mx, my)
                else:
                    px, py = self._mvp(mx, my)
                    if not skip:
                        px = min(max(px + self._mvd(d, c, 0), -_MMC_MV_MAX), _MMC_MV_MAX)
                        py = min(max(py + self._mvd(d, c, 1), -_MMC_MV_MAX), _MMC_MV_MAX)
                    self.mv[mb * 2], self.mv[mb * 2 + 1] = px, py
                    ry, ru, rv = self.ref
                    self._put(Y, w, mx * 16, my * 16, 16, self._mc(ry, w, self.h, mx * 16, my * 16, 16, px, py, 2))
                    self._put(U, cw, mx * 8, my * 8, 8, self._mc(ru, cw, self.ch, mx * 8, my * 8, 8, px, py, 3))
                    self._put(V, cw, mx * 8, my * 8, 8, self._mc(rv, cw, self.ch, mx * 8, my * 8, 8, px, py, 3))
                    for k in range(16):
                        i = (my * 4 + (k >> 2)) * bw + mx * 4 + (k & 3)
                        self.mode[i] = 0
                        self.nz_y[i] = 0
                    if skip:
                        for k in range(4):
                            i = (my * 2 + (k >> 1)) * cbw + mx * 2 + (k & 1)
                            self.nz_c[0][i] = self.nz_c[1][i] = 0
                    else:
                        for k in range(16):
                            bx, by = mx * 4 + (k & 3), my * 4 + (k >> 2)
                            ctx = (bx > 0 and self.nz_y[by * bw + bx - 1]) + (by > 0 and self.nz_y[(by - 1) * bw + bx])
                            z = self._coeffs(d, c, 0, ctx)
                            self.nz_y[by * bw + bx] = z is not None
                            if z is not None:
                                self._add_residual(z, qp, Y, by * 4 * w + bx * 4, w)
                        self._chroma(d, c, mx, my)
                if d.over > 16:
                    raise ValueError("damaged motion frame")
        self._deblock(Y, w, self.h, self.nz_y, 2, qp)
        qpc = _MMC_CHROMA_QP[qp]
        self._deblock(U, cw, self.ch, self.nz_c[0], 1, qpc)
        self._deblock(V, cw, self.ch, self.nz_c[1], 1, qpc)
        self.cur, self.ref = self.ref, self.cur

    def _deblock(self, plane, pw, ph, nz, mbsh, qp):
        alpha, beta = _MMC_ALPHA[qp], _MMC_BETA[qp]
        if not alpha:
            return
        bw, bh = pw // 4, ph // 4
        intra, mv, mbw = self.mb_intra, self.mv, self.mbw
        for direction in range(2):
            for by in range(direction, bh):
                for bx in range(1 - direction, bw):
                    ax, ay = (bx, by - 1) if direction else (bx - 1, by)
                    mba = (ay >> mbsh) * mbw + (ax >> mbsh)
                    mbb = (by >> mbsh) * mbw + (bx >> mbsh)
                    if intra[mba] or intra[mbb]:
                        s = 2
                    elif nz[ay * bw + ax] or nz[by * bw + bx]:
                        s = 1
                    elif mba != mbb and (abs(mv[mba * 2] - mv[mbb * 2]) >= 4 or
                                         abs(mv[mba * 2 + 1] - mv[mbb * 2 + 1]) >= 4):
                        s = 1
                    else:
                        continue
                    tc = _MMC_TC0[qp] + (s == 2)
                    base = by * 4 * pw + bx * 4
                    step = pw if direction else 1
                    for k in range(4):
                        p = base + k if direction else base + k * pw
                        p1, p0, q0, q1 = plane[p - 2 * step], plane[p - step], plane[p], plane[p + step]
                        if abs(p0 - q0) < alpha and abs(p1 - p0) < beta and abs(q1 - q0) < beta:
                            dl = ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3
                            dl = -tc if dl < -tc else tc if dl > tc else dl
                            plane[p - step] = _clip(p0 + dl)
                            plane[p] = _clip(q0 - dl)

    def to_argb(self, width: int, height: int) -> array:
        Y, U, V = self.ref
        cw, ch = self.cw, self.ch
        out = array("I", [0]) * (width * height)
        for y in range(height):
            cy = y >> 1
            cy2 = min(max(cy + 1 if y & 1 else cy - 1, 0), ch - 1)
            u0, u1, v0, v1 = cy * cw, cy2 * cw, cy * cw, cy2 * cw
            row = y * self.w
            o = y * width
            for x in range(width):
                cx = x >> 1
                cx2 = min(max(cx + 1 if x & 1 else cx - 1, 0), cw - 1)
                u = (9 * U[u0 + cx] + 3 * U[u0 + cx2] + 3 * U[u1 + cx] + U[u1 + cx2] + 8) >> 4
                v = (9 * V[v0 + cx] + 3 * V[v0 + cx2] + 3 * V[v1 + cx] + V[v1 + cx2] + 8) >> 4
                lum, cb, cr = Y[row + x], u - 128, v - 128
                r = _clip(lum + ((359 * cr + 128) >> 8))
                g = _clip(lum - ((88 * cb + 183 * cr + 128) >> 8))
                b = _clip(lum + ((454 * cb + 128) >> 8))
                out[o + x] = 0xFF000000 | r << 16 | g << 8 | b
        return out


class VideoFile:
    def __init__(self, data: bytes):
        if (len(data) < 64 or data[:4] != b"MVID" or data[4] not in (1, 2) or
                data[5] not in (3, 4) or data[6] != 6):
            raise ValueError("not a supported .vid file")
        self.codec = _u32(data, 60) if data[4] == 2 else 0
        if self.codec not in (0, 1, 2) or (self.codec != 0 and data[5] != 3):
            raise ValueError(f"unsupported .vid codec {self.codec}")
        self.codec_name = ("tiles", "motion", "h264")[self.codec]
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
        self._motion = _MotionDecoder(self.width, self.height) if self.codec == 1 else None
        self._h264_first = -1       # h264: the frames in memory, from one keyframe to the next
        self._h264_frames: list[array] = []

    def _decode_one(self, number: int) -> None:
        offset, size, _ = self.index[number]
        frame = self.data[offset:offset + size]
        if self._motion:
            if len(frame) < 5:
                raise ValueError(f"truncated .vid frame {number}")
            key = bool(frame[0] & 1)
            if not key and self._decoded != number - 1:
                raise ValueError(f".vid frame {number} requires its previous frame")
            self._motion.decode(frame[4:], key, frame[1])
            self._pixels = self._motion.to_argb(self.width, self.height)
            self._decoded = number
            return
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

    def _h264_frame(self, number: int) -> array:
        """H.264 frames are decoded by ffmpeg, a group of pictures at a time.

        The C players use Minimal-OS-SDK/include/mosmedia_h264.h; this viewer
        has no H.264 decoder of its own. Colours can differ by a step or two
        from the C decoder's conversion.
        """
        if not self._h264_first <= number < self._h264_first + len(self._h264_frames):
            first = number
            while first and not (self.index[first][2] & 1):
                first -= 1
            last = first + 1
            while last < self.frame_count and not (self.index[last][2] & 1):
                last += 1
            stream = b"".join(self.data[o + 4:o + s] for o, s, _ in self.index[first:last])
            try:
                done = subprocess.run(
                    ["ffmpeg", "-v", "error", "-f", "h264", "-i", "-", "-fps_mode", "passthrough", "-f", "rawvideo",
                     "-pix_fmt", "bgra", "-"],
                    input=stream, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            except OSError as error:
                raise ValueError("H.264 .vid files need ffmpeg in PATH for this player") from error
            size = self.width * self.height * 4
            if done.returncode != 0 or len(done.stdout) < size * (number - first + 1):
                detail = done.stderr.decode("utf-8", "replace").strip().splitlines()
                raise ValueError("ffmpeg could not decode the H.264 frames" +
                                 (f": {detail[-1]}" if detail else ""))
            frames = []
            for at in range(0, len(done.stdout) - size + 1, size):
                pixels = array("I")
                pixels.frombytes(done.stdout[at:at + size])
                if sys.byteorder != "little":
                    pixels.byteswap()
                frames.append(pixels)
            self._h264_first, self._h264_frames = first, frames
        return self._h264_frames[number - self._h264_first]

    def frame(self, number: int) -> array:
        if not 0 <= number < self.frame_count:
            raise ValueError("frame number is outside the video")
        if self.codec == 2:
            return self._h264_frame(number)
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
            self.status.set(f"{self.path}  |  VID {media.codec_name} {media.width} x {media.height}  |  "
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
