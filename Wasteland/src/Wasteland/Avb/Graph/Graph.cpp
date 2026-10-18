#include "wlpch.h"

#include "Graph.h"

#include <atomic>
#include <iomanip>
#include <initializer_list>
#include <locale>
#include <sstream>

namespace Wasteland::Avb
{
	namespace
	{
		std::atomic<std::uint64_t> s_NextStableId{1};
	}

	const AvbLiteral* AvbNode::FindLiteral(const std::string& pinId) const noexcept
	{
		for (const AvbPinLiteral& literal : Literals)
			if (literal.PinId == pinId)
				return &literal.Value;
		return nullptr;
	}

	bool AvbGraph::AddNode(AvbNode node, std::string* error)
	{
		if (node.Id.empty())
		{
			do
			{
				node.Id = GenerateStableId();
			} while (FindNode(node.Id));
		}
		if (FindNode(node.Id))
		{
			if (error)
				*error = "duplicate node id '" + node.Id + "'";
			return false;
		}
		if (node.TypeId.empty())
		{
			if (error)
				*error = "node '" + node.Id + "' has an empty type id";
			return false;
		}
		Nodes.push_back(std::move(node));
		return true;
	}

	AvbNode* AvbGraph::FindNode(const std::string& id) noexcept
	{
		for (AvbNode& node : Nodes)
			if (node.Id == id)
				return &node;
		return nullptr;
	}

	const AvbNode* AvbGraph::FindNode(const std::string& id) const noexcept
	{
		for (const AvbNode& node : Nodes)
			if (node.Id == id)
				return &node;
		return nullptr;
	}

	std::string GenerateStableId()
	{
		const std::uint64_t value = s_NextStableId.fetch_add(1, std::memory_order_relaxed);
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << "avb-" << std::hex << std::setfill('0') << std::setw(16) << value;
		return out.str();
	}

	const char* AvbPinTypeName(AvbPinType type) noexcept
	{
		switch (type)
		{
			case AvbPinType::Trigger: return "Trigger";
			case AvbPinType::Audio: return "Audio";
			case AvbPinType::Time: return "Time";
			case AvbPinType::Float: return "Float";
			case AvbPinType::Int: return "Int";
			case AvbPinType::Bool: return "Bool";
			case AvbPinType::Enum: return "Enum";
			case AvbPinType::String: return "String";
			case AvbPinType::AssetRef: return "AssetRef";
		}
		return "<invalid-pin-type>";
	}

	bool AvbPinTypeFromName(const std::string& name, AvbPinType& out) noexcept
	{
		for (AvbPinType type : { AvbPinType::Trigger, AvbPinType::Audio, AvbPinType::Time,
		                         AvbPinType::Float, AvbPinType::Int, AvbPinType::Bool,
		                         AvbPinType::Enum, AvbPinType::String, AvbPinType::AssetRef })
		{
			if (name == AvbPinTypeName(type))
			{
				out = type;
				return true;
			}
		}
		return false;
	}

	const char* AvbPinDirName(AvbPinDir dir) noexcept
	{
		switch (dir)
		{
			case AvbPinDir::Input: return "in";
			case AvbPinDir::Output: return "out";
		}
		return "<invalid-pin-direction>";
	}

	bool AvbPinDirFromName(const std::string& name, AvbPinDir& out) noexcept
	{
		if (name == "in") { out = AvbPinDir::Input; return true; }
		if (name == "out") { out = AvbPinDir::Output; return true; }
		return false;
	}

	const char* AvbPinRateName(AvbPinRate rate) noexcept
	{
		switch (rate)
		{
			case AvbPinRate::Block: return "Block";
			case AvbPinRate::Sample: return "Sample";
		}
		return "<invalid-pin-rate>";
	}

	bool AvbPinRateFromName(const std::string& name, AvbPinRate& out) noexcept
	{
		if (name == "Block") { out = AvbPinRate::Block; return true; }
		if (name == "Sample") { out = AvbPinRate::Sample; return true; }
		return false;
	}

	const char* AvbChannelFormatName(AvbChannelFormat format) noexcept
	{
		switch (format)
		{
			case AvbChannelFormat::Mono: return "Mono";
			case AvbChannelFormat::Stereo: return "Stereo";
		}
		return "<invalid-channel-format>";
	}

	bool AvbChannelFormatFromName(const std::string& name, AvbChannelFormat& out) noexcept
	{
		if (name == "Mono") { out = AvbChannelFormat::Mono; return true; }
		if (name == "Stereo") { out = AvbChannelFormat::Stereo; return true; }
		return false;
	}

	const char* AvbSendModeName(AvbSendMode mode) noexcept
	{
		switch (mode)
		{
			case AvbSendMode::PreFX: return "PreFX";
			case AvbSendMode::PostFX: return "PostFX";
			case AvbSendMode::PostFade: return "PostFade";
		}
		return "<invalid-send-mode>";
	}

	bool AvbSendModeFromName(const std::string& name, AvbSendMode& out) noexcept
	{
		if (name == "PreFX") { out = AvbSendMode::PreFX; return true; }
		if (name == "PostFX") { out = AvbSendMode::PostFX; return true; }
		if (name == "PostFade") { out = AvbSendMode::PostFade; return true; }
		return false;
	}

	bool AvbLiteralMatchesType(const AvbLiteral& literal, AvbPinType type) noexcept
	{
		switch (type)
		{
			case AvbPinType::Time:
			case AvbPinType::Float:
				return std::holds_alternative<double>(literal.Value);
			case AvbPinType::Int:
				return std::holds_alternative<std::int64_t>(literal.Value);
			case AvbPinType::Bool:
				return std::holds_alternative<bool>(literal.Value);
			case AvbPinType::Enum:
			case AvbPinType::String:
			case AvbPinType::AssetRef:
				return std::holds_alternative<std::string>(literal.Value);
			case AvbPinType::Trigger:
			case AvbPinType::Audio:
				return false;
		}
		return false;
	}
}
