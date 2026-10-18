// test_age_band -- a self-contained executable that exercises AgeBand.h with no
// engine, no window and no GPU. It links against the header's implementation
// only, which is what makes it runnable in a build step with no graphics context.
//
// WHY A HAND-WRITTEN HARNESS: the file header explains it -- AgeBand is pure
// arithmetic, so MSBuild proves it compiles and this proves it is correct. The
// property under test is MONOTONICITY, and the header calls a non-monotonic
// result a compliance defect rather than a logic bug.
// Standalone functional test for Wasteland::Age::AgeBand.
//
// AgeBand is pure arithmetic on integers with no engine dependency, so MSBuild
// proves it compiles and this proves it is correct. The property that matters
// is monotonicity: MeetsThreshold is what JurisdictionPolicy, SafetyProfile
// and every studio integration branch on, and a non-monotonic result is a
// compliance defect rather than a logic bug -- an adult who fails to meet a
// lower threshold is a minor incorrectly gated out of their own game.
//
// Sources for the bands themselves:
//   - US App Store Accountability Acts (TX/UT/LA) and the Apple Declared Age
//     Range / Google Play Age Signals / Amazon GetUserAgeData APIs all use
//     0-12, 13-15, 16-17, 18+.
//   - The EU KIDS Act proposal (COM(2026) 681, 2026/0286(COD)) splits 13-15
//     into 13-14 (guardian-supervised) and 15 (autonomous accounts).
// Those do NOT align, and this module deliberately does not reconcile them.
// See the module README and "D4" in
// .hermes/plans/2026-10-01_171500-wasteland-age-assurance.md.

// For the printf calls. Deliberately not iostream: the CHECK macro formats with
// %d and %s, and mixing the two would be noisier than it is worth.
#include <cstdio>
// For the std::string returned by BandName, whose results are tested with
// .empty() in TestName below.
#include <string>

// The one and only engine header. Its own include of string is what the line
// above duplicates -- the explicit include is belt-and-braces, not a mistake.
//
// Included by full path rather than relatively: this file lives in tests/, well
// away from the header, so the include search path must be set up for it. That
// also couples the test to the engine's include layout rather than to a copy.
#include "Wasteland/Age/AgeBand.h"

// A using-DIRECTIVE, not a declaration, so every AgeBand name below is
// unqualified. It is the only such directive in the file and there is no
// namespace of the test's own -- main() and the test functions are at global
// scope.
//
// Acceptable for a test binary; in engine code the same unqualified use would be
// a name-collision hazard.
using namespace Wasteland::Age;

// The two counters every CHECK increments. `static` gives them internal linkage,
// so they cannot collide with anything if this file is ever linked into a larger
// binary.
//
// Globals rather than a fixture object: the macros need them, and C++ has no way
// to reach an instance from a function-like macro.
static int g_Failures = 0;
// The pass counter. Reported at the end so the summary reads "N checks, M
// failures" -- which is how a caller distinguishes "all passed" from "nothing
// ran".
//
// DEFECT (counting, documented in detail below): this is NOT a count of
// comparisons. The two loop tests increment it once per FAILURE plus once at the
// end, so the total is roughly "assertions plus failures". A caller reading the
// number as a coverage figure would be misled. The exit code depends only on
// g_Failures, so correctness is unaffected.
static int g_Checks = 0;

// The boolean assertion macro.
//
// do { } while (0) rather than a bare `if`: the do-while makes the macro a single
// STATEMENT, so `if (x) CHECK(a, b); else ...` compiles and the else binds to
// the caller's if rather than to a dangling macro else. That is the standard
// reason for the idiom, and getting it wrong is a classic macro bug.
//
// Every line ends with a backslash continuation, which is what lets the body
// span multiple lines. DEFECT (fragility): inserting a comment on an interior
// line WITHOUT its own trailing backslash silently truncates the macro and the
// rest of the body becomes stray text at file scope. There is deliberately no
// comment inside either macro body for exactly that reason.
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

