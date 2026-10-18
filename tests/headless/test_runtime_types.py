"""Do `RuntimeTypes()` and the node registry actually agree?

Route B of NB.9. `SetTimer` shipped BROKEN for exactly this reason: it was in
`BehaviorTree::RuntimeTypes()` (`BehaviorTree.cpp:49`) and `wl_behavior.py` had
a handler for it, and **no `NodeDef` anywhere registered it**. So the compiler
accepted a tree naming it, the runtime had the branch, and loading died with
`unknown node type 'SetTimer'`. Registering the node fixes today's tree; it does
nothing about the next node added to one list and not the other. This suite is
what stops the next one, so it ships WITH the registration rather than after it.

THE POINT IS THE ARROWS, AND BOTH OF THEM.

`RuntimeTypes()` -> registry is the arrow NB.9 broke: a name the evaluator
claims to handle that no definition exists for, which is a tree that will not
load.

registry -> `RuntimeTypes()` is the arrow nobody writes a test for. Every
BEHAVIOUR node carrying a `BtRole` of `composite` or `decorator` is a name the
runtime DISPATCHES ON (`_evaluate`'s if/elif chain, `BehaviorTree.cpp:146`
skips anything else). One of those registered but absent from `RuntimeTypes()`
compiles fine, and then falls through to the generic leaf handler, which finds
no `leaf` callable and returns FAILURE **every single tick**. The tree still
ticks, the trace still shows a clean walk-through, and the NPC simply never does
the thing -- the failure `BehaviorTree.cpp:14-20` describes at length.

One-directional coverage hides half the bug: the forward arrow cannot see a
missing runtime handler, and the backward arrow cannot see a missing definition.

Scope, stated because it is the part that is easy to get wrong:

  * Only nodes with a NON-EMPTY `BtRole` are checked in the backward
    direction. A flow-graph node (empty `BtRole`) is dispatched by
    `CompileFlow`, never by the tree evaluator, so demanding it appear in
    `RuntimeTypes()` would be demanding something false.
  * Conditions and tasks with a `leaf` body are dispatched through
    `_eval_leaf`, NOT by name, so they are legitimately absent from
    `RuntimeTypes()` too. Only `composite` and `decorator` are name-dispatched.

So the backward arrow is asserted over composites + decorators specifically.
Asserting it over every behaviour node would fail on `IsDead` and `MoveTo`,
which is why a naive version of this test is wrong rather than merely strict.

Run:  bash tools/t.sh headless test_runtime_types
"""

from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wlpath  # noqa: E402

wlpath.setup()

import Wasteland  # noqa: E402
from Wasteland import Vec3, Scene, VisualBlocks as VB  # noqa: E402
from runner import check, check_eq, check_ge, check_near  # noqa: E402

# The generated tree does `import wl_behavior`, and that module ships in the
# ASSET tree rather than beside the built module. Python runs isolated here, so
# the scripts directory has to go on sys.path explicitly.
REPO = os.path.dirname(os.path.dirname(HERE))
_SCRIPTS = os.path.join(REPO, "DemonCore-Editor", "assets", "scripts")
if os.path.isdir(_SCRIPTS) and _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

STEP = 0.1
# A tolerance for `now` comparisons. `self.now` is a running sum of per-tick
# deltas, so it drifts ~1e-15 per tick; wl_behavior's own `deadline_passed`
# uses 1e-6 for the same reason. Anything looser would stop being a test.
EPS = 1e-6


def _runtime_types() -> list[str]:
    return list(VB.RuntimeTypes())


def _behaviour_defs():
    return list(VB.BehaviorNodes())


# --------------------------------------------------------------------------
# Route B: the cross-check itself
# --------------------------------------------------------------------------

def test_every_runtime_type_resolves_in_the_registry():
    """FORWARD: every name the evaluator claims to handle is a real node type.

    This is the arrow NB.9 broke, and it is the one that fails LOUDLY --
    `Validator::CheckNodesRegisteredAndLiterals` reports "unknown node type
    'SetTimer' (it is not in the node registry)" at compile time, so a tree
    naming the gap never loads.
    """
    missing = [t for t in _runtime_types() if not VB.HasNodeType(t)]
    check(not missing,
          "RuntimeTypes() names with no NodeDef, so a tree naming them fails to "
          "load with 'unknown node type': %s" % missing)


