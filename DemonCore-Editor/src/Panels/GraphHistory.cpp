#include "GraphHistory.h"

// For ostringstream, which is how Fingerprint serialises a graph to compare
// it. A string concatenation would work but this is clearer and avoids
// repeated reallocation.
#include <sstream>
// Not used in this file as written -- Push, Undo and Redo all take const
// references or assign by value, with no pair, move or forward declaration.
// DEFECT: dead include.
#include <utility>

// Engine namespace, matching the VisualBlocks types the snapshots hold.
namespace Wasteland
{

	// A stable digest of everything that is SEMANTIC. Deliberately excludes
	// nothing: if two graphs serialise identically they behave identically,
	// so collapsing them loses no real edit.
	// Serialises a whole graph to one string so two states can be compared for
	// equality by operator==.
	//
	// const and taking a const reference: it reads the graph without modifying
	// it and without touching history state, which is why CollapseIdentical can
	// call it without disturbing the stacks.
	//
	// Called on every Push, every Undo, every Redo and every CollapseIdentical,
	// so this runs more often than anything else in the file. It is O(size of
	// graph) each time, making an edit O(graph size) rather than O(change).
	std::string GraphHistory::Fingerprint(const VisualBlocks::Graph &graph) const
	{
		// Built fresh per call and returned as a string. Not a member, so there is
		// no shared buffer to clear between calls -- the alternative would need a
		// reset and would make Fingerprint non-reentrant.
		std::ostringstream out;
		// Graph-level fields first. Each section is terminated by a one-character
		// separator, which is what stops adjacent fields from bleeding together.
		//
		// DEFECT: this separator scheme is not injective. Names, titles and literal
		// values are written RAW, so a graph with a node title containing ';' or
		// '|' can serialise to the same string as a different graph. The Literals
		// loop below uses '|' and '=' as separators while values are unescaped, and
		// DefaultLiteral is equally unescaped here. Two distinct graphs that compare
		// equal by fingerprint would have CollapseIdentical silently discard the
		// later one -- so an author could make a real edit that Ctrl+Z cannot
		// restore. Escaping the delimiters, or hashing field-by-field with explicit
		// lengths, would close it.
		out << graph.Kind << '|' << graph.Name << '|';
		// Range-for over the variable table, appending one 'V:' record each.
		//
		// Const reference per element: copying a Variable per iteration would be
		// wasteful on a path this hot.
		for (const auto &v : graph.Variables)
			// Type is cast to int because it is an enum class with no stream operator.
			// The cast is what makes the output stable -- printing the enum name would
			// change if a type were ever renamed, silently invalidating fingerprints
			// that were persisted.
			//
			// Two statements of one logical record, wrapped: the ostream chain is split
			// across lines purely for line length.
			out << "V:" << v.Name << ':' << static_cast<int>(v.Type) << ':'
				<< v.DefaultLiteral << ';';
		// The blackboard gets its own 'B:' prefix. The prefix is what keeps a
		// blackboard entry from colliding with a variable of identical name and
		// type.
		for (const auto &b : graph.Blackboard)
			out << "B:" << b.Name << ':' << static_cast<int>(b.Type) << ':'
				<< b.DefaultLiteral << ';';
		// Nodes are the bulk of the fingerprint. Iterated in container order, so
		// the fingerprint depends on the order nodes happen to be stored in.
		//
		// That is fine as long as the order is stable for a given logical graph --
		// which it will be for a graph built by the same sequence of operations. But
		// reordering nodes without changing anything semantic produces a different
		// fingerprint, so a purely cosmetic reorder looks like an edit. The header
		// calls this a digest of everything SEMANTIC; node order is not semantic.
		// Sorting by n.Id before serialising would match that claim.
		for (const auto &n : graph.Nodes)
		{
			// One record per node: id, type, title, then position and flags.
			//
			// n.Id and n.TypeId are both written, so two nodes differing only in which
			// behaviour they instantiate fingerprint differently -- correct, since that
			// is semantic.
			out << "N:" << n.Id << ':' << n.TypeId << ':' << n.Title << ':'
				// Node position IS included. That is deliberate rather than an oversight:
				// dragging a node is a change the author expects Ctrl+Z to undo, so
				// treating it as semantic is right. The cost is that a drag generates a
				// history entry per frame of movement rather than one per drag.
				//
				// X and Y are streamed as floats, so the text depends on the stream's
				// precision. Both sides of a comparison come from the same process, so this
				// is stable in practice -- but it does mean fingerprints are not portable
				// between builds with different float formatting.
				<< n.X << ',' << n.Y << ':' << n.EventName << ':'
				// The two execution flags. Bools stream as 0 or 1 by default, which is
				// compact and unambiguous here.
				//
				// Included because both change behaviour: a latent node does not execute,
				// and an abort-capable node can end the graph.
				<< n.Latent << ':' << n.Abort;
			// Per-node literal overrides, the most common thing an author edits --
			// hence the part CollapseIdentical exists to protect.
			for (const auto &l : n.Literals)
				// Key, value and whether it is set. IsSet matters as much as the value: a
				// literal explicitly cleared to empty must not fingerprint the same as one
				// never assigned.
				//
				// This is where the unescaped-delimiter defect bites hardest. l.Value is
				// written raw, so a literal value containing ';' terminates the node record
				// early, and the next node's 'N:' text gets appended as if it were part of
				// this node. Two graphs differing only in such a literal can then produce
				// the same string.
				out << "|L:" << l.Key << '=' << l.Value << ':' << l.IsSet;
			// Child-node ids, nested inside the parent node's record.
			//
			// DEFECT: the children are written in stored order and never sorted, so the
			// same set of children in a different order fingerprints differently.
			// Combined with node order above, this means reordering is not detected as
			// cosmetic even when it is.
			for (const auto &c : n.ChildIds)
				out << "|C:" << c;
			out << ';';
		}
		// Links last, at graph level. They reference nodes by id rather than
		// living inside them, so they cannot be nested in a node record.
		for (const auto &l : graph.Links)
			// 'K:' marks a link, keeping it distinct from the 'N:'/'V:'/'B:' sections.
			//
			// The explicit "->" between endpoints is what disambiguates the dot
			// separators: FromNode '.' FromPin is a different string from a single node
			// id that happens to contain a dot, because the arrow fixes where the node id
			// ends.
			out << "K:" << l.FromNode << '.' << l.FromPin << "->"
				<< l.ToNode << '.' << l.ToPin << ';';
		// str() on an ostringstream returns a COPY of the buffer, moving out of it --
		// so returning it directly is efficient, unlike the const char* overload
		// which would dangle the moment the stream died.
		return out.str();
	}

