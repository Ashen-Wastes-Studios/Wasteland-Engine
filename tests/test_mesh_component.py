"""Task C.3: `MeshComponent` and the renderer path.

The plan's verification is "screenshot of a glTF cube in the editor viewport".
That is NOT possible on this machine: the editor dies at startup with a DX12
device-removed (0x887a0006) inside ClearAccumulationBuffers during OnAttach,
with an nvlddmkm event 153. No panel has ever been verified on screen.

So this test verifies everything UP TO the draw call, and says so. What it
covers:

  * MeshData -> ProceduralMesh, the conversion an imported mesh cannot be
    drawn without (the two layouts are not interchangeable).
  * MeshAssetCache, the per-scene path -> mesh table: loads once, caches a
    FAILURE rather than re-parsing a missing asset every frame.
  * MeshComponent attach/detach, and a YAML round trip.

What it does NOT cover is the actual rasterisation. A clean build is not
evidence of a drawn triangle, and this file does not pretend otherwise.
"""
import os
import sys
import tempfile

# This file lives directly in tests/, so the repo root is ONE level up.
_ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..")))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_MODULE_DIR = os.path.join(_ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor")
_SCRIPTS = os.path.join(_ROOT, "DemonCore-Editor", "assets", "scripts")
for _path in (_MODULE_DIR, _SCRIPTS):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import Wasteland  # noqa: E402

Mesh = Wasteland.Mesh
W = Wasteland

FAILED = []
PASSED = 0


def check(condition, message):
    global PASSED
    if condition:
        PASSED += 1
    else:
        FAILED.append(message)
        print("    FAIL:", message)


def check_eq(actual, expected, message):
    check(actual == expected, "%s (got %r, want %r)" % (message, actual, expected))


def check_near(actual, expected, tolerance, message):
    check(abs(actual - expected) <= tolerance,
          "%s (got %r, want %r +/- %r)" % (message, actual, expected, tolerance))


# ---------------------------------------------------------------- fixtures

# A unit quad in the XZ plane, two triangles, wound so the normal is +Y.
# Normals are SUPPLIED (vn lines) so the test does not depend on the OBJ
# loader's normal synthesis -- this test is about the conversion.
QUAD_OBJ = """\
v -0.5 0.0 -0.5
v  0.5 0.0 -0.5
v  0.5 0.0  0.5
v -0.5 0.0  0.5
vn 0.0 1.0 0.0
f 1//1 2//1 3//1
f 1//1 3//1 4//1
"""

# The same quad with NO normals at all, for the generated-normal case.
QUAD_NO_NORMALS = """\
v -0.5 0.0 -0.5
v  0.5 0.0 -0.5
v  0.5 0.0  0.5
v -0.5 0.0  0.5
f 1 2 3
f 1 3 4
"""

# A quad whose supplied normal is literally (0, 0, 0). The OBJ loader
# synthesises normals for a face that has none, but a face that NAMES a normal
# gets that normal verbatim -- and a zero one would normalise to NaN in the
# raster batch, blacking out that triangle. This is the input the conversion's
# fallback exists for.
QUAD_ZERO_NORMAL = """\
v -0.5 0.0 -0.5
v  0.5 0.0 -0.5
v  0.5 0.0  0.5
v -0.5 0.0  0.5
vn 0.0 0.0 0.0
f 1//1 2//1 3//1
f 1//1 3//1 4//1
"""


def _parse(text, virtual_path):
    ok, data, error = Mesh.ParseOBJ(text, virtual_path)
    check(ok, "OBJ fixture must parse: %s" % error)
    return data


def _write_temp(name, text):
    path = os.path.join(tempfile.gettempdir(), name)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


# ---------------------------------------------------------------- conversion

def test_build_procedural_mesh_preserves_geometry():
    data = _parse(QUAD_OBJ, "quad.obj")
    ok, mesh, error = Mesh.BuildProceduralMesh(data)
    check(ok, "conversion must succeed: %s" % error)

    # MeshData is DE-INDEXED: two triangles is SIX corners, not four.
    check_eq(len(mesh.Positions), 6, "six de-indexed corners")
    check_eq(len(mesh.Normals), 6, "one normal per corner")
    check_eq(len(mesh.Indices), 6, "two triangles of indices")
    check(not mesh.Empty(), "a mesh with triangles is not Empty")

    # The first corner is the quad's (-0.5, 0, -0.5) vertex.
    first = mesh.Positions[0]
    check_near(first.x, -0.5, 1e-6, "first corner x")
    check_near(first.y, 0.0, 1e-6, "first corner y")
    check_near(first.z, -0.5, 1e-6, "first corner z")

    # Every normal is the supplied +Y, not a recomputed face normal. A
    # conversion that recomputed them would flatten the quad's shading.
    for i, normal in enumerate(mesh.Normals):
        check_near(normal.x, 0.0, 1e-6, "normal %d x is +Y" % i)
        check_near(normal.y, 1.0, 1e-6, "normal %d y is +Y" % i)
        check_near(normal.z, 0.0, 1e-6, "normal %d z is +Y" % i)

    # Indices are 0..5 in order for this fixture, but the conversion must not
    # ASSUME that -- only that they address the corner array.
    for i, index in enumerate(mesh.Indices):
        check(0 <= index < 6, "index %d addresses a real corner" % i)


