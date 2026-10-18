// wlpch.h is the project's forced-include prefix (Wasteland/src/wlpch.h).
// It has to come first: it drags in iostream/string/vector and the engine
// log macros, which much of this module assumes are already declared.
#include "wlpch.h"

// The module's own header immediately after the prefix, by house convention.
// It is what declares PinType/GraphNode/GraphLink and the free functions
// defined further down (PinTypeName, IsAssignable, ParseLiteral, ...).
#include "Graph.h"

// DEFECT (unused include): nothing in this translation unit calls into
// <algorithm> -- there is no std::sort/find/min/clamp anywhere below.
// Appears to be leftover from an earlier revision. Harmless, but it inflates
// the module's apparent dependencies.
#include <algorithm>
// Used exactly once, by std::tolower in ParseBool below. The (unsigned char)
// cast at that call site is what makes it safe for bytes >= 0x80.
#include <cctype>
// std::strtod and std::strtol, used by ParseFloat and ParseInt. They are
// preferred over streams here because the whole point is to REJECT a value
// that is only partially numeric, which needs the end-pointer convention.
#include <cstdlib>
// std::stringstream is the only thing that splits the comma-separated
// contents of a vector literal, in ParseVec below.
#include <sstream>

// Nested namespace definition (C++17). VisualBlocks is a leaf module inside
// the engine namespace; nothing here leaks into Wasteland:: proper.
namespace Wasteland::VisualBlocks
{

	// ---------------------------------------------------------------- literals

	// Const overload of the literal lookup, used by the validator and the
	// emitter when they only need to read a pin's authored value.
	// Linear scan: a node has a handful of literals, so an index would cost more
	// in bookkeeping (and in invalidated-pointer risk) than the comparisons do.
	const PinLiteral *GraphNode::FindLiteral(const std::string &pinId) const
	{
		// Range-for over the literal list. Literals is a PinLiteralList (a
		// std::vector alias) -- see Graph.h, where it is declared opaque to pybind11
		// so Python sees a live list rather than a by-value copy.
		for (const PinLiteral &literal : Literals)
		{
			// Identity is the literal's Key, which holds a PIN ID (a stable string),
			// never a vector index. That is what lets a node type gain a pin without
			// breaking every saved graph that used the old layout.
			if (literal.Key == pinId)
				return &literal;
		}
		// Null means 'this pin has no authored literal', which the caller must
		// distinguish from 'literal authored as the empty string' -- IsSet on the
		// returned record carries that second case.
		return nullptr;
	}

	// The mutable overload, for callers that patch a literal in place (the UI
	// writing a value back into a loaded node). Deliberately a separate function
	// rather than a const_cast: the const version hands back const PinLiteral *.
	PinLiteral *GraphNode::FindLiteral(const std::string &pinId)
	{
		for (PinLiteral &literal : Literals)
		{
			if (literal.Key == pinId)
				return &literal;
		}
		return nullptr;
	}

	// Upsert rather than a strict setter: SetLiteral creates the record when the
	// pin id is unknown, so a node type that declares a pin after the graph was
	// authored still round-trips instead of dropping the value on save.
	void GraphNode::SetLiteral(const std::string &pinId, const std::string &value,
							   bool isSet)
	{
		// Search first, append after. The append happens once, outside the loop,
		// because pushing inside it would invalidate the iteration.
		for (PinLiteral &literal : Literals)
		{
			if (literal.Key == pinId)
			{
				// Both fields are written unconditionally: overwriting with a non-set value
				// is how the UI clears a pin, and there is no RemoveLiteral, so an explicit
				// IsSet=false must be able to replace a previous IsSet=true.
				literal.Value = value;
				literal.IsSet = isSet;
				return;
			}
		}
		// Default-constructs, then fills all three fields. PinLiteral's own default
		// (IsSet = false) means no member is left indeterminate even before the
		// assignments below, so there is no uninitialised-read window here.
		PinLiteral literal;
		literal.Key = pinId;
		literal.Value = value;
		literal.IsSet = isSet;
		// push_back copies the local; move would be a wash for two short strings,
		// and copying keeps this obviously correct.
		Literals.push_back(literal);
	}

