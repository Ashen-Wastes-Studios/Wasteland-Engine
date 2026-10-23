# Wasteland Engine Script — behaviour-tree host
#
# The three generated classes in assets/trees/ -- EnemyChase, EnemyGuard and
# PatrolGuard -- are compiled from .wlbtree graphs by
# VisualBlocks::Compiler::CompileBehavior. Each is a plain Python class with
# a `tick(seconds)` method and NO OnUpdateEntity, which is the ONE hook
# ScriptEngine calls per frame.
#
# A behaviour tree is not attached by a ScriptComponent. It is attached by a
# BEHAVIORTREECOMPONENT, which BehaviorTreeSystem owns: it compiles the
# .wlbtree, execs the emitted source into a per-entity module, constructs the
# class with `(entity, &scene)` and calls `tick(seconds)`. That path is
# verified working end to end (tests/headless/test_tree_attach.py).
#
# SO WHAT IS THIS FILE FOR?
#
# The deserializer has NO BehaviorTreeComponent block. A
# `BehaviorTreeComponent:` key in a .wastescene is silently ignored on load
# -- verified: an entity saved with one, reloaded, comes back with
# `HasBehaviorTreeComponent()` False. Worse, Serialize never emits the
# component either, so an editor session that saves the scene DROPS the tree
# the scene file asked for. A YAML-only attach is therefore not durable.
#
# This host is the durable attach: a normal ScriptComponent, which IS
# deserialized, whose script puts the tree on the entity at construction.
# ScriptEngine constructs it on the first runtime tick, the script calls
# AddOrReplaceBehaviorTreeComponent, and BehaviorTreeSystem picks the tree up
# on the SAME frame (it ticks after scripts by design).
#
# WHY NOT ONE HOST PER TREE
#
# Three near-identical files would be three places for a path typo to hide.
# The tree is chosen by TREE_NAME, so one host covers all of them and the
# scene file states which tree it wants.
#
# Requirements on the entity (unchanged from Enemy.py):
#   - HealthComponent      (so it can be sensed, and so it can be damaged)
#   - RigidBody3DComponent (Kinematic, so the tree's movement carries the body)
#   - BoxCollider3DComponent or SphereCollider3DComponent

import Wasteland

# The authored tree, project-relative and WITHOUT the leading "assets/".
# BehaviorTreeSystem::CompileTree passes this to Project::GetAssetPath, which
# accepts both forms; this is the form the inspector shows.
TREES = {
    "EnemyChase": "trees/EnemyChase.wlbtree",
    "EnemyGuard": "trees/EnemyGuard.wlbtree",
    "PatrolGuard": "trees/PatrolGuard.wlbtree",
}

def attach_tree(entity, tree_name):
    """Put an authored .wlbtree on `entity`. Returns False when not attached.

    AddOrReplace makes this idempotent, so a scene that loads the same script
    twice does not end up with two trees fighting over the transform.

    Raises on an unknown name rather than returning False: a typo here is a
    silent inert NPC, which is the exact failure this layer exists to prevent.
    """
    path = TREES.get(tree_name)
    if path is None:
        raise ValueError(
            "BehaviorTreeHost: unknown tree %r (known: %s)"
            % (tree_name, ", ".join(sorted(TREES))))

    if not entity.IsValid():
        return False

    tree = Wasteland.BehaviorTreeComponent()
    tree.TreePath = path
    # TreeName is an OPTIONAL author override; left empty the runtime derives
    # the class name from the graph's own `name:` field, which is what the
    # compiler emitted. Setting it to the wrong value is only detected later as
    # "compiled but emitted no class", so it is left empty on purpose.
    tree.TreeName = ""
    entity.AddOrReplaceBehaviorTreeComponent(tree)
    return True


class BehaviorTreeHost:
    """Attaches ONE authored .wlbtree to the entity it is constructed on.

    Which tree is chosen by the TREE_NAME class attribute, so one base class
    covers all of them and the scene file states which tree it wants.
    scripts/EnemyHost.py subclasses this to pair a tree with a flow graph on
    the same entity -- an entity carries only ONE ScriptComponent, so a tree
    and a graph cannot be attached as two separate scripts.
    """

    # Which tree this host attaches. A subclass overrides this; the base class
    # is abstract in practice and must not be attached on its own.
    TREE_NAME = ""

    def __init__(self, entity):
        self.entity = entity
        if not self.TREE_NAME:
            raise ValueError(
                "BehaviorTreeHost: TREE_NAME is empty; attach a subclass")
        attach_tree(entity, self.TREE_NAME)

    def OnUpdateEntity(self, dt):
        """Deliberately empty.

        The tree ticks from Scene::OnUpdateRuntime, which runs
        BehaviorTreeSystem AFTER scripts -- so anything done here would move
        the enemy once and then have the tree move it again, every frame.

        The method exists only so ScriptEngine finds a hook and does not warn
        about a missing one on every single frame.
        """
        return
