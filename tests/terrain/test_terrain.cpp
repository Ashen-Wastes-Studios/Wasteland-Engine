#include "Wasteland/Terrain/Terrain.h"
#include "Wasteland/Terrain/TerrainCollision.h"
#include "Wasteland/Terrain/TerrainRuntime.h"
#include "Wasteland/Terrain/TerrainScenePersistence.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <yaml-cpp/yaml.h>

using namespace Wasteland::Terrain;

namespace
{
int s_Failures = 0;

void Check(bool condition, const char* requirement)
{
	if (!condition)
	{
		std::cerr << "FAIL " << requirement << '\n';
		++s_Failures;
	}
}

TerrainAsset MakeAsset()
{
	TerrainAsset asset;
	asset.WorldId = "world.test";
	asset.GeneratorId = "value-noise";
	asset.GeneratorVersion = 1;
	asset.Seed = 42;
	asset.OriginX = -25.0;
	asset.OriginY = 3.5;
	asset.OriginZ = 41.0;
	asset.SampleSpacing = 1.0;
	asset.ChunkResolution = kTerrainChunkCells;
	asset.BaseHeight = 12.0;
	asset.HeightAmplitude = 40.0;
	asset.NoiseFrequency = 0.035;
	asset.NoiseOctaves = 4;
	asset.Lacunarity = 2.0;
	asset.Persistence = 0.5;
	asset.SplatRules = {
		{ "materials/rock", -1000.0, 1000.0, 35.0, 90.0, 1.0 },
		{ "materials/grass", -1000.0, 1000.0, 0.0, 40.0, 1.0 },
	};
	return asset;
}

void TestDefaultTerrainAssetAuthoring(const std::filesystem::path& root)
{
	TerrainAsset asset;
	std::string error;
	Check(CreateDefaultTerrainAsset("world.editor-default", asset, &error),
		"14.6.1: default authoring profile satisfies the terrain asset schema");
	Check(asset.SplatRules.size() == 1 && asset.SplatRules[0].MaterialAssetId == "materials/default",
		"14.6.1: default authoring profile includes a valid fallback splat rule");

	const auto path = root / "editor-default.wlterrain";
	Check(SaveTerrainAsset(path, asset, &error),
		"14.6.1: default authoring profile saves as a valid wlterrain asset");
	TerrainAsset loaded;
	Check(LoadTerrainAsset(path, loaded, &error) && loaded.WorldId == asset.WorldId &&
		loaded.SplatRules.size() == 1 && loaded.SplatRules[0].MaterialAssetId == "materials/default",
		"14.6.1: default authoring profile round-trips its stable identity and fallback material");
}

std::filesystem::path TestDirectory()
{
	const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const char* configuredScratch = std::getenv("TMPDIR");
	const std::filesystem::path scratch = configuredScratch && configuredScratch[0]
		? std::filesystem::path(configuredScratch)
		: std::filesystem::current_path() / ".tmp-terrain-tests";
	std::filesystem::create_directories(scratch);
	const auto path = scratch / ("wasteland-terrain-tests-" + std::to_string(stamp));
	std::filesystem::create_directories(path);
	return path;
}

bool RemoveTestDirectory(const std::filesystem::path& directory, std::error_code& cleanupError)
{
	std::filesystem::path cleanupPath = directory;
#if defined(_WIN32)
	cleanupPath = std::filesystem::absolute(directory, cleanupError);
	if (cleanupError)
		return false;

	std::wstring native = cleanupPath.native();
	constexpr wchar_t kExtendedPrefix[] = L"\\\\?\\";
	if (native.rfind(kExtendedPrefix, 0) != 0)
	{
		if (native.rfind(L"\\\\", 0) == 0)
			native = L"\\\\?\\UNC\\" + native.substr(2);
		else if (native.size() >= 3 && native[1] == L':' &&
			(native[2] == L'\\' || native[2] == L'/'))
			native = kExtendedPrefix + native;
		else
		{
			cleanupError = std::make_error_code(std::errc::invalid_argument);
			return false;
		}
		cleanupPath = std::filesystem::path(native);
	}
#endif

	std::filesystem::remove_all(cleanupPath, cleanupError);
	if (cleanupError)
		return false;
	return !std::filesystem::exists(cleanupPath, cleanupError) && !cleanupError;
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << text;
}

std::string ReadText(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	std::ostringstream contents;
	contents << input.rdbuf();
	return contents.str();
}

std::string RechecksumSidecar(const std::string& text)
{
	const std::size_t marker = text.rfind("checksum ");
	const std::string prefix = text.substr(0, marker);
	std::uint64_t hash = 14695981039346656037ULL;
	for (unsigned char byte : prefix)
	{
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	std::ostringstream output;
	output << prefix << "checksum " << std::hex << std::setfill('0') << std::setw(16) << hash << '\n';
	return output.str();
}

std::string ValidAssetFixture()
{
	return R"(WL_TERRAIN 1
world_id "world.test"
generator_id "value-noise"
generator_version 1
seed 42
origin -25 3.5 41
sample_spacing 1
chunk_resolution 64
height 12 40
noise 0.035 4 2 0.5
splat_count 2
splat "materials/rock" -1000 1000 35 90 1
splat "materials/grass" -1000 1000 0 40 1
end
)";
}

void TestAssetValidationAndLoading(const std::filesystem::path& root)
{
	const TerrainAsset asset = MakeAsset();
	std::string error;
	Check(ValidateTerrainAsset(asset, &error), "14.6.1: valid terrain asset satisfies the authored schema");

	TerrainAsset invalid = asset;
	invalid.ChunkResolution = 32;
	Check(!ValidateTerrainAsset(invalid, &error), "14.6.1: non-64-cell terrain profile is rejected");
	invalid = asset;
	invalid.SampleSpacing = std::numeric_limits<double>::quiet_NaN();
	Check(!ValidateTerrainAsset(invalid, &error), "14.6.1: non-finite asset parameters are rejected");
	invalid = asset;
	invalid.SplatRules[0].MaterialAssetId = invalid.SplatRules[1].MaterialAssetId;
	Check(!ValidateTerrainAsset(invalid, &error), "14.6.1: duplicate material rules are rejected");
	invalid = asset;
	invalid.SplatRules[0].MinimumHeight = -std::numeric_limits<double>::max();
	invalid.SplatRules[0].MaximumHeight = std::numeric_limits<double>::max();
	Check(!ValidateTerrainAsset(invalid, &error), "14.6.1: finite splat bounds with overflowing span are rejected");

	TerrainAsset positiveZero = asset;
	TerrainAsset negativeZero = asset;
	positiveZero.BaseHeight = 0.0;
	negativeZero.BaseHeight = -0.0;
	Check(TerrainAssetFingerprint(positiveZero) == TerrainAssetFingerprint(negativeZero),
		"14.6.1: semantically equal signed-zero values share a stable asset fingerprint");

	std::string oversizedId = ValidAssetFixture();
	const std::size_t idPosition = oversizedId.find("world.test");
	oversizedId.replace(idPosition, std::string("world.test").size(), std::string(257, 'w'));
	TerrainAsset unchanged = asset;
	Check(!ParseTerrainAsset(oversizedId, unchanged, &error),
		"14.6.1: stable identifiers above the byte limit are rejected");
	Check(unchanged.WorldId == asset.WorldId,
		"14.6.1: oversized identifier rejection preserves the output asset");

	const std::string oversizedAsset(kMaximumTerrainAssetBytes + 1, ' ');
	Check(!ParseTerrainAsset(oversizedAsset, unchanged, &error) && error.find("byte limit") != std::string::npos,
		"14.6.1: direct asset parser rejects input above its byte limit");

	const auto assetPath = root / "world.wlterrain";
	WriteText(assetPath, ValidAssetFixture());
	TerrainAsset loaded;
	Check(LoadTerrainAsset(assetPath, loaded, &error), "14.6.1: versioned wlterrain asset loads");
	Check(loaded.WorldId == asset.WorldId && loaded.GeneratorId == asset.GeneratorId &&
		loaded.Seed == asset.Seed && loaded.ChunkResolution == kTerrainChunkCells &&
		loaded.OriginX == asset.OriginX && loaded.OriginY == asset.OriginY && loaded.OriginZ == asset.OriginZ &&
		loaded.SplatRules.size() == asset.SplatRules.size(),
		"14.6.1: loading preserves stable identity, generator and splat rules");
	std::string crlfFixture;
	for (char c : ValidAssetFixture())
	{
		if (c == '\n') crlfFixture += "\r\n";
		else crlfFixture.push_back(c);
	}
	WriteText(assetPath, crlfFixture);
	Check(LoadTerrainAsset(assetPath, loaded, &error),
		"14.6.1: Windows CRLF-authored terrain assets load successfully");
	WriteText(assetPath, oversizedAsset);
	Check(!LoadTerrainAsset(assetPath, loaded, &error) && error.find("byte limit") != std::string::npos,
		"14.6.1: file loader rejects over-limit terrain assets before parsing");
	Check(loaded.WorldId == asset.WorldId,
		"14.6.1: oversized terrain file rejection leaves the output asset unchanged");
	WriteText(assetPath, ValidAssetFixture());

	WriteText(assetPath, "WL_TERRAIN 999\n");
	Check(!LoadTerrainAsset(assetPath, loaded, &error), "14.6.1: unsupported authored schema is rejected");
	Check(loaded.WorldId == asset.WorldId, "14.6.1: failed asset load leaves caller output untouched");
}

void TestAuthoredAssetSaving(const std::filesystem::path& root)
{
	const TerrainAsset asset = MakeAsset();
	const auto assetPath = root / "authored" / "world.wlterrain";
	std::string error;
	Check(SaveTerrainAsset(assetPath, asset, &error),
		"14.6.1: valid authored terrain asset saves to a nested wlterrain path");
	Check(ReadText(assetPath) == SerializeTerrainAsset(asset),
		"14.6.1: saved terrain asset contains only the canonical authored representation");
	TerrainAsset loaded;
	Check(LoadTerrainAsset(assetPath, loaded, &error) &&
		SerializeTerrainAsset(loaded) == SerializeTerrainAsset(asset),
		"14.6.1: saved authored terrain asset round-trips through the loader");

	const std::string priorBytes = "existing terrain asset bytes\n";
	WriteText(assetPath, priorBytes);
	TerrainAsset invalid = asset;
	invalid.SampleSpacing = 0.0;
	Check(!SaveTerrainAsset(assetPath, invalid, &error),
		"14.6.1: invalid authored terrain data is rejected before replacement");
	Check(ReadText(assetPath) == priorBytes,
		"14.6.1: invalid asset save preserves the existing target bytes");

	const auto blockedTarget = root / "asset-target-directory";
	std::filesystem::create_directories(blockedTarget);
	error.clear();
	Check(!SaveTerrainAsset(blockedTarget, asset, &error) && std::filesystem::is_directory(blockedTarget),
		"14.6.1: failed asset replacement reports the write error and preserves the target directory");
	Check(!error.empty() && error.find("sidecar") == std::string::npos,
		"14.6.1: shared failed asset write reports a nonempty terrain-file diagnostic, not sidecar wording");
}

void TestDeterministicNoiseAndChunkIndexing()
{
	const TerrainAsset asset = MakeAsset();
	const auto first = SampleBaseHeight(asset, -137, 82);
	const auto repeated = SampleBaseHeight(asset, -137, 82);
	const auto another = SampleBaseHeight(asset, -136, 82);
	Check(first.has_value() && repeated.has_value() && *first == *repeated,
		"14.6.1: identical generator identity and signed global lattice coordinate produce identical height");
	Check(first.has_value() && another.has_value() && *first != *another,
		"14.6.1: neighboring lattice samples are generated from the deterministic noise field");

	const auto negOne = ChunkCoordinateForLattice(-1);
	const auto neg64 = ChunkCoordinateForLattice(-64);
	const auto neg65 = ChunkCoordinateForLattice(-65);
	Check(negOne.Chunk == -1 && negOne.Local == 63,
		"14.6.1: mathematical floor maps lattice -1 into chunk -1 local 63");
	Check(neg64.Chunk == -1 && neg64.Local == 0,
		"14.6.1: exact negative chunk boundary remains local zero");
	Check(neg65.Chunk == -2 && neg65.Local == 63,
		"14.6.1: mathematical floor maps lattice -65 into chunk -2 local 63");
	const auto negativeWorld = WorldToSampleLattice(-0.01, 0.0, 1.0);
	Check(negativeWorld.has_value() && *negativeWorld == -1,
		"14.6.1: world-to-lattice conversion uses floor rather than truncation for negatives");
	Check(!WorldToSampleLattice(std::numeric_limits<double>::infinity(), 0.0, 1.0).has_value(),
		"14.6.1: invalid world coordinates have explicit no-sample behavior");
	Check(!SampleChunkHeight(asset, 0, 0, kTerrainChunkCells + 1, 0).has_value(),
		"14.6.1: out-of-range chunk sample coordinates are rejected explicitly");
	Check(!SampleChunkHeight(asset, std::numeric_limits<std::int64_t>::max() / kTerrainChunkCells,
		0, kTerrainChunkCells, 0).has_value(),
		"14.6.1: chunk-to-global lattice overflow is rejected explicitly");
}

void TestSplatWeightsAndSharedChunkBorders()
{
	const TerrainAsset asset = MakeAsset();
	for (const auto& conditions : std::vector<std::pair<double, double>>{
		{ -500.0, 0.0 }, { 12.0, 25.0 }, { 10000.0, 90.0 } })
	{
		const auto weights = EvaluateSplatWeights(asset, conditions.first, conditions.second);
		Check(weights.size() == asset.SplatRules.size(), "14.6.1: splat evaluation returns one weight per authored material");
		double sum = 0.0;
		bool finiteAndNonnegative = true;
		for (double weight : weights)
		{
			finiteAndNonnegative = finiteAndNonnegative && std::isfinite(weight) && weight >= 0.0;
			sum += weight;
		}
		Check(finiteAndNonnegative, "14.6.1: slope/height splat weights are finite and nonnegative");
		Check(std::isfinite(sum) && std::abs(sum - 1.0) <= 1e-12,
			"14.6.1: slope/height splat weights normalize to one including out-of-range inputs");
	}

	for (std::int32_t coordinate = 0; coordinate <= kTerrainChunkCells; ++coordinate)
	{
		const auto left = SampleChunkHeight(asset, 7, -3, kTerrainChunkCells, coordinate);
		const auto right = SampleChunkHeight(asset, 8, -3, 0, coordinate);
		Check(left.has_value() && right.has_value() && *left == *right,
			"14.6.1: adjacent X chunks have identical generated samples along their shared border");
		const auto lower = SampleChunkHeight(asset, 7, -3, coordinate, kTerrainChunkCells);
		const auto upper = SampleChunkHeight(asset, 7, -2, coordinate, 0);
		Check(lower.has_value() && upper.has_value() && *lower == *upper,
			"14.6.1: adjacent Z chunks have identical generated samples along their shared border");
	}
}

TerrainChunkEdits MakeEdits(const TerrainAsset& asset, std::int64_t chunkX, std::int64_t chunkZ)
{
	TerrainChunkEdits edits;
	edits.WorldId = asset.WorldId;
	edits.ChunkX = chunkX;
	edits.ChunkZ = chunkZ;
	edits.BaseFingerprint = TerrainAssetFingerprint(asset);
	edits.HeightDeltas[{ 2, 5 }] = 1.25;
	edits.HeightDeltas[{ 64, 32 }] = -0.5;
	edits.MaterialOverrides[{ 3, 4 }] = "materials/rock";
	edits.Dirty = true;
	return edits;
}

void TestCpuChunkMeshLodAndEdits()
{
	const TerrainAsset asset = MakeAsset();
	TerrainChunkMesh full;
	TerrainChunkMesh coarse;
	std::string error;
	Check(BuildTerrainChunkMesh(asset, -2, 9, 0, full, nullptr, &error),
		"14.6.3: a full-resolution 64-cell terrain chunk produces a CPU mesh");
	Check(full.LodLevel == 0 && full.CellsPerSide == 64 && full.Vertices.size() == 65 * 65 &&
		full.Indices.size() == 64 * 64 * 6,
		"14.6.3: LOD0 has a complete 65x65 vertex grid and indexed triangle topology");
	if (full.Vertices.size() == 65 * 65 && full.Indices.size() >= 3)
	{
		const auto expectedHeight = SampleChunkHeight(asset, -2, 9, 2, 5);
		const TerrainMeshVertex& vertex = full.Vertices[5 * 65 + 2];
		Check(expectedHeight && std::abs(vertex.Position[1] - *expectedHeight) < 1.0e-9,
			"14.6.3: mesh positions use the authored deterministic height field");
		const double expectedWorldX = asset.OriginX + (-2LL * kTerrainChunkCells + 2) * asset.SampleSpacing;
		const double expectedWorldZ = asset.OriginZ + (9LL * kTerrainChunkCells + 5) * asset.SampleSpacing;
		Check(vertex.Position[0] == expectedWorldX && vertex.Position[2] == expectedWorldZ,
			"14.6.3: mesh positions preserve authored origin and signed chunk coordinates");
		Check(std::isfinite(vertex.Position[0]) && std::isfinite(vertex.Position[2]) &&
			std::isfinite(vertex.Normal[0]) && std::isfinite(vertex.Normal[1]) &&
			std::isfinite(vertex.Normal[2]) && vertex.Normal[1] > 0.0f,
			"14.6.3: generated vertex positions and upward normals are finite");
		const double normalLength = std::sqrt(vertex.Normal[0] * vertex.Normal[0] +
			vertex.Normal[1] * vertex.Normal[1] + vertex.Normal[2] * vertex.Normal[2]);
		Check(std::abs(normalLength - 1.0) < 1.0e-5,
			"14.6.3: terrain mesh normals are normalized");
		for (std::uint32_t index : full.Indices)
			Check(index < full.Vertices.size(), "14.6.3: mesh indices address an existing vertex");
		const TerrainMeshVertex& a = full.Vertices[full.Indices[0]];
		const TerrainMeshVertex& b = full.Vertices[full.Indices[1]];
		const TerrainMeshVertex& c = full.Vertices[full.Indices[2]];
		const double abx = b.Position[0] - a.Position[0];
		const double abz = b.Position[2] - a.Position[2];
		const double acx = c.Position[0] - a.Position[0];
		const double acz = c.Position[2] - a.Position[2];
		Check(abz * acx - abx * acz > 0.0,
			"14.6.3: terrain triangle winding faces upward");
	}

	Check(BuildTerrainChunkMesh(asset, -2, 9, 2, coarse, nullptr, &error),
		"14.6.3: coarser terrain LOD produces a CPU mesh");
	Check(coarse.LodLevel == 2 && coarse.CellsPerSide == 16 &&
		coarse.Vertices.size() == 17 * 17 && coarse.Indices.size() == 16 * 16 * 6,
		"14.6.3: LOD2 downsamples to 16 cells per side without changing chunk extent");
	TerrainChunkMesh coarsest;
	Check(BuildTerrainChunkMesh(asset, -2, 9, 6, coarsest, nullptr, &error) &&
		coarsest.CellsPerSide == 1 && coarsest.Vertices.size() == 4 && coarsest.Indices.size() == 6,
		"14.6.3: maximum supported LOD still spans the whole chunk with one indexed quad");

	TerrainChunkMesh left;
	TerrainChunkMesh right;
	Check(BuildTerrainChunkMesh(asset, -3, 0, 2, left, nullptr, &error) &&
		BuildTerrainChunkMesh(asset, -2, 0, 2, right, nullptr, &error),
		"14.6.3: adjacent chunks build at the same discrete LOD");
	if (left.Vertices.size() == 17 * 17 && right.Vertices.size() == 17 * 17)
	{
		bool sharedEdgeMatches = true;
		for (std::size_t z = 0; z <= 16; ++z)
		{
			const TerrainMeshVertex& leftVertex = left.Vertices[z * 17 + 16];
			const TerrainMeshVertex& rightVertex = right.Vertices[z * 17];
			sharedEdgeMatches = sharedEdgeMatches && leftVertex.Position == rightVertex.Position;
		}
		Check(sharedEdgeMatches,
			"14.6.3: adjacent chunk meshes share identical world-space boundary vertices");
	}
	TerrainChunkMesh lower;
	TerrainChunkMesh upper;
	Check(BuildTerrainChunkMesh(asset, -3, -2, 2, lower, nullptr, &error) &&
		BuildTerrainChunkMesh(asset, -3, -1, 2, upper, nullptr, &error),
		"14.6.3: adjacent Z chunks build at the same discrete LOD");
	if (lower.Vertices.size() == 17 * 17 && upper.Vertices.size() == 17 * 17)
	{
		bool sharedEdgeMatches = true;
		for (std::size_t x = 0; x <= 16; ++x)
			sharedEdgeMatches = sharedEdgeMatches &&
				lower.Vertices[16 * 17 + x].Position == upper.Vertices[x].Position;
		Check(sharedEdgeMatches,
			"14.6.3: adjacent Z chunk meshes share identical world-space boundary vertices");
	}

	TerrainChunkEdits edits = MakeEdits(asset, -2, 9);
	TerrainChunkMesh edited;
	Check(BuildTerrainChunkMesh(asset, -2, 9, 0, edited, &edits, &error),
		"14.6.3: a valid sparse runtime sidecar overlays the generated chunk mesh");
	if (edited.Vertices.size() == 65 * 65)
	{
		const auto baseHeight = SampleChunkHeight(asset, -2, 9, 2, 5);
		Check(baseHeight && std::abs(edited.Vertices[5 * 65 + 2].Position[1] -
			(*baseHeight + 1.25)) < 1.0e-9,
			"14.6.3: sparse height deltas appear at their exact base-lattice vertices");
		const auto boundaryHeight = SampleChunkHeight(asset, -2, 9, 64, 32);
		Check(boundaryHeight && std::abs(edited.Vertices[32 * 65 + 64].Position[1] -
			(*boundaryHeight - 0.5)) < 1.0e-9,
			"14.6.3: sparse edits retain shared-boundary vertex deltas");
	}

	TerrainChunkMesh unchanged = coarse;
	Check(!BuildTerrainChunkMesh(asset, -2, 9, 7, unchanged, nullptr, &error) &&
		unchanged.LodLevel == coarse.LodLevel && unchanged.Vertices.size() == coarse.Vertices.size(),
		"14.6.3: unsupported LOD fails transactionally without replacing the prior mesh");
	TerrainChunkEdits staleEdits = edits;
	staleEdits.BaseFingerprint = "0000000000000000";
	Check(!BuildTerrainChunkMesh(asset, -2, 9, 0, unchanged, &staleEdits, &error) &&
		unchanged.LodLevel == coarse.LodLevel,
		"14.6.3: a stale sidecar fingerprint is rejected without modifying the mesh");
	TerrainChunkEdits misplacedEdits = edits;
	++misplacedEdits.ChunkX;
	Check(!BuildTerrainChunkMesh(asset, -2, 9, 0, unchanged, &misplacedEdits, &error) &&
		unchanged.LodLevel == coarse.LodLevel,
		"14.6.3: sidecar chunk identity mismatch is rejected transactionally");
	Check(!BuildTerrainChunkMesh(asset, (std::numeric_limits<std::int64_t>::max)() / kTerrainChunkCells,
		0, 0, unchanged, nullptr, &error) && unchanged.LodLevel == coarse.LodLevel,
		"14.6.3: lattice overflow rejects the chunk without replacing its prior mesh");
	TerrainAsset impreciseCoordinates = asset;
	impreciseCoordinates.OriginX = 1.0e20;
	Check(!BuildTerrainChunkMesh(impreciseCoordinates, 0, 0, 0, unchanged, nullptr, &error) &&
		unchanged.LodLevel == coarse.LodLevel,
		"14.6.3: world coordinates that collapse adjacent lattice vertices fail transactionally");
}

void TestSparseSidecarRoundTripAndDirtyFailure(const std::filesystem::path& root)
{
	const TerrainAsset asset = MakeAsset();
	const auto sidecar = root / "state" / "chunk.wlterrainstate";
	TerrainChunkEdits edits = MakeEdits(asset, -2, 9);
	std::string error;
	Check(SaveTerrainChunkEdits(sidecar, asset, edits, &error),
		"14.6.2: sparse base-lattice deltas and material overrides save to a separate sidecar");
	Check(!edits.Dirty, "14.6.2: successful atomic sidecar write clears the dirty marker");

	TerrainWorldState active;
	Check(LoadTerrainChunkEdits(sidecar, asset, -2, 9, active, &error),
		"14.6.2: matching versioned sidecar loads");
	const auto loaded = active.Chunks.find({ asset.WorldId, -2, 9 });
	Check(loaded != active.Chunks.end() && !loaded->second.Dirty &&
		loaded->second.HeightDeltas == MakeEdits(asset, -2, 9).HeightDeltas &&
		loaded->second.MaterialOverrides == MakeEdits(asset, -2, 9).MaterialOverrides,
		"14.6.2: sidecar round-trip preserves only sparse deltas and explicit material overrides");

	edits.HeightDeltas[{ 2, 5 }] = 3.0;
	edits.Dirty = true;
	Check(SaveTerrainChunkEdits(sidecar, asset, edits, &error) && !edits.Dirty,
		"14.6.2: atomic sidecar replacement updates an existing file and clears dirty only on success");
	TerrainWorldState replaced;
	Check(LoadTerrainChunkEdits(sidecar, asset, -2, 9, replaced, &error) &&
		replaced.Chunks.at({ asset.WorldId, -2, 9 }).HeightDeltas.at({ 2, 5 }) == 3.0,
		"14.6.2: atomic replacement publishes the complete newer sparse edit set");

	const auto blockedTarget = root / "existing-directory-target";
	std::filesystem::create_directories(blockedTarget);
	TerrainChunkEdits dirty = MakeEdits(asset, 4, 5);
	Check(!SaveTerrainChunkEdits(blockedTarget, asset, dirty, &error),
		"14.6.2: failed atomic replacement reports persistence failure");
	Check(dirty.Dirty && std::filesystem::is_directory(blockedTarget),
		"14.6.2: failed replacement keeps edits dirty and leaves the existing target intact");
}

void TestSidecarRejectionIsTransactional(const std::filesystem::path& root)
{
	const TerrainAsset asset = MakeAsset();
	const auto validPath = root / "valid.wlterrainstate";
	TerrainChunkEdits edits = MakeEdits(asset, -8, 3);
	std::string error;
	Check(SaveTerrainChunkEdits(validPath, asset, edits, &error), "14.6.2: fixture sidecar writes before rejection checks");
	const std::string validBytes = ReadText(validPath);

	TerrainWorldState active;
	TerrainChunkEdits sentinel = MakeEdits(asset, -8, 3);
	sentinel.HeightDeltas.clear();
	sentinel.HeightDeltas[{ 1, 1 }] = 77.0;
	sentinel.Dirty = true;
	active.Chunks[{ asset.WorldId, -8, 3 }] = sentinel;
	const auto before = active.Chunks.at({ asset.WorldId, -8, 3 });

	TerrainWorldState dirtyActive;
	TerrainChunkEdits dirtySentinel = MakeEdits(asset, -8, 3);
	dirtySentinel.HeightDeltas.clear();
	dirtySentinel.HeightDeltas[{ 1, 1 }] = 77.0;
	dirtySentinel.Dirty = true;
	dirtyActive.Chunks[{ asset.WorldId, -8, 3 }] = dirtySentinel;
	Check(!LoadTerrainChunkEdits(validPath, asset, -8, 3, dirtyActive, &error),
		"14.6.2: loading a sidecar cannot overwrite an already dirty active chunk");
	Check(dirtyActive.Chunks.at({ asset.WorldId, -8, 3 }).Dirty &&
		dirtyActive.Chunks.at({ asset.WorldId, -8, 3 }).HeightDeltas == dirtySentinel.HeightDeltas,
		"14.6.2: rejected duplicate load preserves unsaved active edits");

	const auto oversizedState = root / "oversized.wlterrainstate";
	WriteText(oversizedState, validBytes + std::string(kMaximumTerrainStateBytes, ' '));
	Check(!LoadTerrainChunkEdits(oversizedState, asset, -8, 3, active, &error) &&
		error.find("byte limit") != std::string::npos,
		"14.6.2: sidecar loader rejects inputs above its byte limit");
	Check(active.Chunks.at({ asset.WorldId, -8, 3 }).HeightDeltas == before.HeightDeltas,
		"14.6.2: oversized sidecar rejection leaves active state untouched");

	const auto corruptPath = root / "corrupt.wlterrainstate";
	std::string corrupt = validBytes;
	const auto valuePosition = corrupt.find("1.25");
	if (valuePosition != std::string::npos)
		corrupt[valuePosition] = '9';
	WriteText(corruptPath, corrupt);
	Check(!LoadTerrainChunkEdits(corruptPath, asset, -8, 3, active, &error),
		"14.6.2: checksum-corrupt sidecar is rejected");
	Check(active.Chunks.at({ asset.WorldId, -8, 3 }).HeightDeltas == before.HeightDeltas &&
		active.Chunks.at({ asset.WorldId, -8, 3 }).Dirty == before.Dirty,
		"14.6.2: corrupt sidecar rejection leaves active world state untouched");

	TerrainAsset otherWorld = asset;
	otherWorld.WorldId = "world.other";
	Check(!LoadTerrainChunkEdits(validPath, otherWorld, -8, 3, active, &error),
		"14.6.2: sidecar with another world id is rejected");
	TerrainAsset changedBase = asset;
	++changedBase.GeneratorVersion;
	Check(!LoadTerrainChunkEdits(validPath, changedBase, -8, 3, active, &error),
		"14.6.2: sidecar with a mismatched base generator fingerprint is rejected");
	Check(active.Chunks.at({ asset.WorldId, -8, 3 }).HeightDeltas == before.HeightDeltas,
		"14.6.2: world/fingerprint mismatch leaves active world state untouched");

	std::string unsupported = validBytes;
	const auto schemaPosition = unsupported.find("WL_TERRAIN_STATE 1");
	if (schemaPosition != std::string::npos)
		unsupported.replace(schemaPosition, std::string("WL_TERRAIN_STATE 1").size(), "WL_TERRAIN_STATE 99");
	unsupported = RechecksumSidecar(unsupported);
	const auto unsupportedPath = root / "unsupported.wlterrainstate";
	WriteText(unsupportedPath, unsupported);
	Check(!LoadTerrainChunkEdits(unsupportedPath, asset, -8, 3, active, &error),
		"14.6.2: unsupported sidecar schema is rejected");
	Check(active.Chunks.at({ asset.WorldId, -8, 3 }).HeightDeltas == before.HeightDeltas,
		"14.6.2: unsupported sidecar rejection leaves active world state untouched");

	std::string malformed = validBytes;
	const auto countPosition = malformed.find("delta_count 2");
	if (countPosition != std::string::npos)
		malformed.replace(countPosition, std::string("delta_count 2").size(), "delta_count 3");
	else
		malformed = "not a terrain sidecar";
	malformed = RechecksumSidecar(malformed);
	const auto malformedPath = root / "malformed.wlterrainstate";
	WriteText(malformedPath, malformed);
	Check(!LoadTerrainChunkEdits(malformedPath, asset, -8, 3, active, &error),
		"14.6.2: syntactically malformed sidecar is rejected even with a valid checksum");
	Check(active.Chunks.at({ asset.WorldId, -8, 3 }).HeightDeltas == before.HeightDeltas,
		"14.6.2: malformed sidecar rejection leaves active world state untouched");
}

void TestDeterministicSidecarPathResolution(const std::filesystem::path& root)
{
	const TerrainAsset asset = MakeAsset();
	const std::filesystem::path sidecarRoot = root / "sidecars";
	std::filesystem::path first;
	std::filesystem::path repeated;
	std::string error;
	Check(ResolveTerrainChunkEditsPath(sidecarRoot, asset, -17, 23, first, &error) &&
		ResolveTerrainChunkEditsPath(sidecarRoot, asset, -17, 23, repeated, &error) && first == repeated,
		"14.6.2-path: the same world and signed chunk key resolves to one stable path");

	const auto relative = first.lexically_relative(sidecarRoot);
	Check(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
		"14.6.2-path: the resolved sidecar remains lexically beneath the supplied root");

	TerrainAsset slashAndColon = asset;
	slashAndColon.WorldId = "world/a:b";
	TerrainAsset colonAndSlash = asset;
	colonAndSlash.WorldId = "world:a/b";
	std::filesystem::path slashAndColonPath;
	std::filesystem::path colonAndSlashPath;
	Check(ResolveTerrainChunkEditsPath(sidecarRoot, slashAndColon, 4, -5, slashAndColonPath, &error) &&
		ResolveTerrainChunkEditsPath(sidecarRoot, colonAndSlash, 4, -5, colonAndSlashPath, &error) &&
		slashAndColonPath != colonAndSlashPath,
		"14.6.2-path: valid slash/colon world IDs remain distinct without raw path components");

	const auto minimum = (std::numeric_limits<std::int64_t>::min)();
	const auto maximum = (std::numeric_limits<std::int64_t>::max)();
	const std::vector<std::pair<std::int64_t, std::int64_t>> coordinates = {
		{ 0, 0 }, { 1, 0 }, { -1, 0 }, { minimum, 0 }, { maximum, 0 }, { 0, minimum }, { 0, maximum }
	};
	std::set<std::filesystem::path> coordinatePaths;
	bool everyCoordinateResolved = true;
	for (const auto& coordinate : coordinates)
	{
		std::filesystem::path path;
		everyCoordinateResolved = ResolveTerrainChunkEditsPath(sidecarRoot, asset,
			coordinate.first, coordinate.second, path, &error) && everyCoordinateResolved;
		coordinatePaths.insert(path);
	}
	Check(everyCoordinateResolved && coordinatePaths.size() == coordinates.size(),
		"14.6.2-path: positive, negative, and full-range int64 chunk keys map injectively");

	std::filesystem::path unchanged = root / "sentinel-path";
	TerrainAsset invalidId = asset;
	invalidId.WorldId = "invalid\\world";
	Check(!ResolveTerrainChunkEditsPath(sidecarRoot, invalidId, 0, 0, unchanged, &error) &&
		unchanged == root / "sentinel-path",
		"14.6.2-path: invalid stable IDs fail without modifying the output path");
	Check(!ResolveTerrainChunkEditsPath({}, asset, 0, 0, unchanged, &error) &&
		unchanged == root / "sentinel-path",
		"14.6.2-path: an empty root fails without modifying the output path");

	TerrainAsset invalidAsset = asset;
	invalidAsset.SampleSpacing = 0.0;
	Check(!ResolveTerrainChunkEditsPath(sidecarRoot, invalidAsset, 0, 0, unchanged, &error) &&
		unchanged == root / "sentinel-path",
		"14.6.2-path: invalid terrain assets fail without modifying the output path");
}

void TestMaximumWorldIdSidecarRoundTrip()
{
	TerrainAsset asset = MakeAsset();
	asset.WorldId.assign(kMaximumTerrainStableIdBytes, 'w');
	std::string error;
	Check(asset.WorldId.size() == kMaximumTerrainStableIdBytes && ValidateTerrainAsset(asset, &error),
		"14.6.2-path: maximum-length stable world ID is valid terrain data");

	const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const auto shortRoot = std::filesystem::current_path() /
		(".tmp-terrain-max-world-id-" + std::to_string(stamp));
	std::filesystem::create_directories(shortRoot);
	std::filesystem::path sidecar;
	const bool resolved = ResolveTerrainChunkEditsPath(shortRoot, asset, -17, 23, sidecar, &error);
	Check(resolved, "14.6.2-path: maximum-length world ID resolves beneath a short sidecar root");
	if (resolved)
	{
		Check(sidecar.native().size() > 260 && sidecar.native().rfind(L"\\\\?\\", 0) != 0,
			"14.6.2-path: resolved long path stays logical and unprefixed");
		TerrainChunkEdits edits = MakeEdits(asset, -17, 23);
		Check(SaveTerrainChunkEdits(sidecar, asset, edits, &error) && !edits.Dirty,
			"14.6.2-path: maximum-length world ID sidecar saves through the resolved long path");
		TerrainWorldState loaded;
		Check(LoadTerrainChunkEdits(sidecar, asset, -17, 23, loaded, &error),
			"14.6.2-path: maximum-length world ID sidecar loads through the resolved long path");
		const auto chunk = loaded.Chunks.find({ asset.WorldId, -17, 23 });
		Check(chunk != loaded.Chunks.end() && !chunk->second.Dirty &&
			chunk->second.HeightDeltas == edits.HeightDeltas &&
			chunk->second.MaterialOverrides == edits.MaterialOverrides,
			"14.6.2-path: maximum-length world ID sidecar round-trip preserves edits");
	}
	std::error_code cleanupError;
	Check(RemoveTestDirectory(shortRoot, cleanupError),
		"14.6.2-path: maximum-length world ID fixture cleanup removes its root without error");
}

bool SameEdits(const TerrainChunkEdits& left, const TerrainChunkEdits& right)
{
	return left.WorldId == right.WorldId && left.ChunkX == right.ChunkX && left.ChunkZ == right.ChunkZ &&
		left.BaseFingerprint == right.BaseFingerprint && left.TerrainSchemaVersion == right.TerrainSchemaVersion &&
		left.HeightDeltas == right.HeightDeltas && left.MaterialOverrides == right.MaterialOverrides &&
		left.Dirty == right.Dirty;
}

bool SameWorldState(const TerrainWorldState& left, const TerrainWorldState& right)
{
	if (left.Chunks.size() != right.Chunks.size())
		return false;
	auto leftIt = left.Chunks.begin();
	auto rightIt = right.Chunks.begin();
	for (; leftIt != left.Chunks.end(); ++leftIt, ++rightIt)
	{
		if (leftIt->first != rightIt->first || !SameEdits(leftIt->second, rightIt->second))
			return false;
	}
	return true;
}

void TestWorldSidecarCollectionRoundTripAndIsolation(const std::filesystem::path& root)
{
	const TerrainAsset asset = MakeAsset();
	TerrainAsset otherAsset = asset;
	otherAsset.WorldId = "world.other";
	const auto minimum = (std::numeric_limits<std::int64_t>::min)();
	const auto maximum = (std::numeric_limits<std::int64_t>::max)();
	TerrainWorldState state;
	state.Chunks[{ asset.WorldId, -17, minimum }] = MakeEdits(asset, -17, minimum);
	state.Chunks[{ asset.WorldId, maximum, 23 }] = MakeEdits(asset, maximum, 23);
	state.Chunks[{ otherAsset.WorldId, 7, -8 }] = MakeEdits(otherAsset, 7, -8);
	const auto otherWorldBefore = state.Chunks.at({ otherAsset.WorldId, 7, -8 });
	std::string error;
	Check(SaveTerrainChunkEditsForWorld(root / "collection-roundtrip", asset, state, &error),
		"14.6.2-world: dirty chunks for one world save as a collection");
	Check(!state.Chunks.at({ asset.WorldId, -17, minimum }).Dirty &&
		!state.Chunks.at({ asset.WorldId, maximum, 23 }).Dirty &&
		SameEdits(state.Chunks.at({ otherAsset.WorldId, 7, -8 }), otherWorldBefore),
		"14.6.2-world: save clears only matching-world dirty chunks and preserves unrelated worlds");

	std::filesystem::path firstSidecar;
	Check(ResolveTerrainChunkEditsPath(root / "collection-roundtrip", asset, -17, minimum, firstSidecar, &error),
		"14.6.2-world: collection sidecar path resolves for unrelated-file fixture");
	if (!firstSidecar.empty())
		WriteText(firstSidecar.parent_path() / "atomic-write.tmp", "ignored temporary data");

	TerrainWorldState loaded;
	Check(LoadTerrainChunkEditsForWorld(root / "collection-roundtrip", asset, loaded, &error),
		"14.6.2-world: collection load discovers and merges sidecars");
	TerrainChunkEdits expectedMinimum = MakeEdits(asset, -17, minimum);
	expectedMinimum.Dirty = false;
	TerrainChunkEdits expectedMaximum = MakeEdits(asset, maximum, 23);
	expectedMaximum.Dirty = false;
	Check(loaded.Chunks.size() == 2 && loaded.Chunks.count({ asset.WorldId, -17, minimum }) == 1 &&
		loaded.Chunks.count({ asset.WorldId, maximum, 23 }) == 1 &&
		SameEdits(loaded.Chunks.at({ asset.WorldId, -17, minimum }), expectedMinimum) &&
		SameEdits(loaded.Chunks.at({ asset.WorldId, maximum, 23 }), expectedMaximum),
		"14.6.2-world: collection round-trip preserves negative and extreme signed coordinates and edits");

	TerrainWorldState missingState;
	missingState.Chunks[{ asset.WorldId, 11, 12 }] = MakeEdits(asset, 11, 12);
	const TerrainWorldState missingBefore = missingState;
	Check(LoadTerrainChunkEditsForWorld(root / "no-persisted-world", asset, missingState, &error) &&
		SameWorldState(missingState, missingBefore),
		"14.6.2-world: absent world directory is a successful no-op");
}

void TestWorldSidecarCollectionIsTransactional(const std::filesystem::path& root)
{
	const TerrainAsset asset = MakeAsset();
	const auto sidecarRoot = root / "collection-transaction";
	TerrainWorldState source;
	source.Chunks[{ asset.WorldId, -4, 9 }] = MakeEdits(asset, -4, 9);
	source.Chunks[{ asset.WorldId, 8, -2 }] = MakeEdits(asset, 8, -2);
	std::string error;
	Check(SaveTerrainChunkEditsForWorld(sidecarRoot, asset, source, &error),
		"14.6.2-world: fixtures save before transactional rejection checks");

	TerrainWorldState active;
	TerrainChunkEdits sentinel = MakeEdits(asset, 77, 88);
	sentinel.HeightDeltas.clear();
	sentinel.HeightDeltas[{ 1, 1 }] = 77.0;
	active.Chunks[{ asset.WorldId, 77, 88 }] = sentinel;
	const TerrainWorldState before = active;
	std::filesystem::path directorySidecar;
	Check(ResolveTerrainChunkEditsPath(sidecarRoot, asset, 8, -2, directorySidecar, &error),
		"14.6.2-world: second sidecar path resolves for dirty-conflict fixture");
	if (!directorySidecar.empty())
	{
		TerrainChunkEdits conflict = MakeEdits(asset, 8, -2);
		conflict.HeightDeltas.clear();
		conflict.HeightDeltas[{ 2, 2 }] = 99.0;
		conflict.Dirty = true;
		active.Chunks[{ asset.WorldId, 8, -2 }] = conflict;
		const TerrainWorldState conflictBefore = active;
		Check(!LoadTerrainChunkEditsForWorld(sidecarRoot, asset, active, &error) &&
			SameWorldState(active, conflictBefore),
			"14.6.2-world: dirty-key conflict rejects the whole merge without changing any state");

		active = before;
		const std::string validSidecarBytes = ReadText(directorySidecar);
		std::string corrupt = validSidecarBytes;
		const std::size_t valuePosition = corrupt.find("1.25");
		if (valuePosition != std::string::npos)
			corrupt[valuePosition] = '9';
		WriteText(directorySidecar, corrupt);
		Check(!LoadTerrainChunkEditsForWorld(sidecarRoot, asset, active, &error) &&
			SameWorldState(active, before),
			"14.6.2-world: corrupt sidecar rejects the whole merge without changing any state");

		active = before;
		WriteText(directorySidecar, validSidecarBytes);
		WriteText(directorySidecar.parent_path() / "malformed.wlterrainstate", "not a canonical chunk filename");
		Check(!LoadTerrainChunkEditsForWorld(sidecarRoot, asset, active, &error) &&
			SameWorldState(active, before),
			"14.6.2-world: malformed matching-extension filename fails transactionally");
	}

	const auto failureRoot = root / "collection-partial-save";
	TerrainWorldState partiallySaved;
	partiallySaved.Chunks[{ asset.WorldId, 1, 0 }] = MakeEdits(asset, 1, 0);
	partiallySaved.Chunks[{ asset.WorldId, 2, 0 }] = MakeEdits(asset, 2, 0);
	std::filesystem::path blockedPath;
	Check(ResolveTerrainChunkEditsPath(failureRoot, asset, 2, 0, blockedPath, &error),
		"14.6.2-world: blocked chunk path resolves before partial-save fixture");
	if (!blockedPath.empty())
		std::filesystem::create_directories(blockedPath);
	Check(!SaveTerrainChunkEditsForWorld(failureRoot, asset, partiallySaved, &error),
		"14.6.2-world: later atomic write failure is reported");
	Check(!partiallySaved.Chunks.at({ asset.WorldId, 1, 0 }).Dirty &&
		partiallySaved.Chunks.at({ asset.WorldId, 2, 0 }).Dirty,
		"14.6.2-world: each chunk clears Dirty only after its own successful atomic save");

	const auto prevalidationRoot = root / "collection-prevalidation";
	TerrainWorldState mismatched;
	mismatched.Chunks[{ asset.WorldId, 1, 0 }] = MakeEdits(asset, 1, 0);
	mismatched.Chunks[{ asset.WorldId, 2, 0 }] = MakeEdits(asset, 3, 0);
	Check(!SaveTerrainChunkEditsForWorld(prevalidationRoot, asset, mismatched, &error) &&
		mismatched.Chunks.at({ asset.WorldId, 1, 0 }).Dirty &&
		!std::filesystem::exists(prevalidationRoot),
		"14.6.2-world: mismatched map key is rejected before any avoidable sidecar writes");
}

void TestMaximumWorldIdCollectionDiscovery(const std::filesystem::path& root)
{
	TerrainAsset asset = MakeAsset();
	asset.WorldId.assign(kMaximumTerrainStableIdBytes, 'w');
	const auto sidecarRoot = root / "max-world-collection";
	TerrainWorldState source;
	source.Chunks[{ asset.WorldId, -17, 23 }] = MakeEdits(asset, -17, 23);
	source.Chunks[{ asset.WorldId, 4, -5 }] = MakeEdits(asset, 4, -5);
	std::string error;
	std::filesystem::path firstSidecar;
	Check(ResolveTerrainChunkEditsPath(sidecarRoot, asset, -17, 23, firstSidecar, &error),
		"14.6.2-world-path: maximum-length world ID resolves for collection discovery");
#if defined(_WIN32)
	Check(firstSidecar.parent_path().native().size() > 260,
		"14.6.2-world-path: maximum-length encoded world directory exceeds MAX_PATH");
#endif
	Check(SaveTerrainChunkEditsForWorld(sidecarRoot, asset, source, &error),
		"14.6.2-world-path: multiple sidecars save under a maximum-length world ID");
	TerrainWorldState loaded;
	Check(LoadTerrainChunkEditsForWorld(sidecarRoot, asset, loaded, &error) && loaded.Chunks.size() == 2 &&
		loaded.Chunks.count({ asset.WorldId, -17, 23 }) == 1 && loaded.Chunks.count({ asset.WorldId, 4, -5 }) == 1,
		"14.6.2-world-path: multiple sidecars are discovered from the extended-length world directory");
}

void TestNearMaximumWindowsPathSidecarEnumeration()
{
#if defined(_WIN32)
	constexpr std::size_t kWindowsMaxPath = 260;
	const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const auto shortRoot = std::filesystem::current_path() / ("t-" + std::to_string(stamp));
	std::filesystem::create_directories(shortRoot);

	TerrainAsset asset = MakeAsset();
	std::filesystem::path firstSidecar;
	std::size_t worldDirectoryLength = 0;
	bool foundBoundaryWorldId = false;
	for (std::size_t idLength = 1; idLength <= kMaximumTerrainStableIdBytes; ++idLength)
	{
		asset.WorldId.assign(idLength, 'w');
		if (!ResolveTerrainChunkEditsPath(shortRoot, asset, -17, 23, firstSidecar, nullptr))
			continue;
		worldDirectoryLength = firstSidecar.parent_path().native().size();
		if (worldDirectoryLength == kWindowsMaxPath - 1 ||
			worldDirectoryLength == kWindowsMaxPath - 2)
		{
			foundBoundaryWorldId = true;
			break;
		}
	}
	Check(shortRoot.native().size() < kWindowsMaxPath && foundBoundaryWorldId,
		"14.6.2-world-path: a short caller root and valid world ID produce a 258/259-character world directory");
	if (foundBoundaryWorldId)
	{
		std::filesystem::path secondSidecar;
		const bool resolvedSecond = ResolveTerrainChunkEditsPath(shortRoot, asset, 4, -5, secondSidecar, nullptr);
		Check(worldDirectoryLength == kWindowsMaxPath - 1 || worldDirectoryLength == kWindowsMaxPath - 2,
			"14.6.2-world-path: boundary world directory length is verified below MAX_PATH");
		Check(resolvedSecond && firstSidecar.parent_path() == secondSidecar.parent_path() &&
			firstSidecar.native().size() > kWindowsMaxPath && secondSidecar.native().size() > kWindowsMaxPath,
			"14.6.2-world-path: both real sidecar file paths exceed MAX_PATH");

		TerrainWorldState source;
		source.Chunks[{ asset.WorldId, -17, 23 }] = MakeEdits(asset, -17, 23);
		source.Chunks[{ asset.WorldId, 4, -5 }] = MakeEdits(asset, 4, -5);
		std::string error;
		const bool saved = SaveTerrainChunkEditsForWorld(shortRoot, asset, source, &error);
		Check(saved, "14.6.2-world-path: two sidecars save below-MAX_PATH world directory entries");
		if (saved)
		{
			TerrainWorldState loaded;
			const bool loadedWorld = LoadTerrainChunkEditsForWorld(shortRoot, asset, loaded, &error);
			if (!loadedWorld)
				std::cerr << "14.6.2-world-path: enumeration error: " << error << '\n';
			Check(loadedWorld, "14.6.2-world-path: sidecars enumerate from a 258/259-character world directory");
			if (loadedWorld)
			{
				TerrainChunkEdits expectedFirst = MakeEdits(asset, -17, 23);
				TerrainChunkEdits expectedSecond = MakeEdits(asset, 4, -5);
				expectedFirst.Dirty = false;
				expectedSecond.Dirty = false;
				const auto firstLoaded = loaded.Chunks.find({ asset.WorldId, -17, 23 });
				const auto secondLoaded = loaded.Chunks.find({ asset.WorldId, 4, -5 });
				Check(loaded.Chunks.size() == 2 && firstLoaded != loaded.Chunks.end() &&
					secondLoaded != loaded.Chunks.end() && SameEdits(firstLoaded->second, expectedFirst) &&
					SameEdits(secondLoaded->second, expectedSecond),
					"14.6.2-world-path: collection enumeration loads both exact chunk edits");
			}
		}
	}
	std::error_code cleanupError;
	Check(RemoveTestDirectory(shortRoot, cleanupError),
		"14.6.2-world-path: near-MAX_PATH fixture cleanup removes its root without error");
#endif
}

void TestNearMaximumWindowsPathTemporarySuffixRoundTrip()
{
#if defined(_WIN32)
	constexpr std::size_t kWindowsMaxPath = 260;
	constexpr std::size_t kTargetPathLength = kWindowsMaxPath - 5;
	const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const auto shortRoot = std::filesystem::current_path() / ("t-" + std::to_string(stamp));
	const std::filesystem::path filename("world.wlterrain");
	const std::size_t fixedPathLength = shortRoot.native().size() + 2 + filename.native().size();
	const bool canBuildTargetPath = kTargetPathLength > fixedPathLength &&
		kTargetPathLength - fixedPathLength <= 255;
	Check(canBuildTargetPath, "14.6.1-path: short root can produce a target just below MAX_PATH");
	if (canBuildTargetPath)
	{
		const std::filesystem::path assetPath = shortRoot /
			std::string(kTargetPathLength - fixedPathLength, 'd') / filename;
		std::filesystem::create_directories(assetPath.parent_path());
		const std::size_t targetLength = assetPath.native().size();
		const std::size_t minimumTemporarySuffixLength = std::wstring(L".tmp.0.0").size();
		Check(targetLength < kWindowsMaxPath &&
			targetLength + minimumTemporarySuffixLength >= kWindowsMaxPath,
			"14.6.1-path: logical target is below MAX_PATH while its generated temp sibling reaches MAX_PATH");

		const TerrainAsset asset = MakeAsset();
		WriteText(assetPath, "previous terrain asset bytes\n");
		std::string error;
		const bool saved = SaveTerrainAsset(assetPath, asset, &error);
		Check(saved, "14.6.1-path: authored asset replaces an existing target when only its temporary sibling exceeds MAX_PATH");
		if (!saved)
			Check(error == "could not open temporary terrain file",
				"14.6.1-path: near-MAX_PATH regression is specifically a temporary-file open failure");
		if (saved)
		{
			TerrainAsset loaded;
			Check(LoadTerrainAsset(assetPath, loaded, &error) &&
				SerializeTerrainAsset(loaded) == SerializeTerrainAsset(asset),
				"14.6.1-path: near-MAX_PATH authored asset round-trips after atomic replacement");
		}
	}
	std::error_code cleanupError;
	Check(RemoveTestDirectory(shortRoot, cleanupError),
		"14.6.1-path: near-MAX_PATH temporary-file fixture cleanup removes its root without error");
#endif
}


void TestBudgetedTerrainChunkStreaming()
{
	const TerrainAsset asset = MakeAsset();
	TerrainStreamingConfig config;
	config.LoadRadiusChunks = 1;
	config.LodDistanceThresholds = { 1, 2, 3, 4, 5, 6 };
	config.MaxChunkBuildsPerUpdate = 2;
	TerrainChunkStreamer streamer;
	std::string error;
	Check(streamer.Configure(asset, {}, config, &error),
		"14.6.4: valid asset and bounded streaming configuration initialize the CPU streamer");
	Check(streamer.UpdateView(asset.OriginX, asset.OriginZ, &error),
		"14.6.4: world-space view center maps to signed chunk coordinates");
	Check(streamer.GetPendingChunkCount() == 9 && streamer.GetLoadedChunkCount() == 0,
		"14.6.4: radius-one view queues exactly its nine chunk meshes without eager work");

	std::uint32_t built = 0;
	Check(streamer.ProcessPending(&built, &error) && built == 2,
		"14.6.4: each update respects the configured two-mesh build budget");
	const auto centerKey = std::make_pair<std::int64_t, std::int64_t>(0, 0);
	const auto firstNeighborKey = std::make_pair<std::int64_t, std::int64_t>(-1, -1);
	const auto& initialMeshes = streamer.GetLoadedChunks();
	Check(initialMeshes.size() == 2 && initialMeshes.count(centerKey) == 1 &&
		initialMeshes.count(firstNeighborKey) == 1 &&
		initialMeshes.at(centerKey).LodLevel == 0 && initialMeshes.at(firstNeighborKey).LodLevel == 1,
		"14.6.4: nearest chunks build first with deterministic ties and distance-based LOD");
	Check(streamer.GetPendingChunkCount() == 7,
		"14.6.4: unbuilt desired chunks remain queued after a budget-limited update");

	const std::size_t pendingBeforeBadView = streamer.GetPendingChunkCount();
	Check(!streamer.UpdateView(std::numeric_limits<double>::quiet_NaN(), asset.OriginZ, &error) &&
		streamer.GetPendingChunkCount() == pendingBeforeBadView && streamer.GetLoadedChunkCount() == 2,
		"14.6.4: invalid view coordinates leave the desired set and loaded cache unchanged");

	Check(streamer.UpdateView(asset.OriginX + kTerrainChunkCells * asset.SampleSpacing, asset.OriginZ, &error),
		"14.6.4: moving one chunk updates the streaming window");
	Check(streamer.GetLoadedChunkCount() == 1 && streamer.GetLoadedChunks().count(firstNeighborKey) == 0 &&
		streamer.GetPendingChunkCount() == 9,
		"14.6.4: chunks outside the window are evicted and retained chunks with stale LOD are rebuilt");
	for (int update = 0; update < 8 && streamer.GetPendingChunkCount() != 0; ++update)
		Check(streamer.ProcessPending(&built, &error), "14.6.4: queued chunk meshes build over successive updates");
	const auto& movedMeshes = streamer.GetLoadedChunks();
	const auto movedCenter = movedMeshes.find(centerKey);
	const auto movedInto = movedMeshes.find(std::make_pair<std::int64_t, std::int64_t>(1, 0));
	Check(streamer.GetPendingChunkCount() == 0 && streamer.GetLoadedChunkCount() == 9 &&
		movedCenter != movedMeshes.end() && movedCenter->second.LodLevel == 1 &&
		movedInto != movedMeshes.end() && movedInto->second.LodLevel == 0,
		"14.6.4: streaming converges to a complete window with updated per-chunk LOD");

	TerrainWorldState editedState;
	editedState.Chunks[{ asset.WorldId, 1, 0 }] = MakeEdits(asset, 1, 0);
	Check(streamer.SetWorldState(editedState, &error) && streamer.GetPendingChunkCount() == 1,
		"14.6.4: sparse edit changes invalidate only the affected loaded chunk");
	Check(streamer.ProcessPending(&built, &error) && built == 1,
		"14.6.4: dirty chunk rebuilds without exceeding the per-update budget");
	const auto expectedBase = SampleChunkHeight(asset, 1, 0, 2, 5);
	const auto& editedMeshes = streamer.GetLoadedChunks();
	const auto editedMesh = editedMeshes.find(std::make_pair<std::int64_t, std::int64_t>(1, 0));
	Check(expectedBase && editedMesh != editedMeshes.end() && editedMesh->second.Vertices.size() == 65 * 65 &&
		std::abs(editedMesh->second.Vertices[5 * 65 + 2].Position[1] - (*expectedBase + 1.25)) < 1.0e-9,
		"14.6.4: a streamed chunk rebuild applies its updated sparse height overlay");

	TerrainWorldState invalidState = editedState;
	invalidState.Chunks.begin()->second.BaseFingerprint = "stale";
	const double editedHeight = editedMesh != editedMeshes.end() && editedMesh->second.Vertices.size() == 65 * 65
		? editedMesh->second.Vertices[5 * 65 + 2].Position[1] : std::numeric_limits<double>::quiet_NaN();
	Check(!streamer.SetWorldState(invalidState, &error) && streamer.GetPendingChunkCount() == 0 &&
		streamer.GetLoadedChunks().count(std::make_pair<std::int64_t, std::int64_t>(1, 0)) == 1 &&
		streamer.GetLoadedChunks().at(std::make_pair<std::int64_t, std::int64_t>(1, 0)).Vertices.size() == 65 * 65 &&
		streamer.GetLoadedChunks().at(std::make_pair<std::int64_t, std::int64_t>(1, 0)).Vertices[5 * 65 + 2].Position[1] ==
		editedHeight,
		"14.6.4: invalid sidecar replacement preserves active streamed meshes transactionally");

	TerrainStreamingConfig invalidConfig = config;
	invalidConfig.LoadRadiusChunks = kTerrainMaximumStreamingRadiusChunks + 1;
	Check(!streamer.Configure(asset, editedState, invalidConfig, &error) &&
		streamer.GetPendingChunkCount() == 0 && streamer.GetLoadedChunkCount() == 9,
		"14.6.4: oversized radius is rejected without replacing the active stream");
}

void TestTerrainChunkCollision()
{
	TerrainAsset collisionAsset = MakeAsset();
	collisionAsset.HeightAmplitude = 0.0;
	const auto groundHeight = SampleChunkHeight(collisionAsset, 0, 0, 32, 32);
	std::string error;
	b3WorldDef worldDef = b3DefaultWorldDef();
	worldDef.gravity = { 0.0f, -9.8f, 0.0f };
	const b3WorldId world = b3CreateWorld(&worldDef);
	Check(b3World_IsValid(world), "14.6.5: Box3D test world is created");
	TerrainChunkCollider collider;
	Check(collider.Create(world, collisionAsset, 0, 0, nullptr, &error) && collider.IsValid(),
		"14.6.5: validated terrain chunk creates a static Box3D triangle-mesh collider");
	const b3BodyId originalBody = collider.GetBodyId();
	TerrainChunkEdits staleEdits = MakeEdits(collisionAsset, 0, 0);
	staleEdits.BaseFingerprint = "stale";
	Check(!collider.Create(world, collisionAsset, 0, 0, &staleEdits, &error) &&
		collider.IsValid() && b3Body_IsValid(originalBody),
		"14.6.5: rejected edit replacement preserves the existing collision body");

	TerrainAsset farAsset = collisionAsset;
	farAsset.OriginX = 100000000.0;
	Check(!collider.Create(world, farAsset, 0, 0, nullptr, &error) && collider.IsValid(),
		"14.6.5: single-precision Box3D coordinates are rejected when world spacing collapses");

	if (groundHeight)
	{
		b3BodyDef fallingDef = b3DefaultBodyDef();
		fallingDef.type = b3_dynamicBody;
		fallingDef.position = {
			static_cast<float>(collisionAsset.OriginX + 32.0 * collisionAsset.SampleSpacing),
			static_cast<float>(*groundHeight + 5.0),
			static_cast<float>(collisionAsset.OriginZ + 32.0 * collisionAsset.SampleSpacing)
		};
		fallingDef.motionLocks.angularX = true;
		fallingDef.motionLocks.angularY = true;
		fallingDef.motionLocks.angularZ = true;
		const b3BodyId fallingBody = b3CreateBody(world, &fallingDef);
		const b3BoxHull box = b3MakeBoxHull(0.25f, 0.25f, 0.25f);
		const b3ShapeDef shapeDef = b3DefaultShapeDef();
		const b3ShapeId fallingShape = b3CreateHullShape(fallingBody, &shapeDef, &box.base);
		Check(b3Body_IsValid(fallingBody) && b3Shape_IsValid(fallingShape),
			"14.6.5: dynamic probe body and box shape are valid");
		for (int step = 0; step < 600; ++step)
			b3World_Step(world, 1.0f / 60.0f, 4);
		const b3Pos settled = b3Body_GetPosition(fallingBody);
		const b3Vec3 velocity = b3Body_GetLinearVelocity(fallingBody);
		const int contactCapacity = b3Body_GetContactCapacity(fallingBody);
		std::vector<b3ContactData> contacts(static_cast<std::size_t>(contactCapacity));
		const int contactCount = b3Body_GetContactData(fallingBody, contacts.data(), contactCapacity);
		Check(contactCount > 0 && std::abs(velocity.y) < 0.1f,
			"14.6.5: the resting probe has a Box3D terrain contact and near-zero vertical speed");
		std::ostringstream collisionDetails;
		collisionDetails << "14.6.5: falling Box3D body settles on terrain (y=" << settled.y
			<< ", expected=" << (*groundHeight + 0.25) << ", vy=" << velocity.y
			<< ", contacts=" << contactCount << ")";
		Check(std::abs(static_cast<double>(settled.y) - (*groundHeight + 0.25)) < 0.15,
			collisionDetails.str().c_str());
	}

	b3BodyId disposedBody{};
	{
		TerrainChunkCollider scopedCollider;
		Check(scopedCollider.Create(world, collisionAsset, 1, 0, nullptr, &error),
			"14.6.5: scoped terrain collider is created for RAII teardown");
		disposedBody = scopedCollider.GetBodyId();
	}
	Check(!b3Body_IsValid(disposedBody), "14.6.5: collider destructor releases its Box3D body");

	b3DestroyWorld(world);
	Check(!collider.IsValid(), "14.6.5: collider detects Box3D world teardown");
	collider.Destroy();
}

void TestTerrainRuntimeStreamingAndCollisionOwnership()
{
	TerrainAsset asset = MakeAsset();
	asset.HeightAmplitude = 0.0;
	TerrainWorldState worldState;
	TerrainStreamingConfig config;
	config.LoadRadiusChunks = 0;
	config.MaxChunkBuildsPerUpdate = 2;
	TerrainRuntime runtime;
	std::string error;
	Check(runtime.Configure(asset, worldState, config, &error),
		"14.6.6: runtime owns a configured CPU terrain streamer");
	Check(runtime.GetAsset().WorldId == asset.WorldId && runtime.GetWorldState().Chunks.empty() &&
		runtime.GetConfig().LoadRadiusChunks == 0,
		"14.6.6: runtime retains authored source, edits, and settings for scene copying");
	b3WorldDef worldDef = b3DefaultWorldDef();
	worldDef.gravity = { 0.0f, -9.8f, 0.0f };
	const b3WorldId world = b3CreateWorld(&worldDef);
	runtime.SetPhysicsWorld(world);
	Check(runtime.UpdateView(asset.OriginX + 32.0, asset.OriginZ + 32.0, &error) &&
		runtime.GetStreamer().GetLoadedChunkCount() == 1 && runtime.GetColliderCount() == 1,
		"14.6.6: one budgeted terrain chunk creates one owned Box3D collider");
	const b3BodyId firstBody = runtime.GetColliderBodyId({ 0, 0 });
	Check(b3Body_IsValid(firstBody), "14.6.6: streamed terrain collider body is valid");

	Check(runtime.UpdateView(asset.OriginX + 96.0, asset.OriginZ + 32.0, &error) &&
		runtime.GetStreamer().GetLoadedChunkCount() == 1 && runtime.GetColliderCount() == 1 &&
		!b3Body_IsValid(firstBody) && b3Body_IsValid(runtime.GetColliderBodyId({ 1, 0 })),
		"14.6.6: moving the signed chunk window evicts stale collision and loads the new chunk");

	worldState.Chunks[{ asset.WorldId, 1, 0 }] = MakeEdits(asset, 1, 0);
	Check(runtime.SetWorldState(worldState, &error) && runtime.GetColliderCount() == 0,
		"14.6.6: edit changes invalidate the matching owned collider");
	Check(runtime.ProcessPending(nullptr, &error) && runtime.GetColliderCount() == 1 &&
		b3Body_IsValid(runtime.GetColliderBodyId({ 1, 0 })),
		"14.6.6: processing the changed chunk restores its updated Box3D collider");

	runtime.SetPhysicsWorld(B3_NULL_ID);
	Check(runtime.GetColliderCount() == 0, "14.6.6: detaching the physics world releases owned colliders");
	b3DestroyWorld(world);
}

bool SameStreamingConfig(const TerrainStreamingConfig& left, const TerrainStreamingConfig& right)
{
	return left.LoadRadiusChunks == right.LoadRadiusChunks &&
		left.LodDistanceThresholds == right.LodDistanceThresholds &&
		left.MaxChunkBuildsPerUpdate == right.MaxChunkBuildsPerUpdate;
}

bool SameTerrainScenePayload(const TerrainScenePayload& left, const TerrainScenePayload& right)
{
	return left.Metadata.AssetReference == right.Metadata.AssetReference &&
		SameStreamingConfig(left.Metadata.Streaming, right.Metadata.Streaming) &&
		SerializeTerrainAsset(left.Asset) == SerializeTerrainAsset(right.Asset) &&
		SameWorldState(left.WorldState, right.WorldState);
}

void TestTerrainScenePayloadLoad(const std::filesystem::path& root)
{
	const std::filesystem::path fixtureRoot = root / "scene-payload";
	const std::filesystem::path assetRoot = fixtureRoot / "Assets";
	const std::filesystem::path assetPath = assetRoot / "biomes" / "highlands.wlterrain";
	const std::filesystem::path sidecarRoot = fixtureRoot / "sidecars";
	std::filesystem::create_directories(assetPath.parent_path());

	const TerrainAsset authoredAsset = MakeAsset();
	std::string error;
	Check(SaveTerrainAsset(assetPath, authoredAsset, &error),
		"14.6.8: valid authored terrain asset fixture is written before payload loading");

	TerrainSceneMetadata authoredMetadata;
	Check(MakeTerrainAssetReference(assetRoot, std::filesystem::absolute(assetPath),
		authoredMetadata.AssetReference, &error),
		"14.6.8: payload fixture receives its confined canonical asset reference");
	authoredMetadata.Streaming.LoadRadiusChunks = 5;
	authoredMetadata.Streaming.LodDistanceThresholds = { 2, 4, 7, 11, 16, 22 };
	authoredMetadata.Streaming.MaxChunkBuildsPerUpdate = 3;
	std::string metadataYaml;
	Check(EncodeTerrainSceneMetadataYaml(authoredMetadata, metadataYaml, &error),
		"14.6.8: full scene streaming policy encodes for payload loading");
	const YAML::Node encodedMetadata = YAML::Load(metadataYaml);
	const YAML::Node encodedStreaming = encodedMetadata["Streaming"];
	Check(encodedMetadata.IsMap() && encodedMetadata.size() == 2 && encodedStreaming.IsMap() &&
		encodedStreaming.size() == 3 && !encodedMetadata["Edits"] && !encodedMetadata["Meshes"] &&
		!encodedMetadata["LOD"] && !encodedMetadata["LodCache"] && !encodedMetadata["Colliders"] &&
		!encodedStreaming["Edits"] && !encodedStreaming["Meshes"] && !encodedStreaming["LOD"] &&
		!encodedStreaming["LodCache"] && !encodedStreaming["Colliders"],
		"14.6.8: scene metadata contains no edits, meshes, LOD or collider runtime data");

	TerrainWorldState authoredState;
	authoredState.Chunks[{ authoredAsset.WorldId, -2, 9 }] = MakeEdits(authoredAsset, -2, 9);
	authoredState.Chunks[{ authoredAsset.WorldId, 4, -5 }] = MakeEdits(authoredAsset, 4, -5);
	Check(SaveTerrainChunkEditsForWorld(sidecarRoot, authoredAsset, authoredState, &error),
		"14.6.8: two sparse chunk sidecars are written separately from authored metadata");

	TerrainScenePayload loaded;
	Check(LoadTerrainScenePayload(assetRoot, sidecarRoot, metadataYaml, loaded, &error),
		"14.6.8: authored asset, full scene metadata and complete sidecar collection load");
	Check(loaded.Metadata.AssetReference == authoredMetadata.AssetReference &&
		SameStreamingConfig(loaded.Metadata.Streaming, authoredMetadata.Streaming) &&
		SerializeTerrainAsset(loaded.Asset) == SerializeTerrainAsset(authoredAsset) &&
		loaded.WorldState.Chunks.size() == 2 &&
		SameWorldState(loaded.WorldState, authoredState),
		"14.6.8: loaded payload round-trips authored data and exactly two sparse edits");

	TerrainScenePayload sentinel;
	sentinel.Metadata.AssetReference = "assets/sentinel/keep.wlterrain";
	sentinel.Metadata.Streaming.LoadRadiusChunks = 3;
	sentinel.Metadata.Streaming.LodDistanceThresholds = { 1, 5, 9, 13, 17, 21 };
	sentinel.Metadata.Streaming.MaxChunkBuildsPerUpdate = 6;
	sentinel.Asset = MakeAsset();
	sentinel.Asset.WorldId = "world.sentinel";
	sentinel.WorldState.Chunks[{ sentinel.Asset.WorldId, 77, -88 }] =
		MakeEdits(sentinel.Asset, 77, -88);
	const TerrainScenePayload sentinelBefore = sentinel;

	TerrainSceneMetadata missingMetadata = authoredMetadata;
	missingMetadata.AssetReference = "assets/biomes/missing.wlterrain";
	std::string missingYaml;
	Check(EncodeTerrainSceneMetadataYaml(missingMetadata, missingYaml, &error),
		"14.6.8: missing-asset metadata remains structurally valid");
	Check(!LoadTerrainScenePayload(assetRoot, sidecarRoot, missingYaml, sentinel, &error) &&
		SameTerrainScenePayload(sentinel, sentinelBefore),
		"14.6.8: missing confined asset fails and preserves the complete payload sentinel");

	Check(!LoadTerrainScenePayload(assetRoot, sidecarRoot,
		"AssetRef: assets/../outside.wlterrain\n"
		"Streaming: {LoadRadiusChunks: 1, LodDistanceThresholds: [1,2,3,4,5,6], MaxChunkBuildsPerUpdate: 1}\n",
		sentinel, &error) && SameTerrainScenePayload(sentinel, sentinelBefore),
		"14.6.8: invalid traversal metadata fails without changing any payload field");

	std::filesystem::path sidecarPath;
	Check(ResolveTerrainChunkEditsPath(sidecarRoot, authoredAsset, -2, 9, sidecarPath, &error),
		"14.6.8: first payload sidecar path resolves before corruption check");
	if (!sidecarPath.empty())
	{
		const std::string validSidecar = ReadText(sidecarPath);
		std::string corruptSidecar = validSidecar;
		const std::size_t valuePosition = corruptSidecar.find("1.25");
		if (valuePosition != std::string::npos)
			corruptSidecar[valuePosition] = '9';
		WriteText(sidecarPath, corruptSidecar);
		Check(valuePosition != std::string::npos &&
			!LoadTerrainScenePayload(assetRoot, sidecarRoot, metadataYaml, sentinel, &error) &&
			SameTerrainScenePayload(sentinel, sentinelBefore),
			"14.6.8: corrupt sidecar fails and preserves the complete payload sentinel");
		WriteText(sidecarPath, validSidecar);
	}

	const std::filesystem::path missingSidecarRoot = fixtureRoot / "no-sidecars";
	TerrainScenePayload noEdits;
	Check(LoadTerrainScenePayload(assetRoot, missingSidecarRoot, metadataYaml, noEdits, &error) &&
		noEdits.WorldState.Chunks.empty() &&
		SerializeTerrainAsset(noEdits.Asset) == SerializeTerrainAsset(authoredAsset),
		"14.6.8: a missing sidecar directory is a successful load with empty world state");
}

void TestTerrainSceneMetadataYaml()
{
	TerrainSceneMetadata authored;
	authored.AssetReference = "assets/biomes/highlands.wlterrain";
	authored.Streaming.LoadRadiusChunks = 7;
	authored.Streaming.LodDistanceThresholds = { 3, 7, 11, 15, 19, 23 };
	authored.Streaming.MaxChunkBuildsPerUpdate = 9;
	std::string yamlText;
	std::string error;
	Check(EncodeTerrainSceneMetadataYaml(authored, yamlText, &error),
		"14.6.7: valid scene terrain metadata encodes as YAML");

	const YAML::Node encoded = YAML::Load(yamlText);
	const YAML::Node encodedStreaming = encoded["Streaming"];
	Check(encoded.IsMap() && encoded.size() == 2 && encoded["AssetRef"].as<std::string>() == authored.AssetReference &&
		encodedStreaming.IsMap() && encodedStreaming.size() == 3 &&
		encodedStreaming["LoadRadiusChunks"].as<std::uint32_t>() == authored.Streaming.LoadRadiusChunks &&
		encodedStreaming["MaxChunkBuildsPerUpdate"].as<std::uint32_t>() == authored.Streaming.MaxChunkBuildsPerUpdate &&
		encodedStreaming["LodDistanceThresholds"].IsSequence() &&
		encodedStreaming["LodDistanceThresholds"].size() == authored.Streaming.LodDistanceThresholds.size(),
		"14.6.7: YAML contains only canonical AssetRef and complete Streaming fields");
	Check(!encoded["Edits"] && !encoded["Meshes"] && !encoded["LodCache"] && !encoded["Colliders"] &&
		!encodedStreaming["Edits"] && !encodedStreaming["Meshes"] &&
		!encodedStreaming["LodCache"] && !encodedStreaming["Colliders"],
		"14.6.7: serialized metadata excludes edits, meshes, LOD caches and colliders");
	bool thresholdsRoundTrip = encodedStreaming["LodDistanceThresholds"].size() == 6;
	for (std::size_t index = 0; thresholdsRoundTrip && index < authored.Streaming.LodDistanceThresholds.size(); ++index)
		thresholdsRoundTrip = encodedStreaming["LodDistanceThresholds"][index].as<std::uint32_t>() ==
			authored.Streaming.LodDistanceThresholds[index];
	Check(thresholdsRoundTrip, "14.6.7: YAML preserves all six authored LOD thresholds");

	TerrainSceneMetadata decoded;
	Check(DecodeTerrainSceneMetadataYaml(yamlText, decoded, &error) &&
		decoded.AssetReference == authored.AssetReference && SameStreamingConfig(decoded.Streaming, authored.Streaming),
		"14.6.7: scene terrain metadata YAML round-trips every streaming field");

	TerrainSceneMetadata unchanged;
	unchanged.AssetReference = "assets/keep.wlterrain";
	unchanged.Streaming.LoadRadiusChunks = 3;
	unchanged.Streaming.LodDistanceThresholds = { 2, 5, 8, 11, 14, 17 };
	unchanged.Streaming.MaxChunkBuildsPerUpdate = 6;
	const TerrainSceneMetadata before = unchanged;
	const std::string invalidConfig =
		"AssetRef: assets/invalid.wlterrain\n"
		"Streaming:\n"
		"  LoadRadiusChunks: 9\n"
		"  LodDistanceThresholds: [1, 2, 3, 4, 5, 6]\n"
		"  MaxChunkBuildsPerUpdate: 2\n";
	Check(!DecodeTerrainSceneMetadataYaml(invalidConfig, unchanged, &error) &&
		unchanged.AssetReference == before.AssetReference && SameStreamingConfig(unchanged.Streaming, before.Streaming),
		"14.6.7: invalid streaming config is rejected transactionally");
	Check(!DecodeTerrainSceneMetadataYaml("AssetRef: [bad\n", unchanged, &error) &&
		unchanged.AssetReference == before.AssetReference && SameStreamingConfig(unchanged.Streaming, before.Streaming),
		"14.6.7: malformed YAML conversion is caught without changing output");
	Check(!DecodeTerrainSceneMetadataYaml(
		"AssetRef: assets/a.wlterrain\nStreaming: {LoadRadiusChunks: 1, LodDistanceThresholds: [1,2,3,4,5,6]}\n",
		unchanged, &error) && unchanged.AssetReference == before.AssetReference &&
		SameStreamingConfig(unchanged.Streaming, before.Streaming),
		"14.6.7: incomplete Streaming mappings are rejected transactionally");
	TerrainSceneMetadata noncanonical = authored;
	noncanonical.AssetReference = "assets/../outside.wlterrain";
	std::string encodedBeforeFailure = "keep-yaml";
	Check(!EncodeTerrainSceneMetadataYaml(noncanonical, encodedBeforeFailure, &error) &&
		encodedBeforeFailure == "keep-yaml",
		"14.6.7: YAML encoding rejects noncanonical AssetRef values transactionally");
	Check(!DecodeTerrainSceneMetadataYaml(
		"AssetRef: assets/../outside.wlterrain\n"
		"Streaming: {LoadRadiusChunks: 1, LodDistanceThresholds: [1,2,3,4,5,6], MaxChunkBuildsPerUpdate: 1}\n",
		unchanged, &error) && unchanged.AssetReference == before.AssetReference &&
		SameStreamingConfig(unchanged.Streaming, before.Streaming),
		"14.6.7: YAML decoding rejects noncanonical AssetRef values transactionally");
}

void TestTerrainAssetReferenceConfinement(const std::filesystem::path& root)
{
	const auto assetRoot = root / "Assets";
	const auto terrainDirectory = assetRoot / "biomes";
	const auto outsideDirectory = root / "outside";
	std::filesystem::create_directories(terrainDirectory);
	std::filesystem::create_directories(outsideDirectory);
	const auto terrainFile = terrainDirectory / "highlands.wlterrain";
	const auto outsideFile = outsideDirectory / "outside.wlterrain";
	WriteText(terrainFile, "terrain asset");
	WriteText(outsideFile, "outside terrain asset");

	std::string reference = "unchanged";
	std::string error;
	Check(MakeTerrainAssetReference(assetRoot, std::filesystem::absolute(terrainFile), reference, &error) &&
		reference == "assets/biomes/highlands.wlterrain",
		"14.6.7: asset path becomes the canonical project-relative forward-slashed reference");
	std::filesystem::path resolved = "unchanged/path";
	Check(ResolveTerrainAssetReference(assetRoot, reference, resolved, &error) &&
		resolved == std::filesystem::canonical(terrainFile),
		"14.6.7: canonical terrain asset reference resolves to its existing file");

	const std::string referenceBeforeFailure = "keep-reference";
	reference = referenceBeforeFailure;
	Check(!MakeTerrainAssetReference(assetRoot, std::filesystem::absolute(outsideFile), reference, &error) &&
		reference == referenceBeforeFailure,
		"14.6.7: asset reference creation rejects files outside Assets without changing output");
	const auto directoryTarget = terrainDirectory / "folder.wlterrain";
	std::filesystem::create_directories(directoryTarget);
	Check(!MakeTerrainAssetReference(assetRoot, std::filesystem::absolute(directoryTarget), reference, &error) &&
		reference == referenceBeforeFailure,
		"14.6.7: asset reference creation rejects directories and preserves output");

	const std::filesystem::path pathBeforeFailure = resolved;
	const std::string absoluteReference = std::filesystem::absolute(terrainFile).generic_string();
	const std::array<std::string, 11> invalidReferences = {
		"", "../outside/outside.wlterrain", "assets/../outside/outside.wlterrain",
		"assets/biomes/../../outside/outside.wlterrain", "assets\\biomes\\highlands.wlterrain",
		"/assets/biomes/highlands.wlterrain", "Assets/biomes/highlands.wlterrain",
		"assets/biomes/missing.wlterrain", "assets/biomes/folder.wlterrain/",
		"assets/biomes/highlands.txt", absoluteReference
	};
	for (const std::string& invalid : invalidReferences)
	{
		resolved = pathBeforeFailure;
		Check(!ResolveTerrainAssetReference(assetRoot, invalid, resolved, &error) && resolved == pathBeforeFailure,
			"14.6.7: noncanonical, rooted, traversing, missing or wrong-extension refs are rejected transactionally");
	}
	resolved = pathBeforeFailure;
	Check(!ResolveTerrainAssetReference(assetRoot, "assets//biomes/highlands.wlterrain", resolved, &error) &&
		resolved == pathBeforeFailure,
		"14.6.7: empty path components are not accepted as canonical references");

	std::error_code symlinkError;
	const auto escapeLink = assetRoot / "escape";
	std::filesystem::create_directory_symlink(outsideDirectory, escapeLink, symlinkError);
	if (!symlinkError)
	{
		resolved = pathBeforeFailure;
		Check(!ResolveTerrainAssetReference(assetRoot, "assets/escape/outside.wlterrain", resolved, &error) &&
			resolved == pathBeforeFailure,
			"14.6.7: resolution rejects symlink escapes from the Assets root");
		reference = referenceBeforeFailure;
		Check(!MakeTerrainAssetReference(assetRoot, std::filesystem::absolute(escapeLink / "outside.wlterrain"),
			reference, &error) && reference == referenceBeforeFailure,
			"14.6.7: reference creation rejects symlink escapes without changing output");
	}
}

void TestTerrainRuntimeSharesBudgetAndPrioritizesColliders()
{
	TerrainAsset asset = MakeAsset();
	asset.HeightAmplitude = 0.0;
	TerrainStreamingConfig config;
	config.LoadRadiusChunks = 1;
	config.MaxChunkBuildsPerUpdate = 2;
	TerrainRuntime runtime;
	std::string error;
	Check(runtime.Configure(asset, {}, config, &error),
		"14.6.6: multi-chunk runtime configures for shared budget checks");
	b3WorldDef worldDef = b3DefaultWorldDef();
	worldDef.gravity = { 0.0f, -9.8f, 0.0f };
	const b3WorldId world = b3CreateWorld(&worldDef);
	runtime.SetPhysicsWorld(world);
	Check(runtime.UpdateView(asset.OriginX + 32.0, asset.OriginZ + 32.0, &error),
		"14.6.6: runtime selects a radius-one window");
	Check(runtime.GetStreamer().GetLoadedChunkCount() + runtime.GetColliderCount() <=
		config.MaxChunkBuildsPerUpdate,
		"14.6.6: initial mesh and collider commits share one per-update budget");

	for (int update = 0; update < 24 &&
		(runtime.GetStreamer().GetPendingChunkCount() != 0 ||
		 runtime.GetColliderCount() < runtime.GetStreamer().GetLoadedChunkCount()); ++update)
	{
		const std::size_t collidersBefore = runtime.GetColliderCount();
		std::uint32_t meshBuilds = 0;
		Check(runtime.ProcessPending(&meshBuilds, &error),
			"14.6.6: runtime advances bounded mesh and collider work");
		const std::size_t collidersAdded = runtime.GetColliderCount() - collidersBefore;
		Check(meshBuilds + collidersAdded <= config.MaxChunkBuildsPerUpdate,
			"14.6.6: combined mesh and collider commits never exceed the shared budget");
		if (update == 0)
			Check(runtime.GetColliderCount() > 0 && runtime.GetStreamer().GetPendingChunkCount() > 0,
				"14.6.6: nearest collision comes online while farther mesh chunks remain queued");
	}
	Check(runtime.GetStreamer().GetPendingChunkCount() == 0 && runtime.GetColliderCount() == 9,
		"14.6.6: the full radius-one window eventually receives colliders");

	runtime.SetPhysicsWorld(B3_NULL_ID);
	runtime.SetPhysicsWorld(world);
	std::uint32_t rebuilt = 0;
	Check(runtime.ProcessPending(&rebuilt, &error) && rebuilt == 0 &&
		b3Body_IsValid(runtime.GetColliderBodyId({ 0, 0 })) && runtime.GetColliderCount() == 2,
		"14.6.6: after physics-world attachment the nearest collider builds before map-ordered distant chunks");
	runtime.SetPhysicsWorld(B3_NULL_ID);
	b3DestroyWorld(world);
}
}

