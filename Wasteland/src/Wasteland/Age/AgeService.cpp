#include "wlpch.h"

#include "Wasteland/Age/AgeService.h"

// C++17 nested-namespace form. Shorter than namespace Age { namespace { }
// and it cannot be closed by the wrong brace, which matters in a file
// this short.
// The closing brace on line 136 is NAMED, unlike several other
// namespaces in this codebase.
namespace Wasteland::Age
{

// The default constructor. Note what is absent: no member-initialiser
// list. Every member relies on its in-class default in the header
// (m_Provider null, m_Verified false, m_EuScope true) except
// m_Attestation, which is assigned in the body on line 18.
// That makes this constructor the ONLY place the fail-closed state is
// established for the attestation, which is exactly what the header
// says it is for.
// DEFECT (copy elision not used): the header deletes the copy ctor and
// copy assignment, but NOT the MOVE ctor or move assignment. The
// unique_ptr members make AgeService move-constructible, so an
// accidental move is possible; moving leaves the source with a null
// m_Attestation, and every accessor dereferences m_Attestation without a
// null check (Band(), IsMinor(), Assurance(), CurrentAttestation()).
// So a moved-from AgeService is silently unsafe to query. Deleting the
// move operations, or defaulting AgeService() = default and requiring
// explicit init, would close that. Not fixed here.
AgeService::AgeService()
{
	// The fail-closed default, established once here.
	//
	// An Attestation is stored behind a pointer rather than by value because
	// it is deliberately non-assignable -- a verdict must not be overwritable
	// in place. Refresh() therefore replaces the whole object rather than
	// assigning over the old one, which also makes the "forget the previous
	// verdict" behaviour in Refresh() fall out of the design instead of
	// needing a flag to enforce it.
	// A heap allocation for what is conceptually an immutable value.
	// The comment above explains why it is behind a pointer at all:
	// Attestation is non-assignable, and a std::optional could have
	// emplace-constructed instead, which would avoid the allocation and
	// the null state entirely. Worth noting as the design alternative.
	// DEFECT: if make_unique throws (bad_alloc), m_Attestation is left as
	// it was -- which for a freshly constructed object is a null pointer, and
	// every accessor then dereferences null. A member initialiser would be
	// stronger here.
	m_Attestation = std::make_unique<Attestation>(Attestation::Absent());
}

// Out-of-line definition of the static declared in the header.
AgeService &AgeService::Instance()
{
	// Function-local static: initialised on first use, which means the
	// fail-closed constructor runs before any caller can observe it, and is
	// thread-safe to initialise under C++11 and later.
	// Function-local static rather than a namespace-scope one. The header
	// explains why: construction is deferred to first use, so the
	// fail-closed constructor has demonstrably run before any caller can
	// observe the object.
	// C++11 guarantees this initialisation is thread-safe (one thread
	// runs the constructor, others block). That guarantee does NOT extend
	// to the reads afterwards -- the class is explicitly documented as not
	// thread-safe, because SetProvider and Refresh mutate m_Attestation
	// and m_Verified with no synchronisation.
	// DEFECT: the static is never destroyed, or rather it is destroyed at
	// exit -- which is why m_Attestation being heap-allocated matters: the
	// unique_ptr will run m_Provider's destructor during static teardown,
	// and if that vendor SDK touches another already-destroyed static, the
	// order is undefined. Not verifiable from this file, but it is the kind
	// of hazard the indirection introduces.
	static AgeService service;
	return service;
}

// Takes OWNERSHIP by value, then moves on line 32. A unique_ptr parameter
// is the correct shape: the caller cannot keep using the provider after
// handing it over, and there is no second owner to race with.
// Passing nullptr is explicitly supported and means "remove the
// provider" -- see the header.
void AgeService::SetProvider(std::unique_ptr<IAgeAssuranceProvider> provider)
{
	// DEFECT: this ASSIGNS over the existing provider rather than releasing
	// the old one explicitly. unique_ptr move-assignment does delete the
	// previous object, so there is no leak -- but the destruction of the
	// OUTGOING vendor SDK happens here, inside SetProvider.
	// If that destructor touches GL, a window, or a logging system that is
	// already torn down, this is where it blows up. The header does not
	// document any teardown-order requirement on the outgoing provider.
	m_Provider = std::move(provider);

	// The previous verdict is dropped HERE, not deferred to the next
	// Refresh(). Until the new provider has been asked, this service knows
	// nothing -- so swapping vendors mid-session cannot leave a stale adult
	// verdict in force for even one frame.
	//
	// Attestation is non-assignable, so replace the object rather than
	// assigning into it.
	// Immediately after the swap, the verdict is discarded rather than
	// kept until the next Refresh(). The comment above states the intent
	// and it is the right call: a stale adult verdict must not survive a
	// vendor change for even one frame.
	// Cost: this discards the previous Attestation object. Combined with
	// the same pattern in Refresh(), a per-frame Refresh() means one heap
	// allocation and deallocation per frame for a value that fits in a
	// few bytes. A std::optional with emplace would avoid both.
	m_Attestation = std::make_unique<Attestation>(Attestation::Absent());
	// Reset alongside the attestation. These two are ALWAYS written
	// together in this file -- line 18 leaves it false from the default,
	// lines 42 and 55 clear it, line 73 clears it, and line 92 sets it.
	// There is no path that sets one without the other, so the invariant
	// holds.
	// But it is an INVARIANT maintained by convention across four sites
	// rather than by construction: nothing in the type system ties the
	// two together. A single future edit that sets m_Verified without
	// touching m_Attestation would break it silently.
	m_Verified = false;
}

// The only method that queries the provider.
// Not const because it mutates; the header is explicit that it must
// be called off the render thread and that the accessors are then read
// from the render thread. That split means there is a window in which
// m_Attestation is being replaced while a render thread reads it --
// see the note on line 68.
void AgeService::Refresh()
{
	// No provider, or one that reports itself unavailable, is the same as
	// asking and getting nothing. Both land on Absent().
	//
	// IsAvailable() is called first because a provider that is unavailable
	// should not be asked to do work that may involve a round trip.
	// Short-circuit: IsAvailable() is only called when there IS a provider.
	// DEFECT (TOCTOU): this checks availability and then, at line 68, does
	// the real work, with no lock. A provider that reports available and
	// then becomes unavailable -- or a vendor SDK that is being torn down
	// concurrently -- lands in the catch block on line 70, which is the
	// intended fallback. The design tolerates the race by catching, but the
	// IsAvailable() check is then only an optimisation to avoid a pointless
	// round trip, not a correctness gate. The comment above it says exactly
	// that, which is the right framing.
	if (!m_Provider || !m_Provider->IsAvailable())
	{
		m_Attestation = std::make_unique<Attestation>(Attestation::Absent());
		m_Verified = false;
		return;
	}

	// A provider that throws is treated as unavailable. An exception escaping
	// into a render loop is worse than a restrictive answer, and the
	// restrictive answer is the one the rest of the module is built around.
	//
	// Attestation is non-assignable, so the result cannot be assigned into a
	// local and then moved; it is constructed directly from the provider call
	// inside the try block.
	try
	{
		// The provider call, wrapped in the try below.
		// The result is constructed directly from Query() rather than
		// assigned to a local first, because Attestation is non-assignable --
		// the comment above the try explains this.
		// DEFECT (race, per the documented threading contract): this line
		// REPLACES the unique_ptr. Any render-thread reader that called Band()
		// or IsMinor() at the same moment reads the old object through a pointer
		// that has already been reassigned. Reading the OLD Attestation is still
		// safe (the object is not destroyed until the assignment completes), but
		// there is no ordering guarantee, and if a reader dereferences
		// m_Attestation itself while the assignment is in flight, that is a
		// data race on the pointer. A reader that cached a reference from
		// CurrentAttestation() and used it across the Refresh boundary would be
		// reading freed memory. The header documents the threading contract but
		// the code does not enforce it.
		m_Attestation = std::make_unique<Attestation>(m_Provider->Query());
	}
	catch (...)
	{
		// The fail-closed fallback in the catch. Catch(...) rather than a
		// specific exception type, which is the right choice for a boundary that
		// must not let anything through -- a vendor SDK can throw anything,
		// including something not derived from std::exception.
		// The cost of catch(...) is that a std::bad_alloc from the
		// make_unique on this line is ALSO caught, and then the same
		// make_unique is retried, so allocation failure loops rather than
		// propagating.
		m_Attestation = std::make_unique<Attestation>(Attestation::Absent());
		m_Verified = false;
		return;
	}

	// "Verified" means a real assurance method vouched for the BAND -- which
	// is NOT the same question as "is this player an adult".
	//
	// A verified 13-year-old IS verified. They are also a minor, and both are
	// true at once. Collapsing the two concepts is the bug class this module
	// exists to prevent: the moment "verified" starts meaning "adult", a
	// check written as `if (verified) unlock()` unlocks for every minor.
	//
	// So this asks only about provenance: was the band asserted by something
	// other than the player, and is it a band this engine recognises?
	//
	// A self-declared band is NOT verified even when it reads Adult18 -- the
	// band is a number somebody typed, not a finding. Note this deliberately
	// mirrors Attestation::IsMinor()'s rule rather than reimplementing it, so
	// the two cannot drift apart.
	// The provenance test, split across two lines with the && at the end
	// of the first -- a formatting choice that makes the two independent
	// conditions read as a list.
	// The comment above is unusually good: it explains WHY this is not
	// an adulthood test, and names the specific bug class (verified=>unlock)
	// the separation prevents.
	m_Verified = m_Attestation->Level() != AssuranceLevel::SelfDeclared &&
	             // Second condition: a band this engine recognises.
	             // DEFECT (completeness): this is the only place m_Verified is set to
	             // true, and there is no corresponding check that the provider returned
	             // a SELF-CONSISTENT attestation -- for example, Level() == AdultVerified
	             // with Band() == Unknown passes only because of the second condition,
	             // but a Level of AdultVerified paired with a Minor band is accepted
	             // without comment. Whether the provider is trusted to be self-consistent
	             // is a question for IAgeAssuranceProvider, which is not visible here.
	             m_Attestation->Band() != AgeBand::Unknown;
}

// const: pure read, no mutation. Nothing in the header suggests this
// should be cached, so every call re-derives from the band.
SafetyProfile AgeService::Profile() const
{
	// Jurisdiction-independent. See rule 3 in the header: Art 8(1) child
	// safety does not become conditional on SetEuScope(false).
	//
	// Note this passes the BAND, not "is this a minor". For a self-declared
	// adult the band is Adult18 but IsMinor() is true, and the profile must
	// be the RESTRICTIVE one. AgeBand::Unknown would also be restrictive, but
	// using the band keeps a genuinely verified minor's profile honest -- a
	// 13-15 gets Minor's settings rather than Unconfigured's -- while a
	// self-declared adult, whose band says Adult18, is caught by IsMinor()
	// at every call site.
	//
	// The two are reconciled here by resolving on the effective band: a
	// self-declared adult is treated as unverified, so it gets Unconfigured's
	// restrictive defaults rather than Adult18's.
	// The self-declared-adult reconciliation. Both conditions are needed:
	// a genuinely VERIFIED adult is not a minor, so the first condition
	// distinguishes the self-declared case from the real one.
	// DEFECT (duplication): this exact condition is repeated in Policy()
	// on line 122. Two places must now agree for the safety property to
	// hold; a future edit to one and not the other would let a self-declared
	// adult get an Adult profile OR an Adult guardian policy. It should be
	// a private helper returning the effective band. Not fixed here.
	if (IsMinor() && Band() == AgeBand::Adult18)
		// Falls back to the UNKNOWN band rather than to a hardcoded default
		// profile, so the Unconfigured settings are whatever the registry
		// says they are -- one source of truth.
		return ProfileFor(AgeBand::Unknown);

	// The ordinary path. Band() is AgeBand::Unknown when there is no
	// signal, which lands on the same restrictive profile as line 113 -- so
	// the no-provider case and the self-declared-adult case converge, which
	// is design rule 2.
	return ProfileFor(Band());
}

GuardianPolicy AgeService::Policy() const
{
	// Same reconciliation as Profile(), and the same reason: a self-declared
	// adult must land in Unconfigured, never in Adult.
	// Same reconciliation as line 112, returning a GuardianPolicy this
	// time. See the duplication note there.
	if (IsMinor() && Band() == AgeBand::Adult18)
		return GuardianPolicy::Restricted();

	// Static factory, unlike ProfileFor which is a free function. The
	// inconsistency is cosmetic.
	return GuardianPolicy::ForBand(Band());
}

// The one method on this class that takes jurisdiction into account,
// and it does so ONLY for this question -- see rule 3 and the note at
// the call below.
bool AgeService::IsAssuranceSufficient(const SurfaceKind surface, const AssuranceLevel provided) const
{
	// The service's scope is the only jurisdiction input here. Nothing about
	// SafetyProfile or GuardianPolicy is consulted, because those are not
	// jurisdiction-conditional -- see rule 3.
	// The scope flag selects a jurisdiction; everything else is
	// JurisdictionPolicy's decision, not this class's.
	// DEFECT (robustness): m_EuScope is a plain bool with a default of
	// true, so the safe answer wins by omission -- which is the design
	// intent stated in the header. But because m_EuScope is read here and
	// written by SetEuScope with no synchronisation, a concurrent
	// SetEuScope during a render-thread read is a race on a bool. Low
	// severity, and inherent to the documented not-thread-safe contract.
	return IsSufficient(surface, provided, m_EuScope ? Jurisdiction::Eu : Jurisdiction::UnitedStates);
}

} // namespace Wasteland::Age