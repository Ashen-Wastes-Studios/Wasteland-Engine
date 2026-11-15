"""Does a `.wlblocks` graph attach to an entity like a script or a tree?

The attach spine is the thing the whole parity plan hangs off: one component
model for every authored behaviour. `ScriptComponent` points at a `.py` and
`BehaviorTreeComponent` points at a `.wlbtree`; a `.wlblocks` flow graph has
neither, so today it can only run behind a hand-written host `.py`
(`assets/scripts/DamageFlashHost.py`), which is the defect this suite exists to
close.

Two things are asserted here, and the second is the one that matters:

  * the component attaches and reads back -- trivially satisfied by any struct,
  * it survives a scene save/load round trip -- which is what makes it a real
    scene component rather than a runtime-only flag.

The round trip is included because the sibling `BehaviorTreeComponent` has NO
serialize/deserialize block at all: a scene saved with a tree attached loses the
tree on load. A new component copied from it would inherit the same hole, so
this suite covers both and fails until both are closed.

The negative controls are the point. A suite that only asserts the happy path
passes just as well against a component that does nothing:

  * an entity without the component -> HasComponent false, Get returns default
  * a saved scene with no such component -> the key is absent, not an error

Run:  python tests/headless/runner.py test_component_attach
"""

from __future__ import annotations

import os as _os
import sys as _sys
import tempfile

_HERE = _os.path.dirname(_os.path.abspath(__file__))
if _HERE not in _sys.path:
    _sys.path.insert(0, _HERE)

import wlpath

wlpath.setup()

from Wasteland import (  # noqa: E402
    BehaviorTreeComponent,
    Project,
    Scene,
    VisualBlocksComponent,
)
from runner import check, check_eq  # noqa: E402

# The editor checkout is a valid asset root: it has assets/graphs/*.wlblocks
# and assets/trees/*.wlbtree, which is everything these components resolve.
REPO = _os.path.dirname(_os.path.dirname(_HERE))
ASSET_ROOT = _os.path.join(REPO, "DemonCore-Editor")
GRAPH = "graphs/DamageFlash.wlblocks"
TREE = "trees/EnemyChase.wlbtree"


def attach_root():
    """Point asset resolution at the editor checkout."""
    p = Project.Get()
    p.AttachAssetRoot(ASSET_ROOT)
    return p


def make_actor(name):
    sc = Scene()
    return sc, sc.CreateEntity(name)


def scene_file(name):
    """A scratch .wastescene path, removed if a previous run left one."""
    path = _os.path.join(tempfile.gettempdir(), name)
    if _os.path.exists(path):
        _os.remove(path)
    return path


def only_entity(scene):
    """The scene's single entity, or None.

    Lookup is by enumeration rather than by tag: `TagComponent`'s accessors go
    through BIND_COMP_*, which stringifies the fully-qualified C++ type, so
    `HasTagComponent` does not exist in Python -- the engine-wide macro bug that
    also forces the parallel BIND_GAMEPLAY_COMP block. Every scene in this suite
    holds exactly one entity, which makes the first entry unambiguous; a scene
    with more would need a real tag accessor, and that gap is worth closing
    separately.
    """
    for entity in scene.GetAllEntitiesWithTransform():
        return entity
    return None


# --------------------------------------------------------------------------
def test_visual_blocks_component_attaches():
    """The component attaches and reads back through the ENTITY.

    Reading back through `entity.GetVisualBlocksComponent()` rather than the
    object handed to AddOrReplace is deliberate: pybind hands back a Python-owned
    snapshot, so the original object shows the value as it was at attach time.
    That trap is why a working behaviour tree once appeared to deal no damage.
    """
    attach_root()
    sc, e = make_actor("actor")

    c = VisualBlocksComponent()
    c.GraphPath = GRAPH
    e.AddOrReplaceVisualBlocksComponent(c)

    check(e.HasVisualBlocksComponent(),
          "the component must be present after AddOrReplace")
    check_eq(e.GetVisualBlocksComponent().GraphPath, GRAPH,
             "GraphPath must read back through the entity")
    check_eq(e.GetVisualBlocksComponent().GraphName, "",
             "GraphName defaults to empty (the graph's own name)")


