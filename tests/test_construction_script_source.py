"""Source contract for the Construction Script (Task B.1).

`tests/headless/test_construction_script.py` proves the behaviour; this suite
proves the wiring is the shape the plan requires, by reading the sources. It
runs against a stale or absent build, so it catches a registration that was
never made -- the class of mistake that otherwise surfaces only as an
AttributeError at the end of a ten-minute build.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "Wasteland", "src", "Wasteland")

PASSED = 0
FAILED = []


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def read(*parts):
    with open(os.path.join(ENGINE, *parts), "r", encoding="utf-8",
              errors="replace") as handle:
        return handle.read()


def test_begin_play_is_a_type_named_event():
    """The predicate is ONE function shared by every consumer.

    Three separate copies of `node.TypeId == "OnAnimationEvent"` drifted once
    already: Graph::FindEvent stopped recognising a type-named event, so the
    runtime cached the wrong method name and raised AttributeError every tick.
    """
    graph_header = read("VisualBlocks", "Graph.h")
    check("bool IsTypeNamedEvent() const;" in graph_header,
          "GraphNode::IsTypeNamedEvent is declared")
    check("static std::string TypeIdForEventMethod" in graph_header,
          "GraphNode::TypeIdForEventMethod is declared")

    graph = read("VisualBlocks", "Graph.cpp")
    check('TypeId == "OnBeginPlay"' in graph,
          "IsTypeNamedEvent recognises OnBeginPlay")
    check('TypeId == "OnAnimationEvent"' in graph,
          "IsTypeNamedEvent still recognises OnAnimationEvent")
    check("return TypeId == \"OnBeginPlay\" || TypeId == \"OnAnimationEvent\";"
          in graph.replace("\n", " ").replace("\t", " ") or
          "TypeId == \"OnBeginPlay\" || TypeId == \"OnAnimationEvent\"" in graph,
          "the predicate is the OR of exactly those two types")

    # No consumer may carry its own copy of the comparison.
    for name in ("Compiler.cpp", "Validator.cpp", "Graph.cpp"):
        source = read("VisualBlocks", name)
        check('node.TypeId == "OnAnimationEvent"' not in source,
              "%s no longer carries a private copy of the predicate" % name)
        check('node.TypeId == "OnBeginPlay"' not in source,
              "%s does not special-case OnBeginPlay either" % name)


def test_onbeginplay_node_is_registered():
    source = read("VisualBlocks", "NodeLibraryFlow.cpp")
    check('def.TypeId = "OnBeginPlay"' in source,
          "OnBeginPlay is registered as a flow node")
    check(source.count('def.TypeId = "OnBeginPlay"') == 1,
          "OnBeginPlay is registered exactly once")
    check('"Flow/Event"' in source,
          "OnBeginPlay sits in the event category, not a gameplay category")

    # The dispatcher already maps the id to this exact name; a mismatch here is
    # a graph that compiles and never runs.
    dispatcher = read("Scene", "ScriptEventDispatcher.cpp")
    check('return "OnBeginPlay";' in dispatcher,
          "ScriptEventId::BeginPlay maps to the method name OnBeginPlay")


def test_runtime_start_dispatches_begin_play():
    """The dispatch must come from the runtime lifecycle, not from attach."""
    scene = read("Scene", "Scene.cpp")
    check("DispatchBeginPlay();" in scene,
          "OnRuntimeStart dispatches BeginPlay to the existing entities")
    check("m_RuntimeStarted = true;" in scene,
          "the runtime-active flag is set on start")
    check("DispatchBeginPlay(entity);" in scene,
          "an entity created mid-play gets its own BeginPlay")
    check("ScriptEventDispatcher::Dispatch(entity, ScriptEventId::BeginPlay"
          in scene,
          "the dispatch goes through the enum, so a method-name rename cannot "
          "leave the call site naming an event nothing answers")

    # The flag must be set BEFORE the broadcast, so a handler that spawns an
    # entity sees IsRuntimeActive() already true.
    start = scene.index("void Scene::OnRuntimeStart()")
    flag = scene.index("m_RuntimeStarted = true;", start)
    broadcast = scene.index("DispatchBeginPlay();", start)
    check(flag < broadcast,
          "the flag is set BEFORE the BeginPlay broadcast")


def test_runtime_stop_clears_the_flag():
    scene = read("Scene", "Scene.cpp")
    stop = scene.index("void Scene::OnRuntimeStop()")
    cleared = scene.index("m_RuntimeStarted = false;", stop)
    check(cleared > stop,
          "OnRuntimeStop clears the runtime-active flag")
    # Cleared before the subsystems stop, so a shutdown path that creates an
    # entity does not dispatch BeginPlay into a stopped world.
    audio = scene.index("if (m_AudioSystem)", stop)
    check(cleared < audio,
          "the flag is cleared BEFORE the audio subsystem stops")


def test_mid_play_attach_dispatches_begin_play():
    """A graph attaching while the runtime is active gets its BeginPlay.

    The OnRuntimeStart broadcast cannot reach it: at broadcast time the graph has
    not attached, so FlowGraphSystem has no instance for that entity. Attach is
    the one moment both the instance and the complete entity exist.
    """
    system = read("Scene", "FlowGraphSystem.cpp")
    check("scene.IsRuntimeActive()" in system,
          "the attach path asks the scene whether the runtime is active")
    check("ScriptEventId::BeginPlay" in system,
          "attach dispatches BeginPlay for a mid-play spawn")
    check('s_Methods[id] = methodName;' in system,
          "the dispatch is placed after the instance and method are cached, so "
          "the instance exists when the handler runs")


def test_is_runtime_active_is_queryable():
    header = read("Scene", "Scene.h")
    check("bool IsRuntimeActive() const { return m_RuntimeStarted; }" in header,
          "Scene exposes IsRuntimeActive()")
    check("bool m_RuntimeStarted = false;" in header,
          "the flag defaults to false, so a scene that never starts runtime "
          "does not dispatch BeginPlay")


def test_headless_suite_exists():
    suite = os.path.join(ROOT, "tests", "headless", "test_construction_script.py")
    check(os.path.isfile(suite), "the headless construction-script suite exists")
    text = open(suite, "r", encoding="utf-8").read()
    check("assert the variable is 1 after one tick" in text or
          "OnBeginPlay must have run exactly once" in text,
          "the suite asserts the plan's own verification")
    check("spawned during play must get its own BeginPlay" in text,
          "the suite covers the mid-play spawn path")


def main():
    test_begin_play_is_a_type_named_event()
    test_onbeginplay_node_is_registered()
    test_runtime_start_dispatches_begin_play()
    test_runtime_stop_clears_the_flag()
    test_mid_play_attach_dispatches_begin_play()
    test_is_runtime_active_is_queryable()
    test_headless_suite_exists()

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
