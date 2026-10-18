#include "wlpch.h"

#include "Validator.h"

#include "NodeRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace Wasteland::Avb
{
	std::string AvbDiagnostic::Format(const std::string& fileName) const
	{
		std::string result = fileName;
		if (HasNode)
		{
			if (!result.empty()) result += ": ";
			result += "node '" + NodeId + "'";
			if (!PinId.empty()) result += " pin '" + PinId + "'";
			result += ": ";
		}
		else if (!result.empty())
			result += ": ";
		result += Message;
		return result;
	}

	std::string AvbValidationResult::Report(const std::string& fileName) const
	{
		std::string report;
		for (const AvbDiagnostic& finding : Findings)
		{
			if (!report.empty()) report += '\n';
			report += finding.Format(fileName);
		}
		return report;
	}

	namespace
	{
		void Add(AvbValidationResult& out, std::string message,
		         std::string nodeId = {}, std::string pinId = {})
		{
			AvbDiagnostic finding;
			finding.Message = std::move(message);
			finding.NodeId = std::move(nodeId);
			finding.PinId = std::move(pinId);
			finding.HasNode = !finding.NodeId.empty();
			out.Findings.push_back(std::move(finding));
		}

		bool IsFiniteLiteral(const AvbLiteral& literal)
		{
			return !std::holds_alternative<double>(literal.Value) ||
			       std::isfinite(std::get<double>(literal.Value));
		}

		bool FitsRuntimeFloatRange(const AvbLiteral& literal, AvbPinType type)
		{
			if ((type != AvbPinType::Float && type != AvbPinType::Time) ||
			    !std::holds_alternative<double>(literal.Value))
				return true;
			const double value = std::get<double>(literal.Value);
			const double maxFloat = static_cast<double>((std::numeric_limits<float>::max)());
			return value >= -maxFloat && value <= maxFloat;
		}

		bool IsAllowedEnumValue(const AvbPinDef& pin, const AvbLiteral& literal)
		{
			if (pin.Type != AvbPinType::Enum || pin.AllowedValues.empty() ||
			    !std::holds_alternative<std::string>(literal.Value))
				return true;
			return std::find(pin.AllowedValues.begin(), pin.AllowedValues.end(),
			                 std::get<std::string>(literal.Value)) != pin.AllowedValues.end();
		}

		void ValidateRuntimeFloatRange(AvbValidationResult& out, const AvbLiteral& literal,
		                               AvbPinType type, const std::string& context,
		                               const std::string& nodeId = {}, const std::string& pinId = {})
		{
			if (IsFiniteLiteral(literal) && !FitsRuntimeFloatRange(literal, type))
				Add(out, context + " is outside finite float range", nodeId, pinId);
		}
	}

	bool AvbValidator::Validate(const AvbGraph& graph, AvbValidationResult& out)
	{
		out.Findings.clear();
		if (graph.Schema != 1)
			Add(out, "unsupported graph schema " + std::to_string(graph.Schema) + " (expected 1)");

		if (graph.Name.empty()) Add(out, "graph name must not be empty");
		if (graph.Output != AvbChannelFormat::Mono && graph.Output != AvbChannelFormat::Stereo)
			Add(out, "graph output format is invalid");

		std::map<std::string, const AvbNode*> nodes;
		std::map<std::string, const AvbNodeDef*> defs;
		for (const AvbNode& node : graph.Nodes)
		{
			if (node.Id.empty())
			{
				Add(out, "node id must not be empty", "<empty>");
				continue;
			}
			if (!nodes.emplace(node.Id, &node).second)
			{
				Add(out, "duplicate node id", node.Id);
				continue;
			}
			const AvbNodeDef* def = AvbNodeRegistry::Get().Find(node.TypeId);
			if (!def)
			{
				Add(out, "unknown node type '" + node.TypeId + "'", node.Id);
				continue;
			}
			defs.emplace(node.Id, def);
			if (!std::isfinite(node.X) || !std::isfinite(node.Y))
				Add(out, "node position must be finite", node.Id);

			std::set<std::string> literalPins;
			for (const AvbPinLiteral& literal : node.Literals)
			{
				const AvbPinDef* pin = def->FindPin(literal.PinId);
				if (!pin)
				{
					Add(out, "unknown pin '" + literal.PinId + "' on node type '" + node.TypeId + "'",
					    node.Id, literal.PinId);
					continue;
				}
				if (!literalPins.insert(literal.PinId).second)
					Add(out, "duplicate literal for pin", node.Id, literal.PinId);
				if (pin->Direction != AvbPinDir::Input)
					Add(out, "literal targets an output pin", node.Id, literal.PinId);
				if (!AvbLiteralMatchesType(literal.Value, pin->Type))
					Add(out, "literal type does not match " + std::string(AvbPinTypeName(pin->Type)), node.Id, literal.PinId);
				if (!IsAllowedEnumValue(*pin, literal.Value))
					Add(out, "unsupported enum choice '" + std::get<std::string>(literal.Value.Value) + "'",
					    node.Id, literal.PinId);
				if (!IsFiniteLiteral(literal.Value))
					Add(out, "numeric literal must be finite", node.Id, literal.PinId);
				else
					ValidateRuntimeFloatRange(out, literal.Value, pin->Type, "numeric literal",
					                         node.Id, literal.PinId);
			}
			for (const AvbPinDef& pin : def->Pins)
			{
				if (!pin.DefaultValue) continue;
				if (!IsAllowedEnumValue(pin, *pin.DefaultValue))
					Add(out, "unsupported enum default '" + std::get<std::string>(pin.DefaultValue->Value) + "'",
					    node.Id, pin.Id);
				if (!IsFiniteLiteral(*pin.DefaultValue))
					Add(out, "numeric default must be finite", node.Id, pin.Id);
				else
					ValidateRuntimeFloatRange(out, *pin.DefaultValue, pin.Type, "numeric default",
					                         node.Id, pin.Id);
			}
		}

		std::map<std::pair<std::string, std::string>, std::size_t> incoming;
		std::map<std::pair<std::string, std::string>, std::size_t> triggerReceivers;
		struct DataEdge { std::string To; std::string Pin; };
		std::map<std::string, std::vector<DataEdge>> dataEdges;
		for (const auto& node : nodes) dataEdges[node.first];

		for (const AvbLink& link : graph.Links)
		{
			const auto sourceNode = nodes.find(link.FromNode);
			const auto targetNode = nodes.find(link.ToNode);
			if (sourceNode == nodes.end())
			{
				Add(out, "link references deleted node '" + link.FromNode + "'",
				    link.FromNode, link.FromPin);
				continue;
			}
			if (targetNode == nodes.end())
			{
				Add(out, "link references deleted node '" + link.ToNode + "'",
				    link.ToNode, link.ToPin);
				continue;
			}
			const auto sourceDef = defs.find(link.FromNode);
			const auto targetDef = defs.find(link.ToNode);
			if (sourceDef == defs.end() || targetDef == defs.end()) continue;
			const AvbPinDef* sourcePin = sourceDef->second->FindPin(link.FromPin);
			const AvbPinDef* targetPin = targetDef->second->FindPin(link.ToPin);
			if (!sourcePin)
			{
				Add(out, "unknown pin '" + link.FromPin + "' on node type '" + sourceNode->second->TypeId + "'",
				    link.FromNode, link.FromPin);
				continue;
			}
			if (!targetPin)
			{
				Add(out, "unknown pin '" + link.ToPin + "' on node type '" + targetNode->second->TypeId + "'",
				    link.ToNode, link.ToPin);
				continue;
			}
			if (sourcePin->Direction != AvbPinDir::Output)
				Add(out, "link source must be an output pin", link.FromNode, link.FromPin);
			if (targetPin->Direction != AvbPinDir::Input)
				Add(out, "link target must be an input pin", link.ToNode, link.ToPin);

			if (sourcePin->Type == AvbPinType::Trigger && targetPin->Type != AvbPinType::Trigger)
				Add(out, "Trigger wire cannot connect to data pin '" + link.ToPin + "'",
				    link.ToNode, link.ToPin);
			else if (sourcePin->Type != AvbPinType::Trigger && targetPin->Type == AvbPinType::Trigger)
				Add(out, "data wire cannot connect to Trigger pin '" + link.ToPin + "'",
				    link.ToNode, link.ToPin);
			else if (sourcePin->Type != targetPin->Type)
				Add(out, "incompatible types: " + std::string(AvbPinTypeName(sourcePin->Type)) +
				        " output cannot connect to " + AvbPinTypeName(targetPin->Type) + " input",
				    link.ToNode, link.ToPin);

			if (sourcePin->Rate != targetPin->Rate)
				Add(out, "pin rate mismatch: " + std::string(AvbPinRateName(sourcePin->Rate)) +
				        " output cannot connect to " + AvbPinRateName(targetPin->Rate) + " input",
				    link.ToNode, link.ToPin);

			++incoming[{ link.ToNode, link.ToPin }];
			if (sourcePin->Type == AvbPinType::Trigger && sourcePin->Direction == AvbPinDir::Output)
				++triggerReceivers[{ link.FromNode, link.FromPin }];
			if (!link.Feedback && sourcePin->Type != AvbPinType::Trigger && targetPin->Type != AvbPinType::Trigger &&
			    sourcePin->Direction == AvbPinDir::Output && targetPin->Direction == AvbPinDir::Input)
				dataEdges[link.FromNode].push_back({ link.ToNode, link.ToPin });
		}

		for (const auto& entry : incoming)
			if (entry.second > 1)
				Add(out, "input pin has more than one incoming link", entry.first.first, entry.first.second);

		for (const auto& entry : defs)
		{
			const AvbNode& node = *nodes.at(entry.first);
			for (const AvbPinDef& pin : entry.second->Pins)
			{
				if (pin.Direction == AvbPinDir::Output && pin.Type == AvbPinType::Trigger && pin.Required &&
				    triggerReceivers[{ node.Id, pin.Id }] == 0)
					Add(out, "required trigger output has no receivers", node.Id, pin.Id);
				if (pin.Direction != AvbPinDir::Input || !pin.Required) continue;
				const AvbLiteral* literal = node.FindLiteral(pin.Id);
				const bool hasLiteral = literal && literal->IsSet();
				if (incoming[{ node.Id, pin.Id }] == 0 && !hasLiteral)
					Add(out, "required input has neither a link nor a literal", node.Id, pin.Id);
			}
		}

		// Deterministic DFS over lexically ordered node IDs catches each data
		// back-edge. Declared feedback edges are deliberately omitted above.
		std::map<std::string, unsigned char> color;
		for (const auto& node : nodes) color[node.first] = 0;
		const auto visit = [&](const auto& self, const std::string& id) -> void
		{
			color[id] = 1;
			auto& edges = dataEdges[id];
			std::sort(edges.begin(), edges.end(), [](const DataEdge& a, const DataEdge& b)
				{ return std::tie(a.To, a.Pin) < std::tie(b.To, b.Pin); });
			for (const DataEdge& edge : edges)
			{
				if (color[edge.To] == 1)
				{
					Add(out, "data cycle is not a declared feedback edge", edge.To, edge.Pin);
				}
				else if (color[edge.To] == 0)
					self(self, edge.To);
			}
			color[id] = 2;
		};
		for (const auto& node : nodes)
			if (color[node.first] == 0) visit(visit, node.first);

		std::set<std::string> inputNames;
		for (const AvbGraphInput& input : graph.Inputs)
		{
			if (input.Name.empty() || !inputNames.insert(input.Name).second)
				Add(out, "graph input has an empty or duplicate name");
			if (input.DefaultValue && input.DefaultValue->IsSet() &&
			    !AvbLiteralMatchesType(*input.DefaultValue, input.Type))
				Add(out, "default value type does not match graph input '" + input.Name + "'");
			if (input.DefaultValue && !IsFiniteLiteral(*input.DefaultValue))
				Add(out, "numeric default must be finite for graph input '" + input.Name + "'");
			else if (input.DefaultValue)
				ValidateRuntimeFloatRange(out, *input.DefaultValue, input.Type,
				                         "numeric default for graph input '" + input.Name + "'");
		}
		std::set<std::string> variableNames;
		for (const AvbVariable& variable : graph.Variables)
		{
			if (variable.Name.empty() || !variableNames.insert(variable.Name).second)
				Add(out, "variable has an empty or duplicate name");
			if (variable.DefaultValue.IsSet() && !AvbLiteralMatchesType(variable.DefaultValue, variable.Type))
				Add(out, "default value type does not match variable '" + variable.Name + "'");
			if (!IsFiniteLiteral(variable.DefaultValue))
				Add(out, "numeric default must be finite for variable '" + variable.Name + "'");
			else
				ValidateRuntimeFloatRange(out, variable.DefaultValue, variable.Type,
				                         "numeric default for variable '" + variable.Name + "'");
		}
		for (const AvbSend& send : graph.Sends)
		{
			if (nodes.find(send.FromNode) == nodes.end())
				Add(out, "send references deleted node '" + send.FromNode + "'", send.FromNode);
			if (send.ToBus.empty()) Add(out, "send destination bus must not be empty", send.FromNode);
			if (!std::isfinite(send.Gain)) Add(out, "send gain must be finite", send.FromNode);
			if (std::string(AvbSendModeName(send.Mode)).front() == '<')
				Add(out, "send mode is invalid", send.FromNode);
		}

		return !out.HasErrors();
	}
}
