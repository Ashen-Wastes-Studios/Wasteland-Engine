// PCH first -- it brings in std::filesystem and the standard headers
// the registry types below rely on.
#include "wlpch.h"

// The ValidationResult/GraphError declarations and the three
// PinType/PinDir constants every rule in this file branches on.
#include "Validator.h"

// DEFECT: <algorithm> is included but nothing in this file calls a
// std::algorithm -- the searches below are hand-written loops over
// graph.Links. Unused; harmless, but it implies a helper that is not
// here.
#include <algorithm>
// std::map gives the parent lists in CheckNoSharedChildren a defined
// iteration order, so the multi-parent error lists parents
// consistently between runs.
#include <map>
// std::set is load-bearing here: operator allow-listing and the
// cycle-detection onStack/done marks are all O(log n) membership
// tests over node ids.
#include <set>
// std::ostringstream for the human-readable report below.
#include <sstream>

// The node-type catalogue. Every per-node rule in this file looks its
// node up here, which is what makes a node's pins, role and
// statement-ness data rather than hardcoded.
#include "NodeRegistry.h"

// C++17 nested namespace. VisualBlocks keeps the graph validators
// from colliding with the runtime behaviour-tree code.
namespace Wasteland::VisualBlocks
{

	// "Did this graph fail?" -- the single question every caller asks
	// after Validate*(), so it short-circuits on the first error rather
	// than counting.
	bool ValidationResult::HasErrors() const
	{
		// Findings are held in one flat vector in discovery order, so HasErrors
		// can stop at the first error instead of scanning all of them.
		for (const GraphError &finding : Findings)
		{
			// One error anywhere is enough -- the caller only needs a yes/no to
			// decide whether the graph may be saved or run.
			if (finding.IsError)
				return true;
		}
		return false;
	}

	// Full count, unlike HasErrors(). ErrorCount() and WarningCount()
	// together account for every finding, since IsError is a plain bool.
	size_t ValidationResult::ErrorCount() const
	{
		// Zero-initialised so the accumulation below is a plain increment.
		size_t count = 0;
		for (const GraphError &finding : Findings)
		{
			if (finding.IsError)
				count++;
		}
		return count;
	}

	// The complement of ErrorCount(). Both walk the whole vector, so a
	// caller that needs both pays two passes -- Report below does exactly
	// that.
	size_t ValidationResult::WarningCount() const
	{
		size_t count = 0;
		for (const GraphError &finding : Findings)
		{
			// Warnings are counted by exclusion (IsError false) rather than by a
			// second flag, so a finding can never be neither error nor warning.
			if (!finding.IsError)
				count++;
		}
		return count;
	}

	// Renders the findings for a console or log line.
	std::string ValidationResult::Report(const std::string &fileName) const
	{
		// Accumulated into a stringstream and returned as one string, so the
		// caller does not have to own the buffer.
		std::ostringstream out;
		// DEFECT (minor): ErrorCount() and WarningCount() are each called
		// TWICE per summary -- once for the value and once for the plural
		// test -- so building this header walks the findings vector four
		// times. Harmless at this size, but it is the kind of thing that
		// becomes a profile hotspot once findings grow.
		out << fileName << ": " << ErrorCount() << " error"
			<< (ErrorCount() == 1 ? "" : "s") << ", " << WarningCount()
			<< " warning" << (WarningCount() == 1 ? "" : "s");
		// One line per finding, in the order they were discovered. That order
		// is why the checks run cheapest-and-most-fundamental first below:
		// an unknown node type makes its pins uncheckable, so it is
		// reported before any per-pin rule.
		for (const GraphError &finding : Findings)
		{
			// The severity word is chosen here rather than stored, because
			// GraphError carries IsError as a bool and both labels are derived
			// from it.
			out << "\n  " << (finding.IsError ? "error" : "warning") << ": "
				<< finding.Format(fileName);
		}
		// Copy out of the stream and destroy it. Report() is const, so this
		// is the only mutable object it creates.
		return out.str();
	}

	// The comparison-operator allow-list, because the emitter
	// interpolates the op straight into generated Python. This is a
	// genuine security-relevant check, not a nicety: an unvalidated op is
	// a code-injection hole in a generated file.
	bool IsAllowedOperator(const std::string &op)
	{
		// Function-local static, so the set is built once on first call and
		// then reused. String comparison is exact, hence the Unquote() at
		// the call sites.
		static const std::set<std::string> allowed = {
			"==", "!=", "<", "<=", ">", ">="
		};
		// find != end rather than count(): a bool question, and no count is
		// needed.
		return allowed.find(op) != allowed.end();
	}

	// Internal linkage for the helpers below. None of them are part of
	// the Validator API.
	namespace
	{
		// Error constructor. HasNode is derived from the id rather than passed
		// -- a finding with no node id (a whole-graph problem such as "no
		// root") is still a valid finding, and the distinction is what lets
		// the UI attach a marker to a node.
		GraphError MakeError(const std::string &message, const std::string &nodeId,
							 const std::string &pinId = "")
		{
			// Default-constructed GraphError, then field-by-field. Value
			// semantics throughout -- no heap, no shared ownership.
			GraphError error;
			// The raw text shown to the user. Kept verbatim (no formatting) so a
			// caller can build it from concatenation, as every call site does.
			error.Message = message;
			error.NodeId = nodeId;
			// PinId is optional -- many findings are about a whole node rather
			// than one pin, and those pass "".
			error.PinId = pinId;
			// An empty nodeId means "this finding is about the whole graph", so
			// HasNode is false and no node is highlighted.
			error.HasNode = !nodeId.empty();
			error.IsError = true;
			return error;
		}

