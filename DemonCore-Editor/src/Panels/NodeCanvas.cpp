#include "wlpch.h"
#include "NodeCanvas.h"

#include <algorithm>
#include <cmath>
#include <imgui/imgui.h>

namespace Wasteland
{
namespace
{
	constexpr float kNodePadding = 8.0f;
	constexpr float kPinRadius = 7.0f;
	constexpr float kMinZoom = 0.75f;

	ImVec2 ToImVec(const Point &point)
	{
		return ImVec2(point.X, point.Y);
	}

	Point FromImVec(const ImVec2 &point)
	{
		return Point{point.x, point.y};
	}

	const NodeCanvas::Pin *FindPin(const NodeCanvas::Node &node,
								   const std::string &pinId)
	{
		const NodeCanvas::Pin *result = nullptr;
		for (const NodeCanvas::Pin &pin : node.Pins)
			if (pin.Id == pinId)
				result = &pin;
		return result;
	}

	ImVec4 PinColour(NodeCanvas::PinStyle style)
	{
		switch (style)
		{
		case NodeCanvas::PinStyle::Flow:    return ImVec4(0.90f, 0.90f, 0.85f, 1.00f);
		case NodeCanvas::PinStyle::Boolean: return ImVec4(0.95f, 0.45f, 0.45f, 1.00f);
		case NodeCanvas::PinStyle::Number:  return ImVec4(0.45f, 0.80f, 0.95f, 1.00f);
		case NodeCanvas::PinStyle::Text:    return ImVec4(0.55f, 0.90f, 0.55f, 1.00f);
		case NodeCanvas::PinStyle::Entity:  return ImVec4(0.85f, 0.70f, 0.95f, 1.00f);
		default:                            return ImVec4(0.75f, 0.75f, 0.75f, 1.00f);
		}
	}

