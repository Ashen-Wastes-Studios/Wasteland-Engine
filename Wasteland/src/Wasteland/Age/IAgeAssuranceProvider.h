// Wasteland::Age -- the seam a studio implements to supply age assurance.
//
// The engine does not verify ages. It cannot: doing so would mean shipping
// identity-document processing, biometric matching, or a network client in
// the engine core, none of which belongs in a renderer, and the first two
// of which would put the engine in the business of handling identity data
// that Art 28(1)-(3) of the EU KIDS Act proposal requires be kept separate
// from the assurance process.
//
// So the engine defines the seam and the studio supplies the implementation:
//
//   * Apple Declared Age Range   -- platform, no identity, free.
//   * Google Play Age Signals    -- platform, no identity, free.
//   * Yoti, Persona, et al.      -- commercial vendor, stronger, paid.
//   * A certified EU Age Verification Scheme solution, for Art 29(2)
//     surfaces in EU scope.
//
// Apple and Google ship reference implementations of this interface in
// Age/Providers/. Everything else is the studio's to write against the three
// methods below.
//
// NO NETWORK CALLS live in the engine core. A provider that needs the
// network does it in the studio's own code, on its own thread, with its own
// error handling. That keeps age assurance out of the engine's frame budget
// and keeps the engine shippable in contexts where it must not phone home.

// Include guard, placed below the file's own explanatory header rather than
// at the top. Unusual, but it keeps the essay about the compliance reasoning
// in the first thing a reader sees while still being a valid include guard.
#pragma once

// For the ProviderName() return type below.
#include <string>

// The value type Query() returns.

// Included rather than forward-declared because Query() returns Attestation
// BY VALUE, so the complete type is required at the declaration.
#include "Wasteland/Age/Attestation.h"

// C++17 nested namespace: equivalent to namespace Wasteland { namespace Age {
// but suppresses the repeated prefix inside.

// The Age module is namespaced where the renderer is not -- Shader.h puts
// everything in flat Wasteland. That is a deliberate choice for a compliance
// module: it makes every age-related symbol trivially greppable and hard to
// collide with.
namespace Wasteland::Age
{

// Implemented by the studio; consumed by the engine.
//
// Three methods, all const-correct and all non-blocking in intent. Query() is
// allowed to block -- a vendor SDK may need a round trip -- but it must not
// be called from the render thread, and AgeService is responsible for calling
// it off-thread rather than leaving that to every studio.
// The seam a studio implements. Pure interface -- no data, no non-virtual
// methods, no implementation.

// Note it is NOT exported with WL_API and holds no engine types, so a studio
// can implement it in its own assembly without linking against the engine.
class IAgeAssuranceProvider
{
// Everything is public. There is no protected section, which is correct for
// an interface with no state to protect.
public:
	// Virtual, and not defaulted to nothing: studios hold providers as
	// IAgeAssuranceProvider pointers and delete them through that type, so a
	// non-virtual destructor here would be a silent memory bug in every
	// consumer.
	// Virtual, defaulted in the declaration.

	// The file's own comment above states why: studios hold providers through
	// this base pointer type and delete them through it, so a non-virtual
	// destructor here would be undefined behaviour on every such delete. This is
	// the one member of an interface where `virtual` is not boilerplate.
	virtual ~IAgeAssuranceProvider() = default;

	// Is this provider usable right now?
	//
	// A false here is normal, not an error: the platform API may be
	// unavailable, the vendor SDK may not be initialised, the user may have
	// declined. AgeService treats an unavailable provider as no provider at
	// all, which means SignalAbsent and the restrictive defaults -- so an
	// implementation should be honest rather than optimistic.
	// Whether this provider can be used right now.

	// const because availability is a property of the environment (platform API
	// present, SDK initialised, user has not declined), not of the provider.

	// A false is a NORMAL answer, not an error. The header's comment is explicit
	// that AgeService treats an unavailable provider as no provider, yielding
	// SignalAbsent plus restrictive defaults. So the failure mode is fail-closed
	// by design, and an implementation should report honestly rather than
	// optimistically.
	virtual bool IsAvailable() const = 0;

	// The age attestation, or Attestation::Absent() if there is no signal.
	//
	// Contract: this must never throw and never return an Attestation that
	// overstates the player's age. On any internal error, return Absent().
	// The attestation, or Attestation::Absent() when there is none.

	// Non-const, unlike its two siblings: obtaining an attestation is a query
	// that may have side effects on the provider (a vendor SDK round trip, a
	// cached token refresh), so it cannot promise not to mutate.

	// The header's stated contract is that this must never throw and must never
	// return an Attestation overstating the player's age -- on internal error,
	// return Absent(). So every failure path collapses to the same conservative
	// answer.
	virtual Attestation Query() = 0;

	// Name for logs and the compliance audit trail, e.g. "apple",
	// "google-play", "yoti". Not a user identifier.
	// A short name for logs and the compliance audit trail -- "apple",
	// "google-play", "yoti".

	// Returns const& rather than a string by value: it is called for logging, and
	// a reference avoids allocating per log line. The header is explicit that
	// this is NOT a user identifier, which matters because these names reach
	// audit records.
	virtual const std::string &ProviderName() const = 0;
};

} // namespace Wasteland::Age