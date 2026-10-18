// This test is also why GuardianPolicy has no inline test hooks: every accessor
// below is public, so the state machine can be driven from outside exactly the
// way the engine drives it. Nothing here reaches into privates.
// Standalone functional test for Wasteland::Age::GuardianPolicy.
//
// GuardianPolicy is the state machine behind Art 20 of the EU KIDS Act
// proposal (COM(2026) 681, 2026/0286(COD)), which requires that the assurance
// solution be "easy to use, accessible and activate-able" and that the
// resulting settings be "tailored to the age of the user". A state the
// guardian can actually reach and change is the whole point -- a purely
// automatic profile gives a parent nothing to do.
//
// The rule this suite exists to protect is the PENDING REVIEW rule. A
// guardian may propose a change to a minor's settings, but the settings that
// apply WHILE that change is under review must be the most restrictive
// combination of the current and proposed values -- never the more
// permissive one.
//
// The reason is specific and ugly: if a pending relaxation took effect
// immediately, then flipping a switch would disable protection for as long as
// it took to notice, and reverting it would be the act that re-enabled
// protection. A review state that briefly lowers the floor is not a review
// state, it is a bypass. This is pinned by tests throughout.

// The ONLY standard include. No <string>, no vector, no test framework.
//
// That is a deliberate constraint of the design rather than an omission: the
// suite runs with nothing but the C runtime, so it links in any build step that
// can compile AgeBand.h and GuardianPolicy.h -- including one with no engine, no
// window and no graphics context.
#include <cstdio>

// For AgeBand and AgeBand::Unknown. Included independently of GuardianPolicy.h,
// which includes it too -- harmless (the header is #pragma once), and it
// documents that this test depends on both modules rather than only on the one
// under test.
#include "Wasteland/Age/AgeBand.h"
// GuardianPolicy, GuardianSettings, GuardianState -- the module under test.
//
// Pulls in AgeBand.h transitively, so the include above is technically redundant.
// Including it anyway means the test would keep compiling if GuardianPolicy ever
// stopped depending on AgeBand, which is what you want from a test.
#include "Wasteland/Age/GuardianPolicy.h"

// A using-DIRECTIVE, so GuardianPolicy/GuardianSettings/GuardianState are
// unqualified throughout. The only one in the file; the test has no namespace of
// its own, so main() and every Test function live at global scope.
using namespace Wasteland::Age;

// The shared counters every CHECK increments.
//
// `static` at namespace scope gives INTERNAL linkage, which matters more here
// than it looks: it means test_guardian_policy.cpp and test_age_band.cpp can both
// declare g_Failures and still link into one binary without an ODR violation or
// a duplicate-symbol error. Each translation unit gets its own counter. Copying
// this pair into a third test file is therefore safe, though DRY would be nicer.
//
// Globals rather than a fixture because CHECK is a function-like macro with no
// way to reach an instance.
static int g_Failures = 0;
// The pass counter, reported in the summary line so "all passed" is
// distinguishable from "nothing ran".
//
// Unlike test_age_band.cpp, every increment here happens through the CHECK
// macro, so this figure IS a count of assertions -- with one exception noted at
// TestUnlimitedIsZero.
static int g_Checks = 0;

// The single assertion macro, and the whole harness is this plus a main().
//
// do { } while (0) rather than a bare `if`: the do-while makes the macro one
// STATEMENT, so `if (x) CHECK(a, b); else ...` compiles and the else binds to
// the caller's if instead of dangling off the macro's own brace.
//
// Every line ends with a backslash continuation. DEFECT (fragility): inserting a
// comment on an interior line WITHOUT its own trailing backslash silently
// truncates the macro, and the remainder of the body becomes stray text at file
// scope -- a wall of backslash-nonsense the compiler reports somewhere unrelated.
// There is deliberately no comment inside the macro body for that reason.
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

