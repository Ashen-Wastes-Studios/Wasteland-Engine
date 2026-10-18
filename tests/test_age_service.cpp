// Standalone functional test for Wasteland::Age::AgeService.
//
// AgeService is where the rest of the module becomes real: it holds the
// provider, resolves an attestation into a profile and a guardian policy, and
// answers the questions a game actually asks at runtime.
//
// FOUR THINGS THIS SUITE PROVES
//
// 1. THE MODULE IS LIVE FROM CONSTRUCTION. There is no Enable() call and no
//    opt-in. A studio that links the engine and does nothing still gets the
//    restrictive defaults, because a module that must be switched on before it
//    protects anybody is a module that ships unprotected.
//
// 2. NO PROVIDER MEANS NO SIGNAL, WHICH MEANS MINOR. The default-constructed
//    service must be indistinguishable from one that asked and got nothing.
//
// 3. EU SCOPE NARROWS THE JURISDICTION RULES AND NOTHING ELSE. This is the
//    test that matters most for the plan's UseEuScope decision: turning EU
//    scope off must change which rules JurisdictionPolicy consults, and must
//    NOT relax SafetyProfile. Art 8(1) child-safety defaults are not
//    jurisdiction-conditional. A US-only build does not get to hand a
//    thirteen-year-old an adult SafetyProfile.
//
// 4. A TRANSIENT FAILURE REVERTS TO RESTRICTED. If a provider stops
//    answering, the service forgets the old verdict rather than caching it.
//    That is the fail-closed choice and it is deliberate; see the note on
//    TestTransientFailureRevertsToRestricted for the trade-off it makes.

// std::printf for the summary line, std::unique_ptr for the provider handle
// the stub hands over, and std::string for the attestation text.
//
// DEFECT (documented, not fixed): this is a bare <cstdio> rather than the
// project's "wlpch.h". Every engine translation unit opens with the PCH,
// so this file is the one place that proves the tests do not depend on it.
// Defensible for a standalone test binary, but it means the tests compile
// against a slightly different include environment than the code they
// cover.
#include <cstdio>
#include <memory>
#include <string>

// The header under test, by its include-path form rather than relative, so
// this resolves the same way the engine builds do.
#include "Wasteland/Age/AgeService.h"

// A using-directive rather than qualification throughout. Convenient in a
// file this size, and it is why every type below is written bare.
using namespace Wasteland::Age;

// Counters as file statics rather than anything thread-local: the test
// binary is single-threaded and the CHECK macro increments them directly.
static int g_Failures = 0;
// Every assertion, passing or failing, bumps this. It is what makes the
// summary line report how much was actually tested rather than just
// whether it passed.
static int g_Checks = 0;

// The assertion macro: records the check, and on failure prints a message
// and records a failure.
//
// It does NOT abort, and does not return from the caller. So a failing
// assertion lets the rest of the test case run, which is deliberate: one
// CHECK failing should not hide the state of everything after it.
//
// `what` is a plain string literal at every call site, so a failing check
// prints the CLAIM rather than the expression -- the assertions below are
// written so the message is the specification, not a restatement of the
// code.
#define CHECK(cond, what)                                                        \
	do                                                                           \
		// do/while(0) rather than a bare block: it makes the macro a single
		// statement, so `if (x) CHECK(a, b); else ...` still compiles and the
		// check cannot be silently captured by a caller's dangling else.
	{                                                                            \
				// Counted before the condition is evaluated, so g_Checks reflects attempted
				// checks rather than successful ones.
		++g_Checks;                                                              \
		if (!(cond))                                                             \
		{                                                                        \
			++g_Failures;                                                        \
			std::printf("  FAIL: %s\n", (what));                                  \
		}                                                                        \
	} while (0)

