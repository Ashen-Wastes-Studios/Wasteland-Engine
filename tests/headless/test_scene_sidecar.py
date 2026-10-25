"""The binary scene sidecar must be a pure cache: never faster-but-wrong.

What the sidecar is
-------------------
``SceneChunkStore`` writes ``<scene>.wlbin`` beside a saved ``.wastescene`` so the next
open can skip yaml-cpp entirely. It is a CACHE, not a format: the YAML stays
authoritative and the sidecar can be deleted at any time.

The risk this test exists for
-----------------------------
A cache that silently returns *slightly different* data is the worst possible outcome --
worse than a slow load, because the scene looks fine and is wrong. So every check here
compares a sidecar load against a YAML load of the same scene field by field, and every
refusal path is asserted to leave the YAML path working.

Covered:
  1. a supported scene gets a sidecar on save, with a sane header;
  2. loading via the sidecar reproduces the YAML load exactly;
  3. a sidecar made stale by editing the YAML is IGNORED, and the load is still correct;
  4. a truncated sidecar is IGNORED, and the load is still correct;
  5. a scene holding a component the sidecar cannot represent gets NO sidecar at all.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import wlpath

wlpath.setup()

import Wasteland  # noqa: E402

from runner import check, check_eq, check_ge  # noqa: E402

ENTITY_COUNT = 40
MAGIC = b"WLSC"

_ENTITY_BLOCK = """    TagComponent:
      Tag: {tag}
    TransformComponent:
      Translation: [{tx}, {ty}, {tz}]
      Rotation: [0.1, 0.2, 0.3]
      Scale: [1, 2, 3]
    CubeRendererComponent:
      Color: [0.1, 0.2, 0.3, 1]
      TextureIndex: 2
      TilingFactor: 4
    MaterialComponent:
      Albedo: [0.4, 0.5, 0.6, 1]
      TextureIndex: 3
      HasGeneratedMaps: false
      NormalStrength: 0.25
      Metallic: 0.5
      Roughness: 0.75
