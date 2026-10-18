// Wasteland::Age -- guardian state machine.
//
// GuardianPolicy is what makes Art 20 of the EU KIDS Act proposal
// (COM(2026) 681, 2026/0286(COD)) operable rather than merely satisfied:
//
//   "The age assurance solution referred to in Article 12 shall be easy to
//    use, accessible and activate-able ... and the resulting settings shall
//    be tailored to the age of the user."
//
// A purely automatic SafetyProfile satisfies "tailored to the age" and gives
// a parent nothing to do. This adds the missing half: a state machine a
// guardian can reach, change, and have the change reviewed.
//
// THE RULE THIS FILE EXISTS TO PROTECT
//
// A guardian may propose a change to a minor's settings. While that change is
// under review, the settings that APPLY must be the most restrictive
// combination of the current and proposed values -- never the more permissive
// one.
//
// If a pending relaxation took effect immediately, then flipping a switch
// would disable protection for as long as it took to notice, and REVERTING
// it would be the act that re-enabled protection. A review state that briefly
// lowers the floor is not a review state, it is a bypass.
//
// This is why the type is a value. WithPendingChange() returns a new policy;
// it cannot mutate the one the game is currently enforcing. There is no
// window in which a live policy is briefly permissive, because no in-place
// transition exists to have a window in.
//
// WHY VALUE SEMANTICS RATHER THAN A MUTABLE STATE MACHINE
//
// The same reasoning as SafetyProfile and Attestation: a live protection
// object that can be modified in place is a variable that happens to hold a
// protection. Every transition here returns a new policy, so a caller holding
// the old one keeps a valid, still-restrictive snapshot.
//
// Neither instrument is in force as of this writing. See
// docs/AGE_ASSURANCE.md.

#pragma once

#include "Wasteland/Age/AgeBand.h"

// Nested-namespace form, matching AgeService.h and AgeService.cpp.
// The closing brace on line 189 is NAMED, so the two-brace file ends
// unambiguously.
namespace Wasteland::Age
{

// Where a player stands with respect to guardian-supervised settings.
// The explicit `int` underlying type is worth pausing on. The states
// below are all assigned 0..4 and this enum is almost certainly not
// serialised -- GuardianPolicy is built at runtime from an AgeBand. But
// Graph.h in this same codebase documents at length that enum ordinals
// are persisted in saved files. If a guardian state ever IS persisted,
// an `int` base makes the representation at least explicit; if it is
// NOT, the `: int` costs 4 bytes per policy where a uint8_t would do
// and constrains nothing.
enum class GuardianState : int
{
	// No age signal has been resolved yet. The most restrictive settings
	// apply and the UI should prompt for verification. NEVER reached by a
	// verified adult, and never a synonym for Adult.
	// Zero-initialised, so a default-constructed enum member is
	// Unconfigured rather than an arbitrary value. m_State on line 174
	// relies on that: it has = GuardianState::Unconfigured explicitly, but
	// the convention is the same one.
	Unconfigured = 0,

	// Verified under 13. The guardian protections exist but have not been
	// established with the guardian yet, so this player is not treated as
	// fully configured.
	MinorNeedsSetup = 1,

	// Verified 13-17 with guardian protections established.
	Minor = 2,

	// Verified adult. Outside Art 8(1); guardian settings are the player's
	// own choice.
	Adult = 3,

	// A proposed change is awaiting approval. See the header note: during
	// review the STRICTER of current and proposed settings applies.
	// The fifth and last state, and the only one that is a TRANSITION
	// rather than a resting state -- the others describe who the player
	// is, this one describes where a change is.
	// DEFECT (state machine shape): PendingReview does not say WHICH
	// band is under review. Band() (line 134) returns the underlying band,
	// and the comment above line 132 calls that a feature. But it means two
	// policies with different m_Band and different m_Settings can both
	// report State() == PendingReview, and nothing in the type lets a caller
	// tell a pending relaxation apart from a pending tightening except by
	// diffing EffectiveSettings() against Settings().
	PendingReview = 4,
};

// The guardian-controlled settings. Every default is the restrictive value,
// so a default-constructed instance is safe to enforce.
struct GuardianSettings
{
	// Sentinel for "no limit". Explicit because 0 is a legitimate curfew
	// hour but is not a legitimate zero-minute playtime allowance.
	// constexpr, so it can be used in a constant expression without a
	// definition in a .cpp (implicitly inline since C++17).
	// DEFECT: a `static constexpr` member of an aggregate type still
	// affects whether GuardianSettings is a literal type, and this one is
	// fine -- but note kUnlimited is typed `int` while the field it
	// sentinelises (DailyPlaytimeMinutes, line 94) is also `int`, so at
	// least the sentinel and the value agree. A sentinel in the value's
	// own range is always a trap for a reader; the comment above it is
	// what makes this one safe.
	static constexpr int kUnlimited = 0;