	// ------------------------------------------------------------------ graph

	// Graph-level node lookup, const version. Nodes are searched by the stable
	// Id string; a miss returns nullptr rather than an optional or a throw, so
	// every caller is forced by the compiler to handle 'no such node'.
	const GraphNode *Graph::FindNode(const std::string &id) const
	{
		for (const GraphNode &node : Nodes)
		{
			if (node.Id == id)
				return &node;
		}
		return nullptr;
	}

	// Mutable twin of the above. Having both means a caller that intends to
	// mutate cannot accidentally reach a const GraphNode via the const overload.
	GraphNode *Graph::FindNode(const std::string &id)
	{
		for (GraphNode &node : Nodes)
		{
			if (node.Id == id)
				return &node;
		}
		return nullptr;
	}

	// Returns the single event node, or nullptr. A flow graph has exactly one; a
	// behaviour tree has none (it ticks from OnUpdateEntity instead), which is
	// why a scan -- not a direct index -- is what identifies it.
	const GraphNode *Graph::FindEvent() const
	{
		for (const GraphNode &node : Nodes)
		{
			// 'EventName is non-empty' is the definition of being an event node: a flow
			// graph's entry node carries the engine hook it hangs off; an ordinary node
			// leaves the string empty.
			// DEFECT (unvalidated invariant): Graph.h says only one event node per graph
			// may exist, but a hand-edited or merge-damaged file with two returns the
			// FIRST and never reports the duplicate. Correctness here relies on the
			// validator rather than on this function.
			if (!node.EventName.empty())
				return &node;
		}
		return nullptr;
	}

	// The pin-resolution primitive. A pin's value is 'the link if one exists,
	// otherwise the literal' -- so this returns BOTH the link ids and the
	// literal, and the caller decides which to follow.
	bool Graph::ResolveInput(const std::string &nodeId, const std::string &pinId,
							 PinLiteral &outLiteral, bool &outIsLinked,
							 std::string &outLinkNode, std::string &outLinkPin) const
	{
		// Every out-parameter is reset before any early return, so a caller that
		// ignores the bool still never reads a value left over from a previous call.
		outIsLinked = false;
		outLinkNode.clear();
		outLinkPin.clear();
		// Value-initialised PinLiteral: empty Key/Value, IsSet=false. Deliberate --
		// an uninitialised PinLiteral would make 'no literal' indistinguishable from
		// a garbage one on the not-found paths below.
		outLiteral = PinLiteral();

		// A link wins over a literal. Both present is not an error here -- the
		// validator reports it -- but the link is what the emitter must follow.
		// Links are scanned first and unconditionally: a link WINS over a literal.
		// Having both is a validation error, but the emitter must follow the wire, so
		// this returns the link and leaves outLiteral at its reset value.
		for (const GraphLink &link : Links)
		{
			// Matched on the destination node AND pin id. Both halves are required: a pin
			// id alone would collide across every node in the graph.
			if (link.ToNode == nodeId && link.ToPin == pinId)
			{
				// IsLinked plus the producer ids are the whole 'follow the wire' answer.
				// outLinkNode/outLinkPin name the upstream node's OUTPUT pin, which is what
				// the emitter recurses through.
				outIsLinked = true;
				outLinkNode = link.FromNode;
				outLinkPin = link.FromPin;
				return true;
			}
		}

		// The literal lookup only runs when no link matched, and only after the node
		// itself is confirmed to exist -- a missing node and a missing literal are
		// both 'false', but they mean different things to the caller.
		const GraphNode *node = FindNode(nodeId);
		if (!node)
			return false;

		const PinLiteral *literal = node->FindLiteral(pinId);
		if (!literal)
			return false;

		// The literal is copied out rather than handed back as a pointer because the
		// caller (the emitter) is about to parse it into a Python expression and has
		// no reason to alias the graph's own storage.
		outLiteral = *literal;
		return true;
	}

