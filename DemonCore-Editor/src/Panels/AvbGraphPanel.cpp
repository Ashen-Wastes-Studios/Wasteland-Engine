#include "wlpch.h"
#include "AvbGraphPanel.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

#include "Wasteland/Avb/Graph/GraphSerializer.h"

namespace Wasteland
{
	namespace
	{
		using namespace Avb;

		std::string FormatLiteral(const AvbLiteral& value)
		{
			if (const auto* number = std::get_if<double>(&value.Value))
			{
				std::ostringstream out;
				out << std::setprecision(17) << *number;
				return out.str();
			}
			if (const auto* integer = std::get_if<std::int64_t>(&value.Value))
				return std::to_string(*integer);
			if (const auto* boolean = std::get_if<bool>(&value.Value))
				return *boolean ? "true" : "false";
			if (const auto* text = std::get_if<std::string>(&value.Value))
				return *text;
			return {};
		}

		std::string DefaultText(const AvbPinDef& pin)
		{
			if (pin.DefaultValue)
				return FormatLiteral(*pin.DefaultValue);
			switch (pin.Type)
			{
			case AvbPinType::Time:
			case AvbPinType::Float: return "0.0";
			case AvbPinType::Int: return "0";
			case AvbPinType::Bool: return "false";
			default: return {};
			}
		}

		bool ParseFiniteDouble(const char* text, double& value)
		{
			if (!text || !*text) return false;
			char* end = nullptr;
			errno = 0;
			const double parsed = std::strtod(text, &end);
			if (end == text || errno == ERANGE || !std::isfinite(parsed)) return false;
			while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
			if (*end != '\0') return false;
			value = parsed;
			return true;
		}

		bool ParseInt64(const char* text, std::int64_t& value)
		{
			if (!text || !*text) return false;
			char* end = nullptr;
			errno = 0;
			const long long parsed = std::strtoll(text, &end, 10);
			if (end == text || errno == ERANGE) return false;
			while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
			if (*end != '\0') return false;
			value = static_cast<std::int64_t>(parsed);
			return true;
		}

		NodeCanvas::PinStyle CanvasStyle(AvbPinType type)
		{
			switch (type)
			{
			case AvbPinType::Bool: return NodeCanvas::PinStyle::Boolean;
			case AvbPinType::Time:
			case AvbPinType::Float:
			case AvbPinType::Int: return NodeCanvas::PinStyle::Number;
			case AvbPinType::Enum:
			case AvbPinType::String:
			case AvbPinType::AssetRef: return NodeCanvas::PinStyle::Text;
			// AVB triggers are typed, sample-accurate graph pins, not Visual
			// Blocks exec pins; keep them on the ordinary pin/wire path.
			case AvbPinType::Trigger:
			case AvbPinType::Audio: return NodeCanvas::PinStyle::Other;
			}
			return NodeCanvas::PinStyle::Other;
		}

		int ResizeEditBufferCallback(ImGuiInputTextCallbackData* data)
		{
			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
			{
				auto* buffer = static_cast<std::string*>(data->UserData);
				buffer->resize(data->BufTextLen);
				data->Buf = const_cast<char*>(buffer->c_str());
			}
			return 0;
		}

		bool InputText(const char* label, std::string& buffer)
		{
			return ImGui::InputText(label, const_cast<char*>(buffer.c_str()), buffer.capacity() + 1,
				ImGuiInputTextFlags_CallbackResize, ResizeEditBufferCallback, &buffer);
		}
	}

	std::string& AvbGraphPanel::EditBuffer(const std::string& key, const std::string& seed)
	{
		auto found = m_EditBuffers.find(key);
		if (found == m_EditBuffers.end())
			found = m_EditBuffers.emplace(key, seed).first;
		return found->second;
	}

	bool AvbGraphPanel::OpenGraph(const std::string& path)
	{
		if (m_Dirty)
		{
			m_PendingOpenPath = path;
			m_ConfirmOpen = true;
			m_Status = "Resolve unsaved changes before opening another graph.";
			m_Visible = true;
			return false;
		}
		return LoadGraphFile(path);
	}

