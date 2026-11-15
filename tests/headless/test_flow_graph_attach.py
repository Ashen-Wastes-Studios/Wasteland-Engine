"""Does an authored .wlblocks flow graph actually drive an entity?

`FlowGraphSystem` compiles and ticks flow graphs, and `Scene::OnUpdateRuntime`
calls it -- but nothing proved the chain end to end. Every previous claim came
from the panel emitting Python source, which skips the two things most likely
to be broken:

  * `Project::Get().GetAssetPath()` resolving the authored graph,
  * `Scene::OnUpdateRuntime` ever reaching the tick.

This drives the REAL path: attach a graph to an entity, tick the scene, and
assert on an OBSERVABLE world effect -- the transform actually moving. Not
"the component says Attached", which a system could set without the graph ever
running.

The negative controls are the point. A suite that only asserts the happy path
passes just as well against a system that does nothing, because "nothing
happened" and "it worked" differ only in what you look at:

  * a path that does not exist -> CompileError set, no movement
  * a graph attached but NOT ticked -> no movement, Attached stays False
  * one tick moves exactly 1.0, N ticks move exactly N -- the graph runs EVERY
    tick, not once

Run:  python tests/headless/test_flow_graph_attach.py
"""

from __future__ import annotations

import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import (  # noqa: E402
    Project,
    Scene,
    ScriptEventId,
    Vec3,
    VisualBlocksComponent,
)
from runner import check  # noqa: E402

# The editor checkout is a valid asset root: it has assets/graphs/*.wlblocks and
# assets/scripts/, which is everything a flow graph needs to load.
REPO = _os.path.dirname(_os.path.dirname(_HERE))
ASSET_ROOT = _os.path.join(REPO, "DemonCore-Editor")
GRAPH = "graphs/Mover.wlblocks"

STEP = 0.1

# The authored graph adds exactly this to Translation.x on EVERY tick.
PER_TICK = 1.0


def attach_root():
    """Point asset resolution at the editor checkout."""
    p = Project.Get()
    p.AttachAssetRoot(ASSET_ROOT)
    return p


def make_actor(name, pos):
    sc = Scene()
    e = sc.CreateEntity(name)
    e.SetPosition(Vec3(*pos))
    return sc, e


def attach_graph(sc, entity, path=GRAPH):
    comp = VisualBlocksComponent()
    comp.GraphPath = path
    entity.AddOrReplaceVisualBlocksComponent(comp)
    return comp


def tick(sc, seconds):
    """Drive the FULL runtime tick -- not StepPhysics, which skips graphs."""
    for _ in range(int(seconds / STEP)):
        sc.OnUpdateRuntime(STEP)


def live_x(entity):
    """Read the position from the LIVE entity.

    The object returned by SetPosition is a snapshot copy: pybind hands back a
    Python-owned value, and the graph mutates the registry's instance. Reading
    the original object afterwards shows the value as it was at set time --
    which is why a working graph reported 0 movement until this re-read was
    added. Same trap as test_tree_attach.py's live_health().
    """
    return entity.GetTransform().Translation.x


def read(entity):
    return entity.GetVisualBlocksComponent()


# --------------------------------------------------------------------------
def test_a_graph_attaches_and_moves_the_entity():
    """The real proof: an entity with Mover attached gains 1.0 of x per tick.

    This is the assertion that proves the graph RAN. Nothing about the
    component's own flags is checked first -- a system could set Attached
    without ever executing the emitted method.
    """
    attach_root()
    sc, actor = make_actor("actor", (0.0, 0.0, 0.0))
    attach_graph(sc, actor)

    before = live_x(actor)
    tick(sc, 1.0)  # 10 ticks
    after = live_x(actor)

    comp = read(actor)
    print("  Attached=%s CompileError=%r" % (comp.Attached, comp.CompileError[:70]))
    print("  x %.1f -> %.1f over 10 ticks" % (before, after))

    check(comp.CompileError == "",
          "graph must compile cleanly, got: %s" % comp.CompileError[:120])
    check(comp.Attached, "the graph must attach to the entity")

    moved = after - before
    check(abs(moved - 10.0 * PER_TICK) < 1e-4,
          "the graph must add %.1f per tick: moved %.4f over 10 ticks"
          % (PER_TICK, moved))


