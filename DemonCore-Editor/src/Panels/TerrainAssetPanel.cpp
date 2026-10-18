#include "wlpch.h"
#include "TerrainAssetPanel.h"

#include "Wasteland/Project/Project.h"
#include "Wasteland/Terrain/TerrainScenePersistence.h"

#include <algorithm>
#include <imgui/imgui.h>

namespace Wasteland
{
namespace
{
void SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
}

bool IsPathAtOrBelow(const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart)
    {
        if (candidatePart == candidate.end() || *rootPart != *candidatePart)
            return false;
    }
    return true;
}
}

void TerrainAssetPanel::Reset()
{
    m_Path.clear();
    m_AssetReference.clear();
    m_Asset = {};
    m_Dirty = false;
    m_Status.clear();
    m_Visible = false;
}

bool TerrainAssetPanel::IsConfinedAssetTarget(const std::filesystem::path& assetRoot,
    const std::filesystem::path& target, std::string* error) const
{
    if (error)
        error->clear();
    if (assetRoot.empty() || target.empty() || !assetRoot.is_absolute() ||
        !target.is_absolute() || target.extension() != ".wlterrain")
    {
        SetError(error, "Terrain asset target must be an absolute .wlterrain path under Assets");
        return false;
    }

    std::error_code ec;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(assetRoot, ec);
    if (ec || !std::filesystem::is_directory(canonicalRoot, ec) || ec)
    {
        SetError(error, "Could not resolve the project's Assets directory");
        return false;
    }
    const std::filesystem::path canonicalParent =
        std::filesystem::weakly_canonical(target.parent_path(), ec);
    if (ec || !IsPathAtOrBelow(canonicalRoot, canonicalParent))
    {
        SetError(error, "New terrain assets must be created inside the project's Assets directory");
        return false;
    }
    return true;
}

bool TerrainAssetPanel::OpenAsset(const std::filesystem::path& path,
    const std::filesystem::path& assetRoot)
{
    std::string error;
    std::string reference;
    if (!Terrain::MakeTerrainAssetReference(assetRoot, path, reference, &error))
    {
        m_Status = "Could not open terrain asset: " + error;
        return false;
    }

    std::filesystem::path resolvedPath;
    if (!Terrain::ResolveTerrainAssetReference(assetRoot, reference, resolvedPath, &error))
    {
        m_Status = "Could not open terrain asset: " + error;
        return false;
    }

    if (m_Dirty)
    {
        if (reference != m_AssetReference)
        {
            m_Status = "Save authored changes before opening another terrain asset";
            return false;
        }
        m_Visible = true;
        m_Status = "This terrain asset has unsaved authored changes";
        return true;
    }

    Terrain::TerrainAsset staged;
    if (!Terrain::LoadTerrainAsset(resolvedPath, staged, &error))
    {
        m_Status = "Could not open terrain asset: " + error;
        return false;
    }

    m_Path = resolvedPath;
    m_AssetReference = std::move(reference);
    m_Asset = std::move(staged);
    m_Dirty = false;
    m_Status = "Loaded terrain asset";
    m_Visible = true;
    return true;
}

bool TerrainAssetPanel::CreateNewAsset(const std::filesystem::path& path,
    const std::filesystem::path& assetRoot)
{
    if (m_Dirty)
    {
        m_Status = "Save authored changes before creating another terrain asset";
        return false;
    }

    std::string error;
    if (!IsConfinedAssetTarget(assetRoot, path, &error))
    {
        m_Status = "Could not create terrain asset: " + error;
        return false;
    }

    std::error_code ec;
    if (std::filesystem::exists(path, ec) || ec)
    {
        m_Status = ec ? "Could not inspect the selected terrain asset path" :
            "A file already exists at the selected terrain asset path";
        return false;
    }
    if (!std::filesystem::is_directory(path.parent_path(), ec) || ec)
    {
        m_Status = "The selected terrain asset folder must already exist inside Assets";
        return false;
    }

    const std::string worldId = "world." +
        std::to_string(static_cast<std::uint64_t>(UUID()));
    Terrain::TerrainAsset asset;
    if (!Terrain::CreateDefaultTerrainAsset(worldId, asset, &error))
    {
        m_Status = "Could not create terrain asset: " + error;
        return false;
    }
    if (!Terrain::SaveTerrainAsset(path, asset, &error))
    {
        m_Status = "Could not create terrain asset: " + error;
        return false;
    }

    return OpenAsset(path, assetRoot);
}

bool TerrainAssetPanel::SaveAsset(const std::filesystem::path& assetRoot)
{
    if (m_Path.empty())
    {
        m_Status = "No terrain asset is open";
        return false;
    }

    std::string error;
    std::string reference;
    if (!Terrain::MakeTerrainAssetReference(assetRoot, m_Path, reference, &error) ||
        reference != m_AssetReference)
    {
        m_Status = "Could not save terrain asset safely: " +
            (error.empty() ? std::string("the source path changed") : error);
        return false;
    }
    if (!Terrain::SaveTerrainAsset(m_Path, m_Asset, &error))
    {
        m_Status = "Could not save terrain asset: " + error;
        return false;
    }

    m_Dirty = false;
    m_Status = "Saved terrain asset";
    return true;
}

