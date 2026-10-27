#!/usr/bin/env python3
"""Measure the CPU-side cost of entity count, with no GPU and no editor.

Why this file is separate from ``bench_entity_scale.py``
-------------------------------------------------------
The frame-time ladder in ``bench_entity_scale.py`` needs the editor running on
the GPU, which means it competes with whatever the user is doing and cannot be
run unattended. This file measures the part of the cost model that is pure CPU
and therefore reproducible on any machine, at any time:

  * YAML scene load (``Scene::LoadScene``) -- the L4 lever
  * ``StepPhysics`` over N inert entities -- the L1 lever
  * per-frame submission-list rebuild cost, via the renderer's own counters

Everything here runs against the built ``Wasteland.pyd`` through pybind, so it
measures the real engine code path, not a Python model of it.

Why ``LoadScene`` and not ``CreateEntity``
-----------------------------------------
``Scene::CreateEntity`` cannot build a *renderable* scene from Python. The
component pools are registered by the serializer, so a bare ``Scene()`` has no
storage for ``CubeRendererComponent``/``MaterialComponent`` and
``AddWasteland::MaterialComponent`` asserts ``Slot not available`` from
``entt::storage``. ``Scene::LoadScene`` registers them and is therefore the
only path that produces a scene the renderer will actually submit.

The scenes are produced by ``tools/gen_entity_block.py``, which writes the same
layout the deserializer reads.

Usage::

    python tools/gen_entity_block.py --count 100000 --out /tmp/Scale100k.wastescene
    python tools/bench_cpu_entity_scale.py --scene /tmp/Scale100k.wastescene
    python tools/bench_cpu_entity_scale.py --counts 1000,10000,100000 --frames 30
"""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tests" / "headless"))

import wlpath  # noqa: E402

wlpath.setup()

import Wasteland  # noqa: E402


def _time(fn, repeats: int = 3) -> float:
    """Best-of-N wall time in milliseconds.

    Best-of-N rather than mean: a single GC pause or scheduler preemption
    otherwise dominates the number, and the quantity under test is a lower
    bound on the cost.
    """
    best = float("inf")
    for _ in range(repeats):
        t0 = time.perf_counter()
        fn()
        best = min(best, (time.perf_counter() - t0) * 1000.0)
    return best


def _make_scene(count: int, workdir: Path, spacing: float = 20.0) -> Path:
    """Generate a scene file for ``count`` entities, cached on disk.

    Cached by count so repeated runs of the benchmark do not pay the generator
    each time -- the generator is a separate concern from the measurement.
    """
    path = workdir / f"Scale{count}.wastescene"
    if path.exists() and path.stat().st_size > 0:
        return path
    import subprocess

    subprocess.run(
        [sys.executable, str(HERE / "gen_entity_block.py"),
         "--count", str(count), "--spacing", str(spacing), "--out", str(path)],
        check=True, capture_output=True,
    )
    return path


def bench_scene_load(counts: list[int], workdir: Path) -> list[dict]:
    """Time ``Scene::LoadScene`` -- the L4 lever's cost.

    This is the dominant startup cost at scale: DemoGame is 22.2 MB of YAML and
    the live editor logged 376k deserialize lines before rendering a frame.
    """
    rows = []
    for n in counts:
        scene_path = _make_scene(n, workdir)

        def load():
            sc = Wasteland.Scene()
            if not sc.LoadScene(str(scene_path)):
                raise RuntimeError(f"LoadScene failed for {scene_path}")
            return sc

        ms = _time(load, repeats=3)
        rows.append(dict(op="load", count=n, ms=ms,
                         us_per_entity=ms * 1000.0 / max(1, n)))
        print(f"  load    {n:>8} entities: {ms:>9.1f} ms  "
              f"({ms * 1000.0 / max(1, n):>7.1f} us/entity)", flush=True)
    return rows


