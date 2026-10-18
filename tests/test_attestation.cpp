// Standalone functional test for Wasteland::Age::Attestation and
// IAgeAssuranceProvider.
//
// Two things are proven here, and both are load-bearing.
//
// 1. Attestation fails CLOSED. The default-constructed value reports the
//    player as a minor, because it must: a provider that returns "no
//    signal", or a provider that fails, or a caller that forgets to check
//    the result, all have to land on the restrictive answer. An Attestation
//    is a safety input, not a convenience.
//
// 2. A self-declared adult is NOT an adult. AgeBand::Adult18 reached via
//    AssuranceLevel::SelfDeclared -- a date-of-birth box that the player
//    filled in -- is exactly the case the EU proposal excludes from "age
//    assurance" by Art 3(i), and exactly the case that must not unlock
//    anything. A band alone is not sufficient evidence of adulthood; it has
//    to be a band that some assurance method actually vouched for.
//
// The interface itself is proven implementable: a studio has to be able to
// subclass IAgeAssuranceProvider and have the engine use it, and a test that
// never subclasses it cannot show that.

// For std::printf in the CHECK macro below and in main's summary line.
#include <cstdio>
#include <string>
// For std::has_virtual_destructor, used as a static_assert near the bottom.
// A compile-time trait is the right tool here: the check must hold at BUILD
// time, not merely when a test happens to run and delete an object.
#include <type_traits>

// For AgeBand and its fail-closed semantics -- specifically that Unknown is a
// member of the enum rather than a sentinel, which is what lets the
// default-constructed Attestation report Unknown without inventing a value.
#include "Wasteland/Age/AgeBand.h"
// The type under test. Attestation and its factories are what every check
// here exercises.
#include "Wasteland/Age/Attestation.h"
// The interface, included so FakeProvider below can derive from it -- which
// is itself part of what the test proves, as the file's header comment says.
#include "Wasteland/Age/IAgeAssuranceProvider.h"

using namespace Wasteland::Age;

// Shared failure count, so no test function needs a fixture or a return
// value. Static keeps the name internal to this translation unit.
static int g_Failures = 0;
// Check counter, incremented by the macro. Printed in main so a run that
// executed nothing is visible rather than indistinguishable from success.
static int g_Checks = 0;

// Identical to the one in test_policy.cpp -- counted, non-fatal, reporting
// only failures.
//
// The macro spans backslash continuations, so this comment block sits after
// the macro's closing line rather than inside it.
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

// A studio-supplied provider, of the kind that would wrap a real vendor SDK
// or a platform API. Doubles as proof the interface is implementable.
// A studio-supplied provider, of the kind that would wrap a real vendor SDK
// or a platform age API.
//
// `final` matters: it proves the interface is subclassable while making it
// impossible to subclass THIS type again. The engine never needs to, and
// the keyword documents that FakeProvider is a test double rather than a base
// for further test doubles.
class FakeProvider final : public IAgeAssuranceProvider
{
public:
	// Takes its result BY VALUE and moves it into the member.
	//
	// @param result      The attestation Query() will return.
	// @param available   What IsAvailable() will report, so a test can construct
	//                    both the available and unavailable cases without a
	//                    second type.
	FakeProvider(Attestation result, bool available)
		: m_Result(std::move(result)), m_Available(available)
	{
	}

	// A constant answer. The real implementation would ask the vendor SDK, and
	// the engine is expected to consult this before trusting Query -- which is
	// what the unavailable case below demonstrates.
	bool IsAvailable() const override { return m_Available; }
	// Returns the same attestation on every call, rather than consuming it.
	//
	// The real providers are one-shot: a query spends the signal. Returning a
	// copy here means the test can query twice, which is what the IsAvailable/
	// Query sequence below relies on. So this double is more forgiving than the
	// real interface -- worth knowing when reading a test that passes against
	// it.
	//
	// Returns by value, matching a value-semantics safety input.
	Attestation Query() override { return m_Result; }
	// Returns a const reference to the member -- no copy per call, and the
	// caller cannot mutate the provider's identity.
	//
	// The name is the audit trail: it is how a studio answers "how did this
	// player come to be treated as an adult". A provider that returned a fresh
	// string each call would be wasteful on a path that may be consulted for
	// evidence.
	const std::string &ProviderName() const override { return m_Name; }

private:
	Attestation m_Result;
	bool m_Available;
	// Fixed, and in-class initialised rather than passed to the constructor.
	//
	// DEFECT, minor: the name is not a constructor parameter, so a test cannot
	// use FakeProvider to check that a provider's name is plumbed through
	// correctly -- TestMethodIsRecorded tests Attestation's copy, not the
	// provider's. Parameterising it would close that gap and cost nothing.
	std::string m_Name = "fake-vendor";
};

