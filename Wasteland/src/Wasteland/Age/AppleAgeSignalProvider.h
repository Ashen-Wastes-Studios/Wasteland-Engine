// Wasteland::Age -- Apple Declared Age Range reference provider.
//
// Apple Declared Age Range lets an app ask the OS how old somebody is
// WITHOUT being told their date of birth. The person nominates up to three
// "age gates" -- minimum ages that matter to the product -- and the system
// answers with the band they fall into.
//
// It requires the `com.developer.declared-age-range` entitlement, the
// Declared Age Range capability on the app target in Xcode, and a user
// interaction to present the prompt.
//
// WHY THE MAPPING IS SEPARATED FROM THE PLATFORM CALL
//
// The real API is Swift/Objective-C inside AuthenticationServices and only
// exists on Apple platforms. The Wasteland engine has no iOS backend --
// Wasteland/Core/Core.h rejects one with an #error -- so the platform call
// below can never be compiled or run in this repository today.
//
// Rather than leave the rules untested, the part that decides what Apple's
// numbers MEAN to this engine is factored out as pure functions and tested
// on the desktop (tests/test_apple_provider.cpp, 40+ checks). When iOS
// support lands, only the thin wrapper is new code; the rules it calls are
// already proven.
//
// THE TRAP: A BAND IS NOT ASSURANCE
//
// Apple reports the range AND how it was established:
//
//   .guardianDeclared -- a parent, guardian or Family Organiser supplied it.
//                        Children are ALWAYS in this category.
//   .selfDeclared     -- the person supplied it themselves. Teens outside an
//                        iCloud Family group may be here; so are adults.
//
// An 18+ band that is .selfDeclared is an unverified claim of adulthood. A
// fourteen-year-old who types 18 produces exactly the same bytes as an adult
// who types 18. Nothing in the range distinguishes them.
//
// So the declaration decides the AssuranceLevel, and Attestation::IsMinor()
// refuses a self-declared adult. This is the single most important behaviour
// in the file, and it is pinned by tests. Art 3(i) of the EU KIDS Act
// proposal (COM(2026) 681) excludes self-declaration from "age assurance"
// entirely, which is the same rule arrived at from the legal side.
//
// THE OTHER TRAP: "16 OR OVER" IS NOT "ADULT"
//
// Apple returns an optional upperBound; nil means "at or above the highest
// gate". That is a LOWER BOUND, not a band. "13 or over" is consistent with
// being thirteen.
//
// This engine reads the lower bound, which yields the most restrictive band
// consistent with what Apple said. Reading "13 or over" as adult would hand
// every teenager in it a full adult feature set.
//
// Neither instrument is in force as of this writing. See
// docs/AGE_ASSURANCE.md.

#pragma once

#include <string>

#include "Wasteland/Age/Attestation.h"
#include "Wasteland/Age/IAgeAssuranceProvider.h"

namespace Wasteland::Age
{

// Passed as `lowerBound` when Apple returns a nil lowerBound, meaning the
// person sits below every gate the app nominated.
inline constexpr int AppleNoLowerBound = -1;

// Passed as `upperBound` when Apple returns a nil upperBound, meaning "at or
// above the highest gate" -- e.g. "16 or over".
inline constexpr int AppleNoUpperBound = -1;

// How the range was established. Mirrors Apple's
// AgeRangeService.AgeRangeDeclaration.
enum class AppleAgeDeclaration : int
{
	// The person entered it. NOT an assurance method.
	SelfDeclared = 0,

	// A parent, guardian or Family Organiser entered it. Children are always
	// in this category.
	GuardianDeclared = 1,

	// Family Sharing is set to "never share" for this person. Nothing was
	// returned; the prompt is not even shown.
	NeverShared = 2,

	// The platform could not determine how the band was established.
	Undetermined = 3,
};

// Maps Apple's inclusive (lowerBound, upperBound) range to a normalized band.
//
// Fails closed. AppleNoLowerBound, a negative lower bound, or an inverted
// range all yield Under13 -- the most restrictive answer available -- rather
// than trusting malformed input.
//
// Reads the LOWER bound. See the header note on "16 or over".
AgeBand BandFromAppleRange(int lowerBound, int upperBound);

// The assurance level a declaration supports.
//
// A guardian declaration is somebody else vouching for a child, which is a
// real assurance method. Everything else -- including self-declared, never
// shared, undetermined, and any unrecognised value -- maps to
// AssuranceLevel::SelfDeclared, which unlocks nothing.
AssuranceLevel LevelForAppleDeclaration(AppleAgeDeclaration declaration);

// Builds the full attestation from Apple's three pieces of information.
//
// NeverShares and Undetermined yield Unknown regardless of the bounds: no
// band was returned, so none may be inferred.
Attestation AttestationFromApple(int lowerBound, int upperBound, AppleAgeDeclaration declaration);

// Reference implementation of IAgeAssuranceProvider.
//
// Off Apple platforms this reports IsAvailable() == false and returns
// Attestation::Absent(), which AgeService treats as "no provider" and which
// resolves to the restrictive SafetyProfile. That is the correct behaviour
// for a desktop build: there is no age assurance available, and pretending
// otherwise would be the failure this module exists to prevent.
class AppleAgeSignalProvider final : public IAgeAssuranceProvider
{
public:
	bool IsAvailable() const override;
	Attestation Query() override;
	const std::string &ProviderName() const override;

private:
	// Returns the live range Apple reports, or false when the platform call
	// cannot be made. Unused off Apple platforms, and therefore untested --
	// see the header note. When iOS support lands this becomes the seam that
	// an integration test must cover, and until then it must not be
	// reachable from any code path a desktop build can take.
	bool QueryPlatformRange(int &outLowerBound, int &outUpperBound, AppleAgeDeclaration &outDeclaration);

	const std::string m_Name = "apple-declared-age-range";
};

} // namespace Wasteland::Age