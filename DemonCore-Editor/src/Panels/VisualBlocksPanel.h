#pragma once

// For the path and status strings held by value below.
#include <string>
// For m_LiteralBuffers, keyed by pin identity.
#include <unordered_map>
// For the char buffers stored inside m_LiteralBuffers.
#include <vector>

// The undo stack, held BY VALUE as a member. Every edit in this panel
// brackets itself with BeginEdit/CommitEdit rather than each mutation
// knowing about history.
#include "GraphHistory.h"
// The canvas widget, also held by value. It owns the drawing and interaction
// but no file or compile logic -- the separation is what makes the canvas
// reusable by a future visual-blocks surface.
#include "NodeCanvas.h"

// The .wlblocks document model: the graph, its nodes, and GraphError.
#include "Wasteland/VisualBlocks/Graph.h"
// The node taxonomy, used by the palette and by node creation.
#include "Wasteland/VisualBlocks/NodeRegistry.h"
// Validation, so the panel can show findings without reimplementing any of
// the rules.
#include "Wasteland/VisualBlocks/Validator.h"

namespace Wasteland
{

	// The general visual-scripting panel for `.wlblocks` graphs.
	//
	// A CANVAS, deliberately not a tree list. A graph's structure is wiring and
	// flow, and a tree list cannot show that a value feeds a branch condition
	// three nodes downstream -- which is exactly what a general script is
	// mostly about. The behaviour panel keeps the tree list because a behaviour
	// tree's structure IS priority order, which is a different thing entirely.
	// Two panels, two metaphors, one shared runtime.
	//
	// The canvas itself lives in NodeCanvas and owns no file or compile logic,
	// so this panel is the part that knows about `.wlblocks` and the generated
	// Python; the canvas is reusable by any future visual-blocks surface.
	//
	// Like the behaviour panel, it never RUNS anything. Compiling writes the
	// Python and attaching it to an entity is a scene concern; an editor that
	// started ticking a graph as a side effect of opening a panel would make
	// every authoring action a gameplay event.
	// The panel for .wlblocks graphs. The class comment above is the design
	// rationale -- canvas rather than tree, never runs anything, and owns file
	// and compile concerns the canvas does not.
	class VisualBlocksPanel
	{
	public:
		// Defaulted despite every member having an initialiser of its own, so the
		// constructor body is empty.
		//
		// DEFECT, minor: m_History is default-constructed rather than cleared. That
		// is correct for a fresh panel, but the header's own note on GraphHistory::Clear
		// says undo must not reach across a file swap -- so this panel has to call
		// Clear() whenever it loads a graph. The defaulted constructor means nothing
		// enforces that at construction time.
		VisualBlocksPanel() = default;

		// Draws the panel when visible. Called every frame by the editor layer.
		//
		// The only entry point that draws; everything else it calls is either a
		// Draw* section or an edit helper.
		void OnImGuiRender();

		// Opens a `.wlblocks` from anywhere -- the content browser, a recent
		// list, or a tool call. Makes the panel visible on success; a failed
		// load leaves the open graph untouched and records the reason, so a
		// designer who opens a broken graph keeps their own work on screen.
		// Opens a .wlblocks from anywhere -- browser, recent list, or a tool call.
		//
		// @param path  Graph file to open.
		// @return       True on success.
		//
		// Making the panel visible is a SIDE EFFECT of success, per the comment
		// above. A failed load deliberately leaves the current graph untouched and
		// records the reason, so a designer who opens a broken graph keeps their own
		// work on screen rather than losing it to an error dialog.
		bool OpenGraph(const std::string &path);

		// Compile and write the generated Python next to the graph. This is
		// what makes it live, so it is the default action for a finished
		// graph rather than a separate opt-in.
		// Compiles the graph and writes the generated Python beside it.
		//
		// @param outStatus  Receives a human-readable result for the UI.
		// @return           True on success.
		//
		// This is the action that makes a graph LIVE, which is why it is the
		// default for a finished graph rather than an opt-in. It writes a real file,
		// so it is the one method here with an effect outside the editor's memory.
		bool CompileAndWrite(std::string &outStatus);

