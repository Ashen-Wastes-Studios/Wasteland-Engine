// Standalone functional test for Wasteland::Age::AgeAuditLog.
//
// WHAT AN AUDIT LOG IS FOR HERE
//
// Art 32 of the EU KIDS Act proposal (COM(2026) 681, 2026/0286(COD)) requires
// providers to keep records of what their age-assurance arrangements actually
// did. That means the engine has to be able to show that every age-gated
// decision it made came from the same rules, and that none of them were
// quietly different.
//
// WHAT IT DELIBERATELY CANNOT DO
//
// It cannot answer "what did we do to THIS player". It does not know who any
// player is. AgeAuditEntry holds a band, a surface, a pair of assurance
// levels, a decision and a timestamp -- and nothing that could be joined back
// to a person. A studio that needs per-user audit trails has to add them at
// their own layer, with their own justification and their own retention rules.
//
// That is not a limitation worked around; it is the point. A compliance module
// that could name the children it processed would itself become the
// Art 28(1) identity problem.
//
// THE INVARIANT THIS SUITE PROVES
//
// Unrecorded means not granted. AuthorizeFeature() permits a feature only when
// the policy says the assurance is sufficient AND the decision was recorded.
// A game that cannot evidence the check it made does not get to act on it.
//
// Which also means a capacity-zero log permits nothing at all -- see
// TestZeroCapacityDeniesEverything. A studio who disables logging does not
// silently get an unaudited game; they get a game that gates shut.

// std::printf for the CHECK macro's output. type_traits is for the
// compile-time guarantees in the first test -- the identity argument is
// made by the compiler here, not by reading the header.
#include <cstdio>
#include <type_traits>

// AgeService comes in through this header too: the log needs it to answer
// Art 29(2) scope questions, so the log is not testable without it.
#include "Wasteland/Age/AgeAuditLog.h"

// Flattened so AgeAuditLog, AgeService, SurfaceKind and the rest read
// without qualification in a file where every line is about them.
using namespace Wasteland::Age;

// File-scope counters, no framework: the point of these suites is that
// they build with nothing but the engine headers.
static int g_Failures = 0;
static int g_Checks = 0;

// Incrementing the check count before evaluating means a CHECK whose
// condition has side effects would still be counted -- none here do.
// do/while(0) so a CHECK inside an if cannot capture the else.
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

// The first test is not a behaviour test at all: it is a proof obligation
// enforced at compile time. If someone adds a std::string member to
// AgeAuditEntry, this file stops compiling -- which is the intended
// enforcement, not a broken build.
static void TestEntryCannotHoldAnIdentity()
{
	// The structural guarantee, checked at compile time rather than by
	// reading the header and agreeing with myself.
	//
	// Trivially copyable means no user-defined copy constructor, no
	// destructor, no virtual functions -- and, crucially, no std::string,
	// because std::string is not trivially copyable. Since AgeAuditEntry has
	// no such members, it has nowhere to put a name, an account handle, a
	// session token, or a date of birth. You cannot add one later without
	// breaking this static_assert.
	//
	// Standard layout additionally guarantees no base classes and no vtable,
	// so the representation is fixed and safe to serialise.
	// Trivially copyable is doing real work here, not decoration. It excludes
	// std::string, which is the field an identity would arrive in.
	static_assert(std::is_trivially_copyable<AgeAuditEntry>::value,
	              "AgeAuditEntry must stay trivially copyable so it cannot gain a "
	              "heap-allocated identity field");
	// Standard layout additionally rules out base classes and a vtable, so
	// the byte representation is fixed and safe to serialise as-is.
	static_assert(std::is_standard_layout<AgeAuditEntry>::value,
	              "AgeAuditEntry must stay standard layout for a stable representation");

	// And the size is fixed, so a fixed-size buffer of them is bounded. A log
	// whose entries could grow would be a log whose memory use is unbounded.
	// A size bound rather than an exact size, so adding a small field does
	// not fail the build. 64 bytes is generous for the seven scalar fields
	// the entry carries.
	CHECK(sizeof(AgeAuditEntry) <= 64, "an audit entry must stay small and fixed-size");
}

