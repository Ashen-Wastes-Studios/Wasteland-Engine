"""Animation nodes in visual scripting (Task A.8).

Two layers are asserted, because the node table alone proves nothing:

  EMISSION -- the compiled Python must call the shim the engine actually has.
  BEHAVIOUR -- a tree that names the node must change a real pose on a real
  entity. This is the plan's own verification, and it is the one that catches a
  node that compiles cleanly and does nothing.

The behaviour tests build a real `.wlanim` (two bones, two clips, a machine with
a trigger-driven transition) rather than using the shipped Hero asset, because
the transition is what SetTrigger needs and Hero has no machine.

Run:  python tests/headless/runner.py test_animation_nodes
"""

from __future__ import annotations

import math
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wlpath  # noqa: E402

wlpath.setup()

# The generated tree module does `import wl_behavior`, and that module ships as
# an asset script rather than a package -- so it needs the asset root on
# sys.path, exactly as test_follow_path.py does.
_SCRIPTS = os.path.join(os.path.dirname(os.path.dirname(HERE)),
                        "DemonCore-Editor", "assets", "scripts")
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

from runner import check, check_eq, check_near  # noqa: E402
import Wasteland as W  # noqa: E402

STEP = 1.0 / 60.0

# --- the rig and the machine ------------------------------------------------

# Root at the origin, Arm one metre along +X. The Walk clip swings the Arm 90
# degrees about Z by t=1.0, so a pose that has moved is unambiguous.
BONE_ARM = 1


def _build_asset(path):
    """Write a two-state machine with a trigger-driven transition."""
    anim = W.Animation

    skeleton = anim.Skeleton()
    ok, error = skeleton.BuildFromNames(["Root", "Arm"], ["", "Root"])
    if not ok:
        raise RuntimeError("BuildFromNames failed: " + error)
    pose = anim.Pose()
    pose.Resize(2)
    pose.SetBone(0, anim.BoneTransform(W.Vec3(0.0, 0.0, 0.0),
                                       W.Quat.FromAxisAngle(W.Vec3(0.0, 0.0, 1.0), 0.0),
                                       W.Vec3(1.0, 1.0, 1.0)))
    pose.SetBone(1, anim.BoneTransform(W.Vec3(1.0, 0.0, 0.0),
                                       W.Quat.FromAxisAngle(W.Vec3(0.0, 0.0, 1.0), 0.0),
                                       W.Vec3(1.0, 1.0, 1.0)))
    skeleton.SetRestPose(pose)

    idle = anim.AnimationClip("Idle")
    idle.SetLooping(True)

    walk = anim.AnimationClip("Walk")
    walk.SetLooping(True)
    swing = W.Quat.FromAxisAngle(W.Vec3(0.0, 0.0, 1.0), math.radians(90.0))
    walk.AddRotationKey(BONE_ARM, 0.0, W.Quat.FromAxisAngle(W.Vec3(0.0, 0.0, 1.0), 0.0))
    walk.AddRotationKey(BONE_ARM, 1.0, swing)

    machine = anim.AnimationStateMachine()
    machine.AddStateFromClip("Idle", idle)
    machine.AddStateFromClip("Walk", walk)
    machine.SetStateClip("Idle", idle)
    machine.SetStateClip("Walk", walk)
    transition = anim.AnimationTransition()
    transition.FromState = "Idle"
    transition.ToState = "Walk"
    transition.Trigger = "go"
    transition.Duration = 0.0
    machine.AddTransition(transition)

    asset = anim.AnimationAsset()
    asset.Name = "AnimationNodeProbe"
    asset.Skeleton = skeleton
    asset.Clips = [idle, walk]
    asset.StateMachine = machine
    asset.HasStateMachine = True

    err = anim.SaveAnimation(path, asset)
    if err is not None:
        raise RuntimeError("SaveAnimation failed: " + err.Message)
    return path


def _scene_with_animated(asset_path):
    """A scene and an entity whose AnimationComponent points at the asset."""
    from Wasteland import AnimationComponent, Scene

    scene = Scene()
    entity = scene.CreateEntity("animated")
    comp = AnimationComponent()
    comp.AssetPath = asset_path
    comp.DefaultState = "Idle"
    entity.AddAnimationComponent(comp)
    # One tick loads the asset; without it every later query is None.
    scene.OnUpdateRuntime(STEP)
    return scene, entity


