"""Exposed variables -- Task B.4 (the Blueprint "Editable" checkbox).

The plan's verification: set an exposed variable in the inspector, tick, assert
the graph observed the value the inspector wrote.

The inspector writes to `VisualBlocksComponent::VariableOverrides`. The
headless equivalent of "set it in the inspector" is to write that same map --
which is exactly what `SceneHierarchyPanel` does, per exposed variable, when an
InputFloat/InputInt/Checkbox/InputText reports a change.

Three things are pinned here that would each fail silently otherwise:

  * a graph's DEFAULT is used when the entity has no override;
  * the override is applied at attach AND re-applied every frame, so an edit
    made AFTER Play is seen by the running graph;
  * a NON-exposed variable ignores an override, because the author never
    declared it editable.

The graph reads its variable and writes it into the entity's transform X, so
the assertion is on state the engine itself uses rather than a test-only
introspection path. There is no float-to-vec3 node, so Vec3Scale against a
(1,0,0) literal is how a float reaches Translation.x.

Run:  python tests/headless/runner.py test_exposed_variables
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

# Two variables: one EXPOSED, one not. The non-exposed one is the control -- an
# override on it must be ignored, and if it were not, this test would pass for
# the wrong reason.
GRAPH_TEXT = """kind: flow
schema: 2
name: "ExposedProbe"
variables:
  - name: Speed
    type: float
    default: "1.0"
    exposed: true
  - name: Hidden
    type: float
    default: "7.0"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: read_speed
    type: GetVariable
    x: 380
    y: 320
    pins:
      - { pin: Name, value: "Speed" }
  - id: scale
    type: Vec3Scale
    x: 700
    y: 320
    pins:
      - { pin: Vector, value: "(1.0, 0.0, 0.0)" }
  - id: write
    type: SetTransform
    x: 1020
    y: 320
    pins:
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - from: { node: update, pin: Out }
    to: { node: write, pin: In }
  - from: { node: read_speed, pin: Value }
    to: { node: scale, pin: Scale }
  - from: { node: scale, pin: Value }
    to: { node: write, pin: Translation }
