"""End-to-end tests for the Visual Blocks compiler (VB.1-VB.4).

Every case here is a case where the naive implementation produces code that
compiles, runs, and does the WRONG thing. The validator exists to turn those
into errors; the compiler exists to make the topology explicit.

Runs headless. No scene, no editor, no window.
"""

import os
import re
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent.as_posix()
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from _bootstrap import TREES, configure

_MODULE_DIR = configure()

import Wasteland

VB = Wasteland.VisualBlocks

PASS = 0
FAIL = 0


def check(name, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print("  PASS: %s" % name)
    else:
        FAIL += 1
        print("  FAIL: %s" % (name,))
        if detail != "":
            print("        %s" % (detail,))


# Node FLAGS (latent, abort, event) are attributes. Pin VALUES are literals on
# the node, addressed by pin id -- they are not attributes, and pybind rejects
# setattr for an unknown name. `_` prefixed kwargs are pins.
_FLAGS = {"Latent", "Abort", "EventName", "X", "Y"}


def node(nid, type_id, children=None, **kwargs):
    n = VB.GraphNode()
    n.Id = nid
    n.TypeId = type_id
    if children:
        n.ChildIds = list(children)
    for key, value in kwargs.items():
        if key in _FLAGS:
            setattr(n, key, value)
        else:
            n.SetLiteral(key, value)
    return n


def setpin(n, pin_id, value):
    n.SetLiteral(pin_id, value)


def tree(*nodes, **kwargs):
    g = VB.Graph()
    g.Kind = kwargs.get("kind", "tree")
    g.Name = kwargs.get("name", "T")
    g.Nodes.extend(list(nodes))
    if "blackboard" in kwargs:
        g.Blackboard.extend(kwargs["blackboard"])
    if "links" in kwargs:
        g.Links.extend(list(kwargs["links"]))
    return g


def bkey(name, type_name="Float", default=""):
    k = VB.BlackboardKey()
    k.Name = name
    k.Type = getattr(VB, type_name)
    k.DefaultLiteral = default
    return k


def errors(result):
    return [f.Message for f in result.Findings if f.IsError]


def warnings(result):
    return [f.Message for f in result.Findings if not f.IsError]


# ==================================================================== registry

print("\n== node registry ==")

registered = VB.RegisteredNodes()
check("the registry is populated", len(registered) > 0, len(registered))
check("Sequence is registered", VB.HasNodeType("Sequence"))
check("Selector is registered", VB.HasNodeType("Selector"))
check("MoveTo is registered", VB.HasNodeType("MoveTo"))
check("Cooldown is registered", VB.HasNodeType("Cooldown"))
check("an unknown type is not registered", not VB.HasNodeType("NoSuchNode"))

info = VB.NodeInfo("MoveTo")
check("NodeInfo returns the def", info is not None)
if info is not None:
    pin_ids = [p.Id for p in info.Pins]
    check("MoveTo has a Target pin", "Target" in pin_ids, pin_ids)
    check("MoveTo has a Speed pin", "Speed" in pin_ids, pin_ids)
check("NodeInfo returns None for unknown", VB.NodeInfo("NoSuchNode") is None)

bt_nodes = VB.BehaviorNodes()
check("behavior nodes are separated from flow nodes", len(bt_nodes) > 0)
check("flow node list is non-empty", len(VB.FlowNodes()) > 0)
bt_ids = [d.TypeId for d in bt_nodes]
check("a flow node is NOT in the behavior list", "Log" not in bt_ids, bt_ids)

# Operator allowlist. The emitters interpolate the op straight into generated
# Python, so an unvalidated operator is a code-injection hole.
check("<= is allowed", VB.IsAllowedOperator("<="))
check("== is allowed", VB.IsAllowedOperator("=="))
check("eval is rejected", not VB.IsAllowedOperator("eval(__import__('os'))"))
check("arbitrary text is rejected", not VB.IsAllowedOperator("rm -rf"))
check("empty is rejected", not VB.IsAllowedOperator(""))

# ============================================================== literal typing

print("\n== literal parsing ==")

ok, value, err = VB.ParseLiteral(VB.Float, "3")
check("an int literal parses as a float", ok, err)
# The point is that it is NOT the bare int "3": Wasteland.Vec3(3, 0, 0)
# truncates where Vec3(3.000000, ...) does not. Exact formatting is not the
# contract; "is a float literal" is.
check("and is not emitted as a bare int", value not in ("3", "3.0") or
      "." in value, value)
check("and is a float literal", "." in value, value)

ok, value, err = VB.ParseLiteral(VB.Int, "42")
check("an int literal parses as an int", ok, err)
check("and emits an int", value == "42", value)

ok, value, err = VB.ParseLiteral(VB.Bool, "true")
check("a bool literal parses", ok, err)
check("and emits Python's True", value == "True", value)

ok, value, err = VB.ParseLiteral(VB.Float, "banana")
check("a malformed float is REJECTED", not ok, "accepted!")
check("and explains why", "not a float" in err, err)

ok, value, err = VB.ParseLiteral(VB.Bool, "maybe")
check("a malformed bool is rejected", not ok)

ok, value, err = VB.ParseLiteral(VB.Vec3, "(1.0, 2.0, 3.0)")
check("a vec3 literal parses", ok, err)
check("and emits a Vec3 constructor", "Vec3" in value, value)

ok, value, err = VB.ParseLiteral(VB.Vec3, "(1.0, 2.0)")
check("a vec3 with too few components is rejected", not ok)

ok, value, err = VB.ParseLiteral(VB.String, "hello")
check("a string literal gets quoted", ok and '"hello"' in value, value)

# Int promotes to float; nothing else does. A silent bool->float is how a graph
# compiles into something that runs and is wrong.
check("int -> float is allowed", VB.IsAssignable(VB.Int, VB.Float))
check("float -> int is NOT allowed", not VB.IsAssignable(VB.Float, VB.Int))
check("bool -> float is NOT allowed", not VB.IsAssignable(VB.Bool, VB.Float))
check("string -> float is NOT allowed", not VB.IsAssignable(VB.String, VB.Float))
check("vec3 -> float is NOT allowed", not VB.IsAssignable(VB.Vec3, VB.Float))

# =================================================================== serializer

print("\n== YAML serializer ==")

g = tree(
    node("root", "Selector", ["a", "b"]),
    node("a", "Sequence", ["c"]),
    node("b", "Attack", Target="target", Damage="10.0"),
    node("c", "ConditionBool", Value="true"),
    name="RoundTrip",
)
text = VB.Emit(g)
check("Emit produces text", len(text) > 0)

g2 = VB.Graph()
err2 = VB.Parse(text, g2)
check("the emitted text parses back", err2 is None,
      err2.Message if err2 is not None else "")
if err2 is None:
    check("the node count survives", len(g2.Nodes) == 4, len(g2.Nodes))
    check("the root id survives", g2.FindNode("root") is not None)
    root2 = g2.FindNode("root")
    if root2 is not None:
        check("child order survives -- order IS priority",
              root2.ChildIds == ["a", "b"], root2.ChildIds)

# Determinism: a non-deterministic emitter makes every diff and every test a lie.
text_again = VB.Emit(g)
check("Emit is deterministic", text == text_again)

# Unknown keys are reported, never dropped. A graph that quietly loses a node
# on load is worse than one that refuses to load.
g3 = VB.Graph()
err3 = VB.Parse("kind: tree\nschema: 1\nname: X\ntypo_section: 3\nnodes: []\n", g3)
check("an unknown top-level key is an error", err3 is not None)
if err3 is not None:
    check("and names the key", "typo_section" in err3.Message, err3.Message)

g4 = VB.Graph()
err4 = VB.Parse(
    "kind: tree\nschema: 1\nname: X\nnodes:\n  - id: a\n    typo: 3\n", g4)
check("an unknown node key is an error", err4 is not None)

# Dangling references are a load error, not an empty branch that runs silently.
g5 = VB.Graph()
err5 = VB.Parse(
    "kind: tree\nschema: 1\nname: X\nnodes:\n"
    "  - id: root\n    type: Selector\n    children: [ghost]\n", g5)
check("a child that does not exist is an error", err5 is not None)
if err5 is not None:
    check("and names the missing child", "ghost" in err5.Message, err5.Message)

# Duplicate ids make every link ambiguous.
g6 = VB.Graph()
err6 = VB.Parse(
    "kind: tree\nschema: 1\nname: X\nnodes:\n"
    "  - id: dup\n    type: Selector\n"
    "  - id: dup\n    type: Sequence\n", g6)
check("a duplicate node id is an error", err6 is not None)

# An unknown abort mode is rejected at load.
g7 = VB.Graph()
err7 = VB.Parse(
    "kind: tree\nschema: 1\nname: X\nnodes:\n"
    "  - id: root\n    type: Selector\n    abort: sideways\n", g7)
check("an unknown abort mode is an error", err7 is not None)

# A partial load must not happen: a half graph compiles into a half NPC.
g8 = VB.Graph()
err8 = VB.Parse("kind: tree\nschema: 1\nnodes:\n  - id: a\n    type: Selector\n"
                "    children: [ghost]\n", g8)
check("a failed parse leaves the graph EMPTY", len(g8.Nodes) == 0, len(g8.Nodes))

# =================================================================== validator

print("\n== validator: behaviour trees ==")

# Arity. A composite takes MANY children -- that is what a composite IS. The
# mistake worth catching is a decorator wrapping more than one.
# Each leaf has ONE parent. A node shared by two parents is a validation
# error (it runs once per parent and repeats its effect), so 'x'/'y' below
# are deliberately distinct -- the point of this case is that a composite may
# hold SEVERAL children, not that it may hold the same ones.
g = tree(node("root", "Sequence", ["a", "b"]),
         node("a", "Sequence", ["x"]),
         node("b", "Sequence", ["y"]),
         node("x", "ConditionBool", Value="true"),
         node("y", "ConditionBool", Value="false"))
r = VB.ValidateTree(g)
check("a composite with several children is fine", not r.HasErrors(), str(errors(r)))

g = tree(node("root", "Sequence", ["d"]),
         node("d", "Invert", ["a", "b"]),
         node("a", "ConditionBool", Value="true"),
         node("b", "ConditionBool", Value="false"))
r = VB.ValidateTree(g)
check("a decorator with two children IS an error", r.HasErrors())
check("and says it wraps exactly one", any("wraps exactly one" in m for m in errors(r)),
      str(errors(r)))

g = tree(node("root", "Sequence", ["d"]),
         node("d", "Invert", []),
         node("a", "ConditionBool", Value="true"))
r = VB.ValidateTree(g)
check("a decorator with no children is an error", r.HasErrors())

# Leaves cannot have children.
g = tree(node("root", "Selector", ["a"]), node("a", "ConditionBool", ["b"]),
         node("b", "ConditionBool", Value="true"))
r = VB.ValidateTree(g)
check("a condition with children is an error", r.HasErrors())

# Flow nodes cannot appear in a tree.
g = tree(node("root", "Selector", ["a"]), node("a", "Log"))
r = VB.ValidateTree(g)
check("a flow node in a tree is an error", r.HasErrors())

# Latent declaration. The compiler cannot infer whether a task spans ticks, and
# one that does but reports success on tick one makes the tree stop on it.
g = tree(node("root", "Selector", ["a"]),
         node("a", "MoveTo", Target="t", Speed="3.0"),
         blackboard=[bkey("t", "Entity")])
r = VB.ValidateTree(g)
check("MoveTo without latent WARNS", any("latent" in m for m in warnings(r)),
      str(warnings(r)))
check("but is not a hard error", not r.HasErrors(), str(errors(r)))

g = tree(node("root", "Selector", ["a"]),
         node("a", "MoveTo", Latent=True, Target="t", Speed="3.0"),
         blackboard=[bkey("t", "Entity")])
r = VB.ValidateTree(g)
check("MoveTo WITH latent is clean", not r.WarningCount(), str(warnings(r)))

# ForceSuccess is UNCONDITIONAL. Wrapping a condition makes it irrelevant.
g = tree(node("root", "Selector", ["a"]),
         node("a", "ForceSuccess", ["c"]),
         node("c", "ConditionBool", Value="true"))
r = VB.ValidateTree(g)
check("ForceSuccess around a condition WARNS",
      any("ForceSuccess" in m for m in warnings(r)), str(warnings(r)))

# Operator injection.
g = tree(node("root", "Selector", ["a"]),
         node("a", "ConditionCompare", Left="1.0", Right="2.0",
              Op="__import__('os').system('x')"),
         blackboard=[])
r = VB.ValidateTree(g)
check("an injected operator is an ERROR", r.HasErrors(), str(warnings(r)))
check("and says which operator", any("not a comparison" in m for m in errors(r)),
      str(errors(r)))

# Undeclared blackboard key. A write to an undeclared key is silently lost.
g = tree(node("root", "Selector", ["a"]), node("a", "MoveTo", Latent=True, Target="ghost"))
r = VB.ValidateTree(g)
check("an undeclared blackboard key is an error", r.HasErrors())
check("and names the key", any("ghost" in m for m in errors(r)), str(errors(r)))

# Malformed blackboard default. A key defaulting to 0 because its default was
# malformed is nearly impossible to diagnose later.
g = tree(node("root", "Selector", ["a"]),
         node("a", "ConditionBool", Value="true"),
         blackboard=[bkey("k", "Float", "banana")])
roundtrip = VB.Graph()
err = VB.Parse(VB.Emit(g), roundtrip)
check("a malformed blackboard default is rejected after Emit",
      err is not None,
      err.Message if err is not None else "invalid default was accepted")
g9 = VB.Graph()
err9 = VB.Parse(
    "kind: tree\nschema: 1\nname: X\n"
    "blackboard:\n  - name: k\n    type: float\n    default: banana\n"
    "nodes: []\n", g9)
check("a malformed blackboard default is a LOAD error", err9 is not None)
if err9 is not None:
    check("and names the key", "k" in err9.Message, err9.Message)

# Multiple roots.
g = tree(node("a", "Selector"), node("b", "Selector"))
r = VB.ValidateTree(g)
check("two roots is an error", r.HasErrors())
check("and says so", any("root" in m for m in errors(r)), str(errors(r)))

# Cycle. The compiler recurses over children, so a cycle would hang it.
# A root, so "no root" does not fire first and mask the cycle.
g = tree(node("root", "Selector", ["a"]),
         node("a", "Selector", ["b"]),
         node("b", "Selector", ["a"]))
r = VB.ValidateTree(g)
check("a cycle is an error", r.HasErrors())
check("and mentions the cycle", any("cycle" in m for m in errors(r)), str(errors(r)))

# Unknown node type.
g = tree(node("a", "NoSuchType"))
r = VB.ValidateTree(g)
check("an unknown node type is an error", r.HasErrors())

# ================================================================= flow graph

print("\n== validator: flow graphs ==")

def flink(from_node, from_pin, to_node, to_pin):
    link = VB.GraphLink()
    link.FromNode = from_node
    link.FromPin = from_pin
    link.ToNode = to_node
    link.ToPin = to_pin
    return link


# Type compatibility.
g = VB.Graph()
g.Kind = "flow"
g.Name = "F"
# FlowOnEvent's only PIN is an Out exec. The event NAME is the node's
# EventName FIELD, not a literal pin -- SetLiteral("EventName", ...) is an
# unknown pin and is itself a validation error, which masked the
# exec-to-data case this section is actually testing.
ev = node("ev", "FlowOnEvent")
ev.EventName = "OnUpdate"
log = node("lg", "Log", Message="hello")
g.Nodes.extend([ev, log])
# exec -> DATA pin: 'Out' is Exec, 'Message' is a String input. This is the
# illegal direction. (ev.Out -> lg.In would be exec->exec, which is legal and
# is wired below for the no-exec-path case.)
g.Links.extend([flink("ev", "Out", "lg", "Message")])
r = VB.ValidateFlow(g)
# Report errors(), not warnings(): this asserts on an ERROR, and the detail
# passed to check() was the (necessarily empty) warning list.
check("an exec wire cannot feed a data pin", r.HasErrors(), str(errors(r)))

# clear(), not extend([]): the opaque link list is LIVE, so extend([]) appends
# nothing and the graph under test would still be the previous, invalid one.
g.Links.clear()
r = VB.ValidateFlow(g)
check("a flow graph with no exec path warns", r.WarningCount() > 0,
      str(warnings(r)))
check("but is not an error", not r.HasErrors(), str(errors(r)))

g.Links.extend([flink("ev", "Out", "ev", "In")])
r = VB.ValidateFlow(g)
check("linking a pin to itself is caught", r.HasErrors() or r.WarningCount() > 0)

# ==================================================================== compiler

print("\n== compiler ==")

# The enemy tree from the plan, as data.
g = tree(
    node("root", "Selector", ["dead", "engage"]),
    node("dead", "ConditionBool", Value="false"),
    node("engage", "Sequence", ["healthy", "act"]),
    node("healthy", "Invert", ["hurt"]),
    node("hurt", "ConditionHealthBelow", Threshold="25.0"),
    node("act", "Selector", ["approach", "attack"]),
    node("approach", "Sequence", ["notInRange", "moveTo"]),
    node("notInRange", "Invert", ["inRange"]),
    node("inRange", "ConditionDistanceTo", Target="target", Range="2.0", Op="<="),
    node("moveTo", "MoveTo", Latent=True, Target="target", Speed="3.5"),
    # 'strike' has ONE parent -- the Cooldown's own child. The plan's first
    # draft listed it under 'attack' too, so it ran once per parent and every
    # hit dealt double damage. The validator now rejects that; the real
    # EnemyChase.wlbtree is built the same way as here.
    node("attack", "Sequence", ["inRange2", "rested"]),
    node("inRange2", "ConditionDistanceTo", Target="target", Range="2.0", Op="<="),
    node("rested", "Cooldown", ["strike"], Seconds="1.0"),
    node("strike", "Attack", Target="target", Damage="10.0"),
    name="EnemyGuard",
    blackboard=[bkey("target", "Entity"), bkey("attackCooldown", "Float", "0.0")],
)

r = VB.ValidateTree(g)
check("the plan's enemy tree validates", not r.HasErrors(), str(errors(r)))

py = VB.CompileBehavior(g)
check("it compiles to non-empty Python", len(py) > 0)
check("the output is a class", "class EnemyGuard" in py, py[:200])
check("it imports the shared runtime", "import wl_behavior" in py)
# The tree must be DATA. A condition leaf legitimately compiles to a
# conditional lambda ("SUCCESS if X else FAILURE") -- that is a single
# expression, not control flow. What must NOT appear is nested control flow
# standing in for the tree walk, which is what generated if/else would look
# like. So assert on the SHAPE of the topology, not on a substring.
body = py[py.index("def _build"):py.index("def tick")] if "def _build" in py else ""
check("the topology is a data table", "'t':" in body and "kids" in body)
check("every node is a dict entry, not a statement",
      body.count("return {") == 1 and "if node" not in body, body[:200])
# One lambda per LEAF, none per composite. That ratio is the real invariant:
# a generated-if/else compiler would emit one branch per node instead. The
# enemy tree has 6 leaves and 9 composites, so assert lambdas == leaves.
leaves = sum(1 for n in g.Nodes if n.TypeId.startswith("Condition") or n.TypeId in ("Attack", "MoveTo", "Wait"))
check("composites are referenced, not expanded",
      "Sequence', 'kids'" in body and body.count("lambda") == leaves,
      "lambdas=%d leaves=%d" % (body.count("lambda"), leaves))
check("every composite carries kids", body.count("'kids'") >= 8,
      "kids=%d" % body.count("'kids'"))
check("topology is emitted as data", "'kids'" in py)
check("child ids appear in the table", "'approach'" in py or '"approach"' in py)
declared = [k.Name for k in g.Blackboard]
check("blackboard keys are declared",
      "_blackboard = {" in py and all('"%s"' % k in py for k in declared),
      "declared=%s\n%s" % (declared, py[py.find("_blackboard"):py.find("_blackboard") + 260]))
check("and the declared types are emitted too", "_blackboard_types = {" in py)

# The generated code must actually be valid Python. This is the check that
# catches a string-concatenation bug in the emitter, which is otherwise
# invisible until the NPC is loaded.
try:
    compile(py, "<generated>", "exec")
    compiles = True
    detail = ""
except SyntaxError as e:
    compiles = False
    detail = "line %s: %s" % (e.lineno, e.msg)
check("the generated Python is syntactically valid", compiles, detail)

# Determinism: same tree twice must emit byte-identical output.
py2 = VB.CompileBehavior(g)
check("the compiler is deterministic", py == py2)

# A tree that fails validation must not emit code. A silent "repair" here is
# how an authoring mistake becomes a shipping bug.
g_bad = tree(node("root", "Selector", ["a", "b"]),
             node("a", "Sequence"), node("b", "Sequence"))
py_bad = VB.CompileBehavior(g_bad)
check("an invalid tree compiles to EMPTY", py_bad == "", py_bad[:120])

# Child order is priority and must never be sorted.
g_order = tree(node("root", "Selector", ["zzz", "aaa"]),
               node("zzz", "ConditionBool", Value="false"),
               node("aaa", "ConditionBool", Value="true"))
py_order = VB.CompileBehavior(g_order)
i_zzz = py_order.find("zzz")
i_aaa = py_order.find("aaa")
check("child order is preserved, not sorted", i_zzz < i_aaa,
      "zzz@%d aaa@%d" % (i_zzz, i_aaa))

# An int literal feeding a float pin must emit a decimal point, or a Vec3
# constructor truncates.
g_vec = tree(node("root", "Selector", ["a"]),
             node("a", "ConditionCompare", Left="3", Right="2.0", Op="<"),
             name="Vec")
py_vec = VB.CompileBehavior(g_vec)
# std::to_string renders 3.0f as "3.000000" -- a float literal, and the
# contract. What matters is that the comparison is NOT against a bare int.
import re as _re
_m = _re.search(r"\((\d+\.\d+)\s*<", py_vec)
check("an int literal on a float pin emits a float, not a bare int",
      _m is not None, py_vec[-300:])

# ==================================================================== example

print("\n== shipped example tree ==")

EXAMPLE = os.path.join(TREES, "EnemyGuard.wlbtree")
if os.path.exists(EXAMPLE):
    check("EnemyGuard.wlbtree exists", True)
    gex = VB.Graph()
    err = VB.Load(EXAMPLE, gex)
    check("it loads", err is None,
          err.Message if err is not None else "")
    if err is None:
        check("it is a tree", gex.Kind == "tree", gex.Kind)
        rex = VB.ValidateTree(gex)
        check("it validates with no errors", not rex.HasErrors(), rex.Report(EXAMPLE))
        pyex = VB.CompileBehavior(gex)
        check("it compiles", len(pyex) > 0)
        try:
            compile(pyex, "<example>", "exec")
            check("the shipped example generates valid Python", True)
        except SyntaxError as e:
            check("the shipped example generates valid Python", False,
                  "line %s: %s" % (e.lineno, e.msg))
        check("the shipped example has no warnings", rex.WarningCount() == 0,
              rex.Report(EXAMPLE))
else:
    check("EnemyGuard.wlbtree exists", False, EXAMPLE)

print("\n%d passed, %d failed" % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
