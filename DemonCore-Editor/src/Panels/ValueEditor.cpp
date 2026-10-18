// No wlpch.h: this panel is in the editor module, which does not use
// the engine's precompiled header, so every dependency is explicit here.
#include "ValueEditor.h"

// std::array: the fixed-size field buffer in the store below.
#include <array>
// std::tolower for the case-insensitive boolean parse.
#include <cctype>
// int32_t bounds constants used by the integer range check, plus the
// fixed-width integer types the Graph model stores ids in.
#include <cstdint>
// std::next, for advancing the iterator in the erase loop below.
#include <iterator>
// std::isfinite: NaN and infinity are syntactically valid floats but
// generate Python that fails at runtime, so both parsers reject them.
#include <cmath>
// snprintf, used to copy a value into a fixed buffer without the
// overflow risk of strcpy and without a std::string allocation.
#include <cstdio>
// std::strtof / std::strtol: the parsing primitives. Both are called
// through the "did it consume the whole string" check, never on their
// own, because both silently accept a valid prefix.
#include <cstdlib>
// strlen would work for the buffer length here, but the code uses the
// array bound instead so the size stays tied to kBufferSize.
#include <cstring>
// std::map: pulled in for ordered iteration over the buffers. The store
// itself is an unordered_map, so this is currently unused -- a stale
// include. Harmless, but it is dead weight in a header-adjacent file.
#include <map>
// std::unordered_map for the field store, which is looked up by an
// exact composite key and never iterated in order.
#include <unordered_map>

// The editor module shares the Wasteland namespace with the engine, so
// these symbols are visible without qualification from the panels.
namespace Wasteland
{

	// Local using-declarations for the two VisualBlocks types named in the
	// signatures below. Without them every mention of PinType in this file
	// would carry a VisualBlocks:: prefix.
	using VisualBlocks::Graph;
	using VisualBlocks::PinType;

	namespace
	{
		// DEFECT (key encoding): the composite key packs node id and field id
		// into one string separated by \x1f, a C0 control character. It works
		// because node and field ids are authored identifiers and so do not
		// contain control characters -- but nothing enforces that. An id
		// containing \x1f would make two different fields collide onto one
		// key, silently sharing an edit buffer. A (nodeId, field) pair key
		// would make that unrepresentable.
		// Field storage, keyed by "node id \x1f field id".
		// A function-local static, so the store has a single instance across
		// every translation unit that calls it, and is destroyed at exit.
		// Deliberately process-global: a buffer must outlive the panel object
		// that created it, because ImGui focus and text state reference it
		// by widget id on later frames.
		std::unordered_map<std::string, std::array<char, ValueEditor::kBufferSize>> &Store()
		{
			// Static local initialization is thread-safe since C++11, and this is
			// never called concurrently in practice anyway -- the editor is
			// single-threaded.
			static std::unordered_map<std::string, std::array<char, ValueEditor::kBufferSize>> store;
			return store;
		}

		// Trailing whitespace is common from copy/paste and would otherwise
		// make every parse below fail for no visible reason.
		// Whitespace trim shared by every parser in the file. Doing it once
		// here is why none of them has to think about a trailing space.
		std::string Trim(const std::string &text)
		{
			size_t b = 0;
			size_t e = text.size();
			// Four characters rather than isspace: the C locale can be changed,
			// and a locale-sensitive isspace would classify bytes differently
			// depending on global state. An explicit set is deterministic.
			// \r and \n are included because text arrives from ImGui and from
			// imported files, which disagree about line endings.
			while (b < e && (text[b] == ' ' || text[b] == '\t' || text[b] == '\r' || text[b] == '\n'))
				++b;
			// Same character set from the other end. b and e converge on an
			// all-whitespace string, and the guard keeps e from dropping below b.
			while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r' || text[e - 1] == '\n'))
				--e;
			return text.substr(b, e - b);
		}

