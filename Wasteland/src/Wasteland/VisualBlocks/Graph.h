#pragma once

// DEFECT (unused include): nothing in this header names an int16_t,
// uint8_t or any other fixed-width type. Every field is std::string,
// float, int or an enum. The include is harmless but suggests this
// header once had a serialised-bytes representation that was replaced
// by the text-based serializer.
#include <cstdint>
#include <string>
// Directly used by the container aliases below and by
// GraphNode::ChildIds on line 160, which is a raw
// std::vector<std::string> rather than one of the named aliases --
// see the note there.
#include <vector>

namespace Wasteland::VisualBlocks
{

	// The wire and value types.
	//
	// Exec is NOT a value -- it is control flow, and it has different rules
	// (no implicit conversions, order matters, exactly one producer). Giving
	// it a slot in the same enum as data types is what makes "an exec wire
	// connected to a data pin" expressible as a type error rather than a
	// special case threaded through the validator.
	// Scoped, so PinType::Exec cannot collide with anything else.
	// DEFECT (ordinal stability, same class as the comment on Any below):
	// the comment above warns that the ordinals are persisted in saved
	// .wlbtree/.wlblocks files. But nothing here writes PinType to disk
	// as an integer -- the serializer stores type NAMES (PinTypeName /
	// PinTypeFromName exist for exactly that, lines 230-231). If that is
	// the only persistence path, the ordinal-stability constraint on Any
	// is being observed defensively rather than out of necessity. Worth
	// confirming against the serializer before relying on it.
	enum class PinType
	{
		// Listed FIRST, so PinType::Exec has ordinal 0. That makes it the
		// default-constructed value, since no enumerator has an explicit
		// initialiser -- a PinType default-initialised anywhere in the
		// engine would silently be Exec, not Float. Every PinType member in
		// this header DOES carry an explicit = PinType::Float default (lines
		// 82, 181, 191), which suggests the 0 value was noticed.
		Exec,
		Bool,
		Int,
		Float,
		String,
		Vec2,
		Vec3,
		Vec4,
		// A handle to a scene object rather than a value type. It sits in the
		// same enum as the arithmetic types, so the validator needs a special
		// case for it -- the comment above the enum says the same about Exec.
		// No conversion to or from the numeric types is offered by
		// IsAssignable (line 236), which is the mechanism that enforces that.
		Entity,

		// A wildcard DATA pin: accepts any value type from any producer and
		// adopts it. APPENDED, never inserted -- the ordinals are persisted in
		// saved .wlbtree/.wlblocks files, so renumbering would silently
		// reinterpret every graph already on disk.
		//
		// SetVariable.Value needs this. It is the one pin whose type is the
		// AUTHOR's choice (it is whatever the named variable holds), so pinning
		// it to Float makes `flag = (a == b)` unexpressible: the bool cannot
		// connect to a float pin, and the graph fails validation for wanting to
		// store a comparison result. Sizing that pin by the variable's declared
		// type instead would need the registry to read graph state, which no
		// other pin does.
		Any,
	};

	// Two-valued, so unlike PinType there is no ordinal-stability
	// concern: nothing persists a direction as an integer.
	enum class PinDir
	{
		// Input first, again making it the zero value. A PinDir with no
		// explicit initialiser would default to In -- but PinDef::Dir on
		// line 83 does carry = PinDir::In, so the default is stated.
		In,
		Out,
	};

	// ---- container aliases ----
	//
	// Forward-declared element types, because the aliases must precede the
	// structs that use them (GraphNode holds a PinLiteralList) and the structs
	// are what the aliases name.
	//
	// Named types rather than bare std::vector so pybind11 can be told to
	// treat THEM as opaque (live, mutable) while every other std::vector in
	// the module keeps stl.h's by-value list conversion. See the comment on
	// PYBIND11_MAKE_OPAQUE in Bindings.cpp: without this, graph.Nodes.append
	// silently appends to a copy and authoring a graph from Python does
	// nothing at all.
	struct GraphNode;
	struct GraphLink;
	struct GraphVariable;
	struct BlackboardKey;
	struct PinLiteral;