def test_a_zero_length_normal_becomes_positive_y():
    """A normal of (0,0,0) would normalize to NaN and black out one triangle.

    The raster batch normalizes on the CPU per draw, and a zero vector
    normalizes to NaN, which propagates into the shader as a black pixel on
    exactly that triangle -- the kind of defect that looks like a lighting bug
    rather than a data bug.
    """
    data = _parse(QUAD_ZERO_NORMAL, "quad_zero.obj")
    ok, mesh, error = Mesh.BuildProceduralMesh(data)
    check(ok, "conversion must succeed: %s" % error)

    check_eq(len(mesh.Normals), 6, "every corner still gets a normal")
    for i, normal in enumerate(mesh.Normals):
        check_near(normal.x, 0.0, 1e-6, "normal %d x replaced" % i)
        check_near(normal.y, 1.0, 1e-6, "normal %d y replaced with +Y" % i)
        check_near(normal.z, 0.0, 1e-6, "normal %d z replaced" % i)


def test_a_face_with_no_normal_gets_one_generated():
    """A mesh with no normals at all must still convert, with unit normals.

    Most exported OBJs omit normals. The loader synthesises them from the
    winding, so the conversion must never see an empty normal array -- but it
    is worth asserting the RESULT is usable rather than merely present.
    """
    data = _parse(QUAD_NO_NORMALS, "quad_nonorm.obj")
    ok, mesh, error = Mesh.BuildProceduralMesh(data)
    check(ok, "conversion must succeed: %s" % error)
    check_eq(len(mesh.Normals), 6, "a normal per corner")
    for i, normal in enumerate(mesh.Normals):
        length = (normal.x ** 2 + normal.y ** 2 + normal.z ** 2) ** 0.5
        check_near(length, 1.0, 1e-5,
                   "generated normal %d is unit length (no NaN)" % i)


# Vertices but NO faces. The parser produces a MeshData with positions and an
# empty index list, which is exactly the input the "no triangles" guard exists
# for -- and it cannot be built from Python, because MeshData.Vertices and
# .Indices are copy-out properties with no setter (a deliberate C.1 decision).
VERTICES_ONLY_OBJ = """\
v -0.5 0.0 -0.5
v  0.5 0.0 -0.5
v  0.5 0.0  0.5
v -0.5 0.0  0.5
vn 0.0 1.0 0.0
"""


def test_a_mesh_with_no_triangles_is_rejected_not_padded():
    """A mesh with no triangles draws nothing and must not convert.

    Padding it with a degenerate triangle would produce a draw of zero area --
    harmless to the GPU but a lie about what the asset contains.

    The fixture is a glTF whose POSITION accessor has count 0 and NO
    bufferView, so the importer zero-fills zero components and produces a
    mesh with neither vertices nor triangles. An OBJ cannot express this (the
    loader correctly rejects a file with no 'f' records), and MeshData's
    copy-out properties mean Python cannot build one by hand -- so this is the
    only route to the guard.
    """
    import json

    gltf = {
        "asset": {"version": "2.0"},
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "accessors": [{"componentType": 5126, "count": 0, "type": "VEC3"}],
    }
    result = Mesh.ImportGltfFromText(json.dumps(gltf), b"", ".")
    check(result.Ok(), "an empty glTF must still import: %s" % result.Error)
    check_eq(len(result.Deindexed.Indices), 0, "the fixture has no triangles")

    ok, mesh, error = Mesh.BuildProceduralMesh(result.Deindexed)
    check(not ok, "an index-less mesh must be rejected")
    check("no triangles" in error, "the reason names the missing triangles")

    # A mesh with a zero-length normal is NOT rejected -- it is repaired, so a
    # single bad corner cannot discard a whole asset.
    data = _parse(QUAD_ZERO_NORMAL, "quad_zero.obj")
    ok, mesh, error = Mesh.BuildProceduralMesh(data)
    check(ok, "a zero normal is repaired, not rejected: %s" % error)


