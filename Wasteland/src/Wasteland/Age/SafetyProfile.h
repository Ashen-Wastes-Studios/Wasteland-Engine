// Wasteland::Age -- safety-by-default profile.
//
// SafetyProfile answers: given an age band, what is this player allowed to
// do? It is the module's fail-closed floor, derived from the age band ALONE
// -- no jurisdiction, no assurance level, no account state.
//
// Legal basis, against the EU KIDS Act proposal (COM(2026) 681,
// 2026/0286(COD)):
//
//   Art 8(1)  Providers of online platforms ACCESSIBLE TO CHILDREN shall put
//             in place appropriate and proportionate measures to ensure a
//             high level of privacy, safety and security of minors and, in
//             particular, shall ensure BY DEFAULT that:
//               (a) profiles are not publicly visible;
//               (b) the function allowing contact by strangers is
//                   deactivated;
//               (c) chat functionality is effectively disabled;
//               (d) friend requests require confirmation from the recipient;
//               (e) location sharing is deactivated;
//               (f) the design is age-appropriate.
//
//   Art 20   The assurance solution, and the settings derived from it, must
//             be easy to use, accessible and activate-able, and tailored to
//             the user's age.
//
// Art 8(1) reads "by default", so it is a FLOOR rather than a ceiling. A
// studio may go further -- and must where COPPA-covered data is involved --
// but the engine never treats it as permission to relax.
//
// Two design rules, both enforced by tests in test_safety_profile.cpp:
//
//   1. Unknown resolves to the MOST restrictive profile, never to adult.
//      A missing age signal is not evidence of adulthood, and defaulting it
//      to Adult18 hands a minor every unrestricted feature in the game.
//
//   2. Reporting and muting are never gated by age. They are safety tools,
//      not content features. Art 20 requires them to be usable at every age
//      -- including by a child who is not permitted to chat at all, because
//      that child is precisely the one who needs a way out.
//
// Neither instrument is in force as of this writing. See
// docs/AGE_ASSURANCE.md.

#pragma once

// The only include. AgeBand supplies the enum ProfileFor takes, and nothing
// else -- a std::string or vector header here would be unnecessary, since the
// type stores ten scalars and no dynamic state.
#include "Wasteland/Age/AgeBand.h"

namespace Wasteland::Age
{

// Immutable, copyable, and deliberately NOT assignable.
//
// Not assignable because these are Art 8(1) safety defaults, not
// configuration. If a permissive value could be written back into a live
// profile -- by a settings file, a save game, a mod, or a careless call site
// -- the defaults would be overridable at runtime, which is the single thing
// this type exists to prevent. Studios that need a stricter or looser
// profile implement it in their own layer on top of this one; they do not
// edit it here.
//
// Copy-constructible so it can be returned and passed by value freely.
// The profile itself: ten values, all read through inline getters, none
// writable.
//
// Every design decision worth knowing is in the comment block above the
// class, and each one is load-bearing rather than stylistic -- the deleted
// assignment operators below are what stop these being runtime
// configuration.
class SafetyProfile
{
public:
	// Art 8(1)(a): profiles are not publicly visible by default.
	// Art 8(1)(a): a minor's profile is not publicly visible.
	//
	// inline in the header, returning the member directly. No locking and no
	// lazy work, so const-correctness is the whole contract -- and because
	// assignment is deleted, the value can never change after construction.
	//
	// That last point is what makes these getters safe to call from any thread:
	// the object is effectively immutable, so a read needs no synchronisation.
	bool ProfileVisibilityPublic() const { return m_ProfileVisibilityPublic; }

	// Art 8(1)(b): the function allowing a user to be contacted by
	// strangers is deactivated by default.
	// Art 8(1)(b): the contact-by-strangers function is deactivated by default.
	//
	// Named for the capability rather than the prohibition -- a caller asks
	// 'may strangers message this player' and gets false, which is the shape that
	// makes a missing check fail closed.
	bool DirectMessagesFromStrangers() const { return m_DirectMessagesFromStrangers; }

	// Art 8(1)(c): chat functionality is effectively disabled for children.
	// Art 8(1)(c): chat effectively disabled for children.
	//
	// The word 'effectively' in the article is doing real work: a studio may
	// keep a moderated chat available, and this flag is the floor it relaxes
	// from rather than a prohibition.
	bool FreeTextChat() const { return m_FreeTextChat; }

	// Voice is held to the same floor as text chat. A studio may legitimately
	// treat them differently -- text is often moderated asynchronously while
	// voice is not -- but the engine's default treats voice as at least as
	// risky, so a child cannot be reached through the one channel the studio
	// forgot to gate.
	// Voice held to the text-chat floor.
	//
	// The reasoning in the comment above is the important part: treating voice
	// as at least as risky means a child cannot be reached through the channel a
	// studio forgot to gate. A studio that moderates text asynchronously and not
	// voice has to relax this explicitly.
	bool VoiceChat() const { return m_VoiceChat; }