def _arm_rotation(animator):
    """The Arm bone's swing angle in degrees, read from the last pose."""
    pose = animator.GetLastPose()
    bone = pose.GetBone(BONE_ARM)
    if bone is None:
        return None
    # glm stores a quaternion as (w, x, y, z); the binding exposes .x/.y/.z/.w.
    # For a rotation about +Z the angle is 2*atan2(z, w).
    return math.degrees(2.0 * math.atan2(bone.Rotation.z, bone.Rotation.w))


def _tree_source(name, node_type, literals):
    """Compile a one-task tree through the real NodeDef and emitter."""
    vb = W.VisualBlocks
    graph = vb.Graph()
    graph.Kind = "tree"
    graph.Name = name

    root = vb.GraphNode()
    root.Id = "root"
    root.TypeId = "Sequence"
    root.ChildIds = ["task"]
    graph.Nodes.append(root)

    task = vb.GraphNode()
    task.Id = "task"
    task.TypeId = node_type
    for pin, value in literals.items():
        task.SetLiteral(pin, value)
    graph.Nodes.append(task)

    ok, source, report, message = vb.CompileTree(graph)
    check(ok, "%s must be registered and compile; got %s (%s)"
          % (node_type, message, report))
    return source


def _run_tree(source, name, entity, scene, ticks=3):
    """Run a compiled tree for `ticks` frames.

    The SCENE is ticked on every iteration, and that is not incidental: the
    animation system advances in `Scene::OnUpdateRuntime`, so a tree tick alone
    plays the state but never advances the clip. The first version of this helper
    ticked only the tree and reported a frozen pose as a broken node.
    """
    module = types.ModuleType("_anim_node_probe")
    exec(compile(source, "<%s>" % name, "exec"), module.__dict__)
    instance = getattr(module, name)(entity, scene)
    for _ in range(ticks):
        instance.tick(STEP)
        scene.OnUpdateRuntime(STEP)
    return instance


# --- emission ---------------------------------------------------------------

def test_tree_nodes_emit_their_shim():
    """The compiled Python must call the shim functions the engine provides.

    A substring check rather than a run: this asserts the WIRING, and the
    behaviour tests below assert the EFFECT. Separating them is what makes a
    failure name the broken half.
    """
    source = _tree_source("PlayProbe", "PlayAnimation",
                          {"Animation": "Walk", "Loop": "true"})
    check("_play_animation(ctx.entity, \"Walk\", True)" in source,
          "PlayAnimation must drive _play_animation")

    source = _tree_source("StateProbe", "SetAnimationState",
                          {"State": "Walk", "Blend": "0.25"})
    check("_set_animation_state(ctx.entity, \"Walk\", 0.250000)" in source,
          "SetAnimationState must drive _set_animation_state with the blend")

    source = _tree_source("TriggerProbe", "SetTrigger",
                          {"Trigger": "go"})
    check("_set_trigger(ctx.entity, \"go\")" in source,
          "SetTrigger must drive _set_trigger")