def test_the_graph_runs_every_tick_not_once():
    """One tick moves 1.0, fifty ticks move 50.0.

    A graph that ran only on attach -- or only on the first frame -- would
    satisfy a single "did it move" assertion. The linear relationship is what
    proves the per-frame call.
    """
    attach_root()
    sc, actor = make_actor("actor", (0.0, 0.0, 0.0))
    attach_graph(sc, actor)

    tick(sc, STEP)
    one = live_x(actor)
    check(abs(one - 1.0 * PER_TICK) < 1e-4,
          "one tick must move exactly %.1f, moved %.4f" % (PER_TICK, one))

    tick(sc, 4.9)  # 49 more ticks
    fifty = live_x(actor)
    check(abs(fifty - 50.0 * PER_TICK) < 1e-3,
          "fifty ticks must move exactly %.1f, moved %.4f" % (50.0 * PER_TICK, fifty))


def test_missing_graph_reports_instead_of_silently_standing_still():
    """A bad path must set CompileError, not leave the entity inert with no clue."""
    attach_root()
    sc, actor = make_actor("actor", (0.0, 0.0, 0.0))
    attach_graph(sc, actor, path="graphs/NoSuchGraph.wlblocks")

    before = live_x(actor)
    tick(sc, 0.5)
    comp = read(actor)

    print("  CompileError=%r Attached=%s" % (comp.CompileError[:70], comp.Attached))
    check(comp.CompileError != "",
          "a missing graph must report an error, not fail silently")
    check(not comp.Attached, "a graph that does not compile cannot be attached")
    check(abs(live_x(actor) - before) < 1e-6,
          "a graph that never attached must not move the entity")


def test_attached_but_unticked_does_nothing():
    """The negative control that matters.

    StepPhysics does NOT tick flow graphs -- only OnUpdateRuntime does. An
    entity stepped with the wrong call would stand still forever, which is
    exactly the 'works in the editor' bug class.
    """
    attach_root()
    sc, actor = make_actor("actor", (0.0, 0.0, 0.0))
    attach_graph(sc, actor)

    before = live_x(actor)
    for _ in range(40):
        sc.StepPhysics(STEP)  # physics only

    print("  after StepPhysics x40: x %.1f, Attached=%s"
          % (live_x(actor), read(actor).Attached))
    check(abs(live_x(actor) - before) < 1e-6,
          "StepPhysics must not tick flow graphs (x %.4f)" % live_x(actor))
    check(not read(actor).Attached,
          "an unticked graph must not report itself attached")

    # The same entity, ticked the right way, must now work.
    tick(sc, 0.5)
    print("  after OnUpdateRuntime x5: x %.1f, Attached=%s"
          % (live_x(actor), read(actor).Attached))
    check(read(actor).Attached,
          "OnUpdateRuntime must attach the graph (StepPhysics did not)")
    check(abs(live_x(actor) - before - 5.0 * PER_TICK) < 1e-4,
          "ticking the runtime must move the entity (x %.4f)" % live_x(actor))


def test_a_second_entity_gets_its_own_instance():
    """Two actors on one graph must move independently.

    The instance cache is keyed by entity UUID. A single shared module or a
    single shared instance would make the second actor mirror the first -- a
    bug that a one-entity suite cannot see.

    Both entities must live in ONE scene: `attach_graph` writes into whatever
    registry the entity belongs to, and two scenes would silently test two
    independent runs instead of two instances.
    """
    attach_root()
    sc = Scene()
    a = sc.CreateEntity("a")
    a.SetPosition(Vec3(0.0, 0.0, 0.0))
    b = sc.CreateEntity("b")
    b.SetPosition(Vec3(100.0, 0.0, 0.0))
    attach_graph(sc, a)
    attach_graph(sc, b)

    tick(sc, 0.5)  # 5 ticks

    ax, bx = live_x(a), live_x(b)
    print("  a.x %.1f (from 0), b.x %.1f (from 100)" % (ax, bx))
    check(abs(ax - 5.0 * PER_TICK) < 1e-4,
          "actor a must have moved 5.0 from 0, got %.4f" % ax)
    check(abs(bx - 100.0 - 5.0 * PER_TICK) < 1e-4,
          "actor b must have moved 5.0 from 100, got %.4f" % bx)

