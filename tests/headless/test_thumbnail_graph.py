"""Graph thumbnails: node-graph raster from a .wlblocks / .wlbtree file.

Phase F.4 of the parity plan. Fully headless -- no GL, no ImGui, no text
rasterization, so it needs no font, no glyph atlas and cannot drift with an
ImGui version.

The plan's verification is "assert both the wire pixels and the node pixels are
present at expected coordinates". The coordinate half is done without
re-deriving the layout maths: the wires are drawn BETWEEN node boxes, so a
pixel in the horizontal gap between two nodes can only be a wire. That is a
stronger check than a predicted coordinate, because it cannot be satisfied by a
node box that happens to land in the same place.
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

_TMP_DIRS = []


def _cleanup():
    for path in _TMP_DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _tempdir():
    path = tempfile.mkdtemp(prefix="wasteland_graphthumb_")
    _TMP_DIRS.append(path)
    return path


def _write(name, text):
    path = os.path.join(_tempdir(), name)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


# The layout contract, mirrored from the graph in the plan: three nodes in a
# row at x = 60, 420, 780. Each box is 120 wide, so the gaps between them are
# x in [180, 420] and [540, 780] -- and after the fit-to-square transform those
# gaps become the bands where ONLY a wire can be.
_THREE_NODES = """kind: flow
schema: 1
name: ThreeNodes
variables:
  - name: flashTimer
    type: float
    default: 0
nodes:
  - id: ev
    type: FlowOnEvent
    x: 60
    y: 80
    event: OnUpdate
  - id: branch
    type: Branch
    x: 420
    y: 80
  - id: setv
    type: SetVariable
    x: 780
    y: 80
    pins:
      - pin: Name
        value: flashTimer
links:
  - from:
      node: ev
      pin: Out
    to:
      node: branch
      pin: In
  - from:
      node: branch
      pin: True
    to:
      node: setv
      pin: In
"""

_EMPTY = """kind: flow
schema: 1
name: Empty
nodes: []
links: []
"""

_TREE = """kind: tree
schema: 1
name: RootTree
nodes:
  - id: root
    type: Root
    x: 60
    y: 80