int main()
{
	const auto root = TestDirectory();
	TestTerrainSceneMetadataYaml();
	TestTerrainScenePayloadLoad(root);
	TestTerrainAssetReferenceConfinement(root);
	TestDefaultTerrainAssetAuthoring(root);
	TestAssetValidationAndLoading(root);
	TestAuthoredAssetSaving(root);
	TestDeterministicNoiseAndChunkIndexing();
	TestSplatWeightsAndSharedChunkBorders();
	TestCpuChunkMeshLodAndEdits();
	TestBudgetedTerrainChunkStreaming();
	TestTerrainChunkCollision();
	TestTerrainRuntimeStreamingAndCollisionOwnership();
	TestTerrainRuntimeSharesBudgetAndPrioritizesColliders();
	TestSparseSidecarRoundTripAndDirtyFailure(root);
	TestSidecarRejectionIsTransactional(root);
	TestDeterministicSidecarPathResolution(root);
	TestMaximumWorldIdSidecarRoundTrip();
	TestWorldSidecarCollectionRoundTripAndIsolation(root);
	TestWorldSidecarCollectionIsTransactional(root);
	TestMaximumWorldIdCollectionDiscovery(root);
	TestNearMaximumWindowsPathSidecarEnumeration();
	TestNearMaximumWindowsPathTemporarySuffixRoundTrip();
	std::error_code cleanupError;
	Check(RemoveTestDirectory(root, cleanupError),
		"terrain test root cleanup removes its directory without error");
	if (s_Failures != 0)
		return 1;
	std::cout << "All terrain CPU tests passed.\n";
	return 0;
}
