"""Source contracts for Visual Blocks editor rendering."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


REPO = Path(__file__).resolve().parents[1]
PANEL_SOURCE = REPO / "DemonCore-Editor" / "src" / "Panels" / "VisualBlocksPanel.cpp"


def _mask_cpp_noncode(source: str) -> str:
    """Blank comments and C++ literals while preserving offsets and newlines."""
    masked = list(source)

    def blank(start: int, end: int) -> None:
        for position in range(start, end):
            if masked[position] != "\n":
                masked[position] = " "

    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index)
            if end < 0:
                end = len(source)
            blank(index, end)
            index = end
            continue
        if source.startswith("/*", index):
            close = source.find("*/", index + 2)
            if close < 0:
                raise ValueError("unterminated C++ block comment")
            end = close + 2
            blank(index, end)
            index = end
            continue

        # Raw strings may contain arbitrary quotes, braces, and comment markers.
        if source.startswith('R"', index):
            delimiter_end = source.find("(", index + 2)
            if delimiter_end < 0:
                raise ValueError("malformed C++ raw string literal")
            delimiter = source[index + 2:delimiter_end]
            terminator = ")" + delimiter + '"'
            close = source.find(terminator, delimiter_end + 1)
            if close < 0:
                raise ValueError("unterminated C++ raw string literal")
            end = close + len(terminator)
            blank(index, end)
            index = end
            continue

        if source[index] in ('"', "'"):
            quote = source[index]
            end = index + 1
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == quote:
                    end += 1
                    break
                end += 1
            else:
                raise ValueError("unterminated C++ string or character literal")
            blank(index, end)
            index = end
            continue
        index += 1
    return "".join(masked)


def _matching_delimiter(code: str, opening: int, left: str, right: str) -> int:
    """Find a matching delimiter in comment/literal-masked C++ source."""
    depth = 0
    for index in range(opening, len(code)):
        if code[index] == left:
            depth += 1
        elif code[index] == right:
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("unbalanced C++ delimiter: %s" % left)


def _extract_function(source: str, method_name: str) -> str:
    """Extract a qualified method body without being fooled by comments/literals."""
    code = _mask_cpp_noncode(source)
    signature = re.search(
        r"\bVisualBlocksPanel::" + re.escape(method_name) + r"\s*\(", code
    )
    if signature is None:
        raise AssertionError("VisualBlocksPanel::%s definition not found" % method_name)

    opening_paren = code.find("(", signature.start())
    closing_paren = _matching_delimiter(code, opening_paren, "(", ")")
    opening_brace = code.find("{", closing_paren + 1)
    if opening_brace < 0:
        raise AssertionError("VisualBlocksPanel::%s body not found" % method_name)
    closing_brace = _matching_delimiter(code, opening_brace, "{", "}")
    return source[opening_brace + 1:closing_brace]


class VisualBlocksPanelSourceTests(unittest.TestCase):
    def test_inspector_renders_each_input_once_and_keeps_existing_paths(self):
        source = PANEL_SOURCE.read_text(encoding="utf-8")
        body = _extract_function(source, "DrawInspector")
        code = " ".join(_mask_cpp_noncode(body).split())

        all_pin_loops = re.findall(
            r"\bfor\s*\(\s*const\s+PinDef\s*&\s*pin\s*:\s*def->Pins\s*\)",
            code,
        )
        self.assertEqual(
            len(all_pin_loops),
            1,
            "DrawInspector must handle the current pin in its outer loop, not "
            "reiterate all pins for each unlinked data input",
        )

        for existing_path in (
            "m_Graph.ResolveInput(node->Id, pin.Id",
            "if (isLinked)",
            "else if (pin.Type == PinType::Exec)",
            "switch (pin.Type)",
            "node->SetLiteral(pin.Id, nextValue, true)",
            "CommitEdit()",
        ):
            with self.subTest(existing_path=existing_path):
                self.assertIn(existing_path, code)


if __name__ == "__main__":
    unittest.main(verbosity=2)