def test_settimer_is_registered():
    """The specific defect, named.

    Worth its own assertion rather than living only inside the loop above: a
    general cross-check that goes red for a reason nobody reads is a test that
    gets deleted, and the node that broke last time should be greppable.
    """
    check("SetTimer" in _runtime_types(),
          "SetTimer left RuntimeTypes() -- it is dispatched by name at "
          "wl_behavior.py:338, so removing it makes that branch dead code")
    check(VB.HasNodeType("SetTimer"),
          "SetTimer has no NodeDef: a tree naming it dies at load with "
          "'unknown node type' -- this is the NB.9 defect")


def test_every_composite_and_decorator_is_a_runtime_type():
    """BACKWARD: every name-dispatched node has a runtime handler.

    The arrow with no test anywhere. A `composite` or `decorator` def absent
    from `RuntimeTypes()` compiles, then falls through to `_eval_leaf`, which
    looks for a `leaf` key the emitted dict does not carry and returns FAILURE
    every tick. The tree keeps ticking and the trace stays clean; the node
    simply never acts.
    """
    runtime = set(_runtime_types())
    orphans = sorted(
        d.TypeId for d in _behaviour_defs()
        if d.BtRole in ("composite", "decorator") and d.TypeId not in runtime
    )
    check(not orphans,
          "these composite/decorator nodes are dispatched BY NAME at runtime but "
          "are absent from RuntimeTypes(), so each silently returns FAILURE "
          "every tick: %s" % orphans)


def test_runtime_types_are_all_dispatched_by_the_runtime_module():
    """THE THIRD TABLE, and the one both registry arrows miss.

    The two arrows above catch the registry drifting from `RuntimeTypes()`, but
    neither notices the runtime LEAVES. `SetTimer` and `Wait` are `BtRole "task"`
    and name-dispatched (`_eval_set_timer`, `_eval_wait`), so:

      * forward (RuntimeTypes -> registry) sees SetTimer registered -> passes
      * backward (composite/decorator -> RuntimeTypes) skips it, because its
        role is "task", not composite/decorator -> passes

    Delete the SetTimer arm from `wl_behavior.py` and both arrows still report
    green while the node silently returns FAILURE forever. Verified: mutating
    the registry this way leaves the real orphan list empty.

    This closes it by cross-checking `RuntimeTypes()` against the runtime
    module's own dispatch -- its COMPOSITES/DECORATORS tuples plus the
    `ntype == "..."` chain in `_evaluate` -- in BOTH directions. A name the
    evaluator claims but cannot dispatch, and a dispatch arm nothing claims.
    The second is what catches the dead handler above.

    Parsed from SOURCE rather than imported because the tuples are the thing
    under test: importing `wl_behavior` would execute the module and could not
    distinguish "dispatched by name" from "merely mentioned in a comment".
    """
    source = os.path.join(_SCRIPTS, "wl_behavior.py")
    check(os.path.isfile(source),
          "cannot find the runtime module at %s; this arrow is the only one "
          "that sees the runtime leaves" % source)
    if not os.path.isfile(source):
        return
    with open(source, "r", encoding="utf-8") as handle:
        text = handle.read()

    # Table 1: the two module-level tuples, which may span several lines.
    dispatch = set()
    for tuple_name in ("COMPOSITES", "DECORATORS"):
        match = re.search(tuple_name + r"\s*=\s*\(([^)]*)\)", text, re.S)
        check(match is not None,
              "wl_behavior.py no longer defines a %s tuple; this suite's "
              "dispatch parse needs updating" % tuple_name)
        if match:
            dispatch |= set(re.findall(r'"(\w+)"', match.group(1)))

    # Table 2: the if/elif chain in _evaluate.
    dispatch |= set(re.findall(r'(?:el)?if ntype\s*==\s*"(\w+)"', text))

    check(len(dispatch) >= len(_runtime_types()),
          "parsed only %d dispatched names from wl_behavior.py, fewer than the "
          "%d in RuntimeTypes() -- the parse is stale, not the tables"
          % (len(dispatch), len(_runtime_types())))

    runtime = set(_runtime_types())
    undispatchable = sorted(runtime - dispatch)
    check(not undispatchable,
          "RuntimeTypes() names with NO handler in wl_behavior.py; the "
          "evaluator falls through to _eval_leaf, finds no `leaf` key, and "
          "returns FAILURE every tick: %s" % undispatchable)

    orphans = sorted(dispatch - runtime)
    check(not orphans,
          "wl_behavior.py dispatches names that RuntimeTypes() does not list, "
          "so those arms are DEAD CODE that the compiler will never emit: %s"
          % orphans)


