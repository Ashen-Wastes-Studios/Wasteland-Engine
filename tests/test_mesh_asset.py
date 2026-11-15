"""D.5 -- the .wlmesh asset format.

The plan's ask: a YAML mesh asset carrying vertices, triangles, UVs, normals,
materials, and an optional `sculpt_history` section that is OFF by default.

What this file actually verifies, and why each part matters:

  * A round trip is EXACT for a mesh that came out of `Weld`. That is the
    property the format exists for: an artist edits a mesh, saves it, and gets
    the same mesh back -- not a re-welded approximation of it.
  * Normals and UVs travel with the mesh, and a mesh whose normals are all the
    +Y default and whose UVs are all zero OMITS them. Writing 20,000 identical
    triplets to say "nothing to see here" is noise, not data.
  * A malformed file is REPORTED with a reason and never silently truncated.
    yaml-cpp's own `as<int>` streams `1.5` into `1`, so a triangle index is
    checked as text -- a truncated index is a different mesh, not a rounding
    error.
  * The history section is opt-in. A snapshot is a whole mesh, so a 16-deep
    stack on a 20k-triangle sculpt is hundreds of megabytes, and a scene file
    carrying an undo stack nobody asked for is not something to ship.
  * The emitted file is LINE-ORIENTED. A flow-style triangle list for a
    100k-triangle mesh is one 600 KB line that no editor can diff.

Screenshot verification: none. The editor cannot start on this machine (DX12
device removed, 0x887a0006), so no .wlmesh has been opened in the Content
Browser. Every claim here is from the headless serializer.
"""

import os
import shutil
import sys
import tempfile

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_MODULE_DIR = os.path.join(_ROOT, "bin", "Release-windows-x86_64",
                           "DemonCore-Editor")
if _MODULE_DIR not in sys.path:
    sys.path.insert(0, _MODULE_DIR)

import Wasteland

Mesh = Wasteland.Mesh


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_eq(actual, expected, message):
    if actual != expected:
        raise AssertionError(f"{message}: got {actual!r}, expected {expected!r}")


def check_close(actual, expected, message, tolerance=1e-5):
    if abs(actual - expected) > tolerance:
        raise AssertionError(
            f"{message}: got {actual!r}, expected {expected!r} "
            f"(tolerance {tolerance})")


# ---------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------

# A unit quad in the XZ plane, wound so its normal is +Y, with SUPPLIED normals
# and UVs. Supplied rather than synthesized so the test does not depend on the
# OBJ loader's normal generation -- this file is about the asset format.
QUAD_WITH_SHADING = """\
v -0.5 0.0 -0.5
v  0.5 0.0 -0.5
v  0.5 0.0  0.5
v -0.5 0.0  0.5
vt 0.0 0.0
vt 1.0 0.0
vt 1.0 1.0
vt 0.0 1.0
vn 0.0 0.0 1.0
vn 0.0 0.0 1.0
vn 0.0 0.0 1.0
vn 0.0 0.0 1.0
f 1/1/1 2/2/2 3/3/3
f 1/1/1 3/3/3 4/4/4
"""

# One triangle whose computed normal is exactly +Y and whose UVs are exactly
# zero -- the two "nothing to write" cases the emitter must recognise.
PLAIN_TRIANGLE = """\
v 0.0 0.0 0.0
v 0.0 0.0 1.0
v 1.0 0.0 0.0
f 1 2 3
"""


def _weld(obj_text, name="fixture.obj"):
    ok, data, error = Mesh.ParseOBJ(obj_text, name)
    check(ok, f"the fixture did not parse: {error}")
    return Mesh.EditableMesh.Weld(data)


def _positions(mesh):
    return [(v.Position.x, v.Position.y, v.Position.z) for v in mesh.Vertices]


def _normals(mesh):
    return [(v.Normal.x, v.Normal.y, v.Normal.z) for v in mesh.Vertices]


def _uvs(mesh):
    return [(v.TexCoord.x, v.TexCoord.y) for v in mesh.Vertices]