# ---------------------------------------------------------------- cache

def test_the_cache_loads_a_path_once():
    path = _write_temp("wasteland_c3_quad.obj", QUAD_OBJ)
    cache = Mesh.MeshAssetCache()

    check_eq(cache.Count, 0, "a fresh cache is empty")

    mesh, error = cache.Get(path)
    check(mesh is not None, "a real OBJ must load: %s" % error)
    check_eq(cache.Count, 1, "one entry after one load")

    # A second request for the same path must NOT parse again -- that is the
    # entire point of the cache, and a scene draws the same mesh every frame.
    again, error = cache.Get(path)
    check(again is not None, "the cached mesh must be returned")
    check_eq(cache.Count, 1, "a repeat request must not add an entry")
    check_eq(len(again.Indices), 6, "the cached mesh keeps its triangles")

    # A DIFFERENT path is a different entry, even though the geometry is the
    # same file content. The cache is keyed by path as written.
    other = _write_temp("wasteland_c3_quad_copy.obj", QUAD_OBJ)
    third, error = cache.Get(other)
    check(third is not None, "a second distinct path must load: %s" % error)
    check_eq(cache.Count, 2, "two distinct paths are two entries")


def test_the_cache_caches_a_failure_instead_of_retrying():
    cache = Mesh.MeshAssetCache()
    missing = os.path.join(tempfile.gettempdir(), "wasteland_c3_does_not_exist.obj")

    mesh, error = cache.Get(missing)
    check(mesh is None, "a missing file must not produce a mesh")
    check(error != "", "a missing file must report a reason")
    check_eq(cache.Count, 1, "a failure still occupies an entry")

    # The second request must NOT re-attempt the parse: the same reason comes
    # back and the entry count is unchanged. Re-parsing a missing asset every
    # frame is the failure this cache exists to prevent.
    again, error_again = cache.Get(missing)
    check(again is None, "a cached failure must stay a failure")
    check_eq(error_again, error, "a cached failure returns the SAME reason")
    check_eq(cache.Count, 1, "a cached failure is not re-attempted")


def test_an_empty_path_is_a_no_op_not_a_load():
    cache = Mesh.MeshAssetCache()
    mesh, error = cache.Get("")
    check(mesh is None, "an unassigned path draws nothing")
    check_eq(error, "", "an unassigned path is not an error")
    check_eq(cache.Count, 0, "an unassigned path must not occupy an entry")


def test_an_unsupported_extension_is_a_named_error():
    path = _write_temp("wasteland_c3_asset.fbx", "not a mesh")
    cache = Mesh.MeshAssetCache()
    mesh, error = cache.Get(path)
    check(mesh is None, "an .fbx must not load")
    check("unsupported mesh extension" in error,
          "the reason names the extension: %s" % error)


def test_clear_drops_every_entry():
    path = _write_temp("wasteland_c3_clear.obj", QUAD_OBJ)
    cache = Mesh.MeshAssetCache()
    cache.Get(path)
    check_eq(cache.Count, 1, "one entry before Clear")
    cache.Clear()
    check_eq(cache.Count, 0, "Clear empties the cache")
    # And a path that failed is forgotten too, so it will be tried again.
    missing = os.path.join(tempfile.gettempdir(), "wasteland_c3_still_missing.obj")
    cache.Get(missing)
    cache.Clear()
    check_eq(cache.Count, 0, "Clear also drops cached failures")


# ---------------------------------------------------------------- component

def test_mesh_component_attaches_and_detaches():
    scene = W.Scene()
    entity = scene.CreateEntity("prop")

    check(not entity.HasMeshComponent(), "a new entity has no mesh")
    component = W.MeshComponent()
    component.MeshPath = "models/hero.gltf"
    component.MaterialPath = "materials/hero.mat"
    entity.AddMeshComponent(component)

    check(entity.HasMeshComponent(), "AddMeshComponent must attach")
    fetched = entity.TryGetMeshComponent()
    check(fetched is not None, "TryGetMeshComponent must find it")
    check_eq(fetched.MeshPath, "models/hero.gltf", "MeshPath survives attach")
    check_eq(fetched.MaterialPath, "materials/hero.mat", "MaterialPath survives attach")
    check(fetched.CastShadow, "CastShadow defaults true")
    check(fetched.ReceiveShadow, "ReceiveShadow defaults true")

    entity.RemoveMeshComponent()
    check(not entity.HasMeshComponent(), "RemoveMeshComponent must detach")


def test_the_defaults_are_the_documented_ones():
    component = W.MeshComponent()
    check_eq(component.MeshPath, "", "MeshPath defaults empty")
    check_eq(component.MaterialPath, "", "MaterialPath defaults empty")
    check(component.CastShadow, "CastShadow defaults true")
    check(component.ReceiveShadow, "ReceiveShadow defaults true")


