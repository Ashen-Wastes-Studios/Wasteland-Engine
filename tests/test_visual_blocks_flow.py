"""General Visual Blocks (.wlblocks) -- flow graph codegen and runtime.

Separate from test_visual_blocks.py, which covers the BEHAVIOUR tree path.
The two share the registry, serializer and validator; what differs is the
emitted code shape, so they are verified separately too.

Every check asserts on a read-back VALUE after running the generated class.
"Did not throw" is not a result: a generated method can be syntactically
perfect, raise nothing, and do nothing at all.

Two facts about the palette drive most of the wiring below:

  - An EXPRESSION node (Add, LessThan, GetVariable) has NO exec pins. It
    produces a value on a data wire and is not "run".
  - A STATEMENT node (SetVariable, Branch, While) has exec in/out pins and is
    reached by walking exec links from the event.
"""

import ast
import os
import sys
import tempfile
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
EDITOR_ROOT = os.path.join(os.path.dirname(HERE), "DemonCore-Editor")
SCRIPTS = os.path.join(EDITOR_ROOT, "assets", "scripts")
HEADLESS = os.path.join(HERE, "headless")

for p in (HERE, HEADLESS, SCRIPTS):
    if p not in sys.path:
        sys.path.insert(0, p)

# Resolve exactly the module t.sh selected and printed. The old hard-coded
# Release path bypassed WASTELAND_MODULE_DIR and silently tested a stale .pyd.
import wlpath  # noqa: E402

MODULE_DIR = wlpath.setup()

# Gate on the import BEFORE any assertion runs. A run that exits with a native
# fault and prints nothing has no opinion about the layer, and its silence
# must not be read as a green suite.
import Wasteland  # noqa: E402,F401
import Wasteland.VisualBlocks as vb  # noqa: E402

PASS = 0
FAIL = 0
FAILURES = []


