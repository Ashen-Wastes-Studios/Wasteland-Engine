// Standalone functional test for Wasteland::Age::JurisdictionPolicy.
//
// This is the module's fail-closed boundary. It answers one question: is a
// given assurance method good enough for a given kind of surface? Getting it
// wrong in the permissive direction means a game ships to the EU without the
// assurance the proposal requires, which is the specific outcome both the EU
// and US instruments exist to prevent.
//
// Legal basis, against the EU KIDS Act proposal (COM(2026) 681, 2026/0286(COD)):
//   Art 29(2) -- providers under Art 6 (social networking, video-sharing)
//                shall rely EXCLUSIVELY on an EU age verification solution
//                certified under the EU Age Verification Scheme. No
//                alternatives permitted.
//   Art 29(4) -- providers under Art 8(1) and 16(2)-(3) -- games, AI
//                companions, chatbots, app stores -- MAY use other age
//                assurance solutions, provided they meet Art 27 and Art 28.
//                A game studio is therefore NOT locked to the EU Scheme.
//   Art 3(i)  -- "age assurance" expressly EXCLUDES self-declaration, so a
//                date-of-birth box is not age assurance at all.

// Only for std::printf below. Kept because the CHECK macro needs it and the
// macro must be usable from every test function in the file.
#include <cstdio>
// Required for the braced-init-list range-for loops over Jurisdiction below.
// MSVC does not pull it in transitively here.
#include <initializer_list>

#include "Wasteland/Age/JurisdictionPolicy.h"

// Brings the module's types and both query functions into scope unqualified.
//
// DEFECT: a using-directive at file scope in a test is convenient but it
// also means any future symbol added to Wasteland::Age -- or to any
// namespace it transitively uses -- can silently become ambiguous here.
// Qualifying the handful of names used would be safer, though it is a
// trade-off this file has clearly made deliberately.
using namespace Wasteland::Age;

// File-scope failure counter, so each Test* function can report without a
// fixture object. Static gives it internal linkage -- no other translation
// unit can collide with the name.
static int g_Failures = 0;
// The check counter, incremented by the macro rather than by each test.
// Printed in main() so a run that silently executed nothing is visible.
static int g_Checks = 0;

#define CHECK(cond, what)                                                        \
	do                                                                           \
	{                                                                            \
		++g_Checks;                                                              \
		if (!(cond))                                                             \
		{                                                                        \
			++g_Failures;                                                        \
			std::printf("  FAIL: %s\n", (what));                                  \
		}                                                                        \
	// The assertion primitive. Counts every evaluation, reports only the
	// failures, and never aborts -- so one broken invariant does not hide the
	// state of every check after it.
	//
	// do/while(0) rather than a bare if, so the macro expands to a single
	// statement safe to use unbraced inside an if/else without changing which
	// else it binds to. The trailing zero is what makes that work.
	//
	// `what` is a runtime argument rather than baked into the macro as a
	// literal. Every call site here passes a string literal, so nothing is lost
	// and a computed description would be possible if ever needed.
	//
	// Note the macro spans ten lines of backslash continuations, so a comment
	// cannot be inserted inside it -- this block sits after the closing brace
	// instead.
	} while (0)

// Every surface the module knows about. An unknown/garbage cast is exercised
// separately in TestFailClosedOnUnknownSurface.
// The surface table shared by every exhaustive loop, so a new SurfaceKind
// added to the enum is picked up by the tests as soon as it is listed here.
//
// DEFECT: nothing enforces that. Adding an enumerator does not break this
// array -- the loops silently keep testing the old five, so the new surface
// goes untested. A compile-time count compared against the enum, or
// iterating 0..count-1, would make the omission a build error.
static const SurfaceKind kAllSurfaces[] = {
    // First in the array, so the loops print failures in declaration order.
    // Every element is written explicitly rather than default-initialised -- a
    // partially-filled array would test a zero-valued surface and look like a
    // real one.
    SurfaceKind::OnlineGame,
    SurfaceKind::AiChatbot,
    SurfaceKind::AppStore,
    SurfaceKind::SocialNetwork,
    SurfaceKind::VideoSharing,
};

