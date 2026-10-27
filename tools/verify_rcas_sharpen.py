#!/usr/bin/env python3
"""WLSR sharpen stage (RCAS, pass 15) verifier.

Two halves, in the order that matters:

1. STATIC. The RCAS stage must exist in BOTH twins with the same shape: five taps
   (centre + four diagonals), a gain derived from the LOCAL CONTRAST, a clamp on
   the gain, and a reactive-mask term. Each of those is a real failure mode that
   has already happened in this shader:
     - five taps, not four: the axis neighbours are already exercised by the
       upscale's own kernel, so using them adds nothing and the sharpen is a
       no-op on exactly the frequencies that matter.
     - no clamp: the gain term is unbounded and a single bright texel drags its
       neighbours past the local range -- a visible halo on a hard edge.
     - no mask term: the stage sharpens disocclusions, where the image is a
       single frame's worth of data, amplifying noise and aliasing.

2. MEASURED. The plan's acceptance is a CONTRAST claim: a scale-0.5 render with
   EASU+RCAS must have a higher p99 local contrast than the same render with EASU
   alone. That is checked on real captures, not on the source text, because a
   correctly-written sharpen that is never dispatched is indistinguishable from an
   inert one by reading code.

Run with no arguments for the static half, or --pngs A B for the measured half.

    python3 tools/verify_rcas_sharpen.py
    python3 tools/verify_rcas_sharpen.py --pngs A.png B.png
    python3 tools/verify_rcas_sharpen.py --self-test
"""

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GLSL = ROOT / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl"
HLSL = ROOT / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"

failures = []
passes = []


def check(ok, label):
    (passes if ok else failures).append(label)
    print(("  PASS  " if ok else "  FAIL  ") + label)


# --------------------------------------------------------------------------
# static half
# --------------------------------------------------------------------------

def rcas_body(text, name):
    """Extract the RCAS helper's body from a shader twin."""
    m = re.search(r"(?:^|\n)[^\n]*\b" + name + r"\s*\([^)]*\)\s*\{", text)
    if not m:
        return None
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[start:i - 1] if depth == 0 else None


