#include "wlpch.h"
#include "Wasteland/Project/Project.h"
#include "Wasteland/Renderer/RenderCommand.h"
#include "Wasteland/Renderer/RendererAPI.h"
#include "Wasteland/Scene/SceneSerializer.h"
#include "Wasteland/Terrain/TerrainScenePersistence.h"

#if defined(_WIN32)
#include "Platform/NVRHI/NVRHIRendererAPI.h"
#endif

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

using namespace Wasteland;
using namespace Wasteland::Terrain;

namespace
{
int s_Failures = 0;
int s_Checks = 0;

void Check(bool condition, const char* requirement)
{
	++s_Checks;
	if (!condition)
	{
		std::cerr << "FAIL " << requirement << '\n';
		++s_Failures;
	}
}

void CheckPreWindowRendererDefaults()
{
#if defined(_WIN32)
	Check(RendererAPI::GetAPI() == RendererAPI::API::NVRHI_DX12,
		"Windows renderer API defaults to NVRHI DX12 before window creation");
	RendererAPI* backend = RenderCommand::GetRendererAPI();
	Check(backend != nullptr, "pre-window renderer backend is constructed");
#if defined(_CPPRTTI) || defined(__GXX_RTTI__)
	Check(dynamic_cast<NVRHIRendererAPI*>(backend) != nullptr,
		"pre-window renderer backend is the NVRHI implementation");
#endif
#endif
}

class ScopedAppDataOverride
{
public:
	explicit ScopedAppDataOverride(const std::filesystem::path& replacement)
	{
		if (const char* current = std::getenv("APPDATA"))
		{
			m_HadOriginal = true;
			m_Original = current;
		}
		m_Active = _putenv_s("APPDATA", replacement.string().c_str()) == 0;
	}

	~ScopedAppDataOverride()
	{
		Restore();
	}

	bool IsActive() const noexcept { return m_Active; }

	bool Restore() noexcept
	{
		if (!m_Active)
			return true;
		const int result = _putenv_s("APPDATA", m_HadOriginal ? m_Original.c_str() : "");
		m_Active = false;
		return result == 0;
	}

private:
	bool m_HadOriginal = false;
	bool m_Active = false;
	std::string m_Original;
};

std::filesystem::path ScratchDirectory()
{
	if (const char* configured = std::getenv("TMPDIR"))
	{
		std::string nativePath = configured;
#if defined(_WIN32)
		// MSYS exports paths such as /c/Users/...; convert that spelling for
		// std::filesystem in this native Windows test executable.
		if (nativePath.size() >= 3 && nativePath[0] == '/' && nativePath[2] == '/' &&
			((nativePath[1] >= 'a' && nativePath[1] <= 'z') ||
			 (nativePath[1] >= 'A' && nativePath[1] <= 'Z')))
		{
			const char drive = nativePath[1] >= 'a' && nativePath[1] <= 'z'
				? static_cast<char>(nativePath[1] - 'a' + 'A') : nativePath[1];
			nativePath = std::string(1, drive) + ":" + nativePath.substr(2);
		}
#endif
		if (!nativePath.empty())
			return std::filesystem::path(nativePath);
	}
	// Keep the fallback inside the caller's working tree rather than using
	// the system temporary directory; the unique fixture child is removed below.
	return std::filesystem::current_path();
}

bool WriteText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output.is_open())
		return false;
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	output.close();
	return output.good();
}

TerrainChunkEdits MakeSparseEdits(const TerrainAsset& asset, std::int64_t chunkX,
	std::int64_t chunkZ, std::pair<std::int32_t, std::int32_t> sample,
	double delta, std::pair<std::int32_t, std::int32_t> cell)
{
	TerrainChunkEdits edits;
	edits.WorldId = asset.WorldId;
	edits.ChunkX = chunkX;
	edits.ChunkZ = chunkZ;
	edits.BaseFingerprint = TerrainAssetFingerprint(asset);
	edits.Dirty = true;
	edits.HeightDeltas.emplace(sample, delta);
	edits.MaterialOverrides.emplace(cell, asset.SplatRules.front().MaterialAssetId);
	return edits;
}

