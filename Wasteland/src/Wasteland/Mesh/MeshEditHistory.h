// MeshEditHistory.h -- whole-mesh undo for the mesh editor.
//
// WHY A SNAPSHOT AND NOT A COMMAND INVERSE
// ---------------------------------------
// The plan's rule, and the right one: an inverse command (extrude-inverse,
// bevel-inverse) has to reproduce the exact vertex order, edge order and twin
// links of the mesh it is undoing. Any operation that re-welds -- and extrude,
// inset, merge and flip all do -- permutes all three, so an inverse that
// rebuilds from a stored delta is reconstructing the mesh from a description
// that no longer applies. A snapshot cannot be subtly wrong: it IS the mesh.
//
// The cost is memory: an EditableMesh is a few vectors of POD structs, so a
// snapshot is roughly 40 bytes per half-edge. A 20k-triangle sculpt is ~240k
// half-edges, ~10 MB per snapshot. The stack is therefore BOUNDED, and the
// bound is a constructor parameter rather than a constant -- a prop artist and
// a sculpt artist need different depths.
//
// This lives in the engine rather than the panel so it can be tested without a
// window, and because it is not UI: it is a generic snapshot stack over a
// copyable mesh.
#pragma once

#include <cstdint>
#include <vector>

#include "Wasteland/Mesh/EditableMesh.h"

namespace Wasteland
{

	class MeshEditHistory
	{
	public:
		explicit MeshEditHistory(int32_t depth = 32)
			: m_Depth(depth > 0 ? depth : 1)
		{
		}

		// Records the state BEFORE an operation. Call it before mutating, not
		// after: the whole point is that the snapshot is the mesh the operation
		// is about to change.
		void Push(const EditableMesh &mesh)
		{
			m_Snapshots.push_back(mesh);
			if (static_cast<int32_t>(m_Snapshots.size()) > m_Depth)
			{
				// Erasing the front is O(n) in the depth, which is bounded and
				// small; a deque would be O(1) but would fragment the snapshots
				// across allocations, and the depth is not a hot path.
				m_Snapshots.erase(m_Snapshots.begin());
			}
		}

		// Restores the most recent snapshot and pops it. Returns false when
		// there is nothing to undo -- a caller must be able to tell "undo did
		// nothing" from "the mesh changed", so a bool rather than a void.
		bool Undo(EditableMesh &mesh)
		{
			if (m_Snapshots.empty())
				return false;
			mesh = m_Snapshots.back();
			m_Snapshots.pop_back();
			return true;
		}

		bool CanUndo() const { return !m_Snapshots.empty(); }

		int32_t GetDepth() const { return static_cast<int32_t>(m_Snapshots.size()); }
		int32_t GetMaxDepth() const { return m_Depth; }

		void Clear() { m_Snapshots.clear(); }

	private:
		int32_t m_Depth = 32;
		std::vector<EditableMesh> m_Snapshots;
	};

} // namespace Wasteland
