# Wasteland Engine Script — enemy host (tree + damage flash)
#
# Wires all four generated Visual Blocks files onto the three enemies in
# DemoGame.wastescene:
#
#   Enemy1 -> trees/EnemyChase.py   (compiled from EnemyChase.wlbtree)
#   Enemy2 -> trees/EnemyGuard.py   (compiled from EnemyGuard.wlbtree)
#   Enemy3 -> trees/PatrolGuard.py  (compiled from PatrolGuard.wlbtree)
#   all three -> graphs/DamageFlash.py (compiled from DamageFlash.wlblocks)
#
# WHY ONE SCRIPT PER ENEMY RATHER THAN FOUR
#
# An entity carries exactly ONE ScriptComponent. The deserializer reads a
# single `ScriptComponent:` block per entity and calls AddComponent<>
# unconditionally, so a second block would REPLACE the first rather than
# stack. A tree and a graph therefore cannot be attached as two separate
# scripts on one entity -- they have to share one host.
#
# That is what this file is: a host that attaches a tree (BehaviorTreeHost)
# and runs a flow graph (DamageFlashHost) on the same entity.
#
# WHY THE CLASS NAMES ARE *Host AND NOT THE GENERATED NAMES
#
# ScriptEngine imports the module by the file's stem and then looks up the
# class by ScriptComponent.ScriptName. The module here is `EnemyHost`, so
# `EnemyChaseHost` is unambiguous and cannot be confused with the generated
# `EnemyChase` class living in the trees/EnemyChase module.

import BehaviorTreeHost
import DamageFlashHost


class _EnemyHost(BehaviorTreeHost.BehaviorTreeHost):
    """A tree plus the damage-flash graph, on one entity.

    The graph is constructed once and its event method called once per frame,
    which is what makes its `variables:` persist. Constructing a fresh
    instance per tick would reset flashTimer to 0.0 every frame and the flash
    would never complete.
    """

    def __init__(self, entity):
        # The tree first: BehaviorTreeSystem ticks trees AFTER scripts, so the
        # tree sees the transform as it stood at the end of this frame's
        # script pass either way, but attaching first keeps the failure mode
        # obvious -- a bad tree path raises here, before the graph exists.
        super().__init__(entity)
        # DamageFlashHost is itself a ScriptEngine-shaped host (it exposes
        # OnUpdateEntity and owns a graph instance), so composing it is just
        # delegation rather than a second attachment.
        self._flash = DamageFlashHost.DamageFlashHost(entity)

    def OnUpdateEntity(self, dt):
        # The tree needs nothing here -- see BehaviorTreeHost.OnUpdateEntity.
        # The graph does: its event method is what advances flashTimer.
        self._flash.OnUpdateEntity(dt)


class EnemyChaseHost(_EnemyHost):
    """Enemy1: acquire a target, chase it, hit it on an interval."""
    TREE_NAME = "EnemyChase"


class EnemyGuardHost(_EnemyHost):
    """Enemy2: hold position, engage what comes close."""
    TREE_NAME = "EnemyGuard"


class PatrolGuardHost(_EnemyHost):
    """Enemy3: sweep for intruders, face them, hold the post."""
    TREE_NAME = "PatrolGuard"
