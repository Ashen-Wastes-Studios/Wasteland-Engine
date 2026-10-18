"""Source-level Visual Blocks schema/documentation drift checks.

Unlike the binding-backed suite, this test sees current registration sources
when Wasteland.pyd is stale or cannot be imported. It checks the complete pin
signature, not only the stable node and pin identifiers.
"""

from __future__ import annotations

import ast
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[1]
VB_SOURCE = REPO / "Wasteland" / "src" / "Wasteland" / "VisualBlocks"
SCHEMA = REPO / "docs" / "VISUAL_BLOCKS_SCHEMA.md"
REGISTRATION_FILES = (
    "NodeLibrary.cpp",
    "NodeLibraryFlow.cpp",
    "NodeLibraryV1.cpp",
)
PIN_ROW = re.compile(
    r"`([^`]+)` (in|out) (exec|bool|int|float|string|vec2|vec3|vec4|entity|any)"
    r"( required)?(?: \(default `([^`]*)`\))?"
)


def _strip_cpp_comments(text: str) -> str:
    """Remove C++ comments without treating comment markers inside strings as comments."""
    output = []
    index = 0
    state = "code"
    while index < len(text):
        if state == "code":
            if text.startswith("//", index):
                state = "line"
                index += 2
                continue
            if text.startswith("/*", index):
                state = "block"
                index += 2
                continue
            char = text[index]
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            output.append(char)
            index += 1
        elif state == "line":
            if text[index] == "\n":
                output.append("\n")
                state = "code"
            index += 1
        elif state == "block":
            if text.startswith("*/", index):
                state = "code"
                index += 2
            else:
                if text[index] == "\n":
                    output.append("\n")
                index += 1
        else:
            quote = '"' if state == "string" else "'"
            char = text[index]
            output.append(char)
            if char == "\\" and index + 1 < len(text):
                output.append(text[index + 1])
                index += 2
                continue
            if char == quote:
                state = "code"
            index += 1
    return "".join(output)


