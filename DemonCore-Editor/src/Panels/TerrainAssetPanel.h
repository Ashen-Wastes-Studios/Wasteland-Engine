#pragma once

#include "Wasteland/Scene/Scene.h"
#include "Wasteland/Terrain/Terrain.h"

#include <filesystem>
#include <string>

namespace Wasteland
{
class TerrainAssetPanel
{
public:
    void OnImGuiRender(const std::filesystem::path& assetRoot,
        const std::filesystem::path& projectRoot, const Ref<Scene>& activeScene,
        bool canApply);

    bool OpenAsset(const std::filesystem::path& path,
        const std::filesystem::path& assetRoot);
    bool CreateNewAsset(const std::filesystem::path& path,
        const std::filesystem::path& assetRoot);
    void Reset();

    bool IsVisible() const { return m_Visible; }
    void SetVisible(bool visible) { m_Visible = visible; }
    bool IsOpen() const { return !m_Path.empty(); }

private:
    bool IsConfinedAssetTarget(const std::filesystem::path& assetRoot,
        const std::filesystem::path& target, std::string* error) const;
    bool SaveAsset(const std::filesystem::path& assetRoot);
    bool ApplyToScene(const std::filesystem::path& assetRoot,
        const std::filesystem::path& projectRoot, const Ref<Scene>& activeScene,
        bool canApply);

    std::filesystem::path m_Path;
    std::string m_AssetReference;
    Terrain::TerrainAsset m_Asset;
    bool m_Visible = false;
    bool m_Dirty = false;
    std::string m_Status;
};
}