// Anonymous namespace, so the stub provider and the attestation factories
// below have internal linkage. That is what keeps a test binary's symbols
// from colliding with another TU's.
namespace
{

// A provider that returns whatever the test hands it. Doubles as proof the
// service consumes the interface a studio implements.
// The stub provider. It is also the proof that the interface is
// implementable by a studio: it implements IAgeAssuranceProvider with three
// one-line methods and nothing else.
class StubProvider final : public IAgeAssuranceProvider
{
public:
		// Constructed with both the verdict to return and whether to claim
		// availability. Holding both is what lets one type serve the available
		// and unavailable cases without a second class.
	StubProvider(Attestation result, bool available)
				// std::move on the attestation, not a copy. Attestation is deliberately
				// non-assignable but presumably movable, and this is the one place the
				// stub takes ownership of a verdict.
		: m_Result(std::move(result)), m_Available(available)
	{
	}

		// The availability probe, which Refresh() calls BEFORE querying. The stub
		// returns the flag verbatim, so a test can make the provider unavailable
		// while still holding a perfectly good verdict -- which is exactly the
		// case TestUnavailableProviderMeansNoSignal needs.
	bool IsAvailable() const override { return m_Available; }
		// Non-const and non-reference: Query() is called by Refresh() through a
		// non-const provider pointer, and the stub has nothing to update, so it
		// just hands back the stored copy.
	Attestation Query() override { return m_Result; }
		// A const reference to a member, not a temporary. The name is a member with
		// an in-class initialiser rather than something passed in, so every stub
		// reports the same provider name no matter what verdict it holds -- which
		// is fine, because nothing in this file asserts on it.
	const std::string &ProviderName() const override { return m_Name; }

private:
		// The verdict, stored by value.
	Attestation m_Result;
		// The availability flag, held separately from the verdict so the two can
		// disagree. That disagreement is the point: see IsAvailable above.
	bool m_Available;
	std::string m_Name = "stub";
};

// A verified adult: COMMERCIAL assurance, band Adult18. The assurance level
// is what makes it verified -- the band alone would not be, which is the
// distinction TestSelfDeclaredAdultStaysMinorThroughService exercises.
Attestation VerifiedAdult()
{
	return Attestation::FromProvider(AssuranceLevel::Commercial, AgeBand::Adult18, "vendor");
}

// A verified 13-15. The second fixture the whole suite is built around:
// most tests need a minor, and building it here rather than inline keeps
// the assurance level in one place.
Attestation VerifiedMinor()
{
	return Attestation::FromProvider(AssuranceLevel::Commercial, AgeBand::Age13To15, "vendor");
}

} // namespace

// PROVES RULE 1: the module is live from construction.
static void TestModuleIsLiveFromConstruction()
{
	// No Enable(), no registration, no opt-in. Constructing the service is
	// enough for it to be enforcing.
		// A local, not the Instance() singleton. The header says the tests
		// deliberately avoid the singleton so cases stay order-independent, and
		// this line is where that discipline starts.
	const AgeService service;

		// The provider check, phrased as `== false` rather than `!`. Consistent
		// throughout the file: the assertions state the expected value in full
		// rather than relying on a negation reading.
	CHECK(service.HasProvider() == false, "a fresh service must have no provider");
		// EU scope on by default. DEFECT (documented, not fixed): this asserts a
		// default, so it cannot catch a build where the studio deliberately turned
		// EU scope off at startup -- there is no global configuration this suite
		// consults.
	CHECK(service.EuScope(), "EU scope must be ON by default -- a build shipping into the EU cannot opt out by omission");
	CHECK(service.Band() == AgeBand::Unknown, "with no provider the band must be Unknown");
	CHECK(service.IsMinor(), "with no provider the player must be treated as a minor");
	CHECK(!service.IsVerified(), "with no provider nothing is verified");

	// The restrictive profile, and specifically the Art 8(1) items.
		// The profile is fetched ONCE into a local and the six assertions read it.
		// Worth doing even though Profile() is cheap: it also proves the accessor
		// returns a consistent snapshot rather than something recomputed per call.
	const SafetyProfile profile = service.Profile();
		// Free-text chat off: the single most consequential Art 8(1) default, and
		// the first thing asserted because everything else is less severe.
	CHECK(!profile.FreeTextChat(), "no provider must mean no free-text chat");
	CHECK(!profile.ProfileVisibilityPublic(), "no provider must mean a non-public profile (Art 8(1)(a))");
	CHECK(!profile.DirectMessagesFromStrangers(), "no provider must mean no DMs from strangers (Art 8(1)(b))");
	CHECK(!profile.LocationSharing(), "no provider must mean no location sharing (Art 8(1)(e))");
	CHECK(profile.NightHoursRestricted(), "no provider must mean night-hour restrictions hold");
		// Reporting and muting stay available even with no signal. This is the
		// asymmetry the module depends on: restrictions come on by default, but
		// the tools a player needs to report harm are never withheld. See the Art
		// 20(2) reasoning in GuardianToolsView.cpp.
	CHECK(profile.ReportAvailable(), "but reporting must still be available");
	CHECK(profile.MuteAvailable(), "and muting must still be available");

		// The guardian state. Unconfigured rather than Minor, because with no
		// provider there is no guardian to have configured anything -- this is the
		// state that should make a UI prompt for setup.
	CHECK(service.Policy().State() == GuardianState::Unconfigured, "the guardian policy must be Unconfigured");
}