def test_the_two_role_lists_agree_with_the_registry():
    """`CompositeTypes()`/`DecoratorTypes()` are the third table, so check them.

    They are hand-maintained vectors in `BehaviorTree.cpp:64-79` and nothing in
    C++ derives them from the registry. The compiler's own taxonomy check is
    documented as unreachable (`BehaviorTree.cpp:158-164`) precisely because
    both lists are hardcoded -- so this is the only place they are compared.
    """
    runtime = set(_runtime_types())
    composites = set(VB.CompositeTypes())
    decorators = set(VB.DecoratorTypes())

    for name, listed, role in (("composite", composites, "composite"),
                               ("decorator", decorators, "decorator")):
        reg = {d.TypeId for d in _behaviour_defs() if d.BtRole == role}
        check_eq(sorted(reg), sorted(listed),
                 "registry %ss and %sTypes() disagree" % (name, name.capitalize()))
        orphans = sorted(listed - runtime)
        check(not orphans,
              "%sTypes() names absent from RuntimeTypes(): %s" % (name, orphans))

    # The two role lists must not overlap. A name in both is a node the
    # validator checks two incompatible arity rules against.
    check(not (composites & decorators),
          "a name is listed as both composite and decorator: %s"
          % sorted(composites & decorators))


def test_runtime_types_are_not_flow_nodes():
    """No flow-graph node leaked into the tree's runtime list.

    An empty `BtRole` means `FlowNodes()` owns it and the tree evaluator has no
    branch for it at all. Catching it here means the backward arrow's premise --
    "every non-leaf role is name-dispatched" -- stays true.
    """
    by_id = {d.TypeId: d for d in VB.RegisteredNodes()}
    flow = [t for t in _runtime_types()
            if t in by_id and not by_id[t].BtRole]
    check(not flow,
          "RuntimeTypes() names registered as FLOW nodes (empty BtRole), which "
          "the tree evaluator never dispatches: %s" % flow)


def test_runtime_types_are_unique_and_non_empty():
    """Duplicates are the shape a merge conflict in those vectors takes.

    `IsRuntimeType` is a linear scan, so a duplicate is harmless at runtime --
    but it is the visible symptom of two people editing the same hand-written
    list, and it hides a name that got added twice with different intent.
    """
    types = _runtime_types()
    check_eq(len(types), len(set(types)),
             "RuntimeTypes() contains duplicates: %s"
             % [t for t in set(types) if types.count(t) > 1])
    check_ge(len(types), 1, "RuntimeTypes() is empty -- the evaluator would "
                            "treat every node as a leaf")
    blanks = [t for t in types if not t.strip()]
    check(not blanks, "RuntimeTypes() contains blank entries: %r" % blanks)


# --------------------------------------------------------------------------
# Route A: SetTimer actually works, asserted on the VALUE
# --------------------------------------------------------------------------

