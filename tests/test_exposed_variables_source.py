"""Source-side contract for Task B.4 -- exposed variables.

The headless suite proves the RUNTIME behaviour. This suite proves the SOURCE
says what the runtime does, so a refactor that quietly drops one half is caught
without needing a build.

Run:  python tests/test_exposed_variables_source.py
"""

from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.abspath(os.path.join(__file__, "..")))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

FAILED = []
PASSED = 0


def check(condition, message):
    global PASSED
    if condition:
        PASSED += 1
    else:
        FAILED.append(message)


def read(*parts):
    path = os.path.join(ROOT, "Wasteland", "src", "Wasteland", *parts)
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def read_editor(*parts):
    """Editor sources live beside the engine, not under Wasteland/src."""
    path = os.path.join(ROOT, "DemonCore-Editor", *parts)
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def block(source, type_id):
    """The text from a node's TypeId assignment to the end of its Register."""
    marker = 'def.TypeId = "%s";' % type_id
    start = source.find(marker)
    if start < 0:
        return ""
    end = source.find("registry.Register(def);", start)
    return source[start:end] if end > 0 else source[start:start + 1200]


# ---------------------------------------------------------------- Graph.h

def test_the_variable_struct_declares_both_flags():
    source = read("VisualBlocks", "Graph.h")
    # The header forward-declares GraphVariable near the top; the DEFINITION is
    # the one that opens a body. Indexing the bare name finds the wrong one.
    start = source.index("struct GraphVariable\n\t{")
    body = source[start:start + 2200]
    check("bool Exposed = false;" in body,
          "GraphVariable must declare Exposed, defaulting to false")
    check("bool Replicated = false;" in body,
          "GraphVariable must declare Replicated, defaulting to false")
    check("Editable" in body,
          "the comment must say what the flag means -- the Blueprint "
          "\"Editable\" checkbox -- rather than just naming the field")


# ----------------------------------------------------------- GraphSerializer

def test_the_serializer_round_trips_both_flags():
    source = read("VisualBlocks", "GraphSerializer.cpp")
    check('item["exposed"]' in source,
          "the serializer must READ the exposed key")
    check('item["replicated"]' in source,
          "the serializer must READ the replicated key")
    check('variable.Exposed = item["exposed"].as<bool>();' in source,
          "exposed must be parsed as a bool")
    check('variable.Replicated = item["replicated"].as<bool>();' in source,
          "replicated must be parsed as a bool")
    check('out << YAML::Key << "exposed"' in source,
          "exposed must be WRITTEN")
    check('out << YAML::Key << "replicated"' in source,
          "replicated must be WRITTEN")


def test_a_false_flag_is_omitted_on_save():
    """Omitting the false flag is what keeps an existing .wlblocks file
    byte-identical on a round-trip -- the doc-example comparison asserts it."""
    source = read("VisualBlocks", "GraphSerializer.cpp")
    start = source.index('out << YAML::Key << "exposed"')
    window = source[max(0, start - 600):start]
    check("if (variable.Exposed)" in window,
          "exposed must be emitted only when TRUE")


# ------------------------------------------------------------- Components.h

def test_the_component_stores_per_entity_overrides():
    source = read("Scene", "Components.h")
    start = source.index("struct VisualBlocksComponent")
    body = source[start:start + 4000]
    check("std::map<std::string, std::string> VariableOverrides;" in body,
          "the component must store per-entity overrides as a name->literal map")
    check("#include <map>" in source,
          "Components.h must include <map> for the override map")


# ----------------------------------------------------------- SceneSerializer

def test_the_scene_serializer_round_trips_the_overrides():
    source = read("Scene", "SceneSerializer.cpp")
    check('"VariableOverrides"' in source,
          "the scene serializer must write VariableOverrides")
    check("entry.second.as<std::string>()" in source,
          "an override value must be read back as a string literal")
    check("overrideNode.IsMap()" in source,
          "a malformed override block must be rejected by an IsMap check, "
          "not silently iterated")


# ---------------------------------------------------------- FlowGraphSystem

def test_attach_seeds_the_overrides():
    source = read("Scene", "FlowGraphSystem.cpp")
    check("py::eval(entry->second)" in source,
          "attach must evaluate the authored literal with py::eval")
    check('s_Instances[id].attr(py::str(variable.Name)) = value;' in source,
          "attach must write the value onto the instance attribute")
    check("if (!variable.Exposed)" in source,
          "attach must skip a variable that is not exposed")


def test_the_overrides_are_reapplied_every_frame():
    """The plan's verification needs an edit made AFTER Play to be seen.

    Attach-time seeding alone cannot do that: a Python attribute does not change
    when the component map does.
    """
    source = read("Scene", "FlowGraphSystem.cpp")
    start = source.index('if (s_Methods[id] == "OnUpdate")')
    window = source[max(0, start - 2500):start]
    check("s_OverrideEpoch" in window,
          "the per-frame path must compare against the last-seen overrides")
    check("graph.VariableOverrides" in window,
          "the per-frame path must read the component's overrides")
    check("py::eval(entry->second)" in window,
          "the per-frame path must evaluate the literal")