def check(label, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print("  ok   %s" % label)
    else:
        FAIL += 1
        FAILURES.append("%s %s" % (label, detail))
        print("  FAIL %s %s" % (label, detail))


def section(title):
    print("\n== %s ==" % title)


# ---------------------------------------------------------------- helpers

def node(type_id, node_id, literals=None, event=None):
    n = vb.GraphNode()
    n.Id = node_id
    n.TypeId = type_id
    if event:
        n.EventName = event
    for key, value in (literals or {}).items():
        n.SetLiteral(key, value, True)
    return n


def link(from_node, from_pin, to_node, to_pin):
    l = vb.GraphLink()
    l.FromNode = from_node
    l.FromPin = from_pin
    l.ToNode = to_node
    l.ToPin = to_pin
    return l


def build(name, nodes, links, variables=None):
    g = vb.Graph()
    g.Kind = "flow"
    g.Name = name
    for spec in (variables or []):
        var = vb.GraphVariable()
        var.Name = spec["name"]
        var.Type = getattr(vb.PinType, spec.get("type", "Float"))
        var.DefaultLiteral = spec.get("default", "")
        g.Variables.append(var)
    for n in nodes:
        g.Nodes.append(n)
    for l in links:
        g.Links.append(l)
    return g


def compile_ok(graph, label):
    """Validate, compile, and assert the output is syntactically valid Python.

    CompileFlow returns the source directly and an EMPTY string on failure,
    which is a different contract from CompileTree's 4-tuple. Validating first
    means a graph the validator rejects is reported as a compile failure
    rather than as a syntax error in an empty string.
    """
    result = vb.ValidateFlow(graph)
    if result.HasErrors():
        check(label, False, " | ".join(f.Message for f in result.Findings[:3]))
        return None

    py = vb.CompileFlow(graph)
    if not py:
        check(label, False, "CompileFlow returned empty")
        return None
    try:
        ast.parse(py)
    except SyntaxError as exc:
        check(label, False, "SyntaxError line %s: %s" % (exc.lineno, exc.msg))
        return None
    check(label, True)
    return py


def run(name, py):
    """Import the generated class and return an instance with no entity."""
    fd, path = tempfile.mkstemp(suffix=".py")
    os.close(fd)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(py)
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return getattr(module, name)(None)


def attach_and_tick(graph, scene, entity, label):
    """Attach compiled flow code as a ScriptComponent and drive a live Scene.

    ScriptEngine imports the generated module by its file stem and looks up the
    graph's generated class by name. The temporary source lives under the
    project's scripts directory only for the duration of the runtime tick.
    """
    py = compile_ok(graph, label + " compiles for runtime")
    if not py:
        return False

    module_name = "VisualBlocksLive_" + uuid.uuid4().hex
    path = os.path.join(SCRIPTS, module_name + ".py")
    try:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(py)
        Wasteland.Project.Get().AttachAssetRoot(EDITOR_ROOT)
        script = Wasteland.ScriptComponent()
        script.ScriptPath = "scripts/" + module_name + ".py"
        script.ScriptName = graph.Name
        entity.AddOrReplaceScriptComponent(script)
        check(label + " attached to the runtime entity",
              entity.HasScriptComponent())
        scene.OnUpdateRuntime(1.0 / 60.0)
        return True
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


# ==================================================================== palette

section("flow palette is registered")

for required in ["FlowOnEvent", "SetVariable", "GetVariable", "Branch", "While",
                  "ForEach", "Break", "Add", "Subtract", "Multiply", "Divide",
                  "Modulo", "LessThan", "LessOrEqual", "GreaterThan",
                  "GreaterOrEqual", "Equal", "NotEqual", "Negate", "Clamp",
                  "And", "Or", "Not", "Log", "GetPosition", "SetTransform",
                  "GetHealth", "Raycast", "PlaySound"]:
    check("%s registered" % required, vb.HasNodeType(required))

check("flow palette is substantial (>=20)", len(vb.FlowNodes()) >= 20,
      "got %d" % len(vb.FlowNodes()))
check("behaviour taxonomy still registers", len(vb.BehaviorNodes()) >= 25,
      "got %d" % len(vb.BehaviorNodes()))


section("CompileFlow rejects invalid graphs on its own")

invalid = build("InvalidExecToData",
                [node("FlowOnEvent", "ev", event="OnUpdate"),
                 node("Log", "log", {"Message": "not an exec pin"})],
                [link("ev", "Out", "log", "Message")])
invalid_source = vb.CompileFlow(invalid)
check("CompileFlow emits no partial Python for invalid wiring",
      invalid_source == "",
      "invalid graph emitted %d characters" % len(invalid_source))


# =============================================================== control flow

section("Branch picks the true arm")

g = build("BranchTrue",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("Branch", "br", {"Condition": "True"}),
           node("SetVariable", "yes", {"Name": "hit", "Value": "1"}),
           node("SetVariable", "no", {"Name": "hit", "Value": "2"})],
          [link("ev", "Out", "br", "In"),
           link("br", "True", "yes", "In"),
           link("br", "False", "no", "In")],
          [{"name": "hit", "default": "0"}])
py = compile_ok(g, "true-arm graph compiles")
if py:
    check("emits an if statement", "\t\tif " in py)
    check("emits an else statement", "else:" in py)
    inst = run("BranchTrue", py)
    inst.OnUpdate()
    check("true arm ran, false did not", inst.hit == 1.0, "hit=%r" % inst.hit)


section("Branch picks the false arm")

g = build("BranchFalse",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("Branch", "br", {"Condition": "False"}),
           node("SetVariable", "yes", {"Name": "hit", "Value": "1"}),
           node("SetVariable", "no", {"Name": "hit", "Value": "2"})],
          [link("ev", "Out", "br", "In"),
           link("br", "True", "yes", "In"),
           link("br", "False", "no", "In")],
          [{"name": "hit", "default": "0"}])
py = compile_ok(g, "false-arm graph compiles")
if py:
    inst = run("BranchFalse", py)
    inst.OnUpdate()
    check("false arm ran", inst.hit == 2.0, "hit=%r" % inst.hit)


section("an empty branch body is still valid Python")

g = build("BranchEmpty",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("Branch", "br", {"Condition": "True"})],
          [link("ev", "Out", "br", "In")],
          [{"name": "hit", "default": "0"}])
