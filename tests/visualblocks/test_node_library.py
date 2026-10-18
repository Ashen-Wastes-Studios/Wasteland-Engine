"""NB.4 — the v1 node library, asserted on OBSERVABLE results.

Two rules shape this file, both from the plan and the binding skill:

1. **Every leaf is audited for unbound names.** A leaf that emits
   `entity.Foo()` where `Foo` is not bound compiles clean, walks clean, and
   raises AttributeError the first time an NPC enters that branch -- an NPC
   that appears to work and is silently inert on exactly one state. The audit
   parses the EMITTED python and checks every Wasteland.* and entity.* name
   against the real built module.

2. **Every leaf is asserted on what it CHANGED**, not on "did not throw". A
   stub scene, one tick, then read the value back through the engine. A write
   that lands in a copy also raises nothing and changes nothing.

The audit is the higher-value half: it catches an entire class of bug that
per-leaf tests cannot, because the failing branch is whatever a designer
happens to walk at runtime.
"""

import os
import re
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent.as_posix()
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from _bootstrap import SCRIPTS, TREES, configure

_MODULE_DIR = configure()
print("[using %s]" % _MODULE_DIR)

import Wasteland

VB = Wasteland.VisualBlocks
import wl_behavior as B

PASS = 0
FAIL = 0

# Two environment facts this suite depends on, both learned the hard way:
#
# 1. Wasteland.pyd is HELD OPEN by the Hermes runtime itself, so it cannot be
#    relinked in place from this session. Build to a scratch OutDir and point
#    PYTHONPATH there.
# 2. Wasteland.Application.Get() returns None headless, and Scene.GetActiveScene()
#    is None until a scene is activated. Perception therefore takes the scene as
#    an explicit constructor argument rather than reaching for a global -- an
#    Entity carries no back-reference to its Scene.


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


# ===================================================== the taxonomy, in full

TAXONOMY = {
    "composite": ["Sequence", "Selector", "Parallel"],
    "decorator": ["Invert", "ForceSuccess", "ForceFailure", "Repeat", "Cooldown",
                  "Timeout", "TickEvery"],
    "condition": ["IsDead", "IsLowHealth", "ConditionDistanceTo",
                  "HasLineOfSight",
                  "BlackboardIsSet", "Compare", "Gate"],
    "task": ["Wait", "MoveTo", "FaceTarget", "ApplyDamage", "SenseNearest",
             "SetBlackboard", "MoveToTransform", "PlayAnimation", "CallPython"],
}

registered = {n.TypeId: n for n in VB.RegisteredNodes()}

print("\n== every taxonomy node is registered ==")
for role, names in TAXONOMY.items():
    for name in names:
        check("%s (%s) is registered" % (name, role), name in registered)
for role, names in TAXONOMY.items():
    for name in names:
        if name in registered:
            check("%s has role '%s'" % (name, role), registered[name].BtRole == role,
                  registered[name].BtRole)

# The runtime-type contract must cover every composite and decorator, because
# the evaluator dispatches those by type string rather than by generated body.
print("\n== composites/decorators are in the runtime contract ==")
runtime = set(VB.RuntimeTypes())
for name in TAXONOMY["composite"] + TAXONOMY["decorator"]:
    check("%s is a runtime type" % name, name in runtime)

# ======================================================== the binding audit

print("\n== binding audit: every name a leaf emits must resolve ==")


def build(*nodes, blackboard=None, name="T"):
    g = VB.Graph()
    g.Kind = "tree"
    g.Name = name
    g.Nodes.extend(list(nodes))
    if blackboard:
        g.Blackboard.extend(list(blackboard))
    return g


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


def bkey(name, type_name="Float", default=""):
    k = VB.BlackboardKey()
    k.Name = name
    k.Type = getattr(VB, type_name)
    k.DefaultLiteral = default
    return k


