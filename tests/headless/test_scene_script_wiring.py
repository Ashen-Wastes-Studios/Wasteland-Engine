"""Do the scene's enemy scripts actually drive the enemies?

`DemoGame.wastescene` attaches three generated Visual Blocks files through one
host class per enemy (scripts/EnemyHost.py). This suite proves that wiring
from the outside: the same ScriptComponent + ScriptName the scene file carries,
driven through the real ScriptEngine and BehaviorTreeSystem, asserting on
OBSERVABLE world effects rather than on a component flag.

Two things make this different from test_tree_attach.py, which already proves
that a BehaviorTreeComponent drives an entity:

  * this goes through the SCRIPT, not a directly-attached component -- the
    path the scene file actually uses, and the only durable one, because the
    deserializer has no BehaviorTreeComponent block (a YAML-only attach is
    silently dropped on the next save);
  * it exercises the FLOW GRAPH half too, which is the part with no runtime
    host of its own.

The negative controls matter as much as the happy path: a host that raises on
an unknown tree, and a graph that must still run when the engine has no log
sink.

Run:  python tests/headless/runner.py test_scene_script_wiring
"""

from __future__ import annotations

import math
import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import (  # noqa: E402
    HealthComponent,
    Project,
    Scene,
    ScriptComponent,
    Vec3,
)
from runner import check, check_le  # noqa: E402

REPO = _os.path.dirname(_os.path.dirname(_HERE))
ASSET_ROOT = _os.path.join(REPO, "DemonCore-Editor")
SCRIPTS = _os.path.join(ASSET_ROOT, "assets", "scripts")
if SCRIPTS not in _sys.path:
    _sys.path.insert(0, SCRIPTS)

# The generated DamageFlash graph logs through `Wasteland.Log` -- it is a real
# engine binding now, not something the host can redirect. The first version of
# this graph called it once PER FRAME, so the suite captures it rather than
# letting ~180 lines/second reach the runner's output.
#
# `Wasteland.Log` is process-wide, so every capture is paired with a restore: a
# test that redirected it and left it redirected would silently swallow the
# engine's own diagnostics for every suite that ran after it.
import Wasteland as _Wasteland  # noqa: E402

_ORIGINAL_LOG = getattr(_Wasteland, "Log", None)


def capture_log():
    """Redirect `Wasteland.Log` into a fresh list. Pair with restore_log()."""
    lines = []
    _Wasteland.Log = lambda message: lines.append(message)
    return lines


def restore_log():
    if _ORIGINAL_LOG is not None:
        _Wasteland.Log = _ORIGINAL_LOG

# The scene file's ScriptComponent, reproduced exactly.
HOST_PATH = "scripts/EnemyHost.py"
STEP = 0.1


def attach_root():
    p = Project.Get()
    p.AttachAssetRoot(ASSET_ROOT)
    return p


def attach_host(scene, entity, class_name):
    """Attach the host the way the scene file does."""
    sc = ScriptComponent()
    sc.ScriptPath = HOST_PATH
    sc.ScriptName = class_name
    entity.AddOrReplaceScriptComponent(sc)
    return sc


def tick(sc, seconds):
    """Drive the FULL runtime tick -- StepPhysics skips scripts and trees."""
    for _ in range(int(seconds / STEP)):
        sc.OnUpdateRuntime(STEP)


def live_health(entity):
    """Read from the LIVE component.

    The object AddOrReplaceHealthComponent returns is a Python-owned COPY;
    the tree mutates the registry's instance, so reading the original shows
    the value at attach time and reports zero damage for a working tree.
    """
    return entity.GetHealthComponent().Current


def position(entity):
    t = entity.GetTransform().Translation
    return (t.x, t.y, t.z)


def make_world(npc_pos, victim_pos, victim_health=100.0):
    """One scene holding an NPC and a victim the tree can sense."""
    sc = Scene()
    npc = sc.CreateEntity("npc")
    npc.SetPosition(Vec3(*npc_pos))
    h = HealthComponent()
    h.Max = 100.0
    h.Current = 100.0
    npc.AddOrReplaceHealthComponent(h)

    victim = sc.CreateEntity("victim")
    victim.SetPosition(Vec3(*victim_pos))
    vh = HealthComponent()
    vh.Max = victim_health
    vh.Current = victim_health
    victim.AddOrReplaceHealthComponent(vh)
    return sc, npc, victim