// The two fixture builders below, and they are the load-bearing part of the
// whole suite: every review-state test is a merge of a band baseline with one of
// these, so their extremeness is what makes the assertions unambiguous.
//
// "Clearly looser" is a requirement, not a style note. If Relaxed() were only
// marginally less restrictive than the band baseline, a merge that quietly did
// nothing would still pass every test.
// A set of settings that is clearly looser than the restrictive defaults, so
// a pending change built from it is unambiguously a relaxation.
// Returns by VALUE, and that is forced: GuardianSettings is copy-constructible and
// freely assignable (unlike GuardianPolicy), so a plain struct return is the
// natural interface.
//
// No parameters -- the fixture is a constant, and taking one would suggest it
// parameterises anything.
static GuardianSettings Relaxed()
{
	// DEFAULT-CONSTRUCTED, not zero-initialised, and the distinction is the point.
	//
	// GuardianSettings gives every member a restrictive default (flags true, curfew
	// 21, playtime 60). So `GuardianSettings s;` starts maximally restrictive, and
	// each line below RELAXES exactly one field. Writing every field explicitly would
	// be equally correct but would hide which fields the test considers loose -- and
	// a field added to the struct later would then default to restrictive here
	// accidentally, quietly weakening the fixture.
	GuardianSettings s;
	// Night hours off. Against a minor's baseline this is a relaxation, and StricterOf
	// ORs the two flags, so the effective value stays TRUE -- the assertion that
	// guards this is in TestPendingReviewHoldsTheStricterSettings.
	s.NightHoursEnforced = false;
	// Curfew at 23, the latest possible hour.
	//
	// DEFECT (fixture does not set NightHoursEnforced's interaction, but that is not
	// a fixture bug -- note the value): CurfewHour is documented in the header as
	// only meaningful while NightHoursEnforced is true, and Relaxed() turns enforcement
	// off. So the curfew here is inert data -- which is exactly why the test that
	// checks it compares EFFECTS rather than the field directly.
	//
	// The field is still exercised: StricterOf takes a plain min() on it
	s.CurfewHour = 23;
	// Activity reporting off -- the second relaxation the review-state test guards.
	s.ReportActivityToGuardian = false;
	// Purchase limits off. The third.
	s.PurchaseLimitsEnforced = false;
	// Playtime to kUnlimited, which IS 0. The comment names the sentinel rather than
	// leaving the reader to find it.
	//
	// This is the field with no single "stricter" direction: 0 is the LOOSEST value
	// here, the opposite of a plain min(). GuardianPolicy.cpp's StricterOf has three
	// branches for exactly this reason, and the assertion that a pending unlimited
	// proposal does not remove the limit is the one in
	// TestPendingReviewHoldsTheStricterSettings.
	s.DailyPlaytimeMinutes = 0; // unlimited
	// Chat unrestricted. The fourth and last flag, so all four protective bools are
	// relaxed -- the OR-merge in StricterOf is exercised on every one of them.
	s.ChatRestricted = false;
	return s;
}

// The mirror fixture, and the one that makes the merge test bidirectional.
//
// Without it, a StricterOf implementation that simply returned `current`
// unconditionally would pass every relaxation test. This is what rules that out.
// Clearly tighter than the defaults.
// The tightening fixture. Same value-returning shape as Relaxed().
//
// "Clearly tighter than the defaults" -- but note it is NOT tighter than every
// minor BAND baseline, only than the struct defaults. Against a 16-17 minor's
// 180-minute allowance, Tightened()'s 30 is a tightening; against Under13's
// 19:00 curfew, Tightened()'s 19 is a TIE, not a tightening.
//
// That matters for which tests use it. The tightening test drives a 16-17 policy,
// where every field genuinely moves stricter. Using Under13 would make the curfew
// assertion below vacuous -- it would pass for a merge that ignored the pending
// curfew entirely.
static GuardianSettings Tightened()
{
	GuardianSettings s;
	s.NightHoursEnforced = true;
	// 19:00, earlier than the 21:00 struct default and therefore stricter than every
	// minor band's baseline (Under13 also uses 19, so it only ties there).
	//
	// The assertion that this value reaches enforcement is the one comparing against
	// the literal 19 in TestPendingReviewOfATighteningTakesEffect.
	s.CurfewHour = 19;
	s.ReportActivityToGuardian = true;
	s.PurchaseLimitsEnforced = true;
	// 30 minutes, far below every band baseline (60 / 120 / 180), so the min()
	// "both sides have a real limit" branch of StricterOf is the one exercised -- the
	// branch where the sentinel logic does not apply.
	s.DailyPlaytimeMinutes = 30;
	s.ChatRestricted = true;
	return s;
}

