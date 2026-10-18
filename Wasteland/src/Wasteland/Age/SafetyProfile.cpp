// Precompiled header first, then only the one header this file implements.
// Note there is no <string>, no logging and no Python: this is the
// module's one pure function, and keeping its dependencies that thin
// is what makes it cheap to test exhaustively.
#include "wlpch.h"

#include "Wasteland/Age/SafetyProfile.h"

// One free function in the module's namespace. No state, no statics, no
// singleton -- ProfileFor is a pure mapping from band to profile, so
// the compliance behaviour can be read in full by reading this file.
namespace Wasteland::Age
{

// Takes the band BY VALUE. Small scoped enum, and it makes the function
// obviously pure and safe to call from any thread.
//
// Not taking a jurisdiction: this function answers "what does an age
// band permit", and scope is a separate axis handled by
// JurisdictionPolicy. Keeping them apart is what stops a scope switch
// from quietly widening a profile.
SafetyProfile ProfileFor(const AgeBand band)
{
	// Starts at the member defaults, which are already the most restrictive
	// configuration: every permission false, confirmation required, night
	// hours restricted, zero friend requests. Each case below only OPENS
	// things up, so a case that is added later without setting a field
	// inherits the restrictive default rather than an accidental permission.
	// Default-constructed, NOT brace-initialised with explicit values. The
	// class's in-class member defaults are the restrictive baseline, and
	// reusing them means a field added to SafetyProfile later is
	// automatically restrictive here without anyone editing this file.
	// That is the fail-closed property the comment above describes, and it
	// depends on this being a bare default construction.
	SafetyProfile profile;

	// A switch over every enumerator INCLUDING a default arm, so a band added
	// to AgeBand later compiles without change and -- crucially -- lands in
	// the restrictive default arm rather than in a permissive one. If this
	// were an if/else chain with no fallback the new band would silently
	// take the most recently added branch.
	switch (band)
	{
	// Empty body but not merged with the next case, because the comments
	// a deliberate COPPA-aware decision, Age13To15 is
	// a deliberate EU-13/14-split decision. Collapsing them would lose
	// the reason and invite someone to add a break in the wrong place.
	case AgeBand::Under13:
		// No permissions opened. Art 8(1)(a)-(e) all require restriction, and
		// Art 8(1)(f) is carried by m_NightHoursRestricted staying true.
		//
		// Deliberately includes no UGC upload and no marketplace. Where the
		// player is under 13, COPPA also applies independently of this
		// proposal and imposes its own consent bar on collecting or
		// disclosing personal information; that obligation belongs to the
		// studio's data layer, but the profile must not hand them a channel.
		break;

	// The band exists as a distinct VALUE but produces the same profile as
	// Under13. That is the important property: the EU 13/14 split is a
	// distinction a studio applies in its own layer, and the engine floor
	// does not move with it. Two cases with identical bodies is a statement,
	// not an oversight.
	case AgeBand::Age13To15:
		// Still fully restricted on every Art 8(1) axis. The band exists so
		// that studios can apply the EU's 13-14 / 15 split (guardian-
		// supervised versus autonomous social accounts) in their own layer,
		// but the engine's baseline floor does not move with it. Widening
		// here would mean the engine, not the studio, decided that a
		// 13-year-old may use open chat.
		break;

	// The first band where anything opens, and the reason is spelled out per
	// permission below: these are the two a studio can gate BY BAND rather
	// than by guardian consent.
	case AgeBand::Age16To17:
		// A minor, so Art 8(1) still applies. What opens is the set of
		// features a studio can plausibly gate by band rather than by
		// guardian consent:
		//
		//   - free-text chat: Art 8(1)(c) says "effectively disabled", and
		//     moderated 16+ chat satisfies that in a way that a 13-year-old's
		//     cannot.
		//   - UGC upload: permitted, on the understanding that the studio's
		//     own moderation handles it. The engine has no view of that.
		//
		// Still closed: voice chat, marketplace, public profiles, DMs from
		// strangers, location sharing. Night hours remain restricted.
		// Art 8(1)(c) says chat must be "effectively disabled"; the position here
		// is that a moderated 16+ chat satisfies that, which it does not for a
		// 13-year-old. The judgement is the studio's to contest -- but it is
		// written down where they will find it.
		profile.m_FreeTextChat = true;
		// Opens on the understanding that the studio's own moderation handles
		// the content. The engine has no view of that, which is a real
		// limitation: nothing here verifies a moderation pipeline exists.
		profile.m_UserGeneratedContentUpload = true;
		break;

	// Eight explicit assignments, no wildcard. Listing them one per line means
	// a permission added to SafetyProfile later is visibly NOT opened here
	// -- the compiler will not warn, but the omission is greppable, which a
	// bulk "set everything true" would destroy.
	case AgeBand::Adult18:
		// Outside Art 8(1) entirely -- this is an adult, so the child-safety
		// floor no longer applies. Everything opens.
		//
		// Confirmation on friend requests stays true: that is no longer a
		// legal requirement here and has become a one-line anti-abuse
		// default. Turning it off is a studio decision, not an age-band one.
		// Outside Art 8(1) entirely, so no legal bar on any of these. Each
		// permission is still listed so a reader can see exactly what an adult
		// gets.
		profile.m_ProfileVisibilityPublic = true;
		profile.m_DirectMessagesFromStrangers = true;
		profile.m_FreeTextChat = true;
		profile.m_VoiceChat = true;
		profile.m_UserGeneratedContentUpload = true;
		profile.m_MarketplaceAccess = true;
		profile.m_LocationSharing = true;
		profile.m_NightHoursRestricted = false;
		// A concrete cap rather than unlimited. The comment above explains why
		// this one stays non-default: it is now an anti-abuse measure rather than
		// a child-safety one, and it belongs to the studio to raise.
		profile.m_FriendRequestLimit = 200;
		break;

	// THE branch this module exists to get right, per the comment. Identical
	// behaviour to Under13 with no assignment, so it falls to the member
	// defaults -- the same code path, which is what makes "fail closed"
	// structural rather than a decision repeated in two places.
	case AgeBand::Unknown:
		// No age signal received. Fail closed -- identical to Under13.
		//
		// This is the branch the whole module exists to get right. Treating
		// Unknown as adult would hand every unverified player the full adult
		// feature set, which is the exact failure the EU and US instruments
		// were written to prevent. A studio that wants a different policy for
		// unknown players passes AgeBand::Unknown through its own layer, and
		// is visible in its own code review.
		break;

	// DEFECT (silent handling, documented not fixed): an unrecognised band --
	// a newer provider build or corrupt data -- is treated exactly like
	// Unknown, and NOTHING LOGS IT. The player silently gets the Under13
	// profile with no diagnostic anywhere, so a version-skew bug between
	// the provider and the engine presents as "the child cannot use chat"
	// rather than as the data error it is. A WL_CORE_ERROR here would have
	// cost one line and made this debuggable. Not fixed -- this pass is
	// comments-only.
	default:
		// An unrecognised band -- a newer provider build, or corrupt data.
		// Same reasoning as Unknown: the engine cannot interpret the value,
		// so it must not interpret it permissively. Falls through to the
		// restrictive member defaults.
		break;
	}

	// Returned by value. Every caller gets its own copy, so a caller cannot
	// mutate the profile another caller sees -- which matters because this
	// is a compliance decision, not a shared setting.
	return profile;
}

} // namespace Wasteland::Age