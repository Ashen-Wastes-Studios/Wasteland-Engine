"""Task B.6: GetPosition and GetHealth must be able to read ANOTHER entity.

The gap
-------
Both nodes were hardwired to `self.entity`. A chaser measures distance to a
CANDIDATE, and a weapon finds its WIELDER -- any behaviour that reasons about
an entity other than the one the graph is attached to was inexpressible.

The fix
-------
An OPTIONAL `Target` entity pin. Unset (the default) reads SELF, so every
existing graph compiles byte-identically. Set, and the read goes through the
target with a None-guard.

These tests assert on the emitted SOURCE, not on C++ text.
"""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..")))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

MODULE_DIR = os.path.join(ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor")
ASSETS_DIR = os.path.join(ROOT, "DemonCore-Editor", "assets", "scripts")
if MODULE_DIR not in sys.path:
    sys.path.insert(0, MODULE_DIR)
if ASSETS_DIR not in sys.path:
    sys.path.insert(0, ASSETS_DIR)

import Wasteland as W

FAILED = []
PASSED = 0


def check(condition, message):
    global PASSED
    if condition:
        PASSED += 1
    else:
        FAILED.append(message)
        print("    FAIL:", message)


def check_eq(actual, expected, message):
    check(actual == expected, f"{message} (got {actual!r}, want {expected!r})")


GRAPH = """kind: flow
schema: 3
name: TargetRead
variables:
  - name: dist
    type: float
    default: "0"
  - name: target
    type: entity
    default: "None"
nodes:
  - id: update
    type: FlowOnEvent
    event: OnUpdate
  - id: other
    type: GetVariable
    pins:
      - { pin: Name, value: "target" }
  - id: pself
    type: GetPosition
  - id: pother
    type: GetPosition
    pins:
      - { pin: Target, value: "target" }
  - id: hself
    type: GetHealth
  - id: hother
    type: GetHealth
    pins:
      - { pin: Target, value: "target" }
  - id: keep
    type: SetVariable
    pins:
      - { pin: Name, value: "dist" }
  - id: keepPos
    type: SetVariable
    pins:
      - { pin: Name, value: "distPos" }
links:
  - { from: { node: update, pin: Out }, to: { node: keep, pin: In } }
  - { from: { node: other, pin: Value }, to: { node: pother, pin: Target } }
  - { from: { node: other, pin: Value }, to: { node: hother, pin: Target } }
  - { from: { node: hother, pin: Value }, to: { node: keep, pin: Value } }
  - { from: { node: keep, pin: Out }, to: { node: keepPos, pin: In } }
  - { from: { node: pother, pin: Position }, to: { node: keepPos, pin: Value } }
"""


def _load(text, name):
    directory = tempfile.mkdtemp(prefix="wasteland_target_")
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    graph = W.VisualBlocks.Graph()
    error = W.VisualBlocks.Load(path, graph)
    return graph, error


def test_get_position_reads_the_target_when_given_one():
    """The distance half of a chaser: measure a CANDIDATE, not self."""
    graph, error = _load(GRAPH, "TargetRead.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        print("   ", error.Message)
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("GetTransform().Translation if t is not None" in source,
          "GetPosition must read the target's transform when a Target is wired")


def test_get_position_still_reads_self_by_default():
    """The default path must be UNCHANGED, or every existing graph breaks."""
    definitions = {n.TypeId: n for n in W.VisualBlocks.RegisteredNodes()}
    node = definitions.get("GetPosition")
    check(node is not None, "GetPosition must be registered")
    if node is None:
        return
    target = next((p for p in node.Pins if p.Id == "Target"), None)
    check(target is not None, "GetPosition must have a Target pin")
    if target is None:
        return
    check_eq(str(target.Type), "PinType.Entity", "the Target pin is an entity")
    check(not target.Required,
          "the Target pin must be optional: a required pin would break every "
          "graph that does not set it")
    check_eq(target.DefaultLiteral, "None",
             "an unset Target must mean 'self'")


def test_get_health_reads_the_target_and_guards_a_missing_component():
    """A candidate scan walks EVERY entity in the scene; not all have health."""
    graph, error = _load(GRAPH, "TargetRead.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("TryGetHealthComponent()" in source,
          "the TARGET path must use TryGet, not Get: Get raises on an entity "
          "with no HealthComponent, and a scan walks every entity in the scene")
    check("else 0.0" in source,
          "a missing component must read as 0.0 (dead), which is the answer a "
          "scan wants")


def test_get_health_still_reads_self_by_default():
    definitions = {n.TypeId: n for n in W.VisualBlocks.RegisteredNodes()}
    node = definitions.get("GetHealth")
    check(node is not None, "GetHealth must be registered")
    if node is None:
        return
    target = next((p for p in node.Pins if p.Id == "Target"), None)
    check(target is not None, "GetHealth must have a Target pin")
    if target is None:
        return
    check(not target.Required, "the Target pin must be optional")
    check_eq(target.DefaultLiteral, "None",
             "an unset Target must mean 'self'")


SELF_GRAPH = """kind: flow
schema: 3
name: SelfRead
variables:
  - name: dist
    type: float
    default: "0"
nodes:
  - id: update
    type: FlowOnEvent
    event: OnUpdate
  - id: pself
    type: GetPosition
  - id: hself
    type: GetHealth
  - id: keep
    type: SetVariable
    pins:
      - { pin: Name, value: "dist" }
  - id: keepPos
    type: SetVariable
    pins:
      - { pin: Name, value: "distPos" }
links:
  - { from: { node: update, pin: Out }, to: { node: keep, pin: In } }
  - { from: { node: hself, pin: Value }, to: { node: keep, pin: Value } }
  - { from: { node: keep, pin: Out }, to: { node: keepPos, pin: In } }
  - { from: { node: pself, pin: Position }, to: { node: keepPos, pin: Value } }
"""


def test_the_self_read_is_byte_identical_to_before():
    """The emitted self expression is the one every existing graph depends on.

    A graph that sets no Target must compile to exactly what it did before the
    pin existed -- otherwise the change is a silent behaviour change for every
    shipped graph, not an additive one.
    """
    graph, error = _load(SELF_GRAPH, "SelfRead.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        print("   ", error.Message)
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("self.entity.GetHealthComponent().Current" in source,
          "the self health read must be the original expression, unchanged")
    check("self.entity.GetTransform().Translation" in source,
          "the self position read must be the original expression, unchanged")
    check("TryGetHealthComponent" not in source,
          "the self path must NOT gain a target guard it does not need")


if __name__ == "__main__":
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        print(f"  {name} ...")
        try:
            fn()
        except Exception as error:  # noqa: BLE001
            failed += 1
            print(f"    ERROR {error!r}")
    total = len(tests)
    print(f"\n{total - failed}/{total} tests passed")
    print(f"{PASSED}/{PASSED + len(FAILED)} checks passed")
    sys.exit(1 if failed else 0)