	bool AvbGraphPanel::LoadGraphFile(const std::string& path)
	{
		Avb::AvbGraph candidate;
		Avb::AvbGraphError error;
		if (!Avb::AvbGraphSerializer::Load(path, candidate, error))
		{
			m_Status = error.Format(path);
			m_Visible = true;
			return false;
		}

		m_Graph = std::move(candidate);
		m_Path = path;
		m_HasGraph = true;
		m_Dirty = false;
		m_ConfirmOpen = false;
		m_PendingOpenPath.clear();
		m_FramedOnce = false;
		m_EditBuffers.clear();
		m_CompiledOutput.clear();
		m_Canvas.Select(m_Graph.Nodes.empty() ? std::string() : m_Graph.Nodes.front().Id);
		RefreshValidation();
		m_Status = "Loaded";
		m_Visible = true;
		return true;
	}

	void AvbGraphPanel::RefreshValidation()
	{
		if (!m_HasGraph)
		{
			m_Validation.Findings.clear();
			return;
		}
		Avb::AvbValidator::Validate(m_Graph, m_Validation);
	}

	bool AvbGraphPanel::ValidateGraph()
	{
		if (!m_HasGraph)
		{
			m_Status = "No AVB graph is open.";
			return false;
		}
		Avb::AvbCompiledGraph compiled;
		if (!Avb::AvbCompiler::Compile(m_Graph, compiled, m_Validation))
		{
			m_CompiledOutput.clear();
			m_Status = "Validation failed: " + std::to_string(m_Validation.Findings.size()) + " finding(s).";
			return false;
		}
		m_CompiledOutput = std::move(compiled.Output);
		m_Status = "Valid; compiled deterministic AVB graph.";
		return true;
	}

	bool AvbGraphPanel::SaveGraph()
	{
		if (!m_HasGraph || m_Path.empty())
		{
			m_Status = "No graph path to save.";
			return false;
		}
		if (!ValidateGraph())
			return false;

		Avb::AvbGraphError error;
		if (!Avb::AvbGraphSerializer::Save(m_Path, m_Graph, error))
		{
			m_Status = error.Format(m_Path);
			return false;
		}
		m_Dirty = false;
		m_Status = "Saved and validated.";
		return true;
	}

	void AvbGraphPanel::OnGraphEdited()
	{
		m_Dirty = true;
		m_CompiledOutput.clear();
		m_Status = "Modified";
		RefreshValidation();
	}

	void AvbGraphPanel::AddNode(const std::string& typeId)
	{
		const Avb::AvbNodeDef* def = Avb::AvbNodeRegistry::Get().Find(typeId);
		if (!def) return;
		Avb::AvbNode node = Avb::AvbNodeRegistry::Get().CreateNode(typeId, Avb::GenerateStableId());
		if (node.Id.empty())
		{
			m_Status = "Could not create node type '" + typeId + "'.";
			return;
		}
		const std::size_t index = m_Graph.Nodes.size();
		node.X = 48.0f + static_cast<float>(index % 4) * 240.0f;
		node.Y = 48.0f + static_cast<float>(index / 4) * 190.0f;
		for (const Avb::AvbPinDef& pin : def->Pins)
			if (pin.Direction == Avb::AvbPinDir::Input && pin.DefaultValue)
				node.Literals.push_back({ pin.Id, *pin.DefaultValue });

		const std::string id = node.Id;
		std::string error;
		if (!m_Graph.AddNode(std::move(node), &error))
		{
			m_Status = error;
			return;
		}
		m_Canvas.Select(id);
		OnGraphEdited();
	}