	// The alias exists for a pybind11 reason, documented at length in the
	// block comment above: making these opaque is what stops
	// graph.Nodes.append from silently mutating a copy.
	// DEFECT (incomplete type): at this point GraphNode is only
	// forward-declared, so instantiating std::vector<GraphNode> instantiates
	// a vector of an incomplete type. That is legal to DECLARE and
	// ill-formed to use until GraphNode is complete on line 128 -- which
	// works because nothing here instantiates the vector. Any code that
	// tried to create one at namespace scope would fail to compile.
	using GraphNodeList = std::vector<GraphNode>;
	using GraphLinkList = std::vector<GraphLink>;
	using GraphVariableList = std::vector<GraphVariable>;
	using BlackboardKeyList = std::vector<BlackboardKey>;
	using PinLiteralList = std::vector<PinLiteral>;

	// A node's declared pin. `Id` is the stable, persisted identity — NEVER
	// the index. A node gaining a pin must not break every saved graph that
	// used the old layout, which is the single most important format decision
	// in the system and the easiest to get wrong.
	// The node TYPE's declaration of a pin -- the schema half. The
	// per-instance half is GraphInput on line 115. Keeping them separate is
	// what lets one node type serve many instances with different values.
	struct PinDef
	{
		// DEFECT (no default): every other string in this header either has
		// an in-class default or is a required constructor parameter. A
		// PinDef default-constructed here would have an empty Id, and every
		// lookup is by Id -- so an empty Id silently matches any other empty
		// Id. There is no default constructor on PinDef, so this only bites a
		// caller who aggregate-initialises one.
		std::string Id;
		std::string Label;      // shown to the designer
		// Float as the default type, not Exec -- so a PinDef that forgets to
		// set Type is a data pin rather than a control-flow pin. The safer
		// default of the two for a validator to have to reject.
		PinType Type = PinType::Float;
		// Input by default. An output pin is the exceptional case and has to
		// be asked for, which is the safer direction: a forgotten Dir means
		// the pin cannot feed anything, rather than silently driving whatever
		// links to it.
		PinDir Dir = PinDir::In;

		// A required pin with neither a link nor a literal is a COMPILE ERROR
		// naming the node and pin, not a silent default. Silent defaults are
		// how a graph that looks correct produces an NPC that does nothing.
		// False by default, so a pin is optional unless declared otherwise.
		// The comment above explains why a required-but-unconnected pin is a
		// compile error rather than a default value.
		bool Required = false;

		// Default for an unconnected, non-required input pin. Stored as text
		// and parsed per-type at compile time, so a malformed literal is a
		// reported error rather than a crash or a silent zero.
		// Text, not a parsed value. The comment above explains why: parsing
		// happens at compile time so a malformed literal is a reported error.
		// DEFECT (ambiguity): an EMPTY string here is indistinguishable from
		// "no default authored". ParseLiteral on line 245 would be handed "" for
		// both, and whether that is an error depends on the type. There is no
		// HasDefault companion bool, unlike PinLiteral::IsSet on line 110
		// which solves exactly this problem for the per-instance case.
		std::string DefaultLiteral;
	};

	// A literal value bound to an input pin.
	//
	// Stored as TEXT and parsed per-type at compile time, so a malformed value
	// is a reported error rather than a crash or a silent zero.
	//
	// `Key` is the PIN ID the literal belongs to -- pins are addressed by
	// stable string id, never by index, so a node gaining a pin cannot break a
	// saved graph. `IsSet` distinguishes "authored as 0" from "not authored",
	// which matters because 0 is a perfectly good float; treating it as absent
	// is the same class of bug as a blackboard key holding 0 reading as unset.
	// Per-instance literal, keyed by pin id. Distinct from PinDef::
	// DefaultLiteral above: this is what the AUTHOR typed on this
	// instance, that is the type-level default.
	struct PinLiteral
	{
		std::string Key;      // pin id
		std::string Value;    // raw text as authored
		// The presence flag. The comment above is right that 0 is a valid
		// float, so the value alone cannot signal absence.
		// Set by SetLiteral (line 165), whose isSet parameter defaults to
		// true -- so calling SetLiteral with an empty value still marks it set,
		// which is the correct behaviour and the reason for the explicit flag
		// rather than a non-empty-string test.
		bool IsSet = false;
	};