// The fail-closed baseline: what a studio gets before any age signal exists.
//
// `static` throughout, as in the sibling test -- internal linkage, and it makes
// an unwritten test a link error at main() rather than a silent skip.
static void TestDefaultIsUnconfiguredAndRestrictive()
{
	// Two properties are being asserted together, and they are related: the policy is
	// restrictive AND it says so. A default that quietly enforced adult defaults would
	// be the worst failure in the module, and a default that was restrictive but
	// reported Minor would be nearly as bad -- the UI would never prompt.
	// The policy a studio gets before any age signal exists. It must not be
	// usable as an adult configuration, and it must not be silent about
	// being unconfigured -- State() has to report that so the UI can prompt.
	// Default-constructed in place, no argument.
	//
	// `const` because nothing here mutates it -- and because GuardianPolicy deletes
	// both assignment operators, const is not incidental: it documents that this
	// object is a snapshot the test only reads.
	//
	// GuardianPolicy() is `= default` in the header (Unconfigured with restrictive
	// settings), so this exercises the member initialisers rather than a code path.
	const GuardianPolicy fresh;

	// State() first, because everything else in the module keys off it: AgeBand.h and
	// GuardianPolicy.cpp both repeat that Unconfigured is never a synonym for Adult.
	//
	// One CHECK per property rather than a combined condition, so a failure names
	// WHICH invariant broke instead of printing a single opaque false.
	CHECK(fresh.State() == GuardianState::Unconfigured, "the default policy must be Unconfigured");
	// No pending change on a fresh object, which pins m_HasPending's initialiser.
	//
	// Worth its own line because the failure it would catch is subtle: a default with
	// m_HasPending true would make EffectiveSettings() run the merge path against a
	// default-constructed m_PendingSettings -- all-restrictive -- and coincidentally
	// produce almost the right answer for most fields.
	CHECK(!fresh.HasPendingChange(), "the default policy must have no pending change");

	// Restricted() is the same thing by name, and must agree.
	// The named factory must agree with the default constructor.
	//
	// Restricted() is ForBand(Unknown) in GuardianPolicy.cpp, so this is really an
	// assertion about the enum: that AgeBand::Unknown maps to Unconfigured. It is
	// the first of several -- TestUnknownIsNeverAdult is the thorough version, and it
	// also checks the settings, not just the state.
	//
	// A temporary bound to a const member call: safe, because the temporary outlives
	// the full expression.
	CHECK(GuardianPolicy::Restricted().State() == GuardianState::Unconfigured,
	      "Restricted() must also be Unconfigured");

	// The settings snapshot, taken ONCE.
	//
	// Settings() is the accessor that IGNORES any pending change -- the header is
	// emphatic that EffectiveSettings() is the enforcement entry point and this is
	// not. With nothing pending they are equal, so this is safe to assert against.
	//
	// Returning by value (a small struct copy) rather than by reference, per the
	// header's rationale: a copy beats the aliasing a reference would invite.
	const GuardianSettings settings = fresh.Settings();
	// Four flag assertions, one per line, each with its own message.
	//
	// Individually named rather than combined into `if (!a || !b || !c) CHECK(...)`,
	// because a combined assertion tells you the group failed and nothing about which
	// field -- and these four are the entire definition of "restrictive".
	CHECK(settings.NightHoursEnforced, "unconfigured must enforce night hours");
	CHECK(settings.ReportActivityToGuardian, "unconfigured must report activity to the guardian");
	CHECK(settings.PurchaseLimitsEnforced, "unconfigured must enforce purchase limits");
	CHECK(settings.ChatRestricted, "unconfigured must restrict chat");
	// The inequality form of the check, and it is the right way round.
	//
	// Asserting `== 60` would pin the exact default and fail the moment the default is
	// legitimately tightened further. Asserting `!= kUnlimited` states the actual
	// requirement -- there is a limit -- and stays true if 60 becomes 45.
	//
	// The pre-existing comment above it spells out that kUnlimited is 0, so the test is
	// guarding against the sentinel being read as the permissive value.
	// Note the inequality: unconfigured must NOT be unlimited. The default of
	// 60 minutes is the restrictive value; kUnlimited (0) would be the
	// generous one.
	// The playtime assertion, continued onto its own line because the message is long.
	//
	// Checked as an inequality against the sentinel rather than against a number, so
	// it survives a change to the restrictive default. Same reasoning as the curfew
	// above.
	CHECK(settings.DailyPlaytimeMinutes != GuardianSettings::kUnlimited,
	      "unconfigured must NOT start with a generous playtime allowance");
}

// Pins the sentinel's VALUE, which the rest of the module's correctness rests
// on -- and does it at COMPILE time.
static void TestUnlimitedIsZero()
{
	// The pre-existing comment explains the asymmetry: 0 is a legitimate curfew hour
	// but not a legitimate 0-minute playtime allowance.
	//
	// That is exactly why playtime is the field with no single stricter-direction:
	// the same 0 means "midnight" in one field and "no limit" in the other, so any
	// shared numeric comparison across the two would be wrong.
	// A sentinel that has to be explicit, because 0 is a legitimate curfew
	// hour but not a legitimate 0-minute playtime.
	// A COMPILE-TIME assertion, not a CHECK.
	//
	// Correct choice: if kUnlimited were ever changed to -1, every runtime path in the
	// module that compares against it -- StricterOf's three-branch merge, the
	// Restricted() defaults, the adult band -- would still COMPILE and produce wrong
	// answers. A CHECK here would catch it only after the suite ran; static_assert
	// makes it a build failure.
	//
	// So this is the one test in the file that cannot fail at runtime.
	static_assert(GuardianSettings::kUnlimited == 0, "kUnlimited must be 0 so a zero playtime reads as unlimited");
	// DEFECT (misleading bookkeeping): a lone counter increment with no assertion,
	// purely so the static_assert above contributes to the summary count.
	//
	// It cannot fail -- static_assert already failed the build if it were going to --
	// so incrementing g_Checks overstates it by one and makes the summary read as if
	// a runtime check ran. Harmless, but the check count is not quite an assertion
	// count.
	++g_Checks;
}