// The equality variant, and the only reason this file can compare values that
// are expensive or effectful.
//
// Each argument is bound to a local `const auto` first. Without that, an
// expression with a side effect would be evaluated TWICE -- once in the
// comparison and once in the failure message -- and the printed value could
// differ from the compared one. The temporaries also ensure the comparison and
// the print refer to the same objects.
//
// The trailing-underscore names avoid shadowing anything in the caller's scope,
// which matters because these are macros expanded into someone else's code.
#define CHECK_EQ(actual, expected, what)                                         \
	do                                                                           \
	{                                                                            \
		++g_Checks;                                                              \
		const auto a_ = (actual);                                                \
		const auto e_ = (expected);                                              \
		if (!(a_ == e_))                                                         \
		{                                                                        \
			++g_Failures;                                                        \
			std::printf("  FAIL: %s (got %d, expected %d)\n", (what),             \
			            static_cast<int>(a_), static_cast<int>(e_));             \
		}                                                                        \
	} while (0)

// The band-boundary tests.
//
// `static` on every test function: internal linkage, so the linker can discard
// any that are not called and no name can clash. The cost is that main() must
// name each one explicitly -- which is a benefit, because adding a test function
// without calling it would otherwise be a silent skip.
static void TestBandEdges()
{
	// Band edges are tested at BOTH ends of every band: 0, 12, 13, 15, 16, 17, 18.
	// Two adjacent assertions per boundary is what makes an off-by-one detectable --
	// testing only 13 would pass whether the boundary sits at 12 or at 13.
	CHECK(BandForAge(0) == AgeBand::Under13, "0 is under 13");
	CHECK(BandForAge(12) == AgeBand::Under13, "12 is under 13");
	CHECK(BandForAge(13) == AgeBand::Age13To15, "13 is in the teen band");
	// 15 stays inside 13-15: the US scheme does not distinguish it, and the
	// EU's 15 boundary is applied by JurisdictionPolicy, not here.
	CHECK(BandForAge(15) == AgeBand::Age13To15, "15 is still 13-15 in the US scheme");
	CHECK(BandForAge(16) == AgeBand::Age16To17, "16 starts the older-teen band");
	CHECK(BandForAge(17) == AgeBand::Age16To17, "17 is still the older-teen band");
	CHECK(BandForAge(18) == AgeBand::Adult18, "18 is adult");
	// The overflow probe: 120 is past any real age, so it exercises the unbounded
	// path (kBandUpperBound's -1 sentinel meaning "no upper bound") rather than a
	// table entry.
	//
	// The message says what it guards -- an out-of-range index or an arithmetic
	// overflow would give something other than "adult".
	CHECK(BandForAge(120) == AgeBand::Adult18, "an implausible age is still adult, not an overflow");
}

// The most important test in the file, and the one whose failure mode is invisible
// in every other test.
//
// BandForAge takes `int` rather than `unsigned` precisely so this case is
// expressible. With an unsigned parameter a -1 from a mis-parsed provider
// response becomes 4294967295, which classifies as Adult18 -- an unverified user
// given adult treatment, the specific outcome the US and EU instruments exist to
// prevent.
static void TestNegativeAgeIsRejected()
{
	// An unsigned parameter here would silently turn -1 into a huge age and
	// classify it Adult18 -- the single worst failure this type can have.
	// The flag-then-try pattern: initialised before the call, set again in the catch.
	//
	// The alternative, a CHECK_THROWS helper, would need a third macro; this keeps
	// the file to two.
	bool threw = false;
	try
	{
		// The call under test. Its RESULT is discarded -- only whether it throws matters.
		//
		// Not marked [[nodiscard]] at the call site because the function is not declared
		// that way; discarding a non-void return is legal and MSVC does not warn by
		// default.
		//
		// This compiles and behaves as intended only because BandForAge throws on a
		// negative age, as its header states. If it ever stopped, this test would fail --
		// which is the point.
		BandForAge(-1);
	}
	// Catching the BASE type, so any std::exception-derived throw is accepted. The
	// test asserts that SOMETHING was thrown, not what.
	//
	// Loose by design: this is about the negative-age contract, and pinning the
	// exception type would couple the test to an implementation detail that is free
	// to change.
	//
	// The parameter is unnamed, so it is unused and no unused-parameter warning fires.
	catch (const std::exception &)
	{
		threw = true;
	}
	CHECK(threw, "BandForAge must reject a negative age rather than wrapping it to adult");
}

