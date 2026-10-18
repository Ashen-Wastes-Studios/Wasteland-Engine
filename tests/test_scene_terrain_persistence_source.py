from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


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


class SceneTerrainPersistenceSourceTests(unittest.TestCase):
    def test_deserialize_preflights_terrain_before_entity_mutation_and_commits_afterward(self):
        source = read("Wasteland/src/Wasteland/Scene/SceneSerializer.cpp")
        body = function_body(source, "bool SceneSerializer::Deserialize(")

        preflight = body.index("LoadTerrainScenePayload(")
        first_mutation = body.index("CreateEntityWithUUID(")
        terrain_commit = body.index("ConfigureTerrainRuntime(")
        success = body.rindex("return true;")
        self.assertLess(preflight, first_mutation)
        self.assertLess(first_mutation, terrain_commit)
        self.assertLess(terrain_commit, success)
        self.assertIn("project.GetAssetRoot()", body)
        self.assertIn("project.GetRoot()", body)
        self.assertIn("ClearTerrainRuntime()", body)
        self.assertIn("terrainPayload", body)
        self.assertIn("terrainNode.IsDefined()", body)
        self.assertIn("project.IsOpen()", body)

    def test_serialize_emits_optional_metadata_and_saves_sidecars_under_project_root(self):
        source = read("Wasteland/src/Wasteland/Scene/SceneSerializer.cpp")
        body = function_body(source, "bool SceneSerializer::SerializeChecked(")

        self.assertIn("EncodeTerrainSceneMetadataYaml(", body)
        self.assertIn('YAML::Key << "Terrain"', body)
        self.assertIn("metadata.AssetReference", body)
        self.assertIn("metadata.Streaming", body)
        terrain_emission = body.index('YAML::Key << "Terrain"')
        condition = body.index("if (terrainConfigured)", body.index("YAML::Emitter out;"))
        self.assertLess(condition, terrain_emission)
        resolve = body.index("ResolveTerrainAssetReference(")
        save_edits = body.index("SaveWorldState(")
        self.assertLess(resolve, save_edits)
        self.assertIn("project.GetAssetRoot()", body)
        self.assertIn("Project::Get().GetRoot()", body)
        self.assertIn("outFile.flush()", body)
        self.assertIn("outFile.close()", body)
        self.assertIn("rename", body)
        self.assertNotIn("TerrainAsset", body[body.index('YAML::Key << "Terrain"'):])

    def test_scene_retains_authored_reference_across_configure_copy_and_clear(self):
        header = read("Wasteland/src/Wasteland/Scene/Scene.h")
        source = read("Wasteland/src/Wasteland/Scene/Scene.cpp")
        configure = function_body(source, "bool Scene::ConfigureTerrainRuntime(")
        copy = function_body(source, "Ref<Scene> Scene::Copy(")

        self.assertIn("GetTerrainAssetReference()", header)
        self.assertIn("m_TerrainAssetReference.clear()", header)
        self.assertIn("assetReference", header)
        self.assertLess(configure.index("m_TerrainRuntime.Configure("), configure.index("m_TerrainAssetReference ="))
        self.assertIn("GetTerrainAssetReference()", copy)

    def test_runtime_sidecar_save_uses_dirty_world_save_operation(self):
        header = read("Wasteland/src/Wasteland/Terrain/TerrainRuntime.h")
        source = read("Wasteland/src/Wasteland/Terrain/TerrainRuntime.cpp")
        save = function_body(source, "bool TerrainRuntime::SaveWorldState(")

        self.assertIn("SaveWorldState(", header)
        self.assertIn("SaveTerrainChunkEditsForWorld(", save)
        self.assertIn("m_WorldState", save)

    def test_python_and_editor_save_surfaces_propagate_serializer_status(self):
        binding = read("Wasteland/src/Wasteland/Scripting/Bindings.cpp")
        editor_header = read("DemonCore-Editor/src/EditorLayer.h")
        editor = read("DemonCore-Editor/src/EditorLayer.cpp")
        save_scene_as = function_body(editor, "void EditorLayer::SaveSceneAs(")

        self.assertIn("return serializer.Serialize(path);", binding)
        self.assertIn('.def("Serialize", &Wasteland::SceneSerializer::Serialize', binding)
        self.assertIn("bool SerializeScene(", editor_header)
        self.assertIn("bool EditorLayer::SerializeScene(", editor)
        self.assertIn("WL_CORE_ERROR(\"Failed to save scene", editor)
        self.assertIn("if (SerializeScene(m_ActiveScene, filepath))", save_scene_as)
        self.assertLess(save_scene_as.index("if (SerializeScene("), save_scene_as.index("m_EditorScenePath = filepath;"))

    def test_native_integration_runner_propagates_early_failure(self):
        source = read("tests/scene_terrain_persistence/test_scene_terrain_persistence.cpp")
        main = function_body(source, "int main()")

        self.assertIn("Check(RunPersistenceIntegration(fixtureRoot)", main)


if __name__ == "__main__":
    unittest.main()
