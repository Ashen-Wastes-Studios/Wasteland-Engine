#include "wlpch.h"

#include "Wasteland/Terrain/TerrainCollision.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Wasteland::Terrain
{
namespace
{
void SetCollisionError(std::string* error, const char* message)
{
	if (error)
		*error = message;
}

bool ToFiniteFloat(double value, float& result)
{
	if (!std::isfinite(value) || value < -static_cast<double>((std::numeric_limits<float>::max)()) ||
		value > static_cast<double>((std::numeric_limits<float>::max)()))
		return false;
	result = static_cast<float>(value);
	return std::isfinite(result);
}
}

TerrainChunkCollider::~TerrainChunkCollider()
{
	Destroy();
}

bool TerrainChunkCollider::Create(b3WorldId worldId, const TerrainAsset& asset,
	std::int64_t chunkX, std::int64_t chunkZ, const TerrainChunkEdits* edits, std::string* error)
{
	if (error)
		error->clear();
	if (!b3World_IsValid(worldId))
	{
		SetCollisionError(error, "terrain collider requires a live Box3D world");
		return false;
	}

	TerrainChunkMesh terrainMesh;
	if (!BuildTerrainChunkMesh(asset, chunkX, chunkZ, 0, terrainMesh, edits, error))
		return false;
	const std::size_t samples = static_cast<std::size_t>(kTerrainChunkSamples);
	const std::size_t expectedVertices = samples * samples;
	const std::size_t expectedIndices = static_cast<std::size_t>(kTerrainChunkCells) *
		static_cast<std::size_t>(kTerrainChunkCells) * 6;
	if (terrainMesh.LodLevel != 0 || terrainMesh.CellsPerSide != kTerrainChunkCells ||
		terrainMesh.Vertices.size() != expectedVertices || terrainMesh.Indices.size() != expectedIndices ||
		terrainMesh.Vertices.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
		terrainMesh.Indices.size() / 3 > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
	{
		SetCollisionError(error, "terrain collision requires a complete full-resolution chunk mesh");
		return false;
	}

	const auto& firstPosition = terrainMesh.Vertices.front().Position;
	const double originX = firstPosition[0];
	const double originY = firstPosition[1];
	const double originZ = firstPosition[2];
	const double positionTolerance = asset.SampleSpacing * 1.0e-3;
	if (!std::isfinite(positionTolerance) || positionTolerance <= 0.0 ||
		!std::isfinite(originX) || !std::isfinite(originY) || !std::isfinite(originZ))
	{
		SetCollisionError(error, "terrain collision origin or precision tolerance is not representable");
		return false;
	}

	b3Pos bodyPosition{};
#if defined(BOX3D_DOUBLE_PRECISION)
	bodyPosition = { originX, originY, originZ };
#else
	float bodyX = 0.0f;
	float bodyY = 0.0f;
	float bodyZ = 0.0f;
	if (!ToFiniteFloat(originX, bodyX) || !ToFiniteFloat(originY, bodyY) || !ToFiniteFloat(originZ, bodyZ))
	{
		SetCollisionError(error, "terrain chunk origin exceeds Box3D world-position range");
		return false;
	}
	bodyPosition = { bodyX, bodyY, bodyZ };
#endif

	std::vector<b3Vec3> vertices;
	vertices.reserve(terrainMesh.Vertices.size());
	for (const TerrainMeshVertex& vertex : terrainMesh.Vertices)
	{
		const double worldX = vertex.Position[0];
		const double worldY = vertex.Position[1];
		const double worldZ = vertex.Position[2];
		float localX = 0.0f;
		float localY = 0.0f;
		float localZ = 0.0f;
		if (!ToFiniteFloat(worldX - originX, localX) || !ToFiniteFloat(worldY - originY, localY) ||
			!ToFiniteFloat(worldZ - originZ, localZ))
		{
			SetCollisionError(error, "terrain chunk local vertex exceeds Box3D float range");
			return false;
		}
#if defined(BOX3D_DOUBLE_PRECISION)
		const double reconstructedX = bodyPosition.x + static_cast<double>(localX);
		const double reconstructedY = bodyPosition.y + static_cast<double>(localY);
		const double reconstructedZ = bodyPosition.z + static_cast<double>(localZ);
#else
		const double reconstructedX = static_cast<double>(bodyPosition.x + localX);
		const double reconstructedY = static_cast<double>(bodyPosition.y + localY);
		const double reconstructedZ = static_cast<double>(bodyPosition.z + localZ);
#endif
		if (std::abs(reconstructedX - worldX) > positionTolerance ||
			std::abs(reconstructedY - worldY) > positionTolerance ||
			std::abs(reconstructedZ - worldZ) > positionTolerance)
		{
			SetCollisionError(error, "terrain spacing collapses beyond Box3D position precision");
			return false;
		}
		vertices.push_back({ localX, localY, localZ });
	}

	for (std::size_t z = 0; z < samples; ++z)
	{
		for (std::size_t x = 1; x < samples; ++x)
		{
			const std::size_t index = z * samples + x;
			if (!(vertices[index].x > vertices[index - 1].x))
			{
				SetCollisionError(error, "terrain collision X samples collapse after Box3D conversion");
				return false;
			}
		}
	}
	for (std::size_t z = 1; z < samples; ++z)
	{
		for (std::size_t x = 0; x < samples; ++x)
		{
			const std::size_t index = z * samples + x;
			if (!(vertices[index].z > vertices[index - samples].z))
			{
				SetCollisionError(error, "terrain collision Z samples collapse after Box3D conversion");
				return false;
			}
		}
	}

	std::vector<std::int32_t> indices;
	indices.reserve(terrainMesh.Indices.size());
	for (std::uint32_t index : terrainMesh.Indices)
	{
		if (index >= vertices.size() || index > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
		{
			SetCollisionError(error, "terrain collision mesh contains an invalid triangle index");
			return false;
		}
		indices.push_back(static_cast<std::int32_t>(index));
	}

	b3MeshDef meshDef{};
	meshDef.vertices = vertices.data();
	meshDef.indices = indices.data();
	meshDef.vertexCount = static_cast<int>(vertices.size());
	meshDef.triangleCount = static_cast<int>(indices.size() / 3);
	meshDef.weldTolerance = 0.0f;
	meshDef.weldVertices = false;
	meshDef.useMedianSplit = true;
	meshDef.identifyEdges = true;
	b3MeshData* generatedMesh = b3CreateMesh(&meshDef, nullptr, 0);
	if (!generatedMesh || generatedMesh->triangleCount != meshDef.triangleCount)
	{
		if (generatedMesh)
			b3DestroyMesh(generatedMesh);
		SetCollisionError(error, "Box3D rejected or degenerated the terrain collision mesh");
		return false;
	}

	b3BodyDef bodyDef = b3DefaultBodyDef();
	bodyDef.type = b3_staticBody;
	bodyDef.position = bodyPosition;
	const b3BodyId generatedBody = b3CreateBody(worldId, &bodyDef);
	if (!b3Body_IsValid(generatedBody))
	{
		b3DestroyMesh(generatedMesh);
		SetCollisionError(error, "Box3D failed to create the terrain static body");
		return false;
	}
	const b3ShapeDef shapeDef = b3DefaultShapeDef();
	const b3ShapeId generatedShape = b3CreateMeshShape(generatedBody, &shapeDef, generatedMesh, { 1.0f, 1.0f, 1.0f });
	if (!b3Shape_IsValid(generatedShape))
	{
		b3DestroyBody(generatedBody);
		b3DestroyMesh(generatedMesh);
		SetCollisionError(error, "Box3D failed to attach the terrain collision mesh");
		return false;
	}

	Destroy();
	m_WorldId = worldId;
	m_BodyId = generatedBody;
	m_MeshData = generatedMesh;
	return true;
}

void TerrainChunkCollider::Destroy() noexcept
{
	if (b3Body_IsValid(m_BodyId))
		b3DestroyBody(m_BodyId);
	m_BodyId = {};
	m_WorldId = {};
	if (m_MeshData)
		b3DestroyMesh(m_MeshData);
	m_MeshData = nullptr;
}

bool TerrainChunkCollider::IsValid() const noexcept
{
	return m_MeshData != nullptr && b3World_IsValid(m_WorldId) && b3Body_IsValid(m_BodyId);
}
}