static void TestSelfDeclaredIsNeverSufficient()
{
    // Art 3(i) excludes self-declaration from "age assurance". This is the
    // single most important rule in the policy: a studio that ships a plain
    // date-of-birth box must be refused, not quietly accepted.
    // The 5 x 2 x 2 cross product of surfaces against jurisdictions at the
    // self-declared level: nothing is ever sufficient there.
    for (SurfaceKind s : kAllSurfaces)
    {
        // A braced-init-list range-for, which is why <initializer_list> has to be
        // included explicitly above.
        //
        // DEFECT: the jurisdiction list is spelled out here and again in
        // TestMonotonicity, and a third time implicitly in the jurisdiction tests.
        // A shared kAllJurisdictions[] table -- the same treatment kAllSurfaces gets
        // -- would mean a new jurisdiction is added in one place. As written, a
        // fourth jurisdiction would be silently omitted from this most important
        // loop.
        for (Jurisdiction j : {Jurisdiction::Eu, Jurisdiction::UnitedStates})
        {
            CHECK(!IsSufficient(s, AssuranceLevel::SelfDeclared, j),
                  "self-declaration must never satisfy any surface, in any jurisdiction");
        }
    }
}

static void TestSocialIsLockedToEuScheme()
{
    // Art 29(2): exclusive, but ONLY in EU scope. The proposal binds providers
    // offering services into the EU; a US-only build is outside its scope and
    // no US federal rule imposes the EU Scheme. Asserting this in both
    // jurisdictions would be asserting the law of a jurisdiction that has no
    // such law -- see TestJurisdictionChangesTheFloor for the US side.
    // The Art 29(2) exclusivity, tested negatively: a commercial assurance is
    // not accepted for a social surface in EU scope.
    //
    // Each CHECK is written out rather than looped, because the expected answer
    // differs per surface here -- SocialNetwork and VideoSharing are both locked
    // to the EU Scheme while OnlineGame is not. A table of expectations would be
    // more compact but would also let a wrong row pass unnoticed by a reader.
    CHECK(!IsSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::Commercial, Jurisdiction::Eu),
          "Art 29(2): a commercial assurance must not satisfy social networking in EU scope");
    CHECK(!IsSufficient(SurfaceKind::VideoSharing, AssuranceLevel::Commercial, Jurisdiction::Eu),
          "Art 29(2): a commercial assurance must not satisfy video-sharing in EU scope");
    CHECK(!IsSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal, Jurisdiction::Eu),
          "Art 29(2): an OS signal must not satisfy social networking in EU scope");

    CHECK(IsSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::EuEaa, Jurisdiction::Eu),
          "Art 29(2): an EU proof-of-age attestation satisfies social networking in EU scope");
}

static void TestGamesHaveLatitudeUnderArt294()
{
    // Art 29(4): games, AI chatbots and app stores may use alternatives that
    // meet Art 27/28. This is the commercial lever, and it only exists if the
    // policy encodes the Art 29(2)/29(4) split rather than defaulting to the
    // strictest rule everywhere.
    // The Art 29(4) latitude, and deliberately a SUBSET of kAllSurfaces: the two
    // Art 6 surfaces are excluded because they are the ones Art 29(2) locks.
    //
    // Spelled as its own literal list rather than derived from kAllSurfaces,
    // which is the right call -- the exclusion is the point of the test -- but
    // it does mean a new Art 6-style surface has to be added to this list
    // explicitly.
    for (SurfaceKind s : {SurfaceKind::OnlineGame, SurfaceKind::AiChatbot, SurfaceKind::AppStore})
    {
        CHECK(IsSufficient(s, AssuranceLevel::Commercial, Jurisdiction::Eu),
              "Art 29(4): a game/chatbot/app store may use commercial assurance in EU scope");
        CHECK(!IsSufficient(s, AssuranceLevel::SelfDeclared, Jurisdiction::Eu),
              "Art 29(4) latitude does not extend down to self-declaration");
    }
}

static void TestMonotonicity()
{
    // A stronger method must never be insufficient where a weaker one is
    // sufficient. A non-monotonic ladder means the engine's answer depends on
    // which method a provider happened to return, which is not a policy.
    // The ladder, ordered weakest to strongest. The ORDER IS THE TEST:
    // monotonicity is asserted by walking this array in sequence, so reordering
    // it would invalidate what TestMonotonicity checks while still compiling
    // and still passing.
    //
    // Local static rather than file scope because only this function needs it.
    static const AssuranceLevel kLevels[] = {
        AssuranceLevel::SelfDeclared,
        AssuranceLevel::OsSignal,
        AssuranceLevel::Commercial,
        AssuranceLevel::EuEaa,
    };

    for (SurfaceKind s : kAllSurfaces)
    {
        for (Jurisdiction j : {Jurisdiction::Eu, Jurisdiction::UnitedStates})
        {
            // The monotonicity state machine. Starts false so the weakest level is
            // unconstrained, then latches on the first sufficient method and requires
            // every stronger one to stay sufficient.
            bool sawSufficient = false;
            // Walked weakest-first. Walking strongest-first would make the assertion
            // vacuous -- with sawSufficient latching early, the weaker levels would
            // never be checked.
            for (AssuranceLevel level : kLevels)
            {
                const bool ok = IsSufficient(s, level, j);
                // Once something sufficient is seen, everything stronger must
                // also be sufficient.
                if (sawSufficient)
                    // DEFECT: this is the one failure message in the file that does not identify
                    // what failed. Every other CHECK names the surface, level or jurisdiction.
                    // A monotonicity break here reports the same string for all 5 x 2 x 4
                    // combinations, so the developer has to bisect by hand to find which
                    // surface or level regressed. Including s, j and level in the message would
                    // cost nothing at these call counts.
                    CHECK(ok, "assurance ladder is not monotonic");
                if (ok)
                    sawSufficient = true;
            }
        }
    }
    // DEFECT: a stray increment with no check attached. Every CHECK in this
    // function already counted itself, so this adds one phantom entry to the
    // total printed by main(). Harmless to correctness -- it cannot cause a
    // failure -- but it makes the reported check count not correspond to any
    // assertion, and it is the kind of line a reader has to puzzle over. It
    // reads like a leftover from an earlier version of this test.
    ++g_Checks;
}

