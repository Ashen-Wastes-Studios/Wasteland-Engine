// Wasteland::Age -- jurisdiction policy layer.
//
// Answers one question: is a given age-assurance method good enough for a
// given kind of surface, in a given jurisdiction? Everything else in the
// module either produces an assurance level or consumes this answer.
//
// Legal basis, against the EU KIDS Act proposal (COM(2026) 681,
// 2026/0286(COD)):
//
//   Art 3(i)  "age assurance" EXPRESSLY EXCLUDES self-declaration. A
//             date-of-birth box is therefore not age assurance at all, and
//             no surface may be satisfied by one.
//   Art 29(2) Providers under Art 6 -- social networking and video-sharing
//             -- shall rely EXCLUSIVELY on an age verification solution
//             certified under the EU Age Verification Scheme. No
//             alternatives are permitted.
//   Art 29(4) Providers under Art 8(1) and 16(2)-(3) -- online games, AI
//             companions, chatbots, app stores -- MAY use other age
//             assurance solutions provided they meet Art 27 and Art 28.
//
// The Art 29(2)/29(4) split is the single most important thing this header
// encodes. It is the difference between a game studio being locked to a
// specific EU certificate and being free to procure Yoti, Persona or an
// equivalent -- so getting it wrong is expensive in both directions.
//
// Neither instrument is in force as of this writing. Art 29(2) has a narrow
// account-level carve-out in Art 28(4). See docs/AGE_ASSURANCE.md.

// Header guard. Note this file has NO includes at all -- the whole
// policy layer is pure enums plus two free functions over them, so it
// can be included anywhere without a dependency.
#pragma once

// Nested C++17-style namespace. Modern compilers treat this identically
// to the nested braces form, and it keeps the closing brace from needing a
// matching name later.
namespace Wasteland::Age
{

// Strength of an assurance method, ordered weakest to strongest.
//
// The ordering is load-bearing: IsSufficient compares ordinals, and
// TestMonotonicity in tests/test_policy.cpp asserts the ladder never
// inverts. Reordering these values silently changes every policy decision,
// so they are explicit rather than relying on declaration order.
// Explicit : int so the ordinal survives serialisation and can cross the
// Python binding as a plain integer. Values are assigned one by one
// rather than relying on declaration order -- reordering them would
// silently change every policy decision, because IsSufficient below
// compares ordinals.
enum class AssuranceLevel : int
{
    // A date-of-birth box or an in-game age question. EXCLUDED from "age
    // assurance" by Art 3(i), and never sufficient on any surface. The
    // enum value is retained so a provider can report it honestly and have
    // the refusal be explicit rather than a silent downgrade.
    // ZERO, not a sentinel like -1. That is deliberate: it is a legitimate
    // reportable outcome ("the only thing we have is a self-declaration"),
    // and it must be expressible so a provider can be honest about it and
    // the refusal be explicit. It is the weakest rung, so any >= comparison
    // rejects it -- Art 3(i) is encoded in the ordering, not in a special
    // case.
    SelfDeclared = 0,

    // A platform or store signal: Apple Declared Age Range, Google Play Age
    // Signals. The store performs the verification; the developer consumes
    // the result.
    // The store did the verification and the developer consumes the result.
    // Modelled as an ordinal rather than a capability set because the
    // only question asked of a level anywhere in this header is "is this at
    // least as strong as X".
    OsSignal = 1,

    // A third-party identity or age-estimation vendor, e.g. Yoti or Persona,
    // returning a signed attestation. Permitted for Art 29(4) surfaces.
    // A third-party vendor. Permitted for Art 29(4) surfaces, never sufficient
    // for Art 29(2) ones -- which is enforced by MinimumLevelFor below, not
    // by anything in this enumerator.
    Commercial = 2,

    // An attestation issued under the EU Age Verification Scheme. The only
    // method permitted for Art 29(2) surfaces in EU scope.
    // The top of the ladder and the only value a locked-down surface accepts.
    // Also the value returned for an unrecognised SurfaceKind, which makes
    // it double as the fail-closed default -- a convenient overlap, since
    // the most restrictive level is the right answer for "I do not know".
    EuEaa = 3,
};

// What a surface does. The category decides which article of the proposal
// governs it, and therefore which methods are acceptable.
// Five categories, each mapped to one article of the proposal. The article
// numbers are recorded in the comments below because that mapping is
// the entire content of the enum.
enum class SurfaceKind : int
{
    // Art 15: online games. Art 29(4) latitude applies.
    // Art 15, Art 29(4) latitude: other assurance methods are allowed. Being
            // the DEFAULT (value zero) is a hazard worth knowing -- a
    // zero-initialised SurfaceKind is a game surface, which happens to be
    // the most permissive classification, not the safest one.
    OnlineGame = 0,

