#include "wlpch.h"

#include "Json.h"

#include <cstdlib>

namespace Wasteland {

	namespace
	{
		// A cursor over the document. Line and column are tracked because the
		// only thing a user can do with a JSON error is find the offending
		// token, and "unexpected token" without a position is useless.
		struct Cursor
		{
			const std::string &Text;
			size_t Position = 0;
			size_t Line = 1;
			size_t Column = 1;

			explicit Cursor(const std::string &text) : Text(text) {}

			bool AtEnd() const { return Position >= Text.size(); }

			char Peek() const { return AtEnd() ? '\0' : Text[Position]; }

			char Advance()
			{
				if (AtEnd())
					return '\0';
				const char current = Text[Position++];
				// JSON forbids raw newlines inside strings, so a line counter
				// that only counts them outside strings is still exact for
				// every position a diagnostic can name.
				if (current == '\n')
				{
					++Line;
					Column = 1;
				}
				else
				{
					++Column;
				}
				return current;
			}

			void SkipWhitespace()
			{
				while (!AtEnd())
				{
					const char c = Peek();
					if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
						Advance();
					else
						break;
				}
			}

			std::string Where() const
			{
				return "line " + std::to_string(Line) + " column "
					   + std::to_string(Column);
			}

			// The text consumed since `start`. Used by the number parser, which
			// hands the raw token to strtod rather than reimplementing the
			// grammar it just validated.
			std::string SubstringSince(size_t start) const
			{
				return Text.substr(start, Position - start);
			}
		};

		bool ParseValue(Cursor &cursor, JsonValue &out, std::string &error);

		bool ParseString(Cursor &cursor, std::string &out, std::string &error)
		{
			if (cursor.Peek() != '"')
			{
				error = "expected a string at " + cursor.Where();
				return false;
			}
			cursor.Advance(); // opening quote

			out.clear();
			while (true)
			{
				if (cursor.AtEnd())
				{
					error = "unterminated string starting before " + cursor.Where();
					return false;
				}

				const char c = cursor.Advance();
				if (c == '"')
					return true;

				if (c == '\\')
				{
					if (cursor.AtEnd())
					{
						error = "unterminated escape at " + cursor.Where();
						return false;
					}
					const char escape = cursor.Advance();
					switch (escape)
					{
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u':
					{
						// \uXXXX. glTF uses this for non-ASCII asset names;
						// a UTF-8 encode of the code point is what the rest of
						// the engine expects. Surrogate pairs are handled by
						// combining a high surrogate with the following low
						// one, which is the only legal use of a surrogate in
						// JSON.
						auto readHex4 = [&](unsigned &value) -> bool {
							value = 0;
							for (int i = 0; i < 4; ++i)
							{
								if (cursor.AtEnd())
									return false;
								const char digit = cursor.Advance();
								value <<= 4;
								if (digit >= '0' && digit <= '9')
									value |= (unsigned)(digit - '0');
								else if (digit >= 'a' && digit <= 'f')
									value |= (unsigned)(digit - 'a' + 10);
								else if (digit >= 'A' && digit <= 'F')
									value |= (unsigned)(digit - 'A' + 10);
								else
									return false;
							}
							return true;
						};

						unsigned codePoint = 0;
						if (!readHex4(codePoint))
						{
							error = "invalid \\u escape at " + cursor.Where();
							return false;
						}

						// High surrogate: must be followed by \uDC00-\uDFFF.
						if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
						{
							if (cursor.Peek() != '\\')
							{
								error = "lone high surrogate at " + cursor.Where();
								return false;
							}
							cursor.Advance();
							if (cursor.Peek() != 'u')
							{
								error = "lone high surrogate at " + cursor.Where();
								return false;
							}
							cursor.Advance();
							unsigned low = 0;
							if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF)
							{
								error = "invalid surrogate pair at " + cursor.Where();
								return false;
							}
							codePoint = 0x10000 + ((codePoint - 0xD800) << 10)
										+ (low - 0xDC00);
						}
						else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
						{
							error = "lone low surrogate at " + cursor.Where();
							return false;
						}

						// UTF-8 encode. The widths are fixed by the code point
						// range, so this is a chain of range tests rather than a
						// loop -- one fewer thing to get wrong.
						if (codePoint < 0x80)
						{
							out += (char)codePoint;
						}
						else if (codePoint < 0x800)
						{
							out += (char)(0xC0 | (codePoint >> 6));
							out += (char)(0x80 | (codePoint & 0x3F));
						}
						else if (codePoint < 0x10000)
						{
							out += (char)(0xE0 | (codePoint >> 12));
							out += (char)(0x80 | ((codePoint >> 6) & 0x3F));
							out += (char)(0x80 | (codePoint & 0x3F));
						}
						else
						{
							out += (char)(0xF0 | (codePoint >> 18));
							out += (char)(0x80 | ((codePoint >> 12) & 0x3F));
							out += (char)(0x80 | ((codePoint >> 6) & 0x3F));
							out += (char)(0x80 | (codePoint & 0x3F));
						}
						break;
					}
					default:
						error = "invalid escape '\\" + std::string(1, escape)
								+ "' at " + cursor.Where();
						return false;
					}
					continue;
				}

				// A raw control character inside a string is malformed JSON.
				if ((unsigned char)c < 0x20)
				{
					error = "unescaped control character at " + cursor.Where();
					return false;
				}
				out += c;
			}
		}

