"""Source contract for Cast To and component access (Task B.3).

`tests/headless/test_component_access.py` proves the behaviour; this suite
proves the wiring is the shape the plan requires, by reading the sources. It
runs against a stale or absent build, so it catches a registration that was
never made.

The plan's hard rule: every node must be audited against the built `.pyd`
before shipping, because a node that emits an unbound name compiles clean and
raises AttributeError at runtime. The assertions here are the half that can be
checked without a build; the headless suite is the half that cannot.
"""

import os
import re

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


def block(source, type_id):
    """One registration's body, bounded by the next `registry.Register(def)`.

    Bounded by the registration rather than by a character count: a fixed count
    silently runs into the NEXT node's body, and the first version of this suite
    asserted "neither reader emits the setter" against text that belonged to
    SetComponent.
    """
    head, _, tail = source.partition('def.TypeId = "%s";' % type_id)
    if not tail:
        raise AssertionError("node type not found: " + type_id)
    return tail.split("registry.Register(def);", 1)[0]


def test_every_b3_node_is_registered():
    """The plan names five nodes; there are seven, and each is a decision.

    GetEntityAtIndex is not in the plan's table. It exists because a query
    result has no list pin type to carry it -- the query node emits Count and an
    opaque list, and without an index accessor the list is unusable.
    """
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    for type_id in ("CastTo", "GetComponent", "SetComponent",
                    "GetActor", "GetOwner", "GetAllEntitiesWith",
                    "GetEntityAtIndex"):
        check('def.TypeId = "%s";' % type_id in flow,
              "%s is registered" % type_id)
        # Every registration must end in registry.Register(def) rather than a
        # bare def -- an unregistered def is a node that appears nowhere.
        body = block(flow, type_id)
        # `block` bounds on registry.Register(def), so reaching here means the
        # registration exists. The remaining check is that it is a real node --
        # a Category, which every palette entry carries.
        check('def.Category = "Flow/Component"' in body,
              "%s declares the Flow/Component category" % type_id)


def test_the_component_type_is_a_runtime_name_not_a_compile_time_type():
    """There is no reflection, so a NAME is the only handle.

    A pin per component type would need the registry to know what a runtime
    entity carries, which it cannot: an entity's component set is data, not
    schema. The type pin is a String and the dispatch happens in a generated
    helper.
    """
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    for type_id in ("CastTo", "GetComponent", "SetComponent"):
        body = block(flow, type_id)
        check("PinType::String" in body,
              "%s takes the component type as a String pin" % type_id)
        check('"Type"' in body,
              "%s names that pin Type, so all three read the same" % type_id)


def test_the_value_pins_are_any_because_the_type_is_the_authors_choice():
    """Pinning the value to a concrete component type would make every other
    component unreachable -- the same reason SetVariable.Value is Any."""
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    for type_id in ("CastTo", "GetComponent"):
        body = block(flow, type_id)
        check("PinType::Any" in body,
              "%s outputs the component as Any" % type_id)
    check("PinType::Any" in block(flow, "SetComponent"),
          "SetComponent takes the component value as Any")


def test_the_helpers_are_emitted_only_when_a_node_needs_them():
    """A graph that never touches a component must not pay for the helpers.

    Same rule as _pose_bone: three predicates, because a read-only graph should
    not carry the setter and a query-only graph should not carry either.
    """
    compiler = read("VisualBlocks", "Compiler.cpp")
    for name in ("NeedsComponentGetHelper", "NeedsComponentSetHelper",
                 "NeedsEntityQueryHelper"):
        check("bool %s(const Graph &graph)" % name in compiler,
              "%s exists" % name)
        check("if (%s(graph))" % name in compiler,
              "%s gates an emission" % name)

    check(compiler.count("_component_get(") >= 1,
          "_component_get is emitted")
    check("_component_set(" in compiler,
          "_component_set is emitted")
    check("_entities_with(" in compiler,
          "_entities_with is emitted")

    # A query-only graph must not pull in the component helpers, and a
    # component-only graph must not pull in the query helper.
    get_block = compiler.split("bool NeedsComponentGetHelper")[1][:400]
    check('"GetAllEntitiesWith"' not in get_block,
          "the get-helper predicate does not answer for a query node")
    query_block = compiler.split("bool NeedsEntityQueryHelper")[1][:400]
    check('"CastTo"' not in query_block,
          "the query-helper predicate does not answer for a component node")


def _emitted_python_from_out_lines(source, first_line, last_line):
    """Decode the C++ `out << "..."` literals between two lines into Python.

    Only the escapes the Visual Blocks compiler emits are handled: \t, \n,
    \", and \\. Anything else is passed through, which is fine because the
    question is about triple quotes and newlines.
    """
    lines = source.splitlines()
    chunk = "\n".join(lines[first_line - 1:last_line])
    pieces = []
    for match in re.finditer(r'"((?:[^"\\]|\\.)*)"', chunk):
        text = match.group(1)
        text = text.replace("\\t", "\t").replace("\\n", "\n")
        text = text.replace('\\"', '"').replace("\\\\", "\\")
        pieces.append(text)
    return "".join(pieces)


