#include "wlpch.h"

#include "NodeRegistry.h"

// C++17 nested-namespace syntax. The flow-graph palette lives in its own
// translation unit but registers into the SAME NodeRegistry the behaviour
// tree uses -- one registry, one serializer, one validator. Two registries
// would drift silently.
namespace Wasteland::VisualBlocks
{

	// Flow-graph nodes for general scripting (`.wlblocks`).
	//
	// Separate translation unit from the behaviour-tree library because the
	// two kinds emit DIFFERENT shapes. A tree's nodes build dicts of topology
	// data for the shared `wl_behavior` evaluator to walk, because a tree has
	// to suspend at a node and resume that exact node next tick. A flow graph
	// has no such requirement, so its nodes map onto the target language's own
	// control flow -- `if`/`else`, `while`, `for` -- and run straight through.
	//
	// One shared registry, one shared serializer, one shared validator. Two
	// libraries, never two registries: the alternative drifts invisibly, and
	// each half looks locally reasonable right up until a node registered in
	// one is unknown to the other.
	// An anonymous namespace, so Body/Unquote/VarRef have internal linkage.
	// These are emission helpers, not API: nothing outside this TU can
	// legitimately call them, and giving them external symbols would put them
	// on the linker's collision surface for no benefit.

	// The header comment above explains why this file exists separately from
	// the behaviour-tree library: the two node kinds emit DIFFERENT shapes. A
	// tree node returns one dict describing a position in topology data,
	// because a tree must suspend and resume the same node next tick. A flow
	// node emits real Python control flow and runs straight through.
	namespace
	{
		// The marker a control-flow node writes where the compiler splices in
		// the statements chained off that exec pin.
		//
		// Duplicated from Compiler.cpp deliberately: the node author needs it
		// to write a body placeholder, and the compiler needs it to substitute
		// one. They agree because both spell it the same way and the control
		// characters cannot appear in authored text -- if they ever diverge,
		// the result is a Python syntax error in the generated file, which is
		// loud, rather than a silently empty branch.
		// The marker a control-flow node writes where the compiler splices in the
		// statements chained off that exec pin. Duplicated from Compiler.cpp
		// deliberately -- the node author needs it to write a body placeholder and
		// the compiler needs it to substitute one.

		// They agree because both spell it identically and the \x01 control
		// characters cannot appear in authored text. If the two ever diverged the
		// result would be a Python syntax error in the generated file, which is
		// loud -- far better than a silently empty branch.

		// Verified: Compiler.cpp:87 builds the same "\x01__VB_BODY_" + pinId +
		// "__\x01". A duplicated constant is a latent coupling; the \x01 framing is
		// what keeps the failure mode loud instead of silent.
		std::string Body(const char *pinId)
		{
			// pinId is interpolated rather than an index, so the marker is matched by
			// the pin's ID string ("True", "Body", ...). That is why the compiler can
			// substitute a body without knowing which node wrote the marker.
			return std::string("\x01__VB_BODY_") + pinId + "__\x01";
		}

		// A String pin arrives QUOTED from the emitter, because it has to
		// survive as a Python literal. Strip the quotes when the text is being
		// spliced into generated source as an identifier or a key rather than
		// used as a value.
		// A String pin arrives QUOTED from the emitter, because it has to survive
		// as a Python literal. Strip the quotes when the text is being spliced in
		// as an IDENTIFIER or a KEY rather than used as a value.
		std::string Unquote(const std::string &text)
		{
			// The size >= 2 test matters: with a one-character string of just '"',
			// front() and back() are the same character and substr would return empty
			// while reporting success.
			if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
				// The inner text only -- both delimiters removed, length adjusted by 2.
				return text.substr(1, text.size() - 2);
			// Unquoted already. Tolerating both spellings means a caller need not know
			// whether the emitter quoted the value.
			return text;
		}

