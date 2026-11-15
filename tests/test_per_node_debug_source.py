"""Source-side contract for Task B.5 -- per-node debugging.

The headless suite pins the runtime: a breakpoint pauses the entity on the
marked node, Resume/Step release it. This suite pins that the SOURCE says what
the runtime does, and -- because the plan asks for a UI -- that the panel
actually offers the three controls.

Run:  python tests/test_per_node_debug_source.py
"""

from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..")))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

FAILED = []
PASSED = 0


def check(condition, message):
    global PASSED
    if condition:
        PASSED += 1
    else:
        FAILED.append(message)


def read(*parts):
    path = os.path.join(ROOT, "Wasteland", "src", "Wasteland", *parts)
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def read_editor(*parts):
    path = os.path.join(ROOT, "DemonCore-Editor", *parts)
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


# ------------------------------------------------------------ the flag

def test_the_node_declares_a_breakpoint():
    source = read("VisualBlocks", "Graph.h")
    start = source.index("struct GraphNode\n\t{")
    body = source[start:start + 2400]
    check("bool Breakpoint = false;" in body,
          "GraphNode must declare Breakpoint, defaulting to false")


def test_the_serializer_round_trips_the_breakpoint():
    source = read("VisualBlocks", "GraphSerializer.cpp")
    check('out << YAML::Key << "breakpoint"' in source,
          "the breakpoint must be written")
    check('if (node.Breakpoint)' in source,
          "it must be written only when true, so existing files stay "
          "byte-identical")
    check('node.Breakpoint = item["breakpoint"].as<bool>();' in source,
          "the breakpoint must be read back as a bool")


def test_the_loader_allowlists_the_key():
    """Without this the writer emits a key the reader rejects -- the exact
    defect the 'title' comment records."""
    source = read("VisualBlocks", "GraphSerializer.cpp")
    start = source.index("IsKnownNodeKey")
    body = source[start:start + 1200]
    check('key == "breakpoint"' in body,
          "IsKnownNodeKey must accept 'breakpoint'")


# ------------------------------------------------------------ codegen

def test_the_trace_call_is_emitted_only_on_a_marked_node():
    source = read("VisualBlocks", "Compiler.cpp")
    check("self._vb_trace(" in source,
          "the compiler must emit a trace call")
    check("if (node.Breakpoint)" in source,
          "the trace call must be gated on the node's own flag")
    check("bool NeedsTraceHelper(const Graph &graph)" in source,
          "a helper predicate must decide whether the method is needed")


def test_the_hook_method_lives_inside_the_class():
    """The call site is `self._vb_trace(...)`. A module-level function is not
    reachable through the instance, so emitting it outside the class body
    produces an AttributeError at the first breakpoint."""
    source = read("VisualBlocks", "Compiler.cpp")
    classAt = source.index('out << "class " << className')
    hookAt = source.index("def _vb_trace(self, node_id):")
    check(hookAt > classAt,
          "_vb_trace must be emitted AFTER the class statement, i.e. inside "
          "the class body")


def test_the_trace_call_precedes_the_node_statement():
    """The call is written into the node's own block BEFORE its statement text,
    so the pause lands on the marked node rather than on its successor."""
    source = read("VisualBlocks", "Compiler.cpp")
    # The EMISSION, not the comment or the helper's body -- the first textual
    # occurrence of the string is a comment describing the call site.
    start = source.index('<< "self._vb_trace(" << ')
    # The window reaches BACK to include the `block <<` that opens the line --
    # the call itself is the right-hand operand.
    window = source[start - 80:start + 260]
    check("block << std::string((size_t)indent" in window,
          "the trace call must be written into the node's own block")


def test_the_hook_degrades_to_a_no_op_without_a_runtime_hook():
    source = read("VisualBlocks", "Compiler.cpp")
    check('getattr(self, \\"_vb_traceHook\\", None)' in source,
          "the hook lookup must have a default, so a graph compiled with a "
          "breakpoint but not being debugged does not raise")


# ------------------------------------------------------------ runtime

def test_the_pause_is_carried_by_an_exception_not_a_return_code():
    """The hook fires in the middle of a generated method; the only way to stop
    the rest of that method running is to raise out of it."""
    source = read("Scene", "FlowGraphSystem.cpp")
    check("PauseException()" in source,
          "a dedicated exception type must exist")
    check('PyErr_NewException("Wasteland.VbPause"' in source,
          "the exception must be created with PyErr_NewException")
    check("throw py::error_already_set();" in source,
          "the hook must raise, not return")


