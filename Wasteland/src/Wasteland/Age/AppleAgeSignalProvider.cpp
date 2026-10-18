// The PCH first, which is the engine's convention: it establishes <string>,
// <cstdint> and the platform macros (WL_PLATFORM_IOS below) before any other
// header is parsed.
//
// Load-bearing here specifically, because the WL_PLATFORM_IOS tests in this file
// need Core.h's platform detection to have run first.
#include "wlpch.h"

// The header, which brings AgeBand, AssuranceLevel, Attestation and the
// AppleNoLowerBound / AppleNoUpperBound sentinels this file maps onto.
//
// The two free functions above the platform wrapper (BandFromAppleRange,
// LevelForAppleDeclaration, AttestationFromApple) are declared here rather than
// being file-static, which is what lets the desktop tests call them directly.
#include "Wasteland/Age/AppleAgeSignalProvider.h"

// The module's namespace, matching AgeBand.h and GuardianPolicy.h.
//
// So the whole file is Wasteland::Age, and every bare AgeBand / Attestation below
// resolves without qualification.
namespace Wasteland::Age
{

// Pure and free -- no member, no state, no platform guard. That is deliberate and
// is the pre-existing comment's main instruction: "Do not move the mapping down
// here".
//
// The reason given is testability. WL_PLATFORM_IOS is never defined in this
// repository, so the platform block below compiles to the unavailable path; if
// this mapping moved behind that guard it would stop being testable on desktop
// at all -- and these two rules (how a lower bound reads, how a self-declaration
// is treated) are exactly the ones that decide whether a minor is protected.
AgeBand BandFromAppleRange(const int lowerBound, const int upperBound)
{
	// A nil lowerBound means the person is below every gate the app
	// nominated, so Under13 is both correct and maximally restrictive.
	// The nil-lower-bound case, checked FIRST.
	//
	// Order matters: a nil lower bound is Apple saying "below every gate the app
	// nominated", so Under13 is both correct and maximally restrictive. Checking the
	// inversion guard first would let a nil/absent pair be classified as malformed
	// rather than as the under-13 answer it actually is.
	if (lowerBound < 0)
		return AgeBand::Under13;

	// An inverted range is malformed input -- it cannot come from Apple, so
	// it is either a bug in a studio's integration or corrupt state. Either
	// way the answer is the most restrictive one.
	// The malformed-range guard, and both halves are needed.
	//
	//   upperBound >= 0  -- skips the case where the upper bound is ALSO absent, which
	//                      is Apple's "at or above the highest gate" and is a
	//                      perfectly valid open-ended range, not an inversion
	//   upperBound < lowerBound -- the actual inversion test
	//
	// DEFECT (dead guard on this platform, kept honest by the comment): because
	// WL_PLATFORM_IOS is never defined, no code path in this repository can currently
	// produce an inverted range. The guard is unreachable today and exists for the
	// future Objective-C++ shim described below. Worth keeping -- the comment above
	// it explains that inverted input is either an integration bug or corrupt state.
	if (upperBound >= 0 && upperBound < lowerBound)
		return AgeBand::Under13;

	// Read the LOWER bound, deliberately.
	//
	// Apple returns an optional upperBound, and a nil one means "at or above
	// the highest gate". That is a lower bound, not a band: "13 or over" is
	// perfectly consistent with being thirteen. Reading the lower bound gives
	// the most restrictive band consistent with what Apple actually said, and
	// it is the same answer for closed and open-ended ranges -- (13,15) and
	// (13, or over) both land on Age13To15, which is correct, because in both
	// cases the person may be 13.
	//
	// Reading the open-ended case as adult instead would give every teenager
	// in the "16 or over" bucket a full adult feature set, and nothing
	// downstream would catch it, because the band would then be correct.
	// The LOWER bound is what gets read, deliberately, and the pre-existing comment
	// above it is the longest argument in the file.
	//
	// The reasoning: Apple's upperBound is optional, and a nil one means "at or above
	// the highest gate". That is a lower bound, not a band -- "13 or over" is
	// perfectly consistent with being thirteen. So (13,15) and (13, or over) both
	// land on Age13To15, and both are correct.
	//
	// DEFECT-adjacent (a real hazard, documented not fixed): reading the open-ended
	// case as adult would give every teenager in the "16 or over" bucket a full
	// adult feature set, and NOTHING downstream would catch it, because the band
	// would then be correct. BandForAge itself throws on a negative, which the
	// lowerBound < 0 guard above already excluded.
	return BandForAge(lowerBound);
}

// Declaration -> assurance level, and the mapping is deliberately near-total:
// everything except GuardianDeclared is SelfDeclared.
//
// That collapse is the point, not laziness. Art 3(i) of the EU KIDS Act proposal
// excludes self-declaration from "age assurance", so SelfDeclared must unlock
// nothing even when the band reads 18+. A finer mapping would be inventing
AssuranceLevel LevelForAppleDeclaration(const AppleAgeDeclaration declaration)
{
	// Only a guardian declaration is an assurance method. A guardian,
	// parent or Family Organiser supplied this band, which is somebody else
	// vouching for the child -- and per Apple, children are ALWAYS in this
	// category, so this is the normal and correct path for a minor.
	// The ONLY case that yields an OS signal.
	//
	// A guardian, parent or Family Organiser supplied the band -- somebody else
	// vouching for the child. Per Apple, children are ALWAYS in this category, so
	// this is the normal and correct path for a minor, not an edge case.
	if (declaration == AppleAgeDeclaration::GuardianDeclared)
		return AssuranceLevel::OsSignal;

	// Everything else is SelfDeclared, including:
	//   - SelfDeclared: the person typed a number. Art 3(i) of the EU KIDS
	//     Act proposal excludes self-declaration from "age assurance", so
	//     this unlocks nothing even when the band reads 18+.
	//   - NeverShared / Undetermined: nothing was established.
	//   - any value this engine does not recognise: a newer SDK, or garbage.
	//     Fail closed -- an unrecognised declaration cannot be assumed to be
	//     the one case that is actually verified.
	// The catch-all, and the pre-existing comment enumerates the three things it
	// absorbs: SelfDeclared (a typed number, excluded from age assurance by
	// Art 3(i)), NeverShared and Undetermined (nothing established), and ANY value
	// this engine does not recognise.
	//
	// The last is the important one. A newer SDK, or garbage, lands here too -- and
	// fail-closed is correct, because an unrecognised declaration cannot be assumed
	// to be the one case that is actually verified.
	//
	// So this is the same fail-closed pattern as AgeBand's Unknown: neither is a
	// synonym for adult.
	return AssuranceLevel::SelfDeclared;
}

// The composition of the two functions above, and it is a FREE function for the
// same reason they are: it must stay testable off Apple platforms.
//
// All three parameters BY VALUE -- the enum and two ints -- which is right, since
// they are scalars and the declaration is a small enum. Compare AttestationFromApple's
// siblings, which take structs by const reference.
Attestation AttestationFromApple(const int lowerBound, const int upperBound, const AppleAgeDeclaration declaration)
{
	// "Never share" and "undetermined" mean Apple returned no band at all.
	// Inferring one from the bounds would be inventing data, so the band stays
	// Unknown and the attestation reports the player as a minor.
	// The no-band case, checked before any bound is read.
	//
	// The pre-existing comment's reasoning: "Never share" and "undetermined" mean
	// Apple returned no band at all, so inferring one from the bounds would be
	// INVENTING data. Attestation::Absent() leaves the band Unknown and the player
	// a minor.
	//
	// Order is load-bearing: this runs before BandFromAppleRange, so a "never shared"
	// response with plausible-looking bounds cannot produce a band.
	if (declaration == AppleAgeDeclaration::NeverShared || declaration == AppleAgeDeclaration::Undetermined)
		// A factory rather than a default-constructed Attestation, which matters because
		// it makes the absent case explicit at the call site and keeps the level/band/
		// provider triple from having to be hand-built as "nothing".
		//
		// AgeBand.h states the principle this implements: Unknown is a first-class member,
		// not a sentinel for adult, and every policy layer treats it as fail-closed.
		return Attestation::Absent();

	// The composition itself, and the argument order matters: level first, then band,
	// then the provider name as a string literal.
	//
	// The literal "apple-declared-age-range" is the provider identity recorded in the
	// attestation -- Apple's API name. It is a wire value, so changing it would break
	// stored attestations, and it is duplicated nowhere else in the file.
	return Attestation::FromProvider(LevelForAppleDeclaration(declaration),
	                                 BandFromAppleRange(lowerBound, upperBound),
	                                 "apple-declared-age-range");
}

// The pre-existing section banner, and the comment beneath it is the most
// important operational note in the file: everything below is INERT off Apple
// platforms.
// --- Platform wrapper -----------------------------------------------------
//
// Everything below is inert off Apple platforms. WL_PLATFORM_IOS is never
// defined in this repository -- Core.h rejects an iOS build outright -- so
// this translation unit compiles to the unavailable path and the desktop
// tests exercise the pure mapping above instead.
//
// Do not move the mapping down here. If the rules about how a lower bound
// reads, or how a self-declaration is treated, live behind a platform guard
// they stop being tested, and those two rules are the ones that decide whether
// a minor is protected.

// The provider's availability probe, and it is a compile-time constant rather
// than a runtime check.
//
// `const` and no state involved -- the answer depends only on which platform the
// file was compiled for, so it could equally have been a constexpr.
bool AppleAgeSignalProvider::IsAvailable() const
{
// The platform guard, and it wraps the whole decision rather than branching at
// runtime.
//
// Per the section banner: WL_PLATFORM_IOS is never defined in this repository --
// Core.h rejects an iOS build outright -- so BOTH branches below are known at
// compile time and this translation unit always compiles to the unavailable
// path on any build that exists today.
#if defined(WL_PLATFORM_IOS)
	// Available once the entitlement is present and the API initialises.
	// Deliberately optimistic about the entitlement: a missing entitlement
	// surfaces as a declined/absent response from Query(), which is fail
	// closed anyway, so failing here on a condition that cannot be queried
	// cheaply would only produce a false negative.
	// Optimistic about the entitlement, and the pre-existing comment justifies it:
	// a missing entitlement surfaces as a declined/absent response from Query(),
	// which is fail-closed anyway.
	//
	// So failing HERE on a condition that cannot be queried cheaply would produce a
	// false negative -- reporting "unavailable" when the API would in fact have
	// answered. Unreachable today, but the reasoning is what a future implementer
	// needs.
	//
	// The #else branch below is the same guard with `return false;`, and both
	// branches return the same value -- so on any build that exists today the
	// guard is not observable in behaviour. It is kept because the #else arm is the
	// one that actually compiles, and deleting it would leave the parameters of
	// QueryPlatformRange genuinely unused.
	return true;
#else
	return false;
#endif
}

// The raw platform query, and the shape is a three-out-parameter design that
// returns bool rather than an optional struct.
//
// DEFECT (inconsistent with the rest of the module): GuardianPolicy::Validate and
// Skeleton::BuildFromNames use the same out-parameter-plus-bool shape, so it IS
// the house style -- but AppleAgeSignalProvider::Query() below wraps this and its
// sibling AgeBand functions in an Attestation, so the inconsistency is between
// this layer and the one above it.
//
// The out-params are non-const references and are ONLY written on success; on
// failure they are left untouched, which is why every caller below initialises
// them first.
bool AppleAgeSignalProvider::QueryPlatformRange(int &outLowerBound, int &outUpperBound,
                                                AppleAgeDeclaration &outDeclaration)
{
#if defined(WL_PLATFORM_IOS)
	// Not implemented. The engine has no iOS backend, so this branch cannot
	// be compiled today and is unreachable from any build that exists.
	//
	// The intended implementation is an Objective-C++ shim over
	// AuthenticationServices:
	//
	//     [DeclaredAgeRangeService requestAgeRangeWithAgeGates:@[@13, @15, @18]
	//                                        completion:^(AgeRangeServiceResponse *r) {
	//         if (r.status != .sharing) { ... }
	//         outLowerBound    = r.range.lowerBound;   // NSNumber?, nil => below the lowest gate
	//         outUpperBound    = r.range.upperBound;   // NSNumber?, nil => "highest gate or over"
	//         outDeclaration  = (AppleAgeDeclaration)r.ageRangeDeclaration;
	//     }]
	//
	// Note that lowerBound/upperBound are NSNumber* and must be mapped to
	// AppleNoLowerBound / AppleNoUpperBound when nil -- passing 0 for a nil
	// lowerBound would silently turn "below every gate" into "age 0 or over",
	// which happens to map to Under13 but for the wrong reason, and passing 0
	// for a nil UPPER bound would turn "18 or over" into a closed 18..0 range
	// and fail closed to Under13, treating every adult as a child. That
	// asymmetry is why the sentinels exist and why the wrapper must be
	// reviewed carefully when it is written.
	(void)outLowerBound;
	(void)outUpperBound;
	(void)outDeclaration;
	// The iOS stub's return, and the pre-existing comment above the three (void) casts
	// is the spec for the future implementation: an Objective-C++ shim over
	// AuthenticationServices' DeclaredAgeRangeService.
	//
	// That comment also documents the trap in detail, and it is the reason this stub
	// is worth keeping: lowerBound/upperBound are NSNumber*, and passing 0 for a
	// NIL upper bound would turn "18 or over" into a closed 18..0 range and fail
	// closed to Under13 -- treating every adult as a child. Passing 0 for a nil
	// LOWER bound happens to land on Under13 too, but for the wrong reason. That
	// asymmetry is why the sentinels exist.
	//
	// The three (void) casts above and the identical three below are repeated PER
	// BRANCH on purpose: each preprocessor arm is compiled separately, so only the
	// surviving one sees the out-params as unused. That duplication is necessary, not
	// sloppy -- and it is also why both branches return false with no assignment.
	return false;
#else
	(void)outLowerBound;
	(void)outUpperBound;
	(void)outDeclaration;
	return false;
#endif
}

// The high-level query, and the only method a caller outside this class should
// use: it composes QueryPlatformRange and AttestationFromApple into one
// Attestation.
Attestation AppleAgeSignalProvider::Query()
{
	// The availability short-circuit, and it produces the same Attestation::Absent()
	// that a failed query would.
	//
	// So a caller cannot distinguish "this platform cannot ask" from "the platform
	// declined" by the result alone -- both are Absent, both fail closed. That is
	// correct behaviour and it is also a deliberate loss of diagnostic detail.
	if (!IsAvailable())
		return Attestation::Absent();

	// Initialising the out-params to the SENTINELS before the call, not to 0.
	//
	// This is load-bearing, and the reason is the asymmetry the stub's comment spells
	// out: a nil lower bound means "below every gate" and a nil upper bound means
	// "at or above the highest gate". Seeding them with the sentinels means a query
	// that fails partway leaves BandFromAppleRange seeing the safe values, rather
	// than 0, which would be a real age and a real range.
	int lowerBound = AppleNoLowerBound;
	int upperBound = AppleNoUpperBound;
	AppleAgeDeclaration declaration = AppleAgeDeclaration::Undetermined;

	// The failure branch, and it maps to Absent rather than to a partial band.
	//
	// A declined share means no band was established, so there is nothing to report.
	// Inferring one from whatever partial bounds arrived would be inventing data --
	// the same reasoning AttestationFromApple applies to NeverShared.
	//
	// Both failure branches above and here return Attestation::Absent(), so a caller
	// cannot distinguish "this platform cannot ask" from "the platform declined"
	// by the result alone. Both fail closed, so that is correct behaviour -- and it
	// is also a deliberate loss of diagnostic detail.
	if (!QueryPlatformRange(lowerBound, upperBound, declaration))
		return Attestation::Absent();

	// The composition, and it happens to be the same free function that the iOS path
	// and the tests all use -- so there is exactly one place where Apple's response
	// becomes a band.
	//
	// That single choke point is the design's real strength, and it is what the
	// section banner's "do not move the mapping down here" is protecting.
	return AttestationFromApple(lowerBound, upperBound, declaration);
}

// The provider's name accessor, returning the member by const reference.
//
// `const` because the name identifies the provider rather than configuring it;
// a mutable ProviderName would let a caller change what an attestation claims it
// came from.
const std::string &AppleAgeSignalProvider::ProviderName() const
{
	// Straight member return, and m_Name is never written anywhere in this file.
	//
	// DEFECT (uninitialised or duplicated elsewhere): m_Name is declared in the
	// header and initialised there, not here. Worth checking that it matches the
	// "apple-declared-age-range" literal passed to Attestation::FromProvider above
	// -- if they diverge, the provider's identity would be reported two different ways
	// in one flow. Cannot be confirmed from this file alone.
	return m_Name;
}

// The namespace close, with a trailing comment.
} // namespace Wasteland::Age