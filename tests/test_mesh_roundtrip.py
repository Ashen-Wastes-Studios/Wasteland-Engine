"""C.7 -- the mesh round-trip gate.

A model goes DCC -> engine -> EDIT -> save -> re-import. This is the test that
proves the edit actually left the engine and came back, rather than being
discarded at the export step.

    rigged cube .glb
        -> Mesh.ImportGltfFromText          (the DCC output, as a real exporter
                                             would write it)
        -> EditableMesh.Weld                (the editor's indexed form)
        -> SetVertexPosition                (a sculpt edit: push one corner out)
        -> RecomputeNormals                 (the caller's explicit choice)
        -> MeshWriter.EmitOBJ / SaveOBJ    (leave the engine)
        -> Mesh.ParseOBJ                    (re-import)
        -> EditableMesh.Weld

Assertions, per the plan:
  * vertex and triangle counts survive,
  * the sculpt edit is visible after re-import,
  * the bone hierarchy is unchanged.

THE BONE-HIERARCHY ASSERTION IS DELIBERATELY LIMITED. OBJ has no skeleton, no
skin and no animation: the format cannot carry a rig. So "unchanged" here means
two things -- the .glb is still the source of the rig and re-imports with the
same hierarchy, and the OBJ export writes no bone records at all (it cannot
silently drop one it never saw). A rigged character's mesh should be exported
through the glTF path; OBJ is for props and for this gate.

WHY OBJ AND NOT glTF: the engine already parses OBJ, so the round trip closes
through code this repository owns and tests. See MeshWriter.h.

Screenshot verification: none, and none is possible on this machine -- the
editor crashes at startup (DX12 device removed, 0x887a0006). This test covers
the data path end to end; the on-screen half of the gate is unverified.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(HERE)
_MODULE_DIR = os.path.join(_ROOT, "bin", "Release-windows-x86_64",
                           "DemonCore-Editor")
for _path in (HERE, _MODULE_DIR):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import Wasteland

Mesh = Wasteland.Mesh

# The rigged-cube fixture, built byte by byte in the C.2 test. Imported rather
# than duplicated: a second copy of a hand-built .glb would drift from the one
# the importer is tested against.
import test_gltf_import as gltf_test


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_eq(actual, expected, message):
    if actual != expected:
        raise AssertionError(f"{message}: got {actual!r}, expected {expected!r}")


def _import_cube():
    """The rigged cube, imported the way a real .glb on disk would be."""
    return Mesh.ImportGltfFromText(*gltf_test._split_glb(
        gltf_test.build_cube_with_skeleton()), ".")


def _welded_cube():
    """`GltfImportResult.Mesh` is ALREADY an EditableMesh -- the importer welds on
    the way in. `Deindexed` is the MeshData form. Welding again would take an
    EditableMesh where a MeshData is expected and fail with a pybind argument
    error that says nothing about the actual mix-up."""
    return _import_cube().Mesh


def _write(mesh, directory, name="sculpt.obj"):
    path = os.path.join(directory, name)
    problems = []
    ok = Mesh.MeshWriter.SaveOBJ(path, mesh, problems)
    return path, ok, problems


# ---------------------------------------------------------------------------
# the emitter's own output
# ---------------------------------------------------------------------------

def test_the_emitter_writes_vertex_normal_and_face_records():
    mesh = _welded_cube()
    text = Mesh.MeshWriter.EmitOBJ(mesh)

    check("v " in text, "no vertex records")
    check("vn " in text, "no normal records")
    check("f " in text, "no face records")
    check(text.count("\nv ") + (1 if text.startswith("v ") else 0)
          == mesh.VertexCount,
          "one v record per unique vertex")


def test_the_face_records_share_one_index_space():
    """The writer emits `a/a/a` -- position, texcoord and normal all indexed by
    the same number. That is what keeps a hard edge hard: two coincident
    vertices with different normals stay two vertices."""
    text = Mesh.MeshWriter.EmitOBJ(_welded_cube())
    face_lines = [line for line in text.splitlines() if line.startswith("f ")]
    check(len(face_lines) > 0, "no face lines")
    for line in face_lines:
        for token in line.split()[1:]:
            fields = token.split("/")
            check_eq(len(fields), 3, f"face token {token!r} must be v/vt/vn")
            check_eq(fields[0], fields[1], f"{token!r}: v and vt disagree")
            check_eq(fields[0], fields[2], f"{token!r}: v and vn disagree")


def test_obj_indices_are_one_based():
    """OBJ is 1-based; a 0 in a face record is a malformed reference the parser
    would have to guess at."""
    text = Mesh.MeshWriter.EmitOBJ(_welded_cube())
    for line in text.splitlines():
        if not line.startswith("f "):
            continue
        for token in line.split()[1:]:
            check(int(token.split("/")[0]) >= 1, f"0-based index in {token!r}")


# ---------------------------------------------------------------------------
# the gate
# ---------------------------------------------------------------------------

def test_vertex_and_triangle_counts_survive_the_round_trip():
    before = _welded_cube()
    text = Mesh.MeshWriter.EmitOBJ(before)

    ok, data, error = Mesh.ParseOBJ(text, "sculpt.obj")
    check(ok, f"the writer's own output failed to re-import: {error}")

    after = Mesh.EditableMesh.Weld(data)
    check_eq(after.VertexCount, before.VertexCount,
             "vertex count changed across the round trip")
    check_eq(after.FaceCount, before.FaceCount,
             "triangle count changed across the round trip")


def test_a_moved_vertex_is_visible_after_re_import():
    """The sculpt edit. A corner is pushed out to (9, 9, 9) -- far outside the
    unit cube, so a re-import that silently ignored the edit cannot pass by
    coincidence."""
    before = _welded_cube()
    target = 0
    check(before.SetVertexPosition(target, 9.0, 9.0, 9.0),
          "SetVertexPosition refused a valid vertex index")
    before.RecomputeNormals()

    text = Mesh.MeshWriter.EmitOBJ(before)
    ok, data, error = Mesh.ParseOBJ(text, "sculpt.obj")
    check(ok, f"re-import failed: {error}")

    after = Mesh.EditableMesh.Weld(data)
    moved = [vertex for vertex in after.Vertices
             if abs(vertex.Position.x - 9.0) < 1e-6
             and abs(vertex.Position.y - 9.0) < 1e-6
             and abs(vertex.Position.z - 9.0) < 1e-6]
    check_eq(len(moved), 1, "the pushed corner did not come back")


def test_the_edit_does_not_disturb_the_neighbouring_corners():
    """A sculpt that moves one vertex must not drag the others with it -- the
    half-edge structure is shared, and a mutator that rewrote the whole buffer
    would show up here."""
    before = _welded_cube()
    original = [(vertex.Position.x, vertex.Position.y, vertex.Position.z)
                for vertex in before.Vertices]
    before.SetVertexPosition(0, 9.0, 9.0, 9.0)

    text = Mesh.MeshWriter.EmitOBJ(before)
    ok, data, _ = Mesh.ParseOBJ(text, "sculpt.obj")
    check(ok, "re-import failed")
    after = Mesh.EditableMesh.Weld(data)

    positions = [(vertex.Position.x, vertex.Position.y, vertex.Position.z)
                 for vertex in after.Vertices]
    others = [position for index, position in enumerate(positions)
              if index != 0]
    expected = [position for index, position in enumerate(original)
                if index != 0]
    check_eq(len(others), len(expected), "the neighbour count changed")
    for got, want in zip(sorted(others), sorted(expected)):
        check(all(abs(a - b) < 1e-6 for a, b in zip(got, want)),
              f"a neighbour moved: {got} vs {want}")


def test_the_bone_hierarchy_is_untouched_by_a_mesh_edit():
    """OBJ cannot carry a rig, so the gate's promise is narrow: the .glb is
    still the rig's source and re-imports with the same hierarchy, and the OBJ
    export writes no bone records it could not have read."""
    result = _import_cube()
    before_names = list(result.BoneNames)
    before_parents = list(result.BoneParents)

    mesh = result.Mesh
    mesh.SetVertexPosition(0, 9.0, 9.0, 9.0)
    text = Mesh.MeshWriter.EmitOBJ(mesh)

    lowered = text.lower()
    for forbidden in ("joint", "bone", "skin", "skeleton"):
        check(forbidden not in lowered,
              f"the OBJ export mentions {forbidden!r}, which it cannot carry")

    again = _import_cube()
    check_eq(list(again.BoneNames), before_names,
             "the .glb's bone hierarchy changed")
    check_eq(list(again.BoneParents), before_parents,
             "the .glb's bone parents changed")


def test_a_de_indexed_mesh_data_round_trips_through_obj():
    """The renderer's own shape. MeshData is de-indexed, so every corner becomes
    its own v/vn -- the file is larger and no sharing survives, which is correct
    because a MeshData's sharing is not recoverable information."""
    result = _import_cube()
    before = result.Deindexed
    text = Mesh.MeshWriter.EmitOBJFromMeshData(before)

    ok, data, error = Mesh.ParseOBJ(text, "data.obj")
    check(ok, f"re-import failed: {error}")
    check_eq(data.TriangleCount, before.TriangleCount,
             "triangle count changed across the de-indexed round trip")
    check_eq(len(data.Vertices), len(before.Vertices),
             "corner count changed across the de-indexed round trip")