def test_the_tick_treats_a_pause_as_a_pause_not_a_failure():
    source = read("Scene", "FlowGraphSystem.cpp")
    check("e.matches(PauseException())" in source,
          "the tick's catch must recognise the pause exception")
    # It must be recognised BEFORE the generic failure path, or a breakpoint
    # poisons the entity and erases the instance under the debugger.
    pause = source.index("e.matches(PauseException())")
    failure = source.index("threw while ticking")
    check(pause < failure,
          "the pause branch must precede the generic failure branch")


def test_a_paused_graph_does_not_tick():
    source = read("Scene", "FlowGraphSystem.cpp")
    check("if (IsPaused(id))" in source,
          "a paused entity must not be ticked")


def test_resume_does_not_disable_the_breakpoint():
    source = read("Scene", "FlowGraphSystem.cpp")
    check("s_SkipOnce" in source,
          "Resume must arrange for the released node to run once without "
          "re-pausing, or Continue is a no-op")
    check("s_SkipOnce[id] = entry->second;" in source,
          "the skip must name the node that fired")


def test_the_pause_state_is_dropped_with_the_entity():
    source = read("Scene", "FlowGraphSystem.cpp")
    check("s_PausedNode.erase(id);" in source,
          "the pause state must be dropped on entity destruction")
    check("s_SkipOnce.erase(id);" in source,
          "the skip-once state must be dropped too")


def test_the_debugging_api_is_bound_to_python():
    source = read("Scripting", "Bindings.cpp")
    check('def_readwrite("Breakpoint", &vb::GraphNode::Breakpoint)' in source,
          "GraphNode.Breakpoint must be bound")
    for name in ("IsPaused", "GetPausedNode", "Resume", "Step"):
        check('def_static("%s"' % name in source,
              "FlowGraphSystem.%s must be bound" % name)


# ------------------------------------------------------------ the panel

def test_the_panel_offers_a_breakpoint_toggle():
    source = read_editor("src", "Panels", "VisualBlocksPanel.cpp")
    check('ImGui::Checkbox("Breakpoint"' in source,
          "the node inspector must offer a breakpoint toggle")
    check("node->Breakpoint = breakpoint;" in source,
          "the toggle must write to the node")
    check("BeginEdit();" in source and "CommitEdit();" in source,
          "the toggle must be undoable like every other edit here")


def test_the_panel_offers_the_step_control():
    source = read_editor("src", "Panels", "VisualBlocksPanel.cpp")
    check('ImGui::Button("Resume")' in source,
          "the debug toolbar must offer Resume")
    check('ImGui::Button("Step")' in source,
          "the debug toolbar must offer Step")
    check("FlowGraphSystem::Resume(" in source,
          "Resume must call the runtime")
    check("FlowGraphSystem::Step(" in source,
          "Step must call the runtime")


def test_the_panel_reads_pause_state_from_the_engine():
    """A panel-local 'am I paused' flag would drift from the engine the moment
    a breakpoint fired from a graph the panel did not author."""
    source = read_editor("src", "Panels", "VisualBlocksPanel.cpp")
    check("FlowGraphSystem::IsPaused(" in source,
          "the enabled state must come from the engine")
    check("RefreshPauseState();" in source,
          "the pause state must be refreshed each frame")
    check("FlowGraphSystem::GetPausedNode(" in source,
          "the paused node id must come from the engine, not the graph")


def test_the_generated_panel_highlights_the_executing_line():
    source = read_editor("src", "Panels", "VisualBlocksPanel.cpp")
    check("FindGeneratedLineForNode" in source,
          "the panel must map a node to its generated line")
    check('self._vb_trace(\\"' in source,
          "the line must be found by the trace call the compiler emitted")
    check("m_HighlightLine" in source,
          "the highlight must be carried on the panel")


def main():
    tests = [value for key, value in sorted(globals().items())
             if key.startswith("test_") and callable(value)]
    for test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001
            FAILED.append("%s raised %s: %s" % (test.__name__,
                                                type(exc).__name__, exc))
    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    for message in FAILED:
        print("  FAIL:", message)
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