def _tempdir():
    return tempfile.mkdtemp(prefix="wasteland_d5_")


# ---------------------------------------------------------------------------
# the round trip
# ---------------------------------------------------------------------------

def test_a_round_trip_preserves_the_mesh():
    mesh = _weld(QUAD_WITH_SHADING)
    text, error = Mesh.EmitMesh(mesh, [], [])
    check_eq(error, "", f"the emit failed: {error}")
    check(text != "", "the emit produced no text")

    parsed, materials, face_material, history, error = Mesh.ParseMesh(text)
    check_eq(error, "", f"the parse failed: {error}")

    check_eq(parsed.VertexCount, mesh.VertexCount,
             "the vertex count survived the round trip")
    check_eq(parsed.FaceCount, mesh.FaceCount,
             "the face count survived the round trip")
    check_eq(parsed.EdgeCount, mesh.EdgeCount,
             "the edge count survived the round trip")
    check_eq(parsed.Validate(), "", "the round-tripped mesh is structurally sound")
    check_eq(_positions(parsed), _positions(mesh),
             "every position survived the round trip")


def test_a_round_trip_through_a_file():
    """Emit -> text -> Parse is only half the format. Save -> Load exercises
    the filesystem path, including parent-directory creation."""
    mesh = _weld(QUAD_WITH_SHADING)
    directory = _tempdir()
    try:
        path = os.path.join(directory, "nested", "deeper", "quad.wlmesh")
        ok, error = Mesh.SaveMesh(path, mesh, ["materials/quad.wlmaterial"], [])
        check(ok, f"the save failed: {error}")
        check(os.path.exists(path), "the save produced no file")

        loaded, materials, face_material, history, error = Mesh.LoadMesh(path)
        check_eq(error, "", f"the load failed: {error}")
        check_eq(loaded.VertexCount, mesh.VertexCount,
                 "the vertex count survived the file round trip")
        check_eq(loaded.FaceCount, mesh.FaceCount,
                 "the face count survived the file round trip")
        check_eq(materials, ["materials/quad.wlmaterial"],
                 "the material slot survived the file round trip")
        check_eq(_positions(loaded), _positions(mesh),
                 "every position survived the file round trip")
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def test_normals_and_uvs_survive_the_round_trip():
    """A mesh that has been shaded and unwrapped must come back shaded and
    unwrapped. This is the whole reason the format carries them: OBJ would lose
    the UVs on the way out and the normals on the way back.

    The UVs are written with the mesh's own mutator because the OBJ loader
    parses the texture index and discards it, so no OBJ fixture can carry them.
    """
    mesh = _weld(QUAD_WITH_SHADING)
    const_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    for index, (u, v) in enumerate(const_uvs):
        check(mesh.SetVertexTexCoord(index, u, v),
              f"the UV mutator refused vertex {index}")

    text, _ = Mesh.EmitMesh(mesh, [], [])
    parsed, _, _, _, error = Mesh.ParseMesh(text)
    check_eq(error, "", f"the parse failed: {error}")

    check_eq(_normals(parsed), _normals(mesh),
             "every normal survived the round trip")
    check_eq(_uvs(parsed), const_uvs,
             "every UV survived the round trip")

    # And they are actually present in the file, not merely defaulted.
    check("normals:" in text, "the file carries no normals section")
    check("uvs:" in text, "the file carries no uvs section")


