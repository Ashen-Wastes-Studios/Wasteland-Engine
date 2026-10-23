// The engine precompiled header: everything in the target links it, so
// this file gets the platform, STL and engine includes from one place.
#include "wlpch.h"

// NodeDef, NodeRegistry, EmitContext and EmitTreeNode all come from
// here. It is the only Visual Blocks header this file needs directly.
#include "NodeRegistry.h"
// Validator.h is included ONLY for IsAllowedOperator, and only for the
// dead CheckedOp helper above -- nothing else in this file uses it.
// If that helper is removed, this include goes with it.
#include "Validator.h"   // IsAllowedOperator

// The standard-library includes below are not used by any code in this
// file as it stands: no algorithm, math or stringstream call appears.
// Left in place -- removing them is a code change.
#include <algorithm>
#include <cmath>
#include <sstream>

// Registration of every Visual Blocks node type. Nothing here executes
// game logic: each registration supplies metadata and an EMITTER lambda,
// and the compiler runs the emitters later to produce Python source.
namespace Wasteland::VisualBlocks
{

	// The node library.
	//
	// Behaviour-tree nodes are STRUCTURE, not data flow: a Sequence does not
	// "produce a value", it runs children until one fails. So their emitters
	// build a dict passed to the shared wl_behavior runtime rather than
	// inline control flow. That indirection is the whole point: the topology
	// stays DATA, so the runtime can suspend and resume a specific latent node
	// and re-enter from the root to let a higher-priority branch pre-empt.
	// Generating nested if/else here -- the obvious alternative -- makes
	// suspend/resume inexpressible, which is the one thing trees exist for.
	// Anonymous namespace: these three helpers exist only to be called from
	// the emitters below and must not become part of the module's link
	// interface.
	namespace
	{
		// DEAD CODE: Indent has no callers anywhere in the project. The tree
		// emitters build single-line dict entries and never need to indent, and
		// the flow-graph emitters that would have used it live in a different
		// file. Left in place because removing it is a code change, not a
		// comment change.
		std::string Indent(int level)
		{
			// Builds a string of `level` tab characters. Never called (see the note
			// above), so the tab convention it encodes is inherited, not enforced.
			return std::string((size_t)level, '\t');
		}


		// A composite/decorator node. Its children come from the GRAPH's
		// ChildIds (structure), not from data links -- a tree's shape is its
		// priority order, so it is authored as an ordered child list.
		//
		// The emitter writes ONE dict entry per node; the Compiler then walks
		// the tree depth-first from the root and resolves each child id to its
		// variable name.
		// A String pin arrives QUOTED from the emitter ("\"target\""). Strip
		// the quotes so the key name can be spliced into generated code -- and
		// so a designer seeing Target = "target" understands the quotes are
		// serialization, not part of the key.
		// Reads a String pin and strips the surrounding quotes. The quotes are
		// serialization from the graph file, and everything downstream splices
		// the bare text into generated Python -- so a designer who typed
		// Target = "target" gets the key target, not the six characters
		// "target".
		std::string StrPin(const EmitContext &ctx, const char *pin,
					   const char *fallback = "")
		{
			// Raw pin text, quotes and all. The strip below is what turns a
			// serialized literal into a bare key.
			std::string value = ctx.In(pin);
			// Both ends checked, and the length guard first: a one-character value
			// (just an opening or closing quote) would pass a naive front/back test
			// and then be sliced into something meaningless.
			if (value.size() >= 2 && value.front() == '\"' && value.back() == '\"')
				// Drops exactly one leading and one trailing quote, so a value that is
				// only quotes ("\"\"") becomes empty and then falls back -- which is
				// how a deliberately blank pin ends up substituted.
				value = value.substr(1, value.size() - 2);
			// The empty case falls back rather than returning "". Callers rely on
			// getting a usable key name even when the pin is unbound.
			return value.empty() ? std::string(fallback) : value;
		}