def test_a_mesh_component_survives_a_scene_round_trip():
    """The path, not the geometry, is what a scene stores.

    The mesh is NOT loaded here -- loading happens on first draw through the
    Scene's cache, so a scene can be built and saved in a headless process
    with no renderer.
    """
    scene = W.Scene()
    entity = scene.CreateEntity("prop")
    component = W.MeshComponent()
    component.MeshPath = "props/crate.gltf"
    component.MaterialPath = "materials/crate.mat"
    component.CastShadow = False
    entity.AddMeshComponent(component)

    path = os.path.join(tempfile.gettempdir(), "wasteland_c3_mesh_component.wastescene")
    check(scene.SaveScene(path), "the scene must save")

    text = open(path, "r", encoding="utf-8").read()
    check("MeshComponent" in text, "the saved YAML names MeshComponent")
    check("props/crate.gltf" in text, "the saved YAML carries the mesh path")
    check("assets/" in text, "MeshPath is normalised with the assets/ prefix")

    loaded = W.Scene()
    check(loaded.LoadScene(path), "the scene must load")
    # Find the entity by tag. GetEntities() is EMPTY in a headless scene --
    # GetAllEntitiesWithTag() is the view that enumerates -- and the loader
    # assigns fresh ids, so the saved id is not the loaded one either.
    found = None
    for candidate in loaded.GetAllEntitiesWithTag():
        # BIND_COMP_GET stringifies the QUALIFIED type, so the method is
        # literally "GetWasteland::TagComponent" -- the namespace-qualified
        # convention recorded in the harness notes.
        if getattr(candidate, "GetWasteland::TagComponent")().Tag == "prop":
            found = candidate
            break
    check(found is not None, "the saved entity must come back")
    if found is None:
        return
    check(found.HasMeshComponent(), "the loaded entity must still have a mesh")
    fetched = found.TryGetMeshComponent()
    check(fetched is not None, "the loaded component must be reachable")
    if fetched is None:
        return
    check_eq(fetched.MeshPath, "assets/props/crate.gltf",
             "MeshPath round-trips with the prefix rule")
    check_eq(fetched.MaterialPath, "materials/crate.mat",
             "MaterialPath round-trips verbatim (no prefix rule)")
    check(not fetched.CastShadow, "CastShadow round-trips false")
    check(fetched.ReceiveShadow, "ReceiveShadow round-trips true")


def test_a_scene_without_the_component_still_loads():
    """A scene authored before MeshComponent existed must load unchanged."""
    path = os.path.join(tempfile.gettempdir(), "wasteland_c3_no_mesh.wastescene")
    body = (
        "Scene: NoMesh\n"
        "Entities:\n"
        "  - Entity: 1234567890\n"
        "    TagComponent:\n"
        "      Tag: plain\n"
        "    TransformComponent:\n"
        "      Translation: [0, 0, 0]\n"
        "      Rotation: [0, 0, 0]\n"
        "      Scale: [1, 1, 1]\n"
    )
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(body)

    scene = W.Scene()
    check(scene.LoadScene(path), "a scene with no MeshComponent must load")
    check_eq(len(list(scene.GetAllEntitiesWithTag())), 1, "the entity comes back")


# ---------------------------------------------------------------- main

def main():
    tests = [
        test_build_procedural_mesh_preserves_geometry,
        test_a_zero_length_normal_becomes_positive_y,
        test_a_face_with_no_normal_gets_one_generated,
        test_a_mesh_with_no_triangles_is_rejected_not_padded,
        test_the_cache_loads_a_path_once,
        test_the_cache_caches_a_failure_instead_of_retrying,
        test_an_empty_path_is_a_no_op_not_a_load,
        test_an_unsupported_extension_is_a_named_error,
        test_clear_drops_every_entry,
        test_mesh_component_attaches_and_detaches,
        test_the_defaults_are_the_documented_ones,
        test_a_mesh_component_survives_a_scene_round_trip,
        test_a_scene_without_the_component_still_loads,
    ]
    for test in tests:
        print("  %s ..." % test.__name__)
        test()

    print("")
    if FAILED:
        print("%d/%d tests passed" % (len(tests) - len(FAILED), len(tests)))
        print("%d/%d checks passed" % (PASSED, PASSED + len(FAILED)))
        for message in FAILED:
            print("  FAILED:", message)
        return 1
    print("%d/%d tests passed" % (len(tests), len(tests)))
    print("%d/%d checks passed" % (PASSED, PASSED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
