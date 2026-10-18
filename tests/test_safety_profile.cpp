// Standalone functional test for Wasteland::Age::SafetyProfile.
//
// SafetyProfile is the module's fail-closed safety net: the settings a child
// gets regardless of what the game would otherwise allow. It is deliberately
// NOT derived from a jurisdiction or an assurance level -- it is derived from
// the age band alone, and it fails closed.
//
// Legal basis, against the EU KIDS Act proposal (COM(2026) 681,
// 2026/0286(COD)):
//
//   Art 8(1)  Providers of online platforms ACCESSIBLE TO CHILDREN shall put
//             in place appropriate and proportionate measures to ensure a
//             high level of privacy, safety and security of minors, and in
//             particular shall ensure, by default:
//               (a) profiles are not publicly visible;
//               (b) the function allowing a user to be contacted by strangers
//                   is deactivated;
//               (c) chat functionality is effectively disabled;
//               (d) friend requests require confirmation from the recipient;
//               (e) location sharing is deactivated;
//               (f) the design is age-appropriate.
//
//   Art 20   The assurance solution must be easy to use, accessible and
//             activate-able, and the resulting settings must be tailored to
//             the age of the user.
//
// Note Art 8(1) is written as "by default" -- it is a FLOOR, not a ceiling.
// A studio may go further (and must, for COPPA-covered data) but the engine
// never treats it as permission to relax.
//
// The two properties this suite exists to defend:
//
//   1. Unknown never resolves to adult. A missing age signal produces the
//      most restrictive profile, not a default-adult one.
//   2. Reporting and muting are never gated. They are safety tools, not
//      content features. Art 20 requires them to be easy to use and
//      activate-able for every age -- including a child who is not permitted
//      to chat at all. A child with ReportAvailable() == false is the worst
//      bug this module could ship.

#include <cstdio>
#include <type_traits>

#include "Wasteland/Age/AgeBand.h"
#include "Wasteland/Age/SafetyProfile.h"

using namespace Wasteland::Age;

static int g_Failures = 0;
static int g_Checks = 0;

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

// Bands in increasing permissiveness order. Adult18 is last; Unknown is
// handled separately because it is not a position on this ladder at all.
static const AgeBand kLadder[] = {AgeBand::Under13, AgeBand::Age13To15, AgeBand::Age16To17, AgeBand::Adult18};

static void TestUnknownIsMostRestrictive()
{
    // The single most important property in the module. Unknown means no age
    // signal was received. Treating that as adult hands a minor every
    // unrestricted feature in the game.
    const SafetyProfile unknown = ProfileFor(AgeBand::Unknown);

    CHECK(!unknown.FreeTextChat(), "Unknown: free-text chat must be off");
    CHECK(!unknown.VoiceChat(), "Unknown: voice chat must be off");
    CHECK(!unknown.UserGeneratedContentUpload(), "Unknown: UGC upload must be off");
    CHECK(!unknown.MarketplaceAccess(), "Unknown: marketplace must be off");
    CHECK(!unknown.LocationSharing(), "Unknown: location sharing must be off");
    CHECK(!unknown.ProfileVisibilityPublic(), "Unknown: profile must not be public (Art 8(1)(a))");
    CHECK(!unknown.DirectMessagesFromStrangers(), "Unknown: DMs from strangers must be off (Art 8(1)(b))");
    // NOTE the positive assertion: NightHoursRestricted is a PROTECTIVE flag,
    // so the restrictive state is TRUE. Every flag above is a permission
    // flag, where the restrictive state is FALSE. Getting this polarity
    // backwards is the bug this suite exists to catch.
    CHECK(unknown.NightHoursRestricted(), "Unknown: night-hour restrictions must hold");
    CHECK(unknown.FriendRequestLimit() == 0, "Unknown: no friend requests permitted");
}