// The band -> state mapping, one CHECK per band and no loop.
//
// Written out rather than table-driven so that adding a band forces a visible
// edit here. A loop over a table of {band, state} pairs would be shorter but could
// be extended without anyone noticing a missing case -- the same trade the module
// itself makes in StateForBand.
static void TestBandSelectsState()
{
	// Adult18 -> Adult, the ONLY band that produces a guardian-free state.
	//
	// The single most consequential mapping in the module: everything else fails
	// closed, so this is where a wrong case would be a real compliance failure.
	CHECK(GuardianPolicy::ForBand(AgeBand::Adult18).State() == GuardianState::Adult,
	      "a verified adult is Adult");

	CHECK(GuardianPolicy::ForBand(AgeBand::Age16To17).State() == GuardianState::Minor,
	      "16-17 is a configured Minor");

	CHECK(GuardianPolicy::ForBand(AgeBand::Age13To15).State() == GuardianState::Minor,
	      "13-15 is a configured Minor");

	// Under13 -> MinorNeedsSetup, the one band that is a minor AND not yet
	// configured.
	//
	// The separate state exists because a downstream `state == Minor` check would
	// otherwise treat an un-walked-through under-13 as fully set up. Test
	// MinorNeedsSetupIsNotFullyConfigured below makes that distinction explicit.
	CHECK(GuardianPolicy::ForBand(AgeBand::Under13).State() == GuardianState::MinorNeedsSetup,
	      "under 13 needs guardian setup before it is fully configured");
}

// The recurring bug class, tested against values that are not even in the enum.
//
// This is the most important test in the file after the review-state ones: an
// absent age signal must never produce adult treatment, and a missing or
// out-of-range band value is the realistic way that happens.
static void TestUnknownIsNeverAdult()
{
	// The pre-existing comment names the three-state requirement: Unknown must be
	// neither Adult nor a CONFIGURED minor, because "we do not know yet" and "we
	// know, and the protections are configured" are different facts a parent has to
	// be told differently.
	//
	// A policy that collapsed them would not merely be imprecise -- the UI would
	// never prompt for verification.
	// The recurring bug class across this module. No age signal must not mean
	// an adult, and must not mean a configured minor either -- it means the
	// policy does not know yet, which is its own state.
	// Three inputs: the declared Unknown, plus two values cast from ints that are
	// not enumerators at all.
	//
	// The two casts are the interesting part -- they simulate a band that arrived
	// from outside, or from a build whose AgeBand enum has values this build does
	// not. GuardianPolicy::StateForBand has no `default:` of its own beyond the arm
	// shared with Unknown, so an unrecognised value lands on Unconfigured.
	//
	// static_cast<AgeBand>(99) rather than (AgeBand)99 to make the intent explicit;
	// the C-style cast would also compile.
	for (AgeBand band : {AgeBand::Unknown, static_cast<AgeBand>(99), static_cast<AgeBand>(-5)})
	{
		// ForBand is the only way to build a policy, so this is the constructor path.
		//
		// `const` per iteration, and it is a fresh object each time -- there is no static
		// state in ForBand to leak between iterations, which the value semantics in the
		// header guarantee.
		const GuardianPolicy policy = GuardianPolicy::ForBand(band);
		// State first, for every one of the three inputs.
		//
		// Repeated inside the loop rather than hoisted, so a failure reports which input
		// failed only via the message -- DEFECT (weak diagnostics): the message does not
		// include the band's numeric value, so a reader sees the same three identical FAIL
		// lines and has to infer which value was at fault by reasoning about the list
		// above. Printing the cast value would make this self-diagnosing.
		CHECK(policy.State() == GuardianState::Unconfigured,
		      "an unknown band must yield Unconfigured, never Adult or Minor");
		// One flag, not all four, and not the playtime.
		//
		// DEFECT (weak coverage): Restrictive-SettingsForBand falls out of the switch
		// unchanged for Unknown, so the four flags and the playtime default are all set
		// by GuardianSettings' member initialisers rather than by GuardianPolicy code.
		// Checking one flag is a reasonable smoke test of that path, but it would not
		// catch a change to any of the other three defaults -- which is the same class of
		// gap TestDefaultIsUnconfiguredAndRestrictive covers for the default-constructed
		// case.
		CHECK(policy.Settings().NightHoursEnforced, "an unknown band must keep night-hour enforcement");
	}
}

// Entering review: the transition, and the guarantee that it did not mutate the
// source.
static void TestProposedChangeEntersReview()
{
	// A 16-17 minor: configured, so `minor` is a policy a guardian could propose a
	// change to.
	//
	// Age16To17 rather than Under13 because these tests assert the state returns to
	// Minor afterwards, and Under13 would return to MinorNeedsSetup -- a different
	// expectation. This is a band choice with a purpose.
	const GuardianPolicy minor = GuardianPolicy::ForBand(AgeBand::Age16To17);
	CHECK(!minor.HasPendingChange(), "a fresh minor policy has nothing pending");

	// The proposal, taking a relaxation.
	//
	// const on the RESULT, and that is the test's subject as much as the state:
	// WithPendingChange is declared const and returns a new GuardianPolicy by value.
	// There is no in-place variant, which is the header's stated reason there is no
	// window in which a live policy is briefly permissive -- no such transition
	// exists to have a window in.
	const GuardianPolicy reviewing = minor.WithPendingChange(Relaxed());

	// m_HasPending is now true, and State() is PendingReview. Checked separately
	// because they are separate fields with separate failure modes.
	//
	// DEFECT (redundant, harmless): HasPendingChange() is exactly `m_HasPending`, and
	// State() == PendingReview is set unconditionally alongside it in the same
	// function -- so these two assertions can never disagree. They document intent
	// rather than probe distinct behaviour.
	CHECK(reviewing.HasPendingChange(), "proposing a change must create a pending change");
	CHECK(reviewing.State() == GuardianState::PendingReview, "and must enter PendingReview");

	// The source-unchanged assertions, and these are the ones that would catch a
	// future refactor to a mutable state machine.
	//
	// Worth noting what makes this checkable rather than aspirational: GuardianPolicy
	// DELETES both assignment operators and keeps only its copy constructor. There is
	// no API through which `minor` could be modified after construction.
	// The original is untouched -- the value type means no in-place mutation
	// can loosen a live policy behind a caller's back.
	CHECK(!minor.HasPendingChange(), "the source policy must be unchanged");
	CHECK(minor.State() == GuardianState::Minor, "and must still be Minor");
}

