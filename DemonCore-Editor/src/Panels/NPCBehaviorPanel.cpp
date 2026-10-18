// PCH first: it establishes <string>/<vector> and the logging macros before
// anything else declares a symbol from them.
#include "wlpch.h"
// The panel's own header, which pulls in Graph.h and Validator.h -- so Graph,
// GraphNode, GraphError and ValidationResult are all available without being
// included here.
#include "NPCBehaviorPanel.h"

// The panel's only rendering dependency.
//
// Note this is the PANEL file, not the runtime: the behaviour-tree data
// structures live in Wasteland/VisualBlocks under the engine, and nothing in
// this file is compiled into a shipped game.
#include <imgui/imgui.h>

// For std::find_if and the remove/remove_if erase-remove idiom used in
// DeleteNode and Reparent.
#include <algorithm>
// For snprintf, used to copy a literal into the fixed char buffer the ImGui
// string widget requires.
#include <cstdio>
// For the path manipulation in CompileAndWrite: constructing the .py path from
// the .wlbtree path and removing stale .pyc files.
#include <filesystem>
// For the std::ofstream that writes the generated Python.
#include <fstream>
// BehaviorTree::CompositeTypes/DecoratorTypes/IsRuntimeType -- the taxonomy
// queries, plus Compile and SaveFile.
#include "Wasteland/VisualBlocks/BehaviorTree.h"
// Load, the .wlbtree reader. LoadTree below is the only caller.
#include "Wasteland/VisualBlocks/GraphSerializer.h"
// The registry of node definitions: Find (by type id) and BehaviorNodes (the
// palette listing). A process-wide singleton reached via NodeRegistry::Get().
#include "Wasteland/VisualBlocks/NodeRegistry.h"
// Validator::Validate, called from four places below: the Validate button,
// SaveTree, LoadTree and CompileAndWrite.
#include "Wasteland/VisualBlocks/Validator.h"

namespace Wasteland
{

	// Six using-declarations bringing the VisualBlocks names into Wasteland, so the
	// bodies below read as `Graph` rather than `VisualBlocks::Graph`.
	//
	// File-scoped, so they apply to every function below -- which is why the
	// explicit `VisualBlocks::` qualifications still appear in a few places (see
	// the Validator and GraphSerializer calls). The file is inconsistent about it.
	//
	// `Graph` in particular is a risky name to have unqualified: it collides
	// conceptually with NodeCanvas-style graph types, and only this alias keeps it
	// unambiguous here.
	using VisualBlocks::BehaviorTree;
	using VisualBlocks::Graph;
	using VisualBlocks::GraphError;
	using VisualBlocks::GraphNode;
	using VisualBlocks::NodeDef;
	using VisualBlocks::NodeRegistry;
	using VisualBlocks::PinLiteral;
	using VisualBlocks::PinDir;
	using VisualBlocks::PinType;
	using VisualBlocks::ValidationResult;

	// ------------------------------------------------------------------
	// Section banner. The three helpers below are file-static and exist only to
	// keep the drawing functions readable.
	// Small helpers
	// ------------------------------------------------------------------

	// The node's literal value for `pinId`, or the type's default.
	// The node's literal value for a pin, or an empty string.
	//
	// Returning `std::string` BY VALUE rather than a const reference: the two return
	// paths return different objects (a copy of lit->Value, and a temporary ""),
	// so a reference return would need a static or a dangling alternative. The copy
	// is one short string on a path that also does a linear search, so it is not the
	// bottleneck.
	static std::string LiteralOf(const GraphNode &node, const char *pinId)
	{
		// A POINTER into the node's literal storage, or null. Not a reference, because
		// the not-found case has to be representable.
		//
		// The result is used immediately and the node outlives the call, so the
		// pointer's validity is fine -- the same pattern as the NodeDef* lookups below.
		const PinLiteral *lit = node.FindLiteral(pinId);
		// Two conditions, and the second is not redundant.
		//
		// `lit` tests presence of the entry at all. `IsSet` tests whether the author
		// actually gave it a value: an entry can exist with an empty/cleared value, and
		// treating that as "the value is the empty string" is what the caller's
		// emptiness checks (the `required` marker below) rely on.
		//
		// So three states are distinguishable to a caller: no entry, an entry that is
		// not set, and a set value -- of which only the last returns a non-empty string.
		if (lit && lit->IsSet)
			return lit->Value;
		return "";
	}

	// Whether a type takes CHILDREN rather than pins. The single most consequential
	// predicate in this file, because it drives four separate behaviours: pin
	// suppression, the abort combo, palette placement mode, and drop-target
	// rendering.
	//
	// Static and taking the type id rather than a GraphNode, so it can be called
	// with a bare id from the palette as well as with a loaded node.
	static bool IsCompositeOrDecorator(const std::string &typeId)
	{
		// A LINEAR SCAN over the composite list, and the loop body has no `break`
		// concern because it returns on the first match.
		//
		// PERFECTLY ACCEPTABLE and worth noting as such: the lists are the fixed node
		// taxonomy (a dozen-odd entries), not user data, so this runs a handful of
		// comparisons per node per frame -- and only while the panel is drawing.
		//
		// Called from four sites including one inside DrawNodeRecursive per node, so it
		// is O(nodes x taxonomy) per frame. That is still noise next to ImGui's own
		// per-widget cost.
		for (const auto &t : BehaviorTree::CompositeTypes())
			if (t == typeId)
				return true;
		for (const auto &t : BehaviorTree::DecoratorTypes())
			if (t == typeId)
				return true;
		return false;
	}

	// Every node in `parent`'s subtree, excluding `parent` itself.
	// Every node in parent's subtree, EXCLUDING parent itself. That exclusion is
	// what lets both callers append the root separately (DeleteNode) or compare
	// against it (WouldCreateCycle).
	static void CollectSubtree(const Graph &g, const std::string &parent,
							   std::vector<std::string> &out)
	{
		// An O(n) search for the parent, done once per visited node -- so the whole walk
		// is O(n^2) on a graph of n nodes. Fine for a hand-authored tree of tens of
		// nodes; it would matter for a generated one in the thousands.
		//
		// `auto it` rather than an index, because the erase-if at DeleteNode works on
		// ids and this only needs the lookup.
		auto it = std::find_if(g.Nodes.begin(), g.Nodes.end(),
							   [&](const GraphNode &n) { return n.Id == parent; });
		// The parent does not exist in this graph. A silent return rather than an error:
		// the callers are all internal, and a missing node means the graph is already
		// inconsistent -- which the validator reports.
		//
		// This also makes the walk safe against a dangling child id, rather than
		// recursing into a node that is not there.
		if (it == g.Nodes.end())
			return;
		// Iterating the parent's child list while recursing. The vector is not modified,
		// so the iterator stays valid across the recursive call -- which matters, because
		// nothing here takes a non-const reference to the graph.
		for (const auto &child : it->ChildIds)
		{
			out.push_back(child);
			// The recursive call appends to the SAME output vector, so `out` accumulates
			// depth-first.
			//
			// DEFECT (no cycle guard): a graph containing a cycle -- which Reparent below
			// exists to prevent, but which can still arrive from a hand-edited or
			// externally-produced .wlbtree -- makes this recurse until the stack overflows.
			// The panel has no recovery from that: it is a crash, not a red error.
			// WouldCreateCycle's check only guards the panel's own edits.
			CollectSubtree(g, child, out);
		}
	}

	// ------------------------------------------------------------------
	// Panel
	// ------------------------------------------------------------------

