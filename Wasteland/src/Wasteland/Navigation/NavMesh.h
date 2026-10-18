#pragma once

// NavMesh: a baked, walkable surface plus A* over it.
//
// WHY THIS IS CPU AND GRID-BASED
//
// The plan (item-14.5.2) asks for a navmesh baked from level collision, with
// string pulling and agent types. Recast/Detour is not vendored here, and
// adding it would mean importing a large third-party tree plus a new build
// target -- which is a decision for the project owner, not something to slip
// in alongside a feature.
//
// So this is a uniform grid over the XZ plane, rasterised from the Box3D
// colliders that actually exist in the scene, with a height per cell. It is a
// grid, not a polygon soup, which costs some memory and precision but keeps
// the whole thing dependency-free, deterministic, and testable headlessly --
// which matters more here, because everything else in this codebase is proven
// by running it.
//
// Deliberate limits, all of them visible in the API rather than discovered at
// runtime:
//
//   * Walkability is per-XZ-cell. A cell is walkable when some collider's top
//     face is inside AgentHeight of the sampled ground. Overhangs and
//     multi-storey geometry flatten out; this is a ground-plane navmesh, not a
//     full 3D one.
//   * Step height is `StepHeight`. A gap taller than that is simply not walkable,
//     so an agent does not try to climb a wall.
//   * String pulling uses the scene's own Raycast, so the resulting path is
//     validated against REAL collision, not just against the grid. That is the
//     one property worth insisting on: the bake is a grid approximation, but
//     the agent never actually walks through a wall because the final
//     straight-line segments are checked against collision.
//
// Determinism matters more than speed for the tests: ties in A* break on cell
// index, so the same bake always yields the same path.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace Wasteland
{
	class Scene;

	namespace Navigation
	{
		// One step of a path. World space, on the ground surface.
		struct NavPathPoint
		{
			glm::vec3 Position = glm::vec3(0.0f);
			// True when this point exists only to satisfy a line-of-sight test
			// and can be removed if the surrounding segment still clears.
			bool IsCorner = false;
		};

		struct NavMeshConfig
		{
			// Horizontal cell size in metres. Smaller is more accurate and
			// costs memory as 1/cellSize^2.
			float CellSize = 0.5f;
			// How far the ground may fall away from the agent's feet and still
			// count as reachable.
			float AgentHeight = 2.0f;
			// Largest ledge the agent can step over.
			float StepHeight = 0.4f;
			// Shrink the walkable area by the agent radius. A cell whose centre
			// is closer than this to a blocker is not walkable, which is what
			// stops a path hugging a corner so tightly the agent's body clips it.
			float AgentRadius = 0.4f;
			// How far up to look for the ground when a point is sampled.
			float MaxSampleHeight = 4.0f;
		};

		// The result of a bake. Owns the grid and answers queries about it.
		class NavMesh
		{
		public:
			NavMesh() = default;

			// Rasterise the colliders in `scene` into a walkable grid.
			//
			// Returns false when the scene has no usable geometry or the bounds
			// came out degenerate, leaving the mesh empty. An empty mesh makes
			// every query fail cleanly rather than producing garbage paths.
			bool Bake(Scene &scene, const NavMeshConfig &config);

			// Drop everything. Safe to call on an already-empty mesh.
			void Clear();

			bool IsValid() const { return m_Columns > 0 && m_Rows > 0; }

			// Find a path between two world positions.
			//
			// Returns false when either endpoint is off the mesh, unreachable, or
			// the mesh is empty. `outPath` is only written on success.
			bool FindPath(const glm::vec3 &start, const glm::vec3 &goal,
				std::vector<NavPathPoint> &outPath) const;

			// True when a straight walk between two points is unobstructed.
			//
			// This is what string pulling tests, and it raycasts the real scene,
			// so "clear" means clear of actual collision.
			bool HasLineOfWalk(const glm::vec3 &from, const glm::vec3 &to) const;

		// Walkable-cell lookup by world position. Used by string pulling so a
		// shortcut is only accepted when every cell it crosses is walkable.
		bool IsWalkableAt(const glm::vec3 &point) const;

			// Nearest walkable cell to a world position, searched outward.
			// Lets an agent that has drifted slightly off-mesh still path.
			bool FindNearestWalkable(const glm::vec3 &point, glm::vec3 &out) const;

			// Ground height under a walkable position. False when off-mesh.
			bool SampleHeight(const glm::vec3 &point, float &outHeight) const;

			const NavMeshConfig &GetConfig() const { return m_Config; }
			size_t GetWalkableCellCount() const { return m_WalkableCount; }

			// Bounds of the baked area, in XZ.
			glm::vec2 GetMin() const { return m_Min; }
			glm::vec2 GetMax() const { return m_Max; }

		private:
			// --- grid addressing ---
			// Cells are addressed with a signed 32-bit index so the -1 "no cell"
			// sentinel is free. A grid far larger than 2^31 cells is not
			// something a bake will ever produce.
			int32_t CellIndex(int32_t col, int32_t row) const
			{
				return row * m_Columns + col;
			}
			bool InBounds(int32_t col, int32_t row) const
			{
				return col >= 0 && row >= 0 && col < m_Columns && row < m_Rows;
			}
			glm::vec2 CellCentre(int32_t col, int32_t row) const;
			bool IsWalkable(int32_t col, int32_t row) const;

			// --- pathfinding internals ---
			// Octile distance: the true cost of moving between two cells,
			// counting diagonals as sqrt(2) rather than 2.
			float Heuristic(int32_t aCol, int32_t aRow, int32_t bCol, int32_t bRow) const;
			bool Reconstruct(const std::vector<int32_t> &cameFrom,
				int32_t goalIndex, glm::vec3 startWorld,
				std::vector<NavPathPoint> &outPath) const;
			void PullString(const std::vector<glm::vec3> &raw,
				std::vector<NavPathPoint> &outPath) const;

			NavMeshConfig m_Config;
			// The scene this was baked from, kept so raycast-backed queries
			// (string pulling, line-of-walk) have collision to test against.
			// Null after Clear(); every such query then reports "unobstructed"
			// rather than dereferencing a dead pointer. A raw pointer, because
			// the mesh does not own the scene and must not keep it alive.
			const Scene *m_Scene = nullptr;
			int32_t m_Columns = 0;
			int32_t m_Rows = 0;
			glm::vec2 m_Min = glm::vec2(0.0f);
			glm::vec2 m_Max = glm::vec2(0.0f);
			// Ground height per cell; only meaningful where walkable.
			std::vector<float> m_Heights;
			// 1 = walkable, 0 = not. A separate array from heights so
			// "has a height" and "can be walked on" are not conflated.
			std::vector<uint8_t> m_Walkable;
			size_t m_WalkableCount = 0;
		};

	} // namespace Navigation
} // namespace Wasteland