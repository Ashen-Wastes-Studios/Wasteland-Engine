"""F.7 v1: bone-silhouette thumbnail raster (CPU, headless).

The plan's animation thumbnail v1 is a CPU raster of the bone hierarchy's first
pose -- cheap, recognizable, and verifiable without a GL device, exactly like the
graph thumbnail (F.4). The raster takes WORLD-SPACE bone positions + parent
indices (which the caller computes from a pose via the Animation API); this tests
the raster itself. F.7's stated verifications: deterministic (same input twice ->
same buffer), and different poses -> different buffers.
"""
import atexit
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

Make = Wasteland.MakeBoneHierarchyThumbnail

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _count_nonbackground(result):
    return sum(
        1 for i in range(0, len(result.Pixels), 4)
        if result.Pixels[i] != 20 or result.Pixels[i + 1] != 20
        or result.Pixels[i + 2] != 24)


def _count_nonbackground_rows(result, y0, y1, size):
    count = 0
    for y in range(y0, y1):
        for x in range(size):
            base = (y * size + x) * 4
            if result.Pixels[base] != 20 or result.Pixels[base + 1] != 20 \
                    or result.Pixels[base + 2] != 24:
                count += 1
    return count


def _two_bone():
    # root at origin, child at (0, 1, 0).
    return ([0, 0, 0,  0.0, 1.0, 0.0], [-1, 0])


def test_a_two_bone_hierarchy_renders_silhouette_pixels():
    positions, parents = _two_bone()
    result = Make(positions, parents, 64)
    check(result.BoneCount == 2, "wrong bone count")
    check(_count_nonbackground(result) > 10,
          "a two-bone hierarchy drew almost nothing")


def test_the_buffer_shape_is_size_squared_rgba():
    result = Make(*_two_bone(), 32)
    check_eq(len(result.Pixels), 32 * 32 * 4,
             "the buffer is not size*size*4 bytes")
    check_eq(result.Size, 32, "wrong size reported")


def test_the_same_input_produces_an_identical_buffer_twice():
    a = Make(*_two_bone(), 64)
    b = Make(*_two_bone(), 64)
    check_eq(a.Pixels, b.Pixels,
             "the raster is not deterministic (F.7's determinism check)")


def test_two_different_hierarchies_produce_different_buffers():
    a = Make([0, 0, 0,  0, 1, 0], [-1, 0], 64)
    b = Make([0, 0, 0,  2, 0, 0], [-1, 0], 64)  # child to the right, not up
    check(a.Pixels != b.Pixels,
          "two different poses rendered identical thumbnails")


def test_a_one_bone_hierarchy_differs_from_a_two_bone_one():
    one = Make([0, 0, 0], [-1], 64)
    two = Make(*_two_bone(), 64)
    check(one.Pixels != two.Pixels,
          "adding a bone changed nothing in the thumbnail")


def test_a_child_extent_grows_along_its_direction():
    """The silhouette reflects LENGTH, not just counts: a long child in +Y must
    draw more in the upper half of the image (the y-flip maps +Y up)."""
    positions = [0, 0, 0,  0, 5, 0]
    result = Make(positions, [-1, 0], 64)
    # The top row of the image is y = +max (the flip maps +Y up). A bone reaching
    # +Y must paint pixels up there -- a bone that only reached -Y would not.
    top_rows = _count_nonbackground_rows(result, 0, 16, 64)
    check(top_rows > 0, "a +Y bone never reached the upper quarter")


def test_a_hierarchy_with_no_bones_is_all_background():
    result = Make([], [], 32)
    check_eq(result.BoneCount, 0, "an empty hierarchy reported bones")
    check_eq(_count_nonbackground(result), 0,
             "an empty hierarchy drew something")


def test_size_zero_is_an_empty_result():
    result = Make(*_two_bone(), 0)
    check_eq(result.Pixels, [], "size zero produced a buffer")
    check_eq(result.Size, 0, "size zero reported a nonzero size")


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
