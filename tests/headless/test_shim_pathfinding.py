"""Exercise the generated Visual Blocks shim's pathfinding WITHOUT a build.

The shim is a C++ raw string literal (R"WL_SHIM(...)") in Compiler.cpp. It is
plain Python, so it can be extracted and executed directly against fakes -- no
MSBuild, no module import, no waiting on the renderer build to settle.

Why bother, when tests/headless/test_pathfinding_node.py covers the same ground
for real? Because that test can only run after the module is rebuilt, and the
pathfinding logic is exactly the kind of code where a typo in a rarely-taken
branch survives for weeks: with no mesh baked, or with no route at all, the OFF
path runs and nothing exercises the new code. Testing the shim in isolation
means those branches are checked on every edit, and the real-module test then
confirms the wiring.

The fake surface below is not guesswork -- every attribute it exposes was
extracted from the shim source itself (agent./scene./mesh. calls), so a new
shim call to something unimplemented fails loudly here instead of quietly
working in one test and not the other.
"""

from __future__ import annotations

import ast
import os
import re
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wlpath  # noqa: E402

# Python runs isolated here (python314._pth), so PYTHONPATH and the usual
# script-directory insertion do not happen on their own. wlpath.setup() is what
# puts Wasteland.pyd on the path.
wlpath.setup()

COMPILER = r"C:\dev\Wasteland\Wasteland\src\Wasteland\VisualBlocks\Compiler.cpp"


def load_shim():
    text = open(COMPILER, encoding="utf-8", errors="surrogateescape").read()
    start = text.find('R"WL_SHIM(')
    if start < 0:
        raise AssertionError("no R\"WL_SHIM( literal in Compiler.cpp")
    start += len('R"WL_SHIM(')
    end = text.find(')WL_SHIM"', start)
    shim = text[start:end]
    ast.parse(shim)          # a syntax error here is a compiler bug
    return shim


class V:
    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z


class Tr:
    """Mirrors GetTransform(): a Transform component with Translation/Rotation/Scale."""

    def __init__(self, x, y, z, yaw=0.0):
        self.Translation = V(x, y, z)
        self.Rotation = V(0, yaw, 0)
        self.Scale = V(1, 1, 1)


class Ent:
    """Minimal agent/target.

    SetTransform takes THREE separate Vec3s -- (translation, rotation, scale) --
    not a Transform component. Getting that signature wrong here is how a fake
    silently diverges from the real binding and the test stops proving
    anything, so it matches Entity::SetTransform exactly.
    """

    def __init__(self, x=0.0, y=0.0, z=0.0, yaw=0.0, uid=4242):
        self.p = (x, y, z)
        self.yaw = yaw
        self.uid = uid

    def GetTransform(self):
        return Tr(*self.p, yaw=self.yaw)

    def SetTransform(self, translation, rotation, scale=None):
        self.p = (translation.x, translation.y, translation.z)
        self.yaw = rotation.y

    def GetUUID(self):
        return self.uid


class NavPoint:
    def __init__(self, pos):
        self.Position = pos


class Mesh:
    """A wall spanning |z| < 4, so a route must swing out to z = 5."""

    blocked = False

    def __init__(self):
        self.calls = 0

    def FindPath(self, a, b):
        self.calls += 1
        if Mesh.blocked:
            return []
        return [NavPoint(V(a.x, 0, a.z)), NavPoint(V(0, 0, 5)),
                NavPoint(V(b.x, 0, 5)), NavPoint(V(b.x, 0, b.z))]


class FakeScene:
    def __init__(self, mesh=None):
        self.mesh = mesh

    def GetNavMesh(self):
        return self.mesh

    def Raycast(self, *a):
        # Six-value contract: (hit, point, normal, entity, ?, ?)
        return (False, V(0, 0, 0), V(0, 0, 0), None, 0.0, 0.0)

    def GetAllEntitiesWithTransform(self):
        return []


class Tree:
    """Carries `scene`, which is the shim's FIRST scene-resolution source.

    _scene() tries ctx.tree.scene, then Scene.GetActiveScene(), then
    Application.Get().GetScene(). The tree-owned scene wins because it is the
    only source that can be a DIFFERENT scene from the ambient one. A fake
    without it falls through to the real engine's active scene -- which is why
    the first run of this suite silently got no mesh at all.
    """

    def __init__(self, scene=None):
        self.node_states = {}
        self.scene = scene


class Ctx:
    def __init__(self, tree, ent, bb):
        self.tree = tree
        self.entity = ent
        self.seconds = 0.1
        self.bb = bb


