// Precompiled header, first include as the engine requires. It is what makes
// std::string usable below without this file including <string> itself --
// AgeBand.h pulls that in for its own declarations only.
#include "wlpch.h"

#include "Wasteland/Age/AgeBand.h"

// For the two exception types thrown below. They are the only error
// mechanism this module uses: no error code, no bool out-param, no
// 'Unknown' fallback on the age path. An invalid age throws rather than
// returning AgeBand::Unknown, which is a deliberate asymmetry with
// BandName below -- Unknown means 'no signal arrived', not 'the age was
// rejected'.
#include <stdexcept>

// Nested-namespace definition (C++17), so no braces of its own are needed
// and the closing line carries the comment.
namespace Wasteland::Age
{
	// Maps a concrete age to its band. Declared in AgeBand.h with a long
	// comment on why the parameter is signed rather than unsigned.
	//
	// A free function rather than a member of anything: AgeBand is an enum, not
	// a class, so these operations are namespace-level by necessity.
	AgeBand BandForAge(int age)
	{
		// See the note on the declaration: an unsigned parameter here would let a
		// negative age from a mis-parsed provider response wrap to a huge value
		// and classify as Adult18.
		if (age < 0)
			throw std::invalid_argument("BandForAge: age must be non-negative");

		// Walk the bands from the oldest down so the first match wins. Written
		// as a loop over the tables rather than an if-chain so adding a band
		// cannot leave one entry point updated and another stale.
		// Descending walk over the four bands. The index doubles as the AgeBand
		// value because the enum's real members are 0..3 in this order, and the two
		// bound tables are indexed the same way. That coupling is what lets the
		// return below be a bare cast with no mapping table.
		//
		// Newest-to-oldest rather than oldest-first, so the first match wins with no
		// tie-break logic. With contiguous bounds either order would work, but
		// descending is what makes 'first match' the accurate description.
		for (int i = 3; i >= 0; --i)
		{
			// Hoisted out of the condition so the -1 sentinel is read once. Not a
			// micro-optimisation worth much, but it keeps the next line about the
			// comparison rather than about fetching the bound.
			const int upper = kBandUpperBound[i];
			// Both bounds are tested because the bands are stored as ranges, not just
			// starts. The upper half is short-circuited by the `upper < 0` test, which
			// is how Adult18's unbounded sentinel is handled without a special case.
			//
			// Both comparisons are inclusive, so the tables are genuinely contiguous:
			// 12 lands in Under13, 13 in Age13To15, 15 in Age13To15, 16 in Age16To17.
			// There is no gap for an age to fall through, which is what makes the throw
			// below unreachable.
			if (age >= kBandLowerBound[i] && (upper < 0 || age <= upper))
				// The cast is safe precisely because of the index-to-enum correspondence
				// described above: i runs 0..3 and Under13..Adult18 are 0..3. Unknown (-1)
				// is never produced here -- it is not a possible outcome for a known age,
				// which is the point of the enum's design.
				//
				// static_cast rather than a C-style cast, so a narrowing or wrong-direction
				// error would be a compile error instead of a silent conversion.
				return static_cast<AgeBand>(i);
		}

		// Unreachable given the range check above and a table that covers 0+.
		throw std::logic_error("BandForAge: no band matched a valid age");
	}

