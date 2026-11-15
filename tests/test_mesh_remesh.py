"""D.4 -- remesh and decimation.

The plan's verification: remesh a 100k-triangle mesh to a 20mm voxel grid and
assert the result is watertight and its silhouette is within a stated tolerance
of the original.

The stated tolerances, up front, because "within a tolerance" is meaningless
without the number:

  * remesh silhouette -- 2.5 voxels. The occupied set is {voxels whose CENTRE
    is within half a voxel DIAGONAL (0.866) of the surface}, and a shell vertex
    is a voxel corner, so every shell point is within 0.866 + 0.866 = 1.73
    voxels of the surface. 2.5 leaves room for the support function of a shape
    that is not perfectly convex, which a remeshed sphere nearly is.
  * decimation silhouette -- 0.2 units on a radius-2 sphere (10%). Quadric
    decimation protects large flat regions and collapses small ones, so the
    support function moves by roughly the size of the smallest feature it
    removed, not by a fraction of the model.

Why the remesh marks a TUBE and not a skin: a one-voxel-thick skin of the
surface has holes wherever a triangle passes between two voxel centres, and
the exterior flood fill leaks through those holes into the interior -- the
solid is lost silently, with nothing in the output looking wrong. Half a
diagonal is the smallest radius that makes the occupied set a closed barrier.

Screenshot verification: none, and none is possible on this machine -- the
editor crashes at startup (DX12 device removed, 0x887a0006).
"""

import math
import os
import sys
import time

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


def check_close(actual, expected, message, tolerance=1e-4):
    if abs(actual - expected) > tolerance:
        raise AssertionError(
            f"{message}: got {actual!r}, expected {expected!r} "
            f"(tolerance {tolerance})")


def _uv_sphere(radius=2.0, segments=8, rings=6):
    """A UV sphere as OBJ text, counter-clockwise seen from outside.

    Built, not loaded from an asset: the fixture is part of the test, so a
    change to the asset pipeline cannot silently change what is tested.

    The triangle count is exactly 2*segments*(rings-1) -- the two caps plus two
    triangles per middle quad. That identity is what makes
    test_a_remesh_of_a_dense_mesh_is_watertight able to assert the fixture is
    the plan's 100k-triangle mesh rather than approximately it.
    """
    lines = [f"v 0 0 {radius}"]            # 1: north pole
    for ring in range(1, rings):
        phi = math.pi * ring / rings
        z = radius * math.cos(phi)
        r = radius * math.sin(phi)
        for segment in range(segments):
            theta = 2.0 * math.pi * segment / segments
            lines.append(f"v {r * math.cos(theta):.6f} "
                         f"{r * math.sin(theta):.6f} {z:.6f}")
    lines.append(f"v 0 0 {-radius}")           # south pole
    south = 1 + (rings - 1) * segments + 1

    def index(ring, segment):
        return 1 + (ring - 1) * segments + (segment % segments) + 1

    for segment in range(segments):
        lines.append(f"f 1 {index(1, segment)} {index(1, segment + 1)}")
    for ring in range(1, rings - 1):
        for segment in range(segments):
            a = index(ring, segment)
            b = index(ring, segment + 1)
            c = index(ring + 1, segment + 1)
            d = index(ring + 1, segment)
            lines.append(f"f {a} {c} {b}")
            lines.append(f"f {a} {d} {c}")
    for segment in range(segments):
        lines.append(f"f {south} {index(rings - 1, segment + 1)} "
                     f"{index(rings - 1, segment)}")

    ok, data, error = Mesh.ParseOBJ("\n".join(lines) + "\n", "sphere.obj")
    check(ok, f"the sphere fixture did not parse: {error}")
    return Mesh.EditableMesh.Weld(data)


def _positions(mesh):
    return [(v.Position.x, v.Position.y, v.Position.z) for v in mesh.Vertices]


def _support(mesh, direction):
    """The support function: max over vertices of (position . direction).

    Compared over a fixed set of directions, this bounds the silhouette from
    outside in a way a bounding box cannot -- a box would pass a mesh that had
    collapsed to a rod along its diagonal.
    """
    dx, dy, dz = direction
    best = None
    for vertex in mesh.Vertices:
        p = vertex.Position
        value = p.x * dx + p.y * dy + p.z * dz
        if best is None or value > best:
            best = value
    return best if best is not None else 0.0


def _directions():
    """Six axis directions and the eight cube corners."""
    raw = []
    for axis in range(3):
        for sign in (-1.0, 1.0):
            direction = [0.0, 0.0, 0.0]
            direction[axis] = sign
            raw.append(tuple(direction))
    for x in (-1.0, 1.0):
        for y in (-1.0, 1.0):
            for z in (-1.0, 1.0):
                raw.append((x, y, z))
    return raw


# ---------------------------------------------------------------------------
# the plan's verification
# ---------------------------------------------------------------------------

