#!/usr/bin/env python3
"""Generate a synthetic ``.wastescene`` for the entity-scale benchmark.

Why this file exists
--------------------
``tools/bench_entity_scale.py`` needs a scene with a controllable entity count
to localise where the frame cost stops being flat. Building it through the
Python bindings (``Scene.CreateEntity`` per entity) is fine up to a few tens of
thousands and unbearably slow beyond that, because every entity is a pybind call
that crosses into C++, emplaces three components, and mints a random UUID.

So the scene is emitted as YAML directly, in exactly the shape
``SceneSerializer::Deserialize`` reads. That keeps one source of truth for the
layout (this file) and makes a 1M-entity scene a ~15-second write rather than a
Python interpreter exercise.

The entity block is deliberately the SAME shape as every other scene in
``assets/scenes`` -- three components, no physics, no script, no animation:

    - TagComponent          (required by the hierarchy view, cheap)
    - TransformComponent    (translation / rotation / scale)
    - CubeRendererComponent (the most common renderer in DemoGame: 27,979)
    - MaterialComponent     (the view REQUIRES a material alongside a renderer)

That matters: an entity without a material is skipped by
``Scene::SubmitMeshPrimitives`` and by the cube/sphere submission paths, so a
scene of renderer-only entities would measure nothing.

Usage::

    python tools/gen_entity_block.py --count 100000 --out assets/scenes/Scale100k.wastescene
    python tools/gen_entity_block.py --count 1000  --out -          # stdout

Options::

    --count N        number of entities (default 10000)
    --spacing M      metres between grid cells (default 20.0)
    --side S         world half-extent in metres (default 0 -> derived from count)
    --seed N         RNG seed (default 1337); same seed -> same scene
    --id-base N      first entity UUID (default 9900000000000000000)
    --name-prefix P  entity tag prefix (default "Bulk")
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

# The serializer reads these as <uint64_t> and CreateEntityWithUUID takes a
# UUID, so every id must be a distinct 64-bit value that fits the type. The
# DemoGame scene uses the 9600... range; the generated scenes use 9900... so a
# generated scene can never be mistaken for a hand-authored one.
DEFAULT_ID_BASE = 9_900_000_000_000_000_000

# The grid is a cube of cells. The world is square in XZ and the buildings sit
# on the ground plane at y = height/2, so the camera can be placed above the
# middle of it and see a large fraction of the scene -- which is the point: the
# benchmark must exercise the cull, not a mostly-empty frustum.
GRID_LAYERS = 1


def _num(v: float) -> str:
    """Format a float the way the engine's own YAML writers do.

    ``%g`` rather than ``repr``: the serializer reads these with yaml-cpp's
    ``as<float>()``, which accepts either, but ``%g`` keeps the file compact
    (a 1M-entity scene is ~150 MB at ``repr`` precision).
    """
    return f"{v:g}"


def _vec(v) -> str:
    return "[" + ", ".join(_num(x) for x in v) + "]"


def entity_block(eid: int, tag: str, x: float, y: float, z: float,
                 w: float, h: float, d: float, albedo, metallic: float,
                 roughness: float) -> str:
    """One entity, byte-for-byte the layout SceneSerializer::Deserialize reads.

    Field order matches the writer in SceneSerializer.cpp so a
    load-then-save round trip of a generated scene is stable.
    """
    return (
        f"  - Entity: {eid}\n"
        f"    TagComponent:\n"
        f"      Tag: {tag}\n"
        f"    TransformComponent:\n"
        f"      Translation: {_vec((x, y, z))}\n"
        f"      Rotation: {_vec((0.0, 0.0, 0.0))}\n"
        f"      Scale: {_vec((w, h, d))}\n"
        f"    CubeRendererComponent:\n"
        f"      Color: {_vec(albedo)}\n"
        f"      TextureIndex: 0\n"
        f"      TilingFactor: 1\n"
        f"    MaterialComponent:\n"
        f"      Albedo: {_vec(albedo)}\n"
        f"      TextureIndex: 0\n"
        f"      HasGeneratedMaps: false\n"
        f"      NormalStrength: 0\n"
        f"      Metallic: {_num(metallic)}\n"
        f"      Roughness: {_num(roughness)}\n"
    )


# A fixed palette rather than pure RNG: the benchmark's image must be
# comparable across runs, and a random palette would put different albedos in
# front of the camera depending on the seed.
PALETTE = [
    ((0.30, 0.28, 0.32, 1.0), 0.1, 0.85),
    ((0.16, 0.18, 0.22, 1.0), 0.3, 0.70),
    ((0.44, 0.36, 0.30, 1.0), 0.2, 0.90),
    ((0.22, 0.30, 0.38, 1.0), 0.4, 0.65),
    ((0.52, 0.48, 0.44, 1.0), 0.1, 0.95),
]


def build_scene(count: int, spacing: float, seed: int, id_base: int,
                name_prefix: str, side: float) -> str:
    """Return the complete .wastescene text for ``count`` entities.

    Layout: a square grid in XZ, one entity per cell, each a box of
    ``spacing * 0.6`` on a side and ``spacing * 1.2`` tall, sitting on y=0.

    ``side`` overrides the derived extent when given, so a caller can make a
    deliberately sparse scene (few entities over a large area) to separate
    "too many entities" from "too much world".
    """
    rng = random.Random(seed)

    # A square grid whose area holds `count` cells, rounded up. `cells_per_side`
    # is the square root; the grid is then trimmed back to exactly `count`.
    cells_per_side = max(1, int(count ** 0.5) + (1 if count ** 0.5 % 1 else 0))
    if side > 0.0:
        # Explicit extent: derive spacing so the grid still fills it.
        spacing = (side * 2.0) / max(1, cells_per_side)

    half = (cells_per_side - 1) * spacing / 2.0

    out: list[str] = []
    out.append(f"Scene: {name_prefix} {count}\n")
    out.append("Entities:\n")

    emitted = 0
    for gz in range(cells_per_side):
        for gx in range(cells_per_side):
            if emitted >= count:
                break
            x = -half + gx * spacing
            z = -half + gz * spacing
            # Small per-entity jitter so the scene is not a perfect lattice --
            # a perfect lattice is a degenerate input for a median-split BVH
            # and would flatter the build cost.
            jx = (rng.random() - 0.5) * spacing * 0.20
            jz = (rng.random() - 0.5) * spacing * 0.20
            albedo, metallic, roughness = PALETTE[emitted % len(PALETTE)]
            out.append(entity_block(
                eid=id_base + emitted,
                tag=f"{name_prefix}{emitted}",
                x=x + jx, y=spacing * 0.6, z=z + jz,
                w=spacing * 0.6, h=spacing * 1.2, d=spacing * 0.6,
                albedo=albedo, metallic=metallic, roughness=roughness,
            ))
            emitted += 1
        if emitted >= count:
            break

    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=10000)
    ap.add_argument("--spacing", type=float, default=20.0)
    ap.add_argument("--side", type=float, default=0.0,
                    help="world half-extent in metres; 0 derives it from count")
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--id-base", type=int, default=DEFAULT_ID_BASE)
    ap.add_argument("--name-prefix", default="Bulk")
    ap.add_argument("--out", required=True,
                    help="output path, or '-' for stdout")
    args = ap.parse_args()

    if args.count <= 0:
        print("error: --count must be positive", file=sys.stderr)
        return 2

    text = build_scene(args.count, args.spacing, args.seed, args.id_base,
                       args.name_prefix, args.side)

    if args.out == "-":
        sys.stdout.write(text)
        return 0

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Written in one call rather than line-by-line: a 1M-entity scene is ~200 MB
    # of text and per-line writes make that an order of magnitude slower.
    out_path.write_text(text, encoding="utf-8")
    print(f"wrote {args.count} entities ({len(text) / 1e6:.1f} MB) -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