# --------------------------------------------------------------------------
def test_native_dispatch_reaches_a_graph_event():
    """DispatchScriptEvent must reach a graph's OnInteract.

    Before ScriptEventDispatcher, CallScriptMethod only reached the SCRIPT
    instance -- a graph's OnInteract never fired from native code, so a
    graph could react to Update but not to Interact. That is the gap this
    task closes, and the assertion that proves it is the y movement: the
    graph's OnUpdate moves x, its OnInteract moves y.
    """
    attach_root()
    sc = Scene()
    actor = sc.CreateEntity("actor")
    actor.SetPosition(Vec3(0.0, 0.0, 0.0))
    attach_graph(sc, actor, path="graphs/DualEvent.wlblocks")

    # Tick first: OnUpdate must fire, OnInteract must NOT. If ticking fired
    # both, the y assertion below would be measuring the wrong thing.
    tick(sc, 0.5)  # 5 ticks
    x = live_x(actor)
    y = actor.GetTransform().Translation.y
    print("  after tick:     x=%.1f y=%.1f" % (x, y))
    check(abs(x - 5.0) < 1e-3, "OnUpdate must have ticked (x=%.4f)" % x)
    check(abs(y) < 1e-6,
          "ticking must NOT fire OnInteract (y=%.4f)" % y)

    # Now dispatch the event natively.
    actor.DispatchScriptEvent(ScriptEventId.Interact)
    y = actor.GetTransform().Translation.y
    print("  after dispatch: x=%.1f y=%.1f" % (live_x(actor), y))
    check(abs(y - 5.0) < 1e-3,
          "DispatchScriptEvent(Interact) must reach the graph (y=%.4f)" % y)
    check(abs(live_x(actor) - x) < 1e-6,
          "OnInteract must not also move x (%.4f -> %.4f)" % (x, live_x(actor)))


def test_dispatch_reaches_the_script_and_the_graph_alike():
    """An entity with both a script and a graph must hear the event on both.

    The dispatcher walks script, then graph, then tree. This asserts the
    graph half; the script half is covered by the Interactor regression.

    The graph must have TICKED at least once first. Like the behaviour tree,
    it creates its Python instance lazily on the first tick, whereas a script
    answers immediately because ScriptEngine builds its instance at attach
    time. That asymmetry is real, and it is why the tick comes before the
    dispatch here -- an entity cannot be interacted with before the scene
    has updated at least once.
    """
    attach_root()
    sc = Scene()
    actor = sc.CreateEntity("actor")
    actor.SetPosition(Vec3(0.0, 0.0, 0.0))
    attach_graph(sc, actor, path="graphs/DualEvent.wlblocks")

    # CallScriptMethod must still exist and still answer False for an
    # entity with no script -- the regression guard for the old path.
    check(actor.CallScriptMethod("OnInteract") is False,
          "CallScriptMethod must still work on a scriptless entity")

    # The first tick creates the graph's instance.
    tick(sc, STEP)

    # And the dispatcher must reach the graph regardless of the script.
    actor.DispatchScriptEvent(ScriptEventId.Interact)
    check(abs(actor.GetTransform().Translation.y - 5.0) < 1e-3,
          "the graph must hear the event even with no script attached")


def test_dispatch_with_nothing_attached_is_quiet():
    """An entity with no attached system must not raise.

    Every entity in a scene goes through this path -- the vast majority have
    nothing attached. An exception per entity per event would be the normal
    case rather than the error case.
    """
    attach_root()
    sc = Scene()
    actor = sc.CreateEntity("actor")
    actor.SetPosition(Vec3(0.0, 0.0, 0.0))

    # None of these may raise.
    actor.DispatchScriptEvent(ScriptEventId.Interact)
    actor.DispatchScriptEvent(ScriptEventId.Damage)
    actor.DispatchScriptEvent(ScriptEventId.BeginPlay)
    check(True, "dispatching to a bare entity must be a quiet no-op")

    # And it must not have moved anything.
    check(abs(live_x(actor)) < 1e-6,
          "a bare entity must not move when an event is dispatched")