py = compile_ok(g, "branch with no arms compiles")
if py:
    check("empty arms emit pass", "pass" in py)
    inst = run("BranchEmpty", py)
    inst.OnUpdate()
    check("call returned without raising", inst.hit == 0.0)


section("nested branches nest")

g = build("Nested",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("Branch", "outer", {"Condition": "True"}),
           node("Branch", "inner", {"Condition": "True"}),
           node("SetVariable", "deep", {"Name": "hit", "Value": "42"})],
          [link("ev", "Out", "outer", "In"),
           link("outer", "True", "inner", "In"),
           link("inner", "True", "deep", "In")],
          [{"name": "hit", "default": "0"}])
py = compile_ok(g, "nested branches compile")
if py:
    check("two if statements emitted", py.count("if ") >= 2,
          "got %d" % py.count("if "))
    inst = run("Nested", py)
    inst.OnUpdate()
    check("deeply nested body executed", inst.hit == 42.0, "hit=%r" % inst.hit)


section("a bounded loop iterates and terminates")

# while get(i) < 5 { i = add(get(i), 1) }
g = build("CountToFive",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("SetVariable", "zero", {"Name": "i", "Value": "0"}),
           node("While", "loop"),
           node("LessThan", "test", {"B": "5"}),
           node("Add", "inc", {"B": "1"}),
           node("SetVariable", "store", {"Name": "i"}),
           node("GetVariable", "geti", {"Name": "i"})],
          [link("ev", "Out", "zero", "In"),
           link("zero", "Out", "loop", "In"),
           link("test", "Value", "loop", "Condition"),
           link("loop", "Body", "store", "In"),
           link("geti", "Value", "test", "A"),
           link("geti", "Value", "inc", "A"),
           link("inc", "Value", "store", "Value")],
          [{"name": "i", "default": "0"}])
py = compile_ok(g, "bounded loop compiles")
if py:
    # A condition re-evaluated per iteration, not hoisted above the loop:
    # `while True:` + an in-body `if not <test>: break` is what makes a
    # bounded loop terminate. A hoisted test spins forever.
    check("loop test lives inside the loop body",
          "while True:" in py and "if not (" in py)
    inst = run("CountToFive", py)
    inst.OnUpdate()
    check("loop ran to its bound", inst.i == 5.0, "i=%r" % inst.i)


section("Break leaves the loop early")

g = build("BreakEarly",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("SetVariable", "zero", {"Name": "i", "Value": "0"}),
           node("While", "loop"),
           node("Add", "inc", {"B": "1"}),
           node("SetVariable", "store", {"Name": "i"}),
           node("GetVariable", "geti", {"Name": "i"}),
           # The loop runs while the count is still under the cap; the Break
           # below is what makes it reachable before the condition fails.
           node("LessThan", "test", {"B": "10"}),
           node("GreaterOrEqual", "stopat", {"B": "3"}),
           node("Branch", "br"),
           node("Break", "stop")],
          [link("ev", "Out", "zero", "In"),
           link("zero", "Out", "loop", "In"),
           link("test", "Value", "loop", "Condition"),
           link("loop", "Body", "store", "In"),
           link("geti", "Value", "test", "A"),
           link("geti", "Value", "inc", "A"),
           link("geti", "Value", "stopat", "A"),
           link("inc", "Value", "store", "Value"),
           link("stopat", "Value", "br", "Condition"),
           link("store", "Out", "br", "In"),
           link("br", "True", "stop", "In")],
          [{"name": "i", "default": "0"}])
py = compile_ok(g, "break graph compiles")
if py:
    inst = run("BreakEarly", py)
    inst.OnUpdate()
    # The store precedes the test, so the break fires on the pass whose
    # PRE-increment value reaches 3 and leaves the increment applied: 4.
    check("break ended the loop at 4", inst.i == 4.0, "i=%r" % inst.i)
    # The real claim: the Break ended it, not the condition. Without the
    # Break this same loop runs to its cap of 10.
    check("loop exited early, not at its own bound", inst.i < 10.0,
          "i=%r (cap was 10)" % inst.i)


