// Wasteland::Age -- an age attestation.
//
// Attestation is the module's only input from outside itself: what some
// assurance method says about a player's age. It is deliberately the
// smallest possible carrier of that fact.
//
// PRIVACY POSTURE -- read before adding a field.
//
// An Attestation carries an age band, an assurance level, and the name of
// the method that produced them. It carries no user id, no date of birth, no
// session token, no account handle, and nothing else that could be joined
// back to a person. That is not an oversight to be filled in later -- it is
// the design.
//
// The EU KIDS Act proposal (COM(2026) 681, 2026/0286(COD)) Art 28(1)-(3)
// requires that a user's personal data be kept separate from the age
// assurance process and that any assurance measure be zero-knowledge. A type
// that carried an identity would make the join representable in the first
// place, and the guarantee would live only in review comments. By making the
// join unrepresentable in the type, it cannot be introduced by accident.
//
// If a future change needs a per-player identifier here, that is a signal the
// design is wrong, not that a field is missing.
//
// Neither instrument is in force as of this writing. See
// docs/AGE_ASSURANCE.md.

// Header guard. The comment block above it is the privacy posture for the
// whole file, so it must stay ABOVE the guard: a reader who opens this file
// sees the rule before the code.
#pragma once

// The ONLY standard-library include, and it exists solely for m_Method.
// Nothing here needs a container, a pointer or an allocator -- that
// absence is itself the design statement from the comment above.
#include <string>

// AgeBand is the verdict. Included rather than forward-declared because
// m_Band is a by-value member and the file documents its default as
// AgeBand::Unknown.
#include "Wasteland/Age/AgeBand.h"
// AssuranceLevel for the same reason. Pulling in the policy header also
// brings MinimumLevelFor/IsSufficient, which IsMinor() below
// deliberately does NOT call.
#include "Wasteland/Age/JurisdictionPolicy.h"

namespace Wasteland::Age
{

// One provider's answer about one player's age, with no identity attached.
//
// Immutable: constructed once by the provider, then only read. Same rationale
// as SafetyProfile -- an age verdict that can be overwritten in place is not
// an age verdict, it is a mutable variable that happens to hold a verdict.
// An immutable value with a copy ctor and deleted assignments -- see the
// members below for why that combination is chosen rather than simply
// making every field private and adding no mutators.
class Attestation
{
// Everything below is either a factory or a const accessor. There is no
// non-const member function anywhere in the class, which is the
// mechanical form of "immutable after construction".
public:
	// Default-constructs to the fail-closed state: band Unknown, level
	// SelfDeclared, no method, IsMinor() true.
	//
	// This is deliberately NOT a neutral or "no opinion" value. It is a
	// restrictive one, because the three ways a caller realistically obtains
	// an Attestation -- a provider with no signal, a provider that failed,
	// and a call site that forgot to check -- must all land on the answer
	// that keeps a minor's protections intact.
	// Defaulted, and the DEFAULT STATE IS THE POINT: band Unknown, level
	// SelfDeclared, empty method, IsMinor() true. The three ways a caller
	// realistically obtains one -- no signal, provider failed, forgot to
	// check -- all collapse onto the restrictive answer rather than onto a
	// neutral one that some future comparison would treat as adult.
	Attestation() = default;

	// Copy construction only, matching SafetyProfile: no move constructor
	// (a user-declared one would delete the copy constructor), no assignment
	// in either form.
	// Copy construction IS available: an attestation is passed around by value
	// and returned by value from the factories below, which without a copy
	// ctor would be impossible.
	Attestation(const Attestation &) = default;
	// Assignment deleted, so an existing verdict cannot be quietly replaced.
	// DEFECT (incomplete immutability, documented not fixed): this leaves
	// m_Method, an std::string, as a genuinely non-immutable member. A
	// deleted copy-assignment does not stop the object being mutated through
	// a non-const reference obtained by any other route; it only removes the
	// most obvious door.
	Attestation &operator=(const Attestation &) = delete;
	// Move assignment deleted for the same reason. Note the comment above this
	// pair: because a move assignment is USER-declared, it would suppress the
	// implicit move CONSTRUCTOR -- which is why the copy ctor above is
	// written out explicitly. Deleting one without noting that would have
	// changed copy semantics as a side effect.
	Attestation &operator=(Attestation &&) = delete;