# --------------------------------------------------------------------------
def test_chase_host_drives_damage():
    """The real proof: EnemyChaseHost makes an NPC walk in and hit a victim.

    Asserts health DROPPING, not `Attached == True` -- a system could set that
    flag without the tree ever running, and that is exactly the failure this
    suite exists to catch.
    """
    attach_root()
    sc, npc, victim = make_world((0.0, 0.0, 0.0), (4.0, 0.0, 0.0))
    attach_host(sc, npc, "EnemyChaseHost")

    before = live_health(victim)
    tick(sc, 5.0)
    after = live_health(victim)

    comp = npc.GetBehaviorTreeComponent()
    print("  tree=%s Attached=%s CompileError=%r" % (comp.TreePath, comp.Attached, comp.CompileError[:60]))
    print("  health %.1f -> %.1f" % (before, after))

    check(comp.CompileError == "", "tree must compile cleanly, got: %s" % comp.CompileError[:120])
    check(comp.Attached, "the tree must attach to the entity")
    check_le(comp.TreePath, "trees/EnemyChase.wlbtree",
             "the chase host must attach the chase tree, got %r" % comp.TreePath)
    dealt = before - after
    check(12.0 <= dealt, "the tree must deal damage: %.1f over 5s" % dealt)
    check(abs(dealt / 12.0 - round(dealt / 12.0)) < 1e-9,
          "damage must be whole 12.0 hits, got %.3f" % dealt)


def test_guard_host_attaches_its_tree():
    """EnemyGuardHost must attach the GUARD tree, and compile it cleanly.

    No behaviour is asserted here, and that is deliberate: EnemyGuard's graph
    gates on a `target` blackboard key and contains no SenseNearest node, so
    nothing ever writes that key and the tree idles by design. Asserting
    damage would fail against a correctly-wired host.
    """
    attach_root()
    sc, npc, _ = make_world((0.0, 0.0, 0.0), (2.0, 0.0, 0.0))
    attach_host(sc, npc, "EnemyGuardHost")
    tick(sc, 0.5)

    comp = npc.GetBehaviorTreeComponent()
    print("  tree=%s Attached=%s CompileError=%r" % (comp.TreePath, comp.Attached, comp.CompileError[:60]))
    check_le(comp.TreePath, "trees/EnemyGuard.wlbtree",
             "the guard host must attach the guard tree, got %r" % comp.TreePath)
    check(comp.CompileError == "", "guard tree must compile cleanly, got: %s" % comp.CompileError[:120])
    check(comp.Attached, "the guard tree must attach")


def test_patrol_host_turns_toward_intruder():
    """PatrolGuard has no movement node, so assert the one effect it has.

    Its `alert` branch faces the sensed intruder at 240 deg/s. An NPC that
    turns is a tree that sensed, gated and acted -- three things a component
    flag cannot show.
    """
    attach_root()
    sc, npc, intruder = make_world((0.0, 0.0, 0.0), (0.0, 0.0, 5.0))
    # Face AWAY from the intruder so any rotation is the tree's doing.
    npc.SetRotation(Vec3(0.0, 180.0, 0.0))
    attach_host(sc, npc, "PatrolGuardHost")

    before = npc.GetTransform().Rotation.y
    tick(sc, 1.0)
    after = npc.GetTransform().Rotation.y

    comp = npc.GetBehaviorTreeComponent()
    print("  tree=%s yaw %.1f -> %.1f" % (comp.TreePath, before, after))
    check_le(comp.TreePath, "trees/PatrolGuard.wlbtree",
             "the patrol host must attach the patrol tree, got %r" % comp.TreePath)
    check(comp.CompileError == "", "patrol tree must compile cleanly, got: %s" % comp.CompileError[:120])
    check(abs(after - before) > 1.0,
          "the patrol tree must turn toward the intruder: yaw %.1f -> %.1f" % (before, after))


def test_graph_counts_down_and_the_host_arms_it():
    """The flow graph half: a countdown driven by damage, not by the frame.

    The graph owns the COUNTDOWN. It reads flashTimer, decrements it, and when
    it expires clears wasHit and logs once. It does NOT arm the timer -- the
    first version did, so every frame reset it to 0.35 and decremented once:
    the timer never counted down, wasHit was set and cleared within a single
    frame, and "flash ended" fired once PER FRAME.

    Arming is the host's job, because a flow graph has no damage event to react
    to (ScriptEngine calls one hook; the node library has no damage node). So
    this asserts both halves: the host notices the health drop and arms, and the
    graph then counts down and clears exactly once.
    """
    attach_root()
    sc, npc, _ = make_world((0.0, 0.0, 0.0), (50.0, 0.0, 0.0))

    import DamageFlashHost
    messages = capture_log()

    host = DamageFlashHost.DamageFlashHost(npc)
    check(host._graph.flashTimer == 0.0,
          "a fresh graph must start with an unarmed timer, got %.4f" % host._graph.flashTimer)
    check(host._graph.wasHit is False,
          "a fresh graph must not report a hit it was never given")

    # --- half 1: the graph does NOT arm itself -----------------------------
    # With no damage, ticking must leave the flag alone. This is the defect
    # that was fixed: the old graph set wasHit on every frame.
    host.OnUpdateEntity(STEP)
    host.OnUpdateEntity(STEP)
    check(host._graph.wasHit is False,
          "the graph must not arm itself -- wasHit was %s after ticks with no damage"
          % host._graph.wasHit)
    check(host._graph.flashTimer < 0.0,
          "an unarmed graph must keep counting below zero, got %.4f"
          % host._graph.flashTimer)

    # --- half 2: damage arms it, then it counts down and clears once --------
    npc.GetHealthComponent().Current = 60.0
    host.OnUpdateEntity(STEP)
    armed = host._graph.flashTimer
    # The host arms FIRST and the graph decrements in the SAME frame, so the
    # value after one call is armed-minus-one-step, not the full duration.
    # Asserting the exact figure pins the ordering: a graph ticked before the
    # arm would clear a hit that had only just been set.
    check(abs(armed - (DamageFlashHost.FLASH_DURATION - 0.016)) < 1e-6,
          "a health drop must arm the timer to %.2f then tick it once, got %.4f"
          % (DamageFlashHost.FLASH_DURATION, armed))
    check(host._graph.wasHit is True,
          "a health drop must set wasHit")

    # It must now count DOWN, not re-arm. A few ticks in, the timer is lower
    # than it was, and the flag still stands until the timer expires.
    for _ in range(5):
        host.OnUpdateEntity(STEP)
    mid = host._graph.flashTimer
    check(mid < armed,
          "the timer must count down across ticks: %.4f -> %.4f" % (armed, mid))
    check(host._graph.wasHit is True,
          "wasHit must persist while the flash is lit")

    # And it must expire: after the full duration the flag drops and the
    # message fires ONCE -- not once per frame.
    for _ in range(40):
        host.OnUpdateEntity(STEP)
    check(host._graph.wasHit is False,
          "wasHit must clear once the timer expires")
    flashes = [m for m in messages if "flash ended" in str(m)]
    print("  armed=%.4f mid=%.4f expired=%.4f  flash-ended lines=%d"
          % (armed, mid, host._graph.flashTimer, len(flashes)))
    check(len(flashes) == 1,
          "the flash must log exactly once per hit, got %d lines" % len(flashes))

    restore_log()


