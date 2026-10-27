#!/usr/bin/env python3
"""Measure how many instances the screen-space cull drops, per threshold.

WHY THIS EXISTS
---------------
`CullInstanceForTrace` has had a screen-space cull since early on, but it is off
by default (`ScreenCullPixels = 0.0f`) and only reachable through
`WASTELAND_SCREEN_CULL`. Deciding whether it should be on needs a number: how
many instances does a given threshold actually remove?

That number is CPU-side. The instance list is built by the CPU cull before any
GPU work happens, so `instances` and `screenCulled` in the `Frame time:` line are
exact regardless of GPU contention. That matters here: this machine usually has
an LLM worker holding 100% of the GPU, which makes every *timing* number
untrustworthy but leaves these counts intact. So this probe reports counts and
never claims a frame-time win.

WHAT IT DOES NOT ANSWER
-----------------------
Whether the cull makes the frame faster. Removing N instances only helps if the
BVH traversal cost tracks N, and confirming that needs a quiet GPU. The counts
here bound the *possible* win; they do not measure it.

USAGE
    python tools/probe_screen_cull.py
    python tools/probe_screen_cull.py --count 5000 --thresholds 0,1,2,4,8
    python tools/probe_screen_cull.py --scene assets/scenes/DemoGame.wastescene
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_entity_scale import (  # noqa: E402
    BIN, EXE, LOG, PIN_CAMERA, SCENES, _dist_for, ensure_scene,
)

# Same shape as bench_entity_scale's FRAME_RE, but tolerant: this probe only needs
# instances and screenCulled, and must keep working if the line grows a field.
INST_RE = re.compile(r"instances (\d+)\s+screenCulled (\d+)")


def run_once(scene_name: str, dist: float, threshold: float, frames: int,
             timeout: int) -> tuple[int, int]:
    """Run the editor once; return (instances, screenCulled) from the last frame line."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("WL_", "WASTELAND_"))}
    env.update({
        "WL_CAPTURE_SCENE": scene_name,
        "WL_CAPTURE_PRESET": "3",
        "WL_CAPTURE_SCALE": "1.0",
        "WASTELAND_PIN_CAMERA": PIN_CAMERA.format(dist=dist),
        # Dynamic resolution scaling off: it would change the trace resolution
        # mid-run, and the screen cull threshold is expressed in TRACE pixels, so
        # a drifting scale would silently change what the threshold means.
        "WL_DRS_OFF": "1",
        "WL_SCREENSHOT": str(BIN / f"_cull_{threshold:g}.png"),
        "WL_SCREENSHOT_FRAME": str(frames),
        "WL_SCREENSHOT_LINGER": "3",
    })
    if threshold > 0.0:
        env["WASTELAND_SCREEN_CULL"] = f"{threshold:g}"

    proc = subprocess.Popen([str(EXE)], cwd=str(BIN), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=30)
        raise SystemExit(f"run with threshold {threshold} timed out after {timeout}s")

    text = LOG.read_text(encoding="utf-8", errors="replace")
    matches = INST_RE.findall(text)
    if not matches:
        raise SystemExit(
            f"no 'Frame time:' line with instances/screenCulled for threshold "
            f"{threshold}; the run produced no usable sample")
    instances, culled = matches[-1]
    return int(instances), int(culled)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--count", type=int, default=10000,
                    help="entity count for the generated grid scene (ignored with --scene)")
    ap.add_argument("--spacing", type=float, default=4.0)
    ap.add_argument("--scene", default=None,
                    help="scene path relative to the editor's assets dir")
    ap.add_argument("--thresholds", default="0,1,2,4,8",
                    help="comma-separated WASTELAND_SCREEN_CULL values, in trace px")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    if not EXE.exists():
        raise SystemExit(f"editor not built: {EXE}")

    if args.scene:
        scene_name = args.scene
        dist = 900.0  # DemoGame is ~7.2 km across; frame a good part of it.
    else:
        scene = ensure_scene(args.count, args.spacing)
        scene_name = f"assets/scenes/{scene.name}"
        dist = _dist_for(args.count, args.spacing)

    thresholds = [float(t) for t in args.thresholds.split(",") if t.strip()]
    print(f"scene      {scene_name}")
    print(f"camera     dist {dist:.0f}, preset 3, scale 1.00, DRS off")
    print(f"frames     {args.frames}")
    print()
    print(f"{'cull px':>8} {'instances':>10} {'screenCulled':>13} {'% dropped':>10}")
    print("-" * 45)

    baseline = None
    for threshold in thresholds:
        t0 = time.perf_counter()
        instances, culled = run_once(scene_name, dist, threshold, args.frames,
                                     args.timeout)
        elapsed = time.perf_counter() - t0
        if threshold == 0.0:
            baseline = instances + culled
            dropped = 0.0
        elif baseline:
            dropped = 100.0 * culled / baseline
        else:
            # Single-threshold run: no baseline row to compare against. Report the
            # share of candidates instead of a meaningless ratio against 1.
            dropped = 100.0 * culled / max(instances + culled, 1)
        print(f"{threshold:>8.2f} {instances:>10} {culled:>13} {dropped:>9.1f}%"
              f"   ({elapsed:.0f}s)")
        # The screenshot hook is supposed to end the process; give the log sink a
        # moment to close so the next run truncates cleanly rather than appending.
        time.sleep(1.0)

    print()
    print("Note: counts only. Frame time is NOT reported -- the GPU is shared with")
    print("other processes on this machine, so a timing number would be contention,")
    print("not the engine. Use these counts to bound the win, then measure fps on a")
    print("quiet GPU.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
