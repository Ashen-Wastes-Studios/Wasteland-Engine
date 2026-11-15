"""Save backup ring: a crash between the publish and the next save must not
lose the previous version.

Phase J.6 of the parity plan ("a backup on save -- the last N versions, because
an editor crash during a save is a real event").

`SceneSerializer::Save` already publishes atomically (temp file + rename), which
protects against a CORRUPT file. The ring protects against the other failure:
the process dies after the rename and the old version is gone with no undo.
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

Ring = Wasteland.SaveBackupRing

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_backup_")
    _DIRS.append(path)
    return path


def _ring(keep=5):
    root = _root()
    return Ring(KeepCount=keep, Directory=os.path.join(root, "backups")), root


def _rotate(ring, target):
    """Rotate and return (ok, message). The binding raises on a real failure
    rather than returning an out-parameter."""
    try:
        ring.Rotate(target)
        return True, ""
    except RuntimeError as error:
        return False, str(error)


def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def test_a_missing_target_is_not_an_error():
    """The first save of a file has no previous version to preserve, and a caller
    that rotates unconditionally must not be forced to check first."""
    ring, root = _ring()
    target = os.path.join(root, "scene.wastescene")
    ok, message = _rotate(ring, target)
    check(ok, "a missing target was treated as an error: %s" % message)
    check_eq(message, "", "a no-op rotation reported an error")
    check_eq(ring.Count(target), 0, "a missing target produced a backup")


def test_the_first_rotation_copies_the_target_and_leaves_it_intact():
    """The caller publishes by renaming a temporary file OVER the target, so the
    target must survive Rotate. Renaming it away would turn a crash during the
    publish into a lost file."""
    ring, root = _ring()
    target = os.path.join(root, "scene.wastescene")
    _write(target, "version one")

    ok, message = _rotate(ring, target)
    check(ok, "the first rotate failed: %s" % message)

    check_eq(_read(target), "version one", "Rotate consumed the target")
    check_eq(ring.Count(target), 1, "one backup exists")
    backup = Wasteland.BackupPathFor(ring, target, 0)
    check(os.path.exists(backup), "slot 0 does not exist at %s" % backup)
    check_eq(_read(backup), "version one", "slot 0 holds the wrong content")


def test_the_second_rotation_shifts_the_first_backup_up():
    ring, root = _ring()
    target = os.path.join(root, "scene.wastescene")
    _write(target, "version one")
    _rotate(ring, target)

    _write(target, "version two")
    _rotate(ring, target)

    check_eq(ring.Count(target), 2, "two backups exist")
    check_eq(_read(Wasteland.BackupPathFor(ring, target, 0)), "version two",
             "slot 0 is not the newest version")
    check_eq(_read(Wasteland.BackupPathFor(ring, target, 1)), "version one",
             "slot 1 is not the previous version")


def test_the_ring_caps_at_keep_count_and_drops_the_oldest():
    ring, root = _ring(keep=3)
    target = os.path.join(root, "scene.wastescene")

    for index in range(1, 7):
        _write(target, "version %d" % index)
        _rotate(ring, target)

    check_eq(ring.Count(target), 3, "the ring did not cap at KeepCount")
    check_eq(_read(Wasteland.BackupPathFor(ring, target, 0)), "version 6",
             "slot 0 is not the newest")
    check_eq(_read(Wasteland.BackupPathFor(ring, target, 1)), "version 5",
             "slot 1 is wrong")
    check_eq(_read(Wasteland.BackupPathFor(ring, target, 2)), "version 4",
             "slot 2 is wrong -- the oldest was not dropped")
    check(not os.path.exists(Wasteland.BackupPathFor(ring, target, 3)),
          "a slot beyond KeepCount exists")


def test_a_ring_shifts_without_losing_a_middle_slot():
    """The shift walks from the oldest end backwards. A forwards walk would
    overwrite slot 0 before copying it to slot 1 and silently lose a version."""
    ring, root = _ring(keep=4)
    target = os.path.join(root, "scene.wastescene")
    for index in range(1, 5):
        _write(target, "v%d" % index)
        _rotate(ring, target)

    contents = [_read(Wasteland.BackupPathFor(ring, target, slot))
                for slot in range(4)]
    check_eq(contents, ["v4", "v3", "v2", "v1"],
             "the ring's contents are not newest-first")


def test_keep_count_zero_disables_the_ring():
    ring, root = _ring(keep=0)
    target = os.path.join(root, "scene.wastescene")
    _write(target, "only")
    ok, message = _rotate(ring, target)
    check(ok, "a disabled ring reported an error: %s" % message)
    check_eq(ring.Count(target), 0, "a disabled ring kept a backup")
    check_eq(_read(target), "only", "a disabled ring touched the target")


def test_the_backup_directory_is_created():
    ring, root = _ring()
    ring.Directory = os.path.join(root, "nested", "deeply", "backups")
    target = os.path.join(root, "scene.wastescene")
    _write(target, "x")
    _rotate(ring, target)
    check(os.path.isdir(ring.Directory), "the backup directory was not created")
    check_eq(ring.Count(target), 1, "no backup in the created directory")


def test_two_files_do_not_share_a_ring():
    """The ring is keyed by the target PATH, not by its stem: two scenes in
    different directories that share a name must not overwrite each other's
    backups."""
    ring, root = _ring()
    first = os.path.join(root, "a", "scene.wastescene")
    second = os.path.join(root, "b", "scene.wastescene")
    os.makedirs(os.path.dirname(first), exist_ok=True)
    os.makedirs(os.path.dirname(second), exist_ok=True)
    _write(first, "first")
    _write(second, "second")

    _rotate(ring, first)
    _rotate(ring, second)

    check_eq(ring.Count(first), 1, "the first file's ring was disturbed")
    check_eq(ring.Count(second), 1, "the second file's ring was disturbed")
    check_eq(_read(Wasteland.BackupPathFor(ring, first, 0)), "first",
             "the first file's backup is wrong")
    check_eq(_read(Wasteland.BackupPathFor(ring, second, 0)), "second",
             "the second file's backup is wrong")


def test_the_slot_paths_are_stable_across_calls():
    """A UI offering 'restore previous version' re-derives the paths; if they
    moved, the offer would open the wrong file."""
    ring, root = _ring()
    target = os.path.join(root, "scene.wastescene")
    first = Wasteland.BackupPathFor(ring, target, 0)
    second = Wasteland.BackupPathFor(ring, target, 0)
    check_eq(first, second, "the slot path is not deterministic")
    check(first != Wasteland.BackupPathFor(ring, target, 1),
          "two slots map to the same path")


def test_the_backup_preserves_the_extension():
    """Restore is a copy-and-rename; losing the extension would produce a file
    the engine will not open."""
    ring, root = _ring()
    target = os.path.join(root, "terrain.world")
    _write(target, "x")
    _rotate(ring, target)
    backup = Wasteland.BackupPathFor(ring, target, 0)
    check(backup.endswith(".world"),
          "the backup lost its extension: %s" % os.path.basename(backup))


def test_a_failed_rotation_reports_a_message():
    """A backup failure must be reportable: the caller logs and continues, and a
    silent failure would leave the user believing they are protected."""
    ring, root = _ring()
    target = os.path.join(root, "scene.wastescene")
    _write(target, "x")
    # Point the ring at a path whose parent is a FILE, so create_directories
    # cannot succeed.
    blocker = os.path.join(root, "blocker")
    _write(blocker, "not a directory")
    ring.Directory = os.path.join(blocker, "backups")

    ok, message = _rotate(ring, target)
    check(not ok, "a rotation into an impossible directory reported success")
    check(bool(message), "a failed rotation reported no message")
    check_eq(_read(target), "x", "a failed rotation damaged the target")


def test_the_target_is_untouched_when_the_rotation_fails():
    ring, root = _ring()
    target = os.path.join(root, "scene.wastescene")
    _write(target, "precious")
    blocker = os.path.join(root, "blocker")
    _write(blocker, "not a directory")
    ring.Directory = os.path.join(blocker, "backups")
    _rotate(ring, target)
    check_eq(_read(target), "precious", "the target changed on a failed rotation")


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