def test_default_normals_and_uvs_are_omitted():
    """A mesh with nothing to say about shading must not carry a section saying
    it. This keeps a blockout mesh's file proportional to its content."""
    mesh = _weld(PLAIN_TRIANGLE)
    # The fixture is chosen so the loader's synthesized normal is exactly +Y
    # and the UVs are exactly zero. Assert that first, or the omission below
    # would be a fixture accident rather than a format property.
    for normal in _normals(mesh):
        check_close(normal[0], 0.0, "the fixture normal is not +Y")
        check_close(normal[1], 1.0, "the fixture normal is not +Y")
        check_close(normal[2], 0.0, "the fixture normal is not +Y")
    for uv in _uvs(mesh):
        check_close(uv[0], 0.0, "the fixture UV is not zero")
        check_close(uv[1], 0.0, "the fixture UV is not zero")

    text, error = Mesh.EmitMesh(mesh, [], [])
    check_eq(error, "", f"the emit failed: {error}")
    check("normals:" not in text,
          "a mesh with all-default normals must not carry a normals section")
    check("uvs:" not in text,
          "a mesh with all-zero UVs must not carry a uvs section")

    # And it still reads back to the same mesh.
    parsed, _, _, _, error = Mesh.ParseMesh(text)
    check_eq(error, "", f"the parse failed: {error}")
    check_eq(_positions(parsed), _positions(mesh),
             "the omitted-normal round trip still preserves the geometry")


# ---------------------------------------------------------------------------
# materials
# ---------------------------------------------------------------------------

def test_material_slots_and_face_assignment_round_trip():
    mesh = _weld(QUAD_WITH_SHADING)
    materials = ["materials/quad.wlmaterial", "materials/trim.wlmaterial"]
    face_material = [0, 1]

    text, error = Mesh.EmitMesh(mesh, materials, face_material)
    check_eq(error, "", f"the emit failed: {error}")
    parsed, out_materials, out_face_material, _, error = Mesh.ParseMesh(text)
    check_eq(error, "", f"the parse failed: {error}")

    check_eq(out_materials, materials, "the slot list survived")
    check_eq(list(out_face_material), face_material,
             "the per-face assignment survived")
    check_eq(parsed.FaceCount, len(face_material),
             "the assignment is one per triangle")


def test_an_absent_face_assignment_means_slot_zero():
    """The common case -- one material, no per-face split -- must not write a
    redundant list of identical slot indices."""
    mesh = _weld(QUAD_WITH_SHADING)
    text, error = Mesh.EmitMesh(mesh, ["materials/quad.wlmaterial"], [])
    check_eq(error, "", f"the emit failed: {error}")
    check("face_material:" not in text,
          "an empty face assignment must not be written")
    _, materials, face_material, _, error = Mesh.ParseMesh(text)
    check_eq(error, "", f"the parse failed: {error}")
    check_eq(materials, ["materials/quad.wlmaterial"], "the slot survived")
    check_eq(list(face_material), [], "no assignment reads back as none")


def test_a_face_material_out_of_range_is_an_error():
    mesh = _weld(QUAD_WITH_SHADING)
    text, error = Mesh.EmitMesh(mesh, ["materials/quad.wlmaterial"], [0, 3])
    check_eq(error, "", f"the emit failed: {error}")
    _, _, _, _, error = Mesh.ParseMesh(text)
    check(error != "", "a face assigned to a missing slot must be reported")
    check("slot 3" in error, f"the reason names the bad slot: {error}")


def test_a_face_material_count_mismatch_is_an_error():
    mesh = _weld(QUAD_WITH_SHADING)
    text, error = Mesh.EmitMesh(mesh, ["materials/quad.wlmaterial"], [0])
    check_eq(error, "", f"the emit failed: {error}")
    _, _, _, _, error = Mesh.ParseMesh(text)
    check(error != "",
          "one assignment for two triangles must be reported, not padded")


# ---------------------------------------------------------------------------
# sculpt history
# ---------------------------------------------------------------------------