static void TestUnknownMatchesMostRestrictiveBand()
{
    // Unknown must be exactly as restrictive as the most restricted known
    // band -- not merely "restricted-ish". If a future edit gives Unknown its
    // own looser row in the lookup, this fails.
    const SafetyProfile unknown = ProfileFor(AgeBand::Unknown);
    const SafetyProfile under13 = ProfileFor(AgeBand::Under13);

    CHECK(unknown.FreeTextChat() == under13.FreeTextChat(), "Unknown must match Under13 on chat");
    CHECK(unknown.VoiceChat() == under13.VoiceChat(), "Unknown must match Under13 on voice");
    CHECK(unknown.UserGeneratedContentUpload() == under13.UserGeneratedContentUpload(),
          "Unknown must match Under13 on UGC upload");
    CHECK(unknown.MarketplaceAccess() == under13.MarketplaceAccess(), "Unknown must match Under13 on marketplace");
    CHECK(unknown.ProfileVisibilityPublic() == under13.ProfileVisibilityPublic(),
          "Unknown must match Under13 on profile visibility");
    CHECK(unknown.DirectMessagesFromStrangers() == under13.DirectMessagesFromStrangers(),
          "Unknown must match Under13 on DMs from strangers");
    CHECK(unknown.LocationSharing() == under13.LocationSharing(), "Unknown must match Under13 on location sharing");
    CHECK(unknown.NightHoursRestricted() == under13.NightHoursRestricted(), "Unknown must match Under13 on night hours");
    CHECK(unknown.FriendRequestLimit() == under13.FriendRequestLimit(),
          "Unknown must match Under13 on the friend request limit");
}

static void TestSafetyToolsAreNeverGated()
{
    // Art 20 requires the assurance-derived settings to be easy to use and
    // activate-able, at every age. A child who cannot report or mute has no
    // way out of a harmful interaction, which is precisely the situation
    // SafetyProfile exists to constrain.
    const AgeBand kEveryBandIncludingUnknown[] = {
        AgeBand::Unknown,
        AgeBand::Under13,
        AgeBand::Age13To15,
        AgeBand::Age16To17,
        AgeBand::Adult18,
    };

    for (AgeBand band : kEveryBandIncludingUnknown)
    {
        const SafetyProfile profile = ProfileFor(band);
        CHECK(profile.ReportAvailable(), "Art 20: reporting must be available at every band, including Unknown");
        CHECK(profile.MuteAvailable(), "Art 20: muting must be available at every band, including Unknown");
    }
    ++g_Checks;
}

static void TestFriendRequestsAlwaysRequireConfirmation()
{
    // Art 8(1)(d). Note this is a protective flag: the safe state is TRUE.
    // It stays true even for adults, where it stops being a legal requirement
    // and becomes a one-line anti-abuse default. Relaxing it is a deliberate
    // studio decision, not something the age ladder should do implicitly.
    for (AgeBand band : {AgeBand::Unknown, AgeBand::Under13, AgeBand::Age13To15, AgeBand::Age16To17, AgeBand::Adult18})
        CHECK(ProfileFor(band).FriendRequestsRequireConfirmation(),
              "Art 8(1)(d): friend requests must always require recipient confirmation");
}

static void TestChildBandsAreLockedDown()
{
    // Art 8(1)(c) says chat functionality is effectively disabled by default
    // for children. Both child bands must therefore have no free text chat.
    for (AgeBand band : {AgeBand::Under13, AgeBand::Age13To15})
    {
        const SafetyProfile profile = ProfileFor(band);
        CHECK(!profile.FreeTextChat(), "Art 8(1)(c): children must have no free-text chat");
        CHECK(!profile.UserGeneratedContentUpload(), "Art 8(1): children must not upload UGC");
        CHECK(!profile.MarketplaceAccess(), "Art 8(1): children must have no marketplace access");
        CHECK(!profile.ProfileVisibilityPublic(), "Art 8(1)(a): children must not be publicly visible");
        CHECK(!profile.DirectMessagesFromStrangers(), "Art 8(1)(b): children must not accept DMs from strangers");
        CHECK(!profile.LocationSharing(), "Art 8(1)(e): children must not share location");
        CHECK(profile.NightHoursRestricted(), "Art 8(1)(f)/20: children must have age-appropriate night restrictions");
    }
}

