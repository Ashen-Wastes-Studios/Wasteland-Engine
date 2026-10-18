#pragma once

// UUID.h -- a 64-bit entity identifier with hashing and formatting support.
//
// A UUID is how the engine refers to entities and assets across a save/load
// boundary. Scene files store these, so a UUID has to survive a round trip
// through text, compare by value, and work as a key in an unordered container.
// The fmt and std::hash specializations at the bottom of this file are what
// make the last two of those work.

#include <functional>
#include <spdlog/fmt/fmt.h>

namespace Wasteland
{
	// A 64-bit unique identifier, used as the stable identity of an entity or
	// asset.
	//
	// Equality is by value: two UUIDs are equal when their underlying 64-bit
	// values match. Note there is no operator!=, and the project builds as
	// C++17 (stdcpp17), which does not synthesize a negated candidate from
	// operator== the way C++20 does. So `a != b` will not compile here --
	// write `!(a == b)`.
	class UUID
	{
	public:
		// Default constructor: generates a fresh random UUID.
		//
		// Defined out of line in UUID.cpp because the generation pulls in
		// <random> and the engine's static RNG, which is deliberately kept
		// out of this widely-included header.
		UUID();

		// Constructs a UUID from an existing 64-bit value.
		//
		// This is how a UUID is restored from a saved scene file. It is NOT
		// explicit, so a bare integer converts to a UUID implicitly; see the
		// conversion-operator note below for why that cuts both ways.
		UUID(uint64_t uuid);

		// Copy constructor. Explicitly defaulted rather than implicit: the
		// class is a plain value wrapper, so the compiler-generated copy is
		// exactly right and there is nothing to add.
		UUID(const UUID &) = default;

		// Implicit conversion to the underlying 64-bit value.
		//
		// Lets a UUID be used directly as an integer -- for arithmetic, for
		// printf-style formatting, for comparison against a literal. It also
		// means integers convert *to* UUID implicitly via the constructor
		// above, so `uuid == 0` and `0 == uuid` both compile and both mean
		// "compare against the integer zero". That symmetry is convenient for
		// sentinel checks but is also a genuine footgun: a mistyped argument
		// can silently become a UUID rather than failing to compile.
		operator uint64_t() const { return m_UUID; }

		// Value equality. Const on both operands and non-noexcept, so it
		// cannot be used to terminate an unordered container's equality
		// chain without a potential exception throw; see the std::hash
		// specialization below.
		bool operator==(const UUID &other) const { return m_UUID == other.m_UUID; }

	private:
		// The identifier itself. Single 64-bit value, no version or variant
		// bits in the RFC-4122 sense -- this is a random 64-bit number, not a
		// conforming UUID, and it prints as decimal (see the fmt
		// specialization below).
		uint64_t m_UUID;
	};
}

// Teaches spdlog/fmt how to render a UUID as text.
//
// Specialized on fmt::formatter, which is how a UUID reaches the output of
// spdlog, std::format, and the YAML emitter in SceneSerializer.cpp -- that
// emitter streams a UUID with `out << entity.GetUUID()`, so this formatter is
// what decides the on-disk representation of an entity id in a scene file.
//
// The value is rendered as plain decimal via std::to_string, NOT as the
// conventional hyphenated hex form. That is internally consistent, since the
// matching reader parses the same decimal back through the uint64_t
// constructor, but it means UUIDs in scene files do not look like UUIDs.
namespace fmt
{
	template <>
	struct formatter<Wasteland::UUID> : formatter<std::string_view>
	{
		// Formats the UUID as its decimal string representation.
		//
		// The C-style cast to uint64_t invokes UUID's implicit conversion
		// operator. Base class is formatter<std::string_view> even though a
		// temporary std::string is passed in -- the temporary outlives the
		// call, so the view stays valid for the duration of format().
		auto format(const Wasteland::UUID &uuid, format_context &ctx) const
		{
			return formatter<std::string_view>::format(std::to_string((uint64_t)uuid), ctx);
		}
	};
}

// Teaches std::hash how to hash a UUID, which is what allows a UUID to be a
// key in std::unordered_map / std::unordered_set.
//
// The hash simply defers to hash<uint64_t>, which is exactly consistent with
// operator== above: two UUIDs compare equal if and only if their 64-bit values
// are equal, so equal UUIDs always produce equal hashes. That consistency is
// the whole requirement for a usable specialization, and is why this can be so
// short.
namespace std
{
	template <>
	struct hash<Wasteland::UUID>
	{
		// Returns the hash of the UUID's underlying 64-bit value.
		std::size_t operator()(const Wasteland::UUID &uuid) const
		{
			return hash<uint64_t>()((uint64_t)uuid);
		}
	};
}