"""Construction Script -- Task B.1 (OnBeginPlay).

The plan's verification, stated as a test: a graph that sets a variable on
begin play and increments it on update; assert the variable is 1 after one tick
and N after N ticks.

The variable is not directly readable -- the flow-graph instance lives in
FlowGraphSystem's private instance map -- so the graph WRITES its variable into
the entity's transform. That is both observable and a more honest test: it
asserts the variable's value as the engine itself would use it, not a test-only
introspection path.

Run:  python tests/headless/runner.py test_construction_script
"""

from __future__ import annotations

import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wlpath  # noqa: E402

wlpath.setup()

from runner import check, check_eq, check_near  # noqa: E402
import Wasteland as W  # noqa: E402

STEP = 1.0 / 60.0


def _build_listener_graph(path):
    """OnBeginPlay writes 1 to the entity's X; OnUpdate adds 1 each tick.

    Two events in ONE graph, and OnUpdate is declared FIRST on purpose:
    FlowGraphSystem ticks only the FIRST event method per frame, so a graph that
    put OnBeginPlay first would never tick its OnUpdate -- its handler would run
    once and then the graph would look broken. That ordering rule is part of what
    this test pins.

    The counter travels through the TRANSFORM rather than a graph variable,
    because the flow-graph instance lives in FlowGraphSystem's private instance
    map and there is no supported read-back. It also sidesteps a type error: a
    float variable cannot feed SetTransform's vec3 Translation, and no
    float-to-vec3 node exists. Vec3Add(GetPosition, (1,0,0)) is the vec3-safe
    increment, and reading X back is an assertion on state the engine itself
    uses rather than a test-only introspection path.
    """
    vb = W.VisualBlocks
    text = '''kind: flow
schema: 2
name: "ConstructionProbe"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: read_pos
    type: GetPosition
    x: 380
    y: 320
  - id: step
    type: Vec3Add
    x: 700
    y: 320
    pins:
      - { pin: B, value: "(1.0, 0.0, 0.0)" }
  - id: update_write
    type: SetTransform
    x: 1020
    y: 320
    pins:
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
  - id: begin
    type: OnBeginPlay
    x: 60
    y: 80
  - id: begin_write
    type: SetTransform
    x: 380
    y: 80
    pins:
      - { pin: Translation, value: "(10.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - from: { node: begin, pin: Out }
    to: { node: begin_write, pin: In }
  - from: { node: update, pin: Out }
    to: { node: update_write, pin: In }
  - from: { node: read_pos, pin: Position }
    to: { node: step, pin: A }
  - from: { node: step, pin: Value }
    to: { node: update_write, pin: Translation }
'''
    graph = vb.Graph()
    error = vb.Parse(text, graph)
    check(error is None, "the construction graph must parse")
    if error is not None:
        print("   ", error.Message)
        return None

    result = vb.ValidateFlow(graph)
    check_eq(result.ErrorCount(), 0,
             "the construction graph must validate (got %d errors)"
             % result.ErrorCount())

    source = vb.CompileFlow(graph)
    check("def OnBeginPlay(self" in source,
          "the graph must compile to an OnBeginPlay method")
    check("def OnUpdate(self" in source,
          "the graph must compile to an OnUpdate method")
    check("_beginPlayFired" not in source,
          "the graph must NOT need a hand-rolled `began` flag: the dispatcher "
          "owns the once-per-session contract")

    err = vb.Save(path, graph)
    check(err is None, "the construction graph must save")
    return None if err is not None else path


def _attach(path):
    from Wasteland import Scene, VisualBlocksComponent

    scene = Scene()
    entity = scene.CreateEntity("constructed")
    graph = VisualBlocksComponent()
    graph.GraphPath = path
    entity.AddVisualBlocksComponent(graph)
    # The graph attaches on the first tick.
    scene.OnUpdateRuntime(STEP)
    return scene, entity