def test_pose_nodes_emit_the_helper_only_when_used():
    """The `_pose_bone` preamble is conditional, and the node's output is wired.

    Two things are true at once and both are asserted:
      - the helper appears only in a graph that contains a pose node;
      - a pose node's expression reaches the generated code only where its
        output pin is CONSUMED. A data node wired to nothing is dead code -- the
        flow compiler emits the event's exec chain, so an unconsumed pose read
        compiles to a `pass` body and its literal never appears.
    """
    vb = W.VisualBlocks

    def pose_graph(name, world, consume, expect_valid=True):
        graph = vb.Graph()
        graph.Kind = "flow"
        graph.Name = name
        event = vb.GraphNode()
        event.Id = "event"
        event.TypeId = "FlowOnEvent"
        event.EventName = "OnUpdate"
        graph.Nodes.append(event)

        pose = vb.GraphNode()
        pose.Id = "pose"
        pose.TypeId = "GetPoseBonePosition"
        pose.SetLiteral("Bone", "Arm")
        pose.SetLiteral("World", "true" if world else "false")
        graph.Nodes.append(pose)

        move = vb.GraphNode()
        move.Id = "move"
        move.TypeId = "SetTransform"
        move.SetLiteral("Rotation", "(0.0, 0.0, 0.0)")
        move.SetLiteral("Scale", "(1.0, 1.0, 1.0)")
        graph.Nodes.append(move)

        # The EXEC chain in a flow graph is a LINK, not a child list -- child ids
        # are the behaviour-tree mechanism. This first version used ChildIds and
        # the compiler emitted a `pass` body, because nothing made `move`
        # reachable from the event.
        exec_link = vb.GraphLink()
        exec_link.FromNode = "event"
        exec_link.FromPin = "Out"
        exec_link.ToNode = "move"
        exec_link.ToPin = "In"
        graph.Links.append(exec_link)

        if consume:
            link = vb.GraphLink()
            link.FromNode = "pose"
            link.FromPin = "Position"
            link.ToNode = "move"
            link.ToPin = "Translation"
            graph.Links.append(link)

        result = vb.ValidateFlow(graph)
        if expect_valid:
            check_eq(result.ErrorCount(), 0,
                     "%s must validate (got %d errors)"
                     % (name, result.ErrorCount()))
        return graph, vb.CompileFlow(graph)

    _g, with_pose = pose_graph("WithPose", world=True, consume=True)
    check("_pose_bone_world" in with_pose,
          "a graph with a pose node must carry the world helper")
    check("_pose_bone(" in with_pose,
          "a graph with a pose node must carry the local helper")
    check("Arm" in with_pose, "the bone name must reach the generated code")
    check("_pose_bone_world(self.entity, " in with_pose,
          "the World pin must select the world helper")

    _g, local = pose_graph("LocalPose", world=False, consume=True)
    # The world lookup is still TEXTUALLY present -- it is one arm of a
    # generated conditional -- so the assertion is on the SELECTOR, not on the
    # substring. `_pose_bone_world` is defined either way, because the helper is
    # emitted for any pose node; what must not happen is the local pin taking
    # the world path.
    check("if False else" in local,
          "a local-only read must not select the world helper")
    check("_pose_bone(self.entity, " in local,
          "a local read must call the local helper")

    # The rotation output is a Vec4, and no numeric pin in the current node set
    # takes one -- SetVariable's Value is PinType::Any, the one pin that accepts
    # it. Wired into SetTransform's Vec3 Rotation the graph correctly fails
    # validation, which is the type system doing its job rather than a defect.
    rotation = vb.Graph()
    rotation.Kind = "flow"
    rotation.Name = "RotationPose"
    event = vb.GraphNode()
    event.Id = "event"
    event.TypeId = "FlowOnEvent"
    event.EventName = "OnUpdate"
    rotation.Nodes.append(event)
    pose = vb.GraphNode()
    pose.Id = "pose"
    pose.TypeId = "GetPoseBoneRotation"
    pose.SetLiteral("Bone", "Arm")
    rotation.Nodes.append(pose)
    store = vb.GraphNode()
    store.Id = "store"
    store.TypeId = "SetVariable"
    store.SetLiteral("Name", "lastRotation")
    store.SetLiteral("Value", "0")
    rotation.Nodes.append(store)
    exec_link = vb.GraphLink()
    exec_link.FromNode = "event"
    exec_link.FromPin = "Out"
    exec_link.ToNode = "store"
    exec_link.ToPin = "In"
    rotation.Links.append(exec_link)
    link = vb.GraphLink()
    link.FromNode = "pose"
    link.FromPin = "Rotation"
    link.ToNode = "store"
    link.ToPin = "Value"
    rotation.Links.append(link)
    result = vb.ValidateFlow(rotation)
    check_eq(result.ErrorCount(), 0,
             "the rotation graph must validate (got %d errors)"
             % result.ErrorCount())
    rotation_source = vb.CompileFlow(rotation)
    check("b.Rotation.x" in rotation_source and "b.Rotation.w" in rotation_source,
          "GetPoseBoneRotation must emit the (x, y, z, w) components")

    # The Vec4/Vec3 mismatch is refused rather than coerced.
    mismatch = vb.Graph()
    mismatch.Kind = "flow"
    mismatch.Name = "RotationMismatch"
    event = vb.GraphNode()
    event.Id = "event"
    event.TypeId = "FlowOnEvent"
    event.EventName = "OnUpdate"
    mismatch.Nodes.append(event)
    pose = vb.GraphNode()
    pose.Id = "pose"
    pose.TypeId = "GetPoseBoneRotation"
    pose.SetLiteral("Bone", "Arm")
    mismatch.Nodes.append(pose)
    move = vb.GraphNode()
    move.Id = "move"
    move.TypeId = "SetTransform"
    move.SetLiteral("Translation", "(0.0, 0.0, 0.0)")
    move.SetLiteral("Rotation", "(0.0, 0.0, 0.0)")
    move.SetLiteral("Scale", "(1.0, 1.0, 1.0)")
    mismatch.Nodes.append(move)
    exec_link = vb.GraphLink()
    exec_link.FromNode = "event"
    exec_link.FromPin = "Out"
    exec_link.ToNode = "move"
    exec_link.ToPin = "In"
    mismatch.Links.append(exec_link)
    link = vb.GraphLink()
    link.FromNode = "pose"
    link.FromPin = "Rotation"
    link.ToNode = "move"
    link.ToPin = "Rotation"
    mismatch.Links.append(link)
    result = vb.ValidateFlow(mismatch)
    check(result.ErrorCount() > 0,
          "a Vec4 wired into a Vec3 pin must be a validation error, not a "
          "silent coercion")

    # A pose node wired to NOTHING is a VALIDATION ERROR, not dead code: the
    # validator rejects an unreachable node rather than letting it compile to a
    # `pass` body. That is a better guarantee than the dead-code check this
    # comment originally described, and it is why every assertion above consumes
    # the pin.
    unconsumed_graph, _unconsumed_source = pose_graph(
        "UnconsumedPose", world=True, consume=False, expect_valid=False)
    unconsumed_result = vb.ValidateFlow(unconsumed_graph)
    check(unconsumed_result.ErrorCount() > 0,
          "an unconsumed pose read must be a validation error")

    without_pose = vb.Graph()
    without_pose.Kind = "flow"
    without_pose.Name = "WithoutPose"
    event = vb.GraphNode()
    event.Id = "event"
    event.TypeId = "FlowOnEvent"
    event.EventName = "OnUpdate"
    without_pose.Nodes.append(event)
    move = vb.GraphNode()
    move.Id = "move"
    move.TypeId = "SetTransform"
    move.SetLiteral("Translation", "(0.0, 0.0, 0.0)")
    move.SetLiteral("Rotation", "(0.0, 0.0, 0.0)")
    move.SetLiteral("Scale", "(1.0, 1.0, 1.0)")
    without_pose.Nodes.append(move)
    exec_link = vb.GraphLink()
    exec_link.FromNode = "event"
    exec_link.FromPin = "Out"
    exec_link.ToNode = "move"
    exec_link.ToPin = "In"
    without_pose.Links.append(exec_link)
    result = vb.ValidateFlow(without_pose)
    check_eq(result.ErrorCount(), 0,
             "the no-pose graph must validate (got %d errors)"
             % result.ErrorCount())
    no_pose_source = vb.CompileFlow(without_pose)
    check("_pose_bone" not in no_pose_source,
          "a graph with no pose node must not carry the helper")


