"""C.4 -- glTF PBR -> MaterialComponent, and the .wlmaterial asset.

WHAT THIS COVERS
----------------
  * .wlmaterial save/load round trip, field by field.
  * Missing fields keep their DEFAULT, not zero -- the defect class the
    SceneSerializer tests already caught.
  * A wrong-typed field fails the load with an error instead of silently
    defaulting.
  * The MaterialAssetCache: an empty path is a no-op, a failure is cached, and
    a repeated Get does not re-parse.
  * glTF material parsing: every factor the plan names, the glTF DEFAULTS when
    a factor is absent, a baseColorTexture that is a URI (path recorded), one
    that is an embedded bufferView (flag set, no path), and a file with no
    materials at all.

WHAT THIS DOES NOT COVER
------------------------
No render. The editor cannot start on this machine (DX12 device-removed at
OnAttach), so nothing here proves a material was APPLIED to a drawn surface.
`MaterialPath` overriding the entity's own MaterialComponent is exercised in
test_mesh_component.py only as far as the override being READ.

The material/texture gap is deliberate: a glTF image is either a URI or an
embedded bufferView, and extracting the latter needs a PNG encoder the project
does not vend (stb_image_write.h exists only inside GLFW's own deps). A
textured material therefore imports with its parameters and no albedo texture;
importing it as flat white would be the worse lie.
"""

import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bin", "Release-windows-x86_64",
                                "DemonCore-Editor"))
sys.path.insert(0, HERE)

import Wasteland


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _gltf_json(materials, images=None):
    """A minimal legal glTF 2.0 document carrying only materials.

    The importer validates that `accessors` is present and an array BEFORE it
    looks at anything else, so a materials-only fixture must still declare the
    accessor/bufferView/buffer arrays -- empty is enough, since these tests
    assert on materials and not on geometry.
    """
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": []}],
        "nodes": [],
        # `meshes` must be a NON-EMPTY array -- the importer rejects an empty
        # one before it reaches the materials. The primitive below references
        # a POSITION accessor so the mesh stage has something to read; these
        # tests assert on materials and not on geometry.
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "buffers": [{"byteLength": 36}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
        "accessors": [{"bufferView": 0, "componentType": 5126,
                       "count": 3, "type": "VEC3"}],
        "materials": materials,
    }
    if images is not None:
        doc["images"] = images
    return doc


import struct

# Three VEC3 floats (36 bytes) for the fixture's POSITION accessor. The
# importer validates that an accessor does not read past its buffer, so a
# materials-only fixture must still carry real binary data.
_BINARY_CHUNK = struct.pack("<9f", 0.0, 0.0, 0.0,
                                     1.0, 0.0, 0.0,
                                     0.0, 1.0, 0.0)


def _import(text, binary=_BINARY_CHUNK):
    return Wasteland.Mesh.ImportGltfFromText(json.dumps(text), binary)


def _write_material(path, material):
    ok, error = Wasteland.MaterialSerializer.Save(str(path), material)
    assert ok, error
    return path


def _material(**overrides):
    """A MaterialComponent with distinguishable, non-default values."""
    material = Wasteland.MaterialComponent()
    material.Albedo = Wasteland.Vec4(0.25, 0.5, 0.75, 1.0)
    material.Metallic = 0.1
    material.Roughness = 0.2
    material.NormalStrength = 0.3
    material.EmissionColor = Wasteland.Vec4(0.4, 0.5, 0.6, 1.0)
    material.EmissionIntensity = 1.5
    material.DisplacementScale = 2.5
    material.TexturePath = "assets/mat_albedo.png"
    for name, value in overrides.items():
        setattr(material, name, value)
    return material


def _approx_list(values, expected, tolerance=1e-6):
    """Approximate comparison for glTF factors.

    A glTF factor is a JSON double, narrowed to a float on the way in. 0.6 is
    not representable in float32 -- it becomes 0.6000000238418579 -- so an exact
    equality test against the literal 0.6 fails on a CORRECT parse. 0.25, 0.5
    and 0.75 happen to be exact, which is why an exact test can pass on one
    field and fail on the next.
    """
    return all(abs(a - b) < tolerance for a, b in zip(values, expected))


