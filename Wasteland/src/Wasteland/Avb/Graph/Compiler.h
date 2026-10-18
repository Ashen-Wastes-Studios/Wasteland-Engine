#pragma once

#include <string>
#include <vector>

#include "Graph.h"
#include "Validator.h"

namespace Wasteland::Avb
{
	struct AvbCompiledGraph
	{
		// Non-owning node pointers in deterministic topological order. The source
		// AvbGraph must outlive this view. Output is canonical, path-independent JSON.
		std::vector<const AvbNode*> Nodes;
		std::string Output;
	};

	class AvbCompiler
	{
	public:
		static bool Compile(const AvbGraph& graph, AvbCompiledGraph& out,
		                    AvbValidationResult& validation);
	};
}