// THE rule. If this test fails, the module has a bypass.
//
// The file header's reasoning is the whole point: a review state that briefly
// lowers the floor is not a review state, it is a bypass, because reverting the
// change would be the act that re-enabled protection.
static void TestPendingReviewHoldsTheStricterSettings()
{
	// The pre-existing comment, restated as the assertion set below: all four
	// protective flags hold, the curfew does not move later, and the playtime limit
	// is not removed.
	// THE rule. While a relaxation is under review, protection stays.
	const GuardianPolicy minor = GuardianPolicy::ForBand(AgeBand::Age16To17);
	const GuardianPolicy reviewing = minor.WithPendingChange(Relaxed());

	// EffectiveSettings, NOT Settings().
	//
	// This is the distinction the header calls out as the enforcement entry point:
	// Settings() would ignore the pending change entirely and return the baseline,
	// which would make this test PASS for the wrong reason.
	//
	// The merge is StricterOf(current, pending) in GuardianPolicy.cpp, and the
	// argument order is load-bearing -- OR is commutative so the flags would look
	// right either way, but swapping them would invert the curfew and playtime.
	const GuardianSettings effective = reviewing.EffectiveSettings();

	// Four flag assertions against a relaxation that turned all four off.
	//
	// Each message names WHICH relaxation is being resisted, rather than a generic
	// "flag must hold" -- so a failure points at the specific merge line to read.
	CHECK(effective.NightHoursEnforced, "a pending curfew relaxation must NOT lift enforcement during review");
	CHECK(effective.ReportActivityToGuardian, "a pending report relaxation must NOT lift reporting during review");
	CHECK(effective.PurchaseLimitsEnforced, "a pending purchase-limit relaxation must NOT lift limits during review");
	CHECK(effective.ChatRestricted, "a pending chat relaxation must NOT lift chat restriction during review");
	// The curfew, asserted as an INEQUALITY against the baseline rather than an
	// equality against 21.
	//
	// Deliberate: the requirement is "must not move later", which <= expresses
	// directly. Asserting `== 21` would be stricter than the requirement and would
	// fail if the band baseline changed. It also permits a tightening through review,
	// which is the correct behaviour and is what the next test covers.
	//
	// The baseline is read from minor.Settings() rather than hardcoded, so the
	// assertion tracks the band table if it changes.
	CHECK(effective.CurfewHour <= minor.Settings().CurfewHour,
	      "a pending later-curfew change must not move the effective curfew later");
	// The playtime, and the field with no single stricter-direction.
	//
	// Equality rather than <=, and that is necessary rather than incidental: the
	// pending value is kUnlimited, so `<=` would be satisfied by 0, which is the
	// OPPOSITE of the intent. A plain <= here is exactly the bug StricterOf's
	// sentinel branch exists to avoid -- taking the min of 180 and 0 gives 0, a player
	// with no minutes allowed.
	//
	// This is the assertion that distinguishes a three-branch merge from a naive one.
	CHECK(effective.DailyPlaytimeMinutes == minor.Settings().DailyPlaytimeMinutes,
	      "a pending unlimited-playtime change must not remove the limit during review");
}