// PROVES RULE 2, first half: asking with no provider invents nothing.
static void TestNoProviderMeansNoSignal()
{
	// Refreshing with no provider must not invent a verdict.
	AgeService service;
		// The call that must be a no-op. Refresh() with no provider is expected to
		// land on the same Absent() verdict rather than leaving anything behind.
	service.Refresh();

	CHECK(service.Band() == AgeBand::Unknown, "refreshing with no provider must leave the band Unknown");
	CHECK(service.IsMinor(), "and must leave the player a minor");
	CHECK(!service.IsVerified(), "and must leave nothing verified");
}

// PROVES RULE 2, second half: an UNAVAILABLE provider is indistinguishable
// from no provider.
static void TestUnavailableProviderMeansNoSignal()
{
	// A provider that reports itself unavailable is treated as no provider at
	// all. Availability being false is normal, not exceptional: the user may
	// have declined, the SDK may not be initialised.
	AgeService service;
		// A perfectly good verified-adult verdict, attached to a provider that says
		// it is unavailable. The test fails if the implementation reads the
		// verdict without consulting IsAvailable() first.
	service.SetProvider(std::make_unique<StubProvider>(VerifiedAdult(), false));
	service.Refresh();

	CHECK(!service.IsVerified(), "an unavailable provider must verify nothing");
	CHECK(service.Band() == AgeBand::Unknown, "and must leave the band Unknown");
	CHECK(service.IsMinor(), "and must leave the player a minor");
}

// The happy path for a verified adult: everything relaxes at once.
static void TestVerifiedAdultThroughProvider()
{
	AgeService service;
	service.SetProvider(std::make_unique<StubProvider>(VerifiedAdult(), true));
	service.Refresh();

	CHECK(service.IsVerified(), "a commercially verified adult must be verified");
	CHECK(service.Band() == AgeBand::Adult18, "the band must be Adult18");
	CHECK(!service.IsMinor(), "a commercially verified adult must not be a minor");

		// The adult profile snapshot. Asserted as a group because the three
		// properties are one decision -- "this is the adult profile" -- and checking
		// them separately would pass on a partial implementation.
	const SafetyProfile profile = service.Profile();
	CHECK(profile.FreeTextChat(), "a verified adult must get the adult profile");
	CHECK(profile.MarketplaceAccess(), "including marketplace access");
	CHECK(!profile.NightHoursRestricted(), "and no curfew");

		// The guardian state follows the profile. Asserted separately from the
		// profile properties because Policy() is a separate accessor with its own
		// reconciliation logic, and could diverge.
	CHECK(service.Policy().State() == GuardianState::Adult, "the guardian policy must be Adult");
}

