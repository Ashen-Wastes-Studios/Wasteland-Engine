"""FollowPath must use the scene navmesh to route around solid geometry.

This builds the node through the real VisualBlocks registry/compiler, runs its
emitted behaviour against a baked scene, and checks the NPC's sampled trail
against the wall's actual slab. It deliberately checks geometry at the wall's
own x/y, not route length or a global blocked-z threshold.
"""

from __future__ import annotations

import ast
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import runner  # noqa: E402
from runner import check  # noqa: E402
import wlpath  # noqa: E402

wlpath.setup()

import Wasteland as W  # noqa: E402
from Wasteland import Scene, Vec3  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
ASSET_ROOT = os.path.join(REPO, "DemonCore-Editor")
SCRIPTS = os.path.join(ASSET_ROOT, "assets", "scripts")
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

STEP = 0.1


def _follow_path_source():
    """Compile a one-task tree through the actual NodeDef and emitter."""
    vb = W.VisualBlocks
    graph = vb.Graph()
    graph.Kind = "tree"
    graph.Name = "FollowPathProbe"

    key = vb.BlackboardKey()
    key.Name = "target"
    key.Type = vb.PinType.Entity
    graph.Blackboard.append(key)

    root = vb.GraphNode()
    root.Id = "root"
    root.TypeId = "Sequence"
    root.ChildIds = ["follow"]
    graph.Nodes.append(root)

    follow = vb.GraphNode()
    follow.Id = "follow"
    follow.TypeId = "FollowPath"
    follow.SetLiteral("Target", "target")
    follow.SetLiteral("Speed", "3.0")
    follow.Latent = True
    graph.Nodes.append(follow)

    ok, source, report, message = vb.CompileTree(graph)
    check(ok, "FollowPath must be registered and compile; got %s (%s)" %
          (message, report))
    check('_move(ctx.entity, ctx, "target", 3.000000, True)' in source,
          "FollowPath must delegate to the existing _move path follower")
    check("'latent': True" in source,
          "FollowPath must be emitted as a latent task that can return RUNNING")
    ast.parse(source)
    return source


def _floor(scene, size=30.0, half=0.5):
    floor = scene.CreateEntity("floor")
    floor.SetPosition(Vec3(0.0, -half, 0.0))
    collider = W.BoxCollider3DComponent()
    collider.HalfExtents = Vec3(size * 0.5, half, size * 0.5)
    floor.AddOrReplaceBoxCollider3DComponent(collider)


def _wall(scene):
    wall = scene.CreateEntity("wall")
    wall.SetPosition(Vec3(0.0, 1.5, 0.0))
    collider = W.BoxCollider3DComponent()
    collider.HalfExtents = Vec3(0.5, 1.5, 4.0)
    wall.AddOrReplaceBoxCollider3DComponent(collider)


def test_runner_discovers_follow_path_suite():
    check("test_follow_path" in runner.discover(),
          "tests/headless/runner.py must discover this suite")


def test_follow_path_routes_around_the_wall_geometry():
    """The path may not enter the wall slab while vertically overlapping it."""
    source = _follow_path_source()
    module = type(sys)("wl_follow_path_probe")
    exec(compile(source, "<FollowPathProbe>", "exec"), module.__dict__)

    scene = Scene()
    npc = scene.CreateEntity("npc")
    npc.SetPosition(Vec3(-8.0, 0.0, 0.0))
    target = scene.CreateEntity("target")
    target.SetPosition(Vec3(8.0, 0.0, 0.0))
    _floor(scene)
    _wall(scene)
    scene.BakeNavMesh(W.NavMeshConfig())
    mesh = scene.GetNavMesh()
    check(mesh is not None, "the test scene must have a baked navmesh")

    instance = module.FollowPathProbe(npc, scene)
    instance.tree.bb.set("target", target)
    trail = []
    for _ in range(100):
        instance.tick(STEP)
        p = npc.GetTransform().Translation
        trail.append((p.x, p.y, p.z))

    # Determine the navmesh-blocked half-span AT THE WALL'S OWN X coordinate.
    # The baked span includes agent-radius inflation and cell quantisation.
    wall_x = 0.0
    wall_half_x = 0.5
    wall_bottom, wall_top = 0.0, 3.0
    blocked_half_z = 0.0
    z = 0.5
    while z < 12.0:
        if mesh.SampleHeight(Vec3(wall_x, 0.0, z)) is None:
            blocked_half_z = z
        elif blocked_half_z > 0.0:
            break
        z += 0.1
    check(blocked_half_z > 0.0,
          "could not measure the wall's blocked span at its own x coordinate")

    # The character's transform is one metre above its feet. The x band is
    # inflated by the configured 0.4 m agent radius, and a z value is blocked
    # only while x AND y are inside this wall's own AABB/slab.
    through_wall = [p for p in trail
                    if abs(p[0] - wall_x) <= wall_half_x + 0.4
                    and wall_bottom <= p[1] <= wall_top
                    and abs(p[2]) < blocked_half_z]
    check(not through_wall,
          "FollowPath crossed the wall's own x/y slab inside its blocked z-span: %r"
          % (through_wall[:6],))
    check(max(p[0] for p in trail) > wall_x + wall_half_x + 0.4,
          "FollowPath never made it past the wall: final position %r" % (trail[-1],))
    check(max(abs(p[2]) for p in trail) > blocked_half_z,
          "FollowPath never cleared the blocked span at the wall: max |z|=%.2f, "
          "blocked half-span=%.2f"
          % (max(abs(p[2]) for p in trail), blocked_half_z))