		// Writes the .wlblocks itself. Separate from CompileAndWrite because
		// authoring progress and compiling are different intents -- a designer
		// saving their work is not asking for a Python file.
		bool SaveGraph();

		// Starts an empty graph. Presumably also calls GraphHistory::Clear() -- the
		// header requires it, and nothing else on this class would.
		void NewGraph();

		// True when the open graph has been edited since the last successful
		// compile, so the generated file on disk is stale.
		// True when the graph has been edited since the last successful compile,
		// which is the comment's claim.
		//
		// DEFECT: the comment defines dirty as 'edited since the last successful
		// COMPILE', but a natural implementation clears m_Dirty on Save() too, since
		// a saved graph is not stale on disk. The flag then means two different
		// things depending on which method last touched it, and the editor's title
		// bar and the compile prompt can disagree about whether work is unsaved.
		// Two flags -- m_Dirty and m_NeedsCompile -- would separate them.
		bool IsDirty() const { return m_Dirty; }

		const std::string &GetPath() const { return m_Path; }
		bool IsVisible() const { return m_Visible; }
		// Inline visibility toggle. Note OpenGraph sets this too, so visibility is
		// reachable from two places.
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		// ---- sections ----
		void DrawToolbar();
		void DrawCanvas();
		void DrawPalette();
		// The value a pin shows before it has been authored. Must be
		// one the COMPILER accepts, not just one a widget can render.
		// The source text a pin shows before it has been authored.
		//
		// Returns a const char* rather than std::string -- these are static literals
		// with static storage duration, so no copy and no ownership question.
		//
		// The comment above is the load-bearing part: the value must be one the
		// COMPILER accepts, not merely one a widget can display. That ties this
		// switch directly to the fallbacks in NodeRegistry.cpp's InTyped helpers --
		// two independently written lists of the same per-type literals, which is
		// a duplication worth noting: they can drift, and a mismatch means a pin
		// shows one value and compiles to another.
		const char *DefaultLiteralText(VisualBlocks::PinType type) const;

		// Every structural and value edit brackets itself with these.
		// Snapshotting before the mutation and committing after is the
		// only way undo can be correct for edits made from widgets,
		// which report changes after they have already happened.
		// Snapshots the graph before a mutation; CommitEdit() follows after.
		//
		// Splitting the bracket in two is what makes widget-driven editing
		// undoable: ImGui widgets report that a value CHANGED, after the change has
		// already happened. There is no 'before' hook to hang a snapshot on, so the
		// panel has to take it explicitly beforehand.
		//
		// The contract is that every edit pairs these two. Nothing enforces it -- a
		// mutation that calls only CommitEdit() would push a snapshot of the
		// already-modified graph, and an unpaired BeginEdit() would leave the stack
		// with a duplicate that CollapseIdentical would then have to clean up.
		void BeginEdit();
		// Commits the bracketed edit: collapses a no-op snapshot and marks dirty.
		//
		// Calling CollapseIdentical rather than pushing unconditionally is what
		// makes a keystroke that changed nothing cheap -- see GraphHistory's own
		// comment on that method.
		void CommitEdit();

		// Ctrl+Z / Ctrl+Y, also reachable from the toolbar. Public
		// so the editor's global shortcut can route here.
		// Ctrl+Z, routed through GraphHistory.
		//
		// Public so the editor's global shortcut handler can reach it. Not static
		// and not virtual -- the panel is a concrete editor component, not an
		// interface anything else implements.
		void Undo();
		void Redo();

		// The size of every per-pin literal editing buffer.
		//
		// A named constant rather than a literal at each use, so the buffers
		// allocated in m_LiteralBuffers below and any fixed-size ImGui input bound
		// agree. constexpr static, so it needs no out-of-line definition in C++17.
		static constexpr size_t kLiteralBufferSize = 256;
		void DrawInspector();
		void DrawFindings();
		void DrawGenerated();

