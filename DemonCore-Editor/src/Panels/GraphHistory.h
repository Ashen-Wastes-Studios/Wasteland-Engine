#pragma once

// <deque> rather than <vector>: the history drops from the FRONT when
// it hits capacity, which is a pop_front operation. A vector would
// make every overflow an O(n) memmove of every snapshot.
#include <deque>
#include <string>

#include "Wasteland/VisualBlocks/Graph.h"

namespace Wasteland
{

	// Undo/redo for graph editing, by WHOLE-GRAPH SNAPSHOT.
	//
	// Not a command pattern. A command here would have to cover every
	// mutation the editor can make -- literals, titles, links, positions,
	// variables, blackboard, event names -- and each command would carry its
	// own inverse. That is a lot of code whose only job is to reconstruct a
	// struct, and every new edit path would be a chance to get the inverse
	// subtly wrong and corrupt a graph.
	//
	// A snapshot cannot be subtly wrong: it IS the graph. Cost is memory --
	// graphs are kilobytes -- and the cost is paid only while the author is
	// actually editing.
	//
	// Snapshots are pushed BEFORE a change, so redo always has somewhere to
	// go. The capacity is bounded; dropping the oldest entry is the only
	// acceptable policy, since losing REDO is recoverable and losing the
	// oldest UNDO silently changes what the author expects Ctrl+Z to reach.
	class GraphHistory
	{
	public:
		// Called with the pre-edit graph. Call this immediately BEFORE
		// mutating, not after: after is indistinguishable from the state
		// you were already in.
		// By const reference and COPYING into the deque -- the whole-graph
		// snapshot strategy in one line. The caller keeps mutating its own
		// graph afterwards without disturbing history.
		void Push(const VisualBlocks::Graph &graph);

		// Restore the previous state. False if there is none.
		// The graph comes OUT by reference rather than being written back to
		// the caller's graph directly, so a failed Undo leaves the caller's
		// copy untouched and there is no half-applied state.
		bool Undo(VisualBlocks::Graph &out);
		bool Redo(VisualBlocks::Graph &out);

		// Compares a depth COUNTER, not deque.empty(). These diverge by
		// design: CollapseIdentical drops an entry and decrements the
		// counter together, so they agree -- but the counter is what makes
		// the 'edit then revert' case describable at all.
		bool CanUndo() const { return m_UndoDepth > 0; }
		bool CanRedo() const { return m_RedoDepth > 0; }

		// Drops all history. Call after loading or creating a graph: undo
		// must not reach back across a file swap, which would restore a
		// graph belonging to a different file.
		void Clear();

		// Called when the author makes a NEW change. Any pending redo is
		// invalidated -- the future it described no longer happens.
		// Separate from Push rather than implicit in it: an author who
		// undoes and then types a new character must lose the redo branch,
		// but a caller assembling an undo entry programmatically may not
		// want that. Making it explicit keeps the policy at the call site.
		void BreakRedo();

		size_t UndoDepth() const { return m_UndoDepth; }
		size_t RedoDepth() const { return m_RedoDepth; }

		// Consecutive snapshots of the same state are collapsed: holding a
		// digit in a text field changes the graph every frame, and without
		// this one keystroke would eat the entire history.
		//
		// Call AFTER an edit. Serialising the whole graph to compare is far
		// cheaper than snapshotting on every keystroke and finding the
		// stack full of identical states.
		// Takes the POST-edit graph and compares it against the most recent
		// fingerprint, so the caller's ordering (edit, then Collapse) is
		// part of the contract.
		void CollapseIdentical(const VisualBlocks::Graph &graph);

	private:
		// Private, and the only way anything is compared. Returning a std::string
		// means the implementation is almost certainly Emit() from
		// GraphSerializer -- which also means the history's notion of 'same'
		// is exactly the serializer's notion, so the two cannot disagree.
		std::string Fingerprint(const VisualBlocks::Graph &graph) const;

		// Two independent deques rather than one stack with a cursor index.
		// A cursor would let an undo past the bottom wrap around into redo;
		// separate stacks make that unrepresentable.
		std::deque<VisualBlocks::Graph> m_Undo;
		std::deque<VisualBlocks::Graph> m_Redo;

		// Depth is tracked SEPARATELY from the deques so a mutation that
		// does not change the graph (an edit typed then reverted) can be
		// discarded without having pushed a duplicate.
		// DEFECT (redundant state): these two counters duplicate
		// m_Undo.size() / m_Redo.size() exactly. The comment above claims the
		// separation lets a no-op edit be 'discarded without having pushed a
		// duplicate', but that is precisely the case where the two stay EQUAL
		// -- so the counters carry no information the deques do not already
		// have, and every mutation site has to keep two values in step.
		// CanUndo() could return !m_Undo.empty().
		size_t m_UndoDepth = 0;
		size_t m_RedoDepth = 0;

		// Cached so CollapseIdentical is O(1) in the common case: only the
		// first snapshot after an edit has to compute a full serialisation.
		// Must be invalidated by Clear and by Redo -- if either forgets,
		// the next edit compares against a stale graph and a real undo entry
		// is silently discarded.
		std::string m_LastFingerprint;

		// 128 snapshots. At the kilobyte-per-graph size described above that
		// is a bounded, modest allocation -- but it is a CONSTANT, not a
		// setting, so a user with a large graph cannot trade memory for
		// history depth.
		static constexpr size_t kCapacity = 128;
	};

} // namespace Wasteland