def test_a_remesh_of_a_dense_mesh_is_watertight():
    """The plan's own check, literally: a 100k-triangle mesh remeshed to a
    20mm grid. The fixture is 2 units in radius standing in for a 2m asset, so
    20mm is 0.02 units."""
    started = time.time()
    mesh = _uv_sphere(radius=2.0, segments=400, rings=126)
    check_eq(mesh.FaceCount, 100000,
             "the fixture is the plan's 100k-triangle mesh")

    result, error = Mesh.VoxelRemesh(mesh, 0.02, 40000000)
    elapsed = time.time() - started

    check_eq(error, "", f"the remesh failed: {error}")
    check(result.VertexCount > 0, "the remesh produced no vertices")
    check_eq(result.Validate(), "", "the remesh is structurally sound")
    check_eq(Mesh.CountBoundaryEdges(result), 0,
             "the remesh of a closed mesh is watertight")
    print(f"      (100k triangles -> {result.FaceCount} triangles, "
          f"{result.VertexCount} vertices, {elapsed:.1f}s)")


def test_a_remesh_of_a_dense_mesh_keeps_the_silhouette():
    """The second half of the plan's check: the remeshed silhouette stays
    within 2.5 voxels of the original."""
    mesh = _uv_sphere(radius=2.0, segments=200, rings=63)
    voxel = 0.04
    result, error = Mesh.VoxelRemesh(mesh, voxel, 40000000)
    check_eq(error, "", f"the remesh failed: {error}")

    tolerance = 2.5 * voxel
    for direction in _directions():
        original = _support(mesh, direction)
        remeshed = _support(result, direction)
        check(abs(remeshed - original) <= tolerance,
              f"the silhouette along {direction} moved by "
              f"{abs(remeshed - original):.4f} (limit {tolerance:.4f})")


def test_a_remesh_of_a_sphere_is_watertight():
    """The same property on a coarse fixture, so a failure here is cheap to
    reproduce while the dense test is not."""
    mesh = _uv_sphere(radius=2.0, segments=24, rings=18)
    result, error = Mesh.VoxelRemesh(mesh, 0.1, 40000000)
    check_eq(error, "", f"the remesh failed: {error}")
    check(result.VertexCount > 0, "the remesh produced no vertices")
    check_eq(result.Validate(), "", "the remesh is structurally sound")
    check_eq(Mesh.CountBoundaryEdges(result), 0, "the remesh is watertight")


def test_a_remesh_refuses_a_non_positive_voxel_size():
    """A zero or negative voxel would divide by zero in the grid index and
    produce a garbage shell; refusing is the only sane answer."""
    mesh = _uv_sphere()
    for voxel in (0.0, -1.0):
        result, error = Mesh.VoxelRemesh(mesh, voxel, 40000000)
        check_eq(result.VertexCount, 0,
                 f"a voxel size of {voxel} produced a mesh")
        check(error != "", f"a voxel size of {voxel} reported no reason")


def test_a_remesh_refuses_a_grid_above_the_cap():
    """The occupancy array is a byte per voxel, so an unbounded grid is a
    memory blow-up, not a slow build."""
    mesh = _uv_sphere()
    result, error = Mesh.VoxelRemesh(mesh, 0.001, 1000)
    check_eq(result.VertexCount, 0, "an over-capped grid produced a mesh")
    check(error != "", "an over-capped grid reported no reason")


def test_a_remesh_of_an_empty_mesh_reports_an_error():
    mesh = Mesh.EditableMesh.Weld(Wasteland.MeshData())
    result, error = Mesh.VoxelRemesh(mesh, 0.1, 40000000)
    check_eq(result.VertexCount, 0, "an empty mesh produced a remesh")
    check(error != "", "an empty mesh reported no reason")


def test_an_oversized_voxel_still_produces_a_closed_shell():
    """The coarse end of the parameter range. At a voxel comparable to the
    model's radius the sphere cannot be represented at all, but the remesh
    must still come back closed, valid and centred on the model -- not empty
    and not an error. A parameter range that only works in its middle is a
    parameter range that will fail in someone's content pipeline."""
    mesh = _uv_sphere(radius=2.0, segments=12, rings=9)
    voxel = 2.0
    result, error = Mesh.VoxelRemesh(mesh, voxel, 40000000)
    check_eq(error, "", f"the remesh failed: {error}")
    check(result.VertexCount > 0, "the remesh produced no vertices")
    check_eq(result.Validate(), "", "the remesh is structurally sound")
    check_eq(Mesh.CountBoundaryEdges(result), 0, "the remesh is watertight")


