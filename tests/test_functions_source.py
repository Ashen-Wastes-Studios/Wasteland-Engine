"""Source contract for Functions and Macros (Task B.2).

`tests/headless/test_functions.py` proves the behaviour; this suite proves the
wiring is the shape the plan requires, by reading the sources. It runs against a
stale or absent build, so it catches a registration that was never made.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "Wasteland", "src", "Wasteland")
SCHEMA = os.path.join(ROOT, "docs", "VISUAL_BLOCKS_SCHEMA.md")

PASSED = 0
FAILED = []


def check(cond, message):
    global PASSED
    if cond:
        PASSED += 1
    else:
        FAILED.append(message)
        print("  FAIL:", message)


def read(*parts):
    with open(os.path.join(ENGINE, *parts), "r", encoding="utf-8",
              errors="replace") as handle:
        return handle.read()


def test_graph_model_carries_functions():
    header = read("VisualBlocks", "Graph.h")
    check("struct GraphFunction;" in header,
          "GraphFunction is forward-declared beside the other graph types")
    check("using GraphFunctionList = std::vector<GraphFunction>;" in header,
          "GraphFunctionList alias exists")
    check("GraphFunctionList Functions;" in header,
          "Graph carries a Functions list")
    check('"flow" | "tree" | "function"' in header,
          "the Kind comment names all three kinds")
    check("std::string BasePath;" in header,
          "Graph carries BasePath for resolving relative function paths")

    # A function is a SIGNATURE; the body lives elsewhere. Conflating them is
    # how a function silently gains state.
    check("struct GraphFunctionEndpoint" in header,
          "endpoints are their own struct, not a reused GraphVariable")
    check("std::vector<GraphFunctionEndpoint> Inputs;" in header,
          "a function declares typed inputs")
    check("std::vector<GraphFunctionEndpoint> Outputs;" in header,
          "a function declares typed outputs")
    check("std::string Path;" in header,
          "a function names its body file")


def test_serializer_round_trips_functions():
    source = read("VisualBlocks", "GraphSerializer.cpp")
    check('key == "functions"' in source,
          "the root key allowlist accepts 'functions'")
    check('out << YAML::Key << "functions"' in source,
          "functions are emitted")
    check('out << YAML::Key << "inputs"' in source,
          "inputs are emitted")
    check('out << YAML::Key << "outputs"' in source,
          "outputs are emitted")
    check('out << YAML::Key << "path"' in source,
          "the body path is emitted")
    check('if (root["functions"])' in source,
          "functions are parsed")
    check('"a function has no \'name\'"' in source,
          "an unnamed function is an error, not a silent default")
    check('" has no \'name\'"' in source and "std::string(\"a function \") + what" in source,
          "an unnamed endpoint is an error naming which side it is on")
    check("PinTypeFromName" in source,
          "endpoint types go through the name translator, so a bad type is a "
          "diagnostic rather than an exception")


def test_base_path_is_set_by_the_loader():
    """Set in the SERIALIZER, so every loader sets it.

    It was first set only in FlowGraphSystem, and the probe showed the defect
    immediately: a graph loaded by a test or a tool carried an empty BasePath,
    so every function path resolved against the process working directory and
    the failure was a "body not found" naming the wrong directory.
    """
    serializer = read("VisualBlocks", "GraphSerializer.cpp")
    check("outGraph.BasePath =" in serializer,
          "GraphSerializer::Load records the owning graph's directory")
    check("std::filesystem::path(path).parent_path().string()" in serializer,
          "the base is the graph's directory, not the filesystem root")
    check("#include <filesystem>" in serializer,
          "GraphSerializer includes <filesystem> for the path work")


def test_call_function_resolves_its_pins():
    source = read("VisualBlocks", "NodeLibraryFlow.cpp")
    check('def.TypeId = "CallFunction"' in source,
          "CallFunction is registered")
    check("def.ResolvePins =" in source,
          "CallFunction resolves its pins per node")
    check("for (const GraphFunctionEndpoint &input : found->Inputs)" in source,
          "one input pin per declared parameter")
    check("for (const GraphFunctionEndpoint &output : found->Outputs)" in source,
          "one output pin per declared return value")
    check("node.Literals" in source,
          "the resolver reads the Name literal from node.Literals "
          "(a GraphNode's pin literals are NOT called Pins)")
    check("Compiler::SanitizeIdentifier(" in source,
          "the method name uses the SHARED sanitizer, not a second copy")

    # A resolver must be pure: the validator and the compiler both call it.
    registry = read("VisualBlocks", "NodeRegistry.h")
    check("ResolvePinsFn ResolvePins;" in registry,
          "NodeDef has a ResolvePins hook")
    check("static std::vector<PinDef> EffectivePins(const NodeDef &def," in registry,
          "EffectivePins lives on NodeRegistry, not on NodeDef")
    check("ResolvePinsFn ResolvePins;" in registry,
          "the resolver hook is declared on NodeDef")
    check("EffectivePins" in registry,
          "the accessor is declared on NodeRegistry")


def test_the_exec_successor_walk_uses_effective_pins():
    """`successorsOf` is the last consumer of the static pin set.

    It asked `def->FindPin(link.FromPin)` whether the from-pin was an exec pin.
    For FunctionEntry the static set is empty, so the answer was always "not
    exec" and the exec link was skipped -- the body compiled to a method whose
    last statement was the parameter assignment, with no return at all.
    """
    compiler = read("VisualBlocks", "Compiler.cpp")
    check("def->FindPin(link.FromPin)" not in compiler,
          "successorsOf no longer reads the static pin set")
    check("NodeRegistry::EffectivePins(*def, graph, node)" in compiler,
          "successorsOf resolves the from-pin through the effective set")
    check("fromPinIsExec" in compiler,
          "a miss is reported through a flag rather than a null pointer")


def test_the_function_signature_carries_its_parameters():
    """The authored parameter NAMES are the method's arguments.

    The first version emitted the event form `(self, *args, **kwargs)` for
    every method, so a body referencing `p` raised NameError on its first line
    -- the method existed but could not be called with its own parameters.
    """
    compiler = read("VisualBlocks", "Compiler.cpp")
    check("struct MethodTarget" in compiler,
          "a method target carries its parameter list")
    check("std::vector<std::string> params;" in compiler,
          "the parameter list is built from the declared inputs")
    check('out << "\\tdef " << methodName << "(self, " << signature << "):\\n";'
          in compiler,
          "the signature is emitted from the target, not hardcoded")
    check('"*args, **kwargs"' in compiler,
          "an EVENT method still keeps the host's variadic signature")


def test_effective_pins_are_used_at_every_consumer():
    """A dynamic pin set is only real if EVERY consumer resolves it.

    A consumer reading def->Pins sees none of a CallFunction node's parameter
    pins, so it would report nothing or miscompile -- and each consumer failing
    independently is how the answers drift.
    """
    validator = read("VisualBlocks", "Validator.cpp")
    check(validator.count("NodeRegistry::EffectivePins(") >= 3,
          "the validator resolves effective pins at all three of its pin scans "
          "(found %d)" % validator.count("NodeRegistry::EffectivePins("))

    # NodeDef::FindPin searches only the STATIC set, so the link check and the
    # literal check each needed the effective set as well. Missing them produced
    # six errors on a correct graph: "CallFunction has no pin p".
    check("FindEffectivePin" in validator,
          "the validator has one effective-pin lookup for links and literals")
    check(validator.count("FindEffectivePin(") >= 3,
          "links and literals both resolve through it (found %d)"
          % validator.count("FindEffectivePin("))
    check("fromDef->FindPin(link.FromPin)" not in validator,
          "the link check no longer reads the static pin set")
    check("def->FindPin(literal.Key)" not in validator,
          "the literal check no longer reads the static pin set")

    compiler = read("VisualBlocks", "Compiler.cpp")
    check(compiler.count("NodeRegistry::EffectivePins(") >= 2,
          "the compiler resolves effective pins in buildInputs and in the "
          "consumer walk (found %d)" % compiler.count("NodeRegistry::EffectivePins("))
    check(": effectivePins)" in compiler,
          "the resolved vector is what the loops iterate, not def.Pins")


def test_function_graph_emits_one_method():
    compiler = read("VisualBlocks", "Compiler.cpp")
    check("struct MethodTarget" in compiler,
          "methods are emitted from targets that carry their parameter list, "
          "so a function's name is not derived from an event node")
    check('if (graph.Kind == "function")' in compiler,
          "a function graph contributes its own method target")
    check('"_fn_" + name' in compiler,
          "the generated method name is _fn_<name>")
    check("if (methodTargets.empty())" in compiler,
          "the fallback OnUpdate stub is suppressed when a function supplies "
          "the method")
    check("EmitFunctionMethods(graph, out, error)" in compiler,
          "a flow graph splices its declared functions' bodies into the class")
    check("weakly_canonical" in compiler,
          "function paths are resolved and normalised")
    check("std::filesystem::exists(full)" in compiler,
          "a missing body file is an error, not a silently skipped method")
    check("declares a different function" in compiler,
          "a body file declaring a different function is rejected")


def test_function_graph_rules_are_validated():
    validator = read("VisualBlocks", "Validator.cpp")
    check("a function may not contain an event node" in validator,
          "an event node inside a function is an error")
    check("node.Id" in validator and "a function may not contain an event node"
          in validator,
          "the event-node error carries the node id so the author can find it")
    check("a function may not declare instance variables" in validator,
          "the no-instance-state scope rule is enforced")
    check("a function must contain a Return node" in validator,
          "a function without a Return is an error")
    check("a function must contain a FunctionEntry node" in validator,
          "a function without a parameter entry is an error")
    check("Return is only valid inside a function graph" in validator,
          "Return is rejected in a flow graph")
    check("FunctionEntry is only valid inside a function graph" in validator,
          "FunctionEntry is rejected in a flow graph")
    check("calls itself; function calls must " in validator,
          "self-recursion is rejected with the acyclicity rule named")
    check('graph.Kind != "function"' in validator,
          "the 'must have an event node' requirement exempts function graphs")
    check("CheckFunctionAcyclic(graph, out);" in validator,
          "the acyclic check is called outside the kind split -- inside the "
          "`else` branch it could never fire, because the check itself returns "
          "early for non-function graphs")


def test_entry_and_return_nodes_are_registered():
    source = read("VisualBlocks", "NodeLibraryFlow.cpp")
    check('def.TypeId = "FunctionEntry"' in source,
          "FunctionEntry is registered")
    check('def.TypeId = "Return"' in source,
          "Return is registered")
    check(source.count("def.ResolvePins =") >= 3,
          "CallFunction, FunctionEntry and Return all resolve their pins "
          "(found %d resolvers)" % source.count("def.ResolvePins ="))
    check('"return ("' in source,
          "Return emits a tuple, so a single output is still indexable")


def test_parsed_string_literals_are_unquoted():
    """ParseLiteral yields a QUOTED Python literal; raw text needs unquoting.

    Two call sites compared a parsed name against an unparsed one and so never
    matched: the CallFunction resolver (the node exposed only its base pins, so
    a correct graph reported "has no pin 'p'") and CheckFunctionAcyclic (the
    self-call check could never fire). Both now go through the shared
    UnquotePyLiteral, which is declared beside ParseLiteral because it is its
    inverse.
    """
    header = read("VisualBlocks", "Graph.h")
    check("std::string UnquotePyLiteral(const std::string &text);" in header,
          "UnquotePyLiteral is declared beside ParseLiteral")

    graph = read("VisualBlocks", "Graph.cpp")
    check("std::string UnquotePyLiteral(const std::string &text)" in graph,
          "UnquotePyLiteral is defined")

    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    check("functionName = UnquotePyLiteral(parsed);" in flow,
          "the CallFunction resolver unquotes the parsed name")

    validator = read("VisualBlocks", "Validator.cpp")
    check("target = UnquotePyLiteral(parsed);" in validator,
          "the acyclicity check unquotes the parsed name")


def test_effective_pin_lookups_do_not_dangle():
    """EffectivePins returns its vector BY VALUE.

    The first FindEffectivePin returned a pointer into that temporary, so the
    caller read freed memory -- and the symptom was "the pins do not exist",
    not a dangling read.
    """
    validator = read("VisualBlocks", "Validator.cpp")
    check("PinDef FindEffectivePin(const NodeDef &def, const Graph &graph," in validator,
          "FindEffectivePin returns a copy, not a pointer")
    check("bool &found" in validator,
          "a miss is reported through a flag rather than a null pointer")
    check("const PinDef pinDef =" in validator,
          "the literal check stores the copy")


def test_a_resolved_node_declares_its_static_pins():
    """`def.Pins` must carry everything that does NOT depend on the signature.

    The schema suite reads `def.Pins` statically to derive a node's documented
    signature. FunctionEntry declared none, so the registry reported an empty
    pin list for a node with a real exec OUT and the doc was forced to choose
    between lying and failing. The dynamic half (the parameter pins) still
    cannot be listed, and the doc says so.
    """
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    entry = flow.split('def.TypeId = "FunctionEntry";')[1][:800]
    check("def.Pins = { MakeExecOut(\"Out\", \"Then\") };" in entry,
          "FunctionEntry declares its static exec OUT in def.Pins")

    for type_id in ("CallFunction", "Return"):
        block = flow.split('def.TypeId = "%s";' % type_id)[1][:600]
        check("MakeExecIn(\"In\", \"In\")" in block,
              "%s declares its static exec IN in def.Pins" % type_id)


def test_the_flow_compare_does_not_collide_with_the_tree_compare():
    """Registration is keyed by TypeId, so one name cannot serve two libraries.

    The flow `Compare` was registered after the behaviour-tree `Compare`, which
    REPLACED the tree node's definition -- its role flipped to whatever the
    flow node declared and its pin set changed, so "Compare has role
    'condition'" and "Compare compiles standalone" both failed on a graph that
    had done nothing wrong.
    """
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    check('def.TypeId = "CompareValue";' in flow,
          "the flow comparison is CompareValue, not Compare")
    check('def.TypeId = "Compare";' not in flow,
          "the flow library no longer claims the tree's TypeId")

    v1 = read("VisualBlocks", "NodeLibraryV1.cpp")
    check('def.TypeId = "Compare";' in v1,
          "the behaviour-tree Compare is still registered")


def test_the_schema_table_does_not_register_its_own_keys():
    """`_documented_signatures()` reads the first cell of any 3-column
    backticked row as a node type ID.

    A table documenting schema versions used `| `1` |`, `| `2` |` and
    `| `schema` |` as those cells, so the schema suite reported three phantom
    node types. The key column must not be a bare value.
    """
    with open(SCHEMA, "r", encoding="utf-8", errors="replace") as handle:
        doc = handle.read()
    check("| `1` |" not in doc and "| `2` |" not in doc,
          "no row keys on a bare schema number")
    check("| `schema` |" not in doc,
          "no row keys on the word schema")
    check("| `Vector` in vec3" in doc,
          "Vec3Scale's renamed input is documented")


def test_vector_primitives_exist():
    """ClampLen needs a length and a scalar multiply; neither existed."""
    source = read("VisualBlocks", "NodeLibraryFlow.cpp")
    check('def.TypeId = "Vec3Length"' in source,
          "Vec3Length is registered")
    check('def.TypeId = "Vec3Scale"' in source,
          "Vec3Scale is registered")
    check('def.TypeId = "Max"' in source,
          "Max is registered, enabling a branch-free clamp")
    check("** 0.5" in source,
          "Vec3Length needs no math import (the generated module imports only "
          "Wasteland)")
    check("Wasteland.Vec3(" in source,
          "Vec3Scale constructs a real Vec3 -- Vec3 has no float __mul__")


def test_headless_suite_exists():
    suite = os.path.join(ROOT, "tests", "headless", "test_functions.py")
    check(os.path.isfile(suite), "the headless function suite exists")
    text = open(suite, "r", encoding="utf-8").read()
    check("ClampLen" in text, "the suite exercises ClampLen")
    check("called twice" in text or "called twice" in text,
          "the suite calls the function twice")
    check("must fail validation naming the node" in text,
          "the suite carries the plan's negative control")


def main():
    test_graph_model_carries_functions()
    test_serializer_round_trips_functions()
    test_base_path_is_set_by_the_loader()
    test_call_function_resolves_its_pins()
    test_the_exec_successor_walk_uses_effective_pins()
    test_the_function_signature_carries_its_parameters()
    test_effective_pins_are_used_at_every_consumer()
    test_function_graph_emits_one_method()
    test_function_graph_rules_are_validated()
    test_entry_and_return_nodes_are_registered()
    test_parsed_string_literals_are_unquoted()
    test_effective_pin_lookups_do_not_dangle()
    test_a_resolved_node_declares_its_static_pins()
    test_the_flow_compare_does_not_collide_with_the_tree_compare()
    test_the_schema_table_does_not_register_its_own_keys()
    test_vector_primitives_exist()
    test_headless_suite_exists()

    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
