#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Wasteland/VisualBlocks/Graph.h"
#include "Wasteland/VisualBlocks/NodeRegistry.h"

namespace Wasteland
{

	// A local Point rather than ImVec2, and the comment above gives the reason:
	// keeping imgui out of this header means the canvas can be included from a panel
	// that does not already have ImGui. The conversion is paid for somewhere, and
	// this is the cheaper direction to pay it in.
	// A 2D point, deliberately not ImVec2.
	//
	// No editor header includes imgui or names its types: SceneHierarchyPanel.h
	// and NPCBehaviorPanel.h both keep imgui confined to the .cpp. A canvas is
	// the one component most likely to be included from somewhere that does not
	// already have ImGui, so it does not push that dependency up into a header.
	// Default-initialised to zero rather than left indeterminate, so a Point is
	// usable without construction -- which matters because these appear in
	// default-initialised members below (m_Pan, m_DragGrabOffset).
	struct Point
	{
		float X = 0.0f;
		float Y = 0.0f;
	};

	// A struct, not a class: everything is public and there is no invariant to
	// protect. The absence of private members is what lets a panel read m_Zoom
	// directly instead of needing an accessor for it.
	// A reusable node-graph canvas for any Visual Blocks surface.
	//
	// This is a COMPONENT, not a panel. It owns no file, no palette and no
	// compile action -- it draws a `Graph` and reports interaction. That
	// split is what lets `.wlblocks` graphs and any future visual-blocks
	// surface share one implementation instead of each panel growing its own
	// half-working editor. A panel supplies the graph, the callbacks, and
	// decides what an edit means; the canvas supplies geometry, panning,
	// dragging, wires and selection.
	//
	// Written in-repo against ImGui 1.89.3 rather than vendoring
	// imgui-node-editor: the dependency would arrive with its own build-order
	// risk and a pin/wire model that does not match this engine's
	// `PinType`/`PinDir` typed wires. Everything here is replaceable, so a
	// third-party editor can be dropped in later without touching a panel.
	//
	// Coordinates: node position is stored in GRAPH-SPACE pixels and is part
	// of the authored file, so it survives a reopen. The view transform
	// (pan offset + zoom) is view state and deliberately NOT stored -- it is a
	// property of the current window, not of the graph.
	// A STRUCT with no data members -- every field below is view or interaction
	// state. So sizeof(NodeCanvas) is 1 and a panel can hold one by value, which is
	// what a component like this normally cannot afford.
	struct NodeCanvas
	{
		// One node as the canvas draws it. `Pos` is graph space and persists.
		// A per-frame snapshot rather than a reference into the graph. The canvas derives
		// everything it draws -- title, size, pin count -- so it holds a resolution of
		// the graph for one frame and the memo maps below make that cheap.
		struct Visual
		{
			// The id is what the memo maps key on. DEFECT (aliasing): the fields below
			// are COPIES of the node position, so a graph edit made after the snapshot
			// was taken does not show until the next frame. Render invalidating the
			// memo wholesale (see the note on m_Visuals) is what keeps that invisible.
			std::string Id;
			std::string TypeId;
			float X = 0.0f;
			float Y = 0.0f;
			// Width is derived from the title, not stored: a fixed width would
			// either clip long type ids or waste space on short ones.
		};

		// End of Visual. Note what is NOT here: no pin list and no height. Both
		// derive from the registry at draw time, which is what lets a node type
		// gain a pin without any edit to this struct.
		// What a single frame of interaction produced. The panel applies
		// these; the canvas never mutates the graph itself.
		// An output parameter plus flags rather than a return value or a direct graph
		// mutation: the panel applies these, the canvas never edits the graph. So a
		// panel can veto an edit -- the drawNodeBody return value below is exactly that.
		struct Events
		{
			// One flag per interaction KIND, not per action. Note that NodeMoved and
			// NodeSelected are edges rather than levels: true only for the frame a drag
			// ENDS, so a panel that polls them cannot miss an event.
			bool NodeMoved = false;         // a drag ended this frame
			bool NodeSelected = false;      // selection changed
			bool WireAdded = false;        // a pin-to-pin drag completed
			bool NodeDeleted = false;      // a delete request (key / menu)
			bool CanvasClicked = false;     // empty-space click; clear selection
			// Ids as std::string rather than pointers or indices: the graph can reallocate
			// while a drag is live, so a pointer would dangle and an index would shift.
			std::string MovedNodeId;
			std::string SelectedNodeId;
			std::string LinkFromNode;
			std::string LinkFromPin;
			std::string LinkToNode;
			std::string LinkToPin;
		};

