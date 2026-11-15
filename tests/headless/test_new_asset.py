"""New-asset creation: every kind the Content Browser's New menu offers must be
a VALID EMPTY FILE, not a stub.

Phase F.11 of the parity plan. The plan's verification is "create one of each
from the menu; assert the file exists, parses, and produces a thumbnail". The
MENU is editor UI and cannot run here, so this covers the FILE side -- which is
the part that can be wrong in a way the user never sees.

The headers are produced by each serializer's own Save/Emit on a
default-constructed asset, so "parses" is the meaningful assertion: it proves
the serializer accepts what it wrote, and if a serializer cannot emit a usable
empty asset, that is a defect in the serializer that this test exposes.
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

Editor = Wasteland.Editor
ThumbnailKind = Editor.ThumbnailKind

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_newasset_")
    _DIRS.append(path)
    return path


def _create(root, name, asset_name="New Asset"):
    path = os.path.join(root, "assets", name)
    Editor.CreateEmptyAsset(path, asset_name)
    return path


def _read(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


# ---------------------------------------------------------------------------
# The plan's verification
# ---------------------------------------------------------------------------

def test_every_kind_is_created_and_classified():
    root = _root()
    expected = {
        "wlblocks": ThumbnailKind.FlowGraph,
        "wlbtree": ThumbnailKind.BehaviorTree,
        "wlavb": ThumbnailKind.AudioGraph,
        "wlanim": ThumbnailKind.Animation,
        "wlmesh": ThumbnailKind.Mesh,
        "wlmaterial": ThumbnailKind.Material,
    }

    for extension, kind in expected.items():
        path = _create(root, "New." + extension)
        check(os.path.isfile(path), ".%s was not created" % extension)
        check(os.path.getsize(path) > 0, ".%s is empty" % extension)
        check_eq(Editor.Classify(path), kind,
                 ".%s classifies to the wrong kind" % extension)


def test_a_flow_graph_parses_and_thumbnails():
    root = _root()
    path = _create(root, "New.wlblocks")

    graph = Wasteland.VisualBlocks.Graph()
    error = Wasteland.VisualBlocks.Load(path, graph)
    check(error is None,
          "a newly created .wlblocks does not parse: %s" % error)

    result = Editor.MakeGraphThumbnail(path, 256)
    check(result.Ok(), "a newly created .wlblocks has no thumbnail: %s"
          % result.Error)
    check_eq(result.NodeCount, 0, "a new flow graph has nodes")
    check_eq(result.Entry.Kind, ThumbnailKind.FlowGraph, "flow graph kind")


def test_a_behaviour_tree_parses_and_thumbnails():
    root = _root()
    path = _create(root, "New.wlbtree")

    graph = Wasteland.VisualBlocks.Graph()
    error = Wasteland.VisualBlocks.Load(path, graph)
    check(error is None,
          "a newly created .wlbtree does not parse: %s" % error)

    result = Editor.MakeGraphThumbnail(path, 256)
    check(result.Ok(), "a newly created .wlbtree has no thumbnail: %s"
          % result.Error)
    check_eq(result.Entry.Kind, ThumbnailKind.BehaviorTree, "behaviour tree kind")


def test_an_animation_parses():
    root = _root()
    path = _create(root, "New.wlanim")

    asset = Wasteland.Animation.AnimationAsset()
    error = Wasteland.Animation.LoadAnimation(path, asset)
    check(error is None,
          "a newly created .wlanim does not parse: %s" % error)


def test_a_mesh_parses():
    root = _root()
    path = _create(root, "New.wlmesh")

    mesh, materials, face_material, history, error = Wasteland.Mesh.LoadMesh(path)
    check(error == "",
          "a newly created .wlmesh does not parse: %s" % error)


def test_a_material_parses():
    root = _root()
    path = _create(root, "New.wlmaterial")

    material = Wasteland.MaterialComponent()
    ok, material, error = Wasteland.MaterialSerializer.Load(path, material)
    check(ok, "a newly created .wlmaterial does not parse: %s" % error)


def test_an_audio_graph_is_created_and_classifies():
    """No Python binding exists for the Avb graph serializer, so the parse
    check is not reachable headlessly. Existence and classification are still
    asserted, and the gap is stated rather than papered over."""
    root = _root()
    path = _create(root, "New.wlavb")
    check(os.path.isfile(path), ".wlavb was not created")
    check_eq(Editor.Classify(path), ThumbnailKind.AudioGraph,
             ".wlavb classifies to the wrong kind")


def test_the_asset_name_is_embedded_in_the_file():
    """The serializers embed the display name. A file that does not carry it
    would load with an empty name and show up as "Untitled" in the browser."""
    root = _root()
    path = _create(root, "New.wlblocks", "My Flow Graph")
    text = _read(path)
    check("My Flow Graph" in text,
          "the asset name is not stored in the file:\n%s" % text)


def test_an_empty_name_falls_back_to_the_file_stem():
    root = _root()
    path = _create(root, "MyTree.wlbtree", "")
    text = _read(path)
    check("MyTree" in text,
          "the file stem is not used as the name:\n%s" % text)


def test_an_unsupported_extension_is_rejected():
    root = _root()
    raised = None
    try:
        Editor.CreateEmptyAsset(os.path.join(root, "assets", "New.wlnope"), "x")
    except RuntimeError as error:
        raised = error
    check(raised is not None, "an unsupported extension was accepted")
    check("unsupported asset type" in str(raised),
          "the rejection does not name the problem: %s" % raised)


def test_the_file_round_trips_through_save_and_load():
    """A new asset must survive being opened and re-saved. A file that only
    parses once is a file whose reader and writer disagree."""
    root = _root()
    path = _create(root, "New.wlblocks", "Round Trip")

    graph = Wasteland.VisualBlocks.Graph()
    error = Wasteland.VisualBlocks.Load(path, graph)
    check(error is None, "the new asset does not parse: %s" % error)

    check(Wasteland.VisualBlocks.Save(path, graph) is None,
          "the loaded asset could not be re-saved")

    graph_again = Wasteland.VisualBlocks.Graph()
    error_again = Wasteland.VisualBlocks.Load(path, graph_again)
    check(error_again is None,
          "the re-saved asset does not parse: %s" % error_again)


def test_a_second_create_overwrites_rather_than_failing():
    """The New menu asks for a name in a folder that may already contain a file
    of that name. Refusing to overwrite would put an error dialog in front of
    the user for a normal action."""
    root = _root()
    path = _create(root, "New.wlblocks", "First")
    check(os.path.isfile(path), "the first create failed")

    Editor.CreateEmptyAsset(path, "Second")
    text = _read(path)
    check("Second" in text and "First" not in text,
          "the second create did not overwrite the file")


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