		// Literal inputs for the selected node's data pins, drawn inside the
		// node box by the canvas. Returns true when it consumed the click, so
		// the canvas does not also start a drag.
		// Draws the literal inputs for the selected node's data pins, INSIDE the
		// node box.
		//
		// @param node  The node to draw body widgets for.
		// @return       True if the widget consumed the click.
		//
		// The return value exists to stop the canvas also starting a drag on the
		// same click -- without it, editing a literal inside a node would also move
		// the node, which is the classic ImGui input-vs-click conflict.
		bool DrawNodeBody(const VisualBlocks::GraphNode &node);

		// ---- graph edits ----
		//
		// Structural edits route through here so every one marks the graph
		// dirty. A canvas that adds a wire without setting the flag produces a
		// graph that looks edited, runs as if it were not, and gives the
		// author no reason why.
		// Creates a node of the given type from a palette click.
		//
		// @param typeId  Registry type id of the node to create.
		//
		// Routed through here rather than being done inline in DrawPalette so that
		// the edit is bracketed and the dirty flag is set in one place -- the reason
		// the comment above this group exists.
		void AddNodeFromPalette(const std::string &typeId);
		void DeleteNode(const std::string &nodeId);
		// Adds a wire between two pins.
		//
		// @param fromNode,fromPin  Producer node and its output pin.
		// @param toNode,toPin      Consumer node and its input pin.
		void Connect(const std::string &fromNode, const std::string &fromPin,
					 const std::string &toNode, const std::string &toPin);
		void DisconnectAt(const std::string &fromNode, const std::string &fromPin);

		// Does this link already exist? A second wire into one input is
		// ambiguous -- which producer the compiler honours would be an
		// accident of declaration order -- so the UI refuses it rather than
		// letting the validator report it after the fact.
		// Exact-match link existence test, used to stop the UI offering a
		// duplicate wire.
		//
		// @return  True if that exact wire already exists.
		bool HasLink(const std::string &fromNode, const std::string &fromPin,
					 const std::string &toNode, const std::string &toPin) const;
		// Whether an input is already fed.
		//
		// This is the stricter of the two checks, and the comment above explains
		// why it exists: a second wire into one input is ambiguous, so the UI refuses
		// it rather than letting the validator report it afterwards.
		//
		// The asymmetry is worth noticing -- HasLink prevents a duplicate wire, while
		// InputHasLink prevents any second wire at all. Both are needed, and only
		// the second is what makes the compiler's choice of producer deterministic.
		bool InputHasLink(const std::string &toNode, const std::string &toPin) const;

		// A unique node id. Never derived from the type alone -- two `Add`
		// nodes must not collide, or the second silently overwrites the first.
		// Generates an id for a new node.
		//
		// The comment above states the constraint that matters: an id must never be
		// derived from the type alone, because two Add nodes would then collide and
		// the second would silently overwrite the first.
		//
		// Note this returns std::string by value while the field it feeds -- see
		// m_GeneratedCounter below -- suggests a counter-based scheme. A counter is
		// fine within one session but not across sessions: reopening a saved graph
		// and adding a node could reissue an id that the saved graph already uses,
		// unless the counter is seeded from the loaded graph's nodes.
		std::string MakeNodeId(const std::string &typeId);

		// ---- file ----
		// The private worker behind OpenGraph, split out so the public method can
		// handle visibility and error reporting separately.
		bool LoadGraph(const std::string &path);

