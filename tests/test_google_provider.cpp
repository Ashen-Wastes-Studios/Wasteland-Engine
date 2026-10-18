// Standalone functional test for Wasteland::Age::GooglePlayAgeSignalProvider's
// mapping logic.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE PROVIDER
//
// The Play Age Signals API is reached through the Play Core library on
// Android. The Wasteland engine has no Android backend -- Wasteland/Core/
// Core.h rejects one with an #error -- so the platform call can never be
// compiled or executed here. The mapping is therefore factored out as pure
// functions and tested on the desktop; only the thin wrapper is unwritten.
//
// WHAT PLAY ACTUALLY RETURNS (verified against Google's documentation, not
// recalled)
//
//   * `ageSignalsStatus` is one of:
//       - SHARED                -- a range is available
//       - NOT_SHARED            -- the user (or parent) declined
//       - VERIFICATION_REQUIRED -- the user is in a region where verification
//                                  is mandatory and has not been done yet.
//                                  In applicable US states this is the state,
//                                  and the remedy is to send the user to the
//                                  Play Store to resolve it -- NOT to keep
//                                  asking in-app. The docs are explicit that
//                                  these users may be above OR below 18.
//   * `ageLower` (0-18) and `ageUpper` (2-18). `ageUpper` is null for the
//     highest band. Both are null when the status is not SHARED.
//   * `ageRangeSource` is one of:
//       - TIER_A  the user self-declared
//       - TIER_B  managed by a parent or guardian
//       - TIER_C  assessed via credit card, email address, selfie, government
//                  ID or tax ID
//       - TIER_D  government ID combined with a selfie, or a Digital ID
//   * The default bands are 0-12, 13-15, 16-17, 18+ -- BUT the app can
//     nominate up to three custom minimum ages in the Play Console, at least
//     two years apart, and those override the defaults.
//
// THE TWO THINGS THAT MAKE THIS DIFFERENT FROM APPLE
//
// 1. CUSTOM BANDS. Because the app chooses its own gates, the engine cannot
//    switch on fixed bands -- it has to derive one from the (ageLower,
//    ageUpper) pair. A studio that nominates a single minimum age of 15 gets
//    "15 or over" back, which is NOT an adult. Hardcoding 18+ here would
//    treat every 15-year-old as an adult in any studio that misconfigured a
//    gate.
//
// 2. FOUR TIERS, NOT TWO. Apple splits on who supplied the band; Play also
//    tells you how hard it worked. TIER_A is self-declared and must unlock
//    nothing. TIER_B is guardian-managed. TIER_C and TIER_D involve
//    government ID and selfies, but the developer's integration is still "ask
//    Google Play", so this engine rates the INTEGRATION at OsSignal rather
//    than claiming Commercial. See the note in the .cpp.

#include <cstdio>

#include "Wasteland/Age/AgeBand.h"
#include "Wasteland/Age/GooglePlayAgeSignalProvider.h"

using namespace Wasteland::Age;

static int g_Failures = 0;
static int g_Checks = 0;

#define CHECK(cond, what)                                                        \
	do                                                                           \
	{                                                                            \
		++g_Checks;                                                              \
		if (!(cond))                                                             \
		{                                                                        \
			++g_Failures;                                                        \
			std::printf("  FAIL: %s\n", (what));                                  \
		}                                                                        \
	} while (0)

static void TestDefaultPlayRanges()
{
	// Google's documented default bands: 0-12, 13-15, 16-17, 18+.
	CHECK(BandFromGoogleRange(0, 12) == AgeBand::Under13, "Play [0,12] maps to Under13");
	CHECK(BandFromGoogleRange(13, 15) == AgeBand::Age13To15, "Play [13,15] maps to Age13To15");
	CHECK(BandFromGoogleRange(16, 17) == AgeBand::Age16To17, "Play [16,17] maps to Age16To17");
	CHECK(BandFromGoogleRange(18, GoogleNoUpperBound) == AgeBand::Adult18, "Play [18, or over] maps to Adult18");
}

