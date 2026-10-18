#include "wlpch.h"

#include "Wasteland/Age/AgeAuditLog.h"

// <chrono> for steady_clock. std::vector and std::size_t arrive through
// AgeAuditLog.h, which needs them for the member and for Entries().
#include <chrono>

namespace Wasteland::Age
{

// Anonymous namespace for kDefaultCapacity alone. Internal linkage, so the
// constant cannot collide and needs no header entry -- which also means it is
// invisible to a test that might want to assert against it.
namespace
{
// 4096 entries at 64 bytes is roughly 256 KiB: enough that a normal session's
// gated actions never wrap, small enough that a studio can see the number and
// decide whether it is theirs to change.
// The comment above is the design: 256 KiB is chosen to be VISIBLE. A studio can
// read it, decide it is theirs to change, and construct their own AgeAuditLog; a
// capacity derived from an internal budget would not be reviewable.
constexpr std::size_t kDefaultCapacity = 4096;
} // namespace

// The ring is sized in the body, not the init list: m_Entries is
// default-constructed empty and then resized below.
AgeAuditLog::AgeAuditLog(const std::size_t capacity) : m_Capacity(capacity)
{
	// Pre-sizing the whole vector means Record() never allocates, so it cannot
	// throw partway through a decision. The alternative -- a push_back that
	// grows -- would let an allocation failure leave a decision half-recorded,
	// which is the one state this type exists to make impossible.
	// resize() rather than repeated push_back: every slot is value-initialised once,
	// so Record() below never allocates and cannot throw partway through a decision --
	// a growing push_back could leave a decision half-recorded, the one state this
	// type exists to make impossible.
	//
	// Paid up front: ~256 KiB even for a game that makes two gated decisions. For an
	// engine singleton, allocated once at startup, that is the right way round.
	m_Entries.resize(capacity);
}

// The process-wide log. A function-local static, so it is constructed on first
// use rather than during static initialisation -- the same reason Instrumentor::Get
// is written this way. Construction order between translation units is therefore
// not a concern for anything that touches the log at global scope.
AgeAuditLog &AgeAuditLog::Instance()
{
	// A static local rather than a namespace-scope object, for the same reason: no
	// static-initialisation-order dependency, and thread-safe initialisation since
	// C++11.
	static AgeAuditLog log(kDefaultCapacity);
	return log;
}

// A free function rather than a member: it touches no state, so making it static
// would imply it did. Public and static because tests pin it (the header notes a
// test can supply its own timestamp).
std::uint64_t AgeAuditLog::MonotonicSeconds()
{
	// steady_clock, not system_clock. A wall-clock timestamp could be
	// correlated against an external record of when the player was online, which
	// is a join this module has spent its whole life refusing to make. The
	// ordering is what an audit needs; the absolute date is not, and the studio
	// that wants one can add it at its own layer with its own justification.
	// A using-directive scoped to the function body rather than at file scope, so it
	// cannot shadow a name in the rest of this translation unit.
	using namespace std::chrono;
	return static_cast<std::uint64_t>(
	    // seconds, not milliseconds or nanoseconds: the header calls ordering the point,
	    // and seconds is the coarsest unit that still orders entries from a long session.
	    //
	    // DEFECT (precision): two decisions within the same second carry identical
	    // timestamps, so Entries() cannot be used to establish the ORDER of two grants
	    // made in the same second -- only of those across seconds. For an audit whose
	    // stated purpose is showing decisions came from consistent rules, that ordering
	    // evidence is weak. Not fixed here; comments only.
	    duration_cast<seconds>(steady_clock::now().time_since_epoch()).count());
}

// The raw recorder. AuthorizeFeature below is the gate studios actually call;
// this exists so a decision recorded elsewhere can still enter the same trail.
// It returns false only for the zero-capacity case, which the comment at that
// branch explains.
bool AgeAuditLog::Record(const AgeAuditEntry &entry)
{
	// A zero-capacity log is not a full log that wraps -- it is a log that
	// cannot record, and the distinction matters to AuthorizeFeature: a wrap
	// still wrote the new entry, a refusal wrote nothing.
	// A capacity-zero log is not a full log that wraps -- it is a log that cannot
	// record. The distinction matters to AuthorizeFeature: a wrap still WROTE the new
	// entry, a refusal wrote nothing, and only the second one withholds permission.
	if (m_Capacity == 0)
		return false;

	// The wrap is treated as a retention event, not a write failure, so Record returns
	// true. The studio is told separately via Wrapped().
	// Overwriting is a retention event, not a write failure. The new entry IS
	// recorded, so this returns true -- but the studio is told the trail is no
	// longer complete via Wrapped().
	//
	// Returning false here instead would make AuthorizeFeature refuse, and a
	// game would lock shut permanently after a few thousand gated actions with
	// no way back. That is a worse failure than an incomplete trail that is
	// honestly labelled.
	// Full: the ring is about to overwrite its oldest entry. Setting the flag here
	// rather than after the write means Wrapped() is accurate the instant Record
	// returns.
	if (m_Size == m_Capacity)
		m_Wrapped = true;
	// Not yet full, so the count advances. Note m_Size is the HIGH-WATER count of
	// live entries, not a write counter: it stops growing once the ring is full,
	// which is what makes Entries()'s reserve below correct.
	else
		++m_Size;

	// Overwrite unconditionally at m_Next, then advance modulo the capacity. The
	// modulus is what makes it a ring; with capacity 0 this line would divide by zero,
	// which is exactly why the early return above exists.
	m_Entries[m_Next] = entry;
	m_Next = (m_Next + 1) % m_Capacity;
	return true;
}

// THE GATE. Every other function in this file is bookkeeping; this one is the
// policy decision studios call, and its contract is stated in the header: it
// returns true only when the policy finds the assurance sufficient AND the
// decision was recorded. Both, never either.
//
// `provided` defaults to SelfDeclared, so a call site that forgets the argument
// denies rather than permits -- the failure direction is safe.
bool AgeAuditLog::AuthorizeFeature(const AgeService &service, const SurfaceKind surface,
                                   const AssuranceLevel provided)
{
	// A local aggregate, default-constructed to the fail-closed values the header
	// specifies (Decision = Denied, both levels = SelfDeclared, Band = Unknown,
	// Reason = None). Every field is then overwritten, so the defaults only matter for
	// fields a future edit forgets.
	AgeAuditEntry entry;
	entry.TimestampSeconds = MonotonicSeconds();
	entry.Surface = surface;
	entry.ProvidedAssurance = provided;
	entry.Band = service.Band();
	// Both required and provided levels go into the entry -- which is why
	// MinimumLevelFor and IsSufficient are separate functions rather than one. An
	// auditor can see what was demanded and what was offered.
	//
	// The jurisdiction ternary appears twice in this function, here and below.
	// Duplicated because the two uses sit on opposite sides of the self-declaration
	// branch; hoisting would widen the scope of a value most of the function does not
	// need. A named local at the top would read better. Noted, not changed.
	entry.RequiredAssurance = MinimumLevelFor(surface, service.EuScope() ? Jurisdiction::Eu : Jurisdiction::UnitedStates);

	// Art 3(i) is checked before the policy, and separately from it.
	//
	// Self-declaration is excluded from the definition of age assurance, so it
	// is not "a weaker kind of proof" -- it is not proof. Folding it into the
	// policy would let a future policy revision quietly weaken the exclusion by
	// lowering a number, which is exactly the sort of change that should have
	// to be deliberate.
	// Art 3(i) is checked BEFORE the policy and SEPARATELY from it.
	// Self-declaration is not a weaker kind of proof -- it is excluded from the
	// definition of age assurance, so it cannot satisfy any floor.
	//
	// Folding this into IsSufficient would let a future policy revision quietly weaken
	// the exclusion by lowering a number. Separate, the change must be deliberate.
	if (provided == AssuranceLevel::SelfDeclared)
	{
		entry.Decision = AuditDecision::Denied;
		entry.Reason = DenyReason::SelfDeclared;
		Record(entry);
		return false;
	}

	// IsSufficient also rejects SelfDeclared explicitly (JurisdictionPolicy.cpp),
	// so this check is redundant by design -- but it keeps an audit trail for the
	// self-declared case: entry.Reason below distinguishes it from plain
	// insufficiency. Removing the earlier branch would lose that distinction.
	if (!IsSufficient(surface, provided, service.EuScope() ? Jurisdiction::Eu : Jurisdiction::UnitedStates))
	{
		entry.Decision = AuditDecision::Denied;
		entry.Reason = DenyReason::AssuranceInsufficient;
		Record(entry);
		return false;
	}

	// "Can we prove we said yes?" -- the question the header frames the whole type
	// around. The policy already agreed; what is left is whether the decision can be
	// evidenced.
	// The policy says yes. Now: can we prove we said yes?
	entry.Decision = AuditDecision::Granted;
	entry.Reason = DenyReason::None;
	// DEFECT (silent denial): a failed Record() returns false and the caller learns
	// only that the feature is closed, with no DenyReason recorded anywhere.
	// DenyReason::NotRecorded exists in the header for exactly this case and is never
	// used: nothing here can record it, because recording it is what failed. So the
	// enum value is dead. Not fixed; comments only.
	//
	// The asymmetry: a policy denial is audited, an audit failure is not, and both
	// reach the caller as the same `false`.
	if (!Record(entry))
	{
		// Could not record. Log the reason in memory if there is anywhere to put
		// it -- there is not, so the caller learns from the false return.
		return false;
	}
	return true;
}

// Entries() returns a COPY in chronological order. The alternative -- handing
// out a reference to the internal vector -- would let a caller see slots in ring
// order and would couple them to when Clear() is called next.
std::vector<AgeAuditEntry> AgeAuditLog::Entries() const
{
	// Oldest first.
	//
	// Once the ring has wrapped, the oldest surviving entry is at m_Next.
	// Returning the buffer in raw slot order would look chronological to a
	// human reading it and be exactly backwards, which is a nasty way to
	// mislead an auditor.
	// reserve rather than a fixed-size vector: m_Size is exact, so this allocates
	// once instead of growing.
	std::vector<AgeAuditEntry> out;
	out.reserve(m_Size);
	// Returns before the wrap check: with m_Size == 0 and m_Wrapped true, the
	// wrapped branch below would iterate a full capacity of never-written slots.
	if (m_Size == 0)
		return out;

	// The wrapped case starts at m_Next -- the oldest surviving entry -- and wraps
	// to the end. Getting this wrong would produce output that looks chronological
	// to a human reading it and is exactly backwards, which is a nasty way to
	// mislead an auditor.
	if (m_Wrapped)
	{
		// First half: from the write cursor to the end of the buffer.
		for (std::size_t i = m_Next; i < m_Capacity; ++i)
			out.push_back(m_Entries[i]);
		// Second half: from the start up to the cursor. Together they emit every live
		// entry oldest first.
		for (std::size_t i = 0; i < m_Next; ++i)
			out.push_back(m_Entries[i]);
	}
	// Not-wrapped: slots 0..m_Size are in order already, so a straight walk.
	// No modulo needed -- the ring has not wrapped, so the cursor has not passed the
	// end.
	else
	{
		for (std::size_t i = 0; i < m_Size; ++i)
			out.push_back(m_Entries[i]);
	}
	return out;
}

// Clear() resets the three bookkeeping fields. It does NOT shrink m_Entries,
// so a cleared log is immediately reusable and Record() still cannot allocate --
// which is the property the constructor worked to establish.
void AgeAuditLog::Clear()
{
	m_Size = 0;
	m_Next = 0;
	m_Wrapped = false;
	// Slots are left in place rather than wiped. AgeAuditEntry is trivially
	// copyable and standard layout (both static_asserted in the header) and holds no
	// pointers, so a stale slot cannot leak anything or be mistaken for a live one --
	// Entries() never reads past m_Size, which is 0 by now.
	//
	// Non-throwing is the other reason: studios call this from a shutdown path that
	// must not fail. A resize(0) would free the buffer and make the next Record()
	// allocate -- the exact hazard the constructor avoided.
	// Slots are left in place rather than wiped. AgeAuditEntry holds no
	// pointers and no identity, so a stale slot cannot leak anything, and
	// Clear() stays non-throwing -- which matters because studios call it from
	// a shutdown path that must not fail.
}

} // namespace Wasteland::Age