		// A variable NAME becomes `self.<name>`, and it is author-supplied
		// text, so it is sanitised rather than trusted. Two different names
		// that sanitise to the same identifier would silently share storage,
		// so the validator rejects the collision -- but emitting something
		// valid is still better than emitting a syntax error.
		// A variable NAME becomes `self.<name>`. Author-supplied text, so it is
		// SANITISED rather than trusted -- this is a code-generation boundary, and
		// anything reaching generated Python must be a valid identifier.
		std::string VarRef(const std::string &rawName)
		{
			// Unquoted first, so the sanitiser sees the bare name rather than a
			// leading or trailing quote character, which the character filter below
			// would otherwise turn into underscores.
			std::string name = Unquote(rawName);
			// Accumulator, built one character at a time. Every rejected character
			// becomes '_' rather than being dropped, so distinct names cannot collapse
			// onto the same shorter identifier by deletion alone.
			std::string clean;
			// Per-character classification. Non-ASCII bytes are rejected, which is why
			// a non-English variable name degrades to underscores rather than
			// round-tripping into generated Python.
			for (char c : name)
			{
				// Letters, digits and underscore pass through. Everything else is replaced.
				// DEFECT (documented, not fixed): PYTHON KEYWORDS ARE NOT REJECTED. The
				// filter admits "class", "for", "if", "def" and the rest, and `self.class`
				// is a Python syntax error -- verified with ast.parse. A graph variable
				// named "class" therefore produces a generated file that does not compile.
				// The comment above this function claims the validator rejects collisions;
				// it says nothing about keywords, and there is no keyword check here.
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_')
					clean += c;
				else
					// Replacement, not deletion -- see the accumulator note above.
					clean += '_';
			}
			// A name made entirely of rejected characters ("###") sanitises to empty,
			// which would produce the invalid `self.`.
			if (clean.empty())
				// A neutral fallback identifier. Arbitrary but valid.
				clean = "var";
			// A leading digit makes an invalid identifier. Prefixing rather than
			// stripping, so the name stays recognisable to the author reading the
			// generated source.
			if (clean[0] >= '0' && clean[0] <= '9')
				clean = "v" + clean;
			// Leading underscore would collide with the compiler's own
			// `self.__out_*` temps and the runtime shim's internals.
			// Leading underscores are stripped because they would collide with the
			// compiler's own `self.__out_*` temps and the runtime shim's internals.
			// Looping rather than a single test, so "__x" is fully cleared.
			while (!clean.empty() && clean[0] == '_')
				// Repeated stripping. This is the only reason the function is a loop
				// rather than an if.
				clean = clean.substr(1);
			// DEFECT (documented, not fixed): reachable from a name that sanitises to
			// nothing AFTER stripping -- a name of only underscores ("___") survives
			// the empty check above, is emptied here, and lands on the same "var"
			// fallback as an all-rejected name. Two different author names therefore
			// share one attribute, which is exactly the collision the comment at the
			// top of this function says the validator catches. It cannot: by this
			// point both names are the identical string "var".
			if (clean.empty())
				clean = "var";
			// The attribute access. Prefixed rather than a bare local so the value
			// lives on the generated object's instance and survives across statements.
			return "self." + clean;
		}
	}

