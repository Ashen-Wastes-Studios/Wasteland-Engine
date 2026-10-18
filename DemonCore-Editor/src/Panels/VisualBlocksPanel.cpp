#include "wlpch.h"
#include "VisualBlocksPanel.h"

#include "GraphHistory.h"
#include "ValueEditor.h"


#include <imgui/imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "Wasteland/VisualBlocks/Compiler.h"
#include "Wasteland/VisualBlocks/GraphSerializer.h"

namespace Wasteland
{

	using namespace VisualBlocks;

	// ------------------------------------------------------------ file helpers

	namespace
	{
		// Read a whole file into a string.
		//
		// @param path  File to read.
		// @param ok    OUT: set false on any failure, true otherwise.
		//
		// The ok out-parameter rather than an exception or an optional, matching the
		// file-based helpers elsewhere in the editor.
		//
		// DEFECT: never called anywhere in this file. GraphSerializer::Load does the
		// reading for graphs; this is dead code left from before the serializer
		// existed. Harmless -- it is in an anonymous namespace, so the linker drops
		// it -- but it is a second, divergent way to read a file that a future edit
		// might reach for instead of the serializer.
		std::string ReadAll(const std::string &path, bool &ok)
		{
			std::ifstream in(path, std::ios::binary);
			// Tests the stream's own failbit, so a missing file, a permission problem
			// and a directory all land here.
			//
			// DEFECT: binary mode is set, but there is no check on a read error AFTER
			// the open succeeds -- a truncated or failing read returns a short string
			// with ok still true. GraphSerializer does the real loading so nothing here
			// depends on it today.
			if (!in)
			{
				ok = false;
				return std::string();
			}
			ok = true;
			// The iterator-pair constructor slurps the whole stream.
			//
			// Efficient for an unknown size, and it handles a zero-byte file correctly
			// (both iterators compare equal, giving an empty string) -- which matters
			// because test_project_seeding.cpp treats a zero-byte shader as a failure.
			return std::string(std::istreambuf_iterator<char>(in),
							   std::istreambuf_iterator<char>());
		}

