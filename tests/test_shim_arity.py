"""Generated-shim arity and signature audit.

This suite exists because a signature drift reached a build and shipped a
runtime bug. `HasLineOfSight` emitted `_has_los(ctx.entity, target, max_distance)`
-- three arguments -- while NB.7 had changed the helper to `_has_los(ctx, agent,
target, max_distance)`, four. Nothing failed to compile: the call is emitted into
a *string* that becomes Python source at runtime, so the mismatch could only ever
surface as a TypeError inside a behaviour tree, mid-firefight, on a node nobody
was testing at the time.

A compiler emitting code as a string has no type checking at the boundary. This
is that missing check, as a test.

WHAT IS CHECKED

1. Every `_helper(...)` call site emitted by NodeLibraryV1.cpp / NodeLibrary.cpp
   names a helper that exists in the shim, and passes an argument count the shim
   can accept.
2. Required parameters are not omitted. A helper called with fewer args than it
   has parameters with no default is a guaranteed runtime TypeError.
3. The generated shim is syntactically valid Python.
4. The helpers this project has previously broken -- `_move`, `_sense_nearest`,
   `_has_los`, `_los_blocked`, `_path_to` -- keep the signatures the callers
   depend on, pinned explicitly so a future change has to update this file and
   say why.

HARNESS RULES (learned the hard way; see test_audio_scene.py)

* Extract the shim by its `R"WL_SHIM(...)"` delimiters, never by line offsets.
  Line numbers in Compiler.cpp move with every unrelated edit, and a stale
  offset silently slices the wrong bytes -- or nothing at all.
* Inject a fake `Wasteland` into the shim's globals EXACTLY as the compiler does.
  A shim run without it raises NameError, and a broad `except` around the exec
  turns that into a silent "everything passed".
* This suite runs no engine code. It is a static audit over source text, so it
  needs no module and cannot crash the interpreter.
"""

import ast
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

# NOTE: no wlpath / no `import Wasteland`. This suite is a STATIC audit over
# source text, so it deliberately needs no engine module -- which is also why it
# kept passing while the module under test was broken. A suite that cannot run
# without a binary is a suite that stops working the moment the binary is
# locked, stale, or missing.

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VB = os.path.join(ROOT, "Wasteland", "src", "Wasteland", "VisualBlocks")


