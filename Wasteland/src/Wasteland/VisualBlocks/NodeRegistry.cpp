// Precompiled header, first include as the engine requires.
#include "wlpch.h"

// The module's own header, second as the convention goes -- it declares the
// NodeDef table, EmitContext and the registry singleton defined below.
#include "NodeRegistry.h"

// Nested-namespace form (C++17), so no braces of its own are needed and the
// closing line at the end carries the comment.
namespace Wasteland::VisualBlocks
{

	// ------------------------------------------------------------- EmitContext

	// The untyped pin lookup: find the value bound to a pin, or an empty string.
	//
	// @param pinId  The pin's id, as registered in the node definition.
	// @return        The bound source expression, or empty if unbound.
	//
	// Returns by VALUE. An empty string is the single 'not bound' signal, which
	// is what the InTyped helper below keys on -- and it means a legitimately
	// empty value is indistinguishable from an unbound pin. That is tolerable
	// only because every emitted value is a non-empty expression string.
	std::string EmitContext::In(const std::string &pinId) const
	{
		// A linear scan over the bound inputs.
		//
		// DEFECT, minor: this runs once per pin read during code generation, so
		// emitting a node with k reads costs O(k * inputs). With the handful of pins
		// a node has, irrelevant. Worth noting only because Inputs is a map -- a
		// find() would be O(log n) and no more code.
		for (const auto &entry : Inputs)
		{
			if (entry.first == pinId)
				return entry.second;
		}
		// Explicitly empty rather than relying on a default return. Same value as
		// `return {};`, just more obvious at a glance.
		return std::string();
	}

	namespace
	{
		// A missing pin falls back to a type-appropriate neutral literal so the
		// emitted code is still syntactically valid and the failure is visible
		// as a wrong runtime value rather than a syntax error. The validator has
		// already reported the real problem; this is only a backstop.
		// The fallback helper behind every typed accessor below.
		//
		// In an anonymous namespace, so it has internal linkage and cannot collide
		// with anything else in the translation unit or be called from outside this
		// file.
		//
		// @param ctx       Context to read from.
		// @param pinId     Pin to read.
		// @param fallback  Source text to substitute when the pin is unbound.
		std::string InTyped(const EmitContext &ctx, const std::string &pinId,
							const char *fallback)
		{
			std::string value = ctx.In(pinId);
			// The neutral-literal substitution.
			//
			// DEFECT, and a real one: `value.empty()` is also true for a pin that IS
			// bound to an empty string. The comment above frames this as a backstop for
			// missing pins, but the test cannot tell the two cases apart -- so a node
			// with a legitimately empty literal silently gets the fallback instead.
			// EmitContext would need a bound/unbound flag for In() to distinguish them.
			return value.empty() ? std::string(fallback) : value;
		}
	}

	// Typed accessors: one per pin type, each supplying the literal the COMPILER
	// accepts for an unbound pin of that type.
	//
	// Returning source text rather than a parsed value is the point -- the
	// emitter builds Python, so what flows through here is a fragment of code.
	std::string EmitContext::InFloat(const std::string &pinId) const
	{
		// "0.0" and not "0": a Python int would still work in arithmetic, but the
		// emitted type would then not match a float pin's declared type. The decimal
		// point keeps the generated code type-consistent.
		return InTyped(*this, pinId, "0.0");
	}

	std::string EmitContext::InInt(const std::string &pinId) const
	{
		// "0" for an int pin. The mirror image of InFloat's choice, for the same
		// reason.
		return InTyped(*this, pinId, "0");
	}

	std::string EmitContext::InBool(const std::string &pinId) const
	{
		// Python's False, capitalised -- a lowercase `false` would be a syntax
		// error in the generated file.
		//
		// Worth contrasting with the C++ literal true/false used throughout this
		// file: the two languages coexist here, and the distinction is exactly what
		// makes a typo here expensive.
		return InTyped(*this, pinId, "False");
	}

	std::string EmitContext::InVec3(const std::string &pinId) const
	{
		// A fully-qualified constructor call rather than a bare literal, because the
		// generated Python has to reach into the engine's own namespace to build a
		// vector.
		//
		// The engine name is spelled into a string here, so a namespace rename would
		// break emitted code with no compile-time error in this file.
		return InTyped(*this, pinId, "Wasteland.Vec3(0.0, 0.0, 0.0)");
	}

	std::string EmitContext::InEntity(const std::string &pinId) const
	{
		// Python's own name for the enclosing object, so an unbound entity pin
		// refers to the node's own entity rather than failing to compile.
		return InTyped(*this, pinId, "self");
	}

	// ---------------------------------------------------------------- NodeDef