	// Records the pre-edit state. The header is explicit that this must be
	// called BEFORE the mutation.
	//
	// @param graph  The graph as it is BEFORE the edit about to be made.
	void GraphHistory::Push(const VisualBlocks::Graph &graph)
	{
		// A new edit invalidates the future. Done here as well as in
		// BreakRedo() so a caller that forgets the latter still cannot
		// redo into a state that never followed from the current one.
		// A new edit invalidates the future. Repeated here as well as in
		// BreakRedo() so a caller that forgets the latter still cannot redo into a
		// state that never followed from the current one.
		m_Redo.clear();
		// The counter is reset alongside the deque. Both are maintained in
		// lockstep everywhere in this file; that redundancy is deliberate per the
		// header's comment on the depth fields.
		m_RedoDepth = 0;

		// The deque takes a COPY of the graph. Correct here precisely because the
		// caller pushes before mutating -- a reference would alias and record the
		// post-edit state.
		//
		// This is the memory cost the header's class comment accepts: kilobytes per
		// snapshot, bounded by kCapacity.
		m_Undo.push_back(graph);
		// Evicts the OLDEST entry once the bound is exceeded.
		//
		// DEFECT: m_UndoDepth is NOT decremented here, unlike every other place the
		// eviction policy appears. Compare Redo() at line 100, which pops from
		// m_Undo and does not adjust the depth either -- but Undo() does decrement
		// it. The depth counter is therefore allowed to drift above the deque size.
		// Since CanUndo() tests m_UndoDepth > 0 rather than !m_Undo.empty(), an
		// evicted-and-empty undo stack still reports that undo is available, and the
		// first Undo() call returns false. A user would see Ctrl+Z enabled and
		// nothing happen. Incrementing past the bound also means the depth no longer
		// answers UndoDepth()'s apparent question.
		if (m_Undo.size() > kCapacity)
			m_Undo.pop_front();
		// Unconditionally incremented -- unlike Redo()'s guarded increment, which
		// only counts up to kCapacity. The asymmetry is what lets the depth drift.
		++m_UndoDepth;

		// Stores the digest of what was just pushed. CollapseIdentical compares
		// against this, so it is set on every state-changing entry point: Push,
		// Undo, Redo and Clear.
		m_LastFingerprint = Fingerprint(graph);
	}

	// Drops the most recent snapshot when the current state turns out to be
	// what was already recorded.
	//
	// @param graph  The graph AFTER the edit.
	//
	// This is what makes per-keystroke pushing affordable in a text field: one
	// keystroke that changes nothing meaningful costs one fingerprint, not one
	// history entry.
	void GraphHistory::CollapseIdentical(const VisualBlocks::Graph &graph)
	{
		// Serialised once and reused for both the comparison and the store below.
		const std::string now = Fingerprint(graph);
		// The equality test that drives everything. String equality on the whole
		// fingerprint, which is why the escaping defect in Fingerprint is a
		// correctness problem rather than merely a missed optimisation.
		if (now == m_LastFingerprint)
			// Nothing to do -- the state is already recorded, so the snapshot on top
			// would be a duplicate.
			return;

		// The snapshot pushed for this edit turns out to hold the state we
		// are already in, so it is dead weight. Drop it and re-point the
		// fingerprint at the real current state.
		// DEFECT: these two conditions are checked independently, so they can
		// disagree -- and the header explains that is possible. m_UndoDepth is
		// decremented in Undo() and in the pop below, but pushed back up in Redo(),
		// and never adjusted when Push evicts at kCapacity. A depth larger than the
		// deque size would let this branch pop when it should not. Using the deque's
		// own emptiness as the single source of truth, and sizing the depth from it,
		// would remove the possibility rather than guarding against it.
		if (!m_Undo.empty() && m_UndoDepth > 0)
		{
			// Removes the duplicate snapshot. pop_back is O(1) on a deque, unlike
			// pop_front at the capacity check -- the asymmetry is why the eviction at
			// the top of Push and the discard here have different costs.
			m_Undo.pop_back();
			--m_UndoDepth;
		}
		m_LastFingerprint = now;
	}

