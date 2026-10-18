#include "wlpch.h"

#include "Wasteland/Navigation/NavMesh.h"

#include <queue>

#include "Wasteland/Scene/Components.h"
#include "Wasteland/Scene/Entity.h"
#include "Wasteland/Scene/Scene.h"

namespace Wasteland
{
	namespace Navigation
	{
		namespace
		{
			// A* frontier entry. `f` is cached so the priority queue does not
			// have to re-read the score array.
			struct OpenNode
			{
				float f = 0.0f;
				int32_t index = -1;
			};

			struct OpenGreater
			{
				bool operator()(const OpenNode &a, const OpenNode &b) const
				{
					// Ties break on index so the search order is deterministic.
					// Without this, equal-cost paths come out in whatever order
					// the heap happens to produce, and a test asserting an exact
					// path shape becomes flaky for no visible reason.
					if (a.f != b.f)
						return a.f > b.f;
					return a.index > b.index;
				}
			};

			// Euler (radians, X then Y then Z, identity (0,0,0)) to a
			// quaternion. TransformComponent stores Rotation as Euler, so any
			// rotation of an offset into world space needs this conversion
			// first -- glm::quat_cast on the Euler vector compiles to nothing
			// useful and silently produces an identity rotation.
			glm::quat EulerToQuaternion(const glm::vec3 &euler)
			{
				glm::quat q;
				const glm::vec3 c(std::cos(euler.x * 0.5f), std::cos(euler.y * 0.5f), std::cos(euler.z * 0.5f));
				const glm::vec3 s(std::sin(euler.x * 0.5f), std::sin(euler.y * 0.5f), std::sin(euler.z * 0.5f));
				q.w = c.x * c.y * c.z - s.x * s.y * s.z;
				q.x = s.x * c.y * c.z + c.x * s.y * s.z;
				q.y = c.x * s.y * c.z - s.x * c.y * s.z;
				q.z = c.x * c.y * s.z + s.x * s.y * c.z;
				return q;
			}

			// One collider's footprint, in world space.
			struct Span
			{
				glm::vec3 Centre;   // world centre of the shape
				glm::vec3 Half;     // half-extents (sphere uses X for radius)
				bool IsSphere = false;
			};

			// True when an agent standing with its feet at `groundTop` at
			// `centreXZ` fits: nothing may occupy the body volume above the
			// feet, and the cell must not be inside a collider's footprint
			// (inflated by the agent radius).
			bool HasHeadroom(const std::vector<Span> &spans, float groundTop,
				const glm::vec2 &centreXZ, const NavMeshConfig &cfg)
			{
				const float agentTop = groundTop + cfg.AgentHeight;
				for (const Span &span : spans)
				{
					const float bottom = span.Centre.y - (span.IsSphere ? span.Half.x : span.Half.y);
					const float top = span.IsSphere
						? (span.Centre.y + span.Half.x)
						: (span.Centre.y + span.Half.y);

					// No vertical overlap with the body volume. The epsilon
					// matters: a floor the agent is standing ON has top ==
					// groundTop, and without it the ground would block itself.
					if (top <= groundTop + 0.01f || bottom >= agentTop)
						continue;

					const glm::vec2 delta = glm::abs(
						centreXZ - glm::vec2(span.Centre.x, span.Centre.z));
					const float radius = span.IsSphere ? span.Half.x : 0.0f;

					if (span.IsSphere)
					{
						if (delta.x <= radius + cfg.AgentRadius &&
							delta.y <= radius + cfg.AgentRadius)
							return false;
					}
					else
					{
						if (delta.x <= span.Half.x + cfg.AgentRadius &&
							delta.y <= span.Half.z + cfg.AgentRadius)
							return false;
					}
				}
				return true;
			}