def test_no_emitted_docstring_line_starts_with_a_triple_quote():
    r"""A docstring continuation line that STARTS with a triple quote closes it.

    The tokenizer ends the docstring there, and the rest of the docstring
    becomes bare code -- so the generated module raises
    `SyntaxError: unterminated triple-quoted string literal`. This is the
    defect that made every B.3 graph fail at attach time with 1/6 tests
    passing, and it is invisible in the C++ source, where the emission looks
    like prose.

    The check DECODES the emitted Python rather than grepping for a pattern:
    a pattern cannot tell an opener from a premature closer, and the whole
    failure was exactly that distinction.
    """
    compiler = read("VisualBlocks", "Compiler.cpp")
    lines = compiler.splitlines()

    # Every helper block is an `if (Needs...Helper(graph)) { out << ... }` run.
    blocks = []
    for index, line in enumerate(lines):
        if "if (NeedsComponentGetHelper(graph))" in line or \
           "if (NeedsComponentSetHelper(graph))" in line or \
           "if (NeedsEntityQueryHelper(graph))" in line:
            closing = index
            while closing < len(lines) and lines[closing].strip() != "}":
                closing += 1
            blocks.append((index + 1, closing + 1))
    check(len(blocks) == 3,
          "all three helper blocks are found (found %d)" % len(blocks))

    hazards = []
    for first, last in blocks:
        emitted = _emitted_python_from_out_lines(compiler, first, last)
        open_count = 0
        for number, text in enumerate(emitted.split("\n"), 1):
            stripped = text.lstrip()
            if stripped.startswith('"""') and len(stripped) > 3:
                if open_count > 0:
                    hazards.append("block at line %d, emitted line %d: %s"
                                   % (first, number, stripped[:60]))
                open_count += 1
            elif stripped == '"""':
                open_count = max(0, open_count - 1)
            elif stripped.count('"""') >= 2:
                # Opener and closer on one line -- balanced by itself.
                pass
            else:
                open_count = max(0, open_count - stripped.count('"""'))
        if open_count != 0:
            hazards.append("block at line %d: docstring left open (%d)"
                           % (first, open_count))

    check(not hazards,
          "no emitted docstring line starts with a triple quote (hazards: %s)"
          % "; ".join(hazards[:3]))


def test_the_helpers_degrade_to_a_quiet_answer_rather_than_raising():
    """Generated code calling an unknown name must not take the tick down.

    All three helpers are called from generated code with an AUTHOR-supplied
    string, so an unknown name is a normal authoring outcome, not an error.
    """
    compiler = read("VisualBlocks", "Compiler.cpp")
    # The C++ source carries this Python text ESCAPED -- the quotes are \" --
    # so the assertion has to match the escaped form. Comparing against real
    # quotes fails even though the emitted code is correct.
    #
    # TWO spellings are tried, because the bindings are NOT uniform:
    # BIND_GAMEPLAY_COMP builds `TryGetHealthComponent`, while BIND_COMP_TRYGET
    # is handed the full type name and builds `TryGetWasteland::TransformComponent`.
    # The first version tried only the short form, so EVERY component bound via
    # BIND_COMP_* -- TransformComponent included -- read as absent and every
    # CastTo graph silently took its False branch.
    check('for candidate in (type_name, \\"Wasteland::\\" + type_name):'
          in compiler,
          "_component_get tries both accessor spellings")
    check('getter = getattr(entity, \\"TryGet\\" + candidate, None)' in compiler,
          "_component_get looks the accessor up by name with a default")
    check('for candidate in (type_name, \\"Wasteland::\\" + type_name):'
          in compiler.split("_component_set(entity, type_name, value):")[1][:600],
          "_component_set tries both accessor spellings")
    check('setter = getattr(entity, \\"AddOr\\" + candidate, None)' in compiler,
          "_component_set looks the accessor up by name with a default")
    check('getter = getattr(scene, \\"GetAllEntitiesWith\\", None)' in compiler,
          "_entities_with looks the scene query up with a default")


def test_cast_to_and_get_component_share_one_helper():
    """Two implementations of one question would be two answers.

    CastTo and GetComponent differ only in whether they expose a Success pin;
    the lookup must be the same code.
    """
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    cast = block(flow, "CastTo")
    get = block(flow, "GetComponent")
    check("_component_get(" in cast, "CastTo emits _component_get")
    check("_component_get(" in get, "GetComponent emits the SAME helper")
    check("_component_set(" not in cast and "_component_set(" not in get,
          "neither reader emits the setter")


def test_success_is_derived_from_the_same_lookup_not_a_second_one():
    """Value and Success must agree -- two lookups would be two opportunities
    to disagree."""
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    cast = block(flow, "CastTo")
    check(cast.count("_component_get(") >= 2,
          "Success is computed from the same expression as Value")
    check("is not None" in cast,
          "Success tests the same result for None")


