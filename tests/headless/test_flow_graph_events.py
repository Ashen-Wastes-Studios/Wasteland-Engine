"""Multi-event flow graphs (schema 2): one generated method per event node.

A schema-1 graph has exactly one event node and compiles exactly as before.
A schema-2 graph may declare several, each with a unique non-empty EventName,
and the compiler emits one `def <EventName>` per node -- the same shape as a
Blueprint, where each event is its own function.

The assertions that matter are on the EMITTED METHODS, not on the component's
flags: a compiler that emitted one method and ignored the rest would pass any
"did it attach" check.

Negative fixtures live in a temporary asset root rather than in the repo's
asset tree. `Project::AttachAssetRoot` requires the root to contain an
`assets/` directory, so the temp root gets one.

Run:  python tests/headless/runner.py test_flow_graph_events
"""

from __future__ import annotations

import os as _os
import sys as _sys
import tempfile
import types

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import Project, Scene, Vec3  # noqa: E402
from runner import check  # noqa: E402

REPO = _os.path.dirname(_os.path.dirname(_HERE))
ASSET_ROOT = _os.path.join(REPO, "DemonCore-Editor")
GRAPH = "graphs/DualEvent.wlblocks"

# Two event nodes under schema 1 -- the combination that used to be the only
# rule, and which must keep being rejected now that schema 2 exists.
SCHEMA1_TWO_EVENTS = """\
kind: flow
schema: 1
name: TwoEvents
nodes:
  - id: ev1
    type: FlowOnEvent
    x: 60
    y: 80
    event: OnUpdate
  - id: ev2
    type: FlowOnEvent
    x: 60
    y: 260
    event: OnInteract
"""

# Two events sharing one name. The emitter writes one method per event, so the
# second def would silently overwrite the first and OnUpdate could never fire.
DUPLICATE_EVENTS = """\
kind: flow
schema: 2
name: DuplicateEvents
nodes:
  - id: ev1
    type: FlowOnEvent
    x: 60
    y: 80
    event: OnUpdate
  - id: ev2
    type: FlowOnEvent
    x: 60
    y: 260
    event: OnUpdate
"""


def attach_root():
    p = Project.Get()
    p.AttachAssetRoot(ASSET_ROOT)
    return p


def compile_graph(path):
    """Compile an authored graph, letting the binding's error propagate."""
    return Scene.CompileFlowGraph(path)


def load_instance(source, class_name, entity):
    """exec() the generated source and construct the class, as the runtime does.

    The runtime builds a types.ModuleType, execs into its __dict__, then
    instantiates with (entity, scene). Reproduced here so the test exercises
    the same shape rather than a simplified one.
    """
    mod = types.ModuleType("_test_flow")
    exec(source, mod.__dict__)
    return getattr(mod, class_name)(entity, None)


def write_fixture(root, name, text):
    """Write a graph into <root>/assets and return its project-relative path."""
    assets = _os.path.join(root, "assets")
    _os.makedirs(assets, exist_ok=True)
    with open(_os.path.join(assets, name), "w", newline="") as handle:
        handle.write(text)
    return name


# --------------------------------------------------------------------------
def test_schema_2_emits_one_method_per_event():
    """The emitted source must contain a def for EVERY event node."""
    attach_root()
    source = compile_graph(GRAPH)
    print("  %d chars of generated source" % len(source))

    check("def OnUpdate(self" in source,
          "the OnUpdate event must produce a method")
    check("def OnInteract(self" in source,
          "the OnInteract event must produce its OWN method")
    # And exactly one of each -- a duplicated def would mean the emitter ran
    # the walk twice for one event.
    check(source.count("def OnUpdate(self") == 1,
          "OnUpdate must be emitted exactly once")
    check(source.count("def OnInteract(self") == 1,
          "OnInteract must be emitted exactly once")


def test_each_event_method_has_its_own_effect():
    """Calling each method must produce that event's effect and not the other's.

    This is the assertion that proves the two methods are genuinely separate. A
    single method that ran twice would move the entity along one axis only.
    """
    attach_root()
    sc = Scene()
    actor = sc.CreateEntity("actor")
    actor.SetPosition(Vec3(0.0, 0.0, 0.0))

    source = compile_graph(GRAPH)
    inst = load_instance(source, "DualEvent", actor)

    inst.OnUpdate(0.1)
    x1, y1 = actor.GetTransform().Translation.x, actor.GetTransform().Translation.y
    print("  after OnUpdate:   x=%.1f y=%.1f" % (x1, y1))
    check(abs(x1 - 1.0) < 1e-4, "OnUpdate must move x by 1.0, got %.4f" % x1)
    check(abs(y1) < 1e-4, "OnUpdate must not touch y, got %.4f" % y1)

    inst.OnInteract(0.1)
    x2, y2 = actor.GetTransform().Translation.x, actor.GetTransform().Translation.y
    print("  after OnInteract: x=%.1f y=%.1f" % (x2, y2))
    check(abs(x2 - 1.0) < 1e-4,
          "OnInteract must not move x again, got %.4f" % x2)
    check(abs(y2 - 5.0) < 1e-4, "OnInteract must move y by 5.0, got %.4f" % y2)


def test_the_absent_third_event_is_a_quiet_no_op():
    """A graph with two events has no third method, and asking for one is a
    plain AttributeError rather than a generated stub."""
    attach_root()
    sc = Scene()
    actor = sc.CreateEntity("actor")
    actor.SetPosition(Vec3(0.0, 0.0, 0.0))

    inst = load_instance(compile_graph(GRAPH), "DualEvent", actor)
    check(not hasattr(inst, "OnDamage"),
          "an event the graph does not declare must not exist")


def test_schema_1_with_two_events_is_rejected():
    """Schema 1 keeps its one-event rule, and says why.

    The message names the schema bump, because the fix for an author hitting
    this is to declare schema: 2 -- not to delete an event.
    """
    with tempfile.TemporaryDirectory() as root:
        path = write_fixture(root, "TwoEvents.wlblocks", SCHEMA1_TWO_EVENTS)
        Project.Get().AttachAssetRoot(root)
        try:
            compile_graph(path)
            check(False, "a schema-1 graph with two events must be rejected")
        except RuntimeError as exc:
            message = str(exc)
            print("  rejected: %s" % message[:90])
            check("schema-1" in message or "second event" in message,
                  "the error must name the schema rule, got: %s" % message[:120])


def test_duplicate_event_names_are_rejected():
    """Two events sharing a name would silently overwrite one generated method."""
    with tempfile.TemporaryDirectory() as root:
        path = write_fixture(root, "DuplicateEvents.wlblocks", DUPLICATE_EVENTS)
        Project.Get().AttachAssetRoot(root)
        try:
            compile_graph(path)
            check(False, "duplicate event names must be rejected")
        except RuntimeError as exc:
            message = str(exc)
            print("  rejected: %s" % message[:90])
            check("duplicate event name" in message,
                  "the error must name the duplicate, got: %s" % message[:120])


def test_a_schema_1_graph_still_compiles_unchanged():
    """The shipped Mover graph is schema 1 with one event and must still work.

    This is the regression guard for the whole task: multi-event support must
    not have changed what a single-event graph emits.
    """
    attach_root()
    source = compile_graph("graphs/Mover.wlblocks")
    check("def OnUpdate(self" in source,
          "a schema-1 single-event graph must still emit OnUpdate")
    check(source.count("def OnUpdate(self") == 1,
          "a schema-1 graph must emit exactly one method")
    check("def OnInteract(self" not in source,
          "a schema-1 graph must not gain extra methods")
