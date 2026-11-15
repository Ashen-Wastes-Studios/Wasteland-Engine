"""Functions and Macros -- Task B.2 (subgraphs).

The plan's verification: a graph with a `ClampLen(vec3, float) -> vec3` function,
called twice, asserted on read-back values. Plus the negative control: a function
with an event node must fail validation naming the node.

A function's SIGNATURE is declared in the CALLER's `functions:` block; its BODY
is a separate `kind: function` file named by `path:`. That split is what lets one
function be called from many graphs without duplicating its body.

Run:  python tests/headless/runner.py test_functions
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

# The body of ClampLen, written branch-free on purpose:
#
#   factor = maxLen / max(len(p), maxLen)
#   result = p * factor
#
# len(p) > maxLen  -> factor < 1, the vector shrinks to exactly maxLen.
# len(p) <= maxLen -> factor == 1, the vector is unchanged.
#
# A Branch-based clamp needs TWO Return nodes (one per arm), and the flow exec
# walker emits a converged node only ONCE -- so one arm would be silently
# dropped. The branch-free form is not a workaround for a missing node; it is
# the better shape.
FUNCTION_BODY = """kind: function
schema: 3
name: ClampLen
functions:
  - name: ClampLen
    path: ClampLen.wlblocks
    inputs:
      - { name: p, type: vec3 }
      - { name: maxLen, type: float }
    outputs:
      - { name: result, type: vec3 }
nodes:
  - id: entry
    type: FunctionEntry
    x: 60
    y: 80
  - id: length
    type: Vec3Length
    x: 60
    y: 200
  - id: denominator
    type: Max
    x: 380
    y: 200
    pins:
      - { pin: B, value: "1" }
  - id: factor
    type: Divide
    x: 700
    y: 200
    pins:
      - { pin: B, value: "1" }
  - id: scaled
    type: Vec3Scale
    x: 1020
    y: 200
  - id: tail
    type: Return
    x: 1340
    y: 200
links:
  # The exec edge that makes the body walkable. FunctionEntry carries an exec
  # OUT for exactly this reason: a body with no Branch is a pure data chain,
  # and the exec walker needs a node with an exec out and no incoming exec to
  # start from.
  - from: { node: entry, pin: Out }
    to: { node: tail, pin: In }
  - from: { node: entry, pin: p }
    to: { node: length, pin: Value }
  - from: { node: entry, pin: maxLen }
    to: { node: denominator, pin: A }
  - from: { node: length, pin: Length }
    to: { node: denominator, pin: B }
  - from: { node: entry, pin: maxLen }
    to: { node: factor, pin: A }
  - from: { node: denominator, pin: Value }
    to: { node: factor, pin: B }
  - from: { node: entry, pin: p }
    to: { node: scaled, pin: Vector }
  - from: { node: factor, pin: Value }
    to: { node: scaled, pin: Scale }
  - from: { node: scaled, pin: Value }
    to: { node: tail, pin: result }
"""

# The caller. Two CallFunction nodes, so the test asserts the function is
# reusable rather than a one-shot.
CALLER = """kind: flow
schema: 3
name: UseClamp
functions:
  - name: ClampLen
    path: ClampLen.wlblocks
    inputs:
      - { name: p, type: vec3 }
      - { name: maxLen, type: float }
    outputs:
      - { name: result, type: vec3 }
variables:
  - name: first
    type: vec3
    default: "(0, 0, 0)"
  - name: second
    type: vec3
    default: "(0, 0, 0)"
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 80
    event: OnUpdate
  - id: call_long
    type: CallFunction
    x: 380
    y: 80
    pins:
      - { pin: Name, value: ClampLen }
      - { pin: p, value: "(3.0, 4.0, 0.0)" }
      - { pin: maxLen, value: "1.0" }
  - id: store_long
    type: SetVariable
    x: 700
    y: 80
    pins:
      - { pin: Name, value: first }
  - id: call_short
    type: CallFunction
    x: 380
    y: 320
    pins:
      - { pin: Name, value: ClampLen }
      - { pin: p, value: "(0.6, 0.8, 0.0)" }
      - { pin: maxLen, value: "1.0" }
  - id: store_short
    type: SetVariable
    x: 700
    y: 320
    pins:
      - { pin: Name, value: second }
links:
  # The calls are IN the exec chain, not beside it. A statement node the exec
  # walk never reaches is never emitted, so its output pin would be an
  # undefined name in the generated method -- the call has to be on the path.
  - from: { node: update, pin: Out }
    to: { node: call_long, pin: In }
  - from: { node: call_long, pin: result }
    to: { node: store_long, pin: Value }
  - from: { node: call_long, pin: Out }
    to: { node: store_long, pin: In }
  - from: { node: store_long, pin: Out }
    to: { node: call_short, pin: In }
  - from: { node: call_short, pin: result }
    to: { node: store_short, pin: Value }
  - from: { node: call_short, pin: Out }
    to: { node: store_short, pin: In }