def test_a_remesh_is_a_solid_not_a_hollow_shell():
    """The flood fill's own check. Without it the tube's inner boundary is
    extracted as a second surface and a closed asset comes back as a
    thin-walled void. A solid remesh has only the outer surface, so no vertex
    may sit deep inside the model."""
    mesh = _uv_sphere(radius=2.0, segments=24, rings=18)
    voxel = 0.1
    result, error = Mesh.VoxelRemesh(mesh, voxel, 40000000)
    check_eq(error, "", f"the remesh failed: {error}")

    const_radius = 2.0
    deep = 0
    for vertex in result.Vertices:
        p = vertex.Position
        if math.dist((0.0, 0.0, 0.0), (p.x, p.y, p.z)) < const_radius - 3.0 * voxel:
            deep += 1
    check_eq(deep, 0,
             f"{deep} vertices sit more than three voxels inside the model, "
             f"which is the inner wall of a hollow shell")


# ---------------------------------------------------------------------------
# decimation
# ---------------------------------------------------------------------------

def test_a_decimation_reaches_its_target():
    mesh = _uv_sphere(radius=2.0, segments=64, rings=48)
    before = mesh.FaceCount
    target = before // 10

    result, error = Mesh.DecimateMesh(mesh, target)
    check_eq(error, "", f"the decimation failed: {error}")
    check(result.FaceCount > 0, "the decimation emptied the mesh")
    check(result.FaceCount <= target,
          f"the decimation stopped at {result.FaceCount}, above the target "
          f"{target}")
    check(result.VertexCount < mesh.VertexCount,
          "the decimation removed no vertices")
    check_eq(result.Validate(), "", "the decimated mesh is structurally sound")
    print(f"      ({before} triangles -> {result.FaceCount}, "
          f"{mesh.VertexCount} -> {result.VertexCount} vertices)")


def test_a_decimation_preserves_the_silhouette():
    mesh = _uv_sphere(radius=2.0, segments=48, rings=36)
    target = mesh.FaceCount // 8
    result, error = Mesh.DecimateMesh(mesh, target)
    check_eq(error, "", f"the decimation failed: {error}")

    tolerance = 0.2
    for direction in _directions():
        original = _support(mesh, direction)
        decimated = _support(result, direction)
        check(abs(decimated - original) <= tolerance,
              f"the silhouette along {direction} moved by "
              f"{abs(decimated - original):.4f} (limit {tolerance})")


def test_a_decimation_keeps_a_closed_mesh_closed():
    """Edge contraction on a closed manifold mesh removes exactly the two
    triangles adjacent to the contracted edge and leaves every other edge
    paired, so the result stays watertight. A decimation that opened holes
    would be destroying the asset's topology, not simplifying it."""
    mesh = _uv_sphere(radius=2.0, segments=48, rings=36)
    check_eq(Mesh.CountBoundaryEdges(mesh), 0, "the fixture starts watertight")
    result, error = Mesh.DecimateMesh(mesh, mesh.FaceCount // 4)
    check_eq(error, "", f"the decimation failed: {error}")
    check_eq(Mesh.CountBoundaryEdges(result), 0,
             "the decimated mesh is still watertight")


def test_a_decimation_refuses_a_target_above_the_current_count():
    """A decimation that does not decimate is a caller bug, not a no-op: the
    caller has usually mis-sized the mesh."""
    mesh = _uv_sphere()
    result, error = Mesh.DecimateMesh(mesh, mesh.FaceCount + 1)
    check_eq(result.VertexCount, 0, "a no-op target produced a mesh")
    check(error != "", "a no-op target reported no reason")


def test_a_decimation_refuses_a_target_below_four():
    mesh = _uv_sphere()
    result, error = Mesh.DecimateMesh(mesh, 3)
    check_eq(result.VertexCount, 0, "a target of 3 produced a mesh")
    check(error != "", "a target of 3 reported no reason")


def test_a_decimation_of_an_empty_mesh_reports_an_error():
    mesh = Mesh.EditableMesh.Weld(Wasteland.MeshData())
    result, error = Mesh.DecimateMesh(mesh, 4)
    check_eq(result.VertexCount, 0, "an empty mesh produced a decimation")
    check(error != "", "an empty mesh reported no reason")


def test_count_boundary_edges_counts_a_quad_four_times():
    """A single quad is two triangles sharing one edge, so four of its five
    edges are on the boundary. This is the primitive the watertight
    assertions above rest on."""
    obj = ("v 0 0 0\n"
           "v 1 0 0\n"
           "v 1 1 0\n"
           "v 0 1 0\n"
           "f 1 2 3\n"
           "f 1 3 4\n")
    ok, data, error = Mesh.ParseOBJ(obj, "quad.obj")
    check(ok, f"the quad fixture did not parse: {error}")
    mesh = Mesh.EditableMesh.Weld(data)
    check_eq(mesh.FaceCount, 2, "the fixture is two triangles")
    check_eq(Mesh.CountBoundaryEdges(mesh), 4, "a quad has four boundary edges")


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
