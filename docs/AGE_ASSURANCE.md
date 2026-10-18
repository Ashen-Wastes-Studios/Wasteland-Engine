# Age Assurance (`Wasteland::Age`)

Engine infrastructure so that games built on Wasteland can meet the child-safety
obligations that apply to them, without every studio re-deriving the same rules
or building the same defaults.

This module makes your game **easier to get right**. It does not make it
compliant. The difference is stated in full under
[What this module cannot do](#what-this-module-cannot-do), and you should read
that section before shipping anything.

---

## Legal status — read this first

**Neither instrument this module anticipates is in force.**

| Instrument | Status |
|---|---|
| EU KIDS Act proposal | COM(2026) 681, procedure 2026/0286(COD). Adopted by the Commission 17 Sept 2026. Still going through Parliament and Council. Not in force. |
| US App Store Accountability Acts | H.R. 7757. Passed the House 29 June 2026 (267–117), referred to Senate Commerce 13 July. See CRS report R47884. |
| COPPA Rule (amended) | In force. Compliance deadline 22 April 2026. |

So this is **readiness work**: the defaults, the seams and the record-keeping
are built now, while the details can still change. Nothing here is legal advice,
and no article reference should be treated as a statement of current
obligation. If you need to know what applies to you today, that is a question
for a lawyer, not for this file.

Sources: [Commission FAQ](https://digital-strategy.ec.europa.eu/en/faqs/kids-act-explained),
[proposal text](https://kidsact.eu/proposal/),
[CRS R47884](https://www.congress.gov/crs_external_products/R/HTML/R47884.html).

---

## What this module cannot do

This is the part that matters most, so it is first.

**It cannot make a game compliant.** It supplies defaults, policy and tooling.
Your game still has to call it at every point where a feature is exposed:
opening chat, showing a social feed, starting a purchase, admitting a stranger's
friend request. A game that links `Wasteland::Age` and never calls it has the
same posture as one that does not have it. No engine can do this part, because
only your game knows where its features are.

**It does not verify anybody.** Verification is a third-party service. The
engine consumes *attestations*; it never sees a date of birth, an identity
document, or a session token, and there is no field in any type here that could
hold one.

**It does not implement the proposal's cryptography.** Art. 28(3) contemplates
zero-knowledge age proofs. The module implements none of that. If you deploy in
the EU you will need a solution certified under the EU Scheme — procuring one
is a studio responsibility, and this module gives you the seam to plug it into.

**It cannot tell you that your age-tiering is adequate.** It decides what a
given band may do. Whether those bands match the ages your audience actually
is, and whether your content is age-appropriate, is a product and legal
judgement the module has no view on.

**The audit log cannot name children.** By design — see
[Privacy](#privacy-by-construction). If a regulator asks what you did to a
*specific* player, the engine cannot answer. That is a deliberate trade, and if
you need per-user trails you must add them at your layer with your own
justification and retention rules.

---

## Quick start

```cpp
#include <Wasteland/Age/AgeService.h>
#include <Wasteland/Age/AgeAuditLog.h>

using namespace Wasteland::Age;

// Startup: install a provider. With none, every default below still applies
// and every answer is the restrictive one.
AgeService::Instance().SetProvider(std::move(myProvider));
AgeService::Instance().Refresh();
```

```cpp
// Anywhere a feature is exposed:
auto profile = AgeService::Instance().Profile();
if (profile.FreeTextChat())
    OpenChat();

// Anywhere the law governs a surface:
if (AgeAuditLog::Instance().AuthorizeFeature(AgeService::Instance(),
                                             SurfaceKind::SocialNetwork))
    OpenSocialFeed();
```

That is the whole integration. Everything below is detail.

---

## The defaults

With **no provider at all** — or an unavailable one, or one that throws — the
service reports:

- `Band() == AgeBand::Unknown`
- `IsMinor() == true`
- `IsVerified() == false`
- the restrictive `SafetyProfile`
- `GuardianState::Unconfigured`

A default-constructed service is **indistinguishable** from one that asked a
provider and got nothing. That is deliberate: a game must not be able to tell
the difference and behave differently.

### Safety ladder

| Band | Chat | Voice | UGC | Marketplace | Stranger DMs | Location |
|---|---|---|---|---|---|---|
| `Unknown` | no | no | no | no | no | no |
| `Under13` | no | no | no | no | no | no |
| `Age13To15` | no | no | no | no | no | no |
| `Age16To17` | **yes** | no | yes | no | no | no |
| `Adult18` | yes | yes | yes | yes | yes | yes |

Always on, for everybody, in every band:

- `ReportAvailable` and `MuteAvailable` (Art 20(2))
- `NightHoursRestricted` for minors
- `FriendRequestsRequireConfirmation` for adults — anti-abuse, not age gating

> **`NightHoursRestricted` has inverted polarity.** `true` is the *restrictive*
> state. Every other profile property is safe when `false`. This is the single
> most common way to misread the profile.

---

## Privacy by construction

No type in this module can hold a user id, a date of birth, an identity
document, an account handle, a session token, or anything else joinable to a
person. That is not a convention — it is enforced by the compiler:

```cpp
static_assert(std::is_trivially_copyable<AgeAuditEntry>::value, ...);
```

`std::string` is not trivially copyable, so `AgeAuditEntry` has nowhere to put
an identity field. Adding one breaks the build.

Consequences you should know about rather than discover:

- **`AgeService::Instance()` is process-wide.** Fine for band-derived data,
  which is identical for every player of a given age. Not fine for a
  shared-family console with per-account profiles — construct your own
  `AgeService` per account instead.
- **The audit log cannot answer per-user questions.** See
  [What this module cannot do](#what-this-module-cannot-do).

---

## Jurisdiction and EU scope

`SetEuScope(false)` narrows the **jurisdiction rules only**.

```cpp
AgeService::Instance().SetEuScope(false);  // US-only build
```

It changes which rules `JurisdictionPolicy` consults. It does **not** relax
`SafetyProfile` or `GuardianPolicy` — Art. 8(1) child safety is not
jurisdiction-conditional, and a US-only build still has COPPA obligations. The
test suite asserts this directly:

> *"EU scope must not be the reason a minor's profile is private."*

EU scope is **on by default**: a build shipping into the EU cannot fall outside
the proposal's scope by forgetting to call anything.

### Assurance levels

Ordered; the ordering is load-bearing.

| Level | Meaning |
|---|---|
| `SelfDeclared` | The player or guardian typed it. **Not age assurance** (Art. 3(i)). Satisfies nothing, anywhere. |
| `OsSignal` | The OS or store said so (Apple Declared Age Range, Google Play Age Signals). |
| `Commercial` | A procured commercial verification service. |
| `EuEaa` | A solution certified under the EU Scheme. |

### Art. 29 split

- **Art. 29(2)** — social networking and video sharing in EU scope require
  `EuEaa`. Nothing less.
- **Art. 29(4)** — games, AI chatbots and app stores may use qualifying
  commercial alternatives.

So a game surface accepts `Commercial` in both scopes, and a social surface
accepts only an OS signal outside EU scope. Both are tested.

---

## Providers

Implement `IAgeAssuranceProvider` (three methods: `IsAvailable`, `Query`,
`ProviderName`) and install it. Two reference implementations ship:

- `AppleAgeSignalProvider` — Declared Age Range. Entitlement
  `com.developer.declared-age-range`.
- `GooglePlayAgeSignalProvider` — Play Age Signals. `TIER_A` is
  `SelfDeclared`; `TIER_B`–`TIER_D` are `OsSignal`.

**Both are compile-guarded and return `SignalAbsent` on desktop**, because the
engine has no iOS or Android backend (`Core.h` rejects both with `#error`). Their
mapping logic and their unavailable-on-desktop behaviour are fully tested; the
native calls are not, because they cannot run here.

> Ranges are mapped by their **lower** bound. This can over-restrict a custom
> range like `[0,14]` — reading the upper bound instead would classify a
> fourteen-year-old as an adult, and failing open is worse than failing
> restrictive.

Amazon `GetUserAgeData` is a documented seam, not an implementation.

---

## Guardian tooling

`GuardianPolicy` is a value-type state machine:

```cpp
auto policy = GuardianPolicy::ForBand(AgeBand::Age13To15);
GuardianSettings proposed = policy.Settings();
proposed.DailyPlaytimeMinutes = 240;

policy = policy.WithPendingChange(proposed);  // returns a NEW policy
if (approved) policy = policy.Approved();     // else policy.Denied()
```

**While a change is pending, the enforced settings are the most restrictive
combination of current and proposed.** If a pending relaxation took effect
immediately, flipping a switch would disable protection for as long as it took
to notice, and *reverting* it would be the act that re-enabled protection — a
review state that briefly lowers the floor is a bypass, not a review.

Tightening takes effect immediately, so a guardian adding protection is not made
to wait.

Two UI bugs this prevents, both of which the view-model tests cover:

- A tick mark must show the **effective** value, not the pending one. Showing
  the pending value makes a relaxed switch look like it worked when nothing
  changed.
- Restriction notes must be derived from the **effective** settings. Listing a
  restriction that is not in force is worse than listing none — the player
  stops trusting the panel.

### GuardianToolsLayer

```cpp
Application::Get().PushLayer(std::make_shared<Age::GuardianToolsLayer>());
Age::GuardianToolsLayer::SetIndicatorEnabled(true);
Age::GuardianToolsLayer::SetVisible(true);
```

The panel's decisions live in `GuardianToolsView` — no ImGui, 82 assertions, no
GPU. `GuardianToolsLayer` only draws it.

Two things to know:

- **The panel's Report / Mute / Block buttons are inert.** They are drawn and
  disabled, with a comment. Real moderation is your moderation. Wiring a stub
  that silently does nothing would be worse than not drawing them; omitting
  them would fail Art. 20(2) on the players who most need the button.
- **`SetIndicatorEnabled` defaults to false.** One line, but a surprise banner
  over a build nobody reviewed is a worse first impression than a documented
  opt-in. **Enable it if you ship into the EU.**

---

## The audit log and the gate

```cpp
if (AgeAuditLog::Instance().AuthorizeFeature(AgeService::Instance(),
                                             SurfaceKind::OnlineGame))
    OpenFeature();
```

`AuthorizeFeature` permits only when the policy finds the assurance sufficient
**and** the decision was recorded. Both, never either — *unrecorded means not
granted*. A capacity-zero log therefore authorises nothing: a studio who
disables logging does not silently get an unaudited game, they get a game that
gates shut.

It is a bounded ring (4096 entries, ~256 KiB) so that player behaviour cannot
drive an unbounded allocation. When it wraps it sets `Wrapped()` and overwrites
the oldest entry. Authorisation deliberately **continues** after a wrap —
refusing would lock a game shut permanently after a few thousand gated actions.
Poll `Wrapped()` and drain `Entries()` if you need durable records.

---

## Scripting

`Wasteland.Age` is bound as a submodule. It is **read-only on everything that
enforces a restriction** — Python is the game's own scripting layer, *inside*
the trust boundary, so a setter on a `SafetyProfile` property would be a setter
a script could use to hand a thirteen-year-old the adult feature set.

```python
import Wasteland

p = Wasteland.Age.Profile()
if p.FreeTextChat:
    open_chat()

if Wasteland.Age.AuthorizeFeature(Wasteland.Age.SurfaceKind.SocialNetwork):
    open_social_feed()
```

Two writable names, deliberately: `SetEuScope` (build configuration) and
`Refresh` (re-query the provider after a flow; **blocking**, never per frame).

`AgeGate.py` in `DemonCore-Editor/assets/scripts/` is a worked example. Note the
cache in it and the warning above it: **script instances are per entity,
`AgeService` is process-wide**, so reading `Age.Profile()` inside a per-entity
update runs once per entity per frame. Read once.

---

## Threading

`AgeService` is **not thread-safe**. `Refresh()` may need a round trip to a
vendor SDK, so:

1. Call `Refresh()` off the render thread.
2. Call the read accessors from the render thread afterwards.

The read accessors are cheap and allocation-free by design, which is what makes
that split workable.

---

## Tests

505 C++ checks across 10 suites, plus 60 Python checks against the real `.pyd`.

```bash
cd tests
C:/msys64/mingw64/bin/premake5.exe vs2026          # generates the .vcxproj files
MSB=$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" \
      -latest -products '*' -requires Microsoft.Component.MSBuild \
      -property installationPath)/MSBuild/Current/Bin/MSBuild.exe
"$MSB" AgeTests.vcxproj -p:Configuration=Release -p:Platform=x64 -t:Build
./bin-int/tests/Release-windows-x86_64/AgeTests.exe
```

From the repo root:

```bash
python scripts/age_binding_smoke.py Release
```

> **A green build is not evidence the bindings work.** The smoke test exists
> because `<pybind11/stl.h>` was missing from one translation unit and the
> build was clean while `AuditEntries()` raised `TypeError` at runtime. Always
> run it.

Each suite is a **separate executable** because each has its own `main()` and
its own dependency graph. `tests/WastelandTests.sln` is not generated by the
installed Premake 5.0.0-beta8 — build the `.vcxproj` files directly.

---

## Checklist before you ship

- [ ] Provider installed, and `Refresh()` called off the render thread
- [ ] `SetIndicatorEnabled(true)` if shipping into the EU
- [ ] Every exposed feature calls `Age.Profile()` or `AuthorizeFeature()`
- [ ] Chat, voice, UGC, marketplace, location, stranger DMs, public profiles
- [ ] Report / mute / block **wired to something real**
- [ ] Art. 29(2) surfaces use `authorize()`; `IsSufficient` alone would not record
- [ ] `SetEuScope` set deliberately at startup — narrow it only if you mean to
- [ ] `AuditWrapped()` monitored if you need durable records
- [ ] `age_binding_smoke.py` passes in the configuration you actually ship
- [ ] A lawyer has looked at it