// The other direction, and without it the rule could be satisfied by a merge that
// simply ignores the pending change.
//
// A relaxation-only suite would pass against `return current;` -- the strictest
// possible reading. This is what rules that out.
static void TestPendingReviewOfATighteningTakesEffect()
{
	// The pre-existing comment gives the rationale: a merge that always favoured the
	// CURRENT settings would make tightening impossible without restarting the game.
	//
	// So the rule is symmetric -- "most restrictive of the two", not "the current
	// one" -- and the asymmetry lives only in which side happens to be stricter.
	// The other direction. If a guardian TIGHTENS settings, the stricter value
	// is the pending one -- so the merge must yield the pending value, not
	// the current one. A merge that always favoured the current settings
	// would make tightening impossible without a restart.
	// 16-17 again, and the choice is load-bearing rather than incidental.
	//
	// This band has a 180-minute allowance, so Tightened()'s 30 is a real tightening.
	// Against Under13 -- whose curfew is also 19, the same value Tightened() uses --
	// the curfew assertion would compare 19 to 19 and pass for a merge that ignored
	// the pending curfew entirely. The curfew assertion below would be vacuous.
	const GuardianPolicy minor = GuardianPolicy::ForBand(AgeBand::Age16To17);
	const GuardianPolicy reviewing = minor.WithPendingChange(Tightened());

	const GuardianSettings effective = reviewing.EffectiveSettings();

	// Equality against the LITERAL, not against Tightened().CurfewHour.
	//
	// Hardcoding is the right call here and the opposite of the previous test: the
	// point is that the specific value 19 -- which the fixture set -- reaches
	// enforcement. Comparing against the fixture instead would pass even if the merge
	// substituted a different tightening value.
	//
	// It does mean the test and the fixture must be edited together; a fixture change
	// to 20 would surface here as a failure, which is the intended behaviour.
	CHECK(effective.CurfewHour == 19, "a pending earlier curfew must take effect during review");
	// The playtime, which now IS a plain min(): both sides carry real limits, so
	// 30 is genuinely the stricter of 30 and 180.
	//
	// Same literal-not-fixture reasoning as the curfew above.
	CHECK(effective.DailyPlaytimeMinutes == 30, "a pending shorter playtime must take effect during review");
	// Enforcement re-asserted even though the fixture keeps it true.
	//
	// DEFECT (vacuous assertion): Tightened() sets NightHoursEnforced = true and a 16-17
	// baseline is also true, so `true || true` is true for any implementation that
	// ORs -- and an implementation that returned `pending` verbatim would also pass.
	// The OR half of the merge is genuinely covered by the RELAXATION test above;
	// this line adds nothing beyond confirming the merge did not break the value.
	CHECK(effective.NightHoursEnforced, "and enforcement must stay on");
}

// The approve path: the pending change becomes the settings in force.
static void TestApproveAppliesThePendingChange()
{
	const GuardianPolicy minor = GuardianPolicy::ForBand(AgeBand::Age16To17);
	const GuardianPolicy reviewing = minor.WithPendingChange(Relaxed());
	// Approved() on a const reviewing -- a const member function returning a new
	// policy by value.
	//
	// Chainable in one expression in principle; written as a named local so the
	// result survives for the four assertions below.
	const GuardianPolicy approved = reviewing.Approved();

	// The pending flag is cleared, and the state leaves PendingReview.
	//
	// m_HasPending is assigned LAST in Approved() in GuardianPolicy.cpp, so the
	// returned object is a settled one: band-derived state, approved settings,
	// nothing pending. The two assertions below check the other two halves of that.
	CHECK(!approved.HasPendingChange(), "approval must clear the pending change");
	// != rather than == Minor, which is a weaker but sufficient claim.
	//
	// The exact state IS known here -- the band is 16-17, so it must be Minor -- and
	// TestDenyRevertsToTheOriginal asserts exactly that for the deny path. So the two
	// tests are asymmetric in strictness for no stated reason. A == Minor here would
	// pin the same property this test is really about: that approving returns to the
	// BAND's state rather than to some state that merely is not review.
	CHECK(approved.State() != GuardianState::PendingReview, "approval must leave PendingReview");
	// Settings() is correct on the APPROVED policy -- there is no longer a pending
	// change to ignore.
	//
	// The assertion that an approved relaxation actually takes effect. Without it,
	// approve could be implemented as "discard everything and return Restricted()"
	// and the first two checks would still pass.
	CHECK(approved.Settings().DailyPlaytimeMinutes == GuardianSettings::kUnlimited,
	      "an approved relaxation must actually take effect afterwards");
	// A second field, to show the approval is verbatim rather than partial.
	//
	// GuardianPolicy.cpp takes m_Settings = m_PendingSettings with no re-derivation,
	// and this is what checks it: two fields that move in the same direction both
	// land. A partial copy would pass the playtime check alone.
	CHECK(approved.Settings().ChatRestricted == false, "including the chat restriction");
}

// The deny path, and the mirror of the approve test.
static void TestDenyRevertsToTheOriginal()
{
	const GuardianPolicy minor = GuardianPolicy::ForBand(AgeBand::Age16To17);
	const GuardianPolicy reviewing = minor.WithPendingChange(Relaxed());
	// The same starting point as TestApproveAppliesThePendingChange -- same band, same
	// Relaxed() proposal -- so the two tests differ ONLY in the transition. Any
	// difference in their assertions is then attributable to Approve vs Deny.
	//
	// That symmetry is what makes this suite readable: each test changes one thing.
	const GuardianPolicy denied = reviewing.Denied();

	CHECK(!denied.HasPendingChange(), "denial must clear the pending change");
	// == Minor, STRICTER than the approve test's != PendingReview -- so the deny path
	// does pin the band-derived state exactly. The asymmetry noted above is therefore
	// in the approve test, not here.
	CHECK(denied.State() == GuardianState::Minor, "denial must return to the band's own state");
	CHECK(denied.Settings().NightHoursEnforced, "denial must keep night-hour enforcement");
	// The original settings stand.
	//
	// Compared against the SOURCE policy's settings rather than against a literal,
	// because the requirement is "unchanged from what was in force" -- a stronger
	// and more meaningful claim than asserting 180.
	//
	// GuardianPolicy.cpp re-derives these with SettingsForBand(m_Band) instead of
	// copying m_Settings. The comment there notes the two are equivalent today; this
	// test would pass under either implementation, so it pins the BEHAVIOUR, not the
	// mechanism.
	CHECK(denied.Settings().DailyPlaytimeMinutes == minor.Settings().DailyPlaytimeMinutes,
	      "denial must restore the original playtime limit");
}

