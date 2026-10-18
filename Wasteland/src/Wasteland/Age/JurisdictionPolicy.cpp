#include "wlpch.h"

#include "Wasteland/Age/JurisdictionPolicy.h"

namespace Wasteland::Age
{

// Anonymous namespace: internal linkage, so these three helpers cannot collide
// with a same-named function elsewhere and need no header entry.
namespace
{

// Art 6 surfaces -- social networking and video-sharing.
//
// Art 29(2) is exclusive: these providers SHALL rely exclusively on a
// solution certified under the EU Age Verification Scheme. A commercial
// attestation, however strong, does not satisfy them in EU scope.
// A two-way OR rather than a set test: a bitmask would be more code for the same
// answer and would have to be kept in step with the enum by hand.
bool IsArt6Surface(const SurfaceKind surface)
{
    // Named `surface`: the switch below proves the enum has five enumerators, so
    // this cannot drift out of sync the way a hardcoded list would.
    return surface == SurfaceKind::SocialNetwork || surface == SurfaceKind::VideoSharing;
}

// Guards the fail-closed path.
//
// Without this, an unrecognised SurfaceKind simply is not an Art 6 surface,
// and the function falls through to the Art 29(4) floor of Commercial --
// i.e. an unknown surface would be MORE permissive than a known one. That is
// the exact inversion the policy exists to prevent, so an unrecognised value
// must resolve to the strongest rule rather than falling through.
// DEFECT (redundancy): this lists all five SurfaceKind values, including both
// IsArt6Surface cases, so for every value this build knows it returns true and
// only an out-of-range or future enumerator makes it false.
//
// Not a bug -- `default:` handles corrupt and newer values -- but it means the
// guard "can never be false" for known surfaces, which reads like dead code.
// Noted so nobody deletes it on that reasoning.
bool IsKnownSurface(const SurfaceKind surface)
{
    // No `default:` case returning true, and that is the whole point of the function.
    // An unrecognised surface must NOT be treated as known -- see the header note --
    // so `default:` falls out to `return false` below and the caller resolves it to
    // the strictest rule.
    switch (surface)
    {
    // All five listed explicitly, so a sixth SurfaceKind is a compile-time obligation
    // here rather than a silent fallthrough to `default:`.
    case SurfaceKind::OnlineGame:
    case SurfaceKind::AiChatbot:
    case SurfaceKind::AppStore:
    case SurfaceKind::SocialNetwork:
    case SurfaceKind::VideoSharing:
        return true;
    default:
        return false;
    }
}

// An unrecognised Jurisdiction resolves to the EU rules, which are the
// strictest. Same reasoning as IsKnownSurface: a value the engine cannot
// interpret must not select the more permissive branch.
// The jurisdiction analogue of IsKnownSurface: unknown -> EU rules, strictest.
bool IsEuOrUnknown(const Jurisdiction jurisdiction)
{
    // A switch rather than `jurisdiction == Jurisdiction::Eu`: a new enumerator
    // lands in `default:` and gets EU rules, where a comparison would silently
    // return false and select US rules for a value nobody has reasoned about.
    switch (jurisdiction)
    {
    // Spelled out so the switch is exhaustive over what exists today.
    case Jurisdiction::Eu:
        return true;
    case Jurisdiction::UnitedStates:
        return false;
    default:
        return true;
    }
}

} // namespace

// The module's main entry point: the weakest assurance satisfying this surface
// in this jurisdiction. Every other Age question about strength routes here.
AssuranceLevel MinimumLevelFor(const SurfaceKind surface, const Jurisdiction jurisdiction)
{
    // Fail closed FIRST: every branch below assumes a known surface, so an unknown
    // one must never reach them. EuEaa -- the strongest level -- because the failure
    // mode of a wrong floor is admitting a minor to an adult surface.
    // Fail closed, before any other reasoning. Everything below assumes a
    // known surface.
    // The literal rather than "the strictest thing we would otherwise have said",
    // so the intent is legible where it matters: nothing passes but a certified EU
    // attestation.
    //
    // Cost, stated honestly: a surface id persisted by an older build and read back
    // by a newer engine refuses everything rather than mis-classifying. Intended.
    if (!IsKnownSurface(surface))
        return AssuranceLevel::EuEaa;

    // Independent axes: the surface decides which ARTICLE governs it, the
    // jurisdiction which RULE SET applies.
    //
    // AgeService passes EuScope() ? Eu : UnitedStates, so a studio that never calls
    // SetEuScope gets EU rules -- a build shipping into the EU cannot fall outside
    // the proposal by forgetting to opt in.
    const bool euRules = IsEuOrUnknown(jurisdiction);

    // The Art 6 branch: two surfaces, two answers, split only by jurisdiction --
    // the asymmetry the header spends its first screen on.
    if (IsArt6Surface(surface))
    {
        // EU scope: the EU Scheme and nothing else.
        //
        // US scope: no US federal rule requires a developer to obtain a
        // specific certificate -- the App Store Accountability Acts put the
        // verification duty on the platform store, which shares the result
        // with the app. So the floor is the store signal, not EuEaa.
        // The US-scope case for an Art 6 surface; the comment above is the legal
        // reasoning. Note what is NOT claimed: OsSignal here does not mean the developer
        // did nothing -- it means the platform performed the check.
        //
        // The return below is the EU half: Art 29(2) is exclusive, so it is the ONLY
        // accepted method for these surfaces in EU scope. No fallback exists beneath it,
        // and the return type cannot express "strong enough but not permitted".
        if (!euRules)
            return AssuranceLevel::OsSignal;

        return AssuranceLevel::EuEaa;
    }

    // Art 29(4) surfaces -- games, AI chatbots, app stores.
    //
    // Commercial assurance is the floor, never SelfDeclared: Art 29(4)'s
    // latitude covers "other age assurance solutions", and Art 3(i) has
    // already excluded self-declaration from that category entirely.
    //
    // In US scope a store signal is enough on its own, since no federal rule
    // requires the developer to run a second check the store has already run.
    if (!euRules)
        return AssuranceLevel::OsSignal;

    // The branch above returns OsSignal for US scope -- the same value the Art 6
    // branch returns, for a different reason: no federal rule requires a second check
    // the store already ran. Two separate returns rather than a shared variable,
    // because these rules coincide today and may not stay that way.
    //
    // Commercial as the EU floor for Art 29(4) surfaces: a vendor attestation is
    // permitted, an OS signal is not sufficient alone, and SelfDeclared is not on
    // the scale at all (Art 3(i)).
    return AssuranceLevel::Commercial;
}

// Where MinimumLevelFor answers "what is required", this answers "is it enough".
// Separate so AgeAuditLog::AuthorizeFeature can record BOTH in one entry.
bool IsSufficient(const SurfaceKind surface, const AssuranceLevel provided, const Jurisdiction jurisdiction)
{
    // The unknown-level guard runs before anything else, including the self-declared
    // check below. An unrecognised AssuranceLevel must not reach the ordinal
    // comparison, where it would compare as a large or small integer by accident and
    // could land on either side of the floor.
    // An unrecognised assurance level must not read as "strong enough". This
    // is the missing-signal case: a provider build newer than the engine, or
    // corrupt data. Refusing is correct -- an engine that cannot understand a
    // method cannot certify it meets Art 27.
    // The four known levels with an empty body, purely to VALIDATE the enum; `break`
    // rather than a return because the real answer comes after.
    //
    // DEFECT (maintenance trap): `default:` also catches any enumerator added to
    // AssuranceLevel later, so a future stronger level would be rejected as
    // unrecognised until this switch is updated. Fail-closed, but it would surface in
    // the monotonicity test rather than anywhere obvious. Not fixed; comments only.
    switch (provided)
    {
    // All four fall through to the checks below. SelfDeclared is listed here on
    // purpose: it is a VALID level, never a sufficient one. Rejecting it here would
    // conflate "I do not understand this value" with "I understand it and it is not
    // enough" -- a distinction the audit log needs to report SelfDeclared.
    case AssuranceLevel::SelfDeclared:
    case AssuranceLevel::OsSignal:
    case AssuranceLevel::Commercial:
    case AssuranceLevel::EuEaa:
        break;
    // Everything unrecognised: false rather than falling through to the comparison.
    default:
        return false;
    }

    // SelfDeclared is below every floor the module will ever report, because
    // Art 3(i) excludes it from age assurance. Checked explicitly anyway so
    // that a future edit lowering a floor below SelfDeclared cannot silently
    // re-admit it.
    // DEFECT (deliberate redundancy): SelfDeclared is already below every floor
    // MinimumLevelFor can return, so the comparison on the next line would reject it
    // anyway. This exists so that lowering a floor below SelfDeclared cannot silently
    // re-admit it -- it looks like dead code and is not.
    //
    // It also keeps Art 3(i) in the comparison function rather than only in the audit
    // log, so a direct IsSufficient caller still cannot be satisfied by a form.
    if (provided == AssuranceLevel::SelfDeclared)
        return false;

    // The ordinal comparison, and the only place the enum ORDER is load-bearing:
    // SelfDeclared=0 < OsSignal=1 < Commercial=2 < EuEaa=3 is what makes `>=` right.
    // Reordering the enum would silently invert every policy decision in the module.
    //
    // Both operands are cast to int: scoped enums have no relational operators, so
    // the casts are required rather than stylistic -- and they make the ordering
    // dependency explicit at the point of use.
    return static_cast<int>(provided) >= static_cast<int>(MinimumLevelFor(surface, jurisdiction));
}

} // namespace Wasteland::Age