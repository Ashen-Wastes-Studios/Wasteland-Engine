"""D.2 -- modeling operations: extrude, inset, and whole-mesh undo.

The plan's verification: per op, assert the vertex/edge/face counts against a
hand-computed expectation, and assert undo restores the exact previous counts.

The expectations here are DERIVED, not observed. For a region with V vertices,
a boundary of B edges and F faces:

  extrude -> V + V' vertices where V' is the region's vertex count (every region
             vertex is duplicated for the cap), F + 2B faces (the cap keeps its
             F triangles; each boundary edge becomes a two-triangle quad), and
             the mesh stays structurally sound.

  inset (per face) -> V + 3F vertices and F + 6F faces: each face gains three
             shrunk corners and is replaced by one inset triangle plus three
             two-triangle ring quads.

A single triangle is the smallest region: V=3, B=3, F=1 -> extrude gives
6 vertices and 7 faces. A quad is two triangles: V=4, B=4, F=2 -> 8 vertices
and 10 faces, and the shared interior edge must produce NO wall -- otherwise
the count would be 12.

Screenshot verification: none, and none is possible on this machine -- the
editor crashes at startup (DX12 device removed, 0x887a0006).
"""

import os
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


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_eq(actual, expected, message):
    if actual != expected:
        raise AssertionError(f"{message}: got {actual!r}, expected {expected!r}")


def _obj_to_mesh(text):
    ok, data, error = Mesh.ParseOBJ(text, "test.obj")
    check(ok, f"the fixture OBJ did not parse: {error}")
    return Mesh.EditableMesh.Weld(data)


def _triangle():
    """A standalone triangle: 3 vertices, 3 half-edges, 1 face. Every edge is a
    boundary edge because a lone triangle has no twins."""
    return _obj_to_mesh("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")


def _quad():
    """Two triangles sharing an edge: 4 vertices, 5 edges, 2 faces."""
    return _obj_to_mesh(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "f 1 2 3\nf 1 3 4\n")


# ---------------------------------------------------------------------------
# extrude
# ---------------------------------------------------------------------------

def test_an_extruded_triangle_gains_the_expected_counts():
    mesh = _triangle()
    check_eq(mesh.VertexCount, 3, "the fixture triangle")
    check(mesh.ExtrudeFaces([0], (0.0, 0.0, 2.0)), "ExtrudeFaces refused a valid face")
    # 3 original + 3 duplicated = 6; the cap (1) + 3 boundary quads (6) = 7.
    check_eq(mesh.VertexCount, 6, "extrude vertex count")
    check_eq(mesh.FaceCount, 7, "extrude face count")
    check_eq(mesh.Validate(), "", "the extruded mesh is structurally sound")


def test_an_extruded_quad_does_not_wall_its_interior_edge():
    """Two triangles share an edge. That edge is INTERIOR to the region, so it
    must produce no wall -- a wall there would be internal geometry that
    renders as a z-fighting seam and that no operation could select."""
    mesh = _quad()
    check_eq(mesh.VertexCount, 4, "the fixture quad")
    check_eq(mesh.FaceCount, 2, "the fixture quad")
    check(mesh.ExtrudeFaces([0, 1], (0.0, 0.0, 1.0)), "extrude refused")
    # 4 + 4 = 8 vertices; cap 2 + 4 boundary quads (8 triangles) = 10 faces.
    # 12 would mean the interior edge grew a wall.
    check_eq(mesh.VertexCount, 8, "extrude vertex count")
    check_eq(mesh.FaceCount, 10, "an interior edge must not grow a wall")