// The no-op contract, and the one test here about robustness rather than policy.
//
// Both transitions are declared `const ... Approved() const` returning a NEW
// policy, so the no-pending case must produce an equal one. There is no reference
// overload and no mutating overload, so nothing else could happen even if the
// implementation were wrong -- which makes this a cheap guard.
static void TestApproveOrDenyWithNothingPending()
{
	// The pre-existing comment gives the threat model, and it is a good one: a
	// double-fired button is an ORDINARY UI bug, not an exceptional condition, so it
	// must not need a guard at every call site.
	// Both must be no-ops rather than crashes or state changes. A UI that
	// double-fires a button is a normal bug, not an exceptional one.
	const GuardianPolicy minor = GuardianPolicy::ForBand(AgeBand::Age13To15);

	// Approve with nothing pending.
	//
	// GuardianPolicy.cpp returns `*this` on this path. That requires the copy
	// CONSTRUCTOR, which the header deliberately keeps while deleting both assignment
	// operators -- the header notes a user-declared move constructor would delete the
	// copy constructor the transitions rely on. This line is the reason that comment
	// exists.
	const GuardianPolicy approved = minor.Approved();
	// Compared against minor rather than a literal, because the requirement is
	// "unchanged", not "Minor".
	//
	// Both comparisons here are against the SOURCE, which is the right frame for a
	// no-op test and makes the assertion independent of the band table.
	CHECK(approved.State() == minor.State(), "approving with nothing pending must not change the state");
	CHECK(approved.Settings().ChatRestricted == minor.Settings().ChatRestricted,
	      "and must not change the settings");

	const GuardianPolicy denied = minor.Denied();
	CHECK(denied.State() == minor.State(), "denying with nothing pending must not change the state");
}

// Review is not a route to Adult, over every minor band.
//
// A different question from the one TestUnknownIsNeverAdult asks: that one is
// about an absent signal, this one is about a PRESENT one. Getting review to
// produce Adult would be worse, since it would require a guardian to be misread.
static void TestPendingReviewIsNeverAdult()
{
	// The pre-existing comment states the case: even if a guardian proposes the adult
	// settings, review is a review.
	//
	// The merge still holds the STRICTER of the two, so proposing adult settings does
	// not adopt them -- but the STATE is the thing under test here, and it is set
	// A pending change must not be a way to reach Adult early. Even if a
	// guardian proposes the adult settings, review is a review.
	// All three minor bands, and the two states they map to (MinorNeedsSetup and
	// Minor) both collapse to PendingReview.
	//
	// DEFECT (coverage gap): AgeBand::Adult18 is NOT in this list. So a policy for a
	// verified adult entering review -- which sets PendingReview unconditionally in
	// WithPendingChange -- is untested here. The reasoning above ("a pending change
	// must not be a way to reach Adult early") would suggest Adult18 SHOULD be in
	// the list, asserting that Adult + pending is PendingReview and not Adult. As
	// written, a future change that special-cased the adult band would not be caught.
	//
	// AgeBand::Unknown is excluded too, but that is correct: it cannot reach
	// PendingReview through a meaningful path, and TestProposedChangeEntersReview
	// covers the transition generally.
	for (AgeBand band : {AgeBand::Under13, AgeBand::Age13To15, AgeBand::Age16To17})
	{
		const GuardianPolicy reviewing = GuardianPolicy::ForBand(band).WithPendingChange(Relaxed());
		// Two assertions per band, in the same order as the other loop test: state is
		// review, and it is not Adult.
		//
		// The second is strictly implied by the first -- no band can be both. So of the
		// two, only the first has discriminating power, and the second documents the
		// security property in the output text so a failure elsewhere is unambiguous.
		CHECK(reviewing.State() == GuardianState::PendingReview, "a pending change must be PendingReview");
		CHECK(reviewing.State() != GuardianState::Adult, "PendingReview must never be Adult");
	}
}