# One tree per taxonomy node, each wrapped in a Sequence so it can be compiled
# and ticked standalone.
def solo(type_id, **kwargs):
    """A one-leaf tree, so each taxonomy node can be compiled and ticked alone.

    A composite or decorator as the SOLE leaf has no child to wrap, which is
    a genuine arity error rather than a broken fixture -- those are compiled
    with a real child instead.
    """
    lat = kwargs.pop("Latent", False)
    if type_id in ("Sequence", "Selector", "Parallel", "Invert", "ForceSuccess",
                   "ForceFailure", "Repeat", "Cooldown",
                   "Timeout", "TickEvery"):
        # Composites need >=1 child; decorators wrap exactly one. The node's
        # own pins go on the ROOT, which is where the type actually lives.
        kids = [node("kid1", "ConditionBool", Value="true")]
        return build(node("root", type_id, [k.Id for k in kids], **kwargs), *kids,
                     blackboard=[bkey("target", "Entity")], name="Solo")
    return build(node("root", "Sequence", ["leaf"]),
                 node("leaf", type_id, **kwargs, Latent=lat),
                 blackboard=[bkey("target", "Entity")], name="Solo")


# Mandatory pin values per leaf. The audit must measure BINDINGS, so every
# required pin is supplied -- otherwise every leaf fails with "required pin
# not connected", which is a fixture defect reporting as a binding defect.
REQUIRED_PINS = {
    "IsLowHealth": {"Fraction": "0.5"},
    "Parallel": {"Policy": "all"},
    "ConditionDistanceTo": {"Target": "target", "Range": "5.0", "Op": "<="},
    "HasLineOfSight": {"Target": "target", "MaxDistance": "30.0"},
    "BlackboardIsSet": {"Key": "target"},
    "Compare": {"A": "1.0", "B": "2.0", "Op": "<"},
    "Gate": {"Every": "4"},
    "Wait": {"Seconds": "0.1"},
    "MoveTo": {"Target": "target", "Speed": "3.0"},
    "FaceTarget": {"Target": "target", "TurnRate": "540.0"},
    "ApplyDamage": {"Target": "target", "Damage": "12.0"},
    "SenseNearest": {"Key": "target", "Radius": "20.0"},
    "MoveToTransform": {"Position": "(1, 0, 0)", "Speed": "3.0", "Arrival": "0.5"},
    "PlayAnimation": {"Animation": "idle", "Loop": "true"},
    "CallPython": {"Method": "OnBehaviorStep"},
    "Cooldown": {"Seconds": "1.0"},
    "Timeout": {"Seconds": "1.0"},
    "TickEvery": {"Seconds": "0.25"},
    "Repeat": {"Count": "2"},
    "SetBlackboard": {"Key": "target", "Value": "1.0"},
}


AUDITED = []
for role, names in TAXONOMY.items():
    for name in names:
        AUDITED.append((role, name, solo(name, **REQUIRED_PINS.get(name, {}))))

# Now collect every engine name the emitted python references.
W_NAME = re.compile(r"Wasteland\.([A-Za-z_][A-Za-z0-9_]*)")
E_NAME = re.compile(r"\b(?:agent|other|target|eye|entity)\.([A-Z][A-Za-z0-9_]*)")

emitted = {}
for role, name, graph in AUDITED:
    ok, py, report, err = VB.CompileTree(graph)
    if not ok:
        check("%s compiles standalone" % name, False, report + " " + err)
        continue
    check("%s compiles standalone" % name, True)
    emitted[name] = py

# Wasteland.* names must all exist as module attributes.
print("\n-- module-level names --")
module_names = set()
for name, py in emitted.items():
    module_names.update(W_NAME.findall(py))
for n in sorted(module_names):
    check("Wasteland.%s is bound" % n, hasattr(Wasteland, n))

# Entity methods are not knowable from the class alone (they depend on which
# components exist), so check them against a live entity, and treat a missing
# one as a finding rather than a pass -- that is the whole point of the audit.
print("\n-- entity methods, against a real entity --")
SCENE = Wasteland.Scene()          # named: Entity holds a raw Scene*


def _place(entity, x, y, z):
    """Set a position, preserving rotation.

    Entity.SetTransform takes (translation, rotation[, scale]) as separate
    Vec3s -- NOT a Transform component, which is a different type.
    """
    t = entity.GetTransform()
    entity.SetTransform(Wasteland.Vec3(x, y, z), t.Rotation, t.Scale)


def _px(entity):
    return entity.GetTransform().Translation.x


def _yaw_of(entity):
    return entity.GetTransform().Rotation.y
probe = SCENE.CreateEntity("audit_probe")
probe.AddHealthComponent(Wasteland.HealthComponent())

entity_names = set()
for name, py in emitted.items():
    entity_names.update(E_NAME.findall(py))
