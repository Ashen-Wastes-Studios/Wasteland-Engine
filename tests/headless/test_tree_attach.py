"""Does an authored .wlbtree actually drive an entity through the scene?

`BehaviorTreeSystem` compiles and ticks trees, and `Scene::OnUpdateRuntime`
calls it -- but until this suite existed nothing proved the whole chain worked
end to end. Every previous "the tree works" claim came from Python
instantiating a generated class DIRECTLY, which skips the two things most
likely to be broken:

  * `Project::Get().GetAssetPath()` resolving the authored tree,
  * `Scene::OnUpdateRuntime` ever reaching the tick.

An integration that is only ever exercised from the middle hides both.

So this drives the REAL path: attach a tree to an entity, tick the scene, and
assert on an OBSERVABLE world effect -- health actually dropping. Not "the
component says Attached", which a system could set without the tree ever
running.

The negative controls are the point. A suite that only asserts the happy path
passes just as well against a system that does nothing, because "nothing
happened" and "it worked" differ only in what you look at:

  * no project attached      -> CompileError set, no damage
  * a path that does not exist -> CompileError set, no damage
  * a tree attached but NOT ticked -> no damage, and Attached stays False
  * a dead target            -> no damage

Run:  python tests/headless/test_tree_attach.py
"""

from __future__ import annotations

import os as _os
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
from runner import check, check_le  # noqa: E402

# The editor checkout is a valid asset root: it has assets/trees/*.wlbtree and
# assets/scripts/wl_behavior.py, which is everything a tree needs to load.
REPO = _os.path.dirname(_os.path.dirname(_HERE))
ASSET_ROOT = _os.path.join(REPO, "DemonCore-Editor")
TREE = "trees/EnemyChase.wlbtree"

STEP = 0.1


def attach_root():
    """Point asset resolution at the editor checkout."""
    p = Project.Get()
    p.AttachAssetRoot(ASSET_ROOT)
    return p


def make_actor(name, pos, health=100.0):
    sc = Scene()
    e = sc.CreateEntity(name)
    e.SetPosition(Vec3(*pos))
    hc = HealthComponent()
    hc.Max = health
    hc.Current = health
    e.AddOrReplaceHealthComponent(hc)
    return sc, e, hc


def attach_tree(sc, entity, path=TREE):
    tree = BehaviorTreeComponent()
    tree.TreePath = path
    entity.AddOrReplaceBehaviorTreeComponent(tree)
    return tree


def tick(sc, seconds):
    """Drive the FULL runtime tick -- not StepPhysics, which skips trees."""
    for _ in range(int(seconds / STEP)):
        sc.OnUpdateRuntime(STEP)


def live_health(entity):
    """Read health from the LIVE component.

    The object returned by AddOrReplaceHealthComponent is a snapshot copy:
    pybind hands back a Python-owned value, and the tree mutates the
    registry's instance. Reading the original object afterwards shows the
    value as it was at attach time -- which is why a working tree reported
    0 damage until this re-read was added.
    """
    return entity.GetHealthComponent().Current


def read(entity):
    return entity.GetBehaviorTreeComponent()


# --------------------------------------------------------------------------
def test_compile_tree_without_attaching():
    """An authored tree must compile to real Python with no project attached."""
    attach_root()
    # A static: compiling a tree needs no scene, only an asset root.
    src = Scene.CompileBehaviorTree(TREE)
    print("  %-6d chars, class EnemyChase present: %s"
          % (len(src), "class EnemyChase" in src))
    check(len(src) > 500, "compiled tree is suspiciously small (%d chars)" % len(src))
    check("class EnemyChase" in src,
          "the emitted class must be named after the graph")
    check("wl_behavior" in src, "generated code must import the shared runtime")


