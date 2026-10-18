// wlpch.h first: the editor's forced-include prefix. It supplies <string>,
// <vector> and <iostream>, so this file's own includes below look sparser
// than they are.
#include "wlpch.h"
// The header it implements. JsonMini.h is the whole public surface -- every
// other helper in this file is file-local and lives in the anonymous
// namespace below.
#include "JsonMini.h"

// <cctype> for the isdigit/isspace predicates. std::stoll and std::stod come
// from <string>, which wlpch.h already provided -- so this include list is
// correct but not self-contained.
#include <cctype>

// A deliberately tiny, hand-rolled JSON reader. The comment above the
// namespace says what it is for; the design constraint visible throughout is
// that it must work on a response body that arrived as one string, with no
// DOM and no allocation per node.
namespace Wasteland::JsonMini
{

	// Escape a string for EMBEDDING in a JSON document (an outgoing request).
	// It does not add the surrounding quotes -- callers do that themselves, which
	// is how AIChatPanel.cpp builds its request bodies by hand.
	std::string Escape(const std::string &s)
	{
		// Single allocation for the common case. Reserve is a hint, not a cap: a
		// string full of control characters expands 6x here and reallocates.
		std::string out;
		// reserve avoids repeated reallocation for the overwhelmingly common case
		// (a string with nothing to escape). It is a lower bound, not a cap.
		out.reserve(s.size() + 8);
		// Lowercase hex digit table for the \u00XX escape path below. A raw string
		// literal rather than a char array so no terminator can be read as a digit.
		static const char *hex = "0123456789abcdef";
		// Iteration is over unsigned char, which is what makes the c < 0x20 control
		// check meaningful: a signed char would make every byte >= 0x80 look
		// negative and get escaped as a bogus \u00xx.
		for (unsigned char c : s)
		{
			// The seven escapes JSON requires. Note '/' is NOT escaped, which is legal
			// and cheaper; only the two mandatory cases are handled here plus the
			// control-character fallback.
			switch (c)
			{
			// The two escapes that are structurally required: without them the value
			// cannot be delimited or embedded at all.
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			// \n and \r are separate cases rather than one branch, because they must
			// stay distinct -- collapsing them would silently rewrite line endings.
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			// \b and \f are the two rarer escapes. Accepted because the producers this
			// parser reads (model registries, chat APIs) do emit them.
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			// Every byte without a short escape lands here.
			default:
				// Everything below 0x20 has no short escape (or none that is safe to embed
				// in a line-oriented stream), so it becomes a \u00XX sequence.
				if (c < 0x20)
				{
					// Two hex digits suffice because a control character is always < 0x100. The
					// fixed-width \u00 prefix is why this is not the same as the \uXXXX reader
					// in ParseString below.
					out += "\\u00";
					// High nibble then low. c < 0x20 means c >> 4 is 0 or 1, so only the first
					// two entries of the table are ever reached -- the full 16-entry table is
					// still the right thing to keep.
					out += hex[c >> 4];
					// Low nibble completes the four hex characters emitted by the three lines
					// above.
					out += hex[c & 0xF];
				}
				else
					// Everything else -- printable ASCII and any byte >= 0x80 -- passes through
					// unchanged. UTF-8 continuation bytes land here, so multi-byte text is
					// copied verbatim and needs no special handling.
					out += (char)c;
			}
		}
		// Return by value: NRVO applies, so this is a move, not a copy.
		return out;
	}

