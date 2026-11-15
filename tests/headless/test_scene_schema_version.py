"""Scene layout versioning: a scene written by a NEWER engine must fail loudly,
and one written by an older build (or by hand) must still open.

Phase J.6 of the parity plan. The pattern is the one Project::Settings already
uses (Project.h:34-38): a Version key, a missing key read as current, and forward
migration rather than rejection.
"""
import atexit
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

Scene = Wasteland.Scene
SCHEMA_VERSION = Wasteland.SCENE_SCHEMA_VERSION

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_scene_version_")
    _DIRS.append(path)
    return path


def _scene():
    scene = Scene()
    scene.OnSimulationStart()
    return scene


def _save(scene, name="scene.wastescene"):
    path = os.path.join(_root(), name)
    check(scene.SaveScene(path), "the save must succeed")
    return path


def _read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _strip_version(text):
    """Remove the Version line the writer emitted, simulating a file saved before
    the key existed."""
    return re.sub(r"^Version:.*\n", "", text, count=1, flags=re.M)


def test_the_constant_is_exposed_and_positive():
    check(isinstance(SCHEMA_VERSION, int),
          "SCENE_SCHEMA_VERSION is not an int: %r" % (SCHEMA_VERSION,))
    check(SCHEMA_VERSION >= 1,
          "the schema version must be at least 1, got %r" % (SCHEMA_VERSION,))


def test_a_saved_scene_declares_the_current_version():
    path = _save(_scene())
    text = _read(path)
    found = re.search(r"^Version:\s*(\d+)\s*$", text, re.M)
    check(found is not None, "the saved scene has no Version key:\n%s" % text[:200])
    check_eq(int(found.group(1)), SCHEMA_VERSION, "the written layout version")


def test_the_version_is_the_first_key():
    """A version belongs at the top: a reader that stops at the first unknown key
    still sees it, and a human opening the file sees it first."""
    path = _save(_scene())
    lines = [line for line in _read(path).splitlines() if line.strip()]
    check(bool(lines), "the saved scene is empty")
    check(lines[0].startswith("Version:"),
          "the first key is not Version: %r" % lines[0])


def test_a_scene_without_a_version_key_still_loads():
    """The Project::Settings rule: a missing Version reads as current, so every
    scene written before this key existed still opens unchanged."""
    path = _save(_scene())
    stripped = _strip_version(_read(path))
    check("Version:" not in stripped, "the Version line was not stripped")

    legacy = os.path.join(_root(), "legacy.wastescene")
    _write(legacy, stripped)

    scene = _scene()
    check(scene.LoadScene(legacy), "a scene with no Version key was rejected")


def test_a_scene_from_a_newer_engine_is_rejected():
    """Reading a layout this build does not understand would half-load it, and a
    subsequent SAVE would silently discard every field this build never saw."""
    path = _save(_scene())
    text = re.sub(r"^Version:.*$", "Version: %d" % (SCHEMA_VERSION + 1),
                  _read(path), count=1, flags=re.M)

    future = os.path.join(_root(), "future.wastescene")
    _write(future, text)

    check(not _scene().LoadScene(future),
          "a scene from a newer engine was accepted")


def test_an_unreadable_version_is_rejected():
    path = _save(_scene())
    text = re.sub(r"^Version:.*$", "Version: not-a-number",
                  _read(path), count=1, flags=re.M)

    broken = os.path.join(_root(), "broken.wastescene")
    _write(broken, text)

    check(not _scene().LoadScene(broken),
          "a scene with a non-numeric Version was accepted")


def test_a_versioned_scene_round_trips():
    source = _scene()
    first = _save(source, "first.wastescene")

    middle = _scene()
    check(middle.LoadScene(first), "the load must succeed")
    second = os.path.join(_root(), "second.wastescene")
    check(middle.SaveScene(second), "the second save must succeed")

    # save -> load -> save must be byte-stable: a field read but not written makes
    # the second save differ, which is how a silently dropped field shows up.
    check_eq(_read(first), _read(second),
             "save -> load -> save was not byte-stable")


def test_a_version_zero_is_treated_as_an_older_layout():
    """Version 0 is older than current, so it must load (a migration would run)
    rather than being rejected as invalid."""
    path = _save(_scene())
    text = re.sub(r"^Version:.*$", "Version: 0", _read(path), count=1, flags=re.M)

    older = os.path.join(_root(), "older.wastescene")
    _write(older, text)

    check(_scene().LoadScene(older), "an older layout version was rejected")


def test_the_written_version_survives_a_load():
    """Loading must not strip or rewrite the key: the file on disk is untouched by
    a load, and the next save writes the same version."""
    path = _save(_scene())
    before = _read(path)
    _scene().LoadScene(path)
    check_eq(_read(path), before, "loading a scene rewrote the file on disk")


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
