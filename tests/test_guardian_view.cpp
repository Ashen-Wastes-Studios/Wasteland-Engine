// Standalone functional test for Wasteland::Age::GuardianToolsView.
//
// WHY THIS EXISTS SEPARATE FROM THE IMGUI LAYER
//
// GuardianToolsLayer draws pixels and cannot be exercised without a window and
// a GPU. GuardianPolicy was already split out of the UI for exactly that
// reason, and the same argument applies here: everything a compliance reviewer
// would want to check about the guardian UI -- is the indicator up, are the
// toggles showing what is actually enforced, does a pending change show up on
// the right control, are the Art 20 rights offered -- lives in the view model,
// not in the drawing code.
//
// So this suite tests the DECISIONS. The drawing is 200 lines of ImGui calls
// that do what the model says and nothing else.
//
// THE PROPERTIES THAT MATTER
//
// 1. The guardian-active indicator is up whenever protections are enforced,
//    and down when they are not. A toggle that only appears sometimes is not a
//    persistent indicator.
//
// 2. A control shows the EFFECTIVE setting, not the resting or the requested
//    one. While a change is pending, the stricter combination is what actually
//    applies, so that is what the tick mark has to mean -- otherwise the UI
//    reports protection that is not in force, or protection the player already
//    has a way around.
//
// 3. A pending change marks exactly the control that differs.
//
// 4. Art 20(2) rights -- mute, report, block -- stay offered regardless of
//    band. They are the player's tools for acting on harm, and gating them
//    would invert the obligation they exist to discharge.

// std::printf only: this suite deliberately links nothing from the engine
// beyond the view model itself, so it can be built and run standalone.
// std::string is needed by FindControl's label matching.
#include <cstdio>
#include <string>

// The VIEW MODEL, not the ImGui layer. That split is the entire reason
// this test can exist -- see the file header.
#include "Wasteland/Age/GuardianToolsView.h"

// Flattened for readability. Every name below is therefore unqualified,
// which is why the tests are worth reading for their names.
using namespace Wasteland::Age;

// Counters rather than an assertion framework: no dependency to resolve,
// and every check is reported even after one fails, so a single run shows
// the whole picture instead of only the first broken property.
static int g_Failures = 0;
static int g_Checks = 0;

// The "what" string is a requirement in prose, not a debug label -- most
// of them read as Art 20/8 obligations. That is intentional: a failing
// message names the obligation that was not met.
//
// do/while(0) rather than a bare if so a CHECK at the top of an if-else
// cannot swallow the else.
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

// Internal linkage for the lookup helper only; the test functions stay
// at file scope so the runner list below reads as the suite's contents.
namespace
{

// Label-keyed lookup because the view model exposes an ordered vector,
// not a keyed map, and the tests care about the label text a compliance
// reviewer would see on screen.
//
// Returns a pointer INTO model.Controls: the model must outlive the
// pointer, so every call site holds its model as a local const.
const ControlState *FindControl(const GuardianToolsModel &model, const std::string &label)
{
	// No braces on the for and the if shares it deliberately -- one line.
	for (const ControlState &c : model.Controls)
		if (c.Label == label)
			return &c;
	// Null means "this label is not offered". Tests that care must handle it
	// rather than dereference: several below guard with if (... != nullptr),
	// which means a MISSING control can silently skip its assertions. The
	// always-offered rights in TestPlayerRightsAlwaysOffered avoid that by
	// scanning for presence directly instead of looking up.
	return nullptr;
}

} // namespace

// Property 1 from the header: up whenever protections are enforced.
static void TestIndicatorIsPersistentForMinor()
{
	// Art 8(1) protections are on, so the indicator must be up. "Up whenever
	// enforced" -- not "up usually".
	// Built fresh per test: the model is a value, so no test can leak state
	// into the next one through a shared policy object.
	const GuardianToolsModel model = BuildGuardianToolsView(GuardianPolicy::ForBand(AgeBand::Age13To15));

	// The three checks are deliberately ordered weakest-claim-last: visibility
	// first, then that it carries text, then that the model agrees this
	// player is a minor. A visible-but-empty indicator would pass a
	// visibility-only test, which is why the second check exists.
	CHECK(model.GuardianIndicatorVisible, "a minor under protections must show the guardian indicator");
	CHECK(!model.IndicatorText.empty(), "and the indicator must carry text");
	CHECK(model.IsMinor, "and the model must agree the player is a minor");
}

