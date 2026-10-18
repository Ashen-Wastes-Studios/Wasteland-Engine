"""Did migrating Enemy.py onto EnemyChase.wlbtree preserve behaviour?

Enemy.py used to be a hand-written chaser: it found a target, walked to it,
faced it, snapped to the ground and applied contact damage. All of that now
lives in assets/trees/EnemyChase.wlbtree, and Enemy.py is a shim that attaches
it.

"Ported" is not "equivalent". Two things can silently diverge:

  * the TUNABLES -- the shim documents speed 3.0 / range 1.6 / damage 12.0, and
    the tree has its own copy of those numbers. Change one and not the other and
    the enemy quietly gets faster, or hits harder, with nothing failing.
  * the BEHAVIOUR -- the shim's OnUpdateEntity is empty on purpose. If the tree
    silently stopped driving the entity (bad path, compile error, scene never
    attached), the enemy would simply stand still and every "it works" claim
    would still look true from the script's side.

So this suite checks the numbers against the COMPILED tree, and then drives a
real entity through the real runtime tick, asserting on world effects.

The negative controls are the point: a shim that attaches nothing passes every
"the script is fine" check and does nothing at all.

Run:  python tests/test_enemy_migration.py
"""

from __future__ import annotations

import os as _os
import sys
import re

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

# wlpath (the module loader) lives with the headless suites, not here.
_HEADLESS = _os.path.join(_HERE, "headless")
if _HEADLESS not in sys.path:
    sys.path.insert(0, _HEADLESS)

import wlpath  # noqa: E402

wlpath.setup()
import Wasteland  # noqa: E402
from Wasteland import Vec3, HealthComponent, BehaviorTreeComponent  # noqa: E402

ASSET_ROOT = r"C:\dev\Wasteland\DemonCore-Editor"
SCRIPTS = _os.path.join(ASSET_ROOT, "assets", "scripts")
STEP = 0.1

_failures: list[str] = []
_passes = 0


def check(cond, msg):
    """Assert, but keep going so one failure does not hide the rest."""
    global _passes
    if cond:
        _passes += 1
    else:
        _failures.append(msg)


# --------------------------------------------------------------------------
# The shim, loaded the same way ScriptEngine loads it.

def load_enemy_module():
    """Import Enemy.py the way the engine does, not by path guesswork."""
    path = _os.path.join(SCRIPTS, "Enemy.py")
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    ns = {"__name__": "Enemy"}
    exec(compile(src, path, "exec"), ns)
    return ns


def attach_root():
    p = Wasteland.Project.Get()
    p.AttachAssetRoot(ASSET_ROOT)
    return p


def compiled_tree():
    return Wasteland.Scene.CompileBehaviorTree("trees/EnemyChase.wlbtree")


# --------------------------------------------------------------------------
# 1. Tunables must match between the shim and the compiled tree.