section("ForEach counts")

g = build("ForEachTest",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("SetVariable", "zero", {"Name": "total", "Value": "0"}),
           node("ForEach", "loop", {"Count": "4", "IndexVar": "i"}),
           node("Add", "sum", {"B": "1"}),
           node("SetVariable", "acc", {"Name": "total"}),
           node("GetVariable", "gettotal", {"Name": "total"})],
          [link("ev", "Out", "zero", "In"),
           link("zero", "Out", "loop", "In"),
           link("loop", "Body", "acc", "In"),
           # The running total feeds back in, or every pass recomputes 0+1.
           link("gettotal", "Value", "sum", "A"),
           link("sum", "Value", "acc", "Value")],
          [{"name": "total", "default": "0"}])
py = compile_ok(g, "ForEach graph compiles")
if py:
    check("emits a for/range statement", "for " in py and "range(" in py)
    inst = run("ForEachTest", py)
    inst.OnUpdate()
    check("iterated four times", inst.total == 4.0, "total=%r" % inst.total)


# The counterfactual that gives "exited early" its meaning: the SAME loop
# with the Break removed runs to its own condition.
section("without the Break the same loop reaches its cap")

g = build("NoBreak",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("SetVariable", "zero", {"Name": "i", "Value": "0"}),
           node("While", "loop"),
           node("LessThan", "test", {"B": "10"}),
           node("Add", "inc", {"B": "1"}),
           node("SetVariable", "store", {"Name": "i"}),
           node("GetVariable", "geti", {"Name": "i"})],
          [link("ev", "Out", "zero", "In"),
           link("zero", "Out", "loop", "In"),
           link("test", "Value", "loop", "Condition"),
           link("loop", "Body", "store", "In"),
           link("geti", "Value", "test", "A"),
           link("geti", "Value", "inc", "A"),
           link("inc", "Value", "store", "Value")],
          [{"name": "i", "default": "0"}])
py = compile_ok(g, "no-break loop compiles")
if py:
    inst = run("NoBreak", py)
    inst.OnUpdate()
    check("loop terminated on its condition at 10", inst.i == 10.0,
          "i=%r" % inst.i)


# ================================================================= variables

section("SetVariable writes and GetVariable reads")

g = build("VarTest",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("SetVariable", "set", {"Name": "counter", "Value": "7"}),
           node("GetVariable", "get", {"Name": "counter"}),
           node("Add", "plus", {"B": "3"}),
           node("SetVariable", "out", {"Name": "dst"})],
          [link("ev", "Out", "set", "In"),
           link("set", "Out", "out", "In"),
           link("get", "Value", "plus", "A"),
           link("plus", "Value", "out", "Value")],
          [{"name": "counter", "default": "0"},
           {"name": "dst", "default": "0"}])
py = compile_ok(g, "variable round-trip compiles")
if py:
    inst = run("VarTest", py)
    check("declared default initialised", inst.dst == 0.0)
    inst.OnUpdate()
    check("value flowed source -> math -> sink", inst.dst == 10.0,
          "dst=%r (expected 7 + 3)" % inst.dst)


section("graph variables persist across event callbacks")

g = build("PersistentCounter",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("GetVariable", "get", {"Name": "count"}),
           node("Add", "increment", {"B": "1"}),
           node("SetVariable", "store", {"Name": "count"})],
          [link("ev", "Out", "store", "In"),
           link("get", "Value", "increment", "A"),
           link("increment", "Value", "store", "Value")],
          [{"name": "count", "default": "0"}])
py = compile_ok(g, "persistent-counter graph compiles")
if py:
    inst = run("PersistentCounter", py)
    inst.OnUpdate()
    inst.OnUpdate()
    check("the second callback observes the first callback's write",
          inst.count == 2.0,
          "count=%r after two increments" % inst.count)


section("a declared default survives until assigned")

g = build("VarDefault",
          [node("FlowOnEvent", "ev", event="OnUpdate")],
          [],
          [{"name": "score", "default": "12"}])