		// Identical to MakeError except for IsError. Two constructors rather
		// than one with a bool parameter, so a call site cannot pass the
		// wrong severity by accident.
		GraphError MakeWarning(const std::string &message, const std::string &nodeId,
								const std::string &pinId = "")
		{
			// Identical body to MakeError. The duplication is deliberate so a
			// call site cannot pass the wrong severity as a bool.
			GraphError error;
			error.Message = message;
			error.NodeId = nodeId;
			error.PinId = pinId;
			error.HasNode = !nodeId.empty();
			error.IsError = false;
			return error;
		}

		// Strip the quotes ParseString adds, so a literal compares against its
		// bare text.
		// Strips the quotes the parser adds, so "'speed' > 3" compares
		// against the bare word speed. Only a matched pair is stripped: an
		// unterminated quote is returned unchanged rather than losing its
		// last character.
		std::string Unquote(const std::string &text)
		{
			// Both ends must be quotes AND the string must be at least two
			// characters. A lone " is returned unchanged rather than losing a
			// character to the substr.
			if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
				return text.substr(1, text.size() - 2);
			// Unquoted or empty input passes through untouched.
			return text;
		}

		// Two rules that hold for BOTH graph kinds: every node type must be
		// registered, and every literal must parse as its pin's declared
		// type. Shared so a flow graph and a behaviour tree cannot drift
		// -- a rule added for one applies to both.
		// Every node type must be registered, and every literal must parse as
		// its pin's declared type. Shared by both graph kinds.
		void CheckNodesRegisteredAndLiterals(const Graph &graph, ValidationResult &out)
		{
			// The registry is fetched once per call rather than per node; it is a
			// singleton, so this is a reference, not a copy.
			NodeRegistry &registry = NodeRegistry::Get();

			// Every node, every pin, every link -- the shared check is the
			// broadest loop nest in the file, which is why it runs first and
			// why the O(pins x links) scan below matters only at scale.
			for (const GraphNode &node : graph.Nodes)
			{
				// Looked up by type id. A null result means the graph names a node
				// type this build does not have -- a graph from a newer editor, or a
				// hand-edited file.
				const NodeDef *def = registry.Find(node.TypeId);
				// Unknown type: report and move on. Everything else in this
				// iteration would dereference a null NodeDef.
				if (def == nullptr)
				{
					// Reported as an ERROR, and the node is skipped entirely below:
					// without a NodeDef there are no pins to check, so continuing would
					// only produce confusing follow-on errors.
					out.Findings.push_back(MakeError(
						"unknown node type '" + node.TypeId +
							"' (it is not in the node registry)",
						node.Id));
					// Skips the literal checks too -- not just the pin rules -- because
					// they all need the definition.
					continue;
				}

				// Literals live on the node, not in graph.Links, so this is a direct
				// walk with no lookup.
				for (const PinLiteral &literal : node.Literals)
				{
					// The literal's KEY is looked up in the definition, so a stale key
					// from an older node version surfaces here rather than being
					// silently ignored.
					const PinDef *pinDef = def->FindPin(literal.Key);
					// A literal for a pin the definition no longer has. The graph is
					// from a different node version; reported rather than ignored so a
					// value is never silently dropped.
					if (pinDef == nullptr)
					{
						// A pin the current registry does not know about means the graph was
						// authored against a different node version. Reporting it beats
						// silently ignoring the value.
						// A pin the current registry does not know about means the
						// graph was authored against a different node version.
						// Reporting it beats silently ignoring the value.
						out.Findings.push_back(MakeError(
							"node type '" + node.TypeId + "' has no pin '" +
								literal.Key + "'",
							node.Id, literal.Key));
						// Move to the next literal, not the next node.
						continue;
					}
					// Skip unset literals (nothing to parse) and exec pins (they carry
					// flow, not a value). An exec pin with a stray literal is not
					// flagged here.
					if (!literal.IsSet || pinDef->Type == PinType::Exec)
						continue;

					// Two locals so a parse failure can produce a message and a parse
					// success leaves the error empty. `parsed` is written by
					// ParseLiteral but never inspected here.
					std::string parsed;
					std::string parseError;
					// ParseLiteral is the same routine the runtime uses, so validation
					// cannot disagree with execution about what a valid int looks like.
					// DEFECT: `parsed` receives the converted value and is then never
					// read -- the check is purely "did it parse".
					if (!ParseLiteral(pinDef->Type, literal.Value, parsed, parseError))
					{
						// The message names the expected type and the parser's own reason,
						// so the author does not have to guess which type was meant.
						out.Findings.push_back(MakeError(
							"pin '" + literal.Key + "' expects a " +
								PinTypeName(pinDef->Type) + ": " + parseError,
							node.Id, literal.Key));
					}
				}

				// Required, unlinked, unlit input pins. Reported, never defaulted: a
				// missing required pin must not become a zero that ships.
				// Required, unlinked, unlit pins. Reported, never defaulted:
				// a missing required pin must not become a zero that ships.
				for (const PinDef &pinDef : def->Pins)
				{
					// Only IN pins can be required -- an output pin the graph does not
					// read is legal and common.
					if (!pinDef.Required || pinDef.Dir != PinDir::In)
						continue;
					// Exec reachability is a separate concern with its own check per
					// graph kind; counting it here would double-report.
					if (pinDef.Type == PinType::Exec)
						continue;   // exec reachability is checked separately

					// An explicit literal satisfies the pin without needing a link.
					const PinLiteral *literal = node.FindLiteral(pinDef.Id);
					if (literal != nullptr && literal->IsSet)
						continue;

					// Starts false and is set by the scan below. A local rather than a
					// helper because the loop needs to keep searching for a SECOND
					// producer as well.
					bool linked = false;
					// Links are stored on the GRAPH, not on the node, so answering
					// "is this pin connected?" means scanning graph.Links. O(pins x
					// links) for the whole graph -- fine for a hand-authored tree, worth
					// knowing if graphs ever get large.
					for (const GraphLink &link : graph.Links)
					{
						// Both endpoints must match: a link to the right pin on a different
						// node does not satisfy this pin.
						if (link.ToNode == node.Id && link.ToPin == pinDef.Id)
						{
							// First match is enough -- for requiredness, one connection is as
							// good as two. (The duplicate-producer rule is flow-graph-only.)
							linked = true;
							break;
						}
					}
					// Neither connected nor given a literal value: the pin has no
					// source at all.
					if (!linked)
					{
						out.Findings.push_back(MakeError(
							"required pin '" + pinDef.Id + "' is neither connected "
							"nor given a value",
							node.Id, pinDef.Id));
					}
				}
			}
		}

