"""D.2 -- the two remaining modeling operations: Bevel and Loop cut.

Both rebuild the mesh from a triangle soup and re-weld (the proven Extrude/Inset
pattern), so the operation's own job is deciding WHICH triangles. The plan's
verification applies: derived counts + Validate() empty + whole-mesh undo
restores the exact previous mesh (positions, not just counts).

Derived counts:
  bevel ONE interior edge -> +2 vertices (the chamfer plane's two corners) and
             +2 faces (the two adjacent triangles become: chamfer quad = 2 tris
             + two shrunken companion tris = 4 tris, so -2 +4 = +2).
  two beveled edges that SHARE a vertex are refused (the corner offset is where
             the D.2 solve lives; v1 refuses rather than guess a mangled corner).
  loop cut  -> the ring edges get midpoint vertices; the mesh stays structurally
             sound; every NEW vertex must be the exact midpoint of two original
             vertices (the insertion property), and undo restores the original 8.

Screenshot verification: none, and none is possible on this machine (the editor
crashes at startup, DX12 device removed). Structural verification is therefore
the ceiling here; a viewport is needed to confirm the chamfer/loop READ right.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wlpath

wlpath.setup()

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


def _quad():
    return _obj_to_mesh(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "f 1 2 3\nf 1 3 4\n")


def _cube():
    return _obj_to_mesh(
        "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
        "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
        "f 1 2 3\nf 1 3 4\n"
        "f 6 5 8\nf 6 8 7\n"
        "f 5 1 4\nf 5 4 8\n"
        "f 2 6 7\nf 2 7 3\n"
        "f 4 3 7\nf 4 7 8\n"
        "f 5 6 2\nf 5 2 1\n")


def _first_interior_edge(mesh):
    for i in range(mesh.EdgeCount):
        if mesh.Edges[i].Twin >= 0:
            return i
    raise AssertionError("no interior edge in the fixture")


def _endpoint_ids(mesh, edge):
    e = mesh.Edges[edge]
    a = e.Vertex
    b = mesh.Edges[e.Next].Vertex
    return a, b


def _positions(mesh):
    return [(v.Position.x, v.Position.y, v.Position.z) for v in mesh.Vertices]


# ---------------------------------------------------------------------------
# bevel
# ---------------------------------------------------------------------------

def test_bevelling_one_interior_edge_gains_two_vertices_and_two_faces():
    mesh = _quad()
    check_eq(mesh.VertexCount, 4, "fixture quad")
    check_eq(mesh.FaceCount, 2, "fixture quad")
    edge = _first_interior_edge(mesh)  # the shared diagonal is interior
    ok, error = mesh.BevelEdges([edge], 0.3)
    check(ok, f"bevel refused an interior edge: {error}")
    check_eq(mesh.VertexCount, 6, "one chamfered edge adds two vertices")
    check_eq(mesh.FaceCount, 4, "one chamfered edge adds two faces")
    check_eq(mesh.Validate(), "", "the beveled mesh is structurally sound")


def test_bevel_chamfer_plane_lies_between_edge_midpoint_and_endpoints():
    """The chamfer is `offset` of the way from the endpoints to the edge
    midpoint, so the two NEW vertices sit exactly on the edge's two halves."""
    mesh = _quad()
    edge = _first_interior_edge(mesh)
    a, b = _endpoint_ids(mesh, edge)
    pa = mesh.Vertices[a].Position
    pb = mesh.Vertices[b].Position
    before = set(_positions(mesh))
    mesh.BevelEdges([edge], 0.25)
    new_positions = [p for p in _positions(mesh) if p not in before]
    check_eq(len(new_positions), 2, "bevel should have created two vertices")
    ax, ay, az = pa.x, pa.y, pa.z
    bx, by, bz = pb.x, pb.y, pb.z
    # Ap sits offset*(1/2) along the edge: 25% of the way to the MIDPOINT is
    # 12.5% of the edge, not 25%.
    expect = {
        (ax + (bx - ax) * 0.125, ay + (by - ay) * 0.125, az + (bz - az) * 0.125),
        (bx + (ax - bx) * 0.125, by + (ay - by) * 0.125, bz + (az - bz) * 0.125),
    }
    got = {(round(p[0], 6), round(p[1], 6), round(p[2], 6)) for p in new_positions}
    check_eq(got, expect, "the chamfer plane is not at the offset fraction")


