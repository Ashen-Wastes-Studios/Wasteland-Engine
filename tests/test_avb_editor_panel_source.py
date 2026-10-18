"""Headless source contracts for the AVB editor route and panel."""

from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[1]
BROWSER = REPO / "DemonCore-Editor" / "src" / "Panels" / "ContentBrowserPanel.cpp"
EDITOR = REPO / "DemonCore-Editor" / "src" / "EditorLayer.cpp"
EDITOR_HEADER = REPO / "DemonCore-Editor" / "src" / "EditorLayer.h"
PANEL_HEADER = REPO / "DemonCore-Editor" / "src" / "Panels" / "AvbGraphPanel.h"
PANEL_SOURCE = REPO / "DemonCore-Editor" / "src" / "Panels" / "AvbGraphPanel.cpp"
PROJECT = REPO / "DemonCore-Editor" / "DemonCore-Editor.vcxproj"
FILTERS = REPO / "DemonCore-Editor" / "DemonCore-Editor.vcxproj.filters"


class AvbEditorPanelSourceTests(unittest.TestCase):
    def test_content_browser_marks_wlavb_openable_and_double_clicks_open_it(self):
        source = BROWSER.read_text(encoding="utf-8")
        self.assertIn('ext == ".wlavb"', source)
        self.assertIn('path.extension() == ".wlavb"', source)
        self.assertIn("m_OpenRequested = path.string()", source)

    def test_editor_routes_wlavb_to_its_own_panel(self):
        source = EDITOR.read_text(encoding="utf-8")
        header = EDITOR_HEADER.read_text(encoding="utf-8")
        self.assertTrue('#include "Panels/AvbGraphPanel.h"' in header)
        self.assertTrue("AvbGraphPanel m_AvbGraphPanel" in header)
        route = source[source.index("void EditorLayer::OpenVisualFile"):]
        self.assertTrue('ext == ".wlavb"' in route)
        self.assertTrue("m_AvbGraphPanel.OpenGraph(p.string())" in route)
        self.assertTrue("m_AvbGraphPanel.OnImGuiRender()" in source)

    def test_panel_uses_existing_avb_io_compiler_and_editable_canvas(self):
        source = PANEL_SOURCE.read_text(encoding="utf-8")
        for expected in (
            "AvbGraphSerializer::Load",
            "AvbGraphSerializer::Save",
            "AvbCompiler::Compile",
            "AvbValidator::Validate",
            "m_Canvas.Render",
            "m_Graph.Links.push_back",
            "node->Literals",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, source)

    def test_opening_another_graph_requires_an_explicit_unsaved_change_decision(self):
        source = PANEL_SOURCE.read_text(encoding="utf-8")
        open_graph = source[
            source.index("bool AvbGraphPanel::OpenGraph"):
            source.index("void AvbGraphPanel::RefreshValidation")
        ]
        self.assertIn("m_Dirty", open_graph)
        self.assertIn("m_PendingOpenPath", open_graph)
        self.assertIn("m_ConfirmOpen", open_graph)
        self.assertIn("Save then Open", source)
        self.assertIn("Discard and Open", source)

    def test_new_panel_sources_are_registered_in_both_explicit_project_files(self):
        project = PROJECT.read_text(encoding="utf-8")
        filters = FILTERS.read_text(encoding="utf-8")
        for source in (
            'src\\Panels\\AvbGraphPanel.h',
            'src\\Panels\\AvbGraphPanel.cpp',
        ):
            with self.subTest(source=source):
                self.assertIn(source, project)
                self.assertIn(source, filters)


    def test_edit_buffers_resize_dynamically_without_truncating_long_text(self):
        header = PANEL_HEADER.read_text(encoding="utf-8")
        source = PANEL_SOURCE.read_text(encoding="utf-8")

        self.assertIn("std::unordered_map<std::string, std::string> m_EditBuffers", header)
        self.assertIn("m_EditBuffers.emplace(key, seed)", source)
        self.assertNotIn("kEditBufferSize", header + source)
        self.assertNotIn("std::vector<char>", header + source)
        callback_start = source.index("int ResizeEditBufferCallback")
        callback_end = source.index("bool InputText(", callback_start)
        callback = source[callback_start:callback_end]
        for expected in (
            "data->EventFlag == ImGuiInputTextFlags_CallbackResize",
            "static_cast<std::string*>(data->UserData)",
            "buffer->resize(data->BufTextLen)",
            "data->Buf = const_cast<char*>(buffer->c_str())",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, callback)

        input_text = source[
            callback_end:
            source.index("\n\t}\n\n\tstd::string& AvbGraphPanel::EditBuffer", callback_end)
        ]
        self.assertIn(
            "ImGui::InputText(label, const_cast<char*>(buffer.c_str()), buffer.capacity() + 1,",
            input_text,
        )
        self.assertIn(
            "ImGuiInputTextFlags_CallbackResize, ResizeEditBufferCallback, &buffer)",
            input_text,
        )
        self.assertIn("m_EditBuffers.clear()", source)

        for call in (
            'InputText("Name", name)',
            'InputText("Title", title)',
            'InputText("##literal", literal)',
        ):
            with self.subTest(call=call):
                self.assertIn(call, source)
        self.assertEqual(source.count("ImGui::InputText("), 1)

    def test_successful_save_and_discard_open_close_the_confirmation_popup(self):
        source = PANEL_SOURCE.read_text(encoding="utf-8")
        draw = source[
            source.index("void AvbGraphPanel::DrawOpenConfirmation"):
            source.index("void AvbGraphPanel::DrawCanvas")
        ]
        save_then_open = draw[
            draw.index('if (ImGui::Button("Save then Open")'):
            draw.index('if (ImGui::Button("Discard and Open"))')
        ]
        discard_and_open = draw[
            draw.index('if (ImGui::Button("Discard and Open"))'):
            draw.index('if (ImGui::Button("Cancel"))')
        ]

        self.assertRegex(
            save_then_open,
            r"if\s*\(LoadGraphFile\(path\)\)\s*\{\s*ImGui::CloseCurrentPopup\(\);",
        )
        self.assertIn("const std::string path = m_PendingOpenPath;", discard_and_open)
        self.assertNotIn("m_ConfirmOpen = false", discard_and_open)
        self.assertNotIn("m_PendingOpenPath.clear()", discard_and_open)
        self.assertRegex(
            discard_and_open,
            r"if\s*\(LoadGraphFile\(path\)\)\s*\{\s*ImGui::CloseCurrentPopup\(\);",
        )

if __name__ == "__main__":
    unittest.main(verbosity=2)