	// Type-name round trip. PinTypeName and PinTypeFromName are inverses of each
	// other, and the strings they share are the persisted representation of a pin
	// type in .wlblocks / .wlbtree files.
	// ------------------------------------------------------------------- types

	const char *PinTypeName(PinType type)
	{
		// Exhaustive switch with no default case, so adding an enumerator without
		// handling it here is a -Wswitch warning rather than a silent fallthrough.
		switch (type)
		{
		case PinType::Exec:   return "exec";
		case PinType::Bool:   return "bool";
		case PinType::Int:    return "int";
		case PinType::Float:  return "float";
		case PinType::String: return "string";
		case PinType::Vec2:   return "vec2";
		case PinType::Vec3:   return "vec3";
		case PinType::Vec4:   return "vec4";
		case PinType::Entity: return "entity";
		case PinType::Any:    return "any";
		}
		// Unreachable for any value in the enum as written -- every case above
		// returns. It satisfies the compiler and yields a usable string if a
		// persisted ordinal is out of range, which is a real possibility given the
		// appended-ordinals note in Graph.h (a graph from a newer engine build).
		return "unknown";
	}

	// The inverse of PinTypeName. Returns false and leaves `out` untouched for an
	// unrecognised name, so callers must check the bool rather than trusting a
	// defaulted `out`.
	bool PinTypeFromName(const std::string &name, PinType &out)
	{
		// A flat if-chain rather than a table, so each line reads as one mapping.
		// MAINTAINABILITY DEFECT: these literals duplicate PinTypeName above, so the
		// two tables can drift and a rename touching one but not the other silently
		// breaks round-tripping of saved pin types. One shared table keyed by name
		// would remove the coupling.
		if (name == "exec")   { out = PinType::Exec;   return true; }
		if (name == "bool")   { out = PinType::Bool;   return true; }
		if (name == "int")    { out = PinType::Int;    return true; }
		if (name == "float")  { out = PinType::Float;  return true; }
		if (name == "string") { out = PinType::String; return true; }
		if (name == "vec2")   { out = PinType::Vec2;   return true; }
		if (name == "vec3")   { out = PinType::Vec3;   return true; }
		if (name == "vec4")   { out = PinType::Vec4;   return true; }
		if (name == "entity") { out = PinType::Entity; return true; }
		if (name == "any")    { out = PinType::Any;    return true; }
		// No trimming and no case folding: a saved pin type must match exactly what
		// the writer emitted. Being strict is deliberate -- a pin type that does not
		// round-trip is a file-format bug, and failing loudly beats guessing.
		return false;
	}

	// The one place the type system's compatibility rules live. Called by the
	// validator for every wire in a graph, so it sits on the graph-compilation
	// hot path (not the frame path).
	bool IsAssignable(PinType from, PinType to)
	{
		// Identity first. Also short-circuits Any->Any and Exec->Exec.
		if (from == to)
			return true;
		// int promotes to float. Nothing else: bool->float would make a
		// comparison silently become a number, and string->anything would make
		// a typo a runtime error instead of a compile error.
		// The ONLY implicit conversion. int widening to float is lossless in the
		// direction that matters (float holds every int exactly up to 2^24), and it
		// is what lets a count literal drive a float parameter pin.
		if (from == PinType::Int && to == PinType::Float)
			return true;
		// A wildcard adopts whatever arrives. This is deliberately NOT a
		// blanket pass on either side: Exec stays unconnectable to data, so
		// "an exec wire into a data pin" remains a real error even when that
		// data pin is a wildcard.
		// A wildcard adopts whatever arrives -- but only between two DATA types.
		// Both directions are handled here, so 'Any' as a source and as a target
		// behave the same rather than only one being a pass.
		if (to == PinType::Any || from == PinType::Any)
			// Exec is excluded on BOTH sides, so an exec wire into a wildcard data pin
			// still fails to connect. Deliberate, and consistent with Graph.h's
			// rationale -- but it means Any is NOT a universal escape hatch, which the
			// name alone suggests.
			return from != PinType::Exec && to != PinType::Exec;
		return false;
	}

