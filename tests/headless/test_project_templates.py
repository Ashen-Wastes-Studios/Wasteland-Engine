"""Project template registry: discovery, manifest parsing, version gating.

Phase I.1 of the parity plan. Pure file I/O plus JSON, so it runs headlessly --
the template GRID is editor UI and stays there.

The plan's verification for I.1 is the registry itself; I.2's seed CONTENT and
I.3-I.5's dialog need the editor, which cannot start on this machine.
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

ProjectTemplate = Wasteland.ProjectTemplate
Registry = Wasteland.ProjectTemplateRegistry

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_templates_")
    _DIRS.append(path)
    return path


def _manifest(root, template_id, **overrides):
    """Create a template directory with a manifest and return its path."""
    directory = os.path.join(root, template_id)
    os.makedirs(directory, exist_ok=True)
    data = {
        "id": template_id,
        "name": template_id.replace("_", " ").title(),
        "description": "A template.",
        "category": "Games",
        "defaultLayout": "Scene",
        "engineVersion": "0.1.0",
        "thumbnail": "thumb.png",
    }
    data.update(overrides)
    with open(os.path.join(directory, "template.wasteland.json"),
              "w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle)
    return directory


# ---------------------------------------------------------------------------
# Manifest parsing
# ---------------------------------------------------------------------------

def test_a_minimal_manifest_parses():
    parsed = Registry.ParseManifest(
        json.dumps({"id": "third_person", "name": "Third Person"}),
        "/seed/third_person", True)
    check_eq(parsed.Id, "third_person", "id")
    check_eq(parsed.Name, "Third Person", "name")
    check_eq(parsed.SeedPath, "/seed/third_person",
             "the seed path comes from the directory, not the manifest")
    check(parsed.BuiltIn, "BuiltIn is recorded")
    check(parsed.Compatible, "a template with no version gate is compatible")


def test_absent_optional_fields_take_their_defaults():
    """A template authored before a field existed must still load -- the same
    rule MaterialSerializer::Load applies."""
    parsed = Registry.ParseManifest(
        json.dumps({"id": "x", "name": "X"}), "/seed/x", False)
    check_eq(parsed.Category, "Games", "default category")
    check_eq(parsed.DefaultLayout, "Scene", "default layout")
    check_eq(parsed.EngineVersion, "0.1.0", "default engine version")
    check_eq(parsed.Description, "No description.", "default description")


def test_the_thumbnail_path_is_resolved_against_the_seed_directory():
    parsed = Registry.ParseManifest(
        json.dumps({"id": "x", "name": "X", "thumbnail": "thumb.png"}),
        "/seed/x", False)
    check_eq(parsed.ThumbnailPath.replace("\\", "/"), "/seed/x/thumb.png",
             "the thumbnail is not resolved against the seed directory")


def test_a_manifest_without_an_id_is_rejected():
    raised = None
    try:
        Registry.ParseManifest(json.dumps({"name": "X"}), "/seed/x", False)
    except RuntimeError as error:
        raised = error
    check(raised is not None, "a manifest without an id was accepted")
    check("id" in str(raised), "the rejection does not name the problem: %s" % raised)


def test_a_manifest_without_a_name_is_rejected():
    raised = None
    try:
        Registry.ParseManifest(json.dumps({"id": "x"}), "/seed/x", False)
    except RuntimeError as error:
        raised = error
    check(raised is not None, "a manifest without a name was accepted")


def test_malformed_json_is_rejected():
    raised = None
    try:
        Registry.ParseManifest("{not json", "/seed/x", False)
    except RuntimeError as error:
        raised = error
    check(raised is not None, "malformed JSON was accepted")


def test_a_json_array_is_rejected():
    raised = None
    try:
        Registry.ParseManifest("[1, 2, 3]", "/seed/x", False)
    except RuntimeError as error:
        raised = error
    check(raised is not None, "a JSON array was accepted as a manifest")


# ---------------------------------------------------------------------------
# Version comparison and gating
# ---------------------------------------------------------------------------

def test_version_comparison_is_numeric_not_lexicographic():
    """The bug this guards: lexicographic order puts "0.10.0" before "0.9.0",
    so a template for a newer engine would load as compatible."""
    check(Registry.CompareVersions("0.10.0", "0.9.0") > 0,
          "0.10.0 sorts before 0.9.0")
    check(Registry.CompareVersions("0.9.0", "0.10.0") < 0,
          "0.9.0 sorts after 0.10.0")
    check_eq(Registry.CompareVersions("0.1.0", "0.1.0"), 0, "equal versions")
    check_eq(Registry.CompareVersions("0.1", "0.1.0"), 0,
             "a missing patch component is the same version")


def test_a_newer_template_is_greyed_out_not_hidden():
    """The plan is explicit: a template whose EngineVersion is newer than the
    running editor is shown greyed out WITH the reason, not hidden. Hiding it
    would make a missing template indistinguishable from a bug."""
    root = _root()
    _manifest(root, "future_thing", engineVersion="99.0.0")

    registry = Registry()
    registry.AddBuiltInRoot(root)
    registry.SetEngineVersion("0.1.0")

    found = registry.Discover()
    check_eq(len(found), 1, "the future template was hidden instead of gated")
    check(not found[0].Compatible, "a template for a newer engine is compatible")
    check("99.0.0" in found[0].IncompatibleReason,
          "the incompatibility reason does not name the version: %s"
          % found[0].IncompatibleReason)
    check("0.1.0" in found[0].IncompatibleReason,
          "the incompatibility reason does not name the running version: %s"
          % found[0].IncompatibleReason)


def test_an_older_template_is_compatible():
    root = _root()
    _manifest(root, "old_thing", engineVersion="0.0.1")

    registry = Registry()
    registry.AddBuiltInRoot(root)
    registry.SetEngineVersion("0.1.0")

    found = registry.Discover()
    check_eq(len(found), 1, "one template was discovered")
    check(found[0].Compatible, "an older template is not compatible")
    check_eq(found[0].IncompatibleReason, "",
             "a compatible template carries an incompatibility reason")


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

def test_discovery_finds_templates_in_both_roots():
    root = _root()
    _manifest(root, "one")
    _manifest(root, "two")

    registry = Registry()
    registry.AddBuiltInRoot(root)
    registry.SetUserRoot(os.path.join(root, "user"))
    os.makedirs(os.path.join(root, "user", "three"), exist_ok=True)
    with open(os.path.join(root, "user", "three",
                           "template.wasteland.json"), "w",
              encoding="utf-8", newline="\n") as handle:
        json.dump({"id": "three", "name": "Three"}, handle)

    found = registry.Discover()
    check_eq(len(found), 3, "three templates were discovered")
    ids = sorted(template.Id for template in found)
    check_eq(ids, ["one", "three", "two"], "template ids")


def test_a_directory_without_a_manifest_is_not_a_template():
    """Treating every folder as a template would put the user's own asset
    directories in the dialog."""
    root = _root()
    _manifest(root, "real_template")
    os.makedirs(os.path.join(root, "just_a_folder"), exist_ok=True)

    registry = Registry()
    registry.AddBuiltInRoot(root)

    found = registry.Discover()
    check_eq(len(found), 1, "a folder without a manifest was treated as a template")
    check_eq(found[0].Id, "real_template", "the wrong template was found")


def test_a_broken_manifest_is_skipped_not_fatal():
    """One bad template must not take the whole dialog down. The user can see
    the others and delete the bad directory."""
    root = _root()
    _manifest(root, "good")
    os.makedirs(os.path.join(root, "bad"), exist_ok=True)
    with open(os.path.join(root, "bad", "template.wasteland.json"), "w",
              encoding="utf-8", newline="\n") as handle:
        handle.write("{not json")

    registry = Registry()
    registry.AddBuiltInRoot(root)

    found = registry.Discover()
    check_eq(len(found), 1, "a broken manifest suppressed the good templates")
    check_eq(found[0].Id, "good", "the wrong template survived")


def test_discovery_is_sorted_by_category_then_name():
    """A directory listing order is filesystem-dependent and would reshuffle
    the grid on every launch."""
    root = _root()
    _manifest(root, "bbb", category="Games", name="Bbb")
    _manifest(root, "aaa", category="Games", name="Aaa")
    _manifest(root, "ccc", category="Empty", name="Ccc")

    registry = Registry()
    registry.AddBuiltInRoot(root)

    found = registry.Discover()
    order = [(template.Category, template.Name) for template in found]
    check_eq(order, [("Empty", "Ccc"), ("Games", "Aaa"), ("Games", "Bbb")],
             "the discovered order is not sorted by category then name")


def test_a_missing_root_is_not_an_error():
    registry = Registry()
    registry.AddBuiltInRoot(os.path.join(_root(), "does_not_exist"))
    check_eq(registry.Discover(), [], "a missing root produced templates")


def test_built_in_and_user_templates_are_labelled():
    root = _root()
    _manifest(root, "built_in_one")

    user_root = os.path.join(root, "user")
    os.makedirs(os.path.join(user_root, "user_one"), exist_ok=True)
    with open(os.path.join(user_root, "user_one",
                           "template.wasteland.json"), "w",
              encoding="utf-8", newline="\n") as handle:
        json.dump({"id": "user_one", "name": "User One"}, handle)

    registry = Registry()
    registry.AddBuiltInRoot(root)
    registry.SetUserRoot(user_root)

    by_id = {template.Id: template for template in registry.Discover()}
    check(by_id["built_in_one"].BuiltIn, "a built-in template is not labelled")
    check(not by_id["user_one"].BuiltIn, "a user template is labelled built-in")


# ---------------------------------------------------------------------------
# The declared built-in set
# ---------------------------------------------------------------------------

def test_the_declared_built_in_set_matches_the_plan():
    """I.2's table. Asserted so a template cannot be silently dropped."""
    built_ins = Registry.DeclaredBuiltIns()
    ids = sorted(template.Id for template in built_ins)
    check_eq(ids, sorted([
        "empty", "third_person", "first_person", "top_down", "2d_platformer",
        "animation_showcase", "modeling_starter", "cinematic"]),
        "the declared built-in set differs from the plan")


def test_every_declared_built_in_has_a_default_layout():
    for template in Registry.DeclaredBuiltIns():
        check(bool(template.DefaultLayout),
              "%s has no default layout" % template.Id)
        check(bool(template.Name), "%s has no name" % template.Id)
        check(bool(template.Category), "%s has no category" % template.Id)


def test_the_declared_set_is_independent_of_the_filesystem():
    """The dialog shows the declared set before any seed assets ship, so it must
    not depend on a directory existing."""
    first = Registry.DeclaredBuiltIns()
    second = Registry.DeclaredBuiltIns()
    check_eq(len(first), len(second), "the declared set changed between calls")
    check_eq([t.Id for t in first], [t.Id for t in second],
             "the declared set's order changed between calls")


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
