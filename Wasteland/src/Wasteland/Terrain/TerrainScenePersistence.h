#pragma once

#include "Wasteland/Terrain/Terrain.h"

#include <filesystem>
#include <string>

namespace Wasteland::Terrain
{
struct TerrainSceneMetadata
{
	std::string AssetReference;
	TerrainStreamingConfig Streaming;
};

// A loaded scene-owned terrain source and its separately persisted world edits.
struct TerrainScenePayload
{
	TerrainSceneMetadata Metadata;
	TerrainAsset Asset;
	TerrainWorldState WorldState;
};

// Encode only the scene-owned reference and streaming policy. The authored
// terrain asset, runtime edits, meshes, LOD caches and colliders are separate.
bool EncodeTerrainSceneMetadataYaml(const TerrainSceneMetadata& metadata,
	std::string& outYaml, std::string* error = nullptr);
bool DecodeTerrainSceneMetadataYaml(const std::string& yamlText,
	TerrainSceneMetadata& outMetadata, std::string* error = nullptr);

// Asset references are rooted at the caller's Assets directory and use the
// canonical project-relative spelling "assets/<relative-path>.wlterrain".
bool MakeTerrainAssetReference(const std::filesystem::path& assetRoot,
	const std::filesystem::path& absolutePath, std::string& outReference,
	std::string* error = nullptr);
bool ResolveTerrainAssetReference(const std::filesystem::path& assetRoot,
	const std::string& reference, std::filesystem::path& outPath,
	std::string* error = nullptr);

// Load metadata, its confined authored source asset, then all world sidecars.
// The caller's payload changes only after every stage succeeds.
bool LoadTerrainScenePayload(const std::filesystem::path& assetRoot,
	const std::filesystem::path& sidecarRoot, const std::string& metadataYaml,
	TerrainScenePayload& outPayload, std::string* error = nullptr);
}
