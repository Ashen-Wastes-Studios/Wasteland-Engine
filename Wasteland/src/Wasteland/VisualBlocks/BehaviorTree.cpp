#include "wlpch.h"

#include "BehaviorTree.h"

#include <algorithm>

#include "Compiler.h"
#include "NodeRegistry.h"

namespace Wasteland::VisualBlocks
{

	// The type strings `wl_behavior.py` dispatches on.
	//
	// Kept here as data, and checked by the compiler, because the failure of
	// getting this wrong is invisible: an unknown type falls through to the
	// generic leaf handler, which looks for a `leaf` callable the runtime
	// primitive does not have, and returns FAILURE every tick. The tree still
	// ticks, the trace still shows a clean walk-through, and the NPC simply
	// never does the thing.
	// A function-local static in an inline-capable function: initialised
	// once on first use, thread-safe under C++11 magic statics. Returning
	// a REFERENCE to it avoids a vector copy on every query, at the cost
	// of a caller that stores the reference past a static teardown.
	const std::vector<std::string> &BehaviorTree::RuntimeTypes()
	{
		// The vector is built ONCE at first call, so RuntimeTypes() is cheap
		// after that -- but IsRuntimeType() below does a linear scan of it
		// for every node of every compile, which is O(nodes * 15). Fine at
		// graph sizes, and worth remembering if this ever runs per frame.
		static const std::vector<std::string> types = {
			// composites
			// Composites: all children must succeed, in order. Selector is the
			// fallback; Parallel is the fan-out. Three names because the three
			// evaluation policies are genuinely different, not stylistic.
			"Sequence", "Selector", "Parallel",
			// decorators
			// Decorators wrap a single child with a policy. Note Repeat and
			// Cooldown are here rather than being composites: they change WHEN
			// the child runs, not how many children are consulted.
			"Invert", "ForceSuccess", "ForceFailure", "Repeat", "Cooldown",
			"Timeout", "TickEvery",
			// runtime leaves -- handled by wl_behavior.py itself, NOT by a
			// generated lambda, so they carry no `leaf` key.
			// SetTimer and Wait are the two runtime LEAVES, and the comment above
			// records why they carry no `leaf` key: wl_behavior.py implements
			// them directly. A node type in this list therefore compiles to a
			// call into the generated module rather than to a generated lambda.
			"SetTimer", "Wait",
		};
		return types;
	}

	bool BehaviorTree::IsRuntimeType(const std::string &type)
	{
		const std::vector<std::string> &types = RuntimeTypes();
		// Linear find on a 15-element vector. Using a sorted vector plus
		// binary_search would be faster asymptotically and slower in practice
		// at this size -- the branch predictor handles 15 string compares
		// better than the setup cost of an ordered container.
		return std::find(types.begin(), types.end(), type) != types.end();
	}

	const std::vector<std::string> &BehaviorTree::CompositeTypes()
	{
		static const std::vector<std::string> types = {
			"Sequence", "Selector", "Parallel",
		};
		return types;
	}

	const std::vector<std::string> &BehaviorTree::DecoratorTypes()
	{
		static const std::vector<std::string> types = {
			"Invert", "ForceSuccess", "ForceFailure", "Repeat", "Cooldown",
			"Timeout", "TickEvery",
		};
		return types;
	}