	// The entry point, called by EditorLayer once per frame.
	//
	// Structured as a sequence of regions rather than one long body: the no-graph
	// state draws a different UI and returns early, then the toolbar, the tree, the
	// findings, and two collapsible sections. Only the first region early-returns.
	void NPCBehaviorPanel::OnImGuiRender()
	{
		// The visibility gate, before any ImGui call -- so a hidden panel costs one
		// branch rather than a window that is created and discarded.
		//
		// Set by EditorLayer alongside the other panels, and by OpenTree on success.
		if (!m_Visible)
			return;

		// The window title doubles as its ImGui ID. No close-button pointer is passed,
		// so the panel has no X -- visibility is entirely the editor's decision, which
		// is what keeps the panel from being dismissed and never reopened.
		ImGui::Begin("NPC Behavior");

		// The EMPTY state, and the reason this file opens with a `has graph` split at
		// all: with nothing loaded there is no tree, no validation and nothing to
		// compile, so the panel offers only the two ways to get a graph.
		//
		// This branch ends with its own ImGui::End() and return, so the rest of the
		// function only runs with a graph present.
		if (!m_HasGraph)
		{
			ImGui::TextDisabled("No behavior tree loaded.");
			ImGui::Separator();
			ImGui::SetNextItemWidth(-1.0f);
			// The path field, in the empty state only. Once a tree is loaded the toolbar
			// takes over and this buffer is not shown again -- so it holds whatever was
			// last typed, not necessarily the loaded path.
			//
			// The double-hash label means the widget shows no visible text of its own; the
			// adjacent button supplies the affordance. m_PathBuf is a fixed 512-byte array
			// in the header, which is what sizeof is measuring -- ImGui needs the buffer
			// SIZE, not a std::string.
			ImGui::InputText("##path", m_PathBuf, sizeof(m_PathBuf));
			// LoadTree(m_PathBuf) passes the char array where a std::string is expected, so
			// an implicit conversion constructs one per click. Irrelevant at click frequency.
			//
			// A failed load sets m_LastError and returns false; the error is displayed on
			// the next line. The previously loaded graph is untouched, which is the
			// behaviour the header promises for OpenTree and this path shares.
			if (ImGui::Button("Load .wlbtree"))
				LoadTree(m_PathBuf);
			ImGui::SameLine();
			if (ImGui::Button("New Tree"))
				NewTree();
			// The error display, shown whenever the last operation failed.
			//
			// It is NOT cleared on success anywhere except LoadTree (which resets
			// m_LastError explicitly) and CompileAndWrite's success path. So a stale error
			// can persist across unrelated actions -- for instance after a successful Save,
			// which only sets m_GeneratedStatus and leaves the error in place. That is a
			// UI wart rather than a correctness problem: the message is a record of the last
			// failure, not a live status.
			if (!m_LastError.Message.empty())
				// The panel's error red, reused verbatim in four places (here, the unknown-type
				// marker in DrawNodeInputs, the findings error colour, and the toolbar's "!"
				// popup trigger). A named constant would be the obvious cleanup; it is
				// duplicated instead.
				//
				// "%s" with the message as an argument rather than TextUnformatted, so a percent
				// sign in an error string cannot corrupt the format.
				ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s",
								   m_LastError.Message.c_str());
			ImGui::End();
			return;
		}

		DrawToolbar();
		ImGui::Separator();

		// The tree region: full available width, and 260 pixels LESS than the height
		// available.
		//
		// The negative height is the ImGui idiom for "all remaining space minus this",
		// which is what leaves room below for the findings panel. It is a fixed pixel
		// reservation, so on a short window the findings pane is squeezed rather than
		// the tree -- and at some sizes the two begin to overlap visually.
		//
		// `true` gives it a border and its own scrollbar, which is what keeps a long
		// tree scrollable independently of the panel.
		if (ImGui::BeginChild("tree", ImVec2(0.0f, -260.0f), true))
		{
			DrawTreeList();
		}
		ImGui::EndChild();

		// Between the tree and the collapsible sections, always drawn -- so validation
		// results are visible without expanding anything.
		//
		// DrawFindings reads m_Validation, which is only refreshed by an explicit
		// Validate, a Save, a Load or a Compile. So between those actions the findings
		// shown can be stale relative to the tree above it -- the same class of problem
		// the m_Dirty flag exists to solve for the compiled output, but not solved for
		// validation.
		DrawFindings();

		// A COLLAPSIBLE section, so the preview costs nothing when the author is not
		// looking at it.
		//
		// The TreeNode/TreePop pairing here and for the Blackboard below is balanced on
		// both paths -- TreePop is the last statement in each if-block, and ImGui tracks
		// the stack, so a missing one would corrupt it.
		if (ImGui::TreeNode("Generated Python"))
		{
			ImGui::TextWrapped("%s", m_GeneratedStatus.c_str());
			if (ImGui::Button("Copy to clipboard"))
			{
				// Copies the generated source. The clipboard is OS-global state, so this is the
				// one place in the panel with an effect outside the editor.
				//
				// No confirmation is shown -- a designer clicking Copy has no visual feedback
				// that it worked, other than the absence of an error.
				ImGui::SetClipboardText(m_Generated.c_str());
			}
			if (ImGui::BeginChild("gen", ImVec2(0.0f, 160.0f), true))
			{
				// Unformatted, correctly: the generated Python is full of %-free but brace-
				// and quote-heavy text, and TextUnformatted cannot misinterpret any of it.
				//
				// Also unword-wrapped, so long lines are clipped rather than wrapped -- which is
				// the right call for source code, where wrapping would misrepresent it.
				ImGui::TextUnformatted(m_Generated.c_str());
			}
			ImGui::EndChild();
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Blackboard"))
		{
			DrawBlackboard();
			ImGui::TreePop();
		}

		// The palette is a separate WINDOW (it calls its own Begin), so it is gated
		// here rather than drawn inline. Gating on the same flag the window's close
		// button writes is what makes the X button work: closing the palette sets
		// m_ShowPalette false through the pointer passed to Begin.
		if (m_ShowPalette)
			DrawPalette();

		// The matching End for the panel window at line 77. Reached only on the path
		// where Begin was called and not early-returned; the no-graph branch has its
		// own End at line 93.
		//
		// Every Begin in this file is matched on every path -- worth stating because an
		// unmatched Begin corrupts ImGui's window stack for the rest of the frame.
		ImGui::End();
	}

