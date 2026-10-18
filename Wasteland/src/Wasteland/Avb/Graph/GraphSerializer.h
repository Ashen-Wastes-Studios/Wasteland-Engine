#pragma once

#include <string>

namespace Wasteland::Avb
{
	struct AvbGraph;

	struct AvbGraphError
	{
		std::string Message;
		std::string NodeId;
		std::string PinId;
		bool HasNode = false;
		std::string Format(const std::string& fileName) const;
	};

	// Strict .wlavb YAML IO. Parse is transactional: outGraph is untouched on error.
	class AvbGraphSerializer
	{
	public:
		static bool Parse(const std::string& text, AvbGraph& outGraph, AvbGraphError& error);
		static bool Load(const std::string& path, AvbGraph& outGraph, AvbGraphError& error);
		static std::string Emit(const AvbGraph& graph);
		static bool Save(const std::string& path, const AvbGraph& graph, AvbGraphError& error);
	};
}