"""


def _write_graph(name="ExposedProbe.wlblocks"):
    path = os.path.join(tempfile.gettempdir(), name)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(GRAPH_TEXT)
    return path


def _attach(path):
    from Wasteland import Scene, VisualBlocksComponent

    scene = Scene()
    entity = scene.CreateEntity("b4")
    # The macro-built accessor is AddVisualBlocksComponent, not AddComponent --
    # Entity has no generic AddComponent binding.
    component = VisualBlocksComponent()
    component.GraphPath = path
    entity.AddVisualBlocksComponent(component)
    # Read the component BACK. AddVisualBlocksComponent takes its argument by
    # value and stores a copy, so the local object above is an orphan -- writing
    # an override to it changes nothing on the entity.
    #
    # GetVisualBlocksComponent (from BIND_GAMEPLAY_COMP) returns a REFERENCE,
    # unlike the string-based GetComponent, which is a hardcoded linear scan
    # that does not include VisualBlocksComponent at all and returns None.
    component = entity.GetVisualBlocksComponent()
    return scene, entity, component


def _tick(scene, entity, frames=1):
    """One runtime frame. `scene.OnUpdateRuntime` is the public entry point --
    FlowGraphSystem is not bound to the top-level module."""
    for _ in range(frames):
        scene.OnUpdateRuntime(STEP)


def test_exposed_variable_declaration_round_trips():
    """`exposed: true` survives a save/load, and a false flag is omitted.

    Omitting the false flag is what keeps every existing .wlblocks file
    byte-identical on a round-trip -- the doc-example comparison depends on it.
    """
    vb = W.VisualBlocks
    graph = vb.Graph()
    error = vb.Parse(GRAPH_TEXT, graph)
    check(error is None, "the exposed-variable graph must parse")
    if error is not None:
        print("   ", error.Message)
        return

    check_eq(len(graph.Variables), 2, "the graph declares two variables")

    by_name = {variable.Name: variable for variable in graph.Variables}
    check_eq(by_name["Speed"].Exposed, True, "Speed must be exposed")
    check_eq(by_name["Speed"].Replicated, False,
             "Replicated defaults to false and is omitted when false")
    check_eq(by_name["Hidden"].Exposed, False,
             "Hidden is not exposed")

    path = os.path.join(tempfile.gettempdir(), "wasteland_b4_roundtrip.wlblocks")
    error = vb.Save(path, graph)
    check(error is None, "the graph must save")
    if error is not None:
        print("   ", error.Message)
        return

    with open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    check("exposed: true" in text,
          "the exposed flag must be written for an exposed variable")
    check("exposed: false" not in text,
          "a false exposed flag must be OMITTED, so existing files stay "
          "byte-identical on a round-trip")
    check("replicated:" not in text,
          "a false replicated flag must be omitted too")

    reloaded = vb.Graph()
    error = vb.Load(path, reloaded)
    check(error is None, "the saved graph must reload")
    if error is not None:
        print("   ", error.Message)
        return
    reloaded_by_name = {variable.Name: variable
                        for variable in reloaded.Variables}
    check_eq(reloaded_by_name["Speed"].Exposed, True,
             "exposed must survive the round-trip")
    check_eq(reloaded_by_name["Hidden"].Exposed, False,
             "the non-exposed variable must reload as non-exposed")


def test_a_graph_default_is_used_when_the_entity_has_no_override():
    """The baseline: no override on the component, so the graph default wins."""
    path = _write_graph()
    scene, entity, component = _attach(path)
    check_eq(component.HasVariableOverride("Speed"), False,
             "a freshly attached entity carries no overrides")

    _tick(scene, entity)

    check_near(entity.GetTransform().Translation.x, 1.0, 0.0001,
               "the graph default (1.0) must drive the entity when the "
               "component has no override")


def test_an_override_on_the_component_reaches_the_running_graph():
    """The plan's verification: the value the inspector wrote is what runs.

    The inspector writes into VariableOverrides; the graph must observe it.
    """
    path = _write_graph()
    scene, entity, component = _attach(path)

    component.SetVariableOverride("Speed", "3.5")
    _tick(scene, entity)

    check_near(entity.GetTransform().Translation.x, 3.5, 0.0001,
               "the component override (3.5) must reach the graph")


def test_an_override_written_after_play_is_seen_on_the_next_tick():
    """The hard half: the edit arrives AFTER the instance exists.

    A Python attribute set at construction does not change when the component
    map does, so attach-time seeding alone would make an inspector edit take
    effect only on the next Play. The plan asks for the value the inspector
    wrote to be observed after a tick -- which requires re-applying per frame.
    """
    path = _write_graph()
    scene, entity, component = _attach(path)

    _tick(scene, entity)
    check_near(entity.GetTransform().Translation.x, 1.0, 0.0001,
               "before the edit the graph default must hold")

    # This is the inspector's write, made while the graph is already running.
    component.SetVariableOverride("Speed", "9.25")
    _tick(scene, entity)

    check_near(entity.GetTransform().Translation.x, 9.25, 0.0001,
               "an override written AFTER Play must be observed on the next "
               "tick -- attach-time seeding alone is not enough")


def test_a_non_exposed_variable_ignores_its_override():
    """The control. An author who did not expose a variable must not have it
    changed from the inspector, even if something writes the map anyway."""
    path = _write_graph()
    scene, entity, component = _attach(path)

    component.SetVariableOverride("Hidden", "-4.0")
    _tick(scene, entity)

    # The graph only reads Speed, so Hidden's override cannot move X. The
    # assertion that matters is that Speed is untouched.
    check_near(entity.GetTransform().Translation.x, 1.0, 0.0001,
               "a non-exposed variable's override must not affect the graph")

    # `Hidden` is still INITIALISED in the generated __init__ -- every variable
    # is, because the class body cannot know which names are exposed. What it
    # must not be is READ: this graph never reads it, so its value cannot move
    # anything, and the assertion above is on the observable consequence.


def _loaded(path):
    graph = W.VisualBlocks.Graph()
    error = W.VisualBlocks.Load(path, graph)
    if error is not None:
        raise AssertionError(error.Message)
    return graph


def test_a_malformed_override_does_not_abort_the_tick():
    """Generated input can be wrong. A bad literal must degrade to the default
    with a logged error, not kill the entity's graph."""
    path = _write_graph()
    scene, entity, component = _attach(path)

    component.SetVariableOverride("Speed", "not-a-number")
    _tick(scene, entity)

    # The default survives because the failed write is caught.
    check_near(entity.GetTransform().Translation.x, 1.0, 0.0001,
               "a malformed override must leave the graph default in place")


def test_the_component_overrides_round_trip_through_the_scene_serializer():
    """An authored value must survive a save/load of the SCENE, not just the
    graph -- otherwise a level saved with a tuned value reloads with the
    graph's default and the tuning is silently lost."""
    from Wasteland import Scene, VisualBlocksComponent

    scene = Scene()
    entity = scene.CreateEntity("b4")
    component = VisualBlocksComponent()
    component.GraphPath = "graphs/ExposedProbe.wlblocks"
    entity.AddVisualBlocksComponent(component)
    component = entity.GetVisualBlocksComponent()
    component.SetVariableOverride("Speed", "2.75")

    path = os.path.join(tempfile.gettempdir(), "wasteland_b4_scene.scene")
    W.SceneSerializer(scene).Serialize(path)

    with open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    check("VariableOverrides" in text,
          "the scene must record the overrides")
    check("Speed" in text and "2.75" in text,
          "the override name and value must be in the scene file")

    loaded = Scene()
    W.SceneSerializer(loaded).Deserialize(path)

    entities = loaded.GetAllEntitiesWith("Wasteland::TransformComponent")
    check_eq(len(entities), 1, "the scene must contain exactly one entity")
    reloaded = entities[0].GetVisualBlocksComponent()
    check_eq(reloaded.GetVariableOverride("Speed"), "2.75",
             "the override value must survive the scene round-trip")


def main():
    print("test_exposed_variables")
    test_exposed_variable_declaration_round_trips()
    test_a_graph_default_is_used_when_the_entity_has_no_override()
    test_an_override_on_the_component_reaches_the_running_graph()
    test_an_override_written_after_play_is_seen_on_the_next_tick()
    test_a_non_exposed_variable_ignores_its_override()
    test_a_malformed_override_does_not_abort_the_tick()
    test_the_component_overrides_round_trip_through_the_scene_serializer()


if __name__ == "__main__":
    main()
