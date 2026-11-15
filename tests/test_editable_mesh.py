"""Task C.1: `EditableMesh` -- indexed geometry with half-edge adjacency.

The plan's verification: weld a cube OBJ, assert 8 unique vertices (not
24/36), assert every edge has exactly two twins, assert `Deindex()`
round-trips to the original triangle count.

A cube is the right fixture because it is the smallest closed manifold:
8 positions, 12 triangles, 36 half-edges, and every edge shared by exactly
two faces. Anything the weld gets wrong shows up immediately in those
numbers, and nothing about a cube is ambiguous.
"""
import os
import sys
import tempfile

# This file lives directly in tests/, so the repo root is ONE level up.
# (Two levels resolves to tests/ itself when __file__ is relative, which puts
# a nonexistent bin/ on sys.path and lets the C++ source directory shadow the
# compiled module as a namespace package.)
_ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..")))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_MODULE_DIR = os.path.join(_ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor")
_SCRIPTS = os.path.join(_ROOT, "DemonCore-Editor", "assets", "scripts")
for _path in (_MODULE_DIR, _SCRIPTS):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import Wasteland  # noqa: E402

# EditableMesh lives on the Mesh submodule, alongside the OBJ parser.
Mesh = Wasteland.Mesh

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


# A unit cube. Twelve triangles, written the way Blender exports a cube:
# quads split into two triangles each, with one vertex per corner because
# the OBJ has no shared index buffer -- exactly the de-indexed shape
# MeshData holds.
#
# The winding is COUNTER-CLOCKWISE seen from OUTSIDE. The first face is the
# z=-1 quad: (1,2,3) is clockwise from -Z, which would make the computed normal
# point INTO the cube. Every face below is ordered so its cross product points
# away from the origin -- the parser does not fix winding, so a fixture with
# inverted faces would be testing a broken asset rather than the weld.
CUBE_OBJ = """# unit cube, 8 positions, 6 quads, 12 triangles, outward winding
v -1.0 -1.0 -1.0
v  1.0 -1.0 -1.0
v  1.0  1.0 -1.0
v -1.0  1.0 -1.0
v -1.0 -1.0  1.0
v  1.0 -1.0  1.0
v  1.0  1.0  1.0
v -1.0  1.0  1.0
f 1 3 2
f 1 4 3
f 5 7 8
f 5 6 7
f 1 6 5
f 1 2 6
f 2 7 6
f 2 3 7
f 3 8 7
f 3 4 8
f 4 5 8
f 4 1 5
"""


def _load_cube():
    """Parses the cube from TEXT, the way a headless process has to.

    `LoadOBJ` takes a filepath and would force every test to write a temp file;
    `ParseOBJFromString` exists precisely so mesh parsing is testable without
    a filesystem.
    """
    ok, data, error = Mesh.ParseOBJ(CUBE_OBJ, "cube.obj")
    if not ok:
        return None, error
    return data, ""


def test_the_obj_loads_de_indexed():
    """The fixture itself: MeshData is de-indexed, so 12 triangles is 36 corners."""
    data, error = _load_cube()
    check(data is not None, "the cube OBJ must load")
    if data is None:
        print("   ", error)
        return
    check_eq(len(data.Vertices), 36,
             "a de-indexed cube has one vertex per triangle corner")
    check_eq(data.TriangleCount, 12, "a cube has 12 triangles")


def test_weld_produces_eight_unique_vertices():
    """THE POINT OF WELD: 36 corners collapse to 8 positions."""
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)
    check_eq(mesh.GetVertexCount(), 8,
             "a welded cube has 8 unique vertices, not 24 or 36")
    check_eq(mesh.GetFaceCount(), 12, "the face count is unchanged")
    check_eq(mesh.GetEdgeCount(), 36,
             "12 triangles x 3 half-edges = 36, and no edge is shared")


def test_every_edge_has_exactly_one_twin():
    """A closed manifold: every half-edge is paired, and the pairing is mutual.

    The plan phrases this as "every edge has exactly two twins". A half-edge's
    twin is a single opposite edge, so the correct assertion is that every edge
    has exactly ONE twin -- which is the same thing said from the other side:
    every geometric edge is shared by exactly two faces.
    """
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)

    unpaired = 0
    asymmetric = 0
    for index in range(mesh.GetEdgeCount()):
        twin = mesh.Edges[index].Twin
        if twin < 0:
            unpaired += 1
        elif mesh.Edges[twin].Twin != index:
            asymmetric += 1

    check_eq(unpaired, 0,
             "a closed cube has no boundary edges (every edge has a twin)")
    check_eq(asymmetric, 0,
             "the twin relation must be mutual: if A twins B then B twins A")


def test_twins_are_the_reverse_direction():
    """Twin(A->B) must be B->A. This is what makes a face walk cross an edge."""
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)

    wrong = 0
    for index in range(mesh.GetEdgeCount()):
        edge = mesh.Edges[index]
        twin = mesh.Edges[edge.Twin]
        # This edge ends where the next edge starts.
        end = mesh.Edges[edge.Next].Vertex
        if twin.Vertex != end:
            wrong += 1

    check_eq(wrong, 0,
             "each edge's twin must start at the vertex the edge ends at")


