// The editor-side half of the NB.6 authoring panel. NPCBehaviorPanel.cpp is the
// implementation; this header is the contract EditorLayer and any AI tool call
// program against.
#pragma once

// For the path and the status strings. Passed by const reference and by
// out-parameter throughout rather than by value -- see CompileAndWrite below for
// the one non-const exception.
#include <string>
#include <vector>

// The full Graph definition, because m_Graph is a MEMBER by value. A member of
// incomplete type is not allowed in C++, so this cannot be forward-declared.
//
// That is the cost of holding the graph in the panel rather than through a
// pointer, and it buys value semantics: no ownership flag, no leak, and the
// graph is gone when the panel is.
#include "Wasteland/VisualBlocks/Graph.h"
// For ValidationResult and GraphError -- also members by value, and for the same
// reason.
#include "Wasteland/VisualBlocks/Validator.h"
// Entity is a member too, and the panel uses it for context: which NPC this tree
// belongs to. See m_Entity below for how little that value is actually used.
#include "Wasteland/Scene/Entity.h"

namespace Wasteland
{

	// The forward declaration, and the comment above it states the intent: keep the
	// signatures honest about not needing the full registry or emitter surface.
	// Referenced in signatures below; the real definitions come from the
	// Visual Blocks headers above, but a forward declaration keeps the header
	// honest about not needing the full registry/emitter surface.
	// Note this RE-OPENS the VisualBlocks namespace rather than creating a new one --
	// the declaration below is added to the namespace Graph.h and Validator.h
	// already opened.
	//
	// Redeclaring struct GraphNode when Graph.h has fully defined it is legal: a
	// redeclaration is fine as long as it does not introduce a different type. It
	// would be an error if this second one said `class` instead of `struct`.
	namespace VisualBlocks
	{
		// The only forward-declared type.
		//
		// DEFECT (redundant today): nothing needs it that is not already complete.
		// DrawNodeBody and DrawNodeInputs take GraphNode& -- a reference would be fine
		// against a forward declaration -- but Graph.h is included at line 6 regardless,
		// so the declaration buys nothing. It does document that the .cpp's mutation of a
		// node goes through exactly those two functions, which is the panel's whole
		// editing surface.
		struct GraphNode;
	}

	// The panel's rationale, and the two decisions worth holding before reading any
	// member:
	//
	//   TREE LIST, not a node canvas -- because a tree's structure IS its priority
	//   order, which a freeform canvas cannot show. The header calls the alternative
	//   "spaghetti" and notes it would read worse than the indented Python list it
	//   replaces. That is also why no third-party node editor is needed.
	//
	//   IT NEVER RUNS ANYTHING. No tick, no dispatch. The stated reason: an editor
	//   that starts ticking a tree as a side effect of opening a panel makes every
	//   authoring action a gameplay event. Attaching a compiled tree to an entity is
	//   a scene concern, deliberately not the panel's.
	// The behaviour-tree authoring panel (NB.6).
	//
	// A TREE LIST, not a node canvas. A tree's structure is its priority order,
	// and a freeform canvas cannot show nesting or ordering at a glance -- it
	// turns an eight-node tree into spaghetti and reads worse than the indented
	// list of the Python it replaces. So this panel is an indented, draggable
	// tree list with a palette, and it needs no third-party node editor.
	//
	// What it deliberately does NOT do: run anything. Compiling produces the
	// Python, saving writes the `.wlbtree`, and attaching it to an entity is a
	// scene concern. The panel never dispatches a runtime script on its own,
	// because an editor that starts ticking a tree as a side effect of opening
	// a panel makes every authoring action a gameplay event.
	// No virtual destructor and no base class: the panel is owned by value by the
	// editor layer and never deleted through a base pointer, so none is needed.
	//
	// The cost is that it cannot be substituted or mocked -- acceptable for a
	// concrete ImGui panel, where there is no virtual dispatch to intercept anyway.
	class NPCBehaviorPanel
	{
	public:
		// `= default`, so m_Graph is default-constructed -- an EMPTY graph, not a loaded
		// one.
		//
		// That is safe because the panel starts with m_HasGraph false, so the empty graph
		// is never rendered: OnImGuiRender takes the no-graph branch and shows only the
		// path field and the Load button.
		NPCBehaviorPanel() = default;