def _matching_delimiter(text: str, start: int, left: str, right: str) -> int:
    """Return the matching closing delimiter, ignoring delimiters in literals."""
    depth = 0
    quote = None
    index = start
    while index < len(text):
        char = text[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = None
        elif char in ('"', "'"):
            quote = char
        elif char == left:
            depth += 1
        elif char == right:
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise ValueError("unbalanced %s delimiter" % left)


def _split_arguments(text: str) -> list[str]:
    arguments = []
    start = 0
    depth = 0
    quote = None
    index = 0
    while index < len(text):
        char = text[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = None
        elif char in ('"', "'"):
            quote = char
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            arguments.append(text[start:index].strip())
            start = index + 1
        index += 1
    final = text[start:].strip()
    if final:
        arguments.append(final)
    return arguments


def _calls(text: str, name: str) -> list[list[str]]:
    result = []
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        opening = match.end() - 1
        closing = _matching_delimiter(text, opening, "(", ")")
        result.append(_split_arguments(text[opening + 1:closing]))
    return result


def _pin_signatures(initializer: str) -> list[tuple[str, str, str, bool, str]]:
    signatures = []
    for match in re.finditer(
            r"\b(MakeExecIn|MakeExecOut|MakeIn|MakeOut)\s*\(", initializer):
        opening = match.end() - 1
        closing = _matching_delimiter(initializer, opening, "(", ")")
        args = _split_arguments(initializer[opening + 1:closing])
        factory = match.group(1)
        pin_id = ast.literal_eval(args[0])
        if factory == "MakeExecIn":
            signature = (pin_id, "in", "exec", True, "")
        elif factory == "MakeExecOut":
            signature = (pin_id, "out", "exec", False, "")
        elif factory == "MakeOut":
            signature = (pin_id, "out", args[2].split("::")[-1].lower(), False, "")
        else:
            default = ast.literal_eval(args[4]) if len(args) > 4 else ""
            signature = (
                pin_id,
                "in",
                args[2].split("::")[-1].lower(),
                len(args) > 3 and args[3].strip() == "true",
                default,
            )
        signatures.append(signature)
    return signatures


def _source_registry() -> tuple[dict[str, list[tuple]], dict[str, str]]:
    definitions: dict[str, list[tuple]] = {}
    aliases: dict[str, str] = {}
    sources = [_strip_cpp_comments((VB_SOURCE / name).read_text(encoding="utf-8"))
               for name in REGISTRATION_FILES]

    for source in sources:
        for match in re.finditer(r'def\.TypeId\s*=\s*"([^"]+)"\s*;', source):
            type_id = match.group(1)
            registration = source.find("registry.Register(def);", match.end())
            if registration < 0:
                raise ValueError("registration missing for node type " + type_id)
            body = source[match.end():registration]
            pin_assignment = re.search(r"def\.Pins\s*=\s*\{", body)
            pins = []
            if pin_assignment:
                opening = match.end() + pin_assignment.end() - 1
                closing = _matching_delimiter(source, opening, "{", "}")
                pins = _pin_signatures(source[opening + 1:closing])
            alias = re.search(r'def\.AliasOf\s*=\s*"([^"]+)"', body)
            if alias:
                aliases[type_id] = alias.group(1)
            definitions[type_id] = pins

    flow_source = sources[1]
    for helper in ("addBinary", "addCompare"):
        helper_match = re.search(r"auto\s+" + helper + r"\s*=.*?\{", flow_source, re.S)
        if not helper_match:
            continue
        helper_open = helper_match.end() - 1
        helper_close = _matching_delimiter(flow_source, helper_open, "{", "}")
        body = flow_source[helper_open + 1:helper_close]
        pin_assignment = re.search(r"def\.Pins\s*=\s*\{", body)
        if not pin_assignment:
            raise ValueError("pin list missing from registration helper " + helper)
        pin_open = pin_assignment.end() - 1
        pin_close = _matching_delimiter(body, pin_open, "{", "}")
        signature = _pin_signatures(body[pin_open + 1:pin_close])
        for args in _calls(flow_source[helper_close + 1:], helper):
            definitions[ast.literal_eval(args[0])] = signature

    return definitions, aliases


def _documented_signatures() -> dict[str, list[tuple[str, str, str, bool, str]]]:
    documented = {}
    for line in SCHEMA.read_text(encoding="utf-8").splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 3 or not cells[0].startswith("`"):
            continue
        type_id = cells[0][1:-1]
        documented[type_id] = [
            (match.group(1), match.group(2), match.group(3),
             bool(match.group(4)), match.group(5) or "")
            for match in PIN_ROW.finditer(cells[2])
        ]
    return documented


def _effective_signature(type_id: str, definitions: dict[str, list[tuple]],
                         aliases: dict[str, str]) -> list[tuple]:
    visited = set()
    while type_id in aliases:
        if type_id in visited:
            raise ValueError("alias cycle involving " + type_id)
        visited.add(type_id)
        type_id = aliases[type_id]
    if type_id not in definitions:
        raise ValueError("alias target is not registered: " + type_id)
    return definitions[type_id]


def _toolserver_source() -> str:
    path = REPO / "DemonCore-Editor" / "src" / "Panels" / "ToolServer.cpp"
    return path.read_text(encoding="utf-8")


class VisualBlocksSchemaReferenceTests(unittest.TestCase):
    def test_graph_tools_are_advertised_dispatched_and_read_only(self):
        source = _toolserver_source()
        normalized = source.replace(chr(92) + '"', '"')
        self.assertIn('"name":"validate_graph"', normalized)
        self.assertIn('"name":"list_graph_nodes"', normalized)
        self.assertIn('name == "validate_graph"', source)
        self.assertIn('name == "list_graph_nodes"', source)
        self.assertIn(
            'VisualBlocks::GraphSerializer::Load(fullPath.string(), graph, loadError)',
            source,
        )
        self.assertIn('VisualBlocks::Validator::Validate(graph, validation)', source)
        self.assertIn('registry.Find(node.TypeId)', source)
        self.assertNotIn("GraphSerializer::Save", source)

    def test_graph_tool_paths_are_canonical_and_asset_confined(self):
        source = _toolserver_source()
        start = source.index("ToolServer::ResolveGraphAssetPath(")
        end = source.index("bool ToolServer::LoadGraphAsset(", start)
        resolver = source[start:end]
        for required in (
            "ResolveProjectPath(path, error)",
            "weakly_canonical(Project::Get().GetAssetRoot(), ec)",
            "weakly_canonical(projectPath, ec)",
            "relative(canonicalPath, assetRoot, ec)",
            'part == ".."',
            'extension != ".wlblocks"',
            'extension != ".wlbtree"',
            "is_regular_file(canonicalPath, ec)",
            "return canonicalPath;",
        ):
            with self.subTest(check=required):
                self.assertIn(required, resolver)
        loader_end = source.index("std::string ToolServer::ValidateGraph(", end)
        loader = source[end:loader_end]
        self.assertIn("GraphSerializer::Load(fullPath.string(), graph, loadError)", loader)
        self.assertIn("catch (const std::exception &exception)", loader)

    def test_graph_tool_report_and_listing_preserve_registry_details(self):
        source = _toolserver_source()
        start = source.index("std::string ToolServer::ValidateGraph(")
        end = source.index("std::string ToolServer::ListGraphNodes(", start)
        validate = source[start:end]
        self.assertIn(
            "return validation.Report(Project::Get().MakeProjectRelative(fullPath));",
            validate,
        )
        end_listing = source.index("std::string ToolServer::ReadMemory(", end)
        listing = source[end:end_listing]
        for detail in (
            "registry.Find(node.TypeId)",
            "pin.Id",
            "pin.Dir",
            "PinTypeName(pin.Type)",
            "pin.Required",
            "pin.DefaultLiteral",
            "node.ChildIds",
            "children (priority order)",
        ):
            with self.subTest(detail=detail):
                self.assertIn(detail, listing)

    def test_complete_damageflash_example_matches_the_shipped_asset(self):
        source = SCHEMA.read_text(encoding="utf-8")
        example = re.search(
            r"### `assets/graphs/DamageFlash\.wlblocks`[^\n]*\n.*?```yaml\n(.*?)\n```",
            source,
            re.S,
        )
        self.assertIsNotNone(example, "complete DamageFlash YAML example is missing")
        asset = (REPO / "DemonCore-Editor" / "assets" / "graphs" /
                 "DamageFlash.wlblocks").read_text(encoding="utf-8").strip()
        self.assertEqual(example.group(1).strip(), asset)

    def test_document_matches_all_current_registry_pin_signatures(self):
        definitions, aliases = _source_registry()
        documented = _documented_signatures()
        registered_types = set(definitions)
        self.assertEqual(registered_types, set(documented),
                         "documented node type IDs differ from registration sources")
        for type_id in sorted(registered_types):
            with self.subTest(node_type=type_id):
                self.assertEqual(
                    _effective_signature(type_id, definitions, aliases),
                    documented[type_id],
                    "pin ID/order/direction/type/required/default drift",
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