// Round-trip fidelity. An audit log that quietly dropped the REASON or
// mangled the assurance pair would pass every policy test and still be
// useless to an auditor, so every field is checked individually.
static void TestRecordsDecisionsVerbatim()
{
	// Capacity 8: roomy, so the wrap path is not involved here.
	AgeAuditLog log(8);

	// Default-constructed then field-assigned rather than aggregate-initialised
	// with a brace list, so this stays valid if a field is added -- and so a
	// newly added field is visibly left at its default rather than zeroed
	// by accident.
	AgeAuditEntry entry;
	entry.TimestampSeconds = 1234567;
	// Denied plus a reason and a required/provided assurance gap: the exact
	// shape of an interesting entry. A grant would exercise fewer fields.
	entry.Decision = AuditDecision::Denied;
	entry.Surface = SurfaceKind::SocialNetwork;
	entry.RequiredAssurance = AssuranceLevel::EuEaa;
	entry.ProvidedAssurance = AssuranceLevel::OsSignal;
	entry.Band = AgeBand::Age13To15;
	entry.Reason = DenyReason::AssuranceInsufficient;

	CHECK(log.Record(entry), "recording into a log with room must succeed");
	CHECK(log.Size() == 1, "and must add exactly one entry");

	// Entries() returns by value, so this is a snapshot. That is what makes
	// the pointer-free index access below safe.
	const std::vector<AgeAuditEntry> entries = log.Entries();
	CHECK(entries.size() == 1, "the log must return what was written");
	// DEFECT (test weakness, documented not fixed): the field assertions are
	// skipped if Entries() is empty. Size was asserted == 1 one line above,
	// so reaching here empty would already be a failure -- the guard is
	// belt-and-braces against an out-of-range access, not a second check.
	if (!entries.empty())
	{
		CHECK(entries[0].TimestampSeconds == 1234567, "the timestamp must round-trip");
		CHECK(entries[0].Decision == AuditDecision::Denied, "the decision must round-trip");
		CHECK(entries[0].Surface == SurfaceKind::SocialNetwork, "the surface must round-trip");
		CHECK(entries[0].RequiredAssurance == AssuranceLevel::EuEaa, "the required level must round-trip");
		CHECK(entries[0].ProvidedAssurance == AssuranceLevel::OsSignal, "the provided level must round-trip");
		CHECK(entries[0].Band == AgeBand::Age13To15, "the band must round-trip");
		CHECK(entries[0].Reason == DenyReason::AssuranceInsufficient, "the reason must round-trip");
	}
}

// "Unrecorded means not granted" at its sharpest: a log that cannot record
// has no evidence, so it may not permit.
static void TestCapacityZeroDeniesEverything()
{
	// The invariant, at its sharpest. A log that cannot record must not permit.
	// Zero, not "empty but growable": the capacity is fixed at construction,
	// so this is a log that will never be able to record anything.
	AgeAuditLog log(0);
	AgeService service;

	// The parameters are the most permissive ones the suite knows of -- a game
	// surface with commercial assurance is granted everywhere else -- so the
	// denial can only be coming from the zero capacity.
	CHECK(!log.AuthorizeFeature(service, SurfaceKind::OnlineGame, AssuranceLevel::Commercial),
	      "a capacity-zero log must not authorise anything");
	CHECK(log.Size() == 0, "and must not have recorded anything");
	// Wrapped() must stay FALSE: nothing was ever written, so nothing could
	// have been overwritten. Reporting a wrap here would be a lie that tells
	// the studio their (empty) trail had lost entries.
	CHECK(!log.Wrapped(), "and must not pretend it wrapped -- it simply refused");
}

// The positive statement of the invariant, using a capacity-1 log as the
// cleanest demonstration that a FULL log is not a failure state.
static void TestUnrecordedMeansNotGranted()
{
	// The invariant, stated positively: the ONLY way Record() fails is a
	// zero-capacity log, because overwriting the oldest entry is a successful
	// write of the newest.
	//
	// So "unrecorded means not granted" has exactly one reachable case, and it
	// is the sharp one -- a log with no capacity authorises nothing. Which is
	// why AuthorizeFeature checks Record()'s result rather than assuming it.
	//
	// An earlier draft of this suite asserted that a FULL log refuses, and was
	// wrong: a full log wraps, the new decision was recorded, and refusing would
	// mean a game locks shut after a few thousand gated actions with no way back.
	// A capacity-1 log is the clearest demonstration of that.
	// One slot. Every authorised decision overwrites the previous one, which
	// is what makes the "newest is recorded" assertion below meaningful.
	AgeAuditLog oneSlot(1);
	AgeService service;

	// AuthoriseFeature both decides AND records -- the returned bool is the
	// permission, and the record is a side effect of the same call. That
	// coupling is the invariant: the two cannot drift apart.
	// Identical call to the one above. The test passes only because the second
	// write counts as a successful write even though it destroyed the first.
	CHECK(oneSlot.AuthorizeFeature(service, SurfaceKind::OnlineGame, AssuranceLevel::Commercial),
	      "a game surface with commercial assurance must be permitted");
	CHECK(oneSlot.Size() == 1, "and recorded");

	CHECK(oneSlot.AuthorizeFeature(service, SurfaceKind::OnlineGame, AssuranceLevel::Commercial),
	      "a second decision must still be permitted -- the newest one was recorded");
	CHECK(oneSlot.Wrapped(), "and the studio must be told the trail is no longer complete");
	CHECK(oneSlot.Size() == 1, "while staying bounded");
}