py = compile_ok(g, "unused-variable graph compiles")
if py:
    inst = run("VarDefault", py)
    check("default value initialised in __init__", inst.score == 12.0,
          "score=%r" % inst.score)


# ======================================================================= math

section("arithmetic computes")

for type_id, a, b, expected in [("Add", 3, 4, 7), ("Subtract", 9, 4, 5),
                                ("Multiply", 6, 7, 42), ("Divide", 8, 2, 4),
                                ("Modulo", 9, 4, 1), ("Add", 2, 2, 4)]:
    g = build("M_%s_%d" % (type_id, a),
              [node("FlowOnEvent", "ev", event="OnUpdate"),
               node(type_id, "m", {"A": str(a), "B": str(b)}),
               node("SetVariable", "out", {"Name": "dst"})],
              [link("ev", "Out", "out", "In"),
               link("m", "Value", "out", "Value")],
              [{"name": "dst", "default": "0"}])
    py = compile_ok(g, "%s(%d,%d) compiles" % (type_id, a, b))
    if py:
        inst = run("M_%s_%d" % (type_id, a), py)
        inst.OnUpdate()
        check("%s(%d,%d) == %d" % (type_id, a, b, expected),
              inst.dst == float(expected), "dst=%r" % inst.dst)


section("Negate and Clamp")

g = build("NegClamp",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("Negate", "neg", {"A": "5"}),
           node("SetVariable", "outn", {"Name": "neg"})],
          [link("ev", "Out", "outn", "In"),
           link("neg", "Value", "outn", "Value")],
          [{"name": "neg", "default": "0"}])
py = compile_ok(g, "Negate compiles")
if py:
    inst = run("NegClamp", py)
    inst.OnUpdate()
    check("Negate(5) == -5", inst.neg == -5.0, "neg=%r" % inst.neg)

g = build("ClampTest",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("Clamp", "c", {"Value": "50", "Min": "0", "Max": "10"}),
           node("SetVariable", "out", {"Name": "v"})],
          [link("ev", "Out", "out", "In"),
           link("c", "Out", "out", "Value")],
          [{"name": "v", "default": "0"}])
py = compile_ok(g, "Clamp compiles")
if py:
    inst = run("ClampTest", py)
    inst.OnUpdate()
    check("Clamp(50, 0, 10) == 10", inst.v == 10.0, "v=%r" % inst.v)


section("comparison nodes feed a branch")

for type_id, a, b, expected in [("LessThan", 1, 2, True), ("LessThan", 2, 1, False),
                                ("GreaterOrEqual", 2, 2, True),
                                ("Equal", 5, 5, True), ("NotEqual", 5, 5, False),
                                ("LessOrEqual", 3, 3, True)]:
    g = build("C_%s_%d_%d" % (type_id, a, b),
              [node("FlowOnEvent", "ev", event="OnUpdate"),
               node(type_id, "c", {"A": str(a), "B": str(b)}),
               node("Branch", "br"),
               node("SetVariable", "yes", {"Name": "dst", "Value": "1"}),
               node("SetVariable", "no", {"Name": "dst", "Value": "0"})],
              [link("ev", "Out", "br", "In"),
               link("c", "Value", "br", "Condition"),
               link("br", "True", "yes", "In"),
               link("br", "False", "no", "In")],
              [{"name": "dst", "default": "0"}])
    py = compile_ok(g, "%s(%d,%d) compiles" % (type_id, a, b))
    if py:
        inst = run("C_%s_%d_%d" % (type_id, a, b), py)
        inst.OnUpdate()
        check("%s(%d,%d) is %s" % (type_id, a, b, expected),
              inst.dst == (1.0 if expected else 0.0), "dst=%r" % inst.dst)


section("logic nodes")

