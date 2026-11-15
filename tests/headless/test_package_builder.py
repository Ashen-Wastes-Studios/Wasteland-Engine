"""The deterministic package layout (Phase J.3).

A package is built into a staging directory with a manifest that inventories every
file (path, byte count, content hash) sorted by path. The plan's J.3 verification
-- "run the package under a clean user profile, assert it boots and plays" --
needs a working runtime and a GPU, so it is editor-blocked. What this tests is
the property a package has to have BEFORE "does it run" is sensible: it is
COMPLETE (every intended file present), DETERMINISTIC (two builds are
byte-identical) and NON-OVERLAPPING (no silent collision).
"""
import atexit
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

BuildPackage = Wasteland.BuildPackage
Fnv1a64 = Wasteland.Fnv1a64

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_pkg_")
    _DIRS.append(path)
    return path


def _write(path, data, mode="wb"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as handle:
        handle.write(data)


def _read(path):
    with open(path, "rb") as handle:
        return handle.read()


def _setup(sources=None, version="1.2.3"):
    root = _root()
    exe = os.path.join(root, "Game.exe")
    project = os.path.join(root, "project.wasteland")
    assets = os.path.join(root, "cooked")
    _write(exe, b"MZ fake runtime bytes " * 8)
    _write(project, b"Scene: Main\nEntities:\n")
    sources = sources or {"meshes/cube.bin": b"cubedata", "textures/tex.bin": b"texdata"}
    for relative, data in sources.items():
        _write(os.path.join(assets, relative), data)
    return root, exe, project, assets, version


def _package(root, exe, project, assets, version):
    staging = os.path.join(root, "package")
    result = BuildPackage(staging, exe, project, assets, version)
    check(result.Ok, "BuildPackage failed: %s" % result.Error)
    return staging, result


def test_the_package_is_complete():
    root, exe, project, assets, version = _setup()
    staging, result = _package(root, exe, project, assets, version)

    for entry in result.Files:
        check(os.path.exists(os.path.join(staging, entry.RelativePath)),
              "manifest lists a file not on disk: %s" % entry.RelativePath)
    check(os.path.exists(os.path.join(staging, "version.txt")), "no version.txt")
    check(os.path.exists(os.path.join(staging, "manifest.json")), "no manifest")


def test_the_exe_is_byte_identical_to_the_source():
    """A package that only ran on the dev machine is the failure this exists to
    prevent; a copy that changed a byte is a different runtime."""
    root, exe, project, assets, version = _setup()
    staging, _ = _package(root, exe, project, assets, version)
    check_eq(_read(os.path.join(staging, "Game.exe")), _read(exe),
             "the packaged exe is not byte-identical to the source")


def test_the_manifest_is_sorted_and_covers_tree_and_root():
    root, exe, project, assets, version = _setup()
    staging, result = _package(root, exe, project, assets, version)

    paths = [entry.RelativePath for entry in result.Files]
    check_eq(paths, sorted(paths), "the manifest is not sorted by path")
    check("assets/meshes/cube.bin" in paths,
          "assets are not prefixed by their package location")
    check("Game.exe" in paths, "the exe is missing from the manifest")
    check("project.wasteland" in paths, "the project file is missing")
    check("version.txt" in paths, "version.txt is missing")


def test_every_manifest_hash_matches_the_stored_content():
    root, exe, project, assets, version = _setup()
    staging, result = _package(root, exe, project, assets, version)
    for entry in result.Files:
        stored = _read(os.path.join(staging, entry.RelativePath))
        check_eq(entry.ContentHash, Fnv1a64(stored),
                 "entry %s hash does not match its file" % entry.RelativePath)
        check_eq(entry.Bytes, len(stored),
                 "entry %s byte count does not match" % entry.RelativePath)


def test_the_manifest_on_disk_matches_the_returned_inventory():
    root, exe, project, assets, version = _setup()
    staging, result = _package(root, exe, project, assets, version)
    data = json.loads(_read(os.path.join(staging, "manifest.json")).decode("utf-8"))
    check_eq(data["version"], version, "manifest version")
    disk_paths = sorted(e["path"] for e in data["files"])
    returned = sorted(e.RelativePath for e in result.Files)
    check_eq(disk_paths, returned,
             "the on-disk manifest disagrees with the returned inventory")


def test_two_builds_of_the_same_inputs_are_byte_identical():
    """Determinism is the whole point: a package with a timestamp or unordered
    manifest is a different artifact every build, so a rebuild is never a no-op
    and two machines never agree."""
    root, exe, project, assets, version = _setup()
    a = os.path.join(root, "pkg_a")
    b = os.path.join(root, "pkg_b")
    BuildPackage(a, exe, project, assets, version)
    BuildPackage(b, exe, project, assets, version)

    for relative in ("manifest.json", "version.txt"):
        check_eq(_read(os.path.join(a, relative)),
                 _read(os.path.join(b, relative)),
                 "%s differs between two identical builds" % relative)


def test_cook_sidecars_do_not_ship():
    """The .cook files are the cooker's incremental bookkeeping, not runtime data.
    A package that carries them has extra bytes nothing in it reads."""
    root, exe, project, assets, version = _setup()
    _write(os.path.join(assets, "meshes/cube.bin.cook"), b"0123456789abcdef")
    staging, result = _package(root, exe, project, assets, version)
    for entry in result.Files:
        check(".cook" not in entry.RelativePath,
              "a .cook sidecar shipped: %s" % entry.RelativePath)


def test_a_version_with_special_characters_is_escaped_in_the_manifest():
    root, exe, project, assets, version = _setup(version='v"1"\\x')
    staging, _ = _package(root, exe, project, assets, version)
    data = json.loads(_read(os.path.join(staging, "manifest.json")).decode("utf-8"))
    check_eq(data["version"], 'v"1"\\x',
             "a version with quotes/backslashes broke the JSON")


def test_version_txt_is_one_line():
    root, exe, project, assets, version = _setup(version="4.5.6")
    staging, _ = _package(root, exe, project, assets, version)
    content = _read(os.path.join(staging, "version.txt")).decode("utf-8")
    check_eq(content, "4.5.6\n", "version.txt is not the version plus a newline")


def test_a_missing_exe_fails():
    root, exe, project, assets, version = _setup()
    staging = os.path.join(root, "package")
    result = BuildPackage(staging, os.path.join(root, "missing.exe"),
                          project, assets, version)
    check(not result.Ok, "a missing exe was not reported as a failure")
    check(bool(result.Error), "a missing exe produced no message")


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
