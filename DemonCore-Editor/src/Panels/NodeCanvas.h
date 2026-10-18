#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Wasteland
{
	struct Point
	{
		float X = 0.0f;
		float Y = 0.0f;
	};

	// Canvas-only presentation types. They describe how a graph is drawn, not
	// how any particular graph domain stores or interprets it.
	struct NodeCanvas
	{
		static constexpr float NodeWidth = 180.0f;
		static constexpr float HeaderHeight = 26.0f;
		static constexpr float PinRowHeight = 20.0f;

		enum class PinDirection
		{
			Input,
			Output
		};

		enum class PinStyle
		{
			Flow,
			Boolean,
			Number,
			Text,
			Entity,
			Other
		};

		struct Pin
		{
			std::string Id;
			std::string Label;
			PinDirection Direction = PinDirection::Input;
			PinStyle Style = PinStyle::Other;
		};

		// A wire endpoint is relative to its source node. All identities are
		// stable strings, so vector reallocation cannot invalidate connections.
		struct Wire
		{
			std::string FromPinId;
			std::string ToNodeId;
			std::string ToPinId;
		};

		struct Node
		{
			std::string Id;
			std::string Label;
			Point Position;
			bool Selected = false;
			bool TypeKnown = true;
			std::vector<Pin> Pins;
			std::vector<Wire> WiredTo;
		};

		struct Presentation
		{
			std::vector<Node> Nodes;

			Node *FindNode(const std::string &id)
			{
				for (Node &node : Nodes)
					if (node.Id == id)
						return &node;
				return nullptr;
			}

			const Node *FindNode(const std::string &id) const
			{
				for (const Node &node : Nodes)
					if (node.Id == id)
						return &node;
				return nullptr;
			}
		};

		// Pure graph-space geometry shared by the live canvas and the headless
		// regression. Reverse order matches drawing order: the last node is on top.
		static std::string HitTestNodeId(const std::vector<Node> &nodes,
										  const Point &position)
		{
				for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
				{
					std::size_t rows = 0;
					for (const Pin &pin : it->Pins)
						if (pin.Style != PinStyle::Flow)
							++rows;
					if (rows == 0)
						rows = 1;

					const float height = HeaderHeight + PinRowHeight * static_cast<float>(rows);
					if (position.X >= it->Position.X &&
						position.X <= it->Position.X + NodeWidth &&
						position.Y >= it->Position.Y &&
						position.Y <= it->Position.Y + height)
						return it->Id;
				}
			return std::string();
		}

		// Screen-space adapter for input picking. The five-argument form includes the
		// canvas origin; the four-argument form preserves callers whose point is local.
		static std::string HitTestNodeId(const std::vector<Node> &nodes,
										  const Point &screenPosition,
										  const Point &pan, float zoom)
		{
				return HitTestNodeId(nodes, screenPosition, Point{}, pan, zoom);
		}

		static std::string HitTestNodeId(const std::vector<Node> &nodes,
										  const Point &screenPosition,
										  const Point &canvasOrigin,
										  const Point &pan, float zoom)
		{
				const float safeZoom = zoom > 0.01f ? zoom : 1.0f;
				const Point localPosition{screenPosition.X - canvasOrigin.X,
										  screenPosition.Y - canvasOrigin.Y};
				const Point graphPosition{(localPosition.X - pan.X) / safeZoom,
										  (localPosition.Y - pan.Y) / safeZoom};
				return HitTestNodeId(nodes, graphPosition);
		}

		struct Events
		{
			bool NodeMoved = false;
			bool NodeSelected = false;
			bool WireAdded = false;
			bool NodeDeleted = false;
			bool CanvasClicked = false;
			std::string MovedNodeId;
			Point MovedNodePosition;
			std::string SelectedNodeId;
			std::string LinkFromNode;
			std::string LinkFromPin;
			std::string LinkToNode;
			std::string LinkToPin;
		};

		static std::string TakeSelectedNodeIdForDelete(std::string &selectedNodeId)
		{
			std::string deletedNodeId;
			deletedNodeId.swap(selectedNodeId);
			return deletedNodeId;
		}

		void Render(Presentation &presentation, Events &outEvents,
					std::function<bool(const Node &)> drawNodeBody = {});

		void Select(const std::string &nodeId);
		const std::string &GetSelected() const { return m_Selected; }
		bool HasSelection() const { return !m_Selected.empty(); }
		bool HasBeenPanned() const { return m_Panned; }
		void FrameAll(const Presentation &presentation);

	private:
		Point ToScreen(const Point &graphPos) const;
		Point ToGraph(const Point &screenPos) const;
		float NodeHeight(const Node &node) const;
		void DrawGrid(const Point &origin, const Point &size);
		void DrawNode(const Node &node,
					  const std::function<bool(const Node &)> &drawBody);
		void DrawWires(const Presentation &presentation,
					   const Point &origin, const Point &size);
		std::vector<std::pair<std::string, Point>> InputPinPositions(const Node &node) const;
		std::vector<std::pair<std::string, Point>> OutputPinPositions(const Node &node) const;
		Point InputPinPos(const Node &node, const std::string &pinId) const;
		Point OutputPinPos(const Node &node, const std::string &pinId) const;
		bool FindPinAt(const Presentation &presentation, const Point &graphPos,
					float radius, std::string &outNodeId,
					std::string &outPinId, bool &outIsInput) const;

		Point m_Pan;
		Point m_CanvasOrigin;
		float m_Zoom = 1.0f;
		bool m_Panned = false;

		std::string m_Selected;
		bool m_DraggingNode = false;
		std::string m_DragNodeId;
		Point m_DragGrabOffset;

		bool m_DraggingWire = false;
		std::string m_WireFromNode;
		std::string m_WireFromPin;
		bool m_WireFromIsInput = false;
		Point m_WireCursor;

		mutable std::unordered_map<std::string,
			std::vector<std::pair<std::string, Point>>> m_InPins;
		mutable std::unordered_map<std::string,
			std::vector<std::pair<std::string, Point>>> m_OutPins;
	};
}