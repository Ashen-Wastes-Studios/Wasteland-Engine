// Standalone functional test for Wasteland::Age::AppleAgeSignalProvider's
// mapping logic.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE PROVIDER
//
// The actual Apple Declared Age Range API is Objective-C/Swift inside the
// AuthenticationServices framework, reachable only from an iOS build. The
// Wasteland engine currently rejects iOS at Wasteland/Core/Core.h with an
// #error, so that code can never be compiled or executed here.
//
// That leaves the mapping untested if it lives inside the provider. So the
// mapping -- the part that decides what Apple's numbers MEAN to this engine --
// is factored out as pure functions, tested on the desktop, and the platform
// call is a thin wrapper that cannot be exercised yet. When iOS support
// lands, the wrapper is new code and the rules it calls are already proven.
//
// WHAT APPLE ACTUALLY RETURNS (verified against Apple's documentation, not
// recalled)
//
//   * You nominate up to three "age gates" -- minimum ages that matter to
//     your app, e.g. 13, 15, 18.
//   * The reply is a range with an inclusive `lowerBound` and an OPTIONAL
//     `upperBound`:
//       - nil lowerBound -> below your lowest gate
//       - nil upperBound -> at or above your highest gate ("16 or over")
//       - Apple's documentation is explicit that the reply must be read as a
//         BAND, never as an exact age.
//   * The response is either `.sharing(range)` or `.declinedSharing`.
//   * Alongside the range comes `ageRangeDeclaration`:
//       - `.selfDeclared`   -- the person entered it themselves
//       - `.guardianDeclared` -- a parent, guardian or Family Organiser did
//     Children are ALWAYS guardianDeclared. Teens not in an iCloud Family
//     group may be selfDeclared. Adults are selfDeclared.
//   * Requires the `com.apple.declared-age-range` entitlement.
//
// THE PART THAT MATTERS MOST
//
// A band is not assurance. Apple can return an 18+ band that was SELF
// declared -- a teenager typing a number, or an adult doing the same. The
// range says 18; nothing in it says anyone verified anything.
//
// Attestation::IsMinor() therefore refuses a self-declared adult, and this
// suite proves Apple's self-declared path reaches that guard rather than
// bypassing it. Getting this wrong is how a fourteen-year-old unlocks every
// age gate in a game: the band is genuinely Adult18, so nothing downstream
// re-checks it.

// The ONLY dependency: printf for the failure messages and the summary.
// There is no test framework here -- no gtest, no catch2 -- because
// the file is meant to compile as a standalone desktop binary that
// links only the Age module.
#include <cstdio>

// Quoted: resolved from the include path. AgeBand and AssuranceLevel
// are the value types every assertion compares against.
#include "Wasteland/Age/AgeBand.h"
// The provider header, for the three pure functions under test:
// BandFromAppleRange, LevelForAppleDeclaration and
// AttestationFromApple.
// DEFECT (testability seam): these are declared here and defined
// elsewhere, so this test only proves the mapping if it is LINKED
// against the real definitions rather than against test-local stubs.
// That is the right thing to do and the header comment says so, but
// nothing in this file asserts it -- a stub returning AgeBand::Unknown
// for everything would fail loudly here, so the tests do have teeth.
#include "Wasteland/Age/AppleAgeSignalProvider.h"

// A using-DIRECTIVE, not an alias. It brings every name in the
// namespace into this file, so an AgeBand added later is available
// without another edit.
// Style note: a file-scope using-directive is usually discouraged, but
// in a 150-line test that names AgeBand on nearly every line it is the
// right trade.
using namespace Wasteland::Age;

// file-static counters rather than members of a test fixture: there is
// no fixture, and CHECK on line 58 is a macro so it cannot capture a
// local.
// DEFECT: zero-initialised implicitly, which is fine, but they are
// globals mutated by every test function -- so this suite cannot run
// concurrently, and a test that returned early would still leave its
// count in the totals.
static int g_Failures = 0;
static int g_Checks = 0;

// The whole test framework: evaluate a condition, count it, print a
// message if it failed.
// do/while(0) rather than a bare `if`, so the macro is a single
// statement and a caller can write `if (x) CHECK(...); else ...`
// without a dangling-else problem.
// DEFECT (macro hygiene): `what` and `cond` are evaluated inside
// the if, so both appear once -- correct. But there is no `else`
// reporting a pass, so nothing distinguishes "the condition was
// skipped" from "the condition passed", and no way to bail out early.
// More significant: the macro hardcodes printf rather than the
// engine logging the rest of this codebase uses, which is correct for a
// standalone test but means its output is not captured by any test
// runner.
#define CHECK(cond, what)                                                        \
	do                                                                           \
	{                                                                            \
		++g_Checks;                                                              \
		if (!(cond))                                                             \
		{                                                                        \
			++g_Failures;                                                        \
			// Two leading spaces so failures indent under the header comment
			// style used above.
			// The parenthesised `what` is unnecessary for a plain string
			// literal argument but harmless -- and it would matter if a caller
			// passed an expression.
			std::printf("  FAIL: %s\n", (what));                                  \
		}                                                                        \
	} while (0)