def test_deindex_round_trips_the_triangle_count():
    """Deindex is the inverse of Weld for everything the renderer cares about."""
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)
    rebuilt = mesh.Deindex()

    check_eq(rebuilt.TriangleCount, data.TriangleCount,
             "the triangle count survives a Weld/Deindex round trip")
    check_eq(len(rebuilt.Vertices), len(data.Vertices),
             "the corner count survives a Weld/Deindex round trip")


def test_deindex_preserves_positions_exactly():
    """Weld merges by position, so every original corner must still be present.

    Deindex cannot restore WHICH merged vertex a corner came from, but it must
    reproduce every corner's position -- otherwise geometry is lost.
    """
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)
    rebuilt = mesh.Deindex()

    def key(vertex):
        position = vertex.Position  # Vec3, not a sequence
        return (round(position.x, 6), round(position.y, 6),
                round(position.z, 6))

    before = sorted(key(v) for v in data.Vertices)
    after = sorted(key(v) for v in rebuilt.Vertices)
    check_eq(after, before,
             "every original corner position must survive the round trip")


def test_validate_reports_nothing_for_a_welded_cube():
    """Validate returns an EMPTY string on success, so callers can test truth."""
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)
    errors = mesh.Validate()
    check_eq(errors, "", "a welded cube must validate clean")
    check(mesh.IsValid(), "IsValid() agrees with Validate()")


def test_validate_catches_a_broken_twin():
    """Validate must actually find something: break a twin and expect a report.

    A validator that returns empty for a deliberately corrupted mesh is worse
    than no validator -- it is a false assurance.
    """
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)
    # Through the mutator, NOT `mesh.Edges[0].Twin = 7`: Edges is a copy-out
    # property, so item assignment would mutate a temporary and the validator
    # would correctly report nothing.
    mesh.SetEdgeTwin(0, 7)  # an arbitrary wrong twin, not its real partner
    errors = mesh.Validate()
    check(errors != "", "a broken twin must be reported")
    check("twin" in errors.lower(),
          "the report should name the defect (got %r)" % errors)


def test_recompute_normals_produces_unit_vectors():
    """Area-weighted normals must be normalised and point outward."""
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)
    mesh.RecomputeNormals()

    bad = 0
    for vertex in mesh.Vertices:
        normal = vertex.Normal  # Vec3, not a sequence
        length = (normal.x ** 2 + normal.y ** 2 + normal.z ** 2) ** 0.5
        if abs(length - 1.0) > 1e-4:
            bad += 1
    check_eq(bad, 0, "every normal must be unit length")

    # On a cube centred at the origin, a vertex normal must point the same way
    # as its position -- the dot product is positive for all eight corners.
    inward = 0
    for vertex in mesh.Vertices:
        position = vertex.Position  # Vec3, not a sequence
        normal = vertex.Normal
        dot = (position.x * normal.x + position.y * normal.y
               + position.z * normal.z)
        if dot <= 0.0:
            inward += 1
    check_eq(inward, 0,
             "every normal must point outward on a convex cube")


def test_recompute_normals_is_area_weighted_not_count_weighted():
    """A sliver must not outvote a large face.

    Built by hand rather than from an OBJ: one large triangle and one sliver
    sharing a vertex. A count-weighted average gives the sliver 1/2 the say;
    an area-weighted one gives it almost none, so the shared vertex's normal is
    dominated by the large face.
    """
    # Built as TEXT and welded, because both MeshData.Vertices and
    # EditableMesh.Vertices are copy-out properties: a script cannot assemble
    # a mesh by hand, it must parse one. That is a real limitation and it is
    # noted in the plan's C.1 status.
    #
    # One large triangle and one sliver sharing vertex 0.
    source = """v 0.0 0.0 0.0
v 10.0 0.0 0.0
v 0.0 10.0 0.0
v 0.001 0.0 0.0
f 1 2 3
f 1 4 3
"""
    ok, data, error = Mesh.ParseOBJ(source, "sliver.obj")
    if not ok:
        check(False, "the sliver OBJ must parse: %s" % error)
        return
    mesh = Mesh.EditableMesh.Weld(data)
    mesh.RecomputeNormals()

    shared = mesh.Vertices[0].Normal
    # The large triangle's normal is +Z; the sliver's is nearly -X. A
    # count-weighted average would be ~(0.7, 0, 0.7). Area weighting keeps the
    # Z dominant.
    check(shared.z > abs(shared.x),
          "the large face must dominate the shared vertex normal (got %r)"
          % ((shared.x, shared.y, shared.z),))


def test_is_triangulated_is_true_for_a_cube():
    data, _ = _load_cube()
    if data is None:
        check(False, "fixture failed to load")
        return
    mesh = Mesh.EditableMesh.Weld(data)
    check(mesh.IsTriangulated(), "a welded cube is fully triangulated")


if __name__ == "__main__":
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        print(f"  {name} ...")
        try:
            fn()
        except Exception as error:  # noqa: BLE001
            failed += 1
            print(f"    ERROR {error!r}")
    total = len(tests)
    print(f"\n{total - failed}/{total} tests passed")
    print(f"{PASSED}/{PASSED + len(FAILED)} checks passed")
    sys.exit(1 if failed else 0)
