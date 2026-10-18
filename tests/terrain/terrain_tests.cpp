#define main TerrainLegacyTestsMain
#include "test_terrain.cpp"
#undef main

#include "Wasteland/Renderer/TerrainNovaMesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

static_assert(sizeof(Wasteland::TerrainNovaVertex) == 32, "Nova terrain vertices must be 32 bytes");
static_assert(alignof(Wasteland::TerrainNovaVertex) == 16, "Nova terrain vertices must be 16-byte aligned");

namespace
{
using Wasteland::Terrain::TerrainChunkMesh;
using Wasteland::Terrain::TerrainMeshVertex;
using Wasteland::TerrainNovaChunkGeometry;
using Wasteland::TerrainNovaVertex;

TerrainChunkMesh MakeNovaGrid(std::uint32_t cellsPerSide)
{
	TerrainChunkMesh mesh;
	mesh.CellsPerSide = cellsPerSide;
	mesh.LodLevel = 3;
	const std::uint32_t verticesPerSide = cellsPerSide + 1;
	mesh.Vertices.resize(static_cast<std::size_t>(verticesPerSide) * verticesPerSide);
	for (std::uint32_t z = 0; z < verticesPerSide; ++z)
	{
		for (std::uint32_t x = 0; x < verticesPerSide; ++x)
		{
			TerrainMeshVertex& vertex = mesh.Vertices[static_cast<std::size_t>(z) * verticesPerSide + x];
			vertex.Position = { 100.0 + 2.0 * x, 20.0 + x + 2.0 * z, -50.0 + 3.0 * z };
			vertex.Normal = { 0.25f * static_cast<float>(x), 1.0f, -0.25f * static_cast<float>(z) };
		}
	}
	for (std::uint32_t z = 0; z < cellsPerSide; ++z)
	{
		for (std::uint32_t x = 0; x < cellsPerSide; ++x)
		{
			const std::uint32_t a = z * verticesPerSide + x;
			const std::uint32_t b = a + 1;
			const std::uint32_t c = a + verticesPerSide;
			const std::uint32_t d = c + 1;
			mesh.Indices.insert(mesh.Indices.end(), { a, c, b, b, c, d });
		}
	}
	return mesh;
}

bool SameVec4(const glm::vec4& left, const glm::vec4& right)
{
	for (int component = 0; component < 4; ++component)
		if (left[component] != right[component])
			return false;
	return true;
}

bool SameMat4(const glm::mat4& left, const glm::mat4& right)
{
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			if (left[column][row] != right[column][row])
				return false;
	return true;
}

bool SameNovaGeometry(const TerrainNovaChunkGeometry& left, const TerrainNovaChunkGeometry& right)
{
	if (left.Vertices.size() != right.Vertices.size() || left.CellsPerSide != right.CellsPerSide ||
		left.LodLevel != right.LodLevel || !SameMat4(left.WorldTransform, right.WorldTransform) ||
		!SameMat4(left.InvTransform, right.InvTransform) ||
		!SameVec4(left.BoundsMin, right.BoundsMin) || !SameVec4(left.BoundsMax, right.BoundsMax) ||
		left.MaxDistance != right.MaxDistance)
		return false;
	for (std::size_t index = 0; index < left.Vertices.size(); ++index)
	{
		if (!SameVec4(left.Vertices[index].Position, right.Vertices[index].Position) ||
			!SameVec4(left.Vertices[index].Normal, right.Vertices[index].Normal))
			return false;
	}
	return true;
}

void TestTerrainNovaTwoCellGrid()
{
	TerrainChunkMesh source = MakeNovaGrid(2);
	// Give selected corners distinct elevations so bounds cover the complete grid.
	source.Vertices[0].Position[1] = 20.0;
	source.Vertices[1].Position[1] = 22.0;
	source.Vertices[2].Position[1] = 19.0;
	source.Vertices[3].Position[1] = 24.0;
	source.Vertices[4].Position[1] = 25.0;
	source.Vertices[5].Position[1] = 26.0;
	source.Vertices[6].Position[1] = 22.0;
	source.Vertices[7].Position[1] = 18.0;
	source.Vertices[8].Position[1] = 23.0;

	TerrainNovaChunkGeometry packed;
	std::string error;
	Check(Wasteland::BuildTerrainNovaChunkGeometry(source, packed, &error),
		"terrain-nova: a valid 2x2 terrain grid packs successfully");
	Check(packed.Vertices.size() == 9 && packed.CellsPerSide == 2 && packed.LodLevel == 3,
		"terrain-nova: packing preserves the complete grid and LOD metadata");
	if (packed.Vertices.size() == source.Vertices.size())
	{
		bool verticesMatch = true;
		for (std::size_t index = 0; index < source.Vertices.size(); ++index)
		{
			const TerrainMeshVertex& input = source.Vertices[index];
			const TerrainNovaVertex& output = packed.Vertices[index];
			verticesMatch = verticesMatch &&
				output.Position == glm::vec4(
					static_cast<float>(input.Position[0] - source.Vertices[0].Position[0]),
					static_cast<float>(input.Position[1] - source.Vertices[0].Position[1]),
					static_cast<float>(input.Position[2] - source.Vertices[0].Position[2]), 1.0f) &&
				output.Normal == glm::vec4(input.Normal[0], input.Normal[1], input.Normal[2], 0.0f);
		}
		Check(verticesMatch, "terrain-nova: vertices rebase in double precision and retain source normals");
	}
	Check(packed.WorldTransform[3] == glm::vec4(100.0f, 20.0f, -50.0f, 1.0f) &&
		packed.InvTransform[3] == glm::vec4(-100.0f, -20.0f, 50.0f, 1.0f),
		"terrain-nova: chunk transforms carry the source origin and its inverse translation");
	Check(packed.BoundsMin == glm::vec4(0.0f, -2.0f, 0.0f, 1.0f) &&
		packed.BoundsMax == glm::vec4(4.0f, 6.0f, 6.0f, 1.0f),
		"terrain-nova: bounds enclose local grid coordinates with homogeneous point values");
}

void TestTerrainNovaPacksTerrainBuilderMeshes()
{
	Wasteland::Terrain::TerrainAsset asset;
	std::string error;
	const bool created = Wasteland::Terrain::CreateDefaultTerrainAsset("world.nova-test", asset, &error);
	Check(created, "terrain-nova: default Terrain asset is available for producer integration");
	if (!created)
		return;
	asset.OriginX = -1.0e6;
	asset.OriginZ = 8.0e5;
	asset.SampleSpacing = 0.3;

	const std::uint32_t lodLevels[] = { 0, 3, 6 };
	for (const std::uint32_t lod : lodLevels)
	{
		TerrainChunkMesh source;
		const bool built = Wasteland::Terrain::BuildTerrainChunkMesh(
			asset, 0, 0, lod, source, nullptr, &error);
		Check(built, "terrain-nova: Terrain builder emits a valid uniform chunk mesh");
		if (!built)
			continue;

		TerrainNovaChunkGeometry packed;
		Check(Wasteland::BuildTerrainNovaChunkGeometry(source, packed, &error),
			"terrain-nova: generated terrain mesh packs at LOD 0, 3, and 6");
	}
}

void TestTerrainNovaLargeWorldRebaseAndFlatBounds()
{
	TerrainChunkMesh large = MakeNovaGrid(1);
	large.Vertices[0].Position = { 1.0e6, -1.0e6, 8.0e5 };
	large.Vertices[1].Position = { 1.0e6 + 0.25, -1.0e6 + 0.5, 8.0e5 };
	large.Vertices[2].Position = { 1.0e6, -1.0e6 + 0.25, 8.0e5 + 0.25 };
	large.Vertices[3].Position = { 1.0e6 + 0.25, -1.0e6 + 0.5, 8.0e5 + 0.25 };
	TerrainNovaChunkGeometry packed;
	std::string error;
	Check(Wasteland::BuildTerrainNovaChunkGeometry(large, packed, &error),
		"terrain-nova: large but float-grid-representable world coordinates are accepted");
	Check(packed.Vertices.size() == 4 && packed.Vertices[1].Position.x == 0.25f &&
		packed.Vertices[2].Position.z == 0.25f && packed.Vertices[3].Position.y == 0.5f,
		"terrain-nova: local rebasing preserves sub-unit spacing at large representable coordinates");

	TerrainChunkMesh flat = MakeNovaGrid(1);
	for (TerrainMeshVertex& vertex : flat.Vertices)
		vertex.Position[1] = 5.0;
	Check(Wasteland::BuildTerrainNovaChunkGeometry(flat, packed, &error),
		"terrain-nova: a flat heightfield packs successfully");
	Check(packed.BoundsMin.y < 0.0f && packed.BoundsMax.y > 0.0f &&
		packed.BoundsMax.y - packed.BoundsMin.y < 0.01f,
		"terrain-nova: flat chunk bounds receive a small local Y padding");
}

void TestTerrainNovaRejectsUnrepresentableWorldGridSpacing()
{
	TerrainChunkMesh source = MakeNovaGrid(1);
	for (std::uint32_t z = 0; z < 2; ++z)
	{
		for (std::uint32_t x = 0; x < 2; ++x)
		{
			TerrainMeshVertex& vertex = source.Vertices[static_cast<std::size_t>(z) * 2 + x];
			vertex.Position = { 1.0e12 + 0.25 * x, 0.0, 8.0e11 + 0.25 * z };
		}
	}

	TerrainNovaChunkGeometry output;
	const TerrainNovaChunkGeometry original = output;
	std::string error;
	const bool packed = Wasteland::BuildTerrainNovaChunkGeometry(source, output, &error);
	Check(!packed && !error.empty(),
		"terrain-nova: cells finer than float world-origin precision are rejected");
	Check(SameNovaGeometry(output, original),
		"terrain-nova: unrepresentable world-grid rejection preserves caller geometry");
}

void TestTerrainNovaGridPrecisionAcrossFloatBoundaries()
{
	TerrainNovaChunkGeometry output;
	const TerrainNovaChunkGeometry original = output;
	std::string error;
	TerrainChunkMesh crossing = MakeNovaGrid(64);
	for (std::uint32_t z = 0; z <= 64; ++z)
	{
		for (std::uint32_t x = 0; x <= 64; ++x)
		{
			TerrainMeshVertex& vertex = crossing.Vertices[static_cast<std::size_t>(z) * 65 + x];
			vertex.Position = { 1048575.0 + 0.0625 * x, 0.0, 0.0625 * z };
		}
	}
	const bool crossingPacked = Wasteland::BuildTerrainNovaChunkGeometry(crossing, output, &error);
	Check(!crossingPacked && !error.empty() && SameNovaGeometry(output, original),
		"terrain-nova: float exponent boundary cannot collapse adjacent positive grid lines");

	TerrainChunkMesh negative = MakeNovaGrid(64);
	for (std::uint32_t z = 0; z <= 64; ++z)
	{
		for (std::uint32_t x = 0; x <= 64; ++x)
		{
			TerrainMeshVertex& vertex = negative.Vertices[static_cast<std::size_t>(z) * 65 + x];
			vertex.Position = { -1048576.0 + 0.0625 * x, 0.0, -1048576.0 + 0.0625 * z };
		}
	}
	Check(Wasteland::BuildTerrainNovaChunkGeometry(negative, output, &error),
		"terrain-nova: negative origins accept distinct positive-going grid lines");
}

void TestTerrainNovaTinySpacingRemainsPackable()
{
	TerrainChunkMesh tiny = MakeNovaGrid(1);
	for (std::uint32_t z = 0; z < 2; ++z)
	{
		for (std::uint32_t x = 0; x < 2; ++x)
		{
			TerrainMeshVertex& vertex = tiny.Vertices[static_cast<std::size_t>(z) * 2 + x];
			vertex.Position = { 1.0e-12 * x, 0.0, 1.0e-12 * z };
		}
	}

	TerrainNovaChunkGeometry packed;
	std::string error;
	Check(Wasteland::BuildTerrainNovaChunkGeometry(tiny, packed, &error),
		"terrain-nova: a valid grid with 1e-12 spacing remains packable");
	Check(packed.BoundsMax.x > packed.BoundsMin.x && packed.BoundsMax.z > packed.BoundsMin.z &&
		packed.Vertices[1].Position.x > 0.0f && packed.Vertices[2].Position.z > 0.0f,
		"terrain-nova: tiny horizontal spacing remains non-degenerate after packing");
}

void TestTerrainNovaCullingDistanceCoversFarCorners()
{
	TerrainChunkMesh source = MakeNovaGrid(64);
	const std::uint32_t verticesPerSide = 65;
	for (std::uint32_t z = 0; z < verticesPerSide; ++z)
	{
		for (std::uint32_t x = 0; x < verticesPerSide; ++x)
		{
			TerrainMeshVertex& vertex = source.Vertices[static_cast<std::size_t>(z) * verticesPerSide + x];
			vertex.Position = { 50.0 * x, 0.0, 50.0 * z };
		}
	}

	TerrainNovaChunkGeometry packed;
	std::string error;
	Check(Wasteland::BuildTerrainNovaChunkGeometry(source, packed, &error),
		"terrain-nova: a 64-cell grid with 50-unit spacing packs successfully");
	const double farCornerDistance = std::hypot(
		static_cast<double>(packed.BoundsMax.x), static_cast<double>(packed.BoundsMax.z));
	Check(static_cast<double>(packed.MaxDistance) > farCornerDistance,
		"terrain-nova: chunk culling distance exceeds the far-corner distance from its origin");
}

void TestTerrainNovaCullingDistanceSaturates()
{
	TerrainChunkMesh source = MakeNovaGrid(2);
	const std::uint32_t verticesPerSide = 3;
	for (std::uint32_t z = 0; z < verticesPerSide; ++z)
	{
		for (std::uint32_t x = 0; x < verticesPerSide; ++x)
		{
			TerrainMeshVertex& vertex = source.Vertices[static_cast<std::size_t>(z) * verticesPerSide + x];
			vertex.Position = { 1.65e38 * x, 0.0, 1.65e38 * z };
		}
	}

	TerrainNovaChunkGeometry packed;
	std::string error;
	Check(Wasteland::BuildTerrainNovaChunkGeometry(source, packed, &error),
		"terrain-nova: large but representable horizontal extents pack successfully");
	Check(packed.MaxDistance == (std::numeric_limits<float>::max)(),
		"terrain-nova: an oversized culling distance saturates at float max");
}

void TestTerrainNovaInvalidInputsAreTransactional()
{
	TerrainNovaChunkGeometry sentinel;
	sentinel.Vertices.push_back({ glm::vec4(7.0f, 8.0f, 9.0f, 1.0f), glm::vec4(1.0f, 2.0f, 3.0f, 0.0f) });
	sentinel.WorldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 4.0f, 5.0f));
	sentinel.InvTransform = glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, -4.0f, -5.0f));
	sentinel.BoundsMin = glm::vec4(-1.0f, -2.0f, -3.0f, 1.0f);
	sentinel.BoundsMax = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
	sentinel.MaxDistance = 42.0f;
	sentinel.CellsPerSide = 17;
	sentinel.LodLevel = 9;

	auto rejectsWithoutMutation = [&](const TerrainChunkMesh& malformed, const char* requirement)
	{
		TerrainNovaChunkGeometry output = sentinel;
		std::string error;
		const bool rejected = !Wasteland::BuildTerrainNovaChunkGeometry(malformed, output, &error);
		Check(rejected && !error.empty() && SameNovaGeometry(output, sentinel), requirement);
	};

	TerrainChunkMesh source = MakeNovaGrid(1);
	TerrainChunkMesh malformed = source;
	malformed.CellsPerSide = 0;
	rejectsWithoutMutation(malformed,
		"terrain-nova: zero cells fail with an error and preserve the caller's geometry");

	malformed = MakeNovaGrid(65);
	rejectsWithoutMutation(malformed,
		"terrain-nova: grids larger than the 64-cell shader DDA limit fail transactionally");

	malformed = MakeNovaGrid(2);
	for (std::uint32_t z = 0; z <= 2; ++z)
		malformed.Vertices[static_cast<std::size_t>(z) * 3 + 2].Position[0] += 0.5;
	rejectsWithoutMutation(malformed,
		"terrain-nova: nonuniform horizontal grids fail transactionally");

	malformed = source;
	malformed.Vertices.pop_back();
	rejectsWithoutMutation(malformed,
		"terrain-nova: a non-square vertex count fails transactionally");

	malformed = source;
	malformed.Vertices[2].Position[0] = std::numeric_limits<double>::quiet_NaN();
	rejectsWithoutMutation(malformed,
		"terrain-nova: non-finite positions fail transactionally");

	malformed = source;
	malformed.Vertices[2].Normal[1] = std::numeric_limits<float>::infinity();
	rejectsWithoutMutation(malformed,
		"terrain-nova: non-finite normals fail transactionally");

	malformed = source;
	malformed.Vertices[1].Position[0] = 1.0e100;
	rejectsWithoutMutation(malformed,
		"terrain-nova: local positions outside float range fail transactionally");

	malformed = source;
	for (TerrainMeshVertex& vertex : malformed.Vertices)
		vertex.Position[0] = 100.0;
	rejectsWithoutMutation(malformed,
		"terrain-nova: degenerate horizontal extents fail transactionally");

	malformed = source;
	malformed.Vertices[0].Position[0] = 0.0;
	malformed.Vertices[1].Position[0] = 2.0e38;
	malformed.Vertices[2].Position[0] = -2.0e38;
	malformed.Vertices[3].Position[0] = 0.0;
	rejectsWithoutMutation(malformed,
		"terrain-nova: horizontal bounds wider than float range fail transactionally");
}
}

int main()
{
	TestTerrainNovaTwoCellGrid();
	TestTerrainNovaPacksTerrainBuilderMeshes();
	TestTerrainNovaLargeWorldRebaseAndFlatBounds();
	TestTerrainNovaRejectsUnrepresentableWorldGridSpacing();
	TestTerrainNovaGridPrecisionAcrossFloatBoundaries();
	TestTerrainNovaTinySpacingRemainsPackable();
	TestTerrainNovaCullingDistanceCoversFarCorners();
	TestTerrainNovaCullingDistanceSaturates();
	TestTerrainNovaInvalidInputsAreTransactional();
	const int existingResult = TerrainLegacyTestsMain();
	return s_Failures == 0 ? existingResult : 1;
}