// file-static test functions, called by hand from main. No
// registration, no name mangling tricks, no framework.
// DEFECT (coverage gap visible here): this function name says
// "ClosedRanges" but it also asserts open-ended cases on lines 74
// and 78, which are the same subject as the NEXT function. The two
// overlap, and the split does not match the assertion content.
static void TestClosedRangesFromAppleGates()
{
	// The provenance note matters: these are not invented cases but
	// values from a vendor document, which is what makes them a
	// regression guard rather than a guess.
	// Apple's own worked example: gates at 13 and 16.
	CHECK(BandFromAppleRange(13, 15) == AgeBand::Age13To15,
	      "Apple [13,15] (Olivia, 14, from Apple's WWDC25 example) maps to Age13To15");
	CHECK(BandFromAppleRange(16, AppleNoUpperBound) == AgeBand::Age16To17,
	      "Apple [16, or over] (Ann, 42) maps to Age16To17 -- the most restrictive band consistent with the bound");

	// Second existing comment group in this function.
	// The default gate set 13/15/18 from Apple's guidance.
	CHECK(BandFromAppleRange(AppleNoLowerBound, AppleNoUpperBound) == AgeBand::Under13,
	      "nil lowerBound means below every gate, so Under13");
	CHECK(BandFromAppleRange(13, 14) == AgeBand::Age13To15, "Apple [13,14] maps to Age13To15");
	CHECK(BandFromAppleRange(15, 17) == AgeBand::Age13To15, "Apple [15,17] maps to Age13To15 (15 is inside 13-15)");
	CHECK(BandFromAppleRange(18, AppleNoUpperBound) == AgeBand::Adult18, "Apple [18, or over] maps to Adult18");
}

// The second test group.
// DEFECT (redundancy): line 74 in the previous function asserts
// BandFromAppleRange(16, AppleNoUpperBound) == Age16To17 and line 92
// here asserts exactly the same thing. A duplicate assertion is
// harmless, but it means one of the two functions could be deleted
// without losing coverage, which is a sign the grouping drifted.
static void TestOpenEndedBoundsAreReadConservatively()
{
	// The most important reasoning in the file, stated as a comment so
	// nobody "fixes" the assertion on line 90 to expect Adult18.
	// "13 or over" does NOT mean adult. It means at least 13, which is
	// consistent with being 13. Reading the lower bound is the only
	// interpretation that cannot hand a thirteen-year-old an adult profile.
	CHECK(BandFromAppleRange(13, AppleNoUpperBound) == AgeBand::Age13To15,
	      "13 or over must NOT resolve to adult; it must resolve to Age13To15");
	CHECK(BandFromAppleRange(16, AppleNoUpperBound) == AgeBand::Age16To17, "16 or over maps to Age16To17");
	CHECK(BandFromAppleRange(17, AppleNoUpperBound) == AgeBand::Age16To17, "17 or over maps to Age16To17");
	CHECK(BandFromAppleRange(18, AppleNoUpperBound) == AgeBand::Adult18, "18 or over maps to Adult18");
	CHECK(BandFromAppleRange(21, AppleNoUpperBound) == AgeBand::Adult18, "21 or over maps to Adult18");
	CHECK(BandFromAppleRange(4, AppleNoUpperBound) == AgeBand::Under13, "4 or over maps to Under13");
}

// Negative and inverted bounds.
// DEFECT (coverage gap): AppleAgeSignalProvider.h is not visible
// from here, so it cannot be said whether these inputs are reachable
// in practice. If BandFromAppleRange is only ever called with
// values Apple actually returned, these three assertions guard
// against a case that cannot occur -- which is fine as defence in
// depth, but there is no test here for the range Apple DOES return
// that has a nil lowerBound with a finite upperBound (the one
// combination the header comment describes and none of these tests
// exercise).
static void TestMalformedRangesFailClosed()
{
	// A negative age is not a real Apple reply, so this is testing the
	// function's own guard rather than Apple's behaviour.
	CHECK(BandFromAppleRange(-5, 12) == AgeBand::Under13, "a negative lower bound must fail closed to Under13");
	CHECK(BandFromAppleRange(13, 5) == AgeBand::Under13, "an inverted range (13..5) must fail closed to Under13");
	CHECK(BandFromAppleRange(12, 11) == AgeBand::Under13, "an inverted range (12..11) must fail closed to Under13");
}

