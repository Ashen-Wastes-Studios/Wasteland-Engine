#include "../../DemonCore-Editor/src/Panels/NodeCanvas.h"

#include <iostream>
#include <string>
#include <vector>

int main()
{
	using Canvas = Wasteland::NodeCanvas;
	std::vector<Canvas::Node> nodes;

	Canvas::Node first;
	first.Id = "node-9417";
	first.Label = "First";
	first.Position = Wasteland::Point{0.0f, 0.0f};
	first.Pins = {
		Canvas::Pin{"input-id", "Input", Canvas::PinDirection::Input, Canvas::PinStyle::Other},
		Canvas::Pin{"output-id", "Output", Canvas::PinDirection::Output, Canvas::PinStyle::Number},
		Canvas::Pin{"exec-id", "Flow", Canvas::PinDirection::Output, Canvas::PinStyle::Flow},
	};
	nodes.push_back(first);

	Canvas::Node overlapping;
	overlapping.Id = "node-2083";
	overlapping.Label = "On top";
	overlapping.Position = Wasteland::Point{24.0f, 15.0f};
	overlapping.Selected = false;
	overlapping.Pins = {
		Canvas::Pin{"flow-in", "Flow in", Canvas::PinDirection::Input, Canvas::PinStyle::Flow},
		Canvas::Pin{"flow-out", "Flow out", Canvas::PinDirection::Output, Canvas::PinStyle::Flow},
		Canvas::Pin{"flow-next", "Next", Canvas::PinDirection::Output, Canvas::PinStyle::Flow},
	};
	nodes.push_back(overlapping);

	const Wasteland::Point pan{100.0f, 40.0f};
	constexpr float zoom = 2.0f;
	const Wasteland::Point canvasOrigin{400.0f, 300.0f};
	const auto hitAtGraphPoint = [&](float x, float y) {
		return Canvas::HitTestNodeId(nodes,
			Wasteland::Point{x * zoom + pan.X, y * zoom + pan.Y}, pan, zoom);
	};
	const auto hitAtScreenPoint = [&](float x, float y) {
		return Canvas::HitTestNodeId(nodes,
			Wasteland::Point{canvasOrigin.X + pan.X + x * zoom,
							 canvasOrigin.Y + pan.Y + y * zoom},
			canvasOrigin, pan, zoom);
	};

	auto checkHit = [](const std::string &actual, const std::string &expected,
					   const char *caseName) {
		if (actual == expected)
			return true;
		std::cerr << caseName << ": expected '" << expected << "', got '" << actual << "'\n";
		return false;
	};

	// Screen coordinates include the canvas origin as well as pan and zoom.
	if (!checkHit(hitAtScreenPoint(30.0f, 20.0f), "node-2083", "nonzero canvas origin"))
		return 1;

	// A screen point in the overlap resolves to the topmost stable ID, not the vector offset.
	if (!checkHit(hitAtGraphPoint(30.0f, 20.0f), "node-2083", "overlap/topmost"))
		return 1;
	// Outside the front node's left edge, the point resolves to the back node.
	if (!checkHit(hitAtGraphPoint(5.0f, 60.0f), "node-9417", "back node"))
		return 1;
	// Flow pins do not add body rows; the lower edge is derived from data pins only.
	if (!checkHit(hitAtGraphPoint(30.0f, 65.0f), "node-9417", "flow pins do not add rows"))
		return 1;
	// Bounds are inclusive, matching the live canvas' box hit test.
	if (!checkHit(hitAtGraphPoint(180.0f, 66.0f), "node-9417", "inclusive boundary"))
		return 1;
	if (!checkHit(hitAtGraphPoint(181.0f, 67.0f), "", "outside bounds"))
		return 1;

	std::string selectedNodeId = "node-2083";
	const std::string deleteNodeId = Canvas::TakeSelectedNodeIdForDelete(selectedNodeId);
	if (!checkHit(deleteNodeId, "node-2083", "delete selected stable ID"))
		return 1;
	if (!checkHit(selectedNodeId, "", "delete clears selection"))
		return 1;

	std::string emptySelection;
	const std::string noDeleteNodeId = Canvas::TakeSelectedNodeIdForDelete(emptySelection);
	if (!checkHit(noDeleteNodeId, "", "empty selection emits no delete request"))
		return 1;
	if (!checkHit(emptySelection, "", "empty selection stays empty"))
		return 1;
	return 0;
}