			// True when a surface at `top` is something an agent can actually
			// STAND ON: reached by a rise the agent can actually climb from the
			// ground it already occupies, and not a thin ledge it would fall off.
			//
			// This is what separates a floor from the top of a wall. Both are
			// "the highest solid here"; only one is somewhere an agent's feet
			// can be. HasHeadroom cannot tell them apart because it starts from
			// an assumed ground height -- feed it a wall top and it reports
			// clearance for a surface nothing can reach.
			bool IsGround(const std::vector<Span> &spans, float top,
						  const glm::vec2 &centreXZ, const NavMeshConfig &cfg)
			{
				for (const Span &span : spans)
				{
					const float spanTop = span.IsSphere
						? (span.Centre.y + span.Half.x)
						: (span.Centre.y + span.Half.y);
					// Only spans that actually produce THIS surface.
					if (std::abs(spanTop - top) > 0.01f)
						continue;

					// glm::abs, NOT std::abs: for a glm::vec2 the std overload set
					// resolves to the long double one and fails to compile.
					const glm::vec2 delta = glm::abs(
						centreXZ - glm::vec2(span.Centre.x, span.Centre.z));
					const bool inside = span.IsSphere
						? (delta.x <= span.Half.x + cfg.AgentRadius &&
						   delta.y <= span.Half.x + cfg.AgentRadius)
						: (delta.x <= span.Half.x + cfg.AgentRadius &&
						   delta.y <= span.Half.z + cfg.AgentRadius);
					if (!inside)
						continue;

					// The agent is standing at `top`, so its feet are ON this
					// span. If the span is a wall -- i.e. its own top is well
					// above the ground beside it -- then this is a rooftop, not
					// a surface reachable by walking. Compare against the LOWEST
					// candidate at this cell, which is the floor an agent would
					// actually be on.
					const float thickness = span.IsSphere
						? (2.0f * span.Half.x)
						: (2.0f * span.Half.y);
					// A platform thin enough to be a step-up is walkable; one
					// taller than the agent can climb is not.
					if (thickness > cfg.StepHeight + cfg.AgentHeight)
						return false;
				}
				return true;
			}

			constexpr float kNeighbourStraight = 1.0f;
			constexpr float kNeighbourDiagonal = 1.41421356f;
			// Path requests are bounded so a pathological request on a huge grid
			// cannot hang a frame. A 512x512 grid is 262144 cells, which is far
			// more than any level here needs.
			constexpr int32_t kMaxExpansions = 1 << 18;
		} // namespace

		// -------------------------------------------------------------------
		// Baking

		void NavMesh::Clear()
		{
			m_Columns = 0;
			m_Rows = 0;
			m_Scene = nullptr;
			m_Min = glm::vec2(0.0f);
			m_Max = glm::vec2(0.0f);
			m_Heights.clear();
			m_Walkable.clear();
			m_WalkableCount = 0;
		}

		glm::vec2 NavMesh::CellCentre(int32_t col, int32_t row) const
		{
			return glm::vec2(
				m_Min.x + (static_cast<float>(col) + 0.5f) * m_Config.CellSize,
				m_Min.y + (static_cast<float>(row) + 0.5f) * m_Config.CellSize);
		}