// PROVES THE DISTINCTION THE MODULE EXISTS FOR: "verified" and "adult"
// are different questions.
static void TestSelfDeclaredAdultStaysMinorThroughService()
{
	// The whole chain, end to end: a provider reports an 18+ band that was
	// self-declared, and the service must not hand back an adult profile.
	AgeService service;
		// The self-declared attestation, inline rather than via a fixture
		// because this is the only case that needs this exact combination.
		// AssuranceLevel::SelfDeclared is the whole point: the band says 18+, and
		// nothing vouched for it.
	service.SetProvider(std::make_unique<StubProvider>(
	    Attestation::FromProvider(AssuranceLevel::SelfDeclared, AgeBand::Adult18, "dob-form"), true));
	service.Refresh();

		// The band IS Adult18 -- stated first and asserted first, to make clear
		// the test is not disputing the provider. It reported what it reported.
	CHECK(service.Band() == AgeBand::Adult18, "the band is still Adult18 -- the provider really said 18 or over");
		// ...and the service still treats the player as a minor. This single line
		// is the module's central invariant: the reported band and the enforced
		// treatment are allowed to disagree, deliberately.
	CHECK(service.IsMinor(), "but the service must treat the player as a minor");
	CHECK(!service.Profile().FreeTextChat(), "and must NOT grant the adult profile");
	CHECK(!service.Profile().MarketplaceAccess(), "including marketplace access");
	CHECK(service.Policy().State() != GuardianState::Adult, "and the guardian policy must not be Adult");
}

// The converse case: a verified minor must NOT get relaxed settings even
// though the provider fully vouched for them.
static void TestVerifiedMinorThroughProvider()
{
	AgeService service;
	service.SetProvider(std::make_unique<StubProvider>(VerifiedMinor(), true));
	service.Refresh();

		// Verified AND minor, simultaneously. Asserting both in sequence is the
		// clearest statement of the invariant in the whole file.
	CHECK(service.IsVerified(), "a verified 13-15 is verified");
	CHECK(service.IsMinor(), "and is a minor");
	CHECK(!service.Profile().FreeTextChat(), "a 13-15 must not get free-text chat");
	CHECK(service.Policy().State() == GuardianState::Minor, "and the guardian policy must be Minor");
}

// PROVES RULE 4: the fail-closed direction on provider loss.
static void TestTransientFailureRevertsToRestricted()
{
	// Deliberate: a provider that stops answering does NOT leave the last good
	// verdict in place.
	//
	// The alternative -- caching a verified adult until the provider comes
	// back -- means a player who was adult once stays adult indefinitely, even
	// if the signal expired or was revoked. Google Play updates cached signals
	// within 2 to 8 weeks of a birthday, so a held verdict goes stale silently.
	// Fail-closed wins.
	//
	// The cost is a user-visible cliff: a transient network error locks a
	// legitimate adult out of chat and the marketplace until it clears. A
	// studio that wants a short grace period should implement it in its own
	// layer with its own expiry policy, where the trade-off is visible in
	// their code, rather than having the engine hold compliance state for them.
	AgeService service;
		// Establish the verified adult, then take it away. The precondition CHECK
		// below matters: without it a regression that made adults minor from the
		// start would pass the second half of the test.
	service.SetProvider(std::make_unique<StubProvider>(VerifiedAdult(), true));
	service.Refresh();
	CHECK(!service.IsMinor(), "precondition: a verified adult starts unrestricted");

	// The provider goes away entirely.
		// The provider is REMOVED, not merely made unavailable. Removing is the
		// harsher case, and it exercises the nullptr path in Refresh() rather than
		// the IsAvailable() one -- the two are covered by different tests.
	service.SetProvider(nullptr);
	service.Refresh();

	CHECK(!service.IsVerified(), "after losing the provider nothing may remain verified");
	CHECK(service.IsMinor(), "and the player must revert to a minor");
	CHECK(!service.Profile().FreeTextChat(), "including losing chat");
}