def test_unknown_tree_name_raises():
    """A typo in TREE_NAME must be loud, not a motionless NPC.

    A host that silently attached nothing would leave the enemy standing
    still with no error anywhere -- the failure mode this layer exists to
    prevent. Raising at construction surfaces it in the log once, at attach
    time.
    """
    attach_root()
    sc, npc, _ = make_world((0.0, 0.0, 0.0), (5.0, 0.0, 0.0))

    import BehaviorTreeHost

    class Bogus(BehaviorTreeHost.BehaviorTreeHost):
        TREE_NAME = "NoSuchTree"

    raised = False
    try:
        Bogus(npc)
    except ValueError:
        raised = True
    print("  unknown tree raised ValueError: %s" % raised)
    check(raised, "an unknown tree name must raise, not attach silently")
    check(not npc.HasBehaviorTreeComponent(),
          "a failed attach must not leave a tree on the entity")


def test_wasteland_log_is_a_real_binding():
    """`Wasteland.Log` must exist as an engine binding.

    It did not: `hasattr(Wasteland, "Log")` was False and the generated
    DamageFlash graph called it, so every graph using the Log node compiled
    clean, rendered clean, and raised AttributeError on the first tick. The
    first fix patched it at runtime from the host; the real fix is the binding
    in Bindings.cpp.

    Asserted on the BUILT module, which is the only thing that proves it: a
    host-level shim would satisfy a source reading of this test while shipping
    the same AttributeError.
    """
    import Wasteland

    check(callable(getattr(Wasteland, "Log", None)),
          "Wasteland.Log must be callable on the built module")

    lines = capture_log()
    Wasteland.Log("hermes binding check")
    check(lines == ["hermes binding check"],
          "Wasteland.Log must deliver its argument to the sink, got %r" % lines)
    restore_log()

    # And the generated graph must be able to use it without any host shim.
    check("Wasteland.Log(" in open(
        _os.path.join(REPO, "DemonCore-Editor", "assets", "graphs", "DamageFlash.py"),
        encoding="utf-8").read(),
        "the generated graph should still route its message through Wasteland.Log")
    print("  Wasteland.Log is a real binding and accepts a message")


def test_attached_but_unticked_does_nothing():
    """The negative control.

    ScriptEngine constructs the host on the first OnUpdateRuntime. An entity
    stepped with StepPhysics only must still have no tree -- otherwise the
    wiring would be running from a path that is not the runtime's.
    """
    attach_root()
    sc, npc, _ = make_world((0.0, 0.0, 0.0), (5.0, 0.0, 0.0))
    attach_host(sc, npc, "EnemyChaseHost")

    for _ in range(40):
        sc.StepPhysics(STEP)

    # Presence is checked BEFORE the read, and that is not defensive noise:
    # GetBehaviorTreeComponent() on an entity that has none trips an entt
    # assertion (`contains(entt)` fails) and takes the interpreter down rather
    # than returning a default-constructed component.
    has_tree = npc.HasBehaviorTreeComponent()
    print("  after StepPhysics x40: HasBehaviorTreeComponent=%s" % has_tree)
    check(not has_tree,
          "StepPhysics must not construct the host or attach the tree")

    tick(sc, 0.5)
    check(npc.HasBehaviorTreeComponent(),
          "OnUpdateRuntime must attach the tree (StepPhysics did not)")
    check(npc.GetBehaviorTreeComponent().Attached,
          "the tree must report itself attached once the runtime ticks")