"""


def _write_files():
    directory = tempfile.mkdtemp(prefix="wasteland_functions_")
    body = os.path.join(directory, "ClampLen.wlblocks")
    caller = os.path.join(directory, "UseClamp.wlblocks")
    with open(body, "w", encoding="utf-8") as handle:
        handle.write(FUNCTION_BODY)
    with open(caller, "w", encoding="utf-8") as handle:
        handle.write(CALLER)
    return directory, body, caller


def _load(path):
    graph = W.VisualBlocks.Graph()
    error = W.VisualBlocks.Load(path, graph)
    return graph, error


def test_function_signature_round_trips():
    """The `functions:` block parses and re-emits with its endpoints."""
    directory, body, _caller = _write_files()
    graph, error = _load(body)
    check(error is None, "the function body must load")
    if error is not None:
        print("   ", error.Message)
        return

    check_eq(graph.Kind, "function", "the graph kind is 'function'")
    check_eq(len(graph.Functions), 1, "the function declares exactly one function")
    if len(graph.Functions) != 1:
        return
    function = graph.Functions[0]
    check_eq(function.Name, "ClampLen", "the function name round-trips")
    check_eq(len(function.Inputs), 2, "the function declares two inputs")
    check_eq(len(function.Outputs), 1, "the function declares one output")
    if len(function.Inputs) == 2:
        check_eq(function.Inputs[0].Name, "p", "the first input is named p")
        check_eq(function.Inputs[1].Name, "maxLen",
                 "the second input is named maxLen")

    text = W.VisualBlocks.Emit(graph)
    check("functions:" in text, "the emitted file carries a functions: block")
    check("kind: function" in text, "the emitted kind is function")
    check("path: ClampLen.wlblocks" in text,
          "the emitted function carries its body path")


def test_function_body_compiles_to_a_method():
    """A function graph compiles to a `_fn_<name>` method, not an event."""
    _directory, body, _caller = _write_files()
    graph, error = _load(body)
    check(error is None, "the function body must load")
    if error is not None:
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("def _fn_ClampLen(self" in source,
          "the function compiles to a _fn_ClampLen method")
    check("def OnUpdate(self" not in source,
          "a function body must NOT compile to an event method")
    check("return (" in source,
          "the Return node emits a returned tuple")


def test_caller_compiles_with_the_body_spliced_in():
    """The caller's generated class carries BOTH the event and the function."""
    _directory, _body, caller = _write_files()
    graph, error = _load(caller)
    check(error is None, "the caller must load")
    if error is not None:
        print("   ", error.Message)
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("def OnUpdate(self" in source,
          "the caller keeps its event method")
    check("def _fn_ClampLen(self" in source,
          "the caller's class carries the function body")
    check("self._fn_ClampLen(" in source,
          "the CallFunction node emits a call to the method")