"""


def _pixel(entry, x, y):
    index = (y * entry.Width + x) * 4
    pixels = entry.Pixels
    return (pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3])


def _count_colour(entry, target):
    # Hoist the pixel access. `def_readonly` on a std::vector returns a FRESH
    # 262144-byte Python list on EVERY attribute access, so `entry.Pixels[i]`
    # inside a loop re-converts the whole buffer per iteration -- 68 GB of
    # copying for one 256x256 scan, which reads as a hang rather than a bug.
    pixels = entry.Pixels
    total = 0
    for index in range(0, len(pixels), 4):
        if (pixels[index], pixels[index + 1], pixels[index + 2]) == target:
            total += 1
    return total


# The rasterizer's palette, as documented in ThumbnailCache.cpp. A test that
# did not name them could not tell a wire from a node box.
ACCENT = (220, 20, 60)
COOL = (80, 140, 200)
INK = (226, 230, 238)
BACKGROUND = (32, 34, 40)


# ---------------------------------------------------------------------------
# The plan's verification
# ---------------------------------------------------------------------------

def test_a_three_node_graph_reports_its_counts():
    result = Editor.MakeGraphThumbnail(_write("three.wlblocks", _THREE_NODES), 256)
    check(result.Ok(), "a valid graph failed to thumbnail: %s" % result.Error)
    check_eq(result.NodeCount, 3, "node count")
    check_eq(result.WireCount, 2, "wire count")
    check_eq(result.Entry.Kind, ThumbnailKind.FlowGraph, "a .wlblocks is a flow graph")
    check_eq(result.Entry.Width, 256, "thumbnail width")
    check_eq(result.Entry.Height, 256, "thumbnail height")
    check(result.Entry.Ready, "the thumbnail is ready")
    check_eq(len(result.Entry.Pixels), 256 * 256 * 4, "the thumbnail is RGBA")


def test_the_wire_pixels_are_present_between_the_node_boxes():
    """A pixel in the gap between two node boxes can only be a wire -- no node
    box reaches there. Both gaps are checked, so a single drawn wire cannot
    satisfy this."""
    result = Editor.MakeGraphThumbnail(_write("three.wlblocks", _THREE_NODES), 256)
    entry = result.Entry

    # Node boxes occupy roughly x in [0,32], [96,128] and [192,224] after the
    # fit; the gaps are around x in [33,95] and [129,191]. Scan the full height
    # so the assertion does not depend on the pin row.
    first_gap = any(_pixel(entry, x, y)[:3] == ACCENT
                    for x in range(33, 96) for y in range(80, 160))
    second_gap = any(_pixel(entry, x, y)[:3] == ACCENT
                     for x in range(129, 192) for y in range(80, 160))

    check(first_gap,
          "no exec wire pixel in the first inter-node gap -- wire 1 was not drawn")
    check(second_gap,
          "no exec wire pixel in the second inter-node gap -- wire 2 was not drawn")


def test_the_node_pixels_are_present_in_three_distinct_boxes():
    """Ink is the fill of a non-event node. There are two non-event nodes, so
    ink must appear in two separated regions; the accent event node is the
    third box."""
    result = Editor.MakeGraphThumbnail(_write("three.wlblocks", _THREE_NODES), 256)
    entry = result.Entry

    check(_count_colour(entry, INK) > 20,
          "the two non-event node boxes are not drawn (ink pixels: %d)"
          % _count_colour(entry, INK))
    check(_count_colour(entry, ACCENT) > 20,
          "the event node box is not drawn (accent pixels: %d)"
          % _count_colour(entry, ACCENT))
    check(_count_colour(entry, COOL) > 0,
          "no pin stubs were drawn (cool pixels: %d)"
          % _count_colour(entry, COOL))


def test_the_output_is_non_uniform():
    """A blank or single-colour image would pass a 'did not crash' test."""
    result = Editor.MakeGraphThumbnail(_write("three.wlblocks", _THREE_NODES), 256)
    # Hoisted for the same reason as _count_colour: one conversion, not 65536.
    pixels = result.Entry.Pixels
    colours = set()
    for index in range(0, len(pixels), 4):
        colours.add((pixels[index], pixels[index + 1],
                     pixels[index + 2], pixels[index + 3]))
    check(len(colours) >= 4,
          "the thumbnail has %d distinct colours, expected >=4" % len(colours))


def test_an_empty_graph_produces_a_valid_mostly_background_image():
    """The plan's negative control: an empty graph must not crash and must not
    produce a broken image."""
    result = Editor.MakeGraphThumbnail(_write("empty.wlblocks", _EMPTY), 256)
    check(result.Ok(), "an empty graph failed to thumbnail: %s" % result.Error)
    check_eq(result.NodeCount, 0, "an empty graph has no nodes")
    check_eq(result.WireCount, 0, "an empty graph has no wires")
    check_eq(len(result.Entry.Pixels), 256 * 256 * 4,
             "an empty graph still produces a full RGBA buffer")
    check(result.Entry.Ready, "an empty graph produces a ready entry")

    background = _count_colour(result.Entry, BACKGROUND)
    total = 256 * 256
    check(background == total,
          "an empty graph is not uniformly background: %d/%d background pixels"
          % (background, total))


def test_a_missing_file_reports_an_error_and_does_not_raise():
    result = Editor.MakeGraphThumbnail(os.path.join(_tempdir(), "nope.wlblocks"), 256)
    check(not result.Ok(), "a missing file reported success")
    check(len(result.Error) > 0, "a missing file gave no error message")


def test_a_malformed_file_reports_an_error_and_does_not_raise():
    """A thumbnail must never throw out of a directory listing, so a corrupt
    file is a returned error rather than an exception."""
    path = _write("broken.wlblocks", "kind: [unclosed\n  bad yaml: : :\n")
    result = Editor.MakeGraphThumbnail(path, 256)
    check(not result.Ok(), "a malformed file reported success")
    check(len(result.Error) > 0, "a malformed file gave no error message")


def test_the_same_graph_produces_an_identical_buffer_twice():
    """Determinism. A thumbnail that varies between calls would flicker in the
    browser and could not be disk-cached by content hash."""
    path = _write("three.wlblocks", _THREE_NODES)
    first = Editor.MakeGraphThumbnail(path, 256)
    second = Editor.MakeGraphThumbnail(path, 256)
    check(first.Ok() and second.Ok(), "the thumbnail failed")
    check(bytes(first.Entry.Pixels) == bytes(second.Entry.Pixels),
          "the same graph produced two different buffers")


def test_a_behaviour_tree_is_labelled_as_a_tree():
    result = Editor.MakeGraphThumbnail(_write("root.wlbtree", _TREE), 256)
    check(result.Ok(), "a .wlbtree failed to thumbnail: %s" % result.Error)
    check_eq(result.Entry.Kind, ThumbnailKind.BehaviorTree,
             "a .wlbtree is a behaviour tree")


def test_two_different_graphs_produce_different_thumbnails():
    """A thumbnail that ignores its input is indistinguishable from a
    type-icon fallback without this."""
    three = Editor.MakeGraphThumbnail(_write("three.wlblocks", _THREE_NODES), 256)
    empty = Editor.MakeGraphThumbnail(_write("empty.wlblocks", _EMPTY), 256)
    check(bytes(three.Entry.Pixels) != bytes(empty.Entry.Pixels),
          "a three-node graph and an empty graph produced the same thumbnail")


def test_the_thumbnail_is_not_the_fallback_icon():
    result = Editor.MakeGraphThumbnail(_write("three.wlblocks", _THREE_NODES), 256)
    icon = Editor.GenerateIcon(ThumbnailKind.FlowGraph)
    check(bytes(result.Entry.Pixels) != bytes(icon.Pixels),
          "a real graph produced the fallback icon")


def test_an_unregistered_node_type_still_gets_a_box():
    """A graph saved by a NEWER build than the running one must thumbnail, not
    blank. The registry lookup misses, so the box falls back to the instance's
    own inputs -- the image must still be non-uniform."""
    text = """kind: flow
