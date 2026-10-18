"""Source-level guardrails for AVB.4 while native builds are externally serialized.

Behavioral coverage lives in test_avb_graph.cpp; this small test keeps the AVB
module boundary and public surface discoverable without compiling engine code.
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GRAPH = ROOT / "Wasteland" / "src" / "Wasteland" / "Avb" / "Graph"


def test_avb_graph_module_is_independent_and_complete():
    expected = {
        "Graph.h": ("AvbGraph", "AvbNode", "AvbLink", "AvbLiteral", "AvbVariable", "AvbPinType", "AvbPinDir", "AvbSend"),
        "Graph.cpp": ("GenerateStableId", "AddNode"),
        "GraphSerializer.h": ("Parse", "Emit", "Load", "Save"),
        "GraphSerializer.cpp": ("yaml-cpp/yaml.h", "unknown node type", "unknown pin"),
        "NodeRegistry.h": ("AliasOf", "AvbNodeRegistry", "AvbPinRate"),
        "NodeRegistry.cpp": ("RegisterBuiltins", "WaveShaper"),
        "Validator.h": ("AvbValidator", "AvbDiagnostic", "AvbValidationResult"),
        "Validator.cpp": ("data cycle", "required trigger", "incompatible types"),
        "Compiler.h": ("AvbCompiler", "std::vector<const AvbNode*>", "Output"),
        "Compiler.cpp": ("topological", "classic", "setprecision"),
    }
    missing = []
    for filename, tokens in expected.items():
        path = GRAPH / filename
        if not path.is_file():
            missing.append(f"missing {path.relative_to(ROOT)}")
            continue
        text = path.read_text(encoding="utf-8")
        for token in tokens:
            if token not in text:
                missing.append(f"{filename} is missing contract token {token!r}")

    all_sources = "\n".join(path.read_text(encoding="utf-8") for path in GRAPH.glob("*.*") if path.is_file())
    if "Wasteland::VisualBlocks" in all_sources or 'VisualBlocks/' in all_sources:
        missing.append("AVB graph source must not depend on VisualBlocks")
    assert not missing, "AVB.4 source contract failed:\n" + "\n".join(missing)


if __name__ == "__main__":
    test_avb_graph_module_is_independent_and_complete()
    print("test_avb_graph_contract: 1 check passed")