bool RunPersistenceIntegration(const std::filesystem::path& projectRoot)
{
	const auto assets = projectRoot / "assets";
	const auto terrainDirectory = assets / "terrain";
	const auto scenesDirectory = assets / "scenes";
	std::error_code fsError;
	std::filesystem::create_directories(terrainDirectory, fsError);
	if (!fsError)
		std::filesystem::create_directories(scenesDirectory, fsError);
	Check(!fsError, "project asset and scene fixture directories are created");
	if (fsError)
		return false;

	if (!WriteText(projectRoot / "project.wasteland", "Version: 1\nName: SceneTerrainPersistenceTests\n"))
	{
		Check(false, "temporary project descriptor is authored");
		return false;
	}
	Check(true, "temporary project descriptor is authored");

	TerrainAsset asset;
	std::string error;
	const std::string worldId = "world.scene-terrain-persistence";
	if (!CreateDefaultTerrainAsset(worldId, asset, &error))
	{
		Check(false, "authored terrain fixture is valid");
		std::cerr << "  terrain fixture error: " << error << '\n';
		return false;
	}
	asset.Seed = 0x123456789abcdef0ULL;
	Check(ValidateTerrainAsset(asset, &error), "authored terrain fixture passes the engine schema");
	if (!error.empty())
		std::cerr << "  terrain validation detail: " << error << '\n';

	const auto assetPath = terrainDirectory / "roundtrip.wlterrain";
	Check(SaveTerrainAsset(assetPath, asset, &error), "authored .wlterrain source is written to the project");
	if (!error.empty())
		std::cerr << "  asset save detail: " << error << '\n';
	if (!std::filesystem::is_regular_file(assetPath))
		return false;

	std::string assetReference;
	Check(MakeTerrainAssetReference(assets, assetPath, assetReference, &error),
		"source path becomes the canonical project-relative AssetRef");
	if (!error.empty())
		std::cerr << "  AssetRef detail: " << error << '\n';
	if (assetReference.empty())
		return false;
	Check(assetReference == "assets/terrain/roundtrip.wlterrain",
		"AssetRef uses the canonical assets/<path>.wlterrain spelling");

	TerrainStreamingConfig streaming;
	streaming.LoadRadiusChunks = 7;
	streaming.LodDistanceThresholds = { 3, 6, 10, 15, 21, 28 };
	streaming.MaxChunkBuildsPerUpdate = 11;
	Check(ValidateTerrainStreamingConfig(streaming, &error), "non-default full streaming policy is valid");
	if (!error.empty())
		std::cerr << "  streaming validation detail: " << error << '\n';

	TerrainWorldState sourceState;
	const TerrainChunkEdits first = MakeSparseEdits(asset, -3, 5, { 0, 64 }, 1.25, { 2, 7 });
	const TerrainChunkEdits second = MakeSparseEdits(asset, 12, -9, { 64, 0 }, -0.375, { 61, 4 });
	sourceState.Chunks.emplace(TerrainChunkKey{ asset.WorldId, first.ChunkX, first.ChunkZ }, first);
	sourceState.Chunks.emplace(TerrainChunkKey{ asset.WorldId, second.ChunkX, second.ChunkZ }, second);
	Check(sourceState.Chunks.size() == 2 && first.HeightDeltas.size() == 1 &&
		first.MaterialOverrides.size() == 1 && second.HeightDeltas.size() == 1 &&
		second.MaterialOverrides.size() == 1,
		"source fixture contains only sparse edits for two distinct chunks");

	ScopedAppDataOverride appData(projectRoot / "test-appdata");
	Check(appData.IsActive(), "test isolates Project recents from the user's APPDATA");
	if (!appData.IsActive())
		return false;

	Project& project = Project::Get();
	Check(project.Open(projectRoot, &error), "temporary project opens for the real serializer path");
	if (!error.empty())
		std::cerr << "  project open detail: " << error << '\n';
	if (!project.IsOpen())
		return false;

	const std::filesystem::path scenePath = scenesDirectory / "TerrainRoundTrip.wastescene";
	auto savedScene = std::make_shared<Scene>();
	Check(savedScene->ConfigureTerrainRuntime(asset, sourceState, streaming, &error, assetReference),
		"source Scene accepts the asset, sparse edits, and complete streaming policy");
	if (!error.empty())
		std::cerr << "  terrain configure detail: " << error << '\n';
	if (!savedScene->GetTerrainRuntime().IsConfigured())
		return false;

	SceneSerializer writer(savedScene);
	Check(writer.SerializeChecked(scenePath.string(), &error),
		"SceneSerializer writes a real .wastescene and its terrain sidecars");
	if (!error.empty())
		std::cerr << "  scene serialize detail: " << error << '\n';
	if (!std::filesystem::is_regular_file(scenePath))
		return false;

	std::filesystem::path firstSidecar;
	std::filesystem::path secondSidecar;
	const bool firstPathResolved = ResolveTerrainChunkEditsPath(projectRoot, asset,
		first.ChunkX, first.ChunkZ, firstSidecar, &error);
	Check(firstPathResolved, "first sparse chunk resolves to its project-root sidecar");
	if (!firstPathResolved)
		return false;
	const bool secondPathResolved = ResolveTerrainChunkEditsPath(projectRoot, asset,
		second.ChunkX, second.ChunkZ, secondSidecar, &error);
	Check(secondPathResolved, "second sparse chunk resolves to its project-root sidecar");
	if (!secondPathResolved)
		return false;
	Check(std::filesystem::is_regular_file(firstSidecar) && std::filesystem::is_regular_file(secondSidecar),
		"save publishes both sparse sidecar payloads separately from the scene and asset");

	std::size_t sidecarFileCount = 0;
	const auto sidecarRoot = projectRoot / ".terrain-chunk-edits" / "v1";
	if (std::filesystem::exists(sidecarRoot))
	{
		for (const auto& entry : std::filesystem::recursive_directory_iterator(sidecarRoot))
			if (entry.is_regular_file())
				++sidecarFileCount;
	}
	Check(sidecarFileCount == 2, "sidecar store contains exactly the two authored chunk edits");

	auto loadedScene = std::make_shared<Scene>();
	SceneSerializer reader(loadedScene);
	Check(reader.Deserialize(scenePath.string()), "a fresh Scene loads the actual saved .wastescene");
	const TerrainRuntime& loadedRuntime = loadedScene->GetTerrainRuntime();
	Check(loadedRuntime.IsConfigured(), "scene load restores configured Terrain runtime state");
	Check(loadedScene->GetTerrainAssetReference() == assetReference,
		"scene load restores the authored AssetRef exactly");
	Check(TerrainAssetFingerprint(loadedRuntime.GetAsset()) == TerrainAssetFingerprint(asset),
		"scene load restores the referenced authored Terrain source asset");
	Check(loadedRuntime.GetConfig().LoadRadiusChunks == streaming.LoadRadiusChunks,
		"scene load restores the full streaming load radius");
	Check(loadedRuntime.GetConfig().LodDistanceThresholds == streaming.LodDistanceThresholds,
		"scene load restores all six streaming LOD thresholds");
	Check(loadedRuntime.GetConfig().MaxChunkBuildsPerUpdate == streaming.MaxChunkBuildsPerUpdate,
		"scene load restores the streaming build budget");

	const TerrainWorldState& loadedState = loadedRuntime.GetWorldState();
	Check(loadedState.Chunks.size() == 2, "scene load restores both sparse chunk sidecars");
	const auto loadedFirst = loadedState.Chunks.find(TerrainChunkKey{ asset.WorldId, first.ChunkX, first.ChunkZ });
	const auto loadedSecond = loadedState.Chunks.find(TerrainChunkKey{ asset.WorldId, second.ChunkX, second.ChunkZ });
	Check(loadedFirst != loadedState.Chunks.end(), "first sidecar retains its signed chunk identity");
	Check(loadedSecond != loadedState.Chunks.end(), "second sidecar retains its signed chunk identity");
	if (loadedFirst != loadedState.Chunks.end())
	{
		Check(loadedFirst->second.HeightDeltas == first.HeightDeltas &&
			loadedFirst->second.MaterialOverrides == first.MaterialOverrides,
			"first sidecar round-trips only its sparse height and material edits");
		Check(!loadedFirst->second.Dirty, "loaded first sidecar is clean after its persisted bytes are read");
	}
	if (loadedSecond != loadedState.Chunks.end())
	{
		Check(loadedSecond->second.HeightDeltas == second.HeightDeltas &&
			loadedSecond->second.MaterialOverrides == second.MaterialOverrides,
			"second sidecar round-trips only its sparse height and material edits");
		Check(!loadedSecond->second.Dirty, "loaded second sidecar is clean after its persisted bytes are read");
	}

	const auto legacyPath = scenesDirectory / "LegacyWithoutTerrain.wastescene";
	Check(WriteText(legacyPath, "Scene: LegacyWithoutTerrain\nEntities: []\n"),
		"legacy scene fixture omits optional Terrain metadata");
	if (!std::filesystem::is_regular_file(legacyPath))
		return false;
	Check(loadedRuntime.IsConfigured(), "round-trip Terrain is configured before the legacy-load check");
	Check(reader.Deserialize(legacyPath.string()), "legacy no-Terrain scene remains loadable");
	Check(!loadedScene->GetTerrainRuntime().IsConfigured(),
		"successful legacy no-Terrain load clears previously configured Terrain");
	Check(loadedScene->GetTerrainAssetReference().empty(),
		"successful legacy no-Terrain load clears the previous authored AssetRef");

	project.Close();
	Check(!project.IsOpen(), "temporary Project is closed after serializer integration checks");
	Check(appData.Restore(), "test restores the process APPDATA after isolating recents");
	return true;
}
}