g = build("LogicTest",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("And", "a", {"A": "True", "B": "False"}),
           node("Or", "o", {"A": "True", "B": "False"}),
           node("Not", "n", {"A": "False"}),
           node("SetVariable", "outa", {"Name": "va"}),
           node("SetVariable", "outo", {"Name": "vo"}),
           node("SetVariable", "outn", {"Name": "vn"})],
          [link("ev", "Out", "outa", "In"),
           link("a", "Value", "outa", "Value"),
           # Chained in sequence: exec reaches only what is wired.
           link("outa", "Out", "outo", "In"),
           link("o", "Value", "outo", "Value"),
           link("outo", "Out", "outn", "In"),
           link("n", "Value", "outn", "Value")],
          # Bool-typed, because a bool cannot connect to a float pin -- the
          # validator rejects that wiring, which is the correct behaviour.
          [{"name": "va", "type": "Bool", "default": "False"},
           {"name": "vo", "type": "Bool", "default": "False"},
           {"name": "vn", "type": "Bool", "default": "False"}])
py = compile_ok(g, "logic graph compiles")
if py:
    inst = run("LogicTest", py)
    inst.OnUpdate()
    check("And(True,False) is False", inst.va is False, "va=%r" % inst.va)
    check("Or(True,False) is True", inst.vo is True, "vo=%r" % inst.vo)
    check("Not(False) is True", inst.vn is True, "vn=%r" % inst.vn)


# ================================================== expression node semantics

section("a data node inside a branch is scoped to that branch")

# i = 5; dst = 0; if i < 0 then dst = i + 100
g = build("Scoped",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("SetVariable", "init", {"Name": "i", "Value": "5"}),
           node("SetVariable", "initd", {"Name": "dst", "Value": "0"}),
           node("LessThan", "test", {"B": "0"}),
           node("Branch", "br"),
           node("GetVariable", "geti", {"Name": "i"}),
           node("Add", "plus", {"B": "100"}),
           node("SetVariable", "store", {"Name": "dst"})],
          [link("ev", "Out", "init", "In"),
           link("init", "Out", "initd", "In"),
           link("initd", "Out", "br", "In"),
           link("test", "Value", "br", "Condition"),
           link("br", "True", "store", "In"),
           link("geti", "Value", "plus", "A"),
           link("plus", "Value", "store", "Value")],
          [{"name": "i", "default": "0"}, {"name": "dst", "default": "0"}])
py = compile_ok(g, "branch-scoped data node compiles")
if py:
    inst = run("Scoped", py)
    inst.OnUpdate()
    check("guarded assignment did not run", inst.dst == 0.0,
          "dst=%r" % inst.dst)
    check("the condition itself was computed", inst.i == 5.0)


# =============================================================== determinism

section("codegen is deterministic")

g = build("Determinism",
          [node("FlowOnEvent", "ev", event="OnUpdate"),
           node("Add", "m", {"A": "2", "B": "3"}),
           node("SetVariable", "out", {"Name": "dst"})],
          [link("ev", "Out", "out", "In"), link("m", "Value", "out", "Value")],
          [{"name": "dst", "default": "0"}])
first = vb.CompileFlow(g)
second = vb.CompileFlow(g)
check("same graph emits byte-identical output", first == second)


section("generated output carries a provenance header")

check("names the source graph", "Determinism" in first)
check("warns against hand editing", "Do not edit" in first)


# ==================================================== engine-facing live Scene

section("GetPosition feeds SetTransform on a runtime-created Scene entity")

live_scene = Wasteland.Scene()
position_actor = live_scene.CreateEntity("flow_position_actor")
position_actor.SetPosition(Wasteland.Vec3(4.0, 5.0, 6.0))
g = build("FlowGetPositionLive",
          [node("FlowOnEvent", "ev", event="OnUpdateEntity"),
           node("GetPosition", "position"),
           node("SetTransform", "set", {"Rotation": "(0, 90, 0)",
                                           "Scale": "(2, 2, 2)"})],
          [link("ev", "Out", "set", "In"),
           link("position", "Position", "set", "Translation")])
if attach_and_tick(g, live_scene, position_actor, "GetPosition live graph"):
    transform = position_actor.GetTransform()
    check("GetPosition read the entity translation",
          abs(transform.Translation.x - 4.0) < 1e-5 and
          abs(transform.Translation.y - 5.0) < 1e-5 and
          abs(transform.Translation.z - 6.0) < 1e-5,
          "position=(%r,%r,%r)" % (transform.Translation.x,
                                    transform.Translation.y,
                                    transform.Translation.z))
    check("SetTransform committed rotation and scale",
          abs(transform.Rotation.y - 90.0) < 1e-5 and
          abs(transform.Scale.x - 2.0) < 1e-5,
          "rotation.y=%r scale.x=%r" % (transform.Rotation.y,
                                         transform.Scale.x))
