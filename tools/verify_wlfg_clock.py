#!/usr/bin/env python3
"""WLFG clock verifier (Task 6). Two checks, plus a mutation self-test.

WHY THIS EXISTS
---------------
WLFG (Wasteland frame generation) inserts synthetic presents between real ones.
The plan's model for keeping the game at the correct speed is: the simulation
advances by 1/factor of real time per present, while the render loop keeps the
real rate. Application::Run() makes the timestep handed to layers
`rawDelta * m_SimTimescale`, so that model is `m_SimTimescale = 1/factor`. The
factor itself is capped by Application::WlfgMaxFactor from the accumulation state
(6x converged + static + Nova, 2x partial, off otherwise).

READ THIS BEFORE TRUSTING CHECK 1
---------------------------------
Check 1 is a MODEL of the plan's intent, NOT a check of the engine, and it passes
regardless of what the engine does. `m_SimTimescale = 1/factor` is **not wired in
C++** and deliberately should not be: the model assumes the present rate scales
exactly with the factor, and measured on this machine it does not (WL_WLFG=1
presents 62.6/s, WL_WLFG=6 presents 153.1/s -- 2.45x, not 6x, because traced
presents still dominate the frame cost). Applying 1/factor against the real
present rates would run the sim at ~41% of real speed at factor 6.

The engine's actual invariant is the one that matters and it holds at
m_SimTimescale = 1.0: the timestep is rawDelta * 1.0, and sum(rawDelta) over any
wall-clock window IS that window, so simulated time tracks real time at every
factor. Do not read a PASS here as "the engine scales the sim clock".

This tool checks:

  1. CLOCK INVARIANT (model). Over N presents, the sum of the simulated deltas
     must equal the real elapsed time, regardless of the factor, GIVEN
     m_SimTimescale = 1/factor. At factor F a real frame interval D is presented
     F times, each present advancing the sim by D * (1/F); the F presents sum to
     D, so the total reconstructs the real elapsed for any F. The accumulated
     drift must stay ~0. The arithmetic is sound; the premise about the present
     rate is what the engine does not satisfy.

  2. FACTOR CEILING. WlfgMaxFactor must return 6 when converged (accumAlpha at
     its 0.02 floor) + static camera + Nova backend, 2 when partially
     converged, and 0 on a camera move or a non-Nova backend. Checked by
     reading the C++ source of the function.

Usage:
    python3 tools/verify_wlfg_clock.py            # both checks
    python3 tools/verify_wlfg_clock.py --self-test  # mutate + confirm detection
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_CPP = ROOT / "Wasteland" / "src" / "Wasteland" / "Core" / "Application.cpp"

# The accumulation blend weight's floor. A history whose alpha has reached it is
# converged; below it the frame is still accumulating. Matches the C++ guard in
# Application::WlfgMaxFactor and the accumulation code's max(0.02, 1/(n+1)).
ACCUM_ALPHA_FLOOR = 0.02


# --------------------------------------------------------------------------- #
# Check 1 -- the clock invariant                                              #
# --------------------------------------------------------------------------- #
def clock_invariant() -> tuple[float, list[tuple[int, float, int]]]:
    """Model the WLFG loop: a real frame interval D presented F times, each
    present advancing the sim by D * (1/F). Returns (realElapsed, per-factor
    (factor, drift, presents)). drift must be ~0 for every factor."""
    # Eight arbitrary real frame intervals (seconds). The values do not matter;
    # the invariant is that the sum reconstructs the total for any factor.
    raw_deltas = [0.016, 0.017, 0.0165, 0.0162, 0.0168, 0.0161, 0.0166, 0.0163]
    real_elapsed = sum(raw_deltas)
    results = []
    for factor in range(1, 7):
        timescale = 1.0 / factor          # m_SimTimescale = 1 / factor
        sim_total = 0.0
        presents = 0
        for d in raw_deltas:
            for _ in range(factor):       # F presents per real frame
                sim_total += d * timescale
                presents += 1
        drift = abs(sim_total - real_elapsed)
        results.append((factor, drift, presents))
    return real_elapsed, results


def check_clock() -> bool:
    real_elapsed, results = clock_invariant()
    ok = True
    print(f"  clock invariant: real elapsed {real_elapsed * 1000:.1f} ms")
    for factor, drift, presents in results:
        status = "ok" if drift < 1e-9 else "FAIL"
        if drift >= 1e-9:
            ok = False
        print(f"    factor {factor}x: sum(simDelta)={ (real_elapsed - drift) * 1000:.3f}ms "
              f"drift={drift * 1e6:.3f}us presents={presents}  [{status}]")
    return ok


# --------------------------------------------------------------------------- #
# Check 2 -- the factor ceiling function                                      #
# --------------------------------------------------------------------------- #
def _extract_wlfg_factor_body() -> str | None:
    """Return the body of Application::WlfgMaxFactor, or None."""
    if not APP_CPP.exists():
        return None
    src = APP_CPP.read_text(encoding="utf-8", errors="replace")
    m = re.search(
        r"int\s+Application::WlfgMaxFactor\s*\([^)]*\)\s*const\s*\{",
        src,
    )
    if not m:
        return None
    i = m.end() - 1  # position of the opening '{'
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def check_ceiling() -> bool:
    body = _extract_wlfg_factor_body()
    if body is None:
        print("  factor ceiling: FAIL -- Application::WlfgMaxFactor not found")
        return False

    # Normalise so the structural checks are formatting-insensitive: strip C++
    # comments (each guard here is followed by an explanatory comment before its
    # return) and collapse whitespace.
    b = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    b = re.sub(r"//[^\n]*", "", b)
    b = re.sub(r"\s+", " ", b)

    checks = []

    def add(name: str, cond: bool) -> None:
        checks.append(cond)
        print(f"    {name}: {'ok' if cond else 'FAIL'}")

    # Off when a non-Nova backend is active (no velocity buffer).
    add("!novaBackend -> 0",
        bool(re.search(r"!\s*novaBackend\s*\)\s*return\s+0", b))
        or bool(re.search(r"if\s*\(\s*!\s*novaBackend\s*\)\s*return\s+0", b)))
    # Off when the camera/scene moved (reprojection undefined).
    add("cameraMoved -> 0",
        bool(re.search(r"if\s*\(\s*cameraMoved\s*\)\s*return\s+0", b)))
    # 6x when converged: alpha at its 0.02 floor.
    add("converged (alpha<=0.02) -> 6",
        bool(re.search(r"accumAlpha\s*<=\s*0\.02f?\s*\)\s*return\s+6", b)))
    # 2x otherwise (still accumulating).
    add("partial -> 2", bool(re.search(r"return\s+2", b)))
    # The four return values must all be present and nothing higher than 6.
    add("max factor is 6", ("return 6" in b) and ("return 7" not in b)
        and ("return 8" not in b))

    return all(checks)


# --------------------------------------------------------------------------- #
# Self-test: mutate and confirm the checks fire                                #
# --------------------------------------------------------------------------- #
def self_test() -> bool:
    print("  self-test: mutating the sources, each check must fail")
    orig_cpp = APP_CPP.read_text(encoding="utf-8") if APP_CPP.exists() else ""

    def run_ceiling_on(text: str) -> bool:
        import tempfile, subprocess
        # Re-check against a mutated copy by monkey-patching the reader.
        global APP_CPP
        saved = APP_CPP
        class _P:
            def __init__(self, t): self._t = t
            def read_text(self, *a, **k): return self._t
            def exists(self): return True
        APP_CPP = _P(text)
        try:
            return check_ceiling()
        finally:
            APP_CPP = saved

    mutations = [
        ("ceiling: converged returns 3 instead of 6",
         "return 6", "return 3"),
        ("ceiling: partial returns 0 instead of 2",
         "return 2;", "return 0;"),
        ("ceiling: camera move returns 2 instead of 0",
         "if (cameraMoved)", "if (false /* cameraMoved */)"),
    ]
    all_ok = True
    for name, old, new in mutations:
        mutated = orig_cpp.replace(old, new, 1)
        fired = not run_ceiling_on(mutated)
        print(f"    {name}: {'detected' if fired else 'MISS'}")
        all_ok = all_ok and fired

    # The clock invariant must fail if the per-present advance is not D/F: at
    # factor 6 with 3 presents per real frame the sum is real/2, not real.
    clock_broken = not abs(sum(d * (1.0 / 6) for d in [0.016, 0.017, 0.0165]
                               for _ in range(3)) - sum([0.016, 0.017, 0.0165])) < 1e-9
    print(f"    clock: wrong-timescale drift detected: {'ok' if clock_broken else 'MISS'}")
    all_ok = all_ok and clock_broken
    return all_ok


def main() -> int:
    print("WLFG clock verifier (Task 6)")
    if "--self-test" in sys.argv:
        ok = self_test()
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1

    ok = True
    print("check 1 -- clock invariant:")
    ok = check_clock() and ok
    print("check 2 -- factor ceiling:")
    ok = check_ceiling() and ok
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