	// Register the flow-graph nodes. Called from RegisterAllNodes().
	// Registers the whole flow palette. Called from RegisterAllNodes(),
	// which the header notes is idempotent -- a second call replaces rather
	// than duplicating, because duplicate registration would make Find return
	// whichever came first, non-deterministically.
	void RegisterFlowNodes()
	{
		// Mutable(), not Get(). Get() does NOT trigger self-registration, and
		// RegisterAllNodes is itself called during that registration -- so calling
		// Get() here would recurse. Mutable() is the raw table accessor, public
		// for exactly this one re-entrant case.
		NodeRegistry &registry = NodeRegistry::Mutable();

		// ================================================================
		// EVENTS -- the graph's entry point. Exactly one per graph.
		// ================================================================

		{
			NodeDef def;
			// The graph's entry point. The TypeId is a FILE FORMAT -- saved graphs
			// name it -- so a rename breaks every .wlblocks file on disk. The header
			// notes an AliasOf exists as a cheap way to keep both spellings loadable.
			def.TypeId = "FlowOnEvent";
			// Category is what the editor palette groups by, and the "/"
			// hierarchy gives the sub-menu. Slash-separated rather than a nested
			// struct because it is only ever read for display and filtering.
			def.Category = "Flow/Event";
			// One exec OUT and no exec IN: this node is the head of the chain. A
			// graph has exactly one of these.
			def.Pins = { MakeExecOut("Out", "Then") };
			// Both parameters unnamed -- the emitter reads no inputs and writes no
			// outputs, because the event node IS the method the compiler already
			// opened. Note IsStatement is NOT set here (unlike every other node in
			// this file): with no exec IN and no branch pins it is a plain node, so
			// the compiler's control-flow detection -- which requires IsStatement AND
			// HasBranchPins -- correctly does not treat it as control flow.
			def.Emit = [](const EmitContext &, EmitOutputs &) {
				// The event node IS the method the compiler already opened;
				// it emits no statement of its own.
				// Empty text: emits no statement of its own. Returning an empty string
				// rather than nothing is the "success with no output" signal the EmitFn
				// contract defines.
				return std::string();
			};
			// Copied BY VALUE into the registry (the signature takes NodeDef), so
			// the local `def` is free to go out of scope. One registration per node,
			// wrapped in its own scope block, which is why the same variable name is
			// reused ~15 times without collision.
			registry.Register(def);
		}

		// ================================================================
		// VARIABLES -- declared on the graph, read and written by nodes.
		//
		// A name that matches no declared GraphVariable still compiles: the
		// attribute simply does not exist until something assigns it, and
		// Python raises AttributeError naming it. ValidateVariables reports
		// it as an error first, so the normal path never gets that far.
		// ================================================================

		{
			NodeDef def;
			// Assigns to a graph-declared variable. A name matching no declared
			// GraphVariable still compiles -- the attribute simply does not exist
			// until something assigns it, and Python raises AttributeError naming it.
			// ValidateVariables reports it as an error first, so the normal path never
			// gets that far.
			def.TypeId = "SetVariable";
			def.Category = "Flow/Variable";
			// Braced list of pin definitions, built in place.
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeExecOut("Out", "Then"),
				// The variable's name, author-supplied and REQUIRED. Routed through
				// VarRef at emit time, which is where the sanitisation happens.
				MakeIn("Name", "Name", PinType::String, true, ""),
				// Wildcard: the type here is the AUTHOR's choice, matching
				// whatever the named variable holds. Pinned to Float it made
				// `flag = (a == b)` unexpressible -- the bool could not
				// connect to a float pin, so the graph failed validation for
				// wanting to store a comparison result.
				// DEFECT (documented, not fixed): the emit lambda below always calls
				// ctx.InFloat("Value"), but this pin is PinType::Any -- the wildcard. A
				// pin whose type is "whatever the author wants" being read through a
				// float-only accessor is inconsistent: a String wired in would be
				// coerced rather than rejected at this boundary.

				// The wildcard itself is deliberate and correct (the comment above this
				// line explains why: pinning to Float made `flag = (a == b)`
				// unexpressible). It is the accessor, not the type, that is the mismatch.
				MakeIn("Value", "Value", PinType::Any, false, "0"),
			};
			// A statement node, not an expression. It returns text rather than
			// pushing outputs, and has exactly one exec IN and one exec OUT so the
			// compiler chains straight through it.
			def.IsStatement = true;
			// Outputs unnamed -- nothing is pushed, because an assignment produces a
			// value nobody consumes.
			def.Emit = [](const EmitContext &ctx, EmitOutputs &) {
				// The raw string pin, unquoted by VarRef and sanitised into `self.<name>`.
				std::string target = VarRef(ctx.In("Name"));
				// A plain Python assignment. InFloat falls back to a neutral literal when
				// the pin is unlinked, so this can never emit `self.x = ` with nothing on
				// the right, which would be a syntax error.
				return target + " = " + ctx.InFloat("Value");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// Reads a variable. The counterpart to SetVariable, and deliberately
			// unlike it in shape -- see the IsStatement discussion below.
			def.TypeId = "GetVariable";
			def.Category = "Flow/Variable";
			def.Pins = {
				// The name to read. Same VarRef treatment as the write side, so both
				// ends of a round trip resolve to the same attribute.
				MakeIn("Name", "Name", PinType::String, true, ""),
				// The read result. Typed Float, so a Bool or String variable is
				// inconsistent with this node -- unlike SetVariable's Any pin, there is
				// no wildcard on the read path.
				MakeOut("Value", "Value", PinType::Float),
			};
			// An EXPRESSION node: it produces a value, it does not run. Its
			// output becomes a temp the consuming node reads.
			// Outputs NAMED here, unlike every statement node -- this is an
			// expression node whose result the consumer reads.
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// The output is the attribute access itself, not a temp. Note this node
				// does NOT use ctx.AllocTemp, so a heavily-reused read emits the same
				// `self.x` expression at every use site rather than one hoisted temp.
				out.Outputs.push_back({ "Value", VarRef(ctx.In("Name")) });
				// Empty: an expression node's value travels through `out`, never as
				// emitted text. Both this and the exec-push above together define the
				// EXPR contract described in NodeRegistry.h.
				return std::string();
			};
			registry.Register(def);
		}