		bool NavMesh::Bake(Scene &scene, const NavMeshConfig &config)
		{
			Clear();
			if (config.CellSize <= 0.0f || !std::isfinite(config.CellSize))
				return false;

			m_Config = config;
			m_Scene = &scene;

			// --- gather the colliders that can actually be stood on ---
			//
			// Only Box and Sphere colliders are considered, and only from
			// entities that have a Transform, because a collider with no place
			// in the world contributes nothing to a surface.
			std::vector<Span> spans;

			glm::vec2 lo(std::numeric_limits<float>::max());
			glm::vec2 hi(-std::numeric_limits<float>::max());

			auto consider = [&](const glm::vec3 &centre, const glm::vec3 &half, bool isSphere) {
				spans.push_back({centre, half, isSphere});
				// XZ footprint only; Y is sampled, not bounded, so a tall tower
				// does not inflate the grid into uselessness.
				lo = glm::min(lo, glm::vec2(centre.x - half.x, centre.y - half.z));
				hi = glm::max(hi, glm::vec2(centre.x + half.x, centre.y + half.z));
			};

			// Entity carries the scene reference and does the registry lookup,
			// so this walk reads components the same way CreatePhysicsBody3D
			// does. Doing it any other way means reaching into Scene's private
			// registry, which is exactly the coupling to avoid.
			for (entt::entity rawEntity : scene.GetAllEntitiesWith<TransformComponent>())
			{
				Entity viewEntity{rawEntity, &scene};
				if (!viewEntity.HasComponent<TransformComponent>())
					continue;

				const TransformComponent &transform = viewEntity.GetComponent<TransformComponent>();
				const glm::vec3 world = transform.Translation;

				auto considerCollider = [&](glm::vec3 half, bool isSphere) {
					// The collider offset is in the entity's local frame, so it
					// has to be rotated by the entity's rotation to reach world
					// space. Half-extents are deliberately NOT rotated: a rotated
					// box's AABB is larger, and inflating here would only make
					// the mesh conservative without making it correct.
					glm::vec3 offset(0.0f);
					if (isSphere)
					{
						const SphereCollider3DComponent &sphere = viewEntity.GetComponent<SphereCollider3DComponent>();
						offset = sphere.Offset;
						half = glm::vec3(sphere.Radius, sphere.Radius, sphere.Radius);
					}
					else
					{
						const BoxCollider3DComponent &box = viewEntity.GetComponent<BoxCollider3DComponent>();
						offset = box.Offset;
						half = box.HalfExtents * transform.Scale;
					}
					offset = glm::mat3_cast(glm::quat(EulerToQuaternion(transform.Rotation))) * offset;
					consider(world + offset, half, isSphere);
				};

				if (viewEntity.HasComponent<BoxCollider3DComponent>())
					considerCollider(glm::vec3(0.0f), false);
				else if (viewEntity.HasComponent<SphereCollider3DComponent>())
					considerCollider(glm::vec3(0.0f), true);
			}

			if (spans.empty())
				return false;

			// --- size the grid ---
			// Pad by the agent radius so the outermost cell centre is still a
			// legal standing position rather than sitting exactly on the edge.
			lo -= glm::vec2(m_Config.AgentRadius);
			hi += glm::vec2(m_Config.AgentRadius);

			m_Min = lo;
			m_Max = hi;
			m_Columns = static_cast<int32_t>(std::ceil((hi.x - lo.x) / m_Config.CellSize));
			m_Rows = static_cast<int32_t>(std::ceil((hi.y - lo.y) / m_Config.CellSize));
			if (m_Columns <= 0 || m_Rows <= 0)
			{
				Clear();
				return false;
			}

			const size_t total = static_cast<size_t>(m_Columns) * static_cast<size_t>(m_Rows);
			m_Heights.assign(total, 0.0f);
			m_Walkable.assign(total, 0u);

			// --- rasterise ---
			//
			// Per cell, collect EVERY collider top within the cell footprint and
			// pick the HIGHEST one that has headroom for an agent above it.
			//
			// The obvious implementation -- take the single highest top, then ask
			// if anything blocks it -- is wrong, and wrong in a way that
			// silently amputates the level. Consider a wall standing on a floor:
			// inside the wall's footprint the highest top is the wall's own top,
			// which is always blocked, so the cell is discarded... and so is
			// every cell whose only candidate was that same surface. A 6m wall on
			// a 20m floor then reads as a wall that severs the entire level,
			// because the floor underneath it is never considered as a separate
			// candidate.
			//
			// So: try each candidate top from highest to lowest, and keep the
			// first with clearance. The floor under a wall is a lower candidate
			// that passes the same test, which is why an agent can path beside
			// the wall rather than around a phantom chasm.
			std::vector<float> candidateTops;
			candidateTops.reserve(spans.size());

			for (int32_t row = 0; row < m_Rows; ++row)
			{
				for (int32_t col = 0; col < m_Columns; ++col)
				{
					const glm::vec2 centre = CellCentre(col, row);
					const glm::vec2 probeXZ(centre.x, centre.y);

					candidateTops.clear();
					for (const Span &span : spans)
					{
						const glm::vec2 delta = glm::abs(probeXZ - glm::vec2(span.Centre.x, span.Centre.z));
						const float r = span.IsSphere ? span.Half.x : 0.0f;
						if (span.IsSphere)
						{
							if (delta.x > r || delta.y > r)
								continue;
						}
						else
						{
							if (delta.x > span.Half.x || delta.y > span.Half.z)
								continue;
						}

						// A sphere's surface curves, so the top varies across
						// the cell; take it at the cell centre, which is where
						// the agent will actually stand.
						const float top = span.IsSphere
							? (span.Centre.y + std::sqrt(std::max(0.0f,
								r * r - (delta.x * delta.x + delta.y * delta.y))))
							: (span.Centre.y + span.Half.y);

						if (top <= m_Config.MaxSampleHeight)
							candidateTops.push_back(top);
					}

					if (candidateTops.empty())
						continue;

					// Highest first, so a ledge wins over the floor beneath it.
					std::sort(candidateTops.begin(), candidateTops.end(), std::greater<float>());

					float chosen = 0.0f;
					bool haveGround = false;
					for (const float top : candidateTops)
					{
						if (HasHeadroom(spans, top, probeXZ, m_Config))
						{
							chosen = top;
							haveGround = true;
							break;
						}
					}

					if (!haveGround)
						continue;

					// The chosen surface must be GROUND, not merely the top of
					// whatever solid is here.
					//
					// The candidate loop above is not enough on its own: a wall
					// standing on a floor contributes its own top as a candidate,
					// and that top trivially passes the headroom test -- an agent
					// could stand on the wall if it could reach the top, and
					// HasHeadroom only asks whether the body volume is clear, which
					// it is. What actually makes the wall top unwalkable is that
					// getting there requires a step far taller than any agent can
					// climb. So this is the same step-height rule the traversal
					// uses, applied at bake time to the surface itself.
					//
					// Without it the mesh marks the wall's own roof walkable and
					// A* routes straight over the obstacle, which is the exact
					// failure the detour test exists to catch.
					if (!IsGround(spans, chosen, probeXZ, m_Config))
						continue;

					const int32_t idx = CellIndex(col, row);
					m_Heights[idx] = chosen;
					m_Walkable[idx] = 1u;
					++m_WalkableCount;
				}
			}

			return m_WalkableCount > 0;
		}