	// PROTECTIVE flag -- true is the safe state.
	bool NightHoursEnforced = true;

	// 0-23. Only meaningful while NightHoursEnforced is true. Earlier is
	// more restrictive.
	// 21:00, the restrictive end of a typical evening curfew.
	// DEFECT (no validation, unlike kUnlimited): the comment says 0-23,
	// but nothing enforces it. A guardian UI passing 25 or -1 produces
	// a policy the rest of the engine has to defend against. There is no
	// clamp in WithPendingChange (line 155) or in SettingsForBand (line
	// 172), so the invariant is entirely the caller's responsibility and
	// is not documented as such at the setter.
	int CurfewHour = 21;

	bool ReportActivityToGuardian = true;
	bool PurchaseLimitsEnforced = true;
	// One of four PROTECTIVE booleans in a row (NightHoursEnforced,
	// ReportActivityToGuardian, PurchaseLimitsEnforced, ChatRestricted),
	// all defaulting to true.
	// Only the first carries a "PROTECTIVE flag" comment; the other
	// three are the same polarity by convention alone. A reader has to
	// infer from the name that ReportActivityToGuardian == true is the
	// SAFE state -- which it is, but the asymmetry is unstated. Worth
	// noting because these four are exactly the fields EffectiveSettings()
	// has to combine with AND, and getting one backwards silently
	// permissives a pending change.
	bool ChatRestricted = true;

	// Minutes per day. kUnlimited means no cap. Smaller is more restrictive,
	// so this is where the "stricter of two" rule is not a simple min().
	// One hour, restrictive.
	// The comment above flags that "stricter of two" is not a simple
	// min() here -- it is because a TIGHTER limit means a SMALLER number,
	// except that kUnlimited (0) is the most PERMISSIVE value and also the
	// smallest. So min() on this field alone would treat "no limit" as the
	// most restrictive allowance, which is exactly backwards.
	// That makes EffectiveSettings (line 146) the single place that
	// kUnlimited must be handled specially, and it is a private .cpp
	// function -- so a future caller writing a quick min() elsewhere gets
	// it silently wrong.
	int DailyPlaytimeMinutes = 60;
};

// An immutable snapshot: a state, the settings in force, and an optional
// pending change awaiting review.
//
// Copy-constructible, not assignable -- same rule and same reason as
// SafetyProfile: a live protection object must not be overwritable in place.
// There is deliberately no move constructor, because a user-declared one
// would delete the copy constructor that the transitions rely on.
// A VALUE type by design; the header note at lines 31-36 explains why.
// DEFECT (incomplete value semantics): the copy CONSTRUCTOR is
// defaulted on line 110, so GuardianPolicy is freely copyable -- but
// copy ASSIGNMENT is deleted on line 111. That combination is
// unusual: `p = q` will not compile while `p(q)` is fine, and a
// container element assignment or a std::swap over these fails to
// compile. The comment above line 100 says "copy-constructible, not
// assignable", so it is deliberate, but the practical effect is that
// GuardianPolicy cannot live in a std::vector that is resized or
// sorted, nor in most aggregate structures that need assignment.
class GuardianPolicy
{
public:
	// Unconfigured with fully restrictive settings. This is what a studio
	// gets before any signal exists, and it is what Unknown resolves to.
	GuardianPolicy() = default;
	GuardianPolicy(const GuardianPolicy &) = default;
	// Deleted copy assignment -- the rule the header note describes.
	// DEFECT (asymmetry that will confuse callers): the MOVE assignment
	// is also deleted (line 112), and the comment above line 102 explains
	// there is deliberately no move CONSTRUCTOR either, because declaring
	// one would delete the copy constructor. That reasoning is correct for
	// the constructor but not for assignment: deleting copy assignment
	// does not suppress the implicit move assignment, so line 112 is
	// genuinely necessary. The result is that a policy can only ever be
	// produced by a factory or a transition method -- there is no path
	// that writes into an existing GuardianPolicy at all.
	GuardianPolicy &operator=(const GuardianPolicy &) = delete;
	GuardianPolicy &operator=(GuardianPolicy &&) = delete;

	// Explicitly named for the default case, so a call site reads as intent
	// rather than as "empty policy".
	// Named for the default case so a call site reads as intent.
	// Note it is a FACTORY rather than a constant: Restricted() cannot be
	// constexpr because GuardianSettings is an aggregate but GuardianPolicy
	// has a user-declared copy constructor, so it is not a literal type.
	// That means every call allocates and copies -- cheap here, but it
	// means a `static const GuardianPolicy` cannot be shared, and a
	// caller in a hot path cannot hoist it out.
	static GuardianPolicy Restricted();

