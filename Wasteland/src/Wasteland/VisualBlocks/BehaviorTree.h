#pragma once

#include <string>
#include <vector>

#include "Graph.h"
#include "GraphSerializer.h"
#include "Validator.h"

namespace Wasteland::VisualBlocks
{

	// The `.wlbtree` compiler: load, validate, emit.
	//
	// This is a FACADE, not a second implementation. The serializer, the
	// registry, the validator walk and the Python emitter are all shared with
	// Visual Blocks, which is the whole point -- two registries would mean two
	// incompatible node taxonomies forever, and a graph authored in one would
	// be un-loadable in the other. What lives here is only the behaviour-tree
	// SHAPE: the runtime-type contract and the end-to-end entry point.
	class BehaviorTree
	{
	public:
		// ---- the runtime type contract ----
		//
		// `wl_behavior.py` dispatches on the node's `t` string. A type the
		// evaluator does not know falls through to the generic leaf handler,
		// which needs a callable `leaf` the runtime primitive does not have --
		// so it fails every tick, silently, with a clean-looking trace.
		//
		// This list is the contract, and `IsRuntimeType` is what the compiler
		// checks a node's type against. Adding a runtime primitive to
		// wl_behavior.py without adding it here is the failure mode it exists
		// to prevent.
		static const std::vector<std::string> &RuntimeTypes();
		static bool IsRuntimeType(const std::string &type);

		// Types the evaluator handles as composites / decorators, used to
		// confirm a graph's structure matches what the runtime will do.
		static const std::vector<std::string> &CompositeTypes();
		static const std::vector<std::string> &DecoratorTypes();

		// ---- the end-to-end path ----

		// Load a `.wlbtree`, validate it, and emit Python.
		//
		// Returns false with `error` set on ANY failure, and never returns
		// partially-emitted code: the three stages are the same three the plan
		// names, run in order, because a graph that fails validation must not
		// reach the emitter -- the emitter interpolates operator strings and
		// literals into generated Python, so validating after emitting would
		// mean writing a file first and checking it second.
		static bool CompileFile(const std::string &path, std::string &outPython,
								ValidationResult &outValidation, GraphError &error);

		// Same, from an in-memory graph.
		static bool Compile(const Graph &graph, std::string &outPython,
							ValidationResult &outValidation, GraphError &error);

		// Save a graph back to `.wlbtree`. Round-tripping must preserve
		// `abort`, latent flags, child order and every literal, or a
		// designer who opens and saves a tree silently changes it.
		static bool SaveFile(const std::string &path, const Graph &graph,
							 GraphError &error);
	};

}