def test_bevel_refuses_a_boundary_edge():
    mesh = _quad()  # boundary edges (no twin); the cube is closed and has none
    edge = None
    for i in range(mesh.EdgeCount):
        if mesh.Edges[i].Twin < 0:
            edge = i
            break
    check(edge is not None, "the quad should have boundary half-edges")
    ok, error = mesh.BevelEdges([edge], 0.3)
    check(not ok, "bevel accepted a boundary edge")
    check("boundary" in error, f"the boundary refusal is unexplained: {error}")
    check_eq(mesh.VertexCount, 4, "a refused bevel must not mutate the mesh")


def test_bevel_refuses_adjacent_edges_and_bad_offset():
    mesh = _cube()
    e1 = _first_interior_edge(mesh)
    a1, b1 = _endpoint_ids(mesh, e1)
    e2 = None
    for i in range(mesh.EdgeCount):
        if i == e1:
            continue
        a, b = _endpoint_ids(mesh, i)
        if a == a1 or a == b1 or b == a1 or b == b1:
            e2 = i
            break
    check(e2 is not None, "the cube should have an edge adjacent to e1")
    ok, error = mesh.BevelEdges([e1, e2], 0.3)
    check(not ok, "bevel accepted two adjacent edges (needs a corner solve)")
    check("corner" in error, f"the adjacent refusal is unexplained: {error}")

    ok, error = mesh.BevelEdges([e1], 0.0)
    check(not ok, "bevel accepted offset 0")
    ok, error = mesh.BevelEdges([e1], 1.0)
    check(not ok, "bevel accepted offset 1")


# ---------------------------------------------------------------------------
# loop cut
# ---------------------------------------------------------------------------

def test_loopcut_inserts_midpoints_and_stays_sound():
    mesh = _cube()
    check_eq(mesh.VertexCount, 8, "fixture cube")
    check_eq(mesh.FaceCount, 12, "fixture cube")
    base = set(_positions(mesh))
    edge = _first_interior_edge(mesh)
    ok, error = mesh.LoopCut(edge)
    check(ok, f"loop cut refused the cube: {error}")
    check_eq(mesh.Validate(), "", "the cut mesh is structurally sound")
    check(mesh.VertexCount > 8, "no midpoint vertices were inserted")
    check(mesh.FaceCount > 12, "no faces were split")

    # Every vertex is either an original cube corner or the EXACT midpoint of a
    # pair of original corners -- the insertion property that proves the ring
    # split created midpoint vertices on existing edges, not invented geometry.
    original = list(base)
    midpoint_of = []
    for (x, y, z) in _positions(mesh):
        if (x, y, z) in base:
            continue
        found = False
        for i in range(len(original)):
            for j in range(i + 1, len(original)):
                ax, ay, az = original[i]
                bx, by, bz = original[j]
                mx, my, mz = (ax + bx) / 2, (ay + by) / 2, (az + bz) / 2
                if (round(x, 6), round(y, 6), round(z, 6)) == (
                        round(mx, 6), round(my, 6), round(mz, 6)):
                    found = True
                    break
            if found:
                break
        check(found, f"a vertex ({x},{y},{z}) is not a midpoint of an original edge")
        midpoint_of.append((x, y, z))
    check_eq(len(midpoint_of), mesh.VertexCount - 8,
             "every added vertex should be a midpoint")


def test_loopcut_undo_restores_the_original_cube():
    history = Mesh.MeshEditHistory(8)
    mesh = _cube()
    history.Push(mesh)
    edge = _first_interior_edge(mesh)
    ok, _ = mesh.LoopCut(edge)
    check(ok, "loop cut refused the cube")
    check(mesh.VertexCount > 8, "the cut did not happen")
    check(history.Undo(mesh), "undo returned false with a snapshot present")
    check_eq(mesh.VertexCount, 8, "undo restored the vertex count")
    check_eq(mesh.FaceCount, 12, "undo restored the face count")
    check_eq(sorted(_positions(mesh)),
             sorted([(-1.0, -1.0, -1.0), (1.0, -1.0, -1.0), (1.0, 1.0, -1.0), (-1.0, 1.0, -1.0),
                     (-1.0, -1.0, 1.0), (1.0, -1.0, 1.0), (1.0, 1.0, 1.0), (-1.0, 1.0, 1.0)]),
             "undo restored positions, not just counts")


def test_loopcut_refuses_an_out_of_range_edge():
    mesh = _quad()
    ok, error = mesh.LoopCut(9999)
    check(not ok, "loop cut accepted an out-of-range edge")
    check("range" in error, f"the refusal is unexplained: {error}")
    check_eq(mesh.VertexCount, 4, "a refused loop cut must not mutate the mesh")


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