def _x(entity):
    return entity.GetTransform().Translation.x


def test_begin_play_fires_once_at_runtime_start():
    """THE PLAN'S VERIFICATION: set on begin play, increment on update."""
    path = _build_listener_graph(os.path.join(
        tempfile.gettempdir(), "wasteland_construction.wlblocks"))
    if path is None:
        return
    scene, entity = _attach(path)

    # The attach tick already ran OnUpdate once: X went 0 -> 1. OnBeginPlay has
    # NOT run, and its value is 10 -- so 1 here is unambiguous evidence that only
    # the update half fired.
    check_near(_x(entity), 1.0, 1e-4,
               "before runtime start only OnUpdate must have run (got %.3f)"
               % _x(entity))

    scene.OnRuntimeStart()

    # BeginPlay writes 10 -- a value no update tick can produce.
    check_near(_x(entity), 10.0, 1e-4,
               "OnBeginPlay must have run exactly once at runtime start "
               "(got %.3f)" % _x(entity))

    # One update tick: 10 -> 11.
    scene.OnUpdateRuntime(STEP)
    check_near(_x(entity), 11.0, 1e-4,
               "one update tick must increment the counter to 11")

    for _ in range(9):
        scene.OnUpdateRuntime(STEP)
    check_near(_x(entity), 20.0, 1e-4,
               "N ticks must leave the counter at 10+N (got %.3f)" % _x(entity))

    # A SECOND play session DOES re-run BeginPlay -- the UE-consistent meaning
    # of the event, and the first version of this test got it wrong by asserting
    # once-per-scene-lifetime. The construction script re-runs because a new play
    # is a new beginning, not because the dispatch is broken.
    scene.OnRuntimeStart()
    check_near(_x(entity), 10.0, 1e-4,
               "a second play session must re-run BeginPlay (got %.3f)"
               % _x(entity))


def test_begin_play_fires_for_entities_spawned_mid_play():
    """An entity created AFTER runtime start must still get its BeginPlay.

    Without this a script that spawns during play produces an entity whose
    construction script never runs -- invisible, because the entity still ticks,
    it just never initialised.
    """
    path = _build_listener_graph(os.path.join(
        tempfile.gettempdir(), "wasteland_construction.wlblocks"))
    if path is None:
        return
    scene, _first = _attach(path)
    scene.OnRuntimeStart()

    from Wasteland import VisualBlocksComponent
    spawned = scene.CreateEntity("spawned")
    graph = VisualBlocksComponent()
    graph.GraphPath = path
    spawned.AddVisualBlocksComponent(graph)
    scene.OnUpdateRuntime(STEP)

    # 11, not 10: the attach-time BeginPlay writes 10 and the SAME frame's
    # OnUpdate then adds 1. Both halves ran, which is the point -- an entity
    # spawned mid-play gets its construction script and starts ticking.
    check_near(_x(spawned), 11.0, 1e-4,
               "an entity spawned during play must get its own BeginPlay "
               "and tick in the same frame (got %.3f)" % _x(spawned))


def test_begin_play_does_not_fire_before_runtime_start():
    """Attaching a graph in the editor must not run the construction script."""
    path = _build_listener_graph(os.path.join(
        tempfile.gettempdir(), "wasteland_construction.wlblocks"))
    if path is None:
        return
    scene, entity = _attach(path)

    for _ in range(5):
        scene.OnUpdateRuntime(STEP)

    # _attach ticked once (X = 1) and the loop adds 5 more, so X = 6. BeginPlay
    # never fired -- if it had, the graph would have written 10 and the count
    # would restart from there.
    check_near(_x(entity), 6.0, 1e-4,
               "before runtime start only OnUpdate must run (got %.3f)"
               % _x(entity))


def main():
    test_begin_play_fires_once_at_runtime_start()
    test_begin_play_fires_for_entities_spawned_mid_play()
    test_begin_play_does_not_fire_before_runtime_start()
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