	void AvbGraphPanel::Connect(const std::string& fromNode, const std::string& fromPin,
	                            const std::string& toNode, const std::string& toPin)
	{
		const Avb::AvbNode* source = m_Graph.FindNode(fromNode);
		const Avb::AvbNode* target = m_Graph.FindNode(toNode);
		const Avb::AvbNodeDef* sourceDef = source ? Avb::AvbNodeRegistry::Get().Find(source->TypeId) : nullptr;
		const Avb::AvbNodeDef* targetDef = target ? Avb::AvbNodeRegistry::Get().Find(target->TypeId) : nullptr;
		const Avb::AvbPinDef* sourcePin = sourceDef ? sourceDef->FindPin(fromPin) : nullptr;
		const Avb::AvbPinDef* targetPin = targetDef ? targetDef->FindPin(toPin) : nullptr;
		if (!sourcePin || !targetPin || sourcePin->Direction != Avb::AvbPinDir::Output ||
		    targetPin->Direction != Avb::AvbPinDir::Input || sourcePin->Type != targetPin->Type ||
		    sourcePin->Rate != targetPin->Rate)
		{
			m_Status = "Rejected connection: pin direction, type, or rate does not match.";
			return;
		}

		m_Graph.Links.erase(std::remove_if(m_Graph.Links.begin(), m_Graph.Links.end(),
			[&](const Avb::AvbLink& link) { return link.ToNode == toNode && link.ToPin == toPin; }),
			m_Graph.Links.end());
		m_Graph.Links.push_back({ fromNode, fromPin, toNode, toPin, false });
		OnGraphEdited();
	}

	void AvbGraphPanel::DeleteNode(const std::string& nodeId)
	{
		m_Graph.Nodes.erase(std::remove_if(m_Graph.Nodes.begin(), m_Graph.Nodes.end(),
			[&](const Avb::AvbNode& node) { return node.Id == nodeId; }), m_Graph.Nodes.end());
		m_Graph.Links.erase(std::remove_if(m_Graph.Links.begin(), m_Graph.Links.end(),
			[&](const Avb::AvbLink& link) { return link.FromNode == nodeId || link.ToNode == nodeId; }),
			m_Graph.Links.end());
		m_Graph.Sends.erase(std::remove_if(m_Graph.Sends.begin(), m_Graph.Sends.end(),
			[&](const Avb::AvbSend& send) { return send.FromNode == nodeId; }), m_Graph.Sends.end());
		if (m_Canvas.GetSelected() == nodeId)
			m_Canvas.Select(std::string());
		OnGraphEdited();
	}

