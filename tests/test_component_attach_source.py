"""The component-attach spine must be wired end to end, in the source.

Phases 0.1-0.4 added the ability to attach a Visual Blocks graph to a scene
entity the way a script or a behaviour tree is attached: a component struct, a
chunk-store entry, a serializer block, a runtime system, Python bindings, an
editor menu entry and inspector field, and a native event dispatch path.

Every one of those is a separate edit in a separate file, and none of them
fails the build when it is missing. A component struct without a chunk-store
entry silently never persists. A system without a vcxproj entry compiles but
is never linked. A binding without the BIND_COMP_* macro block leaves the
component unusable from Python. Each omission is invisible until a user tries
the feature.

The headless tests in tests/headless/ exercise the runtime behaviour. This file
asserts the wiring itself, so a future refactor that drops one of these edits
fails here rather than in a user's project.

Run:  python tests/test_component_attach_source.py
"""

from __future__ import annotations

import os
import re
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_ENGINE = os.path.join(_ROOT, "Wasteland", "src", "Wasteland")
_EDITOR = os.path.join(_ROOT, "DemonCore-Editor", "src")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from runner import check, check_eq  # noqa: E402


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _engine_file(rel):
    return os.path.join(_ENGINE, rel.replace("/", os.sep))


def _require(path, label):
    check(os.path.isfile(path), "%s must exist (%s)" % (label, path))
    return _read(path) if os.path.isfile(path) else ""


