#include "wlpch.h"

#include "Wasteland/Age/Attestation.h"

// Nested namespace, C++17. Matches the header and every other Age-module file;
// Wasteland::Age is the module's public namespace.
namespace Wasteland::Age
{

// The named form of the default-constructed state. It adds no behaviour -- it
// returns exactly what Attestation() would -- and that is the point: a provider
// with no signal, a provider that failed, and a call site that forgot to check
// must all land on ONE state, not three that could drift apart.
Attestation Attestation::Absent()
{
	// No signal. Identical to a default-constructed Attestation by design --
	// there is exactly one fail-closed state, not two that could drift.
	// Fail-closed by member initialiser, not by anything written here: m_Level
	// defaults to SelfDeclared and m_Band to Unknown, so IsMinor() returns true for
	// both. Copy-constructible and non-assignable, so a copy cannot be used to
	// mutate an existing verdict.
	return Attestation();
}

// The only way to obtain an attestation that is not fail-closed, and therefore
// the only place the three members are ever written. `method` is taken by value
// so the caller's string can be moved in rather than copied twice.
Attestation Attestation::FromProvider(const AssuranceLevel level, const AgeBand band, std::string method)
{
	// A local rather than brace-init: the class declares a defaulted copy ctor and
	// deleted assignments, so it is not an aggregate.
	Attestation att;
	att.m_Level = level;
	// std::move on a by-value parameter: the caller usually passes a literal, so
	// this avoids a second heap allocation. The class has no move constructor (one
	// would delete the copy ctor the transitions rely on), so the buffer is copied.
	att.m_Band = band;
	att.m_Method = std::move(method);
	return att;
}

// The one question this type exists to answer, and its only non-trivial function.
bool Attestation::IsMinor() const
{
	// A switch rather than a comparison against Adult18: AgeBand carries no ordering
	// guarantee, so a comparison would change meaning if the values were reordered.
	switch (m_Band)
	{
	// The three minor bands share one arm, so a new minor band in AgeBand becomes a
	// compile error here rather than a silent fallthrough: no `default:` label means
	// -Wswitch enforces it.
	case AgeBand::Under13:
	case AgeBand::Age13To15:
	case AgeBand::Age16To17:
		return true;

	// The only non-constant arm: the minor bands return true outright, so this one
	// has to consult the assurance level as well.
	case AgeBand::Adult18:
		// A band of Adult18 is only as good as the method behind it.
		//
		// Art 3(i) of the EU KIDS Act proposal (COM(2026) 681) excludes
		// self-declaration from "age assurance", so an Adult18 band carrying
		// AssuranceLevel::SelfDeclared means a date-of-birth box said 18.
		// That is not evidence of age, and honouring it would hand a minor
		// the full adult feature set with no other check in the module able
		// to catch it -- the band genuinely says Adult18.
		//
		// Every other level (OsSignal, Commercial, EuEaa) is a real assurance
		// method that vouched for the band, so an adult is an adult.
		// The single most important expression in the Age module: an Adult18 band is
		// accepted ONLY when the level is not SelfDeclared.
		//
		// The shape matters -- it tests for the one level that disqualifies rather than
		// enumerating those that qualify. A provider build newer than the engine could
		// report a level this code has never heard of; an allowlist would reject it as a
		// minor, while the denylist keeps the fail-closed branch for the case the law
		// actually names.
		//
		// Residual risk, stated plainly: OsSignal, Commercial and EuEaa are all treated
		// as genuine vouching here. Whether one is strong ENOUGH for a surface is
		// JurisdictionPolicy's question -- asking it here would duplicate the
		// Art 29(2)/29(4) rules in the wrong layer, as the header explains.
		return m_Level == AssuranceLevel::SelfDeclared;

	// Fail-closed for AgeBand::Unknown AND for any value this build does not
	// recognise -- the `default:` label is what makes the second half true. An unknown
	// band from a newer provider or from corrupt data resolves to "treat as a minor"
	// rather than to whichever arm happened to be added last.
	default:
		// AgeBand::Unknown, and any value the engine does not recognise -- a
		// newer provider build, or corrupt data. Fail closed.
		return true;
	}
}

} // namespace Wasteland::Age