		// Detect a cycle over the declared child edges. A tree must be a DAG
		// rooted at one node; a cycle would make the Compiler recurse forever.
		// The three-colour DFS below is the standard formulation: onStack
		// means "on the current path", done means "fully explored".
		// Detect a cycle over the declared child edges. A tree must be a DAG
		// rooted at one node; a cycle would make the Compiler recurse forever.
		bool DetectCycle(const Graph &graph, const std::string &startId,
						 std::set<std::string> &onStack, std::set<std::string> &done,
						 std::string &cycleAt)
		{
			// Re-entering a node already on the path is the cycle. cycleAt is set
			// to the node the cycle closes on, which is what lets the error point
			// at a specific node.
			if (onStack.count(startId) != 0)
			{
				// The node the cycle CLOSES on, which is what the error points at.
				cycleAt = startId;
				return true;
			}
			// Already fully explored on some earlier branch, so no cycle can be
			// reachable from here. The `done` set is what keeps this linear
			// rather than exponential.
			if (done.count(startId) != 0)
				// Already finished on an earlier branch -- proven acyclic.
				return false;

			// A child id naming a node that does not exist is treated as a leaf
			// rather than an error. Dangling child ids are reported by the
			// per-node checks in ValidateTree.
			const GraphNode *node = graph.FindNode(startId);
			// A child id naming a missing node terminates this branch cleanly.
			// Combined with the tree check's dangling-id rules, a typo'd child
			// produces a warning rather than a false cycle.
			if (node == nullptr)
				return false;

			// Push before recursing, pop after -- the invariant that makes the
			// onStack test above correct.
			onStack.insert(startId);
			// Recursion rather than an explicit stack: tree depth is
			// author-controlled and small, so the simpler formulation wins.
			for (const std::string &childId : node->ChildIds)
			{
				if (DetectCycle(graph, childId, onStack, done, cycleAt))
					return true;
			}
		// Popped BEFORE being marked done, so a sibling branch that reaches
		// this node sees it as fully explored rather than as a false cycle.
		// Popping the wrong order here is the classic way to get a
		// three-colour DFS wrong.
		onStack.erase(startId);
		// Marked done only after all children returned cleanly.
		done.insert(startId);
		// Reached only after every child returned false, so the whole
		// subtree rooted here is cycle-free.
		return false;
		}

	// A node reachable from TWO parents is evaluated once per parent, and
	// the compiler emits it as a single shared lambda -- so the second
	// visit repeats its entire effect.
	//
	// Authored by accident this is invisible in the editor (the node looks
	// connected exactly once) and it DOUBLES the observable damage: an
	// enemy with 'strike' listed on its Sequence AND inside its own
	// Cooldown deals 2x per tick and re-arms the cooldown twice. A tree
	// has no legitimate use for a shared child -- a real graph shares an
	// OUTPUT pin, never a node -- so report it rather than guessing which
	// parent was meant.
	// A node reachable from TWO parents is evaluated once per parent, and
	// the compiler emits it as a single shared lambda -- so the second
	// visit repeats its entire effect.
	//
	// Authored by accident this is invisible in the editor (the node looks
	// connected exactly once) and it DOUBLES the observable damage: an
	// enemy with 'strike' listed on its Sequence AND inside its own
	// Cooldown deals 2x per tick and re-arms the cooldown twice. A tree
	// has no legitimate use for a shared child -- a real graph shares an
	// OUTPUT pin, never a node -- so report it rather than guessing which
	// parent was meant.
	void CheckNoSharedChildren(const Graph &graph, ValidationResult &out)
	{
		// child id -> list of parent ids. A map (not a set) because the
		// duplicate case is the one being detected, and the parent NAMES are
		// needed for the error message.
		std::map<std::string, std::vector<std::string>> parents;
		// Build the parent map before any root logic, since the root
		// computation below needs the same child information.
		for (const GraphNode &node : graph.Nodes)
		{
			for (const std::string &childId : node.ChildIds)
			{
				// A child id naming a nonexistent node is skipped -- dangling ids are
				// reported elsewhere, and counting them here would produce a bogus
				// "child of 2 parents" error for a typo.
				if (graph.FindNode(childId) == nullptr)
					continue;   // dangling; reported elsewhere
				// push_back, not a set: a second parent for the same child is
				// precisely what must be preserved for the count to be right.
				parents[childId].push_back(node.Id);
			}
		}
		// std::map, so children are reported in id order -- the same
		// message every run for the same graph.
		for (const auto &entry : parents)
		// Zero or one parent is the normal case; only two or more is the
		// defect. Counting parents rather than testing membership keeps this
		// O(1) against a prebuilt map.
		{
			// Fewer than two parents is the normal case for a tree.
			if (entry.second.size() < 2)
				continue;
			// The parent names are joined for the message. A shared node usually
			// has exactly two parents, so this stays short in practice.
			std::string list;
			for (size_t k = 0; k < entry.second.size(); ++k)
			{
				if (k > 0)
					list += ", ";
				// Quoted so a node id containing a comma is still readable in the
				// message.
				list += "'" + entry.second[k] + "'";
			}
			out.Findings.push_back(MakeError(
				"node '" + entry.first + "' is a child of " +
					std::to_string(entry.second.size()) + " parents (" + list +
					"); it runs once per parent and repeats its effect",
				entry.first));
			}
		}
	}