def main():
    if not os.path.isdir(_ENGINE):
        print("SKIP: no engine source at %s" % _ENGINE)
        return 0

    # --- 1. The component struct -------------------------------------------
    components = _require(_engine_file("Scene/Components.h"), "Components.h")
    check(re.search(r"struct\s+VisualBlocksComponent\b", components),
          "Components.h must define struct VisualBlocksComponent")
    for field in ("GraphPath", "GraphName", "CompileError", "Attached",
                  "SuspendedTicks"):
        check(re.search(r"\b%s\b" % field, components),
              "VisualBlocksComponent must expose the %s field" % field)

    # --- 2. Persistence: chunk store + serializer --------------------------
    chunk = _require(_engine_file("Scene/SceneChunkStore.cpp"),
                     "SceneChunkStore.cpp")
    check(re.search(r"\bVisualBlocksComponent\b", chunk),
          "SceneChunkStore.cpp must list VisualBlocksComponent in its "
          "component list, or attached graphs never reach the chunk store")

    ser = _require(_engine_file("Scene/SceneSerializer.cpp"),
                   "SceneSerializer.cpp")
    check(re.search(r'YAML::Key\s*<<\s*"VisualBlocksComponent"', ser)
          is not None,
          "SceneSerializer.cpp must serialize VisualBlocksComponent")
    check(re.search(r'entity\["VisualBlocksComponent"\]', ser) is not None,
          "SceneSerializer.cpp must deserialize VisualBlocksComponent -- a "
          "scene saved with a graph attached must load it back")
    check(re.search(r'entity\.HasComponent<BehaviorTreeComponent>\(\)', ser)
          is not None,
          "SceneSerializer.cpp must still serialize BehaviorTreeComponent")

    # --- 3. Runtime: component-added hook + per-frame tick -----------------
    scene = _require(_engine_file("Scene/Scene.cpp"), "Scene.cpp")
    check(re.search(
        r"void\s+Scene::OnComponentAdded<VisualBlocksComponent>", scene)
        is not None,
        "Scene.cpp must specialize OnComponentAdded<VisualBlocksComponent>; "
        "the primary template is static_assert(false), so omitting it fails "
        "only at link time")
    check(re.search(r"FlowGraphSystem::OnUpdateEntity", scene) is not None,
          "Scene.cpp must tick FlowGraphSystem::OnUpdateEntity each frame")
    check(re.search(r"FlowGraphSystem::OnDestroyEntity", scene) is not None,
          "Scene.cpp must destroy graph instances with their entities")
    check(re.search(r"ScriptEventDispatcher::DispatchNamed", scene) is not None,
          "Scene.cpp must route engine events through ScriptEventDispatcher")

    # --- 4. The runtime system itself --------------------------------------
    fgs_h = _require(_engine_file("Scene/FlowGraphSystem.h"),
                     "FlowGraphSystem.h")
    for member in ("OnUpdateEntity", "OnDestroyEntity", "Shutdown",
                   "CompileGraph", "GetInstance"):
        check(re.search(r"\b%s\s*\(" % member, fgs_h) is not None,
              "FlowGraphSystem must declare %s()" % member)
    check(re.search(r"s_Instances", fgs_h) is not None,
          "FlowGraphSystem must keep a per-entity instance map")

    fgs_cpp = _require(_engine_file("Scene/FlowGraphSystem.cpp"),
                       "FlowGraphSystem.cpp")
    check(re.search(r"Compiler::CompileFlow", fgs_cpp) is not None,
          "FlowGraphSystem must compile graphs through Compiler::CompileFlow")
    check(re.search(r"py::exec", fgs_cpp) is not None,
          "FlowGraphSystem must compile into a live Python module")

    # --- 5. Compiler: one method per event ---------------------------------
    comp_h = _require(_engine_file("VisualBlocks/Compiler.h"), "Compiler.h")
    check(re.search(r"FlowMethodNameFor", comp_h) is not None,
          "Compiler must expose FlowMethodNameFor so the runtime and the "
          "emitter agree on the generated method name")
    comp_cpp = _require(_engine_file("VisualBlocks/Compiler.cpp"),
                        "Compiler.cpp")
    check(re.search(r"FlowMethodNameFor\s*\(", comp_cpp) is not None,
          "Compiler.cpp must define FlowMethodNameFor")
    check(re.search(r"FlowClassNameFor\s*\(", comp_cpp) is not None,
          "Compiler.cpp must define FlowClassNameFor")

    # --- 6. Validator: schema 1 vs schema 2 event rules --------------------
    val = _require(_engine_file("VisualBlocks/Validator.cpp"), "Validator.cpp")
    check(re.search(r"graph\.Schema", val) is not None,
          "Validator must branch on graph.Schema so schema 2 can carry "
          "several event nodes")
    check(re.search(r"duplicate event name", val) is not None,
          "Validator must reject duplicate event names at any schema -- the "
          "emitter writes one method per event, so a duplicate silently "
          "overwrites the first")

    # --- 7. Node library: Vec3 arithmetic ----------------------------------
    nodes = _require(_engine_file("VisualBlocks/NodeLibraryFlow.cpp"),
                     "NodeLibraryFlow.cpp")
    check(re.search(r'"Vec3Add"', nodes) is not None,
          "NodeLibraryFlow must register a Vec3Add node -- a graph cannot "
          "move an entity without Vec3 arithmetic")

    # --- 8. Python bindings ------------------------------------------------
    bind = _require(_engine_file("Scripting/Bindings.cpp"), "Bindings.cpp")
    check(re.search(r"py::class_<\s*Wasteland::VisualBlocksComponent", bind)
          is not None,
          "Bindings.cpp must expose VisualBlocksComponent to Python")
    for macro in ("BIND_COMP_HAS", "BIND_COMP_GET", "BIND_COMP_TRYGET",
                  "BIND_COMP_ADD", "BIND_COMP_REM"):
        check(re.search(macro + r"\(\s*Wasteland::VisualBlocksComponent", bind)
              is not None,
              "Bindings.cpp must bind %s for VisualBlocksComponent" % macro)
    check(re.search(r"BIND_GAMEPLAY_COMP\(\s*Wasteland::VisualBlocksComponent",
                    bind) is not None,
          "Bindings.cpp must register VisualBlocksComponent in the gameplay "
          "component block")
    check(re.search(r'def_static\(\s*"CompileFlowGraph"', bind) is not None,
          "Bindings must expose Scene.CompileFlowGraph")
    check(re.search(r'def\(\s*"__add__"', bind) is not None,
          "Bindings must support Vec3 + Vec3, or graph math nodes fail at "
          "runtime with a TypeError")
    check(re.search(r'def\(\s*"__sub__"', bind) is not None,
          "Bindings must support Vec3 - Vec3")
    check(re.search(r"py::enum_<\s*Wasteland::ScriptEventId", bind) is not None,
          "Bindings must expose the ScriptEventId enum")
    check(re.search(r'def\(\s*"DispatchScriptEvent"', bind) is not None,
          "Bindings must expose Entity.DispatchScriptEvent")

    # --- 9. Event dispatch -------------------------------------------------
    disp_h = _require(_engine_file("Scene/ScriptEventDispatcher.h"),
                      "ScriptEventDispatcher.h")
    check(re.search(r"DispatchNamed\s*\(", disp_h) is not None,
          "ScriptEventDispatcher must declare DispatchNamed")
    check(re.search(r"Dispatch\s*\(\s*Entity", disp_h) is not None,
          "ScriptEventDispatcher must declare the enum-based Dispatch")
    check(re.search(r"MethodNameFor", disp_h) is not None,
          "ScriptEventDispatcher must map ids to method names in one place")
    disp_cpp = _require(_engine_file("Scene/ScriptEventDispatcher.cpp"),
                        "ScriptEventDispatcher.cpp")
    check(re.search(r"ScriptEventDispatcher::DispatchNamed", disp_cpp)
          is not None,
          "ScriptEventDispatcher.cpp must define DispatchNamed")
    check(re.search(r"FlowGraphSystem::GetInstance", disp_cpp) is not None,
          "the dispatcher must reach flow graphs")
    check(re.search(r"BehaviorTreeSystem::GetInstance", disp_cpp) is not None,
          "the dispatcher must reach behaviour trees")

    entity_h = _require(_engine_file("Scene/Entity.h"), "Entity.h")
    check(re.search(r"DispatchEvent\s*\(", entity_h) is not None,
          "Entity must declare DispatchEvent")
    entity_cpp = _require(_engine_file("Scene/Entity.cpp"), "Entity.cpp")
    check(re.search(r"Entity::DispatchEvent", entity_cpp) is not None,
          "Entity.cpp must define DispatchEvent")

    check(re.search(r"NotifyInventoryChanged", scene) is not None,
          "Scene.cpp must still raise the inventory-changed event")

    # --- 10. Lifecycle -----------------------------------------------------
    app = _require(_engine_file("Core/Application.cpp"), "Application.cpp")
    check(re.search(r"FlowGraphSystem::Shutdown\s*\(\s*\)", app) is not None,
          "Application.cpp must shut the flow graph system down")

    vcx = _require(os.path.join(_ROOT, "Wasteland", "Wasteland.vcxproj"),
                   "Wasteland.vcxproj")
    flat = vcx.replace("\\", "/")
    for src in ("FlowGraphSystem.cpp", "ScriptEventDispatcher.cpp",
                "FlowGraphSystem.h", "ScriptEventDispatcher.h"):
        check(src in flat, "Wasteland.vcxproj must list %s" % src)

    # --- 11. Editor surface ------------------------------------------------
    panel = _require(os.path.join(_EDITOR, "Panels", "SceneHierarchyPanel.cpp"),
                     "SceneHierarchyPanel.cpp")
    check(re.search(r'MenuItem\(\s*"Visual Blocks Component"', panel)
          is not None,
          "the Add-Component menu must offer Visual Blocks Component")
    check(re.search(r"AddComponent<VisualBlocksComponent>\s*\(\s*\)", panel)
          is not None,
          "the menu entry must actually add the component")
    check(re.search(r"HasComponent<VisualBlocksComponent>\s*\(\s*\)", panel)
          is not None,
          "the inspector must detect an attached graph component")
    check(re.search(r"RemoveComponent<VisualBlocksComponent>\s*\(\s*\)", panel)
          is not None,
          "the inspector must be able to remove the component")
    check(re.search(r"FlowGraphSystem::CompileGraph", panel) is not None,
          "the inspector must compile the graph on demand so a broken graph "
          "shows its error instead of failing silently")
    check(re.search(r"CompileError", panel) is not None,
          "the inspector must surface the compile error")

    print("")
    if _failures_report():
        return 1
    return 0


def _failures_report():
    import runner
    return bool(runner._failures)


if __name__ == "__main__":
    sys.exit(main())
