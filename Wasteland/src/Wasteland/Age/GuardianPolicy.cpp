#include "wlpch.h"

#include "Wasteland/Age/GuardianPolicy.h"

namespace Wasteland::Age
{

// Anonymous namespace: StricterOf is internal to this translation unit, so it
// gets internal linkage and needs no declaration in the header. Deliberately not
// a member -- it is a property of two GuardianSettings values, not of a policy.
namespace
{

// The most restrictive combination of two setting sets.
//
// Used while a proposed change is under review. See the header note for why:
// a pending relaxation must have no effect until approved, and a pending
// tightening must take effect immediately.
// The function the whole file exists to make possible: while a change is under
// review, the settings that APPLY must be the most restrictive combination of
// current and proposed.
//
// Field-by-field rather than a single numeric score because the fields do not
// share an ordering: for the flags, larger (more true) is stricter; for the
// curfew hour, smaller is stricter; for playtime, smaller is stricter EXCEPT that
// kUnlimited is the loosest value and 0 would otherwise read as the tightest. A
// weighted sum cannot express that, so each field gets its own rule.
GuardianSettings StricterOf(const GuardianSettings &current, const GuardianSettings &pending)
{
	// Copied from `current` rather than value-initialised, so a field added to
	// GuardianSettings later defaults to the CURRENT value rather than to whatever
	// the struct default happens to be. Conservative for a relaxation, and it means
	// this function does not need updating when the struct grows.
	GuardianSettings stricter = current;

	// Protective flags: the safe state is true, so OR keeps whichever is
	// already enforcing.
	// OR for the protective flags because true is the safe state: whichever side is
	// already enforcing keeps enforcing, and a proposed tightening takes effect
	// immediately. This is the half of the rule that does NOT wait for approval.
	//
	// Written out per flag rather than looping over an array of bools: GuardianSettings
	// is four separate named bools, and a loop would need reflection or a macro.
	stricter.NightHoursEnforced = current.NightHoursEnforced || pending.NightHoursEnforced;
	stricter.ReportActivityToGuardian = current.ReportActivityToGuardian || pending.ReportActivityToGuardian;
	stricter.PurchaseLimitsEnforced = current.PurchaseLimitsEnforced || pending.PurchaseLimitsEnforced;
	stricter.ChatRestricted = current.ChatRestricted || pending.ChatRestricted;

	// Curfew: earlier is more restrictive, so take the earlier hour.
	// min() for the curfew: earlier is more restrictive, so the stricter of two
	// hours is the earlier one. The ternary rather than std::min so both arguments
	// stay plain ints (a 0-23 range with no sentinel semantics beyond the struct
	// default of 21).
	stricter.CurfewHour = current.CurfewHour < pending.CurfewHour ? current.CurfewHour : pending.CurfewHour;

	// The playtime field is where a naive min() would be actively wrong, and the
	// comment above already says why. Restated here as the shape of the code: three
	// branches, and only the last one is an actual comparison.
	// Playtime: this is NOT a plain min(), because kUnlimited is 0 and 0 is
	// the loosest value rather than the tightest. Taking a plain min() would
	// turn "no limit" into "no minutes allowed", which is both nonsense and a
	// denial of service against the player's own guardian.
	//
	// So: if either side has a real limit, that limit applies. Only when
	// BOTH are unlimited is the result unlimited.
	// First branch: current is unlimited, so the pending value governs. This is a
	// TIGHTENING (unlimited -> limited) and the header requires it to apply at once.
	if (current.DailyPlaytimeMinutes == GuardianSettings::kUnlimited)
		stricter.DailyPlaytimeMinutes = pending.DailyPlaytimeMinutes;
	// Second branch: the mirror image. Pending "unlimited" cannot loosen a limit
	// that is already in force -- it can only mean "no change proposed", so current
	// wins. Treating the sentinel as 0 here would produce a player with zero minutes
	// allowed, which is both nonsense and a denial of service against the player's
	// own guardian.
	else if (pending.DailyPlaytimeMinutes == GuardianSettings::kUnlimited)
		stricter.DailyPlaytimeMinutes = current.DailyPlaytimeMinutes;
	else
		// The third branch is the only one that is a plain min(): both sides have a real
		// limit, so the smaller is genuinely the stricter.
		stricter.DailyPlaytimeMinutes =
		    current.DailyPlaytimeMinutes < pending.DailyPlaytimeMinutes ? current.DailyPlaytimeMinutes
		                                                                : pending.DailyPlaytimeMinutes;

	return stricter;
}

} // namespace

// Band -> state. Separate from SettingsForBand below so Approved() and Denied()
// can recover the resting state from the band while keeping different settings --
// the split the header describes as the reason m_Band is stored at all.
GuardianState GuardianPolicy::StateForBand(const AgeBand band)
{
	// A switch with no `default:` of its own: the explicit `default:` below sits
	// beside `case AgeBand::Unknown`, which is the same thing but keeps the
	// unrecognised-value comment attached to the arm it actually protects.
	switch (band)
	{
	// Adult18 -> Adult, and only that combination is a guardian-free state.
	case AgeBand::Adult18:
		return GuardianState::Adult;

	case AgeBand::Age16To17:
	case AgeBand::Age13To15:
		return GuardianState::Minor;

	// The two "protections established" minor bands collapse to one state. The band
	// is still available through Policy().Band(), which is what lets the UI tailor
	// wording per band while the enforcement state stays coarse.
	case AgeBand::Under13:
		// A separate state for Under13 rather than folding it into Minor, because the
		// guardian has not been walked through setup yet. Downstream code that checks
		// `state == Minor` would then treat an unconfigured under-13 player as fully set
		// up -- which is the confusion the extra enumerator exists to prevent.
		// Its own state because the guardian has not yet been walked through
		// setup, so this player must not be treated as fully configured.
		return GuardianState::MinorNeedsSetup;

	// Unknown shares the `default:` arm. DEFECT (fragile, deliberate): a NEW AgeBand
	// added to the enum lands here too, which is the fail-closed behaviour wanted --
	// but it lands silently. Unlike JurisdictionPolicy.cpp, there is no comment on
	// the fallthrough itself saying so, so a future band that should map to Adult
	// would quietly resolve to Unconfigured until someone noticed. Not fixed;
	// comments only.
	//
	// The correct version of this would be to enumerate every band and let the
	// compiler object to a missing arm.
	case AgeBand::Unknown:
	default:
		// No signal, or a value this engine does not recognise. Reported as
		// Unconfigured so the UI can prompt.
		//
		// This is the branch the module depends on. It must not fall through
		// to Adult -- a missing signal must never yield a guardian-free adult
		// configuration -- and it must not fall through to Minor either, since
		// "we do not know yet" and "we know, and protections are configured"
		// are different facts the UI needs to present differently.
		// Unconfigured, not Minor and not Adult. The header is explicit that these are
		// three different facts the UI has to present differently: "we do not know yet"
		// and "we know, and protections are configured" are not the same sentence to a
		// parent.
		return GuardianState::Unconfigured;
	}
}

// Band -> baseline settings. Again split from StateForBand, and again because
// Approved() needs the state from the band but the settings from the proposal.
GuardianSettings GuardianPolicy::SettingsForBand(const AgeBand band)
{
	// Starts from the restrictive defaults so any field not explicitly set for
	// a band keeps the safe value.
	// A default-constructed GuardianSettings, and every member of that struct
	// defaults to the RESTRICTIVE value (flags true, curfew 21, playtime 60). So any
	// field not explicitly set for a band keeps the safe value -- the struct default
	// is doing the fail-closed work, and this function only relaxes what it means to.
	GuardianSettings settings;

	switch (band)
	{
	// The only band that relaxes anything. Note what it does NOT relax:
	// ReportActivityToGuardian stays true, because an adult may have chosen their own
	// guardian and that default is worth keeping. This is the one place in the module
	// where a flag is left restrictive for a reason that is a default, not a law.
	case AgeBand::Adult18:
		// Outside Art 8(1) -- an adult, so the child-safety floor does not
		// apply and guardian settings become the player's own choice.
		//
		// Activity reporting stays on: an adult may have chosen their own
		// guardian, and this is a default worth keeping rather than a legal
		// requirement being enforced.
		settings.NightHoursEnforced = false;
		settings.ChatRestricted = false;
		settings.PurchaseLimitsEnforced = false;
		// kUnlimited for the adult playtime allowance, and this is the ONLY place that
		// value is produced. Every minor band gets a real number, so "unlimited" never
		// arises as a default that a min() could misread.
		settings.DailyPlaytimeMinutes = GuardianSettings::kUnlimited;
		break;

	// 16-17 gets the longest minor allowance. Each band sets only the fields that
	// DIFFER from the restrictive defaults, so the three minor arms below are two-
	// and one-line arms rather than five-line copies of the same seven assignments.
	case AgeBand::Age16To17:
		// A minor with protections established. Longer playtime than the
		// younger bands; chat still restricted, curfew still enforced.
		settings.DailyPlaytimeMinutes = 180;
		break;

	case AgeBand::Age13To15:
		settings.DailyPlaytimeMinutes = 120;
		break;

	// Under13 gets the earliest curfew and the shortest allowance. The curfew is
	// set here and only here -- the older minor bands keep the struct default of 21,
	// which is later than 19 and therefore less restrictive.
	case AgeBand::Under13:
		// Shortest allowance and the earliest curfew.
		settings.CurfewHour = 19;
		settings.DailyPlaytimeMinutes = 60;
		break;

	// Unknown, and any unrecognised band, keeps the restrictive defaults unchanged
	// and falls out of the switch. The `break` is required: without it the code would
	// fall into whatever is added next.
	case AgeBand::Unknown:
	default:
		// Restricted defaults, unchanged. Unconfigured is maximally
		// restrictive because it might be a child.
		break;
	}

	return settings;
}

// Restricted() is ForBand(Unknown) spelled as intent. The header calls this out
// deliberately: a call site should read as "the restrictive policy" rather than
// "the policy for the band we could not determine".
GuardianPolicy GuardianPolicy::Restricted()
{
	return ForBand(AgeBand::Unknown);
}

// The constructor path. All three fields are assigned explicitly rather than
// relying on member initialisers, because m_Band has to be stored -- and the
// note at its declaration in the header explains what breaks without it.
GuardianPolicy GuardianPolicy::ForBand(const AgeBand band)
{
	GuardianPolicy policy;
	policy.m_Band = band;
	// m_State comes from the band, never from the settings. That separation is what
	// stops a minor's approved configuration from being labelled Adult.
	policy.m_State = StateForBand(band);
	policy.m_Settings = SettingsForBand(band);
	return policy;
}

// EffectiveSettings is the enforcement entry point, and the header is emphatic
// that it -- not Settings() -- is what a caller must use to enforce. Settings()
// would ignore a pending tightening entirely.
GuardianSettings GuardianPolicy::EffectiveSettings() const
{
	// The fast path. Returning by value rather than by const reference because
	// GuardianSettings is five ints and bools -- a small copy beats the aliasing a
	// reference would invite at the call site.
	if (!m_HasPending)
		return m_Settings;

	// The pending change has no effect on enforcement until it is approved --
	// unless it TIGHTENS, in which case the stricter value applies at once.
	// The interesting case: StricterOf(current, pending), NOT pending. Getting
	// these two arguments the wrong way round would still "work" for the flags (OR is
	// commutative) but would invert the curfew and playtime comparisons -- so the
	// order is load-bearing and not merely stylistic.
	return StricterOf(m_Settings, m_PendingSettings);
}

// WithPendingChange returns a NEW policy. `*this` is untouched, which is the
// mechanism behind the header's central claim: there is no window in which a live
// policy is briefly permissive, because no in-place transition exists to have a
// window in.
GuardianPolicy GuardianPolicy::WithPendingChange(const GuardianSettings &proposed) const
{
	// Copy the whole policy, so m_Band, m_Settings and m_State all carry over
	// without having to be enumerated. A field added later is carried for free.
	GuardianPolicy next = *this;
	next.m_PendingSettings = proposed;
	next.m_HasPending = true;
	// PendingReview is set unconditionally on this transition. Note it discards the
	// band-derived state -- which is why m_Band has to be kept: Approved() and Denied()
	// both need it to get back.
	next.m_State = GuardianState::PendingReview;
	return next;
}

// Approved(). The no-pending branch is a no-op rather than an error, so a UI
// that double-fires a button does not have to guard against it.
GuardianPolicy GuardianPolicy::Approved() const
{
	// No pending change is a no-op, not an error: a UI that double-fires a
	// button is an ordinary bug, not an exceptional one.
	if (!m_HasPending)
		return *this;

	// A fresh object rather than a copy of *this: everything that matters is set
	// explicitly below, and starting from a known state means a field added later
	// cannot be inherited stale from a pending state.
	GuardianPolicy next;
	next.m_Band = m_Band;

	// The resting STATE comes from the band, never from the pending settings.
	// Deriving it from the settings instead is how a minor's approved
	// configuration ends up labelled Adult -- which is why m_Band is stored
	// rather than recovered from the pending state.
	// The resting state from the band. This is the line the header calls out as
	// the reason m_Band exists -- deriving the state from the pending settings instead
	// is how a minor ends up labelled Adult.
	next.m_State = StateForBand(m_Band);

	// The approved settings are the whole point of approving, so they are
	// taken verbatim rather than re-derived from the band.
	// The approved settings are taken VERBATIM from the proposal, not re-derived
	// from the band. Re-deriving would silently discard whatever the guardian actually
	// asked for, which is the one thing approving is supposed to do.
	next.m_Settings = m_PendingSettings;
	// Cleared last, so a caller that inspects the returned policy sees a settled
	// one: state from the band, settings from the proposal, nothing pending.
	next.m_HasPending = false;
	return next;
}

// Denied(): same shape as Approved() through the no-pending branch.
GuardianPolicy GuardianPolicy::Denied() const
{
	if (!m_HasPending)
		return *this;

	GuardianPolicy next;
	next.m_Band = m_Band;
	next.m_State = StateForBand(m_Band);

	// Re-derived from the band rather than copied from m_Settings. Equivalent
	// today -- nothing but WithPendingChange can change them, and that leaves the
	// source untouched -- but re-deriving means Denied() cannot be fooled by a
	// settings value that arrived from anywhere else.
	// The original settings stand. Re-derived from the band, which is
	// equivalent to the pre-pending settings because nothing else can change
	// them -- WithPendingChange is a value transition, so the source policy is
	// left untouched.
	next.m_Settings = SettingsForBand(m_Band);
	next.m_HasPending = false;
	return next;
}

} // namespace Wasteland::Age