	// DEFECT: no article reference in the comment, unlike every other accessor
	// here.
	//
	// UGC upload is not one of Art 8(1)(a)-(f) as listed in the header, so this
	// is an engine judgement rather than a statutory requirement. That is a
	// reasonable default, but it is undocumented as such, so a reader looking for
	// the legal basis finds none -- and cannot tell whether the omission is
	// deliberate or an oversight.
	bool UserGeneratedContentUpload() const { return m_UserGeneratedContentUpload; }
	// Same gap as the accessor above: no article reference and no comment at
	// all.
	//
	// Marketplace access has a plausible commercial pressure to be relaxed -- it
	// is revenue -- which makes the absence of a stated rationale worth
	// noting more than for the two flags that map to explicit sub-clauses.
	bool MarketplaceAccess() const { return m_MarketplaceAccess; }

	// Art 8(1)(e): location sharing is deactivated by default.
	// Art 8(1)(e): location sharing off by default.
	bool LocationSharing() const { return m_LocationSharing; }

	// Art 8(1)(d): friend requests require confirmation from the recipient.
	//
	// A PROTECTIVE flag -- the safe state is true, which is the opposite
	// polarity from the permission flags above. It stays true for adults too:
	// there it stops being a legal requirement and becomes a one-line
	// anti-abuse default. Relaxing it must be a deliberate studio decision,
	// never a side effect of the age ladder.
	// Art 8(1)(d), and the one flag with INVERTED polarity: the safe state is
	// true, not false.
	//
	// That asymmetry is documented above and it is the most likely source of a
	// bug when reading this class quickly -- every other member defaults to
	// false, so a mental model of 'false is safe' misreads this one.
	//
	// It stays true for adults too, becoming an anti-abuse default rather than a
	// legal requirement, which the comment is careful to say.
	bool FriendRequestsRequireConfirmation() const { return m_FriendRequestsRequireConfirmation; }

	// Art 8(1)(f) / Art 20: age-appropriate design, expressed at runtime as a
	// restriction on when minors may play or interact.
	// Art 8(1)(f) / Art 20, expressed as a runtime restriction rather than as
	// art direction.
	//
	// Worth noting what this flag does NOT do: it does not verify that the
	// content served to a minor is age-appropriate. It records the age-restricted
	// window and nothing more.
	bool NightHoursRestricted() const { return m_NightHoursRestricted; }

	// 0 means no friend requests permitted at all.
	// 0 means no friend requests at all.
	//
	// An int rather than a bool because the value is a policy knob with more than
	// two states -- but note nothing in this class ever sets it to anything but 0,
	// so the non-zero cases exist only for a studio layer built on top.
	//
	// Unlike the bools, there is no way to distinguish 'unlimited' from 'zero'
	// without a sentinel, so a future relaxation needs a convention this type
	// does not yet define.
	int FriendRequestLimit() const { return m_FriendRequestLimit; }

	// Art 20: safety tools, never gated by age. Always true.
	// Always true, unconditionally.
	//
	// Design rule 2 in the header: reporting and muting are never age-gated,
	// because a child barred from chat is precisely the one who needs a way out.
	// Art 20 requires these to be usable at every age.
	//
	// Returning a literal rather than reading a member is deliberate and is the
	// strongest possible form of the guarantee: there is no field a caller could
	// influence, and no serialised state that could disagree with it.
	bool ReportAvailable() const { return true; }
	// Same unconditional guarantee, for muting.
	//
	// Worth noting this can only be relied upon if callers actually ask: nothing
	// in this type prevents a studio layer from hiding the mute button in its
	// own UI. The type guarantees the ANSWER, not that the question gets asked.
	bool MuteAvailable() const { return true; }

	// Default-constructed to the most restrictive state. Explicit because the
	// copy constructor below is user-declared, which suppresses the implicit
	// default constructor -- and ProfileFor() builds its result by starting
	// from this restrictive value and opening only what the band allows.
	// Default-constructs to the most restrictive state, because every member
	// below has its restrictive initialiser.
	//
	// Explicit even though `= default` generates nothing: the comment above
	// notes the user-declared copy constructor suppresses the implicit default
	// constructor, so this line is load-bearing rather than decorative.
	SafetyProfile() = default;

	// Copy construction stays available so a profile can be returned and passed
	// by value. It must be declared EXPLICITLY, because the user-declared
	// deleted move-assignment operators below suppress its implicit
	// generation. Note there is deliberately NO move constructor: declaring
	// one -- even as `= default` -- is still a user-declaration, and a
	// user-declared move constructor DELETES the copy constructor, which
	// breaks ProfileFor() returning a named local. The copy constructor
	// covers every use here, and C++17 copy elision makes a move ctor
	// unnecessary in the common path anyway.
	//
	// PUBLIC: a profile must be storable and passable by callers. Read-only
	// is enforced by deleting assignment, not by withholding construction.
	// Copy construction, kept available and PUBLIC.
	//
	// Public because a profile must be returnable and passable -- ProfileFor()
	// returns one by value. Read-only is enforced by deleting assignment, not
	// by withholding construction, which is the right split: construction is
	// how values enter the system, assignment is how they would be tampered
	// with.
	SafetyProfile(const SafetyProfile &) = default;

