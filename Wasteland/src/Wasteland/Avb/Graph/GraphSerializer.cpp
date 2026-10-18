#include "wlpch.h"

#include "Graph.h"
#include "GraphSerializer.h"
#include "NodeRegistry.h"

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Wasteland::Avb
{
	std::string AvbGraphError::Format(const std::string& fileName) const
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

	namespace
	{
		bool Fail(AvbGraphError& error, std::string message,
		          std::string nodeId = {}, std::string pinId = {})
		{
			error.Message = std::move(message);
			error.NodeId = std::move(nodeId);
			error.PinId = std::move(pinId);
			error.HasNode = !error.NodeId.empty();
			return false;
		}

		bool CheckMapKeys(const YAML::Node& node, std::initializer_list<const char*> allowed,
		                  const std::string& context, AvbGraphError& error,
		                  const std::string& nodeId = {}, const std::string& pinId = {})
		{
			if (!node.IsMap())
				return Fail(error, "expected a mapping for " + context, nodeId, pinId);
			std::set<std::string> seen;
			for (const auto& entry : node)
			{
				const std::string key = entry.first.as<std::string>();
				bool recognized = false;
				for (const char* candidate : allowed)
					if (key == candidate) { recognized = true; break; }
				if (!recognized)
					return Fail(error, "unknown key '" + key + "' in " + context, nodeId, pinId);
				if (!seen.insert(key).second)
					return Fail(error, "duplicate key '" + key + "' in " + context, nodeId, pinId);
			}
			return true;
		}

		bool ReadRequiredString(const YAML::Node& map, const char* key,
		                        std::string& value, const std::string& context,
		                        AvbGraphError& error, const std::string& nodeId = {})
		{
			const YAML::Node field = map[key];
			if (!field || field.IsNull())
				return Fail(error, context + " requires '" + key + "'", nodeId);
			value = field.as<std::string>();
			return true;
		}

		AvbLiteral ReadLiteral(const YAML::Node& node, AvbPinType type)
		{
			if (node.IsNull()) return AvbLiteral{};
			switch (type)
			{
				case AvbPinType::Time:
				case AvbPinType::Float: return AvbLiteral(node.as<double>());
				case AvbPinType::Int: return AvbLiteral(node.as<std::int64_t>());
				case AvbPinType::Bool: return AvbLiteral(node.as<bool>());
				case AvbPinType::Enum:
				case AvbPinType::String:
				case AvbPinType::AssetRef: return AvbLiteral(node.as<std::string>());
				case AvbPinType::Trigger:
				case AvbPinType::Audio: return AvbLiteral{};
			}
			return {};
		}

		bool ReadTypedLiteral(const YAML::Node& node, AvbPinType type,
		                      AvbLiteral& out, AvbGraphError& error,
		                      const std::string& description,
		                      const std::string& nodeId = {}, const std::string& pinId = {})
		{
			if (node.IsNull())
			{
				out = AvbLiteral{};
				return true;
			}
			out = ReadLiteral(node, type);
			if (!out.IsSet() && (type == AvbPinType::Trigger || type == AvbPinType::Audio))
			{
				if (node.IsNull()) return true;
				return Fail(error, description + " cannot hold a literal of type " + AvbPinTypeName(type), nodeId, pinId);
			}
			if (!AvbLiteralMatchesType(out, type))
				return Fail(error, description + " has a value incompatible with " + AvbPinTypeName(type), nodeId, pinId);
			return true;
		}

		bool ReadType(const YAML::Node& map, AvbPinType& type,
		              const std::string& context, AvbGraphError& error,
		              const std::string& nodeId = {})
		{
			std::string name;
			if (!ReadRequiredString(map, "type", name, context, error, nodeId)) return false;
			if (!AvbPinTypeFromName(name, type))
				return Fail(error, "unknown pin type '" + name + "' in " + context, nodeId);
			return true;
		}

		bool ReadEndpoint(const YAML::Node& endpoint, std::string& nodeId,
		                  std::string& pinId, AvbGraphError& error)
		{
			if (!CheckMapKeys(endpoint, { "node", "pin" }, "link endpoint", error)) return false;
			if (!ReadRequiredString(endpoint, "node", nodeId, "link endpoint", error)) return false;
			if (!ReadRequiredString(endpoint, "pin", pinId, "link endpoint", error, nodeId)) return false;
			return true;
		}

		void EmitLiteral(YAML::Emitter& out, const AvbLiteral& literal)
		{
			std::visit([&out](const auto& value)
			{
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, std::monostate>) out << YAML::Null;
				else if constexpr (std::is_same_v<T, std::string>) out << YAML::DoubleQuoted << value;
				else out << value;
			}, literal.Value);
		}
	}

	bool AvbGraphSerializer::Parse(const std::string& text, AvbGraph& outGraph, AvbGraphError& error)
	{
		AvbGraph parsed;
		try
		{
			const YAML::Node root = YAML::Load(text);
			if (!CheckMapKeys(root, { "schema", "name", "output", "inputs", "variables", "nodes", "links", "sends" }, "graph", error))
				return false;
			if (!root["schema"] || root["schema"].as<std::uint32_t>() != 1)
				return Fail(error, "unsupported or missing .wlavb schema (expected 1)");
			parsed.Schema = 1;
			if (root["name"] && !root["name"].IsNull()) parsed.Name = root["name"].as<std::string>();
			if (root["output"])
			{
				const std::string output = root["output"].as<std::string>();
				if (!AvbChannelFormatFromName(output, parsed.Output))
					return Fail(error, "unknown graph output format '" + output + "'");
			}

			if (const YAML::Node inputs = root["inputs"])
			{
				if (!inputs.IsSequence()) return Fail(error, "graph 'inputs' must be a sequence");
				std::set<std::string> names;
				for (const YAML::Node& item : inputs)
				{
					if (!CheckMapKeys(item, { "name", "type", "direction", "default" }, "graph input", error)) return false;
					AvbGraphInput input;
					if (!ReadRequiredString(item, "name", input.Name, "graph input", error)) return false;
					if (!names.insert(input.Name).second) return Fail(error, "duplicate graph input '" + input.Name + "'");
					if (!ReadType(item, input.Type, "graph input '" + input.Name + "'", error)) return false;
					if (item["direction"])
					{
						const std::string direction = item["direction"].as<std::string>();
						if (!AvbPinDirFromName(direction, input.Direction))
							return Fail(error, "unknown direction '" + direction + "' for graph input '" + input.Name + "'");
					}
					if (item["default"])
					{
						AvbLiteral value;
						if (!ReadTypedLiteral(item["default"], input.Type, value, error,
						                      "default for graph input '" + input.Name + "'")) return false;
						input.DefaultValue = std::move(value);
					}
					parsed.Inputs.push_back(std::move(input));
				}
			}

			if (const YAML::Node variables = root["variables"])
			{
				if (!variables.IsSequence()) return Fail(error, "graph 'variables' must be a sequence");
				std::set<std::string> names;
				for (const YAML::Node& item : variables)
				{
					if (!CheckMapKeys(item, { "name", "type", "default" }, "variable", error)) return false;
					AvbVariable variable;
					if (!ReadRequiredString(item, "name", variable.Name, "variable", error)) return false;
					if (!names.insert(variable.Name).second) return Fail(error, "duplicate variable '" + variable.Name + "'");
					if (!ReadType(item, variable.Type, "variable '" + variable.Name + "'", error)) return false;
					if (item["default"] && !ReadTypedLiteral(item["default"], variable.Type, variable.DefaultValue,
					                                         error, "default for variable '" + variable.Name + "'")) return false;
					parsed.Variables.push_back(std::move(variable));
				}
			}

			if (const YAML::Node nodes = root["nodes"])
			{
				if (!nodes.IsSequence()) return Fail(error, "graph 'nodes' must be a sequence");
				for (const YAML::Node& item : nodes)
				{
					if (!CheckMapKeys(item, { "id", "type", "title", "x", "y", "literals" }, "node", error)) return false;
					AvbNode node;
					if (!ReadRequiredString(item, "id", node.Id, "node", error)) return false;
					if (!ReadRequiredString(item, "type", node.TypeId, "node", error, node.Id)) return false;
					const AvbNodeDef* def = AvbNodeRegistry::Get().Find(node.TypeId);
					if (!def) return Fail(error, "unknown node type '" + node.TypeId + "'", node.Id);
					if (item["title"] && !item["title"].IsNull()) node.Title = item["title"].as<std::string>();
					if (item["x"]) node.X = item["x"].as<float>();
					if (item["y"]) node.Y = item["y"].as<float>();
					if (const YAML::Node literals = item["literals"])
					{
						if (!literals.IsSequence()) return Fail(error, "node literals must be a sequence", node.Id);
					std::set<std::string> literalPins;
					for (const YAML::Node& literalNode : literals)
						{
						if (!CheckMapKeys(literalNode, { "pin", "value" }, "node literal", error, node.Id)) return false;
						std::string pinId;
						if (!ReadRequiredString(literalNode, "pin", pinId, "node literal", error, node.Id)) return false;
						const AvbPinDef* pin = def->FindPin(pinId);
						if (!pin) return Fail(error, "unknown pin '" + pinId + "' on node type '" + node.TypeId + "'", node.Id, pinId);
						if (pin->Direction != AvbPinDir::Input) return Fail(error, "literal targets an output pin", node.Id, pinId);
						if (!literalPins.insert(pinId).second) return Fail(error, "duplicate literal for pin", node.Id, pinId);
						if (!literalNode["value"]) return Fail(error, "node literal requires 'value'", node.Id, pinId);
						AvbLiteral value;
						if (!ReadTypedLiteral(literalNode["value"], pin->Type, value, error, "literal", node.Id, pinId)) return false;
						node.Literals.push_back({ std::move(pinId), std::move(value) });
					}
				}
					std::string addError;
					if (!parsed.AddNode(std::move(node), &addError)) return Fail(error, addError);
				}
			}

			if (const YAML::Node links = root["links"])
			{
				if (!links.IsSequence()) return Fail(error, "graph 'links' must be a sequence");
				for (const YAML::Node& item : links)
				{
					if (!CheckMapKeys(item, { "from", "to", "feedback" }, "link", error)) return false;
					if (!item["from"] || !item["to"]) return Fail(error, "link requires 'from' and 'to'");
					AvbLink link;
					if (!ReadEndpoint(item["from"], link.FromNode, link.FromPin, error) ||
					    !ReadEndpoint(item["to"], link.ToNode, link.ToPin, error)) return false;
					if (item["feedback"]) link.Feedback = item["feedback"].as<bool>();
					for (const auto& endpoint : { std::pair<std::string, std::string>{link.FromNode, link.FromPin},
					                              {link.ToNode, link.ToPin} })
					{
						const AvbNode* node = parsed.FindNode(endpoint.first);
						if (!node) return Fail(error, "link references deleted node '" + endpoint.first + "'", endpoint.first, endpoint.second);
						const AvbNodeDef* def = AvbNodeRegistry::Get().Find(node->TypeId);
						if (!def || !def->FindPin(endpoint.second))
							return Fail(error, "unknown pin '" + endpoint.second + "' on node type '" + node->TypeId + "'", node->Id, endpoint.second);
					}
					parsed.Links.push_back(std::move(link));
				}
			}

			if (const YAML::Node sends = root["sends"])
			{
				if (!sends.IsSequence()) return Fail(error, "graph 'sends' must be a sequence");
				for (const YAML::Node& item : sends)
				{
					if (!CheckMapKeys(item, { "from", "to", "mode", "gain" }, "send", error)) return false;
					AvbSend send;
					if (!ReadRequiredString(item, "from", send.FromNode, "send", error)) return false;
					if (!ReadRequiredString(item, "to", send.ToBus, "send", error, send.FromNode)) return false;
					if (!item["mode"]) return Fail(error, "send requires 'mode'", send.FromNode);
					const std::string mode = item["mode"].as<std::string>();
					if (!AvbSendModeFromName(mode, send.Mode)) return Fail(error, "unknown send mode '" + mode + "'", send.FromNode);
					if (item["gain"]) send.Gain = item["gain"].as<float>();
					parsed.Sends.push_back(std::move(send));
				}
			}
		}
		catch (const YAML::Exception& ex)
		{
			return Fail(error, std::string("invalid .wlavb YAML: ") + ex.what());
		}
		catch (const std::exception& ex)
		{
			return Fail(error, std::string("invalid .wlavb value: ") + ex.what());
		}

		outGraph = std::move(parsed);
		error = {};
		return true;
	}

	std::string AvbGraphSerializer::Emit(const AvbGraph& graph)
	{
		YAML::Emitter out;
		out.SetIndent(2);
		out.SetFloatPrecision(17);
		out << YAML::BeginMap;
		out << YAML::Key << "schema" << YAML::Value << graph.Schema;
		out << YAML::Key << "name" << YAML::Value << YAML::DoubleQuoted << graph.Name;
		out << YAML::Key << "output" << YAML::Value << AvbChannelFormatName(graph.Output);
		out << YAML::Key << "inputs" << YAML::Value << YAML::BeginSeq;
		for (const AvbGraphInput& input : graph.Inputs)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "name" << YAML::Value << YAML::DoubleQuoted << input.Name;
			out << YAML::Key << "type" << YAML::Value << AvbPinTypeName(input.Type);
			out << YAML::Key << "direction" << YAML::Value << AvbPinDirName(input.Direction);
			out << YAML::Key << "default" << YAML::Value;
			if (input.DefaultValue) EmitLiteral(out, *input.DefaultValue); else out << YAML::Null;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::Key << "variables" << YAML::Value << YAML::BeginSeq;
		for (const AvbVariable& variable : graph.Variables)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "name" << YAML::Value << YAML::DoubleQuoted << variable.Name;
			out << YAML::Key << "type" << YAML::Value << AvbPinTypeName(variable.Type);
			out << YAML::Key << "default" << YAML::Value; EmitLiteral(out, variable.DefaultValue);
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::Key << "nodes" << YAML::Value << YAML::BeginSeq;
		for (const AvbNode& node : graph.Nodes)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "id" << YAML::Value << YAML::DoubleQuoted << node.Id;
			out << YAML::Key << "type" << YAML::Value << YAML::DoubleQuoted << node.TypeId;
			out << YAML::Key << "title" << YAML::Value << YAML::DoubleQuoted << node.Title;
			out << YAML::Key << "x" << YAML::Value << node.X;
			out << YAML::Key << "y" << YAML::Value << node.Y;
			out << YAML::Key << "literals" << YAML::Value << YAML::BeginSeq;
			for (const AvbPinLiteral& literal : node.Literals)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "pin" << YAML::Value << YAML::DoubleQuoted << literal.PinId;
				out << YAML::Key << "value" << YAML::Value; EmitLiteral(out, literal.Value);
				out << YAML::EndMap;
			}
			out << YAML::EndSeq << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::Key << "links" << YAML::Value << YAML::BeginSeq;
		for (const AvbLink& link : graph.Links)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "from" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "node" << YAML::Value << YAML::DoubleQuoted << link.FromNode;
			out << YAML::Key << "pin" << YAML::Value << YAML::DoubleQuoted << link.FromPin;
			out << YAML::EndMap;
			out << YAML::Key << "to" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "node" << YAML::Value << YAML::DoubleQuoted << link.ToNode;
			out << YAML::Key << "pin" << YAML::Value << YAML::DoubleQuoted << link.ToPin;
			out << YAML::EndMap;
			out << YAML::Key << "feedback" << YAML::Value << link.Feedback;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::Key << "sends" << YAML::Value << YAML::BeginSeq;
		for (const AvbSend& send : graph.Sends)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "from" << YAML::Value << YAML::DoubleQuoted << send.FromNode;
			out << YAML::Key << "to" << YAML::Value << YAML::DoubleQuoted << send.ToBus;
			out << YAML::Key << "mode" << YAML::Value << AvbSendModeName(send.Mode);
			out << YAML::Key << "gain" << YAML::Value << send.Gain;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq << YAML::EndMap;
		return out.c_str() ? std::string(out.c_str()) : std::string();
	}

	bool AvbGraphSerializer::Load(const std::string& path, AvbGraph& outGraph, AvbGraphError& error)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input) return Fail(error, "could not open .wlavb file");
		std::ostringstream contents;
		contents << input.rdbuf();
		if (!input.good() && !input.eof()) return Fail(error, "could not read .wlavb file");
		if (!Parse(contents.str(), outGraph, error)) return false;
		error = {};
		return true;
	}

	bool AvbGraphSerializer::Save(const std::string& path, const AvbGraph& graph, AvbGraphError& error)
	{
		namespace fs = std::filesystem;
		const std::string text = Emit(graph);
		const fs::path targetPath(path);
		static std::atomic<unsigned long long> tempSequence{ 0 };
		fs::path temporaryPath;
		bool temporaryCreated = false;
#ifdef _WIN32
		HANDLE temporaryFile = INVALID_HANDLE_VALUE;
#else
		int temporaryFile = -1;
#endif

		for (unsigned int attempt = 0; attempt < 128; ++attempt)
		{
			const auto timestamp = static_cast<unsigned long long>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			const unsigned long long sequence = tempSequence.fetch_add(1, std::memory_order_relaxed);
			temporaryPath = targetPath;
			temporaryPath += ".tmp-" + std::to_string(timestamp) + "-" + std::to_string(sequence);
#ifdef _WIN32
			temporaryFile = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
			                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (temporaryFile == INVALID_HANDLE_VALUE)
			{
				const DWORD openError = GetLastError();
				if (openError == ERROR_FILE_EXISTS || openError == ERROR_ALREADY_EXISTS)
					continue;
				return Fail(error, "could not create temporary .wlavb file");
			}
#else
		temporaryFile = ::open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
			if (temporaryFile < 0)
			{
				if (errno == EEXIST)
					continue;
				return Fail(error, "could not create temporary .wlavb file");
			}
#endif
			temporaryCreated = true;
			break;
		}
		if (!temporaryCreated)
			return Fail(error, "could not create a unique temporary .wlavb file");

		auto failAndRemoveTemporary = [&](const std::string& message)
		{
#ifdef _WIN32
			const BOOL deleteResult = DeleteFileW(temporaryPath.c_str());
			const DWORD deleteError = deleteResult ? ERROR_SUCCESS : GetLastError();
			const bool removed = deleteResult != FALSE || deleteError == ERROR_FILE_NOT_FOUND ||
			                    deleteError == ERROR_PATH_NOT_FOUND;
#else
			const bool removed = ::unlink(temporaryPath.c_str()) == 0 || errno == ENOENT;
#endif
			return Fail(error, removed ? message : message + "; could not remove temporary .wlavb file");
		};

		bool writeSucceeded = true;
#ifdef _WIN32
		std::size_t offset = 0;
		while (offset < text.size())
		{
			const std::size_t remaining = text.size() - offset;
			const DWORD chunk = static_cast<DWORD>(remaining > MAXDWORD ? MAXDWORD : remaining);
			DWORD written = 0;
			if (!WriteFile(temporaryFile, text.data() + offset, chunk, &written, nullptr) || written == 0)
			{
				writeSucceeded = false;
				break;
			}
			offset += written;
		}
		const bool flushSucceeded = writeSucceeded && FlushFileBuffers(temporaryFile) != FALSE;
		const bool closeSucceeded = CloseHandle(temporaryFile) != FALSE;
#else
		std::size_t offset = 0;
		while (offset < text.size())
		{
			const ssize_t written = ::write(temporaryFile, text.data() + offset, text.size() - offset);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
			{
				writeSucceeded = false;
				break;
			}
			offset += static_cast<std::size_t>(written);
		}
		const bool flushSucceeded = writeSucceeded && ::fsync(temporaryFile) == 0;
		const bool closeSucceeded = ::close(temporaryFile) == 0;
#endif
		if (!writeSucceeded)
			return failAndRemoveTemporary("could not write temporary .wlavb file");
		if (!flushSucceeded)
			return failAndRemoveTemporary("could not flush temporary .wlavb file");
		if (!closeSucceeded)
			return failAndRemoveTemporary("could not close temporary .wlavb file");

#ifdef _WIN32
		if (!MoveFileExW(temporaryPath.c_str(), targetPath.c_str(),
		                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return failAndRemoveTemporary("could not replace .wlavb file");
#else
		if (std::rename(temporaryPath.c_str(), targetPath.c_str()) != 0)
			return failAndRemoveTemporary("could not replace .wlavb file");
#endif
		error = {};
		return true;
	}
}