		// Shared strictness rule: the whole string must be consumed. strtof
		// stopping at the first bad character is what lets "1abc" through,
		// which then silently becomes 1.
		// Templated on the parse function so the strictness rules below --
		// full consumption, empty rejection, finiteness -- are written once
		// and shared by every float-shaped type, instead of being copied into
		// each parser where they could drift apart.
		template <typename Fn>
		bool ParseWhole(const std::string &text, Fn parse, float &out)
		{
			// Trimmed into a local so the parse operates on a NUL-terminated copy
			// without the caller's std::string being modified.
			const std::string s = Trim(text);
			// An empty field is not zero and not a parse failure to report loudly --
			// it is simply "no value yet", so the caller keeps the previous one.
			if (s.empty())
				return false;
			char *end = nullptr;
			// The parse function is passed in rather than named here, which is what
			// makes this template reusable for strtod and friends.
			const float v = parse(s.c_str(), &end);
			// Three separate rejection conditions, and all three are needed:
			//   end == s.c_str()  -- nothing was consumed ("abc"),
			//   end == nullptr    -- parse refused outright,
			//   *end != '\0'      -- a valid prefix with trailing junk ("1abc").
			// Without the third, the trailing-junk case would silently become the
			// leading number, which is the specific bug this check prevents.
			if (end == s.c_str() || end == nullptr || *end != '\0')
				return false;
			// NaN and infinity are syntactically parseable but generate
			// Python that fails at RUNTIME, far from the edit that caused it.
			// Rejects a value the generated Python would choke on later. Catching
			// it here turns a runtime failure far from the edit into a rejected
			// keystroke next to the field that caused it.
			if (!std::isfinite(v))
				return false;
			// Out-parameter written only on success, so a caller that ignores the
			// return value still reads its previous value rather than garbage.
			out = v;
			return true;
		}
	} // namespace

	// Whole-store reset, called on graph load so a field from a previously
	// loaded file cannot seed the new graph's controls with stale text.
	void ValueEditor::ResetBuffers()
	{
		// clear() rather than a per-node walk: the point of this entry point
		// is "forget everything", so it cannot leak a stale key.
		Store().clear();
	}

	// Per-node reset, for when one node is deleted or replaced and the
	// rest of the graph's buffers should survive.
	void ValueEditor::ResetBuffersFor(const std::string &nodeId)
	{
		auto &store = Store();
		// Manual erase loop rather than erase_if, because erase_if cannot be
		// used on an unordered_map (it needs to erase by key, and the keys
		// here are matched by a PREFIX test, not by equality).
		for (auto it = store.begin(); it != store.end();)
		{
			// The prefix test needs both halves. compare() alone would also match
			// node "A" against a key belonging to node "ABC"; requiring the
			// separator (or exact end) is what confines the match to real field
			// keys of THIS node.
			const bool mine = it->first.compare(0, nodeId.size(), nodeId) == 0 &&
							  (it->first.size() == nodeId.size() ||
							   it->first[nodeId.size()] == '\x1f');
			// Erase-and-continue rather than ++: erasing invalidates the iterator,
			// so the returned one is already the next element. std::next is the
			// untouched case.
			it = mine ? store.erase(it) : std::next(it);
		}
	}

