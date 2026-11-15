#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Wasteland {

	// A minimal JSON reader, written for the glTF 2.0 importer.
	//
	// Why not a vendor library: the engine vendors yaml-cpp and that is it, and
	// a glTF file is the one place the engine must read JSON. Pulling in a
	// full JSON library for a reader that only ever walks a known schema would
	// add a dependency and a build step to solve a problem that is genuinely
	// this small: objects, arrays, strings, numbers, and three literals.
	//
	// What this deliberately is NOT: a general-purpose JSON tool. It does not
	// serialise. It does not preserve number formatting. It accepts any
	// well-formed JSON rather than only glTF, because a parser that rejects a
	// file for a reason unrelated to glTF is a worse diagnostic than one that
	// parses it and lets the glTF layer name the missing accessor.
	//
	// Object member order is preserved. glTF does not require it, but a
	// diagnostic that can say "the 3rd mesh primitive" is easier to act on
	// than one that says "some primitive".
	struct JsonValue
	{
		enum class Type
		{
			Null,
			Bool,
			Number,
			String,
			Array,
			Object
		};

		Type ValueType = Type::Null;
		bool BoolValue = false;
		double NumberValue = 0.0;
		std::string StringValue;
		std::vector<JsonValue> ArrayItems;
		std::vector<std::pair<std::string, JsonValue>> ObjectItems;

		bool IsNull() const { return ValueType == Type::Null; }
		bool IsBool() const { return ValueType == Type::Bool; }
		bool IsNumber() const { return ValueType == Type::Number; }
		bool IsString() const { return ValueType == Type::String; }
		bool IsArray() const { return ValueType == Type::Array; }
		bool IsObject() const { return ValueType == Type::Object; }

		// Object lookup. Returns nullptr when the value is not an object or the
		// key is absent -- one call site, one form, and a caller can distinguish
		// "absent" from "present but null" only if it asks twice, which glTF
		// never needs.
		const JsonValue *Find(const std::string &key) const
		{
			if (ValueType != Type::Object)
				return nullptr;
			for (const auto &item : ObjectItems)
			{
				if (item.first == key)
					return &item.second;
			}
			return nullptr;
		}

		// Array element access with a bounds check. operator[] on a vector is
		// unchecked, and a truncated .glb would otherwise read past the end
		// instead of reporting a malformed file.
		const JsonValue *At(size_t index) const
		{
			if (ValueType != Type::Array || index >= ArrayItems.size())
				return nullptr;
			return &ArrayItems[index];
		}

		size_t Size() const { return ArrayItems.size(); }

		// Typed accessors. Each returns false and leaves `out` untouched when
		// the value is absent or of the wrong type, so a caller can write
		// `if (v.AsNumber(n)) ...` rather than a three-line guard. A glTF file
		// with the wrong type for a field is malformed, and the importer wants
		// to report that rather than coerce silently.
		bool AsBool(bool &out) const
		{
			if (ValueType != Type::Bool)
				return false;
			out = BoolValue;
			return true;
		}

		bool AsNumber(double &out) const
		{
			if (ValueType != Type::Number)
				return false;
			out = NumberValue;
			return true;
		}

		bool AsString(std::string &out) const
		{
			if (ValueType != Type::String)
				return false;
			out = StringValue;
			return true;
		}

		// Convenience for the overwhelmingly common "string or absent" case.
		std::string StringOr(const std::string &fallback = "") const
		{
			return ValueType == Type::String ? StringValue : fallback;
		}
	};

	// Parses a complete JSON document.
	//
	// Returns false on a syntax error and fills `error` with a message that
	// includes the LINE and COLUMN. A glTF file with a trailing comma is still
	// a malformed file, and "unexpected token at line 1 column 1" is a
	// diagnostic a user can act on; "parse failed" is not.
	//
	// The parser is iterative over the input but recursive over nesting, which
	// is the shape of JSON. Recursion depth is bounded by the document's own
	// nesting; a hostile file could exhaust the stack, but glTF files come
	// from DCC tools and are never deeply nested.
	bool JsonParse(const std::string &text, JsonValue &out, std::string &error);

} // namespace Wasteland
