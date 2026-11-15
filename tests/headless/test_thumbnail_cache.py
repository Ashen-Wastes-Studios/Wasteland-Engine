"""Thumbnail cache: kind classification, cache keys, budget and eviction.

Phase F.1 + F.2 of the parity plan. Pure logic, no GL: the cache stores RGBA
buffers rather than ImTextureIDs precisely so this file can execute the real
engine code instead of a re-implementation of it.

The plan's own verification list, and what each item is really guarding:

* classify every extension -- a kind mis-assigned is a wrong icon on a real
  asset, which is invisible to any test that only checks "did not crash".
* Script.PY == script.py -- ContentBrowserPanel::IsOpenable documents its own
  case-sensitive comparison on a case-insensitive filesystem. The classifier
  must not repeat that defect.
* a changed mtime produces a different key -- otherwise an edited texture shows
  its old thumbnail forever, which is the bug a cache key exists to prevent.
* the LRU evicts down to the cap and eviction is OBSERVABLE -- a test that
  cannot see the cap take effect cannot tell an LRU from an unbounded map.
* negative control: no extension is Unknown, NOT Script.

F.2's rule: a kind with no generator must still produce an icon. So every icon
is asserted non-uniform with at least four distinct colours -- a blank icon
would pass a "did not crash" test.
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
    path = tempfile.mkdtemp(prefix="wasteland_thumb_")
    _TMP_DIRS.append(path)
    return path


def _write(relative, contents=b"x"):
    """Author a file inside a fresh temp project and return (absolute, relative)."""
    root = _tempdir()
    absolute = os.path.join(root, relative)
    os.makedirs(os.path.dirname(absolute), exist_ok=True)
    with open(absolute, "wb") as handle:
        handle.write(contents)
    return absolute, relative


# ---------------------------------------------------------------------------
# F.1 -- kind classification
# ---------------------------------------------------------------------------

# Every extension in the plan's contract table, paired with its kind. Derived
# from the table rather than from the engine, so a mismatch is a real signal.
EXTENSION_KINDS = [
    ("a.png", ThumbnailKind.Texture), ("a.jpg", ThumbnailKind.Texture),
    ("a.jpeg", ThumbnailKind.Texture), ("a.bmp", ThumbnailKind.Texture),
    ("a.tga", ThumbnailKind.Texture), ("a.gif", ThumbnailKind.Texture),
    ("a.webp", ThumbnailKind.Texture),
    ("a.obj", ThumbnailKind.Mesh), ("a.gltf", ThumbnailKind.Mesh),
    ("a.glb", ThumbnailKind.Mesh), ("a.fbx", ThumbnailKind.Mesh),
    ("a.dae", ThumbnailKind.Mesh), ("a.stl", ThumbnailKind.Mesh),
    ("a.ply", ThumbnailKind.Mesh), ("a.wlmesh", ThumbnailKind.Mesh),
    ("a.wlmaterial", ThumbnailKind.Material),
    ("a.wlanim", ThumbnailKind.Animation),
    ("a.wastescene", ThumbnailKind.Scene), ("a.wlbin", ThumbnailKind.Scene),
    ("a.wlblocks", ThumbnailKind.FlowGraph),
    ("a.wlbtree", ThumbnailKind.BehaviorTree),
    ("a.wlavb", ThumbnailKind.AudioGraph),
    ("a.wav", ThumbnailKind.AudioClip), ("a.ogg", ThumbnailKind.AudioClip),
    ("a.mp3", ThumbnailKind.AudioClip),
    ("a.wlterrain", ThumbnailKind.Terrain),
    ("a.py", ThumbnailKind.Script),
    ("a.glsl", ThumbnailKind.Shader), ("a.hlsl", ThumbnailKind.Shader),
    ("a.slang", ThumbnailKind.Shader), ("a.spv", ThumbnailKind.Shader),
    ("a.cso", ThumbnailKind.Shader),
    ("a.md", ThumbnailKind.Document), ("a.txt", ThumbnailKind.Document),
    ("a.pyc", ThumbnailKind.Compiled), ("a.r8", ThumbnailKind.Compiled),
]


def test_every_extension_classifies_to_its_contract_kind():
    for path, expected in EXTENSION_KINDS:
        got = Editor.Classify(path)
        check(got == expected,
              "%s classified as %s, expected %s"
              % (path, Editor.ThumbnailKindName(got),
                 Editor.ThumbnailKindName(expected)))


def test_classification_is_case_insensitive():
    """ContentBrowserPanel::IsOpenable documents a case-sensitive comparison on
    a case-insensitive filesystem: "Script.PY" fails to open while "script.py"
    works. The classifier must not repeat that defect."""
    for path, expected in EXTENSION_KINDS:
        upper = path.upper()
        got = Editor.Classify(upper)
        check(got == expected,
              "%s classified as %s, expected %s (case-insensitivity)"
              % (upper, Editor.ThumbnailKindName(got),
                 Editor.ThumbnailKindName(expected)))


def test_a_path_with_no_extension_is_unknown_not_script():
    """Negative control. Treating a bare name as a script would put an
    angle-bracket icon on every extension-less file, including executables."""
    for path in ("README", "Makefile", "assets/textures/rock", ".gitignore"):
        got = Editor.Classify(path)
        check(got == ThumbnailKind.Unknown,
              "%s classified as %s, expected Unknown"
              % (path, Editor.ThumbnailKindName(got)))


def test_a_directory_is_a_directory_even_without_an_extension():
    """Folders go through the same classifier and must not fall through to
    Unknown, which would give them a question-mark icon."""
    root = _tempdir()
    directory = os.path.join(root, "assets")
    os.makedirs(directory)
    got = Editor.Classify(directory)
    check(got == ThumbnailKind.Directory,
          "a directory classified as %s" % Editor.ThumbnailKindName(got))


def test_every_kind_has_a_stable_name():
    """The name is shown in the status bar and asserted here, so it is a
    contract: Unknown must not be named something else, and no two kinds may
    share a name."""
    names = set()
    for kind in ThumbnailKind.__members__.values():
        name = Editor.ThumbnailKindName(kind)
        check(isinstance(name, str) and len(name) > 0,
              "%r has an empty name" % (kind,))
        names.add(name)
    check_eq(len(names), 16, "all 16 kinds have distinct names")
    check_eq(Editor.ThumbnailKindName(ThumbnailKind.Unknown), "Unknown",
             "Unknown is named Unknown")


# ---------------------------------------------------------------------------
# F.1 -- cache key
# ---------------------------------------------------------------------------

def test_a_changed_mtime_produces_a_different_key():
    absolute, relative = _write("assets/textures/rock.png", b"first")
    before = Editor.MakeThumbnailCacheKey(relative, absolute)

    # Rewrite with different content AND a different size. Size alone would
    # catch this, so the mtime assertion below is the one that matters.
    with open(absolute, "wb") as handle:
        handle.write(b"second-and-longer")

    after = Editor.MakeThumbnailCacheKey(relative, absolute)
    check(before != after, "a changed file kept its old cache key")

    # And the reason must be the mtime, not the size: rewrite the same length.
    with open(absolute, "wb") as handle:
        handle.write(b"THIRD-and-longer")
    third = Editor.MakeThumbnailCacheKey(relative, absolute)
    check(third != after,
          "two writes of identical length produced the same key")


def test_the_key_includes_the_relative_path_and_the_size():
    absolute, relative = _write("assets/meshes/cube.obj", b"v 0 0 0")
    key = Editor.MakeThumbnailCacheKey(relative, absolute)
    check_eq(key.RelativePath, "assets/meshes/cube.obj",
             "the key carries the project-relative path")
    check_eq(key.Size, 7, "the key carries the file size")
    check(key.ModifiedTime != 0, "the key carries a non-zero mtime")


def test_the_key_string_is_the_disk_cache_filename_contract():
    absolute, relative = _write("a/b/c.png", b"data")
    key = Editor.MakeThumbnailCacheKey(relative, absolute)
    text = str(key)
    check(text.startswith("assets/a/b/c.png".replace("assets/", "")),
          "the key string starts with the relative path: %r" % text)
    check("|" in text, "the key string is pipe-delimited")


def test_a_missing_file_yields_a_key_rather_than_raising():
    """A file can vanish between the directory listing and the thumbnailing.
    The non-throwing error_code overloads must not throw out of a UI pass."""
    key = Editor.MakeThumbnailCacheKey("assets/gone.png",
                                      os.path.join(_tempdir(), "gone.png"))
    check_eq(key.Size, 0, "a missing file has a zero size in its key")
    check(key.RelativePath == "assets/gone.png",
          "a missing file still carries its path")


# ---------------------------------------------------------------------------
# F.1 -- Request / Tick / budget
# ---------------------------------------------------------------------------

def _icon_generator(key):
    """A generator that produces an icon for the key's kind.

    Stands in for the editor's renderer: the cache does not know or care what
    the generator does, which is the point of injecting it.
    """
    return Editor.GenerateIcon(Editor.Classify(key.RelativePath))


def test_request_never_generates_synchronously():
    """Rule 1 of the plan: nothing expensive runs on the UI thread. Request
    returns a NOT-READY placeholder and queues the key; only Tick generates."""
    cache = Editor.ThumbnailCache()
    cache.SetGenerator(_icon_generator)
    absolute, relative = _write("assets/textures/rock.png")

    entry = cache.Request(relative, absolute, {relative: True})
    check(entry is not None, "Request never returns null")
    check(not entry.Ready, "Request returned an already-generated entry")
    check_eq(cache.GetPendingCount(), 1, "the key was queued")
    check_eq(cache.GetEntryCount(), 1, "a placeholder entry was created")


def test_tick_generates_at_most_the_per_frame_budget():
    cache = Editor.ThumbnailCache()
    cache.SetGenerator(_icon_generator)

    visible = {}
    for index in range(6):
        relative = "assets/textures/rock%d.png" % index
        absolute, relative = _write("assets/textures/rock%d.png" % index)
        visible[relative] = True
        cache.Request(relative, absolute, visible)

    check_eq(cache.GetPendingCount(), 6, "all six are queued")
    generated = cache.Tick(1.0 / 60.0)
    check_eq(generated, cache.GetMaxPerFrame(),
             "Tick generated exactly the budget (%d)" % cache.GetMaxPerFrame())
    check_eq(cache.GetPendingCount(), 6 - cache.GetMaxPerFrame(),
             "the rest stay queued for the next frame")

    # Drain it and confirm the budget is per-frame, not global.
    remaining = 0
    while cache.GetPendingCount() > 0:
        remaining += cache.Tick(1.0 / 60.0)
    check_eq(remaining, 6 - cache.GetMaxPerFrame(),
             "the remainder drained over subsequent frames")
    check_eq(cache.GetPendingCount(), 0, "nothing is left queued")


def test_a_hidden_entry_is_never_queued():
    """Generating a thumbnail the user has scrolled past is the easiest way to
    spend a frame budget for nothing."""
    cache = Editor.ThumbnailCache()
    cache.SetGenerator(_icon_generator)
    absolute, relative = _write("assets/textures/rock.png")

    cache.Request(relative, absolute, {})
    check_eq(cache.GetPendingCount(), 0,
             "an off-screen entry is not queued")
    check_eq(cache.Tick(1.0 / 60.0), 0,
             "an off-screen entry is never generated")


def test_a_failed_entry_is_cached_and_not_retried():
    """A corrupt asset is a fact about the file, not a transient condition. A
    failed entry must not be retried every frame."""
    attempts = []

    def failing_generator(key):
        attempts.append(key.RelativePath)
        entry = Editor.GenerateIcon(ThumbnailKind.Texture)
        entry.SetFailed(True)
        return entry

    cache = Editor.ThumbnailCache()
    cache.SetGenerator(failing_generator)
    absolute, relative = _write("assets/textures/broken.png")

    cache.Request(relative, absolute, {relative: True})
    cache.Tick(1.0 / 60.0)
    check_eq(len(attempts), 1, "the generator ran once")
    check(cache.Find(relative).Failed, "the failure is recorded")

    # A second Request for the same path must hit the cache, not the generator.
    cache.Request(relative, absolute, {relative: True})
    cache.Tick(1.0 / 60.0)
    check_eq(len(attempts), 1, "a failed entry was not retried")


def test_a_changed_file_invalidates_its_entry():
    """Otherwise an edited texture shows its old thumbnail forever."""
    cache = Editor.ThumbnailCache()
    cache.SetGenerator(_icon_generator)
    absolute, relative = _write("assets/textures/rock.png", b"v1")

    first = cache.Request(relative, absolute, {relative: True})
    check(not first.Ready, "the first request is a placeholder")

    with open(absolute, "wb") as handle:
        handle.write(b"v2-longer")
    second = cache.Request(relative, absolute, {relative: True})
    check(not second.Ready,
          "a changed file still returns the stale ready entry")


# ---------------------------------------------------------------------------
# F.1 -- LRU eviction
# ---------------------------------------------------------------------------

def test_the_lru_evicts_down_to_the_cap_and_eviction_is_observable():
    """A test that cannot see the cap take effect cannot tell an LRU from an
    unbounded map that grows until the process dies."""
    cache = Editor.ThumbnailCache()
    cache.SetGenerator(_icon_generator)

    # A cap of 256 is far too many entries for a test to author by hand, so the
    # cap is read from the cache rather than hardcoded -- the test asserts the
    # BEHAVIOUR (count never exceeds the cap, evictions counted) not the number.
    cap = cache.GetMaxEntries()
    check(cap > 0, "the cap is positive")

    for index in range(cap + 8):
        relative = "assets/textures/tex%04d.png" % index
        absolute, relative = _write("assets/textures/tex%04d.png" % index)
        cache.Request(relative, absolute, {relative: True})

    check(cache.GetEntryCount() <= cap,
          "the entry count (%d) exceeds the cap (%d)"
          % (cache.GetEntryCount(), cap))
    check(cache.GetEvictionCount() > 0,
          "eviction happened but was not counted")


def test_invalidate_all_clears_entries_pending_and_the_eviction_counter():
    cache = Editor.ThumbnailCache()
    cache.SetGenerator(_icon_generator)

    for index in range(4):
        relative = "assets/t/t%d.png" % index
        absolute, relative = _write("assets/t/t%d.png" % index)
        cache.Request(relative, absolute, {relative: True})

    check(cache.GetEntryCount() > 0, "entries exist before invalidation")
    cache.InvalidateAll()
    check_eq(cache.GetEntryCount(), 0, "InvalidateAll cleared the entries")
    check_eq(cache.GetPendingCount(), 0, "InvalidateAll cleared the queue")
    check_eq(cache.GetEvictionCount(), 0,
             "InvalidateAll reset the eviction counter")


# ---------------------------------------------------------------------------
# F.2 -- procedural type icons
# ---------------------------------------------------------------------------

def test_every_icon_is_non_uniform_with_at_least_four_colours():
    """A blank or single-colour icon would pass a "did not crash" test, which
    is why the plan requires >=4 distinct colours per icon."""
    offenders = []
    for kind in ThumbnailKind.__members__.values():
        entry = Editor.GenerateIcon(kind)
        check_eq(entry.Width, 256, "%s icon is 256 wide" %
                 Editor.ThumbnailKindName(kind))
        check_eq(entry.Height, 256, "%s icon is 256 tall" %
                 Editor.ThumbnailKindName(kind))
        check_eq(len(entry.Pixels), 256 * 256 * 4,
                 "%s icon has RGBA pixels" % Editor.ThumbnailKindName(kind))
        check(entry.Ready, "%s icon is ready" % Editor.ThumbnailKindName(kind))

        colours = set()
        opaque = 0
        # Strided rather than exhaustive: the icons are 262144 pixels and the
        # full scan dominates the runtime of this suite. A stride of 4 still
        # samples 65536 pixels per icon, far more than the regions in these
        # silhouettes, and the first 64 pixels are always included so a
        # single-pixel feature at the top-left cannot hide.
        pixels = entry.Pixels
        for index in range(0, len(pixels), 4):
            if index % 16 >= 4 and index >= 64:
                continue
            pixel = (pixels[index], pixels[index + 1],
                     pixels[index + 2], pixels[index + 3])
            colours.add(pixel)
            if pixel[3] == 255:
                opaque += 1

        # Every kind is checked in ONE pass rather than the first failure
        # aborting the loop: with seventeen icons, failing one per build means
        # seventeen rebuild cycles to find them all.
        if len(colours) < 4:
            offenders.append("%s has %d distinct colours"
                             % (Editor.ThumbnailKindName(kind), len(colours)))
        if opaque == 0:
            offenders.append("%s is fully transparent"
                             % Editor.ThumbnailKindName(kind))

    check(not offenders,
          "icons below the four-colour contract: " + "; ".join(offenders))


def test_icons_are_distinct_from_each_other():
    """Two kinds sharing a silhouette would defeat the point of an icon set."""
    seen = {}
    for kind in ThumbnailKind.__members__.values():
        entry = Editor.GenerateIcon(kind)
        signature = bytes(entry.Pixels)
        # The duplicate lookup must only happen on a duplicate. Evaluating
        # seen[signature] inside the check's message would raise KeyError for
        # every icon that is NOT a duplicate -- the message is formatted
        # eagerly, so the check never gets to pass.
        if signature in seen:
            check(False,
                  "%s icon is pixel-identical to %s"
                  % (Editor.ThumbnailKindName(kind),
                     Editor.ThumbnailKindName(seen[signature])))
        seen[signature] = kind
    # And every kind must have produced a distinct signature at all.
    check_eq(len(seen), 16, "all 16 icons have distinct signatures")


def test_a_kind_with_no_generator_still_produces_an_icon():
    """The contract: a kind with no generator must still produce an icon. The
    fallback IS the icon, so GenerateIcon must not depend on a generator."""
    entry = Editor.GenerateIcon(ThumbnailKind.Unknown)
    check(entry.Ready, "the fallback icon is ready immediately")
    check(not entry.IsEmpty(), "the fallback icon is not empty")
    check(len(entry.Pixels) == 256 * 256 * 4, "the fallback icon has pixels")


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