// MeetsThreshold at its boundaries, for both the int and the AgeBand overload.
//
// The two forms are checked separately here, then cross-checked exhaustively by
// TestBandLookupAgreesWithAgeLookup below.
static void TestThresholdEdges()
{
	CHECK(!MeetsThreshold(12, 13), "12 must not meet 13");
	CHECK(MeetsThreshold(13, 13), "13 meets 13");
	CHECK(MeetsThreshold(15, 15), "15 meets 15");
	CHECK(!MeetsThreshold(15, 16), "15 must not meet 16");
	CHECK(MeetsThreshold(16, 16), "16 meets 16");
	CHECK(MeetsThreshold(18, 18), "18 meets 18");
	CHECK(MeetsThreshold(40, 18), "an adult meets 18");

	// The compliance-critical assertions, and the reason the module treats
	// monotonicity as a safety property rather than a nicety.
	//
	// Art 8(1) permits derogating the safety-by-design defaults only once adulthood is
	// ESTABLISHED. So the adult threshold gates that derogation, and a minor
	// satisfying it would let every protection be waived on an unverified age.
	// A minor must never satisfy an adult-only threshold. Art 8(1) permits
	// derogating the safety-by-design defaults only once adulthood is
	// established, so this is the check that gates that derogation.
	CHECK(!MeetsThreshold(AgeBand::Under13, 18), "under-13 must not meet 18");
	CHECK(!MeetsThreshold(AgeBand::Age13To15, 18), "13-15 must not meet 18");
	CHECK(!MeetsThreshold(AgeBand::Age16To17, 18), "16-17 must not meet 18");
	CHECK(MeetsThreshold(AgeBand::Adult18, 18), "adult meets 18");

	// Unknown's fail-closed property, tested explicitly.
	//
	// Unknown is -1 in the enum (AgeBand.h), below every band's lower bound -- so a
	// table-driven implementation naturally lands it in no band at all. This test
	// makes that a GUARANTEE rather than an accident of the table.
	//
	// The comment names the failure it prevents: a permissive Unknown would hand
	// every user with no age signal full adult treatment.
	// Unknown is not a band that satisfies anything. Treating it as
	// permissive would hand every unverified user adult treatment.
	CHECK(!MeetsThreshold(AgeBand::Unknown, 13), "Unknown must not meet 13");
	CHECK(!MeetsThreshold(AgeBand::Unknown, 18), "Unknown must not meet 18");
}

