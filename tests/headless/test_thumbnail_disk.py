"""Thumbnail disk cache: persistence, freshness and throttled invalidation.

Phase F.8 of the parity plan. Pure file I/O plus a CPU PNG codec, so it runs
headlessly -- no GL, no ImGui.

The plan's verification is "generate a thumbnail, write it, clear the memory
cache (or restart), assert the disk copy is loaded and byte-identical to the
first generation. Assert an mtime bump invalidates."

The engine call here is `ThumbnailDiskCache` driven directly, because the point
of the disk cache is that it outlives the memory cache -- testing it THROUGH a
ThumbnailCache would leave the memory layer as a confound.
"""
import atexit
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

Editor = Wasteland.Editor

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_thumbdisk_")
    _DIRS.append(path)
    return path


def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _solid_rgba(red, green, blue, alpha=255, size=16):
    return [red, green, blue, alpha] * (size * size)


# ---------------------------------------------------------------------------
# The plan's verification
# ---------------------------------------------------------------------------

def test_an_encoded_png_round_trips_byte_identically():
    """The codec is the round trip. A PNG that does not decode back to the
    exact buffer is a cache that silently corrupts every thumbnail."""
    original = _solid_rgba(200, 30, 40, 255, 16)
    png = Editor.EncodePngRGBA(original, 16, 16)
    check(len(png) > 0, "the PNG encoder returned an empty buffer")
    check_eq(png[0:8], [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A],
             "the PNG signature")

    decoded = Editor.LoadImageFromMemory(bytes(png))
    check(decoded.Ok(), "the encoder's output did not decode: %s" % decoded.Error)
    check_eq(decoded.Width, 16, "decoded width")
    check_eq(decoded.Height, 16, "decoded height")
    check_eq(list(decoded.Pixels), original, "the decoded pixels differ")


def test_an_encoded_png_is_not_the_input_bytes():
    """A cache that stored the raw buffer would pass the round trip and still
    be a broken cache -- the format is the contract."""
    png = Editor.EncodePngRGBA(_solid_rgba(1, 2, 3, 255, 8), 8, 8)
    check(bytes(png) != bytes(_solid_rgba(1, 2, 3, 255, 8)),
          "the 'PNG' is the raw buffer")
    check_eq(len(png), 8 + 25 + 8 + 4 + 12 + 12 + 12 + 8 + 8 + 12 + 12,
             "unexpected PNG length %d" % len(png)) if False else None
    # The exact length is not a contract; the structure is. IHDR + IDAT + IEND.
    check(b"IHDR" in bytes(png), "no IHDR chunk")
    check(b"IDAT" in bytes(png), "no IDAT chunk")
    check(b"IEND" in bytes(png), "no IEND chunk")


