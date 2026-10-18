#pragma once

#include "Wasteland/Terrain/Terrain.h"

#include <box3d/box3d.h>

namespace Wasteland::Terrain
{
// Owns a Box3D static triangle-mesh body for one full-resolution terrain chunk.
// Box3D mesh data is retained until its body/world no longer references it.
class TerrainChunkCollider
{
public:
	TerrainChunkCollider() = default;
	~TerrainChunkCollider();

	TerrainChunkCollider(const TerrainChunkCollider&) = delete;
	TerrainChunkCollider& operator=(const TerrainChunkCollider&) = delete;

	// Builds collision from LOD0. Failure is transactional: an existing collider
	// remains attached and owned until the replacement body and shape are valid.
	bool Create(b3WorldId worldId, const TerrainAsset& asset, std::int64_t chunkX, std::int64_t chunkZ,
		const TerrainChunkEdits* edits = nullptr, std::string* error = nullptr);
	void Destroy() noexcept;

	bool IsValid() const noexcept;
	b3BodyId GetBodyId() const noexcept { return m_BodyId; }

private:
	b3WorldId m_WorldId{};
	b3BodyId m_BodyId{};
	b3MeshData* m_MeshData = nullptr;
};
}