"""


def _asset_dir() -> Path:
    base = Path(wlpath.setup()) / "assets" / "scenes"
    base.mkdir(parents=True, exist_ok=True)
    return base


def _write_scene(path: Path, count: int, extra: str = "") -> None:
    out = ["Scene: SidecarTest\n", "Entities:\n"]
    for i in range(count):
        out.append(f"  - Entity: {9_500_000_000_000_000_000 + i}\n")
        # Vary the values per entity so a sidecar that scrambled the order, dropped a
        # record, or reused one entity's data would show up as a mismatch.
        out.append(_ENTITY_BLOCK.format(
            tag=f"Side{i}", tx=i, ty=i * 2, tz=i * 3))
        if extra:
            out.append(extra)
    path.write_text("".join(out), encoding="utf-8")


def _vec(value) -> tuple:
    """glm vec3/vec4 -> tuple, tolerating whichever binding shape is in play."""
    if hasattr(value, "x"):
        out = [value.x, value.y, value.z]
        if hasattr(value, "w"):
            out.append(value.w)
        return tuple(round(float(v), 5) for v in out)
    return tuple(round(float(v), 5) for v in value)


def _fingerprint(scene) -> list:
    """Every field the sidecar claims to preserve, in a stable order."""
    entities = list(scene.GetAllEntitiesWithTransform())
    rows = []
    for entity in entities:
        tag = getattr(entity, "GetWasteland::TagComponent")().Tag
        transform = getattr(entity, "GetWasteland::TransformComponent")()
        row = {
            "tag": tag,
            "translation": _vec(transform.Translation),
            "rotation": _vec(transform.Rotation),
            "scale": _vec(transform.Scale),
            "has_cube": getattr(entity, "HasWasteland::CubeRendererComponent")(),
            "has_material": getattr(entity, "HasWasteland::MaterialComponent")(),
        }
        if row["has_cube"]:
            cube = getattr(entity, "GetWasteland::CubeRendererComponent")()
            row["cube_color"] = _vec(cube.Color)
            row["cube_texture_index"] = int(cube.TextureIndex)
            row["cube_tiling"] = round(float(cube.TilingFactor), 5)
        if row["has_material"]:
            material = getattr(entity, "GetWasteland::MaterialComponent")()
            row["material_albedo"] = _vec(material.Albedo)
            row["material_texture_index"] = int(material.TextureIndex)
            row["material_normal"] = round(float(material.NormalStrength), 5)
            row["material_metallic"] = round(float(material.Metallic), 5)
            row["material_roughness"] = round(float(material.Roughness), 5)
        rows.append(row)
    rows.sort(key=lambda r: r["tag"])
    return rows


def _sidecar_path(scene_path: Path) -> Path:
    return scene_path.with_suffix(".wlbin")


def _load(path: Path) -> list:
    scene = Wasteland.Scene()
    ok = scene.LoadScene(str(path))
    check(ok, f"LoadScene returned false for {path}")
    return _fingerprint(scene)


def _fresh_scene_path(name: str) -> Path:
    path = _asset_dir() / f"_test_sidecar_{name}.wastescene"
    _sidecar_path(path).unlink(missing_ok=True)
    path.unlink(missing_ok=True)
    return path


def test_supported_scene_round_trips_exactly() -> None:
    """A sidecar load must equal a YAML load, field for field."""
    path = _fresh_scene_path("roundtrip")
    _write_scene(path, ENTITY_COUNT)

    # 1. YAML path: no sidecar exists yet.
    from_yaml = _load(path)
    check_eq(len(from_yaml), ENTITY_COUNT, "entities loaded from YAML")

    # 2. Save, which is what writes the sidecar.
    scene = Wasteland.Scene()
    check(scene.LoadScene(str(path)), "reload before save")
    check(scene.SaveScene(str(path)), "SaveScene returned false")

    sidecar = _sidecar_path(path)
    check(sidecar.exists(), f"expected a sidecar at {sidecar}")
    check_ge(sidecar.stat().st_size, 52 + ENTITY_COUNT * 260,
             "sidecar size covers header plus one record per entity")

    header = sidecar.read_bytes()[:52]
    magic, version, header_size, record_size, count = struct.unpack("<4sIIII", header[:20])
    check_eq(magic, MAGIC, "sidecar magic")
    check_eq(version, 2, "sidecar version")
    check_eq(header_size, 52, "sidecar header size")
    check_eq(record_size, 260, "sidecar record size")
    check_eq(count, ENTITY_COUNT, "sidecar entity count")

    # 3. Now the sidecar is present and valid, so this load takes the fast path.
    from_sidecar = _load(path)

    check_eq(len(from_sidecar), len(from_yaml), "entity count: sidecar vs YAML")
    for index, (a, b) in enumerate(zip(from_yaml, from_sidecar)):
        check_eq(b, a, f"entity {index} ({a['tag']}) differs between YAML and sidecar")


def test_stale_sidecar_is_ignored() -> None:
    """Editing the YAML must invalidate the sidecar, not serve stale data."""
    path = _fresh_scene_path("stale")
    _write_scene(path, ENTITY_COUNT)

    scene = Wasteland.Scene()
    check(scene.LoadScene(str(path)), "initial load")
    check(scene.SaveScene(str(path)), "SaveScene returned false")
    check(_sidecar_path(path).exists(), "sidecar should exist after save")

    # Change the YAML underneath the sidecar. Its recorded size no longer matches, so it
    # must be refused.
    original = path.read_text(encoding="utf-8")
    path.write_text(original.replace("Tag: Side0", "Tag: Renamed"), encoding="utf-8")

    reloaded = _load(path)
    tags = {row["tag"] for row in reloaded}
    check("Renamed" in tags,
          "the edited YAML was not honoured -- a stale sidecar was served")
    check("Side0" not in tags, "stale tag Side0 survived, so the sidecar was used")


def test_truncated_sidecar_is_ignored() -> None:
    """A corrupt sidecar must fall back to YAML rather than load a partial scene."""
    path = _fresh_scene_path("truncated")
    _write_scene(path, ENTITY_COUNT)

    scene = Wasteland.Scene()
    check(scene.LoadScene(str(path)), "initial load")
    check(scene.SaveScene(str(path)), "SaveScene returned false")

    expected = _load(path)
    sidecar = _sidecar_path(path)
    check(sidecar.exists(), "sidecar should exist after save")

    # Keep the header (so magic/version/size all validate) but drop most records.
    data = sidecar.read_bytes()
    sidecar.write_bytes(data[: 52 + 260 * 3])

    recovered = _load(path)
    check_eq(len(recovered), len(expected), "entity count after truncated sidecar")
    for index, (a, b) in enumerate(zip(expected, recovered)):
        check_eq(b, a, f"entity {index} differs after truncated sidecar")


def test_primitive_and_light_components_round_trip() -> None:
    """Cylinder, Sphere and PointLight must survive the sidecar with their own fields.

    These carry fields the cube/material records do not (cylinder Radius/Height/Segments,
    light Intensity/Range/Falloff), so a sidecar that merely set the flag would silently
    load default geometry -- the exact class of lossy-cache bug this file guards.
    """
    path = _fresh_scene_path("primitives")

    # Written as YAML rather than added through Python: entt asserts
    # ("Set does not contain entity") when a component pool the scene never registered is
    # emplaced from the bindings, which is a fixture problem, not a sidecar one.
    blocks = [
        # Cylinder: the shape parameters the cube record does not carry.
        """    CylinderRendererComponent:
      Color: [0.1, 0.2, 0.3, 1]
      TextureWidth: 0
      TextureHeight: 0
      Radius: 0.25
      Height: 7
      Segments: 12
      TextureIndex: 0
      TilingFactor: 1