		bool ParseNumber(Cursor &cursor, double &out, std::string &error)
		{
			const size_t start = cursor.Position;

			// Optional minus. A leading plus is not legal JSON.
			if (cursor.Peek() == '-')
				cursor.Advance();

			// Integer part: either a lone 0 or a non-zero digit followed by
			// digits. "01" is malformed, and accepting it would make two
			// spellings of the same number.
			if (cursor.Peek() == '0')
			{
				cursor.Advance();
			}
			else if (cursor.Peek() >= '1' && cursor.Peek() <= '9')
			{
				while (cursor.Peek() >= '0' && cursor.Peek() <= '9')
					cursor.Advance();
			}
			else
			{
				error = "invalid number at " + cursor.Where();
				return false;
			}

			// Fraction.
			if (cursor.Peek() == '.')
			{
				cursor.Advance();
				if (!(cursor.Peek() >= '0' && cursor.Peek() <= '9'))
				{
					error = "number has no digits after the decimal point at "
							+ cursor.Where();
					return false;
				}
				while (cursor.Peek() >= '0' && cursor.Peek() <= '9')
					cursor.Advance();
			}

			// Exponent.
			const char exponent = cursor.Peek();
			if (exponent == 'e' || exponent == 'E')
			{
				cursor.Advance();
				const char sign = cursor.Peek();
				if (sign == '+' || sign == '-')
					cursor.Advance();
				if (!(cursor.Peek() >= '0' && cursor.Peek() <= '9'))
				{
					error = "exponent has no digits at " + cursor.Where();
					return false;
				}
				while (cursor.Peek() >= '0' && cursor.Peek() <= '9')
					cursor.Advance();
			}

			out = std::strtod(cursor.SubstringSince(start).c_str(), nullptr);
			return true;
		}

		bool ParseArray(Cursor &cursor, JsonValue &out, std::string &error)
		{
			cursor.Advance(); // '['
			out.ValueType = JsonValue::Type::Array;
			out.ArrayItems.clear();

			cursor.SkipWhitespace();
			if (cursor.Peek() == ']')
			{
				cursor.Advance();
				return true;
			}

			while (true)
			{
				JsonValue element;
				if (!ParseValue(cursor, element, error))
					return false;
				out.ArrayItems.push_back(std::move(element));

				cursor.SkipWhitespace();
				const char c = cursor.Peek();
				if (c == ',')
				{
					cursor.Advance();
					continue;
				}
				if (c == ']')
				{
					cursor.Advance();
					return true;
				}
				error = "expected ',' or ']' in array at " + cursor.Where();
				return false;
			}
		}