	// Pin lookup within one node definition.
	//
	// Returns a POINTER into this NodeDef's Pins vector -- not a copy and not a
	// reference. A caller must not hold it across anything that could modify
	// Pins, or it dangles.
	//
	// @return  The pin definition, or nullptr if this node has no such pin.
	const PinDef *NodeDef::FindPin(const std::string &pinId) const
	{
		// Linear scan, consistent with EmitContext::In above. Pin counts per node
		// are single digits, so a map would be overhead.
		for (const PinDef &pin : Pins)
		{
			if (pin.Id == pinId)
				return &pin;
		}
		return nullptr;
	}

	// Existence test, expressed in terms of FindPin so the two cannot disagree
	// about what counts as a pin.
	bool NodeDef::HasPin(const std::string &pinId) const
	{
		return FindPin(pinId) != nullptr;
	}

	// ------------------------------------------------------------- pin makers

	// The pin factories below: four small builders that make registration
	// call sites read as declarations rather than as field assignments.
	//
	// Each takes the pin id and an optional label, and returns by value so the
	// caller can push it straight into a node's Pins list.
	PinDef MakeExecIn(const std::string &id, const std::string &label)
	{
		PinDef pin;
		pin.Id = id;
		// An empty label falls back to the id.
		//
		// Repeated identically in all four factories below -- five lines of
		// duplication for a rule that belongs in one place. A PinDef constructor or
		// a small helper would express it once.
		pin.Label = label.empty() ? id : label;
		pin.Type = PinType::Exec;
		pin.Dir = PinDir::In;
		// Forced true, with the reason inline: an execution input that nothing feeds
		// makes the whole node unreachable, so the validator should always say so.
		//
		// Not a parameter, unlike MakeIn's -- because for an exec pin it is not a
		// choice. Worth noting that every other field in these factories is
		// caller-controlled and this one is not.
		pin.Required = true;   // an exec-in with nothing feeding it is unreachable
		return pin;
	}

	// The output counterpart of MakeExecIn.
	//
	// DEFECT, minor: Required is NOT set here, so it keeps whatever default
	// PinDef declares. An output pin being marked required would be meaningless,
	// so the default is presumably false -- but that relies on the header's
	// default rather than stating it. MakeIn sets it explicitly, so the
	// asymmetry reads as an oversight rather than a decision.
	PinDef MakeExecOut(const std::string &id, const std::string &label)
	{
		PinDef pin;
		pin.Id = id;
		pin.Label = label.empty() ? id : label;
		pin.Type = PinType::Exec;
		pin.Dir = PinDir::Out;
		return pin;
	}

	// The general input-pin factory.
	//
	// @param id            Pin id, unique within the node.
	// @param label         Display text; empty falls back to id.
	// @param type          Value type, which decides the typed accessor the
	//                       emitter will use.
	// @param required      Whether the validator demands a link.
	// @param defaultLiteral Source text used when the pin is unlinked and the
	//                       node emits it as a literal.
	PinDef MakeIn(const std::string &id, const std::string &label, PinType type,
				  bool required, const std::string &defaultLiteral)
	{
		PinDef pin;
		pin.Id = id;
		pin.Label = label.empty() ? id : label;
		pin.Type = type;
		pin.Dir = PinDir::In;
		pin.Required = required;
		pin.DefaultLiteral = defaultLiteral;
		return pin;
	}

	// The general output-pin factory. No `required`, because an output pin has
	// nothing to be missing.
	//
	// Returns by value; the caller is expected to move or copy it into a Pins
	// vector.
	PinDef MakeOut(const std::string &id, const std::string &label, PinType type)
	{
		PinDef pin;
		pin.Id = id;
		pin.Label = label.empty() ? id : label;
		pin.Type = type;
		pin.Dir = PinDir::Out;
		return pin;
	}

	// ----------------------------------------------------------- NodeRegistry

	NodeRegistry::NodeRegistry() = default;

	// The mutable singleton accessor. Separate from Get() below, and the
	// separation is load-bearing -- see the long note there.
	NodeRegistry &NodeRegistry::Mutable()
	{
		// Function-local static: initialised on first use, so a headless Python
		// test that never touches Visual Blocks does not pay for the table.
		// A function-local static, so the table is built on first use rather than
		// during static initialisation.
		//
		// That avoids both static-initialisation-order problems and the cost for a
		// headless run that never touches Visual Blocks. The trade is the usual
		// one: first access is not thread-safe, so concurrent first touches would
		// race. In practice the editor initialises on one thread first.
		static NodeRegistry instance;
		return instance;
	}