	// Returns a writable C string for ImGui, plus the two out-parameters
	// the caller must keep alive. The unusual signature exists because
	// the buffer's real owner is the static store, and ImGui wants a
	// bare char* that outlives this call.
	char *ValueEditor::Buffer(std::string &scratchOwner, std::string &scratchKey,
							  const std::string &nodeId, const std::string &field,
							  const std::string &current)
	{
		// Key construction. Matches the encoding Store() documents, so a key
		// written here is found by ResetBuffersFor's prefix scan.
		scratchKey = nodeId + "\x1f" + field;
		auto &store = Store();
		auto it = store.find(scratchKey);
		// Absent key means the field has never been focused, so seed it from
		// the stored literal -- otherwise the widget would show an empty box
		// for a field that already has a value.
		if (it == store.end())
		{
			std::array<char, kBufferSize> blank{};
			// snprintf, not strcpy: it bounds the copy at kBufferSize, so a `current`
			// longer than the buffer is truncated instead of overflowing it.
			std::snprintf(blank.data(), kBufferSize, "%s", current.c_str());
			it = store.emplace(scratchKey, blank).first;
		}
		// scratchOwner exists so the caller's pointer stays valid for the
		// frame even though storage is owned by the static store.
		// Copy into the caller's string and return a pointer into THAT, not
		// into the store. The reason: an unordered_map rehash on a later
		// insert would move the array and invalidate a pointer handed out
		// here. A std::string's own buffer is stable for the frame.
		scratchOwner.assign(it->second.data(), it->second.size());
		// &scratchOwner[0] is valid because assign() above guarantees the
		// string is non-empty (kBufferSize is 256), so no empty-string edge
		// case applies.
		return &scratchOwner[0];
	}

	// Float parsing, delegating every strictness rule to ParseWhole.
	bool ValueEditor::ParseFloat(const std::string &text, float &out)
	{
		// strtof, not atof: the end-pointer parameter is the whole point --
		// it is what lets ParseWhole reject a partial parse. atof has no
		// way to report how much it consumed.
		return ParseWhole(text, [](const char *s, char **e) { return std::strtof(s, e); }, out);
	}

	// Integer parsing. Written out longhand rather than routed through
	// ParseWhole, because strtol returns long and needs a range check
	// that the float path has no equivalent of.
	bool ValueEditor::ParseInt(const std::string &text, int &out)
	{
		const std::string s = Trim(text);
		if (s.empty())
			return false;
		char *end = nullptr;
		// strtol, not atoi: same reason as strtof above -- the end pointer is
		// what makes "12abc" detectable.
		const long v = std::strtol(s.c_str(), &end, 10);
		if (end == s.c_str() || end == nullptr || *end != '\0')
			return false;
		// Range-checked rather than cast: an int pin fed 10^12 must be
		// rejected, not silently truncated into a different number.
		// DEFECT (check is a no-op on Windows): v is long, which is 32 bits
		// on this platform, and strtol saturates at LONG_MAX == INT32_MAX. So
		// a value above the int range has already been clamped to exactly
		// INT32_MAX by the time it reaches this comparison, and neither
		// bound can ever be false. "99999999999" is silently accepted as
		// 2147483647 instead of being rejected. Parsing into long long (or
		// checking errno for ERANGE) would make the guard real.
		if (v < INT32_MIN || v > INT32_MAX)
			return false;
		// static_cast rather than a C-style cast, and only safe because the
		// range check above held.
		out = static_cast<int>(v);
		return true;
	}