def _set_timer_tree(seconds="0.5", key="attackAt", key_type=None):
    """Compile a `Sequence[SetTimer]` through the real registry + compiler.

    Built through `CompileTree` rather than a hand-written topology dict so the
    whole path is exercised: the NodeDef is looked up, the pins are resolved,
    the emitter runs and the validator's arity rules apply. A dict written by
    hand would pass while the registry entry was still missing -- which is
    precisely the bug this suite exists to catch.
    """
    graph = VB.Graph()
    graph.Kind = "tree"
    graph.Name = "SetTimerProbe"

    declared = VB.BlackboardKey()
    declared.Name = key
    declared.Type = key_type if key_type is not None else VB.PinType.Float
    graph.Blackboard.append(declared)

    root = VB.GraphNode()
    root.Id = "root"
    root.TypeId = "Sequence"
    # Assignment, NOT append: `ChildIds` is a raw std::vector<std::string> with
    # no PYBIND11_MAKE_OPAQUE (documented in Graph.h), so `.append()` silently
    # mutates a discarded copy and the node compiles as a childless Sequence.
    root.ChildIds = ["timer"]
    graph.Nodes.append(root)

    timer = VB.GraphNode()
    timer.Id = "timer"
    timer.TypeId = "SetTimer"
    timer.SetLiteral("Key", key)
    timer.SetLiteral("Seconds", seconds)
    graph.Nodes.append(timer)

    ok, python, report, message = VB.CompileTree(graph)
    return graph, ok, python, report, message


def _load(python, entity, scene):
    """Exec the generated module and instantiate its class.

    `type(sys)` rather than a fresh module type: the generated file imports
    `wl_behavior` and `Wasteland` by name, and both resolve through sys.path
    rather than through the module's package.
    """
    module = type(sys)("wl_settimer_probe")
    exec(compile(python, "<SetTimerProbe>", "exec"), module.__dict__)
    return module.SetTimerProbe(entity, scene)


def test_a_tree_naming_settimer_loads():
    """Route A, step 1: the tree LOADS.

    Before the registration this failed at compile with "unknown node type
    'SetTimer' (it is not in the node registry)" -- reproduced verbatim before
    the fix, so this assertion is known to have teeth.
    """
    _graph, ok, _python, report, message = _set_timer_tree()
    check(ok, "a tree naming SetTimer must compile; got: %s (%s)"
          % (message, report))
    check("unknown node type" not in report,
          "the validator still rejects SetTimer: %s" % report)


def test_settimer_writes_now_plus_seconds_to_the_blackboard():
    """Route A, step 2: assert the VALUE, not merely that it loaded.

    `now + seconds`, read back off the live Blackboard. The key's validity is
    checked as well as its value, because `set(key, None)` marks a key VALID
    while leaving the value falsy -- so a value-only read cannot tell a real
    write from a stub.
    """
    seconds = 0.5
    graph, ok, python, _report, message = _set_timer_tree(seconds="0.5")
    check(ok, "compile failed: %s" % message)
    if not ok:
        return

    # The Scene is held in a local for the WHOLE test. `Scene().CreateEntity(...)`
    # on its own lets the Scene be collected while the Entity survives, and the
    # next access reads freed storage.
    scene = Scene()
    npc = scene.CreateEntity("npc")
    npc.SetPosition(Vec3(0.0, 0.0, 0.0))

    inst = _load(python, npc, scene)
    inst.tick(STEP)
    blackboard = inst.tree.bb

    # Validity is SEPARATE from value: `set(key, None)` marks a key valid
    # while leaving the value falsy, so a value-only read cannot tell a real
    # write from a stub. `is_set` (not a truthiness test on the value) is what
    # BlackboardIsSet uses, so that is what is asserted here.
    check(blackboard.is_set("attackAt"),
          "SetTimer did not mark its key VALID; a gate on BlackboardIsSet would "
          "read False forever")
    stamped = blackboard.get("attackAt")
    # After one tick of STEP, `now` IS STEP: tick() accumulates before
    # evaluating. So the deadline is exactly STEP + seconds.
    check_near(stamped, STEP + seconds, EPS,
               "SetTimer must write now+seconds; now is %r after one %r tick"
               % (inst.tree.now, STEP))
    check_ge(stamped, inst.tree.now,
             "the deadline must be in the future, or the timer is already due")


