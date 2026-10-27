#!/usr/bin/env python3
"""Isolate WHAT makes Scene::LoadScene cost 327 us/entity.

The plan proposes a binary scene format on the assumption that YAML node
allocation dominates. That is a guess until measured. This probe varies ONE
thing -- how many components each entity carries -- and times the load, so the
cost can be attributed to either:

  * per-ENTITY overhead (log line, UUID mint, registry emplace) -> flat in
    component count, so a binary format buys little and the fix is elsewhere; or
  * per-COMPONENT overhead (yaml-cpp node construction, as<T>() conversion)
    -> scales with component count, so a binary format is the right fix.

Usage::

    python tools/probe_load_cost.py --count 20000
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tests" / "headless"))

import wlpath  # noqa: E402

wlpath.setup()

import Wasteland  # noqa: E402

# Component sets, from smallest to the full renderable entity. Each entry is the
# YAML block appended after the `- Entity: <id>` line.
BLOCKS = {
    "tag": """    TagComponent:
      Tag: {tag}
""",
    "tag+transform": """    TagComponent:
      Tag: {tag}
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
""",
    "tag+xform+cube": """    TagComponent:
      Tag: {tag}
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    CubeRendererComponent:
      Color: [0.3, 0.3, 0.3, 1]
      TextureIndex: 0
      TilingFactor: 1
""",
    "full": """    TagComponent:
      Tag: {tag}
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    CubeRendererComponent:
      Color: [0.3, 0.3, 0.3, 1]
      TextureIndex: 0
      TilingFactor: 1
    MaterialComponent:
      Albedo: [0.3, 0.3, 0.3, 1]
      TextureIndex: 0
      HasGeneratedMaps: false
      NormalStrength: 0
      Metallic: 0.1
      Roughness: 0.9
""",
}


def write_scene(path: Path, count: int, block_key: str) -> None:
    block = BLOCKS[block_key]
    out = [f"Scene: Probe {block_key}\n", "Entities:\n"]
    for i in range(count):
        out.append(f"  - Entity: {9_700_000_000_000_000_000 + i}\n")
        out.append(block.format(tag=f"P{i}"))
    path.write_text("".join(out), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--repeats", type=int, default=2)
    args = ap.parse_args()

    tmp = Path(wlpath.setup()).parent / "_probe_scenes"
    tmp.mkdir(parents=True, exist_ok=True)

    print(f"module: {wlpath.setup()}")
    print(f"count: {args.count} entities per variant\n")
    print(f"{'variant':>18} {'bytes':>10} {'load ms':>10} {'us/entity':>10} "
          f"{'vs tag':>8}")

    base = None
    for key in BLOCKS:
        path = tmp / f"probe_{key.replace('+', '_')}.wastescene"
        write_scene(path, args.count, key)
        size = path.stat().st_size

        best = float("inf")
        for _ in range(args.repeats):
            sc = Wasteland.Scene()
            t0 = time.perf_counter()
            ok = sc.LoadScene(str(path))
            dt = (time.perf_counter() - t0) * 1000.0
            if not ok:
                print(f"  {key}: LoadScene FAILED", file=sys.stderr)
                return 1
            best = min(best, dt)

        per = best * 1000.0 / args.count
        if base is None:
            base = per
        print(f"{key:>18} {size:>10} {best:>10.1f} {per:>10.1f} "
              f"{per / base:>7.2f}x")

    print("\nInterpretation:")
    print("  'vs tag' near 1.0 for every variant -> cost is per-ENTITY, not per")
    print("  component. A binary format would not help; look at the per-entity")
    print("  trace log and UUID mint instead.")
    print("  'vs tag' growing with component count -> cost is per-COMPONENT YAML")
    print("  work, and the binary format in Phase 4 is the right fix.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