static void TestCustomConsoleRanges()
{
	// From Google's documentation: with custom minimum ages of 13 and 17,
	// Play returns 0-12, 13-16, and 17+.
	CHECK(BandFromGoogleRange(0, 12) == AgeBand::Under13, "custom [0,12] maps to Under13");
	CHECK(BandFromGoogleRange(13, 16) == AgeBand::Age13To15, "custom [13,16] maps to Age13To15");
	// 17 or over is NOT 18 or over. A 17-year-old must not become an adult
	// because a studio chose a different gate than the default.
	CHECK(BandFromGoogleRange(17, GoogleNoUpperBound) == AgeBand::Age16To17,
	      "custom [17, or over] must map to Age16To17, never adult");

	// And the single-gate case from the docs: one minimum age of 15 yields
	// 0-14 and 15+.
	CHECK(BandFromGoogleRange(15, GoogleNoUpperBound) == AgeBand::Age13To15,
	      "custom [15, or over] must map to Age13To15 -- a 15-year-old is not a verified adult");
	// [0,14] read conservatively lands on Under13. A 14-year-old is treated
	// as under 13, which is MORE restrictive than necessary. That is the
	// correct direction for a conservative reading, and it is pinned here so
	// the asymmetry with the open-ended case is visible rather than
	// accidental.
	CHECK(BandFromGoogleRange(0, 14) == AgeBand::Under13,
	      "custom [0,14] maps conservatively to Under13 -- over-restrictive, which is the safe direction");
}

static void TestTierASelfDeclaredUnlocksNothing()
{
	// THE bug this mapping exists to prevent. TIER_A means the user typed a
	// number. An 18+ band that was self-declared is an unverified claim, and
	// a fourteen-year-old who typed 18 produces the same bytes as an adult who
	// did. Art 3(i) of the EU KIDS Act proposal (COM(2026) 681) excludes
	// self-declaration from "age assurance" outright.
	const Attestation att =
	    AttestationFromGoogle(18, GoogleNoUpperBound, GoogleAgeRangeSource::TierASelfDeclared, GoogleAgeSignalsStatus::Shared);

	CHECK(att.Band() == AgeBand::Adult18, "the band itself is Adult18 -- Play really did say 18 or over");
	CHECK(att.Level() == AssuranceLevel::SelfDeclared, "TIER_A must map to SelfDeclared");
	CHECK(att.IsMinor(), "a self-declared adult must still report as minor");
	CHECK(att.Method() == "google-play-age-signals", "the audit trail must name the method");
}

static void TestGuardianAndAssessedTiers()
{
	// TIER_B: managed by a parent or guardian -- somebody else vouched, the
	// same situation as Apple's guardian-declared path.
	CHECK(AssuranceLevelForGoogleSource(GoogleAgeRangeSource::TierBGuardian) == AssuranceLevel::OsSignal,
	      "TIER_B is a platform signal");

	// TIER_C and TIER_D involve government ID and selfies. They are stronger
	// internally, but the developer's integration is still "ask Google Play",
	// so the engine rates the integration, not Play's internal rigour. This is
	// asserted deliberately: a change to Commercial would let a studio claim a
	// third-party assurance it did not procure.
	CHECK(AssuranceLevelForGoogleSource(GoogleAgeRangeSource::TierCAssessment) == AssuranceLevel::OsSignal,
	      "TIER_C must rate the Play integration at OsSignal, not claim Commercial");
	CHECK(AssuranceLevelForGoogleSource(GoogleAgeRangeSource::TierDGovernmentId) == AssuranceLevel::OsSignal,
	      "TIER_D must rate the Play integration at OsSignal, not claim Commercial");

	// A guardian-managed 18+ is a verified adult.
	const Attestation guardian =
	    AttestationFromGoogle(18, GoogleNoUpperBound, GoogleAgeRangeSource::TierBGuardian, GoogleAgeSignalsStatus::Shared);
	CHECK(!guardian.IsMinor(), "a guardian-managed 18+ must not report as minor");

	const Attestation assessed =
	    AttestationFromGoogle(18, GoogleNoUpperBound, GoogleAgeRangeSource::TierCAssessment, GoogleAgeSignalsStatus::Shared);
	CHECK(!assessed.IsMinor(), "a TIER_C 18+ must not report as minor");
}

