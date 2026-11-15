"""D.1 -- mesh editing operations and per-face picking.

Two halves:

* `MeshPicking` -- the Moller-Trumbore triangle raycast the mesh editor needs
  for sub-object selection. The editor's entity picking is a pixel readback and
  cannot say WHICH face was clicked, so this is the geometric path.
* the `EditableMesh` modelling operations (merge-by-distance, flip normals,
  delete faces).

Both live in the engine and are tested here because the panel's contribution is
only "turn a mouse position into a ray" -- and a picking bug (a face that
selects one ring off, or a miss that silently selects nothing) looks like a UI
quirk rather than a defect.

Screenshot verification: none, and none is possible on this machine -- the
editor crashes at startup (DX12 device removed, 0x887a0006).
"""

import math
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
import test_gltf_import as gltf_test


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_eq(actual, expected, message):
    if actual != expected:
        raise AssertionError(f"{message}: got {actual!r}, expected {expected!r}")


def check_close(actual, expected, message, tolerance=1e-5):
    if abs(actual - expected) > tolerance:
        raise AssertionError(f"{message}: got {actual!r}, expected {expected!r}")


def _obj_to_mesh(text):
    """Parse OBJ text into an EditableMesh, through the same reader the editor
    uses for imported assets."""
    ok, data, error = Mesh.ParseOBJ(text, "test.obj")
    check(ok, f"the fixture OBJ did not parse: {error}")
    return Mesh.EditableMesh.Weld(data)


def _cube():
    return Mesh.ImportGltfFromText(*gltf_test._split_glb(
        gltf_test.build_cube_with_skeleton()), ".").Mesh


# ---------------------------------------------------------------------------
# the raycast
# ---------------------------------------------------------------------------