		// DEAD CODE, and the security-relevant kind: CheckedOp is never called.
		// Every Op consumer in this file -- ConditionCompare, ConditionDistanceTo,
		// and the three shared with NodeLibraryV1.cpp -- reads the pin and strips
		// the quotes inline instead, so the allow-list is NOT enforced at emit
		// time here.
		// The check does exist, in Validator.cpp, which rejects a disallowed op
		// as an ERROR before compilation. So an invalid operator is caught --
		// but by a separate pass, and only if that pass runs. The comment above
		// this helper describes it as a security boundary in the emitter, which
		// is not what the code does.
		// Validate an operator BEFORE it reaches generated Python. The
		// emitter interpolates it verbatim, so this is a security boundary as
		// much as a UX one.
		std::string CheckedOp(const EmitContext &ctx, const char *pin)
		{
			// Reads the op pin through StrPin so the quotes are stripped by the
			// same path as every other string pin, then checks it. Unreachable:
			// see the note on this helper.
			std::string op = StrPin(ctx, pin, "<=");
			// Unreachable in the current file. See the note above.
			if (!IsAllowedOperator(op))
				// Substitutes the allow-listed default rather than failing. A rejected
				// operator silently becomes "<=", which means a design typo degrades to
				// a working tree instead of an error -- though the validator reports
				// it separately if it runs.
				// Category is the palette grouping and is a plain string, so it is also
				// a filter key in the node browser. The slash-separated "Behaviour/..."
				// prefix is what keeps these out of the flow-graph category tree.
				// BtRole drives validation: composites and decorators are required to
				// have exactly one child, conditions and tasks none. Setting it wrong
				// makes the validator demand a child that cannot exist.
				// A single out-pin and no in-pins. The pin is declared with a Float
				// type even though the emitted payload is a topology dict, because the
				// pin system is typed and there is no "any" type.
				// The lambda's closing brace; its body is the one-line forward above.
				// Register takes the def BY VALUE and moves it in, so each block's
				// local def is consumed here and cannot be reused after this line.
				// Same Category as Sequence: the palette shows composites together,
				// which is why this line is byte-identical to the one above.
				op = "<=";
			return op;
		}
	}

	// One entry point fills the whole registry. Callers never register
	// individual types, which is what keeps the set of valid node types
	// a single decision rather than one per call site.
	// ----------------------------------------------------------------- register