	// -------------------------------------------------------------- conversion

	// Anonymous namespace: these four parsers are file-local helpers with no
	// external contract, so internal linkage keeps them out of the module's
	// symbol table and free of name-collision risk.
	namespace
	{
		// Trims ASCII whitespace only (space, tab, CR, LF) -- no locale, no Unicode.
		// Sufficient for values authored in the node editor's single-line field.
		std::string Trim(const std::string &text)
		{
			// First non-whitespace index. npos means the string is empty or all
			// whitespace, which the branch below turns into an empty std::string.
			size_t first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return std::string();
			// Last non-whitespace index; +1 turns the inclusive end into a length.
			size_t last = text.find_last_not_of(" \t\r\n");
			return text.substr(first, last - first + 1);
		}

		// strtod-based float parse that REQUIRES the whole trimmed string to
		// convert. This strictness is the point: '3abc' must be a reported error,
		// not a silent 3.0 that then behaves unlike what the author typed.
		bool ParseFloat(const std::string &text, float &out)
		{
			// Guard before the C call. strtod on an empty string is legal and consumes
			// nothing, which the end-pointer check below would also catch -- this early
			// return just avoids the call.
			if (text.empty())
				return false;
			char *end = nullptr;
			// strtod also skips leading whitespace and accepts forms a user can type
			// that are not really float literals: 'inf', 'nan', and C99 hex floats like
			// '0x1p3'. DEFECT: none are rejected, so ParseLiteral can succeed and emit
			// the text 'inf' or 'nan' into generated Python, where they are NameErrors at
			// runtime instead of compile errors. It is also locale-dependent for the
			// decimal separator.
			double value = std::strtod(text.c_str(), &end);
			// No characters consumed at all -- this is how a non-numeric string is
			// detected. Comparing the pointer, not the value, is the standard idiom.
			if (end == text.c_str())
				return false;
			// Tolerates trailing spaces and tabs only (not CR/LF) because the input was
			// already trimmed. Harmless redundancy, not a second full trim.
			while (*end == ' ' || *end == '\t')
				end++;
			// Anything left over means the string was only partially numeric, and that
			// is rejected. This is the line that makes '3abc' an error rather than 3.0.
			if (*end != '\0')
				return false;
			// DEFECT: the narrowing double->float cast is unchecked. '1e400' parses to
			// +inf here and travels on as the text 'inf'; the float arm of ParseLiteral
			// below notices that spelling and declines to append '.0', but the value
			// that reaches the emitter is still not valid Python.
			out = (float)value;
			return true;
		}

		// The int twin of ParseFloat. Base 10 is explicit so a leading '0' cannot be
		// read as octal -- '010' must mean ten here, not eight.
		bool ParseInt(const std::string &text, int &out)
		{
			if (text.empty())
				return false;
			char *end = nullptr;
			// strtol returns long. DEFECT: overflow is not detected. strtod/strtol clamp
			// to LONG_MAX/LONG_MIN and set errno, which is never inspected, so an
			// out-of-range literal is silently saturated rather than reported.
			long value = std::strtol(text.c_str(), &end, 10);
			if (end == text.c_str())
				return false;
			while (*end == ' ' || *end == '\t')
				end++;
			if (*end != '\0')
				return false;
			// DEFECT: this narrowing long->int cast is unchecked, and where long is 64-bit
			// ('9999999999') it truncates rather than saturating. A range check against
			// numeric_limits<int> here would turn both overflow and truncation into
			// reported errors.
			out = (int)value;
			return true;
		}