def test_absent_component_reports_false():
    """Negative control: an entity with no component must say so.

    `TryGet` is the safe accessor, not `Get`: `Get<T>` on an entity that lacks T
    asserts inside entt (`(contains(entt) && "Set does not contain entity")`),
    so a test that calls it on a bare entity fails with a static assertion rather
    than a check failure. That is the engine's contract, not a defect -- it is
    the same reason `test_tree_attach.py` only reads a tree back after attaching
    one.
    """
    attach_root()
    sc, e = make_actor("bare")

    check(not e.HasVisualBlocksComponent(),
          "an entity that never had the component must report false")
    # TryGet returns None for an absent component; Get would assert.
    check(e.TryGetVisualBlocksComponent() is None,
          "TryGet on an absent component must return None, not a default")


def test_scene_round_trip_preserves_visual_blocks():
    """The component survives SaveScene -> LoadScene.

    This is the assertion that separates a real scene component from a
    runtime-only flag. A component with no serialize block is dropped by Serialize
    and ignored on load, so the path does not come back.
    """
    attach_root()
    sc, e = make_actor("actor")
    c = VisualBlocksComponent()
    c.GraphPath = GRAPH
    c.GraphName = "DamageFlash"
    e.AddOrReplaceVisualBlocksComponent(c)

    path = scene_file("wlblocks_roundtrip.wastescene")
    check(sc.SaveScene(path), "SaveScene must succeed")

    loaded = Scene()
    check(loaded.LoadScene(path), "LoadScene must succeed")

    # Find the entity by enumeration rather than assuming an id: the loader
    # assigns ids, so the saved id is not the loaded one.
    #
    # GetAllEntitiesWithTransform() is the view that enumerates. GetEntities() is
    # EMPTY in a headless scene -- a harness rule recorded in test_audio_scene.py,
    # and the reason the first draft of this lookup silently found nothing.
    found = only_entity(loaded)
    check(found is not None, "the saved entity must come back")

    check(found.HasVisualBlocksComponent(),
          "the component must survive the round trip (no serialize block drops it)")
    check_eq(found.GetVisualBlocksComponent().GraphPath, GRAPH,
             "GraphPath must survive the round trip")
    check_eq(found.GetVisualBlocksComponent().GraphName, "DamageFlash",
             "GraphName must survive the round trip")


def test_behavior_tree_component_survives_a_round_trip():
    """The SIBLING gap: BehaviorTreeComponent has no serialize block.

    A scene saved with a tree attached silently loses the tree on load. That is
    the same class of hole the new component would inherit, so it is fixed here
    rather than copied.
    """
    attach_root()
    sc, e = make_actor("npc")
    t = BehaviorTreeComponent()
    t.TreePath = TREE
    e.AddOrReplaceBehaviorTreeComponent(t)

    path = scene_file("tree_roundtrip.wastescene")
    check(sc.SaveScene(path), "SaveScene must succeed")

    loaded = Scene()
    check(loaded.LoadScene(path), "LoadScene must succeed")

    found = only_entity(loaded)
    check(found is not None, "the saved entity must come back")

    check(found.HasBehaviorTreeComponent(),
          "the tree component must survive the round trip")
    check_eq(found.GetBehaviorTreeComponent().TreePath, TREE,
             "TreePath must survive the round trip")


def test_absent_key_is_not_an_error_on_load():
    """Negative control: a scene with no such component loads cleanly.

    The deserializer must tolerate a missing key -- a scene authored before the
    component existed is the common case, not an error.
    """
    attach_root()
    sc, e = make_actor("plain")

    path = scene_file("plain_scene.wastescene")
    check(sc.SaveScene(path), "SaveScene must succeed")

    loaded = Scene()
    check(loaded.LoadScene(path), "a scene with no graph component must still load")
    for entity in loaded.GetAllEntitiesWithTransform():
        check(not entity.HasVisualBlocksComponent(),
              "no entity in a scene that never had the component may gain one")