	void AddBezier(ImDrawList *draw, const Point &from, const Point &to,
				   const ImVec4 &colour, float thickness)
	{
		const float dx = std::max(40.0f, std::fabs(to.X - from.X) * 0.5f);
		draw->AddBezierCubic(ToImVec(from), ToImVec(Point{from.X + dx, from.Y}),
			ToImVec(Point{to.X - dx, to.Y}), ToImVec(to),
			ImGui::ColorConvertFloat4ToU32(colour), thickness);
	}
}

Point NodeCanvas::ToScreen(const Point &graphPos) const
{
	return Point{m_CanvasOrigin.X + graphPos.X * m_Zoom + m_Pan.X,
				 m_CanvasOrigin.Y + graphPos.Y * m_Zoom + m_Pan.Y};
}

Point NodeCanvas::ToGraph(const Point &screenPos) const
{
	const float zoom = m_Zoom > 0.01f ? m_Zoom : 1.0f;
	return Point{(screenPos.X - m_CanvasOrigin.X - m_Pan.X) / zoom,
				 (screenPos.Y - m_CanvasOrigin.Y - m_Pan.Y) / zoom};
}

float NodeCanvas::NodeHeight(const Node &node) const
{
	std::size_t rows = 0;
	for (const Pin &pin : node.Pins)
		if (pin.Style != PinStyle::Flow)
			++rows;
	if (rows == 0)
		rows = 1;
	return HeaderHeight + PinRowHeight * static_cast<float>(rows);
}

std::vector<std::pair<std::string, Point>> NodeCanvas::InputPinPositions(
	const Node &node) const
{
	auto cached = m_InPins.find(node.Id);
	if (cached != m_InPins.end())
		return cached->second;

	std::vector<std::pair<std::string, Point>> positions;
	float y = node.Position.Y + HeaderHeight + PinRowHeight * 0.5f;
	for (const Pin &pin : node.Pins)
	{
		if (pin.Direction != PinDirection::Input || pin.Style == PinStyle::Flow)
			continue;
		positions.push_back({pin.Id, Point{node.Position.X, y}});
		y += PinRowHeight;
	}
	m_InPins[node.Id] = positions;
	return positions;
}

std::vector<std::pair<std::string, Point>> NodeCanvas::OutputPinPositions(
	const Node &node) const
{
	auto cached = m_OutPins.find(node.Id);
	if (cached != m_OutPins.end())
		return cached->second;

	std::vector<std::pair<std::string, Point>> positions;
	float y = node.Position.Y + HeaderHeight + PinRowHeight * 0.5f;
	for (const Pin &pin : node.Pins)
	{
		if (pin.Direction != PinDirection::Output || pin.Style == PinStyle::Flow)
			continue;
		positions.push_back({pin.Id, Point{node.Position.X + NodeWidth, y}});
		y += PinRowHeight;
	}
	m_OutPins[node.Id] = positions;
	return positions;
}

Point NodeCanvas::InputPinPos(const Node &node, const std::string &pinId) const
{
	for (const auto &pin : InputPinPositions(node))
		if (pin.first == pinId)
			return pin.second;
	return Point{node.Position.X, node.Position.Y + HeaderHeight};
}

Point NodeCanvas::OutputPinPos(const Node &node, const std::string &pinId) const
{
	for (const auto &pin : OutputPinPositions(node))
		if (pin.first == pinId)
			return pin.second;
	return Point{node.Position.X + NodeWidth, node.Position.Y + HeaderHeight};
}

bool NodeCanvas::FindPinAt(const Presentation &presentation, const Point &graphPos,
					   float radius, std::string &outNodeId,
					   std::string &outPinId, bool &outIsInput) const
{
	bool found = false;
	float bestSq = radius * radius;
	for (const Node &node : presentation.Nodes)
	{
		for (const auto &pin : InputPinPositions(node))
		{
			const float dx = pin.second.X - graphPos.X;
			const float dy = pin.second.Y - graphPos.Y;
			const float distanceSq = dx * dx + dy * dy;
			if (distanceSq < bestSq)
			{
				bestSq = distanceSq;
				outNodeId = node.Id;
				outPinId = pin.first;
				outIsInput = true;
				found = true;
			}
		}
		for (const auto &pin : OutputPinPositions(node))
		{
			const float dx = pin.second.X - graphPos.X;
			const float dy = pin.second.Y - graphPos.Y;
			const float distanceSq = dx * dx + dy * dy;
			if (distanceSq < bestSq)
			{
				bestSq = distanceSq;
				outNodeId = node.Id;
				outPinId = pin.first;
				outIsInput = false;
				found = true;
			}
		}
	}
	return found;
}

void NodeCanvas::Select(const std::string &nodeId)
{
	m_Selected = nodeId;
}

void NodeCanvas::FrameAll(const Presentation &presentation)
{
	if (presentation.Nodes.empty())
		return;

	float minX = presentation.Nodes[0].Position.X;
	float minY = presentation.Nodes[0].Position.Y;
	float maxX = presentation.Nodes[0].Position.X + NodeWidth;
	float maxY = presentation.Nodes[0].Position.Y + NodeHeight(presentation.Nodes[0]);
	for (const Node &node : presentation.Nodes)
	{
		minX = std::min(minX, node.Position.X);
		minY = std::min(minY, node.Position.Y);
		maxX = std::max(maxX, node.Position.X + NodeWidth);
		maxY = std::max(maxY, node.Position.Y + NodeHeight(node));
	}

	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if (avail.x < 10.0f || avail.y < 10.0f)
		return;

	const float pad = 40.0f;
	const float spanX = std::max(1.0f, maxX - minX);
	const float spanY = std::max(1.0f, maxY - minY);
	m_Zoom = std::min((avail.x - pad * 2.0f) / spanX,
					  (avail.y - pad * 2.0f) / spanY);
	m_Zoom = std::clamp(m_Zoom, kMinZoom, 1.5f);
	m_Pan.X = pad - minX * m_Zoom + (avail.x - pad * 2.0f - spanX * m_Zoom) * 0.5f;
	m_Pan.Y = pad - minY * m_Zoom + (avail.y - pad * 2.0f - spanY * m_Zoom) * 0.5f;
}

void NodeCanvas::DrawGrid(const Point &origin, const Point &size)
{
	ImDrawList *draw = ImGui::GetWindowDrawList();
	const float spacing = 32.0f * m_Zoom;
	if (spacing < 8.0f)
		return;

	const ImU32 line = IM_COL32(255, 255, 255, 18);
	const float right = origin.X + size.X;
	const float bottom = origin.Y + size.Y;
	for (float x = origin.X + std::fmod(m_Pan.X, spacing); x < right; x += spacing)
		draw->AddLine(ImVec2(x, origin.Y), ImVec2(x, bottom), line);
	for (float y = origin.Y + std::fmod(m_Pan.Y, spacing); y < bottom; y += spacing)
		draw->AddLine(ImVec2(origin.X, y), ImVec2(right, y), line);
}

void NodeCanvas::DrawWires(const Presentation &presentation,
						   const Point &origin, const Point &size)
{
	ImDrawList *draw = ImGui::GetWindowDrawList();
	for (const Node &from : presentation.Nodes)
	{
		for (const Wire &wire : from.WiredTo)
		{
			const Node *to = presentation.FindNode(wire.ToNodeId);
			if (to == nullptr || !from.TypeKnown || !to->TypeKnown)
				continue;

			bool isFlow = false;
			ImVec4 colour(0.8f, 0.8f, 0.8f, 1.0f);
			if (const Pin *pin = FindPin(from, wire.FromPinId))
			{
				isFlow = pin->Style == PinStyle::Flow;
				colour = PinColour(pin->Style);
			}

			Point fromPos;
			Point toPos;
			if (isFlow)
			{
				fromPos = Point{from.Position.X + NodeWidth, from.Position.Y + HeaderHeight * 0.5f};
				toPos = Point{to->Position.X, to->Position.Y + HeaderHeight * 0.5f};
			}
			else
			{
				fromPos = OutputPinPos(from, wire.FromPinId);
				toPos = InputPinPos(*to, wire.ToPinId);
			}

			AddBezier(draw, ToScreen(fromPos), ToScreen(toPos), colour,
					  isFlow ? 2.5f * m_Zoom : 1.5f * m_Zoom);
		}
	}

	if (m_DraggingWire)
	{
		const Node *from = presentation.FindNode(m_WireFromNode);
		if (from != nullptr)
		{
			const Point anchor = m_WireFromIsInput
				? InputPinPos(*from, m_WireFromPin)
				: OutputPinPos(*from, m_WireFromPin);
			draw->AddLine(ToImVec(ToScreen(anchor)), ToImVec(ToScreen(m_WireCursor)),
					  IM_COL32(240, 200, 80, 200), 2.0f * m_Zoom);
		}
	}
	(void)origin;
	(void)size;
}

void NodeCanvas::DrawNode(const Node &node,
					  const std::function<bool(const Node &)> &drawBody)
{
	ImDrawList *draw = ImGui::GetWindowDrawList();
	const Point topLeft = ToScreen(node.Position);
	const float width = NodeWidth * m_Zoom;
	const float height = NodeHeight(node) * m_Zoom;
	const ImVec2 tl = ToImVec(topLeft);
	const ImVec2 br(tl.x + width, tl.y + height);
	const bool selected = node.Selected;
	const ImU32 border = selected ? IM_COL32(240, 200, 80, 255)
		: (node.TypeKnown ? IM_COL32(120, 120, 130, 255)
							  : IM_COL32(220, 90, 90, 255));

	draw->AddRectFilled(tl, br, IM_COL32(32, 32, 38, 240));
	draw->AddRect(tl, br, border, selected ? 2.0f : 1.0f);
	draw->AddRectFilled(tl, ImVec2(br.x, tl.y + HeaderHeight * m_Zoom),
		selected ? IM_COL32(70, 62, 36, 255) : IM_COL32(46, 46, 54, 255));

	const ImU32 titleColour = selected ? IM_COL32(250, 220, 140, 255)
									 : IM_COL32(220, 220, 230, 255);
	draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
		ImVec2(tl.x + kNodePadding, tl.y + 5.0f), titleColour, node.Label.c_str());

