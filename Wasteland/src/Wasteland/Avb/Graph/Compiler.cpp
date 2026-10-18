#include "wlpch.h"

#include "Compiler.h"

#include "NodeRegistry.h"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <queue>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Wasteland::Avb
{
	namespace
	{
		std::string JsonString(const std::string& value)
		{
			std::string escaped = "\"";
			for (const unsigned char c : value)
			{
				switch (c)
				{
					case '"': escaped += "\\\""; break;
					case '\\': escaped += "\\\\"; break;
					case '\b': escaped += "\\b"; break;
					case '\f': escaped += "\\f"; break;
					case '\n': escaped += "\\n"; break;
					case '\r': escaped += "\\r"; break;
					case '\t': escaped += "\\t"; break;
					default:
						if (c < 0x20)
					{
						static const char digits[] = "0123456789abcdef";
						escaped += "\\u00";
						escaped += digits[(c >> 4) & 0xf];
						escaped += digits[c & 0xf];
					}
					else escaped += static_cast<char>(c);
				}
			}
			escaped += '"';
			return escaped;
		}

		std::string JsonNumber(double value)
		{
			std::ostringstream out;
			out.imbue(std::locale::classic());
			out << std::setprecision(17) << value;
			return out.str();
		}

		std::string JsonLiteral(const AvbLiteral& literal)
		{
			return std::visit([](const auto& value) -> std::string
			{
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, std::monostate>) return "null";
				else if constexpr (std::is_same_v<T, double>) return JsonNumber(value);
				else if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(value);
				else if constexpr (std::is_same_v<T, bool>) return value ? "true" : "false";
				else return JsonString(value);
			}, literal.Value);
		}

		std::string JsonFloat(float value)
		{
			// Classic locale and setprecision make the JSON payload byte-stable.
			return JsonNumber(static_cast<double>(value));
		}

		template<typename T, typename KeyFn>
		std::vector<const T*> SortedPointers(const std::vector<T>& values, KeyFn key)
		{
			std::vector<const T*> sorted;
			sorted.reserve(values.size());
			for (const T& value : values) sorted.push_back(&value);
			std::sort(sorted.begin(), sorted.end(), [&](const T* a, const T* b)
				{ return key(*a) < key(*b); });
			return sorted;
		}

		std::string BuildCanonicalJson(const AvbGraph& graph,
		                              const std::vector<const AvbNode*>& order)
		{
			std::ostringstream json;
			json.imbue(std::locale::classic());
			json << "{\"schema\":" << graph.Schema
			     << ",\"name\":" << JsonString(graph.Name)
			     << ",\"output\":" << JsonString(AvbChannelFormatName(graph.Output));

			const auto inputs = SortedPointers(graph.Inputs, [](const AvbGraphInput& input) { return input.Name; });
			json << ",\"inputs\":[";
			for (std::size_t i = 0; i < inputs.size(); ++i)
			{
				if (i) json << ',';
				const AvbGraphInput& input = *inputs[i];
				json << "{\"name\":" << JsonString(input.Name)
				     << ",\"type\":" << JsonString(AvbPinTypeName(input.Type))
				     << ",\"direction\":" << JsonString(AvbPinDirName(input.Direction))
				     << ",\"default\":" << (input.DefaultValue ? JsonLiteral(*input.DefaultValue) : "null") << '}';
			}
			json << ']';

			const auto variables = SortedPointers(graph.Variables, [](const AvbVariable& variable) { return variable.Name; });
			json << ",\"variables\":[";
			for (std::size_t i = 0; i < variables.size(); ++i)
			{
				if (i) json << ',';
				const AvbVariable& variable = *variables[i];
				json << "{\"name\":" << JsonString(variable.Name)
				     << ",\"type\":" << JsonString(AvbPinTypeName(variable.Type))
				     << ",\"default\":" << JsonLiteral(variable.DefaultValue) << '}';
			}
			json << ']';

			json << ",\"nodes\":[";
			for (std::size_t i = 0; i < order.size(); ++i)
			{
				if (i) json << ',';
				const AvbNode& node = *order[i];
				json << "{\"id\":" << JsonString(node.Id)
				     << ",\"type\":" << JsonString(node.TypeId)
				     << ",\"title\":" << JsonString(node.Title)
				     << ",\"literals\":[";
				const auto literals = SortedPointers(node.Literals, [](const AvbPinLiteral& literal) { return literal.PinId; });
				for (std::size_t j = 0; j < literals.size(); ++j)
				{
					if (j) json << ',';
					json << "{\"pin\":" << JsonString(literals[j]->PinId)
					     << ",\"value\":" << JsonLiteral(literals[j]->Value) << '}';
				}
				json << "]}";
			}
			json << ']';

			auto links = SortedPointers(graph.Links, [](const AvbLink& link)
				{ return std::tie(link.ToNode, link.ToPin, link.FromNode, link.FromPin, link.Feedback); });
			json << ",\"links\":[";
			for (std::size_t i = 0; i < links.size(); ++i)
			{
				if (i) json << ',';
				const AvbLink& link = *links[i];
				json << "{\"from\":{\"node\":" << JsonString(link.FromNode)
				     << ",\"pin\":" << JsonString(link.FromPin)
				     << "},\"to\":{\"node\":" << JsonString(link.ToNode)
				     << ",\"pin\":" << JsonString(link.ToPin)
				     << "},\"feedback\":" << (link.Feedback ? "true" : "false") << '}';
			}
			json << ']';

			auto sends = SortedPointers(graph.Sends, [](const AvbSend& send)
				{ return std::tie(send.FromNode, send.ToBus, send.Mode, send.Gain); });
			json << ",\"sends\":[";
			for (std::size_t i = 0; i < sends.size(); ++i)
			{
				if (i) json << ',';
				const AvbSend& send = *sends[i];
				json << "{\"from\":" << JsonString(send.FromNode)
				     << ",\"to\":" << JsonString(send.ToBus)
				     << ",\"mode\":" << JsonString(AvbSendModeName(send.Mode))
				     << ",\"gain\":" << JsonFloat(send.Gain) << '}';
			}
			json << "]}";
			return json.str();
		}
	}

	bool AvbCompiler::Compile(const AvbGraph& graph, AvbCompiledGraph& out,
	                          AvbValidationResult& validation)
	{
		out = {};
		if (!AvbValidator::Validate(graph, validation)) return false;

		std::map<std::string, std::vector<std::string>> outgoing;
		std::map<std::string, std::size_t> indegree;
		for (const AvbNode& node : graph.Nodes)
		{
			outgoing[node.Id];
			indegree[node.Id] = 0;
		}
		for (const AvbLink& link : graph.Links)
		{
			if (link.Feedback) continue;
			const AvbNode* fromNode = graph.FindNode(link.FromNode);
			const AvbNode* toNode = graph.FindNode(link.ToNode);
			const AvbNodeDef* fromDef = AvbNodeRegistry::Get().Find(fromNode->TypeId);
			const AvbNodeDef* toDef = AvbNodeRegistry::Get().Find(toNode->TypeId);
			const AvbPinDef* fromPin = fromDef->FindPin(link.FromPin);
			const AvbPinDef* toPin = toDef->FindPin(link.ToPin);
			if (fromPin->Type == AvbPinType::Trigger) continue;
			outgoing[link.FromNode].push_back(link.ToNode);
			++indegree[link.ToNode];
		}

		// Kahn's algorithm uses a lexical min-heap, so every topological tie is
		// broken by stable node ID rather than pointer address or hash iteration.
		std::priority_queue<std::string, std::vector<std::string>, std::greater<std::string>> ready;
		for (const auto& item : indegree)
			if (item.second == 0) ready.push(item.first);
		std::vector<std::string> ids;
		ids.reserve(graph.Nodes.size());
		while (!ready.empty())
		{
			const std::string id = ready.top();
			ready.pop();
			ids.push_back(id);
			for (const std::string& next : outgoing[id])
				if (--indegree[next] == 0) ready.push(next);
		}
		if (ids.size() != graph.Nodes.size())
		{
			// Validation normally catches this first; keep a defensive path in case
			// the graph and registry are concurrently mutated by a caller.
			validation.Findings.push_back({ "data cycle prevented deterministic topological compilation", {}, {}, false });
			return false;
		}

		out.Nodes.reserve(ids.size());
		for (const std::string& id : ids)
			out.Nodes.push_back(graph.FindNode(id));
		out.Output = BuildCanonicalJson(graph, out.Nodes);
		return true;
	}
}
