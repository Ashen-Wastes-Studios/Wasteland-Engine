#!/usr/bin/env python3
"""Task 5 WLAA measurement: p99 local contrast + mean-abs-diff vs a converged
reference, for the TAA (bilinear) and WLAA (sharpen) arms at RenderScale 1.0.

Stdlib only. Reads the PNGs produced by the A/B harness and prints:
  - p99 local contrast per arm (the plan's acceptance statistic: WLAA must beat
    TAA, since a sharpen raises local contrast where TAA softens it);
  - mean abs diff of each 400-frame arm against the 800-frame converged
    reference (how much each arm still has to converge, plus, for WLAA, the
    sharpen's steady-state perturbation).
"""
import struct
import sys
import zlib
from pathlib import Path


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    width = height = None
    bit_depth = color_type = None
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if bit_depth != 8:
        raise ValueError(f"{path}: only 8-bit PNGs supported")
    raw = zlib.decompress(bytes(idat))
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:    # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, out, channels


def luma(px, channels, stride, x, y):
    i = y * stride + x * channels
    if channels == 1:
        return float(px[i])
    return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]


def local_contrast(path, sample=4):
    width, height, px, channels = read_png(path)
    stride = width * channels
    vals = []
    for y in range(1, height - 1, sample):
        for x in range(1, width - 1, sample):
            c = luma(px, channels, stride, x, y)
            best = 0.0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    d = abs(luma(px, channels, stride, x + dx, y + dy) - c)
                    if d > best:
                        best = d
            vals.append(best)
    vals.sort()
    n = len(vals)
    return {"p50": vals[n // 2], "p99": vals[max(0, int(n * 0.99) - 1)],
            "mean": sum(vals) / n, "n": n}


def mean_abs_diff(a_path, b_path, sample=2):
    aw, ah, apx, ach = read_png(a_path)
    bw, bh, bpx, bch = read_png(b_path)
    if (aw, ah) != (bw, bh):
        raise ValueError(f"{a_path} vs {b_path}: size mismatch")
    stride = aw * ach
    total = 0.0
    n = 0
    for y in range(1, ah - 1, sample):
        for x in range(1, aw - 1, sample):
            la = luma(apx, ach, stride, x, y)
            lb = luma(bpx, bch, stride, x, y)
            total += abs(la - lb)
            n += 1
    return total / n, n


def main():
    ev = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        "docs/CLEANROOM_RECONSTRUCTION_EVIDENCE/task5")
    taa = ev / "taa400.png"
    wlaa = ev / "wlaa400.png"
    ref = ev / "ref800.png"

    print("== p99 local contrast (higher = sharper) ==")
    rows = [("TAA  @400", taa), ("WLAA @400", wlaa), ("REF  @800", ref)]
    c = {}
    for name, p in rows:
        r = local_contrast(p)
        c[name] = r
        print(f"    {name}: p50={r['p50']:.3f}  p99={r['p99']:.3f}  "
              f"mean={r['mean']:.3f}  (n={r['n']})")

    print(f"\n    WLAA - TAA p99: {c['WLAA @400']['p99'] - c['TAA  @400']['p99']:+.3f}")
    win = c['WLAA @400']['p99'] > c['TAA  @400']['p99']
    print(f"    acceptance (WLAA p99 > TAA p99): {'PASS' if win else 'FAIL'}")

    print("\n== mean abs diff vs converged reference (REF @800) ==")
    for name, p in [("TAA @400", taa), ("WLAA @400", wlaa)]:
        mad, n = mean_abs_diff(p, ref)
        print(f"    {name}: mad = {mad:.3f}  over {n} sampled px")

    # And the sharpen's own steady-state magnitude: WLAA minus TAA directly,
    # both at 400 frames -- this is the bias the sharpen adds on top of the
    # shared accumulation, independent of convergence noise.
    mad_tt = mean_abs_diff(taa, wlaa)[0]
    print(f"    WLAA vs TAA (both @400, the sharpen's perturbation): mad = {mad_tt:.3f}")


if __name__ == "__main__":
    main()
