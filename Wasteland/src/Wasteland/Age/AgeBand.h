#pragma once

#include <string>

namespace Wasteland::Age
{
	// Normalized age bands shared by the US App Store Accountability Acts, the
	// Apple Declared Age Range / Google Play Age Signals / Amazon
	// GetUserAgeData APIs, the EU KIDS Act proposal (COM(2026) 681,
	// 2026/0286(COD)), and KOSA.
	//
	// The bands deliberately do NOT align across those regimes. The US scheme
	// reports 0-12 / 13-15 / 16-17 / 18+; the EU proposal splits 13-15 into
	// 13-14 (guardian-supervised) and 15 (autonomous social accounts).
	// Reconciling them here would bake one jurisdiction's rule into shared
	// storage and produce an off-by-one that is a compliance defect in
	// another. Store the band; interpret it in JurisdictionPolicy.
	//
	// Unknown is a first-class member, not a sentinel for "adult". It means no
	// age signal was received, and every policy layer in this module treats it
	// as fail-closed -- restricted defaults, no derogation from the Art 8(1)
	// safety-by-design obligations. Defaulting an absent signal to Adult18 is
	// the specific failure both the EU and US instruments exist to prevent,
	// so the type gives it no way to happen by accident.
	enum class AgeBand : int
	{
		Unknown = -1, // No signal received. NEVER treat as adult.
		Under13 = 0,
		Age13To15 = 1,
		Age16To17 = 2,
		Adult18 = 3
	};

	// Upper bound for a band that has none. Used in lookup tables; not an age.
	inline constexpr int kNoUpperBound = -1;

	// Stands in for "satisfies every threshold" when a band has no upper age
	// limit. Any real threshold in this module is far below it.
	inline constexpr int kMaxAgeThreshold = 1 << 30;

	// Inclusive lower bound of each band.
	inline constexpr int kBandLowerBound[] = {0, 13, 16, 18};

	// Inclusive upper bound of each band, for age->band lookup.
	// -1 means unbounded (no upper bound), which is the case for Adult18.
	inline constexpr int kBandUpperBound[] = {12, 15, 17, -1};

	// Highest age threshold each band satisfies.
	//
	// This is deliberately a SEPARATE table from kBandUpperBound. The two
	// answer different questions -- "which band does this age fall in" versus
	// "how old does this band have to be to pass this check" -- and they
	// disagree at the top: Adult18 is unbounded above (so its lookup bound is
	// -1) yet satisfies every threshold up to infinity. Deriving one from the
	// other made MeetsThreshold(Adult18, 18) compare -1 >= 18 and report a
	// verified adult as failing the adult check, which would gate every minor
	// out of their own game. Do not merge these tables.
	inline constexpr int kBandSatisfiesThreshold[] = {0, 15, 17, kMaxAgeThreshold};

	// Maps a concrete age to its normalized band.
	//
	// Takes int rather than unsigned on purpose: an unsigned parameter turns a
	// -1 from a mis-parsed provider response into a huge age, which classifies
	// as Adult18. That is the worst possible outcome for this function, so the
	// type refuses it and a negative age throws instead.
	AgeBand BandForAge(int age);

	// Does this age satisfy the given threshold (13, 15, 16, or 18)?
	bool MeetsThreshold(int age, int threshold);

	// Does this band satisfy the given threshold? This is the overload studios
	// actually call, since an assurance provider returns a band rather than an
	// age. The two overloads must agree for every age -- test_age_band.cpp
	// checks that across 0..120, because a disagreement here means the engine's
	// answer depends on which call site a given studio happened to use.
	bool MeetsThreshold(AgeBand band, int threshold);

	// Human-readable band label, for user-facing refusals and compliance
	// evidence. Never empty, including for Unknown.
	const std::string &BandName(AgeBand band);
}