""",
        # Sphere: carries its own Radius, which an earlier record omitted entirely.
        """    SphereRendererComponent:
      Color: [0.4, 0.5, 0.6, 1]
      TextureWidth: 0
      TextureHeight: 0
      Radius: 0.75
      TextureIndex: 5
      TilingFactor: 3
""",
        """    PointLightComponent:
      Color: [1, 0, 0, 1]
      Intensity: 42
      Range: 99
      Falloff: 1.5
""",
    ]
    out = ["Scene: SidecarPrimitives\n", "Entities:\n"]
    for i, block in enumerate(blocks):
        out.append(f"  - Entity: {9_450_000_000_000_000_000 + i}\n")
        out.append(f"    TagComponent:\n      Tag: Prim{i}\n")
        out.append("    TransformComponent:\n      Translation: [0, 0, 0]\n"
                   "      Rotation: [0, 0, 0]\n      Scale: [1, 1, 1]\n")
        out.append(block)
    path.write_text("".join(out), encoding="utf-8")

    scene = Wasteland.Scene()
    check(scene.LoadScene(str(path)), "load primitives scene")
    check_eq(len(list(scene.GetAllEntitiesWithTransform())), 3, "primitives entity count")

    check(scene.SaveScene(str(path)), "SaveScene returned false")
    check(_sidecar_path(path).exists(),
          "a scene of primitives plus a light should be representable in a sidecar")

    reloaded = Wasteland.Scene()
    check(reloaded.LoadScene(str(path)), "reload via sidecar")
    by_tag = {}
    for entity in reloaded.GetAllEntitiesWithTransform():
        by_tag[getattr(entity, "GetWasteland::TagComponent")().Tag] = entity

    cyl = getattr(by_tag["Prim0"], "GetWasteland::CylinderRendererComponent")()
    check_eq(round(float(cyl.Radius), 5), 0.25, "cylinder Radius")
    check_eq(round(float(cyl.Height), 5), 7.0, "cylinder Height")
    check_eq(int(cyl.Segments), 12, "cylinder Segments")

    sph = getattr(by_tag["Prim1"], "GetWasteland::SphereRendererComponent")()
    check_eq(round(float(sph.Radius), 5), 0.75, "sphere Radius")
    check_eq(int(sph.TextureIndex), 5, "sphere TextureIndex")
    check_eq(round(float(sph.TilingFactor), 5), 3.0, "sphere TilingFactor")

    lit = getattr(by_tag["Prim2"], "GetWasteland::PointLightComponent")()
    check_eq(round(float(lit.Intensity), 5), 42.0, "light Intensity")
    check_eq(round(float(lit.Range), 5), 99.0, "light Range")
    check_eq(round(float(lit.Falloff), 5), 1.5, "light Falloff")


def test_unsupported_component_becomes_a_fragment() -> None:
    """A scene the fast records cannot fully hold must still get a sidecar.

    The sidecar is now total: an entity carrying a component outside the supported set is
    stored as its authored YAML and replayed through the same reader on load. Refusing the
    whole scene (the old behaviour) left every complex scene on the slow path forever.

    This asserts the split, not just the outcome: the plain entities must land in fast
    records and the camera-bearing one must land in a fragment. Checking only "the scene
    reloads" would pass even if the camera entity were silently dropped from the sidecar
    and re-added by the YAML fallback -- which is the bug this format exists to avoid.
    """
    path = _fresh_scene_path("fragment")

    # Written directly rather than via _write_scene: that helper applies `extra` to EVERY
    # entity, which would fragment all five and test nothing about the split. Here exactly
    # one entity carries the camera, so the other four must land in fast records.
    #
    # CameraComponent goes in as YAML rather than through the Python bindings: emplacing a
    # component whose pool the scene never registered trips an entt assertion, which is a
    # fixture problem rather than a sidecar one.
    out = ["Scene: SidecarFragment\n", "Entities:\n"]
    for i in range(5):
        out.append(f"  - Entity: {9_510_000_000_000_000_000 + i}\n")
        out.append(_ENTITY_BLOCK.format(tag=f"Frag{i}", tx=i, ty=i * 2, tz=i * 3))
        if i == 4:
            out.append("    CameraComponent:\n      Camera:\n        ProjectionType: 0\n")
    path.write_text("".join(out), encoding="utf-8")

    scene = Wasteland.Scene()
    check(scene.LoadScene(str(path)), "load scene with a camera")
    check_eq(len(list(scene.GetAllEntitiesWithTransform())), 5, "plain scene entity count")

    check(scene.SaveScene(str(path)), "SaveScene returned false")

    sidecar = _sidecar_path(path)
    check(sidecar.exists(),
          "no sidecar was written for a scene holding CameraComponent; an entity the "
          "records cannot hold should become a fragment, not suppress the whole sidecar")

    # Header: magic, version, header_size, record_size, entity_count, yaml_size,
    # yaml_write_time, fragment_count, fragment_bytes, reserved.
    header = sidecar.read_bytes()[:52]
    (magic, version, header_size, record_size, entity_count,
     _yaml_size, _yaml_time, fragment_count, _frag_bytes, _reserved) = struct.unpack(
        "<4sIIIIQQIQI", header)

    check_eq(magic, MAGIC, "fragment sidecar magic")
    check_eq(version, 2, "fragment sidecar version")
    check_eq(header_size, 52, "fragment sidecar header size")
    check_eq(record_size, 260, "fragment sidecar record size")
    check_eq(entity_count, 4,
             "the four plain entities should be fast records")
    check_eq(fragment_count, 1,
             "the camera entity should be the only fragment")

    # And the reload must reproduce all five entities, camera included.
    reloaded = Wasteland.Scene()
    check(reloaded.LoadScene(str(path)), "reload via sidecar with a fragment")
    entities = list(reloaded.GetAllEntitiesWithTransform())
    check_eq(len(entities), 5, "entity count after fragment replay")

    has_camera = [e for e in entities
                  if getattr(e, "HasWasteland::CameraComponent")()]
    check_eq(len(has_camera), 1,
             "the camera entity did not survive the fragment replay")