static void TestLadderIsMonotonic()
{
    // Every relaxed permission must open as the band rises, and never close.
    // A non-monotonic ladder means the same player could gain a feature by
    // being re-evaluated, which is not a policy anyone can reason about.
    for (size_t i = 1; i < sizeof(kLadder) / sizeof(kLadder[0]); ++i)
    {
        const SafetyProfile younger = ProfileFor(kLadder[i - 1]);
        const SafetyProfile older = ProfileFor(kLadder[i]);

        CHECK(!younger.FreeTextChat() || older.FreeTextChat(), "monotonic: free-text chat");
        CHECK(!younger.VoiceChat() || older.VoiceChat(), "monotonic: voice chat");
        CHECK(!younger.UserGeneratedContentUpload() || older.UserGeneratedContentUpload(), "monotonic: UGC upload");
        CHECK(!younger.MarketplaceAccess() || older.MarketplaceAccess(), "monotonic: marketplace");
        CHECK(!younger.ProfileVisibilityPublic() || older.ProfileVisibilityPublic(), "monotonic: profile visibility");
        CHECK(!younger.DirectMessagesFromStrangers() || older.DirectMessagesFromStrangers(), "monotonic: DMs from strangers");
        CHECK(!younger.LocationSharing() || older.LocationSharing(), "monotonic: location sharing");
        // NightHoursRestricted is a PROTECTIVE flag: TRUE is the restrictive
        // state, the opposite polarity from every permission flag above. So
        // monotonic relaxation means "once a band lifts its restriction, the
        // bands above it have lifted theirs too" -- never the reverse.
        CHECK(!older.NightHoursRestricted() || younger.NightHoursRestricted(), "monotonic: night hours");

        CHECK(older.FriendRequestLimit() >= younger.FriendRequestLimit(), "monotonic: friend request limit");
    }
    ++g_Checks;
}

static void TestAdultGetsAdultFeatures()
{
    // Not a legal requirement -- an adult is outside Art 8(1). This exists so
    // that a bug which locks EVERYONE down still fails visibly instead of
    // silently shipping a game where verified adults cannot chat or trade.
    const SafetyProfile adult = ProfileFor(AgeBand::Adult18);

    CHECK(adult.FreeTextChat(), "Adult18: free-text chat must be available");
    CHECK(adult.VoiceChat(), "Adult18: voice chat must be available");
    CHECK(adult.MarketplaceAccess(), "Adult18: marketplace must be available");
    CHECK(!adult.NightHoursRestricted(), "Adult18: night-hour restrictions must be lifted");
    CHECK(adult.FriendRequestLimit() > 0, "Adult18: friend requests must be permitted");
}

static void TestProfileIsNotAssignable()
{
    // SafetyProfile is a runtime read-only query, not configuration. If a
    // permissive value could be written back into it -- by a settings file, a
    // mod, or a careless call site -- the Art 8(1) defaults would be
    // overridable at runtime, which is the whole thing this type prevents.
    //
    // A compile-time assertion: if this stops compiling, the type gained an
    	// assignment operator and the guarantee is gone. Both overloads are
    	// checked -- copy and move assignment are separate declarations, and
    	// blocking only one would leave the other as a writable surface.
    	static_assert(!std::is_assignable<SafetyProfile &, SafetyProfile>::value,
    	              "SafetyProfile must not be copy-assignable: Art 8(1) defaults are not runtime-writable configuration");
    	static_assert(!std::is_assignable<SafetyProfile &, SafetyProfile &&>::value,
    	              "SafetyProfile must not be move-assignable: Art 8(1) defaults are not runtime-writable configuration");
    	static_assert(std::is_copy_constructible<SafetyProfile>::value,
    	              "SafetyProfile must be copy-constructible so it can be returned by value");
    	++g_Checks;
}

int main()
{
    TestUnknownIsMostRestrictive();
    TestUnknownMatchesMostRestrictiveBand();
    TestSafetyToolsAreNeverGated();
    TestFriendRequestsAlwaysRequireConfirmation();
    TestChildBandsAreLockedDown();
    TestLadderIsMonotonic();
    TestAdultGetsAdultFeatures();
    TestProfileIsNotAssignable();

    std::printf("test_safety_profile: %d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}