		bool NavMesh::IsWalkable(int32_t col, int32_t row) const
		{
			if (!InBounds(col, row))
				return false;
			return m_Walkable[CellIndex(col, row)] != 0u;
		}

		// -------------------------------------------------------------------
		// Queries

		bool NavMesh::SampleHeight(const glm::vec3 &point, float &outHeight) const
		{
			if (!IsValid())
				return false;
			const int32_t col = static_cast<int32_t>(std::floor((point.x - m_Min.x) / m_Config.CellSize));
			const int32_t row = static_cast<int32_t>(std::floor((point.z - m_Min.y) / m_Config.CellSize));
			if (!InBounds(col, row))
				return false;
			const int32_t idx = CellIndex(col, row);
			if (m_Walkable[idx] == 0u)
				return false;
			outHeight = m_Heights[idx];
			return true;
		}

		bool NavMesh::FindNearestWalkable(const glm::vec3 &point, glm::vec3 &out) const
		{
			if (!IsValid())
				return false;

			const int32_t col = static_cast<int32_t>(std::floor((point.x - m_Min.x) / m_Config.CellSize));
			const int32_t row = static_cast<int32_t>(std::floor((point.z - m_Min.y) / m_Config.CellSize));

			// Expanding ring search. Bounded so a point far outside the mesh
			// returns false instead of scanning the whole grid.
			constexpr int32_t kMaxRing = 24;
			for (int32_t ring = 0; ring <= kMaxRing; ++ring)
			{
				for (int32_t dr = -ring; dr <= ring; ++dr)
				{
					for (int32_t dc = -ring; dc <= ring; ++dc)
					{
						// Only the ring's edge, not the filled square, or this
						// rescans the interior at every radius.
						if (std::abs(dr) != ring && std::abs(dc) != ring)
							continue;
						const int32_t c = col + dc;
						const int32_t r = row + dr;
						if (!IsWalkable(c, r))
							continue;
						const glm::vec2 centre = CellCentre(c, r);
						out = glm::vec3(centre.x, m_Heights[CellIndex(c, r)], centre.y);
						return true;
					}
				}
			}
			return false;
		}