def test_the_exposed_name_cache_is_filled_at_attach():
    source = read("Scene", "FlowGraphSystem.cpp")
    check("s_ExposedNames[id]" in source,
          "attach must cache the exposed variable names")
    check("exposed.clear();" in source,
          "the cache must be cleared before it is refilled, so a re-attach "
          "cannot accumulate stale names")
    check("s_ExposedNames.erase(id);" in source,
          "the cache must be dropped with the entity")


def test_the_new_fields_are_bound_to_python():
    """A new C++ field that is not bound is invisible to every script, and the
    editor cannot reach it either -- the failure is an AttributeError at the
    first touch, not a compile error."""
    source = read("Scripting", "Bindings.cpp")
    check('.def_readwrite("Exposed", &vb::GraphVariable::Exposed)' in source,
          "GraphVariable.Exposed must be bound")
    check('.def_readwrite("Replicated", &vb::GraphVariable::Replicated)'
          in source,
          "GraphVariable.Replicated must be bound")
    check('def("SetVariableOverride"' in source,
          "VisualBlocksComponent must expose the override map to Python")


def test_the_override_map_is_exposed_as_accessors():
    """NOT as a bound map.

    The usual recipe -- `PYBIND11_MAKE_OPAQUE(std::map<...>)` plus `bind_map`
    -- looks right and is wrong here: bind_map's holder is a `shared_ptr<Map>`
    while the component holds the map BY VALUE, so a reference-returning
    getter cannot wrap it. The property fails at first use with
    "Did you forget to #include <pybind11/stl.h>?".
    """
    source = read("Scripting", "Bindings.cpp")
    check('def("GetVariableOverride"' in source,
          "the override must be readable by name")
    check('def("SetVariableOverride"' in source,
          "the override must be writable by name")
    # The accessors are real C++ members: the hierarchy inspector is C++, and a
    # pybind lambda is invisible from it. The first version were lambdas and the
    # editor build failed with "'GetVariableOverride' is not a member".
    components = read("Scene", "Components.h")
    start = components.index("struct VisualBlocksComponent")
    body = components[start:start + 5200]
    check("std::string GetVariableOverride(const std::string &name) const"
          in body,
          "the component must declare a C++ getter")
    check("void SetVariableOverride(const std::string &name," in body,
          "the component must declare a C++ setter")
    check("bool HasVariableOverride(const std::string &name) const" in body,
          "the component must declare a C++ presence query")
    check("&Wasteland::VisualBlocksComponent::GetVariableOverride" in source,
          "the binding must point at the MEMBER, not a lambda")
    check("def_readwrite(\"VariableOverrides\"" not in source,
          "the map must NOT be exposed through def_readwrite: the holder "
          "mismatch makes the getter fail at runtime")
    check("#include <map>" in source,
          "Bindings.cpp must include <map> for the component's override map")


def test_the_overrides_survive_a_malformed_literal():
    source = read("Scene", "FlowGraphSystem.cpp")
    check("catch (const std::exception &exc)" in source,
          "a malformed override literal must be caught, not propagated")
    check("unreadable value" in source,
          "the failure must be logged with the variable and its value")


# ------------------------------------------------------------------ Panel

def test_the_inspector_offers_only_exposed_variables():
    source = read_editor("src", "Panels", "SceneHierarchyPanel.cpp")
    check("GraphSerializer::Load" in source,
          "the panel must read the graph's declarations rather than keeping a "
          "parallel list that would drift")
    check("if (!variable.Exposed)" in source,
          "the panel must skip a variable the graph did not expose")
    check("blocks.SetVariableOverride(variable.Name," in source,
          "an edit must write through the component's accessor, which reaches "
          "the real map rather than a copy")
    check("Exposed Variables" in source,
          "the section must be labelled so an author can find it")


def test_the_inspector_edits_the_value_the_runtime_reads():
    """The panel writes a literal string; the runtime evals it. The two must
    agree on the stored form or an edit would never reach Python."""
    source = read_editor("src", "Panels", "SceneHierarchyPanel.cpp")
    check("std::to_string(value)" in source,
          "a numeric edit must be stored as text, the form the runtime evals")
    check('value ? "True" : "False"' in source,
          "a bool edit must be stored as a Python literal, not \"1\"/\"0\"")
    check("variable.DefaultLiteral" in source,
          "the panel must fall back to the graph default when the entity has "
          "no override, so the field shows the value that will actually run")
    check("blocks.GetVariableOverride(variable.Name)" in source,
          "the panel must READ the override by name")


def main():
    tests = [value for key, value in sorted(globals().items())
             if key.startswith("test_") and callable(value)]
    for test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001
            FAILED.append("%s raised %s: %s" % (test.__name__,
                                                type(exc).__name__, exc))
    print("%d passed, %d failed" % (PASSED, len(FAILED)))
    for message in FAILED:
        print("  FAIL:", message)
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
