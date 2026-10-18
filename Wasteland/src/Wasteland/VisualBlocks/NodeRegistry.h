// Header guard over the deprecated #include-guard form; the codebase uses
// #pragma once throughout, which is safe with MSVC/Clang/GCC.
#pragma once

// std::function for the emitter callbacks in EmitContext and NodeDef --
// the node registry stores behaviour, not just data.
#include <functional>
#include <string>
// std::pair, used as the (pin id, expression) pair throughout the
// resolved-input and output containers. A string-keyed map would lose the
// pin order the emitter authored.
#include <utility>
#include <vector>

// PinDef comes from here (used by MakeIn/MakeOut below) as well as
// Graph and GraphNode, which EmitContext holds as pointers.
#include "Graph.h"

namespace Wasteland::VisualBlocks
{

	// What an emitter is given. Node authors return an expression or a
	// statement block; the Compiler owns scoping and indentation, so an author
	// never hand-writes the generated file.
	// Passed by const reference to every emitter. It is a plain struct with
	// function-pointer members rather than an interface, so a node author
	// gets everything in one aggregate and no vtable.
	struct EmitContext
	{
		// A POINTER and default-constructed null, not a reference: an emitter
		// must be able to exist without a graph (the validator constructs one
		// to inspect pins). The nullptr default makes that explicit.
		// DEFECT (weakly-checked contract, documented not fixed): nothing
		// forces an emitter to null-check this before dereferencing, so a
		// node author gets a crash rather than an error message.
		const Graph *Graph = nullptr;
		const GraphNode *Node = nullptr;

		// Resolved Python expression for each INPUT pin id. A linked pin
		// resolves to the producer's temp variable; an unlinked one to its
		// literal. Missing means the validator rejected it, so an emitter may
		// assume every required input is present.
		// A vector of pairs, not a map: pin ORDER is part of the resolved state,
		// and duplicate pin ids are prevented upstream. Emitters look pins up
		// by id via In() below rather than by index.
		std::vector<std::pair<std::string, std::string>> Inputs;

		// Temporary allocation. An emitter asking for a temp gets a unique,
		// stable name (`__t7`) and registers the expression to assign into it.
		// The naming is centralised so temps cannot collide and so re-emitting
		// the same graph produces identical text.
		// An injected callback rather than an owned counter: the Compiler holds
		// the counter, so temp names are unique per compilation and re-emitting
		// the same graph yields byte-identical output. That determinism is the
		// whole point -- a generated file that churns between saves destroys
		// any useful diff.
		std::function<std::string(const std::string &expression)> AllocTemp;

		// Current indent level, for nodes that emit statements.
		// Starts at 1, not 0: generated code goes inside a function body, so the
		// first level of statements is already one indent deep.
		int Indent = 1;

		// Look up a resolved input expression. Returns empty if absent.
		// Linear scan over Inputs. Fine for the pin counts a visual block has, and
		// simpler than maintaining a parallel index map.
		std::string In(const std::string &pinId) const;

		// Convenience: an input as a float literal expression. These fall back
		// to a neutral literal rather than an empty string, so a node body never
		// has to null-check its own pins -- by this point the validator has
		// already rejected a missing required pin.
		// The In* family exists so a node body never has to write a null check.
		// Each has a neutral fallback (0.0 for floats, False for bools), which
		// is safe rather than merely convenient: the validator has already
		// rejected a missing required pin before any emitter runs, so the
		// fallback is unreachable in a valid graph and only hides a defect in an
		// invalid one.
		std::string InFloat(const std::string &pinId) const;
		// Same contract as InFloat above.
		std::string InInt(const std::string &pinId) const;
		std::string InBool(const std::string &pinId) const;
		std::string InVec3(const std::string &pinId) const;
		// Entities are emitted as Python identifiers, so this is a different
		// kind of fallback from the numeric ones -- a missing entity pin has
		// no neutral value.
		std::string InEntity(const std::string &pinId) const;
	};

	// A node's output pins, filled by the emitter.
	// The mutable half of the emitter signature: output pins are written back
	// through it rather than returned, because a statement node has none.
	struct EmitOutputs
	{
		// pin id -> expression, mirroring the Inputs container's shape so the
		// Compiler can treat both the same way when wiring a link.
		std::vector<std::pair<std::string, std::string>> Outputs;  // pin id -> expression
	};