		float NavMesh::Heuristic(int32_t aCol, int32_t aRow, int32_t bCol, int32_t bRow) const
		{
			const float dx = static_cast<float>(std::abs(aCol - bCol));
			const float dy = static_cast<float>(std::abs(aRow - bRow));
			// Octile: diagonal steps cost sqrt(2), so the straight-axis
			// remainder is charged at 1 and the diagonal at 1.414.
			const float lo = std::min(dx, dy);
			const float hi = std::max(dx, dy);
			return (hi - lo) + lo * kNeighbourDiagonal;
		}

		bool NavMesh::Reconstruct(const std::vector<int32_t> &cameFrom,
			int32_t goalIndex, glm::vec3 startWorld,
			std::vector<NavPathPoint> &outPath) const
		{
			std::vector<int32_t> chain;
			int32_t cursor = goalIndex;
			// Guard against a malformed cameFrom producing a cycle.
			int32_t guard = 0;
			const int32_t guardMax = m_Columns * m_Rows + 1;
			while (cursor >= 0 && guard++ < guardMax)
			{
				chain.push_back(cursor);
				cursor = cameFrom[static_cast<size_t>(cursor)];
			}
			if (cursor >= 0)
				return false;

			std::reverse(chain.begin(), chain.end());
			if (chain.empty())
				return false;

			outPath.clear();
			// The agent's actual position, not the cell centre it snapped to.
			// Starting the path at the cell centre would make the agent walk
			// backwards to its own cell before setting off.
			outPath.push_back({startWorld, false});
			for (size_t i = 0; i < chain.size(); ++i)
			{
				const int32_t idx = chain[i];
				const int32_t col = idx % m_Columns;
				const int32_t row = idx / m_Columns;
				const glm::vec2 centre = CellCentre(col, row);
				const bool isGoal = (i + 1 == chain.size());
				NavPathPoint p;
				p.Position = glm::vec3(centre.x, m_Heights[static_cast<size_t>(idx)], centre.y);
				p.IsCorner = !isGoal;
				outPath.push_back(p);
			}
			return true;
		}

		void NavMesh::PullString(const std::vector<glm::vec3> &raw,
			std::vector<NavPathPoint> &outPath) const
		{
			outPath.clear();
			if (raw.empty())
				return;

			size_t anchor = 0;
			NavPathPoint first;
			first.Position = raw.front();
			first.IsCorner = false;
			outPath.push_back(first);

			// Classic string pulling: from the last accepted anchor, walk
			// forward as far as the straight line stays clear, then commit that
			// point as the next corner. The result is the shortest sequence of
			// straight legs that the agent can actually walk, instead of one
			// waypoint per cell -- which is what makes an agent walk diagonally
			// across open floor instead of following a staircase of cells.
			while (anchor + 1 < raw.size())
			{
				size_t furthest = anchor + 1;
				for (size_t probe = raw.size(); probe > anchor + 1; --probe)
				{
					const size_t candidate = probe - 1;
					if (HasLineOfWalk(raw[anchor], raw[candidate]))
					{
						furthest = candidate;
						break;
					}
				}

				const bool isGoal = (furthest + 1 == raw.size());
				NavPathPoint p;
				p.Position = raw[furthest];
				p.IsCorner = !isGoal;
				outPath.push_back(p);
				anchor = furthest;
			}

			// The goal is always last, even if pulling never reached it (which
			// only happens on a single-point raw path, handled above).
			if (outPath.size() < 2 && raw.size() >= 2)
			{
				NavPathPoint goal;
				goal.Position = raw.back();
				goal.IsCorner = false;
				outPath.push_back(goal);
			}
		}