// A refusal is a decision worth recording. Logging only grants would leave
// an auditor unable to see that the check ran and said no.
static void TestInsufficientAssuranceIsDeniedAndRecorded()
{
	// A refusal is still a decision worth recording: "we checked and said no"
	// is exactly what an auditor wants to see.
	AgeAuditLog log(8);
	// Default-constructed means EU scope ON, which is the case the article
	// governs. The contrast with TestSelfDeclaredIsAlwaysDenied, which turns
	// it off, is what proves the flag is load-bearing.
	AgeService service; // EU scope, so Art 29(2) applies to social networking

	// OS signal on a social surface in EU scope: an age-based signal is not
	// sufficient for the social-networking surface under this proposal.
	CHECK(!log.AuthorizeFeature(service, SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal),
	      "Art 29(2) must refuse an OS signal on a social surface in EU scope");
	CHECK(log.Size() == 1, "but the refusal must be recorded");

	const std::vector<AgeAuditEntry> entries = log.Entries();
	if (!entries.empty())
	{
		CHECK(entries[0].Decision == AuditDecision::Denied, "recorded as denied");
		CHECK(entries[0].Reason == DenyReason::AssuranceInsufficient, "with the right reason");
	}
}

// Art 3(i): self-declaration is not age assurance. The claim under test is
// that no scope and no surface rescues it.
static void TestSelfDeclaredIsAlwaysDenied()
{
	// Art 3(i): self-declaration is not age assurance. No surface, no scope,
	// no band may be unlocked by it -- but it is still worth recording that it
	// was offered and refused.
	// Recorded even though it is refused -- the attempt itself is the audit
	// event a reviewer wants to see.
	AgeAuditLog log(8);
	AgeService service;
	// Scope deliberately switched OFF: this proves the denial comes from the
	// assurance LEVEL, not from a jurisdictional gate. Without this line the
	// test would be indistinguishable from the Art 29(2) test above.
	service.SetEuScope(false); // even outside EU scope

	CHECK(!log.AuthorizeFeature(service, SurfaceKind::OnlineGame, AssuranceLevel::SelfDeclared),
	      "self-declaration must never authorise, even in US scope");
	CHECK(log.Size() == 1, "and the attempt must be recorded");
}

// The mirror of the denial test: a grant must also land in the log, and
// must carry no reason.
static void TestGrantsAreRecorded()
{
	AgeAuditLog log(8);
	AgeService service;

	// Game surface plus commercial assurance -- the baseline permission the
	// other tests measure their refusals against.
	CHECK(log.AuthorizeFeature(service, SurfaceKind::OnlineGame, AssuranceLevel::Commercial),
	      "a game surface with commercial assurance must be permitted");
	CHECK(log.Size() == 1, "and recorded");

	const std::vector<AgeAuditEntry> entries = log.Entries();
	if (!entries.empty())
	{
		// DenyReason::None on a grant is asserted too: leaving a stale reason on a
		// grant would make an auditor read a refusal where none happened.
		CHECK(entries[0].Decision == AuditDecision::Granted, "recorded as granted");
		CHECK(entries[0].Reason == DenyReason::None, "with no denial reason");
	}
}

// The other half of the Art 29(2) pair. Without it, the previous test
// would pass just as well if AuthorizeFeature refused every social
// surface regardless of scope.
static void TestUsScopeGrantsSocialSurface()
{
	// The other half of the Art 29(2) test: narrowing scope must actually
	// permit something, or the audit log records a permanent refusal and the
	// switch is decorative.
	AgeAuditLog log(8);
	AgeService service;
	service.SetEuScope(false);

	// The exact arguments denied two tests ago, with scope flipped. The pair
	// together is what pins scope as the deciding factor.
	CHECK(log.AuthorizeFeature(service, SurfaceKind::SocialNetwork, AssuranceLevel::OsSignal),
	      "US scope must permit an OS signal on a social surface");
	CHECK(log.Size() == 1, "and record the grant");
}