static void TestDefaultIsFailClosed()
{
	// The default-constructed Attestation is what a provider returns before
	// it has asked anything, and what a buggy call site constructs by
	// accident. It must read as a minor.
    // Default-constructed, exactly as a buggy call site would produce by
    // accident.
    //
    // const because nothing here mutates it -- the value is being INSPECTED,
    // not tested for mutability. Note nothing in this test checks that
    // Attestation is immutable, so the const here documents intent rather than
    // enforcing a contract.
    const Attestation fresh;

    // Band first. Unknown rather than a minor band is the fail-closed answer:
    // no signal arrived, and AgeBand.h is explicit that Unknown must never be
    // treated as adult.
    CHECK(fresh.Band() == AgeBand::Unknown, "default band must be Unknown");
    // The default LEVEL is the weakest one. That is deliberate and subtle: a
    // default attestation claims the LEAST assurance, so if anything downstream
    // ignores IsMinor() and consults the level directly, it still gets the
    // restrictive answer. SelfDeclared is also the only level AgeBand.h and
    // JurisdictionPolicy agree can never be sufficient for any surface.
    CHECK(fresh.Level() == AssuranceLevel::SelfDeclared, "default level must be SelfDeclared");
    CHECK(fresh.IsMinor(), "a default-constructed Attestation must report the player as a minor");
    // No method name by default -- there was no method, so claiming one would
    // be a fabricated audit trail.
    //
    // DEFECT: this CHECK calls Method() but the line above already returned a
    // const Attestation&, so there is no lifetime issue. However nothing tests
    // that a NON-default attestation always has a non-empty method --
    // TestMethodIsRecorded checks one happy case, not the invariant.
    CHECK(fresh.Method().empty(), "default attestation must carry no method name");
}

static void TestAbsentIsFailClosed()
{
	// The explicit 'no signal' factory, as opposed to the default constructor.
	//
	// Both paths are tested separately (TestDefaultIsFailClosed and
	// TestAbsentIsFailClosed) because they are DIFFERENT constructions reaching
	// the same answer -- the default constructor's members are initialised in
	// the header, Absent() is a named factory. If either were changed without
	// the other, only one test would fail.
	const Attestation absent = Attestation::Absent();

	CHECK(absent.Band() == AgeBand::Unknown, "Absent band must be Unknown");
	CHECK(absent.Level() == AssuranceLevel::SelfDeclared, "Absent level must be SelfDeclared");
	CHECK(absent.IsMinor(), "Absent must report the player as a minor");
}

static void TestUnknownBandIsMinor()
{
	// An unrecognised band -- a provider build newer than the engine, or
	// corrupt data -- must never read as adult.
	// An Unknown band paired with a COMMERCIAL level -- deliberately mismatched.
	//
	// This is the case that catches a naive implementation. A strong assurance
	// level with no band is exactly what a provider returns when it has a signal
	// it cannot map to a band, and a policy that trusted the LEVEL would treat
	// this as a strong signal.
	const Attestation unknown =
	    Attestation::FromProvider(AssuranceLevel::Commercial, AgeBand::Unknown, "vendor-x");
	CHECK(unknown.IsMinor(), "an Unknown band must report the player as a minor");

	const Attestation garbage =
	    // An unrecognised band VALUE, not just the Unknown enumerator. Both are
	    // needed: an implementation could handle Unknown explicitly in a switch and
	    // still mishandle a value no case matches. The test covers the switch's
	    // default path as well as its Unknown path.
	    Attestation::FromProvider(AssuranceLevel::Commercial, static_cast<AgeBand>(999), "vendor-x");
	CHECK(garbage.IsMinor(), "an unrecognised band value must fail closed to minor");

	const Attestation negative =
	    // The negative counterpart, mirroring test_policy.cpp's negative-surface
	    // check. Guards against a clamp-based implementation that handles large
	    // out-of-range values but not small ones.
	    Attestation::FromProvider(AssuranceLevel::Commercial, static_cast<AgeBand>(-42), "vendor-x");
	CHECK(negative.IsMinor(), "a negative band value must fail closed to minor");
}