// Maps AppleAgeDeclaration to AssuranceLevel.
// DEFECT (frozen behaviour): Art 3(i) is cited in the comment on
// line 113 as the reason self-declaration maps to SelfDeclared. A test
// that encodes a citation from a PENDING proposal (per AgeService.h
// line 30, COM(2026) 681 is not yet in force) will outlive the text it
// cites. When the instrument is finalised or withdrawn, this assertion
// becomes either wrong or stale and nothing here says which.
static void TestDeclarationDecidesTheAssuranceLevel()
{
	// A guardian-declared band is somebody else vouching for the child.
	// That is a real assurance method and is the normal path for children.
	CHECK(LevelForAppleDeclaration(AppleAgeDeclaration::GuardianDeclared) == AssuranceLevel::OsSignal,
	      "a guardian-declared range is a platform signal");

	// A self-declared band is the person saying so. Art 3(i) of the EU KIDS
	// Act proposal (COM(2026) 681) excludes self-declaration from "age
	// assurance", so it maps to SelfDeclared and unlocks nothing.
	CHECK(LevelForAppleDeclaration(AppleAgeDeclaration::SelfDeclared) == AssuranceLevel::SelfDeclared,
	      "a self-declared range must map to SelfDeclared, never to a real assurance level");

	// Both collapse to SelfDeclared, which is the most restrictive
	// non-OsSignal level -- a fail-closed choice rather than a claim that
	// the user self-declared.
	// Never-shared and undetermined are not assurances at all.
	CHECK(LevelForAppleDeclaration(AppleAgeDeclaration::NeverShared) == AssuranceLevel::SelfDeclared,
	      "never-shared must map to SelfDeclared");
	CHECK(LevelForAppleDeclaration(AppleAgeDeclaration::Undetermined) == AssuranceLevel::SelfDeclared,
	      "undetermined must map to SelfDeclared");
}

// The out-of-range enum value case.
static void TestUnknownDeclarationFailsClosed()
{
	// static_cast of an out-of-range int into an enum with no fixed
	// underlying type is conditionally supported pre-C++17 and well
	// defined for a scoped enum whose underlying type can represent 77
	// (which int can).
	// 77 is arbitrary but chosen to be well clear of the real values, so
	// the test does not accidentally hit one.
	const AppleAgeDeclaration garbage = static_cast<AppleAgeDeclaration>(77);
	CHECK(LevelForAppleDeclaration(garbage) == AssuranceLevel::SelfDeclared,
	      "an unrecognised declaration must fail closed to SelfDeclared");
}

// The headline test the file exists for.
static void TestSelfDeclaredAdultIsNotAnAdult()
{
	// THE bug this provider exists to prevent. An 18+ range that was self
	// declared is an unverified claim of adulthood, and must leave the
	// player on the minor side of every gate.
	// const local, and the type is copy-constructible by design --
	// GuardianPolicy.h and Attestation both forbid in-place assignment
	// for the same reason, so every test here binds a fresh value.
	const Attestation att = AttestationFromApple(18, AppleNoUpperBound, AppleAgeDeclaration::SelfDeclared);
	CHECK(att.Band() == AgeBand::Adult18, "the band itself is Adult18 -- Apple really did say 18 or over");
	CHECK(att.Level() == AssuranceLevel::SelfDeclared, "the level must record that nobody verified it");
	CHECK(att.IsMinor(), "a self-declared 18+ must still report as minor");
	CHECK(att.Method() == "apple-declared-age-range", "the audit trail must name the method");
}

// The contrast case to the one above: same band, different
// declaration, opposite minor answer.
static void TestGuardianDeclaredAdultIsAnAdult()
{
	// A guardian-declared 18+ is a different thing: a parent vouched.
	const Attestation att = AttestationFromApple(18, AppleNoUpperBound, AppleAgeDeclaration::GuardianDeclared);
	CHECK(att.Band() == AgeBand::Adult18, "band is Adult18");
	CHECK(att.Level() == AssuranceLevel::OsSignal, "a guardian declaration is a platform signal");
	// This is the ONLY assertion in the file that expects a NOT-minor
	// result for an age input. Everything else is restrictive, so if
	// IsMinor() were inverted globally most of this suite would still
	// pass -- this line and line 141 are the pair that pins the polarity.
	CHECK(!att.IsMinor(), "a guardian-declared adult must not report as minor");
}