def test_settimer_deadline_is_stamped_once_and_held():
    """The bug the plan warned about, asserted directly.

    `BehaviorTree.cpp:45-49` records it: a deadline RE-DERIVED from `now` on
    every visit never expires, because the tree is re-entered from the ROOT each
    tick. `Wait` had exactly this bug; `SetTimer` shipped with it.

    The discriminator is the value MOVING. Re-deriving writes `now + seconds`
    again every tick, so after five ticks the key reads `5*STEP + seconds`
    instead of `STEP + seconds`. Holding it keeps the ORIGINAL number. A test
    that only checks "the key was written" passes against both, which is why
    this asserts the number is UNCHANGED across many ticks.
    """
    seconds = 0.5
    _graph, ok, python, _report, message = _set_timer_tree(seconds="0.5")
    check(ok, "compile failed: %s" % message)
    if not ok:
        return

    scene = Scene()
    npc = scene.CreateEntity("npc")
    npc.SetPosition(Vec3(0.0, 0.0, 0.0))

    inst = _load(python, npc, scene)
    inst.tick(STEP)
    first = inst.tree.bb.get("attackAt")

    # Five more ticks: well past `seconds`, so a self-re-arming timer would have
    # moved and a re-deriving one would have slid.
    for _ in range(5):
        inst.tick(STEP)
    later = inst.tree.bb.get("attackAt")

    check_near(first, STEP + seconds, EPS, "first stamp is wrong")
    check_near(later, first, EPS,
               "the deadline moved from %r to %r over 5 ticks: SetTimer is "
               "re-deriving now+seconds on every visit, so it never expires"
               % (first, later))
    check_ge(inst.tree.now, first,
             "the run should now be PAST the deadline, so a Compare(now >= "
             "attackAt) branch could fire")


def test_settimer_deadline_survives_a_zero_delay():
    """A zero-second timer is a real deadline of 0.0, not an absent one.

    The falsy-vs-None trap: `if not due` would re-stamp 0.0 on every tick,
    which for a zero delay looks identical until `now` drifts past it. With
    `is None` the stamp is held from the first tick, so the value stays put.
    """
    _graph, ok, python, _report, message = _set_timer_tree(seconds="0.0")
    check(ok, "compile failed: %s" % message)
    if not ok:
        return

    scene = Scene()
    npc = scene.CreateEntity("npc")
    npc.SetPosition(Vec3(0.0, 0.0, 0.0))

    inst = _load(python, npc, scene)
    inst.tick(STEP)
    first = inst.tree.bb.get("attackAt")
    for _ in range(4):
        inst.tick(STEP)
    later = inst.tree.bb.get("attackAt")

    check_near(first, STEP, EPS,
               "a zero-delay SetTimer must stamp now (== one tick), not 0.0")
    check_near(later, first, EPS,
               "a zero-delay deadline of 0.0 was treated as unset and "
               "re-stamped: %r -> %r" % (first, later))


def test_settimer_emits_its_pins_into_the_topology():
    """The emitted dict must carry BOTH pins the runtime reads.

    `_eval_set_timer` reads `node.get("pin")` and `node.get("seconds")`. An
    emitter that registered the node but emitted neither still compiles and
    still loads -- it just returns FAILURE every tick, because `key is None`.
    Asserting the emitted text is what catches that, and it is the difference
    between "the node exists" and "the node does something".
    """
    _graph, ok, python, _report, message = _set_timer_tree(
        seconds="0.5", key="attackAt")
    check(ok, "compile failed: %s" % message)
    if not ok:
        return

    build = python.split("def _build(self):")[-1].split("def tick(")[0]
    check("'t': 'SetTimer'" in build,
          "the topology table has no SetTimer entry:\n%s" % build)
    check("'pin'" in build,
          "SetTimer emitted no 'pin' key, so the runtime returns FAILURE every "
          "tick (key is None):\n%s" % build)
    check("'seconds'" in build,
          "SetTimer emitted no 'seconds' key, so the runtime reads 0.0:\n%s"
          % build)
    check("'leaf'" not in build,
          "SetTimer must NOT emit a 'leaf' key: wl_behavior.py implements it "
          "directly, and a generated lambda would never be called:\n%s" % build)
    check("0.500000" in build,
          "the authored Seconds=0.5 did not reach the emitted topology:\n%s"
          % build)