for n in sorted(entity_names):
    # Get/Set/Has/Add on a component are the generic ECS surface; the rest
    # must be bound on Entity itself.
    generic = n in ("GetComponent", "SetComponent", "HasComponent", "AddComponent",
                    "RemoveComponent", "GetPosition", "SetPosition", "MoveCapsule",
                    "GetRotation", "SetRotation")
    check("entity.%s resolves" % n, hasattr(probe, n) or generic,
          "not bound on Entity and not generic ECS surface")

# ============================================== observable leaf behaviour

print("\n== observable behaviour, one tick against a real scene ==")


def run(tree_py, entity, ticks=1, seed=None, scene=None):
    """Compile, instantiate, tick. Returns the behavior instance.

    `scene` defaults to the suite scene; pass a dedicated one for anything that
    searches the world, so the result does not depend on which other tests ran.
    """
    ns = {}
    exec(compile(tree_py, "<gen>", "exec"), ns)
    cls = [v for k, v in ns.items() if isinstance(v, type)][0]
    # Perception needs a scene; an Entity carries no back-reference to one, so
    # the tree is given it explicitly.
    inst = cls(entity, scene if scene is not None else SCENE)
    if seed:
        seed(inst)
    results = [inst.tick(0.1) for _ in range(ticks)]
    return inst, results


def make_graph(*nodes, blackboard=None, name="T"):
    return build(*nodes, blackboard=blackboard, name=name)


# --- IsDead ---------------------------------------------------------------
e_dead = SCENE.CreateEntity("dead")
h = Wasteland.HealthComponent()
# Kill it THROUGH ApplyDamage. Setting .Current = 0 directly leaves IsDead
# False -- IsDead is not derived from Current on read, it is what ApplyDamage
# sets. A fixture that assigns Current builds a "dead" entity that is alive.
e_dead.AddHealthComponent(h)
e_dead.GetHealthComponent().ApplyDamage(999.0)
assert e_dead.GetHealthComponent().IsDead, "fixture: entity is not actually dead"

g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "IsDead"), name="IsDeadT")
ok, py, _, _ = VB.CompileTree(g)
inst, res = run(py, e_dead)
check("IsDead succeeds on a dead agent", res[-1] == B.SUCCESS, res)

e_alive = SCENE.CreateEntity("alive")
e_alive.AddHealthComponent(Wasteland.HealthComponent())
inst2, res2 = run(py, e_alive)
check("IsDead fails on a living agent", res2[-1] == B.FAILURE, res2)

e_nohp = SCENE.CreateEntity("nohp")     # no HealthComponent at all
inst3, res3 = run(py, e_nohp)
check("IsDead fails on an entity with no health (not 'dead scenery')",
      res3[-1] == B.FAILURE, res3)

# --- IsLowHealth ----------------------------------------------------------
g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "IsLowHealth", Fraction="0.25"), name="LowT")
ok, py, _, _ = VB.CompileTree(g)
e_low = SCENE.CreateEntity("low")
hl = Wasteland.HealthComponent()
hl.Current = 10.0
hl.Max = 100.0
e_low.AddHealthComponent(hl)
inst, res = run(py, e_low)
check("IsLowHealth is true at 10/100 with a 0.25 threshold", res[-1] == B.SUCCESS, res)

e_hi = SCENE.CreateEntity("hi")
hh = Wasteland.HealthComponent()
hh.Current = 90.0
hh.Max = 100.0
e_hi.AddHealthComponent(hh)
inst, res = run(py, e_hi)
check("IsLowHealth is false at 90/100", res[-1] == B.FAILURE, res)

# --- ApplyDamage: the silent-write test ----------------------------------
victim = SCENE.CreateEntity("victim")
hv = Wasteland.HealthComponent()
hv.Current = 100.0
hv.Max = 100.0
victim.AddHealthComponent(hv)

attacker = SCENE.CreateEntity("attacker")
attacker.AddHealthComponent(Wasteland.HealthComponent())

g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "ApplyDamage", Target="target", Damage="12.0"),
               blackboard=[bkey("target", "Entity")], name="DmgT")
ok, py, report, err = VB.CompileTree(g)
check("ApplyDamage compiles", ok, report + err)


def seed_target(inst):
    inst.tree.bb.set("target", victim)


inst, res = run(py, attacker, seed=seed_target)
after = victim.GetHealthComponent().Current
check("ApplyDamage actually reduced health", after == 88.0,
      "read back %.1f, expected 88.0 -- a by-value write would leave 100.0"
      % after)