	// The single entry point both CompileFile and any editor caller go
	// through. Three out-parameters is a lot, but each is a different
	// kind of output: text, findings, and a single fatal error.
	bool BehaviorTree::Compile(const Graph &graph, std::string &outPython,
							   ValidationResult &outValidation, GraphError &error)
	{
		// Clearing the outputs FIRST means a caller that ignores the return
		// bool still cannot observe stale results from a previous compile.
		outPython.clear();
		// Reset rather than clear-and-push: ValidationResult is reused by
		// the caller across compiles, so replacing it drops any capacity it
		// had already grown.
		outValidation = ValidationResult();

		// Kind is checked BEFORE validation. A flow graph shares the data
		// model but not the evaluator, so this is the earliest point at
		// which the mismatch is knowable.
		if (graph.Kind != "tree")
		{
			// A flow graph compiled as a tree would emit a class with no
			// `tick`, and the failure would surface at load time in the NPC
			// rather than here.
			error = GraphError();
			error.Message = "this graph's kind is '" + graph.Kind +
							"', not 'tree'; a behavior tree must declare "
							"kind: tree";
			return false;
		}

		// Stage 1: validate. Every finding, not just the first, so a designer
		// fixing a tree sees the whole list rather than one error per attempt.
		// Validation runs even when it will be reported wholesale: the whole
		// list is built so a designer sees every problem at once, then this
		// function reports only the first ERROR as the fatal one. Warnings
		// never block the compile.
		Validator::ValidateTree(graph, outValidation);
		if (outValidation.HasErrors())
		{
			// Scans the full findings list rather than stopping at the first
			// error, because outValidation already holds them all -- this loop
			// only has to find which one to surface as the single GraphError.
			for (const GraphError &finding : outValidation.Findings)
			{
				if (finding.IsError)
				{
					// Takes the FIRST error and returns, so the message names one node.
					// The complete set is still available in outValidation, which is the
					// point of collecting them all first.
					error = finding;
					break;
				}
			}
			return false;
		}

		// Stage 2: every node's type must be one the EVALUATOR knows, not just
		// one the registry knows. The registry and the runtime are different
		// tables, and a node in the first but not the second is the silent
		// per-tick failure described in the header.
		// Iterating graph.Nodes by reference: a node is validated for type
		// membership, not for its connections. Link-level problems are the
		// Validator's job, and duplicating them here would give two sources
		// of truth.
		for (const GraphNode &node : graph.Nodes)
		{
			if (IsRuntimeType(node.TypeId))
				continue;
			// Not a runtime type: it must therefore be a generated leaf, which
			// means its registry entry must exist and must not be a composite or
			// decorator role (those are runtime types, so a non-runtime type
			// claiming that role is a taxonomy error).
			// A null registry entry means the validator already reported the
			// unknown type. Continuing rather than returning keeps THIS function
			// from inventing a second, different message for the same problem.
			const NodeDef *def = NodeRegistry::Get().Find(node.TypeId);
			if (def == nullptr)
				continue;   // already reported by the validator
			// DEFECT (unreachable test): the branch above already `continue`d for
			// every type in RuntimeTypes(), and CompositeTypes()/DecoratorTypes()
			// are exact subsets of that list -- so a def whose BtRole is
			// composite or decorator necessarily has a type name that IS in
			// RuntimeTypes() and was skipped at line 99. The taxonomy error this
			// block reports can therefore never be detected here. It needs a
			// check against the REGISTRY's own role lists, not RuntimeTypes().
			if (!def->BtRole.empty() &&
				(def->BtRole == "composite" || def->BtRole == "decorator"))
			{
				error = GraphError();
				error.Message = "node type '" + node.TypeId +
								"' is registered as a " + def->BtRole +
								", but wl_behavior.py has no handler for it -- "
								"the runtime would treat it as a plain leaf and "
								"fail every tick";
				error.NodeId = node.Id;
				error.HasNode = true;
				return false;
			}
		}

		// Stage 3: emit.
		// The emitter returns an empty string on failure, with the reason in
		// `error` out-parameter. Testing the string rather than a bool keeps
		// the compiler's signature honest: there is no separate success flag
		// that could disagree with the output.
		outPython = Compiler::CompileBehavior(graph, error);
		// A false return with a NON-empty outPython would be possible only
		// if the emitter both wrote output and reported failure -- the empty
		// check is what makes those two agree.
		return !outPython.empty();
	}

	bool BehaviorTree::CompileFile(const std::string &path, std::string &outPython,
								   ValidationResult &outValidation, GraphError &error)
	{
		Graph graph;
		// Load then Compile, so the file path and the in-memory graph take
		// the identical validation path. Compiling from a file without this
		// indirection is what would let a save-time check disagree with an
		// edit-time one.
		if (!GraphSerializer::Load(path, graph, error))
			return false;
		return Compile(graph, outPython, outValidation, error);
	}

	bool BehaviorTree::SaveFile(const std::string &path, const Graph &graph,
								 GraphError &error)
	{
		// Refuse to save a graph that would not load back. Writing a broken
		// asset to disk is how a tree loses a node in a way the designer only
		// discovers the next time they open it.
		Graph roundTrip;
		// Emit then Parse -- the round trip. `roundTrip` is a throwaway: the
		// point is to prove the text parses, not to use the result.
		std::string text = GraphSerializer::Emit(graph);
		// Parse is called with the just-emitted text rather than the file,
		// so this tests SERIALISATION fidelity. It does not test that Save()
		// writes what Emit() produced -- a write failure surfaces separately
		// at the Save call below.
		if (!GraphSerializer::Parse(text, roundTrip, error))
		{
			// The inner error message is wrapped rather than replaced, so the
			// designer sees both the refusal and the underlying reason. Note
			// `error` was populated by the Parse above, which is what makes
			// embedding it here correct.
			error.Message = "refusing to save: this graph does not round-trip (" +
							error.Message + ")";
			return false;
		}
		return GraphSerializer::Save(path, graph, error);
	}

}