		// Can an agent walk straight from `from` to `to` without leaving the
		// walkable set?
		//
		// This tests the BAKED GRID, not the physics world, and that is the
		// whole correction. Raycasting here looks reasonable -- a navmesh has
		// geometry, so trace a line and see if it hits something -- and it is
		// wrong in two ways:
		//
		//   1. It needs a physics world. In a headless test, or before
		//      OnSimulationStart, there is none: the raycast returns no hit,
		//      every line reports clear, and string pulling collapses the
		//      entire route to two points -- straight through the wall. The
		//      detour test then fails while the mesh is provably correct.
		//   2. It asks the wrong question. The mesh already knows which cells
		//      are walkable; re-deriving that from colliders can disagree with
		//      it, and when they disagree the puller erases the one piece of
		//      information the search produced.
		//
		// Walking the cells the straight line crosses keeps pulling consistent
		// with A*, which is the only property that matters: pulling may
		// SHORTEN a route, never change where it goes.
		bool NavMesh::HasLineOfWalk(const glm::vec3 &from, const glm::vec3 &to) const
		{
			const glm::vec3 delta = to - from;
			const float horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
			if (horizontal < 1e-4f)
				return true;

			if (!IsValid())
				return false;

			// Both ends must sit on walkable ground, or the segment is not a
			// legal route regardless of what it crosses.
			if (!IsWalkableAt(from) || !IsWalkableAt(to))
				return false;

			// Step no finer than half a cell: at a quarter-cell step this
			// degenerates into "is every point walkable", which is correct but
			// wasteful, and samples cells the agent's body would straddle.
			const float step = m_Config.CellSize * 0.5f;
			const int32_t steps = static_cast<int32_t>(horizontal / step);
			for (int32_t i = 1; i < steps; ++i)
			{
				const float t = static_cast<float>(i) / static_cast<float>(steps);
				const glm::vec3 probe(
					from.x + delta.x * t,
					from.y,
					from.z + delta.z * t);
				if (!IsWalkableAt(probe))
					return false;
			}
			return true;
		}

		// Walkable-cell lookup by world position, using the same cell mapping
		// as SampleHeight so the two agree on where a cell begins.
		bool NavMesh::IsWalkableAt(const glm::vec3 &point) const
		{
			if (!IsValid())
				return false;
			const int32_t col = static_cast<int32_t>(
				std::floor((point.x - m_Min.x) / m_Config.CellSize));
			const int32_t row = static_cast<int32_t>(
				std::floor((point.z - m_Min.y) / m_Config.CellSize));
			return IsWalkable(col, row);
		}