	// No arguments and no return value: the registry is a process-wide
	// singleton, so this both fills it and is the trigger for filling it.
	void RegisterAllNodes()
	{
		// Mutable(), not Get(): Get() lazily calls this function when the table
		// is empty, so asking Get() for the table we are in the middle of
		// filling re-entered the lazy-init and recursed until the stack blew.
		NodeRegistry &registry = NodeRegistry::Mutable();

		// The v1 taxonomy first: if any of its registrations collide with a
		// name below, Register replaces rather than duplicating, and the
		// taxonomy spelling is the one that should win -- saved trees refer
		// to nodes by string, so these names are a file format.
		RegisterBehaviourV1Nodes();

		// ================================================================
		// Behaviour-tree COMPOSITES
		// ================================================================

		{
			NodeDef def;
			// === THE REPEATED REGISTRATION BLOCK ===
			//
			// Every node below follows this exact shape: a scoped NodeDef, a TypeId
			// that is the node's serialized name, a Category used to group it in the
			// palette, a BtRole, a pin list, and an Emit lambda.
			//
			// The block scoping braces are what keep `def` from being redeclared
			// -- without them this file would not compile, since each block
			// redeclares the same name.
			//
			// TypeId is the FILE FORMAT: saved graphs refer to nodes by this exact
			// string, so renaming one silently invalidates every graph on disk that
			// uses it. Register replaces rather than duplicating, so the v1 taxonomy
			// below wins on a collision.
			// BtRole is what makes a node structural rather than data-flowing: it
			// tells the validator and the compiler that children come from the
			// GRAPH's ordered ChildIds, not from data pins.
			// The single "Value/Tree" output is a placeholder, not a data flow: a
			// tree node yields a topology fragment, and the pin exists so the node
			// has a uniform shape across the palette.
			def.TypeId = "Sequence";
			def.Category = "Behaviour/Composite";
			def.BtRole = "composite";
			def.Pins = { MakeOut("Value", "Tree", PinType::Float) };
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// The emitter returns std::string (an error/diagnostic channel) and fills
				// EmitOutputs for the actual payload. EmitTreeNode does the work, so
				// every composite/decorator emitter below is a one-line forward.
				return EmitTreeNode(ctx, out, "Sequence");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// SELECTOR is the mirror of Sequence: first child that succeeds wins.
			// Same single-line emit, different runtime node name.
			def.TypeId = "Selector";
			def.Category = "Behaviour/Composite";
			def.BtRole = "composite";
			def.Pins = { MakeOut("Value", "Tree", PinType::Float) };
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// Selector emits its runtime name and nothing else. The distinction
				// between the two composites is entirely in the string literal here.
				// Parallel is the first block whose def is used for something more
				// than a single-line forward: the policy pin has to be read and
				// defaulted before EmitTreeNode is called.
				// The pins are a braced list because Parallel has two of them. A
				// single-pin block uses the one-line brace form instead.
				// The out-pin is always LAST in the list, so the palette shows inputs
				// above outputs in every block regardless of how many inputs there are.
				return EmitTreeNode(ctx, out, "Selector");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// PARALLEL is the first composite that carries a PIN. A pin here is a
			// parameter of the node, not a data link -- which is why it lands in the
			// topology dict rather than in the emitted control flow.
			def.TypeId = "Parallel";
			def.Category = "Behaviour/Composite";
			def.BtRole = "composite";
			def.Pins = {
				// Pin order in this list is the order the palette shows them, so the
				// in-pin deliberately precedes the out-pin.
				// Policy is optional (`true`) with a "all" default, so an unconnected
				// parallel node still emits valid Python.
				MakeIn("Policy", "Policy", PinType::String, true, "all"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string policy = ctx.In("Policy");
				if (policy.empty())
					// The empty check substitutes the QUOTED default. The value is spliced
					// into a generated Python dict literal, so it has to arrive as
					// source text -- hence the explicit quotes rather than a bare "all".
					policy = "\"all\"";
				// The policy value is concatenated as raw source after the key, so the
				// pin must already be a quoted Python string literal. The default set
				// two lines above quotes it; a value that came from the graph carries
				// its own quotes.
				// Every decorator block from here on repeats this three-field header:
				// Category, BtRole, Pins. The comments above each cover the differences
				// rather than restating these identical lines.
				// BtRole "decorator" -- one child, result transformed. Uniform across
				// Invert/ForceSuccess/ForceFailure/Repeat/Cooldown/Timeout.
				return EmitTreeNode(ctx, out, "Parallel", std::string("'policy': ") + policy);
			};
			registry.Register(def);
		}

		// ================================================================
		// Behaviour-tree DECORATORS
		// ================================================================

		{
			NodeDef def;
			// Decorators wrap exactly one child and transform its result. They are
			// structurally identical to each other, so the three below differ only
			// in the string passed to EmitTreeNode.
			// INVERT negates the child's success/failure. RUNNING is passed through
			// unchanged -- an inverted node cannot manufacture a "still working"
			// answer, it only flips a resolved one.
			def.TypeId = "Invert";
			def.Category = "Behaviour/Decorator";
			def.BtRole = "decorator";
			def.Pins = { MakeOut("Value", "Tree", PinType::Float) };
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				return EmitTreeNode(ctx, out, "Invert");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// ForceSuccess has no pin list at all, which is the only structural
			// difference between it and Invert above.
			def.TypeId = "ForceSuccess";
			def.Category = "Behaviour/Decorator";
			def.BtRole = "decorator";
			def.Pins = { MakeOut("Value", "Tree", PinType::Float) };
			// ForceSuccess has no pin at all: it is a constant. That is deliberate,
			// and the validator warns when it wraps a condition, because a
			// ForceSuccess over a Condition makes the condition dead code.
			// Emitted UNCONDITIONALLY: ForceSuccess succeeds whatever its child
			// did. A designer who wants a conditional gate must use a condition
			// node instead -- the validator warns when ForceSuccess wraps one.
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// Emitted with no extra dict entries -- the runtime knows ForceSuccess
				// takes no parameters, so there is nothing to splice in.
				return EmitTreeNode(ctx, out, "ForceSuccess");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// ForceFailure is the exact mirror of the block above -- same shape,
			// same single-line emit, opposite constant. Flagged here because a
			// reader arriving at ForceFailure first should not assume it has
			// behaviour the block above does not.
			def.TypeId = "ForceFailure";
			def.Category = "Behaviour/Decorator";
			def.BtRole = "decorator";
			def.Pins = { MakeOut("Value", "Tree", PinType::Float) };
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				return EmitTreeNode(ctx, out, "ForceFailure");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// REPEAT is the first decorator with a PIN, so its emit is the first
			// that is longer than one line: the count has to be spliced in.
			def.TypeId = "Repeat";
			def.Category = "Behaviour/Decorator";
			def.BtRole = "decorator";
			def.Pins = {
				// Count is declared NOT required (`false`) with a default of 2, so an
				// unconnected Repeat still emits. An Int pin, unlike the Float Seconds
				// pins on the blocks below.
				MakeIn("Count", "Count", PinType::Int, false, "2"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// The empty check duplicates what InInt would do, but is written
				// out rather than using the typed accessor -- so the default here
				// ("2") is an Int literal, matching the pin type, not a float.
				// Seconds is a Float pin, required, defaulting to "1.0". Note the pin
				// is marked required even though a default exists: required means the
				// palette flags an unset pin, not that emission is impossible.
				// Byte-identical to the Cooldown pin list above. Two adjacent blocks
				// sharing a pin declaration verbatim is exactly the shape that drifts
				// when one of them gains an option.
				std::string count = ctx.In("Count");
				if (count.empty())
					count = "2";
				// The empty-string default mirrors the pin default above ("2"), so an
				// unbound pin and an empty pin emit the same count. The value is raw
				// source text spliced into a Python dict literal.
				return EmitTreeNode(ctx, out, "Repeat", std::string("'count': ") + count);
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// COOLDOWN and TIMEOUT are near-identical blocks: same pin name, same
			// type, same default, same two-line emit. They differ only in the
			// runtime semantics -- Cooldown blocks re-entry after a success,
			// Timeout bounds a single run -- and both are LATENT, since both spend
			// time.
			def.TypeId = "Cooldown";
			def.Category = "Behaviour/Decorator";
			def.BtRole = "decorator";
			def.Pins = {
				MakeIn("Seconds", "Seconds", PinType::Float, true, "1.0"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string seconds = ctx.In("Seconds");
				if (seconds.empty())
					seconds = "1.0";
				// Same shape as Repeat's count above, with the key renamed to
				// "seconds". Nothing validates that this is a non-negative number:
				// a negative or non-numeric literal goes straight into the generated
				// Python and fails there, at compile time, rather than here.
				return EmitTreeNode(ctx, out, "Cooldown", std::string("'seconds': ") + seconds);
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// TIMEOUT. Kept as its own block rather than folded into Cooldown
			// because each is a separate TypeId in the file format and a separate
			// entry in the palette.
			def.TypeId = "Timeout";
			def.Category = "Behaviour/Decorator";
			def.BtRole = "decorator";
			def.Pins = {
				MakeIn("Seconds", "Seconds", PinType::Float, true, "1.0"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string seconds = ctx.In("Seconds");
				if (seconds.empty())
					seconds = "1.0";
				// Identical emit to Cooldown above except for the runtime name and the
				// dict key. Both are latent; neither declares 'latent' in the
				// emitted dict, unlike MoveTo -- which may be intentional (the
				// runtime knows Cooldown and Timeout are time-based) or an omission.
				// Wait's Category is Task while its neighbours above are Decorator,
				// which is the one field where this block departs from the pattern.
				// The same Seconds pin as Cooldown and Timeout, reused a third time.
				return EmitTreeNode(ctx, out, "Timeout", std::string("'seconds': ") + seconds);
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// WAIT is filed under Category "Behaviour/Task" even though it sits
			// here with the decorators -- it wraps nothing. The category is what
			// the palette groups on, and Wait is a task to the designer.
			// BtRole "task", matching the category above: the two are set
			// independently but agree here, and the role is what the validator
			// checks for a required child.
			def.TypeId = "Wait";
			def.Category = "Behaviour/Task";
			// A TASK, not a decorator: it wraps nothing. It suspends on its
			// own timer, so it is a leaf. Declaring it a decorator made the
			// validator demand a child it can never have.
			def.BtRole = "task";
			// Latent by nature: a wait that could not return RUNNING would make
			// the tree block its siblings instead of yielding.
			def.Pins = {
				MakeIn("Seconds", "Seconds", PinType::Float, true, "1.0"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string seconds = ctx.In("Seconds");
				if (seconds.empty())
					seconds = "1.0";
				return EmitTreeNode(ctx, out, "Wait", std::string("'seconds': ") + seconds);
			};
			registry.Register(def);
		}

		// ================================================================
		// Behaviour-tree CONDITIONS (a leaf that gates; success = the check holds)
		// ================================================================

		{
			NodeDef def;
			// Conditions are the nodes that GATE: they succeed when a check holds
			// and never have children. Their emitters below all construct a Leaf
			// inline rather than calling EmitTreeNode, because there is no runtime
			// node type -- the check is compiled straight into a Python lambda.
			// CONDITIONBOOL is the simplest of them: one boolean pin becomes a
			// one-clause lambda. Everything the tree runtime needs is in the lambda,
			// which is why there is no topology dict at all -- just a Leaf.
			def.TypeId = "ConditionBool";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = {
				// The Bool pin default is the C++ spelling "true" (lowercase), not the
				// Python "True" the accessor below emits. The two spellings coexist
				// in this file, so the pin default is never spliced directly --
				// InBool converts it.
				// The Leaf dict is emitted for EVERY condition and task below, always
				// under the same "Value" pin name. That uniformity is what lets the
				// compiler treat all leaves alike regardless of which node produced
				// them.
				// Returns an empty string rather than nothing: the emitter's return is
				// a diagnostic channel, and a successful emit has nothing to report.
				// EmitTreeNode above returns through the same convention.
				MakeIn("Value", "Value", PinType::Bool, true, "true"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// Builds a bound Leaf node: the lambda closes over the expression
				// exactly once at tree-build time, so it allocates per tick.
				std::string expr = ctx.InBool("Value");
				// The lambda is built as SOURCE TEXT and embedded in the dict literal,
				// so the tree's check is deferred to Python compile time. `ctx` is the
				// runtime's per-tick context, bound by the emitted dictionary.
				std::string body = "lambda ctx: SUCCESS if (" + expr +
								   ") else FAILURE";
				out.Outputs.push_back({ "Value", "{'t': 'Leaf', 'leaf': " + body + "}" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// CONDITIONCOMPARE. The op pin is where this file's dead CheckedOp
			// helper would have been used -- see the note on that helper.
			def.TypeId = "ConditionCompare";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			// "==", "!=", "<", "<=", ">", ">="
			def.Pins = {
				// Left and Right are symmetric and both default to "0.0", so an
				// entirely unconnected compare still emits "0.0 < 0.0".
				// The out-pin is last again, after three inputs.
				MakeIn("Left", "Left", PinType::Float, true, "0.0"),
				MakeIn("Right", "Right", PinType::Float, true, "0.0"),
				// Op defaults to "<" here, while the dead CheckedOp helper defaults
				// to "<=". The two defaults disagree, so whichever call site is wired up
				// later will silently change an unbound operator's meaning.
				MakeIn("Op", "Op", PinType::String, true, "<"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// The op is validated against a fixed allowlist at COMPILE time
				// (see the validator), so interpolating it is safe here.
				std::string op = ctx.In("Op");
				if (op.size() >= 2 && op.front() == '"' && op.back() == '"')
					op = op.substr(1, op.size() - 2);
				// Quote-stripping inlined rather than delegated. The op goes into
				// generated Python between two operands with no escaping, so this is
				// the point the allow-list in Validator.cpp has to have already run.
				std::string body = "lambda ctx: SUCCESS if (" + ctx.InFloat("Left") +
								   " " + op + " " + ctx.InFloat("Right") +
								   ") else FAILURE";
				out.Outputs.push_back({ "Value", "{'t': 'Leaf', 'leaf': " + body + "}" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// CONDITIONHEALTHBELOW reads `_health(ctx.entity)`, a helper in the
			// runtime shim (Compiler.cpp kRuntimeShim). It used to read
			// `ctx.entity.health()` directly -- a name that does not exist on
			// the Entity binding (verified: hasattr is False, and the only
			// accessor is GetHealthComponent). The emitter cannot check it, so a
			// runtime without the method failed at tree-build time with
			// AttributeError, after compiling cleanly.
			//
			// The helper returns the ABSOLUTE Current, not a fraction: the
			// thresholds authored against this node (EnemyGuard uses 25.0) are
			// absolute, and _health_fraction would flip the comparison.
			def.TypeId = "ConditionHealthBelow";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = {
				// Threshold is a Float pin requiring a value, defaulting to 30.0.
				// It is an ABSOLUTE health value, matching the emitted helper.
				// Nothing in the emitter or the validator enforces a range.
				// Target names a blackboard key (the pin comment above says so). The
				// default "target" is a convention shared by MoveTo, Attack and
				// ConditionDistanceTo, so the four agree out of the box.
				MakeIn("Threshold", "Threshold", PinType::Float, true, "30.0"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string body =
					"lambda ctx: SUCCESS if (_health(ctx.entity) <= " +
					ctx.InFloat("Threshold") + ") else FAILURE";
				out.Outputs.push_back({ "Value", "{'t': 'Leaf', 'leaf': " + body + "}" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// CONDITIONDISTANCETO is the only condition with TWO string-ish
			// inputs (target key and op) plus a float, which is why its emit is
			// the longest in the file.
			def.TypeId = "ConditionDistanceTo";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			// "Target" is the blackboard key holding the Entity.
			def.Pins = {
				MakeIn("Target", "Target", PinType::String, true, "target"),
				MakeIn("Range", "Range", PinType::Float, true, "2.0"),
				// Op defaults to "<=" here while ConditionCompare above defaults to
				// "<". The inconsistency is harmless -- both are in the allow-list --
				// but it means the two nodes are not interchangeable on an unset pin.
				MakeIn("Op", "Op", PinType::String, true, "<="),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string target = ctx.In("Target");
				if (target.size() >= 2 && target.front() == '"' && target.back() == '"')
					target = target.substr(1, target.size() - 2);
				std::string op = ctx.In("Op");
				if (op.size() >= 2 && op.front() == '"' && op.back() == '"')
					op = op.substr(1, op.size() - 2);
				// The target key is spliced INSIDE a generated double-quoted Python
				// string. A key containing a quote or backslash would produce broken
				// source; nothing here escapes it, and the blackboard-key check in
				// Validator.cpp is a declaration check, not a character check.
				std::string body = "lambda ctx: SUCCESS if (_dist(ctx.entity, "
									   // The blackboard lookup is emitted inline as ctx.bb.get("key"). The
									   // key is NOT escaped: a key containing a double quote or a backslash
									   // would produce invalid Python. The validator checks that the key is
									   // declared, not that it is safe to interpolate.
									   // Tasks share the Task category with Wait and Attack. The comment
									   // above this section explains the family.
									   // Target defaults to the same "target" blackboard key as
									   // ConditionDistanceTo above, so a designer wiring either node without
									   // setting the pin gets the same entity.
									   "ctx.bb.get(\"" + target +
									   "\")) " + op + " " + ctx.InFloat("Range") +
									   ") else FAILURE";
				out.Outputs.push_back({ "Value", "{'t': 'Leaf', 'leaf': " + body + "}" });
				return std::string();
			};
			registry.Register(def);
		}

		// ================================================================
		// Behaviour-tree TASKS (the leaves that DO things)
		// ================================================================

		{
			NodeDef def;
			// Tasks are the leaves that DO things. Unlike the conditions above,
			// they call into runtime helpers (_move, _attack) that the generated
			// module is expected to provide.
			// MOVETO is the only task declared LATENT in its emitted dict. The
			// comment above explains why the compiler cannot infer it.
			def.TypeId = "MoveTo";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			// Latent: movement spans many ticks, so the node must be able to
			// return RUNNING. The COMPILER cannot infer this, so the runtime
			// requires it be declared -- a MoveTo without it would report
			// success on tick one and the tree would stop on it.
			def.Pins = {
				MakeIn("Target", "Target", PinType::String, true, "target"),
				// Speed is NOT required and defaults to "3.0", so MoveTo works
				// unconfigured. Unlike Target, an unset Speed is invisible in the
				// palette because the pin carries a default.
				// The out-pin closes the four-pin list.
				MakeIn("Speed", "Speed", PinType::Float, false, "3.0"),
				// NB.8: route around geometry instead of walking a straight line.
				//
				// DEFAULT FALSE, and that default is the whole point. Turning
				// this on by default would change how every existing .wlbtree
				// on disk behaves the moment the engine is rebuilt -- and a
				// navmesh query per tick per NPC is not free. Off means today's
				// behaviour: direct movement, which is correct inside an open
				// arena and costs nothing.
				MakeIn("UsePathfinding", "Use Pathfinding", PinType::Bool, false, "False"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string target = ctx.In("Target");
				if (target.size() >= 2 && target.front() == '"' && target.back() == '"')
					target = target.substr(1, target.size() - 2);
				std::string body =
					// The emitter reads four pins and emits one call. The bool is compared
					// against "True" and re-emitted as the Python literal -- so a pin that
					// resolved to anything else silently becomes False rather than erroring.
					"lambda ctx: _move(ctx.entity, ctx, \"" + target + "\", " +
					// Speed is spliced as a raw Python float literal, so a non-numeric
					// Speed pin produces invalid Python at compile time rather than a
					// C++-side error.
					// The return convention again: MoveTo reports no diagnostics even
					// though it is the only node emitting extra dict keys.
					// Attack's Target uses the same default key as MoveTo above, so both
					// action nodes read the same entity by default.
					ctx.InFloat("Speed") + ", " +
					// True/False, not 1/0: _move compares against the literal.
					// True/False are Python's capitalised literals, matching what
					// EmitContext::InBool emits. Note this C++ file uses lowercase true/false
					// for its own literals (see the MoveTo pin default above), so the two
					// spellings coexist in one expression -- an easy place to typo.
					(ctx.InBool("UsePathfinding") == "True" ? "True" : "False") + ")";
				out.Outputs.push_back({ "Value",
										"{'t': 'Leaf', 'leaf': " + body +
											// 'latent': True is emitted as a Python bool literal inside the dict,
											// which is what the runtime reads to decide whether this leaf may return
											// RUNNING. No other task carries it, because no other task currently
											// spans ticks.
											", 'latent': True}" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// ATTACK is the same shape as MoveTo minus the latency: a one-shot call
			// that resolves on the tick it runs, so it emits no 'latent' key.
			def.TypeId = "Attack";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				MakeIn("Target", "Target", PinType::String, true, "target"),
				// Damage is optional with a default of 10.0, the pattern used by every
				// numeric parameter in this file.
				MakeIn("Damage", "Damage", PinType::Float, false, "10.0"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string target = ctx.In("Target");
				if (target.size() >= 2 && target.front() == '"' && target.back() == '"')
					target = target.substr(1, target.size() - 2);
				std::string body = "lambda ctx: _attack(ctx.entity, ctx, \"" + target +
								   "\", " + ctx.InFloat("Damage") + ")";
				out.Outputs.push_back({ "Value", "{'t': 'Leaf', 'leaf': " + body + "}" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// SETBLACKBOARD is the only node here that WRITES rather than reads.
			// The Key pin defaults to the empty string, so an unconnected SetBlackboard
			// emits a write to the "" key rather than failing -- the empty-string
			// substitution limitation noted in NodeRegistry.cpp applies to this pin.
			def.TypeId = "SetBlackboard";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				// The only pin in this file defaulting to the EMPTY string, and the
				// only one whose default is not a valid literal for its own type. An
				// unconnected SetBlackboard writes to the "" key.
				MakeIn("Key", "Key", PinType::String, true, ""),
				// The pin is named "Value" for a WRITE, which reads oddly in the
				// palette but keeps the name consistent with the read pins on the
				// other nodes.
				MakeIn("Value", "Value", PinType::Float, true, "0.0"),
				MakeOut("Value", "Tree", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string key = ctx.In("Key");
				if (key.size() >= 2 && key.front() == '"' && key.back() == '"')
					key = key.substr(1, key.size() - 2);
				// The set() call is wrapped in a tuple and indexed at [1], so the
				// expression's value is SUCCESS while the write still happens. That is
				// the standard Python idiom for "do a side effect, yield a constant".
				std::string body = "lambda ctx: (ctx.bb.set(\"" + key +
								   // The tuple-and-index trick: bb.set() returns something, SUCCESS is
								   // appended, and [1] selects the constant. The write happens as a side
								   // effect of evaluating the tuple.
								   // Same empty return as every other emitter in this file.
								   // Register for the last block. Every block in this file ends with
								   // exactly this call, which is why the registry is the only thing that
								   // needs to be correct for a node type to exist at all.
								   "\", " + ctx.InFloat("Value") + "), SUCCESS)[1]";
				out.Outputs.push_back({ "Value", "{'t': 'Leaf', 'leaf': " + body + "}" });
				return std::string();
			};
			registry.Register(def);
		}

		// Flow-graph nodes live in NodeLibraryFlow.cpp: they emit real
		// control flow rather than topology dicts, so keeping them beside the
		// tree nodes invited copy-paste between two emitters whose contracts
		// are opposites. RegisterFlowNodes() is called from here so there is
		// still exactly ONE entry point that fills the registry.
		// Flow-graph nodes are registered from another file but called from
		// here, so there is exactly one entry point into the registry. The
		// comment above explains why the two families were split.
		RegisterFlowNodes();
	}

}
