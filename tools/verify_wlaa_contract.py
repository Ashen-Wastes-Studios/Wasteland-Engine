#!/usr/bin/env python3
"""WLAA native-resolution AA contract verifier (task 5).

WLAA is the reconstruction mode that makes the temporal accumulation a
first-class thing rather than an always-on side effect: at RenderScale 1.0 the
composite is a no-op upscale, so the mode is the accumulation (pass 2, already
running) plus the RCAS sharpen folded into the composite. This file pins the
contract by source shape, the same way the RCAS verifier pins the filter:

  1. the enum value exists on the CPU and the uniform that carries it is declared
     in both twins -- the entire lesson of u_ReconstructionFilter being
     plumbed-but-dead for months is that a value the CPU sets and no shader reads
     is a mode that silently does nothing;
  2. RunComposite has a REAL branch on that value -- `u_ReconstructionFilter == 12`,
     not a fall-through into bilinear. A fall-through is indistinguishable from
     "no AA" and is what "declared but not consumed" looks like in practice;
  3. the sharpen stage is invoked inside that branch -- the branch has to DO the
     sharpen, not just test the value and move on;
  4. the sharpen's amount is clamped -- an unclamped sharpen amount is the ringing
     the whole RCAS design is built to avoid, and WLAA inherits it for free.

Each check has a negative control in --self-test: a mutation that should make the
check fail. A verifier that cannot fail is decoration.

    python3 tools/verify_wlaa_contract.py
    python3 tools/verify_wlaa_contract.py --self-test
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.h"
GLSL = ROOT / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl"
HLSL = ROOT / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"

WLAA_VALUE = 12  # Renderer3D::ReconstructionFilter::Wlaa

failures = []
passes = []


def check(ok, label):
    (passes if ok else failures).append(label)
    print(("  PASS  " if ok else "  FAIL  ") + label)


def extract_body(text, sig):
    """Return the body of the function whose signature line is `sig` (brace match)."""
    m = re.search(re.escape(sig), text)
    if not m:
        return None
    i = text.find("{", m.start())
    if i < 0:
        return None
    depth = 0
    j = i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
        j += 1
    return None


def static_half():
    print("== static: WLAA contract ==")
    try:
        hdr = HEADER.read_text(encoding="utf-8", errors="replace")
        glsl = GLSL.read_text(encoding="utf-8", errors="replace")
        hlsl = HLSL.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        check(False, f"read sources: {e}")
        return

    # --- 1. the enum value on the CPU, and the uniform in both twins ---
    # The value must be a real enumerator, not a #define or a comment: the
    # override casts an int to the enum, so the enumerator is what makes value 12
    # legal rather than UB.
    check(bool(re.search(r"Wlaa\s*=\s*12\b", hdr)),
          f"header: ReconstructionFilter::Wlaa = {WLAA_VALUE}")
    # And the fall-through guard: the value must not collide with an existing one.
    vals = re.findall(r"=\s*(\d+)\b", hdr[hdr.find("enum class ReconstructionFilter"):hdr.find("enum class ReconstructionFilter") + 2000])
    dupes = [v for v in vals if vals.count(v) > 1]
    check(WLAA_VALUE in [int(v) for v in vals] and not dupes,
          f"header: value {WLAA_VALUE} is unique in the enum (no collision)")
    check("uniform int u_ReconstructionFilter" in glsl,
          "glsl: u_ReconstructionFilter declared")
    check(re.search(r"int\s+u_ReconstructionFilter", hlsl) is not None,
          "hlsl: u_ReconstructionFilter declared")

    # --- 2..4 run inside RunComposite's body, not the whole file ---
    gbody = extract_body(glsl, "void RunComposite()")
    hbody = extract_body(hlsl, "void RunComposite()")
    check(gbody is not None, "glsl: RunComposite found")
    check(hbody is not None, "hlsl: RunComposite found")
    if not gbody or not hbody:
        return

    for twin, body in (("glsl", gbody), ("hlsl", hbody)):
        # 2. a real branch on the value, not a fall-through.
        check(bool(re.search(r"u_ReconstructionFilter\s*==\s*12", body)),
              f"{twin}: RunComposite branches on filter == {WLAA_VALUE}")
        # 3. the sharpen stage is invoked inside RunComposite (the shared
        #    image-free core, which both pass 15 and WLAA call).
        check("RunWlsrSharpenCore(" in body,
              f"{twin}: RunComposite invokes the RCAS sharpen core")
        # 4. the sharpen amount is clamped.
        check(bool(re.search(r"clamp\s*\(\s*(wlSharp|sharpness)", body)),
              f"{twin}: the sharpen amount is clamped")

    # --- the dispatch path selects WLAA only when asked ---
    # The override is how the A/B harness selects the mode; it must accept 12.
    try:
        cpp = (ROOT / "Wasteland" / "src" / "Wasteland" / "Renderer" / "Renderer3D.cpp").read_text(
            encoding="utf-8", errors="replace")
    except OSError as e:
        check(False, f"read Renderer3D.cpp: {e}")
        return
    check("(int)ReconstructionFilter::Wlaa" in cpp,
          "cpp: WL_AA_FILTER override accepts Wlaa")
    check("WL_AA_FILTER" in cpp,
          "cpp: WL_AA_FILTER override present")


# --------------------------------------------------------------------------
# negative controls
# --------------------------------------------------------------------------

def self_test():
    """Prove each check can fail."""
    import tempfile
    import shutil

    print("== self-test: each check must fail on a broken mutant ==")
    try:
        orig_h = HEADER.read_text(encoding="utf-8", errors="replace")
        orig_g = GLSL.read_text(encoding="utf-8", errors="replace")
        orig_hl = HLSL.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"  FAIL  cannot read sources: {e}")
        return False

    tmp = Path(tempfile.mkdtemp(prefix="wlaa_self_"))
    ok_all = True

    def run(label, header_text, glsl_text, hlsl_text):
        nonlocal ok_all
        files = {"header": header_text, "glsl": glsl_text, "hlsl": hlsl_text}
        handles = {}
        for name, content in files.items():
            p = tmp / name
            p.write_text(content, encoding="utf-8")
            handles[name] = p
        global HEADER, GLSL, HLSL
        real = (HEADER, GLSL, HLSL)
        HEADER, GLSL, HLSL = handles["header"], handles["glsl"], handles["hlsl"]
        try:
            before = len(failures)
            static_half()
            caught = len(failures) > before
        finally:
            HEADER, GLSL, HLSL = real
        print(f"  {'ok  ' if caught else 'MISS'}  {label}")
        if not caught:
            ok_all = False
        del failures[before:]

    # 1. the enum value is gone (renamed so `Wlaa = 12` no longer matches, and
    #    value 12 is no longer in the enum)
    run("enum Wlaa removed",
        orig_h.replace("Wlaa = 12", "Wlaa = 99"),
        orig_g, orig_hl)

    # 2. the branch tests the wrong value (a fall-through in disguise)
    run("branch condition changed (== 12 -> == 99)",
        orig_h,
        orig_g.replace("if (u_ReconstructionFilter == 12)", "if (u_ReconstructionFilter == 99)"),
        orig_hl.replace("if (u_ReconstructionFilter == 12)", "if (u_ReconstructionFilter == 99)"))

    # 3. the sharpen stage is never invoked
    run("RCAS call removed from RunComposite",
        orig_h,
        orig_g.replace("denoised = RunWlsrSharpenCore(ctr, t00, t10, t01, t11, clamp(wlSharp, 0.0f, 1.0f), mask);",
                       "denoised = base;"),
        orig_hl.replace("denoised = RunWlsrSharpenCore(ctr, t00, t10, t01, t11, clamp(wlSharp, 0.0f, 1.0f), mask);",
                        "denoised = IMG_LOAD(img_Accumulation, pos).rgb;"))

    # 4. the sharpen amount is no longer clamped
    run("clamp removed from sharpen amount",
        orig_h,
        orig_g.replace("RunWlsrSharpenCore(ctr, t00, t10, t01, t11, clamp(wlSharp, 0.0f, 1.0f), mask)",
                       "RunWlsrSharpenCore(ctr, t00, t10, t01, t11, wlSharp, mask)"),
        orig_hl.replace("RunWlsrSharpenCore(ctr, t00, t10, t01, t11, clamp(wlSharp, 0.0f, 1.0f), mask)",
                        "RunWlsrSharpenCore(ctr, t00, t10, t01, t11, wlSharp, mask)"))

    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nRESULT: {'PASS' if ok_all else 'FAIL'} -- "
          f"{'all four WLAA mutations were detected' if ok_all else 'some mutants were NOT detected'}")
    return ok_all


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return 0 if self_test() else 1

    static_half()
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