def bench_step_physics(counts: list[int], workdir: Path, frames: int) -> list[dict]:
    """Time ``StepPhysics`` over N INERT entities (no physics components).

    This is the plan's Phase 1 claim: with zero bodies, the two sync scans are
    pure O(entities) overhead, so the cost should scale linearly with N. If it
    instead stays flat, the scans are already gated and Phase 1 is a no-op.
    """
    rows = []
    for n in counts:
        scene_path = _make_scene(n, workdir)
        sc = Wasteland.Scene()
        if not sc.LoadScene(str(scene_path)):
            raise RuntimeError(f"LoadScene failed for {scene_path}")

        sc.OnSimulationStart()
        # Warm up once: the first step pays for any lazy allocation.
        sc.StepPhysics(1.0 / 60.0)

        t0 = time.perf_counter()
        for _ in range(frames):
            sc.StepPhysics(1.0 / 60.0)
        ms = (time.perf_counter() - t0) * 1000.0 / frames

        rows.append(dict(op="step_physics", count=n, ms=ms,
                         us_per_entity=ms * 1000.0 / max(1, n)))
        print(f"  step    {n:>8} inert: {ms:>9.3f} ms/frame  "
              f"({ms * 1000.0 / max(1, n):>7.3f} us/entity/frame)", flush=True)
        sc.OnSimulationStop()
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--counts", default="1000,10000,100000",
                    help="comma-separated entity counts")
    ap.add_argument("--frames", type=int, default=30,
                    help="physics steps per count")
    ap.add_argument("--workdir", default=None,
                    help="where to cache generated scenes (default: scratch dir)")
    ap.add_argument("--skip-load", action="store_true")
    args = ap.parse_args()

    counts = [int(c) for c in args.counts.split(",") if c.strip()]
    if not counts:
        print("error: no counts", file=sys.stderr)
        return 2

    workdir = Path(args.workdir) if args.workdir else Path(os.environ.get("TMPDIR", "/tmp"))
    workdir.mkdir(parents=True, exist_ok=True)

    print(f"module: {wlpath.setup()}")
    print(f"counts: {counts}   frames/count: {args.frames}")
    print(f"scenes: {workdir}")
    print("note: audio output disabled by wlpath; no GPU is initialised\n")

    print("[1] Scene::LoadScene (YAML deserialize)")
    load = [] if args.skip_load else bench_scene_load(counts, workdir)

    print("\n[2] StepPhysics over inert entities (no bodies)")
    step = bench_step_physics(counts, workdir, args.frames)

    print("\n=== summary ===")
    by_l = {r["count"]: r for r in load}
    by_s = {r["count"]: r for r in step}
    print(f"{'count':>8} {'load ms':>10} {'us/ent':>8} {'step ms/f':>10} {'us/ent/f':>9}")
    for c in counts:
        l = f"{by_l[c]['ms']:>10.1f} {by_l[c]['us_per_entity']:>8.1f}" if c in by_l else f"{'--':>10} {'--':>8}"
        print(f"{c:>8} {l} {by_s[c]['ms']:>10.3f} {by_s[c]['us_per_entity']:>9.3f}")

    # The Phase 1 verdict: does step cost scale with entity count?
    if len(step) >= 2:
        lo, hi = step[0], step[-1]
        ratio_count = hi["count"] / lo["count"]
        ratio_ms = hi["ms"] / max(lo["ms"], 1e-9)
        print(f"\nPhase 1 verdict: {lo['count']} -> {hi['count']} entities "
              f"({ratio_count:.1f}x) moved StepPhysics {lo['ms']:.3f} -> "
              f"{hi['ms']:.3f} ms ({ratio_ms:.2f}x)")
        if ratio_ms > 0.5 * ratio_count:
            print("  => scales with entity count: the whole-registry scan is REAL "
                  "and Phase 1 is worth doing.")
        else:
            print("  => flat: the sync scans are already gated; Phase 1 is a no-op "
                  "and the plan should be re-ranked.")

    if len(load) >= 2:
        lo, hi = load[0], load[-1]
        rc = hi["count"] / lo["count"]
        rm = hi["ms"] / max(lo["ms"], 1e-9)
        print(f"\nL4 verdict: load {lo['count']} -> {hi['count']} ({rc:.1f}x) took "
              f"{lo['ms']:.0f} -> {hi['ms']:.0f} ms ({rm:.2f}x)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