def static_half():
    print("== static: RCAS stage in both twins ==")
    try:
        glsl = GLSL.read_text(encoding="utf-8", errors="replace")
        hlsl = HLSL.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        check(False, f"read shader: {e}")
        return

    gfn = rcas_body(glsl, "RunRCAS")
    hfn = rcas_body(hlsl, "RunRCAS")
    # The sharpen math (gain, clamp, unsharp, mask) lives in the image-free core,
    # shared by pass 15 and the WLAA branch. The tap loads live in RunRCAS.
    gcore = rcas_body(glsl, "RunWlsrSharpenCore")
    hcore = rcas_body(hlsl, "RunWlsrSharpenCore")
    check(gfn is not None, "glsl: RunRCAS pass-15 tap loader present")
    check(hfn is not None, "hlsl: RunRCAS pass-15 tap loader present")
    check(gcore is not None, "glsl: RunWlsrSharpenCore helper present")
    check(hcore is not None, "hlsl: RunWlsrSharpenCore helper present")
    if not (gfn and hfn and gcore and hcore):
        return

    # --- five taps, centre + four diagonals (in the pass-15 tap loader) ---
    # The diagonals are the point: the axis neighbours are what the upscale
    # already exercises.
    for twin, body in (("glsl", gfn), ("hlsl", hfn)):
        diags = len(re.findall(r"\(\s*(-?1)\s*,\s*(-?1)\s*\)", body))
        check(diags >= 4,
              f"{twin}: four diagonal taps present ({diags} offsets found)")

    # --- gain derived from local contrast, not a fixed slider ---
    # The gain is the neighbourhood's range (mx - mn). A flat region has it near
    # zero so it gains nothing; an edge has it large so it gains a lot. That is
    # what makes it adaptive rather than a fixed sharpen.
    for twin, body in (("glsl", gcore), ("hlsl", hcore)):
        has_min = bool(re.search(r"\bmin\s*\(", body))
        has_max = bool(re.search(r"\bmax\s*\(", body))
        # The contrast term is `gain = contrast`, where contrast is the
        # neighbourhood's range. A sharpen that uses a fixed gain sharpens flat
        # areas too, which is what makes a sharpen visible as noise rather than
        # as edge detail. Keyed off the gain assignment, not the `mx - mn` text,
        # because contrast is defined on its own line and that text would survive
        # a mutation that only dropped it from the gain.
        has_contrast = bool(re.search(r"gain\s*=\s*contrast", body))
        check(has_min and has_max, f"{twin}: gain uses local min/max of the neighbourhood")
        check(has_contrast, f"{twin}: gain is derived from the contrast, not a fixed gain")

    # --- the gain is clamped ---
    # The clamp is the anti-ringing guarantee: without it the gain is unbounded
    # and a single bright texel drags its neighbours past the local range, which
    # is the visible halo on a hard edge.
    for twin, body in (("glsl", gcore), ("hlsl", hcore)):
        check(bool(re.search(r"clamp\s*\(\s*gain", body)),
              f"{twin}: gain is clamped")

    # --- the reactive mask modulates the gain, and in the right DIRECTION ---
    # Where the mask says the frame cannot be trusted the image is one frame's
    # worth of data, so sharpening must be REDUCED there -- scaled by (1 - mask),
    # never by mask. Getting this backwards is a real bug: it sharpens exactly the
    # pixels that are least reliable. The mask is READ in the pass-15 tap loader
    # (RunRCAS) and the (1 - mask) term is applied in the shared core.
    for twin, fn_body in (("glsl", gfn), ("hlsl", hfn)):
        check(bool(re.search(r"img_Bloom_Temp", fn_body)),
              f"{twin}: reads the reactive mask")
    for twin, core_body in (("glsl", gcore), ("hlsl", hcore)):
        inverted = bool(re.search(r"1\.0?f?\s*-\s*clamp\s*\(\s*mask", core_body))
        check(inverted, f"{twin}: mask REDUCES the gain (1 - mask, not mask)")

    # --- the sharpen is an unsharp mask: ctr pushed away from its neighbours ---
    # The centre is moved AWAY from the average of its diagonal neighbours, which
    # is what raises local contrast. The earlier weighted-average form moved it
    # TOWARD the neighbours and lowered contrast -- a blur, not a sharpen.
    for twin, body in (("glsl", gcore), ("hlsl", hcore)):
        check(bool(re.search(r"ctr\s*\+\s*gain\s*\*\s*\(ctr\s*-\s*blur\)", body)),
              f"{twin}: unsharp form (ctr + gain*(ctr - blur))")

    # --- the result is clamped to [0, 1] ---
    # The unsharp push can take a texel past the range on a strong edge; the clamp
    # is the second half of the anti-ringing guarantee, after the gain clamp.
    for twin, body in (("glsl", gcore), ("hlsl", hcore)):
        check(bool(re.search(r"return\s+clamp\s*\(\s*result", body)),
              f"{twin}: result is clamped to the range")

    # --- the pass is wired, in both twins and on the dispatch path ---
    for twin, path in (("glsl", glsl), ("hlsl", hlsl)):
        check(bool(re.search(r"case\s+15\s*:\s*RunRCAS\s*\(\s*\)", path)),
              f"{twin}: pass 15 dispatches RunRCAS")

    # --- the gate ---
    # The stage must default inert: a sharpen stage that runs unconditionally is a
    # quality change nobody asked for.
    for twin, path in (("glsl", glsl), ("hlsl", hlsl)):
        check(bool(re.search(r"u_RCSEnabled", path)),
              f"{twin}: u_RCSEnabled gate present")

    # --- no leftover TSR resolve on pass 15 ---
    # The TSR resolve was the previous occupant. If it is still reachable from
    # pass 15 the two stages are wired to the same pass ID and whichever is
    # dispatched last wins -- a silent, order-dependent bug.
    check("case 15: RunTSRResolve" not in glsl and "case 15: RunTSRResolve" not in hlsl,
          "pass 15 is RCAS in both twins (no TSR resolve left on the same ID)")


