"""The deterministic + incremental cook core (Phase J.2).

The plan says a cook "must be deterministic and incremental -- a cook that reruns
from scratch is not usable on a large project." This tests the core that makes
that true, independent of WHAT is being cooked: an asset is re-cooked if and only
if its CONTENT changed, never because its mtime moved.
"""
import atexit
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_ge

import wlpath

wlpath.setup()

import Wasteland

AssetCooker = Wasteland.AssetCooker
Fnv1a64 = Wasteland.Fnv1a64
HashBytesOfFile = Wasteland.HashBytesOfFile

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_cook_")
    _DIRS.append(path)
    return path


def _write(path, data):
    with open(path, "wb") as handle:
        handle.write(data)


def _read(path):
    with open(path, "rb") as handle:
        return handle.read()


class _Op:
    """Counts how many times the cook operation actually ran."""

    def __init__(self, source, output):
        self.count = 0
        self.source = source
        self.output = output

    def __call__(self, out):
        self.count += 1
        shutil.copyfile(self.source, self.output)
        return True


# ---------------------------------------------------------------------------
# Hashes
# ---------------------------------------------------------------------------

def test_fnv1a_is_deterministic_and_order_sensitive():
    check_eq(Fnv1a64(b""), 14695981039346656037, "FNV-1a offset basis")
    check_eq(Fnv1a64(b"hello"), Fnv1a64(b"hello"), "same bytes, same hash")
    check(Fnv1a64(b"ab") != Fnv1a64(b"ba"),
          "a permutation of bytes must not collide")


def test_hash_bytes_of_file_matches_the_inline_hash():
    root = _root()
    path = os.path.join(root, "asset.bin")
    payload = b"mesh bytes " * 100
    _write(path, payload)

    hash_value, error = HashBytesOfFile(path)
    check_eq(hash_value, Fnv1a64(payload), "file hash disagrees with inline hash")
    check_eq(error, "", "a readable file reported an error: %s" % error)


def test_hash_of_a_missing_file_returns_zero_and_an_error():
    hash_value, error = HashBytesOfFile(os.path.join(_root(), "nope.bin"))
    check_eq(hash_value, 0, "a missing file hashed to a non-zero value")
    check(bool(error), "a missing file reported no error")


def test_touching_the_mtime_does_not_change_the_hash():
    root = _root()
    path = os.path.join(root, "asset.bin")
    _write(path, b"fixed content")
    first, _ = HashBytesOfFile(path)

    # Touch without changing content.
    os.utime(path, (1, 1))
    second, _ = HashBytesOfFile(path)
    check_eq(first, second, "a touch changed the content hash")


# ---------------------------------------------------------------------------
# Incremental cooking
# ---------------------------------------------------------------------------

def test_the_first_cook_runs_and_second_is_a_noop():
    root = _root()
    source = os.path.join(root, "src.bin")
    output = os.path.join(root, "cooked.bin")
    _write(source, b"version one")

    op = _Op(source, output)
    cooker = AssetCooker()

    cooked, error = cooker.CookIfStale(source, output, op)
    check(cooked, "the first cook did not run its operation")
    check_eq(op.count, 1, "the operation ran a different number of times")
    check_eq(_read(output), b"version one", "the output was not written")
    check(os.path.exists(output + ".cook"), "no stamp was written")

    cooked, error = cooker.CookIfStale(source, output, op)
    check(not cooked, "the second cook re-ran (not incremental)")
    check_eq(op.count, 1, "a no-op cook still ran the operation")
    check_eq(error, "", "a no-op cook reported an error: %s" % error)


def test_touching_the_source_does_not_recook():
    """The whole point of a content hash over an mtime: a `touch` must not make a
    large project rebuild itself."""
    root = _root()
    source = os.path.join(root, "src.bin")
    output = os.path.join(root, "cooked.bin")
    _write(source, b"content")
    op = _Op(source, output)
    cooker = AssetCooker()

    cooker.CookIfStale(source, output, op)
    os.utime(source, (2, 2))
    cooked, _ = cooker.CookIfStale(source, output, op)
    check(not cooked, "a touch caused a re-cook")
    check_eq(op.count, 1, "the operation ran after a touch")