int main()
{
	CheckPreWindowRendererDefaults();
	std::filesystem::path fixtureRoot;
	bool fixtureCreated = false;
	try
	{
		const auto scratch = ScratchDirectory();
		std::error_code createError;
		std::filesystem::create_directories(scratch, createError);
		if (createError)
			throw std::filesystem::filesystem_error("could not create test scratch directory", scratch, createError);
		const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		fixtureRoot = scratch / ("wasteland-scene-terrain-persistence-" + std::to_string(nonce));
		std::filesystem::create_directories(fixtureRoot, createError);
		if (createError)
			throw std::filesystem::filesystem_error("could not create scene persistence fixture", fixtureRoot, createError);
		fixtureCreated = true;

		Check(RunPersistenceIntegration(fixtureRoot),
			"SceneSerializer integration reaches every filesystem and load postcondition");
	}
	catch (const std::exception& exception)
	{
		std::cerr << "FAIL unexpected exception in native SceneSerializer integration: " << exception.what() << '\n';
		++s_Failures;
	}

	if (Project::Get().IsOpen())
		Project::Get().Close();

	if (fixtureCreated)
	{
		std::error_code cleanupError;
		std::filesystem::remove_all(fixtureRoot, cleanupError);
		Check(!cleanupError, "temporary project, .wastescene, asset, and sidecar fixture is removed");
		if (!cleanupError)
		{
			const bool remains = std::filesystem::exists(fixtureRoot, cleanupError);
			Check(!cleanupError && !remains, "checked fixture cleanup leaves no test files behind");
		}
		else
			std::cerr << "  fixture cleanup detail: " << cleanupError.message() << '\n';
	}

	if (s_Failures == 0)
		std::cout << "PASS: native SceneSerializer Terrain persistence integration (" << s_Checks << " checks)\n";
	else
		std::cerr << "FAILED: " << s_Failures << " of " << s_Checks << " checks failed\n";
	return s_Failures == 0 ? 0 : 1;
}
