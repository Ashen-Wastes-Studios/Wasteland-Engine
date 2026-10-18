// First include by convention: wlpch.h brings in the platform, GLFW and glad
// set every editor translation unit needs before anything else is parsed.
#include "wlpch.h"
// This .cpp is the only place ImGui is needed for the canvas component;
// NodeCanvas.h deliberately avoids ImGui types (see its Point rationale) so
// any panel can include the canvas without pulling imgui into its own header.
#include "NodeCanvas.h"

// std::min / std::max / std::clamp: the FrameAll bounding box and the zoom
// floor that keeps node labels inside their own boxes.
#include <algorithm>
// std::fabs and std::fmod: Bezier handle length, and the pan phase that keeps
// the grid visually fixed while the graph scrolls underneath it.
#include <cmath>

// Reached only through the draw list and the immediate-mode calls below.
#include <imgui/imgui.h>


// The canvas is engine-side, not editor-side: it is reusable by any visual
// blocks surface, and it owns no file, no palette and no compile action.
namespace Wasteland
{

	// The type families this file touches, imported once so the member signatures
	// below stay readable instead of being fully qualified throughout.
	using VisualBlocks::Graph;
	// A committed wire: a source and a target node id, plus a pin id on each.
	using VisualBlocks::GraphLink;
	// One node instance. X/Y are graph-space pixels and are part of the saved file,
	// which is why the view transform is applied only at draw time.
	using VisualBlocks::GraphNode;
	// A registry type record. Its Pins list is the only source of node height and
	// pin positions -- there is no per-node copy of that information.
	using VisualBlocks::NodeDef;
	// Read-only here: the canvas never registers a type, it only looks one up to
	// ask how many pins that type has.
	using VisualBlocks::NodeRegistry;
	// One pin on a type. Exec pins are excluded from rows and from hit-testing
	// because they ride the node edge instead of occupying a labelled row.
	using VisualBlocks::PinDef;
	// In/Out. A pin dot is placed on the left or right box edge from this.
	using VisualBlocks::PinDir;
	// Drives pin colour (PinColour) and the exec-versus-data wire distinction.
	using VisualBlocks::PinType;

	// File-local helpers only: colour, geometry and wire drawing. Nothing here
	// needs to be visible to a panel, so it stays out of the header.
	namespace
	{
		// Node geometry. Fixed, because a node that resizes as it gains pins
		// makes wires jump every time a pin is added and turns the graph into
		// something the author cannot keep in their head.
		// Box width is a CONSTANT even though NodeWidth() takes a type id.
		// DEFECT (stale contract): NodeCanvas.h says node width is 'derived from the
		// title, not stored', but the implementation ignores its argument and returns
		// this, so a long title is clipped and a short one leaves dead space.
		constexpr float kNodeWidth = 180.0f;
		// The title strip. Also the anchor for exec wires, which attach at half its
		// height on the node's side edge (see DrawWires).
		// kMinZoom exists because label text is fixed-size while geometry is not:
		// below roughly 0.7 a 13px default font no longer fits inside this 26px band,
		// which is the clipping that made node text look like it was not rendering.
		constexpr float kNodeHeaderHeight = 26.0f;
		// One labelled row per data pin. Row N is placed at header + rowHeight *
		// (N + 0.5) -- the half-row offset centres the dot in its row. That formula
		// is repeated in InputPinPositions, OutputPinPositions and DrawNode, and all
		// three copies must agree or the dots stop lining up with the labels.
		constexpr float kPinRowHeight = 20.0f;
		// Left inset for title and pin label text. NOT multiplied by zoom at the call
		// sites below, so the gap between box edge and glyphs stays constant on screen
		// while the box itself scales.
		constexpr float kNodePadding = 8.0f;

		// Pin hit radius in graph-space pixels, scaled by zoom so the target
		// feels the same size on screen at any zoom level.
		// Drawn as kPinRadius * m_Zoom (DrawNode) and hit-tested as kPinRadius /
		// m_Zoom (Render). The two are the same circle expressed in the two spaces.
		constexpr float kPinRadius = 7.0f;

		// The smallest zoom at which node labels still fit INSIDE their
		// node. Text is drawn at a fixed font size; geometry is not, so a
		// smaller zoom shrinks the box around text that stays the same
		// size. Used by both FrameAll and the zoom wheel so the two
		// cannot drift apart and reintroduce the clipping.
		// Shared by FrameAll and the zoom wheel so the two cannot disagree: a graph
		// framed at 0.8 and then wheeled down stays legible, one framed low and
		// wheeled is not.
		constexpr float kMinZoom = 0.75f;

		// Point -> ImVec2. The field order matches, so this is a field copy rather
		// than a reinterpretation.
		ImVec2 ToImVec(const Point &p) { return ImVec2(p.X, p.Y); }

		// The inverse, used once per frame on the mouse position to get back into
		// engine types before the view transform is undone.
		Point FromImVec(const ImVec2 &v) { return Point{v.x, v.y}; }

		// ImVec2 defines operator+ but NOT operator-, so the difference of two
		// of them does not compile. Needed to convert a mouse position into
		// canvas-local space.
		ImVec2 Sub(const ImVec2 &a, const ImVec2 &b)
		{
			// Applied in SCREEN space, before the zoom divide, so the result is
			// (cursor - canvasOrigin) / zoom rather than cursor/zoom - origin/zoom.
			// The order matters; the two differ by pan*zoom.
			return ImVec2(a.x - b.x, a.y - b.y);
		}

		// Pin colour by wire type.
		//
		// Exec is drawn thick and pale, data thin and coloured. This is not
		// decoration: an exec wire and a data wire look identical otherwise,
		// and the one mistake an author makes is running an exec into a data
		// pin. Colour makes that visible before the validator says it.
		// One entry per wire type. A new PinType falls to the default grey rather
		// than to no colour at all.
		ImVec4 PinColour(PinType type)
		{
			switch (type)
			{
			// Near-white, and drawn THICK (the thickness argument in DrawWires), so an
			// exec wire cannot be mistaken for a value wire at a glance.
			case PinType::Exec:   return ImVec4(0.90f, 0.90f, 0.85f, 1.00f);
			// Bool: the warm red used for flags and conditions.
			case PinType::Bool:   return ImVec4(0.95f, 0.45f, 0.45f, 1.00f);
			// Int and Float deliberately share a colour. The wire carries a number either
			// way; whether the two ends agree is the validator's problem, not the eye's.
			case PinType::Int:
			case PinType::Float:  return ImVec4(0.45f, 0.80f, 0.95f, 1.00f);
			// String: green.
			case PinType::String: return ImVec4(0.55f, 0.90f, 0.55f, 1.00f);
			// Entity: a scene object handle rather than a value.
			case PinType::Entity: return ImVec4(0.85f, 0.70f, 0.95f, 1.00f);
			// An unrecognised type still gets a dot and a wire, so a graph authored on a
			// newer build degrades to grey rather than to nothing at all.
			default:              return ImVec4(0.75f, 0.75f, 0.75f, 1.00f);
			}
		}