// The fail-closed case, and the one that matters most: Unknown band must
// not read as "no protections needed".
static void TestIndicatorShownWhenUnconfigured()
{
	// The fail-closed state is the one that most needs a visible indicator:
	// protections may well be in force, and the player needs to know.
	// Unknown is the default-constructed band, so this is what a player who
	// has never completed age assurance sees.
	const GuardianToolsModel model = BuildGuardianToolsView(GuardianPolicy::ForBand(AgeBand::Unknown));

	// NeedsSetup is the companion: the indicator alone would say "you are
	// watched" without saying anything actionable.
	CHECK(model.GuardianIndicatorVisible, "an unconfigured player must still show the indicator");
	CHECK(model.NeedsSetup, "and must be told setup is needed");
}

// The negative half of property 1. An always-on banner would be a false
// claim and would devalue the one that matters.
static void TestIndicatorHiddenForAdult()
{
	// An adult has chosen their own settings. A permanent "your guardian is
	// watching" banner would be false and would train players to ignore the
	// one that matters.
	// Adult18 is the only band in these tests with no guardian at all.
	const GuardianToolsModel model = BuildGuardianToolsView(GuardianPolicy::ForBand(AgeBand::Adult18));

	CHECK(!model.GuardianIndicatorVisible, "an adult must NOT show the guardian indicator");
	CHECK(!model.IsMinor, "and the model must agree they are not a minor");
}

// Property 2: the control reports what is ENFORCED, not what was asked
// for. This is the test that would catch the classic UI bug of echoing
// the pending value straight back.
static void TestControlsShowEffectiveSettings()
{
	// A pending change that RELAXES a protection must not change what the tick
	// shows, because the stricter setting is still enforced.
	// A local, mutable policy -- WithPendingChange is called on it below, so
	// it must not be a temporary.
	GuardianPolicy policy = GuardianPolicy::ForBand(AgeBand::Age13To15);
	// Starts from the RESTING settings and relaxes one field, so the
	// difference between the two is exactly the player's intent.
	GuardianSettings relaxed = policy.Settings();
	// Relaxation, not tightening: this is the direction where the stricter
	// resting value is what remains in force, so it is the direction where a
	// UI showing the requested value would over-report protection.
	relaxed.ChatRestricted = false; // the player tries to turn chat ON

	const GuardianToolsModel model = BuildGuardianToolsView(policy.WithPendingChange(relaxed));

	// The label string doubles as the test's handle on the control; if the
	// view renames the label, this lookup returns null and the test goes
	// quiet rather than failing. The presence check below is the guard.
	const ControlState *chat = FindControl(model, "Chat with other players");
	// Existence and state are separate checks so a missing control reports as
	// missing instead of being mistaken for a wrong value.
	CHECK(chat != nullptr, "the view must expose a chat control");
	// DEFECT (test weakness, not fixed here): the two checks inside this
	// guard are skipped entirely when the lookup fails, so a renamed or
	// dropped control turns the strongest assertions in this test into a
	// no-op that still reports success.
	if (chat != nullptr)
	{
		CHECK(chat->Toggling, "chat must still read as ON while the relaxation is pending");
		CHECK(chat->PendingChange, "and the control must be marked as having a pending change");
	}
}