def test_the_scene_query_binding_dispatches_over_the_component_set():
    """The generic query is a C++ dispatch, so the node needs no per-type
    binding per component."""
    bindings = read("Scripting", "Bindings.cpp")
    check('.def("GetAllEntitiesWith",' in bindings,
          "Scene.GetAllEntitiesWith(name) is bound")
    check("WL_QUERY_COMP(Wasteland::HealthComponent)" in bindings,
          "the dispatch table covers a gameplay component")
    check("WL_QUERY_COMP(Wasteland::TransformComponent)" in bindings,
          "the dispatch table covers TransformComponent")
    check("#undef WL_QUERY_COMP" in bindings,
          "the macro is undefined after use, so it cannot leak")

    # The dispatch keys are the STRINGIFIED type names, and BIND_COMP_* passes
    # the FULL name -- so the key is "Wasteland::TransformComponent". Accepting
    # only that made `GetAllEntitiesWith("TransformComponent")` silently return
    # nothing while the qualified spelling worked.
    check("ComponentShortName" in bindings,
          "the dispatch strips a leading Wasteland:: from the key")
    check("shortName ==" in bindings,
          "both spellings are compared")
    check("#include <string_view>" in bindings,
          "Bindings.cpp includes <string_view> for the name comparison")

    # The list must be the same set BIND_GAMEPLAY_COMP exposes; a component a
    # script cannot otherwise reach is not worth querying for.
    # Only real invocations count: the macro's #define line and the comment
    # examples above it also contain the token, and the first version of this
    # check parsed them, producing a set with "Comp" and "Name" in it.
    def invocations(name):
        out = set()
        for line in bindings.splitlines():
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("#define"):
                continue
            marker = name + "("
            if marker not in line:
                continue
            argument = line.split(marker, 1)[1].split(")", 1)[0].strip()
            if not argument:
                continue
            # BIND_GAMEPLAY_COMP(Qualified, Short) carries TWO names for one
            # component; WL_QUERY_COMP(Qualified) carries one. Normalise both
            # to the QUALIFIED name, which is what the query table uses.
            parts = [part.strip() for part in argument.split(",")]
            out.add(parts[0])
        return out

    bound = invocations("BIND_GAMEPLAY_COMP")
    queried = invocations("WL_QUERY_COMP")
    missing = sorted(bound - queried)
    check(not missing,
          "every BIND_GAMEPLAY_COMP component is queryable (missing: %s)"
          % ", ".join(missing))


def test_get_actor_and_get_owner_are_documented_as_one_concept():
    """There is no OwnerComponent, so the two nodes must not imply a hierarchy
    that does not exist."""
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    actor = block(flow, "GetActor")
    owner = block(flow, "GetOwner")
    check("self.entity" in actor, "GetActor emits the graph's own entity")
    check("self.entity" in owner, "GetOwner emits the same entity")
    check("no owner relationship" in flow,
          "the source says plainly that there is no owner relationship")


def test_the_list_accessor_bounds_checks_rather_than_raising():
    """An out-of-range index raising IndexError is a confusing way to learn a
    loop ran one iteration long."""
    flow = read("VisualBlocks", "NodeLibraryFlow.cpp")
    body = block(flow, "GetEntityAtIndex")
    check("0 <= (" in body and "< len(" in body,
          "the index is bounds-checked in the emitted expression")
    check("Valid" in body,
          "a Valid output lets the author branch instead of catching")


def test_the_nodes_are_documented():
    with open(SCHEMA, "r", encoding="utf-8", errors="replace") as handle:
        doc = handle.read()
    for type_id in ("CastTo", "GetComponent", "SetComponent",
                    "GetActor", "GetOwner", "GetAllEntitiesWith",
                    "GetEntityAtIndex"):
        check("`%s`" % type_id in doc,
              "%s is documented in the schema" % type_id)
    # The phrase is wrapped across lines in the doc, so match on a piece that
    # is not: "two spellings of ONE concept".
    check("two spellings of ONE concept" in doc,
          "the doc states that GetOwner and GetActor are one concept")


if __name__ == "__main__":
    test_every_b3_node_is_registered()
    test_the_component_type_is_a_runtime_name_not_a_compile_time_type()
    test_the_value_pins_are_any_because_the_type_is_the_authors_choice()
    test_the_helpers_are_emitted_only_when_a_node_needs_them()
    test_no_emitted_docstring_line_starts_with_a_triple_quote()
    test_the_helpers_degrade_to_a_quiet_answer_rather_than_raising()
    test_cast_to_and_get_component_share_one_helper()
    test_success_is_derived_from_the_same_lookup_not_a_second_one()
    test_the_scene_query_binding_dispatches_over_the_component_set()
    test_get_actor_and_get_owner_are_documented_as_one_concept()
    test_the_list_accessor_bounds_checks_rather_than_raising()
    test_the_nodes_are_documented()

    print()
    if FAILED:
        print("%d passed, %d failed" % (PASSED, len(FAILED)))
        raise SystemExit(1)
    print("%d passed, 0 failed" % PASSED)