		bool NavMesh::FindPath(const glm::vec3 &start, const glm::vec3 &goal,
			std::vector<NavPathPoint> &outPath) const
		{
			outPath.clear();
			if (!IsValid() || m_WalkableCount == 0)
				return false;

			glm::vec3 startWalk;
			if (!FindNearestWalkable(start, startWalk))
				return false;
			glm::vec3 goalWalk;
			if (!FindNearestWalkable(goal, goalWalk))
				return false;

			const int32_t startCol = static_cast<int32_t>(std::floor((startWalk.x - m_Min.x) / m_Config.CellSize));
			const int32_t startRow = static_cast<int32_t>(std::floor((startWalk.z - m_Min.y) / m_Config.CellSize));
			const int32_t goalCol = static_cast<int32_t>(std::floor((goalWalk.x - m_Min.x) / m_Config.CellSize));
			const int32_t goalRow = static_cast<int32_t>(std::floor((goalWalk.z - m_Min.y) / m_Config.CellSize));

			if (!IsWalkable(startCol, startRow) || !IsWalkable(goalCol, goalRow))
				return false;

			const int32_t startIndex = CellIndex(startCol, startRow);
			const int32_t goalIndex = CellIndex(goalCol, goalRow);
			if (startIndex == goalIndex)
			{
				outPath.push_back({start, false});
				outPath.push_back({goalWalk, false});
				return true;
			}

			const size_t count = m_Walkable.size();
			std::vector<float> gScore(count, std::numeric_limits<float>::max());
			std::vector<int32_t> cameFrom(count, -1);
			std::vector<uint8_t> closed(count, 0u);

			std::priority_queue<OpenNode, std::vector<OpenNode>, OpenGreater> open;
			gScore[static_cast<size_t>(startIndex)] = 0.0f;
			open.push({Heuristic(startCol, startRow, goalCol, goalRow), startIndex});

			int32_t expansions = 0;
			bool found = false;
			while (!open.empty())
			{
				if (++expansions > kMaxExpansions)
					break;

				const int32_t current = open.top().index;
				open.pop();

				if (closed[static_cast<size_t>(current)])
					continue;
				closed[static_cast<size_t>(current)] = 1u;

				if (current == goalIndex)
				{
					found = true;
					break;
				}

				const int32_t col = current % m_Columns;
				const int32_t row = current / m_Columns;

				// 8-connected, but a diagonal is only legal when BOTH
				// orthogonal neighbours are open. Without that check an agent
				// squeezes through the diagonal gap between two colliders
				// without fitting, which is the classic corner-cutting bug.
				for (int32_t drow = -1; drow <= 1; ++drow)
				{
					for (int32_t dcol = -1; dcol <= 1; ++dcol)
					{
						if (dcol == 0 && drow == 0)
							continue;
						const int32_t nc = col + dcol;
						const int32_t nr = row + drow;
						if (!IsWalkable(nc, nr))
							continue;

						const bool diagonal = (dcol != 0 && drow != 0);
						if (diagonal &&
							(!IsWalkable(col + dcol, row) || !IsWalkable(col, row + drow)))
							continue;

						// Refuse to climb or drop more than a single step. This
						// is what stops a path from routing over a wall or off a
						// ledge, because both read as "neighbour cell is higher".
						const float here = m_Heights[static_cast<size_t>(current)];
						const float there = m_Heights[static_cast<size_t>(CellIndex(nc, nr))];
						if (std::abs(there - here) > m_Config.StepHeight)
							continue;

						const float step = diagonal ? kNeighbourDiagonal : kNeighbourStraight;
						const float tentative = gScore[static_cast<size_t>(current)] + step;
						const int32_t nIndex = CellIndex(nc, nr);
						if (tentative >= gScore[static_cast<size_t>(nIndex)])
							continue;

						gScore[static_cast<size_t>(nIndex)] = tentative;
						cameFrom[static_cast<size_t>(nIndex)] = current;
						open.push({tentative + Heuristic(nc, nr, goalCol, goalRow), nIndex});
					}
				}
			}

			if (!found)
				return false;

			// Reconstruct yields full points (with the IsCorner flag);
			// PullString works on plain positions and re-derives the flags,
			// because pulling can drop a point entirely. Converting here keeps
			// each function's contract single-purpose.
			std::vector<NavPathPoint> reconstructed;
			if (!Reconstruct(cameFrom, goalIndex, start, reconstructed))
				return false;

			std::vector<glm::vec3> raw;
			raw.reserve(reconstructed.size());
			for (const NavPathPoint &p : reconstructed)
				raw.push_back(p.Position);

			PullString(raw, outPath);
			return !outPath.empty();
		}

	} // namespace Navigation
} // namespace Wasteland