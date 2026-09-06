#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Wasteland::JsonMini
{

	// Escape a string for inclusion in a JSON document.
	std::string Escape(const std::string &s);

	// Finds the first `"key" : <string>` (from the start of json) and decodes it
	// (handles \" \\ \/ \b \f \n \r \t \uXXXX incl. surrogate pairs).
	// Returns false if not found or value is null/non-string.
	bool ExtractString(const std::string &json, const std::string &key, std::string &out);

	// Finds the first `"key" : <number>`. Returns false if not found.
	bool ExtractInt64(const std::string &json, const std::string &key, int64_t &out);
	bool ExtractDouble(const std::string &json, const std::string &key, double &out);

	// Finds the first `"key" : true|false`. Returns false if not found.
	bool ExtractBool(const std::string &json, const std::string &key, bool &out);

	// Raw token after `"key" :` up to ',', '}' or ']' (trimmed, unquoted).
	// Useful for fields that may be bool OR string (e.g. HF "gated").
	bool ExtractRawToken(const std::string &json, const std::string &key, std::string &out);

	// Splits the first top-level JSON array in json into its `{...}` element substrings.
	std::vector<std::string> SplitArrayObjects(const std::string &json);

}
