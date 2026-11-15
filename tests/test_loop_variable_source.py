"""Task B.6: the ForEach loop variable must be REACHABLE from the body.

The blocker
-----------
A graph could iterate a count and could index a list, but could not do both:
`ForEach` emits `for i in range(<Count>):` and `GetEntityAtIndex` needs an
`Index` pin fed by a link or a literal -- and no node outputs the loop
variable. That is exactly what a nearest-target scan is, which is why the
chaser in B.6 could not be authored at all.

The fix
-------
`ForEach` publishes the loop variable into the graph variable of the SAME
NAME at the top of the body (`self.<i> = <i>`), so `GetVariable` can read it
and feed an Int pin. The graph must DECLARE the variable; an undeclared name
gets no publish line, so an authoring error surfaces instead of silently
setting an unused attribute.

These tests assert on the emitted SOURCE, not on C++ text.
"""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..")))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

MODULE_DIR = os.path.join(
    ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor")
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
name: LoopRead
variables:
  - name: i
    type: int
    default: "0"
  - name: total
    type: float
    default: "0"
nodes:
  - id: update
    type: FlowOnEvent
    event: OnUpdate
  - id: loop
    type: ForEach
    pins:
      - { pin: Count, value: "4" }
      - { pin: IndexVar, value: "i" }
  - id: read
    type: GetVariable
    pins:
      - { pin: Name, value: "i" }
  - id: add
    type: Add
    pins:
      - { pin: A, value: "0" }
  - id: store
    type: SetVariable
    pins:
      - { pin: Name, value: "total" }
links:
  # GetVariable AND Add are EXPRESSION nodes -- neither has exec pins. Only
  # the exec chain (loop.Body -> store.In) is walked; the data flows in
  # sideways: read.Value -> add.A, add.Value -> store.Value.
  - { from: { node: update, pin: Out }, to: { node: loop, pin: In } }
  - { from: { node: loop, pin: Body }, to: { node: store, pin: In } }
  - { from: { node: read, pin: Value }, to: { node: add, pin: A } }
  - { from: { node: add, pin: Value }, to: { node: store, pin: Value } }
"""

UNDECLARED = """kind: flow
schema: 3
name: LoopUndeclared
nodes:
  - id: update
    type: FlowOnEvent
    event: OnUpdate
  - id: loop
    type: ForEach
    pins:
      - { pin: Count, value: "4" }
      - { pin: IndexVar, value: "i" }
  - id: use
    type: Log
    pins:
      - { pin: Message, value: "hi" }
links:
  - { from: { node: update, pin: Out }, to: { node: loop, pin: In } }
  - { from: { node: loop, pin: Body }, to: { node: use, pin: In } }
"""


def _load(text, name):
    directory = tempfile.mkdtemp(prefix="wasteland_loopvar_")
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    graph = W.VisualBlocks.Graph()
    error = W.VisualBlocks.Load(path, graph)
    return graph, error


def test_the_loop_variable_is_published_into_a_graph_variable():
    """THE FIX: `self.i = i` at the top of the body, so GetVariable can read it."""
    graph, error = _load(GRAPH, "LoopRead.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        print("   ", error.Message)
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("for i in range(4):" in source,
          "the loop header is unchanged")
    check("self.i = i" in source,
          "the loop variable must be published into the declared graph "
          "variable, or no node in the body can reach it")
    # The publish sits inside the body, so it comes AFTER the header -- that
    # ordering is what makes Python treat it as part of the loop.
    check(source.index("self.i = i") > source.index("for i in range(4):"),
          "the publish must be inside the body, after the header")
    check("self.total" in source,
          "the body reads back through the graph variable")


def test_an_undeclared_loop_variable_is_not_published():
    """Publishing an undeclared name would hide an authoring error."""
    graph, error = _load(UNDECLARED, "LoopUndeclared.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        print("   ", error.Message)
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("for i in range(4):" in source, "the loop still compiles")
    check("self.i = i" not in source,
          "an UNDECLARED loop variable must not be published: the graph has "
          "no such variable, and writing one silently would hide the "
          "authoring error instead of surfacing it")


def test_get_variable_is_typed_any_not_float():
    """A declared Int variable must be able to feed an Int pin.

    `GetVariable` was Float-typed, so an Int loop counter could not feed
    `GetEntityAtIndex.Index` -- the read existed but was unusable for the one
    thing the publish was added for.
    """
    definitions = {n.TypeId: n for n in W.VisualBlocks.RegisteredNodes()}
    node = definitions.get("GetVariable")
    check(node is not None, "GetVariable must be registered")
    if node is None:
        return
    value = next((p for p in node.Pins if p.Id == "Value"), None)
    check(value is not None, "GetVariable must have a Value output")
    if value is None:
        return
    check_eq(str(value.Type), "PinType.Any",
             "GetVariable's output must be Any: it returns whatever the "
             "author declared, not always a float")


LIST_INDEX_GRAPH = """kind: flow
schema: 3
name: LoopIndex
variables:
  - name: i
    type: int
    default: "0"
  - name: target
    type: entity
    default: "None"
nodes:
  - id: update
    type: FlowOnEvent
    event: OnUpdate
  - id: loop
    type: ForEach
    pins:
      - { pin: Count, value: "4" }
      - { pin: IndexVar, value: "i" }
  - id: read
    type: GetVariable
    pins:
      - { pin: Name, value: "i" }
  - id: at
    type: GetEntityAtIndex
    pins:
      - { pin: List, value: "[]" }
  - id: keep
    type: SetVariable
    pins:
      - { pin: Name, value: "target" }
links:
  - { from: { node: update, pin: Out }, to: { node: loop, pin: In } }
  - { from: { node: loop, pin: Body }, to: { node: keep, pin: In } }
  - { from: { node: read, pin: Value }, to: { node: at, pin: Index } }
  - { from: { node: at, pin: Entity }, to: { node: keep, pin: Value } }
"""


def test_the_published_index_reaches_a_list_index():
    """THE END-TO-END CONTRACT: the loop index feeds GetEntityAtIndex.

    This is the composition B.6 needed. The graph reads `self.i` and uses it
    as the Index of `GetEntityAtIndex`, which is the shape of every
    nearest-target scan. `GetEntityAtIndex` is an EXPRESSION node, so its
    output is wired into a SetVariable to make it observable -- an expression
    whose result nothing consumes is never emitted.
    """
    graph, error = _load(LIST_INDEX_GRAPH, "LoopIndex.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        print("   ", error.Message)
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("self.i = i" in source, "the index is published")
    # The node's TypeId never appears in generated Python -- the emitter
    # inlines the subscript expression. What proves the node ran is the
    # bounds-guarded index, which is its signature output.
    check("if 0 <= (self._vb_read_Value) < len(" in source,
          "the indexed lookup compiles to its bounds-guarded subscript")
    check("self._vb_read_Value = self.i" in source,
          "the index pin must be fed by the published variable")
    check("self.target = self._vb_at_Entity" in source,
          "the lookup result must reach a graph variable")


AFTER_LOOP = """kind: flow
schema: 3
name: AfterLoop
variables:
  - name: i
    type: int
    default: "0"
  - name: seen
    type: int
    default: "0"
nodes:
  - id: update
    type: FlowOnEvent
    event: OnUpdate
  - id: loop
    type: ForEach
    pins:
      - { pin: Count, value: "2" }
      - { pin: IndexVar, value: "i" }
  - id: getI
    type: GetVariable
    pins:
      - { pin: Name, value: "i" }
  - id: bump
    type: Add
    pins:
      - { pin: B, value: "1" }
  - id: store
    type: SetVariable
    pins:
      - { pin: Name, value: "seen" }
  - id: after
    type: SetVariable
    pins:
      - { pin: Name, value: "seen" }
      - { pin: Value, value: "99" }
links:
  - { from: { node: update, pin: Out }, to: { node: loop, pin: In } }
  - { from: { node: loop, pin: Body }, to: { node: store, pin: In } }
  - { from: { node: getI, pin: Value }, to: { node: bump, pin: A } }
  - { from: { node: bump, pin: Value }, to: { node: store, pin: Value } }
  # THE CONTINUATION: the chain after the loop, hung off Done.
  - { from: { node: loop, pin: Done }, to: { node: after, pin: In } }
"""


def test_the_exec_chain_continues_after_the_loop():
    """THE BUG THIS FOUND: `ForEach.Done` was declared and linkable but never
    EMITTED.

    The compiler's marker resolver substitutes a body wherever the emitter
    wrote a marker, and `ForEach` wrote one for "Body" only. So an exec chain
    hung off `Done` was silently dropped from the generated method -- the graph
    validated, compiled, and then stopped dead at the loop. Task B.6 hit this:
    the chaser's scan compiled and everything after it vanished.
    """
    graph, error = _load(AFTER_LOOP, "AfterLoop.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        print("   ", error.Message)
        return

    source = W.VisualBlocks.CompileFlow(graph)
    check("for i in range(2):" in source, "the loop compiles")
    check("self.seen = 99" in source,
          "the continuation AFTER the loop must be emitted, or the chain "
          "silently stops at the loop")
    loop_at = source.index("for i in range(2):")
    after_at = source.index("self.seen = 99")
    check(after_at > loop_at,
          "the continuation must come after the loop header")
    # It must be at the LOOP's indent, not inside the body -- otherwise it
    # runs twice.
    loop_line = source[:loop_at].count("\n")
    after_line = source[:after_at].count("\n")
    loop_text = source.split("\n")[loop_line]
    after_text = source.split("\n")[after_line]
    loop_indent = len(loop_text) - len(loop_text.lstrip("\t"))
    after_indent = len(after_text) - len(after_text.lstrip("\t"))
    check(after_indent == loop_indent,
          "the continuation must be at the loop's OWN indent, not inside the "
          "body (inside would run it once per iteration)")


def test_the_published_index_is_read_every_iteration():
    """The publish must be INSIDE the loop, not before it.

    A publish before the `for` would read the loop's final value for every
    iteration -- the classic off-by-one that makes a scan see only the last
    candidate.
    """
    graph, error = _load(GRAPH, "LoopRead.wlblocks")
    check(error is None, "the graph must load")
    if error is not None:
        return
    source = W.VisualBlocks.CompileFlow(graph)
    loop_at = source.index("for i in range(4):")
    publish_at = source.index("self.i = i")
    check(publish_at > loop_at,
          "the publish must be INSIDE the loop body, after the header")
    # And it must be indented deeper than the `for`, which is what makes
    # Python treat it as part of the body.
    loop_line = source[:loop_at].count("\n")
    publish_line = source[:publish_at].count("\n")
    loop_indent = len(source.split("\n")[loop_line]) - len(source.split("\n")[loop_line].lstrip("\t"))
    publish_indent = len(source.split("\n")[publish_line]) - len(source.split("\n")[publish_line].lstrip("\t"))
    check(publish_indent > loop_indent,
          "the publish must be indented INSIDE the loop, not at the loop's "
          "own level")


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
    # `N/M checks passed` is the format tools/run_all_suites.py parses
    # (CHECK_SUMMARY_RE). A bare `N checks passed` is read as UNCOUNTED.
    print(f"{PASSED}/{PASSED + len(FAILED)} checks passed")
    sys.exit(1 if failed else 0)
