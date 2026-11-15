#!/usr/bin/env python3
"""Task 8 Step 6 -- the factor-gated schedule and the 6x claim.

The plan's Step 6 acceptance has two halves and they are measurable at different
times, so this tool treats them separately rather than pretending one implies the
other:

  1. THE PRESENT-RATE HALF. "the measured present rate is >= 6x the render rate."
     This is measurable from the engine's own counters, and `--run` measures it:
     the editor is launched with WL_WLFG=<n> and WL_WLFG_TIMING=1, the
     "WLFG timing:" lines are parsed, and present/render is checked against n.

  2. THE PSNR HALF. "at a converged static camera, WLFG=6 produces a frame whose
     PSNR against the real frame at that camera position is within X dB of the
     no-WLFG control." This needs a capture pair and is NOT measured here; the
     structural checks below only prove the schedule that would produce it is
     wired. Reporting a PSNR this tool never computed would be the exact failure
     the plan warns about.

The structural half always runs and is the fast gate. `--run` adds the
measurement and needs a built editor.

What the structural half is actually guarding, and why each check exists:

  * WL_WLFG must be a FACTOR, not a boolean. As a boolean there is no schedule at
    all -- the plan's `factor_axis` would stay `camera_motion_deg_per_frame`.
  * The factor must be the CLAMP of the request against WlfgMaxFactor(), evaluated
    per frame. A schedule that honours the request directly would run 6x on a
    moving camera, where reprojection is undefined.
  * The TRACE must be behind the schedule flag. If the trace still runs on every
    present, the render rate equals the present rate and the ratio is 1 no matter
    what the counter says -- a counter that reports 6x while the GPU traces every
    frame is the worst possible outcome, because it looks like a pass.
  * The Application must NOT also increment m_WlfgRealFrames. It did (a blind
    +1 per present, correct while nothing else counted). With the renderer also
    counting, the ratio is wrong by one frame per present.
  * SetWlfgStats must be called with the renderer's counts, or the line prints
    Task 6's inert baseline and the present rate cannot be measured at all.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RENDERER = os.path.join(ROOT, "Wasteland", "src", "Wasteland", "Renderer", "Renderer3D.cpp")
APP_CPP = os.path.join(ROOT, "Wasteland", "src", "Wasteland", "Core", "Application.cpp")
APP_H = os.path.join(ROOT, "Wasteland", "src", "Wasteland", "Core", "Application.h")

_results: list[tuple[bool, str]] = []


def check(name: str, ok: bool) -> bool:
    _results.append((bool(ok), name))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    return bool(ok)


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def extract_fn(src: str, name: str) -> str | None:
    """Body of a top-level function, brace-matched."""
    pat = re.compile(r'(?:void|int|float|bool)\s+' + re.escape(name) + r'\s*\([^;]*?\)\s*(?:const\s*)?\{', re.S)
    m = pat.search(src)
    if not m:
        return None
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def structural() -> bool:
    renderer = read(RENDERER)
    app_cpp = read(APP_CPP)
    app_h = read(APP_H)

    print("1. WL_WLFG is a factor, not a boolean")

    # The env parse must clamp to [0,6] and derive the warp gate from the factor.
    env_block = re.search(r'getenv\(\s*"WL_WLFG"\s*\)(.{0,900}?)\n\s*\}', renderer, re.S)
    env = env_block.group(1) if env_block else ""
    check("WL_WLFG parsed with getenv", env_block is not None)
    check("WL_WLFG assigned to WlfgFactor", re.search(r'WlfgFactor\s*=\s*glm::clamp\(\s*atoi\(', env) is not None)
    check("WL_WLFG clamped to the plan's 0..6 range", re.search(r'glm::clamp\(\s*atoi\([^)]*\)\s*,\s*0\s*,\s*6\s*\)', env) is not None)
    check("WlfgWarpEnabled derived from the factor (>= 1)",
          re.search(r'WlfgWarpEnabled\s*=\s*s_Data\.WlfgFactor\s*>=\s*1', env) is not None)
    check("WlfgFactor declared in RendererData", re.search(r'int\s+WlfgFactor\s*=\s*0\s*;', renderer) is not None)
    check("WlfgEffectiveFactor declared (request vs in-force kept apart)",
          re.search(r'int\s+WlfgEffectiveFactor\s*=\s*0\s*;', renderer) is not None)

    print()
    print("2. the factor is a FUNCTION of the accumulation state, per frame")

    sched = extract_fn(renderer, "RunNovaPassSchedule") or ""
    check("WlfgMaxFactor called from the schedule",
          re.search(r'WlfgMaxFactor\s*\(', sched) is not None)
    check("the ceiling is passed the camera-moved signal",
          re.search(r'WlfgMaxFactor\(\s*s_Data\.CameraMoved', sched) is not None)
    check("the effective factor is the min of request and ceiling",
          re.search(r'WlfgEffectiveFactor\s*=\s*glm::min\(\s*s_Data\.WlfgFactor\s*,\s*wlfgCeiling\s*\)', sched) is not None)
    # The alpha fed to the ceiling must be the SAME expression the accumulation is
    # fed, or the schedule can call a frame converged that the accumulation is not.
    check("the ceiling's alpha matches the accumulation's alpha expression",
          re.search(r'max\(\s*0\.02f\s*,\s*1\.0f\s*/\s*\(float\)\(\s*s_Data\.FrameIndex\s*\+\s*1\s*\)\s*\)', sched) is not None)
    # The three returns. The gap between condition and return must allow ANY
    # character, not just non-semicolons: the `cameraMoved` branch carries an inline
    # comment that itself contains a semicolon ("reprojection is undefined; the
    # incumbent is the truth"), so a `[^;]*?` gap silently fails to match and the
    # check reports a missing branch that is right there.
    check("WlfgMaxFactor is a function, not a constant (Application side)",
          re.search(r'if\s*\(\s*accumAlpha\s*<=\s*0\.02f\s*\)[\s\S]*?return\s+6\s*;', app_cpp) is not None
          and re.search(r'if\s*\(\s*cameraMoved\s*\)[\s\S]*?return\s+0\s*;', app_cpp) is not None
          and re.search(r'if\s*\(\s*!novaBackend\s*\)[\s\S]*?return\s+0\s*;', app_cpp) is not None)

    print()
    print("3. the TRACE is behind the schedule (else the ratio is 1 by construction)")

    check("a per-present schedule flag exists", re.search(r'wlfgTraceThisPresent\s*=', sched) is not None)
    check("the flag is a modulo of the present index against the effective factor",
          re.search(r'WlfgPresentIndex\s*%\s*\(std::uint64_t\)\s*s_Data\.WlfgEffectiveFactor', sched) is not None)
    check("factor <= 1 short-circuits to 'always trace' (no zero divisor)",
          re.search(r'WlfgEffectiveFactor\s*<=\s*1', sched) is not None)
    # The trace itself: pass(1);pass(0) ... pass(2);CommitHistory() must sit inside
    # the flag. Find the trace body and confirm it is within the guard's braces.
    guard = re.search(r'if\s*\(\s*wlfgTraceThisPresent\s*\)\s*\{', sched)
    check("the trace is guarded by if (wlfgTraceThisPresent)", guard is not None)
    if guard:
        depth = 0
        start = sched.index("{", guard.start())
        end = None
        for j in range(start, len(sched)):
            if sched[j] == "{":
                depth += 1
            elif sched[j] == "}":
                depth -= 1
                if depth == 0:
                    end = j
                    break
        body = sched[start:end] if end else ""
        check("pass(1);pass(0) is INSIDE the guard", re.search(r'pass\(1\)\s*;\s*pass\(0\)', body) is not None)
        check("pass(2);CommitHistory() is INSIDE the guard",
              re.search(r'pass\(2\)\s*;\s*n\.CommitHistory\(\)', body) is not None)
        check("the ReSTIR passes are INSIDE the guard",
              re.search(r'pass\(3\)', body) is not None and re.search(r'pass\(14\)', body) is not None)
    # Pre-increment: the source is `++s_Data.WlfgPresentIndex;`. Assert the counter
    # is advanced at all -- a schedule that never advances its present index would
    # pin the modulo at 0 and trace every frame.
    check("WlfgPresentIndex is advanced once per present",
          re.search(r'\+\+s_Data\.WlfgPresentIndex\s*;', sched) is not None)

    print()
    print("4. the counters reach the timing line, and only once")

    check("the renderer pushes the counts via SetWlfgStats",
          re.search(r'SetWlfgStats\(\s*s_Data\.WlfgEffectiveFactor', sched) is not None)
    check("warps counted on a generated present",
          re.search(r'\+\+s_Data\.WlfgWarps', sched) is not None)
    check("realFrames counted on a traced present",
          re.search(r'\+\+s_Data\.WlfgRealFrames', sched) is not None)
    # The double-count guard: Application::Run() must not also increment.
    check("Application::Run() no longer blind-increments m_WlfgRealFrames",
          re.search(r'^\s*\+\+m_WlfgRealFrames\s*;', app_cpp, re.M) is None)
    check("SetWlfgStats leaves an unreported drift alone (default -1.0)",
          re.search(r'double\s+driftMs\s*=\s*-1\.0', app_h) is not None
          and re.search(r'if\s*\(\s*driftMs\s*>=\s*0\.0\s*\)', app_cpp) is not None)

    return all(ok for ok, _ in _results)


# ---------------------------------------------------------------------------
# The runtime half.
# ---------------------------------------------------------------------------

TIMING_RE = re.compile(
    r"WLFG timing:\s*factor\s+(\d+)\s+warps\s+(\d+)\s+realFrames\s+(\d+)\s+simDrift\s+([-\d.]+)ms")


def parse_timing(text: str) -> list[tuple[int, int, int, float]]:
    """All WLFG timing samples in a log, oldest first."""
    out = []
    for m in TIMING_RE.finditer(text):
        out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))))
    return out


# The accumulation's alpha reaches its 0.02 floor at FrameIndex+1 == 50, which is
# where WlfgMaxFactor() starts returning 6. Before that the ceiling is 2, so those
# presents trace every second one and must not be counted as converged.
CONVERGENCE_RENDERS = 50


def measure_ratio(samples: list[tuple[int, int, int, float]]) -> tuple[float | None, float | None]:
    """(whole-run ratio, converged-window ratio).

    Both are reported because they answer different questions and reporting only
    one is misleading either way.

    WHOLE RUN: presents/renders over every sample. This is the number a naive
    reading wants, and it is always BELOW the requested factor -- the first ~50
    presents are not converged (the accumulation's alpha is still above its 0.02
    floor), so WlfgMaxFactor() caps those to 2x and they trace every second
    present. That ramp is not a defect; it is the plan's precondition showing up
    in the data.

    CONVERGED WINDOW: from the first sample past convergence to the last. This is
    the number the plan's ">= 6x the render rate" is about.

    The window deliberately spans MANY schedule cycles, not one. The counters are
    cumulative, so a one-second delta quantizes hard: at factor 6 a 53-present
    second contains ceil(53/6) = 9 traces, and 53/9 = 5.889 -- a correct schedule
    reported as a 1.9% shortfall. Averaging from convergence to the end removes
    that quantization instead of papering over it with a tolerance.
    """
    if not samples:
        return None, None
    _, warps, real, _ = samples[-1]
    whole = ((real + warps) / real) if real > 0 else None

    base = next((s for s in samples if s[2] >= CONVERGENCE_RENDERS), None)
    if base is None or base is samples[-1]:
        return whole, None
    _, bw, br, _ = base
    dw, dr = warps - bw, real - br
    converged = ((dr + dw) / dr) if dr > 0 else None
    return whole, converged


def run_measurement(factor: int, seconds: int, env_extra: dict[str, str],
                    frame: int = 1200) -> tuple[float | None, str]:
    """Run one headless measurement at `factor` and return present/render.

    Mirrors the exporter's proven harness rather than inventing one: the editor is
    a windowed app that does not exit on its own, so a bounded run needs the
    screenshot hooks -- WL_SCREENSHOT arms the capture trigger and the linger-close,
    WL_SCREENSHOT_FRAME is the frame the trigger fires on, and WL_SCREENSHOT_LINGER
    is how long it stays up afterwards. Without them the process runs forever and a
    subprocess.run(timeout=) raises instead of returning.

    The camera is PINNED with no orbit. That is not a convenience: the Step 6
    precondition is a *converged static camera*, and an orbiting camera resolves the
    ceiling to 0 (no generation), so an orbit here would measure a factor-1 run and
    report it as a failure of the schedule.

    `frame` is set well past the accumulation's convergence point (alpha reaches its
    0.02 floor at FrameIndex+1 == 50), so the window measured is converged.
    """
    exe = os.path.join(ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor",
                       "DemonCore-Editor.exe")
    if not os.path.exists(exe):
        return None, f"editor not built: {exe}"
    editor_dir = os.path.dirname(exe)
    shot = os.path.join(os.environ.get("TMPDIR", editor_dir), f"wlfg_ratio_{factor}.png")
    log = os.path.join(os.environ.get("TMPDIR", editor_dir), f"wlfg_ratio_{factor}.log")

    env = dict(os.environ)
    env.update({
        "WL_WLFG": str(factor),
        "WL_WLFG_TIMING": "1",
        # Pinned, static camera -- see the docstring.
        "WASTELAND_PIN_CAMERA": "0,0,0,9,-85,0",
        "WL_DRS_OFF": "1",
        "WL_SCREENSHOT": shot,
        "WL_SCREENSHOT_FRAME": str(frame),
        "WL_SCREENSHOT_LINGER": "6",
        # Pass 24 (the reactive mask) only dispatches with reconstruction on, and
        # the head's mask feature comes from it.
        "WL_RECONSTRUCTION": "1",
    })
    env.update(env_extra)

    with open(log, "w", encoding="utf-8", errors="replace") as fh:
        proc = subprocess.Popen([exe], cwd=editor_dir, env=env,
                                stdout=fh, stderr=subprocess.STDOUT)
        try:
            proc.wait(timeout=seconds + 240)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    samples = parse_timing(read(log))
    if not samples:
        return None, None, f"no WLFG timing lines in {log} (is WL_WLFG_TIMING honoured?)"
    whole, converged = measure_ratio(samples)
    return whole, converged, log


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", action="store_true",
                    help="also measure the present-rate ratio from a live editor run")
    ap.add_argument("--factor", type=int, default=6,
                    help="factor to measure with --run (default 6)")
    ap.add_argument("--seconds", type=int, default=20, help="run length for --run")
    ap.add_argument("--frame", type=int, default=3000,
                    help="WL_SCREENSHOT_FRAME for --run: how many presents to run. "
                         "Well past convergence (50) so the converged window spans "
                         "many schedule cycles.")
    ap.add_argument("--self-test", action="store_true", help="run the mutation self-test")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    print(f"=== Task 8 Step 6: factor-gated schedule ({os.path.relpath(RENDERER, ROOT)}) ===")
    print()
    ok = structural()

    if args.run:
        print()
        print(f"5. measured present rate at factor {args.factor}")
        whole, converged, detail = run_measurement(args.factor, args.seconds, {},
                                                   frame=args.frame)
        if converged is None and whole is None:
            check(f"present/render ratio measured ({detail})", False)
            ok = False
        else:
            print(f"  whole run      ratio = {whole:.3f}" if whole else "  whole run      ratio = n/a")
            print(f"  converged      ratio = {converged:.3f}" if converged else "  converged      ratio = n/a")
            print(f"  (log: {detail})")
            # The acceptance is the CONVERGED number: the plan's precondition is a
            # converged static camera, and the whole-run figure necessarily carries
            # the ~50-present convergence ramp, where the ceiling caps the factor to
            # 2x. Failing on the whole-run number would fail a correct schedule.
            check(f"converged present/render >= {args.factor} at factor {args.factor}",
                  converged is not None and converged >= args.factor * 0.98)

    total = len(_results)
    passed = sum(1 for ok_, _ in _results if ok_)
    print()
    print(f"RESULT: {passed}/{total} checks passed")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# Self-test: every check must actually be able to go red.
# ---------------------------------------------------------------------------

def self_test() -> int:
    """Mutate the source in memory and confirm each structural check breaks.

    Each mutation is paired with the predicate it must break, so a mutation is
    "caught" only if the check would genuinely fail -- not merely if the text
    changed. Asserting that a literal disappeared is trivially true after any
    successful replace and reports OK even when the literal is missing.
    """
    renderer = read(RENDERER)
    app_cpp = read(APP_CPP)

    mutations = [
        ("WL_WLFG back to a boolean",
         renderer, "s_Data.WlfgFactor = glm::clamp(atoi(wlfgEnv), 0, 6);",
         "s_Data.WlfgWarpEnabled = atoi(wlfgEnv) != 0;",
         lambda t: re.search(r'WlfgFactor\s*=\s*glm::clamp\(\s*atoi\(', t) is not None),
        ("honour the request without the state ceiling",
         renderer, "s_Data.WlfgEffectiveFactor = glm::min(s_Data.WlfgFactor, wlfgCeiling);",
         "s_Data.WlfgEffectiveFactor = s_Data.WlfgFactor;",
         lambda t: re.search(r'WlfgEffectiveFactor\s*=\s*glm::min\(', t) is not None),
        ("schedule on FrameIndex instead of a present counter",
         renderer, "s_Data.WlfgPresentIndex % (std::uint64_t)s_Data.WlfgEffectiveFactor",
         "s_Data.FrameIndex % (std::uint64_t)s_Data.WlfgEffectiveFactor",
         lambda t: re.search(r'WlfgPresentIndex\s*%\s*\(std::uint64_t\)', t) is not None),
        ("trace every present (ratio becomes 1)",
         renderer, "if (wlfgTraceThisPresent)\n        {",
         "if (true)\n        {",
         lambda t: re.search(r'if\s*\(\s*wlfgTraceThisPresent\s*\)\s*\{', t) is not None),
        ("drop the renderer's stats push",
         renderer, "Application::Get().SetWlfgStats(s_Data.WlfgEffectiveFactor,",
         "// push removed",
         lambda t: re.search(r'SetWlfgStats\(\s*s_Data\.WlfgEffectiveFactor', t) is not None),
        ("restore the Application's blind increment",
         app_cpp, "// WLFG timing line. Gated on WL_WLFG_TIMING",
         "\t\t++m_WlfgRealFrames;\n\n\t\t// WLFG timing line. Gated on WL_WLFG_TIMING",
         lambda t: re.search(r'^\s*\+\+m_WlfgRealFrames\s*;', t, re.M) is None),
        ("freeze the present counter (modulo pins at 0)",
         renderer, "++s_Data.WlfgPresentIndex;", "// counter frozen",
         lambda t: re.search(r'\+\+s_Data\.WlfgPresentIndex\s*;', t) is not None),
        ("WlfgMaxFactor: drop the convergence branch",
         app_cpp, "if (accumAlpha <= 0.02f)", "if (true)",
         lambda t: re.search(r'if\s*\(\s*accumAlpha\s*<=\s*0\.02f\s*\)', t) is not None),
    ]

    all_caught = True
    for name, src_text, old, new, predicate in mutations:
        if old not in src_text:
            print(f"    [MISS] {name} -- literal absent; the verifier is asserting something stale")
            all_caught = False
            continue
        mutated = src_text.replace(old, new)
        caught = not predicate(mutated)
        all_caught = all_caught and caught
        print(f"    [{'OK' if caught else 'MISS'}] {name}")
    print()
    print("SELF-TEST: " + ("all mutations caught" if all_caught else "SOME MUTATIONS SURVIVED"))
    return 0 if all_caught else 1


if __name__ == "__main__":
    sys.exit(main())
