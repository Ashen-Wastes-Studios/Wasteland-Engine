"""Age gating for gameplay, communication and monetisation.

A worked example for the engine's Wasteland.Age module. Copy it, or ignore it
and call Wasteland.Age directly -- it is a naming layer over a handful of
calls, in the same spirit as GamepadBindings.py.

READ THIS BEFORE ASSEMBLING: the module does not make your game compliant.

Wasteland.Age supplies defaults, policy and tooling. You still have to call it
at every point where a feature is exposed -- opening chat, showing a social
feed, starting a purchase, admitting a stranger's friend request. A game that
links the module and never calls it has the same posture as one that does not
have it. There is no way around that from inside an engine, because only your
game knows where its features are.

THE MISMATCH THAT BITES PEOPLE

Script instances are PER ENTITY. AgeService is PROCESS-WIDE. So a script that
reads Wasteland.Age.Profile() inside a per-entity update runs once per entity
per frame -- a hundred NPCs means a hundred identical reads of a value that
changed once.

So this module reads ONCE at import and caches. If your game genuinely has
per-account age (a shared family console, say), do not use this cache; build
your own AgeService per account. See docs/AGE_ASSURANCE.md.

EVERYTHING HERE FAILS CLOSED

If there is no age signal, these helpers return the restrictive answer and say
so. None of them raise, and none of them return None -- a script that forgets
to check a None will usually find None falsy and carry on, which is exactly
the wrong direction.
"""

import Wasteland

Age = Wasteland.Age

# --------------------------------------------------------------------------
# One read, at import. See the note above on why this is cached.
# --------------------------------------------------------------------------

_PROFILE = Age.Profile()
_POLICY = Age.Policy()

# True when we have no usable age signal at all: no provider, provider
# unavailable, or a band this engine does not recognise. The module treats
# every one of those as a minor, and so should your game -- but knowing it is
# different from treating it as a minor, because it changes what you TELL the
# player and whether you should be nagging them about verification.
UNRESOLVED = Age.Band() == Age.AgeBand.Unknown

IS_MINOR = Age.IsMinor()
IS_VERIFIED = Age.IsVerified()


# --------------------------------------------------------------------------
# Feature gates. One call each; each reads as the thing it gates.
# --------------------------------------------------------------------------

def can_chat():
    """Free text chat. False for everyone under 18."""
    return _PROFILE.FreeTextChat


def can_use_voice():
    """Voice chat. Tighter than text: Under13 and 13-15 both lose it."""
    return _PROFILE.VoiceChat


def can_upload_ugc():
    """Levels, skins, mods, uploaded commentary."""
    return _PROFILE.UserGeneratedContentUpload


def can_open_marketplace():
    """Buying AND selling. Read-only storefronts are not covered by this."""
    return _PROFILE.MarketplaceAccess


def can_share_location():
    return _PROFILE.LocationSharing


def profile_is_public():
    """Whether the player's profile defaults to visible."""
    return _PROFILE.ProfileVisibilityPublic


def stranger_requests_need_confirming():
    """Always true here, including for adults -- anti-abuse, not age gating.

    Worth keeping in mind before treating a False as "adults have no such
    restriction". They do not; this one stays on for everybody.
    """
    return _PROFILE.FriendRequestsRequireConfirmation


# --------------------------------------------------------------------------
# Art 20(2) rights. ALWAYS available, in every state.
# --------------------------------------------------------------------------

# These are not gates and must never become gates. They are the player's tools
# for acting on harm, and a child is the person who most needs the button. The
# engine reports them as available in every state for exactly that reason.
#
# They are surfaced here so that a grep for "what does the age module stop"
# returns "not this" -- and so that a studio wiring moderation can see the
# requirement without reading JurisdictionPolicy.
REPORT_AVAILABLE = _PROFILE.ReportAvailable
MUTE_AVAILABLE = _PROFILE.MuteAvailable


# --------------------------------------------------------------------------
# Guardian state, for UI.
# --------------------------------------------------------------------------

def guardian_state():
    """Unconfigured | MinorNeedsSetup | Minor | Adult | PendingReview.

    GuardianState.Unconfigured means protections are ON but nobody has set them
    up. That is a state your UI must show as "on", not as "off" -- the two look
    identical if you map this enum to a checkbox without asking which one you
    are showing.
    """
    return _POLICY.State


def protections_enforced():
    """True whenever Art 8(1) defaults are in force -- including Unconfigured.

    This is the one to bind a persistent indicator to. It is true for a
    configured minor AND for an unresolved signal, because in both cases the
    restrictive settings are the ones running.
    """
    return _POLICY.State != Age.GuardianState.Adult


def effective_playtime_limit():
    """Minutes per day, or Age.UnlimitedPlaytime().

    EFFECTIVE, not the resting setting: while a guardian change is pending this
    returns the stricter of the two. Read _POLICY.Settings for what a guardian
    last chose, and this for what is actually enforced -- they differ during a
    pending review, and using the wrong one shows a limit the player is not
    under.
    """
    return _POLICY.EffectiveSettings.DailyPlaytimeMinutes


def change_awaiting_review():
    """True while a proposed guardian change is waiting.

    While this is true the ENFORCED settings are the more restrictive
    combination, so a pending relaxation has had no effect yet. Your UI should
    say so rather than looking like the switch failed.
    """
    return _POLICY.HasPendingChange


# --------------------------------------------------------------------------
# The gate, for surfaces the law governs.
# --------------------------------------------------------------------------

def authorize(surface):
    """Record a decision and report whether it is permitted.

    Pass a Wasteland.Age.SurfaceKind. Returns False unless the assurance is
    sufficient for that surface AND the decision could be recorded -- an
    unaudited decision is not a permitted one.

    Use this for social networking, video sharing and anywhere you are not yet
    sure a plain profile check is the right test. It is slower than the direct
    property reads above and it writes to the audit log, so prefer those where
    they answer the question.
    """
    return Age.AuthorizeFeature(surface)


# --------------------------------------------------------------------------
# Refresh, after a provider flow.
# --------------------------------------------------------------------------

def refresh():
    """Re-read the age signal after an OS or vendor flow completes.

    BLOCKING. Call it when a flow finishes, never per frame.

    Note this does not update the module-level cache above. If your game runs
    an age-assurance flow mid-session, reload this module (or call Age.Profile()
    directly at your call sites) -- a cached snapshot that survives a refreshed
    signal is a stale safety decision, which is the one bug in this file that
    would actually matter.
    """
    Age.Refresh()