// THE property test. Everything else here checks specific values; this one checks
// the rule that makes those values coherent.
//
// Monotonicity: meeting a threshold implies meeting every lower one. If it held,
// the hand-written edge tests above would be sufficient -- and this test is what
// justifies trusting them instead of enumerating 0..120 by hand.
static void TestMonotonicity()
{
	// The pre-existing comment explains the range choice, and the reasoning is worth
	// keeping: edge cases are exactly what a hand-written table gets RIGHT, because
	// that is what its author checked while writing it. An off-by-one in the MIDDLE
	// of a range slips past edge-only testing. Hence the full sweep.
	// Meeting a threshold implies meeting every lower one. Checked across the
	// whole plausible range rather than at edges, because the edge cases are
	// exactly what a hand-written table gets right while an off-by-one in the
	// middle slips through.
	// The four real thresholds, matching the store schemes' band boundaries.
	//
	// `static const` inside a function gives a local array static storage duration.
	// Cosmetic here -- a plain `const` local would behave identically, and the two
	// spellings are inconsistent with the brace-initializer used in the next test.
	//
	// Duplicated rather than shared with AgeBand.h's tables, and that is correct:
	// the header exports band BOUNDS while this needs THRESHOLDS. Deriving one from
	// the other inside the test would make the test tautological.
	static const int kThresholds[] = {13, 15, 16, 18};

	// 0 to 120 inclusive: above any plausible human age, below anything that could
	// overflow, so the sweep is total over the interesting domain without being
	// unbounded.
	//
	// 484 iterations (121 ages x 4 thresholds), each with an inner loop of at most 4
	// -- a few thousand comparisons. Instant, and it runs on every invocation.
	for (int age = 0; age <= 120; ++age)
	{
		for (int t : kThresholds)
		{
			// The implication's premise: only ages that DO meet t need checking against the
			// lower thresholds. Skipping the rest is what keeps this O(n x 4).
			if (!MeetsThreshold(age, t))
				continue;
			for (int lower : kThresholds)
			{
				// The implication itself, and both halves of the guard are needed:
				//   lower < t    -- restricts to genuinely LOWER thresholds; without it every
				//                  pair including equal and higher ones would be checked, and
				//                  the property says nothing about those
				//   !Meets...   -- the violation being detected
				//
				// A failure here is a real non-monotonicity: an age that passes 18 but fails 13,
				// meaning the table's bounds are inconsistent with each other.
				if (lower < t && !MeetsThreshold(age, lower))
				{
					++g_Checks;
					++g_Failures;
					// The failure message names all three values -- the age and both thresholds -- so
					// a failure is diagnosable without a debugger.
					//
					// This is the pattern the two loop tests share instead of using CHECK_EQ: the
					// condition involves a loop variable, so there is no single expression to hand
					// a macro.
					std::printf("  FAIL: age %d meets %d but not %d\n", age, t, lower);
				}
			}
		}
	}
	// The lump increment after the sweep.
	//
	// DEFECT (misleading count): this adds ONE regardless of how many comparisons
	// ran, and the loop above adds one per FAILURE. The reported "checks" figure is
	// therefore not the number of comparisons performed.
	//
	// Harmless for pass/fail -- the exit code depends on g_Failures -- but the
	// summary line's number is not what a reader would assume it is.
	++g_Checks;
}

// Cross-checks the two MeetsThreshold overloads against each other across the
// whole range.
//
// This is what justifies treating the enum as canonical: it proves the band
// overload is not a second, independently-implemented answer that could drift.
static void TestBandLookupAgreesWithAgeLookup()
{
	// The pre-existing comment gives the rationale, and it is a usability argument
	// rather than a correctness one: studios receive a BAND from an assurance
	// provider, not an age, so the band overload is the one real integrations use.
	//
	// Which makes the disagreement this test guards against consequential: if the two
	// forms diverged, the engine's answer would depend on which overload a given
	// call site happened to use, and that would differ per studio.
	// MeetsThreshold(AgeBand, n) is the form studios will actually call, since
	// they receive a band from a provider rather than an age. It has to agree
	// with MeetsThreshold(int, n) for every age, or the two entry points drift
	// and the engine's answer depends on which one a given call site used.
	for (int age = 0; age <= 120; ++age)
	{
		// The same four thresholds, spelled as a braced list rather than reusing the
		// array from TestMonotonicity.
		//
		// DEFECT (duplication): the two tests must be kept in step by hand. Adding a
		// threshold to one and not the other silently reduces coverage -- the
		// monotonicity test would not check the new value against the others. A shared
		// file-scope constant would remove the coupling.
		//
		// Range-based for over an initializer_list, so the braces deduce int from the
		// literals, matching MeetsThreshold's parameter type.
		for (int t : {13, 15, 16, 18})
		{
			// Both answers computed, then compared.
			//
			// Bound to locals rather than compared inline, so the failure printf below can
			// print BOTH results -- which is what makes a disagreement diagnosable without
			// re-running under a debugger.
			//
			// Each is a real function call (AgeBand.h declares rather than defines these), so
			// this sweep performs roughly 968 calls. Still instant.
			const bool viaAge = MeetsThreshold(age, t);
			const bool viaBand = MeetsThreshold(BandForAge(age), t);
			if (viaAge != viaBand)
			{
				++g_Checks;
				++g_Failures;
				// The disagreement report: the age, the threshold, and both answers.
				//
				// The trailing labels on the last two values are load-bearing -- without them a
				// reader could not tell which overload produced which answer, which is the one
				// thing the message exists to convey.
				//
				// static_cast<int> on the bools, because %d is not the right specifier for a bool
				// in a varargs call.
				std::printf("  FAIL: age %d threshold %d disagrees: age=%d band=%d\n",
				            age, t, static_cast<int>(viaAge), static_cast<int>(viaBand));
			}
		}
	}
	++g_Checks;
}