	// The read-only-looking accessor that in fact performs the lazy population.
	//
	// The comment block inside this function is the most valuable thing in the
	// file: it records a real bug that shipped -- the editor reaching an EMPTY
	// registry and reporting 25 valid node types as unknown, because only the
	// Python bindings ever called RegisterAllNodes(). Self-registering on access
	// is what removed the host-ordering dependency.
	NodeRegistry &NodeRegistry::Get()
	{
		// Populated HERE, on first access, rather than by each host calling
		// RegisterAllNodes() at startup. That was the original shape and it had
		// a silent hole: only the Python bindings ever called it, so the EDITOR
		// reached an EMPTY registry and reported every authored node type as
		// "unknown" -- 25 errors on a tree that validates clean in the test
		// suite, because the tests go through the bindings that do register.
		//
		// Self-registering on access removes the host-ordering dependency
		// entirely: every entry point -- bindings, editor panel, validator,
		// emitter -- gets the same taxonomy without having to remember to
		// initialise it. Registration is idempotent (Register replaces by
		// TypeId), so an explicit RegisterAllNodes() from a host that wants to
		// force it early stays harmless.
		//
		// Mutable() not Get(): RegisterAllNodes() fills this same instance, so
		// calling Get() from there re-tested the still-empty table and called
		// RegisterAllNodes() again, forever. The lazy-init belongs here and
		// only here.
		// Get() must call Mutable(), not itself -- the note above explains that the
		// original recursion through Get() re-tested the still-empty table and
		// called RegisterAllNodes() forever.
		//
		// That is a genuine re-entrancy trap: a lazily-initialising accessor that
		// calls itself before initialising never terminates.
		NodeRegistry &instance = Mutable();
		// The emptiness test, not a 'done' flag.
		//
		// DEFECT: emptiness is used as the initialisation sentinel, which means the
		// guard misfires in two ways. (1) A caller that intentionally registers
		// nothing leaves the table empty, so every later Get() re-runs
		// RegisterAllNodes(). (2) There is no separate flag, so an explicit
		// Clear() -- if one exists -- would also re-trigger population. A bool
		// m_Initialised beside the table would decouple the two concerns; as written,
		// 'unpopulated' and 'populated then emptied' are indistinguishable.
		//
		// Note the access is to the private m_Defs of another instance, which is
		// legal here only because both are members of the same class.
		if (instance.m_Defs.empty())
			RegisterAllNodes();
		return instance;
	}

	// Looks a node type up by id, resolving aliases.
	//
	// @return  The resolved definition, or nullptr if unregistered or unresolvable.
	const NodeDef *NodeRegistry::Find(const std::string &typeId) const
	{
		const NodeDef *found = nullptr;
		// Linear scan of the table, not a hash lookup.
		//
		// DEFECT: Find is called from IsRegistered, from the emitter for every node
		// in a graph, and recursively once per alias hop -- so emitting a graph costs
		// O(nodes * defs) string comparisons. With a few dozen defs that is fine;
		// with a user-extensible registry it would not be. A map keyed on TypeId,
		// kept in step by Register below, would remove the scan.
		for (const NodeDef &def : m_Defs)
		{
			if (def.TypeId == typeId)
			{
				found = &def;
				break;
			}
		}
		// Two exits from the resolution: not found, or found and not an alias. The
		// second is the common case and returns the definition as-is.
		if (found == nullptr || found->AliasOf.empty())
			return found;

		// Resolve the alias to its target, so an alias behaves IDENTICALLY
		// everywhere -- same pins, same role, same emitter -- rather than being
		// a special case each call site has to remember.
		//
		// The depth cap turns an alias cycle (A -> B, B -> A) into nullptr
		// instead of a hang. A cycle is a registration bug, and a hang at
		// startup is a far worse report than "unknown node type".
		// Walks the alias chain from the definition found above.
		const NodeDef *resolved = found;
		// Bounded alias resolution, capped at 8 hops.
		//
		// The cap turns a registration cycle (A -> B -> A) into nullptr rather than
		// an infinite loop -- the comment above is right that a hang at startup is a
		// worse report than 'unknown node type'.
		//
		// DEFECT: the cap returns whatever `resolved` holds when the depth runs out,
		// which for a cycle is a definition that is still an alias -- not nullptr as
		// the comment claims. So a cycle longer than 8 hops yields a partially
		// resolved alias rather than a clean failure. The post-loop check should be
		// `return resolved->AliasOf.empty() ? resolved : nullptr;`.
		for (int depth = 0; depth < 8 && !resolved->AliasOf.empty(); ++depth)
		{
			resolved = Find(resolved->AliasOf);
			if (resolved == nullptr)
				return nullptr;
		}
		return resolved;
	}

	// Existence test defined in terms of Find, so a type whose alias chain is
	// broken reads as unregistered -- which is the conservative answer and
	// matches what Get() would have to do with it.
	bool NodeRegistry::IsRegistered(const std::string &typeId) const
	{
		return Find(typeId) != nullptr;
	}

