#pragma once

#include <string>

#include "Graph.h"
#include "GraphSerializer.h"
#include "Validator.h"

namespace Wasteland::VisualBlocks
{

	// Compiles a graph to a Python behavior class.
	//
	// What it emits is deliberately NOT nested control flow. It emits a
	// topology DICTIONARY that the shared wl_behavior runtime walks. That
	// indirection is the whole point: the tree must stay data so the runtime
	// can suspend and resume a specific latent node and re-enter from the root
	// to let a higher-priority branch pre-empt. Generating if/else instead
	// makes suspend/resume inexpressible, which is the one thing behavior
	// trees exist for.
	class Compiler
	{
	public:
		// Compile a validated graph. `error` is filled on failure; the returned
		// string is empty then.
		//
		// The graph MUST validate first -- the compiler assumes required pins
		// are present and operators are in the allowlist, because the emitters
		// interpolate them into generated code. Validating here too is defence
		// in depth for callers that skip it.
		static std::string CompileBehavior(const Graph &graph, GraphError &error);

		// Compile a flow graph to a Python component class.
		static std::string CompileFlow(const Graph &graph, GraphError &error);

		// Load, validate, then compile. The path NB.3 actually takes.
		static bool CompileBehaviorFile(const std::string &path, std::string &outPython,
										ValidationResult &outValidation, GraphError &error);

		// The class name `CompileBehavior` will emit for this graph.
		//
		// Public because the runtime has to instantiate what the compiler
		// produced, and the name is derived from the author-supplied graph
		// name, so it is SANITIZED -- a designer who names a tree "Enemy Chase
		// (v2)" gets Enemy_Chase__v2_. Re-deriving that rule at the call site
		// is how the two drift apart and the runtime silently fails to find a
		// class it just compiled.
		static std::string ClassNameFor(const Graph &graph);
	};

}