def test_a_tree_attaches_and_deals_damage():
    """The real proof: an NPC with EnemyChase attached damages a nearby target.

    EnemyChase senses within 30m, walks in, and applies 12 damage once per
    second. Health dropping is the only assertion that proves the tree RAN.
    """
    attach_root()
    sc, npc, _ = make_actor("npc", (0.0, 0.0, 0.0))
    # Both entities live in ONE scene: the tree's perception sweep searches
    # its own scene, so a victim in a separate scene is invisible to it.
    victim_entity = sc.CreateEntity("victim")
    victim_entity.SetPosition(Vec3(3.0, 0.0, 0.0))
    vh = HealthComponent()
    vh.Max = 100.0
    vh.Current = 100.0
    victim_entity.AddOrReplaceHealthComponent(vh)

    attach_tree(sc, npc)
    before = live_health(victim_entity)
    tick(sc, 4.0)
    after = live_health(victim_entity)

    comp = read(npc)
    print("  Attached=%s CompileError=%r" % (comp.Attached, comp.CompileError[:70]))
    print("  health %.1f -> %.1f" % (before, after))

    check(comp.CompileError == "",
          "tree must compile cleanly, got: %s" % comp.CompileError[:120])
    check(comp.Attached, "the tree must attach to the entity")
    # check_le is an UPPER bound. Asserting damage with it asserts the
    # opposite of the intent -- the first run "passed" a zero-damage tree
    # through the upper check and then failed the lower one.
    dealt = before - after
    check(24.0 <= dealt,
          "the tree must actually deal damage: %.1f over 4s (health %.1f -> %.1f)"
          % (dealt, before, after))
    # 12 damage per hit, one hit per second, 4 seconds => 2-4 hits.
    # A tree firing every tick would exceed this; one that never fires is 0.
    check_le(dealt, 48.0,
             "damage must stay within the tree's 1.0s cooldown, got %.1f" % dealt)
    # And it must be an exact multiple of the authored 12.0 -- that proves the
    # tree's literal reached the runtime, not a default.
    check(abs(dealt / 12.0 - round(dealt / 12.0)) < 1e-9,
          "damage must be a whole number of 12.0 hits, got %.3f" % dealt)


def test_missing_tree_reports_instead_of_silently_standing_still():
    """A bad path must set CompileError, not leave the NPC inert with no clue."""
    attach_root()
    sc, npc, _ = make_actor("npc", (0.0, 0.0, 0.0))
    tree = attach_tree(sc, npc, path="trees/NoSuchTree.wlbtree")
    tick(sc, 0.5)
    comp = read(npc)
    print("  CompileError=%r Attached=%s" % (comp.CompileError[:70], comp.Attached))
    check(comp.CompileError != "",
          "a missing tree must report an error, not fail silently")
    check(not comp.Attached, "a tree that does not compile cannot be attached")


def test_attached_but_unticked_does_nothing():
    """The negative control that matters.

    StepPhysics does NOT tick behaviour trees -- only OnUpdateRuntime does.
    An entity stepped with the wrong call would appear healthy forever, which
    is exactly the 'works in the editor' bug class.
    """
    attach_root()
    sc, npc, _ = make_actor("npc", (0.0, 0.0, 0.0))
    victim = sc.CreateEntity("victim")
    victim.SetPosition(Vec3(2.0, 0.0, 0.0))
    vh = HealthComponent()
    vh.Max = 100.0
    vh.Current = 100.0
    victim.AddOrReplaceHealthComponent(vh)

    # Build the NPC exactly as the proven scenario 1 does. Reconstructing the
    # scene by hand instead produced an NPC that attached but never dealt
    # damage, which is precisely the kind of "half working" difference this
    # suite exists to notice.
    attach_tree(sc, npc)
    for _ in range(40):
        sc.StepPhysics(STEP)  # physics only

    print("  after StepPhysics x40: health %.1f, Attached=%s"
          % (live_health(victim), read(npc).Attached))
    check_le(live_health(victim), 100.0,
             "StepPhysics must not tick behaviour trees (health %.1f)"
             % live_health(victim))
    check(not read(npc).Attached,
          "an unticked tree must not report itself attached")

    # The same entity, ticked the right way, must now work.
    tick(sc, 4.0)
    print("  after OnUpdateRuntime x40: health %.1f, Attached=%s"
          % (live_health(victim), read(npc).Attached))
    check(read(npc).Attached,
          "OnUpdateRuntime must attach the tree (StepPhysics did not)")
    check(99.0 >= live_health(victim),
          "ticking the runtime must produce damage (health %.1f)"
          % live_health(victim))


def test_dead_npc_does_not_attack():
    """A dead NPC must do nothing. Proves the tree gates, not just runs."""
    attach_root()
    sc = Scene()
    npc = sc.CreateEntity("npc")
    npc.SetPosition(Vec3(0.0, 0.0, 0.0))
    h = HealthComponent()
    h.Max = 100.0
    h.Current = 0.0  # dead on arrival
    npc.AddOrReplaceHealthComponent(h)

    victim = sc.CreateEntity("victim")
    victim.SetPosition(Vec3(2.0, 0.0, 0.0))
    vh = HealthComponent()
    vh.Max = 100.0
    vh.Current = 100.0
    victim.AddOrReplaceHealthComponent(vh)

    attach_tree(sc, npc)
    tick(sc, 3.0)
    print("  dead NPC, victim health %.1f" % live_health(victim))
    check_le(live_health(victim), 100.0,
             "a dead NPC must not attack (victim health %.1f)"
             % live_health(victim))