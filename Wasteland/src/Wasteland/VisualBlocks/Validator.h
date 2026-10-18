#pragma once

// <sstream> is pulled in for Report()'s formatting, which is in the
// .cpp -- so this header pays for a dependency its inline code does
// not use, and every includer inherits that parse cost.
#include <sstream>
#include <string>
#include <vector>

#include "Graph.h"
#include "GraphSerializer.h"

namespace Wasteland::VisualBlocks
{

	// One validation finding. `Severity` separates "this will misbehave" from
	// "this is suspicious but compiles", because a validator that reports
	// everything as an error trains people to ignore it.
	enum class Severity
	{
		Error,   // will not compile, or compiles into something wrong
		Warning, // compiles, but is very likely not what the designer meant
	};

	struct ValidationResult
	{
		// A flat list rather than separate error and warning vectors: one
		// traversal preserves authoring ORDER, which is what makes Report()'s
		// output stable and reviewable. Sorting by severity instead would
		// put a trivial warning ahead of the error that matters.
		std::vector<GraphError> Findings;

		// Declared here, defined in the .cpp: HasErrors is called in a hot
		// path from BehaviorTree::Compile on every recompile, but the cost is
		// dominated by the traversal, not by the call.
		bool HasErrors() const;
		size_t ErrorCount() const;
		size_t WarningCount() const;

		// "file: 2 errors, 1 warning" plus one indented line each, so the whole
		// report is readable in a terminal without a second formatting pass.
		// Takes the file name by value and returns by value -- a report is
		// built and thrown away, never stored, so owning it costs nothing
		// and keeps the caller from having to manage a scratch buffer.
		std::string Report(const std::string &fileName) const;
	};

	// Structural and type checking for a loaded graph.
	//
	// The rule that drives the design: **report, never repair**. A validator
	// that substitutes a default for a missing required pin, or drops an
	// unknown node type, turns an authoring mistake into a silently wrong
	// runtime. The NPC still runs, the graph still "compiles", and the bug
	// surfaces days later as a design problem.
	class Validator
	{
	public:
		// Behaviour trees: arity per BtRole, the latent-declaration rule, the
		// single-root rule, cycle detection, and the blackboard checks.
		// static: no state between calls, so the validator is a pure
		// function of the graph. That is what makes it safe to run on the
		// editor thread while a game thread holds the same Graph.
		static bool ValidateTree(const Graph &graph, ValidationResult &out);

		// Flow graphs: type compatibility on every link, required pins, event
		// reachability, and exec wiring.
		// Same shape as ValidateTree: out-parameter, bool return. The bool
		// and outValidation overlap -- the bool is the convenience form of
		// `!out.HasErrors()`, and a caller using both must not read one as
		// authoritative over the other.
		static bool ValidateFlow(const Graph &graph, ValidationResult &out);

		// Dispatch on `graph.Kind`.
		static bool Validate(const Graph &graph, ValidationResult &out);
	};

	// The comparison operators a ConditionCompare / ConditionDistanceTo may
	// use. Checked at compile time so a node emitting `left <right python`
	// is impossible -- the emitter interpolates the op, so an unvalidated
	// value would be a code-injection hole in the generated file.
	// DEFECT (inconsistent linkage): a free function in a header with
	// NO inline and no `static`. If this is not defined `inline` in a
	// .cpp exactly once, every translation unit including Validator.h
	// emits its own definition and the link fails with a duplicate
	// symbol; if it is inline there, every TU gets its own copy and a
	// static-local cache would not be shared. Either way it does not
	// belong in a class with only static members.
	bool IsAllowedOperator(const std::string &op);

}