def _vec4_eq(a, b, tolerance=1e-6):
    """Component-wise vec4 comparison.

    Vec4 has NO value equality: `Vec4(0.25,0.5,0.75,1) == Vec4(0.25,0.5,0.75,1)`
    is False, because pybind does not generate __eq__ and the inherited
    object.__eq__ is an identity test. Every colour assertion here must go
    through this helper or it is comparing object identity.
    """
    return (abs(a.x - b.x) < tolerance and abs(a.y - b.y) < tolerance
            and abs(a.z - b.z) < tolerance and abs(a.w - b.w) < tolerance)


# ---------------------------------------------------------------------------
# 1. .wlmaterial round trip
# ---------------------------------------------------------------------------

def test_a_material_survives_a_file_round_trip():
    """Every scalar field must survive. A silent drop here means a material
    authored in the editor renders differently after a reload."""
    material = _material()
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "nested", "hero.wlmaterial")
        ok, error = Wasteland.MaterialSerializer.Save(path, material)
        assert ok, error
        assert os.path.isfile(path), "file was not written"

        loaded = Wasteland.MaterialComponent()
        ok, loaded, error = Wasteland.MaterialSerializer.Load(path)
        assert ok, error

    assert _vec4_eq(loaded.Albedo, Wasteland.Vec4(0.25, 0.5, 0.75, 1.0))
    assert abs(loaded.Metallic - 0.1) < 1e-6
    assert abs(loaded.Roughness - 0.2) < 1e-6
    assert abs(loaded.NormalStrength - 0.3) < 1e-6
    assert abs(loaded.EmissionIntensity - 1.5) < 1e-6
    assert abs(loaded.DisplacementScale - 2.5) < 1e-6
    assert loaded.TexturePath == "assets/mat_albedo.png"


def test_emit_and_parse_agree(tmp_path=None):
    """Emit is the text half of the same format. If they disagree, a tool that
    previews a save and the serializer itself would produce different files."""
    material = _material()
    text = Wasteland.MaterialSerializer.Emit(material)
    assert isinstance(text, str) and text.strip(), "Emit returned no text"

    ok, parsed, error = Wasteland.MaterialSerializer.Parse(text)
    assert ok, error
    assert abs(parsed.Roughness - 0.2) < 1e-6
    assert abs(parsed.Metallic - 0.1) < 1e-6
    assert parsed.TexturePath == "assets/mat_albedo.png"


# ---------------------------------------------------------------------------
# 2. missing fields keep their default, not zero
# ---------------------------------------------------------------------------

def test_a_field_absent_from_the_file_keeps_its_default():
    """The defect the SceneSerializer tests caught: `if node["Albedo"]:
    material.Albedo = ... else material.Albedo = {}` -- a missing field
    resetting the colour to black instead of leaving it alone. A .wlmaterial
    authored before a field existed must not silently become black."""
    authored = Wasteland.MaterialComponent()
    authored.Albedo = Wasteland.Vec4(0.9, 0.8, 0.7, 1.0)
    authored.Roughness = 0.4

    # Hand-written file with ONLY Roughness. Albedo, Metallic, NormalStrength,
    # EmissionColor, EmissionIntensity, DisplacementScale and TexturePath are
    # all absent.
    text = "Roughness: 0.4\n"

    loaded = Wasteland.MaterialComponent()
    loaded.Albedo = Wasteland.Vec4(0.9, 0.8, 0.7, 1.0)
    ok, loaded, error = Wasteland.MaterialSerializer.Parse(text, loaded)
    assert ok, error

    assert _vec4_eq(loaded.Albedo, Wasteland.Vec4(0.9, 0.8, 0.7, 1.0)), \
        "absent Albedo was reset instead of kept"
    assert abs(loaded.Roughness - 0.4) < 1e-6, "present field was not read"
    assert loaded.Metallic == 0.0, \
        "absent Metallic should keep the component's default (0), not something else"


def test_a_field_with_the_wrong_type_fails_the_load():
    """A wrong-typed field must FAIL, not default. A material that loaded with
    a silently defaulted colour is worse than one that failed: the author sees
    a different material and no error."""
    text = "Albedo: not_a_colour\n"
    ok, _, error = Wasteland.MaterialSerializer.Parse(text)
    assert not ok, "a non-vec4 Albedo was accepted"
    assert error, "the failure carried no reason"


def test_malformed_yaml_fails_the_load():
    ok, _, error = Wasteland.MaterialSerializer.Parse("Albedo: [1, 2, 3\n")
    assert not ok
    assert error


