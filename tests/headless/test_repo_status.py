"""Repo-status data core (Phase J.5).

The status BAR is editor UI; the data it needs -- branch, dirty count, untracked
count -- is engine-queryable and CI-verifiable. This queries git for it.
"""
import atexit
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq, check_ge

import wlpath

wlpath.setup()

import Wasteland

QueryRepoStatus = Wasteland.QueryRepoStatus

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _root():
    path = tempfile.mkdtemp(prefix="wasteland_repo_")
    _DIRS.append(path)
    return path


def _run(root, *args):
    return subprocess.run(["git", "-C", root, *args],
                          capture_output=True, text=True)


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _make_repo(root, branch="main"):
    _run(root, "init", "-b", branch)
    _run(root, "config", "user.email", "t@example.com")
    _run(root, "config", "user.name", "Tester")
    _write(os.path.join(root, "tracked.txt"), "one\n")
    _run(root, "add", "tracked.txt")
    done = _run(root, "commit", "-m", "init")
    check(done.returncode == 0, "could not create the fixture repo: %s" % done.stderr)


def test_a_clean_repo_reports_branch_and_no_dirty_files():
    root = _root()
    _make_repo(root)
    status = QueryRepoStatus(root)
    check(status.IsRepository, "a real repo is not a repository: %s" % status.Error)
    check(bool(status.Branch), "a repo has an empty branch")
    check_eq(status.DirtyFiles, 0, "a clean repo reported dirty files")
    check_eq(status.UntrackedFiles, 0, "a clean repo reported untracked files")


def test_an_untracked_and_a_dirty_file_are_both_counted():
    root = _root()
    _make_repo(root)

    # Modify a tracked file and add an untracked one.
    _write(os.path.join(root, "tracked.txt"), "one\nmodified\n")
    _write(os.path.join(root, "brand_new.txt"), "new\n")

    status = QueryRepoStatus(root)
    check(status.IsRepository, "not a repository: %s" % status.Error)
    check_ge(status.DirtyFiles, 1, "the modified tracked file was not counted")
    check_ge(status.UntrackedFiles, 1, "the untracked file was not counted")


def test_a_non_repository_is_reported_not_thrown():
    root = _root()
    _write(os.path.join(root, "just_a_file.txt"), "plain\n")
    status = QueryRepoStatus(root)
    check(not status.IsRepository, "a plain directory passed as a repository")
    check(bool(status.Error), "a non-repo produced no message")
    check_eq(status.Branch, "", "a non-repo reported a branch")


def test_the_real_wasteland_repo_is_a_repository():
    """The engine's own checkout must query as a repository -- the status bar's
    most direct consumer."""
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", ".."))
    status = QueryRepoStatus(root)
    check(status.IsRepository, "the Wasteland checkout is not a repository")
    check(bool(status.Branch), "the Wasteland checkout has an empty branch")


def test_an_lfs_threshold_flags_large_untracked_files():
    """J.5: warn when a file over a threshold is added without LFS. A large
    untracked file must appear in LargeFileHints; a small one must not."""
    root = _root()
    _make_repo(root)
    _write(os.path.join(root, "huge_asset.bin"), "x" * 5000)
    _write(os.path.join(root, "small_asset.bin"), "x" * 10)

    # Over a 1 KB threshold.
    status = QueryRepoStatus(root, 1024)
    check(status.IsRepository, "not a repo: %s" % status.Error)
    paths = [hint[0] for hint in status.LargeFileHints]
    check(any("huge_asset.bin" in p for p in paths),
          "the large file was not flagged for LFS: %s" % paths)
    check(not any("small_asset.bin" in p for p in paths),
          "a small file was flagged for LFS: %s" % paths)
    check(status.UntrackedFiles >= 2, "the fixture untracked count changed")


def test_an_absent_threshold_means_no_lfs_hints():
    root = _root()
    _make_repo(root)
    _write(os.path.join(root, "huge_asset.bin"), "x" * 5000)
    status = QueryRepoStatus(root)
    check_eq(status.LargeFileHints, [],
             "the default (no threshold) still collected LFS hints")


def test_a_modified_tracked_file_over_the_threshold_is_flagged():
    """A tracked-but-now-huge file is the other way a big asset reaches a repo
    without LFS -- a per-file convert, not a fresh add."""
    root = _root()
    _make_repo(root)
    _write(os.path.join(root, "tracked.txt"), "x" * 5000)  # was tiny
    status = QueryRepoStatus(root, 1024)
    paths = [hint[0] for hint in status.LargeFileHints]
    check(any("tracked.txt" in p for p in paths),
          "a modified over-threshold tracked file was not flagged: %s" % paths)


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