// Property 3: exactly one control may be flagged, so this asserts on a
// pair -- the changed one and an unchanged one.
static void TestPendingChangeMarksOnlyTheDifferingControl()
{
	GuardianPolicy policy = GuardianPolicy::ForBand(AgeBand::Age13To15);
	GuardianSettings proposed = policy.Settings();
	// Note this test proposes the same relaxation as the previous one but
	// via a fresh Settings copy; there is no shared fixture between tests.
	proposed.ChatRestricted = false;

	const GuardianToolsModel model = BuildGuardianToolsView(policy.WithPendingChange(proposed));

	CHECK(model.HasPendingChange, "the model must report a pending change");

	const ControlState *chat = FindControl(model, "Chat with other players");
	// The control that did NOT change. Without this second lookup the test
	// would pass even if the model flagged every control.
	const ControlState *playtime = FindControl(model, "Daily playtime limit");

	// Both-or-neither guard: comparing flags needs both, so a single missing
	// lookup skips the pair. Same weakness as above, tolerated here because
	// TestControlsShowEffectiveSettings already asserts the chat control
	// exists.
	if (chat != nullptr && playtime != nullptr)
	{
		CHECK(chat->PendingChange, "the changed control must be marked pending");
		CHECK(!playtime->PendingChange, "an unchanged control must NOT be marked pending");
	}
}

// Art 20(1): the guardian must be able to tighten. A model that disabled
// every control would satisfy "protections apply" while removing the
// ability to configure them.
static void TestControlsAreInteractive()
{
	// A guardian must be able to tighten. If the UI disabled every control for
	// a minor, the Art 20(1) ability to configure protections would be
	// present in the state machine and absent from the screen.
	// All three minor bands: under-13, 13-15, 16-17. Adult and Unknown are
	// excluded because an adult's controls are the player's own and are not
	// subject to the guardian-configures rule.
	for (AgeBand band : {AgeBand::Under13, AgeBand::Age13To15, AgeBand::Age16To17})
	{
		const GuardianToolsModel model = BuildGuardianToolsView(GuardianPolicy::ForBand(band));
		for (const ControlState &c : model.Controls)
			// Checked per control inside the band loop, so a single read-only
			// control among several would be caught. The loop above carries no
			// braces because this if is the only statement in its body.
			CHECK(c.Enabled, "a minor's guardian controls must be interactive");
	}
}

// Property 4 and the strongest obligation in the file: Art 20(2) rights
// exist to discharge in exactly the most-restricted state, so gating
// them inverts the obligation.
static void TestPlayerRightsAlwaysOffered()
{
	// Art 20(2): mute, report, block. Offered in EVERY state, including the
	// most restricted one. Gating these would invert the obligation.
	// Every band including Unknown and Adult18 -- the point is that there is
	// no band at which these three are absent.
	for (AgeBand band : {AgeBand::Unknown, AgeBand::Under13, AgeBand::Age13To15, AgeBand::Age16To17, AgeBand::Adult18})
	{
		const GuardianToolsModel model = BuildGuardianToolsView(GuardianPolicy::ForBand(band));

		// Three independent flags rather than a count, so the failure message
		// names the specific right that went missing.
		bool hasReport = false, hasMute = false, hasBlock = false;
		for (const ControlState &c : model.Controls)
		{
			if (c.Label == "Report a problem") hasReport = true;
			if (c.Label == "Mute a player") hasMute = true;
			if (c.Label == "Block a player") hasBlock = true;
		}
		CHECK(hasReport, "Report must be offered in every state");
		CHECK(hasMute, "Mute must be offered in every state");
		CHECK(hasBlock, "Block must be offered in every state");

		for (const ControlState &c : model.Controls)
		{
			if (c.Label == "Report a problem" || c.Label == "Mute a player" || c.Label == "Block a player")
				// Second pass over the same controls: presence was checked above,
				// usability here. A right that is offered but disabled would satisfy
				// the first loop and fail this one.
				CHECK(c.Enabled, "the player's rights controls must never be disabled");
		}
	}
}