def test_sculpt_history_is_written_only_when_asked():
    """The plan's opt-in. A snapshot is a whole mesh, so the default save must
    not carry one -- and asking for one must actually produce it."""
    mesh = _weld(QUAD_WITH_SHADING)
    history = [_weld(PLAIN_TRIANGLE)]

    without, error = Mesh.EmitMesh(mesh, [], [], history, False)
    check_eq(error, "", f"the emit failed: {error}")
    check("sculpt_history:" not in without,
          "the history must be omitted when it was not asked for")

    with_history, error = Mesh.EmitMesh(mesh, [], [], history, True)
    check_eq(error, "", f"the emit failed: {error}")
    check("sculpt_history:" in with_history,
          "an requested history must be written")

    _, _, _, out_history, error = Mesh.ParseMesh(with_history)
    check_eq(error, "", f"the parse failed: {error}")
    check_eq(len(out_history), 1, "the snapshot came back")
    check_eq(_positions(out_history[0]), _positions(history[0]),
             "the snapshot's geometry came back")

    # The default is off, and that is the binding's default too.
    default_text, error = Mesh.EmitMesh(mesh)
    check_eq(error, "", f"the emit failed: {error}")
    check("sculpt_history:" not in default_text,
          "the default emit must not carry a history")


def test_a_history_snapshot_with_a_broken_face_loop_is_refused():
    """A snapshot that cannot be written must fail the whole save rather than
    silently truncating the file."""
    mesh = _weld(QUAD_WITH_SHADING)
    broken = _weld(PLAIN_TRIANGLE)
    # Break a face loop so GetFaceCorners cannot walk it.
    broken.SetEdgeNext(0, -1)
    text, error = Mesh.EmitMesh(mesh, [], [], [broken], True)
    check(text == "", "a broken snapshot must not emit a file")
    check(error != "", "a broken snapshot must report a reason")


# ---------------------------------------------------------------------------
# malformed input
# ---------------------------------------------------------------------------

def _parse(text):
    mesh, materials, face_material, history, error = Mesh.ParseMesh(text)
    return mesh, error


def test_an_empty_file_is_an_error():
    _, error = _parse("")
    check(error != "", "an empty file must be reported")


def test_a_missing_version_is_an_error():
    _, error = _parse("vertices:\n- [0, 0, 0]\ntriangles: [0, 1, 2]\n")
    check(error != "", "a file with no version must be reported")
    check("version" in error, f"the reason names the version: {error}")


def test_a_future_version_is_an_error():
    _, error = _parse("version: 2\n")
    check(error != "", "a version this build cannot read must be reported")
    check("2" in error, f"the reason names the version: {error}")


def test_a_non_integer_version_is_an_error():
    _, error = _parse("version: one\n")
    check(error != "", "a non-integer version must be reported")


def test_an_out_of_range_triangle_index_is_an_error():
    text = ("version: 1\n"
            "vertices:\n"
            "- [0, 0, 0]\n"
            "- [1, 0, 0]\n"
            "- [0, 1, 0]\n"
            "triangles: [0, 1, 9]\n")
    _, error = _parse(text)
    check(error != "", "an index past the vertex list must be reported")
    check("9" in error, f"the reason names the index: {error}")


def test_a_non_multiple_of_three_index_list_is_an_error():
    text = ("version: 1\n"
            "vertices:\n"
            "- [0, 0, 0]\n"
            "- [1, 0, 0]\n"
            "- [0, 1, 0]\n"
            "triangles: [0, 1]\n")
    _, error = _parse(text)
    check(error != "", "an index list that is not a multiple of 3 must be "
                       "reported")


def test_a_float_triangle_index_is_rejected_not_truncated():
    """yaml-cpp's own convert<int> streams the scalar into an int and does not
    require the stream to be consumed, so `1.5` would decode as `1` with no
    error. A truncated index is a DIFFERENT MESH, so the text is checked."""
    text = ("version: 1\n"
            "vertices:\n"
            "- [0, 0, 0]\n"
            "- [1, 0, 0]\n"
            "- [0, 1, 0]\n"
            "triangles: [0, 1.5, 2]\n")
    _, error = _parse(text)
    check(error != "", "a fractional triangle index must be reported")
    check("1.5" in error, f"the reason names the index: {error}")