	// Adds or replaces one definition.
	//
	// @param def  Taken BY VALUE so it can be moved into the table on both paths
	//              below, avoiding a copy of every pin and literal.
	//
	// Replacing rather than appending is what makes registration idempotent --
	// the property Get()'s lazy init relies on.
	void NodeRegistry::Register(NodeDef def)
	{
		// Replace rather than append, so registering twice does not create two
		// entries whose order decides which Find() returns.
		for (NodeDef &existing : m_Defs)
		{
			if (existing.TypeId == def.TypeId)
			{
				// In-place replacement, preserving the entry's POSITION in m_Defs.
				//
				// Worth noting: because position is preserved and Find scans linearly, a
				// re-registered type keeps whatever ordering it first had, which is what
				// makes the node palette's ordering stable across reloads.
				existing = std::move(def);
				return;
			}
		}
		// The append path. DEFECT: no cap on m_Defs, so a misbehaving or
		// third-party registration loop can grow the table without bound -- each
		// entry also carries its full pin list, so it is not cheap.
		m_Defs.push_back(std::move(def));
	}

	// The behaviour-tree half of the taxonomy: definitions with a non-empty
	// BtRole.
	//
	// The next function is the exact complement -- non-empty role versus empty
	// role. Together they partition the table, which is why one of them can be
	// written as the other's negation.
	std::vector<const NodeDef *> NodeRegistry::BehaviorNodes() const
	{
		std::vector<const NodeDef *> result;
		for (const NodeDef &def : m_Defs)
		{
			// Non-empty BtRole marks a behaviour node.
			//
			// A string emptiness test rather than a flag or an enum: it lets a node
			// declare an arbitrary role name, at the cost of making 'is this a behaviour
			// node' a question about a string rather than about a type.
			if (!def.BtRole.empty())
				result.push_back(&def);
		}
		return result;
	}

	// The visual-scripting half: everything with an empty BtRole. The complement
	// of BehaviorNodes above.
	std::vector<const NodeDef *> NodeRegistry::FlowNodes() const
	{
		std::vector<const NodeDef *> result;
		for (const NodeDef &def : m_Defs)
		{
			// The inverted test. Every definition lands in exactly one of the two
			// lists, so a caller can iterate both and know it has seen everything.
			if (def.BtRole.empty())
				result.push_back(&def);
		}
		return result;
	}


	// Emits one behaviour-tree node as a Python dict literal.
	//
	// @param ctx    Context, supplying the node being emitted.
	// @param out    Accumulator the new entry is appended to.
	// @param type   Behaviour node type name.
	// @param extra  Pre-formatted extra fields, spliced in verbatim.
	// @return       An empty string -- always. See the note on the return below.
	std::string EmitTreeNode(const EmitContext &ctx, EmitOutputs &out,
							 const char *type, const std::string &extra)
	{
		// String concatenation builds the Python literal directly.
		//
		// DEFECT: `type` is spliced into a single-quoted Python string with NO
		// escaping, and the same is true of Abort and `extra` below. A node type or
		// abort string containing a quote or a backslash produces syntactically
		// invalid Python whose error message points at the generated file, not at
		// the graph that caused it. The literals from EmitContext above have the
		// same exposure.
		//
		// The explicit std::string(type) is needed only to give the char* an
		// operator+ with the string literal on its left.
		std::string entry = "{'t': '" + std::string(type) + "'";
		if (ctx.Node->Latent)
			entry += ", 'latent': True";
		// The abort field is emitted only when set, and the literal string "none"
		// is treated as unset.
		//
		// DEFECT: that second comparison hardcodes a magic string against data
		// authored in a graph file. "None", "NONE" or "None" all pass through and
		// become an abort behaviour the author did not ask for. An enum or a
		// sentinel constant shared with the graph format would remove the
		// string-typo class of bug entirely.
		if (!ctx.Node->Abort.empty() && ctx.Node->Abort != "none")
			entry += ", 'abort': '" + ctx.Node->Abort + "'";
		if (!extra.empty())
		{
			entry += ", ";
			entry += extra;
		}
		entry += "}";
		// The emitted dict is bound to an output pin called "Value".
		//
		// That pin name is the contract with the rest of the emitter: an output
		// called something else would not be found by the consuming node's lookup.
		out.Outputs.push_back({ "Value", entry });
		// Always empty, and the caller must check for that.
		//
		// This is the emitter's convention for 'nothing to propagate on this pin'.
		// It reads as an unexplained oddity at the call site: a function named
		// EmitTreeNode returning a string that is never non-empty. A void return,
		// or a comment at the declaration in the header, would make it clear.
		return std::string();
	}



	
}