		// A cubic Bezier from an output pin to an input pin.
		//
		// The control points extend horizontally out of each node, so a wire
		// leaving a node's right edge enters the next node's left edge. A
		// straight line would be ambiguous about direction at a glance; the
		// curve makes flow read left-to-right the way the generated code does.
		// Works entirely in screen pixels: the callers apply ToScreen first. Keeping
		// the curve in screen space means its shape does not change with zoom.
		void AddBezier(ImDrawList *draw, const Point &from, const Point &to,
					   const ImVec4 &colour, float thickness)
		{
			// Handle length grows with the horizontal gap but never drops below 40px, so
			// a wire between two adjacent nodes still bows instead of collapsing into a
			// straight diagonal that gives no hint of direction.
			const float dx = std::max(40.0f, std::fabs(to.X - from.X) * 0.5f);
			// The two control points sit on the horizontal through each endpoint (the
			// Point temporaries below), which is what makes the wire leave the right edge
			// and arrive at the left edge rather than leaving at an arbitrary tangent.
			draw->AddBezierCubic(
				ToImVec(from), ToImVec(Point{from.X + dx, from.Y}),
				ToImVec(Point{to.X - dx, to.Y}), ToImVec(to),
				ImGui::ColorConvertFloat4ToU32(colour),
				thickness);
		}

	// End of the file-local helpers.
	} // namespace

	// ---------------------------------------------------------------- geometry

	// The entire view transform: scale by zoom, then translate by pan. Both are
	// plain arithmetic, which is what lets ToGraph below be the exact inverse.
	Point NodeCanvas::ToScreen(const Point &graphPos) const
	{
		// Pan is in screen pixels and zoom is unitless. Nodes are stored unscaled in
		// the graph file, so nothing here is ever written back to disk.
		return Point{graphPos.X * m_Zoom + m_Pan.X,
					 graphPos.Y * m_Zoom + m_Pan.Y};
	}

	// The inverse, and the only place the zoom divide happens -- every mouse
	// interaction below goes through it.
	Point NodeCanvas::ToGraph(const Point &screenPos) const
	{
		// Guard the zoom divide: a zero or negative zoom would collapse every
		// coordinate to infinity and make the canvas unusable with no way back.
		// DEFENSIVE and currently unreachable: both writers of m_Zoom (FrameAll and
		// the wheel) clamp it to [kMinZoom, 2.0f]. It is kept because a collapsed or
		// negative zoom is not recoverable from the UI -- there is no gesture that
		// scales back up from an infinite canvas.
		const float z = (m_Zoom > 0.01f) ? m_Zoom : 1.0f;
		// Subtract pan BEFORE dividing, i.e. the reverse of the order used to build
		// the point. Swapping the two operations would offset the result by pan*zoom.
		// See the note on Sub() above for why the caller works in screen space.
		return Point{(screenPos.X - m_Pan.X) / z, (screenPos.Y - m_Pan.Y) / z};
	}

	// The parameter is unnamed and unused. This is the defect noted at
	// kNodeWidth: the header documents a title-derived width, the code returns a
	// constant, and every call site below passes a type id that is discarded.
	float NodeCanvas::NodeWidth(const std::string &) const
	{
		// Constant for every type. Callers that mean 'the right edge of a node' --
		// the output pin x, the exec wire anchor, the node hit-test box -- all rely on
		// this agreeing with the width DrawNode actually draws.
		return kNodeWidth;
	}

	// Height is derived rather than stored because it depends on the registry
	// entry for this node's type, and the registry is not available when a graph
	// is loaded from disk. It is computed per node per frame instead.
	float NodeCanvas::NodeHeight(const Graph &graph, const NodeRegistry &registry,
								 const GraphNode &node) const
	{
		// One row per DATA pin -- and that means EVERY non-exec pin, not just
		// the inputs. DrawNode lays out a labelled row for outputs too, so
		// counting inputs alone made the box too short and the output labels
		// fell out below it. Exec pins ride the node edge, not a row. At least
		// one row, so a pin-less node is still a clickable box.
		// Counts ROWS, not just inputs. DrawNode below lays out one labelled row for
		// every non-exec pin, so this count must equal that loop's count exactly or
		// labels spill outside the box.
		int dataPins = 0;
		// Null for a type this build does not know -- a graph saved by a newer editor,
		// or a hand-edited file. That falls through to the one-row minimum below,
		// which keeps the node visible and clickable instead of collapsing it.
		const NodeDef *def = registry.Find(node.TypeId);
		if (def != nullptr)
		{
			// Registry order, which is the same order DrawNode walks, so row N counted
			// here is row N drawn there.
			for (const PinDef &pin : def->Pins)
			{
				// Exec pins ride the node edge and get no row: control flow is not a value
				// with somewhere to put a label.
				if (pin.Type == PinType::Exec)
					continue;
				// Both directions count. An output with a label still occupies a row, and it
				// shares one row sequence with the inputs.
				++dataPins;
			}
		}
		// A node with no data pins still needs a body, so the floor is one row.
		if (dataPins < 1)
			// This is also what keeps an unknown type drawable: with no definition there
			// are no pins to count, and a zero-height box cannot be clicked.
			dataPins = 1;
		// No bottom padding is added. kPinRowHeight already covers the last row and
		// its label is centred in it, so adding kNodePadding here would desynchronise
		// this from the row arithmetic in DrawNode.
		return kNodeHeaderHeight + kPinRowHeight * static_cast<float>(dataPins);
	}

	// Memoised per node id (see the mutable maps in the header). Called from the
	// draw pass AND from hit-testing, so the memo avoids walking the registry for
	// every pin of every node several times per frame.
	std::vector<std::pair<std::string, Point>> NodeCanvas::InputPinPositions(
		const Graph &graph, const NodeRegistry &registry,
		const GraphNode &node) const
	{
		// A hit here was filled earlier in THIS frame -- Render clears the maps at the
		// top of every frame. One consequence: on the frame a node drag begins,
		// FindPinAt has already filled this map with the pre-drag positions, so wires
		// trail the node by one frame. Later frames are correct because nothing
		// refills the map before the drag update.
		auto it = m_InPins.find(node.Id);
		// Returned BY VALUE. The caller gets a copy, so nothing it holds can dangle
		// the way a reference into the cache would if the graph were edited.
		if (it != m_InPins.end())
			return it->second;

		// pin id -> graph-space point. Keyed by id and never by index, so a type that
		// gains a pin does not shift the position of every pin below it.
		std::vector<std::pair<std::string, Point>> out;
		const NodeDef *def = registry.Find(node.TypeId);
		// Unknown type: no pins to place. The empty result is returned WITHOUT being
		// cached, so the next frame repeats the same cheap lookup rather than
		// remembering an answer that would be wrong as soon as the type registers.
		if (def == nullptr)
			return out;

		// First row sits half a row below the header so the dot is centred in its row
		// rather than straddling the header border. DrawNode recomputes the same
		// value in screen space; the two must not drift apart.
		float y = node.Y + kNodeHeaderHeight + kPinRowHeight * 0.5f;
		for (const PinDef &pin : def->Pins)
		{
			// Inputs only, never exec. DEFECT: this counter advances once per INPUT, while
			// DrawNode's row counter advances once per non-exec pin regardless of
			// direction. On a node with both inputs and outputs the drawn output dot sits
			// one or more rows below the point returned here, so the circle the author
			// sees and the circle they can grab are not the same circle.
			if (pin.Dir != PinDir::In || pin.Type == PinType::Exec)
				continue;
			// x is the node's LEFT edge: an input is where a wire arrives. The dot is drawn
			// centred on that edge, so half of it overhangs the box.
			out.push_back({pin.Id, Point{node.X, y}});
			// Stepped by the UNSCALED row height, because these points are graph-space
			// and only get multiplied by zoom at draw time.
			y += kPinRowHeight;
		}
		// `graph` is genuinely unused. The layout depends on the node's type and
		// position alone, which is what makes the memo above sound: the same id always
		// yields the same points regardless of what else is in the graph.
		(void)graph;
		// Written from a const member function; the map is mutable for exactly this.
		// Memoisation, not state.
		m_InPins[node.Id] = out;
		// A second copy, since the map entry above is already one.
		return out;
	}