		// Boolean literals are accepted in three spellings per polarity, so a value
		// typed from a config file or a tooltip ('yes'/'no') works unchanged.
		bool ParseBool(const std::string &text, bool &out)
		{
			// Lowercased into a COPY rather than mutating `text`: the argument is a const
			// reference that may be the caller's own buffer.
			std::string lower;
			for (char c : text)
				lower += (char)std::tolower((unsigned char)c);
			// Truthy set first, then falsy. The sets are disjoint so the order is not
			// load-bearing, but keeping both as separate early returns keeps the mapping
			// readable.
			if (lower == "true" || lower == "1" || lower == "yes")
			{
				out = true;
				return true;
			}
			if (lower == "false" || lower == "0" || lower == "no")
			{
				out = false;
				return true;
			}
			return false;
		}

		// Parse a Python-style quoted string, or a bare token. Storing the
		// canonical quoted form keeps round-tripping byte-identical.
		// Produces a Python string literal, always in canonical double-quoted form,
		// regardless of how the author typed it. That is what makes a save/load cycle
		// converge: 'abc', "abc" and bare abc all come back out as "abc".
		std::string ParseString(const std::string &text)
		{
			// Trims here as well as in ParseLiteral. ParseLiteral already trimmed, so on
			// that path this is a no-op -- it exists because ParseString is self-contained
			// and may be handed raw text.
			std::string trimmed = Trim(text);
			// A quoted string must be at least two characters AND open and close with
			// the same quote character.
			// DEFECT (edge case): a one-character string such as '"' fails the size test
			// and is treated as a bare token, which the path below emits unquoted --
			// producing the three-character sequence """ in the output, which is not a
			// valid Python string.
			if (trimmed.size() >= 2 &&
				((trimmed.front() == '"' && trimmed.back() == '"') ||
				 (trimmed.front() == '\'' && trimmed.back() == '\'')))
			{
				// Strips both delimiters in one substr call.
				std::string inner = trimmed.substr(1, trimmed.size() - 2);
				// Escape on the way out so a value containing a quote survives
				// the YAML round trip.
				// Only '"' and '\\' are escaped. DEFECT: control characters (newline, tab,
				// carriage return) inside the value are NOT escaped, despite the stated goal
				// of surviving the YAML round trip -- a value containing a newline will not
				// survive it, and the generated source will contain a raw newline inside a
				// quoted literal.
				std::string escaped;
				for (char c : inner)
				{
					// Escape the delimiter and the escape character itself; anything else passes
					// through unchanged, so a user-typed '\\n' stays two characters.
					if (c == '"' || c == '\\')
						escaped += '\\';
					escaped += c;
				}
				// The delimiters are re-added around the escaped body. Re-escaping on the way
				// OUT is what keeps an already-escaped value stable across repeated
				// load/save cycles instead of growing backslashes every pass.
				return "\"" + escaped + "\"";
			}
			// A bare token is emitted as a quoted string, so a value like `none`
			// cannot be re-read as something else.
			// Bare tokens are quoted so a value like 'None' or '3' cannot be re-read as
			// something else (a Python constant, a number) on the next load.
			// DEFECT: this path applies NO escaping at all, so a bare token containing a
			// backslash or a quote is emitted raw and yields an invalid or altered Python
			// string. It also duplicates the escaping logic above -- the kind of
			// duplication that eventually gets fixed in one place only.
			return "\"" + trimmed + "\"";
		}