		// ---- state ----
		// Panel starts visible rather than waiting to be summoned.
		bool m_Visible = true;
		// The open graph's path, returned by const reference from GetPath().
		//
		// DEFECT: returning a const reference to a member is fine, but nothing here
		// prevents a caller from holding that reference across an OpenGraph call,
		// which replaces the string. The same pattern appears on
		// m_GeneratedStatus below. A by-value return would remove the hazard at the
		// cost of a string copy per frame in the UI.
		std::string m_Path;
		// Whether a graph is open at all -- distinct from m_Dirty, which asks
		// whether it has changed. The panel needs both to answer 'should I offer to
		// save' without offering to save nothing.
		bool m_HasGraph = false;
		VisualBlocks::Graph m_Graph;
		// The last load or compile failure, retained so the UI can display it after
		// the fact rather than only in a modal.
		VisualBlocks::GraphError m_LastError;
		// The last validation pass. Cached rather than recomputed per frame --
		// validation walks the whole graph and would be far too expensive to run on
		// every ImGui frame, so it must be refreshed on edit.
		VisualBlocks::ValidationResult m_Validation;

		// See the note on IsDirty() above: this flag's exact meaning depends on
		// which method last cleared it, which is the defect worth resolving.
		bool m_Dirty = false;
		// The undo stack. Cleared on load and on NewGraph, per GraphHistory's own
		// requirement that undo must not reach back across a file swap.
		//
		// Holding it by value is right: an undo stack belongs to the document being
		// edited, not to the panel.
		GraphHistory m_History;

		// Cleared when a graph is loaded or replaced, so a stale buffer
		// from a previously open graph cannot seed the new one's fields.
		// One 256-byte editing buffer per pin, keyed by pin identity.
		//
		// Buffers persist across frames because ImGui's InputText needs stable
		// storage -- a stack-local would be re-initialised every frame and typing
		// would be impossible. Clearing it on load is what the comment above
		// requires: a stale buffer from a previously open graph would otherwise
		// seed the new graph's fields with the old graph's text.
		//
		// DEFECT: these buffers are the user's in-progress typing, and they are NOT
		// part of the graph, so they are not in any history snapshot. An undo after
		// typing a literal reverts the graph but leaves the buffer showing the typed
		// text, which then disagrees with the value it is supposed to represent.
		std::unordered_map<std::string, std::vector<char>> m_LiteralBuffers;
		// The last generated Python, displayed by DrawGenerated so the author can
		// see what their graph compiled to.
		std::string m_Generated;
		// The compile result text. Empty when the last compile succeeded, per the
		// contract in CompileAndWrite's comment -- so a caller must treat empty as
		// success rather than checking a separate flag.
		std::string m_GeneratedStatus;
		// Disambiguates auto-named nodes, as the comment says.
		//
		// DEFECT: a plain int reset only by default construction. It is never reset
		// when a graph is LOADED, so ids keep counting up across graph switches --
		// which prevents collisions rather than causing them, so this is safe. The
		// real gap is the opposite: it starts at 0 for a loaded graph, so a new node
		// can be issued an id that already exists in that graph. Seeding it from the
		// loaded graph's node count or max id would close that.
		int m_GeneratedCounter = 0;   // disambiguates auto-named nodes

		// The canvas, held by value. It owns no file or compile logic, which is what
		// lets this panel be about .wlblocks while the canvas stays reusable.
		NodeCanvas m_Canvas;
		// A fixed buffer for the path input field -- passed to ImGui's InputText,
		// which needs a char* plus a size, so a std::string will not do.
		//
		// Zero-initialised because ImGui treats it as a NUL-terminated string; the
		// {} matters. Note this buffer is separate from m_Path above, so the two
		// can disagree -- m_PathBuf is what the user is typing, m_Path is what is
		// open.
		char m_PathBuf[512] = {};
		// Whether the node palette is showing. Hidden by default to leave room for
		// the canvas.
		bool m_ShowPalette = false;
		// A one-shot flag, so the camera framing runs on the first frame only rather
		// than re-framing the graph every frame and fighting the user's panning.
		bool m_FramedOnce = false;
		// Set once the window has been given its default size.
		// Set once the window has been given its default size. The same one-shot
		// pattern as m_FramedOnce, for a different reason -- the panel must not
		// resize its own window after the user has arranged it.
		bool m_SizedOnce = false;
	};

} // namespace Wasteland