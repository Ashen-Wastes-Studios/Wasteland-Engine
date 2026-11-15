#include "wlpch.h"

#include "NodeRegistry.h"
// Compiler: the SHARED SanitizeIdentifier. A node's ResolvePins must derive the
// same Python identifier from the same author-supplied name that the compiler
// does -- a second copy is a call node that compiles and then raises
// AttributeError on a method that was never emitted.
#include "Compiler.h"
// std::set: the closed operator allowlist in Compare's emitter.
#include <set>

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
				// The read result, typed ANY.
				//
				// It was Float, with a comment saying a Bool or String variable was
				// "inconsistent with this node". That was wrong: the node emits
				// `self.<name>` and returns whatever the variable holds, so the type
				// is whatever the AUTHOR declared. Float-typing it meant a declared
				// Int variable (a loop counter, say) could not feed an Int pin --
				// exactly the read a ForEach index publish exists to serve.
				MakeOut("Value", "Value", PinType::Any),
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
				// Same Done marker as ForEach: the pin is declared and linkable but
				// was never emitted, so a While silently swallowed everything after
				// it. The comment above this node claims the compiler emits it --
				// it does not; the emitter is the only place the marker can be
				// written, because only the emitter knows the loop's own indent.
				return "while " + ctx.InBool("Condition") + ":\n" + Body("Body") +
					   Body("Done");
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
				// The closing paren, the mandatory block colon+newline, and the body
				// marker.
				//
				// PUBLISH THE LOOP VARIABLE BEFORE THE BODY.
				// ---------------------------------------
				// Without this the loop variable is a Python LOCAL that no node can
				// reach: `GetEntityAtIndex` needs an Index pin fed by a link or a
				// literal, and no node outputs `i`. A graph could iterate a count
				// OR index a list, but not both -- which is exactly what a
				// nearest-target scan is. Task B.6 hit this and could not be
				// authored at all.
				//
				// Writing it into the graph variable of the SAME NAME makes it
				// readable with GetVariable, and the declaration is the author's
				// signal that the variable is part of the graph's state rather than
				// an implementation detail of the loop.
				//
				// ONE tab, not ctx.Indent+1: the caller indents the returned
				// statement by its own level, so this string is RELATIVE. An
				// absolute indentation here produced a body at 2*(indent+1) beside
				// a sibling at indent+1 -- a Python unindent error.
				std::string body = Body("Body");
				// Only publish when the graph actually DECLARES the variable. An
				// undeclared name would still work in Python (it would set an
				// attribute), but a graph that reads it back through GetVariable
				// would fail validation -- so emitting the write would be a silent
				// no-op that hides the authoring error instead of surfacing it.
				// A linear scan: Graphs carry a handful of variables, and this runs
					// once per compilation, not per tick.
				bool declared = false;
				if (ctx.Graph != nullptr)
					for (const GraphVariable &v : ctx.Graph->Variables)
						if (v.Name == clean)
						{
							declared = true;
							break;
						}
				std::string publish;
				if (declared)
					publish = "\tself." + clean + " = " + clean + "\n";
				// The Done marker, at the loop's OWN indent level (column 0 of this
				// relative string; the caller adds the base indent). Without it the
				// exec chain stops dead at the loop: the pin is declared, the
				// validator accepts a link into it, and the continuation is
				// SILENTLY DROPPED from the generated method. Task B.6 hit this --
				// the chaser's scan compiled and everything after it vanished.
				return "for " + clean + " in range(" + ctx.InInt("Count") +
					   "):\n" + publish + body + Body("Done");
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
			// ANY, not Float. The factory was float-only, which made `Equal` and
			// `NotEqual` FLOAT comparisons and left STRING and ENTITY equality
			// inexpressible -- the defect the comment below records. A graph that
			// must exclude SELF from a candidate scan has no other way to compare
			// two entities, and adding a second node would collide with this one
			// on the TypeId that already exists.
			//
			// `ctx.In` rather than `ctx.InFloat`: the pin's type is the author's
			// choice, and a float-only accessor would coerce a string or an entity
			// instead of passing it through.
			def.Pins = {
				MakeIn("A", "A", PinType::Any, false, "0"),
				MakeIn("B", "B", PinType::Any, false, "0"),
				// Bool, unlike addBinary's Float. This is the single behavioural
				// difference between the two factories.
				MakeOut("Value", "Value", PinType::Bool),
			};
			std::string operatorText = op;
			def.Emit = [operatorText](const EmitContext &ctx, EmitOutputs &out) {
				out.Outputs.push_back(
					{ "Value", "(" + ctx.In("A") + " " + operatorText + " " +
								   ctx.In("B") + ")" });
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
		// SCENE -- engine-facing calls are limited to the Python binding surface.
		// ================================================================

		{
			NodeDef def;
			def.TypeId = "GetPosition";
			def.Category = "Flow/Scene";
			def.Pins = {
				// An OPTIONAL target. Unset (the default) reads SELF, which keeps
				// every existing graph byte-identical.
				//
				// Why the pin exists at all: without it the node could only ever
				// read the entity the graph is attached to, so NO behaviour that
				// reasons about ANOTHER entity -- a chaser measuring distance to a
				// candidate, a weapon finding its wielder -- was expressible.
				// Task B.6 hit exactly that.
				MakeIn("Target", "Target", PinType::Entity, false, "None"),
				MakeOut("Position", "Position", PinType::Vec3),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// Entity.GetTransform returns a TransformComponent value whose bound
				// Translation member is the engine's current world position.
				//
				// The comparison against the literal "None" is how "unset" is
				// detected. An authored None means the same thing, which is fine.
				const std::string target = ctx.InEntity("Target");
				const std::string expression =
					(target == "None")
						? "self.entity.GetTransform().Translation"
						: ("((lambda t: t.GetTransform().Translation if t is not None "
						   "else Wasteland.Vec3(0.0, 0.0, 0.0))(" + target + "))");
				out.Outputs.push_back(
					{ "Position", expression });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			def.TypeId = "SetTransform";
			def.Category = "Flow/Scene";
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeExecOut("Out", "Then"),
				MakeIn("Translation", "Translation", PinType::Vec3, true, ""),
				MakeIn("Rotation", "Rotation", PinType::Vec3, false, "(0, 0, 0)"),
				MakeIn("Scale", "Scale", PinType::Vec3, false, "(1, 1, 1)"),
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &) {
				// Entity.SetTransform is bound as separate translation, rotation,
				// and scale Vec3 arguments; do not pass a TransformComponent object.
				return "self.entity.SetTransform(" + ctx.In("Translation") + ", " +
					   ctx.In("Rotation") + ", " + ctx.In("Scale") + ")";
			};
			registry.Register(def);
		}

		{
			// Vec3Add -- the vector counterpart of the Float arithmetic nodes.
			//
			// The five arithmetic nodes are Float-ONLY, so a flow graph had no way
			// to offset a position: GetPosition yields a Vec3 and SetTransform
			// consumes one, but nothing could add them. The most basic thing a
			// component graph does -- move something by a little, every tick --
			// was unauthorable.
			//
			// Deliberately ONE node rather than a vector-math family. A
			// Vec3Subtract or Vec3Scale can be added when a graph needs one;
			// adding them speculatively would be palette noise.
			NodeDef def;
			def.TypeId = "Vec3Add";
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("A", "A", PinType::Vec3, false, "(0, 0, 0)"),
				MakeIn("B", "B", PinType::Vec3, false, "(0, 0, 0)"),
				MakeOut("Value", "Value", PinType::Vec3),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// Vec3 is a pybind11 value type, so `+` is a normal Python
				// operator -- unlike the Float nodes, no operator text is spliced
				// in, and the parentheses are the only precedence guard needed.
				out.Outputs.push_back(
					{ "Value", "(" + ctx.InVec3("A") + " + " +
								   ctx.InVec3("B") + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			// Vec3Length -- the magnitude of a vector.
			//
			// Added for Task B.2's ClampLen function, which cannot be written
			// without it: clamping a vector to a maximum length is
			// `p * (maxLen / len(p))`, and every other piece of that already
			// existed. A length primitive is not a one-off -- it is the most
			// common vector operation there is.
			//
			// Emitted as an exponent rather than math.sqrt: the generated module
			// imports ONLY Wasteland (see CompileFlow's preamble), so referencing
			// math would NameError at runtime. `** 0.5` needs no import and is
			// exact for the magnitudes a length check compares.
			NodeDef def;
			def.TypeId = "Vec3Length";
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("Value", "Value", PinType::Vec3, false, "(0, 0, 0)"),
				MakeOut("Length", "Length", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				const std::string v = ctx.InVec3("Value");
				out.Outputs.push_back(
					{ "Length", "((" + v + ".x * " + v + ".x + " +
							   v + ".y * " + v + ".y + " + v + ".z * " + v +
							   ".z) ** 0.5)" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			// Vec3Scale -- componentwise multiply by a scalar.
			//
			// The counterpart to Vec3Length in ClampLen. Written as an explicit
			// per-component expression rather than `v * s` because Vec3's
			// pybind11 binding defines no __mul__ with a float scalar -- only
			// Vec3 + Vec3 exists (see Vec3Add).
			NodeDef def;
			def.TypeId = "Vec3Scale";
			def.Category = "Flow/Math";
			// The vector input is named "Vector", NOT "Value": an input and an
			// output may not share a pin id. The first version did, and the
			// link check resolved `Value` to the INPUT, so a correct graph
			// reported "pin 'Value' is an input, so it cannot be a link source".
			def.Pins = {
				MakeIn("Vector", "Vector", PinType::Vec3, false, "(0, 0, 0)"),
				MakeIn("Scale", "Scale", PinType::Float, false, "1"),
				MakeOut("Value", "Value", PinType::Vec3),
			};
		{
			// Vec3Normalize (Task B.6). The direction half of "walk toward the
			// target": scale the delta to unit length. Expressible as
			// Vec3Scale(delta, 1/len) but only with a divide, which did not exist.
			NodeDef def;
			def.TypeId = "Vec3Normalize";
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("Vector", "Vector", PinType::Vec3, true, "(0, 0, 0)"),
				MakeOut("Value", "Value", PinType::Vec3),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				const std::string v = ctx.InVec3("Vector");
				// Zero length is the same common case as Divide's zero divisor: a
				// graph standing on its target. The neutral answer is no direction.
				out.Outputs.push_back(
					{ "Value", "((lambda t: Wasteland.Vec3(0.0, 0.0, 0.0) "
							   "if t.Length() == 0 else Wasteland.Vec3(t.x / t.Length(), "
							   "t.y / t.Length(), t.z / t.Length()))(" + v + "))" });
				return std::string();
			};
			registry.Register(def);
		}

			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				const std::string v = ctx.InVec3("Vector");
				const std::string s = ctx.InFloat("Scale");
				out.Outputs.push_back(
					{ "Value", "Wasteland.Vec3(" + v + ".x * " + s + ", " +
							   v + ".y * " + s + ", " + v + ".z * " + s + ")" });
				return std::string();
			};
			registry.Register(def);
		}


		{
			NodeDef def;
			def.TypeId = "GetHealth";
			def.Category = "Flow/Scene";
			def.Pins = {
				// Optional target, same contract as GetPosition above. Reading a
				// candidate's health is the other half of "which entity is closest
				// and worth attacking".
				MakeIn("Target", "Target", PinType::Entity, false, "None"),
				MakeOut("Value", "Health", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// These exact bindings are exposed by Entity and HealthComponent.
				//
				// TryGet (not Get) for the TARGET: Get would raise on an entity
				// with no HealthComponent, and a candidate scan walks every entity
				// in the scene. A missing component reads as 0.0 -- dead, which is
				// the answer a scan wants.
				const std::string target = ctx.InEntity("Target");
				const std::string expression =
					(target == "None")
					? "self.entity.GetHealthComponent().Current"
						: ("((lambda t: (t.TryGetHealthComponent().Current "
					   "if t is not None and t.TryGetHealthComponent() is not None "
					   "else 0.0))(" + target + "))");
				out.Outputs.push_back({ "Value", expression });
				return std::string();
			};
			registry.Register(def);
		}

		{
			// ApplyDamage (Task B.6). The flow library had no damage node, so the
			// enemy's contact attack -- the whole point of the behaviour -- could
			// not be expressed in a graph. GetHealth reads; this writes.
			//
			// A STATEMENT, not an expression: it has an exec in/out so it sits in
			// the tick chain, and it returns nothing because a damage call has no
			// meaningful value to wire.
			NodeDef def;
			// "DamageEntity", NOT "ApplyDamage": the behaviour-tree library
			// already registers an `ApplyDamage` (a blackboard-keyed task), and
			// registration is keyed by TypeId with the LAST register winning -- so
			// a second node under that name silently REPLACES the tree's, and
			// every shipped .wlbtree using it changes behaviour.
			def.TypeId = "DamageEntity";
			def.Category = "Flow/Combat";
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeExecOut("Out", "Then"),
				// The TARGET, not self. Contact damage lands on whoever the enemy
				// is standing next to, which is never the enemy.
				MakeIn("Target", "Target", PinType::Entity, true, "None"),
				MakeIn("Amount", "Amount", PinType::Float, true, "0"),
				// Whether the call landed, so a graph can branch on it. HealthComponent
				// exposes Invulnerable and DamageCooldown, so a hit can be refused --
				// and a graph that cannot tell a refused hit from a landed one would
				// cooldown itself for nothing.
				MakeOut("Applied", "Applied", PinType::Bool),
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				const std::string target = ctx.In("Target");
				const std::string amount = ctx.InFloat("Amount");
				// The target may be None, or may not carry a HealthComponent -- a
				// graph that lost its target this tick still reaches this node.
				// TryGet returns None for both, so the guard covers both.
				//
				// The call goes through the TARGET's HealthComponent, not a Scene
				// method: Scene.ApplyDamage is not bound, and HealthComponent
				// already owns the invulnerability and cooldown rules that decide
				// whether a hit lands.
				const std::string call =
					"((lambda t: (t.TryGetHealthComponent().ApplyDamage(" +
					amount + ") if t is not None and "
					"t.TryGetHealthComponent() is not None else False))(" +
					target + "))";
				out.Outputs.push_back({ "Applied", call });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			def.TypeId = "Raycast";
			def.Category = "Flow/Scene";
			def.Pins = {
				MakeIn("Origin", "Origin", PinType::Vec3, true, ""),
				MakeIn("Direction", "Direction", PinType::Vec3, true, ""),
				MakeIn("MaxDistance", "Max Distance", PinType::Float, true, ""),
				MakeOut("Hit", "Hit", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// Scene.Raycast's existing binding returns a six-tuple beginning
				// with the hit flag. Expose that bool as an expression for Branch.
				out.Outputs.push_back(
					{ "Hit", "Wasteland.Scene.GetActiveScene().Raycast(" +
							   ctx.In("Origin") + ", " + ctx.In("Direction") + ", " +
							   ctx.InFloat("MaxDistance") + ")[0]" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			def.TypeId = "PlaySound";
			def.Category = "Flow/Scene";
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeExecOut("Out", "Then"),
				MakeIn("ClipName", "Clip Name", PinType::String, true, ""),
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &) {
				// The runtime sets this bound active-scene accessor before scripts
				// tick; Scene.PlaySound accepts one registered clip name.
				return "Wasteland.Scene.GetActiveScene().PlaySound(" +
					   ctx.In("ClipName") + ")";
			};
			registry.Register(def);
		}

		// ================================================================
		// ANIMATION -- the pose and the state machine, reachable from a graph.
		//
		// These are the nodes an Animation Blueprint's equivalents compile down to
		// in UE: a state change is a function call, and a bone transform is a read.
		// Both directions are covered -- PlayAnimation/SetAnimationState/
		// SetTrigger write, GetPoseBonePosition/GetPoseBoneRotation read.
		// ================================================================

		{
			NodeDef def;
			def.TypeId = "GetPoseBonePosition";
			def.Category = "Flow/Animation";
			def.Pins = {
				// A bone NAME, not an index: an index would silently address a
				// different bone on any rig whose bone order differs, which is the
				// same trap the retarget mapping exists to avoid.
				MakeIn("Bone", "Bone", PinType::String, true, "Arm"),
				// World rather than local. Local is the pose's own space; a muzzle
				// or footstep test needs world, so the common case is the default.
				MakeIn("World", "World", PinType::Bool, false, "true"),
				MakeOut("Position", "Position", PinType::Vec3),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// ctx.In returns a PARSED Python literal, so a String pin already
				// carries its own quotes -- wrapping it again would emit ""Arm"".
				// An empty pin becomes an empty string literal, which the helper
				// answers with None rather than raising.
				const std::string bone = ctx.In("Bone").empty() ? "\"\"" : ctx.In("Bone");
				const std::string lookup = "_pose_bone(self.entity, " + bone + ")";
				const std::string lookupWorld =
					"_pose_bone_world(self.entity, " + bone + ")";
				// InBool is a Python EXPRESSION, not a C++ bool, so the pin selects
				// between the two through a generated conditional -- a C++ ternary
				// would evaluate the pin's text as a compile-time truthiness test.
				//
				// Local reads the pose's own translation; world reads through
				// Skeleton.GetBoneWorldPosition, which recomputes the world matrices
				// -- correct, and O(bones) per query. A graph reading many bones per
				// tick should compute them once and index; that is a documented cost,
				// not a surprise.
				const std::string condition =
					ctx.InBool("World").empty() ? "True" : ctx.InBool("World");
				out.Outputs.push_back({ "Position", "(" + lookupWorld + " if " +
												 condition + " else " + lookup + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			def.TypeId = "GetPoseBoneRotation";
			def.Category = "Flow/Animation";
			def.Pins = {
				MakeIn("Bone", "Bone", PinType::String, true, "Arm"),
				// Vec4, not Quat: a flow pin is a plain value, and a quaternion is
				// four floats. The component order is (x, y, z, w) -- the same order
				// the Quat binding uses, and the OPPOSITE of glm's internal (w, x, y,
				// z), which is the trap that makes Quat(0,0,0,1) not an identity.
				MakeOut("Rotation", "Rotation", PinType::Vec4),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// A parsed literal, as in GetPoseBonePosition -- already quoted.
				const std::string bone = ctx.In("Bone").empty() ? "\"\"" : ctx.In("Bone");
				// A lambda so the lookup runs once; a bare attribute chain would ask
				// the engine twice per read when the bone is missing.
				const std::string lookup = "_pose_bone(self.entity, " + bone + ")";
				out.Outputs.push_back(
					{ "Rotation",
					  "(lambda b: None if b is None else (b.Rotation.x, b.Rotation.y, "
					  "b.Rotation.z, b.Rotation.w))(" + lookup + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			// The animation event entry point.
			//
			// A SECOND event node rather than the general FlowOnEvent, because an
			// author should not have to know that the engine's hook is named
			// "OnAnimationEvent" -- the same reason a palette has a BeginPlay node
			// instead of a generic Event node they must type a name into.
			//
			// The method name is derived by TYPE when the authored `event` field is
			// empty: Compiler.cpp maps an OnAnimationEvent node to the
			// "OnAnimationEvent" handler, so a graph that names the type and nothing
			// else is correct rather than silently compiling to OnUpdate.
			NodeDef def;
			def.TypeId = "OnAnimationEvent";
			def.Category = "Flow/Animation";
			def.Pins = { MakeExecOut("Out", "Then") };
			def.Emit = [](const EmitContext &, EmitOutputs &) {
				// The event node IS the method the compiler already opened; it emits
				// no statement of its own.
				return std::string();
			};
			registry.Register(def);
		}

		{
			// OnBeginPlay -- the Construction Script entry (plan B.1).
			//
			// A TYPE-named event, like OnAnimationEvent: nothing to author in the
			// `event:` field, because the type IS the hook. The dispatcher already
			// maps ScriptEventId::BeginPlay to the method name "OnBeginPlay"
			// (ScriptEventDispatcher.cpp:15), so this node needs no compiler
			// special case beyond GraphNode::IsTypeNamedEvent.
			//
			// Runs ONCE per runtime start, not per frame: Scene::OnRuntimeStart
			// dispatches BeginPlay, and FlowGraphSystem ticks only a graph whose
			// entry method is OnUpdate -- so a graph whose first event is this one
			// is inert between runs, which is exactly the construction-script
			// contract.
			NodeDef def;
			def.TypeId = "OnBeginPlay";
			def.Category = "Flow/Event";
			def.Pins = { MakeExecOut("Out", "Then") };
			def.Emit = [](const EmitContext &, EmitOutputs &) {
				return std::string();
			};
			registry.Register(def);
		}

		{
			// CallFunction -- Task B.2.
			//
			// The pin set is RESOLVED per node, because one pin per PARAMETER of
			// the named function is the entire point: parameter names are
			// author-supplied and differ between functions, so no static vector
			// can describe them.
			//
			// ResolvePins is shared by the validator (to type-check links) and
			// the compiler (to emit), via NodeRegistry::EffectivePins -- the same
			// consolidation that B.1 made for IsTypeNamedEvent. A resolver that
			// answered differently in the two would let a graph validate and then
			// miscompile.
			NodeDef def;
			def.TypeId = "CallFunction";
			def.Category = "Flow/Function";
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeExecOut("Out", "Then"),
				MakeIn("Name", "Function", PinType::String, true, ""),
			};
			def.ResolvePins = [](const Graph &graph, const GraphNode &node)
				-> std::vector<PinDef>
			{
				std::vector<PinDef> pins = {
					MakeExecIn("In", "In"),
					MakeExecOut("Out", "Then"),
					MakeIn("Name", "Function", PinType::String, true, ""),
				};

				// The selected function's Name literal. Empty when unset or
				// unparsable, which is the validator's to report -- the resolver
				// just returns the base pins so nothing downstream crashes.
				std::string functionName;
				for (const PinLiteral &literal : node.Literals)
				{
					if (literal.Key != "Name")
						continue;
					std::string parsed;
					std::string parseError;
					if (ParseLiteral(PinType::String, literal.Value, parsed,
									 parseError))
						// ParseLiteral yields a QUOTED Python literal; the
						// declaration's Name is raw text. Without unquoting the
						// comparison never matched, so the resolver returned only
						// the base pins and the node looked like it had no
						// parameters at all.
						functionName = UnquotePyLiteral(parsed);
				}

				if (functionName.empty())
					return pins;

				// Lookup by name. A miss returns the base pins; the validator
				// reports "unknown function" against the node.
				const GraphFunction *found = nullptr;
				for (const GraphFunction &function : graph.Functions)
				{
					if (function.Name == functionName)
					{
						found = &function;
						break;
					}
				}
				if (found == nullptr)
					return pins;

				// One input pin per declared parameter, named after it. Required:
				// a function argument is not optional, and the validator's
				// required-input check must be able to see it.
				for (const GraphFunctionEndpoint &input : found->Inputs)
					pins.push_back(MakeIn(input.Name, input.Name, input.Type, true,
										  ""));

				// One output pin per declared return value.
				for (const GraphFunctionEndpoint &output : found->Outputs)
					pins.push_back(MakeOut(output.Name, output.Name, output.Type));

				return pins;
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out)
				-> std::string
			{
				// The call itself is a STATEMENT: it advances the exec chain and
				// assigns the results to temps that the output pins read.
				//
				// `self._fn_<Name>` is emitted by CompileFlow on the same class,
				// so the call and the body cannot drift apart -- there is no
				// module-level registry to look up.
				std::string functionName = ctx.In("Name");
				// ctx.In returns an ALREADY-QUOTED Python literal (B.1 trap).
				// Stripping the quotes here would be wrong; the name is used both
				// as an identifier fragment and as a call argument.
				// ctx.In returns an already-quoted Python literal; the method
				// name needs the raw text. UnquotePyLiteral is the shared
				// inverse of ParseLiteral, so the two cannot drift.
				const std::string bareName =
					UnquotePyLiteral(functionName);

				std::string method = Compiler::SanitizeIdentifier(bareName, "fn");
				if (method.empty())
					return "CallFunction: the function name is empty";

				// Arguments in DECLARATION order, read from the resolved inputs.
				std::vector<std::string> args;
				for (const auto &input : ctx.Inputs)
					args.push_back(input.second);

				std::string call = "self._fn_" + method + "(";
				for (size_t i = 0; i < args.size(); ++i)
				{
					if (i)
						call += ", ";
					call += args[i];
				}
				call += ")";

				// One temp holding the whole returned tuple. The output pins
				// index into it, so a single-output function still round-trips
				// through the same shape -- one code path, not two.
				// The call expression is handed to the compiler as this node's
				// OUTPUT, not emitted as a statement here.
				//
				// Why not ctx.AllocTemp: the Compiler never wires that callback
				// (EmitContext::AllocTemp is declared, and nothing in Compiler.cpp
				// assigns it), so calling it would throw std::bad_function_call.
				// More importantly, emitOwnStatement ALREADY writes
				// `self._vb_<node>_<pin> = <expression>` for every output pin --
				// so returning the expression here produces exactly one assignment
				// and successors read it back through the normal temp attribute.
				//
				// The exec chain is advanced separately: this node's exec OUT is
				// followed by the walker, and the statement it needs is the
				// assignment the compiler writes.
				const Graph *graph = ctx.Graph;
				const GraphNode *node = ctx.Node;
				if (graph != nullptr && node != nullptr)
				{
					const NodeDef *self = NodeRegistry::Get().Find(node->TypeId);
					if (self != nullptr && self->ResolvePins)
					{
						for (const PinDef &pin : self->ResolvePins(*graph, *node))
						{
							// Exec outputs carry no value; they are control edges.
							if (pin.Dir != PinDir::Out || pin.Type == PinType::Exec)
								continue;
							out.Outputs.emplace_back(pin.Id, call);
						}
					}
				}

				// A statement node's return value is its statement text. There is
				// none here: the assignment is emitted by the compiler from the
				// output above, and the exec edge is a link.
				return std::string();
			};
			registry.Register(def);
		}

		{
			// Max -- the larger of two floats.
			//
			// Added for Task B.2's ClampLen, which needs a branch-free clamp:
			// `p * (maxLen / max(len(p), maxLen))` shrinks an over-long vector and
			// leaves a short one alone, with no Branch and therefore no second
			// Return node. A clamp expressed with a Branch needs two returns, and
			// the flow exec walker emits a converged node only ONCE -- so the
			// branch version silently drops one arm.
			NodeDef def;
			def.TypeId = "Max";
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("A", "A", PinType::Float, false, "0"),
				MakeIn("B", "B", PinType::Float, false, "0"),
				MakeOut("Value", "Value", PinType::Float),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				out.Outputs.push_back(
					{ "Value", "max(" + ctx.InFloat("A") + ", " +
							   ctx.InFloat("B") + ")" });
				return std::string();
			};
			registry.Register(def);
		}


		{
			// Compare -- float comparison producing a Bool, for Branch's
			// Condition pin.
			//
			// Added for Task B.2's ClampLen body, which branches on
			// `length > maxLen`. NodeLibraryV1 has a `Compare` too, but it is a
			// BEHAVIOUR-TREE leaf: it emits a lambda returning SUCCESS/FAILURE
			// and is rejected by the tree validator's role rules. The two share
			// a TypeId across libraries on purpose -- the registry is per-library
			// -- so this is the flow-shaped one.
			NodeDef def;
			// "CompareValue", NOT "Compare": the behaviour-tree library already
			// registers a `Compare` TypeId with role `condition`, and
			// registration is keyed by TypeId -- so two libraries sharing one
			// made RuntimeTypes() answer a single role and the node library
			// test failed on "Compare has role 'condition'". The name states
			// what this one does: it returns a BOOL for a Branch condition,
			// where the tree's Compare emits a SUCCESS/FAILURE lambda.
			def.TypeId = "CompareValue";
			def.Category = "Flow/Math";
			def.Pins = {
				MakeIn("A", "A", PinType::Float, false, "0"),
				MakeIn("B", "B", PinType::Float, false, "0"),
				MakeIn("Op", "Op", PinType::String, false, ">"),
				MakeOut("Value", "Value", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// The operator is validated against a closed set rather than
				// spliced in blindly: an authored `Op` is a STRING PIN, so
				// without this a typo would emit `a >x b` and surface as a
				// SyntaxError in generated code.
				static const std::set<std::string> allowed = {
					"==", "!=", "<", "<=", ">", ">="
				};
				std::string op = ctx.In("Op");
				if (op.size() >= 2 && op.front() == '"' && op.back() == '"')
					op = op.substr(1, op.size() - 2);
				if (allowed.count(op) == 0)
					op = ">";

				out.Outputs.push_back(
					{ "Value", "(" + ctx.InFloat("A") + " " + op + " " +
							   ctx.InFloat("B") + ")" });
				return std::string();
			};
			registry.Register(def);
		}


		{
			// FunctionEntry -- the implicit entry point of a `kind: function`
			// graph (Task B.2).
			//
			// Its output pins are RESOLVED from the graph's declared function, so
			// each parameter appears as a named, typed pin. That is what lets an
			// author wire a parameter into the body without a variable read: the
			// emitted expression is the bare LOCAL name, not `self.<name>`, which
			// is correct because a function has no instance state to read.
			//
			// The alternative -- routing parameters through GetVariable -- would
			// emit `self.p`, and `self.p` does not exist on a function method.
			NodeDef def;
			def.TypeId = "FunctionEntry";
			def.Category = "Flow/Function";
			// The exec OUT is STATIC -- it does not depend on the function
			// signature -- so it belongs in def.Pins. Leaving it out made the
			// registry report an empty pin list for a node that genuinely has
			// one, and the schema suite then demanded a doc row with no pins.
			// The parameter outputs stay in ResolvePins: their names and types
			// come from the signature, which no static list can describe.
			def.Pins = { MakeExecOut("Out", "Then") };
			def.ResolvePins = [](const Graph &graph, const GraphNode &)
				-> std::vector<PinDef>
			{
				std::vector<PinDef> pins = { MakeExecOut("Out", "Then") };
				if (!graph.Functions.empty())
				{
					for (const GraphFunctionEndpoint &input :
						 graph.Functions[0].Inputs)
						pins.push_back(MakeOut(input.Name, input.Name, input.Type));
				}
				return pins;
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out)
				-> std::string
			{
				// One output per parameter, named after it, whose expression is
				// the method's own argument. ctx.Node is set, so the resolver's
				// pin list is available for the ordering.
				//
				// The exec OUT is what makes a body with no Branch walkable:
				// findFunctionEntry looks for a node with an exec out and no
				// incoming exec, and a pure data chain (entry -> ... -> Return)
				// has none without it. The author wires entry.Out to Return.In
				// exactly as they would wire an event to its first node.
				const Graph *graph = ctx.Graph;
				const GraphNode *node = ctx.Node;
				if (graph == nullptr || node == nullptr)
					return std::string();

				const NodeDef *self = NodeRegistry::Get().Find(node->TypeId);
				if (self == nullptr || !self->ResolvePins)
					return std::string();

				for (const PinDef &pin : self->ResolvePins(*graph, *node))
				{
					// Exec outputs carry no value; they are control edges.
					if (pin.Dir != PinDir::Out || pin.Type == PinType::Exec)
						continue;
					const std::string name = Compiler::SanitizeIdentifier(pin.Id, "arg");
					if (name.empty())
						return "FunctionEntry: a parameter name is empty";
					out.Outputs.emplace_back(pin.Id, name);
				}
				return std::string();
			};
			registry.Register(def);
		}


		{
			// Return -- the terminator of a `kind: function` graph (Task B.2).
			//
			// Its pins are RESOLVED from the graph's single declared function, so
			// a return statement always matches the signature the caller was
			// compiled against. A mismatch is a validation error, not a runtime
			// tuple of the wrong arity.
			//
			// Only meaningful inside a function graph; the validator rejects a
			// Return in a flow graph.
			NodeDef def;
			def.TypeId = "Return";
			def.Category = "Flow/Function";
			def.Pins = { MakeExecIn("In", "In") };
			def.ResolvePins = [](const Graph &graph, const GraphNode &)
				-> std::vector<PinDef>
			{
				std::vector<PinDef> pins = { MakeExecIn("In", "In") };
				// A function graph declares exactly one function: itself. Taking
				// [0] is correct there; on a flow graph this node is rejected by
				// the validator, so an empty list is never observed in practice.
				if (!graph.Functions.empty())
				{
					for (const GraphFunctionEndpoint &output :
						 graph.Functions[0].Outputs)
						pins.push_back(MakeIn(output.Name, output.Name, output.Type,
										  true, ""));
				}
				return pins;
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &)
				-> std::string
			{
				// The returned tuple, in DECLARATION order. A single output still
				// returns a one-tuple -- the caller indexes [0] either way, so
				// there is one code shape rather than two.
				std::vector<std::string> values;
				for (const auto &input : ctx.Inputs)
					values.push_back(input.second);

				std::string statement = "return (";
				for (size_t i = 0; i < values.size(); ++i)
				{
					if (i)
						statement += ", ";
					statement += values[i];
				}
				// A trailing comma for the one-element case is what makes it a
				// tuple rather than a parenthesised expression.
				if (values.size() == 1)
					statement += ",";
				statement += ")\n";
				return statement;
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

		// ==================================================== Flow/Component
		//
		// Task B.3. A component is addressed by NAME (a string pin) rather than by
		// a pin per type, because a pin per type would need a registry lookup at
		// AUTHORING time and the validator already cannot know what a runtime
		// entity carries. The name is resolved by a generated helper that
		// dispatches over the bound TryGet*/AddOr* accessors.
		//
		// The helpers are emitted only when a graph contains a node that needs
		// them -- the same rule as _pose_bone.
		{
			NodeDef def;
			def.TypeId = "CastTo";
			def.Category = "Flow/Component";
			def.Pins = {
				MakeIn("Entity", "Entity", PinType::Entity, true, "None"),
				// The component type NAME, exactly as bound on Entity: HealthComponent,
				// TransformComponent, and so on. Not a class reference -- this engine
				// has no reflection, so a name is the only handle there is.
				MakeIn("Type", "Component Type", PinType::String, true, ""),
				// Any, because the component's type is the AUTHOR's choice. Pinning it
				// to a concrete type would make every other component uncastable.
				MakeOut("Value", "Value", PinType::Any),
				MakeOut("Success", "Success", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				const std::string entity = ctx.In("Entity");
				const std::string type = Unquote(ctx.In("Type"));
				// `_component_get` returns None when the entity is null, the type is
				// unknown, or the component is absent -- three different reasons that
				// all read as "no component", which is exactly UE's Cast To result.
				//
				// The Success output is computed from the SAME expression rather than
				// a second lookup: two lookups would be two opportunities to disagree,
				// and a graph reading Value and Success from the same node must see
				// one answer.
				out.Outputs.push_back(
					{ "Value", "_component_get(" + entity + ", \"" + type + "\")" });
				out.Outputs.push_back(
					{ "Success", "(_component_get(" + entity + ", \"" + type +
							"\") is not None)" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// The same operation as CastTo without the Success pin: GetComponent is
			// the spelling an author uses when a None is a fine answer and a branch
			// on Success would be noise. Both emit the SAME helper -- two helpers
			// would be two implementations of one question.
			def.TypeId = "GetComponent";
			def.Category = "Flow/Component";
			def.Pins = {
				MakeIn("Entity", "Entity", PinType::Entity, true, "None"),
				MakeIn("Type", "Component Type", PinType::String, true, ""),
				MakeOut("Value", "Value", PinType::Any),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				out.Outputs.push_back(
					{ "Value", "_component_get(" + ctx.In("Entity") + ", \"" +
							Unquote(ctx.In("Type")) + "\")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// AddOrReplace, by name. The component VALUE arrives as Any because its
			// type is the author's choice; the helper passes it straight through to
			// the bound AddOr<Name> accessor.
			def.TypeId = "SetComponent";
			def.Category = "Flow/Component";
			// Exec pins, because this is a STATEMENT: it mutates the entity and
			// must sit in the exec chain. The first version omitted them, so a
			// correct graph reported "'SetComponent' has no input pin 'In'" --
			// a node that cannot be wired to anything.
			def.Pins = {
				MakeExecIn("In", "In"),
				MakeExecOut("Out", "Then"),
				MakeIn("Entity", "Entity", PinType::Entity, true, "None"),
				MakeIn("Type", "Component Type", PinType::String, true, ""),
				MakeIn("Value", "Value", PinType::Any, false, "None"),
				MakeOut("Success", "Success", PinType::Bool),
			};
			def.IsStatement = true;
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				out.Outputs.push_back(
					{ "Success", "_component_set(" + ctx.In("Entity") + ", \"" +
							Unquote(ctx.In("Type")) + "\", " + ctx.In("Value") + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// The graph's OWN entity. In this engine there is no owner relationship
			// (no OwnerComponent exists), so GetActor and GetOwner answer the same
			// question and both are documented as such -- a second spelling rather
			// than a second concept.
			def.TypeId = "GetActor";
			def.Category = "Flow/Component";
			def.Pins = { MakeOut("Actor", "Actor", PinType::Entity) };
			def.Emit = [](const EmitContext &, EmitOutputs &out) {
				out.Outputs.push_back({ "Actor", "self.entity" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			def.TypeId = "GetOwner";
			def.Category = "Flow/Component";
			def.Pins = { MakeOut("Owner", "Owner", PinType::Entity) };
			def.Emit = [](const EmitContext &, EmitOutputs &out) {
				out.Outputs.push_back({ "Owner", "self.entity" });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// The typed scene query, generalised from SenseNearest's "nearest with
			// a Transform". The component type is a name; the helper dispatches
			// over Scene's bound queries and returns a Python list.
			//
			// Count and Entities are BOTH emitted because a list pin type does not
			// exist: an author loops over Count and reads by index, or consumes
			// Entities as an opaque list.
			def.TypeId = "GetAllEntitiesWith";
			def.Category = "Flow/Component";
			def.Pins = {
				MakeIn("Type", "Component Type", PinType::String, true, ""),
				MakeOut("Count", "Count", PinType::Int),
				MakeOut("Entities", "Entities", PinType::Any),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				const std::string list =
					"_entities_with(self.scene, \"" + Unquote(ctx.In("Type")) + "\")";
				out.Outputs.push_back({ "Count", "len(" + list + ")" });
				out.Outputs.push_back({ "Entities", list });
				return std::string();
			};
			registry.Register(def);
		}

		{
			NodeDef def;
			// The list accessor that makes Entities/Count usable: index a query
			// result. `List` is Any because the only producer is the query node,
			// whose output is a Python list.
			def.TypeId = "GetEntityAtIndex";
			def.Category = "Flow/Component";
			def.Pins = {
				MakeIn("List", "List", PinType::Any, true, "None"),
				MakeIn("Index", "Index", PinType::Int, false, "0"),
				MakeOut("Entity", "Entity", PinType::Entity),
				MakeOut("Valid", "Valid", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				const std::string list = ctx.In("List");
				const std::string index = ctx.In("Index");
				// A bounds check rather than a bare subscript: an out-of-range index
				// raising IndexError from generated code is a confusing way to learn
				// a loop ran one iteration long, and Valid is the branch an author
				// actually wants.
				const std::string item = ("(((" + list + ")[(" + index + ")]"
					" if 0 <= (" + index + ") < len(" + list + ") else None))");
				out.Outputs.push_back({ "Entity", item });
				out.Outputs.push_back(
					{ "Valid", "0 <= (" + index + ") < len(" + list + ")" });
				return std::string();
			};
			registry.Register(def);
		}

		}
	}

}