def test_animation_event_node_names_its_method():
    """An OnAnimationEvent node must compile to a method of that name.

    The method name comes from the TYPE when the authored `event` field is
    empty. Without that rule the node compiles to OnUpdate -- silently wrong,
    and a graph carrying both would have two OnUpdate entry points.
    """
    vb = W.VisualBlocks
    graph = vb.Graph()
    graph.Kind = "flow"
    graph.Name = "EventProbe"
    graph.Schema = 2
    node = vb.GraphNode()
    node.Id = "event"
    node.TypeId = "OnAnimationEvent"
    graph.Nodes.append(node)

    result = vb.ValidateFlow(graph)
    check_eq(result.ErrorCount(), 0,
             "an OnAnimationEvent node must validate (got %d errors)"
             % result.ErrorCount())
    source = vb.CompileFlow(graph)
    check("def OnAnimationEvent(self" in source,
          "the generated class must expose an OnAnimationEvent method")
    check("def OnUpdate(self" not in source,
          "the node must NOT fall back to a second OnUpdate entry point")


def test_animation_event_node_survives_validation():
    """An unedited event node must not make the graph report 'no event'."""
    vb = W.VisualBlocks
    graph = vb.Graph()
    graph.Kind = "flow"
    graph.Name = "EventValidate"
    graph.Schema = 2
    node = vb.GraphNode()
    node.Id = "event"
    node.TypeId = "OnAnimationEvent"
    graph.Nodes.append(node)

    result = vb.ValidateFlow(graph)
    check_eq(result.ErrorCount(), 0,
             "an OnAnimationEvent node alone must validate with no errors")