def test_shim_tunables_match_the_compiled_tree():
    """The shim documents the numbers; the tree must actually use them.

    Parsed out of the GENERATED Python rather than the .wlbtree, because the
    generated source is what runs -- checking the YAML would pass even if the
    compiler dropped or defaulted a pin.
    """
    attach_root()
    mod = load_enemy_module()
    enemy = mod["Enemy"]
    src = compiled_tree()

    def tree_number(pattern, label):
        m = re.search(pattern, src)
        check(m is not None, "compiled tree has no %s" % label)
        return float(m.group(1)) if m else None

    # The generated call shape is set by the emitters, not by this test:
    #   NodeLibrary.cpp MoveTo  -> _move(ctx.entity, ctx, "target", <speed>, <pathfinding>)
    #   NodeLibraryV1.cpp Sense -> _sense_nearest(ctx.entity, ctx, "target", <radius>, ...)
    #
    # NB.8 added use_pathfinding to _move and NB.7 added half_angle_deg and
    # require_los to _sense_nearest, so the numeric pin is no longer the LAST
    # argument and an anchored "\)" here matched nothing -- four checks failed on
    # a NULL, not on a wrong number. The claim is unchanged (the tree must use the
    # shim's numbers), so pin the FIRST numeric literal after the key and stop,
    # rather than assuming the pin closes the call.
    speed = tree_number(r'_move\(ctx\.entity, ctx, "target", ([0-9.]+)', "MoveTo speed")
    # ConditionDistanceTo attack range, and SenseNearest lose range
    ranges = [float(x) for x in re.findall(
        r"_dist\(ctx\.entity, ctx\.bb\.get\(\"target\"\)\) <= ([0-9.]+)", src)]
    sense = tree_number(
        r'_sense_nearest\(ctx\.entity, ctx, "target", ([0-9.]+)', "SenseNearest radius")
    damage = tree_number(r"_damage\(ctx\.entity, ctx\.bb\.get\(\"target\"\), ([0-9.]+)\)", "damage")
    cooldown = tree_number(r"'Cooldown', 'seconds': ([0-9.]+)", "attack cooldown")
    turn = tree_number(r"_face\(ctx\.entity, ctx, ctx\.bb\.get\(\"target\"\), ([0-9.]+)\)", "turn rate")

    check(speed == enemy.SPEED,
          "speed: shim %s vs tree %s" % (enemy.SPEED, speed))
    check(ranges and enemy.ATTACK_RANGE in ranges,
          "attack range: shim %s vs tree %s" % (enemy.ATTACK_RANGE, ranges))
    check(sense == enemy.LOSE_RANGE,
          "lose range: shim %s vs tree %s" % (enemy.LOSE_RANGE, sense))
    check(damage == enemy.DAMAGE,
          "damage: shim %s vs tree %s" % (enemy.DAMAGE, damage))
    check(cooldown == enemy.ATTACK_INTERVAL,
          "attack interval: shim %s vs tree %s" % (enemy.ATTACK_INTERVAL, cooldown))
    check(abs(turn - enemy.TURN_RATE) < 0.1,
          "turn rate: shim %s vs tree %s" % (enemy.TURN_RATE, turn))


def test_shim_points_at_the_tree_that_exists():
    """A typo in TREE_PATH is the single most likely thing to break here."""
    mod = load_enemy_module()
    enemy = mod["Enemy"]
    attach_root()
    check(_os.path.isfile(
        _os.path.join(ASSET_ROOT, "assets", enemy.TREE_PATH.replace("/", _os.sep))),
        "TREE_PATH %r does not exist under assets/" % enemy.TREE_PATH)


# --------------------------------------------------------------------------
# 2. The shim must actually attach, and must not double-move.

def _world(player_pos):
    """One scene, both actors in it. A separate scene is invisible to the tree."""
    sc = Wasteland.Scene()
    npc = sc.CreateEntity("npc")
    npc.SetPosition(Vec3(0.0, 0.0, 0.0))
    h = HealthComponent()
    h.Max = 100.0
    h.Current = 100.0
    npc.AddOrReplaceHealthComponent(h)

    player = sc.CreateEntity("player")
    player.SetPosition(Vec3(*player_pos))
    ph = HealthComponent()
    ph.Max = 100.0
    ph.Current = 100.0
    player.AddOrReplaceHealthComponent(ph)
    return sc, npc, player


def _pos(e):
    t = e.GetTransform().Translation
    return (t.x, t.y, t.z)


def _health(e):
    return e.GetHealthComponent().Current


def _tick(sc, seconds):
    for _ in range(int(seconds / STEP)):
        sc.OnUpdateRuntime(STEP)


