// Precompiled header first, by engine convention.
#include "wlpch.h"

// This class's declaration, plus the GoogleAgeRangeSource and
// GoogleAgeSignalsStatus enums used by the free functions below.
#include "Wasteland/Age/GooglePlayAgeSignalProvider.h"

// C++17 nested namespace, matching the rest of the Age module.
namespace Wasteland::Age
{

// Maps a Google Play age RANGE onto one AgeBand.

// Free function rather than a private member: the decision logic is the part
// that must be testable, and the file's own comment insists the mapping stay
// OUTSIDE the platform guard for exactly that reason. A member of a class
// whose Query() is Android-only would be untestable on the desktop.

// `const` on by-value parameters is stylistic and has no effect on the
// signature; the Age module uses it consistently.
AgeBand BandFromGoogleRange(const int lowerBound, const int upperBound)
{
	// Per Google's documentation, both bounds are null unless the status is
	// SHARED. A null lower bound therefore means there is no signal, not
	// "age 0" -- and Under13 is the most restrictive answer available.
	// Negative lower bound means Google reported null. Per the file's comment
	// both bounds are null unless the status is SHARED, so a null lower bound
	// means NO SIGNAL -- not "age 0".

	// Under13 is the most restrictive band available, so the absence of a signal
	// resolves to maximum restriction. That is the fail-closed direction: an
	// unknown age is treated as the youngest permitted category.
	if (lowerBound < 0)
		return AgeBand::Under13;

	// An inverted range cannot come from Play. Either the studio's
	// integration has a bug or the state is corrupt; either way, fail closed.
	// An inverted range cannot come from Play -- either the studio's integration
	// is wrong or the state is corrupt. Both map to Under13.

	// The `upperBound >= 0` half matters: a null upper bound (-1) is legitimate
	// and means the top band, so testing only `upperBound < lowerBound` would
	// misread every open-ended range as malformed and force the most restrictive
	// band on every adult.
	if (upperBound >= 0 && upperBound < lowerBound)
		return AgeBand::Under13;

	// Read the LOWER bound.
	//
	// This is what makes custom Play Console gates safe. With a single
	// minimum age of 15, Play reports [15, or over] -- a LOWER bound, not an
	// adult verdict. Mapping on the lower bound gives Age13To15, so a
	// fifteen-year-old does not become a verified adult. Likewise with gates
	// at 13 and 17, Play's [17, or over] maps to Age16To17, not Adult18.
	//
	// It also matches the Apple provider, so the two agree on the same
	// numbers. That agreement is worth having: a cross-platform studio
	// should not get different answers from its iOS and Android builds.
	//
	// The cost is that a wide custom range such as [0,14] maps to Under13,
	// treating a fourteen-year-old as under thirteen. That is
	// over-restrictive, which is the safe direction, and it is a consequence
	// of the studio's own gate choice rather than a bug.
	// The LOWER bound decides the band, and the file's comment block above
	// explains why at length.

	// The reasoning is worth restating because it looks wrong at a glance: with a
	// single minimum age of 15, Play reports [15, or over], which maps to
	// Age13To15 rather than Adult18. That is deliberate -- mapping the upper
	// bound would turn a custom Play Console gate into a claim of verified
	// adulthood that Play never made. It also makes Android agree with the
	// provider on identical numbers.

	// The acknowledged cost: a wide custom gate such as [0,14] maps to Under13,
	// treating a fourteen-year-old as under thirteen. Over-restrictive, which is
	// the safe direction, and caused by the studio's own gate choice.
	return BandForAge(lowerBound);
}

// Maps the tier that supplied the range onto an AssuranceLevel.

// The rating is of the INTEGRATION, not of Google's internal rigour -- the
// comment on the TierC/TierD arm makes that explicit.
AssuranceLevel AssuranceLevelForGoogleSource(const GoogleAgeRangeSource source)
{
	// Exhaustive over the enum, with the self-declared tier folded into `default`
	// so an unrecognised value is handled too.
	switch (source)
	{
	// A guardian set the age range, which is a real signal from a real person.
	case GoogleAgeRangeSource::TierBGuardian:
		// OsSignal, not anything stronger: the engine rates the assurance it actually
		// received.
		return AssuranceLevel::OsSignal;

	// An assessment (Tier C) and a government ID check (Tier D) are strong
	// signals, but they arrive by way of "ask Google Play".
	case GoogleAgeRangeSource::TierCAssessment:
	case GoogleAgeRangeSource::TierDGovernmentId:
		// Strong internally, but the developer's integration is still "ask
		// Google Play". This engine rates the integration, not Play's
		// internal rigour, so a studio cannot claim a third-party assurance
		// it did not procure. A studio wanting Commercial implements
		// IAgeAssuranceProvider against Yoti or Persona directly.
		// Same rating as the guardian tier, for a different reason: the developer's
		// integration is still platform-mediated, so a studio cannot claim
		// third-party assurance it did not procure. A studio wanting Commercial must
		// implement IAgeAssuranceProvider against Yoti or Persona directly.

		// Note these two tiers return the same value as Tier B despite being
		// materially stronger evidence. That is the rating of the integration being
		// the point, not an oversight in the mapping.
		return AssuranceLevel::OsSignal;

	// The user typed a number.
	case GoogleAgeRangeSource::TierASelfDeclared:
	default:
		// TIER_A: the user typed a number. Art 3(i) of the EU KIDS Act
		// proposal excludes self-declaration from "age assurance", so this
		// unlocks nothing -- not even for an 18+ band, where Attestation's
		// IsMinor() guard turns the answer back to minor.
		//
		// The default arm handles both TIER_A and any unrecognised value. An
		// unknown tier -- a newer Play SDK, or corrupt state -- must not be
		// assumed to be the one verified case, and TIER_A is the only tier
		// that unlocks nothing.
		// SelfDeclared, which unlocks nothing.

		// The comment gives the reason: Art 3(i) of the EU KIDS Act proposal
		// excludes self-declaration from "age assurance" -- and Attestation's
		// IsMinor() guard turns even an 18+ band back to minor, so the value is safe
		// to return without further gating here.

		// `default` falls into this same arm deliberately, per the comment above it:
		// an unknown tier (a newer Play SDK, corrupt state) must not be assumed to
		// be the one verified case, and TIER_A is the only tier that unlocks nothing.
		return AssuranceLevel::SelfDeclared;
	}
}

// The full range+source+status to Attestation conversion.

// Free function for the same testability reason as the two above.
Attestation AttestationFromGoogle(const int lowerBound, const int upperBound, const GoogleAgeRangeSource source,
                                  const GoogleAgeSignalsStatus status)
{
	// Only SHARED carries a range. NOT_SHARED, VERIFICATION_REQUIRED,
	// Undetermined and anything unrecognised all mean no usable signal, so
	// the band stays Unknown and the attestation reports the player as a
	// minor.
	//
	// Note this deliberately discards the bounds in the VERIFICATION_REQUIRED
	// case. A studio wanting to tell the player "resolve this in the Play
	// Store" -- which is what Google asks for, rather than re-prompting
	// in-app -- reads the status enum directly. It must not be inferred from
	// a band.
	// Only SHARED carries a usable range. NOT_SHARED, VERIFICATION_REQUIRED,
	// Undetermined and anything unrecognised all mean no signal, so this
	// collapses every one of them to Absent() -- which reports the player as a
	// minor.

	// Worth noting for whoever reads this next: VERIFICATION_REQUIRED
	// deliberately discards the bounds. A studio that wants to tell the player
	// "resolve this in the Play Store" -- which is what Google asks for, rather
	// than re-prompting in-app -- must read the status enum directly. That
	// information cannot be recovered from the resulting band.
	// The fail-closed answer: no attestation means the restrictive defaults.
	if (status != GoogleAgeSignalsStatus::Shared)
		return Attestation::Absent();

	// Assembles the attestation from the two mappings above. Both are pure, so
	// this is where the policy lives and the platform code need not.

	// The provider string is a literal, unlike the Apple provider's returned
	// m_Name -- this class is a singleton-shaped provider, so there is no name
	// field to return.
	return Attestation::FromProvider(AssuranceLevelForGoogleSource(source),
	                                 BandFromGoogleRange(lowerBound, upperBound),
	                                 "google-play-age-signals");
}

// --- Platform wrapper -----------------------------------------------------
//
// Everything below is inert off Android. WL_PLATFORM_ANDROID is never defined
// in this repository -- Core.h rejects an Android build outright -- so this
// translation unit compiles to the unavailable path and the desktop tests
// exercise the pure mapping above instead.
//
// Do not move the mapping down here. The rules about reading a lower bound,
// about custom console gates, and about TIER_A unlocking nothing are exactly
// the ones that decide whether a minor is protected, and behind a platform
// guard they would stop being tested.

// Platform wrapper. Everything from here down is inert off Android, which is
// what lets the desktop tests exercise the pure mapping above.
bool GooglePlayAgeSignalProvider::IsAvailable() const
{
// Never true in this repository -- Core.h rejects an Android build outright,
// per the file's own comment. So this compiles to the #else arm.
#if defined(WL_PLATFORM_ANDROID)
	// Available once Play Core is initialised and the app is in a region where
	// Play is required to supply age category data. Google's own note is that
	// the API returns nothing outside such regions, so this returning true
	// elsewhere is harmless -- Query() fails closed.
	// Would return true unconditionally inside the guard, on the reasoning that
	// the real check is whether Play returns data: outside a region where Play
	// supplies age categories the API returns nothing anyway, so Query() fails
	// closed.

	// As written that branch is unreachable, so the argument is a design note
	// about the future Android port rather than behaviour.
	return true;
#else
	// The only reachable return today: no Android backend exists, so the provider
	// reports itself unavailable and AgeService treats it as no provider.
	return false;
#endif
}

// The JNI seam. Four out-parameters rather than a struct because the Android
// port is unwritten; a small result struct would be the better shape if it
// were being written today.
bool GooglePlayAgeSignalProvider::QueryPlatformSignal(int &outLowerBound, int &outUpperBound,
                                                      GoogleAgeRangeSource &outSource,
                                                      GoogleAgeSignalsStatus &outStatus)
{
// Unreachable branch. The long comment inside is the specification for the
// intended JNI shim over Play Core, including the exact null-handling rules.
#if defined(WL_PLATFORM_ANDROID)
	// Not implemented. The engine has no Android backend, so this branch
	// cannot be compiled today and is unreachable from any build that exists.
	//
	// The intended implementation is a JNI shim over Play Core:
	//
	//     AgeSignalsClient.getClient(context).getAgeSignals()
	//         .addOnSuccessListener(info -> {
	//             // info.getAgeSignalsStatus()  -> map to GoogleAgeSignalsStatus
	//             // info.getAgeRangeSource()   -> map to GoogleAgeRangeSource
	//             // info.getAgeLower()         -> Integer, may be null
	//             // info.getAgeUpper()         -> Integer, null for the top band
	//         });
	//
	// The null handling is the part to get right. Play returns Integer
	// objects, and both are null unless the status is SHARED:
	//
	//   - A null ageLower means there is NO SIGNAL. Passing 0 would instead
	//     mean "age 0 to 14", which happens to map to Under13 -- the same
	//     answer by luck. Passing 0 is still wrong, because it makes a
	//     missing signal indistinguishable from a genuine under-13 signal in
	//     the audit trail, and that difference matters when someone is
	//     reconstructing why a player was restricted.
	//
	//   - A null ageUpper means the top band. Passing 0 instead would produce
	//     an inverted range [18, 0], which BandFromGoogleRange rejects as
	//     malformed and maps to Under13 -- treating EVERY adult as a child.
	//     That failure is safe but useless, and it would look like a working
	//     integration because nothing errors.
	//
	// Map nulls to GoogleNoLowerBound / GoogleNoUpperBound, and only consult
	// the bounds when the status is Shared.
	// Explicitly discarding the out-parameters so the stub compiles warning-free
	// without initialising them.

	// DEFECT (silent failure by design, and worth flagging): the function returns
	// false without ever WRITING the out-parameters. Its caller below checks only
	// the bool, so today nothing reads uninitialised values -- the contract is
	// "on false, outputs are undefined". That contract is not stated anywhere on
	// the declaration, so a future caller that read an output before checking
	// the return would get garbage with no diagnostic. Initialising the outs at
	// the top of the function, as the caller already does, would remove the
	// hazard at no cost.
	(void)outLowerBound;
	(void)outUpperBound;
	(void)outSource;
	(void)outStatus;
	return false;
// The #else arm, which is the one that actually compiles today.

// It is the same four discards and the same `return false` as the guarded
// arm above, so both branches of this function currently return false. The
// #if exists to keep the Android specification visible in the source, not
// to select between two behaviours.
#else
	(void)outLowerBound;
	(void)outUpperBound;
	(void)outSource;
	(void)outStatus;
	return false;
#endif
}

// The interface method: obtain a signal and convert it.

// Deliberately thin -- availability check, initialise outputs, delegate,
// convert -- so that all the policy lives in the pure functions the tests
// exercise.
Attestation GooglePlayAgeSignalProvider::Query()
{
	// Fail closed on an unavailable provider, per the IAgeAssuranceProvider
	// contract above.
	if (!IsAvailable())
		return Attestation::Absent();

	// Outputs pre-initialised to the documented "null" sentinels, so a failed
	// QueryPlatformSignal leaves well-defined values rather than indeterminate
	// ones.

	// These are named constants rather than the bare -1 the checks above test
	// for, which makes the intent readable at the call site.
	int lowerBound = GoogleNoLowerBound;
	int upperBound = GoogleNoUpperBound;
	GoogleAgeRangeSource source = GoogleAgeRangeSource::TierASelfDeclared;
	GoogleAgeSignalsStatus status = GoogleAgeSignalsStatus::Undetermined;

	// The platform call. A false return means no signal, and the next line returns
	// Absent() without consulting the outputs -- which is what makes the stub's
	// failure to write them safe today.
	if (!QueryPlatformSignal(lowerBound, upperBound, source, status))
		return Attestation::Absent();

	return AttestationFromGoogle(lowerBound, upperBound, source, status);
}

// The audit-trail name. Returns the member rather than a literal, unlike
// AttestationFromGoogle above which hard-codes "google-play-age-signals" --
// so a single rename has to be made in two places to keep them consistent.
const std::string &GooglePlayAgeSignalProvider::ProviderName() const
{
	return m_Name;
}

} // namespace Wasteland::Age