	// The policy for a verified band.
	//
	// Unknown -- and any unrecognised band value -- resolves to Restricted()
	// with State() == Unconfigured. It does NOT resolve to Adult. That is the
	// same rule every layer in this module follows, and getting it wrong here
	// would let a missing signal produce a guardian-free adult configuration.
	// The mapping from a verified band to a policy.
	// DEFECT (unverifiable, and load-bearing): the comment promises
	// Unknown and "any unrecognised band value" resolve to Restricted()
	// with State() == Unconfigured. That is the single most important
	// safety property in this file. Nothing in the TYPE enforces it -- it
	// is a promise about the .cpp, and if SettingsForBand (line 172) ever
	// gains an else branch that falls into Adult, the failure is silent:
	// a missing age signal would produce a guardian-free adult
	// configuration. A defensive default inside StateForBand plus a
	// WL_CORE_ASSERT on the Unknown path would make the guarantee
	// checkable. Not fixed here.
	static GuardianPolicy ForBand(AgeBand band);

	GuardianState State() const { return m_State; }

	// The band this policy was built for.
	//
	// Public because the guardian UI must be AGE-TAILORED (Art 20(3)) and
	// because the reminder cadence differs by band. Note that in PendingReview
	// this still reports the underlying band, not the pending state -- which is
	// what lets the view show age-appropriate wording while a change waits.
	// The band this policy was BUILT for, which is not the same as the
	// band of the settings now in force -- see EffectiveSettings on line
	// 146.
	// The comment above explains why this is public (Art 20(3) age-
	// tailored UI) and why it is useful during review.
	AgeBand Band() const { return m_Band; }

	// The settings currently in force, ignoring any pending change. Use
	// EffectiveSettings() when enforcing.
	// BY VALUE, unlike every other accessor in this file.
	// That is correct and deliberate: it is the pre-pending baseline,
	// and returning a reference would let a caller hold a view onto
	// internal state that Approved() later replaces.
	// The header comment warns to use EffectiveSettings() when
	// enforcing -- which is a correctness instruction carried only by a
	// comment. DEFECT: nothing stops a caller from enforcing the wrong one.
	// Settings() and EffectiveSettings() have the same return type and
	// different names that differ by seven characters; a mistake here is
	// the exact bug the whole file exists to prevent, and the type system
	// does nothing about it. Renaming Settings() to CurrentSettings() would
	// at least make the pair read symmetrically with Effective.
	GuardianSettings Settings() const { return m_Settings; }

	// The settings that should actually be enforced right now.
	//
	// With no pending change, these are Settings(). With a pending change,
	// these are the MOST RESTRICTIVE combination of the current and proposed
	// settings -- so a proposed relaxation has no effect until it is approved,
	// and a proposed tightening takes effect at once.
	// THE function. Every enforcement decision should read this and not
	// Settings().
	// Declared, not inline: the combine logic is the substantive part of
	// the file and is private to the .cpp.
	// DEFECT (four-way AND, four booleans): combining current and
	// proposed means ANDing the four protective booleans and taking the
	// min of the numeric limits, with kUnlimited treated as permissive.
	// That is correct in outline, but it is a place where adding a FIFTH
	// protective field to GuardianSettings does not produce a compiler
	// error at this function -- it just silently stops considering the new
	// field. A policy object that combines fields by hand is a policy that
	// can be under-restrictive after an additive change.
	GuardianSettings EffectiveSettings() const;

	// The proposed settings, meaningful only while HasPendingChange().
	// By value, like Settings() -- see the note there.
	// Meaningful only while HasPendingChange(); before that it returns
	// a default-constructed GuardianSettings, which is the RESTRICTIVE
	// one (every field defaults true/curated). So reading it out of context
	// yields a safe-looking value rather than a signal that there is
	// nothing pending -- which is convenient, and also means a caller who
	// forgets to check HasPendingChange() will apply restrictive settings
	// rather than crash.
	GuardianSettings PendingSettings() const { return m_PendingSettings; }

	// The flag, separate from the pending settings themselves.
	// Note that m_PendingSettings is ALWAYS default-constructed even
	// when there is no pending change, so the bool is the only signal -- see
	// the note above.
	bool HasPendingChange() const { return m_HasPending; }

