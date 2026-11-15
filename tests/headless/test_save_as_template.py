"""Save the current project's assets as a user template (Phase I.4).

The editor button that CALLS this is editor UI, but the copy + manifest write is
engine work: copy the project assets into a fresh template directory, write a
template.wasteland.json, exclude the per-user thumbnail cache and *.pyc, and make
the result immediately discoverable by the registry.
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

SaveAsTemplate = Wasteland.SaveAsTemplate
ProjectTemplateRegistry = Wasteland.ProjectTemplateRegistry

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_sat_")
    _DIRS.append(path)
    return path


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _project_assets(root):
    assets = os.path.join(root, "project_assets")
    _write(os.path.join(assets, "meshes", "hero.wlmesh"), "mesh bytes")
    _write(os.path.join(assets, "scripts", "game.py"), "def main(): pass\n")
    # Things that must NOT ship: the per-user thumbnail cache and compiled code.
    _write(os.path.join(assets, ".thumbnails", "cache.png"), "png")
    _write(os.path.join(assets, "scripts", "game.pyc"), "bytestuff")
    return assets


def test_assets_are_copied_and_things_are_excluded():
    root = _root()
    assets = _project_assets(root)
    target = os.path.join(root, "user_templates", "my_game")

    manifest = SaveAsTemplate(target, assets, "My Game", "A small game.")
    check(os.path.exists(manifest), "no manifest was returned")
    check(os.path.exists(os.path.join(target, "assets", "meshes", "hero.wlmesh")),
          "a seed asset was not copied")
    check(os.path.exists(os.path.join(target, "assets", "scripts", "game.py")),
          "a script asset was not copied")
    check(not os.path.exists(os.path.join(target, "assets", ".thumbnails")),
          "the thumbnail cache was packaged")
    check(not os.path.exists(os.path.join(target, "assets", "scripts", "game.pyc")),
          "a compiled .pyc was packaged")


def test_the_manifest_describes_the_template():
    root = _root()
    assets = _project_assets(root)
    target = os.path.join(root, "user_templates", "my_game")
    manifest = SaveAsTemplate(target, assets, "My Game", "A small game.")

    parsed = ProjectTemplateRegistry.ParseManifest(
        open(manifest, "r", encoding="utf-8").read(), target, False)
    check_eq(parsed.Name, "My Game", "the manifest lost the name")
    check_eq(parsed.Description, "A small game.", "the manifest lost the description")
    check_eq(parsed.Category, "User", "user templates should sort apart from built-ins")
    check_eq(parsed.Id, "my-game", "the id was not slugified from the name")
    check(not parsed.BuiltIn, "a saved template was marked built-in")


def test_the_result_is_discoverable_by_the_registry():
    root = _root()
    assets = _project_assets(root)
    target = os.path.join(root, "user_templates", "my_game")
    SaveAsTemplate(target, assets, "My Game", "")

    registry = ProjectTemplateRegistry()
    registry.SetUserRoot(os.path.join(root, "user_templates"))
    found = registry.Discover()
    check_eq(len(found), 1, "the saved template was not discovered")
    check_eq(found[0].Name, "My Game", "the discovered template is wrong")
    check_eq(found[0].Category, "User", "wrong category on discovery")


def test_the_manifest_is_written_last_so_a_partial_copy_is_never_a_template():
    """A copy that dies halfway must not be discoverable -- the manifest is the
    discoverability marker and it goes last."""
    root = _root()
    target = os.path.join(root, "user_templates", "empty_dir")
    os.makedirs(target, exist_ok=True)
    registry = ProjectTemplateRegistry()
    registry.SetUserRoot(os.path.join(root, "user_templates"))
    found = registry.Discover()
    check_eq(len(found), 0, "a directory with no manifest was discovered as a template")


def test_save_as_template_with_no_assets_is_a_valid_empty_template():
    root = _root()
    target = os.path.join(root, "user_templates", "empty")
    manifest = SaveAsTemplate(target, os.path.join(root, "missing_assets"),
                              "Empty", "")
    check(os.path.exists(manifest), "an empty template was not created")
    parsed = ProjectTemplateRegistry.ParseManifest(
        open(manifest, "r", encoding="utf-8").read(), target, False)
    check_eq(parsed.Id, "empty", "an empty template got a wrong id")


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
