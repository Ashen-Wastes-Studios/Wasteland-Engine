"""Scene load must not pay a per-entity cost that grows with the probe list.

The defect this guards
----------------------
``SceneSerializer::Deserialize`` iterated entities with a **mutable**
``YAML::Node``::

    for (auto entity : entities)          // mutable -> non-const operator[]
        ...
        if (entity["HealthComponent"])    // 25 such probes per entity
        if (entity["WeaponComponent"])
        ...

yaml-cpp's non-const ``Node::operator[]`` routes to the non-const
``node_data::get()``, which on a **missing** key creates a node and **inserts it
into the map** (``vendor/yaml-cpp/src/node_data.cpp:242-244``). The const
overload (``include/yaml-cpp/node/impl.h:325``) returns a ZombieNode and inserts
nothing. ``node_data::get()`` is a linear scan over ``m_map``, so each phantom
insertion also made every later probe scan a longer map — an entity's map grew
from its real ~5 keys to ~26 while being read.

A typical entity carries 4 of the 25 probed component types, so ~21 probes per
entity were pure insertion. Measured cost: ~109 us/entity of overhead that was
independent of the entity's actual content. At 1M entities that is ~21M wasted
nodes.

Why the test asserts TIME rather than structure
-----------------------------------------------
The phantom keys never reach disk: the writer enumerates components through
entt (``HasComponent``), not through the YAML node, so a load-then-save round
trip is byte-identical either way. There is no structural artefact to assert on,
which is exactly why the defect survived — it was invisible to every round-trip
test. Per-entity load time is the only observable.

The bound is set from measurement, not guessed:

    before the const fix : 296 us/entity (20k entities, 4 components each)
    after  the const fix : 175 us/entity
    bound here           : 250 us/entity

250 sits between the two with headroom on both sides: machine noise would have
to exceed +43% to false-fail, and the bug would have to shrink by 70% to
false-pass.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import wlpath

wlpath.setup()

import Wasteland  # noqa: E402

from runner import check, check_eq, check_le  # noqa: E402

# 20k keeps the test at a few seconds while still being far enough above the
# fixed costs (file open, YAML document setup) for the per-entity term to
# dominate. Below ~2k the per-entity measurement is mostly noise.
ENTITY_COUNT = 20_000

# Per-entity load budget. See the module docstring for how this number was set.
MAX_US_PER_ENTITY = 250.0

# Same 4-component shape as a real renderable entity, so the test measures the
# path scenes actually take rather than a degenerate one.
_ENTITY_BLOCK = """    TagComponent:
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
"""


def _write_scene(path: Path, count: int) -> None:
    """Emit a scene in exactly the layout SceneSerializer::Deserialize reads."""
    out = ["Scene: LoadScaleTest\n", "Entities:\n"]
    for i in range(count):
        out.append(f"  - Entity: {9_600_000_000_000_000_000 + i}\n")
        out.append(_ENTITY_BLOCK.format(tag=f"Load{i}"))
    path.write_text("".join(out), encoding="utf-8")


def _scene_path() -> Path:
    """Where to write the fixture: the asset dir the module already resolves.

    ``wlpath.setup()`` returns the directory holding ``Wasteland.pyd``, which is
    the editor's bin dir and therefore has an ``assets/scenes`` folder next to
    it. Writing there means ``LoadScene`` takes the same path as a real scene.
    """
    base = Path(wlpath.setup()) / "assets" / "scenes"
    base.mkdir(parents=True, exist_ok=True)
    return base / f"_test_load_scale_{ENTITY_COUNT}.wastescene"


def test_load_is_linear_and_correct() -> None:
    """The scene loads, every entity arrives, and no component is lost.

    Correctness first: the const fix must not change *what* loads, only how
    fast. A speed-up that silently drops the MaterialComponent would pass a
    timing-only test.
    """
    path = _scene_path()
    _write_scene(path, ENTITY_COUNT)

    scene = Wasteland.Scene()
    ok = scene.LoadScene(str(path))
    check(ok, f"LoadScene returned false for {path}")

    entities = scene.GetAllEntitiesWithTransform()
    check_eq(len(entities), ENTITY_COUNT, "entity count after load")

    # Sample the first and last entity: a reader that bailed early or dropped
    # the tail would still report the right count if it over-allocated, so both
    # ends are checked, and components are checked on a real entity rather than
    # inferred from the count.
    for label, entity in (("first", entities[0]), ("last", entities[-1])):
        for comp, accessor in (
            ("CubeRendererComponent", "HasWasteland::CubeRendererComponent"),
            ("MaterialComponent", "HasWasteland::MaterialComponent"),
            ("TagComponent", "HasWasteland::TagComponent"),
        ):
            check(getattr(entity, accessor)(),
                  f"{label} entity is missing {comp} after load")


def test_load_per_entity_cost_is_bounded() -> None:
    """Loading must not regress into per-entity phantom-node insertion.

    This is the regression guard for the mutable-YAML::Node defect. Before the
    const fix a 20k-entity scene cost ~296 us/entity; after, ~175. The bound is
    250, so this fails if the insertion behaviour returns.
    """
    path = _scene_path()
    if not path.exists():
        _write_scene(path, ENTITY_COUNT)

    # Best of 3: a single sample on a machine doing anything else can be an
    # outlier in either direction, and this is a lower bound on the cost.
    best_ms = float("inf")
    for _ in range(3):
        scene = Wasteland.Scene()
        t0 = time.perf_counter()
        ok = scene.LoadScene(str(path))
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        check(ok, f"LoadScene returned false for {path}")
        best_ms = min(best_ms, elapsed_ms)

    us_per_entity = best_ms * 1000.0 / ENTITY_COUNT
    check_le(us_per_entity, MAX_US_PER_ENTITY,
             f"per-entity load cost, {ENTITY_COUNT} entities in {best_ms:.0f} ms "
             f"(a mutable YAML::Node loop variable re-inserts absent component "
             f"keys; see the module docstring)")