	// Assignment does not exist, in either form. A live SafetyProfile cannot
	// be overwritten with a permissive one, which is what keeps the Art 8(1)
	// defaults off the runtime configuration path: no settings file, save
	// game, mod, or call site can relax them in place.
	//
	// Declared public and deleted rather than private so the diagnostic at a
	// misuse site is "use of deleted function", which names the problem,
	// instead of "inaccessible", which sends people looking for accessors.
	// Copy assignment deleted.
	//
	// DEFECT-adjacent design consequence: this also makes SafetyProfile
	// non-assignable as a consequence of deleting only assignment. So a caller
	// cannot write `profile = ProfileFor(band);` -- they must construct or
	// rebuild. For a value that is immutable once built that is arguably
	// correct, but it means a caller holding a SafetyProfile cannot update it in
	// place, which is a real ergonomic cost and should be a conscious choice.
	//
	// The diagnostics argument above is sound: a deleted public function gives a
	// clearer error than a private one.
	SafetyProfile &operator=(const SafetyProfile &) = delete;
	// Move assignment also deleted.
	//
	// Deliberate, and the comment above explains why no move CONSTRUCTOR is
	// declared instead: a user-declared move constructor would delete the copy
	// constructor, which would break ProfileFor() returning a named local.
	// Deleting move assignment avoids that trap while still closing the
	// assignment path.
	//
	// Note the consequence: because move assignment is deleted, an rvalue cannot
	// be assigned either -- so this deletion is doing real work rather than
	// being implied by the copy-assignment deletion.
	SafetyProfile &operator=(SafetyProfile &&) = delete;

private:
	// The single privileged function.
	//
	// Friendship rather than a public setter, so there is exactly ONE way to
	// build a non-default profile and it is auditable in one place. A studio
	// wanting a different profile writes its own layer; the comment above is
	// explicit that nobody edits these members.
	//
	// DEFECT: friendship is not scoped to the members, so ProfileFor can also
	// read the private getters' state and, more importantly, any future private
	// member added later is silently exposed to it. A private constructor with a
	// token argument would be tighter, but the practical risk is low.
	friend SafetyProfile ProfileFor(AgeBand band);

	// Every member is private and there are no setters of any kind.
	// Every member defaults to the restrictive value.
	//
	// This is what makes `SafetyProfile()` -- defaulted, no body -- produce the
	// most restrictive profile: ProfileFor() starts from a default-constructed
	// value and opens only what the band allows. There is no other initialiser
	// path, so a restrictive default cannot be bypassed.
	bool m_ProfileVisibilityPublic = false;
	bool m_DirectMessagesFromStrangers = false;
	bool m_FreeTextChat = false;
	bool m_VoiceChat = false;
	bool m_UserGeneratedContentUpload = false;
	bool m_MarketplaceAccess = false;
	bool m_LocationSharing = false;
	// The one member defaulting to true, per the polarity note above.
	//
	// Worth flagging to anyone adding a member: this line is the only place the
	// 'safe default' rule inverts, and it is easy to copy the false pattern from
	// the lines above by habit.
	bool m_FriendRequestsRequireConfirmation = true;
	bool m_NightHoursRestricted = true;
	// Zero, so no friend requests by default.
	//
	// The only non-bool member, and the only one whose default is a policy
	// decision rather than a boolean flag -- zero means the feature is off, not
	// 'no limit', which is the fail-closed reading.
	int m_FriendRequestLimit = 0;
};

// The profile for a verified age band.
//
// Pure function of the band: same input, same output, no hidden state, no
// jurisdiction argument. That is deliberate -- a studio that ships into both
// the EU and the US still gets ONE baseline of child safety, and applies any
// jurisdictional tightening on top of it. Letting jurisdiction weaken this
// function would make the Art 8(1) floor conditional on where the player
// happens to be.
//
// Fail-closed: an unrecognised band value -- a provider build newer than the
// engine, or corrupt data -- produces the most restrictive profile.
// The only factory, and a pure function of the band.
//
// No jurisdiction parameter, deliberately: a studio shipping into both the
// EU and the US gets ONE child-safety baseline and layers jurisdictional
// tightening on top. Letting jurisdiction weaken this would make the Art
// 8(1) floor depend on where the player happens to be.
//
// Fail-closed for an unrecognised band, so a provider build newer than the
// engine cannot hand back a permissive profile.
//
// Declared here rather than defined inline because the friend declaration
// above must match the signature exactly -- moving one without the other is
// a compile error.
SafetyProfile ProfileFor(AgeBand band);

} // namespace Wasteland::Age