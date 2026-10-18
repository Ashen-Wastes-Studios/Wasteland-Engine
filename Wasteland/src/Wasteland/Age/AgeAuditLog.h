// Wasteland::Age -- the record of what the age-assurance arrangements did.
//
// Art 32 of the EU KIDS Act proposal (COM(2026) 681, 2026/0286(COD)) requires
// providers to keep records of their age-assurance arrangements. In practice
// that means being able to show that every age-gated decision came from the
// same rules and none of them were quietly different.
//
// WHAT THIS DELIBERATELY CANNOT DO
//
// It cannot answer "what did we do to THIS player". It does not know who any
// player is. An AgeAuditEntry holds a band, a surface, two assurance levels, a
// decision, a reason and a timestamp -- nothing joinable to a person.
//
// A studio needing per-user audit trails must add them at their own layer with
// their own justification and retention rules. That is not a gap worked around
// here; a compliance module that could name the children it processed would
// become the Art 28(1) identity problem it exists to avoid.
//
// THE INVARIANT
//
// Unrecorded means not granted. AuthorizeFeature() permits a feature only when
// the policy finds the assurance sufficient AND the decision was recorded. A
// game that cannot evidence the check it made does not get to act on it.
//
// So a capacity-zero log permits nothing at all. A studio who disables logging
// does not silently get an unaudited game; they get a game that gates shut.
//
// WHY A BOUNDED IN-MEMORY RING
//
// An unbounded log is an unbounded memory allocation driven by player
// behaviour -- a denial of service wearing a compliance hat. So the log is a
// fixed-capacity ring that overwrites its oldest entry and sets Wrapped().
// Authorisation deliberately CONTINUES after a wrap: the new decision was
// recorded, and refusing to authorise would mean a game locks shut permanently
// after a few thousand gated actions. A studio that needs durable records
// drains Entries(); one that does not at least gets Wrapped() telling them the
// trail is no longer complete.
//
// Neither instrument is in force as of this writing. See
// docs/AGE_ASSURANCE.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "Wasteland/Age/AgeService.h"

namespace Wasteland::Age
{

// What the gate concluded.
enum class AuditDecision : int
{
	Granted = 0,
	Denied = 1,
};

// Why a decision went the way it did. An enum rather than free text: free text
// in a compliance record eventually gets filled in by a human at 2am and is
// then worth nothing as evidence.
enum class DenyReason : int
{
	None = 0,

	// The policy found the provided assurance weaker than the surface requires.
	AssuranceInsufficient = 1,

	// The provided assurance was self-declared, which Art 3(i) excludes from
	// age assurance entirely.
	SelfDeclared = 2,

	// The decision could not be written to the log. Distinct from a policy
	// denial so an auditor can tell "we said no" from "we could not record".
	NotRecorded = 3,
};

// One recorded decision.
//
// TRIVIALLY COPYABLE, AND THAT IS THE POINT.
//
// std::string is not trivially copyable, so this type having no such member
// means it has nowhere to put a name, an account handle, a session token or a
// date of birth -- and adding one later breaks the static_assert at the bottom
// of this header. The identity guarantee is enforced by the compiler, not by
// review.
//
// Standard layout additionally fixes the representation, so a fixed-size array
// of these is exactly that size.
struct AgeAuditEntry
{
	// Supplied by the caller, not read from a clock here: the module makes no
	// assumption about monotonic versus wall time, and a test can pin it.
	std::uint64_t TimestampSeconds = 0;

	AuditDecision Decision = AuditDecision::Denied;
	SurfaceKind Surface = SurfaceKind::OnlineGame;
	AssuranceLevel RequiredAssurance = AssuranceLevel::SelfDeclared;
	AssuranceLevel ProvidedAssurance = AssuranceLevel::SelfDeclared;
	AgeBand Band = AgeBand::Unknown;
	DenyReason Reason = DenyReason::None;
};

// Enforced at compile time for every translation unit that includes this
// header. If a future change adds a std::string or a virtual function, the
// build stops here rather than at a privacy review.
static_assert(std::is_trivially_copyable<AgeAuditEntry>::value,
              "AgeAuditEntry must remain trivially copyable so it cannot acquire a "
              "heap-allocated identity field");
static_assert(std::is_standard_layout<AgeAuditEntry>::value,
              "AgeAuditEntry must remain standard layout for a stable representation");

// A fixed-capacity ring of recorded decisions.
class AgeAuditLog
{
public:
	// Deliberately explicit rather than a defaulted constructor: capacity is a
	// compliance decision, not an incidental detail, and a caller should have
	// to look at it.
	explicit AgeAuditLog(std::size_t capacity);

	AgeAuditLog(const AgeAuditLog &) = delete;
	AgeAuditLog &operator=(const AgeAuditLog &) = delete;

	// The process-wide log, 4096 entries (about 256 KiB). Studios wanting
	// durable records should own their own AgeAuditLog and drain it.
	static AgeAuditLog &Instance();

	// Record a decision verbatim.
	//
	// Returns false only when there is no capacity at all (see the capacity-zero
	// note in the .cpp). Overwriting an old entry is a success, not a failure,
	// and is reported separately by Wrapped().
	bool Record(const AgeAuditEntry &entry);

	// The gate studios call before opening an age-relevant feature:
	//
	//     if (Age::AgeAuditLog::Instance().AuthorizeFeature(
	//             Age::AgeService::Instance(), Age::SurfaceKind::OnlineGame))
	//         OpenFeature();
	//
	// Returns true only when the policy finds `provided` sufficient for
	// `surface` AND the decision was recorded. Both, never either.
	bool AuthorizeFeature(const AgeService &service, SurfaceKind surface,
	                      AssuranceLevel provided = AssuranceLevel::SelfDeclared);

	std::size_t Size() const { return m_Size; }
	std::size_t Capacity() const { return m_Capacity; }

	// True once an entry has been overwritten. Poll it; it never clears except
	// via Clear().
	bool Wrapped() const { return m_Wrapped; }

	// A copy, oldest first.
	std::vector<AgeAuditEntry> Entries() const;

	// Drop everything, including the wrapped flag. Studios call this after
	// durably persisting Entries().
	void Clear();

	// The timestamp to record with. Monotonic seconds from process start --
	// deliberately NOT wall-clock time, so an entry cannot be correlated with
	// any external record of when the player was present.
	static std::uint64_t MonotonicSeconds();

private:
	std::vector<AgeAuditEntry> m_Entries;
	std::size_t m_Capacity = 0;
	std::size_t m_Size = 0;
	std::size_t m_Next = 0;
	bool m_Wrapped = false;
};

} // namespace Wasteland::Age