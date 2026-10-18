"""Real per-entity runtime trace integration checks.

This suite requires a freshly rebuilt Wasteland module exposing the trace API.
It drives Scene.OnUpdateRuntime and reads the actual BehaviorTreeSystem log;
it does not mock the runtime or claim to render the editor panel.
"""

from __future__ import annotations

import os as _os
import subprocess as _subprocess
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import (  # noqa: E402
    BehaviorTreeComponent,
    HealthComponent,
    Project,
    Scene,
    Vec3,
)
from runner import check  # noqa: E402

_REPO = _os.path.dirname(_os.path.dirname(_HERE))
_ASSET_ROOT = _os.path.join(_REPO, "DemonCore-Editor")
_TRACE_CAPACITY = 512


def _attach_root() -> None:
    Project.Get().AttachAssetRoot(_ASSET_ROOT)


def _attach_tree(entity, path: str) -> None:
    component = BehaviorTreeComponent()
    component.TreePath = path
    entity.AddOrReplaceBehaviorTreeComponent(component)


def _exercise_runtime_trace_order_abort_bounded_snapshot_and_disable():
    """Capture authored node order, a real abort, and safe bounded readback."""
    _attach_root()

    # EnemyGuard's root declares `dead` first. Its false ConditionBool therefore
    # provides an exact event-order assertion independent of later branch detail.
    guard_scene = Scene()
    guard = guard_scene.CreateEntity("trace-guard")
    _attach_tree(guard, "trees/EnemyGuard.wlbtree")
    check(guard.SetBehaviorTreeTrace(True),
          "trace enable must accept a valid entity before its runtime tree attaches")
    check(guard.IsBehaviorTreeTraceEnabled(),
          "trace enable state must read back for the same entity")
    check(len(guard.GetBehaviorTreeTrace()) == 0,
          "an enabled but not-yet-ticked tree has no fabricated events")

    guard_scene.OnUpdateRuntime(0.1)
    first_tick = guard.GetBehaviorTreeTrace()
    check(len(first_tick) >= 4,
          "one runtime tick must record root and first-child enter/exit events")
    check((first_tick[0].Tick, first_tick[0].NodeId,
           first_tick[0].NodeType, first_tick[0].Event)
          == (1, "root", "Selector", "enter"),
          "the real root-enter event is first in the authored runtime order")
    check((first_tick[1].NodeId, first_tick[1].NodeType,
           first_tick[1].Event)
          == ("dead", "ConditionBool", "enter"),
          "the Selector enters its first declared child before later branches")
    check((first_tick[2].NodeId, first_tick[2].Event)
          == ("dead", "exit:FAILURE"),
          "the actual false condition exits as FAILURE before the next child")
    check(first_tick[-1].NodeId == "root"
          and first_tick[-1].Event.startswith("exit:"),
          "the root exits after all events from its child traversal")

    # A read returns a snapshot, not a reference to the storage that will be
    # advanced by later runtime ticks.
    snapshot_count = len(first_tick)
    for _ in range(200):
        guard_scene.OnUpdateRuntime(0.1)
    bounded = guard.GetBehaviorTreeTrace()
    check(len(bounded) == _TRACE_CAPACITY,
          "per-entity trace storage must retain only its newest 512 events")
    check(len(first_tick) == snapshot_count,
          "previously returned trace snapshots must remain unchanged")
    check(bounded[0].Tick > 1,
          "a full ring must discard older events and preserve chronological order")

    check(guard.SetBehaviorTreeTrace(False),
          "trace disable must detach the callback cleanly")
    retained_count = len(guard.GetBehaviorTreeTrace())
    guard_scene.OnUpdateRuntime(0.1)
    check(not guard.IsBehaviorTreeTraceEnabled(),
          "trace enabled state must read back as disabled")
    check(len(guard.GetBehaviorTreeTrace()) == retained_count,
          "disabling capture preserves history and stops adding events")

    # EnemyChase first suspends its latent MoveTo node; marking the NPC dead makes
    # the root Selector choose its higher-priority branch and emits the runtime's
    # actual abort for that suspended node.
    chase_scene = Scene()
    npc = chase_scene.CreateEntity("trace-chaser")
    npc.SetPosition(Vec3(0.0, 0.0, 0.0))
    npc_health = HealthComponent()
    npc_health.Max = 100.0
    npc_health.Current = 100.0
    npc.AddOrReplaceHealthComponent(npc_health)

    target = chase_scene.CreateEntity("trace-target")
    target.SetPosition(Vec3(5.0, 0.0, 0.0))
    target_health = HealthComponent()
    target_health.Max = 100.0
    target_health.Current = 100.0
    target.AddOrReplaceHealthComponent(target_health)

    _attach_tree(npc, "trees/EnemyChase.wlbtree")
    check(npc.SetBehaviorTreeTrace(True),
          "trace enable must succeed for a second entity independently")
    chase_scene.OnUpdateRuntime(0.1)
    before_abort = npc.GetBehaviorTreeTrace()
    check(any(entry.NodeId == "walk" and entry.Event == "enter"
              for entry in before_abort),
          "the real chase tick must enter its latent MoveTo node")

    live_health = npc.GetHealthComponent()
    live_health.Current = 0.0
    live_health.IsDead = True
    chase_scene.OnUpdateRuntime(0.1)
    after_abort = npc.GetBehaviorTreeTrace()
    abort_indices = [i for i, entry in enumerate(after_abort)
                     if entry.NodeId == "walk" and entry.NodeType == "MoveTo"
                     and entry.Event == "abort"]
    check(len(abort_indices) == 1,
          "preempting the suspended MoveTo must produce one actual abort event")
    if abort_indices:
        abort_index = abort_indices[0]
        check(after_abort[abort_index].Tick == 2,
              "the abort belongs to the second runtime tree tick")
        check(abort_index < len(after_abort) - 1
              and after_abort[-1].NodeId == "root"
              and after_abort[-1].Event == "exit:SUCCESS",
              "abort is recorded before the successful root exit")

    check(npc.SetBehaviorTreeTrace(False),
          "the independent second entity trace can also be disabled")
    guard_scene.DestroyEntity(guard)
    chase_scene.DestroyEntity(npc)
    chase_scene.DestroyEntity(target)


def test_runtime_trace_order_abort_bounded_snapshot_and_disable():
    """Require a real OS child to finish cleanly after ticking behavior trees."""
    child_code = (
        "import runpy, sys; "
        "runpy.run_path(sys.argv[1]) "
        "['_exercise_runtime_trace_order_abort_bounded_snapshot_and_disable']()"
    )
    result = _subprocess.run(
        [_sys.executable, "-c", child_code, __file__],
        cwd=_HERE,
        capture_output=True,
        text=True,
        errors="replace",
    )
    check(
        result.returncode == 0,
        "behavior-tree trace child must exit 0 after preserving all trace "
        f"assertions (exit {result.returncode})\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
    )