	// An input pin on an instance: the resolved literal, or the id of the node
	// + pin feeding it. Exactly one of these is meaningful per input pin.
	// The RESOLVED input state on an instance: either a literal or a
	// link.
	// DEFECT (redundant state): this struct carries BOTH a Literal with
	// a HasLiteral bool AND a LinkedNode/LinkedPin pair with an IsLinked
	// bool, and the comment says "exactly one of these is meaningful".
	// Nothing in the type prevents both being set or both being clear, and
	// the clear case is genuinely ambiguous. Graph::ResolveInput
	// (line 221) returns outIsLinked plus all the out-parameters, so the
	// same four-way ambiguity exists in the resolution path. A
	// std::variant would express this exactly. Not fixed here.
	struct GraphInput
	{
		// The trailing comment states the precedence rule: an empty
		// LinkedNode is what makes the literal authoritative. That makes
		// LinkedNode the single source of truth for which branch is live, and
		// the HasLiteral/IsLinked flags redundant with it.
		std::string Literal;         // when LinkedNode is empty
		bool HasLiteral = false;

		std::string LinkedNode;      // producer node id
		std::string LinkedPin;       // producer pin id
		// False by default, matching HasLiteral -- an unconnected pin is
		// neither linked nor literal-bearing until something says so.
		bool IsLinked = false;
	};

	// An instance of a node type. `TypeId` names a registry entry; the rest is
	// per-instance state (position for the canvas, literal values, and a
	// flag the emitter honours).
	// An instance. The Id/TypeId split is the module's core distinction:
	// TypeId names a registry entry, Id names this instance, and links
	// reference Ids.
	struct GraphNode
	{
		// The stability requirement is stated three times in this header
		// (here, line 74 and line 101). It is the load-bearing invariant of
		// the whole format.
		std::string Id;              // stable, referenced by links and children
		std::string TypeId;          // "Branch", "SetVariable", ...

		// A user-supplied name for this INSTANCE, shown in the node header
		// in place of TypeId. Empty means "use TypeId", so existing graphs
		// are unaffected and nothing has to be back-filled. Purely an author-
		// facing label: the compiler and validator address nodes by TypeId,
		// never by this, so renaming can never change behaviour.
		std::string Title;
		// Zero-initialised, and the second line continues the comment for Y.
		// Both are canvas coordinates: authoring-only, never emitted into the
		// compiled Python, so they cannot affect behaviour.
		// DEFECT (dangling comment): the two trailing comments say "canvas
		// position; authoring metadata," and "NOT semantic", but there is NO
		// comment attached to X and Y explaining what they actually DO beyond
		// being a position -- in particular nothing says which is X and which
		// is Y, nor the unit. A reader has to guess from the names.
		float X = 0.0f;              // canvas position; authoring metadata,
		float Y = 0.0f;              // NOT semantic

		// Event graphs name the engine hook they hang off ("OnUpdate",
		// "OnInteract"). Only one node per event may exist per graph.
		// Only meaningful for a flow graph; empty for a tree node.
		// The comment above says only one event node may exist per graph,
		// and Graph::FindEvent (line 217) is what enforces that at read time.
		// DEFECT: nothing on this struct marks the node as an event node.
		// A non-empty EventName is the only signal, which means a typo here
		// produces a node that is simply not found by FindEvent rather than a
		// diagnostic.
		std::string EventName;