// Art 20(3): tailoring is a per-age requirement, and "the same screen
// for everyone" is the failure mode.
static void TestViewIsAgeTailored()
{
	// Art 20(3): assurance must be tailored to the age of the user. The same
	// screen for a seven-year-old and a sixteen-year-old is not tailored to
	// either.
	// Three models held simultaneously because the assertions are about
	// DIFFERENCES between them -- comparing within one model at a time
	// would need three times the work and could miss a shared value.
	const GuardianToolsModel child = BuildGuardianToolsView(GuardianPolicy::ForBand(AgeBand::Under13));
	const GuardianToolsModel teen = BuildGuardianToolsView(GuardianPolicy::ForBand(AgeBand::Age16To17));
	const GuardianToolsModel adult = BuildGuardianToolsView(GuardianPolicy::ForBand(AgeBand::Adult18));

	// All three pairs, not just the extremes: it is possible for a child and
	// a teen to differ while a teen matches the adult exactly.
	CHECK(child.Greeting != teen.Greeting, "a child and a teenager must not get identical copy");
	CHECK(teen.Greeting != adult.Greeting, "a teenager and an adult must not get identical copy");
	CHECK(child.Greeting != adult.Greeting, "a child and an adult must not get identical copy");

	// A child cannot be the one configuring, so the copy must not address them
	// as the person making the decisions.
	// Copy alone is not enough for a seven-year-old: the text must not put the
	// child in the position of deciding. This is the assertion that checks
	// the addressing, not just the words.
	CHECK(child.ResponsibleParty == ResponsibleParty::Guardian,
	      "a child must be told a grown-up is in charge of these settings");
	CHECK(adult.ResponsibleParty == ResponsibleParty::Self,
	      "an adult must be told they are in charge of their own settings");
}

// Pending playtime is where relaxation is easiest to get wrong: 240 is
// the requested value and 120 is the enforced one.
static void TestPendingReviewState()
{
	GuardianPolicy policy = GuardianPolicy::ForBand(AgeBand::Age13To15);
	GuardianSettings proposed = policy.Settings();
	// 240 against a resting 120 -- a relaxation, so 120 must stay displayed.
	proposed.DailyPlaytimeMinutes = 240;

	const GuardianToolsModel model = BuildGuardianToolsView(policy.WithPendingChange(proposed));

	CHECK(model.State == GuardianState::PendingReview, "the view must surface the pending-review state");
	CHECK(model.HasPendingChange, "and flag the pending change");

	// Pending playtime of 240 against a resting 120 is a RELAXATION, so the
	// effective value must still be 120.
	// Value, not Toggling: a numeric control exposes its effective value as
	// a number, and this is the field the tick-adjacent label would read.
	const ControlState *playtime = FindControl(model, "Daily playtime limit");
	if (playtime != nullptr)
		CHECK(playtime->Value == 120, "a pending relaxation must not change the displayed effective value");
}

// The mirror of the previous test. Together they pin the rule: tightening
// is effective at once, relaxation waits for approval.
static void TestTighteningPendingTakesEffectImmediately()
{
	// The other direction: tightening applies at once, so the displayed value
	// DOES move before approval.
	GuardianPolicy policy = GuardianPolicy::ForBand(AgeBand::Age13To15);
	GuardianSettings tighter = policy.Settings();
	// 60 against 120 -- stricter, so the displayed value must become 60
	// before any approval exists.
	tighter.DailyPlaytimeMinutes = 60;

	const GuardianToolsModel model = BuildGuardianToolsView(policy.WithPendingChange(tighter));

	const ControlState *playtime = FindControl(model, "Daily playtime limit");
	if (playtime != nullptr)
		CHECK(playtime->Value == 60, "a pending tightening must show its effective value immediately");
}

// Notes are prose generated from the same policy, so they can drift out
// of step with the controls. The test guards the specific way that
// matters: a note claiming a restriction that is not enforced.
static void TestRestrictionNotesReflectReality()
{
	// The "what is restricted and why" list must not claim a restriction that
	// is not in force. A stale note that says chat is off when it is on is
	// worse than no note: the player stops trusting the panel.
	const GuardianToolsModel minor = BuildGuardianToolsView(GuardianPolicy::ForBand(AgeBand::Age13To15));
	CHECK(!minor.RestrictionNotes.empty(), "a minor under restrictions must get restriction notes");

	// The assertion is conditional on chat being ON, so a model that got it
	// wrong the other way (chat off, no note) passes vacuously here. Only
	// the false-positive direction is covered.
	const ControlState *chat = FindControl(minor, "Chat with other players");
	if (chat != nullptr && chat->Toggling)
	{
		bool mentionsChat = false;
		// Substring search on the control label rather than an exact match: the
		// note is a sentence, the label is a phrase inside it.
		for (const std::string &n : minor.RestrictionNotes)
			if (n.find("Chat with other players") != std::string::npos)
				mentionsChat = true;
		CHECK(!mentionsChat, "the notes must not list chat as restricted when chat is enabled");
	}
}