def test_duplicate_animation_event_is_an_error():
    """Two handlers for one event would silently overwrite in the class."""
    vb = W.VisualBlocks
    graph = vb.Graph()
    graph.Kind = "flow"
    graph.Name = "DuplicateEvent"
    graph.Schema = 2
    typed = vb.GraphNode()
    typed.Id = "typed"
    typed.TypeId = "OnAnimationEvent"
    graph.Nodes.append(typed)
    named = vb.GraphNode()
    named.Id = "named"
    named.TypeId = "FlowOnEvent"
    named.EventName = "OnAnimationEvent"
    graph.Nodes.append(named)

    result = vb.ValidateFlow(graph)
    check(result.ErrorCount() > 0,
          "a typed event node and a named one for the same event must be "
          "reported as a duplicate rather than overwriting in the class")


# --- behaviour --------------------------------------------------------------

def test_play_animation_changes_the_pose():
    """THE PLAN'S VERIFICATION: a tree node that was inert now drives a pose."""
    path = _build_asset(os.path.join(tempfile.gettempdir(),
                                     "wasteland_anim_nodes.wlanim"))
    scene, entity = _scene_with_animated(path)
    animator = W.GetAnimationAnimator(entity)
    check(animator is not None, "the entity must have a live animator")
    if animator is None:
        return

    before = _arm_rotation(animator)
    check(before is not None and abs(before) < 1.0,
          "the Idle state holds the Arm at rest (got %r)" % before)

    source = _tree_source("PlayWalk", "PlayAnimation",
                          {"Animation": "Walk", "Loop": "true"})
    _run_tree(source, "PlayWalk", entity, scene, ticks=60)

    after = _arm_rotation(animator)
    check(after is not None, "the Arm bone must be present after playing")
    if after is not None:
        check(abs(abs(after) - 90.0) < 2.0,
              "playing Walk must swing the Arm ~90 degrees (got %.2f)" % after)


def test_set_animation_state_crossfades():
    """SetAnimationState reaches a different state than the default."""
    path = _build_asset(os.path.join(tempfile.gettempdir(),
                                     "wasteland_anim_nodes.wlanim"))
    scene, entity = _scene_with_animated(path)
    animator = W.GetAnimationAnimator(entity)
    check(animator is not None, "the entity must have a live animator")
    if animator is None:
        return

    source = _tree_source("FadeToWalk", "SetAnimationState",
                          {"State": "Walk", "Blend": "0.01"})
    _run_tree(source, "FadeToWalk", entity, scene, ticks=60)

    after = _arm_rotation(animator)
    check(after is not None and abs(abs(after) - 90.0) < 5.0,
          "SetAnimationState must reach Walk (got %r)" % after)


def test_set_trigger_drives_the_machine():
    """A trigger fires a transition without a node per cause."""
    path = _build_asset(os.path.join(tempfile.gettempdir(),
                                     "wasteland_anim_nodes.wlanim"))
    scene, entity = _scene_with_animated(path)
    animator = W.GetAnimationAnimator(entity)
    check(animator is not None, "the entity must have a live animator")
    if animator is None:
        return

    # Default state is Idle, whose clip holds the Arm at rest.
    check(abs(_arm_rotation(animator) or 0.0) < 1.0,
          "the machine starts in Idle")

    source = _tree_source("FireGo", "SetTrigger", {"Trigger": "go"})
    _run_tree(source, "FireGo", entity, scene, ticks=60)

    after = _arm_rotation(animator)
    check(after is not None and abs(abs(after) - 90.0) < 5.0,
          "firing the 'go' trigger must transition Idle -> Walk (got %r)" % after)