static void TestFailClosedOnUnknownSurface()
{
    // A SurfaceKind value persisted by an older build, or cast in from bad
    // data, must land on the STRICTEST rule. Defaulting to anything
    // permissive is how a permissiveness bug reaches production.
    // A deliberately out-of-range value, standing in for a value persisted by an
    // older build or cast in from corrupt data.
    //
    // 999 rather than a neighbouring enumerator: the point is to be outside the
    // known range entirely, so a switch-based implementation hits its default
    // case.
    const SurfaceKind garbage = static_cast<SurfaceKind>(999);
    CHECK(MinimumLevelFor(garbage, Jurisdiction::Eu) == AssuranceLevel::EuEaa,
          "an unknown surface must require the strongest assurance, not the weakest");
    CHECK(!IsSufficient(garbage, AssuranceLevel::Commercial, Jurisdiction::Eu),
          "an unknown surface must not be satisfied by a commercial assurance");
    CHECK(!IsSufficient(garbage, AssuranceLevel::OsSignal, Jurisdiction::Eu),
          "an unknown surface must not be satisfied by an OS signal");

    // The negative counterpart. Both directions are needed because an
    // implementation that clamps with a comparison rather than a switch could
    // handle one and not the other.
    const SurfaceKind negative = static_cast<SurfaceKind>(-7);
    CHECK(MinimumLevelFor(negative, Jurisdiction::Eu) == AssuranceLevel::EuEaa,
          "a negative surface value must fail closed too");
}

static void TestUnknownLevelNeverSatisfies()
{
    // An assurance level the engine does not recognise -- a newer provider
    // build, or corrupt data -- must not read as "strong enough".
    // Same technique against the other axis: an unrecognised assurance level.
    //
    // This is the fail-closed check for the LEVEL dimension where
    // TestFailClosedOnUnknownSurface covers the SURFACE dimension.
    const AssuranceLevel garbage = static_cast<AssuranceLevel>(42);
    // DEFECT: only Jurisdiction::Eu is exercised. Every other
    // unknown-value test in this file checks both jurisdictions, but this one
    // checks a single jurisdiction across all five surfaces. If the US branch of
    // IsSufficient returned true for an unrecognised level, nothing here would
    // notice -- which is the permissive direction this file exists to prevent.
    for (SurfaceKind s : kAllSurfaces)
        CHECK(!IsSufficient(s, garbage, Jurisdiction::Eu),
              "an unrecognised assurance level must never satisfy any surface");
}

static void TestMinimumLevels()
{
    // The floor for each surface in EU scope, asserted per surface rather than
    // looped, because MinimumLevelFor is a query whose expected answer differs
    // per row and each row encodes a different article of the proposal.
    CHECK(MinimumLevelFor(SurfaceKind::SocialNetwork, Jurisdiction::Eu) == AssuranceLevel::EuEaa,
          "Art 29(2): social networking requires the EU Scheme");
    CHECK(MinimumLevelFor(SurfaceKind::VideoSharing, Jurisdiction::Eu) == AssuranceLevel::EuEaa,
          "Art 29(2): video-sharing requires the EU Scheme");
    CHECK(MinimumLevelFor(SurfaceKind::OnlineGame, Jurisdiction::Eu) == AssuranceLevel::Commercial,
          "Art 29(4): a game may use commercial assurance");
    CHECK(MinimumLevelFor(SurfaceKind::AiChatbot, Jurisdiction::Eu) == AssuranceLevel::Commercial,
          "Art 29(4): an AI chatbot may use commercial assurance");
    CHECK(MinimumLevelFor(SurfaceKind::AppStore, Jurisdiction::Eu) == AssuranceLevel::Commercial,
          "Art 29(4): an app store may use commercial assurance");
}