def test_an_extrude_moves_the_cap_by_the_delta():
    """The cap must sit at original+delta and the original boundary must stay
    put -- an extrude that moves both is a translation, not an extrusion."""
    mesh = _obj_to_mesh("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
    mesh.ExtrudeFaces([0], (0.0, 0.0, 2.0))

    lows = [v for v in mesh.Vertices if abs(v.Position.z) < 1e-6]
    highs = [v for v in mesh.Vertices if abs(v.Position.z - 2.0) < 1e-6]
    check_eq(len(lows), 3, "the original boundary stayed at z=0")
    check_eq(len(highs), 3, "the cap moved to z=2")

    # And the cap is a copy of the original footprint, not a projection.
    low_xy = sorted((round(v.Position.x, 5), round(v.Position.y, 5)) for v in lows)
    high_xy = sorted((round(v.Position.x, 5), round(v.Position.y, 5)) for v in highs)
    check_eq(low_xy, high_xy, "the cap is the original footprint translated")


def test_an_extrude_leaves_untouched_faces_alone():
    """A two-triangle strip, extruding only the FIRST face. The second must
    survive with its exact original corner positions."""
    mesh = _obj_to_mesh(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "f 1 2 3\nf 1 3 4\n")
    original_positions = sorted((v.Position.x, v.Position.y, v.Position.z)
                                for v in mesh.Vertices)
    check(mesh.ExtrudeFaces([0], (0.0, 0.0, 1.0)), "extrude refused")
    after = sorted((v.Position.x, v.Position.y, v.Position.z)
                   for v in mesh.Vertices)
    # Every original position must still be present.
    for position in original_positions:
        check(position in after, f"the untouched face lost {position}")


def test_an_extrude_with_an_out_of_range_face_is_refused():
    mesh = _triangle()
    check(not mesh.ExtrudeFaces([0, 99], (0.0, 0.0, 1.0)),
          "an out-of-range index must be refused")
    check_eq(mesh.VertexCount, 3, "the mesh must be untouched")
    check_eq(mesh.FaceCount, 1, "the mesh must be untouched")


def test_an_extrude_of_an_empty_selection_is_refused():
    mesh = _triangle()
    check(not mesh.ExtrudeFaces([], (0.0, 0.0, 1.0)),
          "an empty selection must be refused")


def test_an_extrude_produces_no_degenerate_faces():
    """Every face must have three DISTINCT vertices. An extrude with a zero
    delta collapses the cap onto the original, which is why a zero delta is a
    translation of nothing -- the test asserts the structure survives it."""
    mesh = _triangle()
    mesh.ExtrudeFaces([0], (0.0, 0.0, 1.0))
    for face_index in range(mesh.FaceCount):
        face = mesh.Faces[face_index]
        first = mesh.Edges[face.FirstEdge].Vertex
        second = mesh.Edges[mesh.Edges[face.FirstEdge].Next].Vertex
        third = mesh.Edges[mesh.Edges[mesh.Edges[face.FirstEdge].Next].Next].Vertex
        check(first != second and second != third and first != third,
              f"face {face_index} has two coincident corners")


# ---------------------------------------------------------------------------
# inset
# ---------------------------------------------------------------------------

def test_an_inset_triangle_shrinks_the_face_and_keeps_the_boundary():
    mesh = _obj_to_mesh("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
    check(mesh.InsetFaces([0], 0.1), "InsetFaces refused a valid face")
    # 3 original + 3 shrunk = 6; 1 inset triangle + 3 ring quads (6) = 7.
    check_eq(mesh.VertexCount, 6, "inset vertex count")
    check_eq(mesh.FaceCount, 7, "inset face count")
    check_eq(mesh.Validate(), "", "the inset mesh is structurally sound")


def test_an_inset_keeps_the_original_boundary_put():
    """An inset is a shrink, not a move: the outer boundary must be untouched
    and the new inner face must be strictly smaller."""
    mesh = _obj_to_mesh("v 0 0 0\nv 4 0 0\nv 0 4 0\nf 1 2 3\n")
    mesh.InsetFaces([0], 0.25)

    # Positions compared as TUPLES, not as Vec3 objects: Vec3 has no __eq__, so
    # `vertex in list` is an identity test and is true only for the very same
    # Python wrapper -- a membership test over bound glm types always passes.
    def key(vertex):
        return (round(vertex.Position.x, 5), round(vertex.Position.y, 5),
                round(vertex.Position.z, 5))

    original = {(0.0, 0.0, 0.0), (4.0, 0.0, 0.0), (0.0, 4.0, 0.0)}
    present = {key(v) for v in mesh.Vertices}
    for corner in original:
        check(corner in present,
              f"the original corner {corner} is still on the boundary")

    inner = [v for v in mesh.Vertices if key(v) not in original]
    check_eq(len(inner), 3, "three shrunk corners")

    # Each shrunk corner is a quarter of the way from its original corner to
    # the centroid (4/3, 4/3).
    expected = set()
    for ox, oy in ((0.0, 0.0), (4.0, 0.0), (0.0, 4.0)):
        expected.add((round(ox + 0.25 * (4.0 / 3.0 - ox), 5),
                      round(oy + 0.25 * (4.0 / 3.0 - oy), 5), 0.0))
    check_eq({key(v) for v in inner}, expected,
             "the shrunk corners sit a quarter of the way to the centroid")


def test_an_inset_of_a_quad_insets_both_faces():
    mesh = _quad()
    check(mesh.InsetFaces([0, 1], 0.1), "inset refused")
    # Each face: +3 vertices and 7 triangles. Two faces: +6 vertices, 14 faces.
    check_eq(mesh.VertexCount, 10, "inset vertex count")
    check_eq(mesh.FaceCount, 14, "inset face count")


def test_an_inset_of_zero_is_refused():
    """A zero inset puts the shrunk face exactly on the original, so the ring
    quads are zero-area. Refused rather than emitted."""
    mesh = _triangle()
    check(not mesh.InsetFaces([0], 0.0), "a zero inset must be refused")
    check_eq(mesh.VertexCount, 3, "the mesh must be untouched")


def test_an_inset_with_an_out_of_range_face_is_refused():
    mesh = _triangle()
    check(not mesh.InsetFaces([0, 42], 0.1), "an out-of-range index must be refused")
    check_eq(mesh.VertexCount, 3, "the mesh must be untouched")


# ---------------------------------------------------------------------------
# undo -- a whole-mesh snapshot stack
# ---------------------------------------------------------------------------

def test_undo_restores_the_exact_previous_mesh():
    history = Mesh.MeshEditHistory(8)
    mesh = _quad()
    check_eq(history.GetDepth(), 0, "a new history is empty")
    check(not history.CanUndo(), "a new history cannot undo")

    history.Push(mesh)              # the state BEFORE the extrude
    check(mesh.ExtrudeFaces([0, 1], (0.0, 0.0, 1.0)), "extrude refused")
    check_eq(mesh.VertexCount, 8, "the extrude happened")
    check_eq(mesh.FaceCount, 10, "the extrude happened")

    check(history.CanUndo(), "there is a snapshot")
    check(history.Undo(mesh), "undo returned false with a snapshot present")
    check_eq(mesh.VertexCount, 4, "undo restored the vertex count")
    check_eq(mesh.FaceCount, 2, "undo restored the face count")
    check_eq(history.GetDepth(), 0, "the snapshot was consumed")
    check(not history.Undo(mesh), "a second undo with an empty stack is refused")


def test_undo_restores_positions_not_just_counts():
    """The counts could coincide on a different mesh. The positions are the
    real check."""
    history = Mesh.MeshEditHistory(4)
    mesh = _quad()
    before = sorted((v.Position.x, v.Position.y, v.Position.z)
                    for v in mesh.Vertices)
    history.Push(mesh)
    mesh.ExtrudeFaces([0], (0.0, 0.0, 3.0))
    history.Undo(mesh)
    after = sorted((v.Position.x, v.Position.y, v.Position.z)
                   for v in mesh.Vertices)
    check_eq(after, before, "undo restored every position")


def test_the_history_depth_is_bounded():
    """The stack is bounded because a snapshot is tens of bytes per half-edge
    and an unbounded stack would grow without limit under repeated ops."""
    history = Mesh.MeshEditHistory(3)
    mesh = _triangle()
    for _ in range(10):
        history.Push(mesh)
    check_eq(history.GetDepth(), 3, "the depth is capped")
    check_eq(history.GetMaxDepth(), 3, "the cap is reported")


def test_clear_empties_the_history():
    history = Mesh.MeshEditHistory(4)
    mesh = _triangle()
    history.Push(mesh)
    history.Push(mesh)
    history.Clear()
    check_eq(history.GetDepth(), 0, "clear emptied the stack")
    check(not history.CanUndo(), "a cleared history cannot undo")


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main():
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

    # One check per test, the convention the runner sums. A suite that only
    # prints "tests passed" is reported as UNCOUNTED rather than as passing.
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