def test_on_animation_event_fires():
    """The hook a flow graph listens for must actually be dispatched.

    Without this the OnAnimationEvent node is inert -- the exact failure the
    plan calls out for PlayAnimation. The graph's handler moves the entity to a
    known position, so "the handler ran" is observable on the entity rather than
    on a private instance map.
    """
    vb = W.VisualBlocks

    graph = vb.Graph()
    graph.Kind = "flow"
    graph.Name = "AnimationEventListener"
    graph.Schema = 2

    event = vb.GraphNode()
    event.Id = "event"
    event.TypeId = "OnAnimationEvent"
    graph.Nodes.append(event)

    move = vb.GraphNode()
    move.Id = "move"
    move.TypeId = "SetTransform"
    move.SetLiteral("Translation", "(1.0, 2.0, 3.0)")
    graph.Nodes.append(move)

    # The exec chain: event Out -> move In. A LINK, not a child list -- child
    # ids are the behaviour-tree mechanism, and a flow graph whose event has no
    # outgoing link compiles to a `pass` body.
    exec_link = vb.GraphLink()
    exec_link.FromNode = "event"
    exec_link.FromPin = "Out"
    exec_link.ToNode = "move"
    exec_link.ToPin = "In"
    graph.Links.append(exec_link)

    result = vb.ValidateFlow(graph)
    check_eq(result.ErrorCount(), 0,
             "the listener graph must validate (got %d errors)" % result.ErrorCount())

    source = vb.CompileFlow(graph)
    check(isinstance(source, str) and "def OnAnimationEvent(self" in source,
          "the listener must compile to an OnAnimationEvent method")

    path = os.path.join(tempfile.gettempdir(), "wasteland_anim_event.wlblocks")
    err = vb.Save(path, graph)
    check(err is None, "the listener graph must save")
    if err is not None:
        print("   ", err.Message)
        return

    asset_path = _build_asset(os.path.join(
        tempfile.gettempdir(), "wasteland_anim_nodes.wlanim"))
    scene, entity = _scene_with_animated(asset_path)

    from Wasteland import VisualBlocksComponent
    graph_comp = VisualBlocksComponent()
    graph_comp.GraphPath = path
    entity.AddVisualBlocksComponent(graph_comp)

    # The graph attaches on the first tick, which is also the tick that makes
    # the entity's position readable as something other than the default.
    scene.OnUpdateRuntime(STEP)
    check(entity.GetVisualBlocksComponent().Attached,
          "the listener graph must attach on tick")
    # Vec3 has no __eq__ (see the binding at Bindings.cpp:611), so a whole-vector
    # comparison would be an identity test and always fail. Read components.
    at_rest = entity.GetTransform().Translation
    check_near(at_rest.x, 0.0, 1e-4,
               "the entity has not been moved before the event fires")
    check_near(at_rest.y, 0.0, 1e-4, "the entity is at rest on Y")
    check_near(at_rest.z, 0.0, 1e-4, "the entity is at rest on Z")

    # Playing a state is what dispatches OnAnimationEvent.
    check(entity.PlayAnimation("Walk", True),
          "PlayAnimation must succeed on a live machine")

    moved = entity.GetTransform().Translation
    check_near(moved.x, 1.0, 1e-4,
               "the OnAnimationEvent handler must have run and moved the entity")
    check_near(moved.y, 2.0, 1e-4, "the handler's Y translation must apply")
    check_near(moved.z, 3.0, 1e-4, "the handler's Z translation must apply")


def main():
    test_tree_nodes_emit_their_shim()
    test_pose_nodes_emit_the_helper_only_when_used()
    test_animation_event_node_names_its_method()
    test_animation_event_node_survives_validation()
    test_duplicate_animation_event_is_an_error()
    test_play_animation_changes_the_pose()
    test_set_animation_state_crossfades()
    test_set_trigger_drives_the_machine()
    test_on_animation_event_fires()

    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