def test_an_empty_file_fails_the_load():
    ok, _, error = Wasteland.MaterialSerializer.Parse("")
    assert not ok
    assert error


def test_loading_a_missing_file_fails_with_a_reason():
    ok, _, error = Wasteland.MaterialSerializer.Load(
        os.path.join(tempfile.gettempdir(), "wasteland_no_such_material.wlmaterial"))
    assert not ok
    assert error


def test_saving_to_an_unwritable_path_fails_rather_than_raising():
    """A material save is a user action; a full disk is user data, not a crash."""
    ok, error = Wasteland.MaterialSerializer.Save(
        os.path.join("Z:", "definitely", "not", "writable", "m.wlmaterial"),
        Wasteland.MaterialComponent())
    assert not ok
    assert error


# ---------------------------------------------------------------------------
# 3. the material cache
# ---------------------------------------------------------------------------

def test_an_empty_material_path_is_a_no_op():
    """The common case: MeshComponent is added before an asset is chosen. An
    empty path must not resolve to some accidental file."""
    cache = Wasteland.MaterialAssetCache()
    material, error = cache.Get("")
    assert material is None
    assert error == ""
    assert cache.Count == 0, "an empty path was cached as a load"


def test_a_missing_material_is_cached_as_a_failure():
    """Re-opening a missing file every frame is an unbounded cost; a failure is
    cached with its reason so a second Get does not touch the filesystem."""
    cache = Wasteland.MaterialAssetCache()
    material, error = cache.Get("assets/definitely_missing.wlmaterial")
    assert material is None
    assert error, "a failed load carried no reason"

    assert cache.Count == 1, "the failure was not cached"

    material_again, error_again = cache.Get("assets/definitely_missing.wlmaterial")
    assert material_again is None
    assert error_again == error, "the cached reason changed between reads"


def test_a_material_is_loaded_once_per_path():
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "hero.wlmaterial")
        _write_material(path, _material())

        cache = Wasteland.MaterialAssetCache()
        first, error = cache.Get(path)
        assert first is not None, error
        assert cache.Count == 1

        second, _ = cache.Get(path)
        assert second is not None
        assert cache.Count == 1, "a second Get re-loaded the file"

    assert abs(first.Roughness - 0.2) < 1e-6


def test_clear_resets_the_cache():
    cache = Wasteland.MaterialAssetCache()
    cache.Get("assets/definitely_missing.wlmaterial")
    assert cache.Count == 1
    cache.Clear()
    assert cache.Count == 0


# ---------------------------------------------------------------------------
# 4. glTF material parsing
# ---------------------------------------------------------------------------

def test_a_gltf_material_maps_onto_the_engine_fields():
    """baseColorFactor, metallicFactor, roughnessFactor, emissiveFactor and
    normalTexture.scale are the six fields the plan names."""
    doc = _gltf_json([{
        "name": "Hero",
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.25, 0.5, 0.75, 1.0],
            "metallicFactor": 0.3,
            "roughnessFactor": 0.4,
        },
        "emissiveFactor": [0.6, 0.7, 0.8],
        "normalTexture": {"index": 0, "scale": 0.9},
    }], images=[{"uri": "textures/hero_basecolor.png"}])

    result = _import(doc)
    assert result.Error == "", result.Error
    assert len(result.Materials) == 1

    material = result.Materials[0]
    assert material.Name == "Hero"
    assert _approx_list(
        [material.BaseColorFactor.x, material.BaseColorFactor.y,
         material.BaseColorFactor.z, material.BaseColorFactor.w],
        [0.25, 0.5, 0.75, 1.0])
    assert abs(material.MetallicFactor - 0.3) < 1e-6
    assert abs(material.RoughnessFactor - 0.4) < 1e-6
    assert _approx_list(
        [material.EmissiveFactor.x, material.EmissiveFactor.y,
         material.EmissiveFactor.z], [0.6, 0.7, 0.8])
    assert abs(material.NormalScale - 0.9) < 1e-6
    assert material.HasNormalTexture


