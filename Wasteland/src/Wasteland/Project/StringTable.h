// StringTable.h -- the engine-side string table (Phase J.7).
//
// The plan's J.7 has two halves. "All editor strings go through a string table"
// is a ONE-TIME SWEEP over the editor panels -- editor work, blocked. But the
// table itself, and the pseudo-localization pass that proves strings are table
// strings at all, are engine behaviour that a headless test can verify now.
//
// A missing key returns the KEY (not empty): an un-translated string showing the
// literal key is a visible "this text bypassed the table" defect; an empty string
// is indistinguishable from a real empty value. Same convention as most i18n
// runtimes.
//
// Pseudo-localization ("expand every string by 30%" in the plan) replaces vowels
// with accented twins (so a genuinely localized string is visibly different) and
// brackets the result (adding width) -- a string that is NOT long enough after
// the pass, or that truncates in a fixed-width panel, has a translation bug that
// only a pseudo-LOC run exposes. Deterministic: same text in, same bytes out.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace Wasteland
{
	// Pseudo-localizes `text`: bracket it and replace ASCII vowels with accented
	// twins so the byte length grows ~30%+ and hardcoded (un-table) strings are
	// visibly different from translated ones. Free function so a test can assert
	// the transform in isolation.
	std::string PseudoLocalizeText(const std::string &text);

	class StringTable
	{
	public:
		// The base-language text for a key.
		void SetBase(const std::string &key, const std::string &text);
		// A per-locale override. An override for a key with no base entry is
		// stored but never returned (there is nothing to localize yet).
		void SetLocaleOverride(const std::string &locale, const std::string &key,
		                       const std::string &text);
		void SetActiveLocale(const std::string &locale);

		// The active locale's text for `key`, else the base text, else the key.
		// Missing keys return the key -- visible, not empty.
		std::string Translate(const std::string &key) const;

		bool Has(const std::string &key) const;
		std::size_t Count() const;

		// A pseudo-LOC run of every base key, returning (key, pseudo) pairs so a
		// caller can paint every table string through the pass in one call.
		std::vector<std::pair<std::string, std::string>> PseudoRun() const;

	private:
		std::unordered_map<std::string, std::string> m_Base;
		std::unordered_map<std::string, std::string> m_Locale;   // active locale
		std::vector<std::string> m_Keys;                         // insertion order
		std::string m_ActiveLocale;
	};
}