// Ring-buffer semantics: bounded memory, oldest-first retrieval, and an
// honest report when evidence has been destroyed.
static void TestRingWrapsAndSaysSo()
{
	// A bounded in-memory log has to do one of two things when it fills:
	// refuse, or overwrite. Overwriting silently would mean a studio believes
	// they have an audit trail and do not. So it wraps AND reports that it did,
	// and AuthorizeFeature surfaces the wrap so the studio can react.
	// Capacity 3: small enough that the fourth write is visible in a diff of
	// the assertions below.
	AgeAuditLog log(3);

	// Filled to exactly capacity, with distinguishable timestamps 0/1/2 so the
	// ordering assertion later has something to compare.
	for (int i = 0; i < 3; ++i)
	{
		AgeAuditEntry entry;
		// Explicit cast from int: the field is unsigned, and without it a narrowing
		// warning would fire here and in any future caller.
		entry.TimestampSeconds = static_cast<std::uint64_t>(i);
		// Return value deliberately discarded. These are direct Record calls, not
		// AuthorizeFeature, so a failure would not gate anything -- which is why
		// the assertions that follow check size and wrap state instead.
		log.Record(entry);
	}

	CHECK(!log.Wrapped(), "a log filled to capacity exactly has not wrapped");
	CHECK(log.Size() == 3, "and holds three entries");

	// The overflow write. Timestamp 99 is unmistakable in the ordering checks
	// that follow, where 0 (the dropped entry) would be equally identifiable.
	AgeAuditEntry fourth;
	fourth.TimestampSeconds = 99;
	log.Record(fourth);

	CHECK(log.Wrapped(), "writing past capacity must be reported");
	CHECK(log.Size() == 3, "and must still be bounded at capacity");

	// Ordering is checked BEFORE the AuthorizeFeature call below, because that
	// call records an entry of its own and would wrap the ring again -- moving
	// the front of the buffer and turning this test into a test of nothing.
	// MUST be read before any AuthorizeFeature call: that call records an
	// entry of its own and would wrap the ring a second time, evicting
	// timestamp 1 and making the ordering assertions meaningless.
	const std::vector<AgeAuditEntry> ordered = log.Entries();
	CHECK(ordered.size() == 3, "still bounded");
	// Size-checked, then indexed: without this the three subscripted reads
	// below would be out of bounds if Entries() ever returned a short
	// vector. Unlike the guard in TestRecordsDecisionsVerbatim, this one
	// can actually be reached with the wrong size, so it is load-bearing.
	if (ordered.size() == 3)
	{
		CHECK(ordered.front().TimestampSeconds == 1, "the oldest entry must be the one dropped");
		CHECK(ordered.back().TimestampSeconds == 99, "and the newest must be kept");
		// Most importantly: oldest first. Returning the raw buffer in slot order
		// would look chronological to a reader and be exactly backwards.
		// Strictly increasing across all three, not just front/back: this is what
		// distinguishes chronological order from ring-slot order, which would
		// look plausible and be exactly backwards.
		CHECK(ordered[0].TimestampSeconds < ordered[1].TimestampSeconds &&
		          ordered[1].TimestampSeconds < ordered[2].TimestampSeconds,
		      "entries must come back oldest first, not in ring-slot order");
	}

	// Authorisation CONTINUES after a wrap. Overwriting the oldest entry is a
	// retention event, not a write failure -- the new decision was recorded.
	//
	// The alternative, refusing to authorise once the log has wrapped, would
	// mean a game locks shut permanently after a few thousand gated actions and
	// never recovers. A studio that needs durable records drains the log; a
	// studio that does not at least gets Wrapped() telling them the trail is
	// no longer complete.
	AgeService service;
	// Last assertion in the file, and the one that states the retention
	// position: a wrap is a data-retention event, not a write failure, so
	// permission continues. Refusing here would lock the game shut forever
	// after a few thousand gated actions.
	CHECK(log.AuthorizeFeature(service, SurfaceKind::OnlineGame, AssuranceLevel::Commercial),
	      "a wrapped log must still authorise and record, or the game locks shut forever");
}

// Flat sequence, no runner. The order is dependency-free; each test
// constructs its own log and service.
int main()
{
	TestEntryCannotHoldAnIdentity();
	TestRecordsDecisionsVerbatim();
	TestCapacityZeroDeniesEverything();
	TestUnrecordedMeansNotGranted();
	TestInsufficientAssuranceIsDeniedAndRecorded();
	TestSelfDeclaredIsAlwaysDenied();
	TestGrantsAreRecorded();
	TestUsScopeGrantsSocialSurface();
	TestRingWrapsAndSaysSo();

	// Totals plus a non-zero exit code is the entire interface to CI.
	std::printf("test_age_audit_log: %d checks, %d failures\n", g_Checks, g_Failures);
	// Any failure fails the binary; there is no partial-credit mode.
	return g_Failures ? 1 : 0;
}