// BandName's contract, and the only test here about user-visible strings.
static void TestName()
{
	// The pre-existing comment gives the reason, and it is a compliance one rather
	// than a cosmetic one: these labels appear in user-facing refusals and in
	// compliance evidence, so a missing one is a support problem.
	//
	// That is why the header promises "never empty, including for Unknown" and why
	// the first assertion below tests Unknown specifically -- it is the band most
	// likely to be mishandled, since it has no numeric band behind it.
	// Studios surface these strings to users and regulators. An empty or
	// missing label for a band that gates a minor is a support problem.
	// The Unknown case first, and deliberately so: it is the value a provider returns
	// when nothing was signalled, so it is the one most likely to reach a lookup
	// table with no entry for it.
	CHECK(!BandName(AgeBand::Unknown).empty(), "Unknown has a name");
	CHECK(!BandName(AgeBand::Under13).empty(), "Under13 has a name");
	CHECK(!BandName(AgeBand::Age13To15).empty(), "Age13To15 has a name");
	CHECK(!BandName(AgeBand::Age16To17).empty(), "Age16To17 has a name");
	CHECK(!BandName(AgeBand::Adult18).empty(), "Adult18 has a name");

	// Distinctness, and the only assertion that could catch a copy-paste error in
	// the name table -- a hand-written switch with a copied return would pass every
	// emptiness check above.
	//
	// DEFECT (weak coverage): only ONE pair is compared, not all ten. A table with
	// Under13 equal to Age13To15 but everything else distinct would pass. Comparing
	// every pair, or checking uniqueness against a set, would be thorough.
	CHECK(BandName(AgeBand::Under13) != BandName(AgeBand::Age13To15),
	      "distinct bands have distinct names");
}

// The entry point, and the only place the test functions are referenced -- which
// is what makes an unwritten test a link error rather than a silent skip.
//
// Returns int so the process exit code can signal pass/fail to CI. No argv
// handling: there are no options to pass.
int main()
{
	// Six calls, in dependency-free order. Each test function is self-contained and
	// increments the shared counters, so the order affects only the output sequence,
	// not the result.
	//
	// DEFECT (no isolation): a failure in one test does not stop the others, and
	// there is no per-test summary. A reader sees interleaved FAIL lines with no
	// indication of which test produced each -- so the `what` strings have to carry
	// that, which is why every one below is written as a full sentence.
	TestBandEdges();
	TestNegativeAgeIsRejected();
	TestThresholdEdges();
	TestMonotonicity();
	TestBandLookupAgreesWithAgeLookup();
	TestName();

	// The summary line, named after the file so a CI log is self-identifying.
	//
	// Printed even when everything passed, so a successful run produces output rather
	// than silence.
	//
	// The counts are subject to the accounting quirk noted above: the figure is not a
	// count of comparisons.
	std::printf("test_age_band: %d checks, %d failures\n", g_Checks, g_Failures);
	// 0 for pass, 1 for any failure -- the contract every CI system expects.
	//
	// A ternary rather than `return !!g_Failures`, so the success value is the
	// literal 0 rather than a converted bool. The distinction is academic; the
	// explicitness is the point.
	//
	// The counter is never reset between tests, so this is the total across all six.
	return g_Failures ? 1 : 0;
}