	// The action row. Drawn only when a graph is loaded, so every button here acts
	// on something.
	//
	// Almost entirely SameLine-separated rather than laid out with columns or a
	// horizontal group, so the row wraps badly on a narrow panel -- a known ImGui
	// consequence of the SameLine idiom, not a bug in this code.
	void NPCBehaviorPanel::DrawToolbar()
	{
		// The tree's name, first on the row. %s rather than the unformatted overload so
		// a name containing a percent sign is safe -- a tree named "100% complete" would
		// otherwise corrupt the row.
		ImGui::Text("%s", m_Graph.Name.c_str());
		ImGui::SameLine();
		// The node count, greyed. %zu for size_t, which is correct -- m_Graph.Nodes is
		// a vector, so its size() is size_t and a %d would be wrong on 64-bit.
		//
		// Counts every node in the graph, including any that are unreachable from the
		// root -- so after a delete that orphaned something, this can exceed what the
		// tree list shows.
		ImGui::TextDisabled("(%zu nodes)", m_Graph.Nodes.size());

		ImGui::SameLine();
		// TOGGLES the palette rather than opening it directly. The button label does not
		// say it toggles, so the second click appears to do nothing -- it closes the
		// palette. Minor, and the palette's own close button has the same effect.
		if (ImGui::Button("Add Node"))
			m_ShowPalette = !m_ShowPalette;

		ImGui::SameLine();
		// Validation on demand, NOT on every edit. That is why m_Validation can lag the
		// tree: nothing re-validates when a node changes.
		//
		// Deliberate -- validation walks the whole graph, and doing it per keystroke
		// would be wasteful. The cost is that the findings pane shows the state as of
		// the last explicit validation.
		if (ImGui::Button("Validate"))
		{
			// Qualified with the namespace even though `using VisualBlocks::Validator` is
			// NOT among the using-declarations at the top -- only the types are aliased, not
			// the class. So the qualification is required here, not stylistic.
			//
			// Writes straight into the member, so the findings pane updates with no
			// intermediate state. The output parameter is a reference, not a return value,
			// because ValidationResult carries a findings vector that would otherwise be
			// copied.
			VisualBlocks::Validator::Validate(m_Graph, m_Validation);
			// A bare status string with no elaboration -- the author learns that validation
			// ran but not how many findings there are. The findings themselves are in the
			// panel below.
			//
			// Note this OVERWRITES a "compiled" status without recompiling, so the status
			// line stops reflecting what is on disk. The m_Dirty flag is the reliable
			// indicator, which is why the header exposes IsDirty().
			m_GeneratedStatus = "validated";
		}

		ImGui::SameLine();
		// Preview-only compile: fills m_Generated and shows it, and touches no file.
		//
		// The distinction from Compile && Write below is the whole reason both exist --
		// the pre-existing comment says a designer can preview without touching disk.
		// Both paths refuse to emit when validation fails.
		if (ImGui::Button("Compile"))
		{
			// A local, not written directly into m_Generated. Necessary because the compile
			// either succeeds completely or leaves `python` untouched -- on failure
			// m_Generated is explicitly cleared instead, so a stale preview cannot survive a
			// failed recompile.
			std::string python;
			// A SECOND validation result, local to this block, distinct from m_Validation.
			//
			// BehaviorTree::Compile takes its own out-param for validation, so there are two
			// results in play: the one the panel displays and the one the compile produced.
			// On success the local is copied into the member below, so they agree; on
			// failure the local is ALSO copied (see the else branch), which is what makes the
			// findings pane reflect the compile attempt rather than the older explicit
			// validation.
			ValidationResult vr;
			GraphError err;
			// Compile NEVER emits when validation fails: the emitter
			// interpolates literals and operators into Python, so writing a
			// file first and checking it second would mean shipping a
			// broken artifact that merely failed loudly.
			// The pre-existing comment above explains the ordering: never write before
			// validating, because the emitter interpolates literals into Python and a broken
			// artifact is worse than a loud failure.
			//
			// All four arguments are out-params or references. Compile does not throw; a
			// false return with `err` filled is the error channel.
			if (BehaviorTree::Compile(m_Graph, python, vr, err))
			{
				// The success path's first act: the preview text. Copied rather than moved
				// because `python` is not used again in this branch.
				m_Generated = python;
				// Adopt the compile's validation, so the findings pane shows what the compile
				// saw rather than whatever the last Validate button press found.
				m_Validation = vr;
				// Status only. DEFECT (inconsistency): m_Dirty is NOT cleared here, even though
				// this is a successful compile. That is correct for the preview-only button --
				// nothing was written, so the tree on disk is still stale -- but it means the
				// status says "compiled" while the toolbar's "! not compiled" warning is still
				// shown, because m_Dirty is unchanged. Only CompileAndWrite clears it.
				m_GeneratedStatus = "compiled";
			}
			else
			{
				// On failure, the preview is CLEARED rather than left stale. Deliberate and
				// correct: a stale "compiled" preview next to a tree that no longer compiles
				// would be actively misleading.
				//
				// Compare with m_LastError below, which is set -- so the author gets both the
				// cleared preview and the reason.
				m_Generated.clear();
				m_Validation = vr;
				m_LastError = err;
				m_GeneratedStatus = "compile failed";
			}
		}

		// The button that actually makes the NPC change behaviour: compile
		// AND write the .py the ScriptEngine loads. Kept separate from
		// Compile so a designer can preview output without touching disk.
		ImGui::SameLine();
		// THE button that actually changes what an NPC does. The pre-existing comment
		// makes the split explicit: compile alone only previews, so the default action
		// for a finished tree has to do both or the author will believe it is live.
		//
		// The status string is discarded -- `status` is declared and never read, because
		// CompileAndWrite also writes m_GeneratedStatus, which the panel displays.
		// So the out-parameter is redundant on this path.
		if (ImGui::Button("Compile && Write"))
		{
			std::string status;
			CompileAndWrite(status);
		}

		ImGui::SameLine();
		if (ImGui::Button("Save"))
			SaveTree();

		// The stale-tree warning. An NPC running behaviour from before the
		// last edit is indistinguishable from broken AI unless the panel says
		// so, and the author has no other way to tell.
		// The stale-tree warning.
		//
		// DEFECT (redundant test): m_HasGraph is checked, but DrawToolbar is only ever
		// called from OnImGuiRender AFTER the `if (!m_HasGraph)` branch returns. So the
		// first operand is always true here. Harmless, and it documents the
		// precondition -- but it is not doing any work.
		if (m_HasGraph && m_Dirty)
		{
			ImGui::SameLine();
			// Amber rather than red: the tree is not broken, it is out of date. A red
			// "!" would read as an error.
			//
			// This is the panel's most valuable single line. The header explains why: an NPC
			// running behaviour from before the last edit is indistinguishable from broken
			// AI, and the author has no other way to tell.
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
							   "! not compiled");
			// BeginItemTooltip needs ImGui 1.90; this vendored copy is
			// 1.89.3 WIP, so use the IfHovered + BeginTooltip pair that
			// has been stable across every version.
			// The hover test for the tooltip, paired with IsItemHovered because the
			// vendored ImGui is 1.89.3 and BeginItemTooltip needs 1.90. The pre-existing
			// comment records that, which is the kind of version pin worth keeping.
			//
			// Must test the item immediately after drawing it -- IsItemHovered refers to the
			// LAST item, so any widget drawn between the TextColored above and this line
			// would change what is being tested.
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(
					"The tree on disk has changed since it was compiled.\n"
					"Press Compile && Write, or the NPC runs the old behaviour.");
				ImGui::EndTooltip();
			}
		}

		if (!m_LastError.Message.empty())
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "!");
			// The error popup, triggered by the "!" marker. A popup rather than inline text
			// so a long compiler error does not push the toolbar's layout around.
			//
			// Named "lastError" and therefore a single shared popup id -- correct here
			// because only one error exists at a time and only one marker can open it.
			if (ImGui::BeginPopup("lastError"))
			{
				ImGui::TextUnformatted(m_LastError.Message.c_str());
				ImGui::EndPopup();
			}
		}
	}

	// Draws the node hierarchy as an indented tree. The recursion into children is
	// in DrawNodeRecursive; this function's job is to find the root.
	void NPCBehaviorPanel::DrawTreeList()
	{
		// The preferred root: the tree's event node, if the graph declares one.
		// CONST-correct -- the drawing code does not modify the node through this
		// pointer, and every mutation goes through the GraphNode& parameters of
		// DrawNodeInputs/DrawNodeBody, which re-look-up the node by id.
		const GraphNode *root = m_Graph.FindEvent();
		// A tree has no event node; the root is whichever node no other node
		// lists as a child.
		// The fallback path, and the reason it exists: a tree may have no explicit event
		// node, in which case the root is structural.
		//
		// This whole block is O(n^2): the outer loop is over all nodes and the inner
		// std::find is over every child reference collected. For a hand-authored tree
		// that is nothing; the validator reports orphans anyway.
		if (!root)
		{
			// A flat list of every id that appears as SOMEONE'S CHILD. A node absent from
			// this list is a root candidate.
			//
			// Built by flattening all ChildIds. A Set would be the natural structure and
			// would make the membership test O(1); a sorted vector would do too. As written
			// the test below is a linear scan.
			std::vector<std::string> referenced;
			for (const auto &n : m_Graph.Nodes)
				for (const auto &c : n.ChildIds)
					referenced.push_back(c);
			for (const auto &n : m_Graph.Nodes)
			{
				// The membership test. A node no one claims as a child is a root.
				//
				// DEFECT (ambiguous root): the loop BREAKS on the first match, so a graph with
				// two or more roots silently displays only the first and the others vanish from
				// the panel. The validator presumably reports the multiple-root condition, but
				// the tree view itself gives no indication that anything is hidden. A cycle, by
				// contrast, produces NO root at all -- every node is referenced -- and the
				// "no root" message below is the correct outcome for that.
				if (std::find(referenced.begin(), referenced.end(), n.Id) ==
					referenced.end())
				{
					// A second lookup: `n` is a reference INTO m_Graph.Nodes, so `&n` would have
					// sufficed. FindNode re-searches by id, which is redundant but keeps `root` a
					// plain pointer with the same provenance as the FindEvent() case above.
					root = m_Graph.FindNode(n.Id);
					break;
				}
			}
		}

		// Neither an event node nor an orphan-free root: the graph is empty, entirely
		// cyclic, or every node is somebody's child.
		//
		// The message says "has no root" and suggests Add Node, which is the right
		// advice for an empty graph and only half the story for a cyclic one.
		if (!root)
		{
			ImGui::TextDisabled("This tree has no root. Use Add Node to create one.");
			return;
		}

		// PLACEMENT MODE: the author has picked a composite or decorator from the
		// palette and must now click a parent.
		//
		// Represented by a non-empty m_PendingPaletteType rather than a separate bool, so
		// the pending TYPE is known -- a composite could become a root if placed on the
		// root row, which is exactly what AddNode's empty parentId expresses.
		if (!m_PendingPaletteType.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Click a parent to place the new node...");
			ImGui::SameLine();
			if (ImGui::SmallButton("cancel"))
				m_PendingPaletteType.clear();
			ImGui::Separator();
		}

		DrawNodeRecursive(root->Id, 0);
	}

	void NPCBehaviorPanel::DrawNodeRecursive(const std::string &nodeId, int depth)
	{
		GraphNode *node = m_Graph.FindNode(nodeId);
		if (!node)
			return;

		ImGui::PushID(nodeId.c_str());

		// Pending-placement mode: any row is a drop target.
		if (!m_PendingPaletteType.empty() &&
			IsCompositeOrDecorator(m_PendingPaletteType))
		{
			// The drop target row, 80 pixels wide with automatic height.
			//
			// The second parameter is `selected`, hard-coded false -- a drop target is not a
			// selection state, and the real selection is the tree node itself. Passing 0 for
			// flags means no SpanFullWidth, which is why an explicit width is needed.
			//
			// Available only while the pending type is a composite or decorator (the guard
			// at the top of DrawNodeRecursive), which is what stops a designer from placing
			// a task node under a parent that cannot hold children.
			if (ImGui::Selectable("drop here", false, 0, ImVec2(80.0f, 0.0f)))
			{
				// Placement: the new node becomes a child of the clicked node.
				//
				// Note the asymmetry with the palette's own path -- an event node CAN be a root
				// (AddNode with an empty parentId), but a composite placed via this route always
				// gets a parent. There is no way to add a second composite at root level from
				// this UI.
				AddNode(m_PendingPaletteType, nodeId);
				m_PendingPaletteType.clear();
			}
			ImGui::PopID();
			// Early return after a placement, so the row does not also render as a normal
			// tree node on the frame it was clicked -- the PopID above is matched, so the
			// ID stack is clean.
			return;
		}

		// The node type's definition.
		//
		// DEFECT (dead variable): `def` is never used in this function. DrawNodeInputs
		// below performs its own NodeRegistry lookup, which is where the definition is
		// actually consumed. This lookup is a wasted registry probe per node per frame,
		// and a compiler with -Wunused-variable would flag it. It looks like a leftover
		// from when the body was drawn inline here.
		const NodeDef *def = NodeRegistry::Get().Find(node->TypeId);
		const bool hasKids = !node->ChildIds.empty();

		// The label string must outlive the call. Building it inline with
		// ("abort=" + node->Abort).c_str() leaves a dangling temporary whose
		// storage dies at the end of the full expression, and TreeNodeEx
		// then reads freed memory for its hidden label.
		std::string label = node->TypeId + "  [" + node->Id + "]";
		if (node->Latent)
			label += "  latent";
		if (node->Abort != "none")
			label += "  abort=" + node->Abort;

		// Indentation by 2 pixels per level. A fixed pixel step rather than ImGui's style
		// spacing, so deep trees compress rather than run off the panel -- acceptable for
		// the shallow trees this panel is designed for.
		//
		// Balanced by the matching Unindent at the end of the function, including on the
		// early-return paths above (which push/pop their own indent-free pairs).
		ImGui::Indent(depth * 2);
		// DefaultOpen so the whole tree is visible without clicking anything -- right for
		// an authoring view where the structure IS the content.
		//
		// SpanFullWidth so the clickable area spans the panel, so right-clicking anywhere
		// on the row reaches the context menu below.
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen |
								   ImGuiTreeNodeFlags_SpanFullWidth;
		if (!hasKids)
			// A childless node is drawn as a leaf.
			//
			// NoTreePushOnOpen is REQUIRED alongside Leaf: without it ImGui would push a
			// tree arrow for a node that has no children, and the matching TreePop at the
			// bottom of the function would then pop the wrong entry. This pairing is the
			// classic ImGui leaf-node requirement and getting it wrong corrupts the ID stack.
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		// label.c_str() appears TWICE, and both uses are needed: the first is the
		// visible/ID string, the second is the format argument for the variadic
		// TreeNodeEx.
		//
		// Passing "%s" with the label as an argument (rather than relying on the label
		// being a format string) is what makes a node type or id containing a percent sign
		// safe.
		//
		// `open` is false when the node is collapsed -- and, for a leaf, false as well,
		// which is why the body is drawn under `open || !hasKids` below.
		bool open = ImGui::TreeNodeEx(label.c_str(), flags, "%s", label.c_str());

		// Right-click for structural actions.
		// The right-click menu. A POPUP rather than a menu bar item, so it appears where
		// the cursor is.
		//
		// Begins and ends within this function, and returns a bool for the contents --
		// the standard ImGui popup idiom.
		if (ImGui::BeginPopupContextItem("nodeCtx"))
		{
			// The only structural action offered.
			//
			// The header's comment claims node rows carry "add child / delete / rename",
			// but only Delete is implemented here. Add child is a button in the node body
			// instead, and there is no rename -- so the header's comment is inaccurate on
			// two of three counts.
			//
			// DEFECT (unreachable UI): Reparent and WouldCreateCycle below are the panel's
			// cycle-safety machinery, and NOTHING in this file calls Reparent. So the only
			// way to restructure a tree is Delete plus re-add, and the cycle check exists
			// for a code path no user can reach.
			if (ImGui::MenuItem("Delete"))
			{
				// Deletes the node AND its whole subtree, then cleans up dangling parent
				// references -- see DeleteNode.
				//
				// PopID and return immediately: the node is gone, so continuing would draw a
				// deleted node and recurse into freed children.
				DeleteNode(nodeId);
				ImGui::PopID();
				return;
			}
			ImGui::Separator();
			ImGui::EndPopup();
		}

		// The body is drawn when the node is open OR has no children.
		//
		// The `|| !hasKids` is necessary because TreeNodeEx returns false for a leaf --
		// there is nothing to expand, so a leaf's inputs would never render without it.
		// That is the standard fix and easy to get wrong, since a leaf also has open==false
		if (open || !hasKids)
		{
			DrawNodeInputs(*node);
			// A literal ID pushed around the body. This is what makes the body's widgets
			// -- Checkbox("latent"), Combo("abort"), SmallButton("Add child") -- resolve to
			// DISTINCT ids per node rather than colliding across every row.
			//
			// Without it, every node's "latent" checkbox would share one ImGui ID, and
			// ImGui's state (hover, click, focus) would bleed between rows.
			//
			// Note DrawNodeBody pushes node.Id AGAIN inside, so the nesting is redundant but
			// harmless.
			ImGui::PushID("body");
			DrawNodeBody(*node, depth);
			ImGui::PopID();
		}

		// Recursion, and only for expanded nodes with children. Collapsed subtrees cost
		// nothing beyond the TreeNode itself.
		//
		// Unbounded depth: a pathological or cyclic graph would recurse until the stack
		// overflows, the same hazard as CollectSubtree above. Cycles cannot arrive from
		// this UI -- nothing here creates one -- but they can arrive from a loaded file.
		if (open && hasKids)
		{
			for (const auto &child : node->ChildIds)
				DrawNodeRecursive(child, depth + 1);
			ImGui::TreePop();
		}

		ImGui::Unindent(depth * 2);
		ImGui::PopID();
	}

	void NPCBehaviorPanel::DrawNodeBody(GraphNode &node, int depth)
	{
		(void)depth;
		ImGui::PushID(node.Id.c_str());

		// Latent is DECLARED, never inferred: the validator requires it and a
		// leaf that returns RUNNING without it would be re-entered from the
		// root every tick.
		// Latent is only meaningful for RUNTIME types. The pre-existing comment
		// explains the rule: it is declared, never inferred, because a leaf returning
		// RUNNING without it would be re-entered from the root every tick.
		//
		// So the checkbox is shown only where it has an effect, rather than greyed out
		// elsewhere.
		if (BehaviorTree::IsRuntimeType(node.TypeId))
		{
			bool latent = node.Latent;
			// A LOCAL copy seeded from the node, for the same reason as the toggle in the
			// other panel: ImGui writes through the pointer, so passing &node.Latent would
			// mutate on every frame rather than only on a click.
			//
			// The label doubles as the ImGui ID, which is unique per row because of the
			// PushID("body") in the caller.
			if (ImGui::Checkbox("latent", &latent))
			{
				// Writes back only on the frame the checkbox was clicked -- so the node is
				// touched exactly once per interaction.
				//
				// DEFECT (possible stale write): ImGui can report a click on a row the user did
				// not intend (a drag that ends over the box, a popup opening on top). There is
				// no confirmation step, so a stray click on `latent` silently changes the
				// node's runtime semantics and marks the tree dirty.
				node.Latent = latent;
				m_Dirty = true;
			}
			ImGui::SameLine();
		}

		if (IsCompositeOrDecorator(node.TypeId))
		{
			// The three abort scopes, as a fixed local array. Composite/decorator only -- a
			// leaf has no children to abort from, which is what the guard above enforces.
			//
			// These are the strings persisted into the node's Abort field, so they are a
			// wire format: changing one here without a matching change in the compiler
			// silently breaks every existing tree.
			const char *aborts[] = {"none", "self", "lower"};
			int ai = 0;
			// String-to-index translation, to give the Combo a selected index.
			//
			// DEFECT (silent mismatch): no `break`, and no else branch. An unrecognised Abort
			// string leaves ai at 0, which the Combo displays as "none" -- so a tree whose
			// Abort is (say) "self_upper" shows and re-writes it as "none" the moment the
			// author touches the combo. The corruption is invisible until compile time.
			// A lookup that reported an unknown value would be safer.
			for (int i = 0; i < 3; ++i)
				if (node.Abort == aborts[i])
					ai = i;
			if (ImGui::Combo("abort", &ai, aborts, 3))
			{
				node.Abort = aborts[ai];
				m_Dirty = true;
			}
		}

		// Opens the palette. It does NOT arm placement mode directly -- that happens when
		// the author picks a composite or decorator from the palette.
		//
		// The `{ }` braces around a single statement are redundant but harmless; this
		// file's style uses both forms.
		if (ImGui::SmallButton("Add child"))
		{
			m_ShowPalette = true;
		}

		ImGui::PopID();
	}

	void NPCBehaviorPanel::DrawNodeInputs(GraphNode &node)
	{
		// The definition lookup, here where it is actually USED. This is the lookup the
		// dead one in DrawNodeRecursive should have been.
		//
		// Called per node per frame while the tree is drawn -- a linear scan of the
		// registry's node list. Fine for a static registry of tens of entries.
		const NodeDef *def = NodeRegistry::Get().Find(node.TypeId);
		// An unknown type id: the graph references a node type the registry does not
		// have. That means a .wlbtree written against a different or newer build.
		//
		// Reported INLINE and in red rather than being skipped silently, so a designer
		// opening an incompatible file sees exactly which node is the problem instead of
		// a tree with a hole in it.
		if (!def)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "  (unknown type)");
			return;
		}

		for (const auto &pin : def->Pins)
		{
			// Input pins only. Output pins are connections, and this panel edits literals,
			// which only exist on inputs -- the graph's links are shown by nesting, not by
			// drawing edges.
			if (pin.Dir != PinDir::In)
				continue;
			// Composites and decorators take CHILDREN, not pins. A pin list
			// that pretended otherwise is what leads a designer to wire a
			// wire where the structure belongs.
			// Composites and decorators take CHILDREN, not pins, so their input pins are
			// suppressed entirely.
			//
			// The pre-existing comment gives the design reason: a pin list that pretended
			// otherwise is what leads a designer to wire where the structure belongs. So a
			// composite's NodeDef may still declare inputs -- they are simply not editable
			// here.
			if (IsCompositeOrDecorator(node.TypeId))
				continue;

			ImGui::PushID(pin.Id.c_str());
			ImGui::Indent(12.0f);
			// The pin's label, falling back to its id when no label is defined.
			//
			// The ternary yields a `const char*` from either branch -- both are c_str()
			// results, so the types agree and no temporary std::string is involved.
			//
			// This is a LABEL, not an ImGui ID -- the ID for these widgets is the PushID
			// above plus the "##v" used at each widget.
			ImGui::TextDisabled("%s", pin.Label.empty() ? pin.Id.c_str()
														: pin.Label.c_str());
			ImGui::SameLine();

			if (pin.Type == PinType::Bool)
			{
				// A bool pin's value, reconstructed from the stored STRING.
				//
				// The wire format is the literal "True"/"False" -- Python's spelling, because
				// these literals are interpolated into the generated Python. Comparing against
				// "True" only means any other value (including "true", "1", a typo) reads as
				// false.
				//
				// It is also self-correcting in one direction: touching the checkbox rewrites the
				// literal to a canonical spelling.
				bool b = LiteralOf(node, pin.Id.c_str()) == "True";
				if (ImGui::Checkbox("##v", &b))
				{
					// The canonical spellings, matching what the Python emitter expects.
					//
					// `pin.Id` is a std::string passed where a const char* is presumably taken, so
					// an implicit conversion happens per call -- at click frequency, so free.
					node.SetLiteral(pin.Id, b ? "True" : "False");
					m_Dirty = true;
				}
			}
			else if (pin.Type == PinType::Int)
			{
				// An int pin, parsed from its literal with atoi.
				//
				// DEFECT (error handling): atoi returns 0 for unparseable input and has no way
				// to report failure. So a literal of "abc" silently reads as 0, and the widget
				// displays 0 -- which looks like a real value rather than a parse error. std::
				// stoi with a try/catch, as the ApiKeyStore temperature parse does elsewhere in
				// this codebase, would distinguish them.
				int v = std::atoi(LiteralOf(node, pin.Id.c_str()).c_str());
				// DEFECT (missing dirty flag, and it is a real inconsistency): this branch calls
				// SetLiteral but does NOT set m_Dirty = true, where the Bool, Float and string
				// branches all do.
				//
				// Consequence: editing an int pin does not mark the tree stale, so the toolbar's
				// "! not compiled" warning does not appear and the author has no indication that
				// -- the running behaviour does not match what is on screen. It is the one input
				// -- type where an edit can go completely unnoticed.
				if (ImGui::InputInt("##v", &v))
					node.SetLiteral(pin.Id, std::to_string(v));
			}
			// The float branch, structurally identical to the int branch above -- but it
			// DOES set m_Dirty, which is what makes the omission above an oversight rather
			// than a deliberate difference.
			else if (pin.Type == PinType::Float)
			{
				// atof for floats, with the same silent-zero behaviour on unparseable input.
				//
				// Written back with std::to_string, which emits six decimal places -- so a value
				// the author typed as 0.5f is stored as "0.500000" in the graph and appears in
				// the generated Python that way.
				float v = std::atof(LiteralOf(node, pin.Id.c_str()).c_str());
				if (ImGui::InputFloat("##v", &v))
				{
					node.SetLiteral(pin.Id, std::to_string(v));
					m_Dirty = true;
				}
			}
			else
			{
				// The string-pin buffer, zero-initialised.
				//
				// A fixed 128 bytes because ImGui's InputText takes (char*, size). The graph's
				// literal is copied in with snprintf below.
				//
				// DEFECT (truncation): a literal longer than 127 characters is silently cut, and
				// the truncated text is then written back to the node. The author sees the cut
				// text in the box, so it is at least visible -- but the original value is gone
				// from the graph as soon as the field is touched.
				char buf[128] = {};
				std::string cur = LiteralOf(node, pin.Id.c_str());
				// The safe copy INTO ImGui's buffer: snprintf with the destination size, so a
				// long literal is truncated rather than overflowing. Using strcpy here would be
				// a buffer overflow.
				//
				// "%s" with the value as an argument rather than passing cur.c_str() as the
				// format, so a literal containing a percent sign is not misread.
				std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
				if (ImGui::InputText("##v", buf, sizeof(buf)))
				{
					node.SetLiteral(pin.Id, buf);
					m_Dirty = true;
				}
			}

			// The REQUIRED marker, in orange.
			//
			// Both conditions matter. Required is the type's declaration; empty() is the
			// runtime check, and it is empty() rather than `!lit->IsSet` because the two
			// differ: an entry explicitly set to the empty string would count as satisfied
			// here while LiteralOf reports it as unset. A pin required to be non-empty and
			// explicitly set to "" is therefore shown as satisfied -- an edge case the
			// validator may or may not catch.
			//
			// The same LiteralOf is called a third time for this pin in this iteration,
			// after two calls in the widget branches above. Each is a linear search of the
			// node's literals.
			if (pin.Required && LiteralOf(node, pin.Id.c_str()).empty())
				ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "  required");
			ImGui::Unindent(12.0f);
			ImGui::PopID();
		}
	}

	// The node-type chooser, as a floating window.
	void NPCBehaviorPanel::DrawPalette()
	{
		// A default SIZE, applied only the first time the window is ever shown.
		// FirstUseEver rather than Always, so the palette remembers a resize across
		// sessions -- which is why closing and reopening it does not snap it back.
		ImGui::SetNextWindowSize(ImVec2(420.0f, 420.0f), ImGuiCond_FirstUseEver);
		// The close button pointer. Writing m_ShowPalette from ImGui's X button is what
		// makes the gate in OnImGuiRender (`if (m_ShowPalette)`) agree with the window's
		// own state -- a neat trick that avoids a separate flag for "the user closed
		// it".
		//
		// The same m_ShowPalette is also toggled by the toolbar's Add Node button, so
		// the two controls share one flag by design.
		if (!ImGui::Begin("Node Palette", &m_ShowPalette))
		{
			ImGui::End();
			return;
		}

		ImGui::TextDisabled(
			"Click a node to add it. Composite/decorator types need a parent.");
		ImGui::Separator();

		// A COPY of the registry's node list, by value.
		//
		// Unnecessary -- the palette only reads the definitions -- but the registry may
		// return by value, in which case this is the copy that API forces. It is walked
		// once per frame while the palette is open.
		auto nodes = NodeRegistry::Get().BehaviorNodes();
		// Four buckets, partitioned by the BtRole string.
		//
		// DEFECT (silent drop): the if/else-if chain below has no else, so a node whose
		// BtRole is anything other than those four strings -- including an empty or
		// misspelled role -- appears NOWHERE in the palette. It is not an error and not
		// reported; the node type simply does not exist as far as the UI is concerned,
		// while remaining loadable by id if a .wlbtree references it.
		//
		// The partitioning is rebuilt every frame rather than cached, which is fine at
		// this scale.
		std::vector<const NodeDef *> composites, decorators, conditions, tasks;
		for (const NodeDef *d : nodes)
		{
			// String comparison against a magic value, repeated for all four roles. A
			// `role` enum on NodeDef would make this exhaustive-checkable by the compiler;
			// as strings it cannot be.
			if (d->BtRole == "composite")
				composites.push_back(d);
			else if (d->BtRole == "decorator")
				decorators.push_back(d);
			else if (d->BtRole == "condition")
				conditions.push_back(d);
			else if (d->BtRole == "task")
				tasks.push_back(d);
		}

		// A lambda standing in for a helper function, taking the bucket by NON-CONST
		// reference so the TreeNode body can read it.
		//
		// Captures nothing but what it uses ([&] is present but the body only touches its
		// parameters), so it could be []. Written as a lambda rather than a file-static
		// function because the four buckets are locals.
		auto section = [&](const char *label, std::vector<const NodeDef *> &list) {
			if (list.empty())
				return;
			// Collapsible per role, so a palette with dozens of task types does not open as
			// one long list.
			//
			// Composites and decorators are always the first two sections, which suits the
			// placement workflow: the structural nodes are the ones that need a parent click.
			if (!ImGui::TreeNode(label))
				return;
			for (const NodeDef *d : list)
			{
				// The raw type id as the visible label.
				//
				// DEFECT (poor UX, by design or oversight): there is no Label field being used
				// here even though NodeDef pins have one and DrawNodeInputs prefers pin.Label.
				// A designer picking from the palette sees "Sequence", "Selector",
				// "InvertCondition" -- identifier spellings, not descriptions. AddNode's dropdown
				// would be more usable with d->Label.
				if (ImGui::Selectable(d->TypeId.c_str()))
				{
					// THE placement split, and the reason m_PendingPaletteType exists at all.
					//
					// Composite/decorator -> ARM placement mode; the author must next click a parent
					// in the tree.
					// Event/condition/task -> add immediately, with an EMPTY parentId meaning root.
					//
					// So a condition can be a root but a sequence cannot be placed at root through
					// this path, even though the model would allow it.
					if (IsCompositeOrDecorator(d->TypeId))
						m_PendingPaletteType = d->TypeId;
					else
						// An empty braced-init-list for parentId, meaning ROOT.
						//
						// `{}` rather than `""` -- both construct an empty std::string. The empty value
						// is the sentinel AddNode tests with `if (!parentId.empty())`.
						AddNode(d->TypeId, {});
				}
			}
			ImGui::TreePop();
		};

		section("Composites", composites);
		section("Decorators", decorators);
		section("Conditions", conditions);
		section("Tasks", tasks);

		ImGui::End();
	}

	// The validation results pane.
	void NPCBehaviorPanel::DrawFindings()
	{
		// Counts first, because the display is driven by them rather than by the findings
		// list itself.
		//
		// Both are size_t and formatted with %zu below, which matches.
		size_t errors = m_Validation.ErrorCount();
		size_t warns = m_Validation.WarningCount();

		// The all-clear branch. Green, and an early return so no findings pane is
		// created -- which means the successful state is one line rather than an empty
		// bordered box.
		//
		// Worth noting what this does NOT mean: it means the last VALIDATION found
		// nothing, not that the tree is currently valid. Edits since then do not
		// re-validate.
		if (errors == 0 && warns == 0)
		{
			ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "no errors, no warnings");
			return;
		}

		ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%zu error(s)", errors);
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%zu warning(s)", warns);

		if (ImGui::BeginChild("findings", ImVec2(0.0f, 140.0f), true))
		{
			// The findings list, each coloured by severity. No node navigation -- clicking a
			// finding does not select or scroll to the node it concerns, so the author has
			// to find the node themselves.
			//
			// The child window is bounded at 140 pixels, so a tree with many findings shows
			// a scrollable slice.
			for (const auto &f : m_Validation.Findings)
			{
				ImVec4 c = f.IsError ? ImVec4(1.0f, 0.42f, 0.42f, 1.0f)
									 : ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
				ImGui::TextColored(c, "%s", f.Message.c_str());
			}
		}
		ImGui::EndChild();
	}

	// The blackboard declaration list -- the shared variables the tree reads and
	// writes.
	void NPCBehaviorPanel::DrawBlackboard()
	{
		// The empty case, one greyed line. Blackboard keys come from the file, so there
		// is no way to add one from this panel -- the section is read-only.
		if (m_Graph.Blackboard.empty())
		{
			ImGui::TextDisabled("No blackboard keys declared.");
			return;
		}
		for (const auto &k : m_Graph.Blackboard)
		{
			ImGui::Text("%s : %s", k.Name.c_str(), PinTypeName(k.Type));
			// COMMA OPERATOR joining two ImGui calls onto one line.
			//
			// Legal and compact, and both calls happen -- SameLine must run before the text
			// to place it alongside. It is the only place in this file that uses the comma
			// operator, and it is the least readable line in it: a reader has to know the
			// sequencing to see that SameLine is not conditional on anything.
			//
			// Every other line in the file puts one call per line.
			if (!k.DefaultLiteral.empty())
				ImGui::SameLine(), ImGui::TextDisabled("= %s", k.DefaultLiteral.c_str());
		}
	}

	// ------------------------------------------------------------------
	// Edits
	// ------------------------------------------------------------------

	// Adds a node, optionally under a parent. Called from the palette (immediate or
	// armed) and from the drop-target path.
	void NPCBehaviorPanel::AddNode(const std::string &typeId,
								   const std::string &parentId)
	{
		GraphNode n;
		n.TypeId = typeId;
		// The id is derived from the CURRENT NODE COUNT, so it is only a starting
		// guess -- uniqueness is not guaranteed by it.
		//
		// Readable ("n7" for the seventh node) and stable for the author, at the cost of
		// requiring the collision loop below.
		n.Id = "n" + std::to_string(m_Graph.Nodes.size() + 1);
		// Guarantee uniqueness: deleting and re-adding can otherwise
		// collide, and a duplicate id makes every link ambiguous.
		// The uniqueness guarantee, and it is a genuine one rather than a hopeful one:
		// the loop keeps appending underscores until the id is unused.
		//
		// The pre-existing comment gives the reason -- delete and re-add can collide, and
		// a duplicate id makes every link ambiguous.
		//
		// DEFECT (linear and unbounded in appearance): each iteration is a full graph
		// scan, and the loop appends one underscore per collision. In practice at most a
		// couple of iterations, since ids only collide after deletions. It terminates
		// because each iteration lengthens a string that cannot collide with itself.
		while (m_Graph.FindNode(n.Id))
			n.Id += "_";

		// Seed required pins from the node type's defaults so a freshly
		// added node is not immediately an error the author has to fix
		// before it can be inspected.
		// The type definition, for seeding defaults below.
		//
		// Checked for null rather than assumed -- a palette entry could name a type the
		// registry does not have, and this path is reachable from a loaded graph's ids,
		// so the guard is warranted even though the palette only offers registered types.
		const NodeDef *def = NodeRegistry::Get().Find(typeId);
		if (def)
		{
			for (const auto &pin : def->Pins)
			{
				if (pin.Dir != PinDir::In)
					continue;
				// Only pins WITH a default are seeded.
				//
				// The pre-existing comment explains the intent: a freshly added node should not
				// be an immediate error the author must fix before inspecting it. Pins with no
				// default stay unset and show the orange `required` marker in DrawNodeInputs,
				// which is the right division -- defaults where the type declares them, an
				// explicit prompt where it does not.
				if (!pin.DefaultLiteral.empty())
					n.SetLiteral(pin.Id, pin.DefaultLiteral);
			}
		}

		// The insertion. A copy, so `n` -- a local, with all its seeded literals -- is
		// fully populated before it enters the graph.
		//
		// May reallocate, which is why the `parent` lookup below happens AFTER the
		// push: taking a pointer into the vector first and then pushing would be a
		// use-after-realloc.
		m_Graph.Nodes.push_back(n);

		if (!parentId.empty())
		{
			// The parent lookup, after the push.
			//
			// DEFECT (silent orphan): if parentId names a node that does not exist, the child
			// is added to the graph but linked to nothing, and nothing reports it here. The
			// validator will call it an orphan, so it is eventually visible -- but the
			// placement click that caused it looked successful.
			//
			// Reachable in principle: the drop-target path passes the id of a row the author
			// clicked, so in normal use it always resolves.
			GraphNode *parent = m_Graph.FindNode(parentId);
			if (parent)
				parent->ChildIds.push_back(n.Id);
		}
		m_Dirty = true;
	}

	// Deletes a node and everything beneath it. The destructive counterpart to AddNode.
	//
	// Not undoable -- there is no undo stack anywhere in this panel, so a mis-click
	// on Delete loses the subtree permanently until the file is reloaded from disk.
	// The pre-existing comment in the header lists "rename" as a node-row feature
	// that does not exist; an undo would arguably be a better use of that effort.
	void NPCBehaviorPanel::DeleteNode(const std::string &nodeId)
	{
		// Collected first, then removed -- so the erase pass below does not mutate the
		// graph while iterating it.
		//
		// The alternative, deleting during traversal, would invalidate the iterators
		// CollectSubtree is walking.
		std::vector<std::string> doomed;
		CollectSubtree(m_Graph, nodeId, doomed);
		doomed.push_back(nodeId);

		// The ERASE-REMOVE IDIOM: remove_if physically moves the survivors to the front
		// and returns the new logical end, then erase truncates.
		//
		// Not stable_partition or a manual loop. This is the standard vector idiom and
		// is correct: remove_if preserves the relative order of the kept elements, which
		// matters because node order here is authoring order.
		//
		// The predicate is O(doomed) per node, so the whole pass is O(n x subtree) --
		// fine at this scale.
		m_Graph.Nodes.erase(
			std::remove_if(m_Graph.Nodes.begin(), m_Graph.Nodes.end(),
						   [&](const GraphNode &n) {
							   return std::find(doomed.begin(), doomed.end(), n.Id) !=
									  doomed.end();
						   }),
			m_Graph.Nodes.end());

		// Drop dangling parent references, or the validator will (correctly)
		// report the survivors as orphaned.
		// The SECOND pass, and its necessity is the non-obvious part.
		//
		// Deleting a subtree removes the nodes, but surviving nodes may still LIST the
		// deleted ids in their ChildIds -- the graph stores parent->child links one way
		// only, with no back-references. Those dangling ids would make every survivor
		// look like it has children that do not exist.
		//
		// The pre-existing comment says the validator would report the survivors as
		// orphaned. This pass prevents that, so the finding is accurate rather than
		// spurious.
		for (auto &n : m_Graph.Nodes)
		{
			auto &kids = n.ChildIds;
			kids.erase(std::remove_if(kids.begin(), kids.end(),
									  [&](const std::string &c) {
										  return std::find(doomed.begin(), doomed.end(),
														   c) != doomed.end();
									  }),
					   kids.end());
		}
		m_Dirty = true;
	}

	// Asks whether making newParentId the parent of nodeId would create a cycle.
	//
	// `const` because it only reads the graph.
	//
	// DEAD CODE: nothing in this file calls Reparent, and Reparent is the only caller
	// of this. So both functions, and the cycle protection they implement, are
	// unreachable from the UI.
	bool NPCBehaviorPanel::WouldCreateCycle(const std::string &nodeId,
											const std::string &newParentId) const
	{
		// Self-parenting is the trivial cycle, checked before the expensive walk.
		//
		// DEFECT (redundant): CollectSubtree excludes the node itself, so its result
		// could never contain nodeId -- this check is not what catches self-parenting.
		// It is harmless and it makes the intent explicit, and the identical check in
		// Reparent above does produce the user-facing message.
		if (nodeId == newParentId)
			return true;
		std::vector<std::string> subtree;
		// The real test: is the proposed parent already inside the node's own subtree?
		// If so, parenting it here would make it an ancestor of itself.
		//
		// Walks the whole subtree, so O(subtree) with an O(n) lookup per node -- and it
		// inherits CollectSubtree's unbounded-recursion hazard on a cyclic graph, so
		// calling this on an already-cyclic graph would crash rather than return true.
		CollectSubtree(m_Graph, nodeId, subtree);
		return std::find(subtree.begin(), subtree.end(), newParentId) !=
			   subtree.end();
	}

	// Moves a node under a new parent, refusing anything that would make the tree
	// invalid.
	//
	// DEAD CODE -- see the WouldCreateCycle note. The header documents this as the
	// mechanism by which "an edit that would produce an INVALID tree is refused at
	// the point of the click", but no click reaches it.
	//
	// `outReason` is an out-parameter rather than an exception or an enum, because
	// the messages are user-facing sentences.
	bool NPCBehaviorPanel::Reparent(const std::string &nodeId,
									const std::string &newParentId,
									std::string &outReason)
	{
		if (nodeId == newParentId)
		{
			// A user-facing sentence rather than an error code -- the caller would show it
			// verbatim.
			//
			// Written before the return so the caller can rely on outReason being meaningful
			// whenever the return is false.
			outReason = "A node cannot be its own parent.";
			return false;
		}
		if (WouldCreateCycle(nodeId, newParentId))
		{
			// Refused HERE, with a reason, rather than leaving a cycle for
			// the validator to report as an opaque structural error.
			outReason = "That would make the node its own ancestor (cycle).";
			return false;
		}

		// Detach first: remove the node from EVERY current parent, then attach to the
		// new one.
		//
		// Scanning all nodes rather than tracking a parent pointer, because the graph
		// stores links one way only. std::remove (not remove_if) plus erase, since the
		// predicate is a single value comparison.
		//
		// Correctly removes ALL occurrences, so a node listed twice under one parent
		// (possible from a malformed file) is fully detached.
		for (auto &n : m_Graph.Nodes)
		{
			auto &kids = n.ChildIds;
			kids.erase(std::remove(kids.begin(), kids.end(), nodeId), kids.end());
		}
		// An empty newParentId means "make it a root", so the detach above is the whole
		// operation. That is how a node is promoted to root level -- a capability with
		// no caller, like the rest of this function.
		if (!newParentId.empty())
		{
			GraphNode *parent = m_Graph.FindNode(newParentId);
			if (parent)
				parent->ChildIds.push_back(nodeId);
		}
		m_Dirty = true;
		return true;
	}

	// Resets everything to a blank tree. Destructive and unconfirmed -- there is no
	// "discard changes?" prompt, so clicking New Tree with unsaved work loses it.
	void NPCBehaviorPanel::NewTree()
	{
		// A fresh default-constructed graph, assigned over the old one.
		//
		// Assignment rather than clear(): Graph is a plain struct, so this is a
		// memberwise copy that releases the old nodes' storage. Correct, and it avoids
		// the risk of clearing fields individually and missing one.
		m_Graph = Graph();
		m_Graph.Kind = "tree";
		m_Graph.Name = "NewBehavior";
		m_Generated.clear();
		m_GeneratedStatus.clear();
		m_Validation = ValidationResult();
		m_LastError = GraphError();
		m_HasGraph = true;
		// The path is CLEARED, so the new tree has no file. That is deliberate: it means
		// SaveTree will refuse ("No path") rather than silently overwriting whatever
		// "NewTree" was opened from, and CompileAndWrite refuses too.
		//
		// The cost is that a new tree cannot be saved until the author types a path into
		// the empty-state field -- which requires going back through the no-graph UI or
		// some other route, since the toolbar's Save has no path entry.
		m_Path.clear();
		// New and unsaved: nothing on disk can be stale yet, but nothing is
		// compiled either, so treat it as needing a compile.
		// New trees start dirty. The pre-existing comment explains the reasoning: nothing
		// on disk can be stale yet, but nothing is compiled either, so the panel should
		// say a compile is needed.
		//
		// The header frames m_Dirty as "edited since the last compile", which this
		// case stretches -- it has never been compiled at all.
		m_Dirty = true;
	}

	// Loads a .wlbtree, replacing whatever is open. Returns false with the reason in
	// m_LastError on failure, leaving the previous graph intact.
	bool NPCBehaviorPanel::LoadTree(const std::string &path)
	{
		Graph g;
		GraphError err;
		// Qualified with the namespace -- again because Serializer is not among the
		// using-declarations at the top of the file.
		//
		// Into LOCALS: `g` and `err` are only adopted on success. Assigning m_Graph
		// before checking the result would discard the author's open tree on a failed
		// load, which is exactly the behaviour the header promises against.
		if (!VisualBlocks::GraphSerializer::Load(path, g, err))
		{
			m_LastError = err;
			return false;
		}
		// Adopt only on success. One copy of the whole graph; for a large file this is
		// the dominant cost of a load, but loads are user-initiated.
		m_Graph = g;
		m_HasGraph = true;
		m_Path = path;
		// The error is CLEARED on a successful load.
		//
		// The only place besides CompileAndWrite that resets it -- which is why a stale
		// error can otherwise survive unrelated actions, as noted at the empty-state
		// error display.
		m_LastError = GraphError();
		m_Generated.clear();
		// A fresh status, and m_Generated is cleared on the line above so no stale
		// preview from the previous tree is shown next to the new one.
		//
		// Deliberately NOT "compiled": loading does not compile.
		m_GeneratedStatus = "loaded";
		// A freshly-loaded tree matches the file, so it is not stale. The
		// dirty flag means "edited since the last compile", and nothing has
		// been edited yet.
		// A loaded tree is NOT dirty. The pre-existing comment gives the reasoning:
		// dirty means "edited since the last compile", and nothing has been edited.
		//
		// Worth being precise about the consequence: the .wlbtree on disk has not been
		// compiled to .py by this panel. An NPC may still be running older behaviour. The
		// panel reports the tree as clean because the tree matches its file, which is a
		// different question from whether the file matches what is running.
		m_Dirty = false;
		VisualBlocks::Validator::Validate(m_Graph, m_Validation);
		return true;
	}

	// Writes the graph back to m_Path. Void, not bool: failures are reported through
	// m_LastError, which the panel displays, rather than to a caller.
	void NPCBehaviorPanel::SaveTree()
	{
		// No destination. The message names both remedies ("Load a tree first, or set a
		// path"), though the second is not actually reachable from the toolbar -- the
		// path field only exists in the no-graph state.
		if (m_Path.empty())
		{
			m_LastError.Message = "No path. Load a tree first, or set a path.";
			return;
		}
		// The pre-existing comment, and the design rationale is sound: the author
		// finds out days later when the NPC misbehaves.
		//
		// The gap: this validates and refuses, but does not tell the author WHICH
		// errors -- they would have to press Validate to see them. The findings pane is
		// only refreshed on success below.
		// Refuse to save a tree that would not load back. A graph that saves
		// but fails to reload is worse than one that refuses to save: the
		// author finds out days later when the NPC misbehaves.
		ValidationResult vr;
		// Into a LOCAL, not straight into m_Validation.
		//
		// That is deliberate: on the error path m_Validation IS assigned (so the author
		// sees why), but on the success path it is not -- so a successful save leaves
		// whatever findings were there before. Since the save only happens when there are
		// none, that is consistent, but it means the pane is not refreshed by a save.
		VisualBlocks::Validator::Validate(m_Graph, vr);
		if (vr.HasErrors())
		{
			m_Validation = vr;
			m_LastError.Message = "Refusing to save: the tree has validation errors.";
			return;
		}

		GraphError err;
		// The write. Note this does NOT clear m_Dirty: saving the .wlbtree does not
		// recompile the .py, so the tree on disk may still differ from what an NPC
		// runs. m_Dirty means "not compiled", not "not saved".
		//
		// Which also means Save does not fix the "! not compiled" warning -- only
		// Compile && Write does.
		if (!BehaviorTree::SaveFile(m_Path, m_Graph, err))
			m_LastError = err;
		else
			m_GeneratedStatus = "saved";
	}

	// The public entry point for loading from elsewhere -- the content browser, a
	// recent list, or a tool call. The header explains why it is public rather than
	// forcing callers through the path field.
	bool NPCBehaviorPanel::OpenTree(const std::string &path)
	{
		if (!LoadTree(path))
			// Delegating entirely to LoadTree and returning its result unchanged.
			//
			// The pre-existing comment is the contract: a failed open leaves the previous
			// tree on screen. LoadTree honours that by only assigning m_Graph on success.
			//
			// m_Visible is NOT set here on the failure path either -- a failed open leaves
			// the panel exactly as it was.
			return false;   // LoadTree records the reason; the open tree stands
		m_Visible = true;
		return true;
	}

	// The end-to-end action: validate, compile, write the .py, clear the stale
	// bytecode, and mark the tree clean. This is what makes an NPC's behaviour
	// change.
	bool NPCBehaviorPanel::CompileAndWrite(std::string &outStatus)
	{
		// Guard one. Reachable from outside the panel (it is public), so unlike the
		// toolbar it cannot assume the caller already established a graph exists.
		//
		// outStatus is set in every failure branch, so the caller can show it verbatim.
		if (!m_HasGraph)
		{
			outStatus = "no tree open";
			return false;
		}
		// Guard two: writing needs a destination, and the .py is derived from the .py
		// path.
		//
		// The em dash in the message is a non-ASCII character in a string literal --
		// harmless for a UTF-8 source file, but it is the only one in this file's user-
		// facing strings.
		if (m_Path.empty())
		{
			outStatus = "no path — save the tree before compiling";
			return false;
		}

		// Validate first and refuse on error. The emitter interpolates
		// literals and operators into Python, so a tree that fails validation
		// must never reach the filesystem.
		ValidationResult vr;
		// Validate into a local, then assign to the member UNCONDITIONALLY on the line
		// after.
		//
		// Unlike SaveTree, this refreshes m_Validation on BOTH paths -- so a failed
		// compile leaves the author looking at the findings that caused it, which is the
		// behaviour that makes this action debuggable.
		VisualBlocks::Validator::Validate(m_Graph, vr);
		m_Validation = vr;
		if (vr.HasErrors())
		{
			m_Generated.clear();
			// The message is composed with the error COUNT but not the errors themselves.
			// So the toolbar's "!" popup says "Refusing to compile: 3 validation error(s)
			//" and the author must find the three in the findings pane above. Reasonable
			// division, since the pane already lists them.
			//
			// Only .Message is assigned, unlike the branch below which assigns the whole
			// GraphError -- so any other field of m_LastError keeps its previous value.
			m_LastError.Message = "Refusing to compile: " +
								  std::to_string(vr.ErrorCount()) + " validation error(s).";
			m_GeneratedStatus = "compile failed";
			outStatus = m_LastError.Message;
			return false;
		}

		// A local again, for the same reason as the toolbar's Compile: the file write
		// below must not happen unless the compile succeeded outright.
		std::string python;
		GraphError err;
		if (!BehaviorTree::Compile(m_Graph, python, vr, err))
		{
			m_Generated.clear();
			m_LastError = err;
			m_GeneratedStatus = "compile failed";
			outStatus = err.Message;
			return false;
		}
		m_Generated = python;

		// Write next to the tree, as <stem>.py.
		//
		// Beside the tree rather than into a shared generated/ directory: the
		// pairing is then obvious on disk, and a tree copied to another
		// project brings its compiled form with it instead of silently
		// resolving to nothing.
		// The path is COPIED into a filesystem::path rather than used in place, so
		// replace_extension below cannot mutate m_Path.
		//
		// That distinction matters: m_Path is the .wlbtree path and must survive this
		// function unchanged, or the next Save would target the .py.
		std::filesystem::path treePath(m_Path);
		std::filesystem::path pyPath = treePath;
		// The derived .py path. Replaces the extension rather than appending, so
		// "npc.wlbtree" becomes "npc.py" -- and any other extension would be replaced
		// the same way.
		//
		// Derived from `treePath`, which is why the copy above matters.
		pyPath.replace_extension(".py");

		std::error_code ec;
		// Creates the containing directory if needed.
		//
		// `ec` is passed so the call does NOT throw on failure -- the non-throwing
		// overload. And `ec` is never inspected: a failure here is ignored, and the
		// ofstream below reports the consequence. Deliberate, since the directory
		// normally exists.
		std::filesystem::create_directories(pyPath.parent_path(), ec);

		// BINARY, not text. On Windows a text-mode write would translate every newline,
		// so the generated Python's indentation -- which is significant in Python --
		// would still work but line endings would be CRLF and the byte count would
		// differ from what a Python tool expects.
		//
		// trunc so a recompile fully replaces the previous output rather than leaving a
		// tail of the old file if the new source is shorter. A DEFECT of this line: the
		// write is not atomic (no temp-file-then-rename), so a failure part-way through
		// leaves a truncated .py that the ScriptEngine will fail to import.
		std::ofstream out(pyPath, std::ios::binary | std::ios::trunc);
		if (!out)
		{
			m_LastError.Message = "Could not write " + pyPath.string();
			outStatus = m_LastError.Message;
			return false;
		}
		// The write itself. No trailing newline is added explicitly, though the emitter
		// supplies its own.
		//
		// No check on the stream state after writing -- a full disk or a network share
		// failing here is not detected, and the function proceeds to report success.
		out << python;
		out.close();

		// Written and closed cleanly, so the module is importable now. The
		// stale .pyc from a previous compile of the same stem is the one
		// thing that can still shadow it -- CPython checks mtime, but a
		// same-second edit on a coarse clock can slip through.
		// The stale-bytecode cleanup, which the pre-existing comment calls the one thing
		// that can still shadow a freshly written .py.
		//
		// The concern is real: CPython compares mtimes, and a same-second edit on a
		// coarse clock can leave the cached .pyc looking current.
		std::filesystem::path pycPath = pyPath;
		pycPath.replace_extension(".pyc");
		{
			std::error_code ec2;
			std::filesystem::remove(pycPath, ec2);
			// DEFECT (version hard-coded): the cached filename embeds `cpython-314`, so on
			// any other interpreter version the real .pyc has a different name and survives
			// -- defeating the entire purpose of this block. The version should come from
			// the running interpreter (sys.implementation.cache_tag) rather than being
			// written into C++.
			//
			// The legacy pyPath.replace_extension(".pyc") location above is also removed,
			// which covers the pre-3.2 layout; the __pycache__ one is the modern path.
			//
			// Both removes ignore their error codes, which is right -- a missing .pyc is the
			// expected case, not a failure.
			std::filesystem::remove(pyPath.parent_path() / "__pycache__" /
										(pyPath.stem().string() + ".cpython-314.pyc"),
									ec2);
		}

		// The tree is now in sync with what runs, so the "! not compiled" warning
		// clears. This is the ONLY place m_Dirty is set false after an edit.
		//
		// Set before the error reset, and only on the full success path -- so any failure
		// above leaves the warning up, which is the correct fail-safe.
		m_Dirty = false;
		m_LastError = GraphError();
		// The status names the output FILE, not just the fact of success -- so an author
		// with several trees can tell which .py was written.
		//
		// Duplicated into outStatus on the next line, so the caller and the panel agree.
		m_GeneratedStatus = "compiled -> " + pyPath.filename().string();
		// Mirroring the member into the out-parameter, so a caller driving this from a
		// tool gets the same message the panel would show.
		//
		// Note the toolbar discards this -- see the unused `status` local there.
		outStatus = m_GeneratedStatus;
		return true;
	}

}