static void TestUnknownTierFailsClosed()
{
	// A newer Play SDK, or corrupt state. TIER_A is the only tier that
	// unlocks nothing, so an unrecognised tier must be treated as if it
	// might be TIER_A.
	for (GoogleAgeRangeSource tier :
	     {static_cast<GoogleAgeRangeSource>(77), static_cast<GoogleAgeRangeSource>(-3)})
	{
		CHECK(AssuranceLevelForGoogleSource(tier) == AssuranceLevel::SelfDeclared,
		      "an unrecognised tier must fail closed to SelfDeclared");

		const Attestation att = AttestationFromGoogle(18, GoogleNoUpperBound, tier, GoogleAgeSignalsStatus::Shared);
		CHECK(att.IsMinor(), "an unrecognised tier must not unlock an adult");
	}
}

static void TestUnsharedAndVerificationRequired()
{
	// Both mean there is no usable signal, so both are fail-closed. The
	// distinction is preserved in the status enum so a studio can show the
	// right remedy -- for VERIFICATION_REQUIRED, Google says to send the user
	// to the Play Store rather than re-prompting in-app.
	for (GoogleAgeSignalsStatus status : {GoogleAgeSignalsStatus::NotShared, GoogleAgeSignalsStatus::VerificationRequired})
	{
		const Attestation att =
		    AttestationFromGoogle(GoogleNoLowerBound, GoogleNoUpperBound, GoogleAgeRangeSource::TierDGovernmentId, status);
		CHECK(att.Band() == AgeBand::Unknown, "a non-shared status must yield an Unknown band");
		CHECK(att.IsMinor(), "a non-shared status must report as minor");
	}
}

static void TestUndeterminedStatusFailsClosed()
{
	const Attestation att = AttestationFromGoogle(
	    18, GoogleNoUpperBound, GoogleAgeRangeSource::TierDGovernmentId, GoogleAgeSignalsStatus::Undetermined);
	CHECK(att.Band() == AgeBand::Unknown, "an undetermined status must yield an Unknown band");
	CHECK(att.IsMinor(), "an undetermined status must report as minor");

	const GoogleAgeSignalsStatus garbage = static_cast<GoogleAgeSignalsStatus>(99);
	const Attestation bad =
	    AttestationFromGoogle(18, GoogleNoUpperBound, GoogleAgeRangeSource::TierDGovernmentId, garbage);
	CHECK(bad.IsMinor(), "an unrecognised status must fail closed");
}

static void TestMalformedRangesFailClosed()
{
	CHECK(BandFromGoogleRange(-1, 12) == AgeBand::Under13, "a negative lower bound must fail closed to Under13");
	CHECK(BandFromGoogleRange(13, 5) == AgeBand::Under13, "an inverted range must fail closed to Under13");
	CHECK(BandFromGoogleRange(GoogleNoLowerBound, 15) == AgeBand::Under13,
	      "a null lower bound with a real upper bound is malformed and must fail closed");
}

static void TestProviderIsUnavailableOnDesktop()
{
	// The engine has no Android backend, so the provider must admit it cannot
	// work here rather than pretending. AgeService treats an unavailable
	// provider as no provider at all, which yields Absent() and the
	// restrictive SafetyProfile -- correct for a desktop build.
	GooglePlayAgeSignalProvider provider;

	CHECK(!provider.IsAvailable(), "the Google Play provider must report unavailable off Android");
	CHECK(provider.Query().IsMinor(), "an unavailable provider must return a fail-closed attestation");
	CHECK(provider.Query().Band() == AgeBand::Unknown, "and an Unknown band");
	CHECK(provider.ProviderName() == "google-play-age-signals", "the provider name must be stable for the audit trail");
}

int main()
{
	TestDefaultPlayRanges();
	TestCustomConsoleRanges();
	TestTierASelfDeclaredUnlocksNothing();
	TestGuardianAndAssessedTiers();
	TestUnknownTierFailsClosed();
	TestUnsharedAndVerificationRequired();
	TestUndeterminedStatusFailsClosed();
	TestMalformedRangesFailClosed();
	TestProviderIsUnavailableOnDesktop();

	std::printf("test_google_provider: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? 1 : 0;
}