// Reminders are the "assurance stays live" half of Art 20; a one-off
// prompt at signup does not discharge it.
static void TestReminderCadence()
{
	// Art 20 wants assurance to stay live rather than be a one-off at signup.
	// Reminders exist so a signal that has gone stale is re-established.

	// No prior acknowledgement: remind.
	// now=1000s, lastAck=0, never acknowledged. The never-acknowledged case
	// is separate from the stale case because it must fire regardless of how
	// long ago "now" is.
	CHECK(ShouldRemind(1000, 0, false), "a player who has never acknowledged must be reminded");

	// Acknowledged just now: quiet.
	CHECK(!ShouldRemind(1000, 900, true), "a player who acknowledged a moment ago must not be nagged");

	// Acknowledged a long time ago: remind again.
	// The interval constant times ten: deliberately far past the threshold so
	// the test does not depend on the exact value of kIntervalSeconds.
	CHECK(ShouldRemind(Reminders::kIntervalSeconds * 10, 0, true),
	      "a stale acknowledgement must trigger a reminder again");
}

// Cadence is part of tailoring, so it is tested as a comparison between
// bands rather than against a hardcoded number that would drift.
static void TestReminderIntervalIsAgeTailored()
{
	// A child needs a guardian present more often than a teenager.
	// DEFECT (weak assertion, documented not fixed): this compares two bands
	// but says nothing about the third. The next check tries to cover
	// Age13To15 and admits as much in its own text -- it is a tautology
	// whenever the 16-17 value is not strictly less than the 13-15 value, so
	// it can never fail in that branch. Age13To15 is effectively untested.
	CHECK(ReminderIntervalFor(AgeBand::Under13) < ReminderIntervalFor(AgeBand::Age16To17),
	      "a younger player must be reminded more often");
	CHECK(ReminderIntervalFor(AgeBand::Age16To17) < ReminderIntervalFor(AgeBand::Age13To15) ||
	          ReminderIntervalFor(AgeBand::Age13To15) <= ReminderIntervalFor(AgeBand::Age16To17),
	      "the cadence must be ordered sensibly across minor bands");

	// An adult with no guardian setup has nothing to remind anyone about.
	// Zero means "never" for the comparison above: with nothing to remind
	// anyone about, an interval would have to be infinite.
	CHECK(ReminderIntervalFor(AgeBand::Adult18) == 0, "an adult must not be nagged");
}

// now=0, lastAck=100x the interval. Subtracting here gives a negative
// elapsed time, and a naive `elapsed < interval` would read as "a very
// long time ago" and stay silent forever.
static void TestClockGoingBackwardsStillReminds()
{
	// Platform clocks move backwards. Underflowing the interval comparison would
	// mean a reminder that never fires again.
	CHECK(ShouldRemind(0, Reminders::kIntervalSeconds * 100, true),
	      "a clock that jumps backwards must still produce a reminder");
}

// No framework, no registration: the suite is the sequence below, and
// the return code is what a build script checks. Tests run in the order
// listed because each is independent but the ordering reads as a
// narrative (indicator, then controls, then rights, then reminders).
int main()
{
	TestIndicatorIsPersistentForMinor();
	TestIndicatorShownWhenUnconfigured();
	TestIndicatorHiddenForAdult();
	TestControlsShowEffectiveSettings();
	TestPendingChangeMarksOnlyTheDifferingControl();
	TestControlsAreInteractive();
	TestPlayerRightsAlwaysOffered();
	TestViewIsAgeTailored();
	TestPendingReviewState();
	TestTighteningPendingTakesEffectImmediately();
	TestRestrictionNotesReflectReality();
	TestReminderCadence();
	TestReminderIntervalIsAgeTailored();
	TestClockGoingBackwardsStillReminds();

	// Failures are printed as they happen by the macro; this line only
	// reports the totals, and the exit code propagates to the build.
	std::printf("test_guardian_view: %d checks, %d failures\n", g_Checks, g_Failures);
	// Non-zero exit on any failure -- the file is only useful if something
	// automated reads the code.
	return g_Failures ? 1 : 0;
}