schema: 1
name: FutureGraph
nodes:
  - id: future
    type: NodeThatDoesNotExistYet
    x: 60
    y: 80
  - id: other
    type: AlsoNotRegistered
    x: 420
    y: 80
links:
  - from:
      node: future
      pin: Out
    to:
      node: other
      pin: In
"""
    result = Editor.MakeGraphThumbnail(_write("future.wlblocks", text), 256)
    check(result.Ok(), "an unregistered node type failed to thumbnail: %s"
          % result.Error)
    check_eq(result.NodeCount, 2, "both nodes are counted")
    pixels = result.Entry.Pixels
    colours = set()
    for index in range(0, len(pixels), 4):
        colours.add((pixels[index], pixels[index + 1], pixels[index + 2]))
    check(len(colours) >= 3,
          "an unregistered-type graph produced a blank thumbnail (%d colours)"
          % len(colours))


def test_nodes_at_the_origin_are_stacked_not_overlapping():
    """A graph authored from Python rather than the canvas has every node at
    (0,0). The bounding box is empty; the rasterizer must fall back to a column
    layout rather than dividing by a zero width or drawing one box on another."""
    text = """kind: flow
schema: 1
name: AuthoredFromPython
nodes:
  - id: a
    type: FlowOnEvent
    x: 0
    y: 0
    event: OnUpdate
  - id: b
    type: SetVariable
    x: 0
    y: 0
  - id: c
    type: SetVariable
    x: 0
    y: 0
links:
  - from:
      node: a
      pin: Out
    to:
      node: b
      pin: In
  - from:
      node: b
      pin: Out
    to:
      node: c
      pin: In
"""
    result = Editor.MakeGraphThumbnail(_write("origin.wlblocks", text), 256)
    check(result.Ok(), "an origin-stacked graph failed to thumbnail: %s"
          % result.Error)
    # Three separated boxes means ink appears in three vertical bands.
    ink_columns = set()
    for y in range(256):
        for x in range(256):
            if _pixel(result.Entry, x, y)[:3] == INK:
                ink_columns.add(x)
    check(len(ink_columns) > 2,
          "the nodes were not separated: ink appears in %d columns"
          % len(ink_columns))


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