	// The mirror image of InputPinPositions, with the same memo structure.
	std::vector<std::pair<std::string, Point>> NodeCanvas::OutputPinPositions(
		const Graph &graph, const NodeRegistry &registry,
		const GraphNode &node) const
	{
		// Separate map from the input one so the two caches cannot be confused.
		auto it = m_OutPins.find(node.Id);
		// Cache hit, returned by value as above.
		if (it != m_OutPins.end())
			return it->second;

		// Same shape as the input list; only the x edge differs.
		std::vector<std::pair<std::string, Point>> out;
		const NodeDef *def = registry.Find(node.TypeId);
		// Unknown type: nothing to place, and nothing worth caching.
		if (def == nullptr)
			return out;

		// Identical to the input version. Duplicated rather than shared because the
		// two differ in exactly one expression, and a shared helper would have to
		// re-test the direction on every pin.
		float y = node.Y + kNodeHeaderHeight + kPinRowHeight * 0.5f;
		for (const PinDef &pin : def->Pins)
		{
			// Outputs only. The same counting defect as the input version applies here:
			// this counter restarts at the first output, while DrawNode's continues from
			// wherever the inputs left off.
			if (pin.Dir != PinDir::Out || pin.Type == PinType::Exec)
				continue;
			// Right edge. NodeWidth currently ignores the type id and returns kNodeWidth,
			// so this is the same value DrawNode uses for br.x and for the output dot.
			out.push_back({pin.Id, Point{node.X + NodeWidth(node.TypeId), y}});
			// Graph-space step, as above; the zoom multiply happens only at draw time.
			y += kPinRowHeight;
		}
		// Unused, as in the input version.
		(void)graph;
		// Memoised in its own map; the two are never mixed.
		m_OutPins[node.Id] = out;
		// By value, like the input version.
		return out;
	}

	// Convenience wrapper over InputPinPositions for the single-pin case the wire
	// drawer needs. Linear scan, which is fine: a type has a handful of pins.
	Point NodeCanvas::InputPinPos(const Graph &graph, const NodeRegistry &registry,
								  const GraphNode &node, const std::string &pinId) const
	{
		// The temporary vector lives for the duration of the loop; the returned point
		// is copied out of it, so no reference escapes.
		for (const auto &pair : InputPinPositions(graph, registry, node))
		{
			// Exact id match. Pins are addressed by id, never by index.
			if (pair.first == pinId)
				return pair.second;
		}
		// FALLBACK, not an error. A link naming a pin the type no longer declares
		// lands here: the wire is drawn from the header corner instead of vanishing,
		// so a broken link stays visible. The validator reports the real problem.
		return Point{node.X, node.Y + kNodeHeaderHeight};
	}

	// Mirror of InputPinPos, for the source end of a data wire.
	Point NodeCanvas::OutputPinPos(const Graph &graph, const NodeRegistry &registry,
								   const GraphNode &node, const std::string &pinId) const
	{
		// Same lifetime reasoning as above.
		for (const auto &pair : OutputPinPositions(graph, registry, node))
		{
			// Exact id match.
			if (pair.first == pinId)
				return pair.second;
		}
		// Same fallback as the input side, mirrored onto the right edge so a broken
		// wire still visibly leaves and enters the boxes.
		return Point{node.X + NodeWidth(node.TypeId), node.Y + kNodeHeaderHeight};
	}

	// One hit test for the whole canvas: every pin of every node is compared and
	// the NEAREST within `radius` wins. Called only on click and on wire release,
	// never per frame, so the cost of walking every pin is paid twice a drag.
	bool NodeCanvas::FindPinAt(const Graph &graph, const NodeRegistry &registry,
							   const Point &graphPos, float radius,
							   std::string &outNodeId, std::string &outPinId,
							   bool &outIsInput) const
	{
		// The three out-params are only written when something is found, so the caller
		// owns their initial values; both call sites in Render initialise them.
		bool found = false;
		// Seeded with the accept threshold and then reused as the running best, so no
		// separate 'closest so far' variable is needed. Compared in squared distance:
		float bestSq = radius * radius;

		// Nearest-wins. Iterating in declaration order and taking the first
		// hit makes the target depend on pin order rather than on where the
		// author actually pointed, which feels broken even when it is not.
		// Declaration order. Exact ties resolve to whichever pin comes first, which is
		// arbitrary but stable, and only happens when two pins are exactly the same
		// distance from the cursor -- i.e. effectively on top of each other.
		for (const GraphNode &node : graph.Nodes)
		{
			// The memo above is what makes this affordable: positions come from a hash
			// lookup on a hit and are computed at most once per node per frame.
			for (const auto &pair : InputPinPositions(graph, registry, node))
			{
				const float dx = pair.second.X - graphPos.X;
				const float dy = pair.second.Y - graphPos.Y;
				// squared distance avoids a sqrt per pin, and comparison against it is
				// monotone, so nearest-wins needs no actual distance.
				const float dSq = dx * dx + dy * dy;
				// Strictly less than, so an exact tie keeps the earlier pin. bestSq does double
				// duty as the radius gate: once a pin is inside it, only closer pins can win.
				if (dSq < bestSq)
				{
					// Shrinks the gate for every pin that comes after, including pins on later
					// nodes -- which is precisely the nearest-wins behaviour.
					bestSq = dSq;
					outNodeId = node.Id;
					outPinId = pair.first;
					// The third out-param. It is what lets a drag be normalised to out -> in on
					// release regardless of which end the author started from.
					outIsInput = true;
					found = true;
				}
			}
			// Outputs share the SAME bestSq, so an input and an output compete against
			// each other rather than inputs always winning by priority.
			for (const auto &pair : OutputPinPositions(graph, registry, node))
			{
				const float dx = pair.second.X - graphPos.X;
				const float dy = pair.second.Y - graphPos.Y;
				const float dSq = dx * dx + dy * dy;
				if (dSq < bestSq)
				{
					bestSq = dSq;
					outNodeId = node.Id;
					outPinId = pair.first;
					// The distinguishing bit for the output side.
					outIsInput = false;
					found = true;
				}
			}
		}
		// False means the click was not near a pin, and Render falls through to the
		// node body hit-test instead.
		return found;
	}

	// ------------------------------------------------------------------ render

	// Plain assignment with no existence check. DEFECT: the header documents this
	// as 'Select a node by id (no-op if absent)', but an id that is not in the
	// graph becomes the selection anyway, and no event is emitted -- unlike the
	// click path below, which reports NodeSelected. Harmless in practice only
	// because the panel calls it with an id it just found in this graph.
	void NodeCanvas::Select(const std::string &nodeId) { m_Selected = nodeId; }

