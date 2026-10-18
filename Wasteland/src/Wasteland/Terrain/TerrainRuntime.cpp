#include "wlpch.h"

#include "Wasteland/Terrain/TerrainRuntime.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace Wasteland::Terrain
{
namespace
{
void SetRuntimeError(std::string* error, const char* message)
{
	if (error)
		*error = message;
}

bool SameEdits(const TerrainChunkEdits* left, const TerrainChunkEdits* right)
{
	if (!left || !right)
		return left == right;
	return left->WorldId == right->WorldId && left->ChunkX == right->ChunkX && left->ChunkZ == right->ChunkZ &&
		left->BaseFingerprint == right->BaseFingerprint &&
		left->TerrainSchemaVersion == right->TerrainSchemaVersion &&
		left->HeightDeltas == right->HeightDeltas && left->MaterialOverrides == right->MaterialOverrides;
}
}

bool TerrainRuntime::Configure(const TerrainAsset& asset, const TerrainWorldState& worldState,
	const TerrainStreamingConfig& config, std::string* error)
{
	if (error)
		error->clear();
	TerrainChunkStreamer generatedStreamer;
	if (!generatedStreamer.Configure(asset, worldState, config, error))
		return false;

	m_Colliders.clear();
	m_Asset = asset;
	m_WorldState = worldState;
	m_Config = config;
	m_Streamer = std::move(generatedStreamer);
	m_Configured = true;
	return true;
}

bool TerrainRuntime::SetWorldState(const TerrainWorldState& worldState, std::string* error)
{
	if (error)
		error->clear();
	if (!m_Configured)
	{
		SetRuntimeError(error, "terrain runtime is not configured");
		return false;
	}
	if (!m_Streamer.SetWorldState(worldState, error))
		return false;

	for (auto it = m_Colliders.begin(); it != m_Colliders.end();)
	{
		const TerrainChunkEdits* oldEdits = FindEdits(m_WorldState, it->first);
		const TerrainChunkEdits* newEdits = FindEdits(worldState, it->first);
		if (!SameEdits(oldEdits, newEdits))
			it = m_Colliders.erase(it);
		else
			++it;
	}
	m_WorldState = worldState;
	return true;
}

void TerrainRuntime::SetPhysicsWorld(b3WorldId worldId)
{
	m_Colliders.clear();
	m_PhysicsWorld = worldId;
	if (!b3World_IsValid(m_PhysicsWorld))
		m_PhysicsWorld = {};
}

bool TerrainRuntime::UpdateView(double worldX, double worldZ, std::string* error)
{
	if (error)
		error->clear();
	if (!m_Configured)
	{
		SetRuntimeError(error, "terrain runtime is not configured");
		return false;
	}
	if (!m_Streamer.UpdateView(worldX, worldZ, error))
		return false;
	return ProcessPending(nullptr, error);
}

bool TerrainRuntime::ProcessPending(std::uint32_t* outBuilt, std::string* error)
{
	if (error)
		error->clear();
	if (outBuilt)
		*outBuilt = 0;
	if (!m_Configured)
	{
		SetRuntimeError(error, "terrain runtime is not configured");
		return false;
	}

	std::uint32_t built = 0;
	const std::uint32_t totalBuildBudget = m_Config.MaxChunkBuildsPerUpdate;
	std::uint32_t colliderReservation = 0;
	if (b3World_IsValid(m_PhysicsWorld))
	{
		const auto& currentlyLoadedChunks = m_Streamer.GetLoadedChunks();
		std::size_t missingColliderCount = 0;
		for (const auto& loaded : currentlyLoadedChunks)
		{
			if (m_Colliders.find(loaded.first) == m_Colliders.end())
				++missingColliderCount;
		}

		if (missingColliderCount != 0)
		{
			colliderReservation = static_cast<std::uint32_t>((std::min)(
				static_cast<std::size_t>(totalBuildBudget), missingColliderCount));
		}
		else if (totalBuildBudget > 1 && m_Streamer.GetPendingChunkCount() != 0)
		{
			const std::uint64_t windowDiameter =
				static_cast<std::uint64_t>(m_Config.LoadRadiusChunks) * 2 + 1;
			const std::uint64_t desiredChunkCount = windowDiameter * windowDiameter;
			if (currentlyLoadedChunks.size() < desiredChunkCount)
				colliderReservation = 1;
		}
	}

	const std::uint32_t meshBuildBudget = totalBuildBudget - colliderReservation;
	if (!m_Streamer.ProcessPending(meshBuildBudget, &built, error))
		return false;
	if (outBuilt)
		*outBuilt = built;

	const auto& loadedChunks = m_Streamer.GetLoadedChunks();
	for (auto it = m_Colliders.begin(); it != m_Colliders.end();)
	{
		if (loadedChunks.find(it->first) == loadedChunks.end())
			it = m_Colliders.erase(it);
		else
			++it;
	}

	if (!b3World_IsValid(m_PhysicsWorld))
	{
		m_Colliders.clear();
		m_PhysicsWorld = B3_NULL_ID;
		return true;
	}

	std::uint32_t collisionBuildBudget = m_Config.MaxChunkBuildsPerUpdate - built;
	if (collisionBuildBudget == 0)
		return true;

	const auto center = m_Streamer.GetViewCenterChunk();
	std::vector<TerrainChunkStreamer::ChunkKey> missingColliders;
	missingColliders.reserve(loadedChunks.size());
	for (const auto& loaded : loadedChunks)
	{
		if (m_Colliders.find(loaded.first) != m_Colliders.end())
			continue;
		missingColliders.push_back(loaded.first);
	}
	const auto distanceFromCenter = [&center](const TerrainChunkStreamer::ChunkKey& key)
	{
		const std::int64_t offsetX = key.first - center.first;
		const std::int64_t offsetZ = key.second - center.second;
		const std::int64_t absoluteX = offsetX < 0 ? -offsetX : offsetX;
		const std::int64_t absoluteZ = offsetZ < 0 ? -offsetZ : offsetZ;
		return (std::max)(absoluteX, absoluteZ);
	};
	std::sort(missingColliders.begin(), missingColliders.end(),
		[&distanceFromCenter](const TerrainChunkStreamer::ChunkKey& left,
			const TerrainChunkStreamer::ChunkKey& right)
		{
			const std::int64_t leftDistance = distanceFromCenter(left);
			const std::int64_t rightDistance = distanceFromCenter(right);
			if (leftDistance != rightDistance)
				return leftDistance < rightDistance;
			if (left.first != right.first)
				return left.first < right.first;
			return left.second < right.second;
		});

	for (const auto& key : missingColliders)
	{
		if (collisionBuildBudget == 0)
			break;

		auto collider = std::make_unique<TerrainChunkCollider>();
		const TerrainChunkEdits* edits = FindEdits(m_WorldState, key);
		if (!collider->Create(m_PhysicsWorld, m_Asset, key.first, key.second, edits, error))
			return false;
		m_Colliders.emplace(key, std::move(collider));
		--collisionBuildBudget;
	}
	return true;
}

bool TerrainRuntime::SaveWorldState(const std::filesystem::path& projectRoot, std::string* error)
{
	if (error)
		error->clear();
	if (!m_Configured)
	{
		SetRuntimeError(error, "terrain runtime is not configured");
		return false;
	}
	return SaveTerrainChunkEditsForWorld(projectRoot, m_Asset, m_WorldState, error);
}

void TerrainRuntime::Clear() noexcept
{
	m_Colliders.clear();
	m_Streamer.Reset();
	m_Asset = {};
	m_WorldState = {};
	m_Config = {};
	m_PhysicsWorld = B3_NULL_ID;
	m_Configured = false;
}

b3BodyId TerrainRuntime::GetColliderBodyId(TerrainChunkStreamer::ChunkKey key) const noexcept
{
	const auto it = m_Colliders.find(key);
	if (it == m_Colliders.end() || !it->second || !it->second->IsValid())
		return b3_nullBodyId;
	return it->second->GetBodyId();
}

const TerrainChunkEdits* TerrainRuntime::FindEdits(const TerrainWorldState& state,
	TerrainChunkStreamer::ChunkKey key) const noexcept
{
	const auto it = state.Chunks.find({ m_Asset.WorldId, key.first, key.second });
	return it == state.Chunks.end() ? nullptr : &it->second;
}
}