		// Records which NPC this tree is being authored for, passed BY VALUE -- cheap
		// because Entity is an entt handle plus a Scene pointer, not component data.
		//
		// DEFECT (write-only field): nothing in the .cpp reads m_Entity. The member and
		// this setter are currently dead weight, and the natural feature they were built
		// for -- showing which NPC owns this tree -- is not implemented.
		void SetEntity(Entity entity) { m_Entity = entity; }
		// The one method the editor layer must call, once per frame.
		//
		// Not a virtual override and not derived from a Layer: the panel is a sub-window
		// inside EditorLayer's own ImGui pass, not a layer of its own. That is why there
		// is no PushLayer/PopLayer pair and no update/draw split.
		void OnImGuiRender();

		// Opens a `.wlbtree` from anywhere -- the content browser, a recent
		// list, or an AI tool call. Public so callers do not have to reach
		// for the toolbar's path field, and so opening a tree is one call
		// rather than "set the buffer, then press Load".
		//
		// The failure contract, and it is deliberate: a failed open leaves the
		// PREVIOUSLY OPEN tree untouched and records the reason.
		//
		// The motivating scenario: a designer double-clicks a broken tree in the content
		// browser and should still have their own work on screen, with an error beside
		// it. A load that cleared the graph first would leave them with an empty panel
		// and a dialog -- the worst outcome for an accidental click.
		// Makes the panel visible on success. A load that fails leaves the
		// previously open tree untouched and records the reason: a designer
		// who double-clicks a broken tree should still have their own work
		// on screen, with an error beside it.
		// Public load entry point, and the header explains why it exists at all rather
		// than being a convenience: callers should not have to reach for the toolbar's
		// path field, and opening becomes ONE call instead of "set the buffer, then
		// press Load".
		//
		// That matters because the buffer is private -- an external caller literally
		// cannot drive the empty-state UI without this method.
		bool OpenTree(const std::string &path);

		// Compile the open tree and write the generated Python next to it.
		// Returns false with a reason in `outStatus` on any failure.
		//
		// The write is what makes a tree RUN. Compiling without writing only
		// fills the preview pane, so the default action for a finished tree
		// has to do both or the author will believe it is live.
		// The action that makes a tree RUN, and the header is explicit that the write is
		// the part that matters: compiling without writing only fills the preview pane.
		//
		// Hence the note that the default action for a finished tree has to do both, or
		// the author will believe it is live -- which is why the .cpp offers both a
		// Compile and a Compile && Write button.
		//
		// `outStatus` is a non-const REFERENCE, so the caller must supply an lvalue and
		// receives either a success description or a failure reason.
		bool CompileAndWrite(std::string &outStatus);

		// IsDirty's documentation, explaining the flag's whole reason for existing: the
		// panel surfaces staleness so the author finds out HERE rather than as an NPC
		// silently ignoring its behaviour.
		//
		// Note the wording is narrower than the flag's actual semantics -- see m_Dirty
		// below, where a newly created tree is also dirty despite never having been
		// compiled.
		// True when there is a tree open and its file has been compiled to
		// Python since the last edit. The panel surfaces this so a stale
		// tree is visible rather than discovered as an NPC ignoring its
		// behaviour.
		// A public read of the dirty flag, so the editor layer -- or a toolbar outside
		// the panel -- can show staleness without the panel drawing its own warning.
		//
		// Duplicated with the in-panel "!" warning at the top of the toolbar, so one
		// flag can drive UI in two places.
		bool IsDirty() const { return m_Dirty; }

		// Project-relative path of the open tree, or empty.
		// Returns a reference to the member, not a copy -- the standard choice for a
		// getter over a string member that outlives the call.
		//
		// Project-relative, so it is safe to show in the UI and to hand to a file
		// dialog; empty when nothing is open. Callers must not cache it across a load of
		// a different tree, since it aliases a member.
		const std::string &GetPath() const { return m_Path; }