	// -------------------------------
	// tree
	// -------------------------------
	// ------------------------------------------------------------------ tree

	// Behaviour-tree rules. Returns false when any ERROR was found;
	// warnings do not block.
	bool Validator::ValidateTree(const Graph &graph, ValidationResult &out)
	{
		// The registry, fetched once for the whole tree.
		NodeRegistry &registry = NodeRegistry::Get();

		// The two kind-independent checks run FIRST, so a graph with an
		// unknown node type reports that before any tree-specific rule tries
		// to interpret it.
		CheckNodesRegisteredAndLiterals(graph, out);
		// Second shared check. It runs after the registration/literal pass
		// so a graph with unknown types is reported for those first.
		CheckNoSharedChildren(graph, out);

		// Empty tree: nothing to check and no root to speak of. Returns early
		// because every rule below indexes roots[0].
		if (graph.Nodes.empty())
		{
			out.Findings.push_back(MakeError("a behaviour tree must have a root node", ""));
			// Returns before the root analysis, which would dereference
			// roots[0] on an empty graph.
			return !out.HasErrors();
		}

		// DEFECT: the comment above this block says "exactly one root", but
		// the code does not return after the roots.empty() error -- it falls
		// through to the roots.size() > 1 test and the per-node loop, which
		// is harmless (an empty `roots` fails both) but means an empty-tree
		// finding is not the only one reported.
		// --- exactly one root ---
		std::vector<const GraphNode *> roots;
		// Every id that appears as somebody's child, across all nodes.
		std::set<std::string> childOf;
		// Two passes over the nodes: collect every id that appears as somebody
		// else's child, then treat every node not in that set as a root.
		for (const GraphNode &node : graph.Nodes)
		{
			for (const std::string &childId : node.ChildIds)
				// Insert rather than assign: several parents naming the same child
				// collapse to one entry, which is all this set needs.
				childOf.insert(childId);
		}
		for (const GraphNode &node : graph.Nodes)
		{
			// Roots are nodes with no PARENT -- the definition that makes this work
			// for a tree rather than needing an explicit root flag. A node named
			// as a child of a nonexistent node still counts as a child, which is
			// why the dangling-id check above is separate.
			if (childOf.count(node.Id) == 0)
				// A pointer into graph.Nodes, not a copy -- so roots stays valid
				// without copying node data.
				roots.push_back(&node);
		}
		// No root means every node is somebody's child, i.e. the graph is a
		// pure cycle or has no entry point.
		if (roots.empty())
		{
			out.Findings.push_back(MakeError(
				"every node is somebody's child -- there is no root", ""));
		}
		// Two or more roots: the graph is a forest. Reported with the full list
		// -- there is no correct choice of which one to keep, so guessing
		// would be worse than saying so.
		else if (roots.size() > 1)
		{
			// Root ids joined for the message. Kept separate from the finding
			// so the error text itself stays a single expression.
			std::string names;
			for (const GraphNode *root : roots)
			{
				if (!names.empty())
					names += ", ";
				names += "'" + root->Id + "'";
			}
			out.Findings.push_back(MakeError(
				"a tree must have exactly one root, but these are all roots: " + names,
				// The finding is anchored to the FIRST root, which is arbitrary but
				// gives the UI something to highlight.
				roots[0]->Id));
		}

		// -------------------------------
		// cycles
		// -------------------------------
		// --- cycles ---
		std::set<std::string> onStack;
		// Per-call DFS state. `done` persists across the whole walk, which
		// is what keeps it linear.
		std::set<std::string> done;
		// Out-parameter, set only when a cycle is actually found -- so an
		// empty cycleAt after a true return would be a bug.
		std::string cycleAt;
		// Cycle detection runs only when there is exactly ONE root. With
		// several roots the trees are separate and the per-root walk would
		// have to start from each; with none there is nothing to start from.
		// Since both cases already reported an error, skipping here costs
		// nothing.
		if (roots.size() == 1 &&
			DetectCycle(graph, roots[0]->Id, onStack, done, cycleAt))
		{
			out.Findings.push_back(MakeError(
				"the tree contains a cycle through this node; a child list must "
				"form a tree, not a loop",
				cycleAt));
		}

		// -------------------------------
		// per-node rules
		// -------------------------------
		// --- per-node rules ---
		for (const GraphNode &node : graph.Nodes)
		{
			// Looked up again per node. A null def was already reported by the
			// shared check, so skipping avoids a duplicate.
			const NodeDef *def = registry.Find(node.TypeId);
			// Skip nodes the shared check already reported.
			if (def == nullptr)
				continue;   // already reported by the registration check

			// A node with no behaviour-tree role is a FLOW-graph node. Catching it
			// here means a node dragged in from a flow graph is reported by name
			// rather than silently never running.
			if (def->BtRole.empty())
			{
				// 'continue' after reporting, because a flow-graph node has no
				// meaningful BtRole and every rule below tests it.
				out.Findings.push_back(MakeError(
					"'" + node.TypeId + "' is a flow-graph node and cannot appear "
					"in a behaviour tree",
					node.Id));
				// Same reason: nothing below is valid for a node with no tree role.
				continue;
			}

			// Arity rules, and the reasoning behind which direction they point:
			//
			// A COMPOSITE takes many children -- running several sub-trees is
			// the entire point of Sequence and Selector. The mistake worth
			// catching is the OPPOSITE one: a Selector whose children are the
			// approach and the attack, where the author meant "one or the
			// other" and wrote them as a Sequence, which means "do both".
			// A composite with zero children is a different failure -- it can
			// never succeed or fail, so the tree stalls on it forever.
			//
			// A DECORATOR takes exactly one child, because it wraps a single
			// sub-tree and reinterprets its result. That is a real constraint.
			// Arity.
			//
			// A COMPOSITE takes many children -- running several sub-trees is
			// the entire point of Sequence and Selector. The mistake worth
			// catching is the OPPOSITE one: a Selector whose children are the
			// approach and the attack, where the author meant "one or the
			// other" and wrote them as a Sequence, which means "do both".
			//
			// A DECORATOR takes exactly one child, because it wraps a single
			// sub-tree and reinterprets its result. That is a real constraint.
			if (def->BtRole == "decorator" && node.ChildIds.size() != 1)
			{
				out.Findings.push_back(MakeError(
					"decorator '" + node.TypeId + "' wraps exactly one child, but "
					"has " + std::to_string(node.ChildIds.size()),
					node.Id));
			}
			// Zero children is the degenerate composite: no sub-tree to run, so
			// it can neither succeed nor fail.
			if (def->BtRole == "composite" && node.ChildIds.empty())
			{
				out.Findings.push_back(MakeError(
					"composite '" + node.TypeId + "' has no children, so it can "
					"never succeed or fail",
					node.Id));
			}

			// Conditions and tasks are leaves. Their sub-trees are meaningless --
			// a condition that had children would have to decide what their
			// success MEANS.
			// Conditions and tasks are leaves.
			if ((def->BtRole == "condition" || def->BtRole == "task") &&
				!node.ChildIds.empty())
			{
				out.Findings.push_back(MakeError(
					def->BtRole + " '" + node.TypeId + "' is a leaf and cannot have "
					"children",
					node.Id));
			}

			// The latent declaration. The compiler cannot infer whether a task
			// spans ticks, and a task that spans ticks but reports success on
			// tick one makes the tree stop on it -- the exact bug the 14.5
			// tests had to chase. So a known-latent task that forgot the flag
			// is a WARNING, not an error: the designer may have a legitimate
			// instant variant.
			// The latent declaration. The compiler cannot infer whether a task
			// spans ticks, and a task that spans ticks but reports success on
			// tick one makes the tree stop on it -- the exact bug the 14.5
			// tests had to chase. So a known-latent task that forgot the flag
			// is a WARNING, not an error: the designer may have a legitimate
			// instant variant.
			// Hardcoded type ids rather than a registry flag. DEFECT: if a node is
			// renamed in the registry these rules silently stop applying --
			// there is no "this node spans ticks" attribute to key off, so the
			// knowledge lives here in a string comparison.
			if (node.TypeId == "MoveTo" && !node.Latent)
			{
				out.Findings.push_back(MakeWarning(
					"MoveTo spans several ticks, so it must be marked latent. "
					"Without it the tree treats movement as complete on the "
					"first tick and never reaches a sibling that runs after it",
					node.Id));
			}
			// Same latent rule as MoveTo, and the same hardcoded-id coupling to
			// the registry.
			if (node.TypeId == "Wait" && !node.Latent)
			{
				out.Findings.push_back(MakeWarning(
					"Wait spans several ticks, so it must be marked latent",
					node.Id));
			}

			// ForceSuccess is UNCONDITIONAL. Wrapping a condition with it makes
			// the condition irrelevant and the parent succeed every tick -- a
			// tree that looks correct in the trace while doing nothing.
			// ForceSuccess is UNCONDITIONAL. Wrapping a condition with it makes
			// the condition irrelevant and the parent succeed every tick -- a
			// tree that looks correct in the trace while doing nothing.
			if (node.TypeId == "ForceSuccess" || node.TypeId == "ForceFailure")
			{
				// DEFECT: a ternary with a trailing comma, split across three lines:
				// `node.ChildIds.empty() ? nullptr : graph.FindNode(node.ChildIds[0]),`
				// The comma makes the second operand a comma-expression, which is
				// legal C++ but reads like a typo. It compiles to the same thing.
				const GraphNode *child = node.ChildIds.empty() ?
											 // The child is looked up, not assumed -- a decorator whose child id
											 // dangles yields nullptr and the warning below is skipped rather
											 // than dereferencing null.
											 nullptr :
											 graph.FindNode(node.ChildIds[0]);
				// Null means "no resolvable child", which is already an error
				// reported elsewhere.
				if (child != nullptr)
				{
					// The CHILD's definition, needed because the rule is about what is
					// being wrapped, not what is wrapping.
					const NodeDef *childDef = registry.Find(child->TypeId);
					// Only a CONDITION child is flagged. Wrapping a task or a composite
					// in ForceSuccess is redundant but harmless; wrapping a condition
					// is the mistake that makes the tree do nothing.
					if (childDef != nullptr && childDef->BtRole == "condition")
					{
						out.Findings.push_back(MakeWarning(
							node.TypeId + " around a condition ignores the condition "
							"entirely -- it returns " +
								(node.TypeId == "ForceSuccess" ? "SUCCESS" : "FAILURE") +
								" whatever the check says. Use the condition node "
								"directly to gate a branch",
							node.Id));
					}
				}
			}

			// Operator validation, because the emitter interpolates the op
			// straight into generated Python. An unvalidated op is a code
			// injection hole in a generated file -- this is the security-
			// relevant rule in the tree validator.
			// Operator validation, because the emitter interpolates the op
			// straight into generated Python. An unvalidated op is a code
			// injection hole in a generated file.
			// Same hardcoded-id pattern as the latent checks above: only these two
			// node types are known to carry an Op literal.
			if (node.TypeId == "ConditionCompare" || node.TypeId == "ConditionDistanceTo")
			{
				// The literal is optional. A ConditionCompare with no Op set is left
				// to whatever the emitter's default is -- NOT reported as an error,
				// so an unset operator is never validated.
				const PinLiteral *opLiteral = node.FindLiteral("Op");
				// An absent or unset Op is not validated at all -- there is nothing
				// to check, and whatever default the emitter applies is out of
				// this file's scope.
				if (opLiteral != nullptr && opLiteral->IsSet)
				{
					// Unquoted before comparison, because the allow-list holds bare
					// operator strings while the stored literal carries its quotes.
					std::string op = Unquote(opLiteral->Value);
					// The allow-list test. A rejected op is an ERROR because the
					// emitter would splice it straight into generated source.
					if (!IsAllowedOperator(op))
					{
						out.Findings.push_back(MakeError(
							"'" + op + "' is not a comparison operator "
							"(allowed: == != < <= > >=)",
							node.Id, "Op"));
					}
				}
			}
		}

		// -------------------------------
		// blackboard keys referenced by the tree must be declared
		// -------------------------------
		// An undeclared key reads as absent and a write to it is silently
		// lost, which shows up as an NPC that ignores what you told it to
		// remember.
		// --- blackboard keys referenced by the tree must be declared ---
		// An undeclared key reads as absent and a write to it is silently lost,
		// which shows up as an NPC that ignores what you told it to remember.
		for (const GraphNode &node : graph.Nodes)
		{
			// DEFECT: a single-element range-for written as if it were a list.
			// The braces around one string are the shape you would use for
			// several pins to check, and only "Target" is ever validated -- if
			// another node type gains a blackboard pin, nothing here notices and
			// nothing forces an update to this loop.
			for (const char *pin : { "Target" })
			{
				// An unset pin says nothing about blackboards, so it is skipped
				// -- including a pin the author deliberately blanked.
				const PinLiteral *literal = node.FindLiteral(pin);
				// An unset Target says nothing about the blackboard.
				if (literal == nullptr || !literal->IsSet)
					continue;
				// Unquoted, because the declared key in graph.Blackboard is stored
				// bare while the literal carries quotes.
				std::string key = Unquote(literal->Value);
				if (key.empty())
					continue;

				// Linear scan of the declared keys per referencing node. The key
				// lists are tiny, so a map would be noise.
				bool declared = false;
				// Linear scan per referencing node. Blackboard key lists are
				// hand-sized, so a map would be more code than it saves.
				for (const BlackboardKey &blackboard : graph.Blackboard)
				{
					if (blackboard.Name == key)
					{
						// Break on the first match -- one declaration is all that is
						// needed to make the reference valid.
						declared = true;
						break;
					}
				}
				// Undeclared after a full scan. Reported as an ERROR, unlike
				// unreachable nodes, because a silently-lost write is much harder
				// to diagnose.
				if (!declared)
				{
					out.Findings.push_back(MakeError(
						"references blackboard key '" + key +
							"', which is not declared in this tree",
						node.Id, pin));
				}
			}
		}

		return !out.HasErrors();
	}