static void TestJurisdictionChangesTheFloor()
{
    // A US-only build does not ship into the EU proposal's scope, so the EU
    // Scheme floor does not apply to it. What DOES apply is the App Store
    // Accountability Acts, under which the platform store performs the
    // verification and shares the age range for the developer to consume --
    // hence OsSignal as the US floor.
    //
    // Note what this does NOT do: it does not relax SafetyProfile defaults.
    // Scope selects which rules are consulted; it never switches protection
    // off. See AgeService::SetEuScope.
    // The US floor differs, which is what makes jurisdiction a real parameter
    // rather than a label.
    CHECK(MinimumLevelFor(SurfaceKind::SocialNetwork, Jurisdiction::UnitedStates) == AssuranceLevel::OsSignal,
          "US scope: the store does the verification, so an OS signal is the floor");
    CHECK(IsSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal, Jurisdiction::UnitedStates),
          "US scope: an OS signal satisfies a social surface");

    // EU scope is strictly stronger for the Art 6 surfaces.
    CHECK(!IsSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal, Jurisdiction::Eu),
          "EU scope: an OS signal must NOT satisfy social networking");

    // Pin the relaxation deliberately. Art 29(2) is an EU rule; outside EU
    // scope a commercial attestation is acceptable for a social surface, and
    // asserting it here means a future tightening shows up as a test failure
    // rather than as a silent behaviour change.
    // Pins the relaxation deliberately, as the comment above says. Worth
    // emphasising that a POSITIVE assertion of the permissive answer is the
    // harder kind of test to keep: it fails when the policy tightens, which is
    // the safe direction to fail, but it also means an intentional relaxation
    // later requires editing this line rather than just deleting it.
    CHECK(IsSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::Commercial, Jurisdiction::UnitedStates),
          "US scope: a commercial assurance satisfies a social surface, since Art 29(2) does not bind here");
}

static void TestUnknownJurisdictionFailsClosed()
{
    // Same class of bug as an unknown surface: a Jurisdiction value the engine
    // cannot interpret must resolve to the strictest rules, never to the
    // permissive US branch. Corrupt persisted state must not be able to
    // quietly downgrade the floor.
    // The third fail-closed axis: surface, level, and now jurisdiction. Together
    // the three tests cover every enum the policy takes as input, which is what
    // makes 'corrupt persisted state cannot downgrade the floor' a claim rather
    // than an aspiration.
    const Jurisdiction garbage = static_cast<Jurisdiction>(77);

    CHECK(MinimumLevelFor(SurfaceKind::SocialNetwork, garbage) == AssuranceLevel::EuEaa,
          "an unknown jurisdiction must resolve to EU rules for Art 6 surfaces");
    CHECK(MinimumLevelFor(SurfaceKind::OnlineGame, garbage) == AssuranceLevel::Commercial,
          "an unknown jurisdiction must resolve to EU rules for Art 29(4) surfaces");
    CHECK(!IsSufficient(SurfaceKind::SocialNetwork, AssuranceLevel::Commercial, garbage),
          "an unknown jurisdiction must not select the permissive US branch");
}

// No argv: the test takes no inputs, so there is nothing to validate. Every
// case is a fixed property of the policy.
//
// Deliberately not a test framework -- standalone main with printf, matching
// test_project_seeding.cpp, so these can run without a dependency.
int main()
{
    // Call order is deliberate: the single most important rule (Art 3(i)) runs
    // first, so a broken self-declaration policy is visible in the first lines of
    // output rather than buried.
    //
    // No return values checked -- these functions report through g_Failures, so
    // the return value would carry no information.
    TestSelfDeclaredIsNeverSufficient();
    TestSocialIsLockedToEuScheme();
    TestGamesHaveLatitudeUnderArt294();
    TestMonotonicity();
    TestFailClosedOnUnknownSurface();
    TestUnknownLevelNeverSatisfies();
    TestMinimumLevels();
    TestJurisdictionChangesTheFloor();
    TestUnknownJurisdictionFailsClosed();

    // Both counters printed. The check count is what makes a vacuous run
    // visible: a count of zero with zero failures still reads as suspicious,
    // which is the point of printing it.
    std::printf("test_policy: %d checks, %d failures\n", g_Checks, g_Failures);
    // Non-zero exit on any failure, which is the only thing the CI actually
    // checks -- the printed text is for the human reading the log.
    return g_Failures ? 1 : 0;
}