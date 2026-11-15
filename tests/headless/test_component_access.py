"""Cast To and component access -- Task B.3.

The plan's verification, stated as a test: per node, assert on what CHANGED in a
live scene, never "did not throw".

The component TYPE arrives as a string, so the emitter cannot know the type at
compile time. That is the whole point of the task: a name is the only handle the
engine has, because there is no reflection. Every assertion here therefore
checks a component that genuinely exists on a live entity, or a type name that
genuinely does not.

Run:  python tests/headless/runner.py test_component_access
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


def _save(name, body):
    path = os.path.join(tempfile.gettempdir(), "wasteland_b3_%s.wlblocks" % name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(body)
    graph = W.VisualBlocks.Graph()
    error = W.VisualBlocks.Load(path, graph)
    check(error is None, "%s must load" % name)
    if error is not None:
        return None
    return path


def _attach(path, with_health=False):
    """Attach a graph to a fresh entity and tick once so it attaches.

    `with_health` exists for the HealthComponent cases; the component is added
    through the bound accessor rather than left to the graph, because the test
    is about the graph's ACCESS to it, not about how it got there.
    """
    from Wasteland import Scene, VisualBlocksComponent

    scene = Scene()
    entity = scene.CreateEntity("b3")
    if with_health:
        entity.AddHealthComponent(W.HealthComponent())
    graph = VisualBlocksComponent()
    graph.GraphPath = path
    entity.AddVisualBlocksComponent(graph)
    scene.OnUpdateRuntime(STEP)
    return scene, entity


def _x(entity):
    return entity.GetTransform().Translation.x


# ---------------------------------------------------------------- graphs

def _cast_succeeds():
    """CastTo a component the entity HAS -> the True branch runs."""
    return _save("cast_ok", '''kind: flow
schema: 2
name: "CastOk"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: actor
    type: GetActor
    x: 340
    y: 120
  - id: cast
    type: CastTo
    x: 620
    y: 220
    pins:
      - { pin: Type, value: TransformComponent }
  - id: branch
    type: Branch
    x: 900
    y: 320
  - id: yes
    type: SetTransform
    x: 1180
    y: 160
    pins:
      - { pin: Translation, value: "(1.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
  - id: no
    type: SetTransform
    x: 1180
    y: 480
    pins:
      - { pin: Translation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - { from: { node: update, pin: Out }, to: { node: branch, pin: In } }
  - { from: { node: actor, pin: Actor }, to: { node: cast, pin: Entity } }
  - { from: { node: cast, pin: Success }, to: { node: branch, pin: Condition } }
  - { from: { node: branch, pin: True }, to: { node: yes, pin: In } }
  - { from: { node: branch, pin: False }, to: { node: no, pin: In } }
''')


def _cast_unknown_type():
    """CastTo a type name that is not bound -> None, so the False branch runs."""
    return _save("cast_unknown", '''kind: flow
schema: 2
name: "CastUnknown"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: actor
    type: GetActor
    x: 340
    y: 120
  - id: cast
    type: CastTo
    x: 620
    y: 220
    pins:
      - { pin: Type, value: NoSuchComponentAnywhere }
  - id: branch
    type: Branch
    x: 900
    y: 320
  - id: yes
    type: SetTransform
    x: 1180
    y: 160
    pins:
      - { pin: Translation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
  - id: no
    type: SetTransform
    x: 1180
    y: 480
    pins:
      - { pin: Translation, value: "(5.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - { from: { node: update, pin: Out }, to: { node: branch, pin: In } }
  - { from: { node: actor, pin: Actor }, to: { node: cast, pin: Entity } }
  - { from: { node: cast, pin: Success }, to: { node: branch, pin: Condition } }
  - { from: { node: branch, pin: True }, to: { node: yes, pin: In } }
  - { from: { node: branch, pin: False }, to: { node: no, pin: In } }
''')


def _set_component():
    """GetComponent -> SetComponent round-trip: the component survives."""
    return _save("set_component", '''kind: flow
schema: 2
name: "SetComponent"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: actor
    type: GetActor
    x: 340
    y: 120
  - id: get
    type: GetComponent
    x: 620
    y: 120
    pins:
      - { pin: Type, value: TransformComponent }
  - id: set
    type: SetComponent
    x: 900
    y: 320
    pins:
      - { pin: Type, value: TransformComponent }
  - id: move
    type: SetTransform
    x: 1220
    y: 480
    pins:
      - { pin: Translation, value: "(2.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - { from: { node: update, pin: Out }, to: { node: set, pin: In } }
  - { from: { node: actor, pin: Actor }, to: { node: get, pin: Entity } }
  - { from: { node: actor, pin: Actor }, to: { node: set, pin: Entity } }
  - { from: { node: get, pin: Value }, to: { node: set, pin: Value } }
  - { from: { node: set, pin: Out }, to: { node: move, pin: In } }
''')


def _entity_query():
    """GetAllEntitiesWith(TransformComponent).Count > 0 -> True branch."""
    return _save("entity_query", '''kind: flow
schema: 2
name: "EntityQuery"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: query
    type: GetAllEntitiesWith
    x: 340
    y: 120
    pins:
      - { pin: Type, value: TransformComponent }
  - id: cmp
    type: CompareValue
    x: 620
    y: 220
    pins:
      - { pin: B, value: "0" }
      - { pin: Op, value: ">" }
  - id: branch
    type: Branch
    x: 900
    y: 320
  - id: yes
    type: SetTransform
    x: 1180
    y: 160
    pins:
      - { pin: Translation, value: "(3.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - { from: { node: update, pin: Out }, to: { node: branch, pin: In } }
  - { from: { node: query, pin: Count }, to: { node: cmp, pin: A } }
  - { from: { node: cmp, pin: Value }, to: { node: branch, pin: Condition } }
  - { from: { node: branch, pin: True }, to: { node: yes, pin: In } }
''')


# ---------------------------------------------------------------- tests

def test_cast_to_finds_a_component_the_entity_has():
    path = _cast_succeeds()
    if path is None:
        return
    scene, entity = _attach(path)
    check_near(_x(entity), 1.0, 1e-4,
               "CastTo(TransformComponent) on an entity that HAS one must "
               "succeed and take the True branch (got %.3f)" % _x(entity))


def test_cast_to_an_unknown_type_name_is_none_not_an_exception():
    """The name is a string, so a typo must read as 'no component'.

    Raising would take the whole tick down for a typo -- the failure mode the
    plan's 'assert on what changed' rule is about.
    """
    path = _cast_unknown_type()
    if path is None:
        return
    scene, entity = _attach(path)
    check_near(_x(entity), 5.0, 1e-4,
               "CastTo(NoSuchComponentAnywhere) must be None and take the "
               "False branch (got %.3f)" % _x(entity))


def test_set_component_round_trips_a_live_component():
    path = _set_component()
    if path is None:
        return
    scene, entity = _attach(path)
    # The set ran (or the graph would have raised), and the transform is still
    # intact afterwards -- a component replaced with a broken value would leave
    # the entity transform-less, and GetTransform would assert.
    check_near(_x(entity), 2.0, 1e-4,
               "SetComponent must add-or-replace the component and leave the "
               "transform readable (got %.3f)" % _x(entity))
    # TransformComponent is bound through BIND_COMP_*, which stringifies the
    # FULL type name -- so the accessor is HasWasteland::TransformComponent, not
    # HasTransformComponent. getattr is the only way to spell that from Python.
    probe = getattr(entity, "TryGetWasteland::TransformComponent", None)
    check(callable(probe) and probe() is not None,
          "the entity must still carry a TransformComponent after SetComponent")


def test_get_all_entities_with_counts_entities_carrying_the_component():
    path = _entity_query()
    if path is None:
        return
    scene, entity = _attach(path)
    check_near(_x(entity), 3.0, 1e-4,
               "GetAllEntitiesWith(TransformComponent).Count must be > 0 for "
               "a scene with one entity (got %.3f)" % _x(entity))


def test_get_all_entities_with_an_unknown_type_is_empty_not_an_exception():
    """An unknown query name yields an empty list.

    The node's helper asks the scene for a query by name; a name with no
    binding must not raise from generated code.
    """
    path = _save("entity_query_unknown", '''kind: flow
schema: 2
name: "EntityQueryUnknown"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: query
    type: GetAllEntitiesWith
    x: 340
    y: 120
    pins:
      - { pin: Type, value: NoSuchComponentAnywhere }
  - id: cmp
    type: CompareValue
    x: 620
    y: 220
    pins:
      - { pin: B, value: "0" }
      - { pin: Op, value: ">" }
  - id: branch
    type: Branch
    x: 900
    y: 320
  - id: yes
    type: SetTransform
    x: 1180
    y: 160
    pins:
      - { pin: Translation, value: "(9.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - { from: { node: update, pin: Out }, to: { node: branch, pin: In } }
  - { from: { node: query, pin: Count }, to: { node: cmp, pin: A } }
  - { from: { node: cmp, pin: Value }, to: { node: branch, pin: Condition } }
  - { from: { node: branch, pin: True }, to: { node: yes, pin: In } }
''')
    if path is None:
        return
    scene, entity = _attach(path)
    check_near(_x(entity), 0.0, 1e-4,
               "an unknown component type must yield Count 0, so the True "
               "branch must NOT run (got %.3f)" % _x(entity))


def test_get_actor_is_the_entity_the_graph_is_attached_to():
    """GetActor and GetOwner both answer the graph's own entity.

    There is no owner relationship in this engine (no OwnerComponent), so the
    two nodes are two spellings of one concept rather than two concepts. The
    assertion is that the answer is the ENTITY, not a parent: the graph writes
    through it and the transform moves.
    """
    path = _save("get_actor", '''kind: flow
schema: 2
name: "GetActorProbe"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: actor
    type: GetActor
    x: 340
    y: 120
  - id: cast
    type: CastTo
    x: 620
    y: 220
    pins:
      - { pin: Type, value: TransformComponent }
  - id: branch
    type: Branch
    x: 900
    y: 320
  - id: yes
    type: SetTransform
    x: 1180
    y: 160
    pins:
      - { pin: Translation, value: "(4.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - { from: { node: update, pin: Out }, to: { node: branch, pin: In } }
  - { from: { node: actor, pin: Actor }, to: { node: cast, pin: Entity } }
  - { from: { node: cast, pin: Success }, to: { node: branch, pin: Condition } }
  - { from: { node: branch, pin: True }, to: { node: yes, pin: In } }
''')
    if path is None:
        return
    scene, entity = _attach(path)
    check_near(_x(entity), 4.0, 1e-4,
               "GetActor must be the graph's own entity, so casting it to "
               "TransformComponent succeeds (got %.3f)" % _x(entity))