def test_gltf_defaults_apply_when_factors_are_absent():
    """glTF's defaults are opaque white, metallic 1.0 and roughness 1.0. A file
    that declares a material and names no factors is a valid metallic white
    surface -- not a black dielectric."""
    result = _import(_gltf_json([{}]))
    assert result.Error == "", result.Error
    assert len(result.Materials) == 1

    material = result.Materials[0]
    assert _approx_list(
        [material.BaseColorFactor.x, material.BaseColorFactor.y,
         material.BaseColorFactor.z, material.BaseColorFactor.w],
        [1.0, 1.0, 1.0, 1.0])
    assert abs(material.MetallicFactor - 1.0) < 1e-6, \
        "absent metallicFactor must default to glTF's 1.0"
    assert abs(material.RoughnessFactor - 1.0) < 1e-6
    assert _approx_list(
        [material.EmissiveFactor.x, material.EmissiveFactor.y,
         material.EmissiveFactor.z], [0.0, 0.0, 0.0])
    assert material.AlphaMode == "OPAQUE"


def test_a_base_color_texture_with_a_uri_is_recorded():
    """A URI image is resolvable, so the material can point at the file. The
    texture is NOT loaded here -- importing is a parse, not an asset pull."""
    doc = _gltf_json([{
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
        },
    }], images=[{"uri": "textures/hero_basecolor.png"}])

    result = _import(doc)
    assert result.Error == "", result.Error
    material = result.Materials[0]
    assert material.HasBaseColorTexture
    assert material.AlbedoTexturePath == "textures/hero_basecolor.png"


def test_an_embedded_image_is_flagged_but_has_no_path():
    """A bufferView image would need PNG encoding to become an asset, which v1
    does not do. The flag says a texture existed; the empty path says it could
    not be pointed at. Importing it as flat white would hide the gap."""
    doc = _gltf_json([{
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
        },
    }], images=[{"bufferView": 0, "mimeType": "image/png"}])

    result = _import(doc)
    assert result.Error == "", result.Error
    material = result.Materials[0]
    assert material.HasBaseColorTexture, "an embedded texture was not detected"
    assert material.AlbedoTexturePath == "", \
        "an embedded image cannot be resolved to a file path"


def test_a_non_opaque_material_is_recorded_not_flattened():
    """MaterialComponent has no alpha-mode field. A BLEND material flattened to
    opaque would render wrong in a way nothing downstream could detect, so the
    mode is carried on the import result."""
    result = _import(_gltf_json([{"alphaMode": "BLEND"}]))
    assert result.Error == "", result.Error
    assert result.Materials[0].AlphaMode == "BLEND"


def test_a_file_with_no_materials_imports_fine():
    """A mesh with no materials is legal glTF and means every surface renders
    with the entity's own MaterialComponent. The import must still succeed."""
    result = _import(_gltf_json([]))
    assert result.Error == "", result.Error
    assert result.Materials == [] or len(result.Materials) == 0


def test_materials_are_in_file_order():
    """A mesh primitive names a material by INDEX. Reordering or merging them
    would apply the wrong material to part of a surface."""
    doc = _gltf_json([
        {"name": "first"},
        {"name": "second"},
        {"name": "third"},
    ])
    result = _import(doc)
    assert result.Error == "", result.Error
    names = [m.Name for m in result.Materials]
    assert names == ["first", "second", "third"]


def test_a_malformed_material_fails_the_import_with_a_reason():
    """A materials entry that is not an object is a corrupt file; silently
    skipping it would import a mesh with a material the author did not see."""
    doc = _gltf_json(["not an object"])
    result = _import(doc)
    assert result.Error != ""
    assert "materials[0]" in result.Error


def test_a_material_error_does_not_lose_the_mesh():
    """Materials are parsed AFTER the mesh, so a bad material must not discard
    geometry that was already read correctly."""
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": []}],
        "nodes": [],
        "buffers": [{"byteLength": 36}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
        "accessors": [{"bufferView": 0, "componentType": 5126,
                       "count": 3, "type": "VEC3"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "materials": [{"name": "ok"}, "corrupt"],
    }
    result = _import(doc, binary=_BINARY_CHUNK)
    assert result.Error != "", "the corrupt material was not reported"
    assert result.Mesh.VertexCount == 3, \
        "a material error discarded the mesh"


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main():
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

    checks = 0
    failed = 0
    for name, test in tests:
        try:
            test()
            print(f"  {name} ...")
        except AssertionError as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {error}")
        except Exception as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {type(error).__name__}: {error}")
        checks += 1

    print(f"\n{len(tests) - failed}/{len(tests)} tests passed")
    print(f"{checks - failed}/{checks} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
