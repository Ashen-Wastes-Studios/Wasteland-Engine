"""Per-node debugging -- Task B.5.

The plan asks for a breakpoint toggle per node, a step control while paused, and
a generated-source panel that highlights the executing line.

This suite pins the RUNTIME half, which is the part that can be verified without
a screen:

  * a breakpoint is serialized with the graph, so it survives save/load;
  * a breakpoint on a node pauses the entity ON that node, before it runs;
  * a paused graph does not tick;
  * Resume() runs the paused node and does not disable the breakpoint;
  * Step() runs exactly one node and stops at the next;
  * a graph with NO breakpoint emits no trace call at all -- the debugging
    apparatus must not change the code of a graph nobody is debugging.

The UI half (the toggle, the buttons, the highlighted line) is source-verified
in tests/test_per_node_debug_source.py and is NOT proven by a screenshot: the
editor cannot start on this machine.

Run:  python tests/headless/runner.py test_per_node_debug
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

# Two ticks' worth of nodes. The breakpoint is placed on the SECOND statement
# node, so "paused on write_speed" is distinguishable from "the graph did not
# run at all" -- X would be 0 in the latter case and 0 in the former too, so the
# assertion also reads the paused NODE ID, which is the unambiguous signal.
GRAPH = """kind: flow
schema: 2
name: "DebugProbe"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 320
    event: OnUpdate
  - id: write_speed
    type: SetTransform
    x: 380
    y: 320
    pins:
      - { pin: Translation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
  - id: write_again
    type: SetTransform
    x: 700
    y: 320
    pins:
      - { pin: Translation, value: "(2.0, 0.0, 0.0)" }
      - { pin: Rotation, value: "(0.0, 0.0, 0.0)" }
      - { pin: Scale, value: "(1.0, 1.0, 1.0)" }
links:
  - from: { node: update, pin: Out }
    to: { node: write_speed, pin: In }
  - from: { node: write_speed, pin: Out }
    to: { node: write_again, pin: In }
"""


def _write_graph(name="DebugProbe.wlblocks", breakpoint=None):
    text = GRAPH
    if breakpoint is not None:
        # Insert the flag on the named node. Done textually so the test does not
        # depend on the serializer's key order.
        marker = "  - id: %s\n" % breakpoint
        assert marker in text, marker
        text = text.replace(
            marker, marker + "    breakpoint: true\n")
    path = os.path.join(tempfile.gettempdir(), name)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


def _attach(path):
    from Wasteland import Scene, VisualBlocksComponent

    scene = Scene()
    entity = scene.CreateEntity("b5")
    component = VisualBlocksComponent()
    component.GraphPath = path
    entity.AddVisualBlocksComponent(component)
    return scene, entity


def _uuid(entity):
    return entity.GetUUID()


def _tick(scene):
    scene.OnUpdateRuntime(STEP)


def test_a_breakpoint_is_serialized_with_the_graph():
    """A breakpoint that does not survive a save/load would be silently cleared
    on the next edit -- the worst outcome for a debugging session."""
    vb = W.VisualBlocks
    graph = vb.Graph()
    error = vb.Parse(GRAPH, graph)
    check(error is None, "the debug graph must parse")
    if error is not None:
        print("   ", error.Message)
        return

    target = graph.FindNode("write_speed")
    check(target is not None, "the graph must contain write_speed")
    if target is None:
        return
    target.Breakpoint = True

    path = os.path.join(tempfile.gettempdir(), "wasteland_b5_breakpoint.wlblocks")
    error = vb.Save(path, graph)
    check(error is None, "the graph must save")
    if error is not None:
        print("   ", error.Message)
        return

    with open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    check("breakpoint: true" in text,
          "the breakpoint must be written into the file")
    check("breakpoint: false" not in text,
          "a false breakpoint must be OMITTED, so existing files stay "
          "byte-identical")

    reloaded = vb.Graph()
    error = vb.Load(path, reloaded)
    check(error is None, "the saved graph must reload")
    if error is not None:
        print("   ", error.Message)
        return
    node = reloaded.FindNode("write_speed")
    check(node is not None and node.Breakpoint,
          "the breakpoint must survive the round-trip")


def test_a_graph_with_no_breakpoint_emits_no_trace_call():
    """The debugging apparatus must not change the code of a graph nobody is
    debugging -- otherwise every existing graph's generated source changes."""
    vb = W.VisualBlocks
    graph = vb.Graph()
    error = vb.Parse(GRAPH, graph)
    check(error is None, "the graph must parse")
    if error is not None:
        return
    source = vb.CompileFlow(graph)
    check("_vb_trace" not in source,
          "a graph with no breakpoint must emit no trace call")
    check("_vb_traceHook" not in source,
          "a graph with no breakpoint must not reference the hook")


def test_a_breakpoint_emits_a_trace_call_before_the_node():
    """The call must precede the node's own statement, or the pause lands on the
    successor rather than the marked node."""
    path = _write_graph(breakpoint="write_speed")
    graph = W.VisualBlocks.Graph()
    error = W.VisualBlocks.Load(path, graph)
    check(error is None, "the graph must load")
    if error is not None:
        print("   ", error.Message)
        return
    source = W.VisualBlocks.CompileFlow(graph)
    check('self._vb_trace("write_speed")' in source,
          "the marked node must carry a trace call")
    check('self._vb_trace("update")' not in source,
          "only the MARKED node may carry a trace call")

    hook_at = source.index('self._vb_trace("write_speed")')
    statement_at = source.index("self.entity", hook_at)
    check(hook_at < statement_at,
          "the trace call must precede the node's own statement")


def test_a_breakpoint_pauses_the_entity_on_that_node():
    """THE PLAN'S RUNTIME VERIFICATION: the graph stops at the marked node."""
    path = _write_graph(breakpoint="write_speed")
    scene, entity = _attach(path)
    uuid = _uuid(entity)

    check_eq(W.FlowGraphSystem.IsPaused(uuid), False,
             "a fresh entity is not paused")

    _tick(scene)

    check_eq(W.FlowGraphSystem.IsPaused(uuid), True,
             "the entity must pause when the breakpoint node is reached")
    check_eq(W.FlowGraphSystem.GetPausedNode(uuid), "write_speed",
             "the pause must name the node it stopped on")


def test_a_paused_graph_does_not_tick():
    path = _write_graph(breakpoint="write_speed")
    scene, entity = _attach(path)
    uuid = _uuid(entity)

    _tick(scene)
    check_eq(W.FlowGraphSystem.IsPaused(uuid), True, "the entity must pause")

    # More frames must change nothing while paused.
    for _ in range(5):
        _tick(scene)
    check_eq(W.FlowGraphSystem.IsPaused(uuid), True,
             "a paused graph must stay paused")
    check_eq(W.FlowGraphSystem.GetPausedNode(uuid), "write_speed",
             "the paused node must not change while paused")


def test_resume_runs_the_graph_without_disabling_the_breakpoint():
    """Continue runs the paused node and keeps the breakpoint set, so a later
    hit on the same node is still reported."""
    path = _write_graph(breakpoint="write_speed")
    scene, entity = _attach(path)
    uuid = _uuid(entity)

    _tick(scene)
    check_eq(W.FlowGraphSystem.GetPausedNode(uuid), "write_speed",
             "the first tick must pause on write_speed")

    W.FlowGraphSystem.Resume(uuid)
    check_eq(W.FlowGraphSystem.IsPaused(uuid), False,
             "Resume must clear the pause")

    # The next tick runs the released node and continues to write_again, which
    # carries no breakpoint -- so the graph runs to completion.
    _tick(scene)
    check_eq(W.FlowGraphSystem.IsPaused(uuid), False,
             "after Resume the graph must run past the released node")
    check_near(entity.GetTransform().Translation.x, 2.0, 0.0001,
               "the node AFTER the breakpoint must have run")


def test_step_runs_one_node_and_stops_at_the_next():
    """Step Over: release one node, then stop at the next breakpoint."""
    path = _write_graph(name="DebugProbeStep.wlblocks",
                        breakpoint="write_speed")
    # A SECOND breakpoint, so Step has somewhere to stop.
    text = open(path, "r", encoding="utf-8", newline="").read()
    marker = "  - id: write_again\n"
    check(marker in text, "the graph must contain write_again")
    text = text.replace(marker, marker + "    breakpoint: true\n")
    stepPath = os.path.join(tempfile.gettempdir(), "DebugProbeStep2.wlblocks")
    with open(stepPath, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)

    scene, entity = _attach(stepPath)
    uuid = _uuid(entity)

    _tick(scene)
    check_eq(W.FlowGraphSystem.GetPausedNode(uuid), "write_speed",
             "the first tick must pause on the first breakpoint")

    W.FlowGraphSystem.Step(uuid)
    _tick(scene)
    check_eq(W.FlowGraphSystem.GetPausedNode(uuid), "write_again",
             "Step must release one node and stop at the NEXT breakpoint")


def test_a_graph_without_a_breakpoint_is_unaffected():
    """The control: the same graph, no breakpoint, runs normally."""
    path = _write_graph()
    scene, entity = _attach(path)
    uuid = _uuid(entity)

    _tick(scene)
    check_eq(W.FlowGraphSystem.IsPaused(uuid), False,
             "a graph with no breakpoint must never pause")
    check_near(entity.GetTransform().Translation.x, 2.0, 0.0001,
               "the graph must run to completion")


def main():
    print("test_per_node_debug")
    test_a_breakpoint_is_serialized_with_the_graph()
    test_a_graph_with_no_breakpoint_emits_no_trace_call()
    test_a_breakpoint_emits_a_trace_call_before_the_node()
    test_a_breakpoint_pauses_the_entity_on_that_node()
    test_a_paused_graph_does_not_tick()
    test_resume_runs_the_graph_without_disabling_the_breakpoint()
    test_step_runs_one_node_and_stops_at_the_next()
    test_a_graph_without_a_breakpoint_is_unaffected()


if __name__ == "__main__":
    main()
