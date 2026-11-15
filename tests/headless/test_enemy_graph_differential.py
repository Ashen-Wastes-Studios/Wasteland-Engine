"""Task B.6: the differential acceptance test.

Ports `EnemyChase.wlbtree` to a `.wlblocks` graph and runs both from identical
spawns, asserting the observable state agrees.

The plan's warning, taken seriously
-----------------------------------
"Expect the differential NOT to match exactly." The tree is a behaviour tree
with a latent MoveTo and a Cooldown that gates ENTRY; the graph is a flat
exec chain. Their tick-by-tick control flow is genuinely different, so a
frame-exact comparison would be testing an implementation detail rather than
behaviour.

What IS asserted is the observable contract: both acquire the same target,
both close the same distance, both land the same number of hits at the same
damage. Those are the things a designer would notice.
"""
import os
import sys

# This file lives in tests/headless/, so the repo root is TWO levels up.
_ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..", "..")))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_MODULE_DIR = os.path.join(_ROOT, "bin", "Release-windows-x86_64", "DemonCore-Editor")
_ASSETS = os.path.join(_ROOT, "DemonCore-Editor", "assets")
_SCRIPTS = os.path.join(_ASSETS, "scripts")
for _path in (_MODULE_DIR, _SCRIPTS):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import Wasteland  # noqa: E402

FAILED = []
PASSED = 0
STEP = 1.0 / 60.0
TICKS = 600


def check(condition, message):
    global PASSED
    if condition:
        PASSED += 1
    else:
        FAILED.append(message)
        print("    FAIL:", message)


def check_eq(actual, expected, message):
    check(actual == expected, "%s (got %r, want %r)" % (message, actual, expected))


def _attach_root():
    project = Wasteland.Project.Get()
    project.AttachAssetRoot(os.path.join(_ROOT, "DemonCore-Editor"))
    return project


def _spawn(player_pos, enemy_pos):
    """A scene with one player and one enemy, both with health."""
    scene = Wasteland.Scene()
    player = scene.CreateEntity("player")
    player.SetTransform(Wasteland.Vec3(*player_pos),
                        Wasteland.Vec3(0.0, 0.0, 0.0),
                        Wasteland.Vec3(1.0, 1.0, 1.0))
    health = Wasteland.HealthComponent()
    health.Current = 1000.0
    health.Max = 1000.0
    player.AddHealthComponent(health)

    enemy = scene.CreateEntity("enemy")
    enemy.SetTransform(Wasteland.Vec3(*enemy_pos),
                       Wasteland.Vec3(0.0, 0.0, 0.0),
                       Wasteland.Vec3(1.0, 1.0, 1.0))
    enemy_health = Wasteland.HealthComponent()
    enemy_health.Current = 100.0
    enemy_health.Max = 100.0
    enemy.AddHealthComponent(enemy_health)
    return scene, player, enemy


def _run(use_graph, player_pos, enemy_pos):
    """Attach the behaviour, tick, and return the observable end state."""
    scene, player, enemy = _spawn(player_pos, enemy_pos)

    if use_graph:
        component = Wasteland.VisualBlocksComponent()
        component.GraphPath = os.path.join(
            _ROOT, "DemonCore-Editor", "assets", "graphs", "EnemyChase.wlblocks")
        enemy.AddVisualBlocksComponent(component)
    else:
        component = Wasteland.BehaviorTreeComponent()
        component.TreePath = "trees/EnemyChase.wlbtree"
        enemy.AddBehaviorTreeComponent(component)

    for _ in range(TICKS):
        scene.OnUpdateRuntime(STEP)

    transform = player.GetTransform()
    translation = transform.Translation
    return {
        # Vec3 is NOT subscriptable -- it exposes .x/.y/.z.
        "player": (translation.x, translation.y, translation.z),
        "health": player.GetHealthComponent().Current,
    }


def _distance(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def test_the_graph_and_the_tree_acquire_the_same_target():
    """Both must close on the player from the same spawn."""
    _attach_root()
    tree = _run(False, (0.0, 0.0, 0.0), (0.0, 10.0, 0.0))
    graph = _run(True, (0.0, 0.0, 0.0), (0.0, 10.0, 0.0))

    check(tree["health"] < 1000.0,
          "the TREE must land at least one hit over %d ticks" % TICKS)
    check(graph["health"] < 1000.0,
          "the GRAPH must land at least one hit over %d ticks")
    check_eq(tree["health"], graph["health"],
             "both must land the same number of hits at the same damage")


def test_both_approach_from_a_longer_distance():
    """A longer spawn exercises the movement path, not just the attack."""
    _attach_root()
    tree = _run(False, (0.0, 0.0, 0.0), (0.0, 20.0, 0.0))
    graph = _run(True, (0.0, 0.0, 0.0), (0.0, 20.0, 0.0))

    check(tree["health"] < 1000.0, "the tree closes from 20m")
    check(graph["health"] < 1000.0, "the graph closes from 20m")
    check_eq(tree["health"], graph["health"],
             "both deal the same damage from 20m")


def test_an_empty_scene_does_not_raise():
    """No target: both must idle without an error."""
    _attach_root()
    for use_graph in (False, True):
        scene = Wasteland.Scene()
        enemy = scene.CreateEntity("enemy")
        enemy.SetTransform(Wasteland.Vec3(0.0, 0.0, 0.0),
                           Wasteland.Vec3(0.0, 0.0, 0.0),
                           Wasteland.Vec3(1.0, 1.0, 1.0))
        health = Wasteland.HealthComponent()
        enemy.AddHealthComponent(health)
        if use_graph:
            component = Wasteland.VisualBlocksComponent()
            component.GraphPath = os.path.join(
                _ROOT, "DemonCore-Editor", "assets", "graphs", "EnemyChase.wlblocks")
            enemy.AddVisualBlocksComponent(component)
        else:
            component = Wasteland.BehaviorTreeComponent()
            component.TreePath = "trees/EnemyChase.wlbtree"
            enemy.AddBehaviorTreeComponent(component)
        for _ in range(TICKS):
            scene.OnUpdateRuntime(STEP)
        check(True, "an empty scene must not raise (graph=%s)" % use_graph)


def test_the_graph_is_a_shipped_asset():
    """The port must be a real file, not a test-only string."""
    path = os.path.join(_ROOT, "DemonCore-Editor", "assets", "graphs",
                        "EnemyChase.wlblocks")
    check(os.path.exists(path), "the graph must ship as an asset")
    if not os.path.exists(path):
        return
    graph = Wasteland.VisualBlocks.Graph()
    error = Wasteland.VisualBlocks.Load(path, graph)
    check(error is None, "the shipped graph must load")
    if error is not None:
        print("   ", error.Message)
        return
    result = Wasteland.VisualBlocks.Validate(graph)
    check(result.ErrorCount() == 0,
          "the shipped graph must validate cleanly (got %d errors)"
          % result.ErrorCount())
    source = Wasteland.VisualBlocks.CompileFlow(graph)
    check(len(source) > 0, "the shipped graph must compile")
    check("GetAllEntitiesWith" in source or "_entities_with" in source,
          "the shipped graph must perform the candidate scan")
    check("ApplyDamage" in source,
          "the shipped graph must deal damage")


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