live_scene.DestroyEntity(position_actor)
del position_actor
del live_scene


section("SetTransform writes separate TRS fields on a live entity")

live_scene = Wasteland.Scene()
transform_actor = live_scene.CreateEntity("flow_transform_actor")
g = build("FlowSetTransformLive",
          [node("FlowOnEvent", "ev", event="OnUpdateEntity"),
           node("SetTransform", "set", {"Translation": "(7, 2, -3)",
                                          "Rotation": "(0, 35, 0)",
                                          "Scale": "(1.5, 2, 0.5)"})],
          [link("ev", "Out", "set", "In")])
if attach_and_tick(g, live_scene, transform_actor, "SetTransform live graph"):
    transform = transform_actor.GetTransform()
    check("SetTransform translation read back",
          abs(transform.Translation.x - 7.0) < 1e-5 and
          abs(transform.Translation.y - 2.0) < 1e-5 and
          abs(transform.Translation.z + 3.0) < 1e-5)
    check("SetTransform rotation and scale read back",
          abs(transform.Rotation.y - 35.0) < 1e-5 and
          abs(transform.Scale.x - 1.5) < 1e-5 and
          abs(transform.Scale.y - 2.0) < 1e-5 and
          abs(transform.Scale.z - 0.5) < 1e-5)
live_scene.DestroyEntity(transform_actor)
del transform_actor
del live_scene


section("GetHealth gates a live Scene transform update")

live_scene = Wasteland.Scene()
health_actor = live_scene.CreateEntity("flow_health_actor")
health_actor.SetPosition(Wasteland.Vec3(0.0, 0.0, 0.0))
health = Wasteland.HealthComponent()
health.Max = 100.0
health.Current = 73.0
health_actor.AddOrReplaceHealthComponent(health)
g = build("FlowGetHealthLive",
          [node("FlowOnEvent", "ev", event="OnUpdateEntity"),
           node("GetHealth", "health"),
           node("GreaterThan", "healthy", {"B": "60"}),
           node("Branch", "branch"),
           node("SetTransform", "alive", {"Translation": "(8, 0, 0)"})],
          [link("ev", "Out", "branch", "In"),
           link("health", "Value", "healthy", "A"),
           link("healthy", "Value", "branch", "Condition"),
           link("branch", "True", "alive", "In")])
if attach_and_tick(g, live_scene, health_actor, "GetHealth live graph"):
    check("GetHealth observed the current component value",
          abs(health_actor.GetPosition().x - 8.0) < 1e-5,
          "position.x=%r; health was 73.0" % health_actor.GetPosition().x)
    check("health component remained unchanged",
          abs(health_actor.GetHealthComponent().Current - 73.0) < 1e-5)
live_scene.DestroyEntity(health_actor)
del health_actor
del live_scene


section("Raycast hit in a live Scene gates an entity effect")

live_scene = Wasteland.Scene()
ray_actor = live_scene.CreateEntity("flow_ray_actor")
ray_actor.SetPosition(Wasteland.Vec3(0.0, 0.0, 0.0))
wall = live_scene.CreateEntity("flow_ray_wall")
wall.SetPosition(Wasteland.Vec3(0.0, 0.0, 5.0))
body = Wasteland.RigidBody3DComponent()
body.Type = Wasteland.RigidBody3DType.Static
wall.AddOrReplaceRigidBody3DComponent(body)
collider = Wasteland.BoxCollider3DComponent()
collider.HalfExtents = Wasteland.Vec3(0.5, 0.5, 0.5)
collider.Density = 0.0
wall.AddOrReplaceBoxCollider3DComponent(collider)
live_scene.OnSimulationStart()
g = build("FlowRaycastLive",
          [node("FlowOnEvent", "ev", event="OnUpdateEntity"),
           node("Raycast", "ray", {"Origin": "(0, 0, 0)",
                                     "Direction": "(0, 0, 1)",
                                     "MaxDistance": "10"}),
           node("Branch", "branch"),
           node("SetTransform", "hit", {"Translation": "(9, 0, 0)"})],
          [link("ev", "Out", "branch", "In"),
           link("ray", "Hit", "branch", "Condition"),
           link("branch", "True", "hit", "In")])