def test_changing_the_content_recooks():
    root = _root()
    source = os.path.join(root, "src.bin")
    output = os.path.join(root, "cooked.bin")
    _write(source, b"version one")
    op = _Op(source, output)
    cooker = AssetCooker()

    cooker.CookIfStale(source, output, op)
    _write(source, b"version two")
    cooked, _ = cooker.CookIfStale(source, output, op)
    check(cooked, "a content change did not re-cook")
    check_eq(op.count, 2, "a content change is not one cook")
    check_eq(_read(output), b"version two", "the output was not refreshed")


def test_cooking_the_same_bytes_from_a_different_file_is_already_current():
    """Incrementality follows the source CONTENT, not its path: a cook of identical
    bytes is a no-op even if the filename is new."""
    root = _root()
    source_a = os.path.join(root, "a.bin")
    source_b = os.path.join(root, "b.bin")
    _write(source_a, b"identical")
    _write(source_b, b"identical")
    output = os.path.join(root, "cooked.bin")

    op_a = _Op(source_a, output)
    cooker = AssetCooker()
    cooker.CookIfStale(source_a, output, op_a)

    op_b = _Op(source_b, output)
    cooked, _ = cooker.CookIfStale(source_b, output, op_b)
    check(not cooked, "identical bytes were re-cooked under a new path")
    check_eq(op_b.count, 0, "the second operation ran at all")


def test_a_pipeline_of_cooks_tracks_each_output_independently():
    root = _root()
    first_source = os.path.join(root, "one.bin")
    second_source = os.path.join(root, "two.bin")
    _write(first_source, b"first")
    _write(second_source, b"second")
    out_one = os.path.join(root, "out_one.bin")
    out_two = os.path.join(root, "out_two.bin")

    cooker = AssetCooker()
    op = _Op
    cooker.CookIfStale(first_source, out_one, op(first_source, out_one))
    cooker.CookIfStale(second_source, out_two, op(second_source, out_two))

    # Re-cook the whole project: both are current.
    cooked1, _ = cooker.CookIfStale(first_source, out_one, op(first_source, out_one))
    cooked2, _ = cooker.CookIfStale(second_source, out_two, op(second_source, out_two))
    check(not cooked1 and not cooked2,
          "a second pass over the project re-cooked current outputs")

    # Change ONE; only it re-cooks.
    _write(first_source, b"first v2")
    cooked1, _ = cooker.CookIfStale(first_source, out_one, op(first_source, out_one))
    cooked2, _ = cooker.CookIfStale(second_source, out_two, op(second_source, out_two))
    check(cooked1, "the changed asset was not re-cooked")
    check(not cooked2, "an unchanged asset re-cooked when its sibling changed")


def test_a_failed_cook_reports_an_error_and_does_not_take_down_the_pipeline():
    root = _root()
    source = os.path.join(root, "src.bin")
    output = os.path.join(root, "cooked.bin")
    _write(source, b"content")
    cooker = AssetCooker()

    raised = None
    try:
        cooker.CookIfStale(source, output, lambda out: False)
    except RuntimeError as error:
        raised = error
    check(raised is not None, "a failed operation did not raise")
    check(not os.path.exists(output), "a failed cook still wrote its output")
    check(not os.path.exists(output + ".cook"), "a failed cook still wrote a stamp")


def test_the_stamp_is_the_hex_hash():
    root = _root()
    source = os.path.join(root, "src.bin")
    output = os.path.join(root, "cooked.bin")
    _write(source, b"stamped")
    op = _Op(source, output)
    AssetCooker().CookIfStale(source, output, op)

    content = _read(output + ".cook")
    expected = ("%016x" % Fnv1a64(b"stamped")).encode("ascii")
    check_eq(content, expected,
             "the stamp is not the hex of the source hash: %r" % content)


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