bool TerrainAssetPanel::ApplyToScene(const std::filesystem::path& assetRoot,
    const std::filesystem::path& projectRoot, const Ref<Scene>& activeScene,
    bool canApply)
{
    if (!canApply)
    {
        m_Status = "Apply is available only while editing a scene";
        return false;
    }
    if (!activeScene)
    {
        m_Status = "There is no active edit scene";
        return false;
    }
    if (m_Path.empty())
    {
        m_Status = "No terrain asset is open";
        return false;
    }
    if (m_Dirty)
    {
        m_Status = "Save the authored terrain asset before applying it";
        return false;
    }

    std::string error;
    std::string reference;
    if (!Terrain::MakeTerrainAssetReference(assetRoot, m_Path, reference, &error) ||
        reference != m_AssetReference)
    {
        m_Status = "Could not apply terrain asset safely: " +
            (error.empty() ? std::string("the source path changed") : error);
        return false;
    }

    Terrain::TerrainStreamingConfig streaming;
    const bool replacingConfiguredTerrain = activeScene->GetTerrainRuntime().IsConfigured();
    if (replacingConfiguredTerrain)
    {
        streaming = activeScene->GetTerrainRuntime().GetConfig();
        if (!activeScene->SaveTerrainWorldState(projectRoot, &error))
        {
            m_Status = "Cannot replace terrain: dirty terrain sidecars could not be saved: " + error;
            return false;
        }
    }

    Terrain::TerrainSceneMetadata metadata;
    metadata.AssetReference = reference;
    metadata.Streaming = streaming;
    std::string metadataYaml;
    if (!Terrain::EncodeTerrainSceneMetadataYaml(metadata, metadataYaml, &error))
    {
        m_Status = "Could not prepare terrain apply: " + error;
        return false;
    }

    Terrain::TerrainScenePayload payload;
    if (!Terrain::LoadTerrainScenePayload(assetRoot, projectRoot, metadataYaml, payload, &error))
    {
        m_Status = "Could not load terrain asset and sidecars: " + error;
        return false;
    }
    if (!activeScene->ConfigureTerrainRuntime(payload.Asset, payload.WorldState,
        payload.Metadata.Streaming, &error, payload.Metadata.AssetReference))
    {
        m_Status = "Could not apply terrain asset: " + error;
        return false;
    }

    m_Asset = std::move(payload.Asset);
    m_Status = "Applied saved terrain asset to the active edit scene";
    return true;
}

void TerrainAssetPanel::OnImGuiRender(const std::filesystem::path& assetRoot,
    const std::filesystem::path& projectRoot, const Ref<Scene>& activeScene,
    bool canApply)
{
    if (!m_Visible)
        return;

    ImGui::Begin("Terrain Asset", &m_Visible);
    if (m_Path.empty())
    {
        ImGui::TextDisabled("Open a .wlterrain asset from the Content Browser or use File > New Terrain Asset...");
    }
    else
    {
        ImGui::TextWrapped("Source: %s", m_Path.string().c_str());
        ImGui::Text("Asset Reference: %s", m_AssetReference.c_str());
        ImGui::Text("World ID: %s", m_Asset.WorldId.c_str());
        ImGui::Text("Generator: %s (version %u)", m_Asset.GeneratorId.c_str(), m_Asset.GeneratorVersion);
        ImGui::Text("Schema: %u    Chunk Resolution: %u", m_Asset.SchemaVersion, m_Asset.ChunkResolution);
        ImGui::Separator();

        bool changed = false;
        changed |= ImGui::InputScalar("Seed", ImGuiDataType_U64, &m_Asset.Seed);
        changed |= ImGui::InputDouble("Origin X", &m_Asset.OriginX, 0.0, 0.0, "%.6f");
        changed |= ImGui::InputDouble("Origin Y", &m_Asset.OriginY, 0.0, 0.0, "%.6f");
        changed |= ImGui::InputDouble("Origin Z", &m_Asset.OriginZ, 0.0, 0.0, "%.6f");
        changed |= ImGui::InputDouble("Sample Spacing", &m_Asset.SampleSpacing, 0.0, 0.0, "%.6f");
        changed |= ImGui::InputDouble("Base Height", &m_Asset.BaseHeight, 0.0, 0.0, "%.6f");
        changed |= ImGui::InputDouble("Height Amplitude", &m_Asset.HeightAmplitude, 0.0, 0.0, "%.6f");
        changed |= ImGui::InputDouble("Noise Frequency", &m_Asset.NoiseFrequency, 0.0, 0.0, "%.6f");
        int octaves = static_cast<int>(m_Asset.NoiseOctaves);
        if (ImGui::InputInt("Noise Octaves", &octaves))
        {
            m_Asset.NoiseOctaves = static_cast<std::uint32_t>((std::max)(1, octaves));
            changed = true;
        }
        changed |= ImGui::InputDouble("Lacunarity", &m_Asset.Lacunarity, 0.0, 0.0, "%.6f");
        changed |= ImGui::InputDouble("Persistence", &m_Asset.Persistence, 0.0, 0.0, "%.6f");
        m_Dirty = m_Dirty || changed;

        if (m_Dirty)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Unsaved authored changes");
        if (ImGui::Button("Save Asset"))
            SaveAsset(assetRoot);
        ImGui::SameLine();
        if (canApply && activeScene && !m_Dirty)
        {
            if (ImGui::Button("Apply to Active Scene"))
                ApplyToScene(assetRoot, projectRoot, activeScene, canApply);
        }
        else
        {
            ImGui::TextDisabled("Apply to Active Scene (save changes and use Edit mode)");
        }
    }

    if (!m_Status.empty())
        ImGui::TextWrapped("%s", m_Status.c_str());
    ImGui::End();
}
}