	// Everything from here to the closing brace of this namespace is
	// file-local: these helpers have no external contract and no test surface,
	// so internal linkage is deliberate.
	namespace
	{
		// This is the primitive every Extract* is built on, and it carries the whole
		// module's central weakness, documented at its own loop below.
		// Locate `"key"` followed by optional whitespace and ':'.
		// Returns position just past ':', or npos.
		size_t FindKeyValue(const std::string &json, const std::string &key, size_t from = 0)
		{
			// The search key is quoted, so the scan cannot match the bare word inside a
			// string VALUE that merely contains it... except that it absolutely can:
			// see the note at the loop.
			std::string quoted = "\"" + key + "\"";
			// `from` defaults to 0, so a caller wanting only the first match need not
			// pass anything. No caller in this file uses the parameter -- see the note
			// on the loop below.
			size_t pos = from;
			// Scans forward for the quoted key, resuming from `pos` so a rejected
			// candidate does not restart the search.
			while ((pos = json.find(quoted, pos)) != std::string::npos)
			{
				// The index just past the closing quote of the candidate key.
				size_t after = pos + quoted.size();
				// Skip JSON whitespace between the closing quote and the colon. Only the
				// four JSON whitespace characters; a form feed or vertical tab would stop
				// the skip and fail the match.
				while (after < json.size() && (json[after] == ' ' || json[after] == '\t' || json[after] == '\n' || json[after] == '\r'))
					// Resume scanning AFTER the whitespace run rather than at the candidate
					// itself -- otherwise the same quoted text could be found again and the
					// loop would not terminate.
					after++;
				// The quote must actually be followed by ':' -- this is what distinguishes
				// a KEY occurrence from the same text appearing as a string value.
				// DEFECT: it does not distinguish a key at the TOP level from one nested
				// inside an object or an array. FindKeyValue is a flat text scan, so
				// {"a":{"x":1},"x":2} with key 'x' returns the INNER value. Every
				// Extract* inherits this, so a caller extracting a key that also appears in
				// a nested object gets the nested one.
				if (after < json.size() && json[after] == ':')
					// Returned value is one PAST the colon, which is why every caller follows
					// it with SkipWs. The distinction is the whole reason this returns early
					// rather than at the colon.
					return after + 1;
				// Resume just past the rejected match rather than at pos+1, so the search
				// cannot spin on the same position.
				pos = after;
			}
			// npos when the key is absent anywhere in the document. Callers compare
			// against npos directly.
			return std::string::npos;
		}

		// Shared whitespace skipper, extracted because all five public Extract* need
		// it and duplicating the four-character test five times is how one of them
		// ends up disagreeing with the others.
		size_t SkipWs(const std::string &s, size_t p)
		{
			// The four JSON whitespace characters only. A form feed or vertical tab
			// (legal in some producers' output, illegal in JSON) would stop the skip
			// and cause a spurious 'key not found'.
			while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
				p++;
			// Idempotent: calling SkipWs on an already-trimmed position returns it
			// unchanged.
			return p;
		}