	// Fit every node into the visible content region. Called when the canvas first
	// appears and when the author asks to frame the graph.
	void NodeCanvas::FrameAll(const Graph &graph,
								 const NodeRegistry &registry)
	{
		// Nothing to frame: pan and zoom are left alone rather than reset to an
		// arbitrary origin.
		if (graph.Nodes.empty())
			return;

		// Seeded from node 0 so the loop below can use plain min/max with no sentinel
		// value. node 0 is also visited by that loop, so the seed is redundant rather
		// than wrong -- a leftover from a version that started the loop at index 1.
		float minX = graph.Nodes[0].X;
		float minY = graph.Nodes[0].Y;
		float maxX = graph.Nodes[0].X + kNodeWidth;
		// Node HEIGHT on the Y axis, not the width constant: boxes are taller than
		// they are wide for most types, and framing to width alone would crop them.
		float maxY = graph.Nodes[0].Y + NodeHeight(graph, registry, graph.Nodes[0]);
		for (const GraphNode &node : graph.Nodes)
		{
			minX = std::min(minX, node.X);
			minY = std::min(minY, node.Y);
			maxX = std::max(maxX, node.X + kNodeWidth);
			maxY = std::max(maxY, node.Y + NodeHeight(graph, registry, node));
		}

		// Measured at CALL time. The region shrinks as items are submitted, so the
		// panel must invoke this before laying out anything else in the same window.
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		// A collapsed or not-yet-laid-out panel would otherwise produce a negative or
		// enormous zoom, and the author would have no gesture to recover from it.
		if (avail.x < 10.0f || avail.y < 10.0f)
			return;

		// Screen-space margin left around the graph after fitting. Not scaled by zoom,
		// so it stays the same visual gutter whatever the fit chose.
		const float pad = 40.0f;
		// Floored at 1px: a single node, or a graph whose nodes all sit on one point,
		// would otherwise divide by zero below.
		const float spanX = std::max(1.0f, maxX - minX);
		const float spanY = std::max(1.0f, maxY - minY);
		// Fit to the TIGHTER of the two axes, so the whole bounding box lands inside
		// the content region rather than one dimension overflowing.
		m_Zoom = std::min((avail.x - pad * 2.0f) / spanX,
						  (avail.y - pad * 2.0f) / spanY);
		// Floor the fit at 0.75, and the reason is not aesthetics.
		//
		// Node text is drawn at a FIXED font size while the node's geometry
		// scales with zoom. ImGui's draw list takes a font, not a size, so
		// there is no cheap way to scale label text with the zoom. The
		// consequence is that the box shrinks while the text does not: below
		// roughly 0.7 the 13px default font no longer fits inside the 26px
		// header, the label overflows its own box, and the clip rect eats it.
		// That is what "the node text does not show" looked like.
		//
		// So the layout floor is the LEGIBILITY floor: keep the box bigger
		// than its own text. A graph that does not wholly fit is pannable;
		// one whose labels are clipped is not readable at any zoom.
		// The floor is the legibility floor explained at kMinZoom; the 1.5 ceiling
		// stops a two-node graph from being blown up until one node fills the screen.
		m_Zoom = std::clamp(m_Zoom, kMinZoom, 1.5f);
		// Three terms: shift the graph's left edge `pad` pixels in, then centre the
		// leftover slack. That slack goes NEGATIVE when the clamp above raised the
		// zoom past the fit -- in which case the graph overflows evenly on both sides
		// and stays reachable by panning, which is the intended trade.
		m_Pan.X = pad - minX * m_Zoom + (avail.x - pad * 2.0f - spanX * m_Zoom) * 0.5f;
		// Same three terms against the height span rather than the width one.
		m_Pan.Y = pad - minY * m_Zoom + (avail.y - pad * 2.0f - spanY * m_Zoom) * 0.5f;
	}

	// Background lattice. Purely decorative, drawn first so nodes and wires land
	// on top of it.
	void NodeCanvas::DrawGrid(const Point &origin, const Point &size)
	{
		// The CURRENT window's draw list. Every draw call in this file targets the
		// window the canvas was submitted to, so the canvas cannot span two windows.
		ImDrawList *draw = ImGui::GetWindowDrawList();

		// The grid is drawn in screen space and spaced by zoom, so it stays
		// visually constant as the author zooms -- a grid that scales with the
		// graph gives no sense of movement while panning.
		// Base spacing scaled by zoom, then given a pan phase below. Scaling by zoom
		// and phasing by pan together are what make the grid look pinned to the
		// window instead of to the graph.
		const float spacing = 32.0f * m_Zoom;
		// Below this the lines merge into a grey wash, which is worse than no grid.
		// Currently unreachable: kMinZoom 0.75 gives 24px. The guard is kept because
		// it costs nothing and protects the look if the zoom floor is ever lowered.
		if (spacing < 8.0f)
			return;   // too dense to read; a grey wash is worse than none

		// Alpha 18 of 255. The grid is a hint and must not compete with wires or text.
		const ImU32 line = IM_COL32(255, 255, 255, 18);
		// Clip bound on the far edge. Unlike DrawWires and DrawNode, this pass IS
		// bounded by the canvas rect -- see the (void)origin note in DrawWires for the
		// pass that is not.
		const float right = origin.X + size.X;
		const float bottom = origin.Y + size.Y;

		// fmod(pan, spacing) gives the phase of the first line past the left edge, so
		// the grid stays stationary on screen while the graph pans under it. For a
		// negative pan fmod is negative, so the first line lands left of the origin
		// and the loop simply draws one extra line.
		for (float x = origin.X + std::fmod(m_Pan.X, spacing); x < right; x += spacing)
			// Full-height vertical line. No per-line clip is needed: x is bounded by the
			// loop condition and y is bounded by origin/bottom.
			draw->AddLine(ImVec2(x, origin.Y), ImVec2(x, bottom), line);
		// The same phase trick on the vertical axis.
		for (float y = origin.Y + std::fmod(m_Pan.Y, spacing); y < bottom; y += spacing)
			// Full-width horizontal line.
			draw->AddLine(ImVec2(origin.X, y), ImVec2(right, y), line);
	}

