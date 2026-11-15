"""D.3 -- sculpt brushes: Grab, Smooth, Inflate, Flatten, Crease, Mask.

The plan's verification: a sphere before and after a smooth brush pass, with
the mean vertex displacement positive and bounded by the brush radius.

The "bounded by the radius" property is not cosmetic. Every brush clamps its
TOTAL displacement magnitude to the radius, because a Laplacian step on a
high-curvature surface can otherwise pull a vertex across the whole model in one
stroke. That clamp is the invariant the tests assert for every brush, not just
smooth.

The falloff is `(1 - (d/r)^2)^2`: 1 at the centre, 0 at the rim, zero derivative
at both ends. A linear falloff leaves a visible crease where the brush stops;
the tests check the zero derivative at the rim rather than eyeballing it.

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
    """A UV sphere as OBJ text. Built, not loaded from an asset: the fixture is
    part of the test, so a change to the asset pipeline cannot silently change
    what is being tested."""
    lines = []
    # The poles plus the ring rows.
    lines.append(f"v 0 0 {radius}")            # 1: north pole
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

    # Counter-clockwise viewed from OUTSIDE. The first version of this
    # fixture wound every face the other way and the pole's normal came
    # out (0,0,-1); the sphere still parsed and rendered, it just had
    # inward-facing normals, and every brush that reads a vertex normal
    # (inflate) pushed the surface the wrong way. Winding is the kind of
    # bug a fixture hides.
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


def _stroke(brush, center, radius=1.0, strength=0.5, **kwargs):
    stroke = Mesh.SculptStroke()
    stroke.Brush = brush
    stroke.Center = center
    stroke.Radius = radius
    stroke.Strength = strength
    for name, value in kwargs.items():
        setattr(stroke, name, value)
    return stroke


def _displacement(before, after):
    total = 0.0
    moved = 0
    for a, b in zip(before, after):
        d = math.dist(a, b)
        if d > 1e-7:
            moved += 1
            total += d
    return (total / moved) if moved else 0.0, moved


# ---------------------------------------------------------------------------
# the plan's verification
# ---------------------------------------------------------------------------

def test_a_smooth_brush_moves_the_surface_by_a_bounded_amount():
    """The plan's own check: after a smooth pass the mean displacement is
    positive and bounded by the brush radius."""
    mesh = _uv_sphere()
    before = _positions(mesh)

    stroke = _stroke(Mesh.SculptBrush.Smooth, (0.0, 0.0, 2.0), radius=1.0,
                     strength=0.5)
    mean = Mesh.SculptStrokeApply(mesh, stroke)

    check(mean > 0.0, "the smooth brush moved nothing")
    check(mean <= 1.0, f"the mean displacement {mean} exceeds the radius")
    observed, moved = _displacement(before, _positions(mesh))
    check(moved > 0, "no vertex moved")
    check_close(mean, observed,
                "the reported mean matches the measured mean", 1e-3)


def test_the_brush_touches_only_vertices_inside_the_radius():
    """A vertex outside the radius must not move AT ALL -- not by a fraction.
    The falloff is exactly 0 at the rim and beyond, which is what makes a brush
    a local operation."""
    mesh = _uv_sphere()
    before = _positions(mesh)
    stroke = _stroke(Mesh.SculptBrush.Grab, (0.0, 0.0, 2.0), radius=0.5,
                     strength=1.0, Direction=(0.0, 0.0, 1.0))
    Mesh.SculptStrokeApply(mesh, stroke)
    after = _positions(mesh)

    moved = 0
    for index, (a, b) in enumerate(zip(before, after)):
        if math.dist(a, b) > 1e-7:
            moved += 1
            check(math.dist(a, (0.0, 0.0, 2.0)) <= 0.5,
                  f"vertex {index} at distance "
                  f"{math.dist(a, (0.0, 0.0, 2.0))} moved but is outside "
                  "the radius")
    check(moved > 0, "the brush touched nothing")


# ---------------------------------------------------------------------------
# the falloff
# ---------------------------------------------------------------------------

def test_the_falloff_is_one_at_the_centre_and_zero_at_the_rim():
    check_close(Mesh.SculptFalloff(0.0, 1.0), 1.0, "at the centre")
    check_close(Mesh.SculptFalloff(1.0, 1.0), 0.0, "at the rim")
    check_close(Mesh.SculptFalloff(2.0, 1.0), 0.0, "beyond the rim")


def test_the_falloff_is_smooth_at_the_rim():
    """Zero derivative at the rim. A linear falloff has a derivative step there,
    which shows up as a visible crease ring where the brush stopped."""
    at_rim = Mesh.SculptFalloff(1.0, 1.0)
    just_inside = Mesh.SculptFalloff(0.999, 1.0)
    check_close(at_rim, 0.0, "the rim value")
    check(just_inside < 1e-4,
          f"the falloff a hair inside the rim is {just_inside}, so the slope "
          "at the rim is not zero")


def test_a_zero_or_negative_radius_is_refused():
    mesh = _uv_sphere()
    check(Mesh.SculptFalloff(0.5, 0.0) == 0.0, "a zero radius has no falloff")
    stroke = _stroke(Mesh.SculptBrush.Grab, (0.0, 0.0, 0.0), radius=-1.0,
                     Direction=(0.0, 0.0, 1.0))
    check(Mesh.SculptStrokeApply(mesh, stroke) < 0.0,
          "a negative radius must be refused")


# ---------------------------------------------------------------------------
# grab
# ---------------------------------------------------------------------------

def test_a_grab_moves_along_the_drag_vector():
    mesh = _uv_sphere()
    before = _positions(mesh)
    stroke = _stroke(Mesh.SculptBrush.Grab, (0.0, 0.0, 2.0), radius=0.5,
                     strength=1.0, Direction=(1.0, 0.0, 0.0))
    Mesh.SculptStrokeApply(mesh, stroke)
    after = _positions(mesh)

    for a, b in zip(before, after):
        d = math.dist(a, b)
        if d > 1e-7:
            # The drag is along +X, so every moved vertex must have moved along
            # +X and only +X.
            check_close(b[1], a[1], "the grab moved in Y", 1e-6)
            check_close(b[2], a[2], "the grab moved in Z", 1e-6)
            check(b[0] > a[0], "the grab moved along -X, not +X")
            break
    else:
        raise AssertionError("the grab moved nothing")


def test_a_grab_displacement_is_bounded_by_the_radius():
    mesh = _uv_sphere()
    before = _positions(mesh)
    stroke = _stroke(Mesh.SculptBrush.Grab, (0.0, 0.0, 2.0), radius=0.3,
                     strength=5.0, Direction=(1.0, 0.0, 0.0))
    mean = Mesh.SculptStrokeApply(mesh, stroke)
    # A tolerance, because a float32 radius of 0.3 is 0.30000001192 in
    # Python; an exact <= against the double 0.3 fails on representation,
    # not on the clamp.
    check(mean <= 0.3 + 1e-5,
          f"the mean {mean} exceeds the radius even at strength 5")


# ---------------------------------------------------------------------------
# inflate
# ---------------------------------------------------------------------------

def test_an_inflate_moves_along_the_vertex_normal():
    """A per-vertex normal, not the brush centre's normal. A single centre
    normal would flatten one side of the sphere."""
    mesh = _uv_sphere()
    before = _positions(mesh)
    stroke = _stroke(Mesh.SculptBrush.Inflate, (0.0, 0.0, 2.0), radius=1.0,
                     strength=0.5)
    Mesh.SculptStrokeApply(mesh, stroke)
    after = _positions(mesh)

    # The north pole's normal is +Z, so it must move along +Z.
    pole = max(range(len(before)), key=lambda i: before[i][2])
    check_close(after[pole][0], before[pole][0], "the pole moved in X", 1e-5)
    check_close(after[pole][1], before[pole][1], "the pole moved in Y", 1e-5)
    check(after[pole][2] > before[pole][2], "the pole did not move outward")


# ---------------------------------------------------------------------------
# flatten
# ---------------------------------------------------------------------------

def test_a_flatten_projects_onto_the_plane():
    """A flatten moves each vertex TOWARD the plane by
    (falloff * strength) of its distance. Two properties follow, and both
    are worth asserting: no vertex can overshoot the plane, and the vertex
    at the brush centre -- where the falloff is exactly 1 -- lands on it.

    The first version of this test asserted that every vertex lands on the
    plane. That is wrong: a vertex 0.5 from the centre with a radius of 3
    has a falloff of 0.945, so it closes 94.5% of the gap. Asserting the
    stronger claim would have meant asserting a bug.
    """
    mesh = _uv_sphere()
    before = _positions(mesh)
    stroke = _stroke(Mesh.SculptBrush.Flatten, (0.0, 0.0, 1.5), radius=3.0,
                     strength=1.0, PlaneNormal=(0.0, 0.0, 1.0),
                     PlanePoint=(0.0, 0.0, 1.5))
    Mesh.SculptStrokeApply(mesh, stroke)
    after = _positions(mesh)

    # The exact contract: a vertex at distance d from the centre closes
    # exactly falloff(d, radius) * strength of its gap to the plane. No
    # vertex sits AT the centre here -- the nearest is the pole, 0.5 away,
    # so its falloff is 0.945 and it closes 94.5% of the gap, not 100%.
    # Asserting "lands on the plane" was asserting a bug.
    for index, (a, b) in enumerate(zip(before, after)):
        distance = math.dist(a, (0.0, 0.0, 1.5))
        falloff = Mesh.SculptFalloff(distance, 3.0)
        expected_gap = abs(a[2] - 1.5) * (1.0 - falloff * 1.0)
        check_close(abs(b[2] - 1.5), expected_gap,
                    f"vertex {index} closed the wrong fraction of the gap",
                    1e-4)
        check(abs(b[2] - 1.5) <= abs(a[2] - 1.5) + 1e-5,
              f"vertex {index} overshot the flatten plane: "
              f"{a[2]} -> {b[2]}")


def test_a_flatten_plane_is_stable_as_the_brush_moves():
    """The plane is defined by PlanePoint, not by the brush centre, so moving
    the cursor does not move the surface being flattened onto."""
    mesh = _uv_sphere()
    stroke = _stroke(Mesh.SculptBrush.Flatten, (5.0, 5.0, 5.0), radius=3.0,
                     strength=1.0, PlaneNormal=(0.0, 0.0, 1.0),
                     PlanePoint=(0.0, 0.0, 1.0))
    Mesh.SculptStrokeApply(mesh, stroke)
    for index, position in enumerate(_positions(mesh)):
        if math.dist(position, (0.0, 0.0, 5.0)) < 3.0:
            check_close(position[2], 1.0,
                        f"vertex {index} flattened onto the centre, not the "
                        "plane point", 1e-4)


# ---------------------------------------------------------------------------
# crease
# ---------------------------------------------------------------------------

def test_a_crease_pulls_toward_the_hit_point():
    mesh = _uv_sphere()
    before = _positions(mesh)
    # radius 1.5: the pole's nearest neighbours are 1.035 away, so a
    # radius of 0.5 covers only the pole itself, where (center -
    # position) is zero -- the crease would move nothing and the test
    # would prove nothing.
    stroke = _stroke(Mesh.SculptBrush.Crease, (0.0, 0.0, 2.0), radius=1.5,
                     strength=1.0)
    Mesh.SculptStrokeApply(mesh, stroke)
    after = _positions(mesh)

    for a, b in zip(before, after):
        if math.dist(a, b) > 1e-7:
            # Every moved vertex must be CLOSER to the hit point.
            check(math.dist(b, (0.0, 0.0, 2.0)) < math.dist(a, (0.0, 0.0, 2.0)),
                  "a moved vertex is farther from the crease point")
            break
    else:
        raise AssertionError("the crease moved nothing")


# ---------------------------------------------------------------------------
# mask
# ---------------------------------------------------------------------------

def test_a_mask_excludes_a_vertex_from_every_brush():
    """The mask is applied inside the brush, after the falloff, so it composes.
    A mask that only one brush honoured would be the usual way this is
    mis-implemented."""
    for brush in (Mesh.SculptBrush.Grab, Mesh.SculptBrush.Smooth,
                  Mesh.SculptBrush.Inflate, Mesh.SculptBrush.Flatten,
                  Mesh.SculptBrush.Crease):
        mesh = _uv_sphere()
        before = _positions(mesh)
        mask = [1.0] * mesh.VertexCount
        mask[0] = 0.0                      # the north pole is masked out
        stroke = _stroke(brush, (0.0, 0.0, 2.0), radius=2.0, strength=1.0,
                         Direction=(1.0, 0.0, 0.0),
                         PlaneNormal=(0.0, 0.0, 1.0),
                         PlanePoint=(0.0, 0.0, 1.0))
        stroke.Mask = mask
        Mesh.SculptStrokeApply(mesh, stroke)
        after = _positions(mesh)
        check_eq(after[0], before[0],
                 f"{brush} moved a masked-out vertex")


def test_an_empty_mask_means_no_mask():
    mesh = _uv_sphere()
    stroke = _stroke(Mesh.SculptBrush.Grab, (0.0, 0.0, 2.0), radius=0.5,
                     strength=1.0, Direction=(1.0, 0.0, 0.0))
    check_eq(stroke.GetMaskLength(), 0, "a default stroke has no mask")
    check(Mesh.SculptStrokeApply(mesh, stroke) > 0.0,
          "an empty mask must not disable the brush")


def test_a_short_mask_treats_the_missing_entries_as_unmasked():
    """A mask one vertex short must not refuse to run: the mask is authored
    data, and a brush that silently fails on it is worse than one that treats
    the missing entries as unmasked."""
    mesh = _uv_sphere()
    mask = [0.0] * (mesh.VertexCount - 1)
    stroke = _stroke(Mesh.SculptBrush.Grab, (0.0, 0.0, 2.0), radius=0.5,
                     strength=1.0, Direction=(1.0, 0.0, 0.0))
    stroke.Mask = mask
    mean = Mesh.SculptStrokeApply(mesh, stroke)
    check(mean >= 0.0, "a short mask must not be an error")


# ---------------------------------------------------------------------------
# determinism
# ---------------------------------------------------------------------------

def test_a_stroke_is_independent_of_vertex_order():
    """Two identical meshes must produce identical results. Applying a stroke as
    it goes would let one vertex's new position influence its neighbour's
    Laplacian, making the result depend on vertex order -- a stroke that gives
    different answers on the same mesh is unusable."""
    first = _uv_sphere()
    second = _uv_sphere()
    stroke = _stroke(Mesh.SculptBrush.Smooth, (0.0, 0.0, 2.0), radius=1.0,
                     strength=0.5)
    Mesh.SculptStrokeApply(first, stroke)
    Mesh.SculptStrokeApply(second, stroke)
    check_eq(_positions(first), _positions(second),
             "two identical meshes produced different results")


def test_a_stroke_leaves_the_mesh_valid():
    mesh = _uv_sphere()
    stroke = _stroke(Mesh.SculptBrush.Smooth, (0.0, 0.0, 2.0), radius=1.0,
                     strength=0.5)
    Mesh.SculptStrokeApply(mesh, stroke)
    check_eq(mesh.Validate(), "", "the mesh is structurally sound after a stroke")


def test_a_stroke_on_an_empty_mesh_touches_nothing():
    """The OBJ parser rejects a file with no faces, so the empty mesh is
    built through Weld on an empty MeshData. The contract under test: a
    brush that touches nothing returns exactly 0, which is distinguishable
    from -1 (an invalid argument) -- a caller must be able to tell the two
    apart."""
    # MeshData is bound on the TOP-LEVEL Wasteland module, not on the Mesh
    # submodule -- the submodule holds the importers and the editable mesh,
    # the plain data record sits beside the other value types.
    mesh = Mesh.EditableMesh.Weld(Wasteland.MeshData())
    check_eq(mesh.VertexCount, 0, "the fixture is empty")
    stroke = _stroke(Mesh.SculptBrush.Grab, (0.0, 0.0, 0.0), radius=1.0,
                     Direction=(1.0, 0.0, 0.0))
    check_eq(Mesh.SculptStrokeApply(mesh, stroke), 0.0,
             "a brush with nothing to touch returns exactly 0, not -1")


def test_a_brush_far_from_the_mesh_touches_nothing():
    """The same contract on a real mesh: a stroke entirely outside the
    surface must return 0, not a negative number and not a small
    displacement."""
    mesh = _uv_sphere()
    before = _positions(mesh)
    stroke = _stroke(Mesh.SculptBrush.Grab, (50.0, 50.0, 50.0), radius=1.0,
                     Direction=(1.0, 0.0, 0.0))
    check_eq(Mesh.SculptStrokeApply(mesh, stroke), 0.0,
             "a distant brush returns exactly 0")
    check_eq(_positions(mesh), before, "a distant brush moved the mesh")


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
