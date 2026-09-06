#include "wlpch.h"
#include "JsonMini.h"

#include <cctype>

namespace Wasteland::JsonMini
{

	std::string Escape(const std::string &s)
	{
		std::string out;
		out.reserve(s.size() + 8);
		static const char *hex = "0123456789abcdef";
		for (unsigned char c : s)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			default:
				if (c < 0x20)
				{
					out += "\\u00";
					out += hex[c >> 4];
					out += hex[c & 0xF];
				}
				else
					out += (char)c;
			}
		}
		return out;
	}

	namespace
	{
		// Locate `"key"` followed by optional whitespace and ':'.
		// Returns position just past ':', or npos.
		size_t FindKeyValue(const std::string &json, const std::string &key, size_t from = 0)
		{
			std::string quoted = "\"" + key + "\"";
			size_t pos = from;
			while ((pos = json.find(quoted, pos)) != std::string::npos)
			{
				size_t after = pos + quoted.size();
				while (after < json.size() && (json[after] == ' ' || json[after] == '\t' || json[after] == '\n' || json[after] == '\r'))
					after++;
				if (after < json.size() && json[after] == ':')
					return after + 1;
				pos = after;
			}
			return std::string::npos;
		}

		size_t SkipWs(const std::string &s, size_t p)
		{
			while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
				p++;
			return p;
		}

		bool ParseHex4(const std::string &s, size_t p, uint32_t &out)
		{
			if (p + 4 > s.size())
				return false;
			uint32_t v = 0;
			for (int i = 0; i < 4; i++)
			{
				char c = s[p + i];
				v <<= 4;
				if (c >= '0' && c <= '9')
					v |= (uint32_t)(c - '0');
				else if (c >= 'a' && c <= 'f')
					v |= (uint32_t)(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F')
					v |= (uint32_t)(c - 'A' + 10);
				else
					return false;
			}
			out = v;
			return true;
		}

		void AppendUtf8(std::string &out, uint32_t cp)
		{
			if (cp < 0x80)
				out += (char)cp;
			else if (cp < 0x800)
			{
				out += (char)(0xC0 | (cp >> 6));
				out += (char)(0x80 | (cp & 0x3F));
			}
			else if (cp < 0x10000)
			{
				out += (char)(0xE0 | (cp >> 12));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			}
			else
			{
				out += (char)(0xF0 | (cp >> 18));
				out += (char)(0x80 | ((cp >> 12) & 0x3F));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			}
		}

		// Parse a JSON string starting at s[p] == '"'. On success sets out and past-end pos.
		bool ParseString(const std::string &s, size_t p, std::string &out, size_t &endPos)
		{
			if (p >= s.size() || s[p] != '"')
				return false;
			p++;
			std::string result;
			while (p < s.size())
			{
				char c = s[p];
				if (c == '"')
				{
					out = result;
					endPos = p + 1;
					return true;
				}
				if (c == '\\')
				{
					p++;
					if (p >= s.size())
						return false;
					char e = s[p];
					switch (e)
					{
					case '"': result += '"'; break;
					case '\\': result += '\\'; break;
					case '/': result += '/'; break;
					case 'b': result += '\b'; break;
					case 'f': result += '\f'; break;
					case 'n': result += '\n'; break;
					case 'r': result += '\r'; break;
					case 't': result += '\t'; break;
					case 'u':
					{
						uint32_t cp = 0;
						if (!ParseHex4(s, p + 1, cp))
							return false;
						p += 4;
						if (cp >= 0xD800 && cp <= 0xDBFF && p + 5 < s.size() && s[p + 1] == '\\' && s[p + 2] == 'u')
						{
							uint32_t lo = 0;
							if (ParseHex4(s, p + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF)
							{
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
								p += 6;
							}
						}
						AppendUtf8(result, cp);
						break;
					}
					default:
						result += e;
						break;
					}
					p++;
				}
				else
				{
					result += c;
					p++;
				}
			}
			return false;
		}
	}

	bool ExtractString(const std::string &json, const std::string &key, std::string &out)
	{
		size_t v = FindKeyValue(json, key);
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		size_t end = 0;
		return ParseString(json, v, out, end);
	}

	bool ExtractInt64(const std::string &json, const std::string &key, int64_t &out)
	{
		size_t v = FindKeyValue(json, key);
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		size_t e = v;
		if (e < json.size() && (json[e] == '-' || json[e] == '+'))
			e++;
		size_t start = e;
		while (e < json.size() && (std::isdigit((unsigned char)json[e]) || json[e] == '.' || json[e] == 'e' || json[e] == 'E' || json[e] == '+' || json[e] == '-'))
			e++;
		if (e == start)
			return false;
		try
		{
			out = std::stoll(json.substr(v, e - v));
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool ExtractDouble(const std::string &json, const std::string &key, double &out)
	{
		int64_t i = 0;
		// Reuse number scan but parse as double.
		size_t v = FindKeyValue(json, key);
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		size_t e = v;
		if (e < json.size() && (json[e] == '-' || json[e] == '+'))
			e++;
		size_t start = e;
		while (e < json.size() && (std::isdigit((unsigned char)json[e]) || json[e] == '.' || json[e] == 'e' || json[e] == 'E' || json[e] == '+' || json[e] == '-'))
			e++;
		if (e == start)
			return false;
		try
		{
			out = std::stod(json.substr(v, e - v));
			(void)i;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool ExtractBool(const std::string &json, const std::string &key, bool &out)
	{
		std::string tok;
		if (!ExtractRawToken(json, key, tok))
			return false;
		if (tok == "true")
		{
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

	bool ExtractRawToken(const std::string &json, const std::string &key, std::string &out)
	{
		size_t v = FindKeyValue(json, key);
		if (v == std::string::npos)
			return false;
		v = SkipWs(json, v);
		size_t e = v;
		if (e < json.size() && json[e] == '"')
		{
			std::string s;
			size_t end = 0;
			if (!ParseString(json, e, s, end))
				return false;
			out = s;
			return true;
		}
		while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != ']' && json[e] != '\n')
			e++;
		// trim
		size_t s = v;
		while (s < e && std::isspace((unsigned char)json[s]))
			s++;
		while (e > s && std::isspace((unsigned char)json[e - 1]))
			e--;
		if (e == s)
			return false;
		out = json.substr(s, e - s);
		return true;
	}

	std::vector<std::string> SplitArrayObjects(const std::string &json)
	{
		std::vector<std::string> items;
		size_t arr = json.find('[');
		if (arr == std::string::npos)
			return items;
		size_t p = arr + 1;
		while (p < json.size())
		{
			// Find next '{' at depth 0 of the array, respecting strings.
			bool inStr = false;
			bool esc = false;
			size_t objStart = std::string::npos;
			while (p < json.size())
			{
				char c = json[p];
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
					{
						objStart = p;
						break;
					}
					else if (c == ']')
						return items;
				}
				p++;
			}
			if (objStart == std::string::npos)
				return items;
			// Consume balanced object.
			int depth = 0;
			inStr = false;
			esc = false;
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
						depth++;
					else if (c == '}')
					{
						depth--;
						if (depth == 0)
						{
							items.push_back(json.substr(objStart, q - objStart + 1));
							p = q + 1;
							break;
						}
					}
				}
				q++;
			}
			if (q >= json.size())
				return items;
		}
		return items;
	}

}