	// One pass over every committed link, then the in-progress drag. Drawn before
	// the nodes so wires pass underneath the boxes.
	void NodeCanvas::DrawWires(const Graph &graph, const NodeRegistry &registry,
							   const Point &origin, const Point &size)
	{
		ImDrawList *draw = ImGui::GetWindowDrawList();

		// Straight from the graph every frame. No wire cache: a link is a handful of
		// floats and the expensive part, the pin lookup, is memoised separately.
		for (const GraphLink &link : graph.Links)
		{
			// Both ends resolved from ids on every frame. The graph stores ids rather than
			// pointers because any edit can reallocate the node vector mid-drag.
			const GraphNode *from = graph.FindNode(link.FromNode);
			const GraphNode *to = graph.FindNode(link.ToNode);
			// A link to a deleted node is stale data, not a crash: the graph is
			// mid-edit and the validator will report it. Skip it.
			if (from == nullptr || to == nullptr)
				continue;

			// The SOURCE type record, and the only one consulted for the wire's
			const NodeDef *fromDef = registry.Find(from->TypeId);
			const NodeDef *toDef = registry.Find(to->TypeId);
			// Same stale-reference guard for an unregistered type. DEFECT: toDef is
			// fetched and null-checked but never used for anything else. The DESTINATION
			// pin's type is never consulted, so a link joining a float to a bool is drawn
			// in the source colour with nothing on screen to say it is wrong -- the
			// validator is the only thing that catches it. Compare FindPinAt, which at
			// least checks that both pins exist.
			if (fromDef == nullptr || toDef == nullptr)
				continue;

			// Exec wires land on the node edge, not on a pin: a control-flow
			// wire connects the statement chain, and giving it a pin row would
			// imply it carries a value.
			// Default is data. A link naming an exec pin flips this, which changes BOTH
			// the endpoints and the thickness below.
			bool isExec = false;
			// Neutral grey, used only when FromPin matches no pin on the source type -- a
			// pin the type no longer declares. Visible rather than invisible, so a link to
			// a removed pin is still drawn and can be found.
			ImVec4 colour(0.8f, 0.8f, 0.8f, 1.0f);
			// Linear scan of the source type's pins, with no break on a match. That is only
			// observable if a type declared the same pin id twice, in which case the LAST
			// match wins rather than the first.
			for (const PinDef &pin : fromDef->Pins)
			{
				if (pin.Id != link.FromPin)
					continue;
				// Taken from the pin's declared type, not from the link: a GraphLink carries
				// no type field of its own.
				isExec = (pin.Type == PinType::Exec);
				// The source pin alone decides the colour, so a wire's look is a property of
				// where it starts and not of what it lands on.
				colour = PinColour(pin.Type);
			}

			Point fromPos;
			// Both endpoints are declared here and filled by exactly one of the branches
			// below; Point's defaults make an unfilled read harmless rather than UB.
			Point toPos;
			// Exec wires attach to the node edge at half the header height. There is no
			// pin dot for them, and using the header band keeps them clear of the data
			// pin rows below it.
			if (isExec)
			{
				// Uses the kNodeWidth constant directly rather than NodeWidth(). Harmless
				// today because NodeWidth returns that same constant, but the two would have
				// to be changed together.
				fromPos = Point{from->X + kNodeWidth, from->Y + kNodeHeaderHeight * 0.5f};
				// Enters the target's LEFT edge, the same edge a data wire would arrive at,
				// so converting a link from exec to data moves only its curvature, not its
				// endpoints.
				toPos = Point{to->X, to->Y + kNodeHeaderHeight * 0.5f};
			}
			else
			{
				// Data wire: the real pin point, out of the memoised list. The fallback inside
				// OutputPinPos covers a pin id the type no longer declares.
				fromPos = OutputPinPos(graph, registry, *from, link.FromPin);
				toPos = InputPinPos(graph, registry, *to, link.ToPin);
			}

			// Explicit acknowledgement that toDef is needed only for the null check
			// above. See the note there: the destination pin's type is never inspected.
			(void)toDef;
			// The only place graph space becomes screen space for wires; the curve helper
			// itself works purely in screen pixels.
			AddBezier(draw, ToScreen(fromPos), ToScreen(toPos), colour,
					  // Thickness scales with zoom as well, so an exec wire stays visibly thicker at
					  // every zoom level instead of converging on the data width when zoomed out.
					  isExec ? 2.5f * m_Zoom : 1.5f * m_Zoom);
		}

		// The wire being dragged, in a distinct colour so it does not read as
		// a committed connection.
		// The in-progress drag is drawn HERE as well as at the very end of Render.
		// This pass puts it under the node boxes; the second pass puts it on top.
		if (m_DraggingWire)
		{
			// Re-resolved from the id every frame, for the same reason committed wires are.
			const GraphNode *from = graph.FindNode(m_WireFromNode);
			if (from != nullptr)
			{
				// The anchor is whichever pin the drag STARTED on, input or output.
				Point anchor = m_WireFromIsInput
								   // Started on an input: anchor there.
								   ? InputPinPos(graph, registry, *from, m_WireFromPin)
								   // Otherwise an output. Both branches use the same fallback point as the
								   // committed-wire path, so the anchor cannot vanish mid-drag.
								   : OutputPinPos(graph, registry, *from, m_WireFromPin);
				draw->AddLine(ToImVec(ToScreen(anchor)),
							  // A straight segment, not a Bezier: the far end is the cursor, and a curve
							  // would misstate where the wire will actually land. Alpha 200 marks it as
							  // uncommitted.
							  ToImVec(ToScreen(m_WireCursor)),
							  IM_COL32(240, 200, 80, 200), 2.0f * m_Zoom);
			}
		}
		// DEFECT: the canvas rect is accepted and then discarded, so this pass is NOT
		// clipped to it. A long wire leaves the canvas and draws over whatever the
		// panel laid out below it. DrawGrid bounds itself with the same two values;
		(void)origin;
		// ...and the wire pass does not. Nothing pushes a clip rect here either, so
		// DrawNode below has the same exposure.
		(void)size;
	}

	// Box, header, title, pin dots and labels, then the panel's own body. This
	// function draws; it does not mutate the graph.
	void NodeCanvas::DrawNode(Graph &graph, const NodeRegistry &registry,
							  Visual &visual, Events &events,
							  const std::function<bool(const GraphNode &)> &drawBody)
	{
		ImDrawList *draw = ImGui::GetWindowDrawList();
		// The Visual handed in is a snapshot taken earlier in Render; the node is
		// re-looked-up by id so the draw uses current fields rather than the copy.
		GraphNode *node = graph.FindNode(visual.Id);
		// The node was deleted between the snapshot and this call. Skipping is
		// correct: there is nothing left to draw.
		if (node == nullptr)
			return;

		const NodeDef *def = registry.Find(node->TypeId);
		// Drives both the border colour and the ' (?)' title marker below.
		const bool known = (def != nullptr);
		// A user-supplied Title wins over TypeId. Unknown types keep the
		// " (?)" marker so a broken reference is still obvious.
		// An authored title replaces the type id -- the rename the editor supports.
		// Note the title is never measured: NodeWidth is constant, so a long title is
		// not accommodated by a wider box.
		const std::string title = !node->Title.empty()
									 ? node->Title
									 : (known ? def->TypeId
											: node->TypeId + " (?)");

		// Graph space to screen space, taking the position from the Visual snapshot
		// rather than from node->X. Render fills the Visual AFTER the drag update, so
		// the two agree; reading the snapshot is what keeps this function independent
		// of the graph's current position.
		const Point topLeft = ToScreen(Point{visual.X, visual.Y});
		// Zoom is applied once, here, so every primitive below works in screen pixels
		// and none of them re-derives the scale.
		const float w = NodeWidth(node->TypeId) * m_Zoom;
		// Derived height, so a type with more pins is a taller box and the pin dots
		// stay on the rows NodeHeight counted.
		const float h = NodeHeight(graph, registry, *node) * m_Zoom;
		// Top-left in ImGui's type, ready for the draw calls.
		const ImVec2 tl = ToImVec(topLeft);
		// Bottom-right. Pin dots are placed on tl.x and br.x, so both corners have to
		// derive from the same width or the dots will not sit on the border.
		const ImVec2 br(tl.x + w, tl.y + h);

		// Read, never set: selection is decided in the interaction block of Render and
		// this function only reflects it.
		const bool selected = (m_Selected == node->Id);
		// Three states: selected (amber, 2px), known type (grey, 1px), unknown type
		// (red, 1px). The red is the only on-canvas signal that a type is missing.
		const ImU32 border =
			// Amber, the same colour as the in-progress wire, so selection and dragging
			// read as one visual system rather than two.
			selected ? IM_COL32(240, 200, 80, 255)
					 // Neutral grey for a node whose type resolves.
					 : (known ? IM_COL32(120, 120, 130, 255)
							  // Red for an unregistered type.
							  : IM_COL32(220, 90, 90, 255));

		// Body fill, slightly translucent so the grid shows through faintly. Drawn
		// before the border so the border is not half-covered.
		draw->AddRectFilled(tl, br, IM_COL32(32, 32, 38, 240));
		// Border thickness doubles on selection -- the primary selection cue at small
		// zoom, where the fill colours are barely distinguishable.
		draw->AddRect(tl, br, border, selected ? 2.0f : 1.0f);
		// Header strip. Note this one DOES multiply the header height by zoom, unlike
		// the unscaled kNodePadding used for the text inside it.
		draw->AddRectFilled(tl, ImVec2(br.x, tl.y + kNodeHeaderHeight * m_Zoom),
							// The selected header is tinted amber to match its border.
							selected ? IM_COL32(70, 62, 36, 255)
									  // Otherwise a lighter grey than the body, so the title band reads as a region
									  // of its own rather than as part of the fill.
									  : IM_COL32(46, 46, 54, 255));

		// The font is passed EXPLICITLY. Every other primitive on this
		// canvas renders while the text does not, which rules out geometry,
		// colour and the clip rect, and leaves the implicit _Data->Font that
		// the (pos, col, fmt, ...) overload picks up.
		// A brighter title when selected, for the same reason the border thickens.
		const ImU32 titleColour = selected ? IM_COL32(250, 220, 140, 255)
											 : IM_COL32(220, 220, 230, 255);
		// The explicit-font overload. The comment above explains why the
		// (pos, colour, fmt, ...) form must not be used on this canvas.
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
				  // Neither the padding nor the 5.0f baseline offset is zoom-scaled, so at
				  // kMinZoom the title sits closer to the left edge than the geometry implies.
				  ImVec2(tl.x + kNodePadding, tl.y + 5.0f), titleColour,
				  // Null-terminated, no truncation and no ellipsis. DEFECT: nothing pushes a
				  // per-node clip rect, so a title wider than 180px is drawn straight out over
				  // the canvas rather than stopping at the box edge.
				  title.c_str());

