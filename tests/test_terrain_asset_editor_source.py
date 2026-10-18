from pathlib import Path
import xml.etree.ElementTree as ET
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def read_panel(testcase: unittest.TestCase) -> str:
    path = ROOT / "DemonCore-Editor/src/Panels/TerrainAssetPanel.cpp"
    testcase.assertTrue(path.exists(), "TerrainAssetPanel implementation is missing")
    return path.read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
        index += 1
    raise AssertionError(f"unterminated function body for {signature}")


class TerrainAssetEditorSourceTests(unittest.TestCase):
    def test_content_browser_routes_wlterrain_to_the_terrain_panel(self):
        browser = read("DemonCore-Editor/src/Panels/ContentBrowserPanel.cpp")
        editor = read("DemonCore-Editor/src/EditorLayer.cpp")

        self.assertIn('ext == ".wlterrain"', browser)
        self.assertIn('else if (ext == ".wlterrain")', editor)
        self.assertIn("m_TerrainAssetPanel.OpenAsset(", editor)
        self.assertIn("m_TerrainAssetPanel.SetVisible(true)", editor)

    def test_open_save_and_apply_use_confined_terrain_asset_apis(self):
        panel = read_panel(self)
        open_asset = function_body(panel, "bool TerrainAssetPanel::OpenAsset(")
        save_asset = function_body(panel, "bool TerrainAssetPanel::SaveAsset(")
        apply_asset = function_body(panel, "bool TerrainAssetPanel::ApplyToScene(")

        self.assertIn("MakeTerrainAssetReference(", open_asset)
        self.assertIn("LoadTerrainAsset(", open_asset)
        self.assertIn("SaveTerrainAsset(", save_asset)
        self.assertIn("MakeTerrainAssetReference(", apply_asset)
        self.assertIn("LoadTerrainScenePayload(", apply_asset)
        self.assertIn("ConfigureTerrainRuntime(", apply_asset)
        self.assertLess(apply_asset.index("SaveTerrainWorldState("), apply_asset.index("LoadTerrainScenePayload("))
        self.assertLess(apply_asset.index("LoadTerrainScenePayload("), apply_asset.index("ConfigureTerrainRuntime("))

    def test_opening_or_creating_another_asset_cannot_drop_unsaved_source_edits(self):
        panel = read_panel(self)
        open_asset = function_body(panel, "bool TerrainAssetPanel::OpenAsset(")
        create = function_body(panel, "bool TerrainAssetPanel::CreateNewAsset(")

        self.assertLess(open_asset.index("m_Dirty"), open_asset.index("m_Path = resolvedPath"))
        self.assertLess(create.index("m_Dirty"), create.index("SaveTerrainAsset("))

    def test_apply_is_limited_to_edit_mode_and_dirty_edits_are_saved_or_refused(self):
        editor = read("DemonCore-Editor/src/EditorLayer.cpp")
        panel = read_panel(self)
        render = function_body(panel, "void TerrainAssetPanel::OnImGuiRender(")
        apply = function_body(panel, "bool TerrainAssetPanel::ApplyToScene(")

        self.assertIn("m_SceneState == SceneState::Edit", editor)
        self.assertIn("canApply", render)
        self.assertIn("SaveTerrainWorldState(", apply)
        self.assertIn("m_Status", apply)
        self.assertIn("return false", apply)

    def test_new_asset_target_is_confined_before_save_and_uses_fresh_world_identity(self):
        panel = read_panel(self)
        confinement = function_body(panel, "bool TerrainAssetPanel::IsConfinedAssetTarget(")
        component_check = function_body(panel, "bool IsPathAtOrBelow(")
        create = function_body(panel, "bool TerrainAssetPanel::CreateNewAsset(")
        terrain = read("Wasteland/src/Wasteland/Terrain/Terrain.cpp")
        default_profile = function_body(terrain, "bool CreateDefaultTerrainAsset(")

        self.assertIn("weakly_canonical", confinement)
        self.assertIn("rootPart", component_check)
        self.assertIn("candidatePart", component_check)
        self.assertLess(create.index("IsConfinedAssetTarget("), create.index("SaveTerrainAsset("))
        self.assertIn("CreateDefaultTerrainAsset(", create)
        self.assertIn('candidate.GeneratorId = "value-noise"', default_profile)
        self.assertIn("candidate.SplatRules", default_profile)
        self.assertIn('worldId = "world."', create)

    def test_new_asset_uses_schema_valid_default_terrain_profile(self):
        panel = read_panel(self)
        create = function_body(panel, "bool TerrainAssetPanel::CreateNewAsset(")

        self.assertIn("Terrain::CreateDefaultTerrainAsset(", create)

    def test_editor_resets_panel_on_project_change_and_registers_panel_files(self):
        header = read("DemonCore-Editor/src/EditorLayer.h")
        editor = read("DemonCore-Editor/src/EditorLayer.cpp")
        project_change = function_body(editor, "void EditorLayer::OnProjectChanged(")
        project = read("DemonCore-Editor/DemonCore-Editor.vcxproj")
        filters = read("DemonCore-Editor/DemonCore-Editor.vcxproj.filters")

        self.assertIn("TerrainAssetPanel m_TerrainAssetPanel", header)
        self.assertIn("m_TerrainAssetPanel.Reset()", project_change)
        self.assertIn('MenuItem("New Terrain Asset..."', editor)
        self.assertIn('src\\Panels\\TerrainAssetPanel.cpp', project)
        self.assertIn('src\\Panels\\TerrainAssetPanel.h', project)
        self.assertIn('src\\Panels\\TerrainAssetPanel.cpp', filters)
        self.assertIn('src\\Panels\\TerrainAssetPanel.h', filters)

    def test_post_build_copies_assets_from_project_dir_without_removing_output_files(self):
        project = ET.fromstring(read("DemonCore-Editor/DemonCore-Editor.vcxproj"))
        namespace = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
        groups = {
            group.attrib.get("Condition"): group
            for group in project.findall("msbuild:ItemDefinitionGroup", namespace)
        }
        expected_configurations = {
            "'$(Configuration)|$(Platform)'=='Debug|x64'",
            "'$(Configuration)|$(Platform)'=='Release|x64'",
            "'$(Configuration)|$(Platform)'=='Dist|x64'",
        }

        self.assertEqual(expected_configurations, set(groups))
        for configuration in expected_configurations:
            command = groups[configuration].findtext(
                "msbuild:PostBuildEvent/msbuild:Command", namespaces=namespace
            )
            self.assertIsNotNone(command, configuration)
            self.assertIn(
                'robocopy /E "$(ProjectDir)assets" "$(TargetDir)assets"', command
            )
            self.assertIn("if errorlevel 8 exit /b 1", command)
            self.assertNotIn("exit /b 0", command.lower())
            self.assertLess(
                command.index("if errorlevel 8 exit /b 1"),
                command.index("python314.dll"),
            )
            self.assertNotIn("$(SolutionDir)/DemonCore-Editor/assets", command)
            self.assertNotIn("/MIR", command.upper())


if __name__ == "__main__":
    unittest.main()