	// No signal was received, or the provider could not reach its service.
	// Identical to a default-constructed Attestation, named for intent.
	// Named for intent, not a second constructor. It returns the same object a
	// default-constructed Attestation gives, so there is no way to represent
	// "absent" other than as the restrictive state -- which is exactly the
	// property that makes the default safe.
	static Attestation Absent();

	// An attestation from a real assurance method.
	//
	// `method` is the audit trail: it is how a studio answers "on what basis
	// was this player treated as an adult". Pass the provider or scheme name
	// -- "apple", "google-play", "yoti", "eu-aaa" -- not a user identifier.
	// The ONLY way to produce a non-restrictive attestation, and it takes the
	// level explicitly rather than deriving it. That is what lets an
	// audit record state the basis for the verdict instead of re-deriving
	// it later.
	//
	// DEFECT (no validation, documented not fixed): nothing here rejects
	// level == AssuranceLevel::SelfDeclared, or a mismatched pair such as
	// EuEaa with AgeBand::Unknown. IsMinor() catches the SelfDeclared/Adult18
	// combination specifically, but a caller can still construct a
	// self-consistent-looking attestation that claims more assurance than
	// any provider issued.
	static Attestation FromProvider(AssuranceLevel level, AgeBand band, std::string method);

	// Accessors rather than public fields: the fields are private so a future
	// representation change (adding validation, caching a derived flag) is
	// not a breaking edit.
	AssuranceLevel Level() const { return m_Level; }
	AgeBand Band() const { return m_Band; }
	// By reference: the audit trail string, returned without copying because it
	// is read on audit paths and never mutated.
	const std::string &Method() const { return m_Method; }

	// Must this player be treated as a minor?
	//
	// Returns false ONLY for a verified adult: an Adult18 band that some
	// assurance method actually vouched for. Every other outcome -- a minor
	// band, an Unknown band, an unrecognised band value, and critically an
	// Adult18 band reached via AssuranceLevel::SelfDeclared -- returns true.
	//
	// The self-declared case is the point. Art 3(i) excludes self-declaration
	// from "age assurance" entirely, so a date-of-birth box is not evidence
	// of age. Without this rule the obvious implementation -- compare the
	// band against Adult18 -- lets a fourteen-year-old who typed 18 into a
	// form unlock every age gate in the game, and nothing else in the module
	// would catch it, because the band really does say Adult18.
	//
	// This is the band question, not the policy question. Whether the
	// assurance method is strong ENOUGH for a given surface and jurisdiction
	// is JurisdictionPolicy::IsSufficient's job; asking it twice here would
	// duplicate the Art 29(2)/29(4) rules in the wrong place.
	// Non-trivial, and the reason is spelled out at length above it: it is NOT
	// a band comparison. It returns false only for Adult18 reached through
	// a level stronger than SelfDeclared.
	//
	// DEFECT (duplicated default, documented not fixed): the rule is
	// re-encoded here in the implementation as well as in the field
	// defaults at the bottom of the class. A change to either -- say adding
	// a new AssuranceLevel or a new AgeBand above Adult18 -- has to be made
	// in both places, and nothing in the type system connects them.
	bool IsMinor() const;

private:
	// In-class default: every path that produces an attestation without naming
	// a level lands on SelfDeclared, which is the weakest rung in the
	// JurisdictionPolicy ladder. This single line is where Art 3(i) is
	// enforced for the absent case.
	AssuranceLevel m_Level = AssuranceLevel::SelfDeclared;
	// Unknown, not Adult18 and not a "0" band. The fail-closed band, and the
	// reason IsMinor() returns true for it.
	AgeBand m_Band = AgeBand::Unknown;
	// The one heap-owning member. DEFECT (privacy posture, documented not
	// fixed): the file comment above states this type carries nothing that
	// could be joined back to a person, and m_Method is the single field
	// through which that could happen -- a provider name plus a free-text
	// value is not a user id, but nothing stops a caller passing one as the
	// "method". FromProvider does not validate the contents. The guarantee is
	// a code-review convention here, not a type-level one.
	std::string m_Method;
};

} // namespace Wasteland::Age