#!/usr/bin/env python3
"""Nova phase baseline: frame time + instance counts at a pinned camera.

WHY THIS EXISTS
---------------
Every phase of the virtualized-instances plan is gated on a measurement. This
produces that measurement in the exact shape the gates need: one row per
(entity count, preset) with `app`, `gpu`, `cpuSubmit`, `instances` and
`screenCulled` read from the editor's own `Frame time:` line.

CONFIDENCE LABEL -- READ THIS
-----------------------------
A row is only a baseline if the GPU was quiet while it ran. This machine often
has background GPU work; the harness records the measured utilisation alongside
every run so a later reader can judge the number instead of trusting it.

`gpu` is the honest signal on OpenGL: if it reads 0.0 the GL timer produced
nothing and the row is INVALID, not fast. The harness fails such a row rather
than averaging it.

That rule is backend-specific. `gpu` comes from GL_TIME_ELAPSED queries, which
only exist on the OpenGL path -- on DX11/DX12/Vulkan the field is 0.0 by
construction, because there is no timer to fail. The harness reads the backend
from the run's own log and applies the rule only where it means something; on
the NVRHI backends `app` is the only frame-time signal and is real.

USAGE
    python tools/bench_nova_phase.py --counts 10000,100000 --presets 3
    python tools/bench_nova_phase.py --counts 1000 --presets 0,3 --frames 300
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
    BIN, EXE, LOG, PIN_CAMERA, _dist_for, ensure_scene,
)

# app / fps / gpu / cpuSubmit / scale / preset / instances / screenCulled
# Eye-level framing. The default PIN_CAMERA pulls back 2.2x the grid half-extent so the
# WHOLE grid fits -- at 100k entities that is ~3.5 km away, which makes each cube 1-2 px.
# The result renders (verified) but measures pixel coverage, not the engine: a near-black
# field of sub-pixel cubes. Eye-level puts the camera INSIDE the grid at a fixed short
# distance with a horizontal pitch, which is what a player actually sees.
# Focal point raised to y=6 and pitched -8 degrees. With the focal at y=0 and pitch 0 the
# camera sits AT ground level and the frame is a flat sky-over-ground horizon with the
# cubes edge-on -- which is what the first attempt produced. Standing a few units up and
# tilting down puts the grid in front of the lens.
PIN_CAMERA_EYE = "0,6,0,{dist},-8,45"
EYE_DISTANCE = 40.0

FRAME_RE = re.compile(
    r"Frame time: app ([\d.]+)ms \(([\d.]+) fps\)\s+gpu ([\d.]+)ms\s+"
    r"cpuSubmit ([\d.]+)ms\s+scale ([\d.]+)\s+preset (\d+)\s+instances (\d+)\s+"
    r"screenCulled (\d+)"
)


def detect_backend() -> str:
    """Return the backend the measured run used, read from its own log.

    `Frame time:`'s `gpu` field is produced by GL_TIME_ELAPSED queries, which only
    exist on the OpenGL path. On DX11/DX12/Vulkan it is 0.0 by construction -- not
    because a timer failed, but because there is no timer. The validity rule in
    main() has to know which of those two situations it is looking at, and the log
    is the only place the run states it.

    Returns "unknown" when the log cannot be read, so the caller keeps the strict
    rule rather than assuming a backend it cannot prove.
    """
    try:
        text = LOG.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "unknown"
    backend = "unknown"
    for line in text.splitlines():
        if "Renderer API initialized:" in line:
            backend = line.rsplit(":", 1)[-1].strip()
    # Vulkan never prints the line above -- it announces itself with these instead,
    # and a project that pins RendererAPI: 4 will switch to it several seconds into
    # the run, after the DX12 line has already been written. Checked separately so
    # the result does not depend on which line happens to come last.
    if ("NVRHI Vulkan device created successfully" in text
            or "Selected Vulkan GPU:" in text):
        backend = "Vulkan"
    return backend
# WLFG accounting line (Application.cpp, gated on WL_WLFG_TIMING):
#   WLFG timing: factor 1 warps 0 realFrames 1234 simDrift 0.0ms
WLFG_RE = re.compile(
    r"WLFG timing: factor (\d+) warps (\d+) realFrames (\d+) simDrift ([\d.]+)ms"
)


def wlfg_last(text: str):
    """The last WLFG accounting line's fields, or None if the log has none."""
    m = WLFG_RE.findall(text)
    return m[-1] if m else None