def test_duplicate_vertex_positions_are_an_error():
    """EditableMesh is position-welded, so two vertices at one position cannot
    be represented. Weld would merge them and keep the first-seen normal, which
    silently turns a hard edge into a smooth one -- so the file is refused."""
    text = ("version: 1\n"
            "vertices:\n"
            "- [0, 0, 0]\n"
            "- [0, 0, 0]\n"
            "- [1, 0, 0]\n"
            "triangles: [0, 1, 2]\n")
    _, error = _parse(text)
    check(error != "", "duplicate positions must be reported")
    check("duplicate" in error, f"the reason says what is wrong: {error}")


def test_a_malformed_vertex_is_an_error():
    text = ("version: 1\n"
            "vertices:\n"
            "- [0, 0]\n"
            "- [1, 0, 0]\n"
            "- [0, 1, 0]\n"
            "triangles: [0, 1, 2]\n")
    _, error = _parse(text)
    check(error != "", "a two-component vertex must be reported")


def test_a_missing_vertex_list_is_an_error():
    _, error = _parse("version: 1\ntriangles: [0, 1, 2]\n")
    check(error != "", "a file with no vertices must be reported")


def test_a_non_integer_material_slot_is_an_error():
    text = ("version: 1\n"
            "vertices:\n"
            "- [0, 0, 0]\n"
            "- [1, 0, 0]\n"
            "- [0, 1, 0]\n"
            "triangles: [0, 1, 2]\n"
            "materials:\n"
            "- materials/a.wlmaterial\n"
            "face_material: [0, 1.5]\n")
    _, error = _parse(text)
    check(error != "", "a fractional material slot must be reported")


# ---------------------------------------------------------------------------
# the emitted file
# ---------------------------------------------------------------------------

def test_the_emitted_file_is_line_oriented():
    """A flow-style triangle list for a large mesh is one enormous line that no
    editor can diff and no human can read. This is a format for artists, so the
    long lists are block-style."""
    mesh = _weld(QUAD_WITH_SHADING)
    text, error = Mesh.EmitMesh(mesh, [], [])
    check_eq(error, "", f"the emit failed: {error}")

    lines = [line.strip() for line in text.splitlines()]
    check(any(line == "- 0" for line in lines),
          f"the triangle indices are not one per line; first lines: {lines[:8]}")
    check(text.startswith("version:"),
          "the file starts with its version, so a reader can dispatch on it")


def test_the_cache_loads_a_wlmesh():
    """The format is only real if a MeshComponent can point at one. The asset
    cache is the path -> mesh table the renderer draws through."""
    mesh = _weld(QUAD_WITH_SHADING)
    directory = _tempdir()
    try:
        path = os.path.join(directory, "quad.wlmesh")
        ok, error = Mesh.SaveMesh(path, mesh, [], [])
        check(ok, f"the save failed: {error}")

        cache = Mesh.MeshAssetCache()
        loaded, error = cache.Get(path)
        check(loaded is not None, f"the cache must load a .wlmesh: {error}")
        check_eq(len(loaded.Indices) // 3, 2,
                 "the cached mesh has both triangles")
        check_eq(cache.Count, 1, "one entry after one load")
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def test_the_cache_reports_a_malformed_wlmesh():
    """A bad asset is user data, not a crash: the cache stores the FAILURE with
    its reason rather than re-parsing every frame."""
    directory = _tempdir()
    try:
        path = os.path.join(directory, "broken.wlmesh")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("version: 1\nvertices:\n- [0, 0]\n")

        cache = Mesh.MeshAssetCache()
        loaded, error = cache.Get(path)
        check(loaded is None, "a malformed asset must not load")
        check(error != "", "a malformed asset must report a reason")
        check_eq(cache.Count, 1, "the failure is cached as an entry")

        # A second request serves the cached failure, it does not re-parse.
        _, again = cache.Get(path)
        check(again != "", "the cached failure keeps its reason")
        check_eq(cache.Count, 1, "a repeat request must not add an entry")
    finally:
        shutil.rmtree(directory, ignore_errors=True)


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main():
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

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

    print(f"\n{len(tests) - failed}/{len(tests)} tests passed")
    print(f"{len(tests) - failed}/{len(tests)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