static void TestVerifiedBands()
{
	// A band backed by a real assurance method is honoured. This is the only
	// path on which IsMinor() may return false.
	// Every genuine minor band, each verified by a real method.
	//
	// Braced-init-list range-for again, which is why <initializer_list> is NOT
	// included here -- test_policy.cpp needed it explicitly because MSVC does
	// not pull it in transitively. It compiles here, so this file relies on the
	// compiler providing it. Inconsistent between the two test files, harmless
	// today.
	for (AgeBand band : {AgeBand::Under13, AgeBand::Age13To15, AgeBand::Age16To17})
	{
		const Attestation att =
		    Attestation::FromProvider(AssuranceLevel::Commercial, band, "vendor-x");
		CHECK(att.IsMinor(), "a verified minor band must report minor");
	}

	const Attestation adult =
	    Attestation::FromProvider(AssuranceLevel::Commercial, AgeBand::Adult18, "vendor-x");
	// The one assertion in the file that requires IsMinor() to return FALSE.
	//
	// It is here to prove the method is not simply hardcoded to true -- without
	// it, every other test in the file would also pass if IsMinor() always
	// returned true. This is what gives the fail-closed tests their meaning.
	CHECK(!adult.IsMinor(), "a commercially verified adult must NOT report minor");
}

static void TestSelfDeclaredAdultIsStillMinor()
{
	// Art 3(i) of the EU KIDS Act proposal (COM(2026) 681) excludes
	// self-declaration from "age assurance" entirely. A date-of-birth box is
	// not evidence of age, so it must not unlock anything -- not even for a
	// player who typed 18 into it.
	//
	// This is the single most exploitable bug in an age-assurance module:
	// the band says Adult18, the code checks only the band, and every
	// age gate in the game opens for a fourteen-year-old who typed a number.
	const Attestation selfDeclaredAdult =
	    Attestation::FromProvider(AssuranceLevel::SelfDeclared, AgeBand::Adult18, "dob-form");
	// The centrepiece: a band that SAYS Adult18, produced by the weakest
	// assurance level.
	//
	// Both facts are needed together for the trap the comment above describes:
	// code that checks only the band opens every gate. IsMinor() has to consider
	// the level, and this is the assertion that proves it does.
	CHECK(selfDeclaredAdult.IsMinor(),
	      "a self-declared adult must still report as minor: Art 3(i) excludes self-declaration");

	// Same for the weakest other-than-self-declared level. An OS signal is
	// the lowest real assurance method, and it is enough -- but it is still
	// not self-declaration.
	// The boundary case on the other side: an OS signal is the weakest REAL
	// assurance method and is sufficient, so this must NOT be minor.
	//
	// It sits immediately after the self-declared case to pin the exact
	// boundary -- SelfDeclared fails, OsSignal passes, Adult18 is the band in
	// both. A regression that made IsMinor() simply reject Adult18 would break
	// this line; one that accepted SelfDeclared would break the one above.
	const Attestation osSignalAdult =
	    Attestation::FromProvider(AssuranceLevel::OsSignal, AgeBand::Adult18, "apple");
	CHECK(!osSignalAdult.IsMinor(), "an OS-signal adult must not report as minor");
}

static void TestMethodIsRecorded()
{
	// The method name is the audit trail: it is how a studio answers "how
	// did this player come to be treated as an adult". Losing it makes the
	// attestation unusable as compliance evidence.
	// The strongest level, so this also exercises the EuEaa enum member -- the
	// one JurisdictionPolicy requires for Art 6 surfaces.
	const Attestation att = Attestation::FromProvider(AssuranceLevel::EuEaa, AgeBand::Adult18, "eu-aaa");
	CHECK(att.Method() == "eu-aaa", "the assurance method must be recorded on the attestation");
	CHECK(att.Level() == AssuranceLevel::EuEaa, "the assurance level must be recorded");
}