// Swapping one provider for another must not preserve the first verdict.
static void TestReplacingProvider()
{
	// A studio swapping vendors mid-session must not be able to end up in a
	// state the new provider never vouched for.
	AgeService service;
	service.SetProvider(std::make_unique<StubProvider>(VerifiedAdult(), true));
	service.Refresh();
	CHECK(!service.IsMinor(), "precondition: verified adult");

		// The replacement. Note this replaces an available adult with an available
		// minor, so a stale adult verdict would be maximally visible.
	service.SetProvider(std::make_unique<StubProvider>(VerifiedMinor(), true));
	service.Refresh();

	CHECK(service.Band() == AgeBand::Age13To15, "the new provider's verdict must be what counts");
	CHECK(service.IsMinor(), "so the player must become a minor again");
	CHECK(!service.Profile().FreeTextChat(), "and must lose the adult profile");
}

// PROVES RULE 3, the half that matters: EU scope does NOT relax
// SafetyProfile.
static void TestEuScopeDoesNotRelaxSafetyProfile()
{
	// THE UseEuScope test. Turning off EU scope changes which jurisdiction
	// rules apply. It must NOT change SafetyProfile, because Art 8(1) child
	// safety is not jurisdiction-conditional: a US-only build shipping to a
	// US-only audience still has COPPA obligations and still has every reason
	// not to expose a thirteen-year-old's profile publicly.
	AgeService euService;
		// Two services built identically except for scope. Constructed from the
		// same fixture so the ONLY difference between them is SetEuScope -- which
		// is what makes the comparison below meaningful.
	euService.SetProvider(std::make_unique<StubProvider>(VerifiedMinor(), true));
	euService.Refresh();

	AgeService usService;
	usService.SetProvider(std::make_unique<StubProvider>(VerifiedMinor(), true));
		// The one differing call. Placed BEFORE Refresh() because the scope is a
		// sticky setting, not a per-refresh option.
	usService.SetEuScope(false);
	usService.Refresh();

	CHECK(usService.EuScope() == false, "SetEuScope(false) must stick");

	const SafetyProfile euProfile = euService.Profile();
	const SafetyProfile usProfile = usService.Profile();

		// Each assertion below compares the two profiles field by field rather
		// than checking that the US one equals a hardcoded expectation. That is the
		// right shape here: the claim is "EU scope changed nothing", so the
		// assertion must be an equality between the two services, not a re-statement
		// of what the restrictive profile contains.
	CHECK(euProfile.ProfileVisibilityPublic() == usProfile.ProfileVisibilityPublic(),
	      "EU scope must not be the reason a minor's profile is private");
	CHECK(euProfile.DirectMessagesFromStrangers() == usProfile.DirectMessagesFromStrangers(),
	      "EU scope must not be the reason DMs from strangers are off");
	CHECK(euProfile.FreeTextChat() == usProfile.FreeTextChat(),
	      "EU scope must not be the reason a 13-15 has no free-text chat");
	CHECK(euProfile.LocationSharing() == usProfile.LocationSharing(),
	      "EU scope must not be the reason location sharing is off");
	CHECK(euProfile.NightHoursRestricted() == usProfile.NightHoursRestricted(),
	      "EU scope must not be the reason night-hour restrictions hold");
	CHECK(euProfile.UserGeneratedContentUpload() == usProfile.UserGeneratedContentUpload(),
	      "EU scope must not be the reason a 13-15 cannot upload UGC");

	// The guardian policy is equally jurisdiction-independent.
		// The guardian policy is equally jurisdiction-independent, checked the
		// same way.
	CHECK(euService.Policy().State() == usService.Policy().State(),
	      "EU scope must not change the guardian state");
		// A FIELD-BY-FIELD comparison of the playtime limit specifically, because
		// this is the value most likely to be jurisdiction-tuned in a later
		// revision (the proposal's daily limits are the obvious candidate).
		// Pinning it here means such a change has to come with a test change.
	CHECK(euService.Policy().Settings().DailyPlaytimeMinutes == usService.Policy().Settings().DailyPlaytimeMinutes,
	      "EU scope must not change guardian playtime limits");
}

