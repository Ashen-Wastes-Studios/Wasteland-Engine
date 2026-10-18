#pragma once

#include "Wasteland/Terrain/TerrainCollision.h"

#include <map>
#include <memory>
#include <filesystem>
#include <string>

namespace Wasteland::Terrain
{
// Scene-owned bridge between the bounded CPU streamer and Box3D. Call update
// and world-state methods on the scene thread, between Box3D steps.
class TerrainRuntime
{
public:
	bool Configure(const TerrainAsset& asset, const TerrainWorldState& worldState,
		const TerrainStreamingConfig& config, std::string* error = nullptr);
	bool SetWorldState(const TerrainWorldState& worldState, std::string* error = nullptr);
	void SetPhysicsWorld(b3WorldId worldId);
	bool UpdateView(double worldX, double worldZ, std::string* error = nullptr);
	bool ProcessPending(std::uint32_t* outBuilt = nullptr, std::string* error = nullptr);
	bool SaveWorldState(const std::filesystem::path& projectRoot, std::string* error = nullptr);
	void Clear() noexcept;

	bool IsConfigured() const noexcept { return m_Configured; }
	const TerrainAsset& GetAsset() const noexcept { return m_Asset; }
	const TerrainWorldState& GetWorldState() const noexcept { return m_WorldState; }
	const TerrainStreamingConfig& GetConfig() const noexcept { return m_Config; }
	const TerrainChunkStreamer& GetStreamer() const noexcept { return m_Streamer; }
	std::size_t GetColliderCount() const noexcept { return m_Colliders.size(); }
	b3BodyId GetColliderBodyId(TerrainChunkStreamer::ChunkKey key) const noexcept;

private:
	const TerrainChunkEdits* FindEdits(const TerrainWorldState& state,
		TerrainChunkStreamer::ChunkKey key) const noexcept;

	bool m_Configured = false;
	TerrainAsset m_Asset;
	TerrainWorldState m_WorldState;
	TerrainStreamingConfig m_Config;
	TerrainChunkStreamer m_Streamer;
	b3WorldId m_PhysicsWorld = B3_NULL_ID;
	std::map<TerrainChunkStreamer::ChunkKey, std::unique_ptr<TerrainChunkCollider>> m_Colliders;
};
}