		// ================================================================
		// CONTROL FLOW
		//
		// These own their bodies: each exec OUT carries statements, and the
		// compiler recognises that shape from the pins alone (two or more
		// exec outs) rather than from a list here, so a new control-flow
		// node needs no change to the compiler.
		// ================================================================

		{
			NodeDef def;
			// An if/else. The canonical control-flow node: two exec outs, each
			// carrying statements.
			def.TypeId = "Branch";
			def.Category = "Flow/Control";
			def.Pins = {
				MakeExecIn("In", "In"),
				// Required Bool. InBool supplies the literal when unlinked, so the
				// emitted `if` always has a well-formed test expression.
				MakeIn("Condition", "Condition", PinType::Bool, true, ""),
				// The taken branch. Note the pin ID is reused as the body marker key
				// below -- Body("True") and the compiler's substitution key are the same
				// string.
				MakeExecOut("True", "True"),
				// The not-taken branch. Two exec outs is what makes this CONTROL FLOW:
				// the compiler detects the shape from the pins alone (HasBranchPins),
				// not from a list in the registry, so a new control-flow node needs no
				// compiler change.
				MakeExecOut("False", "False"),
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &) {
				// The shell: a real `if`, then a MARKER where the True chain is
				// substituted. The trailing ":\n" is mandatory Python block syntax.
				// 					   "else:\n" + Body("False");
				// The else half, same marker trick on the False pin. Both markers are on
				// one source line, so the compiler must substitute them independently --
				// which is why the markers embed the pinId rather than being positional.
				return "if " + ctx.InBool("Condition") + ":\n" + Body("True") +
					   "else:\n" + Body("False");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// A while loop. Emits only the loop shell; the Done exec out is emitted by
			// the compiler after the body.
			def.TypeId = "While";
			def.Category = "Flow/Control";
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeIn("Condition", "Condition", PinType::Bool, true, ""),
				// The loop body.
				MakeExecOut("Body", "Body"),
				// Exiting the loop explicitly is what makes a `break`
				// reachable. Without it a loop whose condition never goes
				// false can only be escaped by a condition the author has to
				// compute -- which usually means the loop cannot be written.
				// Explicit loop exit. This pin is what makes `break` reachable: without
				// it, a loop whose condition never goes false could only be escaped by a
				// condition the author must compute -- which usually means the loop
				// cannot be written at all.
				MakeExecOut("Done", "Done"),
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &) {
				// The loop shell and its body marker. NO Done marker is emitted here,
				// even though the pin exists -- the compiler appends whatever hangs off
				// Done after the loop closes. Contrast Branch, which emits both markers.
				// That asymmetry is a contract between this file and Compiler.cpp.
				return "while " + ctx.InBool("Condition") + ":\n" + Body("Body");
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// A counted for-loop, `for i in range(n):`. "ForEach" is a slight
			// misnomer -- it iterates an integer range, not a collection.
			def.TypeId = "ForEach";
			def.Category = "Flow/Control";
			def.Pins = {
				MakeExecIn("In", "In"),
				// Required Int. InInt supplies the range bound. A count of 0 produces an
				// empty loop, which is legal Python and a silent no-op at runtime.
				MakeIn("Count", "Count", PinType::Int, true, "0"),
				// The loop variable is a real Python local, so the body reads
				// it by name. Scoped to the generated method, which is what
				// makes it safe to nest loops.
				// The loop variable NAME, defaulting to the quoted string "i". The default
				// is a quoted Python literal because that is the form the emitter expects
				// to receive -- hence the Unquote below.
				MakeIn("IndexVar", "Index Var", PinType::String, false, "\"i\""),
				MakeExecOut("Body", "Body"),
				MakeExecOut("Done", "Done"),
			};
			def.IsStatement = true;
			// Sanitised INLINE below rather than through VarRef. DEFECT (documented,
			// not fixed): the two sanitiser implementations have DIVERGED. VarRef
			// strips leading underscores and rejects keywords; this copy strips
			// neither. A loop variable named "_i" is legal Python but is inconsistent
			// with the VarRef rules, and "for class in range(3):" is a syntax error
			// (verified with ast.parse) with nothing to catch it.
			def.Emit = [](const EmitContext &ctx, EmitOutputs &) {
				// Unquoted because the pin default arrives as "\"i\"" -- a quoted
				// literal, not a bare name.
				std::string var = Unquote(ctx.In("IndexVar"));
				std::string clean;
				for (char c : var)
				{
					if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
						(c >= '0' && c <= '9') || c == '_')
						clean += c;
					else
						clean += '_';
				}
				// Same two guards as VarRef, applied together in one condition. The
				// leading-digit guard prefixes "i" below rather than "v".
				if (clean.empty() || (clean[0] >= '0' && clean[0] <= '9'))
					// Neutral fallback. Unlike VarRef there is no second emptiness check after
					// the loop, because this copy has no underscore-stripping loop to create
					// that second way to become empty.
					clean = "i";
				// The loop header. The loop variable is a real Python LOCAL scoped to the
				// generated method, which is what makes nested loops safe -- an inner
				// `for i` shadows an outer one rather than colliding on an attribute.
				// 					   "):\n" + Body("Body");
				// The closing paren, the mandatory block colon+newline, and the body
				// marker.
				return "for " + clean + " in range(" + ctx.InInt("Count") +
					   "):\n" + Body("Body");
			};
			registry.Register(def);
		}