		// Parses '(a, b, c)' into a Wasteland.VecN(...) call. `expected` is the
		// component count demanded by the pin type; `ok` is an out-parameter because
		// a rejected tuple and a legitimately empty result are otherwise
		// indistinguishable from the return value alone.
		std::string ParseVec(const std::string &text, int expected, bool &ok)
		{
			// Fail-closed default: every early return below leaves ok=false, so callers
			// cannot mistake a rejected tuple for a parsed one.
			ok = false;
			std::string trimmed = Trim(text);
			// Parens are mandatory. A bare '1, 2, 3' is rejected rather than guessed at,
			// because the surrounding literal field may legitimately contain commas.
			if (trimmed.size() < 2 || trimmed.front() != '(' || trimmed.back() != ')')
				return std::string();

			std::string inner = trimmed.substr(1, trimmed.size() - 2);
			// Accumulator for the components. std::vector<float> rather than a fixed
			// array because the count is only known to be 2, 3 or 4 at the call site and
			// still has to be range-checked.
			std::vector<float> parts;
			std::stringstream stream(inner);
			std::string token;
			// Split on commas via the stream, so the tuple syntax matches Python's own
			// and no manual index arithmetic is needed.
			while (std::getline(stream, token, ','))
			{
				float value = 0.0f;
				// Each token is trimmed before parsing, so '(1, 2, 3)' with spaces works.
				// An empty token -- a trailing or doubled comma in '(1,,2)' -- fails here,
				// which is the desired behaviour.
				if (!ParseFloat(Trim(token), value))
					return std::string();
				// Parsed strictly (see ParseFloat), so 'Vec3(1, a, 3)' is an error.
				parts.push_back(value);
			}
			// The component count is a hard requirement, not a minimum: a Vec3 pin fed
			// '(1, 2)' is an error rather than a zero-filled vector, so the author finds
			// out at compile time instead of getting a silently wrong direction.
			if ((int)parts.size() != expected)
				return std::string();

			// The emitted call hard-codes the module name 'Wasteland'. That is a coupling
			// to the generated script's import list: if the runtime module is renamed,
			// this string has to change with it.
			std::string result = "Wasteland.Vec" + std::to_string(expected) + "(";
			for (size_t i = 0; i < parts.size(); ++i)
			{
				if (i > 0)
					result += ", ";
				// std::to_string(float) is %f with six decimals, so 0.1 becomes '0.100000'.
				// Correct for Python, but fixed-point: precision beyond six decimals is lost,
				// and very large magnitudes render as long digit strings.
				result += std::to_string(parts[i]);
			}
			result += ")";
			ok = true;
			return result;
		}
	}

