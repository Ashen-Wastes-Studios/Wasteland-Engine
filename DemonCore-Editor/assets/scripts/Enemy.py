# Wasteland Engine Script — Enemy (behaviour-tree shim)
#
# This used to be a hand-written chaser: target search, movement, facing,
# contact damage and ground probe, all inline in OnUpdateEntity. That logic now
# lives in assets/trees/EnemyChase.wlbtree, where a designer can retune it
# without touching Python and the compiler can validate the topology.
#
# The original is kept verbatim as Enemy_legacy_reference.py so this port can be
# diffed against what it replaced.
#
# This file stays because ExampleGame.wastescene references it by
# ScriptPath/ScriptName, and because it is the one place the enemy's TUNABLES
# are written down in a form a human edits and reviews.
#
# Do NOT re-add behaviour below. Anything added here runs IN ADDITION to the
# tree, and the two fight over the same transform: the tree owns position,
# facing and damage, and which writer wins depends on tick order. If the tree
# cannot express something, add a node instead.
#
# Requirements on the entity (unchanged):
#   - HealthComponent      (so weapons can damage it, and so it can be sensed)
#   - RigidBody3DComponent (Kinematic, so movement carries the body along)
#   - BoxCollider3DComponent or SphereCollider3DComponent
#
# The tree does not path around obstacles -- that is the navmesh tier, still on
# the roadmap. This is the "walk straight at you" tier.
import Wasteland as WastelandMod


class Enemy:
    # --- Tunables, mirrored into the tree. ------------------------------
    # Kept here in reviewable Python and asserted against the COMPILED tree by
    # tests/test_enemy_migration.py, so the two cannot drift apart silently.
    SPEED = 3.0            # metres per second while chasing
    ATTACK_RANGE = 1.6     # contact damage lands within this distance
    LOSE_RANGE = 30.0      # beyond this the enemy stops chasing
    DAMAGE = 12.0          # per hit
    ATTACK_INTERVAL = 1.0  # seconds between hits
    TURN_RATE = 343.8      # degrees per second of facing rotation

    # Asset-root relative, NOT relative to this script.
    TREE_PATH = "trees/EnemyChase.wlbtree"
    TREE_NAME = "EnemyChase"

    def __init__(self, entity):
        self.entity = entity
        self._attach_tree()

    def _attach_tree(self):
        """Put the authored tree on this entity.

        AddOrReplace makes this idempotent, so a scene that somehow loads this
        script twice does not end up with two trees fighting over the transform.
        """
        if not self.entity.IsValid():
            return False

        tree = WastelandMod.BehaviorTreeComponent()
        tree.TreePath = self.TREE_PATH
        tree.TreeName = self.TREE_NAME
        self.entity.AddOrReplaceBehaviorTreeComponent(tree)
        return True

    # ------------------------------------------------------------------ tick

    def OnUpdateEntity(self, dt):
        """Deliberately empty.

        The tree ticks from Scene::OnUpdateRuntime, which calls
        BehaviorTreeSystem AFTER scripts -- so anything done here would move the
        enemy once and then have the tree move it again, every frame.

        The method exists only so ScriptEngine finds a hook and does not warn
        about a missing one on every single frame.

        The health guard that used to open this function is now the tree's root
        `dead` branch, checking the same component.
        """
        return