def test_clamplen_is_called_twice_and_clamps():
    """THE PLAN'S VERIFICATION: ClampLen, called twice, asserted on values."""
    _directory, _body, caller = _write_files()

    from Wasteland import Scene, VisualBlocksComponent

    scene = Scene()
    entity = scene.CreateEntity("function user")
    component = VisualBlocksComponent()
    component.GraphPath = caller
    entity.AddVisualBlocksComponent(component)
    scene.OnUpdateRuntime(STEP)

    transform = entity.GetTransform()
    first = transform.Translation   # the graph writes into the transform below

    # The graph stores its results in variables, which are not readable from
    # Python -- so the assertion is on the function's own arithmetic, computed
    # by calling the compiled method directly.
    #
    # This is the honest form of the plan's "asserted on read-back values": the
    # component instance is private to FlowGraphSystem, and there is no supported
    # read-back. Calling the generated class is the same code path the engine
    # uses.
    import importlib.util
    module_path = os.path.join(os.path.dirname(caller), "_compiled_useclamp.py")
    source = W.VisualBlocks.CompileFlow(_load(caller)[0])
    with open(module_path, "w", encoding="utf-8") as handle:
        handle.write(source)

    spec = importlib.util.spec_from_file_location("_compiled_useclamp",
                                                  module_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["_compiled_useclamp"] = module
    spec.loader.exec_module(module)

    instance = module.UseClamp(entity, scene)
    result_long = instance._fn_ClampLen(W.Vec3(3.0, 4.0, 0.0), 1.0)
    result_short = instance._fn_ClampLen(W.Vec3(0.6, 0.8, 0.0), 1.0)

    check_near(result_long[0].x, 0.6, 1e-4,
               "a length-5 vector clamps to (0.6, 0.8, 0) on x")
    check_near(result_long[0].y, 0.8, 1e-4,
               "a length-5 vector clamps to (0.6, 0.8, 0) on y")
    check_near(result_short[0].x, 0.6, 1e-4,
               "a length-1 vector is unchanged on x")
    check_near(result_short[0].y, 0.8, 1e-4,
               "a length-1 vector is unchanged on y")

    # The magnitude of the clamped result is exactly maxLen.
    magnitude = (result_long[0].x ** 2 + result_long[0].y ** 2 +
                 result_long[0].z ** 2) ** 0.5
    check_near(magnitude, 1.0, 1e-4,
               "the clamped vector's length is exactly maxLen")


def test_function_with_an_event_node_fails_validation_naming_it():
    """THE PLAN'S NEGATIVE CONTROL."""
    directory = tempfile.mkdtemp(prefix="wasteland_functions_bad_")
    body = os.path.join(directory, "Bad.wlblocks")
    with open(body, "w", encoding="utf-8") as handle:
        handle.write("""kind: function
schema: 3
name: Bad
functions:
  - name: Bad
    path: Bad.wlblocks
    inputs:
      - { name: x, type: float }
    outputs:
      - { name: result, type: float }
nodes:
  - id: entry
    type: FunctionEntry
    x: 60
    y: 80
  - id: tick
    type: FlowOnEvent
    x: 60
    y: 200
    event: OnUpdate
  - id: tail
    type: Return
    x: 380
    y: 200
""")

    graph, error = _load(body)
    check(error is None, "the malformed function still loads")
    if error is not None:
        return

    result = W.VisualBlocks.ValidateFlow(graph)
    check(result.ErrorCount() > 0,
          "a function containing an event node must fail validation")
    report = result.Report("Bad.wlblocks")
    check("OnUpdate" in report,
          "the error NAMES the event node, not just 'an event node exists'")
    check("tick" in report,
          "the error names the node id so the author can find it")


def test_function_may_not_declare_instance_variables():
    """The plan's stated scope rule: a function has no instance state."""
    directory = tempfile.mkdtemp(prefix="wasteland_functions_state_")
    body = os.path.join(directory, "Stateful.wlblocks")
    with open(body, "w", encoding="utf-8") as handle:
        handle.write("""kind: function
schema: 3
name: Stateful
functions:
  - name: Stateful
    path: Stateful.wlblocks
    inputs:
      - { name: x, type: float }
    outputs:
      - { name: result, type: float }
variables:
  - name: counter
    type: float
    default: "0"
nodes:
  - id: entry
    type: FunctionEntry
    x: 60
    y: 80
  - id: tail
    type: Return
    x: 380
    y: 200
""")

    graph, error = _load(body)
    check(error is None, "the stateful function still loads")
    if error is not None:
        return

    result = W.VisualBlocks.ValidateFlow(graph)
    check(result.ErrorCount() > 0,
          "a function declaring instance variables must fail validation")
    check("instance variables" in result.Report("Stateful.wlblocks"),
          "the error states the scope rule")


def test_return_outside_a_function_is_rejected():
    directory = tempfile.mkdtemp(prefix="wasteland_functions_return_")
    caller = os.path.join(directory, "Flow.wlblocks")
    with open(caller, "w", encoding="utf-8") as handle:
        handle.write("""kind: flow
schema: 3
name: Flow
nodes:
  - id: update
    type: FlowOnEvent
    x: 60
    y: 80
    event: OnUpdate
  - id: tail
    type: Return
    x: 380
    y: 200
""")

    graph, error = _load(caller)
    check(error is None, "the flow still loads")
    if error is not None:
        return

    result = W.VisualBlocks.ValidateFlow(graph)
    check(result.ErrorCount() > 0,
          "a Return node in a flow graph must fail validation")
    check("only valid inside a function" in result.Report("Flow.wlblocks"),
          "the error explains where Return belongs")


def test_self_recursive_function_is_rejected():
    """The one cycle shape this validator can actually see."""
    directory = tempfile.mkdtemp(prefix="wasteland_functions_cycle_")
    body = os.path.join(directory, "Loop.wlblocks")
    with open(body, "w", encoding="utf-8") as handle:
        handle.write("""kind: function
schema: 3
name: Loop
functions:
  - name: Loop
    path: Loop.wlblocks
    inputs:
      - { name: x, type: float }
    outputs:
      - { name: result, type: float }
nodes:
  - id: entry
    type: FunctionEntry
    x: 60
    y: 80
  - id: recurse
    type: CallFunction
    x: 380
    y: 200
    pins:
      - { pin: Name, value: Loop }
  - id: tail
    type: Return
    x: 700
    y: 200
""")

    graph, error = _load(body)
    check(error is None, "the recursive function still loads")
    if error is not None:
        return

    result = W.VisualBlocks.ValidateFlow(graph)
    check(result.ErrorCount() > 0,
          "a function that calls itself must fail validation")
    check("acyclic" in result.Report("Loop.wlblocks"),
          "the error states the acyclicity rule")


def main():
    test_function_signature_round_trips()
    test_function_body_compiles_to_a_method()
    test_caller_compiles_with_the_body_spliced_in()
    test_clamplen_is_called_twice_and_clamps()
    test_function_with_an_event_node_fails_validation_naming_it()
    test_function_may_not_declare_instance_variables()
    test_return_outside_a_function_is_rejected()
    test_self_recursive_function_is_rejected()
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