	// Restores the previous state.
	//
	// @param out  IN: the current graph, to become the redo target.
	//             OUT: the state being restored to.
	//
	// In/out by reference rather than returning the graph, because VisualBlocks::
	// Graph is copied wholesale either way and the caller already holds the
	// object to overwrite.
	bool GraphHistory::Undo(VisualBlocks::Graph &out)
	{
		// The real availability test. Note it checks the deque, not m_UndoDepth as
		// CanUndo() does -- so CanUndo() can say yes to this returning false, as
		// the eviction defect in Push allows.
		if (m_Undo.empty())
			// No history to undo. False is the signal for 'leave out untouched', which
			// the caller must honour -- out is not written on this path.
			return false;

		// Current state becomes the redo target. Taken BEFORE popping, so
		// the popped entry is the pre-edit state and the pushed one is the
		// state being left.
		// The CURRENT state, pushed as the redo target BEFORE the pop below.
		//
		// Order is the whole trick, and the comment above says so: push first, so
		// the entry added is the state being left, and the entry popped is the
		// pre-edit state. Reversing these two lines would make undo and redo
		// symmetric but wrong.
		m_Redo.push_back(out);
		if (m_Redo.size() > kCapacity)
			m_Redo.pop_front();
		// Unguarded increment, so the redo depth too can exceed the deque size
		// after repeated evictions.
		++m_RedoDepth;

		// Copies the snapshot into the caller's graph. A deep copy of every node,
		// link and literal -- the cost the snapshot design trades for correctness.
		out = m_Undo.back();
		// Consumes the entry just restored, so a second Undo() moves further back
		// rather than re-applying the same state.
		m_Undo.pop_back();
		if (m_UndoDepth > 0)
			--m_UndoDepth;

		m_LastFingerprint = Fingerprint(out);
		return true;
	}

	// Re-applies an undone state. The mirror of Undo(), with the same in/out
	// contract.
	//
	// @param out  IN: the current graph, to become an undo target.
	//             OUT: the state being re-applied.
	bool GraphHistory::Redo(VisualBlocks::Graph &out)
	{
		if (m_Redo.empty())
			return false;

		// The state being left becomes an undo point, exactly mirroring Undo()'s
		// redo push.
		m_Undo.push_back(out);
		if (m_Undo.size() > kCapacity)
			m_Undo.pop_front();
		// DEFECT: Redo() GUARDS its undo-depth increment against kCapacity while
		// Push() does not, and Undo() guards nothing. Three entry points, three
		// different policies for the same counter.
		//
		// The guard means a redo into a full undo stack leaves m_UndoDepth
		// reporting a smaller history than m_Undo actually holds, so CanUndo() can
		// say no while Undo() would still succeed. Combined with the unguarded
		// increment in Push, the depth is not a reliable count of either deque in
		// any state.
		if (m_UndoDepth < kCapacity)
			++m_UndoDepth;

		// The state to re-apply. Asymmetric with Undo's order on purpose: here the
		// undo push happens first and the redo pop last, so a failed call midway
		// through would leave the stacks inconsistent -- there is no early return
		// between these lines.
		out = m_Redo.back();
		m_Redo.pop_back();
		if (m_RedoDepth > 0)
			--m_RedoDepth;

		m_LastFingerprint = Fingerprint(out);
		return true;
	}

	// Drops everything. The header is explicit that this must be called after
	// loading or creating a graph, so undo cannot reach back across a file
	// swap and restore a graph belonging to a different file.
	void GraphHistory::Clear()
	{
		m_Undo.clear();
		m_Redo.clear();
		m_UndoDepth = 0;
		m_RedoDepth = 0;
		// The fingerprint is cleared too, not left stale. That matters for
		// CollapseIdentical: an empty fingerprint matches no real graph, so the
		// next call after a Clear takes the pop branch -- and its guard
		// !m_Undo.empty() correctly prevents popping an empty stack.
		m_LastFingerprint.clear();
	}

	// Invalidates the future without touching the undo side. Push() does the
	// same two lines inline, which is what makes this method optional rather
	// than mandatory.
	void GraphHistory::BreakRedo()
	{
		m_Redo.clear();
		m_RedoDepth = 0;
	}

} // namespace Wasteland