"""Source contract for the animation nodes (Task A.8).

`tests/headless/test_animation_nodes.py` proves the behaviour; this suite proves
the code is wired the way the plan requires, by reading the sources. It runs
against a stale or absent build, so it catches a registration that was never
made -- the class of mistake that otherwise surfaces only as an AttributeError
at the end of a ten-minute build.
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


def test_behaviour_nodes_are_registered():
    """A node that compiles but has no NodeDef is the NB.9 failure."""
    source = read("VisualBlocks", "NodeLibraryV1.cpp")
    for type_id in ("PlayAnimation", "SetAnimationState", "SetTrigger"):
        check('def.TypeId = "%s"' % type_id in source,
              "%s is registered as a behaviour node" % type_id)
        check(source.count('def.TypeId = "%s"' % type_id) == 1,
              "%s is registered exactly once (duplicate registrations would "
              "make Find() order-dependent)" % type_id)


def test_flow_nodes_are_registered():
    source = read("VisualBlocks", "NodeLibraryFlow.cpp")
    for type_id in ("GetPoseBonePosition", "GetPoseBoneRotation",
                    "OnAnimationEvent"):
        check('def.TypeId = "%s"' % type_id in source,
              "%s is registered as a flow node" % type_id)


def test_state_nodes_use_the_crossfade_not_a_cut():
    """SetAnimationState must crossfade; PlayAnimation must hard-cut.

    One node with a mode pin would make the careless cut and the deliberate fade
    the same authoring act, which is why they are separate nodes.
    """
    source = read("VisualBlocks", "NodeLibraryV1.cpp")
    check("_set_animation_state(ctx.entity" in source,
          "SetAnimationState drives _set_animation_state")
    check("_play_animation(ctx.entity" in source,
          "PlayAnimation still drives _play_animation (the hard cut)")

    system = read("Scene", "AnimationSystem.cpp")
    check("TransitionTo(stateName, effective)" in system,
          "the crossfade path calls AnimationStateMachine::TransitionTo")
    check("runtime.Machine.SetState(stateName)" in system,
          "the hard-cut path still calls SetState")


def test_shim_helpers_exist_and_do_not_raise():
    """Every shim returns SUCCESS/FAILURE. A raise would leave the node inert.

    The script host swallows exceptions and carries on, so a shim that raises is
    silently dead on exactly the branch that needs it.
    """
    source = read("VisualBlocks", "Compiler.cpp")
    for helper in ("_set_animation_state", "_set_trigger"):
        check("def %s(" % helper in source,
              "the %s shim exists" % helper)
        check("return FAILURE" in source,
              "the shims report failure rather than raising")

    # The helpers must be called by the nodes, and the call must not be nested
    # inside another `_`-prefixed function (which would break test_node_library's
    # arity scan).
    check("def _play_animation(agent, name, loop):" in source,
          "_play_animation is still a module-level shim")


def test_pose_helper_is_conditional():
    """The preamble helper is emitted only when a pose node is present."""
    source = read("VisualBlocks", "Compiler.cpp")
    check("bool NeedsPoseBoneHelper(const Graph &graph)" in source,
          "a predicate decides whether the helper is needed")
    check('node.TypeId == "GetPoseBonePosition"' in source,
          "the predicate recognises GetPoseBonePosition")
    check('node.TypeId == "GetPoseBoneRotation"' in source,
          "the predicate recognises GetPoseBoneRotation")
    check("if (NeedsPoseBoneHelper(graph))" in source,
          "the helper is emitted conditionally, not unconditionally")


def test_pose_helper_reads_the_pose_not_the_component():
    """The getter must read the Animator's pose, not re-sample the clip.

    Re-sampling would need a time and would return a different value than the
    renderer draws; the pose is the single source of truth.
    """
    source = read("VisualBlocks", "Compiler.cpp")
    check("anim.GetLastPose()" in source,
          "the pose helper reads Animator::GetLastPose")
    check("skeleton.GetBoneWorldPosition" in source,
          "the world helper goes through Skeleton::GetBoneWorldPosition")
    check("skeleton.FindBone(name)" in source,
          "bones are resolved by NAME, not by index")


def test_event_method_name_is_derived_from_the_type():
    """An unedited OnAnimationEvent node must not compile to OnUpdate.

    Otherwise a graph carrying both has two OnUpdate entry points and one is
    silently unreachable.
    """
    source = read("VisualBlocks", "Compiler.cpp")
    check("event.IsTypeNamedEvent()" in source,
          "FlowMethodNameFor delegates to the shared predicate")
    check("GraphNode::TypeIdForEventMethod(event.TypeId)" in source,
          "the derived name comes from the shared mapping")

    validator = read("VisualBlocks", "Validator.cpp")
    check("node.IsTypeNamedEvent()" in validator,
          "the validator also recognises the type-named event, so an unedited "
          "node is not reported as 'no event node'")
    check("GraphNode::TypeIdForEventMethod(node.TypeId)" in validator,
          "the validator derives the same method name the compiler does")
    check("ResolveEventMethodName" in validator,
          "the duplicate check compares resolved method names")

    # The predicate is shared, so no TU may carry its own copy of the comparison.
    # Graph.cpp is exempt only inside the definition of IsTypeNamedEvent itself --
    # that one comparison IS the predicate.
    for name in ("Compiler.cpp", "Validator.cpp"):
        body = read("VisualBlocks", name)
        check('TypeId == "OnAnimationEvent"' not in body,
              "%s does not carry a private copy of the predicate" % name)

    graph_body = read("VisualBlocks", "Graph.cpp")
    definition = graph_body.index("bool GraphNode::IsTypeNamedEvent() const")
    after = graph_body[definition:]
    check('TypeId == "OnAnimationEvent"' not in after[after.index("}"):],
          "Graph.cpp has no copy of the predicate outside its own definition")


def test_animation_event_is_actually_dispatched():
    """The hook must be fired by the engine, not merely named.

    A dispatcher entry nobody calls is the inert-node failure the plan calls out.
    """
    system = read("Scene", "AnimationSystem.cpp")
    check('"OnAnimationEvent"' in system,
          "AnimationSystem dispatches OnAnimationEvent")
    check("DispatchNamedById" in system,
          "the dispatch goes through the UUID form")

    dispatcher = read("Scene", "ScriptEventDispatcher.cpp")
    check("DispatchNamedById" in dispatcher,
          "the UUID form exists in the dispatcher")
    check("DispatchNamedById(entity.GetComponent<IDComponent>().ID" in dispatcher,
          "DispatchNamed forwards to the UUID form rather than duplicating it")


def test_play_animation_is_idempotent():
    """Firing PlayAnimation every tick must not restart the clip.

    SetState removes the live instance and adds a new one, so a tree node that
    runs every frame restarted the clip each frame and the pose never left its
    first keyframe -- measured as exactly 1.5 degrees after 60 ticks. The loop
    flag is still applied so a mid-play change reaches the live instance.
    """
    system = read("Scene", "AnimationSystem.cpp")
    check("GetCurrentState() == stateName" in system,
          "PlayState skips the re-cut when the machine is already in that state")
    check("SetLooping(current, loop)" in system,
          "the loop flag still reaches an already-playing instance")
    # The crossfade path needs the same guard: TransitionTo rebuilds the
    # instance, so a per-tick SetAnimationState would restart the fade.
    check(system.count("GetCurrentState() == stateName") >= 2,
          "both PlayState and TransitionState are idempotent (got %d guard(s))"
          % system.count("GetCurrentState() == stateName"))


def test_find_event_matches_the_compiler_predicate():
    """FindEvent must recognise a type-named event, exactly as CompileFlow does.

    It did not, and an OnAnimationEvent-only graph got "OnUpdate" cached as its
    entry method, so every tick raised AttributeError and the handler never ran.
    """
    graph = read("VisualBlocks", "Graph.cpp")
    check("node.IsTypeNamedEvent()" in graph,
          "FindEvent delegates to the shared predicate")
    compiler = read("VisualBlocks", "Compiler.cpp")
    check("node.IsTypeNamedEvent()" in compiler,
          "the compiler's event loop uses the same predicate")


def test_flow_graph_tick_is_onupdate_only():
    """The runtime must not tick a non-OnUpdate event method every frame.

    s_Methods[id] is the FIRST event in declaration order. Calling it
    unconditionally made an OnAnimationEvent-only graph run its handler on every
    frame -- the entity moved to the handler's target on tick one, before any
    animation event had fired. Other events belong to ScriptEventDispatcher.
    """
    system = read("Scene", "FlowGraphSystem.cpp")
    check('s_Methods[id] == "OnUpdate"' in system,
          "the per-frame call is gated on the entry method being OnUpdate")


def test_bindings_expose_the_state_api():
    bindings = read("Scripting", "Bindings.cpp")
    check('"TransitionAnimationState"' in bindings,
          "Entity.TransitionAnimationState is bound (the SetAnimationState node)")
    check('"SetAnimationTrigger"' in bindings,
          "Entity.SetAnimationTrigger is bound (the SetTrigger node)")
    check('"PlayAnimation"' in bindings,
          "Entity.PlayAnimation is bound (the PlayAnimation node)")


def test_headless_suite_exists():
    suite = os.path.join(ROOT, "tests", "headless", "test_animation_nodes.py")
    check(os.path.isfile(suite), "the headless animation-nodes suite exists")

    text = open(suite, "r", encoding="utf-8").read()
    # The plan's own verification: a tree node drives a visible pose change.
    check("playing Walk must swing the Arm" in text,
          "the suite asserts a pose change from a graph node")
    check("the OnAnimationEvent handler must have run" in text,
          "the suite asserts the animation event actually fires")


def main():
    test_behaviour_nodes_are_registered()
    test_flow_nodes_are_registered()
    test_state_nodes_use_the_crossfade_not_a_cut()
    test_shim_helpers_exist_and_do_not_raise()
    test_pose_helper_is_conditional()
    test_pose_helper_reads_the_pose_not_the_component()
    test_event_method_name_is_derived_from_the_type()
    test_animation_event_is_actually_dispatched()
    test_play_animation_is_idempotent()
    test_find_event_matches_the_compiler_predicate()
    test_flow_graph_tick_is_onupdate_only()
    test_bindings_expose_the_state_api()
    test_headless_suite_exists()

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