// The refusal paths.
static void TestDeclinedAndUndeterminedAreMinor()
{
	// `.declinedSharing` and "never share" are ordinary outcomes, not
	// errors, and they mean there is no signal. Both must be restrictive.
	// Range-for over a braced initialiser list, which needs no
	// #include<initializer_list> because it is deduced.
	// The loop variable is decl rather than d.
	for (AppleAgeDeclaration decl : {AppleAgeDeclaration::NeverShared, AppleAgeDeclaration::Undetermined})
	{
		// Both bounds nil: the maximal "we told you nothing" input, which
		// must land on Unknown rather than on Under13.
		// Note the shadowing of att from the earlier functions is avoided --
		// this is a new scope, and the earlier atts are out of scope by now,
		// so there is no -Wshadow issue.
		const Attestation att = AttestationFromApple(AppleNoLowerBound, AppleNoUpperBound, decl);
		CHECK(att.Band() == AgeBand::Unknown, "declined or undetermined must yield an Unknown band");
		CHECK(att.IsMinor(), "declined or undetermined must report as minor");
	}
}

// The normal child path.
static void TestGuardianDeclaredMinor()
{
	// The normal child path, and the one Apple documents: children are always
	// guardian-declared.
	// A closed range with an in-between upper bound, so this also
	// exercises the same BandFromAppleRange(13,14) case asserted in
	// TestClosedRangesFromAppleGates line 80 -- but through the full
	// AttestationFromApple path, which the earlier test does not.
	const Attestation att = AttestationFromApple(13, 14, AppleAgeDeclaration::GuardianDeclared);
	CHECK(att.Band() == AgeBand::Age13To15, "a guardian-declared 13-14 maps to Age13To15");
	CHECK(att.Level() == AssuranceLevel::OsSignal, "and is a platform signal");
	CHECK(att.IsMinor(), "and must report as minor");
}

// The only test that constructs the real provider object rather than
// calling the pure functions.
static void TestProviderIsUnavailableOnDesktop()
{
	// The engine has no iOS backend -- Core.h rejects it with #error -- so
	// the provider must admit it cannot work here rather than pretending.
	// AgeService treats an unavailable provider as no provider, which means
	// Absent() and the restrictive SafetyProfile, which is the correct
	// outcome for a desktop build with no assurance available.
	// Default-constructed on the stack, then destroyed at scope exit.
	// DEFECT (untested): nothing here checks the provider's destructor,
	// or what happens if Query() (line 186) is called after the object has
	// been moved from. Query() is also called TWICE in this function
	// (lines 186 and 187) and each call constructs a fresh Attestation --
	// fine for a pure-ish provider, but it means the test does not
	// establish that two calls return the same thing.
	AppleAgeSignalProvider provider;

	CHECK(!provider.IsAvailable(), "the Apple provider must report unavailable off iOS");
	// Query() on an unavailable provider. AgeService.cpp treats this as
	// Absent(), and this line proves the provider agrees -- but note the
	// provider returns a fail-closed Attestation directly rather than an
	// error, so a caller cannot distinguish "unavailable" from "asked and
	// got nothing". AgeService makes them indistinguishable on purpose, so
	// this is consistent, but it is a design decision rather than an
	// accident.
	CHECK(provider.Query().IsMinor(), "an unavailable provider must return a fail-closed attestation");
	CHECK(provider.Query().Band() == AgeBand::Unknown, "and an Unknown band");
	// A string comparison on a stable identifier.
	// DEFECT (literal duplication): this exact string is asserted again
	// on line 142 for Attestation::Method(). Two different sources -- a
	// method name and a provider name -- are hardcoded as independent
	// literals here. If the provider renames itself, one assertion fails
	// and the other may not, which is the signal; but nothing in this file
	// ties them together, and neither string is checked against the
	// header.
	CHECK(provider.ProviderName() == "apple-declared-age-range", "the provider name must be stable for the audit trail");
}

// No argc/argv, so the binary cannot be parameterised -- which is
// fine, since there is nothing to pass.
// Returning int rather than using a framework entry point is what
// makes this runnable directly.
int main()
{
	// Ten hand-called functions in order.
	// DEFECT (no ordering dependency, but also no isolation): each test
	// runs against the same globals from the CHECK macro. A test that
	// threw would skip the remaining ones silently and the summary line
	// would still print.
	TestClosedRangesFromAppleGates();
	TestOpenEndedBoundsAreReadConservatively();
	TestMalformedRangesFailClosed();
	TestDeclarationDecidesTheAssuranceLevel();
	TestUnknownDeclarationFailsClosed();
	TestSelfDeclaredAdultIsNotAnAdult();
	TestGuardianDeclaredAdultIsAnAdult();
	TestDeclinedAndUndeterminedAreMinor();
	TestGuardianDeclaredMinor();
	TestProviderIsUnavailableOnDesktop();

	// The machine-readable summary. The process exit code on the next
	// line is what a CI job would actually gate on -- the printed counts
	// are for humans.
	std::printf("test_apple_provider: %d checks, %d failures\n", g_Checks, g_Failures);
	// Conventional exit code: nonzero means failure. The ternary yields
	// an int via the usual integral promotions, so the implicit
	// conversion to the function return type is fine.
	return g_Failures ? 1 : 0;
}