		// Pins. Drawn, not hit-tested, here -- interaction is resolved once in
		// Render against cached positions, because ImGui hit-testing a moving
		// node body and a pin separately is how a wire drag turns into a node
		// drag.
		// Rows are laid out in GRAPH space and then scaled, so they line up
		// with the box the same way the pins do.
		// Screen-space mirror of the graph-space start row in InputPinPositions.
		// DEFECT: this counter advances once per non-exec pin, while
		// InputPinPositions and OutputPinPositions each advance only for their own
		// direction. On a node with both inputs and outputs, the dots drawn here sit
		// on different rows from the points the hit test uses, so the circle the author
		// sees is not the circle they can grab.
		float rowY = tl.y + (kNodeHeaderHeight + kPinRowHeight * 0.5f) * m_Zoom;
		// An unregistered type has no pins to draw; the box is still clickable.
		if (def != nullptr)
		{
			for (const PinDef &pin : def->Pins)
			{
				if (pin.Type == PinType::Exec)
					continue;
				// Same palette as the wire, so a dot and the wire leaving it share a colour.
				const ImVec4 col = PinColour(pin.Type);
				// Hand-rolled float-to-byte conversion instead of IM_COL32's byte arguments.
				// Alpha is forced to 255, so the alpha channel of PinColour is ignored here.
				const ImU32 c = IM_COL32(col.x * 255, col.y * 255, col.z * 255, 255);
				// Zoom-scaled, which is the exact inverse of the hit-test radius in Render
				const float r = kPinRadius * m_Zoom;
				// The pin's direction decides which edge the dot sits on.
				if (pin.Dir == PinDir::In)
					// Centred ON the left edge, so half the dot overhangs the box -- the same
					// convention the wire endpoints use, which is why the two look joined.
					draw->AddCircleFilled(ImVec2(tl.x, rowY), r, c);
				else
					// Output dot on the right edge. DEFECT: the label below is still drawn at the
					// LEFT inset for both directions, so an output pin's id appears across the
					// box instead of beside the dot it names.
					draw->AddCircleFilled(ImVec2(br.x, rowY), r, c);
			// Explicit font again, for the same reason as the title.
			draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
							  // -6.0f is a fixed optical centring offset for a ~13px font: the row's
							  // midpoint sits above the text baseline. Unscaled, so at low zoom labels sit
							  // slightly high within their rows.
							  ImVec2(tl.x + kNodePadding, rowY - 6.0f),
							  // Pin ids are labelled for both directions. Nothing is matched against the
							  // type's declared pins because this loop walks the type's own pin list.
							  IM_COL32(200, 200, 210, 255), pin.Id.c_str());
				// One row per pin, counted regardless of direction -- the other half of the
				// mismatch documented at the rowY initialisation above.
				rowY += kPinRowHeight * m_Zoom;
			}
		}

		// Let the panel draw the node's own contents. Skipped while dragging,
		// because an ImGui widget inside a box that is itself moving gets
		// clicks that land somewhere the author did not aim.
		if (drawBody && !m_DraggingNode)
			// DEFECT: the return value is discarded. NodeCanvas.h documents the callback
			// as 'returning true means the panel consumed the click, so the canvas does
			// not start a drag or change selection', but nothing here reads it. A click on
			// a literal field drawn inside a node therefore also selects the node and
			// starts dragging it, which is exactly the fight the callback exists to stop.
			drawBody(*node);
		// The Events& parameter is never written to. Every event the canvas produces is
		// built in Render's input block; the parameter itself is dead.
		(void)events;
	}

	// One frame: input handling and interaction state first, then the draw order of
	// grid, wires, nodes, and the drag wire on top. Everything reported comes back
	// in outEvents. The graph is only ever read, apart from node drags below.
	void NodeCanvas::Render(Graph &graph, const NodeRegistry &registry,
							Events &outEvents,
							std::function<bool(const GraphNode &)> drawNodeBody)
	{
		// Pin caches are keyed by node id and invalidated by a graph edit, not
		// by a timer. Clearing per frame would make them worthless; keeping
		// them across frames without invalidating would draw wires to pins that
		// no longer exist.
		// DEFECT (comment contradicts code): the comment above says the caches are
		// invalidated by a graph edit rather than per frame, but all three maps are
		// cleared unconditionally on every frame. The memo is still worth having
		// because within a single frame the pin walk is repeated by the hit test, the
		// wire pass and the node pass.
		m_InPins.clear();
		// Same, for the output map. The two are never shared.
		m_OutPins.clear();
		// The Visual snapshot map is also rebuilt every frame, by the node loop at the
		// bottom of this function.
		m_Visuals.clear();

		ImVec2 avail = ImGui::GetContentRegionAvail();
		// One invisible button owns the entire canvas rect. That is what makes
		// IsItemActive and IsItemHovered mean 'over the canvas' for every check below,
		// without submitting a widget per node. The ## prefix hides the label from the
		// ImGui id/debug overlay so it cannot collide with a real control.
		ImGui::InvisibleButton("##vbcanvas", ImVec2(std::max(avail.x, 50.0f),
												   // Floored at 50px on both axes: a collapsed panel would otherwise submit a
												   // zero-size item, which ImGui refuses and which IsItemHovered never reports.
												   std::max(avail.y, 50.0f)),
							   ImGuiButtonFlags_MouseButtonLeft |
								   // All three buttons are claimed so ImGui tracks a drag for each of them. Only
								   // the middle button actually pans; claiming the others stops ImGui treating
								   // them as unhandled.
								   ImGuiButtonFlags_MouseButtonRight |
								   ImGuiButtonFlags_MouseButtonMiddle);
		// True while one of those buttons is held AND the press began on this item, so
		// a drag that started in another panel cannot pan this graph.
		const bool canvasActive = ImGui::IsItemActive();
		// Hover gates zoom, picking and Delete below, so none of them fire once the
		// cursor leaves the canvas.
		const bool canvasHovered = ImGui::IsItemHovered();
		// DEFECT: read AFTER the button, and InvisibleButton calls ItemSize, which
		// advances DC.CursorPos.y to the next line (imgui.cpp:9835);
		// GetCursorScreenPos() returns that cursor verbatim (imgui.cpp:9984). So this
		// is the position one canvas-height BELOW the canvas, not its top-left corner.
		// The fix would be GetItemRectMin(), or reading the cursor before the button.
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		// The same corner in engine types, for the grid call and the mouse conversion.
		const Point originPoint{origin.x, origin.y};

		// Grid first. It is the only pass that actually uses this rect, because it is
		// the only one that bounds itself to it.
		DrawGrid(originPoint, Point{avail.x, avail.y});

		// Zoom on wheel, anchored under the cursor so the point under the
		// pointer stays put -- otherwise zooming walks the graph away from
		// whatever the author was looking at.
		// Zoom requires hover, so scrolling a list in the panel beside the canvas does
		// not change the graph scale.
		if (canvasHovered && ImGui::GetIO().MouseWheel != 0.0f)
		{
			const ImVec2 cursor = ImGui::GetMousePos();
			// Captured because the graph point under the cursor has to be computed with the
			// PRE-zoom scale and then re-projected with the new one.
			const float oldZoom = m_Zoom;
			// Same floor as FrameAll, and for the same reason: label text is
			// fixed-size while the box scales, so below ~0.7 a label no longer
			// fits inside its own node and is clipped away. Letting the wheel
			// go below that floor would reintroduce the exact bug the fit
			// floor prevents.
			// DEFECT: only the SIGN of MouseWheel is used, so a trackpad's stream of small
			// deltas and a mouse wheel's few large notches both zoom exactly 10% per frame
			// in which the value is non-zero -- and two notches arriving in one frame
			// count as a single step. The clamp floor is the shared legibility floor.
			m_Zoom = std::clamp(oldZoom *
									(ImGui::GetIO().MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f),
								kMinZoom, 2.0f);
			// Keep the graph point under the cursor fixed: that point is
			// (cursor - pan) / zoom, so solving for pan after changing zoom
			// means pan = cursor - graphPoint * newZoom. The previous form
			// divided by the NEW zoom, which is algebraically a no-op and
			// made every zoom walk the graph sideways.
			// The graph point currently under the cursor, computed with the old zoom.
			// Computing it with the NEW zoom instead is what made every zoom step walk the
			// graph sideways.
			const Point under =
				// The same zero-zoom guard as ToGraph, inlined because this value must be the
				// OLD zoom rather than the clamped new one.
				Point{(cursor.x - m_Pan.X) / (oldZoom > 0.01f ? oldZoom : 1.0f),
					  (cursor.y - m_Pan.Y) / (oldZoom > 0.01f ? oldZoom : 1.0f)};
			// Solving ToScreen for pan under the new zoom. Because `under` was captured at
			// the old zoom, the graph point beneath the cursor is unchanged by the zoom.
			m_Pan.X = cursor.x - under.X * m_Zoom;
			m_Pan.Y = cursor.y - under.Y * m_Zoom;
			// Latches the view as the author's own, so the panel stops auto-framing the
			// graph on a later open (see HasBeenPanned in the header).
			m_Panned = true;
		}

		// Pan with middle-drag, or space-drag, or right-drag on empty space.
		// Middle-drag pans. canvasActive additionally requires the press to have
		// started on the canvas, so a middle-drag begun in a neighbouring panel does
		// not move this graph.
		if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
		{
			// Delta-based rather than cursor-difference based, so panning behaves the same
			// at any zoom and needs no anchor. It keeps applying after the cursor leaves
			// the canvas, which is what makes a long pan possible.
			m_Pan.X += ImGui::GetIO().MouseDelta.x;
			m_Pan.Y += ImGui::GetIO().MouseDelta.y;
			m_Panned = true;
		}

		// The mouse in graph space. Every hit test below works in this space, which is
		// why the zoom has to divide out first.
		const Point mouseGraph =
			// Cursor minus the canvas corner, then the inverse view transform. DEFECT: the
			// origin is the post-button cursor (see above), so this subtracts a canvas
			// height that shifts every hit test below by one canvas downward.
			ToGraph(FromImVec(Sub(ImGui::GetMousePos(), origin)));

		// Start a wire drag from a pin, or grab a node.
		// One left click starts EITHER a wire drag or a node drag, never both. Order
		// matters: pins are tested first, because a pin dot sits on the node border and
		// is therefore inside the node's own hit box too.
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canvasHovered)
		{
			std::string pinNode;
			std::string pinId;
			// Which side the drag began on. It is normalised to out -> in on release, so
			// the panel only ever sees one ordering.
			bool pinIsInput = false;
			// The radius is converted into graph space, so the grab target is the same size
			// on screen at any zoom. Passing kPinRadius unscaled would make pins
			// effectively unclickable when zoomed out.
			if (FindPinAt(graph, registry, mouseGraph, kPinRadius / m_Zoom,
						   pinNode, pinId, pinIsInput))
			{
				// The drag is held as ids, never node pointers, because any edit can
				// reallocate the graph's storage while the drag is live.
				m_DraggingWire = true;
				m_WireFromNode = pinNode;
				m_WireFromPin = pinId;
				m_WireFromIsInput = pinIsInput;
				// Seeded with the click position, so the first frame of the drag has a
				// non-degenerate length and the preview line is visible immediately.
				m_WireCursor = mouseGraph;
			}
			// No pin under the cursor: this is either a node grab or a click on empty space.
			else
			{
				// Hit-test nodes top-down so the visually frontmost wins.
				// Reverse iteration, so the LAST node in the list wins an overlap. That is
				// correct because DrawNode below iterates forward, drawing the last node last
				// and therefore on top.
				for (auto it = graph.Nodes.rbegin(); it != graph.Nodes.rend(); ++it)
				{
					// The node's current graph-space left edge, read once for the box test below
					// and reused for the grab offset.
					const float nx = it->X;
					const float ny = it->Y;
					// An axis-aligned box test in graph space, so it is zoom-independent. Note
					// this walks the type's pin list again through NodeHeight -- by this point in
					// the frame the same pins have already been walked several times.
					if (mouseGraph.X >= nx && mouseGraph.X <= nx + NodeWidth(it->TypeId) &&
						mouseGraph.Y >= ny && mouseGraph.Y <= ny + NodeHeight(graph, registry, *it))
					{
						// An event is emitted only on an actual CHANGE, so a panel that rebuilds an
						// inspector on every NodeSelected is not rebuilt when the author clicks the
						// node they already have selected.
						if (m_Selected != it->Id)
						{
							m_Selected = it->Id;
							outEvents.NodeSelected = true;
							// The id travels with the flag so the panel does not need a second accessor.
							outEvents.SelectedNodeId = m_Selected;
						}
						// Set even when the node was already selected, so a click-drag on an
						// already-selected node moves it instead of doing nothing.
						m_DraggingNode = true;
						m_DragNodeId = it->Id;
						// Where inside the box the grab happened. Without it the node would snap its
						// top-left corner to the cursor on the first dragged frame.
						m_DragGrabOffset = Point{mouseGraph.X - nx,
												mouseGraph.Y - ny};
						break;
					}
				}
				// The click hit neither a pin nor a node body: a click on empty canvas.
				if (!m_DraggingNode)
				{
					// Cleared by the canvas rather than by the panel, because a CanvasClicked
					// event with an empty id IS the instruction to clear.
					m_Selected.clear();
					outEvents.NodeSelected = true;
					outEvents.SelectedNodeId.clear();
					// Distinct from NodeSelected: a panel uses it to close a context menu or drop
					// an in-flight edit.
					outEvents.CanvasClicked = true;
				}
			}
		}

		if (m_DraggingWire)
		{
			// Held in GRAPH space and converted to screen only in the draw calls, so the
			// preview follows correctly at any zoom without re-solving the transform.
			m_WireCursor = mouseGraph;
			// The RELEASE resolves the link, not the click: a wire exists only once the
			// author has dropped it on a valid pin.
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				std::string pinNode;
				std::string pinId;
				bool pinIsInput = false;
				if (FindPinAt(graph, registry, mouseGraph, kPinRadius / m_Zoom,
							   pinNode, pinId, pinIsInput) &&
					// Two rejections: a node cannot wire to itself, and the drop must be on the
					// opposite side (an input for an output and vice versa). No type check happens
					// here -- whether the two pins are compatible is the panel's and validator's
					// decision.
					pinNode != m_WireFromNode && pinIsInput != m_WireFromIsInput)
				{
					// Normalised so the panel always receives out -> in, no
					// matter which end the author started from. Dragging from
					// the input pin backwards is natural, and the graph has one
					// representation of a wire.
					// Dragged backwards, from the input: swap the ends so the panel always
					// receives out -> in no matter which side the author started from.
					if (m_WireFromIsInput)
					{
						// The pin the drag STARTED on becomes the link's FROM, because the author
						// started from an input and this is the output they dropped on.
						outEvents.LinkFromNode = pinNode;
						outEvents.LinkFromPin = pinId;
						outEvents.LinkToNode = m_WireFromNode;
						outEvents.LinkToPin = m_WireFromPin;
					}
					else
					{
						outEvents.LinkFromNode = m_WireFromNode;
						outEvents.LinkFromPin = m_WireFromPin;
						outEvents.LinkToNode = pinNode;
						outEvents.LinkToPin = pinId;
					}
					// Reported, never applied: the canvas owns no graph mutation for links.
					outEvents.WireAdded = true;
				}
				// Ended whether or not the drop was valid, so a drag released over empty
				// canvas does not stick to the cursor.
				m_DraggingWire = false;
			}
		}

		if (m_DraggingNode)
		{
			// Re-resolved by id every frame, for the same reason the wire anchors are.
			GraphNode *dragged = graph.FindNode(m_DragNodeId);
			// Move only while the button is still down. DEFECT: this writes straight into
			// the graph, so it is the one place the canvas mutates it -- while the header
			// describes the canvas as one that never mutates the graph.
			if (dragged != nullptr && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				// The grab offset is subtracted so the node keeps the point that was grabbed.
				// No grid snapping: positions are free-form graph-space pixels that are saved.
				dragged->X = mouseGraph.X - m_DragGrabOffset.X;
				dragged->Y = mouseGraph.Y - m_DragGrabOffset.Y;
			}
			else
			{
				// Cleared on release AND when the node has vanished, so deleting a node
				// mid-drag cannot leave the canvas stuck in drag state.
				m_DraggingNode = false;
				if (dragged != nullptr)
				{
					// Emitted once, on release, not per frame -- so a panel can push a single
					// undo entry per drag instead of one per frame.
					outEvents.NodeMoved = true;
					outEvents.MovedNodeId = dragged->Id;
				}
			}
		}

		// Delete the selection. Only on Delete, and only when a node is
		// selected, so an idle canvas never reacts to a stray key.
		// Delete removes the selection, gated on hover and on there being one. DEFECT:
		// nothing checks whether a widget drawn by drawBody has keyboard focus, and
		// drawBody's return value is ignored (see DrawNode), so pressing Delete while
		// editing a literal inside a node also deletes that node.
		if (canvasHovered && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
			!m_Selected.empty())
		{
			// A REQUEST. The canvas does not remove the node, so the panel is free to
			// veto it or route it into its undo stack.
			outEvents.NodeDeleted = true;
			// DEFECT: cleared immediately, before the panel has seen the request. If the
			// panel declines the deletion, the selection has already been lost.
			m_Selected.clear();
		}

		// Wires after the interaction block, so the preview reflects this frame's
		// cursor position, and before the nodes, so wires pass underneath them.
		DrawWires(graph, registry, originPoint, Point{avail.x, avail.y});

		// Forward order, so the last node in the list is drawn last and sits on top --
		// the assumption the reverse-order hit test above relies on.
		for (GraphNode &node : graph.Nodes)
		{
			// A per-node snapshot of identity and position. DEFECT: the map it is stored in
			// is written here and cleared at the top of every frame, and nothing anywhere
			// reads m_Visuals -- it has no accessor in the header and no reader in this
			// file. The whole map is pure overhead.
			Visual visual;
			visual.Id = node.Id;
			visual.TypeId = node.TypeId;
			visual.X = node.X;
			visual.Y = node.Y;
			// Stored, then immediately passed to DrawNode by reference. operator[] would
			// default-insert a blank Visual for a missing id, so this assignment is what
			// guarantees DrawNode sees real values.
			m_Visuals[node.Id] = visual;
			// The map entry is passed as a mutable reference because DrawNode's signature
			// takes Visual&, even though it only reads. That costs a second operator[]
			// lookup per node; the local `visual` above would do.
			DrawNode(graph, registry, m_Visuals[node.Id], outEvents, drawNodeBody);
		}

		// The wire being dragged draws last so it is not hidden under a node.
		// DEFECT: the in-progress wire is drawn a second time here, and DrawWires
		// already drew it. The two passes use the same colour and thickness, so the
		// visible effect is one slightly more opaque line drawn over the node boxes,
		// not two different things.
		if (m_DraggingWire)
		{
			ImDrawList *draw = ImGui::GetWindowDrawList();
			const GraphNode *from = graph.FindNode(m_WireFromNode);
			if (from != nullptr)
			{
				Point anchor = m_WireFromIsInput
								   ? InputPinPos(graph, registry, *from, m_WireFromPin)
								   : OutputPinPos(graph, registry, *from, m_WireFromPin);
				draw->AddLine(ToImVec(ToScreen(anchor)),
							  ToImVec(ToScreen(m_WireCursor)),
							  IM_COL32(240, 200, 80, 220), 2.0f * m_Zoom);
			}
		}
	}

// End of namespace Wasteland.
} // namespace Wasteland