	// The per-type entry point behind LiteralToPython. Turns authored text into
	// the exact Python expression the emitter splices into generated code.
	// Returns false with `error` set for a malformed value -- never throws, and
	// never substitutes a zero.
	bool ParseLiteral(PinType type, const std::string &literal, std::string &outValue,
					  std::string &error)
	{
		std::string trimmed = Trim(literal);


		// A wildcard pin has no declared type to parse against, so the narrowest
		// round-trippable form is inferred from the text itself.
		// A wildcard pin's literal carries no declared type. Infer the
		// narrowest type that round-trips, checking bool BEFORE number so the
		// strings "True"/"False" do not fall through to a float parse.
		if (type == PinType::Any)
		{
			const std::string t = trimmed;
			// DEFECT (asymmetry): only the Python-capitalised 'True'/'False' are
			// recognised. ParseBool below also accepts 'true'/'false'/'1'/'0'/'yes'/'no', so
			// a wildcard pin holding 'true' falls through both numeric checks and lands
			// as the quoted STRING "true" -- the compiler then accepts a comparison
			// against a str.
			if (t == "True" || t == "False")
			{
				outValue = t;
				return true;
			}
			// All-digits test. An EMPTY string also passes: find_first_not_of on an empty
			// string returns npos, so an empty wildcard literal is emitted as '.0' below,
			// which is not a valid Python expression. DEFECT.
			if (t.find_first_not_of("0123456789") == std::string::npos)
			{
				outValue = t + ".0";
				return true;
			}
			// Broader numeric set: sign and decimal point, but not an exponent ('e'), so
			// '1e5' is NOT recognised as a number here and falls through to the string
			// path. Consistent with the fixed-point intent, but surprising.
			if (t.find_first_not_of("-+.0123456789") == std::string::npos)
			{
				outValue = t;
				return true;
			}
			// Fallback: quote it. Always returns true -- a wildcard pin accepts anything,
			// so there is no error path here at all.
			outValue = ParseString(t);
			return true;
		}

		switch (type)
		{
		// Exec has no value form. Returning false means an exec pin carrying a
		// literal is a reported authoring error, which is what the required-literal
		// validation described in Graph.h expects.
		case PinType::Exec:
			error = "exec is not a value and has no literal";
			return false;

		case PinType::Bool:
		{
			bool value = false;
			if (!ParseBool(trimmed, value))
			{
				error = "'" + trimmed + "' is not a bool (expected true/false/1/0/yes/no)";
				return false;
			}
			// Emitted in Python's capitalised spelling so the generated code reads as a
			// bool. Matches the Any-pin inference above, so the two paths agree.
			outValue = value ? "True" : "False";
			return true;
		}

		case PinType::Int:
		{
			int value = 0;
			if (!ParseInt(trimmed, value))
			{
				error = "'" + trimmed + "' is not an int";
				return false;
			}
			// Re-formatted through std::to_string, which normalises '+3' and '03' to '3'.
			// Canonicalising here is what makes repeated round-trips converge.
			outValue = std::to_string(value);
			return true;
		}

		case PinType::Float:
		{
			float value = 0.0f;
			if (!ParseFloat(trimmed, value))
			{
				error = "'" + trimmed + "' is not a float";
				return false;
			}
			// Always emit a decimal point. `3` where a float is expected reads
			// as an int and truncates in a Vec3 constructor.
			outValue = std::to_string(value);
			if (outValue.find('.') == std::string::npos &&
				outValue.find('e') == std::string::npos &&
				outValue.find("inf") == std::string::npos &&
				outValue.find("nan") == std::string::npos)
			{
				outValue += ".0";
			}
			return true;
		}

		// Strings go through ParseString, which returns true unconditionally -- there
		// is no malformed string literal, only different spellings of one.
		case PinType::String:
			outValue = ParseString(trimmed);
			return true;

		case PinType::Vec2:
		case PinType::Vec3:
		case PinType::Vec4:
		{
			// The three Vec cases share one body, so the component count is derived here
			// from the type rather than repeated per case.
			int expected = (type == PinType::Vec2) ? 2 : (type == PinType::Vec3 ? 3 : 4);
			bool ok = false;
			// ParseVec writes the value and reports success via `ok`; what it returns is
			// meaningless when ok is false.
			std::string value = ParseVec(trimmed, expected, ok);
			if (!ok)
			{
				// The error text names the pin type via PinTypeName and states the expected
				// shape, because these strings are the user interface of the validator.
				error = "'" + trimmed + "' is not a " + PinTypeName(type) +
						" (expected a comma-separated tuple of " +
						std::to_string(expected) + " numbers)";
				return false;
			}
			outValue = value;
			return true;
		}

		// An entity pin holds either an empty reference or an identifier. It is kept
		// as text like every other literal rather than a resolved handle, so a save
		// stays valid across a session in which the handle differs.
		case PinType::Entity:
		{
			// Three spellings of 'no entity' are accepted, so 'null' from a JSON-ish
			// default and 'None' from Python both land on the same canonical output.
			if (trimmed.empty() || trimmed == "null" || trimmed == "None")
			{
				outValue = "None";
				return true;
			}
			// A named entity is emitted as a QUOTED STRING, not a bare Python identifier.
			// The emitter is therefore expected to resolve the name at runtime; if it does
			// not, a named entity silently becomes a string.
			outValue = ParseString(trimmed);
			return true;
		}
		}

		error = "unknown literal type";
		return false;
	}

	// Convenience wrapper for callers that only want the value or a bool.
	std::string LiteralToPython(PinType type, const std::string &literal, bool &ok)
	{
		// DEFECT: `error` is filled by ParseLiteral and then discarded -- the caller
		// gets only 'ok == false', with no reason. Every other diagnostic in this
		// module is built to reach the user (see the error strings above), so this
		// wrapper drops the one message that would explain the failure.
		std::string error;
		std::string value;
		// Delegates entirely; there is no second parsing path, so the two entry
		// points cannot disagree about what a given literal means.
		ok = ParseLiteral(type, literal, value, error);
		if (!ok)
			return std::string();
		return value;
	}

}
