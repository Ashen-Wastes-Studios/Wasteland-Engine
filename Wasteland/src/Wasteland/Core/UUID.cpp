#include "wlpch.h"
#include "UUID.h"

#include <random>

#include <unordered_map>

namespace Wasteland {

	// Process-wide RNG used to mint new UUIDs.
	//
	// These are namespace-scope statics with internal linkage, so they are
	// dynamically initialized before main() in this translation unit's init
	// order -- top to bottom. That ordering is load-bearing rather than
	// incidental: s_Engine is seeded from s_RandomDevice, and the default
	// UUID constructor below draws from s_UniformDistribution. Reordering these
	// three declarations would silently change the seeding behaviour or read
	// an unconstructed distribution.
	//
	// Because they are `static`, each has internal linkage and there is one
	// instance per process, shared by every UUID generated in this binary.
	// Note that they are therefore NOT thread-safe against concurrent
	// construction: two threads calling the default UUID constructor at the
	// same moment both hit the same unsynchronized s_Engine.
	//
	// s_RandomDevice is the entropy source, typically the OS CSPRNG. It is
	// held as a named object rather than used in a one-shot expression so the
	// handle is reused across calls.
	static std::random_device s_RandomDevice;

	// The 64-bit Mersenne Twister engine, seeded once from s_RandomDevice.
	// mt19937_64 is used rather than mt19937 because UUIDs are 64 bits wide;
	// the 32-bit variant would have to be called twice per UUID and would
	// still only yield 64 bits of a weaker generator.
	//
	// Must be declared after s_RandomDevice -- see the ordering note above.
	static std::mt19937_64 s_Engine(s_RandomDevice());

	// Uniform distribution over the full uint64_t range, [0, 2^64).
	//
	// Held as a named object because the standard permits a distribution to
	// cache state between calls; reusing one instance is both correct and
	// faster than constructing a fresh distribution per UUID.
	static std::uniform_int_distribution<uint64_t> s_UniformDistribution;

	// Generates a new random UUID.
	//
	// Initialised in the member-init list rather than assigned in the body,
	// so m_UUID is never briefly default-initialized.
	//
	// Collision probability is negligible (birthday bound puts a collision
	// around 2^32 UUIDs), but nothing here checks for one: if a generated
	// value collided with a live entity's id, the two would be
	// indistinguishable.
	UUID::UUID()
		: m_UUID(s_UniformDistribution(s_Engine))
	{

	}

	// Wraps an existing 64-bit value, for restoring a UUID from a save file.
	//
	// Straight copy with no validation -- the caller is trusted to supply a
	// value that was previously produced by this class.
	UUID::UUID(uint64_t uuid)
		: m_UUID(uuid)
	{

	}

}