if attach_and_tick(g, live_scene, ray_actor, "Raycast live graph"):
    check("Raycast hit the runtime collider and ran the true branch",
          abs(ray_actor.GetPosition().x - 9.0) < 1e-5,
          "position.x=%r" % ray_actor.GetPosition().x)
live_scene.DestroyEntity(ray_actor)
live_scene.DestroyEntity(wall)
del ray_actor, wall
del live_scene


section("PlaySound starts a registered clip through the live Scene")

live_scene = Wasteland.Scene()
sound_actor = live_scene.CreateEntity("flow_sound_actor")
mixer = live_scene.GetAudio().GetMixer()
clip_name = "vb_flow_probe_" + uuid.uuid4().hex
clip = Wasteland.Audio.AudioClip.FromSamples([0.25] * 44100, 44100)
mixer.RegisterClip(clip_name, clip)
voices_before = mixer.GetActiveVoiceCount()
g = build("FlowPlaySoundLive",
          [node("FlowOnEvent", "ev", event="OnUpdateEntity"),
           node("PlaySound", "sound", {"ClipName": clip_name})],
          [link("ev", "Out", "sound", "In")])
if attach_and_tick(g, live_scene, sound_actor, "PlaySound live graph"):
    voices_after = mixer.GetActiveVoiceCount()
    check("PlaySound started the named clip",
          mixer.HasClip(clip_name) and voices_after > voices_before,
          "voices %d -> %d" % (voices_before, voices_after))
live_scene.DestroyEntity(sound_actor)
del sound_actor, mixer, clip
del live_scene


# ============================================================ shipped samples

section("the shipped .wlblocks sample loads, validates and compiles")

import glob as _glob  # noqa: E402

GRAPH_DIR = os.path.join(os.path.dirname(HERE), "DemonCore-Editor", "assets",
                         "graphs")
samples = sorted(_glob.glob(os.path.join(GRAPH_DIR, "*.wlblocks")))
check("a sample graph ships", len(samples) >= 1,
      "none found in %s" % GRAPH_DIR)

for sample in samples:
    name = os.path.basename(sample)
    graph = vb.Graph()
    # Load returns None on success or a GraphError naming the problem -- NOT a
    # bool. Treating it as a bool silently ignores every load failure.
    err = vb.Load(sample, graph)
    check("%s loads" % name, err is None,
          getattr(err, "Message", "unknown error") if err is not None else "")
    if err is not None:
        continue

    check("%s is a flow graph" % name, graph.Kind == "flow", graph.Kind)
    result = vb.ValidateFlow(graph)
    check("%s validates clean" % name,
          not result.HasErrors(),
          " | ".join(f.Message for f in result.Findings[:3]))

    src = vb.CompileFlow(graph)
    check("%s compiles to Python" % name, bool(src))
    if src:
        try:
            ast.parse(src)
            check("%s generated Python parses" % name, True)
        except SyntaxError as exc:
            check("%s generated Python parses" % name, False,
                  "line %s: %s" % (exc.lineno, exc.msg))

    # And the generated module must actually import and expose its event.
    if src:
        try:
            inst = run(os.path.splitext(name)[0], src)
            check("%s generated class instantiates" % name, inst is not None)
        except Exception as exc:  # noqa: BLE001
            check("%s generated class instantiates" % name, False,
                  "%s: %s" % (type(exc).__name__, exc))


# ================================================================== summary

print("\n" + "-" * 60)
print("passed %d, failed %d" % (PASS, FAIL))
if FAILURES:
    print("\nfailures:")
    for f in FAILURES:
        print("  - %s" % f)
sys.exit(1 if FAIL else 0)
