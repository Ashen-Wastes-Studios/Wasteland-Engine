"""Lint every shipped .wlbtree / .wlblocks against the real engine.

Route map VB-5, Route C: "a headless lint gate on every shipped asset" --
the cheapest regression guard in the document, because it retroactively covers
every graph already on disk rather than only the next one written.

WHAT THE REAL API IS (verified against Bindings.cpp, not guessed -- a first
attempt here guessed `W.ValidateTree(path)` and every test errored):

    Wasteland.VisualBlocks.Load(path, graph)      -> None on success, str on failure
    Wasteland.VisualBlocks.ValidateTree(graph)   -> ValidationResult
    Wasteland.VisualBlocks.ValidateFlow(graph)   -> ValidationResult
    Wasteland.VisualBlocks.CompileTreeFile(path) -> (ok, python, report, error)
    Wasteland.VisualBlocks.CompileFlow(graph)    -> generated Python source

Note these live on the `VisualBlocks` SUBMODULE, not on `Wasteland` directly, and
Load/Validate/CompileFlow take a LOADED Graph rather than a path. Only
CompileTreeFile takes a path.

What this catches:
  - a node type registered on one side of the system but not the other, so an
    authored asset stops loading;
  - a validator rule that starts rejecting content that used to load;
  - a graph that compiles but emits Python which does not ast.parse();
  - non-determinism: two compiles of one asset must be byte-identical.

No filename list is hardcoded -- assets are globbed, so an asset added later is
linted the moment it lands. A suite that pins today's four files passes forever
after the new fifth one breaks.
"""

from __future__ import annotations

import ast
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import runner  # noqa: E402
from runner import check, check_eq, check_ge  # noqa: E402

import wlpath  # noqa: E402

# REQUIRED. The interpreter is isolated via python314._pth, so the engine module
# is not importable until wlpath.setup() puts it on sys.path. Without this every
# test raises ModuleNotFoundError -- and because run_module catches Exception per
# test, the run still prints a tidy summary instead of failing loudly.
wlpath.setup()

# Anchor the repo on THIS FILE, not the cwd. t.sh runs the runner from inside
# tests/headless, so a cwd-relative path resolves elsewhere and the asset glob
# silently matches nothing: 0/4 checks, looking like it ran.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(REPO, "DemonCore-Editor", "assets")


def _graphs():
    return (sorted(glob.glob(os.path.join(ASSETS, "**", "*.wlbtree"), recursive=True))
            + sorted(glob.glob(os.path.join(ASSETS, "**", "*.wlblocks"), recursive=True)))


def _rel(path):
    return os.path.relpath(path, REPO).replace(os.sep, "/")


def _finding_count(result):
    """ValidationResult exposes a list of findings; accept several shapes."""
    if result is None:
        return 0
    for attr in ("findings", "Findings", "errors", "Errors"):
        got = getattr(result, attr, None)
        if got is not None and not callable(got):
            return len(got)
    try:
        return len(result)
    except TypeError:
        return 0


def test_shipped_graphs_are_discovered():
    """Guard the guard. If the glob silently matches nothing, every other check
    here passes vacuously -- which has happened twice in this repo."""
    found = _graphs()
    check(os.path.isdir(ASSETS),
          "asset root %s must exist -- a wrong path makes this suite vacuous"
          % ASSETS)
    check_ge(len(found), 4,
             "expected at least the 4 known shipped graphs (found %d)" % len(found))
    trees = [f for f in found if f.endswith(".wlbtree")]
    flows = [f for f in found if f.endswith(".wlblocks")]
    check_ge(len(trees), 3, "expected the shipped .wlbtree assets (found %d)" % len(trees))
    check_ge(len(flows), 1, "expected the shipped .wlblocks asset (found %d)" % len(flows))


def test_every_shipped_graph_loads():
    import Wasteland as W

    for path in _graphs():
        rel = _rel(path)
        graph = W.VisualBlocks.Graph()
        try:
            err = W.VisualBlocks.Load(path, graph)
        except Exception as exc:                       # noqa: BLE001
            check(False, "%s: Load raised %s: %s" % (rel, type(exc).__name__, exc))
            continue
        check(err is None, "%s: Load reported %r" % (rel, err))
        check_ge(len(graph.Nodes), 1, "%s must contain at least one node" % rel)