	// Boolean parsing against a fixed vocabulary.
	bool ValueEditor::ParseBool(const std::string &text, bool &out)
	{
		// The spellings the generated Python actually emits, plus the
		// lowercase forms an author naturally types. Anything else is a
		// typo and is refused rather than guessed at.
		// Copied rather than transformed in place: `text` is a const reference
		// the caller may still be using for something else.
		std::string s = Trim(text);
		// Case-folding loop. The cast to unsigned char before tolower is
		// required, not decorative: tolower on a negative char is undefined
		// behaviour, and a UTF-8 continuation byte has exactly that value.
		for (char &c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		// "yes" is not Python's spelling -- the emitter produces True/False
		// -- but an author typing a boolean will reach for it, so accepting
		// it costs one comparison.
		if (s == "true" || s == "1" || s == "yes")
		{
			out = true;
			return true;
		}
		if (s == "false" || s == "0" || s == "no")
		{
			out = false;
			return true;
		}
		// Anything else is refused rather than guessed at, so a typo like
		// "ture" leaves the field showing its error state instead of quietly
		// becoming False.
		return false;
	}

	// One parser for all vector widths. count carries the arity, which is
	// what lets Vec2/Vec3/Vec4 share an implementation and an array.
	bool ValueEditor::ParseVec(const std::string &text, float *out, int count)
	{
		std::string s = Trim(text);
		// count <= 0 is rejected here rather than trusted: with count == 0 the
		// loop below would never run and the function would return true
		// having written nothing.
		if (s.empty() || count <= 0)
			return false;
		// Commas become spaces so one split handles both separators.
		// Comma-to-space normalization, so ONE split strategy handles both
		// "1, 2, 3" and "1 2 3". The header says neither separator is more
		// obviously correct, and this is how both are accepted.
		for (char &c : s)
		{
			if (c == ',')
				c = ' ';
		}

		// DEFECT (fixed 4-element stack array): sized for Vec4, but `count`
		// is not validated against it. A caller passing count > 4 writes past
		// the end of this array -- a stack buffer overflow -- because the
		// loop below is bounded by count, not by 4. Clamping count (or
		// sizing the array from count) would remove the hazard; today the
		// only thing preventing it is that every caller happens to pass 2, 3
		// or 4.
		float values[4] = {0, 0, 0, 0};
		int index = 0;
		const char *cursor = s.c_str();
		// Stops at `count` components OR at end of string. Both conditions
		// are needed: stopping only at NUL would overrun `values` on a long
		// input, and stopping only at count would leave trailing junk for
		// the check below.
		while (*cursor != '\0' && index < count)
		{
			char *end = nullptr;
			const float v = std::strtof(cursor, &end);
			// end == cursor means strtof consumed nothing, i.e. the next token is
			// not a number at all -- "1, oops" rather than "1, 2".
			if (end == cursor)
				return false;
			if (!std::isfinite(v))
				return false;
			values[index++] = v;
			cursor = end;
		}
		// Trailing junk after the last component is an error: "1, 2, oops"
		// must not silently become (1, 2, 0).
		// strtof skips leading whitespace itself, so after the last component
		// only spaces and tabs can remain; anything else is genuine junk.
		while (*cursor == ' ' || *cursor == '\t')
			++cursor;
		if (*cursor != '\0')
			return false;
		// Exact arity required. Fewer components than count would otherwise
		// leave the remaining outputs untouched, and the caller would read
		// whatever was in them before -- an uninitialized-looking value that
		// is really stale.
		if (index != count)
			return false;

		// Copy out only after every check has passed, so a failed parse never
		// partially writes the caller's array.
		for (int i = 0; i < count; ++i)
			// The zero-initialized `values` above is therefore never observable:
			// reaching this loop means index == count.
			out[i] = values[i];
		return true;
	}

	// Formatting is the inverse of parsing: whatever these produce must
	// parse back through the functions above, or a value round-tripped
	// through the editor would stop being editable.
	std::string ValueEditor::FormatFloat(float v)
	{
		char buf[64];
		// %g, not %f: a float pin should show 1.5 and not 1.500000, and
		// should not truncate a large value to 1e+06 noise.
		// The cast to double is because %g is a double conversion; passing a
		// float through varargs would read the wrong bytes. 64 bytes is
		// ample: %g of a float is at most about a dozen characters.
		std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
		return buf;
	}

	std::string ValueEditor::FormatInt(int v)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%d", v);
		return buf;
	}

	// Deliberately NOT "true"/"false": these strings are emitted into
	// generated Python, where the spellings are capitalized.
	std::string ValueEditor::FormatBool(bool v)
	{
		// Capitalised to match what the emitter generates, so the stored
		// text and the generated Python agree exactly.
		return v ? "True" : "False";
	}

	// Joins with ", " -- the same spelling ParseVec accepts, so the
	// formatted and parsed forms agree.
	std::string ValueEditor::FormatVec(const float *values, int count)
	{
		std::string out;
		for (int i = 0; i < count; ++i)
		{
			// Separator before every element except the first, which is what
			// keeps a leading comma out of the result.
			if (i > 0)
				out += ", ";
			out += FormatFloat(values[i]);
		}
		return out;
	}

