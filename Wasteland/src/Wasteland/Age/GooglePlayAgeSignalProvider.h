// Wasteland::Age -- Google Play Age Signals reference provider.
//
// Play Age Signals lets an app ask Google Play for the user's age category --
// never a date of birth. It exists to satisfy the US App Store Accountability
// Acts: the platform store performs the verification and shares the result
// with the developer.
//
// Google states the API only returns data for users in regions where Play is
// required by law to provide age category data, and that Play updates cached
// signals within 2-8 weeks of a user's birthday.
//
// WHY THE MAPPING IS SEPARATED FROM THE PLATFORM CALL
//
// The real API is reached through the Play Core library on Android. The engine
// has no Android backend -- Wasteland/Core/Core.h rejects one with an #error
// -- so the platform call cannot be compiled or run in this repository. The
// mapping is factored out as pure functions and tested on the desktop
// (tests/test_google_provider.cpp); only the wrapper is unwritten.
//
// THE TWO THINGS THAT MAKE THIS DIFFERENT FROM THE APPLE PROVIDER
//
// 1. CUSTOM BANDS.
//
//    Apple's gates are chosen at request time, and this engine calls the
//    mapping with whatever it asked for. Play is different: an app nominates
//    up to three minimum ages in the PLAY CONSOLE, and those bands then
//    override Play's defaults for that app permanently. A studio can
//    therefore have a "15 or over" top band that Play reports as an
//    open-ended range.
//
//    So the band must be DERIVED from the (ageLower, ageUpper) pair rather
//    than matched against fixed bands. Hardcoding 18+ would turn every
//    15-year-old in a studio that nominated 15 as its minimum age into a
//    verified adult, in any jurisdiction, with no downstream check able to
//    tell it apart from a real one.
//
// 2. FOUR ASSURANCE TIERS.
//
//    Apple splits on who supplied the band. Play also reports how hard it
//    worked: TIER_A is self-declared, TIER_B is guardian-managed, TIER_C and
//    TIER_D involve government ID and selfies.
//
//    TIER_A maps to SelfDeclared and unlocks nothing -- see the .cpp.
//
// Neither instrument is in force as of this writing. See
// docs/AGE_ASSURANCE.md.

#pragma once

#include <string>

#include "Wasteland/Age/Attestation.h"
#include "Wasteland/Age/IAgeAssuranceProvider.h"

namespace Wasteland::Age
{

// Passed as `ageLower` when Play reports no usable lower bound. Per Google's
// documentation both bounds are null unless the status is SHARED, so this
// means "no signal" rather than "age 0".
inline constexpr int GoogleNoLowerBound = -1;

// Passed as `ageUpper` when Play reports a null upper bound, which is how it
// expresses the highest, open-ended band (e.g. "18 or over").
inline constexpr int GoogleNoUpperBound = -1;

// Mirrors Play's `ageSignalsStatus`.
enum class GoogleAgeSignalsStatus : int
{
	// The user, or a parent, declined to share. Nothing was returned.
	NotShared = 0,

	// The user is in a region where verification is mandatory and has not
	// completed it yet. Google is explicit that these users may be above OR
	// below 18.
	//
	// The remedy is to send the user to the Play Store to resolve their
	// status, NOT to re-prompt in-app. The status is exposed so a studio can
	// say the right thing; the attestation is absent either way.
	VerificationRequired = 1,

	// A range is available.
	Shared = 2,

	// The platform could not determine the status.
	Undetermined = 3,
};

// Mirrors Play's `ageRangeSource`.
enum class GoogleAgeRangeSource : int
{
	// The user self-declared their age. NOT an assurance method.
	TierASelfDeclared = 0,

	// The user's age is managed by a parent or guardian.
	TierBGuardian = 1,

	// Assessed using a credit card, email address, selfie assessment,
	// government ID or tax ID.
	TierCAssessment = 2,

	// Checked using government ID combined with a selfie assessment, or a
	// Digital ID.
	TierDGovernmentId = 3,
};

// Maps Play's inclusive (ageLower, ageUpper) range to a normalized band.
//
// Fails closed. A null lower bound, a negative lower bound, or an inverted
// range all yield Under13.
//
// Reads the LOWER bound, deliberately -- see the .cpp. A consequence worth
// knowing: a custom console range of [0,14] maps to Under13, so a 14-year-old
// is treated as under 13. That is more restrictive than necessary. It is the
// correct direction, and it is pinned by a test so it cannot drift silently.
AgeBand BandFromGoogleRange(int lowerBound, int upperBound);

// The assurance level a tier supports.
//
// TIER_A maps to SelfDeclared and unlocks nothing: the user typed a number.
// Art 3(i) of the EU KIDS Act proposal (COM(2026) 681) excludes
// self-declaration from "age assurance" entirely, and a fourteen-year-old who
// types 18 produces bytes identical to an adult's.
//
// TIER_B maps to OsSignal: a parent or guardian vouched.
//
// TIER_C and TIER_D ALSO map to OsSignal, not Commercial. Google performs
// genuine identity work -- government ID, selfies, Digital ID -- but the
// developer's integration is still "ask Google Play". This engine rates the
// integration, not the platform's internal rigour. Claiming Commercial here
// would let a studio assert a third-party assurance it never procured.
//
// A studio that wants Commercial wires Yoti or Persona directly and
// implements IAgeAssuranceProvider itself.
//
// An unrecognised tier maps to SelfDeclared -- the only tier that unlocks
// nothing -- because the engine cannot assume an unknown tier is the verified
// case.
AssuranceLevel AssuranceLevelForGoogleSource(GoogleAgeRangeSource source);

// Builds the full attestation from Play's four pieces of information.
//
// Any status other than Shared yields Unknown regardless of the bounds: no
// range was available, so none may be inferred. Use the status enum directly
// when you need to distinguish "declined" from "verification required" for
// the message shown to the player.
Attestation AttestationFromGoogle(int lowerBound, int upperBound, GoogleAgeRangeSource source,
                                  GoogleAgeSignalsStatus status);

// Reference implementation of IAgeAssuranceProvider.
//
// Off Android this reports IsAvailable() == false and returns
// Attestation::Absent(), which AgeService treats as "no provider" and which
// resolves to the restrictive SafetyProfile. Correct for a desktop build:
// there is no age assurance available, and pretending otherwise would be the
// failure this module exists to prevent.
class GooglePlayAgeSignalProvider final : public IAgeAssuranceProvider
{
public:
	bool IsAvailable() const override;
	Attestation Query() override;
	const std::string &ProviderName() const override;

private:
	// Returns the live signal Play reports, or false when the platform call
	// cannot be made. Unused off Android and therefore untested; when Android
	// support lands this is the seam an integration test must cover, and
	// until then it must not be reachable from any desktop code path.
	bool QueryPlatformSignal(int &outLowerBound, int &outUpperBound, GoogleAgeRangeSource &outSource,
	                         GoogleAgeSignalsStatus &outStatus);

	const std::string m_Name = "google-play-age-signals";
};

} // namespace Wasteland::Age