		// Write a whole file, creating parent directories as needed.
		//
		// @param path     Destination file.
		// @param text     Contents.
		// @param outError OUT: set to a human-readable reason on failure.
		// @return         True on success.
		bool WriteAll(const std::string &path, const std::string &text,
					  std::string &outError)
		{
			// The non-throwing overloads below need one.
			//
			// DEFECT: `ec` is never inspected. create_directories can fail -- a path
			// under a file rather than a directory, or a permissions problem -- and this
			// code proceeds to the ofstream, which then fails and produces a less
			// specific message ('cannot open ... for writing') than the directory error
			// would have given. Checking it would turn a confusing message into an
			// accurate one.
			std::error_code ec;
			const std::filesystem::path parent =
				std::filesystem::path(path).parent_path();
			if (!parent.empty())
				// Creates the parent chain. Guarded by !parent.empty() because a bare
				// filename has no parent path, and passing an empty path to
				// create_directories is an error on some implementations.
				std::filesystem::create_directories(parent, ec);

			// trunc discards any existing contents.
			//
			// DEFECT: non-atomic. A failure partway through the write below leaves the
			// previous file already truncated, so a failed compile-and-write can destroy
			// the last good .py. Writing to a temporary and renaming would make the
			// replace atomic. Not a comment on the intent -- the intent is clearly to
			// overwrite -- just on the failure mode.
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				outError = "cannot open '" + path + "' for writing";
				return false;
			}
			// Buffered by the stream; the flush happens in the destructor.
			out << text;
			// Catches a failed write -- a full disk, or a bad descriptor.
			//
			// good() is the strictest of the three stream state tests, so it also
			// catches eofbit, which is why a partial read-then-fail is caught here too.
			if (!out.good())
			{
				outError = "write failed for '" + path + "'";
				return false;
			}
			return true;
		}

	} // namespace

	// -------------------------------------------------------------- lifecycle

	// Starts an empty flow graph.
	//
	// Sets Kind to "flow" explicitly, which matters: Validator::Validate
	// dispatches on Kind, so a graph left at its default would be validated by
	// the wrong rules.
	void VisualBlocksPanel::NewGraph()
	{
		m_Graph = Graph();
		m_Graph.Kind = "flow";
		m_Graph.Name = "NewGraph";
		m_HasGraph = true;
		// A new graph has no file yet.
		//
		// This is what makes SaveGraph refuse with a message rather than writing to
		// an empty path -- see the guard there.
		m_Path.clear();
		m_LastError = GraphError();
		m_Generated.clear();
		m_GeneratedStatus = "new graph";
		// A fresh graph IS unsaved work, so it starts dirty. The alternative --
		// starting clean -- means the author makes edits, sees no warning, and
		// closes the panel having written nothing.
		// A fresh graph IS unsaved work, as the comment says. Starting clean would
		// let the author close the panel having written nothing.
		//
		// DEFECT: m_History is NOT cleared here, though it is in LoadGraph. So undo
		// after NewGraph can restore a snapshot of the PREVIOUS graph -- and worse,
		// m_GeneratedCounter is not reset either, so node ids keep counting up
		// rather than starting from the new graph's own ids. LoadGraph resets the
		// history and the buffers; NewGraph resets neither, so the two entry points
		// that both replace the graph do not agree on what to discard.
		m_Dirty = true;
		m_FramedOnce = false;
		Validator::ValidateFlow(m_Graph, m_Validation);
	}

	// Loads a .wlblocks, replacing whatever is open.
	//
	// @param path  Graph file to load.
	// @return       True on success; on failure the open graph is untouched.
	//
	// The failure contract -- leave the current graph alone -- is what lets an
	// author keep their work on screen while investigating a broken file.
	bool VisualBlocksPanel::LoadGraph(const std::string &path)
	{
	// A fresh file starts with no history and no edit buffers: undo must
	// not reach back into a different graph, and a buffer seeded from the
	// previous file would overwrite this one's values on first edit.
		// Undo must not reach back into a different graph. Without this, Ctrl+Z
		// after opening a file would restore the previously open one -- the exact
		// scenario GraphHistory's own class comment says must not happen.
		m_History.Clear();
		// And the per-field edit buffers go too, for the same reason: a buffer
		// seeded from the previous file's text would overwrite this graph's values
		// on first edit.
		ValueEditor::ResetBuffers();
		// Loaded into a LOCAL, not into m_Graph.
		//
		// That is the mechanism behind the failure contract: m_Graph is only
		// assigned once the load has fully succeeded, so every early return below
		// leaves it untouched without needing to restore it.
		Graph g;
		GraphError err;
		if (!GraphSerializer::Load(path, g, err))
		{
			// The open graph stands. An author who opens a broken file should
			// still have their own work on screen, with the reason beside it.
			m_LastError = err;
			return false;
		}
		// A `.wlbtree` opened here would compile as a flow graph and produce
		// nonsense, so it is rejected with a reason that names the fix rather
		// than silently "working".
		// Rejects behaviour trees opened in the wrong panel.
		//
		// Checked on the LOCAL g, before m_Graph is touched, so the rejection also
		// preserves the open graph.
		//
		// DEFECT: compares against the string "tree" rather than whatever the
		// Kind field's domain is. Every Kind comparison in this file is a literal --
		// NewGraph writes "flow" at line 71 and this reads "tree" -- so renaming
		// a Kind value in the serializer would silently make this guard accept
		// everything. An enum for Kind would make it a compile error.
		if (g.Kind == "tree")
		{
			m_LastError.Message =
				"'" + path +
				"' is a behaviour tree. Open .wlbtree graphs in the NPC Behavior "
				"panel.";
			return false;
		}

		// The commit point. Everything before this line was validation of the
		// candidate; everything after is bookkeeping on an accepted graph.
		m_Graph = g;
		m_HasGraph = true;
		m_Path = path;
		m_LastError = GraphError();
		m_Generated.clear();
		m_GeneratedStatus = "loaded";
		// Matches the file on disk -- this is the one place the flag starts false,
		// and it is why the header's IsDirty comment should be read as 'edited
		// since load', not 'edited since compile'.
		m_Dirty = false;   // matches the file on disk
		m_FramedOnce = false;
		Validator::ValidateFlow(m_Graph, m_Validation);

		// Open with something selected.
		//
		// A node editor that opens with an empty inspector reads as broken:
		// the author has to know they must click before they can see
		// anything at all. Selecting the first node shows a real, editable
		// node immediately. Nodes are in authored order, so this is the
		// node the author put first -- not an arbitrary one.
		// Selects the first node, or clears the selection on an empty graph.
		//
		// The ternary is there because Select takes a string and an empty graph has
		// no front() to call. Opening with something selected is a deliberate UX
		// choice, argued in the comment above.
		m_Canvas.Select(m_Graph.Nodes.empty() ? std::string()
													: m_Graph.Nodes.front().Id);
		return true;
	}

	// The public entry point: load, then reveal the panel.
	//
	// @return  True on success; the panel is made visible as a side effect.
	bool VisualBlocksPanel::OpenGraph(const std::string &path)
	{
		// Propagates the failure. No visibility change on the error path -- a failed
		// open must not also steal focus from whatever the author was looking at.
		if (!LoadGraph(path))
			return false;   // LoadGraph recorded the reason
		m_Visible = true;
		return true;
	}

	// Writes the .wlblocks, refusing to write a graph that will not load back.
	//
	// @return  True on success.
	bool VisualBlocksPanel::SaveGraph()
	{
		// A new graph has nowhere to go yet. The message points at the toolbar
		// rather than silently choosing a path.
		if (m_Path.empty())
		{
			m_LastError.Message =
				"No path. Set one in the toolbar before saving.";
			return false;
		}
		// Refuse to save a graph that will not load back. A file that saves
		// and then fails to reopen is worse than one that refuses to save: the
		// author finds out later, with no way back to the unsaved state.
		// A local result, assigned to m_Validation below so the findings panel
		// reflects what was actually checked rather than a stale earlier run.
		ValidationResult vr;
		Validator::ValidateFlow(m_Graph, vr);
		// Refreshed even on the failure path below, so the author sees the errors
		// that caused the refusal -- the save guard doubles as a way to display
		// them.
		m_Validation = vr;
		// DEFECT, minor: the bool return of ValidateFlow is discarded in favour of
		// the out-parameter. Validator.h explicitly warns that the two overlap and
		// that a caller must not read one as authoritative over the other; this
		// reads only the out-parameter, which is the safer half, but the discarded
		// bool makes the call site look inconsistent with the header's guidance.
		if (vr.HasErrors())
		{
			m_LastError.Message =
				"Refusing to save: " + std::to_string(vr.ErrorCount()) +
				" validation error(s). Fix them, or the graph will not load.";
			return false;
		}

		GraphError err;
		// The serializer reports its own error, so m_LastError takes the whole
		// struct rather than just a message -- unlike the two paths above, which
		// assign to .Message alone and leave the rest of the struct stale.
		if (!GraphSerializer::Save(m_Path, m_Graph, err))
		{
			m_LastError = err;
			return false;
		}
		m_Dirty = false;
		// Saved. Note this is the second meaning of the flag: LoadGraph uses it for
		// 'differs from disk', and SaveGraph clears it -- so it tracks the .wlblocks,
		// not the generated .py. The toolbar's own text says 'Unsaved / uncompiled',
		// which conflates the two.

		// Both cleared here and in CompileAndWrite below -- so m_Dirty means
		// 'differs from what was last written to EITHER file', which is a
		// third meaning after LoadGraph's 'differs from the .wlblocks'.
		m_GeneratedStatus = "saved";
		return true;
	}

	// The action that makes a graph live: validate, compile, save the graph, and
	// write the generated Python beside it.
	//
	// @param outStatus  OUT: a human-readable result for the UI.
	// @return           True on success.
	bool VisualBlocksPanel::CompileAndWrite(std::string &outStatus)
	{
		if (!m_HasGraph)
		{
			outStatus = "no graph open";
			return false;
		}
		// Same guard as SaveGraph, with its own message because the method writes
		// TWO files and the author needs to know which one is blocked.
		if (m_Path.empty())
		{
			outStatus = "no path - set one in the toolbar before compiling";
			return false;
		}

		// Validate BEFORE compiling. The emitter interpolates literals and
		// operators into Python source, so a graph that fails validation must
		// never reach the filesystem.
		// A SECOND validation pass. SaveGraph below validates again on the same
		// graph, so a compile validates the graph twice.
		//
		// DEFECT: redundant work, and a small inconsistency -- this method validates
		// into m_Validation while SaveGraph validates into its own local before
		// assigning. Both are correct in isolation; together they mean every compile
		// walks the whole graph an extra time for no benefit, since the graph cannot
		// change between the two calls.
		ValidationResult vr;
		Validator::ValidateFlow(m_Graph, vr);
		m_Validation = vr;
		if (vr.HasErrors())
		{
			m_Generated.clear();
			outStatus = "refused: " + std::to_string(vr.ErrorCount()) +
						" validation error(s)";
			return false;
		}

		GraphError err;
		// Compiles to Python source text. Failure is signalled by an EMPTY string,
		// which is why the check below tests emptiness rather than an error code.
		const std::string source = Compiler::CompileFlow(m_Graph, err);
		if (source.empty())
		{
			m_LastError = err;
			m_Generated.clear();
			outStatus = "compile failed: " + err.Message;
			return false;
		}

		// Write the GRAPH too, not just the Python. Compiling writes the
		// generated file but leaves the authored graph stale on disk, so the
		// next open reverts every edit -- the author sees their work vanish.
		// Writes the GRAPH too, not just the Python.
		//
		// The comment above is right that omitting this loses every edit on the next
		// open. Note the ordering consequence: the .wlblocks is updated BEFORE the
		// .py is written below, so a failure in the Python write leaves the graph
		// saved and the Python stale -- m_Dirty is false at that point, so nothing
		// on screen signals the mismatch.
		if (!SaveGraph())
		{
			outStatus = "graph save failed: " + m_LastError.Message;
			return false;
		}

		// The Python lives beside the graph with a .py extension, matching the
		// behaviour panel, so both formats produce an inspectable artefact in
		// the same place.
		// Derives the Python path from the graph path: same directory, same stem,
		// .py extension.
		//
		// Matching the behaviour panel, so both formats leave an inspectable
		// artefact in the same place.
		//
		// DEFECT: p.stem() strips the LAST extension only. A graph named
		// player.wlblocks.backup would produce player.wlblocks.py, and one named
		// my.graph.wlblocks would produce my.graph.py -- neither wrong, but neither
		// obvious to the author either. Worth knowing the mapping is mechanical.
		std::string pyPath = m_Path;
		const std::filesystem::path p(pyPath);
		pyPath = (p.parent_path() / (p.stem().string() + ".py")).string();

		std::string writeError;
		if (!WriteAll(pyPath, source, writeError))
		{
			m_LastError.Message = writeError;
			outStatus = "python write failed";
			return false;
		}

		// Cached so the DrawGenerated panel can show the source without recompiling
		// every frame.
		m_Generated = source;
		// Cleared by the COMPILE, not just by a save -- reinforcing that the flag
		// does not mean what its name suggests.
		m_Dirty = false;
		// Records the artefact's location, so the author can find the .py without
		// guessing the naming rule.
		m_GeneratedStatus = "compiled -> " + pyPath;
		outStatus = m_GeneratedStatus;
		return true;
	}

	// ------------------------------------------------------------- graph edits

	// Generates a node id from the type plus a monotonic counter.
	//
	// @param typeId  Registry type id.
	// @return        A unique-within-this-session id.
	//
	// DEFECT: unique only within a session. The counter starts at 0 for a fresh
	// panel and is never seeded from a LOADED graph's ids, so opening a graph
	// whose nodes are already named FlowOnEvent_1, FlowOnEvent_2 and then adding
	// one from the palette produces FlowOnEvent_1 again -- a duplicate id.
	// Everything keyed on node id then refers to two different nodes, and the
	// wire the author just drew attaches to whichever was found first.
	// Seeding the counter from the loaded graph (or scanning for an unused id)
	// would close it.
	std::string VisualBlocksPanel::MakeNodeId(const std::string &typeId)
	{
		// Unique by construction. Two `Add` nodes must not share an id, or the
		// second silently becomes unreachable and every wire into the first
		// drives both.
		// Pre-increment, so the first id ends in 1 rather than 0.
		//
		// The type prefix is for human legibility in the inspector's id line, and
		// the suffix is what makes it unique.
		return typeId + "_" + std::to_string(++m_GeneratedCounter);
	}

	// Exact four-part match: is this precise wire already present?
	//
	// @return  True if that exact wire exists.
	//
	// Linear scan over every link. Fine at graph scale, and the duplicate-wire
	// check it enables is what stops two identical wires accumulating from a
	// double-click.
	bool VisualBlocksPanel::HasLink(const std::string &fromNode,
									const std::string &fromPin,
									const std::string &toNode,
									const std::string &toPin) const
	{
		for (const GraphLink &l : m_Graph.Links)
		{
			if (l.FromNode == fromNode && l.FromPin == fromPin &&
				l.ToNode == toNode && l.ToPin == toPin)
				return true;
		}
		return false;
	}

	// The stricter question: is this INPUT fed at all?
	//
	// @return  True if any wire targets that input pin.
	//
	// Distinct from HasLink above and deliberately so: this is what enforces
	// one producer per input, so which wire the compiler honours is never an
	// accident of declaration order.
	bool VisualBlocksPanel::InputHasLink(const std::string &toNode,
										 const std::string &toPin) const
	{
		for (const GraphLink &l : m_Graph.Links)
		{
			if (l.ToNode == toNode && l.ToPin == toPin)
				return true;
		}
		return false;
	}

	// Creates a node of the given type and selects nothing -- the canvas shows
	// it in place.
	//
	// @param typeId  Registry type id from the palette.
	void VisualBlocksPanel::AddNodeFromPalette(const std::string &typeId)
	{
		// Snapshot BEFORE anything changes. Correct even though the very next line
		// can return early -- see the note there.
		BeginEdit();
		const NodeDef *def = NodeRegistry::Get().Find(typeId);
		// An unregistered type id.
		//
		// DEFECT: returns WITHOUT CommitEdit. BeginEdit above already pushed a
		// snapshot, so the undo stack now holds a duplicate of the current state,
		// and no CollapseIdentical runs to remove it. The next real edit's
		// CollapseIdentical pops one, but the stack is left one entry deeper than it
		// should be -- a stray undo step that appears to do nothing. The same
		// unpaired-BeginEdit pattern appears in DeleteNode, Connect and DisconnectAt
		// below.
		if (def == nullptr)
			return;

		GraphNode node;
		node.Id = MakeNodeId(typeId);
		node.TypeId = typeId;

		// Lay the new node out in a column so it does not land on top of an
		// existing one. Cascading from the node count means each new node
		// lands in free space without the panel tracking coordinates.
		// The new node's index in the current list, used only for layout.
		const int index = static_cast<int>(m_Graph.Nodes.size());
		// Cascading grid: five columns, 230px apart.
		//
		// A cascade rather than a stack because a stack puts every new node exactly
		// on top of the last one, hiding it. Cascading from the node count means no
		// position tracking is needed at all.
		//
		// DEFECT: the layout is derived from node COUNT, not from what is already on
		// screen. Delete a node and the next one added reuses that slot, so two nodes
		// can occupy the same coordinates after a delete-and-add cycle.
		node.X = 60.0f + static_cast<float>(index % 5) * 230.0f;
		node.Y = 60.0f + static_cast<float>(index / 5) * 120.0f;

		// Seed each unlinked input pin with its declared default, so a node
		// added from the palette is immediately runnable rather than failing
		// validation on every required pin until each is filled by hand.
		// Seeds defaults from the registry's own declaration, which is the whole
		// schema-driven point: a node registered tomorrow gets working defaults
		// without any editor code.
		for (const PinDef &pin : def->Pins)
		{
			// Only data inputs. Exec pins have no literal, and output pins are driven
			// by their node.
			if (pin.Dir != PinDir::In || pin.Type == PinType::Exec)
				continue;
			if (!pin.DefaultLiteral.empty())
				node.SetLiteral(pin.Id, pin.DefaultLiteral, true);
		}

		// An event node is what a flow graph hangs off; adding one is the
		// usual first action, so give it the default hook.
		// Gives the event node a default hook.
		//
		// DEFECT: a hardcoded TypeId string in the editor -- the exact thing
		// ValueEditor.h's class comment says this editor is designed to avoid. It
		// also means renaming the node in the registry silently stops this from
		// working, and the node arrives with an empty event name. The default
		// belongs on the PinDef or NodeDef in the registry, where it would be
		// declared once and validated with the rest of the schema.
		if (std::string(def->TypeId) == "FlowOnEvent" && node.EventName.empty())
			node.EventName = "OnUpdate";

		// Appended, so authored order is creation order -- which is why LoadGraph
		// selecting Nodes.front() picks the node the author put first.
		m_Graph.Nodes.push_back(node);
		CommitEdit();
		Validator::ValidateFlow(m_Graph, m_Validation);
	}

	// Removes a node and everything attached to it.
	//
	// @param nodeId  Id to delete; empty is a no-op.
	void VisualBlocksPanel::DeleteNode(const std::string &nodeId)
	{
		BeginEdit();
		if (nodeId.empty())
			return;

		// Remove the node AND every wire touching it. Leaving orphan wires
		// behind produces findings that name pins on a node the author can no
		// longer see, which reads as a validator bug rather than a stale edit.
		// Cascade-delete the wires FIRST, before the node itself.
		//
		// remove_if plus erase rather than a loop: one pass, and the surviving links
		// keep their relative order, which the graph fingerprint in GraphHistory
		// depends on -- reordering links would make an unrelated edit look like a
		// change to every link after the removed one.
		m_Graph.Links.erase(
			std::remove_if(m_Graph.Links.begin(), m_Graph.Links.end(),
						   [&](const GraphLink &l) {
							   return l.FromNode == nodeId || l.ToNode == nodeId;
						   }),
			m_Graph.Links.end());

		m_Graph.Nodes.erase(
			std::remove_if(m_Graph.Nodes.begin(), m_Graph.Nodes.end(),
						   [&](const GraphNode &n) { return n.Id == nodeId; }),
			m_Graph.Nodes.end());

		// Clears the selection rather than letting it dangle on a node that no
		// longer exists -- DrawInspector would otherwise render an empty panel with
		// no explanation.
		if (m_Canvas.GetSelected() == nodeId)
			m_Canvas.Select(std::string());

		CommitEdit();
		Validator::ValidateFlow(m_Graph, m_Validation);
	}

	// Adds a wire, refusing the ambiguous cases.
	//
	// @param fromNode,fromPin  Producer.
	// @param toNode,toPin      Consumer input.
	void VisualBlocksPanel::Connect(const std::string &fromNode,
									const std::string &fromPin,
									const std::string &toNode,
									const std::string &toPin)
	{
		BeginEdit();
		// Three rejections: missing endpoints, and self-connection.
		//
		// The self-connection check is a deliberate refusal rather than a validator
		// error -- a node feeding its own input is nearly always a mistake, and
		// allowing it risks an infinite loop in the generated Python.
		//
		// Note the pins are NOT validated here, so a wire from an output pin to an
		// output pin is only caught later by ValidateFlow.
		if (fromNode.empty() || toNode.empty() || fromNode == toNode)
			return;
		if (HasLink(fromNode, fromPin, toNode, toPin))
			return;

		// One producer per input. Which of two wires the compiler would honour
		// is an accident of declaration order, so the second is refused here
		// rather than reported by the validator after the author has wired it.
		// One producer per input, refused here with a message naming the pin and
		// node rather than reported by the validator afterwards.
		if (InputHasLink(toNode, toPin))
		{
			m_LastError.Message =
				"'" + toPin + "' on " + toNode + " already has an input.";
			return;
		}

		GraphLink link;
		link.FromNode = fromNode;
		link.FromPin = fromPin;
		link.ToNode = toNode;
		link.ToPin = toPin;
		m_Graph.Links.push_back(link);
		// Clears any previous error on a SUCCESSFUL connect.
		//
		// The only edit path that does this -- AddNodeFromPalette, DeleteNode,
		// DisconnectAt and the literal edits all leave a stale m_LastError message
		// on screen after a fix. Inconsistent, though harmless since the message is
		// only informational.
		m_LastError = GraphError();
		CommitEdit();
		Validator::ValidateFlow(m_Graph, m_Validation);
	}

	// Removes every wire from one output pin.
	//
	// @param fromNode,fromPin  The producer to cut loose from.
	//
	// Keyed on the PRODUCER rather than the consumer, which is what lets the
	// inspector's Disconnect button work: it knows the link it resolved, and
	// cutting by producer removes exactly that wire.
	void VisualBlocksPanel::DisconnectAt(const std::string &fromNode,
										 const std::string &fromPin)
	{
		BeginEdit();
		m_Graph.Links.erase(
			std::remove_if(m_Graph.Links.begin(), m_Graph.Links.end(),
						   [&](const GraphLink &l) {
							   return l.FromNode == fromNode && l.FromPin == fromPin;
						   }),
			m_Graph.Links.end());
		CommitEdit();
		Validator::ValidateFlow(m_Graph, m_Validation);
	}

	// ----------------------------------------------------------------- drawing

	// The toolbar: history, file actions, path entry, and status.
	void VisualBlocksPanel::DrawToolbar()
	{
		// ID scope for the whole toolbar, so its widget ids cannot collide with
		// another panel's -- every other Draw* function does the same with its own
		// prefix.
		ImGui::PushID("vb_toolbar");

		// Undo/redo lead, because they are the controls a node editor
		// is judged on and the ones with no keyboard route until the
		// shortcut is wired.
		// Greys out Undo when there is nothing to undo.
		//
		// This reads CanUndo(), which tests m_UndoDepth rather than the deque's
		// emptiness -- so per GraphHistory's own defect notes, the button can be
		// enabled when Undo() would return false. The user sees a live button that
		// does nothing.
		ImGui::BeginDisabled(!m_History.CanUndo());
		if (ImGui::Button("Undo"))
		{
			Undo();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_History.CanRedo());
		if (ImGui::Button("Redo"))
		{
			Redo();
		}
		ImGui::EndDisabled();
		// The step count, shown only when there is history.
		//
		// Same depth-versus-emptiness caveat as above: this can display a count that
		// the actual undo stack does not have.
		if (m_History.UndoDepth() > 0)
			ImGui::SameLine();
		if (m_History.UndoDepth() > 0)
			// Static_cast to int for the %d format. size_t would be wrong for %d --
			// harmless at these magnitudes, wrong in principle on a 64-bit build.
			ImGui::TextDisabled("%d step(s)", static_cast<int>(m_History.UndoDepth()));
		ImGui::SameLine();
		if (ImGui::Button("Load"))
		{
			// Loads from the text buffer, not from m_Path.
			//
			// So the toolbar's Load is really 'load whatever path is typed here' --
			// which is how an author opens a different file without a file dialog. The
			// return is discarded, but LoadGraph records the reason in m_LastError, which
			// the toolbar displays below.
			LoadGraph(m_PathBuf);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save"))
		{
			SaveGraph();
		}
		ImGui::SameLine();
		// The `&&` in the label is the author's shorthand for 'do both', not a C++
		// operator -- it is inside a string literal.
		if (ImGui::Button("Compile && Write"))
		{
			// A local for the out-parameter.
			//
			// DEFECT: the status is written here and then DISCARDED. The toolbar relies
			// on m_GeneratedStatus, which CompileAndWrite also sets -- so the local is
			// pure waste, and it means the toolbar shows only the status CompileAndWrite
			// chose to mirror into the member, not the out-parameter it was given.
			std::string status;
			CompileAndWrite(status);
		}
		ImGui::SameLine();
		if (ImGui::Button("New"))
		{
			NewGraph();
		}

		ImGui::Checkbox("Palette", &m_ShowPalette);

		ImGui::Separator();
		ImGui::TextUnformatted("Path");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		// The `##` prefix hides the label and uses the rest as the widget id, so the
		// field shows no caption while staying uniquely identified.
		//
		// sizeof rather than a separate constant, because m_PathBuf is a real array
		// member -- which is why it is an array and not a std::string.
		ImGui::InputText("##vbpath", m_PathBuf, sizeof(m_PathBuf));

		// The unsaved warning. Amber, and placed before the status text so both can
		// appear on one line.
		if (m_Dirty)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
							   "Unsaved / uncompiled - the file on disk is stale");
		}
		if (!m_GeneratedStatus.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", m_GeneratedStatus.c_str());
		}
		if (!m_LastError.Message.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
							   m_LastError.Message.c_str());
		}

		ImGui::PopID();
	}

	// The node palette: every flow node, grouped by category.
	void VisualBlocksPanel::DrawPalette()
	{
		if (!m_ShowPalette)
			return;

		ImGui::Separator();
		ImGui::TextUnformatted("Flow Nodes");

		// Flow nodes only. A `.wlblocks` graph is a flow graph, so offering the
		// behaviour taxonomy here would produce graphs that validate as
		// behaviour trees and compile to nothing.
		// COPIES the vector of pointers every frame.
		//
		// Each NodeDef* stays valid -- it points into the registry's table, which
		// does not change -- but the vector itself is rebuilt per frame by a full
		// scan of m_Defs. DEFECT: minor per-frame allocation and scan in a function
		// called every frame. The palette is only drawn when m_ShowPalette is true,
		// which limits the cost, and caching it would risk staleness if the registry
		// is ever extended at runtime.
		const std::vector<const NodeDef *> nodes = NodeRegistry::Get().FlowNodes();
		std::string currentCategory;
		for (const NodeDef *def : nodes)
		{
			if (def == nullptr)
				continue;
			// Groups by category, relying on the registry returning nodes already
			// sorted by it.
			//
			// DEFECT: nothing enforces that ordering. If the registry's order ever
			// changes, the same category header is printed repeatedly as the palette is
			// walked -- cosmetically wrong rather than broken, but the grouping silently
			// depends on an invariant the registry does not promise.
			if (def->Category != currentCategory)
			{
				currentCategory = def->Category;
				ImGui::TextDisabled("%s", currentCategory.c_str());
			}
			if (ImGui::SmallButton(def->TypeId.c_str()))
				AddNodeFromPalette(def->TypeId);
		}
	}

	// The per-node body drawn inside the canvas box.
	//
	// @param node  The node being drawn.
	// @return       Whether the body consumed the click.
	//
	// Always returns false as written, which matches the comment above: inputs
	// are edited in the inspector, not in the node, precisely so a text field
	// cannot fight the canvas for a drag.
	//
	// So the return value exists for a contract the current body does not use.
	bool VisualBlocksPanel::DrawNodeBody(const GraphNode &node)
	{
		const NodeDef *def = NodeRegistry::Get().Find(node.TypeId);
		if (def == nullptr)
			return false;

		// Inputs are edited in the side inspector, not inside the node: a text
		// field inside a box that is also a drag target fights the canvas for
		// the click. The canvas renders the pins; the inspector owns values.
		// Scoped by node id so two nodes' bodies cannot produce the same widget id
		// -- which would make ImGui treat them as one widget and cross-wire their
		// state.
		ImGui::PushID(node.Id.c_str());
		ImGui::TextDisabled("%s", node.Id.c_str());
		ImGui::PopID();
		return false;
	}

	// The canvas, plus the translation of canvas events into graph edits.
	void VisualBlocksPanel::DrawCanvas()
	{
		if (!m_HasGraph)
		{
			ImGui::TextDisabled("No graph open. Load a .wlblocks file, or New.");
			return;
		}

		// A per-frame output struct. Local rather than a member because the canvas
		// fills it fresh each frame and nothing outside this function reads it.
		NodeCanvas::Events events;
		m_Canvas.Render(m_Graph, NodeRegistry::Get(), events,
						[this](const GraphNode &node) {
							return DrawNodeBody(node);
						});

		// The canvas detects a completed drag and the PANEL performs the edit.
		//
		// That split is deliberate: the canvas owns geometry and interaction, the
		// panel owns the graph and its rules -- so the one-producer-per-input
		// refusal in Connect applies to a canvas-initiated wire exactly as it does
		// to a programmatic one.
		if (events.WireAdded)
			Connect(events.LinkFromNode, events.LinkFromPin,
					events.LinkToNode, events.LinkToPin);
		if (events.NodeDeleted)
			DeleteNode(events.SelectedNodeId);
		// The only edit path that sets dirty WITHOUT bracketing itself with
		// BeginEdit/CommitEdit.
		//
		// DEFECT: a node drag is therefore not undoable, and does not run
		// CollapseIdentical. Either it should bracket the move, or node movement
		// should be excluded from history deliberately -- but as written it is an
		// oversight, since every other mutation in this file is undoable. Note the
		// position change also has to reach GraphHistory::Fingerprint, which does
		// include X and Y -- so the machinery to support it is already there.
		if (events.NodeMoved)
			m_Dirty = true;

		// Frame the content once per open graph. Re-framing every frame would
		// fight the author's panning and make the canvas unusable.
		// Frames the content once per open graph.
		//
		// Three guards: not yet framed, the author has not panned (so their view is
		// never overridden), and there is something to frame.
		if (!m_FramedOnce && !m_Canvas.HasBeenPanned() && !m_Graph.Nodes.empty())
		{
			m_Canvas.FrameAll(m_Graph, NodeRegistry::Get());
			m_FramedOnce = true;
		}
	}

	// The text a pin shows before it has been authored.
	//
	// @param type  Pin type.
	// @return       A static string literal the COMPILER accepts.
	//
	// The 'compiler accepts' constraint is the load-bearing part, and it is a
	// duplication risk: NodeRegistry.cpp's InTyped has its own per-type fallback
	// list. The two are written independently and must agree -- if they diverge,
	// a pin displays one value and compiles to another, with nothing to catch
	// it. One shared table would remove the hazard.
	const char *VisualBlocksPanel::DefaultLiteralText(PinType type) const
	{
		// The value a pin shows before it is authored. Must be a value the
		// COMPILER accepts, not merely one the widget can display: a field
		// seeded with "" for a float would emit an empty expression and fail
		// to compile.
		switch (type)
		{
		case PinType::Bool:   return "False";
		case PinType::Int:    return "0";
		case PinType::Float:  return "0.0";
		case PinType::Vec2:   return "0.0, 0.0";
		case PinType::Vec3:   return "0.0, 0.0, 0.0";
		case PinType::Vec4:   return "0.0, 0.0, 0.0, 0.0";
		// Empty for strings.
		//
		// DEFECT: an empty string is indistinguishable from 'no default', and the
		// default branch below returns the same thing. A String pin therefore shows
		// blank until the author types, which is arguably right -- but a caller
		// cannot tell 'no default' from 'default is empty'.
		case PinType::String: return "";
		// "player" for an unlinked entity pin.
		//
		// Note the contrast with NodeRegistry.cpp's InTyped, whose Entity fallback
		// is "self". The two defaults disagree: an unlinked entity pin displays
		// "player" here while the emitter substitutes "self" if the value is
		// empty. Concrete instance of the divergence risk noted above.
		case PinType::Entity: return "player";
		default:              return "";
		}
	}

	// Snapshot the graph before a mutation.
	void VisualBlocksPanel::BeginEdit()
	{
		// Snapshot BEFORE mutating. Undo restores what is pushed now, so
		// pushing after the edit would make undo a no-op.
		// Push BEFORE mutating, which is what makes undo restore the previous state
		// rather than being a no-op.
		m_History.Push(m_Graph);
	}

	// Finish a bracketed edit: collapse no-ops, mark dirty, revalidate, and
	// drop the stale generated source.
	void VisualBlocksPanel::CommitEdit()
	{
		// A keystroke changes the graph every frame. Collapsing an edit
		// that turned out to change nothing is what keeps one drag from
		// filling the whole undo stack.
		// Drops the snapshot if the edit changed nothing, so holding a key does not
		// fill the undo stack with duplicates.
		m_History.CollapseIdentical(m_Graph);
		m_Dirty = true;
		// Dispatching Validate rather than ValidateFlow.
		//
		// DEFECT: inconsistent with the rest of this file, which calls ValidateFlow
		// everywhere else. Since Validate dispatches on graph.Kind, the two agree
		// for a flow graph -- so this works today and would break only if Kind were
		// ever something else, which LoadGraph's "tree" guard prevents. The
		// inconsistency is a latent trap rather than a live bug.
		Validator::Validate(m_Graph, m_Validation);
		// The generated source on screen is the authored graph's shadow. If
		// it is not refreshed on every edit, the panel shows Python that
		// does not match the graph the author is looking at.
		// The shown Python is invalidated on every edit, so the panel can never
		// display source that disagrees with the graph in front of the author.
		m_Generated.clear();
		m_GeneratedStatus.clear();
	}

	// Restores the previous state and brings every derived view back into step.
	void VisualBlocksPanel::Undo()
	{
		// Nothing to undo; the derived state is deliberately left untouched rather
		// than being needlessly cleared.
		if (!m_History.Undo(m_Graph))
			return;
		// A restored graph must also drop its edit buffers, or a field
		// shows the value from the state we just left.
		// Drops every edit buffer, not just the affected node's.
		//
		// DEFECT: ValueEditor also offers ResetBuffersFor(nodeId), which would be
		// the precise fix -- clearing all buffers discards uncommitted typing in
		// every OTHER field too. Correct, but heavier than it needs to be.
		ValueEditor::ResetBuffers();
		m_Dirty = true;
		Validator::Validate(m_Graph, m_Validation);
		m_Generated.clear();
		m_GeneratedStatus.clear();
	}

	// The mirror of Undo above, body for body.
	//
	// DEFECT: the duplication is real -- the two functions are identical except
	// for which history method they call. A single Restore(bool forward) helper
	// would halve it, and would guarantee the two cannot drift apart as more
	// derived state is added.
	void VisualBlocksPanel::Redo()
	{
		if (!m_History.Redo(m_Graph))
			return;
		ValueEditor::ResetBuffers();
		m_Dirty = true;
		Validator::Validate(m_Graph, m_Validation);
		m_Generated.clear();
		m_GeneratedStatus.clear();
	}

	// The side panel for the selected node: name, pins, and delete.
	void VisualBlocksPanel::DrawInspector()
	{
		ImGui::Separator();
		ImGui::TextUnformatted("Node");

		if (!m_Canvas.HasSelection())
		{
			ImGui::TextDisabled("Select a node on the canvas.");
			return;
		}

		// A raw POINTER into the graph's node vector, not a reference or a copy.
		//
		// Safe only as long as nothing reallocates m_Graph.Nodes while this
		// function runs -- and DeleteNode at the bottom of this function DOES erase
		// from that vector, after which `node` dangles. The use is immediate (the
		// button reads node->Id to build the id string) so no dereference happens
		// after the erase, but the pattern is fragile: any future code that touched
		// node after a delete would be a use-after-free.
		GraphNode *node = m_Graph.FindNode(m_Canvas.GetSelected());
		if (node == nullptr)
			return;

		// The instance NAME, editable. Blank means "show the type", so a
		// graph nobody renamed is byte-identical to before.
		// Only labels this node: the compiler addresses nodes by TypeId,
		// never by this, so renaming cannot change behaviour.
		ImGui::TextUnformatted("Name");
		std::string titleOwner;
		std::string titleKey;
		// The per-field buffer for the name.
		//
		// titleOwner and titleKey are LOCALS whose addresses are passed so the
		// buffer map can key on them and keep the storage alive -- the mechanism
		// ValueEditor.h documents as making a buffer outlive one frame without a
		// string per keystroke.
		char *titleBuf = ValueEditor::Buffer(titleOwner, titleKey, node->Id,
															"title", node->Title);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##title", titleBuf, kLiteralBufferSize))
		{
			// Trim: a name of only spaces still suppresses the type name and
			// reads as a blank header, which is never what someone wanted.
			std::string typed(titleBuf);
			// Trailing-whitespace trim, as a loop rather than find_last_not_of.
			//
			// Only trailing: a leading space survives, so a name of ' x' renders with a
			// visible gap. Either both ends should be trimmed or the comment should say
			// why only one.
			while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\t'))
				typed.pop_back();
			// Only commits a real change.
			//
			// Without this, every keystroke would push an undo snapshot even when the
			// typed text was identical to what was stored -- which is what
			// CollapseIdentical exists to clean up after.
			if (node->Title != typed)
			{
				BeginEdit();
				node->Title = typed;
				CommitEdit();
			}
		}
		if (node->Title.empty())
			ImGui::TextDisabled("showing type: %s", node->TypeId.c_str());
		ImGui::TextDisabled("id: %s", node->Id.c_str());

		const NodeDef *def = NodeRegistry::Get().Find(node->TypeId);
		if (def == nullptr)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
							   "Unknown node type. It cannot be edited.");
			return;
		}

		ImGui::Separator();

		// One iteration per input pin, with the widget chosen by the pin's
		// declared type.
		for (const PinDef &pin : def->Pins)
		{
			if (pin.Dir != PinDir::In)
				continue;

			// A linked pin shows its source instead of an editable value:
			// editing a literal on a connected pin would look like it took
			// effect while the wire kept winning.
			PinLiteral lit;
			bool isLinked = false;
			std::string linkNode;
			std::string linkPin;
			// One call answering three questions at once: the literal, whether the pin
			// is linked, and the producer's identity.
			//
			// DEFECT: ValueEditor::IsLinked and ValueEditor::LinkSourceNode below answer
			// the same two questions by a second route. Two independent answers to 'is
			// this pin linked' in one function is a consistency hazard -- if they ever
			// disagree, the inspector would show a control for a pin it also describes
			// as linked.
			m_Graph.ResolveInput(node->Id, pin.Id, lit, isLinked, linkNode, linkPin);

			ImGui::PushID(pin.Id.c_str());
			if (isLinked)
			{
				ImGui::Text("%s", pin.Id.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("<- %s.%s", linkNode.c_str(),
									linkPin.c_str());
				if (ImGui::SmallButton("Disconnect"))
					DisconnectAt(linkNode, linkPin);
			}
			else if (pin.Type == PinType::Exec)
			{
				ImGui::TextDisabled("%s (exec)", pin.Id.c_str());
			}
			// The editable case: an unlinked data pin.
			//
			// The branch structure here is the schema-driven design in miniature --
			// linked pins show their source, exec pins show a label, and everything else
			// gets a type-appropriate widget with no per-node code.
			else
			{
			// Schema-driven: the control for every pin comes from the
			// pin's declared type, not from the node's type. A node added
			// to the registry is therefore editable with the right
			// widgets the day it is registered, with no editing code
			// written for it.
			// DEFECT, and the most consequential in this file: this inner loop REDECLARES
			// `pin`, shadowing the outer loop's variable of the same name, and iterates
			// ALL pins rather than handling the current one.
			//
			// So for a node with N unlinked data pins, the block below runs N times and
			// draws N*N widgets -- every pin's editor once per unlinked pin. Worse, the
			// shadowed inner `pin` is what every reference below uses, so the outer
			// loop's progress is discarded entirely and the same set of editors is drawn
			// repeatedly. Only one of those duplicates is interactive; the rest are
			// stacked on top of each other.
			//
			// The comment above claims schema-driven editing, and the mechanism is
			// right, but the loop as written cannot be doing what the surrounding code
			// assumes. This looks like a loop that should not be there at all -- the
			// outer `pin` is already the one to edit.
			for (const PinDef &pin : def->Pins)
			{
				if (pin.Dir != PinDir::In || pin.Type == PinType::Exec)
					continue;

				// A linked input's value is its producer's output. Offering
				// a control that silently does nothing is worse than saying
				// where the value comes from.
				// A SECOND linked check, inside the shadowing loop, after ResolveInput
				// already established isLinked for this exact pin at the top of the outer
				// iteration.
				//
				// Redundant even setting the shadowing aside.
				if (ValueEditor::IsLinked(m_Graph, node->Id, pin.Id))
				{
					const std::string source =
						ValueEditor::LinkSourceNode(m_Graph, node->Id, pin.Id);
					ImGui::Text("%s", pin.Id.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("<- %s", source.c_str());
					continue;
				}

				const PinLiteral *lit = node->FindLiteral(pin.Id);
				const std::string current =
					(lit != nullptr && lit->IsSet) ? lit->Value
														   // Falls back to the pin's declared default when no literal is stored, so a
														   // pin that has never been edited shows what the registry says it should.
														   : pin.DefaultLiteral;

				// TYPES, not raw text, drive the widget. Committed is set
				// only when the parsed value differs from what is stored,
				// so an invalid or half-typed entry never overwrites a good
				// value with garbage.
				std::string scratchOwner;
				std::string scratchKey;
				// What the buffer starts as: the stored value, or the type's default text.
				//
				// Seeded rather than left blank so a pin with a declared default shows that
				// default instead of an empty field the author has to fill.
				const std::string seed =
					current.empty() ? DefaultLiteralText(pin.Type) : current;
				// scratchOwner/scratchKey are declared fresh for every pin on every frame,
				// so the buffer map has to re-key each time -- which is the documented
				// cost of the storage mechanism, paid per widget per frame.
				char *buf = ValueEditor::Buffer(scratchOwner, scratchKey,
										 node->Id, pin.Id, seed);
				bool committed = false;
				std::string nextValue;

				ImGui::PushID(pin.Id.c_str());
				ImGui::Text("%s", pin.Id.c_str());
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-1.0f);

				// The widget selection, driven entirely by the pin's type. Adding a node
				// type to the registry needs no new case here; only a new PIN type would
				// fall to default.
				switch (pin.Type)
				{
				case PinType::Bool:
				{
					// A checkbox. A free-text bool would accept "true",
					// "1", "yes" and "TRUE"; constraining the control
					// means the stored text can only ever be what the
					// emitter emits.
					bool value = false;
					// Parse into a local, then let the widget write it back.
					//
					// The parse happens on every frame, so the buffer is the source of truth and
					// the typed value is derived. That is what lets a half-typed entry survive
					// instead of snapping to a default.
					ValueEditor::ParseBool(buf, value);
					if (ImGui::Checkbox("##val", &value))
					{
						nextValue = ValueEditor::FormatBool(value);
						committed = true;
					}
					break;
				}
				case PinType::Int:
				{
					int value = 0;
					ValueEditor::ParseInt(buf, value);
					// Drag-and-edit: dragging gives the fast path, typing
					// gives the exact one.
					// Drag for the fast path, typing for the exact one. A free-text int field
					// would be worse for a value that is usually nudged.
					if (ImGui::DragInt("##val", &value, 1.0f))
					{
						nextValue = ValueEditor::FormatInt(value);
						// Writes the formatted value BACK into the buffer, so the text the author
						// sees matches the text that was stored.
						//
						// Without this the buffer would keep a stale string while the stored literal
						// moved on, and the next frame's parse would fight the drag.
						//
						// DEFECT: kLiteralBufferSize is this panel's own constant (256), duplicated
						// from ValueEditor::kBufferSize, which is what actually sized the buffer.
						// The two must agree or this writes past the allocation. Nothing ties them
						// together, and the one used here is the copy that can drift.
						std::snprintf(buf, kLiteralBufferSize, "%s", nextValue.c_str());
						committed = true;
					}
					break;
				}
				case PinType::Float:
				{
					float value = 0.0f;
					ValueEditor::ParseFloat(buf, value);
					// Same drag pattern for floats, with unbounded min/max and a %.3f display
					// format.
					//
					// DEFECT: FLT_MAX is used but <cfloat> is not included here -- it must be
					// arriving transitively through wlpch.h. A header change elsewhere could
					// break this build.
					if (ImGui::DragFloat("##val", &value, 0.01f, -FLT_MAX, FLT_MAX,
											"%.3f"))
					{
						nextValue = ValueEditor::FormatFloat(value);
						std::snprintf(buf, kLiteralBufferSize, "%s", nextValue.c_str());
						committed = true;
					}
					break;
				}
				case PinType::Vec2:
				case PinType::Vec3:
				case PinType::Vec4:
				{
					// Vectors are ONE field, not N. A separate X/Y/Z box
					// each would need to survive a half-entered number and
					// would make a vec3 three widgets wide in a narrow
					// inspector.
					// Component count from the type. The nested ternary is correct but hard to
					// scan; a switch would be clearer.
					const int count = pin.Type == PinType::Vec2   ? 2
									   : pin.Type == PinType::Vec3 ? 3
																	   : 4;
					if (ImGui::InputText("##val", buf, kLiteralBufferSize))
					{
						float parsed[4] = {0, 0, 0, 0};
						// Only commits if the text parses COMPLETELY, which is what stops a
						// half-entered '1, 2,' from replacing a good stored value with a truncated
						// one.
						if (ValueEditor::ParseVec(buf, parsed, count))
						{
							nextValue = ValueEditor::FormatVec(parsed, count);
							std::snprintf(buf, kLiteralBufferSize, "%s", nextValue.c_str());
							committed = true;
						}
						// Invalid text is left in place for the author to
						// finish; the stored value is untouched.
					}
					break;
				}
				// String, Entity and Any fall here as free text.
				//
				// DEFECT: Entity is treated as untyped text even though DefaultLiteralText
				// gives it a 'player' default, and EntityEditor has no name-based widget.
				// An entity pin is a reference to a named object, so a free-text field
				// means a typo produces a runtime failure in the generated Python rather
				// than a validation error at author time.
				default:
				{
					// String, Entity and Any are free text. Entity names
					// are stored raw and quoted by the emitter, so this
					// is where the escaping boundary lives.
					if (ImGui::InputText("##val", buf, kLiteralBufferSize))
					{
						nextValue = buf;
						committed = true;
					}
					break;
				}
				}
				ImGui::PopID();

				// The single commit point for every pin type: only on a real change, so
				// holding a key does not flood the history.
				//
				// One commit path for five widget cases is what keeps the dirty flag and the
				// validation refresh from being forgotten in one of them.
				if (committed && nextValue != current)
				{
					BeginEdit();
					node->SetLiteral(pin.Id, nextValue, true);
					CommitEdit();
				}
				// Type shown dimmed, because the widget already conveys it
				// for bool/int/float and the text ones need spelling out.
				if (pin.Type == PinType::String || pin.Type == PinType::Any ||
					pin.Type == PinType::Entity)
					ImGui::TextDisabled("%s", ValueEditor::TypeName(pin.Type));
			}
			}
			ImGui::PopID();
		}

		ImGui::Separator();
		// At the bottom of the inspector, where it cannot be hit by accident.
		//
		// No confirmation -- Ctrl+Z covers it, and the node's wires go with it.
		if (ImGui::Button("Delete Node"))
			DeleteNode(node->Id);
	}

	// The validation panel: counts, then the findings themselves.
	void VisualBlocksPanel::DrawFindings()
	{
		ImGui::Separator();
		ImGui::TextUnformatted("Validation");

		// Read once, because ErrorCount() walks the findings vector -- calling it
		// in the printf below would be a second walk per frame.
		const size_t errors = m_Validation.ErrorCount();
		const size_t warnings = m_Validation.WarningCount();

		// The all-clear. Green rather than silence, so the panel does not look
		// broken when there is nothing to report.
		if (errors == 0 && warnings == 0)
		{
			ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "No problems found.");
			return;
		}
		if (errors > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%d error(s)", static_cast<int>(errors));
		if (warnings > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%d warning(s)",
							   static_cast<int>(warnings));

		// A fixed-height scrolling region. 120px shows a few findings and scrolls
		// for the rest, rather than the panel growing without bound.
		if (ImGui::BeginChild("vb_findings", ImVec2(0.0f, 120.0f), true))
		{
			for (const GraphError &f : m_Validation.Findings)
			{
				// Severity is GraphError::IsError -- the same struct the loader
				// uses, with the flag flipped for warnings.
				const ImVec4 colour = f.IsError
										  ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
										  : ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
				ImGui::TextColored(colour, "%s", f.Message.c_str());
			}
		}
		// Outside the if, deliberately -- BeginChild must always be paired, whether
		// or not its contents were submitted. The reverse of the usual guard.
		ImGui::EndChild();
	}

	// The generated-Python viewer.
	void VisualBlocksPanel::DrawGenerated()
	{
		ImGui::Separator();
		ImGui::TextUnformatted("Generated Python");
		if (m_Generated.empty())
		{
			ImGui::TextDisabled("Nothing compiled yet.");
			return;
		}
		// Read-only on purpose. The generated file is the runtime artefact and
		// an edit here would be overwritten by the next compile, so the panel
		// shows it rather than pretending to own it.
		// Taller than the findings panel, because compiled output is longer and is
		// the thing an author actually reads when a graph misbehaves.
		if (ImGui::BeginChild("vb_generated", ImVec2(0.0f, 220.0f), true))
		{
			ImGui::TextUnformatted(m_Generated.c_str());
		}
		ImGui::EndChild();
	}

	// The per-frame entry point: visibility, one-time sizing, and the two-column
	// layout.
	void VisualBlocksPanel::OnImGuiRender()
	{
		// Hidden panels draw nothing at all -- not even the window, so the panel
		// does not appear as an empty titled window in the docking system.
		if (!m_Visible)
			return;

		// A first-frame default size.
		//
		// Deliberately NOT a DockBuilder. Adding a panel to an existing
		// dockspace by rebuilding the node tree destroys every OTHER panel's
		// layout, and doing it per-frame re-does it continuously -- the whole
		// editor rearranges under the user. An earlier attempt here did
		// exactly that.
		//
		// Sizing once is enough: ImGui then either docks this window with the
		// rest or floats it, and the author drags it wherever they want. That
		// is the normal way a panel joins an editor and it cannot break anyone
		// else's layout.
		// A one-shot default size. ImGuiCond_FirstUseEver means the author's own
		// resize is remembered afterwards and never overridden.
		//
		// The comment above is emphatic that this is deliberately not a DockBuilder,
		// and the reasoning is worth keeping: rebuilding the dockspace node tree
		// destroys every other panel's layout.
		if (!m_SizedOnce)
		{
			ImGui::SetNextWindowSize(ImVec2(900.0f, 520.0f), ImGuiCond_FirstUseEver);
			m_SizedOnce = true;
		}

		// Its own ImGui window, like every other panel.
		//
		// Without Begin/End, ImGui submits these widgets to whatever window
		// happens to be current -- here that is the content browser -- so the
		// panel did not appear missing so much as appearing SOMEWHERE ELSE,
		// glued to the bottom of an unrelated window with no title of its own.
		// Its own window, as every panel has. Without this the widgets would attach
		// to whatever window happened to be current -- the failure the comment above
		// describes as the panel appearing somewhere else entirely.
		ImGui::Begin("Visual Blocks");

		DrawToolbar();
		DrawPalette();

		// Canvas and inspector SIDE BY SIDE.
		//
		// Stacked in one window, DrawCanvas() consumed the whole content
		// region and everything after it -- inspector, findings, generated
		// source -- was laid out below the visible area. The inspector was
		// not broken, it was off-screen.
		//
		// A table gives each column a guaranteed share: dragging the
		// splitter then resizes the inspector without the canvas ever
		// starving it.
		// Canvas and inspector side by side.
		//
		// The table guarantees each column a share, which is what stops the canvas
		// from consuming the whole region and pushing the inspector off-screen --
		// the bug the comment above records.
		if (ImGui::BeginTable("vb_split", 2, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Canvas", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Inspector",
								   ImGuiTableColumnFlags_WidthFixed,
								   320.0f);

			ImGui::TableNextColumn();
			DrawCanvas();

			ImGui::TableNextColumn();
			// 0x0 means 'fill the column', so the three right-hand sections stack in
			// whatever height the table gives them.
			ImGui::BeginChild("vb_right", ImVec2(0.0f, 0.0f), true);
			DrawInspector();
			DrawFindings();
			DrawGenerated();
			ImGui::EndChild();

			ImGui::EndTable();
		}
		// Closes the window opened at line 923. The Begin/End count is what ImGui
		// asserts on in debug builds, so an unbalanced pair here would be caught.
		ImGui::End();
	}

} // namespace Wasteland