	float rowY = tl.y + (HeaderHeight + PinRowHeight * 0.5f) * m_Zoom;
	for (const Pin &pin : node.Pins)
	{
		if (pin.Style == PinStyle::Flow)
			continue;
		const ImVec4 colour = PinColour(pin.Style);
		const ImU32 packed = IM_COL32(colour.x * 255, colour.y * 255,
										  colour.z * 255, 255);
		const float radius = kPinRadius * m_Zoom;
		if (pin.Direction == PinDirection::Input)
			draw->AddCircleFilled(ImVec2(tl.x, rowY), radius, packed);
		else
			draw->AddCircleFilled(ImVec2(br.x, rowY), radius, packed);
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
			ImVec2(tl.x + kNodePadding, rowY - 6.0f),
			IM_COL32(200, 200, 210, 255), pin.Label.c_str());
		rowY += PinRowHeight * m_Zoom;
	}

	if (drawBody && !m_DraggingNode)
		(void)drawBody(node);
}

void NodeCanvas::Render(Presentation &presentation, Events &outEvents,
						std::function<bool(const Node &)> drawNodeBody)
{
	outEvents = Events{};
	m_InPins.clear();
	m_OutPins.clear();

	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::InvisibleButton("##vbcanvas", ImVec2(std::max(avail.x, 50.0f),
										std::max(avail.y, 50.0f)),
		ImGuiButtonFlags_MouseButtonLeft |
		ImGuiButtonFlags_MouseButtonRight |
		ImGuiButtonFlags_MouseButtonMiddle);
	const bool canvasActive = ImGui::IsItemActive();
	const bool canvasHovered = ImGui::IsItemHovered();
	const ImVec2 origin = ImGui::GetItemRectMin();
	const Point originPoint{origin.x, origin.y};
	m_CanvasOrigin = originPoint;
	DrawGrid(originPoint, Point{avail.x, avail.y});

	if (canvasHovered && ImGui::GetIO().MouseWheel != 0.0f)
	{
		const ImVec2 cursor = ImGui::GetMousePos();
		const float oldZoom = m_Zoom;
		m_Zoom = std::clamp(oldZoom *
			(ImGui::GetIO().MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f),
			kMinZoom, 2.0f);
		const Point under{(cursor.x - origin.x - m_Pan.X) / (oldZoom > 0.01f ? oldZoom : 1.0f),
					  (cursor.y - origin.y - m_Pan.Y) / (oldZoom > 0.01f ? oldZoom : 1.0f)};
		m_Pan.X = cursor.x - origin.x - under.X * m_Zoom;
		m_Pan.Y = cursor.y - origin.y - under.Y * m_Zoom;
		m_Panned = true;
	}

	if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		m_Pan.X += ImGui::GetIO().MouseDelta.x;
		m_Pan.Y += ImGui::GetIO().MouseDelta.y;
		m_Panned = true;
	}

	const Point mouseScreen = FromImVec(ImGui::GetMousePos());
	const Point mouseGraph = ToGraph(mouseScreen);
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canvasHovered)
	{
		std::string pinNode;
		std::string pinId;
		bool pinIsInput = false;
		if (FindPinAt(presentation, mouseGraph, kPinRadius / m_Zoom,
					  pinNode, pinId, pinIsInput))
		{
			m_DraggingWire = true;
			m_WireFromNode = pinNode;
			m_WireFromPin = pinId;
			m_WireFromIsInput = pinIsInput;
			m_WireCursor = mouseGraph;
		}
		else
		{
			const std::string hitId = HitTestNodeId(
				presentation.Nodes, mouseScreen, originPoint, m_Pan, m_Zoom);
			Node *hit = presentation.FindNode(hitId);
			if (hit != nullptr)
			{
				if (m_Selected != hit->Id)
				{
					m_Selected = hit->Id;
					outEvents.NodeSelected = true;
					outEvents.SelectedNodeId = m_Selected;
				}
				m_DraggingNode = true;
				m_DragNodeId = hit->Id;
				m_DragGrabOffset = Point{mouseGraph.X - hit->Position.X,
										mouseGraph.Y - hit->Position.Y};
			}
			else if (!m_DraggingNode)
			{
				m_Selected.clear();
				outEvents.NodeSelected = true;
				outEvents.SelectedNodeId.clear();
				outEvents.CanvasClicked = true;
			}
		}
	}

	if (m_DraggingWire)
	{
		m_WireCursor = mouseGraph;
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			std::string pinNode;
			std::string pinId;
			bool pinIsInput = false;
			if (FindPinAt(presentation, mouseGraph, kPinRadius / m_Zoom,
						  pinNode, pinId, pinIsInput) &&
				pinNode != m_WireFromNode && pinIsInput != m_WireFromIsInput)
			{
				if (m_WireFromIsInput)
				{
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
				outEvents.WireAdded = true;
			}
			m_DraggingWire = false;
		}
	}

	if (m_DraggingNode)
	{
		Node *dragged = presentation.FindNode(m_DragNodeId);
		if (dragged != nullptr)
		{
			dragged->Position = Point{mouseGraph.X - m_DragGrabOffset.X,
									 mouseGraph.Y - m_DragGrabOffset.Y};
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				m_DraggingNode = false;
				outEvents.NodeMoved = true;
				outEvents.MovedNodeId = dragged->Id;
				outEvents.MovedNodePosition = dragged->Position;
			}
		}
		else
		{
			m_DraggingNode = false;
		}
	}

	if (canvasHovered && ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_Selected.empty())
	{
		outEvents.SelectedNodeId = TakeSelectedNodeIdForDelete(m_Selected);
		outEvents.NodeDeleted = !outEvents.SelectedNodeId.empty();
	}

	for (Node &node : presentation.Nodes)
		node.Selected = node.Id == m_Selected;

	DrawWires(presentation, originPoint, Point{avail.x, avail.y});
	for (const Node &node : presentation.Nodes)
		DrawNode(node, drawNodeBody);

	if (m_DraggingWire)
	{
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const Node *from = presentation.FindNode(m_WireFromNode);
		if (from != nullptr)
		{
			const Point anchor = m_WireFromIsInput
				? InputPinPos(*from, m_WireFromPin)
				: OutputPinPos(*from, m_WireFromPin);
			draw->AddLine(ToImVec(ToScreen(anchor)), ToImVec(ToScreen(m_WireCursor)),
					  IM_COL32(240, 200, 80, 220), 2.0f * m_Zoom);
		}
	}
}
}