	// -------------------------------
	// flow
	// -------------------------------
	// ------------------------------------------------------------------ flow

	// Flow-graph rules. Also returns false on any ERROR; unreachable
	// statements are warnings and do not block saving.
	bool Validator::ValidateFlow(const Graph &graph, ValidationResult &out)
	{
		// The registry, for the link-compatibility rules below.
		NodeRegistry &registry = NodeRegistry::Get();

		// Only the kind-independent checks run here. Note the asymmetry with
		// ValidateTree: CheckNoSharedChildren is deliberately NOT called
		// -- a flow graph legitimately shares a node (that is how a value is
		// used in two places), and only a behaviour tree's once-per-parent
		// -- evaluation makes sharing a defect.
		CheckNodesRegisteredAndLiterals(graph, out);

		// Exactly one event node, and it is the only legal entry point.
		// Exactly one event node, and it is the only legal entry point.
		const GraphNode *event = graph.FindEvent();
		// No event means the graph can never be triggered. Reported with no
		// node id because the cause is the absence of something.
		if (event == nullptr)
		{
			out.Findings.push_back(MakeError(
				"a flow graph must have exactly one event node "
				"(name an existing engine event in its Event field)", ""));
		}
		else
		{
			// A second event node is reported by name. Comparing node ADDRESSES
			// against the event pointer works because FindEvent returns a
			// -- pointer into this same vector, so it is the same object.
			for (const GraphNode &node : graph.Nodes)
			{
				// Address comparison, not id comparison: FindEvent returned a
				// pointer into this same vector, so identity is exact and cheap.
				if (&node != event && !node.EventName.empty())
				{
					out.Findings.push_back(MakeError(
						"a second event node ('" + node.EventName +
							"'); a flow graph has exactly one entry point",
						node.Id));
				}
			}
		}

		// -------------------------------
		// link type compatibility
		// -------------------------------
		// Link type compatibility.
		for (const GraphLink &link : graph.Links)
		{
			// Both endpoints resolved up front; a link with a missing end cannot
			// be type-checked.
			const GraphNode *from = graph.FindNode(link.FromNode);
			const GraphNode *to = graph.FindNode(link.ToNode);
			// Dangling links were already rejected at load, so skipping them here
			// -- reporting them again would duplicate the loader's message.
			if (from == nullptr || to == nullptr)
				continue;   // dangling links were already rejected at load

			// Definitions for both ends, needed because a pin's type and
			// direction are properties of its node type.
			const NodeDef *fromDef = registry.Find(from->TypeId);
			const NodeDef *toDef = registry.Find(to->TypeId);
			if (fromDef == nullptr || toDef == nullptr)
				continue;

			// Pins are resolved through each endpoint's own definition. Both
			// ends must resolve before any direction or type rule can run.
			const PinDef *outPin = fromDef->FindPin(link.FromPin);
			// The TARGET pin resolved separately from the source, since the two
			// ends are different nodes with different definitions.
			const PinDef *inPin = toDef->FindPin(link.ToPin);
			// Every failure below `continue`s rather than piling on: a link with
			// -- a bad source pin tells you nothing useful about its target pin.
			if (outPin == nullptr)
			{
				out.Findings.push_back(MakeError(
					"'" + from->TypeId + "' has no output pin '" + link.FromPin + "'",
					from->Id, link.FromPin));
				continue;
			}
			if (inPin == nullptr)
			{
				out.Findings.push_back(MakeError(
					"'" + to->TypeId + "' has no input pin '" + link.ToPin + "'",
					to->Id, link.ToPin));
				continue;
			}
			// Direction is checked from the definitions, not from the link
			// -- a link naming an input pin as its source is a data-flow error the
			// -- author can make by picking the wrong pin.
			if (outPin->Dir != PinDir::Out)
			{
				out.Findings.push_back(MakeError(
					"pin '" + link.FromPin + "' is an input, so it cannot be a "
					"link source",
					from->Id, link.FromPin));
				continue;
			}
			// A link into an output pin. Distinct from the exec rules below:
			// this one is about direction, not type.
			if (inPin->Dir != PinDir::In)
			{
				out.Findings.push_back(MakeError(
					"pin '" + link.ToPin + "' is an output, so it cannot be a "
					"link target",
					to->Id, link.ToPin));
				continue;
			}

			// Exec-to-exec is fine; exec-to-data is a hard error, and it is a
			// separate check because it is the mistake people actually make.
			// Exec-to-exec is fine; exec-to-data is a hard error, and it is a
			// separate check because it is the mistake people actually make.
			if (outPin->Type == PinType::Exec && inPin->Type != PinType::Exec)
			{
				out.Findings.push_back(MakeError(
					"exec wire cannot feed a data pin; '" + link.ToPin +
						"' is a " + PinTypeName(inPin->Type),
					to->Id, link.ToPin));
				continue;
			}
			// The mirror of the exec-to-data check above. Both are needed --
			// checking only one would let the other through.
			if (inPin->Type == PinType::Exec && outPin->Type != PinType::Exec)
			{
				out.Findings.push_back(MakeError(
					"a data wire cannot drive an exec pin; '" + link.ToPin +
						"' is exec",
					to->Id, link.ToPin));
				continue;
			}
			// Data-to-data goes through IsAssignable rather than requiring exact
			// -- type equality, so widening conversions are allowed.
			// DEFECT: this is the only rule in the loop that does NOT `continue`
			// -- harmless today (it is the last check), but any rule added after
			// -- it would run even for a link that already failed here.
			if (inPin->Type != PinType::Exec && !IsAssignable(outPin->Type, inPin->Type))
			{
				out.Findings.push_back(MakeError(
					"cannot connect " + std::string(PinTypeName(outPin->Type)) +
						" to " + PinTypeName(inPin->Type),
					to->Id, link.ToPin));
			}
		}

		// -------------------------------
		// one producer per input pin
		// -------------------------------
		// An input pin with two producers is ambiguous. Whichever link the
		// -- compiler happens to honour first would be an accident of
		// -- declaration order.
		// An input pin with two producers is ambiguous. Whichever link the
		// compiler happens to honour first would be an accident of declaration
		// order.
		for (const GraphNode &node : graph.Nodes)
		{
			// Per-node, so this is inside the producer-count walk rather than
			// the link loop above.
			const NodeDef *def = registry.Find(node.TypeId);
			if (def == nullptr)
				continue;
			// Driven by the DEFINITION's pin list rather than by the graph's
			// links -- so a pin with zero links is checked and simply passes.
			for (const PinDef &pinDef : def->Pins)
			{
				if (pinDef.Dir != PinDir::In)
					continue;
				// Counted rather than early-exited, because the error message needs
				// -- the total. O(pins x links) over the whole graph.
				int producers = 0;
				for (const GraphLink &link : graph.Links)
				{
					if (link.ToNode == node.Id && link.ToPin == pinDef.Id)
						// Every link into this pin, counted. Not short-circuited, because
						// the message needs the exact number.
						producers++;
				}
				if (producers > 1)
				{
					out.Findings.push_back(MakeError(
						"pin '" + pinDef.Id + "' has " + std::to_string(producers) +
							" connections; an input accepts exactly one",
						node.Id, pinDef.Id));
				}
			}
		}

		// A node with no path from the event never runs. Reported as a
		// warning: it is legal to keep unfinished scaffolding around, and an
		// error would block saving the work.
		// A node with no path from the event never runs. Reported as a warning:
		// it is legal to keep unfinished scaffolding around, and an error would
		// block saving the work.
		if (event != nullptr)
		{
			// Separate from the link-validation loop above: this asks a
			// reachability question, not a type question.
			std::set<std::string> reachable;
			// Reachability by iterative walk rather than recursion, so a large
			// graph cannot overflow the stack. `frontier` is used as a LIFO stack
			// (back/pop_back), which is a DFS -- order does not matter for
			// reachability.
			std::vector<std::string> frontier = { event->Id };
			while (!frontier.empty())
			{
				// back()/pop_back() makes this a LIFO, i.e. depth-first. Order is
				// irrelevant to reachability.
				std::string current = frontier.back();
				frontier.pop_back();
				// Skip nodes already reached. Without this a diamond-shaped graph
				// -- perfectly legal -- would enqueue the shared node once per path.
				if (reachable.count(current) != 0)
					continue;
				// Recorded on visit, not on enqueue -- so a node reachable by many
				// paths is expanded once.
				reachable.insert(current);
				// DEFECT: the walk follows ALL links, including data wires, so
				// "reachable" here means "connected by something", not "exec-
				// reachable". That is deliberate and compensated for below, but it
				// means this set alone cannot answer the exec question.
				for (const GraphLink &link : graph.Links)
				{
					// Follows data wires as well as exec wires; the isStatement filter
					// further down is what reconciles that.
					if (link.FromNode == current)
						frontier.push_back(link.ToNode);
				}
			}
			// Every node tested against the reached set, including ones with
			// no links at all.
			for (const GraphNode &node : graph.Nodes)
			{
				if (reachable.count(node.Id) != 0)
					continue;

				// A PURE EXPRESSION node (Add, LessThan, GetVariable) has no
				// exec pins at all: it is invoked by whatever consumes its
				// output, not by the exec chain. The walk above does reach it
				// through the data wire, but a node wired ONLY as a producer
				// with nothing consuming it is genuinely dead -- and an
				// expression node that some node does consume is alive.
				//
				// Reporting it as unreachable anyway trained authors to ignore
				// this warning, which is how the real dead-statement case --
				// the one worth a warning -- got lost in the noise.
				// A PURE EXPRESSION node (Add, LessThan, GetVariable) has no
				// exec pins at all: it is invoked by whatever consumes its
				// output, not by the exec chain. The walk above does reach it
				// through the data wire, but a node wired ONLY as a producer
				// with nothing consuming it is genuinely dead -- and an
				// expression node that some node does consume is alive.
				//
				// Reporting it as unreachable anyway trained authors to ignore
				// this warning, which is how the real dead-statement case --
				// the one worth a warning -- got lost in the noise.
				const NodeDef *def = registry.Find(node.TypeId);
				if (def == nullptr)
					continue;   // unknown type: reported elsewhere
				// A node counts as a statement if the registry says so OR if it has
				// -- any exec pin at all. The pin scan is the fallback for definitions
				// -- that never set the flag explicitly.
				bool isStatement = def->IsStatement;
				// Any exec pin makes a node statement-like, overriding a false
				// IsStatement flag.
				for (const PinDef &pin : def->Pins)
				{
					if (pin.Type == PinType::Exec)
						isStatement = true;
				}
				if (!isStatement)
					// Pure expression: no exec pin and not flagged as a statement, so
					// being unreached is by design.
					continue;   // an expression: not exec-routed by design

				// Only genuinely unreachable STATEMENT nodes are warned about. Pure
				// expressions are skipped by the check above, so this fires on the
				// dead-statement case the warning exists for.
				out.Findings.push_back(MakeWarning(
					"no exec path from the event reaches this node, so it will "
					"never run",
					node.Id));
			}
		}

		// Warnings never fail validation -- only errors do.
		return !out.HasErrors();
	}

	// Dispatches on the graph's declared kind. Anything that is not
	// exactly "tree" is validated as a flow graph, so a typo in a graph
	// file's kind field produces flow-graph diagnostics against a
	// behaviour tree rather than an "unknown kind" error.
	bool Validator::Validate(const Graph &graph, ValidationResult &out)
	{
		// Exact string match on the kind field. Anything else -- including a
		// typo or a new kind -- falls through to flow validation.
		if (graph.Kind == "tree")
			return ValidateTree(graph, out);
		return ValidateFlow(graph, out);
	}

}