def gpu_util() -> str:
    """Current GPU utilisation, for labelling the row. 'n/a' if nvidia-smi is absent."""
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=utilization.gpu,memory.used",
             "--format=csv,noheader"],
            capture_output=True, text=True, timeout=10)
        return out.stdout.strip() or "n/a"
    except (OSError, subprocess.SubprocessError):
        return "n/a"


def run_one(count: int, preset: int, frames: int, spacing: float,
            timeout: int, scale: float = 1.0, drs: bool = False,
            eye: bool = False, eye_dist: float = EYE_DISTANCE,
            viewport: str = "") -> dict:
    scene = ensure_scene(count, spacing)
    dist = _dist_for(count, spacing)

    # Strip inherited WL_/WASTELAND_ vars: a stray WL_AUTO_SWITCH_TO would put
    # the run on the NVRHI path and measure the wrong renderer entirely.
    # WL_DRS_DEBUG is an allowlisted EXCEPTION -- it only turns on the GL query
    # diagnostics and does not select a renderer, and without it the timer
    # read-back cannot be diagnosed at all.
    _PASSTHROUGH = {"WL_DRS_DEBUG", "WL_FRAME_TIMING", "WL_WLFG_TIMING", "WASTELAND_NOVA_PROFILE", "WL_GL_FINISH", "WL_MAX_INSTANCES"}
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("WL_", "WASTELAND_")) or k in _PASSTHROUGH}
    # WL_DRS_OFF is set by default: it removes the scaler as a confound so the reading is
    # attributable. Pass --drs-on to measure the engine's own behaviour instead.
    # NOTE: an EMPTY value still disables the DRS -- the engine tests getenv() != nullptr,
    # not the string -- so the key must be absent, not blank.
    env.update({
        "WL_CAPTURE_SCENE": f"assets/scenes/{scene.name}",
        "WL_CAPTURE_PRESET": str(preset),
        "WL_CAPTURE_SCALE": str(scale),
        "WASTELAND_PIN_CAMERA": ((PIN_CAMERA_EYE.format(dist=eye_dist))
                                  if eye else PIN_CAMERA.format(dist=dist)),
        "WL_SCREENSHOT": str(BIN / f"_phase_{count}_{preset}.png"),
        "WL_SCREENSHOT_FRAME": str(frames),
        "WL_SCREENSHOT_LINGER": "3",
    })
    if not drs:
        env["WL_DRS_OFF"] = "1"

    # The viewport PANEL, not the window, is what m_ViewportSize -- and therefore the
    # editor framebuffer and the Nova render target -- follows (EditorLayer.cpp:2515,
    # :1167, :1169). In the default bench layout that panel is 1280x720, so without
    # this override a 4K target cannot be measured at all. This must be a flag rather
    # than a shell env var: the sanitising filter above strips every WL_* name that is
    # not allowlisted, so an inherited WL_CAPTURE_VIEWPORT would never arrive.
    if viewport:
        env["WL_CAPTURE_VIEWPORT"] = viewport

    before = gpu_util()
    t0 = time.perf_counter()
    proc = subprocess.Popen([str(EXE)], cwd=str(BIN), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=30)
        raise SystemExit(f"run {count}/{preset} timed out after {timeout}s")
    elapsed = time.perf_counter() - t0
    after = gpu_util()

    text = LOG.read_text(encoding="utf-8", errors="replace")
    matches = FRAME_RE.findall(text)
    if not matches:
        raise SystemExit(f"run {count}/{preset} produced no usable Frame time line")
    app, fps, gpu, cpu, scale, pres, inst, culled = matches[-1]
    return {
        "count": count, "preset": preset,
        "app": float(app), "fps": float(fps), "gpu": float(gpu),
        "cpu": float(cpu), "scale": float(scale), "preset_read": int(pres),
        "instances": int(inst), "culled": int(culled),
        "wall": elapsed, "gpu_before": before, "gpu_after": after,
        # WLFG accounting (Step 3): factor/warps/realFrames/simDrift, or None if
        # WL_WLFG_TIMING was not set for this run.
        "wlfg": wlfg_last(text),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--counts", default="10000,100000")
    ap.add_argument("--presets", default="3")
    ap.add_argument("--spacing", type=float, default=4.0)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--eye-distance", type=float, default=EYE_DISTANCE,
                    help="distance for --eye-level; sweep this to find a view that "
                         "shows a representative instance count")
    ap.add_argument("--eye-level", action="store_true",
                    help="pin the camera inside the grid at eye height instead of "
                         "framing the whole grid from far above")
    ap.add_argument("--drs-on", action="store_true",
                    help="leave the dynamic resolution scaler enabled")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="render scale; cpuSubmit that falls with scale is GPU wait, not CPU work")
    ap.add_argument("--viewport", default="",
                    help="force the editor viewport / Nova target, e.g. 3840x2160 "
                         "(default: the docked panel, 1280x720 in the bench layout)")
    args = ap.parse_args()

    if not EXE.exists():
        raise SystemExit(f"editor not built: {EXE}")

    counts = [int(c) for c in args.counts.split(",") if c.strip()]
    presets = [int(p) for p in args.presets.split(",") if p.strip()]

    print(f"{'count':>8} {'preset':>7} {'app ms':>8} {'fps':>6} {'gpu ms':>7} "
          f"{'cpu ms':>7} {'instances':>10} {'culled':>7}")
    print("-" * 74)
    rows = []
    for count in counts:
        for preset in presets:
            r = run_one(count, preset, args.frames, args.spacing, args.timeout,
                        args.scale, drs=args.drs_on, eye=args.eye_level,
                        eye_dist=args.eye_distance, viewport=args.viewport)
            rows.append(r)
            print(f"{r['count']:>8} {r['preset_read']:>7} {r['app']:>8.1f} "
                  f"{r['fps']:>6.1f} {r['gpu']:>7.1f} {r['cpu']:>7.1f} "
                  f"{r['instances']:>10} {r['culled']:>7}")
            time.sleep(1.0)

    print()
    # The gpu-validity rule only means something where a GL timer exists. On the
    # NVRHI backends `gpu` is 0.0 by construction, so failing those rows would
    # discard the only measurement the plan has on the backends it targets.
    backend = detect_backend()
    if backend == "OpenGL":
        invalid = [r for r in rows if r["gpu"] <= 0.0]
        if invalid:
            print(f"INVALID ROWS: {len(invalid)} run(s) reported gpu 0.0 -- the GL timer "
                  f"yielded nothing, so those rows are not measurements.")
    else:
        invalid = []
        print(f"BACKEND: {backend} -- no GL timer exists on this backend, so `gpu` reads "
              f"0.0 by construction. `app` is the only frame-time signal and is real; "
              f"`gpu`/`cpuSubmit` are not applicable on this backend.")
    for r in rows:
        print(f"  {r['count']:>7}/{r['preset_read']}  GPU during run: "
              f"{r['gpu_before']} -> {r['gpu_after']}")
    print()
    wlfg_rows = [r for r in rows if r.get("wlfg")]
    if wlfg_rows:
        print("WLFG accounting (factor / warps / realFrames / simDrift ms):")
        for r in wlfg_rows:
            f, w, rf, drift = r["wlfg"]
            print(f"  {r['count']:>7}/{r['preset_read']}  factor {f}  warps {w}  "
                  f"realFrames {rf}  simDrift {drift}")
    else:
        print("WLFG accounting: none (set WL_WLFG_TIMING=1 to report it)")
    print()
    print("A row is only a BASELINE if the GPU line above shows a quiet card.")
    print("Contended rows are still valid for instance counts (CPU-side), never for ms.")
    return 1 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