    // Art 14: AI companions and chatbots, including in-game NPCs backed by
    // a model. Art 29(4) latitude applies -- worth noting, because an AI
    // chatbot is exactly the surface a studio might assume is locked down.
    // Art 14, and Art 29(4) latitude applies. The comment above flags the
    // trap this is here to prevent: an AI chatbot is a surface a studio
    // would reasonably assume is locked to the EU Scheme, and it is not.
    AiChatbot = 1,

    // Art 16: app stores. Art 29(4) latitude applies.
    // Art 16, Art 29(4) latitude.
    AppStore = 2,

    // Art 6: social networking. Art 29(2) -- EU Scheme only.
    // Art 6, Art 29(2): EU Scheme ONLY. The last value in the enum that has
    // latitude, so the two locked categories are the two highest values --
    // no code depends on that ordering, but it is worth being aware of
    // because MinimumLevelFor switches on the value.
    SocialNetwork = 3,

    // Art 6: video-sharing. Art 29(2) -- EU Scheme only.
    // Art 6, Art 29(2). The highest value, so it is what an out-of-range
    // integer cast into this enum most likely resembles.
    VideoSharing = 4,
};

// Which rule set applies.
//
// This is NOT a licence-to-weaken switch. A US-only build is outside the EU
// proposal's scope, so the EU Scheme floor genuinely does not bind it --
// but what does bind it is the App Store Accountability Acts, under which
// the platform store performs verification and shares the age range for the
// developer to consume. Hence OsSignal as the US floor rather than nothing.
//
// SafetyProfile defaults are unaffected by this choice. Scope selects which
// rules are consulted; it never switches a safeguard off. See
// AgeService::SetEuScope.
// Which rule SET applies -- emphatically not a licence-to-weaken switch.
// The distinction is load-bearing for compliance: scope selects the
// rules consulted, and never disables a safety default. See
// AgeService::SetEuScope, which is where that separation is enforced.
enum class Jurisdiction : int
{
    // ZERO again, and again the more restrictive default: an uninitialised
    // Jurisdiction is EU scope, so a caller that forgets to set it gets
    // the stricter rule set rather than the looser one. Fail-closed by
    // construction.
    Eu = 0,
    // Still not "no floor": the App Store Accountability Acts bind, and the
    // store shares an age range, which is exactly what OsSignal models. So
    // MinimumLevelFor returns OsSignal here rather than SelfDeclared --
    // nothing is genuinely exempt.
    UnitedStates = 1,
};

// The weakest assurance level that satisfies `surface` under `jurisdiction`.
//
// Fails closed. An unrecognised SurfaceKind returns EuEaa rather than a
// permissive default: a value persisted by an older build, or cast in from
// corrupt data, must not be able to talk the engine into accepting a
// commercial assurance for a surface the law would have locked to the EU
// Scheme.
// A FREE FUNCTION in a header, not a method: it is a pure table lookup with
// no state to carry, which makes it trivially testable -- the
// monotonicity and fail-closed assertions in the test suite call it
// directly.
//
// DEFECT (immutability of law, documented not fixed): the result is
// computed per call from a hard-coded table. If an instrument changes,
// this function is the one place that must change with it, and nothing
// in the code marks which literals are legal thresholds as opposed to
// arbitrary values -- the article references live only in the comments
// on the enumerators above.
AssuranceLevel MinimumLevelFor(SurfaceKind surface, Jurisdiction jurisdiction);

// True when `provided` is at least as strong as the minimum required.
//
// `provided` is an AssuranceLevel rather than a boolean so that "we have no
// signal at all" and "the signal says the player is a minor" remain
// distinguishable at the call site -- collapsing them into a bool is how a
// missing signal turns into an adult.
//
// Fails closed for unrecognised inputs on either argument.
// Ordinal comparison, per the comment at the enum: `provided >= minimum`.
// Two fail-closed properties are documented here and are the reason
// this is the ONLY place a level is judged -- MinimumLevelFor cannot
// be called with a level, so the two questions cannot be confused.
//
// DEFECT (no error channel, documented not fixed): both failures -- an
// unrecognised surface and an unrecognised jurisdiction -- return the
// same false. A caller cannot tell a genuinely insufficient assurance
// level from a corrupt enum value, so a debugging session has to inspect
// the enum inputs directly. The header says "fails closed for
// unrecognised inputs on either argument" without distinguishing them.
bool IsSufficient(SurfaceKind surface, AssuranceLevel provided, Jurisdiction jurisdiction);

} // namespace Wasteland::Age