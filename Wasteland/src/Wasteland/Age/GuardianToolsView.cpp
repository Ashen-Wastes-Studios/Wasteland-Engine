// PCH first, then the header that declares what this file builds. The header
// carries the substantive rationale -- read that before this body.
#include "wlpch.h"

// The interface being implemented. Quoted and include-path form, so it
// resolves the same way in every target that compiles this file.
#include "Wasteland/Age/GuardianToolsView.h"

// The module's own namespace, opened with the C++17 nested form. Used
// because the module is small and the nested form keeps the two levels
// visible in one token.
namespace Wasteland::Age
{

// Anonymous namespace for the helpers. Keeping Toggle, Action and Hours out
// of the public header is what lets them change signature without touching
// a consumer.
//
// The labels below are constexpr rather than string literals at the point
// of use, so the same pointer is compared in the view and in the tests --
// that identity is what makes the label a testable contract.
namespace
{

// Row labels, defined once so the view and any UI-side lookup agree. The test
// suite matches on these strings, which is intentional: it means a renamed
// control breaks a test rather than silently changing what the panel says.
// The seven row labels. constexpr const char* rather than std::string_view
// or std::string: they are assigned into ControlState::Label, which is a
// std::string, so the conversion happens once per row either way.
constexpr const char *kChatLabel = "Chat with other players";
constexpr const char *kPlaytimeLabel = "Daily playtime limit";
constexpr const char *kNightHoursLabel = "Night-time play curfew";
constexpr const char *kPurchaseLabel = "Purchase limits";
constexpr const char *kReportLabel = "Report a problem";
constexpr const char *kMuteLabel = "Mute a player";
constexpr const char *kBlockLabel = "Block a player";

// A toggle row.
//
// `effective` is what is ENFORCED. `pendingDiffers` is whether a proposed
// change differs from the resting setting.
//
// The two are deliberately independent, and conflating them is the bug this
// function is shaped around: a pending RELAXATION has effective == resting
// while still being a real pending change. If the tick mark followed the
// pending value the player would see "chat off", the switch would appear to
// have worked, and nothing would have changed. If the tick mark follows the
// effective value -- as here -- it stays on, and the separate PendingChange
// flag is what explains why the switch looks stuck.
// Builds one toggle row. The two booleans it takes are the entire reason
// this function exists as a named helper rather than four inlined
// constructions -- see the note on ControlState::Toggling in the header.
ControlState Toggle(const char *label, const bool effective, const bool pendingDiffers)
{
		// Default-constructed rather than aggregate-initialised. ControlState is
		// an aggregate, so `ControlState{...}` would work and be shorter, but it
		// would be order-dependent: adding a field to the struct would silently
		// reshuffle the initialisers. Field-by-field assignment cannot.
	ControlState control;
	control.Label = label;
	control.Kind = ControlKind::Toggle;
	control.Enabled = true;
		// The tick mark follows the ENFORCED value. This is the single most
		// consequential line in the file: setting it from `pendingDiffers` or from
		// the resting value instead would make a pending relaxation look applied.
	control.Toggling = effective;
	control.PendingChange = pendingDiffers;
	return control;
}

// An action row. Distinct from Toggle because the ImGui layer draws a
// button rather than a checkbox, and because an action has no on/off state
// at all.
ControlState Action(const char *label)
{
	// Always enabled.
	//
	// Art 20(2) rights -- mute, report, block -- are the player's tools for
	// acting on harm. Gating them, or worse, hiding them in the most
	// restricted state, would invert the obligation they exist to discharge:
	// the players most at risk are the ones who most need the button.
		// Same default-then-assign pattern as Toggle, so the two cannot drift in
		// how they construct a ControlState.
	ControlState control;
	control.Label = label;
	control.Kind = ControlKind::Action;
	control.Enabled = true;
	return control;
}

// Formats an hour of day. DEFECT (documented, not fixed): there is no
// zero-padding, so midnight renders as "0:00" and 9am as "9:00". That is
// valid 24-hour notation but inconsistent, and the string is concatenated
// straight into a player-facing sentence in RestrictionNotes below.
std::string Hours(const int hour)
{
		// string concatenation rather than a format call. The `:00` suffix is
		// hardcoded, so this function cannot express a half-hour or a minute --
		// which matches GuardianSettings::CurfewHour, an int hour.
	return std::to_string(hour) + ":00";
}

} // namespace

// The panel builder. Takes the policy by const reference: it only reads,
// and copying a policy with its pending state would be wasteful.
GuardianToolsModel BuildGuardianToolsView(const GuardianPolicy &policy)
{
		// Default-constructed GuardianToolsModel. Its in-class defaults matter here:
		// GuardianIndicatorVisible defaults false, State defaults Unconfigured and
		// IsMinor defaults TRUE. So an unpopulated model already reads as the
		// restrictive case -- the safe direction if a field below were missed.
	GuardianToolsModel model;

	// EffectiveSettings(), NOT Settings(): the panel must show what is
	// enforced. During a pending review that is the stricter combination, so a
	// pending relaxation cannot make the panel claim protection the player does
	// not actually have.
		// Three reads of the same policy, deliberately kept distinct: the enforced
		// values, the resting values, and whether anything is pending. Conflating
		// the first two is the bug this view is shaped to avoid.
	const GuardianSettings effective = policy.EffectiveSettings();
	const GuardianSettings resting = policy.Settings();
		// Read once into a local rather than queried repeatedly in the lambdas
		// below, so every control row agrees about whether a change is pending.
	const bool pending = policy.HasPendingChange();
	const AgeBand band = policy.Band();

		// State copied out first: the three derived flags below are all functions
		// of it, and this keeps the switch at the top of the function.
	model.State = policy.State();
	model.HasPendingChange = pending;

	// Adult is the only state that is not a minor. Everything else -- including
	// PendingReview, which a minor can be in -- keeps the player protected.
		// IsMinor as "not Adult". Conservative by construction -- a state added to
		// the enum later would default to the protected reading rather than the
		// relaxed one.
	model.IsMinor = model.State != GuardianState::Adult;
		// Two states need setup rather than one. Listing them explicitly (rather
		// than `state < Adult`) means a new enum member is not silently treated as
		// needing setup.
	model.NeedsSetup = model.State == GuardianState::Unconfigured || model.State == GuardianState::MinorNeedsSetup;

	// Age-tailored wording, Art 20(3).
	//
	// Driven off the BAND rather than the state, so a minor awaiting setup or
	// mid-review still reads as a minor and is not handed adult wording
	// because of a transient state.
		// Switch on the BAND. Every enum member is handled and the default shares
		// the Unknown arm, so a band added later falls into the no-signal wording
		// rather than leaving Greeting empty.
	switch (band)
	{
	case AgeBand::Under13:
				// Three of the five bands resolve to the same responsible party but not the
				// same wording, which is why they are separate arms rather than one with a
				// parameterised string.
		model.ResponsibleParty = ResponsibleParty::Guardian;
				// String literals assigned to a std::string member: the concatenated form
				// is one allocation at assignment, not two.
		model.Greeting = "These settings are managed by a grown-up who looks after you. "
		                 "You can still report, mute, and block anybody.";
				// `break` rather than a fallthrough comment. The enum arms are independent
				// assignments into the same model, so a fallthrough here would be harmless
				// but misleading.
		break;

	case AgeBand::Age13To15:
		model.ResponsibleParty = ResponsibleParty::Guardian;
		model.Greeting = "A grown-up sets the limits here. You can still report, mute, and block anybody.";
		break;

	case AgeBand::Age16To17:
		model.ResponsibleParty = ResponsibleParty::Guardian;
		model.Greeting = "You and a grown-up decide these settings together. "
		                 "You can still report, mute, and block anybody.";
		break;

		// The only arm where the player is in charge. The short greeting is
		// intentional -- an adult has chosen these settings and needs no reminder
		// that a grown-up is involved.
	case AgeBand::Adult18:
		model.ResponsibleParty = ResponsibleParty::Self;
		model.Greeting = "You control these settings.";
		break;

		// Unknown and `default` share one arm. Worth noting that `default` is
		// unreachable today because every enum member is listed above; it exists so
		// the compiler does not warn about a missing case when the enum grows.
	case AgeBand::Unknown:
	default:
		// No signal. Wording assumes the WORST case, so the panel reads as a
		// child's rather than as an adult's with the restrictions quietly off.
		model.ResponsibleParty = ResponsibleParty::Guardian;
		model.Greeting = "We do not have an age signal yet, so the safest settings apply "
		                 "until one is provided. You can still report, mute, and block anybody.";
		break;
	}

	// The Art 8(1) indicator. Up whenever protections are enforced.
	//
	// Keyed off IsMinor, which is false only for a verified adult. That makes
	// it up for Unconfigured and PendingReview as well as for a configured
	// minor -- the states where the indicator matters most -- and down only
	// when there is genuinely no supervision to indicate.
		// The indicator follows IsMinor, which is the conservative flag rather than
		// the policy state. So it is visible even in PendingReview, which is when a
		// player most needs to see that protections are still on.
	model.GuardianIndicatorVisible = model.IsMinor;
	if (model.GuardianIndicatorVisible)
	{
		model.IndicatorText = model.NeedsSetup ? "Protective settings are on and not yet fully set up."
		                                       : "Protective settings are on.";
	}

	// Pending-change marker compares the PROPOSAL against the resting setting,
	// not against the effective value: a pending relaxation differs from
	// resting even though it has no effect on what is enforced.
		// A generic lambda comparing a proposed field against its resting
		// counterpart. Generic so the same lambda serves four different field
		// types without a template parameter list.
	//
		// `pending &&` first: with nothing pending there is nothing to differ from,
		// and short-circuiting avoids reading pending settings that may not be
		// meaningful.
	//
		// DEFECT (documented, not fixed): the comparison is `!=`, which for the
		// playtime field treats "60 minutes requested" and "0 = unlimited requested"
		// as different even when a pending relaxation is what is under review. That
		// is the intended behaviour for the pending MARKER, but it means the same
		// helper cannot be reused for anything that needs to know which direction the
		// change goes.
	auto Differs = [pending, &resting](const auto &proposed, const auto &member) {
		return pending && proposed != member;
	};
		// PendingSettings() read after the lambda is defined, because the lambda
		// CAPTURES resting by reference and does not need `proposed` itself. The
		// capture list is what forces this ordering.
	const GuardianSettings proposed = policy.PendingSettings();

		// Chat, night hours and purchase limits, in that order. The order is the
		// panel's visual order and is therefore part of what the tests assert;
		// reordering these three lines changes the panel layout.
	model.Controls.push_back(Toggle(kChatLabel, effective.ChatRestricted, Differs(proposed.ChatRestricted, resting.ChatRestricted)));
	model.Controls.push_back(Toggle(kNightHoursLabel, effective.NightHoursEnforced, Differs(proposed.NightHoursEnforced, resting.NightHoursEnforced)));
	model.Controls.push_back(Toggle(kPurchaseLabel, effective.PurchaseLimitsEnforced, Differs(proposed.PurchaseLimitsEnforced, resting.PurchaseLimitsEnforced)));

		// The playtime row is built by hand rather than through a helper, because
		// it is a Value row and the Toggle/Action helpers both hardcode a Kind.
		// A third helper for a single call site would not earn its keep.
	ControlState playtime;
	playtime.Label = kPlaytimeLabel;
	playtime.Kind = ControlKind::Value;
	playtime.Enabled = true;
	// The ENFORCED number, not the requested one -- see the note in Toggle().
		// The enforced number, so a pending increase in allowance cannot be shown
		// as if it were already available.
	playtime.Value = effective.DailyPlaytimeMinutes;
	playtime.PendingChange = Differs(proposed.DailyPlaytimeMinutes, resting.DailyPlaytimeMinutes);
	model.Controls.push_back(playtime);

	// Art 20(2) rights. Last, so they sit apart from the settings a guardian
	// changes -- a player scanning for "block this person" should not have to
	// read past somebody else's playtime limit.
		// Art 20(2) rights, appended AFTER every setting row. The order is
		// deliberate: report, mute and block are the actions a player may need in
		// an emergency, so they should not require scrolling past somebody else's
		// playtime limit.
	model.Controls.push_back(Action(kReportLabel));
	model.Controls.push_back(Action(kMuteLabel));
	model.Controls.push_back(Action(kBlockLabel));

	// Restriction notes, derived from the EFFECTIVE settings.
	//
	// Listing a restriction that is not in force is worse than listing none:
	// the player concludes the panel is unreliable and stops reading it. So a
	// row appears here only when the effective value actually restricts.
	//
	// Adults get none -- their profile is not restricted by age, and telling an
	// adult that chat is "restricted" because an adult profile has chat off
	// would be plainly wrong.
		// Notes only for minors. An adult whose profile happens to have chat off
		// for their own reasons is not age-restricted, and saying otherwise would
		// be a lie.
	if (model.IsMinor)
	{
		if (!effective.ChatRestricted)
						// Chat note is emitted when chat is NOT restricted -- i.e. this is the
						// permissive case. The polarity is the opposite of the two notes below it,
						// which is easy to misread and worth stating.
			model.RestrictionNotes.push_back(std::string(kChatLabel) + " is turned off.");
		if (effective.NightHoursEnforced)
						// The curfew note carries the enforced HOUR, not a range, and not the
						// requested hour. If a guardian is asking for a later curfew, the panel
						// shows the hour still in force.
			model.RestrictionNotes.push_back(std::string(kNightHoursLabel) + " ends at " + Hours(effective.CurfewHour) + ".");
		if (effective.PurchaseLimitsEnforced)
			model.RestrictionNotes.push_back(std::string(kPurchaseLabel) + " are in force.");
		if (effective.DailyPlaytimeMinutes != GuardianSettings::kUnlimited)
						// The playtime note, guarded on kUnlimited. Note kUnlimited is 0, so this
						// comparison is "not the sentinel" rather than "greater than zero" -- and a
						// genuinely-zero-minute allowance is indistinguishable from unlimited by
						// construction. The static_assert in test_guardian_policy.cpp pins that.
			model.RestrictionNotes.push_back(std::string(kPlaytimeLabel) + " is " + std::to_string(effective.DailyPlaytimeMinutes) + " minutes.");
	}

		// Returned by value: GuardianToolsModel holds two vectors and four strings,
		// so a move out of the local is what happens here rather than a copy.
	return model;
}

// The reminder cadence per band. Separate from ShouldRemind below rather
// than folded into it, so the cadence is testable on its own -- and so it
// CAN be tested separately, which is what makes the defect below easy to
// miss.
std::uint64_t ReminderIntervalFor(const AgeBand band)
{
	switch (band)
	{
		// Under13 gets half the base interval. Integer division on a 14-day
		// constant, so this is exactly 7 days with no rounding loss.
	case AgeBand::Under13:
		// Half the base cadence. A younger player needs a guardian present more
		// often, and this is also the band most likely to age out of it.
				// The two bands that share a cadence fall through to one return. Grouped
				// with no comment because the grouping is the statement.
		return Reminders::kIntervalSeconds / 2;

	case AgeBand::Age13To15:
	case AgeBand::Age16To17:
		return Reminders::kIntervalSeconds;

	case AgeBand::Adult18:
		// Nobody to remind. An adult with guardian settings has chosen them, and
		// nagging them about a choice they made is just friction.
				// Zero for adults, which the header defines as "never". Returning 0 rather
				// than a sentinel string means a caller that ignores it would divide by
				// zero -- but the contract is that 0 means never, so the caller must test
				// it.
		return 0;

	case AgeBand::Unknown:
	default:
		// No signal is the state most worth re-asking about, so remind at the
		// base cadence rather than suppressing.
		return Reminders::kIntervalSeconds;
	}
}

// The reminder predicate. Three unsigned parameters, no band -- which is
// the shape of the defect below.
bool ShouldRemind(const std::uint64_t now, const std::uint64_t lastAcknowledged, const bool hasAcknowledged)
{
	// The first prompt is not nagging -- nobody has had a chance to acknowledge
	// it yet.
		// Never acknowledged means always remind. Checked FIRST, before the clock
		// comparison, so a player who has never seen a prompt is not made to wait
		// out an interval measured from a lastAcknowledged of 0.
	if (!hasAcknowledged)
		return true;

	// A clock that moves backwards -- a user changing a device's date, an NTP
	// correction, a restore from backup -- would otherwise underflow the
	// subtraction and leave the player unreminded until the clock caught up.
	// Reminding is the fail-closed direction, so take it.
		// Clock-went-backwards guard, before the subtraction. Underflow on an
		// unsigned type wraps to a huge number, so without this a corrected clock
		// would suppress reminders for centuries.
	if (now < lastAcknowledged)
		return true;

		// DEFECT (documented, not fixed): THE BAND IS NOT CONSIDERED. This compares
		// against Reminders::kIntervalSeconds -- the flat base cadence -- and never
		// consults ReminderIntervalFor(). The consequence is that the whole switch
		// statement in ReminderIntervalFor has no effect on whether a reminder is
		// actually shown:
		//   - Under13 is documented as being reminded TWICE as often, and is not;
		//   - Adult18 is documented as never being reminded, and IS.
		// A search of the tree finds ReminderIntervalFor called only from
		// test_guardian_view.cpp, never from production code. The header presents
		// the two functions as a matched pair ("How often this band should be
		// reminded" and "Should a reminder be shown now?"), and the tests treat
		// them as independently correct, so neither reveals the gap.
		// Fixing it means adding a band parameter and routing the interval through
		// ReminderIntervalFor, plus the zero-means-never case for adults.
	//
		// The comparison is >= rather than >, so a reminder fires exactly on the
		// interval boundary rather than one second after it.
	return (now - lastAcknowledged) >= Reminders::kIntervalSeconds;
}

} // namespace Wasteland::Age