#!/usr/bin/env python3
"""Entity-scale frame-time ladder: the Phase 0 baseline for the million-entity work.

What this measures
------------------
Real editor frame time as a function of (entity count x quality preset), on a
PINNED camera, in a CLEAN log. It is the number every later optimisation is
judged against, so it deliberately measures the shipped binary through the
shipped harness hooks rather than a synthetic model.

The harness contract it relies on (all in `DemonCore-Editor/src/EditorLayer.cpp`)
------------------------------------------------------------------------------
    WASTELAND_PIN_CAMERA=<fx,fy,fz>,<dist>,<pitch>,<yaw>   hold the camera still
    WL_CAPTURE_SCENE=<path>                                scene to load
    WL_CAPTURE_PRESET=<0..3>                               QualityPreset
    WL_CAPTURE_SCALE=<0.25..1.0>                           render scale
    WL_SCREENSHOT / WL_SCREENSHOT_FRAME / WL_SCREENSHOT_LINGER
                                                           scripted capture, and
                                                           the only clean way to
                                                           make the process EXIT

`Renderer3D` emits its `Frame time:` line every 100 frames, so `--frames` must be
a multiple of 100 and at least 200 to get more than one sample.

Why `WL_AUTO_SWITCH_TO` is deliberately NOT set
-----------------------------------------------
Setting it switches the renderer to NVRHI mid-run. On this machine that leaves
`NVRHIRendererAPI::m_Context` null (the "NVRHI Renderer API initialized" line is
logged in the CONSTRUCTOR, before `SetContext`), so every raster draw logs
`DrawIndexed: Missing context, shader, or vertex array`. Because `Log.cpp` does
`spdlog::flush_on(level::warn)`, each of those is a synchronous flushed write --
measured at a capped 200 per frame. A previous "baseline" of 196 ms/frame at a
static camera was that spam, not rendering. Leaving the switch alone keeps the
run on the native OpenGL path with a real context.

Usage::

    python tools/bench_entity_scale.py --counts 1000,10000 --presets 0,3
    python tools/bench_entity_scale.py --counts 100000 --presets 3 --frames 300
    python tools/bench_entity_scale.py --counts 1000 --presets 0 --report
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BIN = REPO / "bin" / "Release-windows-x86_64" / "DemonCore-Editor"
EXE = BIN / "DemonCore-Editor.exe"
LOG = BIN / "Wasteland.log"
SCENES = BIN / "assets" / "scenes"

# `Frame time: app 66.7ms (15.0 fps)  gpu 0.0ms  cpuSubmit 0.8ms  scale 1.00
#  preset 3  instances 498  screenCulled 0  cam (-78.8,592.8,48.6)`
FRAME_RE = re.compile(
    r"Frame time: app ([\d.]+)ms \(([\d.]+) fps\)\s+gpu ([\d.]+)ms\s+"
    r"cpuSubmit ([\d.]+)ms\s+scale ([\d.]+)\s+preset (\d+)\s+instances (\d+)\s+"
    r"screenCulled (\d+)\s+cam \(([-\d.]+),([-\d.]+),([-\d.]+)\)"
)

# A pinned camera that looks at the generated grid from outside it. The grid is
# a square in XZ centred on the origin, so focal=origin and a distance well
# beyond the half-extent keeps a large fraction of the scene in frustum -- the
# benchmark must exercise the cull, not stare at empty sky.
PIN_CAMERA = "0,0,0,{dist},-30,45"


def _dist_for(count: int, spacing: float) -> float:
    """Camera distance that frames the whole grid.

    The grid half-extent is `(cells_per_side-1)*spacing/2`; 2.2x that puts the
    far edge inside the frustum at a 30-degree pitch.
    """
    cells = max(1, int(count ** 0.5) + (1 if count ** 0.5 % 1 else 0))
    half = (cells - 1) * spacing / 2.0
    return max(50.0, half * 2.2)


def ensure_scene(count: int, spacing: float) -> Path:
    path = SCENES / f"Scale{count}.wastescene"
    if path.exists() and path.stat().st_size > 0:
        return path
    SCENES.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [sys.executable, str(REPO / "tools" / "gen_entity_block.py"),
         "--count", str(count), "--spacing", str(spacing), "--out", str(path)],
        check=True, capture_output=True,
    )
    return path


def run_one(count: int, preset: int, frames: int, spacing: float,
            scale: float, timeout: int, allow_drs: bool = False) -> dict:
    """Run the editor once and return the parsed frame samples."""
    scene = ensure_scene(count, spacing)
    dist = _dist_for(count, spacing)

    # Start from a clean environment: an inherited WL_AUTO_SWITCH_TO from the
    # user's shell would silently put the run on the broken NVRHI path.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("WL_", "WASTELAND_"))}
    env.update({
        "WL_CAPTURE_SCENE": f"assets/scenes/{scene.name}",
        "WL_CAPTURE_PRESET": str(preset),
        "WL_CAPTURE_SCALE": str(scale),
        "WASTELAND_PIN_CAMERA": PIN_CAMERA.format(dist=dist),
        "WL_SCREENSHOT": str(BIN / f"_bench_{count}_{preset}.png"),
        "WL_SCREENSHOT_FRAME": str(frames),
        "WL_SCREENSHOT_LINGER": "5",
    })
    # Dynamic resolution scaling is OFF by default: left on, it drops render
    # scale toward 0.40 within a few frames, so the run measures the scaler's
    # response to a slow frame rather than the cost of the entity count. That
    # also silently makes every row a different resolution, which is not
    # comparable across rows. `--allow-drs` opts back in.
    if not allow_drs:
        env["WL_DRS_OFF"] = "1"

    # Do NOT unlink the log: the editor holds it open, and a previous run's
    # process can still be shutting down. The app truncates the log itself at
    # startup (spdlog's basic_file_sink opens with truncate), so a stale file is
    # overwritten anyway. Deleting it only created a WinError 32 race.
    t0 = time.perf_counter()
    proc = subprocess.Popen([str(EXE)], cwd=str(BIN), env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    timed_out = False
    try:
        rc = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        # The screenshot hook is supposed to end the process. If it did not, the
        # run is not a usable sample and must not leave an editor holding the
        # GPU and the log for the next iteration.
        proc.kill()
        proc.wait(timeout=30)
        rc = None
        timed_out = True
    wall = time.perf_counter() - t0

    rows = []
    if LOG.exists():
        with open(LOG, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = FRAME_RE.search(line)
                if m:
                    # app 0.0ms is the startup frame: the counter's first sample
                    # has no previous timestamp to difference against, so it
                    # reports zero. Including it drags every median toward zero
                    # and makes a slow run look fast.
                    if float(m.group(1)) <= 0.0:
                        continue
                    rows.append(dict(
                        app=float(m.group(1)), fps=float(m.group(2)),
                        gpu=float(m.group(3)), cpu=float(m.group(4)),
                        scale=float(m.group(5)), preset=int(m.group(6)),
                        inst=int(m.group(7)), scull=int(m.group(8)),
                    ))

    # A run that produced no Frame time line measured nothing. Say so instead
    # of reporting a zero that looks like a fast frame.
    if not rows:
        return dict(count=count, preset=preset, ok=False, rc=rc,
                    timed_out=timed_out, wall=wall, n=0)

    apps = [r["app"] for r in rows]
    return dict(
        count=count, preset=preset, ok=True, rc=rc, timed_out=timed_out,
        wall=wall, n=len(rows),
        app_med=statistics.median(apps), app_min=min(apps), app_max=max(apps),
        fps_med=statistics.median([r["fps"] for r in rows]),
        cpu_med=statistics.median([r["cpu"] for r in rows]),
        inst=max(r["inst"] for r in rows),
        scale=rows[-1]["scale"],
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--counts", default="1000,10000,100000")
    ap.add_argument("--presets", default="0,3")
    ap.add_argument("--frames", type=int, default=200,
                    help="frames per run; must be a multiple of 100 (log cadence)")
    ap.add_argument("--spacing", type=float, default=20.0)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--allow-drs", action="store_true",
                    help="leave dynamic resolution scaling ON (default: WL_DRS_OFF=1)")
    ap.add_argument("--report", action="store_true",
                    help="write tools/bench_entity_scale_report.md")
    args = ap.parse_args()

    if not EXE.exists():
        print(f"error: editor not built: {EXE}", file=sys.stderr)
        return 2
    if args.frames % 100 != 0:
        print(f"warning: --frames {args.frames} is not a multiple of 100; "
              f"the Frame time line is emitted every 100 frames", file=sys.stderr)

    counts = [int(c) for c in args.counts.split(",") if c.strip()]
    presets = [int(p) for p in args.presets.split(",") if p.strip()]

    print(f"editor: {EXE}")
    print(f"counts {counts}  presets {presets}  frames {args.frames}  "
          f"scale {args.scale}")
    print("renderer: native OpenGL (WL_AUTO_SWITCH_TO unset)\n")

    results = []
    for count in counts:
        for preset in presets:
            print(f"  {count:>8} entities  preset {preset} ... ", end="", flush=True)
            r = run_one(count, preset, args.frames, args.spacing,
                        args.scale, args.timeout, args.allow_drs)
            results.append(r)
            if not r["ok"]:
                print(f"NO DATA (rc={r['rc']} timeout={r['timed_out']} "
                      f"wall={r['wall']:.0f}s)")
            else:
                print(f"{r['app_med']:>8.1f} ms  {r['fps_med']:>5.1f} fps  "
                      f"inst {r['inst']:>6}  cpu {r['cpu_med']:>5.1f} ms  "
                      f"scale {r['scale']:.2f}  (n={r['n']}, {r['wall']:.0f}s)")

    ok = [r for r in results if r["ok"]]
    if not ok:
        print("\nno usable samples", file=sys.stderr)
        return 1

    print(f"\n{'entities':>9} {'preset':>7} {'app med':>9} {'fps med':>8} "
          f"{'instances':>10} {'cpuSubmit':>10} {'scale':>6}")
    for r in sorted(ok, key=lambda r: (r["preset"], r["count"])):
        print(f"{r['count']:>9} {r['preset']:>7} {r['app_med']:>9.1f} "
              f"{r['fps_med']:>8.1f} {r['inst']:>10} {r['cpu_med']:>10.1f} "
              f"{r['scale']:>6.2f}")

    # The question the plan is actually asking: which preset/count combinations
    # hold the 30 fps floor the user asked for?
    print("\n30 fps floor:")
    for r in sorted(ok, key=lambda r: (r["preset"], r["count"])):
        verdict = "PASS" if r["fps_med"] >= 30.0 else "FAIL"
        print(f"  {r['count']:>9} @ preset {r['preset']}: "
              f"{r['fps_med']:>5.1f} fps  {verdict}")

    if args.report:
        out = REPO / "tools" / "bench_entity_scale_report.md"
        with open(out, "w", encoding="utf-8") as fh:
            fh.write("# Entity-scale frame-time baseline\n\n")
            fh.write(f"Captured {time.strftime('%Y-%m-%d %H:%M')} on "
                     f"{os.environ.get('COMPUTERNAME', 'unknown')}, "
                     f"native OpenGL path, pinned camera, render scale "
                     f"{args.scale}, {args.frames} frames per run.\n\n")
            fh.write("| entities | preset | app median ms | fps median | "
                     "instances | cpuSubmit ms | scale |\n")
            fh.write("|---|---|---|---|---|---|---|\n")
            for r in sorted(ok, key=lambda r: (r["preset"], r["count"])):
                fh.write(f"| {r['count']} | {r['preset']} | {r['app_med']:.1f} | "
                         f"{r['fps_med']:.1f} | {r['inst']} | "
                         f"{r['cpu_med']:.1f} | {r['scale']:.2f} |\n")
        print(f"\nwrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