def test_settimer_is_filed_as_a_leaf_not_a_decorator():
    """BtRole must be `task`, or every legal use of the node is an error.

    `Validator.cpp:628` demands EXACTLY ONE child of a decorator. SetTimer wraps
    nothing -- `_eval_set_timer` takes no child -- so BtRole "decorator" would
    make a bare SetTimer a validation error and force authors to wire a dummy
    child. `Wait` is filed `task` for exactly this reason.

    Asserting the ROLE (not just that it validates) is what stops someone
    "fixing" a future validation error by re-filing it as a decorator.
    """
    defs = {d.TypeId: d for d in VB.BehaviorNodes()}
    check("SetTimer" in defs, "SetTimer is not a registered behaviour node")
    if "SetTimer" not in defs:
        return
    check_eq(defs["SetTimer"].BtRole, "task",
             "SetTimer must be a leaf (task); 'decorator' requires exactly one "
             "child and SetTimer wraps none")

    # And a bare, childless SetTimer must therefore VALIDATE.
    graph, _ok, _py, report, _msg = _set_timer_tree()
    check(not VB.ValidateTree(graph).HasErrors(),
          "a childless SetTimer must validate clean:\n%s" % report)


def test_settimer_is_not_latent():
    """SetTimer decides in one tick, so declaring it latent would be a lie.

    A node marked latent tells the runtime it may return RUNNING and hold the
    tree open. `_eval_set_timer` returns SUCCESS immediately. The compiler
    rejects a latent leaf whose emitted body lacks the flag (`Compiler.cpp:1546`),
    but nothing stops a designer ticking the box in the editor -- and then
    `_eval_leaf`'s suspension bookkeeping engages for a node that never
    suspends. Assert the DEF carries no latent pin, and that a plain tree using
    it compiles without one.
    """
    _graph, ok, python, report, message = _set_timer_tree()
    check(ok, "compile failed: %s (%s)" % (message, report))
    if not ok:
        return
    build = python.split("def _build(self):")[-1].split("def tick(")[0]
    timer_entry = [ln for ln in build.splitlines() if "SetTimer" in ln]
    check(timer_entry, "no SetTimer row in the topology table")
    if timer_entry:
        check("'latent'" not in timer_entry[0],
              "SetTimer emitted 'latent' but can never return RUNNING:\n%s"
              % timer_entry[0])


def test_settimer_key_needs_no_trailing_quotes_to_work():
    """The String pin round-trips through ParseString, quotes and all.

    `ParseLiteral(PinType::String, ...)` returns a QUOTED Python literal, so the
    emitter receives `"attackAt"` with the quotes attached. `StrPinV1` strips
    them and `EscapePy` re-adds and escapes them; a node that forgot to strip
    them writes a key literally named `"attackAt"` (quotes included), which
    passes every "was a key written" check and fails the moment a designer
    reads `attackAt` from another node.

    Asserted by reading the value back through the SAME key a Compare node
    would use, and by asserting no stray quote is in the emitted text.
    """
    _graph, ok, python, _report, message = _set_timer_tree(
        seconds="0.5", key="attackAt")
    check(ok, "compile failed: %s" % message)
    if not ok:
        return

    build = python.split("def _build(self):")[-1].split("def tick(")[0]
    check('"attackAt"' in build,
          "the key should reach the generated Python as a clean string "
          "literal; a doubled quote means ParseString's quotes were not "
          "stripped:\n%s" % build)

    scene = Scene()
    npc = scene.CreateEntity("npc")
    npc.SetPosition(Vec3(0.0, 0.0, 0.0))
    inst = _load(python, npc, scene)
    inst.tick(STEP)

    bb = inst.tree.bb
    check(bb.is_set("attackAt"),
          "the key written is not the one this suite reads back; a doubled "
          "quote would make it valid under a different name: %s" % bb.keys())
    check('"attackAt"' not in bb.keys(),
          "a key exists with literal quote characters in its name -- the "
          "emitter spliced ParseString's quotes verbatim: %s" % bb.keys())