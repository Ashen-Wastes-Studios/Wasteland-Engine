#pragma once

#include "Wasteland/Terrain/Terrain.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Wasteland
{
struct alignas(16) TerrainNovaVertex
{
	glm::vec4 Position;
	glm::vec4 Normal;
};

static_assert(sizeof(TerrainNovaVertex) == 32, "Nova terrain vertices must be 32 bytes");

struct TerrainNovaChunkGeometry
{
	std::vector<TerrainNovaVertex> Vertices;
	glm::mat4 WorldTransform{ 1.0f };
	glm::mat4 InvTransform{ 1.0f };
	glm::vec4 BoundsMin{};
	glm::vec4 BoundsMax{};
	float MaxDistance = 0.0f;
	std::uint32_t CellsPerSide = 0;
	std::uint32_t LodLevel = 0;
};

namespace TerrainNovaDetail
{
inline bool Fail(std::string* error, const char* message)
{
	if (error)
		*error = message;
	return false;
}

inline bool IsFloatRepresentable(double value)
{
	const double limit = static_cast<double>((std::numeric_limits<float>::max)());
	return std::isfinite(value) && value >= -limit && value <= limit;
}
}

// Terrain::BuildTerrainChunkMesh emits row-major vertices (z * (cells + 1) + x)
// and, for each cell, indices { a, c, b, b, c, d }. This packer preserves that
// vertex order, validates the uniform horizontal grid required by the shader DDA,
// and deliberately does not rescan the index buffer for validation.
inline bool BuildTerrainNovaChunkGeometry(const Terrain::TerrainChunkMesh& source,
	TerrainNovaChunkGeometry& outGeometry, std::string* error = nullptr)
{
	if (source.CellsPerSide == 0)
		return TerrainNovaDetail::Fail(error, "terrain Nova grid must have at least one cell per side");
	if (source.CellsPerSide > 64)
		return TerrainNovaDetail::Fail(error, "terrain Nova grid cannot exceed the 64-cell shader DDA limit");

	const std::uint64_t verticesPerSide = static_cast<std::uint64_t>(source.CellsPerSide) + 1u;
	const std::uint64_t maxVertexCount = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
	if (verticesPerSide > maxVertexCount / verticesPerSide)
		return TerrainNovaDetail::Fail(error, "terrain Nova grid vertex count overflows addressable storage");
	const std::size_t expectedVertexCount = static_cast<std::size_t>(verticesPerSide * verticesPerSide);
	if (source.Vertices.size() != expectedVertexCount)
		return TerrainNovaDetail::Fail(error, "terrain Nova grid does not contain exactly (cells + 1)^2 vertices");

	const Terrain::TerrainMeshVertex& originVertex = source.Vertices.front();
	for (std::size_t axis = 0; axis < 3; ++axis)
	{
		if (!TerrainNovaDetail::IsFloatRepresentable(originVertex.Position[axis]))
			return TerrainNovaDetail::Fail(error, "terrain Nova chunk origin is not finite or representable as float");
	}

	TerrainNovaChunkGeometry generated;
	generated.CellsPerSide = source.CellsPerSide;
	generated.LodLevel = source.LodLevel;
	generated.WorldTransform = glm::mat4(1.0f);
	generated.InvTransform = glm::mat4(1.0f);
	for (std::size_t axis = 0; axis < 3; ++axis)
	{
		const float translation = static_cast<float>(originVertex.Position[axis]);
		if (!std::isfinite(translation))
			return TerrainNovaDetail::Fail(error, "terrain Nova chunk origin is not finite or representable as float");
		generated.WorldTransform[3][static_cast<glm::length_t>(axis)] = translation;
		generated.InvTransform[3][static_cast<glm::length_t>(axis)] = -translation;
	}

	const float floatMax = (std::numeric_limits<float>::max)();
	const float floatLowest = (std::numeric_limits<float>::lowest)();
	glm::vec3 boundsMin(floatMax);
	glm::vec3 boundsMax(floatLowest);
	generated.Vertices.reserve(source.Vertices.size());
	for (const Terrain::TerrainMeshVertex& input : source.Vertices)
	{
		float local[3]{};
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(input.Position[axis]))
				return TerrainNovaDetail::Fail(error, "terrain Nova vertex position is not finite");
			const double difference = input.Position[axis] - originVertex.Position[axis];
			if (!TerrainNovaDetail::IsFloatRepresentable(difference))
				return TerrainNovaDetail::Fail(error, "terrain Nova local vertex position is not representable as float");
			local[axis] = static_cast<float>(difference);
			if (!std::isfinite(local[axis]))
				return TerrainNovaDetail::Fail(error, "terrain Nova local vertex position is not representable as float");
			if (!std::isfinite(input.Normal[axis]))
				return TerrainNovaDetail::Fail(error, "terrain Nova vertex normal is not finite");
		}

		TerrainNovaVertex output;
		output.Position = glm::vec4(local[0], local[1], local[2], 1.0f);
		output.Normal = glm::vec4(input.Normal[0], input.Normal[1], input.Normal[2], 0.0f);
		generated.Vertices.push_back(output);
		for (glm::length_t axis = 0; axis < 3; ++axis)
		{
			boundsMin[axis] = (std::min)(boundsMin[axis], local[axis]);
			boundsMax[axis] = (std::max)(boundsMax[axis], local[axis]);
		}
	}

	const double horizontalExtentX = static_cast<double>(boundsMax.x) - static_cast<double>(boundsMin.x);
	const double horizontalExtentZ = static_cast<double>(boundsMax.z) - static_cast<double>(boundsMin.z);
	if (!(boundsMin.x < boundsMax.x) || !TerrainNovaDetail::IsFloatRepresentable(horizontalExtentX) ||
		!(boundsMin.z < boundsMax.z) || !TerrainNovaDetail::IsFloatRepresentable(horizontalExtentZ))
		return TerrainNovaDetail::Fail(error, "terrain Nova grid has degenerate or unrepresentable horizontal extents");

	const float cellWidthX = static_cast<float>(horizontalExtentX / source.CellsPerSide);
	const float cellWidthZ = static_cast<float>(horizontalExtentZ / source.CellsPerSide);
	if (!(cellWidthX > 0.0f) || !std::isfinite(cellWidthX) ||
		!(cellWidthZ > 0.0f) || !std::isfinite(cellWidthZ))
		return TerrainNovaDetail::Fail(error, "terrain Nova grid must have finite positive horizontal cell spacing");

	// The shader's DDA assumes a uniform, positive-going grid. Verify that the
	// packed vertices match that grid and remain distinct after the float world
	// translation, including where the coordinate range crosses an exponent edge.
	const float translationX = generated.WorldTransform[3][0];
	const float translationZ = generated.WorldTransform[3][2];
	const float gridToleranceFactor = 8.0f * std::numeric_limits<float>::epsilon();
	const std::size_t packedVerticesPerSide = static_cast<std::size_t>(verticesPerSide);
	for (std::size_t z = 0; z < packedVerticesPerSide; ++z)
	{
		for (std::size_t x = 0; x < packedVerticesPerSide; ++x)
		{
			const std::size_t index = z * packedVerticesPerSide + x;
			const glm::vec4& local = generated.Vertices[index].Position;
			const float expectedX = boundsMin.x + cellWidthX * static_cast<float>(x);
			const float expectedZ = boundsMin.z + cellWidthZ * static_cast<float>(z);
			const float toleranceX = gridToleranceFactor * (std::max)(
				cellWidthX, (std::max)(std::abs(local.x), std::abs(expectedX)));
			const float toleranceZ = gridToleranceFactor * (std::max)(
				cellWidthZ, (std::max)(std::abs(local.z), std::abs(expectedZ)));
			if (!std::isfinite(expectedX) || !std::isfinite(expectedZ) ||
				std::abs(local.x - expectedX) > toleranceX ||
				std::abs(local.z - expectedZ) > toleranceZ)
				return TerrainNovaDetail::Fail(error, "terrain Nova grid must be uniformly spaced and axis-aligned");

			const float worldX = translationX + local.x;
			const float worldZ = translationZ + local.z;
			if (!std::isfinite(worldX) || !std::isfinite(worldZ) ||
				(x > 0 && !(worldX > translationX + generated.Vertices[index - 1].Position.x)) ||
				(z > 0 && !(worldZ > translationZ + generated.Vertices[index - packedVerticesPerSide].Position.z)))
				return TerrainNovaDetail::Fail(error, "terrain Nova world transform collapses adjacent grid lines");
		}
	}

	if (boundsMin.y == boundsMax.y)
	{
		constexpr float kFlatBoundsPadding = 0.001f;
		boundsMin.y -= kFlatBoundsPadding;
		boundsMax.y += kFlatBoundsPadding;
	}
	if (!std::isfinite(boundsMin.y) || !std::isfinite(boundsMax.y) || !(boundsMin.y < boundsMax.y))
		return TerrainNovaDetail::Fail(error, "terrain Nova grid has invalid vertical bounds");

	generated.BoundsMin = glm::vec4(boundsMin, 1.0f);
	generated.BoundsMax = glm::vec4(boundsMax, 1.0f);
	const double boundWidth = static_cast<double>(generated.BoundsMax.x) - generated.BoundsMin.x;
	const double boundHeight = static_cast<double>(generated.BoundsMax.y) - generated.BoundsMin.y;
	const double boundDepth = static_cast<double>(generated.BoundsMax.z) - generated.BoundsMin.z;
	// The shader's per-instance distance cull is this base plus the chunk's own
	// diagonal, so a chunk whose origin is out of range is still traced while any
	// part of it is in range. The base tracks Renderer3DData::MaxDrawDistance --
	// keep the two equal, or the shader culls terrain the CPU still uploads.
	const double maxDistance = 10000.0 + std::hypot(boundWidth, boundHeight, boundDepth);
	const double floatMaxAsDouble = static_cast<double>((std::numeric_limits<float>::max)());
	generated.MaxDistance = std::isfinite(maxDistance) && maxDistance <= floatMaxAsDouble
		? static_cast<float>(maxDistance)
		: (std::numeric_limits<float>::max)();
	outGeometry = std::move(generated);
	return true;
}
}
