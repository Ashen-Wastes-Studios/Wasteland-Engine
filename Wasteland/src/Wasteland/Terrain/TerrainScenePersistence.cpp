#include "wlpch.h"

#include "Wasteland/Terrain/TerrainScenePersistence.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Wasteland::Terrain
{
namespace
{
void SetError(std::string* error, const std::string& message)
{
	if (error)
		*error = message;
}

bool CanonicalAssetRoot(const std::filesystem::path& assetRoot,
	std::filesystem::path& outRoot, std::string* error)
{
	std::error_code filesystemError;
	const std::filesystem::path canonicalRoot = std::filesystem::canonical(assetRoot, filesystemError);
	if (filesystemError || !std::filesystem::is_directory(canonicalRoot, filesystemError) || filesystemError)
	{
		SetError(error, "terrain asset root must be an existing directory");
		return false;
	}
	outRoot = canonicalRoot;
	return true;
}

bool IsStrictDescendant(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	auto rootPart = root.begin();
	auto candidatePart = candidate.begin();
	for (; rootPart != root.end(); ++rootPart, ++candidatePart)
	{
		if (candidatePart == candidate.end() || *rootPart != *candidatePart)
			return false;
	}
	return candidatePart != candidate.end();
}

bool CanonicalContainedTerrainFile(const std::filesystem::path& canonicalRoot,
	const std::filesystem::path& candidate, std::filesystem::path& outFile,
	std::string* error)
{
	std::error_code filesystemError;
	const std::filesystem::path canonicalFile = std::filesystem::canonical(candidate, filesystemError);
	if (filesystemError)
	{
		SetError(error, "terrain asset reference target does not exist");
		return false;
	}
	if (!std::filesystem::is_regular_file(canonicalFile, filesystemError) || filesystemError)
	{
		SetError(error, "terrain asset reference target must be a regular file");
		return false;
	}
	if (canonicalFile.extension() != ".wlterrain")
	{
		SetError(error, "terrain asset target must use the .wlterrain extension");
		return false;
	}
	if (!IsStrictDescendant(canonicalRoot, canonicalFile))
	{
		SetError(error, "terrain asset target escapes the Assets directory");
		return false;
	}
	outFile = canonicalFile;
	return true;
}

bool ParseCanonicalAssetReference(const std::string& reference,
	std::string& outRelative, std::string* error)
{
	constexpr const char* kPrefix = "assets/";
	if (reference.empty() || reference.find('\\') != std::string::npos ||
		reference.compare(0, 7, kPrefix) != 0)
	{
		SetError(error, "terrain asset reference must use canonical assets/<path> spelling");
		return false;
	}

	const std::string relative = reference.substr(7);
	if (relative.empty())
	{
		SetError(error, "terrain asset reference has no path below Assets");
		return false;
	}
	std::size_t componentStart = 0;
	while (componentStart <= relative.size())
	{
		const std::size_t separator = relative.find('/', componentStart);
		const std::size_t componentEnd = separator == std::string::npos ? relative.size() : separator;
		const std::string component = relative.substr(componentStart, componentEnd - componentStart);
		if (component.empty() || component == "." || component == "..")
		{
			SetError(error, "terrain asset reference contains a noncanonical path component");
			return false;
		}
		if (separator == std::string::npos)
			break;
		componentStart = separator + 1;
	}

	const std::filesystem::path relativePath(relative);
	if (relativePath.is_absolute() || relativePath.has_root_name() || relativePath.has_root_directory() ||
		relativePath.generic_string() != relative || relativePath.extension() != ".wlterrain")
	{
		SetError(error, "terrain asset reference must be a canonical relative .wlterrain path");
		return false;
	}
	outRelative = relative;
	return true;
}
}

bool EncodeTerrainSceneMetadataYaml(const TerrainSceneMetadata& metadata,
	std::string& outYaml, std::string* error)
{
	if (error)
		error->clear();
	std::string relativeAssetPath;
	if (!ParseCanonicalAssetReference(metadata.AssetReference, relativeAssetPath, error))
		return false;
	if (!ValidateTerrainStreamingConfig(metadata.Streaming, error))
		return false;

	try
	{
		YAML::Node root(YAML::NodeType::Map);
		root["AssetRef"] = metadata.AssetReference;
		YAML::Node streaming(YAML::NodeType::Map);
		streaming["LoadRadiusChunks"] = metadata.Streaming.LoadRadiusChunks;
		YAML::Node thresholds(YAML::NodeType::Sequence);
		for (std::uint32_t threshold : metadata.Streaming.LodDistanceThresholds)
			thresholds.push_back(threshold);
		streaming["LodDistanceThresholds"] = thresholds;
		streaming["MaxChunkBuildsPerUpdate"] = metadata.Streaming.MaxChunkBuildsPerUpdate;
		root["Streaming"] = streaming;

		YAML::Emitter emitter;
		emitter << root;
		if (!emitter.good())
		{
			SetError(error, emitter.GetLastError());
			return false;
		}
		const std::string encoded(emitter.c_str(), emitter.size());
		outYaml = encoded;
		return true;
	}
	catch (const YAML::Exception& exception)
	{
		SetError(error, exception.what());
		return false;
	}
}

bool DecodeTerrainSceneMetadataYaml(const std::string& yamlText,
	TerrainSceneMetadata& outMetadata, std::string* error)
{
	if (error)
		error->clear();
	try
	{
		const std::vector<YAML::Node> documents = YAML::LoadAll(yamlText);
		if (documents.size() != 1 || !documents.front().IsMap() || documents.front().size() != 2)
		{
			SetError(error, "terrain scene metadata must be one YAML mapping with exactly AssetRef and Streaming");
			return false;
		}

		const YAML::Node root = documents.front();
		const YAML::Node assetReference = root["AssetRef"];
		const YAML::Node streaming = root["Streaming"];
		if (!assetReference.IsScalar() || assetReference.IsNull() || !streaming.IsMap() || streaming.size() != 3)
		{
			SetError(error, "terrain scene metadata has missing or malformed AssetRef or Streaming fields");
			return false;
		}
		const std::string parsedAssetReference = assetReference.as<std::string>();
		std::string relativeAssetPath;
		if (!ParseCanonicalAssetReference(parsedAssetReference, relativeAssetPath, error))
			return false;

		const YAML::Node loadRadius = streaming["LoadRadiusChunks"];
		const YAML::Node thresholds = streaming["LodDistanceThresholds"];
		const YAML::Node buildBudget = streaming["MaxChunkBuildsPerUpdate"];
		if (!loadRadius.IsScalar() || loadRadius.IsNull() || !thresholds.IsSequence() ||
			thresholds.size() != 6 || !buildBudget.IsScalar() || buildBudget.IsNull())
		{
			SetError(error, "terrain scene Streaming mapping must contain all three fields and six LOD thresholds");
			return false;
		}

		TerrainSceneMetadata parsed;
		parsed.AssetReference = parsedAssetReference;
		parsed.Streaming.LoadRadiusChunks = loadRadius.as<std::uint32_t>();
		for (std::size_t index = 0; index < parsed.Streaming.LodDistanceThresholds.size(); ++index)
		{
			if (!thresholds[index].IsScalar() || thresholds[index].IsNull())
			{
				SetError(error, "terrain scene LOD thresholds must be integer scalars");
				return false;
			}
			parsed.Streaming.LodDistanceThresholds[index] = thresholds[index].as<std::uint32_t>();
		}
		parsed.Streaming.MaxChunkBuildsPerUpdate = buildBudget.as<std::uint32_t>();
		if (!ValidateTerrainStreamingConfig(parsed.Streaming, error))
			return false;

		outMetadata = std::move(parsed);
		return true;
	}
	catch (const YAML::Exception& exception)
	{
		SetError(error, exception.what());
		return false;
	}
	catch (const std::exception& exception)
	{
		SetError(error, exception.what());
		return false;
	}
}

bool MakeTerrainAssetReference(const std::filesystem::path& assetRoot,
	const std::filesystem::path& absolutePath, std::string& outReference,
	std::string* error)
{
	if (error)
		error->clear();
	if (!absolutePath.is_absolute() || absolutePath.extension() != ".wlterrain")
	{
		SetError(error, "terrain asset path must be absolute and use the .wlterrain extension");
		return false;
	}

	std::filesystem::path canonicalRoot;
	if (!CanonicalAssetRoot(assetRoot, canonicalRoot, error))
		return false;
	std::filesystem::path canonicalFile;
	if (!CanonicalContainedTerrainFile(canonicalRoot, absolutePath, canonicalFile, error))
		return false;
	const std::filesystem::path relative = canonicalFile.lexically_relative(canonicalRoot);
	if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
		relative.generic_string().empty())
	{
		SetError(error, "terrain asset path is not a canonical descendant of Assets");
		return false;
	}

	const std::string reference = "assets/" + relative.generic_string();
	std::string checkedRelative;
	if (!ParseCanonicalAssetReference(reference, checkedRelative, error))
		return false;
	outReference = reference;
	return true;
}

