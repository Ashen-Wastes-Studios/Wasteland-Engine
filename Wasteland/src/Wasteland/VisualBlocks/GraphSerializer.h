#pragma once

#include <string>
#include <vector>

namespace Wasteland::VisualBlocks
{

	// Forward declaration, NOT an include: Graph is only ever used through
	// a reference parameter here, so this header does not force every
	// includer to parse the full graph data model.
	class Graph;

	// A structured error, so a load failure names the file AND the offending
	// node rather than being a bool. "did not load" with no reason is the
	// single worst API in a visual language.
	struct GraphError
	{
		std::string Message;
		std::string NodeId;      // empty when not node-specific
		std::string PinId;
		// HasNode is the guard for NodeId rather than NodeId being empty --
		// a node with a legitimately empty id string is representable, and
		// testing the string would conflate that with 'no node'.
		bool HasNode = false;

		// Load-time errors are always errors. The Validator reuses this struct
		// for its findings and flips this to false for warnings -- a validator
		// that reports everything as an error trains people to ignore it.
		// Reused as the severity flag by Validator, which sets it false for
		// warnings. So one struct serves both the parse error path and the
		// static analysis path -- the coupling is why IsError has no setter
		// and must be assigned by the producer.
		bool IsError = true;

		// "file: node 'n3' (Branch): message" -- one string a designer can act on.
		std::string Format(const std::string &fileName) const;
	};

	// Load and save .wlblocks / .wlbtree.
	//
	// Both extensions share this implementation and the same data model; they
	// differ only in the `kind` field. Two formats would mean two
	// incompatible registries forever, which is the cost the plan refuses.
	//
	// **Every unknown key, unknown node type, and unknown pin type is a
	// reported error, never a silent drop.** A graph that quietly loses a node
	// on load is worse than one that refuses to load: the designer saves work
	// that silently stops existing, and finds out when the NPC misbehaves.
	class GraphSerializer
	{
	public:
		// Load a graph. Returns false and fills `error` on failure. Never
		// partially loads: a graph either arrives complete or not at all.
		// `outGraph` is left UNTOUCHED on failure -- the contract is all-or-
		// nothing, so a caller that ignores the bool cannot end up with half
		// a graph. That guarantee depends on Parse building into a local and
		// only assigning at the end.
		static bool Load(const std::string &path, Graph &outGraph, GraphError &error);

		// Parse from text, for tests and for an agent authoring graphs without
		// touching the filesystem.
		// Text-in rather than path-in, so this is the seam an AI agent authors
		// through and the same code path a loaded file takes. Having Parse and
		// Load diverge would mean the agent's graphs bypass the unknown-key
		// errors the format's whole design turns on.
		static bool Parse(const std::string &text, Graph &outGraph, GraphError &error);

		// Serialize to text. Deterministic: the same graph always produces
		// byte-identical output. A non-deterministic emitter makes every diff
		// and every test a lie, and a generated file that churns on every save
		// destroys its own usefulness as a reviewable artifact.
		// Determinism is a HARD requirement here, not a nicety:
		// BehaviorTree::SaveFile round-trips through Parse to decide whether a
		// graph is safe to write, so a non-deterministic Emit would make
		// saves fail intermittently rather than just churn the file.
		static std::string Emit(const Graph &graph);

		// Write to disk. Returns false with `error` set on failure.
		// Takes the graph by const reference and writes no state -- the
		// caller keeps ownership of the path, and Save does not itself choose
		// a filename.
		static bool Save(const std::string &path, const Graph &graph, GraphError &error);
	};

}