# ---------------------------------------------------------------------------
# the writer's own failures
# ---------------------------------------------------------------------------

def test_a_degenerate_face_is_reported_not_emitted():
    """A face whose half-edge loop does not close cannot be written as a
    triangle. The writer must SKIP it and say so, not emit a malformed record
    that the parser would accept as a real triangle."""
    mesh = _welded_cube()
    mesh.SetEdgeNext(0, 0)  # the loop now closes on itself too early

    text = Mesh.MeshWriter.EmitOBJ(mesh)
    check("omitted" in text, "the skip was not reported in the file")
    face_lines = [line for line in text.splitlines() if line.startswith("f ")]
    check_eq(len(face_lines), mesh.FaceCount - 1,
             "exactly one face should have been dropped")


def test_an_out_of_range_vertex_index_is_not_moved():
    mesh = _welded_cube()
    check(not mesh.SetVertexPosition(9999, 1.0, 1.0, 1.0),
          "an out-of-range index must be refused, not clamped")
    check(not mesh.SetVertexPosition(-1, 1.0, 1.0, 1.0),
          "a negative index must be refused, not clamped")


def test_saving_an_empty_mesh_is_refused(tmp_path):
    empty = Mesh.EditableMesh()
    problems = []
    ok = Mesh.MeshWriter.SaveOBJ(os.path.join(str(tmp_path), "empty.obj"),
                                 empty, problems)
    check(not ok, "an empty mesh must not be written")
    check(len(problems) == 1 and problems[0], "no reason was reported")