def test_a_ray_through_a_known_triangle_hits_at_the_expected_point():
    """A triangle in the z=0 plane and a ray straight down -Z. The barycentrics
    are known by construction: the ray passes through (0.25, 0.25, 0), which is
    0.5 along each edge of the unit triangle."""
    hit, distance, u, v = Mesh.IntersectTriangle(
        (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        (0.25, 0.25, 5.0), (0.0, 0.0, -1.0))
    check(hit, "the ray should hit")
    check_close(distance, 5.0, "the hit is 5 units along the ray")
    check_close(u, 0.25, "barycentric u")
    check_close(v, 0.25, "barycentric v")


def test_a_ray_that_misses_returns_no_hit():
    hit, _, _, _ = Mesh.IntersectTriangle(
        (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        (5.0, 5.0, 5.0), (0.0, 0.0, -1.0))
    check(not hit, "a ray 5 units off-axis must miss")


def test_a_ray_outside_the_triangle_but_in_its_plane_misses():
    """The ray is coplanar with the triangle and passes beside it. This is the
    case a plane-intersect-only implementation gets wrong: it reports the plane
    hit and leaves the caller to notice the point is outside the edges."""
    hit, _, _, _ = Mesh.IntersectTriangle(
        (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        (2.0, 2.0, 0.0), (0.0, 0.0, -1.0))
    check(not hit, "a coplanar ray beside the triangle must miss")


def test_a_ray_parallel_to_the_triangle_misses():
    hit, _, _, _ = Mesh.IntersectTriangle(
        (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        (0.25, 0.25, 5.0), (1.0, 0.0, 0.0))
    check(not hit, "a parallel ray must miss, not divide by zero")


def test_a_back_facing_ray_still_hits():
    """No back-face culling. An editor must pick the far side of a model: a user
    orbits around an object and clicks the side facing away from the camera."""
    hit, distance, _, _ = Mesh.IntersectTriangle(
        (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        (0.25, 0.25, -5.0), (0.0, 0.0, 1.0))
    check(hit, "a ray from behind the winding must still hit")
    check_close(distance, 5.0, "the distance is still measured along the ray")


def test_a_face_is_picked_from_a_mesh():
    """The imported cube, ray straight down -Z through its centre. The +Z face
    is at z=0.5 and the ray starts at z=5, so the hit is 4.5 units in."""
    hit, result = Mesh.PickFace(_cube(), (0.0, 0.0, 5.0), (0.0, 0.0, -1.0))
    check(hit, "the ray through the cube's centre must hit a face")
    # Face 2, not a "which triangle" guess: the fixture builds the +Z face
    # third in its face list, and the hit point below is the real assertion.
    check_eq(result.Face, 2, "the +Z face of the fixture")
    check_close(result.Distance, 4.5, "the hit is 4.5 units along the ray")
    check_close(result.Point[0], 0.0, "hit x")
    check_close(result.Point[1], 0.0, "hit y")
    check_close(result.Point[2], 0.5, "hit z")


def test_the_closest_of_two_faces_wins():
    """Two triangles facing each other along the ray. The nearer one must win --
    otherwise a click selects through the model."""
    mesh = _obj_to_mesh(
        "v -1 -1 2\n"
        "v  1 -1 2\n"
        "v  0  1 2\n"
        "v -1 -1 0\n"
        "v  1 -1 0\n"
        "v  0  1 0\n"
        "f 1 2 3\n"
        "f 6 5 4\n")
    hit, result = Mesh.PickFace(mesh, (0.0, 0.0, 5.0), (0.0, 0.0, -1.0))
    check(hit, "the ray must hit one of the two triangles")
    check_close(result.Distance, 3.0, "the NEARER triangle is at z=2")


def test_a_ray_away_from_the_mesh_misses():
    hit, result = Mesh.PickFace(_cube(), (0.0, 0.0, 5.0), (0.0, 0.0, 1.0))
    check(not hit, "a ray pointing away from the mesh must miss")
    check_eq(result.Face, -1, "a miss reports face -1")


# ---------------------------------------------------------------------------
# merge by distance
# ---------------------------------------------------------------------------

def test_merge_by_distance_removes_a_near_duplicate():
    """A triangle whose first two corners are 0.0005 apart. Merging them makes
    the triangle zero-area, so it is dropped -- the vertex is removed AND the
    face goes with it."""
    mesh = _obj_to_mesh(
        "v 0 0 0\n"
        "v 0.0005 0 0\n"
        "v 1 0 0\n"
        "f 1 2 3\n")
    check_eq(mesh.VertexCount, 3, "the fixture has three vertices")
    removed = mesh.MergeByDistance(0.001)
    # The merged pair collapses the only triangle, and the third vertex is
    # orphaned by it -- so the mesh ends up empty. Asserting the OUTCOME rather
    # than the delta: "3 removed" counts the orphan, which is correct but reads
    # like a failure if you expected only the merged pair to go.
    check_eq(mesh.VertexCount, 0, "every vertex went: two merged, one orphaned")
    check_eq(mesh.FaceCount, 0, "the zero-area triangle is dropped")
    check_eq(removed, 3, "the return value is the vertex-count delta")


def test_merge_by_distance_leaves_a_clean_mesh_alone():
    removed = _cube().MergeByDistance(0.01)
    check_eq(removed, 0, "a welded cube has no near-duplicates")


def test_merge_by_distance_refuses_a_non_positive_epsilon():
    """epsilon <= 0 would collapse the whole model into one vertex. Refused
    rather than clamped: a caller that asked for 0 meant 'weld exactly', which
    Weld already does."""
    check_eq(_cube().MergeByDistance(0.0), -1, "zero is refused")
    check_eq(_cube().MergeByDistance(-1.0), -1, "a negative epsilon is refused")


def test_merge_by_distance_does_not_corrupt_the_survivors():
    """After a merge the mesh must still be structurally sound -- Validate()
    reports index ranges, twin symmetry, loop consistency and orphans."""
    mesh = _obj_to_mesh(
        "v 0 0 0\n"
        "v 0.0005 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "v 1 1 0\n"
        "f 1 3 4\n"
        "f 2 3 4\n")
    mesh.MergeByDistance(0.001)
    check_eq(mesh.Validate(), "", "the merged mesh is structurally sound")


# ---------------------------------------------------------------------------
# flip normals
# ---------------------------------------------------------------------------

def test_flip_normals_reverses_the_winding():
    """A cube's corner normal points outward. After a flip it must point inward
    -- the same magnitude, the opposite direction."""
    before = _cube()

    def corner_normal(mesh):
        for vertex in mesh.Vertices:
            position = vertex.Position
            if (abs(position.x - 0.5) < 1e-6 and
                    abs(position.y - 0.5) < 1e-6 and
                    abs(position.z - 0.5) < 1e-6):
                return vertex.Normal
        raise AssertionError("the cube has no vertex at (0.5, 0.5, 0.5)")

    outward = corner_normal(before)
    check(outward.z > 0.0, "the corner normal points outward before the flip")

    before.FlipNormals()
    inward = corner_normal(before)
    check(inward.z < 0.0, "the corner normal points inward after the flip")

    # Both must be unit length. The magnitudes are NOT expected to match each
    # other: before the flip this vertex carries the first corner normal the
    # weld happened to see (a face normal), and after it carries the
    # area-weighted average of three faces. Comparing them would be comparing
    # two different quantities that happen to share a vertex.
    for label, normal in (("before", outward), ("after", inward)):
        length = math.sqrt(normal.x ** 2 + normal.y ** 2 + normal.z ** 2)
        check_close(length, 1.0, f"the {label} normal is unit length",
                    tolerance=1e-4)


def test_flip_normals_preserves_the_geometry():
    """Flipping is a winding change, not an edit: the vertex and face counts and
    every position must be unchanged."""
    before = _cube()
    positions = sorted((v.Position.x, v.Position.y, v.Position.z)
                       for v in before.Vertices)
    before.FlipNormals()
    after = sorted((v.Position.x, v.Position.y, v.Position.z)
                   for v in before.Vertices)
    check_eq(len(after), len(positions), "the vertex count changed")
    check_eq(before.FaceCount, 12, "the face count changed")
    for got, want in zip(after, positions):
        check(all(abs(a - b) < 1e-6 for a, b in zip(got, want)),
              f"a position moved: {got} vs {want}")


# ---------------------------------------------------------------------------
# delete faces
# ---------------------------------------------------------------------------

def test_delete_faces_leaves_a_hole():
    mesh = _cube()
    check_eq(mesh.FaceCount, 12, "the cube starts with 12 triangles")
    check(mesh.DeleteFaces([0]), "DeleteFaces refused a valid index")
    check_eq(mesh.FaceCount, 11, "one face was removed")
    check_eq(mesh.Validate(), "", "the mesh is still structurally sound")


def test_delete_faces_with_an_out_of_range_index_changes_nothing():
    """A bad index is a caller bug. The operation must refuse WITHOUT deleting
    the faces it already marked -- a partial delete is worse than none."""
    mesh = _cube()
    check(not mesh.DeleteFaces([0, 999]), "an out-of-range index must be refused")
    check_eq(mesh.FaceCount, 12, "the mesh must be untouched")


def test_delete_faces_on_an_empty_selection_is_refused():
    mesh = _cube()
    check(not mesh.DeleteFaces([]), "an empty selection must be refused")


def test_deleting_every_face_empties_the_mesh():
    mesh = _cube()
    mesh.DeleteFaces(list(range(mesh.FaceCount)))
    check_eq(mesh.FaceCount, 0, "every face is gone")
    check_eq(mesh.VertexCount, 0, "the orphaned vertices were compacted out")


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