		bool ParseObject(Cursor &cursor, JsonValue &out, std::string &error)
		{
			cursor.Advance(); // '{'
			out.ValueType = JsonValue::Type::Object;
			out.ObjectItems.clear();

			cursor.SkipWhitespace();
			if (cursor.Peek() == '}')
			{
				cursor.Advance();
				return true;
			}

			while (true)
			{
				cursor.SkipWhitespace();
				std::string key;
				if (!ParseString(cursor, key, error))
					return false;

				cursor.SkipWhitespace();
				if (cursor.Peek() != ':')
				{
					error = "expected ':' after object key at " + cursor.Where();
					return false;
				}
				cursor.Advance();

				JsonValue value;
				if (!ParseValue(cursor, value, error))
					return false;
				out.ObjectItems.emplace_back(std::move(key), std::move(value));

				cursor.SkipWhitespace();
				const char c = cursor.Peek();
				if (c == ',')
				{
					cursor.Advance();
					continue;
				}
				if (c == '}')
				{
					cursor.Advance();
					return true;
				}
				error = "expected ',' or '}' in object at " + cursor.Where();
				return false;
			}
		}

		bool ParseValue(Cursor &cursor, JsonValue &out, std::string &error)
		{
			cursor.SkipWhitespace();
			const char c = cursor.Peek();

			if (c == '{')
				return ParseObject(cursor, out, error);
			if (c == '[')
				return ParseArray(cursor, out, error);
			if (c == '"')
			{
				out.ValueType = JsonValue::Type::String;
				return ParseString(cursor, out.StringValue, error);
			}
			if (c == 't')
			{
				const char *literal = "true";
				for (int i = 0; i < 4; ++i)
				{
					if (cursor.Advance() != (unsigned char)literal[i])
					{
						error = "invalid literal at " + cursor.Where();
						return false;
					}
				}
				out.ValueType = JsonValue::Type::Bool;
				out.BoolValue = true;
				return true;
			}
			if (c == 'f')
			{
				const char *literal = "false";
				for (int i = 0; i < 5; ++i)
				{
					if (cursor.Advance() != (unsigned char)literal[i])
					{
						error = "invalid literal at " + cursor.Where();
						return false;
					}
				}
				out.ValueType = JsonValue::Type::Bool;
				out.BoolValue = false;
				return true;
			}
			if (c == 'n')
			{
				const char *literal = "null";
				for (int i = 0; i < 4; ++i)
				{
					if (cursor.Advance() != (unsigned char)literal[i])
					{
						error = "invalid literal at " + cursor.Where();
						return false;
					}
				}
				out.ValueType = JsonValue::Type::Null;
				return true;
			}
			if (c == '-' || (c >= '0' && c <= '9'))
			{
				// The type must be set by the caller, because ParseNumber only
				// receives the double. Leaving it unset types every JSON
				// number as Null, and then IsNumber() is false while
				// AsNumber() still returns the value -- so a caller that
				// guards on the type (the glTF importer checks IsNumber() on
				// every accessor index) silently rejects valid data.
				out.ValueType = JsonValue::Type::Number;
				return ParseNumber(cursor, out.NumberValue, error);
			}

			error = std::string("unexpected character '") + c + "' at "
					+ cursor.Where();
			return false;
		}
	} // namespace

	bool JsonParse(const std::string &text, JsonValue &out, std::string &error)
	{
		Cursor cursor(text);
		if (!ParseValue(cursor, out, error))
			return false;

		// Trailing content after the top-level value is malformed. A .gltf
		// with two JSON documents concatenated would otherwise be accepted
		// with the second silently ignored.
		cursor.SkipWhitespace();
		if (!cursor.AtEnd())
		{
			error = "unexpected trailing content at " + cursor.Where();
			return false;
		}
		return true;
	}

} // namespace Wasteland
