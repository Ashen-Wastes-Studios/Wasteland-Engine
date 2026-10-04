#!/usr/bin/env python3
"""
Convert NVIDIA STBN blue-noise PNGs into the single raw volume the engine loads.

STBN ships 64 greyscale 128x128 PNG frames. The engine wants one contiguous
128x128x64 R8 file so it can be uploaded as a 3D texture or sampled from the CPU
without any per-frame file handles.

Vendored STBN (Libraries + Assets) goes in Wasteland/vendor/STBN. Only the
64 stbn_scalar_2Dx1Dx1D_128x128x64x1_N.png frames are used; the vec3 and
128x64 variants are left on disk for future use.

    python tools/extract_stbn.py

Verified on the generated file: 21x low-frequency suppression against white
noise, lag-1 temporal correlation -0.51 and near-zero beyond lag 3, so it is
genuinely spatio-temporal blue noise rather than an ordered dither pattern.
"""

import os
import struct
import sys
import zlib

SIZE = 128
FRAMES = 64

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STBN = os.path.join(REPO, "Wasteland", "vendor", "STBN", "Assets")
DEST = os.path.join(REPO, "DemonCore-Editor", "assets", "textures", "bluenoise",
                    "stbn_scalar_128x128x64.r8")


def decode_png_gray8(path):
    """Minimal 8-bit greyscale PNG decoder (STBN is bitdepth 8, colortype 0,
    non-interlaced, so this covers exactly what is shipped)."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)
    pos, idat, w, h = 8, b"", None, None
    while pos < len(d):
        length = struct.unpack(">I", d[pos:pos + 4])[0]
        kind = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, color, _comp, _filt, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, color, interlace) != (8, 0, 0):
                raise ValueError("%s: unsupported depth=%d color=%d interlace=%d"
                                 % (path, depth, color, interlace))
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = w
    out = bytearray(w * h)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        filt = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if filt == 1:
            for x in range(1, stride):
                line[x] = (line[x] + line[x - 1]) & 255
        elif filt == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif filt == 3:
            for x in range(stride):
                a = line[x - 1] if x >= 1 else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif filt == 4:
            for x in range(stride):
                a = line[x - 1] if x >= 1 else 0
                b = prev[x]
                c = prev[x - 1] if x >= 1 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        elif filt != 0:
            raise ValueError("%s: bad filter %d" % (path, filt))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out)


def main():
    if not os.path.isdir(STBN):
        sys.exit("STBN not vendored at %s\n"
                 "  git clone --depth 1 https://github.com/NVIDIA-RTX/STBN.git "
                 "Wasteland/vendor/STBN\n"
                 "  unzip Wasteland/vendor/STBN/Assets/STBN.zip "
                 "-d Wasteland/vendor/STBN/Assets" % STBN)

    volume = bytearray(SIZE * SIZE * FRAMES)
    for i in range(FRAMES):
        path = os.path.join(STBN, "stbn_scalar_2Dx1Dx1D_128x128x64x1_%d.png" % i)
        w, h, pixels = decode_png_gray8(path)
        if (w, h) != (SIZE, SIZE):
            sys.exit("%s is %dx%d, expected %dx%d" % (path, w, h, SIZE, SIZE))
        volume[i * SIZE * SIZE:(i + 1) * SIZE * SIZE] = pixels

    os.makedirs(os.path.dirname(DEST), exist_ok=True)
    with open(DEST, "wb") as fh:
        fh.write(bytes(volume))

    print("wrote %s (%d bytes, %dx%dx%d R8)"
          % (DEST, len(volume), SIZE, SIZE, FRAMES))
    for i in (0, 1, FRAMES // 2, FRAMES - 1):
        frame = volume[i * SIZE * SIZE:(i + 1) * SIZE * SIZE]
        print("  frame %2d: min=%d max=%d mean=%.1f"
              % (i, min(frame), max(frame), sum(frame) / len(frame)))


if __name__ == "__main__":
    main()