check("and reported SUCCESS", res[-1] == B.SUCCESS, res)

# A target that does not exist must FAIL, not raise.
no_target = make_graph(node("root", "Selector", ["leaf"]),
                       node("leaf", "ApplyDamage", Target="target", Damage="12.0"),
                       blackboard=[bkey("target", "Entity")], name="DmgNoT")
ok, py2, _, _ = VB.CompileTree(no_target)
inst, res = run(py2, attacker)
check("ApplyDamage with no target FAILS rather than raising", res[-1] == B.FAILURE, res)

# --- SenseNearest: which entity, and what it skipped ----------------------
print("\n-- SenseNearest picks the right entity --")
# A DEDICATED scene. The suite shares one scene, and entities left by earlier
# tests are still in the world -- perception sees what is actually there, so a
# shared world makes "which entity did it pick" depend on test order.
SENSE_SCENE = Wasteland.Scene()
watcher = SENSE_SCENE.CreateEntity("watcher")
watcher.AddHealthComponent(Wasteland.HealthComponent())
_place(watcher, 0.0, 0.0, 0.0)

far_live = SENSE_SCENE.CreateEntity("far_live")
hfl = Wasteland.HealthComponent()
far_live.AddHealthComponent(hfl)
_place(far_live, 5.0, 0.0, 0.0)

near_dead = SENSE_SCENE.CreateEntity("near_dead")
hnd = Wasteland.HealthComponent()
near_dead.AddHealthComponent(hnd)          # dead, and closer
near_dead.GetHealthComponent().ApplyDamage(999.0)
_place(near_dead, 2.0, 0.0, 0.0)

g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "SenseNearest", Key="target", Radius="20.0"),
               blackboard=[bkey("target", "Entity")], name="SenseT")
ok, py, report, err = VB.CompileTree(g)
check("SenseNearest compiles", ok, report + err)
if ok:
    inst, res = run(py, watcher, scene=SENSE_SCENE)
    picked = inst.tree.bb.get("target")
    def at(entity, axis="x"):
        return getattr(entity.GetTransform().Translation, axis)

    # Identity CANNOT be used: each call returns a new pybind wrapper, so even
    # the correct entity fails `is`. Compare the position it picked instead,
    # which is what "picked the right one" means here.
    check("SenseNearest picked the live entity, not the nearer DEAD one",
          picked is not None and at(picked) == 5.0,
          "picked position x=%s -- the dead one is at x=2" %
          (at(picked) if picked is not None else None))
    check("SenseNearest did not pick the watcher itself",
          picked is not None and at(picked) != 0.0,
          "picked the watcher's own position")
    check("and the node still reports SUCCESS", res[-1] == B.SUCCESS, res)

    # Nothing live nearby: the key must be CLEARED, not left stale.
    lonely = SENSE_SCENE.CreateEntity("lonely")
    lonely.AddHealthComponent(Wasteland.HealthComponent())
    lonely
    _place(lonely, 500.0, 0.0, 0.0)
    g2 = make_graph(node("root", "Selector", ["leaf"]),
                    node("leaf", "SenseNearest", Key="target", Radius="1.0"),
                    blackboard=[bkey("target", "Entity")], name="SenseFar")
    ok2, py2, _, _ = VB.CompileTree(g2)
    inst2, res2 = run(py2, lonely, scene=SENSE_SCENE)
    check("with nothing in range, SenseNearest CLEARS the key",
          inst2.tree.bb.get("target") is None,
          "key is %r -- a stale key means the NPC attacks a corpse"
          % (inst2.tree.bb.get("target"),))

# --- MoveTo: the movement test that makes everything else matter ---------
mover = SCENE.CreateEntity("mover")
mover.AddHealthComponent(Wasteland.HealthComponent())
_place(mover, 0.0, 0.0, 0.0)

goal = SCENE.CreateEntity("goal")
goal.AddHealthComponent(Wasteland.HealthComponent())
_place(goal, 10.0, 0.0, 0.0)

g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "MoveTo", Latent=True, Target="target", Speed="6.0"),
               blackboard=[bkey("target", "Entity")], name="MoveT")