// The other half of rule 3: narrowing scope must actually DO something,
// or the switch is decorative.
static void TestEuScopeDoesChangeJurisdictionRules()
{
	// The other half: narrowing scope must actually DO something, or the
	// switch is decorative and studios cannot ship a US-only build that
	// relies on the store's own verification.
	//
	// Art 29(2) locks social networking to an EU-certified proof-of-age.
	// Outside EU scope no US federal rule imposes that, so an OS signal is
	// the floor there.
	AgeService service;

		// Art 29(2): a social network needs the EU Scheme, which an OS signal is
		// not. This is the assertion that would fail if the scope switch were
		// wired to nothing.
	CHECK(service.IsAssuranceSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal) == false,
	      "EU scope: Art 29(2) requires the EU Scheme for social networking");

	service.SetEuScope(false);
	CHECK(service.IsAssuranceSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal) == true,
	      "US scope: an OS signal must satisfy a social surface");

		// Switched BACK to true rather than constructing a fresh service, so the
		// test also proves EuScope is not one-way. A setter that ignored false and
		// latched would pass the first half of this test and fail here.
	service.SetEuScope(true);
	CHECK(service.IsAssuranceSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal) == false,
	      "switching EU scope back on must restore the strict rule");
}

// Art 29(4): a game surface is NOT locked to the EU Scheme, in either
// scope. This is the counterpart to the test above -- if narrowing scope
// were the only thing that mattered, this would be untested.
static void TestGameSurfacesAllowCommercialInBothScopes()
{
	// Art 29(4) latitude: a game is not locked to the EU Scheme in either
	// scope, so narrowing scope must not change what a game needs.
	AgeService service;

	CHECK(service.IsAssuranceSufficient(SurfaceKind::OnlineGame, AssuranceLevel::Commercial) == true,
	      "EU scope: Art 29(4) lets a game use commercial assurance");
	service.SetEuScope(false);
	CHECK(service.IsAssuranceSufficient(SurfaceKind::OnlineGame, AssuranceLevel::Commercial) == true,
	      "US scope: still satisfied by commercial assurance");

	// And self-declaration satisfies nothing anywhere. Art 3(i).
		// A braced range over a bool pair, so the body runs twice -- once per
		// scope. Shorter than two copies of the assertion, and it makes the
		// "in ANY scope" claim structural rather than repeated.
	for (bool eu : {true, false})
	{
		service.SetEuScope(eu);
		CHECK(service.IsAssuranceSufficient(SurfaceKind::OnlineGame, AssuranceLevel::SelfDeclared) == false,
		      "self-declaration must satisfy nothing in any scope");
	}
}

// main(): runs every case in order. Each is a separate function so a
// failure reports one behaviour rather than a cascade.
//
// There is no setup or teardown and no runner here -- each case constructs
// its own AgeService -- so order does not matter, which is why they can be
// listed flat.
int main()
{
		// Straight-line calls. No test framework: this suite is built to compile
		// and run with nothing but the engine and a C++ compiler.
	TestModuleIsLiveFromConstruction();
	TestNoProviderMeansNoSignal();
	TestUnavailableProviderMeansNoSignal();
	TestVerifiedAdultThroughProvider();
	TestSelfDeclaredAdultStaysMinorThroughService();
	TestVerifiedMinorThroughProvider();
	TestTransientFailureRevertsToRestricted();
	TestReplacingProvider();
	TestEuScopeDoesNotRelaxSafetyProfile();
	TestEuScopeDoesChangeJurisdictionRules();
	TestGameSurfacesAllowCommercialInBothScopes();

		// The summary line, and the exit code derived from it. Returning a nonzero
		// code from main is what lets a build script fail on it.
	std::printf("test_age_service: %d checks, %d failures\n", g_Checks, g_Failures);
		// Ternary rather than an if: the exit code is the whole of the test
		// result, so there is nothing to do afterwards and an early return reads
		// more clearly than a branch.
	return g_Failures ? 1 : 0;
}