		// Visibility is owned by the editor, not the user: the .cpp passes no close
		// pointer to ImGui::Begin, so the X that would dismiss the panel does not
		// exist. The panel can be hidden but never closed.
		bool IsVisible() const { return m_Visible; }
		// Inline setter matching the inline getter, so visibility is readable from
		// outside without a round-trip through the editor layer.
		//
		// Defaults to TRUE -- the panel opens with the editor rather than needing to be
		// enabled somewhere.
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		// A section banner in the private block, worth reading as a claim about
		// coupling: these four are the panel's entire persistence surface.
		//
		// DEFECT (unreachable): NewTree() is declared and defined in the .cpp but is
		// called from NOTHING there -- there is no New Tree button in the toolbar. So a
		// user has no way to start a blank tree from this panel, which is also why the
		// 512-byte m_PathBuf below is reachable only from the no-graph state.
		// ---- file actions ----
		// Called once per frame from OnImGuiRender, and only when a graph is loaded.
		//
		// Private, unlike OnImGuiRender -- the editor layer has no business invoking an
		// individual region of the panel.
		void DrawToolbar();
		bool LoadTree(const std::string &path);
		void SaveTree();
		// Resets every field to a blank tree, including clearing m_Path.
		//
		// DEFECT (dead code): no caller. See the section banner above. Its one side
		// effect that matters, clearing m_Path, means a new tree correctly refuses to
		// save until a path is set -- which is also unreachable, since the path field
		// renders only in the no-graph state a new tree leaves behind.
		void NewTree();

		// ---- the tree itself ----
		void DrawTreeList();
		// The tree walker, and both parameters are load-bearing.
		//
		// `nodeId` rather than a GraphNode&, because the recursion must survive the
		// container being edited: a node is re-looked-up by id at each level, so a
		// reference into m_Graph.Nodes cannot be held across a mutation.
		//
		// `depth` drives the ImGui::Indent(depth * 2) that draws the hierarchy, so the
		// nesting in the data and the nesting on screen cannot disagree.
		void DrawNodeRecursive(const std::string &nodeId, int depth);

		// DEFECT (inaccurate on two of three counts): the .cpp's DrawNodeRecursive
		// context menu contains ONLY "Delete". There is no add-child and no rename menu
		// item -- Add child is a SmallButton in the node body instead, and renaming does
		// not exist at all.
		//
		// So this header documents an intent that was partially built, and a reader
		// looking for the rename path will not find one. Not fixed here; comments only.
		// Node rows carry a context menu (add child / delete / rename), a
		// latent checkbox, and the literal inputs for their type's pins.
		// Takes a MUTABLE reference, so this is one of the two places the panel writes
		// to the graph directly.
		//
		// Mutating through the reference rather than by id re-lookup saves a search per
		// edit. Safe because the caller re-resolves the node immediately before the
		// call and performs no container operation during it.
		void DrawNodeBody(VisualBlocks::GraphNode &node, int depth);
		// The literal editors -- one widget per input pin.
		//
		// DEFECT (inconsistent dirty flag, and this one has real consequences): the
		// Bool, Float and String branches all set m_Dirty = true, but the Int branch
		// does not. So editing an int pin does not mark the tree stale, no warning
		// appears, and the edit can go entirely unnoticed.
		void DrawNodeInputs(VisualBlocks::GraphNode &node);

		// The node-type chooser, a separate floating window rather than an inline region.
		//
		// Gated by m_ShowPalette below, which doubles as the pointer ImGui's close
		// button writes -- one flag serving as both the gate and the close handler.
		void DrawPalette();
		void DrawFindings();
		void DrawBlackboard();
		void DrawBehaviorTrace();
		// The Python preview pane.
		//
		// DEFECT (dead method): declared here and NOT defined in the .cpp. The preview
		// is drawn inline inside OnImGuiRender under a TreeNode("Generated Python")
		// instead. Harmless in practice -- an undefined private method that is never
		// called produces no linker error -- but it is a stale declaration left behind by
		// that inline refactor.
		void DrawGenerated();

		// Add `typeId` under `parentId` (empty = root), giving it a unique id.
		// Both parameters by const reference; the .cpp seeds a new node from the type id
		// and links it under the parent.
		//
		// An empty parentId means ROOT, which the header documents and the .cpp tests
		// with `if (!parentId.empty())`. So "no parent" is encoded as an empty string
		// rather than a pointer or an optional -- a sentinel, but an unambiguous one.
		//
		// Uniqueness of the generated id is the .cpp's job: it starts from the node
		// count and appends underscores until FindNode returns null.
		void AddNode(const std::string &typeId, const std::string &parentId);
		void DeleteNode(const std::string &nodeId);