	// The band overload -- the one studios actually call, since an assurance
	// provider returns a band rather than an age. AgeBand.h's comment on the
	// declaration notes the two overloads must agree for every age, and
	// test_age_band.cpp checks that across 0..120.
	bool MeetsThreshold(AgeBand band, int threshold)
	{
		// Reused as the index into kBandSatisfiesThreshold below. Note this casts
		// AgeBand::Unknown (-1) to -1 without complaint -- the bounds check on the
		// next line is what turns that into a safe false rather than an
		// out-of-bounds read.
		const int index = static_cast<int>(band);
		// Range check before indexing. Both ends matter: Unknown is -1, and a value
		// cast in from persisted data could exceed the top.
		//
		// DEFECT: the upper bound is hardcoded as 3, duplicating knowledge that
		// lives in the enum definition and in the tables' sizes. Adding a fifth band
		// would silently exclude it here rather than fail to compile. Comparing
		// against the table's element count, or a named kBandCount constant, would
		// make the two places update together.
		if (index < 0 || index > 3)
			// Fail-closed on an out-of-range band, which is the correct default here:
			// Unknown must never unlock an adult-only path, and AgeBand.h is explicit
			// that it is 'NEVER treat as adult'.
			//
			// Worth noting the asymmetry against BandForAge: invalid input here returns
			// false quietly, while an invalid AGE there throws. Both are fail-closed, but
			// a caller cannot react to one of them.
			return false;

		// Reads kBandSatisfiesThreshold, NOT kBandUpperBound. The two differ at
		// the top band: Adult18 has no upper bound (-1) yet satisfies every
		// threshold. Using the lookup bound here reported verified adults as
		// failing the adult check. See the table's comment in AgeBand.h.
		//
		// Under13's entry is 0, which satisfies no threshold this module tests
		// against (all are 13 or higher), so an under-13 user can never unlock
		// an adult-only behaviour.
		// The single line the two-table design exists to serve. It reads the
		// threshold table, NOT the upper-bound table -- see the comment directly
		// above and the long one on kBandSatisfiesThreshold in AgeBand.h for why
		// conflating them reported verified adults as failing the adult check.
		//
		// The comparison is >=, so threshold = 0 succeeds for every band including
		// Under13, whose entry is 0. Only thresholds of 13 or above lock anything,
		// which is why the under-13 entry can never unlock an adult-only
		// behaviour.
		return kBandSatisfiesThreshold[index] >= threshold;
	}

	// The age overload: convert, then delegate. One line, because the band
	// overload already holds the logic.
	//
	// The delegation propagates the failure mode -- an age below 0 throws
	// std::invalid_argument out of BandForAge rather than returning false, so
	// this function throws despite its bool return type.
	bool MeetsThreshold(int age, int threshold)
	{
		// The call resolves to the AgeBand overload, not to itself: the inner call
		// returns an AgeBand, which the int overload's first parameter cannot match
		// without a conversion, so overload resolution has only one candidate. No
		// infinite recursion, but the two-overload trick does rely on the argument
		// types to select correctly.
		return MeetsThreshold(BandForAge(age), threshold);
	}

	// Human-readable label for UI refusals and compliance evidence. Returns a
	// reference to a static, so it is cheap and the string outlives the call --
	// the caller cannot free it, and cannot mutate it either since it is const.
	const std::string &BandName(AgeBand band)
	{
		// Function-local statics so this stays header-light and needs no
		// separate translation unit for the tables above.
		// Five function-local statics rather than namespace-scope constants, for the
		// reason given just below the switch: keeping them here means AgeBand.h need
		// not declare them, so a translation unit that only wants the enum and the
		// tables does not drag in <string>.
		//
		// Local statics are initialised on first pass through the function rather
		// than at static-init time, so there is no static-initialisation-order
		// problem; their destructors run at exit, so no leak is reported either.
		static const std::string kUnknown = "Unknown";
		static const std::string kUnder13 = "Under 13";
		static const std::string kAge13To15 = "13-15";
		static const std::string kAge16To17 = "16-17";
		static const std::string kAdult18 = "18+";

		// Exhaustive over all five enumerators, with Unknown handled as an explicit
		// break rather than a return. That is the one case not returned here, so
		// Unknown falls through to the kUnknown return below -- a deliberate shape
		// rather than a missing case.
		switch (band)
		{
		case AgeBand::Under13:
			return kUnder13;
		case AgeBand::Age13To15:
			return kAge13To15;
		case AgeBand::Age16To17:
			return kAge16To17;
		case AgeBand::Adult18:
			return kAdult18;
		// Falls out of the switch instead of returning a name directly, so Unknown
		// and any other unexpected value are handled by the same return.
		case AgeBand::Unknown:
			break;
		}

		// The single return covering both Unknown and any out-of-range value cast
		// in from persisted data. The guarantee it provides, restated in AgeBand.h's
		// declaration comment, is that the returned string is never empty -- which is
		// what lets a UI label be built from it with no fallback branch.
		//
		// This is the only one of the three module entry points that degrades
		// instead of throwing, and it is the right call: a label has no correct
		// value to report an error through.
		// Any value cast in from persisted data lands here rather than
		// returning a dangling reference or an empty string into a UI label.
		// Never returns a dangling reference or an empty string -- the reason these
		// five are statics rather than function-scope temporaries, which would dangle
		// the instant this function returned.
		return kUnknown;
	}
// Trailing comment names the namespace being closed, which a bare brace
// cannot convey.
} // namespace Wasteland::Age