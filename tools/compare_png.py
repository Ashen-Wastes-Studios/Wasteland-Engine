#!/usr/bin/env python3
"""Compare two PNGs pixel-for-pixel, stdlib only.

WHY THIS EXISTS
---------------
Task 2's acceptance criterion is that pass 24 changes NOTHING visible: it writes
the reactive mask into the alpha channel of img_Bloom_Temp, and nothing in the
shader reads that alpha. So a run with WL_RECONSTRUCTION=1 must produce a frame
bit-identical to a run with WL_RECONSTRUCTION=0. Any pixel difference is a bug --
either the pass wrote to a channel something reads, or it touched a channel the
composite consumes.

The obvious way to check this is Pillow + numpy, and both are absent from the
interpreter this repo's verification tools run under. Rather than add a
dependency to the verification path, this uses the standard library: a PNG is
zlib-compressed scanlines, and the only hard part is un-filtering, which is
forty lines. tools/train_neural/dataset.py uses Pillow and needs numpy; this tool
does not, because it only needs a difference count, not a tensor.

USAGE
-----
    python3 tools/compare_png.py A.png B.png [--tolerance N] [--report PATH]

Exit codes: 0 identical (within tolerance), 1 different, 2 unusable input.

`--tolerance` is in 8-bit units and defaults to 0, because the acceptance is
BIT-identical, not "close". A tolerance above zero is for the cases where a
deterministic-but-not-bit-identical comparison is what is actually wanted --
e.g. across a driver update -- and those cases should say so in the report.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

FILTER_NONE = 0
FILTER_SUB = 1
FILTER_UP = 2
FILTER_AVERAGE = 3
FILTER_PAETH = 4


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path: Path) -> tuple[int, int, int, bytes]:
    """Return (width, height, channels, raw_rgb_bytes).

    Only 8-bit RGB and RGBA are supported, which is what this engine writes.
    Anything else raises -- a verifier that silently mis-reads a PNG would
    report a match for two different images, which is worse than failing.
    """
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")

    pos = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # length + type + data + CRC

        if ctype == b"IHDR":
            (width, height, bit_depth, color_type, _comp, _filt, _inter) = \
                struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break

    if width is None or bit_depth != 8:
        raise ValueError(f"{path}: unsupported (bit_depth={bit_depth})")

    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"{path}: unsupported color_type={color_type}")

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 3)

    prev = bytearray(stride)
    src = 0
    for y in range(height):
        ftype = raw[src]
        src += 1
        line = bytearray(raw[src:src + stride])
        src += stride

        if ftype == FILTER_NONE:
            pass
        elif ftype == FILTER_SUB:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ftype == FILTER_UP:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == FILTER_AVERAGE:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == FILTER_PAETH:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                c = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        else:
            raise ValueError(f"{path}: unknown filter {ftype} on row {y}")

        # Normalise to RGB, dropping alpha. Both captures are RGB, but the
        # conversion is what makes an RGBA capture comparable rather than
        # silently mis-strided.
        for x in range(width):
            o = x * channels
            d = (y * width + x) * 3
            out[d] = line[o]
            out[d + 1] = line[o + 1] if channels >= 3 else line[o]
            out[d + 2] = line[o + 2] if channels >= 3 else line[o]
        prev = line

    return width, height, 3, bytes(out)


def compare(pa: Path, pb: Path, tolerance: int = 0) -> dict:
    wa, ha, ca, da = read_png(pa)
    wb, hb, cb, db = read_png(pb)
    if (wa, ha) != (wb, hb):
        return {"error": f"dimension mismatch: {wa}x{ha} vs {wb}x{hb}"}

    n = len(da)
    diff = 0
    max_delta = 0
    sum_delta = 0
    # Bounding box of the differing region, so a failure says WHERE rather than
    # only how much. A single-pixel difference in a corner and a uniform shift
    # have the same count and very different meanings.
    min_x = min_y = 1 << 30
    max_x = max_y = -1

    for i in range(0, n, 3):
        d = 0
        for k in range(3):
            delta = abs(da[i + k] - db[i + k])
            if delta > d:
                d = delta
        if d > tolerance:
            diff += 1
            sum_delta += d
            if d > max_delta:
                max_delta = d
            px = (i // 3) % wa
            py = (i // 3) // wa
            if px < min_x:
                min_x = px
            if px > max_x:
                max_x = px
            if py < min_y:
                min_y = py
            if py > max_y:
                max_y = py

    total = wa * ha
    return {
        "width": wa, "height": ha, "pixels": total,
        "differing": diff,
        "fraction": diff / total if total else 0.0,
        "max_delta": max_delta,
        "mean_delta": (sum_delta / diff) if diff else 0.0,
        "bbox": None if not diff else (min_x, min_y, max_x, max_y),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--tolerance", type=int, default=0,
                    help="per-channel 8-bit tolerance (default 0 = bit-identical)")
    ap.add_argument("--report", type=Path, default=None)
    args = ap.parse_args()

    if not args.a.exists() or not args.b.exists():
        print(f"FAIL missing input: {args.a} / {args.b}")
        return 2

    try:
        r = compare(args.a, args.b, args.tolerance)
    except Exception as e:
        print(f"FAIL could not read PNG: {e}")
        return 2

    if "error" in r:
        print(f"FAIL {r['error']}")
        return 1

    lines = [
        f"{r['width']}x{r['height']}  {r['pixels']} pixels",
        f"tolerance      : {args.tolerance} (8-bit units per channel)",
        f"differing      : {r['differing']} ({r['fraction']*100:.6f}%)",
        f"max delta      : {r['max_delta']}",
        f"mean delta     : {r['mean_delta']:.4f}",
    ]
    if r["bbox"]:
        lines.append(f"bbox           : x {r['bbox'][0]}..{r['bbox'][2]}, "
                     f"y {r['bbox'][1]}..{r['bbox'][3]}")
    report = "\n".join(lines)
    print(report)

    if args.report:
        args.report.write_text(report + "\n", encoding="utf-8")

    # Bit-identical is the acceptance; anything else is a failure so the exit
    # code can gate a commit.
    if r["differing"] == 0:
        print("\nRESULT: IDENTICAL")
        return 0
    print(f"\nRESULT: DIFFERENT ({r['differing']} pixels)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