		// The cycle-protection comment, and its claim does not currently hold: Reparent
		// has NO caller anywhere in the .cpp. The only context-menu action is Delete.
		//
		// So the header documents a guarantee -- "an edit that would produce an INVALID
		// tree is refused at the point of the click, with a reason" -- that the
		// implementation cannot deliver, because the only structural edit the UI offers
		// is deletion.
		// Structural edits go through here so an edit that would produce an
		// INVALID tree is refused at the point of the click, with a reason,
		// rather than leaving the author to discover it as a red error list
		// entry they have to diagnose from scratch.
		// DEAD CODE, fully implemented and unreachable.
		//
		// Reparent and WouldCreateCycle together are the panel's cycle-safety mechanism,
		// and neither is called. The consequence is concrete: a tree can only be
		// restructured by deleting and re-adding, and the check that exists to prevent a
		// cycle can never fire from the UI.
		//
		// `outReason` is a non-const reference, so a caller must supply a string to read
		// the refusal message from.
		bool Reparent(const std::string &nodeId, const std::string &newParentId,
					   std::string &outReason);
		// The predicate Reparent would consult, and its only caller is Reparent.
		//
		// `const`, matching the rest of the accessors -- it only reads the graph.
		//
		// Implemented by collecting the node's whole subtree and asking whether the
		// proposed parent is inside it, which is the right question and inherits
		// CollectSubtree's unbounded recursion on an already-cyclic graph.
		bool WouldCreateCycle(const std::string &nodeId,
							  const std::string &newParentId) const;

		// The state section. Its ordering roughly follows data lifetime: editor context
		// first, then the loaded graph, then the derived views, then the transient UI
		// flags.
		// ---- state ----
		// DEFECT (unused): declared and assigned by SetEntity, read by NOTHING. The .cpp
		// never mentions m_Entity.
		//
		// The obvious intended feature -- showing which NPC this behaviour tree belongs
		// to, so the author does not edit the wrong one -- is not implemented. Entity
		// being a cheap handle makes the unused member harmless, just misleading.
		Entity m_Entity;
		// True by default, so the panel opens with the editor rather than needing to be
		// enabled somewhere. SetVisible can hide it; nothing can permanently close it.
		bool m_Visible = true;

		// The open tree's project-relative path, or empty when nothing is loaded.
		//
		// DEFECT (a second source of truth): the toolbar's path field is a SEPARATE
		// m_PathBuf below, which is what the empty-state Load button actually reads.
		// The two agree only because LoadTree copies one into the other on success --
		// so what a designer types and what GetPath() reports are distinct strings held
		// in step by a single assignment.
		std::string m_Path;
		// Distinguishes "no tree" from "an empty tree".
		//
		// Needed because Graph is a value type that is always constructed: without this
		// flag the panel could not tell a fresh default-constructed graph from a loaded
		// tree that happens to have no nodes, and would render a toolbar for nothing.
		//
		// Checked in two places in the .cpp -- the no-graph branch of OnImGuiRender and
		// CompileAndWrite -- which is slightly redundant, since DrawToolbar only runs
		// after the first.
		bool m_HasGraph = false;
		// By VALUE, not a pointer. The panel OWNS the working copy of the tree being
		// edited; the .wlbtree on disk is a different thing, synchronised only by an
		// explicit Save.
		//
		// That is exactly why m_Dirty exists: two copies with no automatic
		// synchronisation is the condition a dirty flag is for.
		VisualBlocks::Graph m_Graph;
		// The last failure, retained so the panel can display it beside whatever is on
		// screen.
		//
		// DEFECT (never fully cleared): LoadTree and CompileAndWrite's success path reset
		// it, but Compile, SaveTree and NewTree do not. So an error from one action can
		// still be on screen after an unrelated later action succeeded. It is a record of
		// the last failure rather than live status, and the .cpp's own comment concedes
		// it reads as more current than it is.
		VisualBlocks::GraphError m_LastError;