bool ResolveTerrainAssetReference(const std::filesystem::path& assetRoot,
	const std::string& reference, std::filesystem::path& outPath,
	std::string* error)
{
	if (error)
		error->clear();
	std::string relative;
	if (!ParseCanonicalAssetReference(reference, relative, error))
		return false;

	std::filesystem::path canonicalRoot;
	if (!CanonicalAssetRoot(assetRoot, canonicalRoot, error))
		return false;
	std::filesystem::path canonicalFile;
	if (!CanonicalContainedTerrainFile(canonicalRoot, canonicalRoot / relative, canonicalFile, error))
		return false;
	const std::string canonicalRelative = canonicalFile.lexically_relative(canonicalRoot).generic_string();
	if (canonicalRelative != relative)
	{
		SetError(error, "terrain asset reference is not the canonical path to its target");
		return false;
	}
	outPath = canonicalFile;
	return true;
}

bool LoadTerrainScenePayload(const std::filesystem::path& assetRoot,
	const std::filesystem::path& sidecarRoot, const std::string& metadataYaml,
	TerrainScenePayload& outPayload, std::string* error)
{
	TerrainScenePayload staged;
	if (!DecodeTerrainSceneMetadataYaml(metadataYaml, staged.Metadata, error))
		return false;

	std::filesystem::path assetPath;
	if (!ResolveTerrainAssetReference(assetRoot, staged.Metadata.AssetReference, assetPath, error))
		return false;
	if (!LoadTerrainAsset(assetPath, staged.Asset, error))
		return false;
	if (!LoadTerrainChunkEditsForWorld(sidecarRoot, staged.Asset, staged.WorldState, error))
		return false;

	outPayload = std::move(staged);
	return true;
}
}