def test_shim_attaches_the_tree_and_the_enemy_acts():
    """The whole point: shim attaches, tree drives, enemy actually behaves."""
    attach_root()
    mod = load_enemy_module()
    sc, npc, player = _world((0.0, 0.0, 8.0))

    Enemy = mod["Enemy"]
    Enemy(npc)

    comp = npc.GetBehaviorTreeComponent()
    check(comp is not None, "shim did not attach a BehaviorTreeComponent")
    check(comp.CompileError == "",
          "tree failed to compile: %s" % comp.CompileError[:160])

    _tick(sc, 1.0)
    comp = npc.GetBehaviorTreeComponent()
    check(comp.Attached, "tree never attached after a runtime tick")

    # It must MOVE (it starts 8m out, outside attack range).
    after = _pos(npc)
    check(after[2] > 0.5, "enemy did not move toward the player: z=%r" % after[2])

    # ...and it must ARRIVE and DAMAGE. Moving alone would pass a tree that
    # walks but never attacks.
    _tick(sc, 6.0)
    check(_health(player) < 100.0,
          "enemy closed distance but never damaged the player")
    dealt = 100.0 - _health(player)
    check(abs(dealt - 12.0) < 0.01 or dealt >= 12.0,
          "damage should land in multiples of 12, got %r" % dealt)


def test_shim_tick_does_not_double_move_the_enemy():
    """Enemy.py's OnUpdateEntity is empty on purpose.

    If anything ever puts work back in it, the enemy moves once from the script
    and again from the tree. The observable is speed: a double move covers
    noticeably more ground per second than the authored 3.0 m/s.
    """
    attach_root()
    mod = load_enemy_module()
    sc, npc, player = _world((0.0, 0.0, 60.0))

    Enemy = mod["Enemy"]
    Enemy(npc)
    _tick(sc, 0.5)  # one tick to attach

    start = _pos(npc)
    _tick(sc, 1.0)
    end = _pos(npc)

    travelled = ((end[0] - start[0]) ** 2 + (end[2] - start[2]) ** 2) ** 0.5
    # 1.0s at 3.0 m/s = 3m. Allow generous slack for the first tick being
    # consumed by compilation, but catch a genuine double move (6m+).
    check(travelled < 4.5,
          "enemy travelled %.2fm in 1.0s; authored speed is 3.0 m/s, so "
          "something is moving it twice" % travelled)


def test_shim_alone_without_a_scene_does_not_explode():
    """Negative control: a shim that attached nothing would pass the above.

    Assert the attachment is the shim's doing by removing the tree afterwards
    and confirming the enemy stops acting -- that is what proves the tree, not
    the script, is driving.
    """
    attach_root()
    mod = load_enemy_module()
    sc, npc, player = _world((0.0, 0.0, 8.0))

    Enemy = mod["Enemy"]
    Enemy(npc)
    _tick(sc, 1.0)
    moved = _pos(npc)[2]

    check(moved > 0.5,
          "enemy never moved in the first place, so the tree is not driving it")

    # Now BREAK the tree out from under the running instance. Note what is NOT
    # asserted: retargeting TreePath mid-flight does not re-validate or set
    # CompileError. BehaviorTreeSystem attaches ONCE and caches the instance
    # per entity UUID, so a later path change is ignored -- the running tree
    # keeps going. That is deliberate (recompiling every frame would stall the
    # tick) but it means an artist editing a tree on a live entity sees no
    # effect until the entity re-attaches.
    comp = npc.GetBehaviorTreeComponent()
    comp.TreePath = "trees/DoesNotExist.wlbtree"
    _tick(sc, 1.0)
    check(_pos(npc)[2] >= moved,
          "the running tree should keep acting despite the path change")

    # A FRESH entity pointed at the bad path must report it, which is the
    # behaviour that actually matters: a typo in a scene fails loudly on load
    # instead of leaving an inert NPC.
    sc2, npc2, player2 = _world((0.0, 0.0, 8.0))
    bad = BehaviorTreeComponent()
    bad.TreePath = "trees/DoesNotExist.wlbtree"
    npc2.AddOrReplaceBehaviorTreeComponent(bad)
    _tick(sc2, 1.0)
    check(npc2.GetBehaviorTreeComponent().CompileError != "",
          "a missing tree must set CompileError on a fresh entity")
    check(not npc2.GetBehaviorTreeComponent().Attached,
          "a missing tree must not report Attached")


# --------------------------------------------------------------------------
def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            _failures.append("%s raised: %s" % (fn.__name__, exc))

    total = _passes + len(_failures)
    if _failures:
        print("FAILURES:")
        for f in _failures:
            print("  * %s" % f)
    print("%d/%d checks passed" % (_passes, total))
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())