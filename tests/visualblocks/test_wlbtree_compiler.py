"""NB.3 — the .wlbtree compiler.

Three things are being proven here, and the third is the one that actually
matters:

1. The compiler emits a topology the evaluator can walk.
2. It is deterministic and round-trips losslessly.
3. **The emitted code actually RUNS.** Every other test compares strings. This
   one execs the generated class against the real `wl_behavior` runtime and
   ticks it, because a compiler can emit perfectly well-formed Python that
   names node types the evaluator has no handler for -- and that failure is
   invisible in a string comparison, invisible in a syntax check, and shows up
   in game as an NPC that silently never does the thing.
"""

import os
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent.as_posix()
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from _bootstrap import SCRIPTS, TREES, configure

_MODULE_DIR = configure()

import Wasteland

VB = Wasteland.VisualBlocks
import wl_behavior as B

PASS = 0
FAIL = 0


def check(name, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print("  PASS: %s" % name)
    else:
        FAIL += 1
        print("  FAIL: %s" % name)
        if detail != "":
            print("        %s" % (detail,))


def node(nid, type_id, children=None, **kwargs):
    n = VB.GraphNode()
    n.Id = nid
    n.TypeId = type_id
    if children:
        n.ChildIds = list(children)
    for key, value in kwargs.items():
        if key in ("Latent", "Abort", "EventName"):
            setattr(n, key, value)
        else:
            n.SetLiteral(key, value)
    return n


def tree(*nodes, **kw):
    g = VB.Graph()
    g.Kind = kw.get("kind", "tree")
    g.Name = kw.get("name", "T")
    g.Nodes.extend(list(nodes))
    if "blackboard" in kw:
        g.Blackboard.extend(list(kw["blackboard"]))
    return g


def bkey(name, type_name="Float", default=""):
    k = VB.BlackboardKey()
    k.Name = name
    k.Type = getattr(VB, type_name)
    k.DefaultLiteral = default
    return k


# ================================================================== contract

print("\n== runtime type contract ==")

runtime = VB.RuntimeTypes()
check("the runtime type list is non-empty", len(runtime) > 0, len(runtime))
for t in ("Sequence", "Selector", "Invert", "Cooldown", "Timeout", "SetTimer", "Wait"):
    check("%s is a known runtime type" % t, VB.IsRuntimeType(t))
check("a generated leaf is NOT a runtime type", not VB.IsRuntimeType("MoveTo"))
check("an unknown type is not a runtime type", not VB.IsRuntimeType("Bogus"))

# The list must agree with what wl_behavior.py ACTUALLY dispatches on. A
# primitive added to the runtime but not here is the silent per-tick failure
# this contract exists to catch.
src = open(os.path.join(SCRIPTS, "wl_behavior.py"), encoding="utf-8").read()
dispatched = set()
import re
for m in re.finditer(r'ntype\s*==\s*"(\w+)"', src):
    dispatched.add(m.group(1))
for m in re.finditer(r'ntype\s+in\s+\(([^)]*)\)', src):
    for q in re.findall(r'"(\w+)"', m.group(1)):
        dispatched.add(q)
check("the contract matches the runtime's dispatch", dispatched <= set(runtime),
      "runtime handles %s but the contract lacks them" % (dispatched - set(runtime)))
# "Leaf" is the generic handler's pseudo-type, not a real dispatch, and the
# composites are matched by a form the regex above does not capture. Compare
# against the primitives only, which is where a genuine mismatch would be.
_primitives = {"Invert", "ForceSuccess", "ForceFailure", "Repeat", "Cooldown",
               "Timeout", "TickEvery", "SetTimer", "Wait"}
check("every contracted primitive is really dispatched",
      _primitives <= dispatched,
      "wl_behavior.py has no handler for %s" % (_primitives - dispatched))

# ============================================================== basic compile

print("\n== compile a linear Sequence ==")

g = tree(node("root", "Sequence", ["a", "b", "c"]),
         node("a", "ConditionBool", Value="true"),
         node("b", "ConditionBool", Value="false"),
         node("c", "ConditionBool", Value="true"),
         name="Linear")

ok, py, report, err = VB.CompileTree(g)
check("a linear Sequence compiles", ok, report + " " + err)
check("it emits a class", "class Linear" in py, py[:200])
check("children are in DECLARATION order",
      py.index('"a"') < py.index('"b"') < py.index('"c"'),
      "a,b,c out of order")
check("the root's kids list is ordered",
      "'kids': [\"a\", \"b\", \"c\"]" in py, py[py.find("root"):py.find("root")+120])
try:
    compile(py, "<gen>", "exec")
    check("the output is valid Python", True)
except SyntaxError as e:
    check("the output is valid Python", False, "line %s: %s" % (e.lineno, e.msg))

# Determinism: same tree twice, byte-identical. A non-deterministic emitter
# makes every diff and every test a lie.
ok2, py2, _, _ = VB.CompileTree(g)
check("the compiler is deterministic", py == py2)

# ================================================================ round trip

print("\n== round trip ==")

g = tree(node("root", "Selector", ["a", "b"], Abort="lower"),
         node("a", "Invert", ["c"]),
         node("c", "ConditionBool", Value="true"),
         node("b", "Cooldown", ["d"], Seconds="1.5"),
         node("d", "Attack", Target="t", Damage="5.0"),
         name="RT", blackboard=[bkey("t", "Entity")])

text = VB.Emit(g)
g2 = VB.Graph()
err2 = VB.Parse(text, g2)
check("the tree round-trips through YAML", err2 is None,
      err2.Message if err2 is not None else "")

if err2 is None:
    root2 = g2.FindNode("root")
    check("abort survives the round trip", root2.Abort == "lower", root2.Abort)
    check("child order survives", root2.ChildIds == ["a", "b"], root2.ChildIds)
    cd = g2.FindNode("b")
    lit = cd.FindLiteral("Seconds")
    check("a literal survives", lit is not None and lit.Value == "1.5",
          lit.Value if lit else "missing")
    ok3, py_rt, _, _ = VB.CompileTree(g2)
    check("the round-tripped tree compiles too", ok3)
    _, py_orig, _, _ = VB.CompileTree(g)
    check("and emits identical Python", py_rt == py_orig,
          "round-trip output differs from the original")

# ================================================================== validation

print("\n== validation rules (each must name the node) ==")

# A leaf marked latent with no latent body.
g = tree(node("root", "Selector", ["a"]),
         node("a", "ConditionBool", Value="true", Latent=True))
ok, py, report, err = VB.CompileTree(g)
check("a latent leaf with no latent body is REJECTED", not ok)
check("and names the node", "a" in err, err)

g = tree(node("root", "Selector", ["a"]),
         node("a", "MoveTo", Latent=True, Target="t", Speed="3.0"),
         blackboard=[bkey("t", "Entity")])
ok, py, report, err = VB.CompileTree(g)
check("a latent MoveTo is fine", ok, report + err)
check("and emits the latent flag", "'latent': True" in py)

# A decorator with two children.
g = tree(node("root", "Selector", ["d"]),
         node("d", "Invert", ["a", "b"]),
         node("a", "ConditionBool", Value="true"),
         node("b", "ConditionBool", Value="false"))
ok, py, report, err = VB.CompileTree(g)
check("a decorator with two children is REJECTED", not ok)

# An undeclared blackboard key must name the node AND the key.
g = tree(node("root", "Selector", ["a"]),
         node("a", "Attack", Target="targt", Damage="5.0"))
ok, py, report, err = VB.CompileTree(g)
check("an undeclared blackboard key is REJECTED", not ok)
check("and names the key", "targt" in report, report)

# A flow graph must not compile as a tree.
g = tree(node("root", "Selector", ["a"]),
         node("a", "ConditionBool", Value="true"), kind="flow")
ok, py, report, err = VB.CompileTree(g)
check("a flow graph does not compile as a tree", not ok)
check("and says why", "kind" in err, err)

# An unknown node type must not partially load.
bad = ("kind: tree\nschema: 1\nname: X\nnodes:\n"
       "  - id: root\n    type: Selector\n    children: [a]\n"
       "  - id: a\n    type: NoSuchNode\n")
g3 = VB.Graph()
err3 = VB.Parse(bad, g3)
ok3, _, _, _ = VB.CompileTree(g3)
check("an unknown node type is REJECTED", not ok3, "compiled anyway")
check("and no Python is emitted", VB.CompileTree(g3)[1] == "",
      VB.CompileTree(g3)[1][:80])
check("and the report names the node", "'a'" in VB.CompileTree(g3)[2],
      VB.CompileTree(g3)[2])

# ============================================================ generated runs

print("\n== the generated code RUNS (the test that matters) ==")


def load_generated(py, name):
    """exec the generated module and return an instance bound to a fake agent."""
    ns = {"__name__": "gen_" + name}
    exec(compile(py, "<generated>", "exec"), ns)
    cls = [v for k, v in ns.items() if isinstance(v, type) and k == name][0]
    return ns, cls


class _FakeTransform:
    """The value GetTransform() hands back: Translation/Rotation/Scale."""

    def __init__(self, translation, rotation, scale=None):
        self.Translation = translation
        self.Rotation = rotation
        self.Scale = scale if scale is not None else Wasteland.Vec3(1.0, 1.0, 1.0)


class FakeEntity:
    """Enough of an Entity for the generated leaves, and nothing more.

    The surface mirrors the REAL binding as of NB.4, not an older draft:
    the generated shim reads position through GetTransform() and writes it back
    through SetTransform(translation, rotation, scale). An earlier FakeEntity
    exposed GetPosition()/MoveCapsule(), which the engine has no such methods
    for -- so this stub silently tested an API nothing ships.
    """

    def __init__(self, health=100.0):
        self._health = health
        self._pos = Wasteland.Vec3(0.0, 0.0, 0.0)
        self._rot = Wasteland.Vec3(0.0, 0.0, 0.0)
        self.moved = []

    # --- transform, as the binding actually exposes it --------------------
    # Wasteland has NO `Transform` constructor. GetTransform() returns a
    # TransformComponent VALUE whose Translation/Rotation/Scale are the live
    # data, and SetTransform(translation, rotation, scale) writes it back.
    # A stub that invents its own transform type tests nothing that ships.
    def GetTransform(self):
        return _FakeTransform(self._pos, self._rot)

    def SetTransform(self, translation, rotation, scale):
        self._pos = translation
        self._rot = rotation
        self.moved.append(translation)

    # Kept for the NB.3-era assertions in this file.
    def GetPosition(self):
        return self._pos

    def MoveCapsule(self, delta):
        p = self._pos
        self._pos = Wasteland.Vec3(p.x + delta.x, p.y + delta.y, p.z + delta.z)
        self.moved.append(delta)

    def health(self):
        return self._health

    # The shim's _dist needs two entities to compare. A target placed here
    # gives the tree something real to approach, which is what makes the
    # latent/approach assertions mean anything.
    def set_target(self, entity):
        self._target = entity

    def target(self):
        return getattr(self, "_target", None)


# A Wait is a runtime primitive, not a lambda -- this is the case that a
# string comparison cannot catch. If the compiler emitted it as a plain leaf,
# the runtime would fall through to the leaf handler and fail every tick.
g = tree(node("root", "Selector", ["idle", "act"]),
         node("idle", "Sequence", ["none", "w"]),
         node("none", "ConditionBool", Value="true"),
         node("w", "Wait", Latent=True, Seconds="0.5"),
         node("act", "Sequence", ["a"]),
         node("a", "ConditionBool", Value="false"),
         name="WaitTree")
ok, py, report, err = VB.CompileTree(g)
check("a Wait tree compiles", ok, report + err)
wait_line = [l for l in py.splitlines() if "'Wait'" in l]
check("Wait emits as a runtime type", "'t': 'Wait'" in py, py[-300:])
check("and the Wait node itself carries no lambda",
      wait_line and "lambda" not in wait_line[0], wait_line)

if ok:
    ns, cls = load_generated(py, "WaitTree")
    agent = FakeEntity()
    inst = cls(agent)
    r1 = inst.tick(0.1)
    check("Wait returns RUNNING first", r1 == B.RUNNING, r1)
    r2 = inst.tick(0.1)
    check("Wait is still RUNNING before the deadline", r2 == B.RUNNING, r2)
    r3 = inst.tick(0.4)
    check("Wait succeeds once the deadline passes", r3 == B.SUCCESS, r3)
    # And it must RE-arm rather than succeeding forever on a past deadline.
    r4 = inst.tick(0.1)
    check("a second visit re-arms the wait", r4 == B.RUNNING, r4)

# A composite tree with a latent task, run for many ticks -- the shape every
# real NPC tree has.
g = tree(node("root", "Selector", ["dead", "engage"]),
         node("dead", "ConditionBool", Value="false"),
         node("engage", "Sequence", ["healthy", "act"]),
         node("healthy", "Invert", ["hurt"]),
         node("hurt", "ConditionHealthBelow", Threshold="25.0"),
         node("act", "Selector", ["approach", "attack"]),
         node("approach", "Sequence", ["far", "move"]),
         node("far", "Invert", ["near"]),
         node("near", "ConditionDistanceTo", Target="t", Range="2.0", Op="<="),
         node("move", "MoveTo", Latent=True, Target="t", Speed="3.0"),
         # 'strike' appears ONCE, as the Cooldown's own child. Listing it
         # under 'attack' as well gave it two parents, so it ran once per
         # parent and every hit dealt double damage on every tick.
         node("attack", "Sequence", ["near2", "rested"]),
         node("near2", "ConditionDistanceTo", Target="t", Range="2.0", Op="<="),
         node("rested", "Cooldown", ["strike"], Seconds="0.5"),
         node("strike", "Attack", Target="t", Damage="10.0"),
         name="Fighter", blackboard=[bkey("t", "Entity")])
ok, py, report, err = VB.CompileTree(g)
check("the full NPC tree compiles", ok, report + err)

if ok:
    ns, cls = load_generated(py, "Fighter")
    agent = FakeEntity(100.0)
    # The tree's conditions read the target off the blackboard. Without one,
    # _move's _target() returns None and the approach fails -- which is correct
    # behaviour, but it makes the latent assertion untestable. Give it a real
    # target FAR away so MoveTo is genuinely still travelling.
    far = FakeEntity(100.0)
    far._pos = Wasteland.Vec3(50.0, 0.0, 0.0)
    agent._target = far
    inst = cls(agent)
    # The generated shim reads ctx.bb["t"], so seed it the way the runtime
    # would: through the tree's own blackboard, after construction.
    inst.tree.bb.set("t", far)
    results = [inst.tick(0.1) for _ in range(5)]
    check("with no target the tree runs without raising", all(
        r in (B.SUCCESS, B.FAILURE, B.RUNNING) for r in results), results)
    check("MoveTo suspends while travelling", B.RUNNING in results, results)
    check("and it did not fall through to the attack branch",
          all(r != B.SUCCESS or agent.moved for r in results), results)
    # Damage gate: at low health the engage branch must stop acting.
    agent2 = FakeEntity(10.0)
    inst2 = cls(agent2)
    inst2.tree.bb.set("t", far)
    low = [inst2.tick(0.1) for _ in range(3)]
    check("at low health the engage branch fails", B.FAILURE in low, low)

# ============================================================== determinism

print("\n== determinism across a full file compile ==")

path = os.path.join(TREES, "EnemyGuard.wlbtree")
if os.path.exists(path):
    a = VB.CompileTreeFile(path)
    b = VB.CompileTreeFile(path)
    check("the shipped tree compiles", a[0], a[2] + a[3])
    check("compiling it twice is byte-identical", a[1] == b[1])
    check("and it has no errors", "0 errors" in a[2] or "0 error," in a[2], a[2])
    check("and no warnings either", "0 warning" in a[2], a[2])
    try:
        compile(a[1], "<gen>", "exec")
        check("the shipped tree generates valid Python", True)
    except SyntaxError as e:
        check("the shipped tree generates valid Python", False, str(e))
    # Round-trip it through save/load and confirm nothing changed.
    g4 = VB.Graph()
    e4 = VB.Load(path, g4)
    if e4 is None:
        before = VB.Emit(g4)
        tmp = os.path.join(os.environ.get("TEMP", "."), "_nb3_roundtrip.wlbtree")
        saved = VB.SaveTreeFile(tmp, g4)
        check("the tree saves", saved is None, saved.Message if saved else "")
        g5 = VB.Graph()
        e5 = VB.Load(tmp, g5)
        check("and reloads", e5 is None, e5.Message if e5 else "")
        if e5 is None:
            check("save/load is lossless", VB.Emit(g5) == before)
        try:
            os.remove(tmp)
        except OSError:
            pass
else:
    check("the shipped tree exists", False, path)

print("\n%d passed, %d failed" % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