		// Behaviour-tree nodes (NB): whether the leaf may return RUNNING.
		// Declared, never inferred.
		// The comment says "declared, never inferred", which is the right
		// call: inferring it would make graph behaviour depend on analysis.
		// NB the abbreviation -- this is behaviour-tree terminology for a
		// node that may return RUNNING, and it has no analogue in the flow-graph
		// half of the format.
		bool Latent = false;

		// Behaviour-tree abort mode: "none" | "self" | "lower".
		// A STRING enum, not an enum class. The three legal values are
		// written in a comment ("none" | "self" | "lower") rather than encoded
		// as enumerators, so nothing rejects a typo at compile time.
		// DEFECT: an unrecognised value -- "Self", "none ", a typo -- is
		// stored and only fails wherever the abort mode is read. There is no
		// parse function for it in the type-helper block below, unlike the Pin
		// types which have PinTypeFromName and ParseLiteral. That asymmetry is
		// the real gap: PinType got validation helpers and this string enum did
		// not.
		std::string Abort = "none";

		// Node-specific literal values, keyed by PIN ID (not index).
		// The declared container alias, so pybind11 treats it opaquely.
		// A vector rather than a map keyed by pin id: the key is duplicated
		// inside each PinLiteral::Key (line 108), so lookups are linear. For a
		// node with a handful of pins that is fine, and it keeps insertion
		// order stable for the serializer.
		PinLiteralList Literals;

		// Behaviour-tree children, in PRIORITY ORDER (first is highest).
		// Declared here rather than derived from links because a tree's
		// structure IS its priority, and a link list cannot express "first
		// declared wins" without an explicit order field anyway.
		// DEFECT (alias not used): this is a raw std::vector rather than one
		// of the named container aliases declared above. The alias block gives
		// GraphNodeList/GraphLinkList/etc. for the pybind11 opacity reason and
		// this vector misses out -- so PYBIND11_MAKE_OPAQUE may not cover it,
		// and graph.Nodes[0].ChildIds.append from Python would have the same
		// silently-appends-to-a-copy hazard the alias block was written to
		// avoid.
		// The ordering semantics are load-bearing: first is highest priority,
		// which a link list could not express. Noted in the comment above.
		std::vector<std::string> ChildIds;

		// Convenience accessors used by the serializer and validator.
		// Returns a POINTER into m_.../Literals, or null. Two overloads
		// (const and non-const) follow, as with FindNode on line 212.
		// DEFECT (lifetime): the returned pointer is invalidated by any
		// SetLiteral call that causes the vector to reallocate, and nothing
		// document here warns a caller. Returning a reference to the element,
		// or an optional<reference_wrapper>, would not be safer, but the raw
		// pointer should at least say so.
		const PinLiteral *FindLiteral(const std::string &pinId) const;
		PinLiteral *FindLiteral(const std::string &pinId);
		// Note this is declared but NOT const -- it mutates Literals.
		// isSet defaults to true, so a caller clearing a literal must pass
		// false explicitly. There is no dedicated ClearLiteral, so "forget this
		// literal" and "set this literal to the text 'false'" are one call
		// apart.
		void SetLiteral(const std::string &pinId, const std::string &value, bool isSet = true);
	};

	// A directed wire between two pins, addressed by stable string ids.
	// Four ids, all strings. No validation at this level: a link naming a
	// node that does not exist is representable and must be rejected by
	// the validator at compile time.
	struct GraphLink
	{
		std::string FromNode;
		std::string FromPin;
		std::string ToNode;
		// Last of the four ids, and the only one that is not duplicated
		// elsewhere: FromNode/FromPin mirror ToNode/ToPin, so a link is a
		// directed edge rather than an undirected pair.
		// DEFECT (no direction typing): FromNode/ToNode are the same
		// std::string type as PinDef::Id, so nothing here distinguishes a pin
		// id from a node id. IsAssignable (line 236) works on PinType, not on
		// id roles, so the validator is the only thing preventing a node id
		// being passed where a pin id is expected.
		std::string ToPin;
	};