def build_module(shim):
    import Wasteland
    m = types.ModuleType("shim_under_test")
    m.__dict__.update(dict(
        Wasteland=Wasteland, Vec3=Wasteland.Vec3, NavPathPoint=NavPoint,
        SUCCESS="SUCCESS", FAILURE="FAILURE", RUNNING="RUNNING"))
    exec(compile(shim, "<shim>", "exec"), m.__dict__)
    return m


def world(mesh, uid=4242):
    """An agent at x=-8 chasing a target at x=+8, its context, and its scene."""
    scene = FakeScene(mesh)
    npc = Ent(-8, 0, 0, uid=uid)
    ctx = Ctx(Tree(scene), npc, {"target": Ent(8, 0, 0)})
    return npc, ctx, scene


_failures: list[str] = []
_passes = 0


def check(cond, msg):
    global _passes
    if cond:
        _passes += 1
    else:
        _failures.append(msg)


def test_off_walks_straight_and_ignores_the_mesh():
    M = build_module(load_shim())
    Mesh.blocked = False
    mesh = Mesh()
    npc, ctx, _sc = world(mesh)
    for _ in range(12):
        M._move(npc, ctx, "target", 3.0, False)
    check(npc.p[0] > -8.0, "OFF: NPC did not move at all (%r)" % (npc.p,))
    check(abs(npc.p[2]) < 1e-9,
          "OFF: NPC detoured in Z (%r) -- the default must preserve today's "
          "behaviour" % (npc.p,))
    check(mesh.calls == 0,
          "OFF: asked the navmesh %d times; pathfinding must not cost anything "
          "when disabled" % mesh.calls)


def test_on_detours_past_the_wall():
    M = build_module(load_shim())
    Mesh.blocked = False
    npc, ctx, _sc = world(Mesh())
    zs = []
    for _ in range(60):
        M._move(npc, ctx, "target", 3.0, True)
        zs.append(round(npc.p[2], 2))
    check(max(zs) > 4.0,
          "ON: never swung past the wall, max |z|=%r -- it went through" % max(zs))
    check(npc.p[0] > 0.5, "ON: never got past the wall, x=%r" % npc.p[0])


def test_on_without_a_mesh_behaves_exactly_like_off():
    M = build_module(load_shim())
    npc_off, ctx_off, _ = world(Mesh())
    for _ in range(12):
        M._move(npc_off, ctx_off, "target", 3.0, False)

    npc_on, ctx_on, _ = world(None)
    for _ in range(12):
        M._move(npc_on, ctx_on, "target", 3.0, True)

    check(npc_on.p == npc_off.p,
          "ON-with-no-mesh (%r) diverged from OFF (%r); a missing navmesh must "
          "not change how an NPC moves" % (npc_on.p, npc_off.p))


def test_unreachable_target_does_not_freeze():
    M = build_module(load_shim())
    Mesh.blocked = True
    npc, ctx, _sc = world(Mesh())
    for _ in range(20):
        M._move(npc, ctx, "target", 3.0, True)
    check(npc.p != (-8.0, 0.0, 0.0),
          "ON: an unreachable target FROZE the NPC at its start (%r); it must "
          "fall back to direct movement" % (npc.p,))


def test_a_missing_target_is_a_failure_not_a_crash():
    M = build_module(load_shim())
    npc = Ent(-8, 0, 0)
    ctx = Ctx(Tree(), npc, {"target": None})
    try:
        result = M._move(npc, ctx, "target", 3.0, True)
        check(result == "FAILURE",
              "a missing target returned %r, expected FAILURE" % (result,))
    except Exception as exc:  # noqa: BLE001
        check(False, "a missing target raised %r" % (exc,))


def test_two_agents_sharing_one_tree_keep_separate_paths():
    """The reason _agent_key exists at all."""
    M = build_module(load_shim())
    Mesh.blocked = False
    mesh = Mesh()
    a, ca, _ = world(mesh, uid=1)
    b, cb, _ = world(mesh, uid=2)
    ca.tree = cb.tree          # deliberately the SAME tree, as in one scene
    for _ in range(3):
        M._move(a, ca, "target", 3.0, True)
        M._move(b, cb, "target", 3.0, True)
    keys = sorted(k for k in ca.tree.node_states if k.startswith("path:"))
    check(len(keys) == 2,
          "two agents on one tree shared %d path slots (%r); each NPC needs its "
          "own route" % (len(keys), keys))


def test_the_path_is_not_replanned_every_tick():
    """Re-planning each tick is the obvious implementation and the expensive one."""
    M = build_module(load_shim())
    Mesh.blocked = False
    mesh = Mesh()
    npc, ctx, _sc = world(mesh)
    for _ in range(40):
        M._move(npc, ctx, "target", 3.0, True)
    check(mesh.calls <= 3,
          "FindPath was called %d times over 40 ticks; the cached route is "
          "being thrown away and re-planned every frame" % mesh.calls)


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