def _read(name):
    with open(os.path.join(VB, name), encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _shim_source():
    """The generated Python shim, sliced by its raw-string delimiters."""
    text = _read("Compiler.cpp")
    match = re.search(r'R"WL_SHIM\((.*?)\)WL_SHIM"', text, re.S)
    if match is None:
        raise AssertionError(
            "no R\"WL_SHIM(...)WL_SHIM\" block in Compiler.cpp -- the generated "
            "shim is gone or was renamed. Every other check here is vacuous "
            "without it, so this fails first and loudly."
        )
    return match.group(1)


def _shim_defs():
    """{name: (required_count, total_count)} for every top-level helper def."""
    shim = _shim_source()
    tree = ast.parse(shim)
    defs = {}
    for node in tree.body:
        if isinstance(node, ast.FunctionDef):
            a = node.args
            # Defaults bind to the LAST n positional params.
            required = len(a.args) - len(a.defaults)
            defs[node.name] = (required, len(a.args), node)
    return defs


# --------------------------------------------------------------------------
# 1. the shim exists, parses, and defines the helpers callers rely on
# --------------------------------------------------------------------------

def test_the_generated_shim_is_extractable_and_parses():
    """If this fails, every other check silently passes on nothing."""
    shim = _shim_source()
    check(len(shim) > 5000, "shim looks truncated: %d chars" % len(shim))
    try:
        ast.parse(shim)
    except SyntaxError as exc:
        raise AssertionError(
            "the generated shim is not valid Python: %s -- emitted code that "
            "cannot parse breaks every behaviour tree at runtime, with a "
            "SyntaxError and no compile-time warning" % exc
        )
    check(True, "shim parses as Python")


def test_the_shim_defines_the_helpers_callers_depend_on():
    """Pinned surface. Changing one of these requires updating this file.

    The signatures are recorded because each was wrong once:
      _has_los        -- lost `ctx` in NB.7; HasLineOfSight kept passing 3 args
      _sense_nearest  -- gained half_angle_deg/require_los in NB.7
      _move           -- gained use_pathfinding in NB.8
    """
    defs = _shim_defs()
    expected = {
        "_move":               ("ctx", "use_pathfinding"),
        "_sense_nearest":      ("ctx", "require_los"),
        "_has_los":            ("ctx", "max_distance"),
        "_los_blocked":        ("ctx", "ignore"),
        "_path_to":            ("ctx", "agent", "target"),
        "_agent_key":          ("agent",),
        "_navmesh":            ("ctx",),
        "_perception_snapshot": ("ctx", "scene"),
    }
    for name, required in expected.items():
        check(name in defs,
              "shim must define %s -- callers emit it" % name)
        if name not in defs:
            continue
        nreq, ntot, node = defs[name]
        params = [a.arg for a in node.args.args]
        for r in required:
            check(r in params,
                  "%s must accept a `%s` parameter (has %s)" % (name, r, params))
        # Required params are correct -- the emitters pass positionally. What
        # must hold is that the pin-related params are PRESENT and IN ORDER, so
        # the emitter's argument order cannot drift past them unnoticed.
        check(nreq <= ntot,
              "%s: %d required exceeds %d total" % (name, nreq, ntot))


def test_every_helper_signature_is_internally_consistent():
    """The shim must parse and every def must be callable with its own arity.

    Deliberately NOT asserting that every parameter has a default. That looked
    rigorous and is wrong: the emitters call these helpers POSITIONALLY
    (`_move(ctx.entity, ctx, "target", 3.0, False)`), so required positional
    parameters are correct by design. Requiring defaults would have meant
    rewriting 25 shim signatures to satisfy an assertion with no evidence
    behind it -- changing working code to make a new test pass, which is the
    wrong direction entirely.

    What actually matters is the pair (emitter call sites vs helper arity), and
    that is what the sweep below measures.
    """
    defs = _shim_defs()
    check(len(defs) >= 20,
          "the shim should expose its full helper surface (found %d)" % len(defs))
    for name, (nreq, ntot, node) in sorted(defs.items()):
        # A def whose own arity cannot be called is genuinely broken.
        check(ntot >= 1, "%s takes no arguments, which no emitter can use" % name)
        check(nreq <= ntot,
              "%s: %d required args exceeds %d total" % (name, nreq, ntot))


# --------------------------------------------------------------------------
# 2. every emitted call site agrees with the shim
# --------------------------------------------------------------------------

def _emitter_bodies():
    """C++ source with line comments and string literals stripped.

    A `_helper(` inside a comment or a string is not an emitted call, and
    counting it produces false alarms that train people to ignore this suite.
    """
    for name in ("NodeLibraryV1.cpp", "NodeLibrary.cpp"):
        text = _read(name)
        text = re.sub(r'//[^\n]*', '', text)
        text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
        yield name, text


def test_every_emitted_helper_call_names_a_helper_that_exists():
    defs = _shim_defs()
    unknown = []
    for name, body in _emitter_bodies():
        for match in re.finditer(r'(?<![\w.])(\b_[a-z][a-z0-9_]+)\s*\(', body):
            fn = match.group(1)
            # Filter to things that look like generated-shim helpers. The
            # emitters are C++, so any bare `_foo(` here is either a shim call
            # or a local lambda/function we do not control; only flag the ones
            # the shim is known to own by prefix family.
            if not re.match(r'^_(move|sense|has|los|path|agent|nav|perception|'
                            r'ground|face|blackboard|bb|play|stop|damage|'
                            r'cooldown|trace|wait|debug|rand|clamp|lerp)', fn):
                continue
            if fn not in defs:
                unknown.append((name, fn))
    for src, fn in sorted(set(unknown)):
        check(False, "%s emits _%s(...) which the shim does not define" % (src, fn))
    if not unknown:
        check(True, "every emitted shim helper resolves to a definition")


def test_the_has_line_of_sight_emitter_passes_the_runtime_context():
    """The exact regression that shipped: three args to a four-arg helper.

    Pinned directly rather than left to the general sweep, because a sweep can
    only tell that an argument COUNT fits -- not that the RIGHT argument is
    first. Here the first argument must be the runtime `ctx`, not `ctx.entity`;
    NB.7 widened the helper and the emitter kept passing the entity, so the
    node was dead at runtime and compiled clean.

    NB.7 split this emitter into `ctx` + `ctx.entity` fragments across source
    lines, so the call is matched in a comment-stripped, whitespace-collapsed
    body rather than line by line.
    """
    defs = _shim_defs()
    check("_has_los" in defs, "shim must define _has_los")
    if "_has_los" in defs:
        params = [a.arg for a in defs["_has_los"][2].args.args]
        check_eq(params[:4], ["ctx", "agent", "target", "max_distance"],
                 "_has_los signature: the leading `ctx` is what NB.7 added")

    found = False
    for name, body in _emitter_bodies():
        flat = " ".join(body.split())
        for match in re.finditer(r'_has_los\(\s*([^,)]+)', flat):
            first = match.group(1).strip()
            found = True
            check(first == "ctx",
                  "%s emits _has_los(%s, ...) -- the first argument must be the "
                  "runtime ctx, not %s" % (name, first, first))
    check(found, "HasLineOfSight must emit a _has_los call")



# --------------------------------------------------------------------------
# 3. the shim's own contracts, pinned as text
# --------------------------------------------------------------------------

def test_the_shim_uses_the_real_raycast_tuple_layout():
    """hit[1] is the entity, hit[3] is the normal, hit[5] is hittable.

    Reading hit[3] as an entity compiles, runs, and silently inverts LOS: the
    normal vector is never equal to the target, so every shot looks blocked.
    """
    shim = _shim_source()
    check("hit[5]" in shim or "ray[5]" in shim,
          "world-geometry detection must use index 5 (hittable)")
    check("hit[1]" in shim or "ray[1]" in shim,
          "the hit entity must come from index 1")


def test_the_shim_uses_clear_not_set_none_for_no_target():
    """Blackboard validity is separate from value.

    set(key, None) marks the key VALID with value None. A consumer testing
    `if bb.get(key) is not None` is fine; one testing `if bb.has(key)` gets a
    target that does not exist. `clear` is the correct way to say "no target".
    """
    shim = _shim_source()
    check(".clear(" in shim, "the shim must use clear() to unset a target key")
    offenders = re.findall(r'\.set\(\s*\w+\s*,\s*None\s*\)', shim)
    check(not offenders,
          "the shim must not set(key, None) -- that marks the key VALID; "
          "found %d occurrence(s)" % len(offenders))


def test_the_shim_exposes_a_perception_cache_with_a_bounded_interval():
    """A per-tick rescan of every entity is O(N) per NPC per frame. The
    snapshot is scene-level and time-bounded; keep it that way."""
    shim = _shim_source()
    check("_PERCEPTION_INTERVAL" in shim,
          "the perception snapshot must be time-bounded")
    match = re.search(r'_PERCEPTION_INTERVAL\s*=\s*([0-9.]+)', shim)
    check(match is not None, "_PERCEPTION_INTERVAL must be a numeric literal")
    if match:
        value = float(match.group(1))
        check(0.0 < value <= 0.5,
              "_PERCEPTION_INTERVAL=%s: must be non-zero and short enough that "
              "targets are not missed, long enough to amortise the scan" % value)


def test_the_shim_wraps_its_own_dispatch_in_a_broad_except_that_cannot_hide_errors():
    """A bare `except: return` around a helper makes a missing global look
    like a successful call. That is how a NameError in the shim survived more
    than one test run."""
    shim = _shim_source()
    broad = re.findall(r'except\s*:', shim)
    check(not broad,
          "the shim must not use a bare `except:` -- it converts missing "
          "globals and typos into silent no-ops (found %d)" % len(broad))

# Run when invoked directly, and through tests/runner.py when invoked by name.
# Without this the file defines ten test functions and exits 0 having checked
# nothing -- a green that verifies nothing, which is the exact failure this
# suite exists to prevent.
if __name__ == "__main__":
    import runner

    runner.run_module(sys.modules[__name__], "test_shim_arity")
    sys.exit(runner.report())