	// An instance variable, emitted as `self.<Name>` initialized in __init__.
	// An instance variable emitted as self.<Name>.
	struct GraphVariable
	{
		std::string Name;
		// Same Float default as PinDef::Type, for the same reason.
		// No string-literal validation here either, but unlike Abort on line
		// 151 the type IS an enum, so at least a bad value cannot be stored --
		// it would have to come from a PinTypeFromName failure.
		PinType Type = PinType::Float;
		std::string DefaultLiteral;
	};

	// The compiled-to-be blackboard key, for behaviour trees. Declared and
	// typed, so an undeclared key read or write is a load error naming the
	// node rather than a silent None.
	// A behaviour-tree-only concept: a name on the shared blackboard,
	// declared so an undeclared access is a load error (per the comment
	// above) rather than a silent None.
	struct BlackboardKey
	{
		std::string Name;
		// Third occurrence of this exact default. It is consistent, which is
		// the important part: a graph node, a variable and a blackboard key all
		// default to a float rather than to something that would silently
		// change the generated Python.
		PinType Type = PinType::Float;
		std::string DefaultLiteral;
	};

	// A whole graph file.
	// The root. Note that FindEvent on line 217 returns const only --
	// there is no non-const overload, so a caller holding a Graph& cannot
	// locate its event node for mutation. That may be deliberate.
	struct Graph
	{
		// Distinguishes a flow graph (.wlblocks) from a behaviour tree
		// (.wlbtree). They share this data model and this serializer; only the
		// compiler's shape differs. Keeping them separate types would mean two
		// incompatible formats forever.
		// Another string enum. Same defect as Abort on line 151: the two
		// legal values are in a comment, not encoded, and no parse helper
		// exists. So a file containing Kind = "Flow" or "tree " loads and then
		// compiles as the wrong shape, with the failure surfacing as a compiler
		// that does not match the data model.
		// The comment above explains why flow graphs and behaviour trees
		// share a type at all, which is the more interesting design note.
		std::string Kind = "flow";     // "flow" | "tree"

		// The format version, and the ONLY integer that is persisted as an
		// integer in this format.
		// DEFECT (versioning gap): there is no evidence in this header of
		// what a version 2 would look like, nor of a migration path. A loader
		// reading Schema > 1 has no branch to take. More importantly, Schema
		// is a plain int with no validation: a file with Schema = 0 or a
		// negative value is indistinguishable from an unset field, and
		// nothing rejects it. Since the whole module rests on saved-graph
		// stability (see the ordinal comments above), the absence of a version
		// check is the largest structural risk here.
		int Schema = 1;
		std::string Name;

		GraphVariableList Variables;
		BlackboardKeyList Blackboard;
		GraphNodeList Nodes;
		GraphLinkList Links;

		// The first of a const/non-const overload pair, matching FindLiteral.
		// Linear scan over Nodes. Fine for a hand-authored graph; a
		// machine-generated one with thousands of nodes would make every link
		// resolution O(n).
		const GraphNode *FindNode(const std::string &id) const;
		GraphNode *FindNode(const std::string &id);

		// The single event node, or nullptr. A flow graph has exactly one;
		// a tree has none (it ticks from OnUpdateEntity instead).
		// Linear scan for the one node with a non-empty EventName.
		// The header says "only one node per event may exist per
		// graph" -- note per EVENT, not per graph. A graph could therefore
		// legitimately carry OnUpdate and OnInteract nodes, and this function
		// returns whichever it finds FIRST. A caller wanting the OnUpdate node
		// has no way to ask for it; it must scan Nodes itself. That is a real
		// gap between the stated rule and the available API.
		const GraphNode *FindEvent() const;

