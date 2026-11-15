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

		// The identifier sanitizer every generated name goes through.
		//
		// PUBLIC because a node's ResolvePins (Task B.2: CallFunction,
		// FunctionEntry) must derive the SAME Python identifier from the SAME
		// author-supplied name that the compiler does. A second copy of this
		// rule in NodeLibraryFlow would be a second answer to "what is the
		// method name for function 'My Func'?" -- and the two drifting apart
		// is a call node that compiles and then raises AttributeError.
		static std::string SanitizeIdentifier(const std::string &text,
											  const char *fallback);

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

		// The class name `CompileFlow` will emit for this graph.
		//
		// Separate from ClassNameFor because the two compilers use DIFFERENT
		// fallback strings: a tree falls back to "GeneratedBehavior" and a
		// flow graph to "GeneratedFlow". The flow emitter used to derive this
		// inline, with its own copy of the rule, and had no public accessor --
		// so a caller that had to instantiate a generated flow component could
		// not ask the compiler for the name and was left to re-derive it.
		// Re-deriving is how the two drift apart and the runtime silently
		// fails to find a class it just compiled.
		static std::string FlowClassNameFor(const Graph &graph);

		// The METHOD name `CompileFlow` will emit for this graph's event.
		//
		// The emitter names the generated method after the event node's
		// EventName, sanitised, falling back to "OnUpdate" when the graph has
		// no event node or its name is blank. The runtime has to CALL that
		// method, so it must ask the compiler rather than re-deriving the
		// sanitisation -- the same drift risk as the class name, and the same
		// reason it is not re-implemented at the call site.
		static std::string FlowMethodNameFor(const Graph &graph);

		// The METHOD name `CompileFlow` will emit for ONE event node.
		//
		// The per-node form, as opposed to the per-graph form above: a
		// schema-2 graph emits one method per event, so the emitter needs the
		// name for a specific node. Both forms share this one sanitisation
		// rule, which is why neither re-implements it.
		static std::string FlowMethodNameFor(const GraphNode &event);
	};

}