		// Draws the canvas over `graph` and fills `outEvents`.
		//
		// `drawNodeBody` lets a panel render a node's own contents (literal
		// inputs, a nested list) inside the node box. Returning true means the
		// panel consumed the click, so the canvas does not start a drag or
		// change selection -- without that, editing a literal inside a node
		// would fight the canvas for the same click.
		// The registry is a required parameter rather than an optional one: framing and
		// pin positions both need each node's real pin count, which is registry data.
		void Render(VisualBlocks::Graph &graph,
					const VisualBlocks::NodeRegistry &registry,
					Events &outEvents,
					// A default-constructed std::function, so a panel that does not need custom
					// bodies can omit the argument.
					//
					// DEFECT (cost): omitting it constructs a std::function on every call -- a
					// small but non-zero per-frame cost. Not fixed; comments only.
					std::function<bool(const VisualBlocks::GraphNode &)> drawNodeBody =
						std::function<bool(const VisualBlocks::GraphNode &)>());

		// Select a node by id (no-op if absent). Used by the panel when a
		// search or an error click should reveal a node.
		void Select(const std::string &nodeId);

		// A const reference to the member rather than a copy: the id is compared, not
		// stored, and copying a string per query in a per-frame path would be wasteful.
		const std::string &GetSelected() const { return m_Selected; }
		bool HasSelection() const { return !m_Selected.empty(); }

		// True once the author has dragged the graph. A canvas that has never
		// been panned opens framing its own content; one that has keeps the
		// author's viewpoint.
		// The one piece of view state that is NOT derivable from the current transform.
		// m_Pan and m_Zoom alone cannot tell "never panned" from "panned back to the
		// origin", and the two want different behaviour when the canvas opens.
		bool HasBeenPanned() const { return m_Panned; }

		// The registry is required, not optional: framing needs each node's
		// real height, which depends on its pin count.
		// FrameAll takes the registry for the same reason Render does: node height
		// depends on pin count, which is registry data rather than graph data.
		void FrameAll(const VisualBlocks::Graph &graph,
					const VisualBlocks::NodeRegistry &registry);

	private:
		// Screen <-> graph space. The graph stores GRAPH space so a node position
		// survives a reopen; only the view transform is inverted per frame.
		// Screen <-> graph space. Nodes are stored in graph space so positions
		// persist; only the view transform is inverted per frame.
		Point ToScreen(const Point &graphPos) const;
		Point ToGraph(const Point &screenPos) const;
		float NodeWidth(const std::string &typeId) const;
		// Derived from the pin count, not a constant. The comment above explains the
		// bug a fixed height caused: text drawn just outside its own box, which reads
		// as "the text is not rendering" when it very much is.
		// Height DERIVED FROM THE PIN COUNT, not a constant.
		//
		// A fixed height cannot work: a node with two data pins needs two
		// label rows, and a fixed box either clips them or (at a wide node)
		// leaves them spilling over the border onto the canvas behind. That
		// is what made node text look like it was not rendering -- the text
		// WAS drawn, just outside its own box.
		float NodeHeight(const VisualBlocks::Graph &graph,
					   const VisualBlocks::NodeRegistry &registry,
					   const VisualBlocks::GraphNode &node) const;

		// DrawGrid takes an origin and a SIZE rather than the canvas rect, so it can be
		// drawn in whatever space the caller has already transformed into.
		void DrawGrid(const Point &origin, const Point &size);
		void DrawNode(VisualBlocks::Graph &graph,
					  const VisualBlocks::NodeRegistry &registry,
					  Visual &visual, Events &events,
					  const std::function<bool(const VisualBlocks::GraphNode &)> &drawBody);
		// The wire pass is separate from the node pass so wires can be drawn under or
		// over all nodes without either pass knowing about the other.
		void DrawWires(const VisualBlocks::Graph &graph,
					   const VisualBlocks::NodeRegistry &registry,
					   const Point &origin, const Point &size);

