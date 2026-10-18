// Wasteland::Age -- the module's integration point.
//
// AgeService is where the rest of the Age module becomes real. It holds the
// studio's provider, resolves an attestation into a SafetyProfile and a
// GuardianPolicy, and answers the questions a game asks at runtime:
//
//     if (AgeService::Instance().Profile().FreeTextChat()) { ... }
//
// FOUR DESIGN RULES
//
// 1. LIVE FROM CONSTRUCTION. There is no Enable() and no opt-in. A studio
//    that links the engine and does nothing still gets the restrictive
//    defaults. A module that must be switched on before it protects anybody
//    is a module that ships unprotected.
//
// 2. NO PROVIDER MEANS NO SIGNAL, WHICH MEANS MINOR. Constructing an
//    AgeService with no provider produces exactly the state of one that asked
//    a provider and got nothing. The two are indistinguishable on purpose: a
//    game must not be able to tell the difference and behave differently.
//
// 3. EU SCOPE NARROWS JURISDICTION RULES AND NOTHING ELSE. SetEuScope(false)
//    changes which rules JurisdictionPolicy consults. It does NOT relax
//    SafetyProfile or GuardianPolicy, because Art 8(1) child safety is not
//    jurisdiction-conditional. A US-only build has COPPA obligations and
//    every reason not to expose a thirteen-year-old's profile publicly, and it
//    gets no exemption here.
//
// 4. A TRANSIENT FAILURE REVERTS TO RESTRICTED. See Refresh().
//
// NEITHER INSTRUMENT IS IN FORCE. The EU KIDS Act proposal (COM(2026) 681,
// 2026/0286(COD)) is pending legislative passage; the US App Store
// Accountability Acts are in force. See docs/AGE_ASSURANCE.md.

#pragma once

#include <memory>

#include "Wasteland/Age/Attestation.h"
#include "Wasteland/Age/GuardianPolicy.h"
#include "Wasteland/Age/IAgeAssuranceProvider.h"
#include "Wasteland/Age/JurisdictionPolicy.h"
#include "Wasteland/Age/SafetyProfile.h"

namespace Wasteland::Age
{

// Holds a provider and answers the runtime questions.
//
// Constructing one is safe and already enforcing: with no provider it reports
// AgeBand::Unknown, IsMinor() == true, the restrictive SafetyProfile, and
// GuardianState::Unconfigured. Every one of those is the fail-closed answer.
//
// Not thread-safe. Refresh() must be called off the render thread -- a vendor
// SDK may need a round trip -- and the read accessors then called from the
// render thread. See docs/AGE_ASSURANCE.md for the threading contract.
class AgeService
{
public:
	// Fail-closed from the first statement. Defined out of line so the
	// default state -- an Absent attestation, no provider, EU scope on -- is
	// established in exactly one place.
	AgeService();

	AgeService(const AgeService &) = delete;
	AgeService &operator=(const AgeService &) = delete;

	// The process-wide service used by the engine and by game scripts.
	//
	// A singleton is a compromise, and a deliberate one: SafetyProfile and
	// GuardianPolicy are derived from a BAND, which is the same for every
	// player of a given age, so there is nothing per-player to lose here.
	// Studios that genuinely need per-account age -- a shared-family console,
	// say -- construct their own AgeService rather than reaching for this one.
	//
	// Tests deliberately do NOT use Instance(): each case constructs its own
	// service so the tests stay order-independent.
	static AgeService &Instance();

	// Install (or replace, or remove) the provider.
	//
	// Passing nullptr removes it. Replacing a provider does NOT preserve the
	// previous verdict: call Refresh() afterwards, and until you do the
	// service is unverified. Swapping vendors mid-session must not be able to
	// leave a stale adult verdict in force.
	void SetProvider(std::unique_ptr<IAgeAssuranceProvider> provider);

	bool HasProvider() const { return m_Provider != nullptr; }

	// Ask the provider and re-resolve.
	//
	// With no provider, or an unavailable one, the result is Absent() -- the
	// same as not asking.
	//
	// DELIBERATE: a provider that stops answering does NOT leave the last good
	// verdict in place. Caching a verified adult until the provider returns
	// would mean a player who was adult once stays adult indefinitely, even if
	// the signal expired or was revoked -- and Google Play updates cached
	// signals within 2 to 8 weeks of a birthday, so a held verdict goes stale
	// silently.
	//
	// The cost is a user-visible cliff: a transient network error locks a
	// legitimate adult out of chat and the marketplace until it clears. A
	// studio wanting a grace period should implement one in its own layer,
	// with its own expiry policy, where the trade-off is visible in their
	// code -- rather than having the engine hold compliance state on their
	// behalf.
	//
	// Never throws. A provider that throws is treated as unavailable, because
	// an exception crossing into a render loop is worse than a restrictive
	// answer.
	void Refresh();

	// True only when a provider supplied a real attestation. A self-declared
	// band is NOT verified -- see Attestation::IsMinor().
	bool IsVerified() const { return m_Verified; }

	// The resolved band, or AgeBand::Unknown when there is no signal.
	AgeBand Band() const { return m_Attestation->Band(); }

	// Must this player be treated as a minor? Fail-closed; see
	// Attestation::IsMinor().
	bool IsMinor() const { return m_Attestation->IsMinor(); }

	AssuranceLevel Assurance() const { return m_Attestation->Level(); }

	// Named CurrentAttestation rather than Attestation: a member function
	// called Attestation would hide the TYPE name inside the rest of this
	// class, so `std::unique_ptr<Attestation>` below would fail to compile.
	// That collision is easy to create by accident and produces a baffling
	// "too few template arguments" rather than anything mentioning shadowing.
	const Attestation &CurrentAttestation() const { return *m_Attestation; }

	// The Art 8(1) safety-by-default profile for the resolved band.
	//
	// Jurisdiction-independent by design. See rule 3.
	SafetyProfile Profile() const;

	// The guardian state machine for the resolved band.
	//
	// Jurisdiction-independent by design. See rule 3.
	GuardianPolicy Policy() const;

	// Whether EU rules apply. TRUE by default: a build shipping into the EU
	// cannot fall outside the proposal's scope by forgetting to call this.
	bool EuScope() const { return m_EuScope; }

	// Narrow the jurisdiction rules to US-only.
	//
	// Does NOT relax SafetyProfile or GuardianPolicy. See rule 3.
	void SetEuScope(bool enabled) { m_EuScope = enabled; }

	// Is `provided` strong enough for `surface` under the current scope?
	//
	// This is the runtime form of JurisdictionPolicy::IsSufficient, with the
	// service's scope applied. A studio calls this before opening a feature
	// that the proposal governs -- social or video-sharing surfaces under
	// Art 29(2), and increasingly game surfaces too.
	bool IsAssuranceSufficient(SurfaceKind surface, AssuranceLevel provided) const;

private:
	std::unique_ptr<IAgeAssuranceProvider> m_Provider;

	// Stored as an optional because Attestation is deliberately
	// non-assignable: a verdict cannot be overwritten in place. Refresh()
	// emplaces a new one rather than assigning over the old.
	std::unique_ptr<Attestation> m_Attestation;

	bool m_Verified = false;
	bool m_EuScope = true;
};

} // namespace Wasteland::Age