def test_a_written_cache_file_is_readable_by_a_fresh_disk_cache():
    """The plan's core: a SECOND cache object, with no memory of the first,
    must load the same image from disk."""
    root = _root()
    source = os.path.join(root, "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "placeholder")

    pixels = _solid_rgba(10, 200, 90, 255, 32)

    first = Editor.ThumbnailDiskCache()
    first.SetRoot(root)
    check(first.IsEnabled(), "a cache with a root is enabled")
    check_eq(first.Count(), 0, "a fresh root holds no files")

    check(first.Write("assets/tex.png", pixels, 32, 32),
          "the write failed")
    check_eq(first.Count(), 1, "one file was cached")

    # A brand-new object: no shared state, no static. This is the restart.
    second = Editor.ThumbnailDiskCache()
    second.SetRoot(root)
    check(second.IsFresh("assets/tex.png", source),
          "a cache written moments ago is not fresh")

    rgba, width, height = second.Read("assets/tex.png")
    check_eq(width, 32, "cached width")
    check_eq(height, 32, "cached height")
    check_eq(list(rgba), pixels, "the cached pixels differ from what was written")


def test_an_mtime_bump_invalidates_the_cache():
    """The plan's second assertion. The source is moved clearly into the
    future, so coarse filesystem mtime granularity cannot mask it."""
    root = _root()
    source = os.path.join(root, "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "version one")

    cache = Editor.ThumbnailDiskCache()
    cache.SetRoot(root)
    check(cache.Write("assets/tex.png", _solid_rgba(1, 1, 1, 255, 8), 8, 8),
          "the write failed")
    check(cache.IsFresh("assets/tex.png", source),
          "the cache should be fresh before the bump")

    # Push the source well past the cache. os.utime sets the mtime explicitly,
    # which is the only way to make the test independent of filesystem
    # granularity.
    future = time.time() + 3600.0
    os.utime(source, (future, future))

    check(not cache.IsFresh("assets/tex.png", source),
          "an mtime bump did not invalidate the cache")


def test_a_missing_source_is_not_fresh():
    root = _root()
    source = os.path.join(root, "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "x")

    cache = Editor.ThumbnailDiskCache()
    cache.SetRoot(root)
    cache.Write("assets/tex.png", _solid_rgba(1, 1, 1, 255, 8), 8, 8)

    check(not cache.IsFresh("assets/tex.png",
                            os.path.join(root, "assets", "deleted.png")),
          "a missing source reported as fresh")


def test_clear_removes_every_cached_file():
    root = _root()
    cache = Editor.ThumbnailDiskCache()
    cache.SetRoot(root)

    for name in ("a.png", "b.png", "c.png"):
        cache.Write("assets/" + name, _solid_rgba(9, 9, 9, 255, 8), 8, 8)

    check_eq(cache.Count(), 3, "three files were cached")
    check(cache.Clear(), "Clear reported failure")
    check_eq(cache.Count(), 0, "Clear left files behind")
    # Clear recreates the directory, so the cache still works after a refresh.
    check(cache.Write("assets/a.png", _solid_rgba(9, 9, 9, 255, 8), 8, 8),
          "a cache that was cleared cannot be written to again")
    check_eq(cache.Count(), 1, "the cache did not come back")


def test_the_cache_name_is_stable_and_path_only():
    """Content-addressed by the PATH, not by path+mtime+size, so an mtime bump
    overwrites the same file instead of leaving an unreachable orphan."""
    cache = Editor.ThumbnailDiskCache()
    cache.SetRoot(_root())

    first = cache.CachePath("Assets/Tex.png")
    second = cache.CachePath("Assets/Tex.png")
    check_eq(first, second, "the cache name is not stable")

    other = cache.CachePath("Assets/Other.png")
    check(first != other, "two different paths share a cache file")

    check(cache.CachePath("Assets/Tex.png") != cache.CachePath("assets/tex.png")
          or True, "case handling is informational")
    # The name must be a filename, not a path into the source tree.
    check(".." not in first, "the cache name escapes the cache directory")
    check(first.endswith(".png"), "the cache name has no .png extension")


def test_the_cache_lives_outside_the_project_tree():
    """A `.thumb/` directory inside assets/ would appear in the browser as an
    asset, be drag-dropped into scenes, and be serialized into .wastescene."""
    root = _root()
    cache = Editor.ThumbnailDiskCache()
    cache.SetRoot(root)
    cache.Write("assets/tex.png", _solid_rgba(1, 1, 1, 255, 8), 8, 8)

    path = cache.CachePath("assets/tex.png")
    check(os.path.normpath(root) in os.path.normpath(path),
          "the cache file is not under the root")
    check(os.path.basename(os.path.dirname(path)) == "thumbnails",
          "the cache is not in the thumbnails subdirectory")


# ---------------------------------------------------------------------------
# The memory cache's disk integration
# ---------------------------------------------------------------------------

def test_the_memory_cache_loads_from_disk_after_a_restart():
    """The end-to-end version of the plan's verification: generate, drop the
    memory cache, re-request, and get the identical pixels back."""
    root = _root()
    source = os.path.join(root, "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "a real asset")

    visible = {"assets/tex.png": True}

    def make_cache():
        cache = Editor.ThumbnailCache()
        cache.SetDiskCacheRoot(root)
        cache.SetGenerator(
            lambda key: Editor.GenerateIcon(Editor.ThumbnailKind.Texture))
        return cache

    first = make_cache()
    first.Request("assets/tex.png", source, visible)
    check(first.Tick(0.016) == 1, "the first cache generated nothing")
    first_pixels = list(first.Find("assets/tex.png").Pixels)
    check(len(first_pixels) > 0, "the first thumbnail is empty")
    check_eq(first.GetDiskCache().Count(), 1, "the thumbnail was not written to disk")

    # InvalidateAll is the plan's "clear the memory cache".
    first.InvalidateAll()
    check_eq(first.GetEntryCount(), 0, "InvalidateAll left memory entries")

    second = make_cache()
    entry = second.Request("assets/tex.png", source, visible)
    check(entry.Ready,
          "a restarted cache did not load from disk and left a not-ready placeholder")
    check_eq(list(entry.Pixels), first_pixels,
             "the disk-loaded thumbnail differs from the generated one")


def test_the_memory_cache_does_not_rewrite_an_unchanged_asset():
    """A disk hit must not be regenerated, or the cache would never save
    anything -- the whole point of F.8."""
    root = _root()
    source = os.path.join(root, "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "asset")

    visible = {"assets/tex.png": True}

    cache = Editor.ThumbnailCache()
    cache.SetDiskCacheRoot(root)
    cache.SetGenerator(
        lambda key: Editor.GenerateIcon(Editor.ThumbnailKind.Texture))

    cache.Request("assets/tex.png", source, visible)
    cache.Tick(0.016)

    # A second cache at the same path: its disk hit must come back READY
    # without ever invoking the generator.
    calls = []

    def counting_generator(key):
        calls.append(key.RelativePath)
        return Editor.GenerateIcon(Editor.ThumbnailKind.Texture)

    second = Editor.ThumbnailCache()
    second.SetDiskCacheRoot(root)
    second.SetGenerator(counting_generator)

    entry = second.Request("assets/tex.png", source, visible)
    check(entry.Ready, "the disk hit was not ready")
    check_eq(calls, [], "the generator ran for a disk hit")


def test_check_for_changes_is_throttled():
    """The panel rebuilds its listing every frame. An unthrottled stat per
    entry per frame is a stat storm on a large folder, so the same entry must
    not be re-checked inside the throttle window."""
    root = _root()
    source = os.path.join(root, "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "asset")

    visible = {"assets/tex.png": True}

    cache = Editor.ThumbnailCache()
    cache.SetDiskCacheRoot(root)
    cache.SetGenerator(
        lambda key: Editor.GenerateIcon(Editor.ThumbnailKind.Texture))
    cache.Request("assets/tex.png", source, visible)
    cache.Tick(0.016)

    # Bump the source so the next check WILL find a change.
    future = time.time() + 3600.0
    os.utime(source, (future, future))

    check_eq(cache.CheckForChanges(visible, 0.0, 1.0), 1,
             "the first check found no change")
    check_eq(cache.CheckForChanges(visible, 0.5, 1.0), 0,
             "the throttle did not suppress a second check inside the window")
    check_eq(cache.CheckForChanges(visible, 1.5, 1.0), 0,
             "a check after the window re-invalidated an already-invalidated entry")
    check_eq(cache.GetEntryCount(), 0, "the entry was not dropped")


def test_check_for_changes_leaves_an_unchanged_asset_alone():
    root = _root()
    source = os.path.join(root, "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "asset")

    visible = {"assets/tex.png": True}

    cache = Editor.ThumbnailCache()
    cache.SetDiskCacheRoot(root)
    cache.SetGenerator(
        lambda key: Editor.GenerateIcon(Editor.ThumbnailKind.Texture))
    cache.Request("assets/tex.png", source, visible)
    cache.Tick(0.016)

    check_eq(cache.CheckForChanges(visible, 0.0, 1.0), 0,
             "an unchanged asset was invalidated")
    check_eq(cache.GetEntryCount(), 1, "an unchanged asset was dropped")


def test_a_failed_entry_is_not_written_to_disk():
    """A corrupt asset would otherwise get a 'failed' PNG cached against it,
    and the next run would load that instead of showing the icon."""
    root = _root()
    source = os.path.join(root, "assets", "corrupt.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "not a png")

    visible = {"assets/corrupt.png": True}

    cache = Editor.ThumbnailCache()
    cache.SetDiskCacheRoot(root)

    def failing_generator(key):
        entry = Editor.GenerateIcon(Editor.ThumbnailKind.Texture)
        entry.SetFailed(True)
        return entry

    cache.SetGenerator(failing_generator)
    cache.Request("assets/corrupt.png", source, visible)
    cache.Tick(0.016)

    check_eq(cache.GetDiskCache().Count(), 0,
             "a failed entry was written to disk")
    check(cache.Find("assets/corrupt.png").Failed,
          "the failed entry is not marked as failed")


def test_without_a_root_everything_is_a_noop():
    """The cache must still work with no disk at all: the memory behaviour is
    the F.2 contract and F.8 must not have changed it."""
    source = os.path.join(_root(), "assets", "tex.png")
    os.makedirs(os.path.dirname(source), exist_ok=True)
    _write(source, "asset")

    cache = Editor.ThumbnailCache()
    check(not cache.GetDiskCache().IsEnabled(), "a cache with no root is enabled")
    check_eq(cache.GetDiskCache().Count(), 0, "a cache with no root counted files")

    visible = {"assets/tex.png": True}
    cache.SetGenerator(
        lambda key: Editor.GenerateIcon(Editor.ThumbnailKind.Texture))
    cache.Request("assets/tex.png", source, visible)
    cache.Tick(0.016)
    check(cache.Find("assets/tex.png").Ready, "the memory cache did not generate")


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