		// Reads exactly four hex digits. The size check is 'p + 4 > size', so a
		// \u escape at the very end of a string with exactly four digits left is
		// accepted.
		bool ParseHex4(const std::string &s, size_t p, uint32_t &out)
		{
			if (p + 4 > s.size())
				return false;
			// Accumulator. uint32_t because four hex digits cannot exceed 0xFFFF, so the
			// high half is always zero and the narrowing in AppendUtf8 is safe.
			uint32_t v = 0;
			// Four digits, fixed count -- a \u escape is exactly four hex digits, so
			// this is not a 'read while hex' loop and cannot over-consume.
			for (int i = 0; i < 4; i++)
			{
				char c = s[p + i];
				// Shift-then-or accumulates in base 16 with no multiply. Done before the
				// digit is inspected so the first digit is not shifted out of the word.
				v <<= 4;
				// Both hex cases are handled explicitly rather than via isxdigit, so a
				// non-hex character is a hard failure and not a silently wrong value.
				if (c >= '0' && c <= '9')
					// '0'-'9' maps to 0-9 by subtracting the digit's own base.
					v |= (uint32_t)(c - '0');
				else if (c >= 'a' && c <= 'f')
					v |= (uint32_t)(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F')
					// Upper case maps identically to lower case -- a third copy of the same
					// 'subtract and add the base' shape, which is why the two branches cannot
					// disagree.
					v |= (uint32_t)(c - 'A' + 10);
				else
					// Any other byte -- including a letter or a space -- rejects the whole
					// escape. Strictness here is what keeps a malformed \uXXXX from decoding to
					// garbage that then propagates into UI text.
					return false;
			}
			// Written only on success, so a rejected escape leaves the caller's out
			// parameter untouched.
			out = v;
			return true;
		}

		// Encodes a code point as UTF-8 by length. Hand-rolled rather than via
		// codecvt or a library because the target is a UTF-8 buffer on all
		// supported platforms and the four cases are fixed.
		// DEFECT: no rejection of surrogates. A lone \uD800 from malformed input is
		// encoded as a 3-byte sequence in the D800..DFFF range, which is not valid
		// UTF-8 and will render as a replacement character (or worse) downstream.
		void AppendUtf8(std::string &out, uint32_t cp)
		{
			// 1-byte form, ASCII. Note cp < 0x80 rather than cp < 0x7F: 0x7F (DEL) is
			// technically legal unescaped in JSON and passes through.
			if (cp < 0x80)
				// ASCII: a single byte, and the only case where cp is used directly rather
				// than through a bit pattern.
				out += (char)cp;
			// 2-byte form covers U+0080..U+07FF.
			else if (cp < 0x800)
			{
				// 2-byte lead: 110xxxxx. The fixed high bits distinguish this length from the
				// others, which is how a decoder knows how many continuation bytes follow.
				out += (char)(0xC0 | (cp >> 6));
				// 6 data bits, low end. Every continuation byte repeats the 10xxxxxx
				// pattern with the next 6 bits of the code point.
				out += (char)(0x80 | (cp & 0x3F));
			}
			// 3-byte form covers the BMP including the surrogate range -- which is why
			// the invalid case noted above can be reached from here.
			else if (cp < 0x10000)
			{
				// 3-byte lead: 1110xxxx. This is the branch that also admits the surrogate
				// range, which is the invalid case flagged above.
				out += (char)(0xE0 | (cp >> 12));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			}
			// 4-byte form. There is no >U+10FFFF check, but ParseString can never
			// produce such a value from a single \uXXXX escape, so it is unreachable
			// from the only caller.
			else
			{
				// 4-byte lead: 11110xxx. Only reachable from a merged surrogate pair, since
				// a single \uXXXX escape cannot exceed 0xFFFF.
				out += (char)(0xF0 | (cp >> 18));
				out += (char)(0x80 | ((cp >> 12) & 0x3F));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			}
		}

		// The string scanner. Returns the decoded value and a past-end position, so
		// a caller wanting successive fields can continue from where this stopped.
		// Parse a JSON string starting at s[p] == '"'. On success sets out and past-end pos.
		bool ParseString(const std::string &s, size_t p, std::string &out, size_t &endPos)
		{
			// The opening quote is the caller's precondition, verified rather than
			// assumed -- ExtractRawToken checks it before calling.
			if (p >= s.size() || s[p] != '"')
				return false;
			// Step onto the character after the backslash. The trailing p++ below then
			// lands on the character after the whole escape.
			p++;
			// Accumulated into a local and only assigned to `out` on success. So a failed
			// parse leaves the caller's out-parameter untouched instead of half-filled.
			std::string result;
			while (p < s.size())
			{
				// One character at a time; the loop exits only on a closing quote or on
				// end-of-input.
				char c = s[p];
				// Unescaped closing quote: success. endPos points one past it, which is what
				// makes the returned position directly usable as the next scan origin.
				if (c == '"')
				{
					// Assigned only on the success path (see the note above), so a partially
					// decoded string never reaches the caller.
					out = result;
					// One past the closing quote -- the resume position for a subsequent field.
					endPos = p + 1;
					return true;
				}
				// Backslash: every escape sequence is two source characters, so the cursor
				// is advanced past the backslash first and the loop's trailing p++ consumes
				// the second.
				if (c == '\\')
				{
					p++;
					// A trailing backslash at end of input is a hard failure rather than a
					// read past the end.
					if (p >= s.size())
						return false;
					char e = s[p];
					switch (e)
					{
					// The two mandatory escapes plus the four control ones. '/' is accepted
					// (legal but not required) because real-world emitters do emit it.
					case '"': result += '"'; break;
					// '\\' and '/': the former is mandatory (a literal backslash in a value
					// must be escaped), the latter optional but very common in real payloads.
					case '\\': result += '\\'; break;
					case '/': result += '/'; break;
					case 'b': result += '\b'; break;
					case 'f': result += '\f'; break;
					case 'n': result += '\n'; break;
					case 'r': result += '\r'; break;
					case 't': result += '\t'; break;
					// \u is the only escape needing a lookahead -- the digits and possibly a
					// whole surrogate PAIR follow.
					case 'u':
					{
						// The decoded code point. A lone surrogate reaching AppendUtf8 unpaired is
						// the invalid-UTF-8 case documented there.
						uint32_t cp = 0;
						// ParseHex4 starts one past the 'u', and its own bounds check makes a
						// truncated escape a clean false rather than an overread.
						if (!ParseHex4(s, p + 1, cp))
							// A \u with fewer than four hex digits before end-of-input is a hard
							// failure, so a truncated escape cannot decode to a partial value.
							return false;
						// Advance past the four hex digits just read, so p now sits on the LAST
						// high-surrogate digit. The bounds test below and the ParseHex4 offset of
						// p+3 are both relative to that position.
						p += 4;
						// Surrogate pairing. A high surrogate (D800..DBFF) is only meaningful
						// combined with a following low surrogate (DC00..DFFF), so both halves must
						// be present and correctly shaped before they are merged.
						// DEFECT (off-by-one in the bound): the guard is `p + 5 < s.size()` but the
						// low surrogate occupies p+1..p+6, so a \uXXXX\uXX at the very end of the
						// string is rejected as unpaired even though it is complete. The result is
						// an unpaired-surrogate encoding, not a crash.
						if (cp >= 0xD800 && cp <= 0xDBFF && p + 5 < s.size() && s[p + 1] == '\\' && s[p + 2] == 'u')
						{
							// The low half of a surrogate pair, validated to be in DC00..DFFF before
							// being merged.
							uint32_t lo = 0;
							// The low surrogate must also be in DC00..DFFF, otherwise the pair is
							// malformed and the high surrogate is emitted alone.
							if (ParseHex4(s, p + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF)
							{
								// The standard astral-plane formula: 0x10000 + ((high - D800) << 10) +
								// (low - DC00). The <<10 is the 10-bit offset within the supplementary
								// plane.
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
								// Skip both halves of the pair. The +6 is measured from the last digit of the
								// HIGH surrogate: backslash, 'u', and four low digits. The loop's trailing
								// p++ then steps past the last low digit.
								p += 6;
							}
						}
						// The single point where a decoded escape becomes bytes. One call for both
						// the BMP case and the merged astral case -- the merging happened above.
						AppendUtf8(result, cp);
						break;
					}
					// DEFECT: an unrecognised escape falls through to `result += e`, so "\q"
					// silently decodes to 'q' instead of failing. That is lenient enough to turn
					// a malformed producer into quietly wrong text with no signal anywhere.
					default:
						// The unrecognised-escape fallback noted above: the offending character is
						// kept and the backslash dropped.
						result += e;
						break;
					}
					// Single trailing advance, shared by the escape and literal branches.
					p++;
				}
				else
				{
					// The ordinary path: an unescaped character is copied verbatim.
					result += c;
					p++;
				}
			}
			// End of input without a closing quote. Hard failure -- a truncated JSON
			// string must not be reported as a short value.
			return false;
		}
	}

	// Extract a string value by key. Thin: find the key, skip whitespace, parse
	// the string. Returns false when the key is absent or the value is not a
	// quoted string (including null), matching the header's contract.
	bool ExtractString(const std::string &json, const std::string &key, std::string &out)
	{
		// Position just past the ':'. SkipWs below turns it into the first
		// character of the value.
		size_t v = FindKeyValue(json, key);
		// Key absent: not an error, just 'not found'. Every Extract* treats this
		// and a malformed value identically, which is why callers cannot tell the
		// two apart.
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		// `end` is computed but discarded here -- this API extracts one field, so
		// there is nothing to resume from. Unused-output, not a defect.
		size_t end = 0;
		// `end` is discarded because this API extracts a single field; see the note
		// in the first pass.
		return ParseString(json, v, out, end);
	}

	// Extract a signed 64-bit integer. The header says "finds the first key :
	// <number>"; note it does NOT verify the number is integral -- see the scan
	// loop below.
	bool ExtractInt64(const std::string &json, const std::string &key, int64_t &out)
	{
		// Find the key; npos means absent.
		size_t v = FindKeyValue(json, key);
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		// An optional sign is consumed before the digit scan. DEFECT: JSON does not
		// permit a leading '+', so '+5' is accepted here where a strict parser would
		// reject it. Harmless in practice, but it is a real deviation from JSON.
		size_t e = v;
		if (e < json.size() && (json[e] == '-' || json[e] == '+'))
			e++;
		// `start` marks the first digit, so the 'no digits at all' test below is
		// not fooled by a lone sign character.
		size_t start = e;
		// The number scan accepts '.', 'e', 'E', '+' and '-' ANYWHERE in the run,
		// not just in valid positions -- so '1.5e+3' scans whole (good) but so does
		// '1-2-3' (garbage in, garbage out). The std::stoll below is what actually
		// decides; this loop only bounds the substring.
		// NOTE: because of this, ExtractInt64 happily returns 1 for the value 1.5,
		// since stoll stops at the '.'. Silent truncation, not an error.
		while (e < json.size() && (std::isdigit((unsigned char)json[e]) || json[e] == '.' || json[e] == 'e' || json[e] == 'E' || json[e] == '+' || json[e] == '-'))
			e++;
		// No digits at all -> not a number. This is what makes a string or bool
		// value under this key a clean 'not found' rather than a parse error.
		if (e == start)
			return false;
		// std::stoll is the only throwing call in this function, hence the only
		// reason for the try.
		try
		{
			// substr from `v` (not `start`) so the sign is included. stoll THROWS on
			// out-of-range, which the catch below converts to a plain false.
			out = std::stoll(json.substr(v, e - v));
			// Success. Note the returned int64_t is assigned to the caller's out
			// parameter, which may have been pre-seeded with a default.
			return true;
		}
		// catch(...) rather than catch(std::exception&): the intent is only 'this
		// literal is not representable as an int64', and no exception should escape
		// a JSON field extractor called from UI code.
		catch (...)
		{
			return false;
		}
	}

	// Extract a double. Deliberately a near-copy of ExtractInt64 rather than a
	// shared helper -- see the note on the unused local below.
	// The double twin of ExtractInt64. Kept as a separate function rather than
	// sharing a scan helper, which is why the (dead) local below exists.
	bool ExtractDouble(const std::string &json, const std::string &key, double &out)
	{
		// DEFECT: `i` is declared, never assigned, never read, and silenced with
		// the (void)i below. It is a leftover from an earlier version that parsed
		// into an int64 first. Dead code that a reader must nonetheless work out.
		int64_t i = 0;
		// Reuse number scan but parse as double.
		// Same find-then-skip sequence as ExtractInt64.
		size_t v = FindKeyValue(json, key);
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		// Same scan setup as ExtractInt64, duplicated verbatim -- see the note at
		// the top of this function.
		size_t e = v;
		if (e < json.size() && (json[e] == '-' || json[e] == '+'))
			e++;
		size_t start = e;
		while (e < json.size() && (std::isdigit((unsigned char)json[e]) || json[e] == '.' || json[e] == 'e' || json[e] == 'E' || json[e] == '+' || json[e] == '-'))
			e++;
		if (e == start)
			return false;
		// std::stod here instead of stoll. Overflow yields HUGE_VAL and sets errno,
		// which stod reports as out_of_range, so the catch below turns it into a
		// plain false.
		try
		{
			// stod, not stoll: the difference between this function and the one above
			// is the final conversion only. Both share the same overly permissive scan.
			out = std::stod(json.substr(v, e - v));
			// The (void)i that exists solely to suppress the unused-variable warning
			// for the dead local declared at the top of the function. A deletion, not a
			// fix.
			(void)i;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	// Extract a bool. Delegated to ExtractRawToken rather than scanning for
	// 'true'/'false' itself, so a field that is a bool in one response and a
	// string in another is handled by one code path -- the reason this exists.
	bool ExtractBool(const std::string &json, const std::string &key, bool &out)
	{
		std::string tok;
		// Delegating to ExtractRawToken is what lets one caller handle a field that
		// is a JSON bool in one response and a quoted "true" in another.
		if (!ExtractRawToken(json, key, tok))
			return false;
		// Strict equality on the token text. So 'True' or '1' under this key returns
		// false; the leniency lives in the caller's fallback, not here.
		if (tok == "true")
		{
			// Only the exact lowercase spellings are accepted. 'True' or '1' under this
			// key returns false, and the caller's fallback (usually ExtractRawToken or
			// treating it as a string) decides what happens next.
			out = true;
			return true;
		}
		if (tok == "false")
		{
			out = false;
			return true;
		}
		return false;
	}

	// The workhorse behind ExtractBool, and the documented escape hatch for
	// fields that may legitimately arrive as either a JSON string or a bare
	// token (the header's HF "gated" example).
	bool ExtractRawToken(const std::string &json, const std::string &key, std::string &out)
	{
		// Same locate-and-skip preamble as every other extractor.
		size_t v = FindKeyValue(json, key);
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		// `e` starts at the value's first character; the quoted and bare branches
		// below both use it as their origin.
		size_t e = v;
		// Quoted value -> decode it properly, including escapes. This branch is what
		// makes ExtractBool work for "gated": "true" decodes to the token 'true',
		// which the caller's equality test then accepts.
		if (e < json.size() && json[e] == '"')
		{
			std::string s;
			// `end` is discarded: this branch only needs the decoded string.
			size_t end = 0;
			if (!ParseString(json, e, s, end))
				return false;
			// A quoted value is returned DECODED and unquoted. That is the difference
			// from the bare branch below, which returns the raw text.
			out = s;
			return true;
		}
		// Bare value: scan to the nearest structural terminator. DEFECT: the
		// terminator set omits ':', so for a nested object or array value the token
		// runs past the inner terminator -- "x":{"a":1} yields the token {"a":1}
		// including the brace. Callers wanting a scalar should not use this on an
		// object.
		while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != ']' && json[e] != '\n')
			e++;
		// Whitespace trim by walking both ends inward. Done after the scan rather
		// than before it so the scan itself stays a simple 'until delimiter' loop.
		// trim
		size_t s = v;
		// Leading whitespace: walk the start forward.
		while (s < e && std::isspace((unsigned char)json[s]))
			s++;
		// Trailing whitespace: walk the end backward. Both loops stop at the other
		// end, so an all-whitespace token collapses to s == e and is rejected below.
		while (e > s && std::isspace((unsigned char)json[e - 1]))
			e--;
		// Nothing but whitespace between the delimiters -- an empty token, which is
		// not a value. Returning false here is what makes a null value report as
		// absent rather than as the string "null".
		if (e == s)
			return false;
		// The trimmed slice. The caller cannot distinguish an unquoted literal from
		// a decoded string, which is the documented trade-off of this API.
		out = json.substr(s, e - s);
		return true;
	}

	// Split the first JSON array in a document into its object substrings. Used
	// by ModelDownloaderPanel.cpp to walk a HuggingFace-style model listing
	// without a DOM.
	std::vector<std::string> SplitArrayObjects(const std::string &json)
	{
		// Accumulated result. Returned by value -- NRVO makes this a move.
		std::vector<std::string> items;
		// The FIRST '[' anywhere in the document. DEFECT: no depth tracking before
		// this point, so if a key earlier in the document contains a '[' inside a
		// string value, the split starts in the wrong place. The header calls this
		// the 'first top-level JSON array', which is not what the code guarantees.
		size_t arr = json.find('[');
		if (arr == std::string::npos)
			return items;
		// Start just past the '['. The outer loop below is what walks the array.
		size_t p = arr + 1;
		// Outer loop: walk the array looking for the next object to capture. Each
		// successful capture below advances p past it, so this terminates.
		while (p < json.size())
		{
			// Locate the next '{' at the array's own depth, ignoring braces inside
			// strings. Without the inStr tracking a '{' inside a name field would start
			// a capture at the wrong offset.
			// Find next '{' at depth 0 of the array, respecting strings.
			// Two-flag string state: inStr for 'are we inside a quoted run', esc for
			// 'was the previous character a backslash'. Reset per scan below.
			bool inStr = false;
			bool esc = false;
			size_t objStart = std::string::npos;
			while (p < json.size())
			{
				// One character at a time through the region before the next object.
				char c = json[p];
				if (inStr)
				{
					// The escape flag is one-shot: it absorbs exactly the character after the
					// backslash, so "\\\"" does not end the string.
					if (esc)
						esc = false;
					else if (c == '\\')
						esc = true;
					// Closing quote while inside a string: back to normal scanning.
					else if (c == '"')
						inStr = false;
				}
				else
				{
					if (c == '"')
						inStr = true;
					// Only a brace OUTSIDE a string can open an object. This is the line that
					// makes nested-string braces safe.
					else if (c == '{')
					{
						// Record the object's first character and stop scanning -- the balanced
						// consume pass below takes over from here.
						objStart = p;
						break;
					}
					// ']' ends the array: stop and return what has been collected so far. A
					// non-object element (a bare number or string in the array) therefore ends
					// collection early rather than being skipped -- the items vector would be
					// shorter than the array. Defensible for an array-of-objects API, but it is
					// an undocumented truncation.
					else if (c == ']')
						// ']' means the array ended without another object. Returning here is what
						// truncates the result for an array containing a non-object element.
						return items;
				}
				p++;
			}
			if (objStart == std::string::npos)
				return items;
			// Consume balanced object.
			// Second pass: consume a balanced object from objStart. Brace depth is
			// counted so nested objects come back as one substring.
			int depth = 0;
			inStr = false;
			esc = false;
			// The second scanner starts at the object's own opening brace, so `depth`
			// reaches zero exactly at its matching close.
			size_t q = objStart;
			while (q < json.size())
			{
				char c = json[q];
				if (inStr)
				{
					if (esc)
						esc = false;
					else if (c == '\\')
						esc = true;
					else if (c == '"')
						inStr = false;
				}
				else
				{
					if (c == '"')
						inStr = true;
					else if (c == '{')
						// Brace depth counted only outside strings -- the reason this pass repeats
						// the inStr tracking of the first pass rather than sharing it.
						depth++;
					else if (c == '}')
					{
						depth--;
						// Depth returns to zero: the object is complete. Capture INCLUDING the
						// closing brace, so the returned substring is itself valid JSON.
						if (depth == 0)
						{
							// Capture including the closing brace, so each returned substring is itself
							// a complete, valid JSON object that callers can feed straight back into
							// ExtractString/ExtractInt64 (which is exactly what ModelDownloaderPanel
							// does).
							items.push_back(json.substr(objStart, q - objStart + 1));
							// Resume after the captured object. This is what makes the outer loop make
							// progress, and it relies on q having been advanced inside the inner loop.
							p = q + 1;
							break;
						}
					}
				}
				// Trailing advance shared by every path through the balanced scan.
				q++;
			}
			// Ran off the end without closing: return what was collected rather than
			// emitting a truncated object. A malformed response degrades to a shorter
			// list instead of a crash.
			if (q >= json.size())
				return items;
		}
		return items;
	}

}