# --------------------------------------------------------------------------
# measured half
# --------------------------------------------------------------------------

def read_png(path):
    """Minimal stdlib PNG reader: returns (width, height, pixels, channels)."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    width = height = None
    bit_depth = color_type = None
    idat = bytearray()
    palette = None
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"PLTE":
            palette = chunk
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length

    if bit_depth != 8:
        raise ValueError(f"{path}: only 8-bit PNGs supported, got {bit_depth}")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"{path}: unsupported colour type {color_type}")

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
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

    if palette:
        px = bytearray(width * height * 3)
        for i in range(width * height):
            j = out[i] * 3
            px[i * 3:i * 3 + 3] = palette[j:j + 3]
        return width, height, px, 3
    return width, height, out, channels


def luma_at(px, channels, stride, x, y):
    i = y * stride + x * channels
    if channels == 1:
        return float(px[i])
    return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]


def local_contrast(path, sample=4):
    """p99 of the 3x3 local contrast, sampled for speed.

    Local contrast here is the max absolute difference between a texel and its
    3x3 neighbourhood. Sharpening raises exactly that statistic, so a sharpen stage
    that is inert shows no change in it -- which is how a correctly-written-but-
    never-dispatched pass gets caught.
    """
    width, height, px, channels = read_png(path)
    stride = width * channels
    vals = []
    step = sample
    for y in range(1, height - 1, step):
        for x in range(1, width - 1, step):
            c = luma_at(px, channels, stride, x, y)
            best = 0.0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    d = abs(luma_at(px, channels, stride, x + dx, y + dy) - c)
                    if d > best:
                        best = d
            vals.append(best)
    if not vals:
        return None
    vals.sort()
    n = len(vals)
    return {
        "p50": vals[n // 2],
        "p99": vals[max(0, int(n * 0.99) - 1)],
        "mean": sum(vals) / n,
        "n": n,
    }


def measured_half(a_path, b_path):
    print(f"== measured: {a_path} vs {b_path} ==")
    try:
        a = local_contrast(a_path)
        b = local_contrast(b_path)
    except Exception as e:
        check(False, f"read PNGs: {e}")
        return
    if not a or not b:
        check(False, "empty capture")
        return

    print(f"    A  p50={a['p50']:.2f}  p99={a['p99']:.2f}  mean={a['mean']:.2f}  (n={a['n']})")
    print(f"    B  p50={b['p50']:.2f}  p99={b['p99']:.2f}  mean={b['mean']:.2f}  (n={b['n']})")

    # The plan's acceptance: EASU+RCAS must have a HIGHER p99 contrast than EASU
    # alone. The direction matters more than the magnitude -- a sharpen that
    # lowers p99 contrast is a blur, not a sharpen.
    check(b["p99"] > a["p99"], f"p99 contrast rises ({a['p99']:.2f} -> {b['p99']:.2f})")

    # And it must not be a no-op: an unchanged p99 means the stage is inert,
    # which is the failure this whole file exists to catch.
    check(abs(b["p99"] - a["p99"]) > 0.05,
          f"change is measurable, not noise ({abs(b['p99'] - a['p99']):.2f})")


# --------------------------------------------------------------------------
# negative controls
# --------------------------------------------------------------------------

def self_test():
    """Prove each check can fail. A verifier that cannot fail is decoration."""
    import tempfile
    import shutil

    print("== self-test: each check must fail on a broken mutant ==")
    try:
        orig_g = GLSL.read_text(encoding="utf-8", errors="replace")
        orig_h = HLSL.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"  FAIL  cannot read shaders: {e}")
        return False

    tmp = Path(tempfile.mkdtemp(prefix="rcas_self_"))
    ok_all = True

    def run(label, glsl_text, hlsl_text):
        nonlocal ok_all
        tmp_g = tmp / "a.glsl"
        tmp_h = tmp / "a.hlsl"
        tmp_g.write_text(glsl_text, encoding="utf-8")
        tmp_h.write_text(hlsl_text, encoding="utf-8")
        global GLSL, HLSL
        real_g, real_h = GLSL, HLSL
        GLSL, HLSL = tmp_g, tmp_h
        try:
            before = len(failures)
            static_half()
            caught = len(failures) > before
        finally:
            GLSL, HLSL = real_g, real_h
        print(f"  {'ok  ' if caught else 'MISS'}  {label}")
        if not caught:
            ok_all = False
        del failures[before:]

    # 1. the clamp is removed
    run("gain clamp removed",
        orig_g.replace("    gain = clamp(gain, 0.0f, RCAS_MAX_SHARPEN);", "    gain = gain;"),
        orig_h.replace("    gain = clamp(gain, 0.0f, RCAS_MAX_SHARPEN);", "    gain = gain;"))

    # 2. the mask is applied in the WRONG DIRECTION -- the bug this file was
    #    written to catch. Sharpening more where the frame is least reliable.
    run("mask direction inverted (sharpens disocclusions)",
        orig_g.replace("(1.0f - clamp(mask, 0.0f, 1.0f))", "clamp(mask, 0.0f, 1.0f)"),
        orig_h.replace("(1.0f - clamp(mask, 0.0f, 1.0f))", "clamp(mask, 0.0f, 1.0f)"))

    # 3. the contrast term is dropped, so the gain is a fixed slider. A fixed
    #    gain sharpens flat areas too, which is what makes a sharpen visible as
    #    noise rather than as edge detail.
    run("contrast term dropped (fixed gain)",
        orig_g.replace("vec3 gain = contrast * clamp(sharpness, 0.0f, 1.0f) * (1.0f - clamp(mask, 0.0f, 1.0f));",
                       "vec3 gain = 1.0f * clamp(sharpness, 0.0f, 1.0f) * (1.0f - clamp(mask, 0.0f, 1.0f));"),
        orig_h.replace("float3 gain = contrast * clamp(sharpness, 0.0f, 1.0f) * (1.0f - clamp(mask, 0.0f, 1.0f));",
                       "float3 gain = 1.0f * clamp(sharpness, 0.0f, 1.0f) * (1.0f - clamp(mask, 0.0f, 1.0f));"))

    # 4. the stage is never wired to a pass
    run("pass 15 arm removed",
        orig_g.replace("case 15: RunRCAS(); break;", "case 15: break;"),
        orig_h.replace("case 15: RunRCAS(); break;", "case 15: break;"))

    # 5. the result is not clamped, so a strong edge can push a texel out of range
    run("result clamp removed",
        orig_g.replace("return clamp(result, 0.0f, 1.0f);", "return result;"),
        orig_h.replace("return clamp(result, 0.0f, 1.0f);", "return result;"))

    # 6. the unsharp form is broken -- the centre is added to the gain instead of
    #    pushed away from the blur, so the filter no longer raises contrast.
    run("unsharp form broken (ctr + gain*blur)",
        orig_g.replace("vec3 result = ctr + gain * (ctr - blur);",
                       "vec3 result = ctr + gain * blur;"),
        orig_h.replace("float3 result = ctr + gain * (ctr - blur);",
                       "float3 result = ctr + gain * blur;"))

    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nRESULT: {'PASS' if ok_all else 'FAIL'} -- "
          f"{'all six RCAS mutations were detected' if ok_all else 'some mutants were NOT detected'}")
    return ok_all


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pngs", nargs=2, metavar=("A", "B"),
                    help="A = no RCAS, B = with RCAS; checks the contrast claim")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return 0 if self_test() else 1

    static_half()
    if args.pngs:
        measured_half(*args.pngs)

    total = len(passes) + len(failures)
    print(f"\n{len(passes)}/{total} checks passed")
    if failures:
        print("\nFAILED:")
        for f in failures:
            print("  - " + f)
        return 1
    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