ok, py, report, err = VB.CompileTree(g)
check("MoveTo compiles", ok, report + err)
if ok:
    def seed_goal(inst):
        inst.tree.bb.set("target", goal)

    before_x = _px(mover)
    inst, res = run(py, mover, ticks=5, seed=seed_goal)
    after_x = _px(mover)
    check("MoveTo returned RUNNING while travelling (it is latent)",
          B.RUNNING in res, res)
    check("MoveTo actually moved the entity", after_x > before_x,
          "x went %.2f -> %.2f -- a by-value position write moves nothing"
          % (before_x, after_x))
    check("and it moved TOWARD the target", after_x < 10.0, after_x)

# --- FaceTarget: shortest arc, no wrap the long way ----------------------
facee = SCENE.CreateEntity("facee")
facee.AddHealthComponent(Wasteland.HealthComponent())
_place(facee, 0.0, 0.0, 0.0)          # yaw 0, facing +Z

tgt = SCENE.CreateEntity("ftgt")
tgt.AddHealthComponent(Wasteland.HealthComponent())
# Due EAST: bearing +90. From yaw 0 the shortest arc is 90 degrees -- if the
# wrap were missing it would travel 270 the other way and land near -270.
_place(tgt, 5.0, 0.0, 0.0)

g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "FaceTarget", Target="target", TurnRate="720.0"),
               blackboard=[bkey("target", "Entity")], name="FaceT")
ok, py, report, err = VB.CompileTree(g)
check("FaceTarget compiles", ok, report + err)
if ok:
    def seed_ft(inst):
        inst.tree.bb.set("target", tgt)

    inst, res = run(py, facee, ticks=1, seed=seed_ft)
    yaw = _yaw_of(facee)
    check("FaceTarget rotated the entity", yaw != 0.0, yaw)
    # 720 deg/s over one 0.1s tick is 72 degrees; the SHORTEST arc toward +90
    # is forward through +72, not backward to -288.
    check("FaceTarget took the SHORTEST arc to a due-east target",
          60.0 <= yaw <= 90.0,
          "yaw=%.2f -- a long-way swing lands near -288" % yaw)

    # A due-NORTH target has bearing 0 while yaw is already ~0: the wrap is
    # what makes that a tiny correction instead of a full turn.
    north = SCENE.CreateEntity("ftgt_north")
    north.AddHealthComponent(Wasteland.HealthComponent())
    _place(north, 0.0, 0.0, 5.0)

    facee2 = SCENE.CreateEntity("facee2")
    facee2.AddHealthComponent(Wasteland.HealthComponent())
    _place(facee2, 0.0, 0.0, 0.0)

    def seed_north(inst):
        inst.tree.bb.set("target", north)

    _, rn = run(py, facee2, ticks=1, seed=seed_north)
    yaw2 = _yaw_of(facee2)
    check("a due-north target needs only a tiny correction, not a full turn",
          abs(yaw2) < 1.0, "yaw=%.2f" % yaw2)

# --- BlackboardIsSet: validity, not truthiness ----------------------------
# BlackboardIsSet reads VALIDITY, not truthiness. The case that distinguishes
# them is a key holding a FALSY value: `attackAt = 0.0` is a written value,
# and a bool test would say "unset" when it means "due now".
#
# Note every DECLARED key is present from tick one -- the compiler emits the
# whole declared schema into the blackboard dict, defaulting to None -- so a
# declared key is always set, and this node is meaningful for values written
# at RUNTIME.
g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "BlackboardIsSet", Key="attackAt"),
               blackboard=[bkey("attackAt", "Float")], name="BBSetT")
ok, py, report, err = VB.CompileTree(g)
check("BlackboardIsSet compiles", ok, report + err)

inst, res = run(py, watcher)
bb = inst.tree.bb
check("a declared key is present from tick one (the schema is seeded)",
      bb.is_set("attackAt") is True, "is_set=%r" % bb.is_set("attackAt"))
check("so BlackboardIsSet succeeds for a declared key", res[-1] == B.SUCCESS, res)

# The real distinction: a FALSY written value must still read as valid.
bb.set("attackAt", 0.0)
check("BlackboardIsSet reads VALIDITY, not truthiness",
      bb.is_set("attackAt") is True,
      "is_set=%r for an explicitly written 0.0" % bb.is_set("attackAt"))

inst, res = run(py, watcher)
inst.tree.bb.set("attackAt", 0.0)
inst.tick(0.1)
inst.tick(0.1)
check("and the node succeeds on a falsy-but-written value",
      inst.tick(0.1) == B.SUCCESS)