def test_every_shipped_graph_validates_with_zero_findings():
    import Wasteland as W

    for path in _graphs():
        rel = _rel(path)
        tree = path.endswith(".wlbtree")
        graph = W.VisualBlocks.Graph()
        try:
            err = W.VisualBlocks.Load(path, graph)
            if err is not None:
                check(False, "%s: Load failed, cannot validate (%s)" % (rel, err))
                continue
            result = (W.VisualBlocks.ValidateTree(graph) if tree
                      else W.VisualBlocks.ValidateFlow(graph))
        except Exception as exc:                       # noqa: BLE001
            check(False, "%s: validation raised %s: %s" % (rel, type(exc).__name__, exc))
            continue
        n = _finding_count(result)
        check_eq(n, 0, "%s must validate clean (got %d finding(s))" % (rel, n))


def test_every_shipped_graph_compiles_to_parseable_python():
    import Wasteland as W

    for path in _graphs():
        rel = _rel(path)
        tree = path.endswith(".wlbtree")
        try:
            if tree:
                ok, python, report, err = W.VisualBlocks.CompileTreeFile(path)
                check(bool(ok), "%s: CompileTreeFile reported failure: %s / %s"
                      % (rel, report, err))
                if not ok:
                    continue
            else:
                graph = W.VisualBlocks.Graph()
                load_error = W.VisualBlocks.Load(path, graph)
                if load_error is not None:
                    check(False, "%s: Load failed before CompileFlow (%s)"
                          % (rel, load_error))
                    continue
                python = W.VisualBlocks.CompileFlow(graph)
                check(bool(python), "%s: CompileFlow returned empty" % rel)
                if not python:
                    continue
        except Exception as exc:                       # noqa: BLE001
            compiler = "CompileTreeFile" if tree else "CompileFlow"
            check(False, "%s: %s raised %s: %s"
                  % (rel, compiler, type(exc).__name__, exc))
            continue
        check(len(python) > 0, "%s: compiler produced empty output" % rel)
        try:
            ast.parse(python)
        except SyntaxError as exc:
            # The most useful failure this suite can produce: generated code an
            # author shipped that cannot even parse.
            check(False, "%s: generated Python does not parse -- line %s: %s"
                  % (rel, exc.lineno, exc.msg))
            continue
        check(True, "%s: generated Python parses" % rel)


def test_compilation_is_deterministic():
    """Two compiles of the same asset must be byte-identical. Non-determinism
    means the compiler iterates an unordered container, so a rebuild silently
    changes emitted code -- invisible until someone diffs a generated file, and
    it makes every future comparison of generated output meaningless."""
    import Wasteland as W

    for path in _graphs():
        rel = _rel(path)
        tree = path.endswith(".wlbtree")
        try:
            if tree:
                a = W.VisualBlocks.CompileTreeFile(path)
                b = W.VisualBlocks.CompileTreeFile(path)
                if not a[0] or not b[0]:
                    check(False, "%s: tree compile failed: %s / %s"
                          % (rel, a[2:], b[2:]))
                    continue
                first, second = a[1], b[1]
            else:
                graph = W.VisualBlocks.Graph()
                load_error = W.VisualBlocks.Load(path, graph)
                if load_error is not None:
                    check(False, "%s: Load failed before determinism check (%s)"
                          % (rel, load_error))
                    continue
                first = W.VisualBlocks.CompileFlow(graph)
                second = W.VisualBlocks.CompileFlow(graph)
                if not first or not second:
                    check(False, "%s: CompileFlow returned empty output" % rel)
                    continue
        except Exception as exc:                       # noqa: BLE001
            compiler = "CompileTreeFile" if tree else "CompileFlow"
            check(False, "%s: %s raised %s: %s"
                  % (rel, compiler, type(exc).__name__, exc))
            continue
        check_eq(first, second, "%s must compile deterministically" % rel)

if __name__ == "__main__":
    runner.run_module("test_shipped_graphs", verbose=True)
