#pragma once

#include <cstdint>
#include <string>
#include <vector>

// JsonMini, not Wasteland::JsonMini at every call site. A nested namespace keeps
// the name short in the editor panels that use it.
namespace Wasteland::JsonMini
{

	// Escape a string for inclusion in a JSON document.
	// Escape handles the ENCLOSING quotes as well as the contents: a caller writing
	// a key into a document it is assembling needs the quotes too.
	std::string Escape(const std::string &s);

	// Finds the first `"key" : <string>` (from the start of json) and decodes it
	// (handles \" \\ \/ \b \f \n \r \t \uXXXX incl. surrogate pairs).
	// Returns false if not found or value is null/non-string.
	// The workhorse. Searches from the START of `json` and takes the FIRST match,
	// with no knowledge of nesting.
	//
	// DEFECT (scope): for {"usage":{"prompt_tokens":1},"prompt_tokens":2} this
	// returns 1 -- the inner key shadows the outer. Fine for the flat responses it is
	// used on; silently wrong for anything nested. Not fixed; comments only.
	//
	// False covers a missing key AND a null or non-string value, so `out` is left
	// untouched either way.
	bool ExtractString(const std::string &json, const std::string &key, std::string &out);

	// Finds the first `"key" : <number>`. Returns false if not found.
	// The number extractors share that same first-match scan, and so the same
	// nesting caveat as ExtractString above.
	bool ExtractInt64(const std::string &json, const std::string &key, int64_t &out);
	bool ExtractDouble(const std::string &json, const std::string &key, double &out);

	// Finds the first `"key" : true|false`. Returns false if not found.
	// Bool is matched as a bare keyword, so a QUOTED true will not match -- correct
	// per the format, and the reason ExtractRawToken exists for either-or fields.
	bool ExtractBool(const std::string &json, const std::string &key, bool &out);

	// Raw token after `"key" :` up to ',', '}' or ']' (trimmed, unquoted).
	// Useful for fields that may be bool OR string (e.g. HF "gated").
	// The escape hatch for the bool-or-string case the header names (a HuggingFace
	// "gated" field). Stops at the first delimiter, so a nested value comes back
	// truncated -- consistent with the scan above, fine for scalars.
	bool ExtractRawToken(const std::string &json, const std::string &key, std::string &out);

	// Splits the first top-level JSON array in json into its `{...}` element substrings.
	// DEFECT (first array only, shallow): slices element substrings without
	// tracking escapes or nesting, so a brace inside a string splits in the wrong
	// place. Acceptable for the array-of-objects responses it is used on; the
	// alternative is a real parser, which is what this header exists to avoid.
	std::vector<std::string> SplitArrayObjects(const std::string &json);

}