// Under13's own state, and the settings that make waiting safe.
//
// Overlaps TestBandSelectsState (which asserts the same state); the value here is
// the settings assertions, which no other test makes for this band.
static void TestMinorNeedsSetupIsNotFullyConfigured()
{
	// The pre-existing comment gives the reason the state exists at all: the guardian
	// has not been walked through setup, so a `state == Minor` check downstream would
	// misread an unconfigured under-13 as fully set up.
	//
	// And it must stay restrictive WHILE waiting -- the wait is not a licence.
	// Under 13 sits in its own state because the guardian protections have
	// not been established yet. It must still be restrictive while it waits.
	const GuardianPolicy setup = GuardianPolicy::ForBand(AgeBand::Under13);

	CHECK(setup.State() == GuardianState::MinorNeedsSetup, "under 13 must be MinorNeedsSetup");
	CHECK(setup.State() != GuardianState::Minor, "and must not claim to be fully configured");
	// The playtime, as an inequality against the sentinel again -- the same reasoning
	// as the default-policy test: assert the REQUIREMENT (a limit exists), not the
	// value.
	//
	// For Under13 that limit is 60, matching the struct default, so this does not
	// distinguish SettingsForBand's explicit 60 from the default. It pins the
	// property, not which code path set it.
	CHECK(setup.Settings().DailyPlaytimeMinutes != GuardianSettings::kUnlimited,
	      "MinorNeedsSetup must not start with unlimited playtime");
	CHECK(setup.Settings().ChatRestricted, "MinorNeedsSetup must restrict chat");
}

// The adult case, and the one test whose failure is a shipping bug rather than a
// compliance bug.
static void TestAdultIsLeastRestrictive()
{
	// The pre-existing comment is explicit that this is NOT a legal requirement, and
	// that distinction is the point: the other tests guard compliance, this one guards
	// against a build where nobody can chat or play.
	//
	// Worth stating that it is still a real test rather than a nice-to-have. A merge or
	// a band table that failed closed everywhere would pass every other assertion in
	// this file.
	// Not a legal requirement -- an adult is outside Art 8(1). It exists so a
	// bug that locks every adult down fails visibly instead of shipping a game
	// where nobody can chat or play.
	// Settings(), not EffectiveSettings() -- there is nothing pending on a fresh
	// policy, so the two are equal and the simpler accessor is the right one.
	//
	// A temporary's member called directly, which is safe because the temporary
	// outlives the full expression.
	const GuardianSettings settings = GuardianPolicy::ForBand(AgeBand::Adult18).Settings();

	// Equality against the sentinel -- the opposite of the minor assertions, and
	// necessarily so: here the REQUIREMENT is that there is no limit.
	//
	// This is the only place kUnlimited is produced as a band baseline in
	// GuardianPolicy.cpp, and it is also the value that makes the adult path the
	// only one where StricterOf's first branch is reachable -- see the coverage gap
	// noted below.
	CHECK(settings.DailyPlaytimeMinutes == GuardianSettings::kUnlimited, "an adult must not have a playtime cap");
	CHECK(!settings.NightHoursEnforced, "an adult must not have a curfew");
	CHECK(!settings.ChatRestricted, "an adult must not have chat restricted");
	// The one field left RESTRICTIVE for an adult, and the assertion is inverted:
	// here true is correct.
	//
	// GuardianPolicy.cpp calls this out as the single place in the module where a
	// flag stays restrictive for a reason that is a DEFAULT rather than a legal
	// requirement. So this CHECK is a guard against a future "clean up the adult
	// defaults" edit -- which would look harmless and would break a deliberate
	// choice.
	CHECK(settings.ReportActivityToGuardian, "but activity reporting to a self-chosen guardian may remain on");
}

// The entry point, and the only reference to any test function.
//
// Thirteen calls, and that is what makes an unwritten test a LINK ERROR rather
// than a silent skip. Returns int so the exit code can signal pass/fail to CI.
int main()
{
	// The call order is dependency-free: every test constructs its own policies from
	// ForBand and shares nothing but the counters, so the order affects only the
	// sequence of FAIL lines, not the result.
	//
	// DEFECT (no isolation, same as the sibling test): a failure does not stop the
	// suite, and there is no per-test summary. A reader sees interleaved FAIL lines
	// with no indication of which test produced each -- which is why every `what`
	// string in this file is written as a self-describing sentence.
	TestDefaultIsUnconfiguredAndRestrictive();
	// Second, though it is the only check that cannot fail at runtime. It is ordered
	// here to mirror the header's list rather than for any dependency.
	TestUnlimitedIsZero();
	TestBandSelectsState();
	TestUnknownIsNeverAdult();
	TestProposedChangeEntersReview();
	TestPendingReviewHoldsTheStricterSettings();
	TestPendingReviewOfATighteningTakesEffect();
	TestApproveAppliesThePendingChange();
	TestDenyRevertsToTheOriginal();
	TestApproveOrDenyWithNothingPending();
	TestPendingReviewIsNeverAdult();
	TestMinorNeedsSetupIsNotFullyConfigured();
	TestAdultIsLeastRestrictive();

	// The summary line, named after the file so a CI log is self-identifying, and
	// printed even on success so a passing run still produces output.
	//
	// DEFECT (count off by one): g_Checks includes the bare `++g_Checks` in
	// TestUnlimitedIsZero, which asserts nothing at runtime. So the figure is
	// assertions-plus-one.
	std::printf("test_guardian_policy: %d checks, %d failures\n", g_Checks, g_Failures);
	// 0 for pass, 1 for any failure -- the contract every CI system expects.
	//
	// A ternary rather than `return !!g_Failures`, so the success value is the literal
	// 0. Academic, but the explicitness is the point.
	//
	// The counters are never reset, so this is the total across all thirteen tests.
	return g_Failures ? 1 : 0;
}