		// Literal lookup that also walks the link graph, because a pin's
		// resolved source is "linked if linked, otherwise its literal".
		// The central resolution step: "linked if linked, otherwise literal".
		// Five out-parameters, three of them references (outLiteral,
		// outIsLinked, outLinkNode, outLinkPin -- four, in fact) plus the bool
		// return. That is a lot of out-parameters for one query, and it
		// reproduces the four-way ambiguity noted on GraphInput above.
		// DEFECT (contract): nothing says what the return value means. The
		// name suggests "was this resolved", but IsOpenable in
		// ContentBrowserPanel.h uses the same name for a different question.
		// With four out-parameters, false could mean "no such pin", "no
		// literal and no link", or "internal error". Undocumented and
		// unverifiable without the .cpp.
		bool ResolveInput(const std::string &nodeId, const std::string &pinId,
						  PinLiteral &outLiteral, bool &outIsLinked,
						  std::string &outLinkNode, std::string &outLinkPin) const;
	};

	// ---- type helpers ----

	// Human-readable name, used in EVERY error message. These strings are the
	// user interface of the validator.
	// Returns a pointer to a static string, not a std::string -- cheaper
	// for the error-message use case the comment describes.
	// Not marked noexcept or [[nodiscard]]; returning const char* is the
	// conventional "this cannot fail" shape.
	const char *PinTypeName(PinType type);
	// The inverse of PinTypeName, and the round trip that makes the text
	// serialization safe -- as long as every enumerator has a name, which
	// includes the two string-enum fields (Kind, Abort) that have NO
	// equivalent helper.
	// DEFECT (unverifiable from here): nothing states whether the
	// comparison is case-sensitive. If it is, a saved graph authored with
	// "float" loads and one authored with "Float" does not, which is a
	// silent file-format incompatibility.
	bool PinTypeFromName(const std::string &name, PinType &out);

	// Is `from` assignable to `to`? Equal types always are; `int` promotes to
	// `float` implicitly. Nothing else does — a silent `bool`→`float` is how a
	// graph compiles into something that runs and is wrong.
	// The connection rule. The comment names the one permitted promotion
	// (int -> float) and the one that is refused (bool -> float).
	// DEFECT: the comment above the enum says Exec is in the same enum
	// precisely so that exec-to-data is a TYPE ERROR rather than a special
	// case -- but this signature takes two PinTypes with no way to express
	// "this is an exec wire" as distinct from a data type, so the function
	// must be treating PinType::Exec as a case internally.
	// The Any wildcard is also unaccounted for in the documented rule,
	// which only covers equal types and int->float. How Any behaves is not
	// stated, and it is the pin type SetVariable.Value depends on.
	bool IsAssignable(PinType from, PinType to);

	// The Python expression for a literal of `type`. An `int` literal feeding a
	// `float` pin emits `3.0`, not `3`, because `Wasteland.Vec3(3, 0, 0)`
	// would truncate where `Vec3(3.0, ...)` would not.
	// Generates Python source text for a literal, with an ok flag rather
	// than an exception.
	// DEFECT (code generation as a string): this returns code that is
	// presumably concatenated into the generated module. That means the
	// entire correctness of a compiled graph depends on this function
	// producing syntactically valid, non-injectable Python for every input
	// string the author typed.
	// The ok out-flag has the same ambiguity as ResolveInput on line 221,
	// and there is no error out-parameter -- so a failure produces an empty
	// string with ok=false and the caller must produce its own message.
	std::string LiteralToPython(PinType type, const std::string &literal, bool &ok);

	// Parse a literal per its declared type. Returns false with `error` set for
	// a malformed value; never throws and never silently substitutes zero.
	// The inverse direction: text in, canonical text out, plus an error
	// string.
	// Note it returns a STRING rather than a parsed value. The header
	// never explains why -- presumably because the consumer is code
	// generation, so keeping values as text avoids a type-erased variant
	// and preserves exact author intent.
	// Both out-parameters are references and neither has an in-out
	// contract documented, so a caller cannot know whether outValue is
	// untouched when this returns false. It is also not marked [[nodiscard]],
	// so a failure can be ignored entirely -- which contradicts the
	// comment's claim that a malformed value is "a reported error".
	bool ParseLiteral(PinType type, const std::string &literal, std::string &outValue,
					  std::string &error);

}
