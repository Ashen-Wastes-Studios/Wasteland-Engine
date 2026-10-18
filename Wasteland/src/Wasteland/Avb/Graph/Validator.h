#pragma once

#include <string>
#include <vector>

#include "Graph.h"

namespace Wasteland::Avb
{
	struct AvbDiagnostic
	{
		std::string Message;
		std::string NodeId;
		std::string PinId;
		bool HasNode = false;
		std::string Format(const std::string& fileName = {}) const;
	};

	struct AvbValidationResult
	{
		std::vector<AvbDiagnostic> Findings;
		bool HasErrors() const noexcept { return !Findings.empty(); }
		std::string Report(const std::string& fileName = {}) const;
	};

	class AvbValidator
	{
	public:
		static bool Validate(const AvbGraph& graph, AvbValidationResult& out);
	};
}