# An UNDECLARED key is genuinely absent -- the one case that reads false.
inst.tree.bb.clear_all()
check("after clear_all, BlackboardIsSet reads false",
      inst.tree.bb.is_set("attackAt") is False,
      "is_set=%r" % inst.tree.bb.is_set("attackAt"))
inst.tick(0.1)
inst.tick(0.1)
check("and the node fails for a cleared key", inst.tick(0.1) == B.FAILURE)


# --- Compare: the operator allowlist is a security boundary --------------
print("\n-- Compare operator allowlist --")
g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "Compare", A="1.0", B="2.0", Op="<"),
               name="CmpT")
ok, py, _, _ = VB.CompileTree(g)
inst, res = run(py, watcher)
check("Compare(1.0 < 2.0) succeeds", res[-1] == B.SUCCESS, res)

evil = '1.0); __import__("os").system("echo pwned")  #'
g = make_graph(node("root", "Selector", ["leaf"]),
               node("leaf", "Compare", A=evil, B="2.0", Op="<"),
               name="EvilT")
ok, py, report, err = VB.CompileTree(g)
check("Compare refuses an operator/input that is not a plain float",
      not ok or "os" not in py, report + err)
if ok:
    check("and the injected text did not survive into the output",
          "__import__" not in py, "injection reached generated code")

# --- emitter/shim arity must agree -------------------------------------
# A call site that disagrees with the shim signature raises TypeError only when
# an NPC enters that branch -- the silent-inert-NPC failure this layer exists to
# prevent. Derive BOTH sides from the generated python and compare, so a
# mismatch is a test failure rather than a runtime surprise.
print("\n-- every shim call in generated code matches its signature --")


def call_arity(source, callee):
    """Count the arguments of each call to `callee`, at any nesting depth.

    Splitting an argument list on commas is wrong -- a dict literal or a
    nested call contains commas that are not argument separators -- and a
    regex also mistakes `self._build()` for a call to `_build`. Scan instead,
    tracking bracket depth and string state.
    """
    results = []
    i = 0
    while True:
        i = source.find(callee + "(", i)
        if i == -1:
            return results
        # Require a word boundary BEFORE the callee: `_position(` is a suffix
        # of `_set_position(`, and without this the scanner finds the longer
        # name's call and counts ITS arguments against the shorter signature.
        if i > 0 and (source[i - 1].isalnum() or source[i - 1] == "_"):
            i += len(callee) + 1
            continue
        # A definition is not a call: `def _position(agent):` contains the
        # callee name too, and counting its parameter list as an argument list
        # reports every shim function as mis-called.
        prefix = source[max(0, i - 4):i]
        if prefix.endswith("def ") or prefix == "def ":
            i += len(callee) + 1
            continue
        start = i + len(callee) + 1
        depth = 1
        quote = None
        j = start
        commas = 0
        while j < len(source) and depth:
            ch = source[j]
            if quote:
                if ch == quote and source[j - 1] != "\\":
                    quote = None
            elif ch in "'\"":
                quote = ch
            elif ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
                if depth == 0:
                    break
            elif ch == "," and depth == 1:
                commas += 1
            j += 1
        body = source[start:j]
        # a call with no arguments has whitespace-only between the parens
        n = 0 if not body.strip() else commas + 1
        results.append((callee, n))
        i = j


for name, py in sorted(emitted.items()):
    sigs = {f: [x for x in a.split(",") if x.strip()]
            for f, a in re.findall(r"^def (\w+)\(([^)]*)\)", py, re.M)}
    arity = {f: len(v) for f, v in sigs.items()}
    for callee, want in sorted(arity.items()):
        if not callee.startswith("_"):
            continue
        for called, got in call_arity(py, callee):
            check("%s: _%s called with %d args (takes %d)"
                  % (name, callee.lstrip("_"), got, want), got == want,
                  "def _%s(%s)" % (callee.lstrip("_"), ", ".join(sigs[callee])))

# --- every leaf emits syntactically valid python -------------------------
print("\n-- generated python compiles for every taxonomy node --")
for role, name, graph in AUDITED:
    ok, py, report, err = VB.CompileTree(graph)
    if not ok:
        continue                       # already reported above
    try:
        compile(py, "<gen>", "exec")
        check("%s emits valid python" % name, True)
    except SyntaxError as e:
        check("%s emits valid python" % name, False,
              "line %s: %s" % (e.lineno, e.msg))

print("\n%d passed, %d failed" % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
