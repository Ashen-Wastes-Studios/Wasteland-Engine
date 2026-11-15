#include "wlpch.h"

#include "Wasteland/Project/StringTable.h"

#include <algorithm>

namespace Wasteland
{
	namespace
	{
		// One ASCII vowel -> a 2-byte UTF-8 accented twin. Building the UTF-8
		// bytes by hand keeps this dependency-free and byte-deterministic.
		std::string Accent(char character)
		{
			switch (character)
			{
				case 'a': return "\xc3\xa1"; // á
				case 'e': return "\xc3\xa9"; // é
				case 'i': return "\xc3\xad"; // í
				case 'o': return "\xc3\xb3"; // ó
				case 'u': return "\xc3\xba"; // ú
				case 'A': return "\xc3\x81"; // Á
				case 'E': return "\xc3\x89"; // É
				case 'I': return "\xc3\x8d"; // Í
				case 'O': return "\xc3\x93"; // Ó
				case 'U': return "\xc3\x9a"; // Ú
				default: return std::string(1, character);
			}
		}
	}

	std::string PseudoLocalizeText(const std::string &text)
	{
		std::string widened = "[";
		for (char c : text)
			widened += Accent(c);
		widened += "]";
		return widened;
	}

	void StringTable::SetBase(const std::string &key, const std::string &text)
	{
		const bool first = !m_Base.count(key);
		m_Base[key] = text;
		if (first)
			m_Keys.push_back(key);
	}

	void StringTable::SetLocaleOverride(const std::string &locale,
	                                    const std::string &key,
	                                    const std::string &text)
	{
		if (locale == m_ActiveLocale)
		{
			if (m_Base.count(key))
				m_Locale[key] = text; // only meaningful once a base exists
		}
	}

	void StringTable::SetActiveLocale(const std::string &locale)
	{
		m_ActiveLocale = locale;
		m_Locale.clear();
	}

	std::string StringTable::Translate(const std::string &key) const
	{
		const auto localeHit = m_Locale.find(key);
		if (localeHit != m_Locale.end())
			return localeHit->second;
		const auto baseHit = m_Base.find(key);
		if (baseHit != m_Base.end())
			return baseHit->second;
		// A missing key returns the key, visible -- an empty string is
		// indistinguishable from a real empty value.
		return key;
	}

	bool StringTable::Has(const std::string &key) const
	{
		return m_Base.count(key) != 0;
	}

	std::size_t StringTable::Count() const
	{
		return m_Base.size();
	}

	std::vector<std::pair<std::string, std::string>> StringTable::PseudoRun() const
	{
		std::vector<std::pair<std::string, std::string>> run;
		run.reserve(m_Keys.size());
		for (const std::string &key : m_Keys)
		{
			const std::string text =
			    m_Locale.find(key) != m_Locale.end() ? m_Locale.at(key)
			                                         : m_Base.at(key);
			run.emplace_back(key, PseudoLocalizeText(text));
		}
		return run;
	}
}