	// One node type: its pins, its category, and how it emits.
	// Value type copied into the registry (Register takes it by value). One
	// struct holds identity, presentation, and behaviour together --
	// deliberately, so a new node really is one registration.
	struct NodeDef
	{
		// The stable id written into saved tree files. This is the compatibility
		// surface: renaming one of these breaks every graph on disk, which is
		// what AliasOf below exists to avoid.
		std::string TypeId;      // "Branch", "SetVariable", ... (stable)
		// Purely for the editor's node palette grouping. Appears not to affect
		// validation or codegen.
		std::string Category;    // "Flow Control", "Scene", "Math", "Behaviour"
		// Pins carry their own type and required flag, so the validator reads them
		// from here rather than from a parallel table that could drift.
		std::vector<PinDef> Pins;

		// Emits the node's Python. Return an error string, empty on success.
		//
		// Two kinds:
		//  - EXPR nodes fill `out` with one expression per output pin.
		//  - STMT nodes return a statement block and may emit several lines
		//    (Branch, While, ForEach). They control flow, so they own their own
		//    successors -- see Compiler for how that is wired.
		// Returns std::string: an empty string means success and anything else is
		// an error message. Using the same type for both keeps the emitter
		// signature to two parameters with no Result type to thread.
		using EmitFn = std::function<std::string(const EmitContext &, EmitOutputs &)>;
		// DEFECT (unset emitter, documented not fixed): std::function is default-
		// constructed EMPTY, and there is no check here or in Register() that
		// Emit is callable. A NodeDef registered without an emitter would
		// throw std::bad_function_call at generation time rather than being
		// rejected at registration.
		EmitFn Emit;

		// The EXPR/STMT distinction the comment above describes, stored as one
		// bool rather than two node classes. A STMT node fills no Outputs.
		bool IsStatement = false;   // emits statements, not a single expression

		// Behaviour-tree role: "composite" | "decorator" | "condition" |
		// "task" | "". Drives the NB validator's arity rules.
		// Empty means "not a behaviour-tree node" -- an empty string is doing
		// double duty as both "unset" and "not applicable", which is why
		// FlowNodes() below tests for that empty value.
		std::string BtRole;

		// Non-empty when this type is another name for an existing node.
		//
		// Node ids are a FILE FORMAT -- saved trees name them -- so a rename
		// breaks every graph already on disk. An alias costs one entry and
		// keeps both spellings loadable. The registry resolves it on Find,
		// so an alias behaves exactly like its target everywhere.
		// A rename that does not break saved files: an alias entry costs one
		// registration and resolves on Find, so both spellings behave
		// identically downstream. Chained aliases would need cycle detection --
		// nothing here prevents an alias pointing at another alias.
		std::string AliasOf;

		// Returns a pointer into Pins, so it dangles if the NodeDef is copied or
		// the Pins vector reallocates. Callers must not hold it long.
		const PinDef *FindPin(const std::string &pinId) const;
		bool HasPin(const std::string &pinId) const;
	};

	// One table maps node type -> pins + emitter. This is THE extension point:
	// a new node is one registration, and it becomes available to the editor,
	// the validator, the codegen and the docs from that single entry.
	// A singleton holding every node type. One table for the editor, the
	// validator, codegen and docs is the design constraint -- two tables
	// would drift silently, as the comment above RegisterFlowNodes says.
	class NodeRegistry
	{
	public:
		// Accessor rather than a global object, so construction order does not
		// matter for translation units that touch nodes during static init.
		static NodeRegistry &Get();

		// The raw table, WITHOUT triggering self-registration. Public only so
		// RegisterAllNodes() can reach the instance it is in the middle of
		// filling; nothing else should call it. Prefer Get().
		// DEFECT (encapsulation escape hatch, documented not fixed -- and the
		// header itself says so): this hands out a mutable registry from a
		// const-looking accessor pair. The stated reason is that RegisterAllNodes
		// needs the instance it is filling, but the exposure is permanent and
		// any caller can now insert nodes at any time, including after the
		// editor has built its palette.
		static NodeRegistry &Mutable();

		// Returns a pointer into m_Defs. Any Register() that reallocates that
		// vector invalidates every pointer previously handed out.
		const NodeDef *Find(const std::string &typeId) const;
		// By reference, exposing the storage directly: a caller could const_cast
		// it, and more practically the reference is invalidated by any
		// Register(). The palette builder uses this once at startup.
		const std::vector<NodeDef> &All() const { return m_Defs; }

		// Behaviour-tree nodes only, in registration order.
		// Returns a filtered COPY of pointers, not a view. Callers get a snapshot;
		// registration order is preserved because it is registration order in
		// m_Defs that the palette relies on for stable display.
		std::vector<const NodeDef *> BehaviorNodes() const;

		// Flow-graph nodes only (no BtRole).
		// The complement of the above: nodes with an empty BtRole. Between the
		// two, every registered node lands in exactly one list -- provided
		// BtRole is exactly one of the four documented values, which nothing
		// enforces.
		std::vector<const NodeDef *> FlowNodes() const;

