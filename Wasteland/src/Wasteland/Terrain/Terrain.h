#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Wasteland::Terrain
{
inline constexpr std::uint32_t kTerrainSchemaVersion = 1;
inline constexpr std::uint32_t kTerrainStateSchemaVersion = 1;
inline constexpr std::int32_t kTerrainChunkCells = 64;
inline constexpr std::int32_t kTerrainChunkSamples = kTerrainChunkCells + 1;
// Bound authored and runtime-state text input before allocating parser buffers.
inline constexpr std::size_t kMaximumTerrainAssetBytes = 1024 * 1024;
inline constexpr std::size_t kMaximumTerrainStateBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kMaximumTerrainStableIdBytes = 256;
inline constexpr std::uint32_t kTerrainMaximumStreamingRadiusChunks = 8;

struct SplatRule
{
	std::string MaterialAssetId;
	double MinimumHeight = 0.0;
	double MaximumHeight = 0.0;
	double MinimumSlopeDegrees = 0.0;
	double MaximumSlopeDegrees = 90.0;
	double Weight = 1.0;
};

// Authored data only. Runtime samples and edits are held separately and never
// written back to a .wlterrain source asset.
struct TerrainAsset
{
	std::uint32_t SchemaVersion = kTerrainSchemaVersion;
	std::string WorldId;
	std::string GeneratorId;
	std::uint32_t GeneratorVersion = 1;
	std::uint64_t Seed = 0;
	double OriginX = 0.0;
	double OriginY = 0.0;
	double OriginZ = 0.0;
	double SampleSpacing = 1.0;
	std::uint32_t ChunkResolution = kTerrainChunkCells;
	double BaseHeight = 0.0;
	double HeightAmplitude = 1.0;
	double NoiseFrequency = 0.05;
	std::uint32_t NoiseOctaves = 4;
	double Lacunarity = 2.0;
	double Persistence = 0.5;
	std::vector<SplatRule> SplatRules;
};

struct ChunkCoordinate
{
	std::int64_t Chunk = 0;
	std::int32_t Local = 0;
};

struct TerrainChunkEdits
{
	std::string WorldId;
	std::int64_t ChunkX = 0;
	std::int64_t ChunkZ = 0;
	std::string BaseFingerprint;
	std::uint32_t TerrainSchemaVersion = kTerrainSchemaVersion;
	// Height deltas include boundary samples: local coordinates are [0, 64].
	std::map<std::pair<std::int32_t, std::int32_t>, double> HeightDeltas;
	// Material overrides address base cells, not vertices: [0, 63].
	std::map<std::pair<std::int32_t, std::int32_t>, std::string> MaterialOverrides;
	bool Dirty = false;
};

using TerrainChunkKey = std::tuple<std::string, std::int64_t, std::int64_t>;

struct TerrainWorldState
{
	std::map<TerrainChunkKey, TerrainChunkEdits> Chunks;
};

// Derived CPU geometry. Positions stay double-precision in world space so a
// later renderer can rebase them before converting to GPU float vertices.
// Meshes and indices are transient caches and are never serialized.
struct TerrainMeshVertex
{
	std::array<double, 3> Position{};
	std::array<float, 3> Normal{};
};

struct TerrainChunkMesh
{
	std::vector<TerrainMeshVertex> Vertices;
	std::vector<std::uint32_t> Indices;
	std::uint32_t LodLevel = 0;
	std::uint32_t CellsPerSide = 0;
};

struct TerrainStreamingConfig
{
	std::uint32_t LoadRadiusChunks = 4;
	std::array<std::uint32_t, 6> LodDistanceThresholds{ 2, 4, 6, 8, 10, 12 };
	std::uint32_t MaxChunkBuildsPerUpdate = 4;
};

// Bounded CPU-side chunk mesh cache. UpdateView selects a signed chunk window
// and ProcessPending builds only a fixed budget per call; renderer, Scene and
// collision ownership are deliberately left to the host.
class TerrainChunkStreamer
{
public:
	using ChunkKey = std::pair<std::int64_t, std::int64_t>;

	bool Configure(const TerrainAsset& asset, const TerrainWorldState& worldState,
		const TerrainStreamingConfig& config, std::string* error = nullptr);
	bool SetWorldState(const TerrainWorldState& worldState, std::string* error = nullptr);
	bool UpdateView(double worldX, double worldZ, std::string* error = nullptr);
	bool ProcessPending(std::uint32_t* outBuilt = nullptr, std::string* error = nullptr);
	bool ProcessPending(std::uint32_t maxBuildsPerCall, std::uint32_t* outBuilt,
		std::string* error = nullptr);
	void Reset() noexcept;

	bool IsConfigured() const noexcept { return m_Configured; }
	std::size_t GetLoadedChunkCount() const noexcept { return m_LoadedChunks.size(); }
	std::size_t GetPendingChunkCount() const noexcept;
	const std::map<ChunkKey, TerrainChunkMesh>& GetLoadedChunks() const noexcept { return m_LoadedChunks; }
	ChunkKey GetViewCenterChunk() const noexcept { return { m_CenterChunkX, m_CenterChunkZ }; }

private:
	bool m_Configured = false;
	bool m_HasView = false;
	TerrainAsset m_Asset;
	TerrainWorldState m_WorldState;
	TerrainStreamingConfig m_Config;
	std::int64_t m_CenterChunkX = 0;
	std::int64_t m_CenterChunkZ = 0;
	std::map<ChunkKey, std::uint32_t> m_DesiredLods;
	std::map<ChunkKey, TerrainChunkMesh> m_LoadedChunks;
	std::map<ChunkKey, TerrainChunkEdits> m_AppliedEdits;
	std::set<ChunkKey> m_InvalidatedChunks;
};

bool ValidateTerrainAsset(const TerrainAsset& asset, std::string* error = nullptr);
// Build the editor's default procedural profile with a caller-supplied stable
// world identity. The output remains unchanged when the identity is invalid.
bool CreateDefaultTerrainAsset(const std::string& worldId, TerrainAsset& outAsset,
	std::string* error = nullptr);
bool ValidateTerrainStreamingConfig(const TerrainStreamingConfig& config, std::string* error = nullptr);
std::string SerializeTerrainAsset(const TerrainAsset& asset);
bool ParseTerrainAsset(const std::string& text, TerrainAsset& outAsset, std::string* error = nullptr);
bool SaveTerrainAsset(const std::filesystem::path& path, const TerrainAsset& asset, std::string* error = nullptr);
bool LoadTerrainAsset(const std::filesystem::path& path, TerrainAsset& outAsset, std::string* error = nullptr);
std::string TerrainAssetFingerprint(const TerrainAsset& asset);

// All sample identities are signed global integer lattice coordinates. World
// positions convert with mathematical floor, and chunk lookup uses floor division.
std::optional<std::int64_t> WorldToSampleLattice(double worldCoordinate, double origin, double spacing) noexcept;
ChunkCoordinate ChunkCoordinateForLattice(std::int64_t latticeCoordinate) noexcept;
std::optional<double> SampleBaseHeight(const TerrainAsset& asset, std::int64_t globalLatticeX, std::int64_t globalLatticeZ) noexcept;
std::optional<double> SampleChunkHeight(const TerrainAsset& asset, std::int64_t chunkX, std::int64_t chunkZ,
	std::int32_t localSampleX, std::int32_t localSampleZ) noexcept;
// Generate one 64-cell chunk at a discrete LOD (0..6). Runtime edits are
// validated against the asset and chunk identity before any output is changed.
// On failure outMesh is left untouched; no derived mesh data is persisted.
bool BuildTerrainChunkMesh(const TerrainAsset& asset, std::int64_t chunkX, std::int64_t chunkZ,
	std::uint32_t lodLevel, TerrainChunkMesh& outMesh,
	const TerrainChunkEdits* edits = nullptr, std::string* error = nullptr);
std::vector<double> EvaluateSplatWeights(const TerrainAsset& asset, double height, double slopeDegrees);

// Sidecars are caller-placed world-state files, separate from authored assets
// and scene/save serialization. Load is transactional: live state changes only
// after the complete schema, identity, fingerprint, bounds and checksum validate.
// A valid load refuses to replace an already-dirty active chunk.
// The resolver encodes the complete stable world ID and both signed chunk
// coordinates injectively beneath the caller-owned sidecar root.
bool ResolveTerrainChunkEditsPath(const std::filesystem::path& root, const TerrainAsset& asset,
	std::int64_t chunkX, std::int64_t chunkZ, std::filesystem::path& outPath,
	std::string* error = nullptr);
bool SaveTerrainChunkEdits(const std::filesystem::path& path, const TerrainAsset& asset,
	TerrainChunkEdits& edits, std::string* error = nullptr);
bool LoadTerrainChunkEdits(const std::filesystem::path& path, const TerrainAsset& asset,
	std::int64_t expectedChunkX, std::int64_t expectedChunkZ, TerrainWorldState& activeState,
	std::string* error = nullptr);
// World-level collection operations use the resolver's encoded WorldId directory.
// Save prevalidates the state, writes only dirty entries for asset.WorldId, and
// clears each entry's Dirty flag only after that chunk's atomic replacement succeeds.
// Load merges every canonical sidecar transactionally; a missing directory is a
// successful no-op, and no existing dirty chunk may be replaced.
bool SaveTerrainChunkEditsForWorld(const std::filesystem::path& root, const TerrainAsset& asset,
	TerrainWorldState& worldState, std::string* error = nullptr);
bool LoadTerrainChunkEditsForWorld(const std::filesystem::path& root, const TerrainAsset& asset,
	TerrainWorldState& activeState, std::string* error = nullptr);
}