		// DEFECT (not quite true): "every structural edit" is aspirational. The Int
		// branch of DrawNodeInputs sets a pin without setting m_Dirty, so not every edit
		// marks the tree stale.
		//
		// Also cleared only by CompileAndWrite -- the preview-only Compile button
		// deliberately leaves it set, which is correct, since nothing was written.
		// Set by every structural edit, cleared by a successful compile.
		//
		// The distinction that matters: "this tree has never been compiled"
		// and "this tree was compiled and then edited" both mean the file on
		// disk is stale, but only the second one silently disagrees with what
		// the author can see. A dirty flag is the only way the panel can warn
		// before an NPC runs last week's behaviour.
		// The staleness flag. The header's explanation is the important part:
		//
		//   "never been compiled" and "compiled then edited" BOTH mean the file on
		//   disk is stale, but only the second silently disagrees with what the author
		//   can see.
		//
		// So a dirty flag is the only way the panel can warn before an NPC runs last
		// week's behaviour. NewTree sets it true -- a new tree has never been compiled,
		// which the header's wording does not quite cover -- and a loaded tree starts
		// false, since nothing has been edited since the file was written.
		bool m_Dirty = false;

		// Cached validation output, refreshed only by an explicit Validate button, a
		// Save, a Load or a Compile -- never on every edit.
		//
		// That is the right trade (validation walks the whole graph), and the cost is
		// that the findings pane can be stale relative to the tree above it. Note the
		// .cpp assigns to this member in some paths and to a LOCAL ValidationResult in
		// others, so a successful SaveTree does not refresh the pane.
		VisualBlocks::ValidationResult m_Validation;
		// The generated Python, for the preview pane.
		//
		// Deliberately CLEARED when a compile fails rather than left stale: showing the
		// last good preview next to a tree that no longer compiles would be actively
		// misleading. Hence the clears in the failure branch of both compile paths and
		// on a successful LoadTree.
		std::string m_Generated;
		// A short status for the toolbar: "loaded", "validated", "compiled", or
		// "compiled -> <file>.py".
		//
		// Purely cosmetic, and NOT a reliable indicator of what is on disk -- the
		// Validate button overwrites it with "validated" without recompiling. m_Dirty
		// is the reliable one, which is why IsDirty() exists as a separate query.
		std::string m_GeneratedStatus;

		// The armed-placement state; the inline comment says the whole thing.
		//
		// A NON-EMPTY string rather than a separate bool, because the pending TYPE is
		// needed: the .cpp calls IsCompositeOrDecorator(m_PendingPaletteType) to decide
		// whether to offer drop targets in the tree. A bool would lose that.
		//
		// Empty means not armed, which is why `if (!m_PendingPaletteType.empty())` is the
		// placement-mode test throughout the .cpp.
		std::string m_PendingPaletteType;   // type awaiting a parent click
		// DEFECT (dead field): declared, never read and never written. There is no dialog
		// and no New Tree button; NewTree() itself is unreachable.
		//
		// Presumintended for a name or path prompt when creating a tree. Left in place
		// because this pass is comments-only.
		std::string m_NewTreeDialog;
		// A fixed 512-byte C string rather than a std::string, because ImGui's InputText
		// takes (char*, size) and writes through it directly.
		//
		// Zero-initialised, so the empty-state field starts empty rather than
		// uninitialised. sizeof(m_PathBuf) is what the .cpp passes as the buffer size.
		//
		// DEFECT (a second copy of the path): separate from m_Path above. Rendered only
		// in the no-graph state, and the Load button passes it to LoadTree, which copies
		// it into m_Path. Once a tree is open the field is not shown and this buffer
		// holds whatever was last typed -- which need not be the loaded path.
		char m_PathBuf[512] = {};
		// The palette's visibility, and the flag does double duty: it is the gate in
		// OnImGuiRender AND the pointer passed to ImGui::Begin("Node Palette",
		// &m_ShowPalette), so ImGui's close button writes it directly.
		//
		// That is also why the toolbar's Add Node button TOGGLES this rather than
		// setting it true -- a second click closes the palette, which reads as the button
		// doing nothing.
		bool m_ShowPalette = false;
	// The end of the class, with no trailing comment.
	//
	// Everything here is either a UI region, a file action, or the state those two
	// share: no interface abstraction, no separate view-model struct, and no undo
	// stack. The missing undo is the most consequential gap, given that Delete
	// removes a whole subtree irrecoverably.
	};

}