	void AvbGraphPanel::DrawToolbar()
	{
		ImGui::PushID("avb_toolbar");
		ImGui::BeginDisabled(!m_HasGraph);
		if (ImGui::Button("Save")) SaveGraph();
		ImGui::SameLine();
		if (ImGui::Button("Validate / Compile")) ValidateGraph();
		ImGui::SameLine();
		if (ImGui::BeginCombo("Add node", "AVB node palette"))
		{
			for (const Avb::AvbNodeDef& def : Avb::AvbNodeRegistry::Get().All())
			{
				if (!def.AliasOf.empty()) continue;
				ImGui::PushID(def.TypeId.c_str());
				ImGui::TextDisabled("%s", def.Category.c_str());
				if (ImGui::Selectable(def.TypeId.c_str())) AddNode(def.TypeId);
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();

		if (m_Dirty)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Unsaved");
		}
		if (!m_Status.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", m_Status.c_str());
		}
		if (!m_Path.empty())
			ImGui::TextDisabled("%s", m_Path.c_str());
		ImGui::Separator();
		ImGui::PopID();
	}

	void AvbGraphPanel::DrawOpenConfirmation()
	{
		if (!m_ConfirmOpen)
			return;
		ImGui::OpenPopup("Unsaved AVB Changes");
		if (!ImGui::BeginPopupModal("Unsaved AVB Changes", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
			return;

		ImGui::TextUnformatted("This graph has unsaved changes.");
		ImGui::TextWrapped("Save or discard them before opening the selected .wlavb file.");
		if (ImGui::Button("Save then Open") && SaveGraph())
		{
			const std::string path = m_PendingOpenPath;
			if (LoadGraphFile(path))
			{
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard and Open"))
		{
			const std::string path = m_PendingOpenPath;
			if (LoadGraphFile(path))
			{
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			m_ConfirmOpen = false;
			m_PendingOpenPath.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void AvbGraphPanel::DrawCanvas()
	{
		if (!m_HasGraph)
		{
			ImGui::TextDisabled("Open a .wlavb file from Content Browser.");
			return;
		}
		NodeCanvas::Presentation presentation;
		presentation.Nodes.reserve(m_Graph.Nodes.size());
		for (const Avb::AvbNode& graphNode : m_Graph.Nodes)
		{
			const Avb::AvbNodeDef* def = Avb::AvbNodeRegistry::Get().Find(graphNode.TypeId);
			NodeCanvas::Node node;
			node.Id = graphNode.Id;
			node.Label = graphNode.Title.empty() ? graphNode.TypeId : graphNode.Title;
			node.Position = { graphNode.X, graphNode.Y };
			node.Selected = graphNode.Id == m_Canvas.GetSelected();
			node.TypeKnown = def != nullptr;
			if (def)
			{
				node.Pins.reserve(def->Pins.size());
				for (const Avb::AvbPinDef& pin : def->Pins)
					node.Pins.push_back({ pin.Id, pin.Id,
						pin.Direction == Avb::AvbPinDir::Input ? NodeCanvas::PinDirection::Input
						                                      : NodeCanvas::PinDirection::Output,
						CanvasStyle(pin.Type) });
			}
			presentation.Nodes.push_back(std::move(node));
		}
		for (const Avb::AvbLink& link : m_Graph.Links)
			if (NodeCanvas::Node* from = presentation.FindNode(link.FromNode))
				from->WiredTo.push_back({ link.FromPin, link.ToNode, link.ToPin });

		if (!m_FramedOnce && !presentation.Nodes.empty())
		{
			m_Canvas.FrameAll(presentation);
			m_FramedOnce = true;
		}
		NodeCanvas::Events events;
		m_Canvas.Render(presentation, events);
		if (events.WireAdded)
			Connect(events.LinkFromNode, events.LinkFromPin, events.LinkToNode, events.LinkToPin);
		if (events.NodeDeleted)
			DeleteNode(events.SelectedNodeId);
		if (events.NodeMoved)
		{
			if (Avb::AvbNode* moved = m_Graph.FindNode(events.MovedNodeId))
			{
				moved->X = events.MovedNodePosition.X;
				moved->Y = events.MovedNodePosition.Y;
				OnGraphEdited();
			}
		}
	}

	void AvbGraphPanel::DrawGraphInspector()
	{
		if (!m_HasGraph) return;
		ImGui::TextUnformatted("Graph");
		std::string& name = EditBuffer("graph/name", m_Graph.Name);
		ImGui::SetNextItemWidth(-1.0f);
		if (InputText("Name", name) && m_Graph.Name != name)
		{
			m_Graph.Name = name;
			OnGraphEdited();
		}
		const char* outputName = Avb::AvbChannelFormatName(m_Graph.Output);
		if (ImGui::BeginCombo("Output", outputName))
		{
			for (Avb::AvbChannelFormat format : { Avb::AvbChannelFormat::Mono, Avb::AvbChannelFormat::Stereo })
			{
				const bool selected = format == m_Graph.Output;
				if (ImGui::Selectable(Avb::AvbChannelFormatName(format), selected))
				{
					m_Graph.Output = format;
					OnGraphEdited();
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::Text("Nodes: %d   Links: %d   Inputs: %d   Variables: %d   Sends: %d",
			static_cast<int>(m_Graph.Nodes.size()), static_cast<int>(m_Graph.Links.size()),
			static_cast<int>(m_Graph.Inputs.size()), static_cast<int>(m_Graph.Variables.size()),
			static_cast<int>(m_Graph.Sends.size()));
		if (ImGui::TreeNode("Inputs / Variables / Sends"))
		{
			for (const Avb::AvbGraphInput& input : m_Graph.Inputs)
				ImGui::BulletText("Input %s : %s", input.Name.c_str(), Avb::AvbPinTypeName(input.Type));
			for (const Avb::AvbVariable& variable : m_Graph.Variables)
				ImGui::BulletText("Variable %s : %s = %s", variable.Name.c_str(),
					Avb::AvbPinTypeName(variable.Type), FormatLiteral(variable.DefaultValue).c_str());
			for (const Avb::AvbSend& send : m_Graph.Sends)
				ImGui::BulletText("Send %s -> %s (%s)", send.FromNode.c_str(), send.ToBus.c_str(), Avb::AvbSendModeName(send.Mode));
			ImGui::TreePop();
		}
	}

	void AvbGraphPanel::DrawNodeInspector()
	{
		ImGui::Separator();
		ImGui::TextUnformatted("Selected Node");
		Avb::AvbNode* node = m_Graph.FindNode(m_Canvas.GetSelected());
		if (!node)
		{
			ImGui::TextDisabled("Select a node on the canvas.");
			return;
		}
		ImGui::TextDisabled("%s  (%s)", node->Id.c_str(), node->TypeId.c_str());
		std::string& title = EditBuffer("node/" + node->Id + "/title", node->Title);
		ImGui::SetNextItemWidth(-1.0f);
		if (InputText("Title", title) && node->Title != title)
		{
			node->Title = title;
			OnGraphEdited();
		}
		float position[2] = { node->X, node->Y };
		if (ImGui::DragFloat2("Position", position, 1.0f))
		{
			node->X = position[0];
			node->Y = position[1];
			OnGraphEdited();
		}

		const Avb::AvbNodeDef* def = Avb::AvbNodeRegistry::Get().Find(node->TypeId);
		if (!def)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Unknown AVB node type.");
			return;
		}
		ImGui::Separator();
		for (const Avb::AvbPinDef& pin : def->Pins)
		{
			if (pin.Direction != Avb::AvbPinDir::Input) continue;
			const Avb::AvbLink* incoming = nullptr;
			for (const Avb::AvbLink& link : m_Graph.Links)
				if (link.ToNode == node->Id && link.ToPin == pin.Id) { incoming = &link; break; }
			ImGui::PushID(pin.Id.c_str());
			ImGui::Text("%s (%s, %s)", pin.Id.c_str(), Avb::AvbPinTypeName(pin.Type), Avb::AvbPinRateName(pin.Rate));
			if (incoming)
			{
				ImGui::TextDisabled("<- %s.%s", incoming->FromNode.c_str(), incoming->FromPin.c_str());
				ImGui::PopID();
				continue;
			}
			if (pin.Type == Avb::AvbPinType::Trigger || pin.Type == Avb::AvbPinType::Audio)
			{
				ImGui::TextDisabled("Connect this typed pin on the canvas.");
				ImGui::PopID();
				continue;
			}

			Avb::AvbLiteral nextValue;
			bool changed = false;
			const Avb::AvbLiteral* stored = node->FindLiteral(pin.Id);
			const Avb::AvbLiteral* current = stored ? stored : (pin.DefaultValue ? &*pin.DefaultValue : nullptr);
			if (pin.Type == Avb::AvbPinType::Bool)
			{
				bool value = current && std::holds_alternative<bool>(current->Value)
					? std::get<bool>(current->Value) : false;
				changed = ImGui::Checkbox("##literal", &value);
				if (changed) nextValue = Avb::AvbLiteral(value);
			}
			else
			{
				const std::string seed = current ? FormatLiteral(*current) : DefaultText(pin);
				std::string& literal = EditBuffer("node/" + node->Id + "/pin/" + pin.Id, seed);
				ImGui::SetNextItemWidth(-1.0f);
				if (InputText("##literal", literal))
				{
					if (pin.Type == Avb::AvbPinType::Time || pin.Type == Avb::AvbPinType::Float)
					{
						double parsed = 0.0;
						changed = ParseFiniteDouble(literal.c_str(), parsed);
						if (changed) nextValue = Avb::AvbLiteral(parsed);
					}
					else if (pin.Type == Avb::AvbPinType::Int)
					{
						std::int64_t parsed = 0;
						changed = ParseInt64(literal.c_str(), parsed);
						if (changed) nextValue = Avb::AvbLiteral(parsed);
					}
					else
					{
						changed = true;
						nextValue = Avb::AvbLiteral(literal);
					}
					if (!changed) m_Status = "Invalid literal; stored value unchanged.";
				}
			}
			if (changed)
			{
				Avb::AvbPinLiteral* literal = nullptr;
				for (Avb::AvbPinLiteral& entry : node->Literals)
					if (entry.PinId == pin.Id) { literal = &entry; break; }
				if (!literal)
				{
					node->Literals.push_back({ pin.Id, std::move(nextValue) });
					OnGraphEdited();
				}
				else if (literal->Value.Value != nextValue.Value)
				{
					literal->Value = std::move(nextValue);
					OnGraphEdited();
				}
			}
			ImGui::PopID();
		}
		ImGui::Separator();
		if (ImGui::Button("Delete Node")) DeleteNode(node->Id);
	}

	void AvbGraphPanel::DrawLinks()
	{
		if (!m_HasGraph) return;
		ImGui::Separator();
		ImGui::TextUnformatted("Connections");
		if (m_Graph.Links.empty())
		{
			ImGui::TextDisabled("No links. Drag between pins to connect nodes.");
			return;
		}
		for (std::size_t i = 0; i < m_Graph.Links.size(); ++i)
		{
			const Avb::AvbLink& link = m_Graph.Links[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TextWrapped("%s.%s -> %s.%s%s", link.FromNode.c_str(), link.FromPin.c_str(),
				link.ToNode.c_str(), link.ToPin.c_str(), link.Feedback ? " (feedback)" : "");
			ImGui::SameLine();
			if (ImGui::SmallButton("Disconnect"))
			{
				m_Graph.Links.erase(m_Graph.Links.begin() + static_cast<std::ptrdiff_t>(i));
				OnGraphEdited();
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
	}

	void AvbGraphPanel::DrawFindings()
	{
		ImGui::Separator();
		ImGui::Text("Validation (%d)", static_cast<int>(m_Validation.Findings.size()));
		if (m_Validation.Findings.empty())
			ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "No problems found.");
		else if (ImGui::BeginChild("avb_findings", ImVec2(0.0f, 110.0f), true))
		{
			for (const Avb::AvbDiagnostic& finding : m_Validation.Findings)
				ImGui::TextWrapped("%s", finding.Format(m_Path).c_str());
		}
		if (!m_Validation.Findings.empty()) ImGui::EndChild();
		if (!m_CompiledOutput.empty())
		{
			ImGui::TextUnformatted("Compiled JSON");
			if (ImGui::BeginChild("avb_compiled", ImVec2(0.0f, 130.0f), true))
				ImGui::TextUnformatted(m_CompiledOutput.c_str());
			ImGui::EndChild();
		}
	}

	void AvbGraphPanel::OnImGuiRender()
	{
		if (!m_Visible) return;
		if (!m_SizedOnce)
		{
			ImGui::SetNextWindowSize(ImVec2(1000.0f, 640.0f), ImGuiCond_FirstUseEver);
			m_SizedOnce = true;
		}
		ImGui::Begin("AVB Graph", &m_Visible);
		DrawToolbar();
		DrawOpenConfirmation();
		if (!m_HasGraph)
		{
			ImGui::TextDisabled("Open a .wlavb file from Content Browser.");
			ImGui::End();
			return;
		}
		if (ImGui::BeginTable("avb_split", 2, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Canvas", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 330.0f);
			ImGui::TableNextColumn();
			ImGui::BeginChild("avb_canvas_region", ImVec2(0.0f, 0.0f), true);
			DrawCanvas();
			ImGui::EndChild();
			ImGui::TableNextColumn();
			ImGui::BeginChild("avb_inspector", ImVec2(0.0f, 0.0f), true);
			DrawGraphInspector();
			DrawNodeInspector();
			DrawLinks();
			DrawFindings();
			ImGui::EndChild();
			ImGui::EndTable();
		}
		ImGui::End();
	}
}