		// Pin anchors as a vector of (pinId, Point) pairs, so the id travels with its
		// position. The two functions below are separate rather than one with a flag
		// because a caller almost always wants exactly one edge.
		// Pin anchor points, in graph space, for one node.
		std::vector<std::pair<std::string, Point>> InputPinPositions(
			const VisualBlocks::Graph &graph,
			const VisualBlocks::NodeRegistry &registry,
			const VisualBlocks::GraphNode &node) const;
		std::vector<std::pair<std::string, Point>> OutputPinPositions(
			const VisualBlocks::Graph &graph,
			const VisualBlocks::NodeRegistry &registry,
			const VisualBlocks::GraphNode &node) const;

		Point InputPinPos(const VisualBlocks::Graph &graph,
						   const VisualBlocks::NodeRegistry &registry,
						   const VisualBlocks::GraphNode &node,
						   const std::string &pinId) const;
		Point OutputPinPos(const VisualBlocks::Graph &graph,
							const VisualBlocks::NodeRegistry &registry,
							const VisualBlocks::GraphNode &node,
							const std::string &pinId) const;

		// Nearest-wins within a radius rather than first-hit. With pins a few pixels
		// apart, first-hit resolves to whichever the draw order happened to put first;
		// nearest-wins resolves to the one the author actually aimed at.
		// Find the pin under `graphPos` within `radius`. Nearest-wins so two
		// adjacent pins resolve to the one the author actually aimed at.
		bool FindPinAt(const VisualBlocks::Graph &graph,
					   const VisualBlocks::NodeRegistry &registry,
					   const Point &graphPos, float radius,
					   std::string &outNodeId, std::string &outPinId,
					   bool &outIsInput) const;

		// View state, deliberately not persisted -- the header says node positions are
		// part of the authored file and pan/zoom are a property of the current window.
		// ---- view state (not persisted) ----
		Point m_Pan = Point();
		float m_Zoom = 1.0f;
		bool m_Panned = false;

		// Interaction state is separate from view state, and the split is not
		// arbitrary: everything in this block is discarded on a graph reload.
		// ---- interaction state ----
		std::string m_Selected;
		bool m_DraggingNode = false;
		std::string m_DragNodeId;
		Point m_DragGrabOffset = Point();

		// Strings rather than pointers, because the graph can reallocate mid-drag. The
		// cost is a string comparison per pin hit-test.
		// A wire drag runs from an OUT pin to an IN pin. Held as strings, not
		// pointers, because the graph can reallocate while the drag is live.
		bool m_DraggingWire = false;
		std::string m_WireFromNode;
		std::string m_WireFromPin;
		bool m_WireFromIsInput = false;
		// The mouse position while a wire is being dragged -- interaction state, not
		// view state, because it does not survive a graph reload.
		Point m_WireCursor = Point();

		// THE MEMOISATION, and the two `mutable` keywords are the interesting part: they
		// let a const lookup fill the cache, a deliberate widening of constness. The
		// values are derived rather than chosen, so the comment calling this
		// memoisation and not state is fair.
		//
		// DEFECT (thread safety): mutable state written from a const method is not
		// thread-safe. Safe only because a canvas is drawn on one thread, which nothing
		// in this header enforces. Not fixed; comments only.
		// Geometry memo: pin positions derive from the node's TYPE and size
		// alone, so the same id always yields the same points. Mutable so a
		// const lookup can fill them -- this is memoisation, not state.
		// Invalidated wholesale by Render on any graph edit.
		// Invalidated wholesale by Render on any graph edit. That is the safe choice: a
		// per-entry check would have to detect every way a node's size could change,
		// and pin count is not the only one.
		mutable std::unordered_map<std::string, Visual> m_Visuals;
		mutable std::unordered_map<std::string,
								   std::vector<std::pair<std::string, Point>>>
			m_InPins;
		mutable std::unordered_map<std::string,
								   std::vector<std::pair<std::string, Point>>>
			m_OutPins;
	// End of the struct. Everything above is either a documented public surface
	// or a private helper with a stated reason for existing.
	};

} // namespace Wasteland