def test_saving_to_an_unwritable_path_fails_with_a_reason(tmp_path):
    """A directory that does not exist: the writer must return false and say
    why, not raise -- a failed export is a condition, not a bug."""
    mesh = _welded_cube()
    problems = []
    ok = Mesh.MeshWriter.SaveOBJ(
        os.path.join(str(tmp_path), "missing-dir", "out.obj"), mesh, problems)
    check(not ok, "writing into a missing directory must fail")
    check(len(problems) == 1 and "could not open" in problems[0],
          f"the reason was not reported: {problems!r}")


def test_save_and_emit_agree(tmp_path):
    mesh = _welded_cube()
    path, ok, _ = _write(mesh, str(tmp_path))
    check(ok, "SaveOBJ failed")
    with open(path, "r", encoding="utf-8", newline="") as handle:
        written = handle.read()
    check_eq(written, Mesh.MeshWriter.EmitOBJ(mesh),
             "the file on disk differs from the emitter's output")


def test_a_saved_file_re_imports(tmp_path):
    mesh = _welded_cube()
    path, ok, _ = _write(mesh, str(tmp_path))
    check(ok, "SaveOBJ failed")
    with open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    ok, data, error = Mesh.ParseOBJ(text, path)
    check(ok, f"the saved file did not re-import: {error}")
    check_eq(data.TriangleCount, mesh.FaceCount,
             "triangle count changed across a real file round trip")


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main(tmp_path=None):
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

    import tempfile
    if tmp_path is None:
        with tempfile.TemporaryDirectory() as directory:
            return _run(tests, directory)
    return _run(tests, tmp_path)


def _run(tests, tmp_path):
    # One check per test, the convention the runner sums. A suite that only
    # prints "tests passed" is reported as UNCOUNTED rather than as passing.
    checks = 0
    failed = 0
    for name, test in tests:
        try:
            test(tmp_path) if "tmp_path" in test.__code__.co_varnames else test()
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