	// Propose a change. Returns a NEW policy in PendingReview; `this` is
	// unchanged.
	// THE transition that the file exists to get right: returns a new
	// policy and leaves *this untouched.
	// DEFECT (no validation of `proposed`): nothing here checks that
	// CurfewHour is 0-23, that DailyPlaytimeMinutes is non-negative, or
	// that the settings are internally consistent. A guardian UI that can
	// produce an out-of-range value produces a policy the rest of the engine
	// must defend against -- and because the result is applied only after
	// approval, the bad value sits unnoticed until then.
	// Also: nothing prevents a pending change from being proposed on a
	// policy that already has one, so a second call silently replaces the
	// first proposal.
	GuardianPolicy WithPendingChange(const GuardianSettings &proposed) const;

	// Accept the pending change: it becomes the settings in force and the
	// policy returns to the state its band implies. No pending change is a
	// no-op, not an error.
	// Accepts the pending change and returns to the band-implied state.
	// A no-op with no pending change, per the comment above.
	// The header note on line 170 explains why m_Band is retained:
	// Approved() recovers the resting STATE from it while keeping the
	// APPROVED settings. Getting that wrong is how an approved minor ends
	// up under GuardianState::Adult, which is exactly the failure the
	// comment on m_Band (line 179) says it prevents.
	GuardianPolicy Approved() const;

	// Reject the pending change: the original settings stand and the policy
	// returns to the state its band implies. No pending change is a no-op.
	// Mirror of Approved(): discards the proposal, keeps the current
	// settings, returns to the band-implied state.
	// Asymmetric by design with Approved(): this one restores m_Settings
	// unchanged while Approved overwrites it. Both must clear
	// m_HasPending, and nothing in the type enforces that either.
	GuardianPolicy Denied() const;

private:
	// The state a band implies, and the baseline settings for that band.
	// Split into two so that Approved() can recover the resting STATE while
	// keeping the APPROVED settings -- it must not re-derive the settings,
	// because those are the whole point of approving.
	// Private, so a caller cannot bypass ForBand and get a state for a
	// band without the matching settings.
	// DEFECT (split API): the comment explains why state and settings
	// are separate functions, and the reason is sound. But the consequence
	// is that these two are a MATCHED PAIR that nothing ties together.
	// ForBand on line 124 must call both, and both must handle Unknown.
	// Adding a band to one and not the other yields a policy whose State()
	// and Settings() disagree -- e.g. Adult state with a minor curfew --
	// and nothing detects it. A single function returning a pair of both
	// would make the pairing structural.
	static GuardianState StateForBand(AgeBand band);
	// The settings half of the pair above.
	// As above: nothing enforces that this and StateForBand agree on the
	// Unknown default, which is the safety-critical case.
	static GuardianSettings SettingsForBand(AgeBand band);

	// The default-constructed policy is Unconfigured with restrictive
	// settings -- which is what makes GuardianPolicy() = default on line
	// 109 safe to enforce directly, exactly as the comment on GuardianPolicy
	// claims.
	GuardianState m_State = GuardianState::Unconfigured;
	// No explicit initialiser: value-initialised from the in-class
	// defaults declared in GuardianSettings.
	// This is the resting settings, i.e. the baseline a pending change
	// is measured against. Never overwritten in place (assignment is
	// deleted), only replaced wholesale by a transition.
	GuardianSettings m_Settings;

	// The band this policy was built for.
	//
	// Kept so that Approved() and Denied() can return the policy to the state
	// its band implies. Without it, entering PendingReview would discard the
	// band and the only way back would be a hardcoded guess -- which is how a
	// minor's approved settings end up under GuardianState::Adult.
	// Retained so Approved() and Denied() can recover the band-implied
	// state. Defaulted to Unknown, so a default-constructed policy is
	// consistent with State() == Unconfigured -- the two defaults agree.
	// Note this is the member that makes the class NOT trivially
	// default-constructible in the aggregate sense; the class has a
	// user-declared copy ctor anyway.
	AgeBand m_Band = AgeBand::Unknown;

	// Always present, whether or not there is a pending change -- see the
	// note on PendingSettings() above. One redundant member rather than an
	// optional, which is the deliberate cost of a type with no null state.
	GuardianSettings m_PendingSettings;
	// The only flag, and the only thing distinguishing "no proposal" from
	// "a proposal identical to the current settings".
	// DEFECT: separate from m_PendingSettings with nothing tying them
	// together -- a policy with m_HasPending true and a default
	// m_PendingSettings is representable, and would apply a restriction to
	// the current settings during review. The invariant holds only because
	// all three writers (WithPendingChange, Approved, Denied) in the .cpp
	// set both.
	bool m_HasPending = false;
};

// Named closing brace.
} // namespace Wasteland::Age