static void TestAttestationCarriesNoIdentity()
{
	// Structural guarantee, not a naming convention. An Attestation must not
	// be joinable to a person: the module's entire privacy posture rests on
	// age being an isolated fact. A size assertion is a cheap tripwire --
	// it will fail loudly if someone bolts on a std::string handle, an
	// account GUID, or a token, any of which would reintroduce exactly the
	// identity join the design forbids.
	//
	// An empty std::string on MSVC is 32 bytes, so the floor here is
	// comfortable rather than tight; the point is to catch an added
	// identifier, not to pin the layout.
	// A structural guarantee expressed as a size assertion -- the cheapest
	// possible tripwire against someone adding an identity field.
	//
	// static_assert rather than CHECK because it must hold at compile time: a
	// runtime check could be skipped, and the whole point is that the layout is
	// wrong, not that some call misbehaves.
	//
	// The 128-byte budget is loose, as the comment above says -- an empty
	// std::string is 32 bytes on MSVC -- so this catches a bolted-on GUID or
	// token, not a field or two. A std::string member added for convenience
	// would slip through if the type were already near the limit.
	static_assert(sizeof(Attestation) <= 128,
	              "Attestation has grown: it must not carry identity, only an isolated age fact");
	// DEFECT: the same stray-increment pattern flagged in test_policy.cpp's
	// TestMonotonicity. A static_assert fires no CHECK, so the count is bumped
	// by hand to stop the function contributing zero checks to the total.
	//
	// It works, and it inflates the reported total by one per static_assert so
	// the number does not correspond to runtime assertions. A local CHECK(true,
	// ...) or a separate printed count for compile-time checks would be cleaner.
	++g_Checks;
}

static void TestProviderInterfaceIsUsable()
{
	// The interface has to be implementable by a studio, and usable through
	// a base pointer -- otherwise the engine cannot consume a provider at all.
	// A mutable local so it can be moved into the provider below --
	// FakeProvider takes by value and the move into m_Result makes that cheap.
	Attestation result = Attestation::FromProvider(AssuranceLevel::OsSignal, AgeBand::Under13, "google");
	// The provider, constructed available with an under-13 OS signal.
	FakeProvider provider(std::move(result), true);

	// Upcast to the base, and the reason for the test's existence per the file
	// header: a studio hands the engine a provider by this type, so every call
	// below goes through the base pointer rather than the concrete class.
	//
	// This is what makes the checks polymorphic-dispatch checks rather than
	// direct calls.
	IAgeAssuranceProvider *asInterface = &provider;
	CHECK(asInterface->IsAvailable(), "an available provider must report available");
	// DEFECT: a double negative that reads backwards. `!IsMinor() == false`
	// means IsMinor() is true, which is the intended assertion -- but written as
	// IsMinor() it says the same thing and cannot be misread.
	//
	// Worse, the failure message contradicts the expression: if this fails, the
	// message printed says the signal "must report minor", which is what the
	// test wants, while the condition tests `!IsMinor() == false`. A future
	// editor flipping one `!` would change the condition but not the message.
	// This is the only CHECK in the file with this problem.
	CHECK(!asInterface->Query().IsMinor() == false, "an under-13 OS signal must report minor");
	CHECK(asInterface->ProviderName() == "fake-vendor", "the provider name must be readable through the interface");

	// The second provider: unavailable, returning Absent().
	//
	// Constructed with Absent() rather than a real band to model the real
	// sequence -- a provider that cannot answer returns the absent attestation
	// rather than a stale or default one.
	FakeProvider unavailable(Attestation::Absent(), false);
	CHECK(!unavailable.IsAvailable(), "an unavailable provider must report unavailable");
	CHECK(unavailable.Query().IsMinor(),
	      "an unavailable provider must still return a fail-closed attestation");

	// Destroying through the base pointer must be well-defined. A virtual
	// destructor is not optional on a polymorphic base.
	// The interface must be safely destructible through a base pointer, since
	// the engine owns providers polymorphically.
	//
	// A trait-based static_assert rather than a delete-through-base test: it
	// fires at compile time and names the requirement in the message, where a
	// runtime delete would only crash if a test happened to exercise that path.
	static_assert(std::has_virtual_destructor<IAgeAssuranceProvider>::value,
	              "IAgeAssuranceProvider must have a virtual destructor: studios delete providers through this type");
	++g_Checks;
}

// No argv -- nothing to configure. Every case is a fixed property of the
// type.
int main()
{
	// Call order is meaningful: the fail-closed default first, then the more
	// specific cases. A break in the default would be the most consequential
	// and appears first in the log.
	TestDefaultIsFailClosed();
	TestAbsentIsFailClosed();
	TestUnknownBandIsMinor();
	TestVerifiedBands();
	TestSelfDeclaredAdultIsStillMinor();
	TestMethodIsRecorded();
	TestAttestationCarriesNoIdentity();
	// The interface test last, since it is about the shape of the API rather
	// than the semantics of one value.
	TestProviderInterfaceIsUsable();

	// Both counters, so a vacuous run -- zero checks, zero failures -- is visible
	// as suspicious rather than as success.
	std::printf("test_attestation: %d checks, %d failures\n", g_Checks, g_Failures);
	// Non-zero exit on any failure, which is all CI checks.
	return g_Failures ? 1 : 0;
}