		// ================================================================
		// MATH
		//
		// Expression nodes: no exec pins, one value out. The operator is
		// validated against an allowlist before it reaches generated source,
		// so this is a security boundary as well as a UX one.
		// ================================================================

		// A factory for the five arithmetic nodes. Registering them through one
		// lambda rather than five near-identical blocks is the reason this file
		// is short; the cost is that the pins are identical across all five and
		// an author cannot tell, from the palette, what type each operand is.
		// Captured by reference so the lambda can call registry.Register.
		auto addBinary = [&registry](const char *typeId, const char *label,
									 // The operator text, taken as a C string literal by every caller.
									 const char *op)
		{
			// Built fresh per call. Reusing one def across calls would leak pins from
			// the previous node, since Register copies by value and nothing resets it.
			NodeDef def;
			def.TypeId = typeId;
			// Shared category, so all five arithmetic nodes group together in the
			// editor palette.
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("A", "A", PinType::Float, false, "0"),
				MakeIn("B", "B", PinType::Float, false, "0"),
				MakeOut("Value", "Value", PinType::Float),
			};
			std::string operatorText = op;
			// operatorText is CAPTURED BY VALUE, and it is a std::string copy rather
			// than the const char* parameter. That is load-bearing: capturing `op` by
			// reference would dangle, since `op` is a pointer to a string literal
			// local to the enclosing call. The copy into a std::string first makes the
			// capture safe by construction.
			def.Emit = [operatorText](const EmitContext &ctx, EmitOutputs &out) {
				// The output expression. Wrapped in parentheses so the precedence of the
				// authored operator cannot leak outward -- without them, `(a + b) * c`
				// authored as a chain would re-associate.
				// 					   { "Value", "(" + ctx.InFloat("A") + " " + operatorText + " " +
				// The left operand and the operator, then:
				// 					   ctx.InFloat("B") + ")" });
				// The right operand and the closing paren.
				out.Outputs.push_back(
					{ "Value", "(" + ctx.InFloat("A") + " " + operatorText + " " +
								   ctx.InFloat("B") + ")" });
				return std::string();
			};
			registry.Register(def);
			// DEFECT (documented, not fixed): `label` is an UNUSED PARAMETER. It is
			// taken by all five callers (each passes a string equal to its typeId),
			// and the compiler warning it exists to silence is evidence that the
			// parameter was meant to be used -- most likely as a display label on
			// the node, which NodeDef has no field for. The five call sites below
			// all pass the same text as the typeId, so it carries no information
			// today.
			(void)label;
		};

		// typeId, label, operator. Each of the five calls below registers one
		// node with identical pins and a different operator.
		addBinary("Add", "Add", "+");
		// Subtraction. Same shape; note no unary form exists -- Negate below
		// covers that case.
		addBinary("Subtract", "Subtract", "-");
		// Multiplication.
		addBinary("Multiply", "Multiply", "*");
		// DEFECT (documented, not fixed): the generated Python is `a / b`, so a
		// zero divisor raises ZeroDivisionError at RUNTIME in generated code.
		// Neither the validator nor this emitter checks for a literal zero, and
		// the failure surfaces in user-authored content at run time rather than
		// at compile time.
		addBinary("Divide", "Divide", "/");
		// Modulo. Python's % on floats differs from C's fmod for negative
		// operands (Python's result takes the sign of the divisor), which is
		// worth knowing when a graph author expects C semantics.
		addBinary("Modulo", "Modulo", "%");

		// The comparison factory. Structurally IDENTICAL to addBinary -- same
		// pins, same parenthesised emission, same by-value operator capture. The
		// ONLY difference is the output pin's type: Bool rather than Float.
		// That the two are not one parameterised function is mild duplication.
		auto addCompare = [&registry](const char *typeId, const char *op)
		{
			NodeDef def;
			def.TypeId = typeId;
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("A", "A", PinType::Float, false, "0"),
				MakeIn("B", "B", PinType::Float, false, "0"),
				// Bool, unlike addBinary's Float. This is the single behavioural
				// difference between the two factories.
				MakeOut("Value", "Value", PinType::Bool),
			};
			std::string operatorText = op;
			def.Emit = [operatorText](const EmitContext &ctx, EmitOutputs &out) {
				out.Outputs.push_back(
					{ "Value", "(" + ctx.InFloat("A") + " " + operatorText + " " +
								   ctx.InFloat("B") + ")" });
				return std::string();
			};
			registry.Register(def);
		};

		// Strict less-than.
		addCompare("LessThan", "<");
		// Less-than or equal.
		addCompare("LessOrEqual", "<=");
		// Strict greater-than.
		addCompare("GreaterThan", ">");
		// Greater-than or equal.
		addCompare("GreaterOrEqual", ">=");
		// DEFECT (documented, not fixed): both operands are PinType::Float
		// (inherited from the shared pin list above), so `==` is FLOAT equality.
		// Two values that differ only by float rounding compare unequal, which
		// is the classic trap -- and there is no way to express STRING equality
		// through this node at all.
		addCompare("Equal", "==");
		// Inequality, with the same float-only constraint as Equal.
		addCompare("NotEqual", "!=");

		{
			NodeDef def;
			// Unary negation. A separate node rather than an addBinary with an
			// implicit zero, so the palette shows a real negate operation.
			def.TypeId = "Negate";
			def.Category = "Flow/Math";
			def.Pins = {
				// Single operand. Note the pin ID is "A" for consistency with the binary
				// nodes' first operand.
				MakeIn("A", "A", PinType::Float, false, "0"),
				MakeOut("Value", "Value", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// Parenthesised so a negated expression cannot absorb an operator from
				// whatever consumes it.
				out.Outputs.push_back({ "Value", "(-" + ctx.InFloat("A") + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// Constrains a value to a range.
			def.TypeId = "Clamp";
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("Value", "Value", PinType::Float, false, "0"),
				// Lower bound, default 0.
				MakeIn("Min", "Min", PinType::Float, false, "0"),
				// Upper bound, default 1. The differing defaults mean an unconfigured
				// Clamp still produces a sane 0..1 range.
				MakeIn("Max", "Max", PinType::Float, false, "1"),
				// DEFECT (documented, not fixed): the output pin is "Out" while every
				// other expression node in this file names its output "Value". Cosmetic
				// in generated code, but it means a graph saved against this node does
				// not transfer to a node expecting the convention used elsewhere.
				// min() then max() is the clamp that stays defined when the authored
				// bounds are swapped, which happens by accident and is not worth a
				// validator error.
				// DEFECT (documented, not fixed): the comment above states the intent --
				// correct for swapped bounds -- but the ORDER chosen does not deliver it.
				// `max(Min, min(Max, Value))` with Min=5, Max=1, Value=3 returns 5,
				// i.e. the LOWER bound wins outright and the value is discarded
				// entirely (traced directly). The GLSL-style `min(max(v,Min),Max)`
				// would return 1. So a swapped clamp silently pins the output to one
				// bound for every input, which is a much stronger failure than the
				// benign one the comment claims.
				MakeOut("Out", "Out", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// min() then max() is the clamp that is correct when the
				// authored bounds are swapped, which happens by accident and
				// is not worth a validator error.
				// max() applied OUTSIDE min(). Reversing these two produces a different
				// function; the reason is documented at the defect note above.
				// 								   ", min(" + ctx.InFloat("Max") +
				// The inner min(), against the upper bound.
				// 								   ", " + ctx.InFloat("Value") + "))" });
				// The value, then both closing parens. All three operands are inlined
				// rather than evaluated once -- so if Value is a function call with side
				// effects it is evaluated exactly once here, which is correct.
				out.Outputs.push_back({ "Out", "max(" + ctx.InFloat("Min") +
													   ", min(" + ctx.InFloat("Max") +
													   ", " + ctx.InFloat("Value") + "))" });
				return std::string();
			};
			registry.Register(def);
		}

		// ================================================================
		// LOGIC
		// ================================================================

		{
			NodeDef def;
			// Boolean conjunction.
			def.TypeId = "And";
			def.Category = "Flow/Logic";
			def.Pins = {
				// Default TRUE, not false. Neutral element for `and`: an unconfigured
				// operand must not change the result, and true is the identity here.
				MakeIn("A", "A", PinType::Bool, false, "True"),
				// Same neutral default on the second operand.
				// `and`, not `&`: short-circuit matters when B is a call with a side
				// effect, and a designer wiring a condition expects evaluation to stop
				// once the answer is known.

				// Wrapped in bool(): Python's `and`/`or` yield an OPERAND, not a bool --
				// (0.0 or 1.0) is 1.0, ("" or "hi") is "hi". On a Bool pin that is a
				// silent type lie, which happens to read correctly for bool inputs and
				// hands a float or a string to whatever consumes the result.
				MakeIn("B", "B", PinType::Bool, false, "True"),
				MakeOut("Value", "Value", PinType::Bool),
			};
			// `and`, not `&`: short-circuit matters when B is a call with a
			// side effect, and a designer wiring a condition expects it to
			// stop evaluating once the answer is known.
			//
			// Wrapped in bool(): Python's `and`/`or` yield an OPERAND, not a
			// bool -- (0.0 or 1.0) is 1.0, ("" or "hi") is "hi". On a Bool pin
			// that is a silent type lie which happens to read correctly for
			// bool inputs and hands a float or a string to whatever consumes
			// the result.
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// The bool() wrapper is the whole point of this node's emission.
				// 					 " and " + ctx.InBool("B") + ")" });
				// Short-circuiting `and`, closed and wrapped.
				out.Outputs.push_back({ "Value", "bool(" + ctx.InBool("A") +
										 " and " + ctx.InBool("B") + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// Boolean disjunction. Structurally identical to And with `or`.
			def.TypeId = "Or";
			def.Category = "Flow/Logic";
			def.Pins = {
				// Default FALSE, the neutral element for `or` -- the mirror of And's
				// true default. Whoever wrote these two kept the identities straight.
				MakeIn("A", "A", PinType::Bool, false, "False"),
				MakeIn("B", "B", PinType::Bool, false, "False"),
				MakeOut("Value", "Value", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				out.Outputs.push_back({ "Value", "bool(" + ctx.InBool("A") +
										 " or " + ctx.InBool("B") + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// Boolean negation.
			def.TypeId = "Not";
			def.Category = "Flow/Logic";
			def.Pins = {
				MakeIn("A", "A", PinType::Bool, false, "False"),
				MakeOut("Value", "Value", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// NOT rather than `not A == B`. The parenthesised form yields a real
				// bool, and unlike `bool(...)` here the operand is already a Bool pin so
				// no coercion wrapper is needed.
				out.Outputs.push_back({ "Value", "(not " + ctx.InBool("A") + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		// ================================================================
		// UTILITY
		// ================================================================

		{
			NodeDef def;
			// Prints a message. The only node in this file that reaches outside the
			// generated method.
			def.TypeId = "Log";
			def.Category = "Flow/Utility";
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeExecOut("Out", "Then"),
				// Required String. Unlike a variable NAME, a message is DATA -- so it is
				// unquoted and escaped, not sanitised into an identifier.
				MakeIn("Message", "Message", PinType::String, true, ""),
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &) {
				// Unquoted so the surrounding Python quotes can be re-added around
				// ESCAPED content, rather than nesting quotes inside the authored
				// literal.
				std::string message = Unquote(ctx.In("Message"));
				// DEFECT (documented, not fixed): `Wasteland` is the name of the module
				// the generated file imports -- Compiler.cpp emits "import Wasteland" --
				// but a grep of the bindings source finds NO definition of `Log` on that
				// module. The only other "Log" string in the tree is this node's own
				// TypeId. So as far as the source shows, this emits a call to an
				// attribute that does not exist, and every Log node fails at run time
				// with AttributeError. EscapePy itself is correct and necessary -- it is
				// the quoting boundary that stops authored text terminating the literal.
				return "Wasteland.Log(\"" + EscapePy(message) + "\")";
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// Exits the enclosing loop.
			def.TypeId = "Break";
			def.Category = "Flow/Control";
			def.Pins = {
				// Exec in, so it chains like any other statement node.
				MakeExecIn("In", "In"),
				// DEFECT (documented, not fixed): this exec out is UNREACHABLE. A single
				// exec out means the compiler treats Break as a plain node and chains
				// past it -- but `break` leaves the enclosing loop, so whatever hangs off
				// Out can never execute. The comment below the pins acknowledges this and
				// defers to a validator rule; that rule is what stops an author wiring
				// it. A Break with nothing on Out is fine and is presumably the normal
				// use.
				MakeExecOut("Out", "Then"),
			};
			def.IsStatement = true;
			// A single exec out, so the compiler treats it as a plain node and
			// chains past it -- but `break` leaves the enclosing loop, so
			// whatever hangs off Out is unreachable. The validator reports an
			// exec-out wired inside a loop body.
			def.Emit = [](const EmitContext &, EmitOutputs &) {
				// A bare Python `break`. No marker and no block colon, because `break` is
				// a single statement with no body.
				return std::string("break");
			};
			registry.Register(def);
		}
	}

}