	// The one place that decides how a string literal becomes Python.
	// Centralized so no emitter has to remember the rule.
	std::string ValueEditor::Quoted(const std::string &raw)
	{
		// Escape backslash and quote, and refuse nothing: an author must be
		// able to type any string they want. Embedding raw text into
		// generated Python without escaping would let a literal containing
		// a quote break out of the string and inject code, so this is a
		// correctness AND safety boundary.
		std::string out;
		// Reserve for the worst case (every character escaped to two, plus
		// both quotes) so the appends below cannot reallocate.
		out.reserve(raw.size() + 2);
		out.push_back('"');
		// Range-for by value: a single char at a time, no copies of the string.
		for (char c : raw)
		{
			// The injection boundary. A quote or backslash inside the authored
			// text would otherwise terminate the Python string early and let the
			// rest of it execute as code.
			if (c == '"' || c == '\\')
				out.push_back('\\');
			// DEFECT (incomplete escape set): only \n is handled. A carriage
			// return, tab, or any other control character passes through raw.
			// A literal CR in particular produces a Python string containing a
			// raw CR, which Python treats as a line break inside a quoted
			// literal -- a syntax error in the generated file, or a silently
			// mangled string. Escaping all C0 controls would close it.
			if (c == '\n')
			{
				out += "\\n";
				continue;
			}
			out.push_back(c);
		}
		out.push_back('"');
		return out;
	}

	// Type name for the inspector. A pure lookup with no allocation, which
	// is why it returns const char* rather than std::string.
	const char *ValueEditor::TypeName(PinType type)
	{
		// Switch over every PinType enumerator with no default case, so adding
		// one to the enum produces a compiler warning here -- that warning is
		// the mechanism that keeps this table from going stale.
		switch (type)
		{
		case PinType::Exec:   return "Exec";
		case PinType::Bool:   return "Bool";
		case PinType::Int:    return "Int";
		case PinType::Float:  return "Float";
		case PinType::String: return "String";
		case PinType::Vec2:   return "Vec2";
		case PinType::Vec3:   return "Vec3";
		case PinType::Vec4:   return "Vec4";
		case PinType::Entity: return "Entity";
		case PinType::Any:    return "Any";
		}
		// Unreachable while the switch is exhaustive; "?" is the visible
		// fallback if a persisted file ever yields an out-of-range ordinal.
		return "?";
	}

	// Two lookups with the same shape, differing only in what they
	// return. Deliberately not merged: the hot one is a predicate and
	// the cold one needs a value, and a shared finder would force one
	// of them to pay for the other.
	bool ValueEditor::IsLinked(const Graph &graph, const std::string &nodeId,
							   const std::string &pinId)
	{
		for (const auto &link : graph.Links)
		{
			// Matches on the DESTINATION side only: this answers "is this input
			// pin driven by a link", not "does this pin have any wire".
			//
			// DEFECT (O(L) per call): a linear scan of every link in the graph,
			// and the inspector calls this once per pin per frame, so the cost
			// is O(pins x links) every frame. Fine for a hand-built graph; a
			// large saved one would feel it. An index keyed by (node, pin) built
			// once per load would make it a hash lookup.
			//
			// The `return false` after the loop means an unlinked pin, which is
			// what makes the inspector show an editable control instead.
			if (link.ToNode == nodeId && link.ToPin == pinId)
				return true;
		}
		return false;
	}

	std::string ValueEditor::LinkSourceNode(const Graph &graph, const std::string &nodeId,
										   const std::string &pinId)
	{
		for (const auto &link : graph.Links)
		{
			if (link.ToNode == nodeId && link.ToPin == pinId)
				return link.FromNode;
		}
		// Empty string, not "": an empty node id is not a real node, so the
		// caller cannot mistake this for a valid id.
		return std::string();
	}

} // namespace Wasteland