		// False for a type id no node ever registered. The validator reports
		// this rather than dropping the node.
		// Distinguished from Find()==nullptr so the validator can report an
		// unknown type rather than silently dropping the node.
		bool IsRegistered(const std::string &typeId) const;

		// BY VALUE: the caller keeps ownership of its NodeDef, so the registry
		// stores its own copy and the caller cannot mutate a registered node
		// behind the registry's back.
		void Register(NodeDef def);

	private:
		// Private, and NOT defaulted explicitly -- it is declared here so the
		// singleton in Get() is the only construction path. There is no code
		// here, so it is an empty ctor: the registry does not self-populate,
		// unlike some singletons that call an init routine.
		NodeRegistry();

		// DEFECT (linear registry, documented not fixed -- appears to be an
		// intentional tradeoff): Find and IsRegistered are documented as
		// lookups but this is a linear vector scan. For the current node count
		// that is not measurable, but it is why Find returns a pointer into a
		// container that reallocates on every registration.
		std::vector<NodeDef> m_Defs;
	};

	// Register everything. Idempotent, so a second call (a test, a reload)
	// replaces rather than duplicating -- duplicate registration would make
	// Find return whichever came first, non-deterministically.
	// Idempotent BY DESIGN, and the comment explains why that matters:
	// duplicate registration would make Find return whichever copy came
	// first, so a reload could silently swap which implementation a type
	// id resolves to.
	void RegisterAllNodes();

	// ---- pin helpers used when authoring a NodeDef ----

	// Factory helpers rather than aggregate initialisation at each call site:
	// they set the PinType that the type argument implies, so an exec pin
	// cannot be constructed with the wrong type by mistake.
	PinDef MakeExecIn(const std::string &id, const std::string &label = "");
	PinDef MakeExecOut(const std::string &id, const std::string &label = "");
	// required and defaultLiteral are separate, so a required pin can still
	// carry a literal used when the validator reports it missing. Note
	// the label has no default here, unlike the exec helpers above -- an
	// input pin is always labelled because the palette shows it.
	PinDef MakeIn(const std::string &id, const std::string &label, PinType type,
				  bool required = false, const std::string &defaultLiteral = "");
	PinDef MakeOut(const std::string &id, const std::string &label, PinType type);

	// Escape a string for emission as a Python literal. Node ids and pin
	// values reach generated code verbatim, so this is a quoting
	// boundary: an unescaped quote or backslash would let authored
	// content terminate the literal it sits in.
	// A QUOTING BOUNDARY, and the only defence between authored content and
	// generated Python. Node ids and pin values reach the output verbatim,
	// so an unescaped quote or backslash would let a value terminate the
	// literal containing it and inject arbitrary code into the generated
	// file. Worth auditing against every string-to-Python path.
	std::string EscapePy(const std::string &text);

	// Declare a composite/decorator node: ONE topology entry per node, with
	// its type, abort flag and any runtime keys. Children are STRUCTURE,
	// resolved by the Compiler from the graph's ChildIds -- not data pins.
	// The extra string is raw Python literal source (e.g. "'seconds': 0.5").
	// The behaviour-tree emitter helper. `extra` is raw Python SOURCE, not a
	// value -- callers must pass already-quoted literals such as
	// "'seconds': 0.5". That is a sharp edge: the string is spliced
	// into the output unescaped, so anything interpolated into it needs
	// EscapePy first.
	std::string EmitTreeNode(const EmitContext &ctx, EmitOutputs &out,
								 const char *type,
								 const std::string &extra = std::string());

	// The v1 behaviour taxonomy (NB.4), registered alongside the nodes
	// above. Split into its own translation unit because it is a
	// checklist item in its own right, and a file you can read against
	// the plan.
	// A separate TU purely for reviewability, per the comment above -- one
	// checklist item, one file you can read against the plan.
	void RegisterBehaviourV1Nodes();

	// The general-scripting palette (.wlblocks): variables, control flow,
	// math, logic and utility.
	//
	// Its own translation unit because a flow node's emitter has the
	// OPPOSITE contract to a tree node's. A tree node returns one dict entry
	// describing its position in topology data; a flow node returns real
	// Python control flow with placeholders where the compiler splices in a
	// body. Sharing a file made the two look interchangeable when they are
	// not. Same registry, same serializer, same validator -- never two of
	// those, because two drift silently.
	// Same registry, same serializer, same validator as the tree nodes, by
	// explicit choice: only the EMITTER CONTRACT differs (control flow with
	// body placeholders, versus a dict entry describing a topology slot).
	// Two registries would have been the tempting mistake.
	void RegisterFlowNodes();

}
