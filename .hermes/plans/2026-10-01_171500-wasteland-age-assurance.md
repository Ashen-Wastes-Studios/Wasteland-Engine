# Wasteland Age Assurance — In-Engine Compliance Module


> **Reconciliation, 2026-10-02 — the C++ module is BUILT and its tests PASS.**
> This plan is the design record; the implementation has moved past it.
>
> On disk: `Wasteland/src/Wasteland/Age/` — 23 files, all 23 registered in
> `Wasteland.vcxproj`. Bindings in `Scripting/BindsAge.cpp`... correction,
> `Scripting/BindingsAge.cpp`. 11 test projects in `tests/`, all built in both
> Debug and Release.
>
> **11 suites, 585 checks, 0 failures** (Release/x64, run 2026-10-02):
>
> | Suite | Checks |
> |---|---|
> | ProjectTests | 80 |
> | SafetyProfileTests | 82 |
> | GuardianViewTests | 82 |
> | AgeServiceTests | 59 |
> | GuardianPolicyTests | 58 |
> | PolicyTests | 55 |
> | AuditLogTests | 40 |
> | AppleProviderTests | 38 |
> | GoogleProviderTests | 36 |
> | AgeTests (AgeBand) | 30 |
> | AttestationTests | 25 |
>
> D1 (no identity type exists) holds: the only `std::string` in the whole module
> is `AgeBand::BandName()`, a display label for an enum, not an identifier.
> `docs/AGE_ASSURANCE.md` exists (Task 10).
>
> **Actually still open:**
> 1. **Task 9, `test_no_identity_types`, does not exist** — the D1 guard is the
>    plan's own highest-value enforcement and it is unwritten. Nothing currently
>    *proves* the property; it is merely true today. One of the 11 suites should
>    assert it so a future identity field cannot be added silently.
> 2. **`SeedingSmokeTest` is orphaned.** It is built but is not referenced by
>    any script, solution hook, or CI file, and it exits with a usage message
>    when run without `<engine-assets-dir>`. It has therefore never actually
>    run. Either wire it or delete it.
> 3. **Python bindings unverified end-to-end.** `BindingsAge.cpp` compiles into
>    the module, but no Python test exercises `Wasteland.Age`. Task 8 is
>    built-and-compiled, not verified.
> 4. Task 11 (guardian Tools UI) is C++ only — no ImGui panel
>    (`GuardianToolsView` is a non-UI view type; `GuardianToolsLayer` is logic).
>    Needs an editor surface to be usable in-engine.
> 5. Nothing consumes the module in a real game yet. `assets/scripts/AgeGate.py`
>    exists and is the intended first consumer; confirm it is wired to a scene.


> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Ship an age-assurance module *inside the Wasteland engine* so any game built on it inherits the minors-safety obligations of the EU KIDS Act proposal and US youth-privacy law as engine defaults — not as a per-studio integration project.

**Architecture:** A new `Wasteland::Age` static library namespace, dependency-free (no new vendor trees, no network in the core), implementing normalized age bands, jurisdiction policy resolution, safety-by-design default profiles, guardian tooling (state machine **and** ImGui UI), and an append-only audit log. Age *verification* itself is a pluggable provider interface — the engine never touches an identity document, because Art 28(1)–(3) forbids it. Studios wire their own vendor, app-store signal, or EU-certified issuer behind that interface.

**DECIDED 1 Oct 2026:** reference providers ship (Apple + Google, Tasks 4b/4c) · `SafetyProfile` is a **runtime read-only query**, never a hand-editable config · guardian tooling ships **UI + state machine** (Task 5b) · the module is **on by default**, with `UseEuScope=false` for US-only builds.

**Tech Stack:** C++17, no new dependencies. Follows existing engine conventions: tabs, `Wasteland::` namespace, `.h`/`.cpp` pairs under `Wasteland/src/Wasteland/<Module>/`, tested with the standalone `CHECK`/`CHECK_EQ` style from `tests/test_project.cpp`.

**License:** Apache-2.0 (`LICENSE.txt`). Contributors' changes ship under the same terms by default (License §4).

---

## 0. Why in-engine changes what gets built

This is the reframing that drives every decision below, so it goes first.

**The compliance surface is shared; the verification is not.** Every studio shipping a game to the EU faces the *same* obligations in Art 8–15, Art 19–20, and Art 27–28. None of them should re-derive that "geolocation off by default for minors" (Art 11(1)) is a legal requirement. That logic belongs in the engine, once, maintained by people who track the legislative file. What genuinely differs per studio is *how* they verify age — Yoti, Persona, an in-house EU Scheme issuer, an app-store signal — and that must stay a pluggable seam.

So the split is:

| Ships in the engine (shared, maintained centrally) | Stays with the studio (varies) |
|---|---|
| Normalized bands + threshold predicate | Which verification vendor |
| Jurisdiction policy resolution (Art 29(2) vs 29(4)) | The vendor's credentials, contracts, retention |
| Safety-by-design default profiles (Arts 8–15) | Their account system and backend |
| Guardian tooling state machine (Art 20) | Their identity/account store |
| Append-only audit log (Art 5-era readiness) | Their store for the Art 28(4) threshold cache |
| Provider interface + refusal semantics | |

**Consequence: there is no backend in this plan.** The engine ships a client-side capability library; studios run their own services. That deletes the Python/uv scaffolding from the previous draft and the entire §"unjoinable verdict store" server design — the anti-join property is now a *type-level* guarantee in C++ (see D1), which is strictly stronger than a SQLite schema check because it can't be bypassed by writing to the wrong table.

## 1. Legal status — unchanged, still the reason to move now

Neither named law is in force. This is unchanged from the previous draft and remains the most important framing.

- **EU KIDS Act** — `COM(2026) 681 final`, procedure `2026/0286(COD)`, Commission-adopted 17 Sept 2026. Requires Parliament **and** Council. Not adopted as of last check. Applies 6 months after entry into force (Art 43). Fines to 6% of worldwide turnover (Art 34(2)).
- **US KIDS Act** — `H.R. 7757`, passed House 29 June 2026 (267–117), Senate Commerce referral 13 July 2026, no further action. Not enacted. CRS R47884: *"No federal statute explicitly requires website operators to determine the age of individuals who use their websites."*

**What already binds every studio on this engine:**

| Obligation | Status |
|---|---|
| FTC COPPA Rule amendments (90 FR 16918) | **In force. Compliance deadline 22 Apr 2026 — already passed.** |
| Texas App Store Accountability Act (SB 2420) | **In force** (injunction stayed); deletion required after verification |
| Utah / Louisiana ASAA | 6 May 2027 / 1 Jul 2027 |
| Apple `Declared Age Range` + `PermissionKit` | Available |
| Google Play Age Signals API | Available (beta) |
| Amazon `GetUserAgeData` | **Live for TX users** |
| DSA Art 28 + Guidelines C/2025/5519 | In force |

The COPPA deadline has already passed. Studios shipping on this engine may be non-compliant today without knowing it. That — plus six named US states and the EU all converging on age signals by 2027 — is the real reason to build now rather than when either bill is final.

## 2. Scope

**In scope:**
- `Wasteland::Age` static library in the engine.
- Python bindings so studios can drive it from scripts.
- Docs other studios read to know what they inherit and what they still owe.

**Out of scope (deliberate):**
- **Any identity-document handling.** Art 28(1)–(3) forbids it and it would poison the module's fitness for every jurisdiction. The engine never sees a passport.
- **Zero-knowledge proof implementation.** Art 28(3) mandates ZKP for any age assurance measure. Do not hand-roll cryptography. The engine consumes attestations; it does not prove anything. Reference impl: `ageverification.dev` / EU Age Verification Scheme per Reg (EU) 2025/1569.
- **Art 5 VLOP compliance plan + independent audit.** ≥45M EU MAU threshold. Any studio that hits it needs counsel, not a code generator.
- **Editor UI.** `DemonCore-Editor` gets no age gates — it's a developer tool on a developer machine, not a service accessible to recipients (Art 2).
- **A hosted service.** Studios bring their own.

---

## 3. The five design decisions

### D1 — No identity type exists in the module

The strongest possible statement of Art 28(2)'s prohibition on combining age data with other personal data is: **there is no type in this module capable of expressing an identity.** No `UserId`, no `DeviceId`, no `DateOfBirth`, no `Email`. A `AgeAssuranceProvider` receives an age *band*, never a person.

This is why the engine approach is stronger than the backend approach. In a server, unlinkability is a schema convention somebody can violate with one `ALTER TABLE`. In C++, it's enforced by the type system — you cannot pass what you cannot name.

Make it a test: a grep-based check that the module's headers contain no identity-shaped type, plus a compile-time assertion.

```cpp
// Compile-time proof that the module has no identity-bearing type.
// A studio cannot accidentally grow one in without breaking this.
template <typename T> struct HasIdentityField { static constexpr bool value = false; };
```

### D2 — Never implement the ZKP cryptography

Art 28(3) is absolute: *"Any age assurance measure shall be zero knowledge proof."* Consume attestations minted by a certified issuer; do not build a prover. Treat the crypto as a pinned dependency to review, not code to write. This is the module's largest schedule risk and belongs in a studio's procurement conversation, not in this repo.

### D3 — A game has latitude a social network does not

The most useful finding in the proposal for a game engine, and easy to miss:

- **Art 29(2)** — providers under Art 6 (**social networking / video-sharing only**) *"shall rely exclusively on an EU age verification solution"* certified under the Scheme and verifiable against the Commission-published EU lists. No alternatives.
- **Art 29(4)** — providers under **Art 8(1)** and **Art 16(2)–(3)** — i.e. games, AI companions, chatbots, app stores — *"may use age assurance solutions other than the EU age verification solutions"*, provided they meet Art 27 (accuracy, reliability, security, robustness, non-intrusiveness, privacy, data protection, non-discrimination) and Art 28.

A game studio is **not** locked to the EU Scheme. That is a real commercial lever and it is why `JurisdictionPolicy` must encode this distinction rather than defaulting to the strictest rule.

Caveat to hand to studios: Art 28(3)'s ZKP mandate means most off-the-shelf vendor products (ID upload → face match → database hit) **do not** satisfy the proposal as written. "We retain your ID for 30 days for fraud prevention" is disqualifying.

### D4 — One band ladder, many jurisdictions

US ASAA laws, the OS APIs, the EU age rules, and KOSA use overlapping but non-identical bands. Normalize once:

| Normalized | US ASAA / OS APIs | EU proposal | KOSA |
|---|---|---|---|
| `Under13` | 0–12 | no account / guardian-only (<13) | child |
| `Age13To15` | 13–15 | guardian-supervised (13–14), autonomous (15) | teen (13–16) |
| `Age16To17` | 16–17 | autonomous | teen/adult boundary |
| `Adult18` | 18+ | adult — unlocks Art 8(1) derogation | adult |

The overlap is genuinely awkward at 15: EU permits autonomous social accounts at 15; US ASAA's 13–15 band doesn't distinguish 15 from 13. **Do not resolve this in the band type.** Store the normalized band, keep each jurisdiction's rule in `JurisdictionPolicy`, expose a single `MeetsThreshold(int)` predicate. Building EU logic against US band edges is how you get an off-by-one that is a compliance defect in one jurisdiction and noise in another.

### D5 — Safety-by-design is the default, and defaults must be *provable*

Art 8(1): safety-by-design applies **by default regardless of whether the service is accessed with an account**, and a provider may derogate only after establishing the user is an adult via age assurance.

That last clause is the subtle one and the easiest to get wrong: a studio that ships a game with unrestricted settings and no age gate has not merely omitted a feature — it has taken the Art 8(1) derogation **without establishing adulthood**. So the engine's job is to make the restricted profile the zero-configuration state, and make it structurally awkward to ship unrestricted settings without an `Adult18` band in hand.

The `SafetyProfile` returned by the engine must be inspectable by a studio and printable into their compliance evidence. If a studio can't show what the engine enforced, the engine isn't delivering compliance — it's delivering a suggestion.

---

## 4. Module layout

New files auto-join the build: `premake5.lua:59-60` globs `%{prj.name}/src/**.h` and `**.cpp`. **No premake edit is required** for any task below. (Verify this claim by building — don't take it on faith.)

```
Wasteland/src/Wasteland/Age/
  AgeBand.h / .cpp              D4: bands + MeetsThreshold, monotonic
  AssuranceLevel.h / .cpp       D3: SELF_DECLARED..EU_EAA
  JurisdictionPolicy.h / .cpp   D3: per-jurisdiction minimum level
  SurfaceKind.h                 online_game / social / video_sharing / ai_chatbot / app_store
  AgeAssuranceProvider.h        the pluggable seam (pure interface, no identity)
  AgeAssuranceError.h           refusal reasons studios must surface to users
  SafetyProfile.h / .cpp        D5: Arts 8/9/11/12/14/15 defaults
  GuardianPolicy.h / .cpp       Art 20 state machine
  AuditLog.h / .cpp             append-only, identity-free
  AgeService.h / .cpp           facade tying it together
  GuardianToolsLayer.h / .cpp   Art 20(1)(b) UI -- DECIDED: ship it (see Task 5b)
  Providers/
    AppleAgeSignalProvider.h/.cpp    Declared Age Range API (iOS)  -- see Task 4b
    GoogleAgeSignalProvider.h/.cpp   Play Age Signals API (Android) -- see Task 4b
  README.md                     what studios inherit vs. what they still owe
```

**Platform reality check (verified in the repo, affects Tasks 4b/4c):** `Core.h:24-25` defines `WL_PLATFORM_IOS` and then immediately `#error "IOS is not supported!"`; `Core.h:35-36` does the same for `WL_PLATFORM_ANDROID`. **The engine cannot build for iOS or Android today.** Both OS age-signal APIs are mobile-only, so the reference providers must be written compile-guarded and will return `SignalAbsent` on every platform this repo currently builds. That is correct behaviour anyway — an absent signal must never become adult — and it means the providers ship as a verified *contract* rather than exercisable device code until those `#error` guards are lifted. See §8 risk 8.

Tests mirror the engine's existing standalone-C++ convention (`tests/test_project.cpp`):
```
tests/test_age_band.cpp
tests/test_policy.cpp
tests/test_safety_profile.cpp
tests/test_guardian_policy.cpp
tests/test_audit_log.cpp
tests/test_no_identity_types.cpp   D1 enforcement
```

Bindings: append to `Wasteland/src/Wasteland/Scripting/Bindings.cpp` (4143 lines — see the pybind11 skill for `def_property` by-value traps and the `/bigobj` requirement).

---

## 5. Tasks

Convention for all: write failing test → run → minimal implementation → run → build both configs → commit. Tests use `CHECK`/`CHECK_EQ` exactly as `tests/test_project.cpp:35-55` defines them.

**Build commands for every task** (msbuild is not on `PATH`; use vswhere per the pybind11 skill):
```bash
cd C:/dev/Wasteland
MSB=$(dirname "$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" \
  -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath)")/MSBuild
"$MSB/Current/Bin/MSBuild.exe" Wasteland.sln -p:Configuration=Release -p:Platform=x64 -t:Build -v:minimal -m -nologo
"$MSB/Current/Bin/MSBuild.exe" Wasteland.sln -p:Configuration=Debug   -p:Platform=x64 -t:Build -v:minimal -m -nologo
```
Both must pass. Build the **solution**, not one project — the `.pyd` can link while the editor target is what failed.

---

### Task 1: `AgeBand` — the normalized ladder (D4)

**Files:** Create `Age/AgeBand.h`, `Age/AgeBand.cpp`, `tests/test_age_band.cpp`

**Step 1 — failing test**

```cpp
#include "Wasteland/Age/AgeBand.h"
#include <cstdio>

// US ASAA and the OS APIs give 0-12, 13-15, 16-17, 18+. The EU proposal splits
// 13-15 into 13-14 (guardian) and 15 (autonomous). AgeBand stores the US band
// and deliberately does NOT reconcile them -- see D4 in
// .hermes/plans/2026-10-01_171500-wasteland-age-assurance.md. Policy lives in
// JurisdictionPolicy; this type only answers "at or above which thresholds".

static int failures = 0, checks = 0;
#define CHECK(cond, what) do { ++checks; if(!(cond)) { ++failures; \
  std::printf("  FAIL: %s\n", (what)); } } while (0)

int main()
{
    using namespace Wasteland::Age;

    CHECK(BandForAge(12) == AgeBand::Under13,   "12 is under 13");
    CHECK(BandForAge(13) == AgeBand::Age13To15, "13 is in the teen band");
    CHECK(BandForAge(15) == AgeBand::Age13To15, "15 is still 13-15 in the US scheme");
    CHECK(BandForAge(16) == AgeBand::Age16To17, "16 starts the older-teen band");
    CHECK(BandForAge(18) == AgeBand::Adult18,   "18 is adult");

    // Monotonicity is the property the whole policy layer leans on. A
    // non-monotonic MeetsThreshold is a compliance defect, so assert it
    // across the full range rather than spot-checking edges.
    for (int age = 0; age <= 120; ++age)
        for (int t : {13, 15, 16, 18})
            if (MeetsThreshold(age, t))
                for (int lower : {13, 15, 16, 18})
                    if (lower < t)
                        CHECK(MeetsThreshold(age, lower),
                              "meeting a threshold implies meeting every lower one");

    CHECK(!MeetsThreshold(12, 13), "12 must not meet 13");
    CHECK(!MeetsThreshold(15, 16), "15 must not meet 16");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
```

**Step 2 — verify failure:** compile errors, `BandForAge` undeclared.

**Step 3 — implement**

```cpp
// AgeBand.h
#pragma once

namespace Wasteland::Age {

	// Normalized bands shared by the US App Store Accountability Acts, the
	// Apple/Google/Amazon age-signal APIs, the EU KIDS Act proposal, and KOSA.
	//
	// The bands deliberately do NOT align across those regimes -- the EU splits
	// 13-15 into 13-14 (guardian-supervised) and 15 (autonomous). Reconciling
	// them here would bake one jurisdiction's rule into shared storage and
	// produce an off-by-one that is a compliance defect elsewhere. Store the
	// band; interpret it in JurisdictionPolicy.
	enum class AgeBand : int
	{
		Unknown = -1, // No signal received. NEVER treat as adult.
		Under13 = 0,
		Age13To15 = 1,
		Age16To17 = 2,
		Adult18 = 3
	};

	AgeBand BandForAge(int age);
	bool   MeetsThreshold(int age, int threshold);
	bool   MeetsThreshold(AgeBand band, int threshold);

}
```

`BandForAge` rejects negatives and takes `int` (not `unsigned`) so `-1` can't silently become huge.

**Step 4 — verify pass:** `0 failures`. Then build both configs per §5.

**Step 5 — commit:** `feat(age): normalized AgeBand ladder with monotonic threshold predicate`

---

### Task 2: `AssuranceLevel` + `JurisdictionPolicy` (D3)

**Files:** Create `AssuranceLevel.h`, `SurfaceKind.h`, `JurisdictionPolicy.h/.cpp`, `tests/test_policy.cpp`

```cpp
// AssuranceLevel.h
#pragma once

namespace Wasteland::Age {

	// Ordered weakest to strongest. SELF_DECLARED is included only so the
	// engine can name the refusal it exists to produce -- it is never
	// sufficient under the EU proposal (Art 3(i) excludes self-declaration
	// from "age assurance" entirely).
	enum class AssuranceLevel : int
	{
		SelfDeclared = 0, // NEVER sufficient in EU scope
		OsSignal     = 1, // OS / app-store age signal
		Commercial   = 2, // vendor assurance meeting Art 27/28
		EuEaa        = 3  // EU proof-of-age attestation (Art 3(o))
	};

}
```

```cpp
// SurfaceKind.h
#pragma once
namespace Wasteland::Age {
	enum class SurfaceKind : int
	{
		OnlineGame,     // Art 15
		AiChatbot,      // Art 14
		AppStore,       // Art 16
		SocialNetwork,  // Art 6 -- the only strictly-locked surface
		VideoSharing    // Art 6
	};
}
```

```cpp
// JurisdictionPolicy.cpp
namespace Wasteland::Age {

	// Art 29(2): providers under Art 6 (social networking, video-sharing) shall
	// rely EXCLUSIVELY on an EU age verification solution certified under the EU
	// Age Verification Scheme. No alternatives permitted.
	//
	// Art 29(4): providers under Art 8(1) and 16(2)-(3) -- games, AI chatbots,
	// app stores -- MAY use other age assurance solutions, provided they meet
	// Art 27 and Art 28. A game studio is therefore not locked to the EU Scheme.
	AssuranceLevel MinimumLevelFor(SurfaceKind surface)
	{
		switch (surface)
		{
		case SurfaceKind::SocialNetwork:
		case SurfaceKind::VideoSharing:
			return AssuranceLevel::EuEaa;   // Art 29(2), exclusive
		case SurfaceKind::OnlineGame:
		case SurfaceKind::AiChatbot:
		case SurfaceKind::AppStore:
			return AssuranceLevel::Commercial; // Art 29(4), alternatives allowed
		}
		return AssuranceLevel::EuEaa; // fail closed on an unknown surface
	}

	bool IsSufficient(SurfaceKind s, AssuranceLevel provided)
	{
		// Self-declaration is never sufficient under the EU proposal. A studio
		// shipping a US-only build may set UseEuScope=false (see below), but the
		// engine must not silently treat a DOB box as age assurance.
		if (provided == AssuranceLevel::SelfDeclared) return false;
		return static_cast<int>(provided) >= static_cast<int>(MinimumLevelFor(s));
	}

}
```

**Tests to write:**
- `SelfDeclared` rejected for **every** surface, including `OnlineGame`.
- `SocialNetwork` rejects `Commercial`.
- `OnlineGame` accepts `Commercial`; rejects `SelfDeclared`.
- **Fail-closed:** an out-of-range `SurfaceKind` cast returns `EuEaa`, never something permissive.
- Monotonic: `EuEaa` sufficient everywhere `Commercial` is.

That fail-closed test matters. An enum cast from a persisted value that predates a new member is exactly how a permissiveness bug reaches production.

**Verify:** pass; build both configs.

**Commit:** `feat(age): assurance levels and per-surface minimums (Art 29(2) vs 29(4)), fail-closed`

---

### Task 3: `SafetyProfile` — safety-by-design defaults (D5)

**Files:** Create `SafetyProfile.h/.cpp`, `tests/test_safety_profile.cpp`

This is the module's largest legal payload and the core of what studios inherit. Every field cites its article.

```cpp
// SafetyProfile.h
#pragma once
#include "Wasteland/Age/AgeBand.h"

namespace Wasteland::Age {

	// Defaults that apply to every user until age assurance establishes
	// adulthood. Art 8(1): these apply by default REGARDLESS of whether the
	// service is accessed with an account, and a provider may derogate only
	// after establishing the recipient is an adult.
	//
	// DECIDED: runtime query, exposed read-only to studios. A generated config
	// file is friendlier to non-programmers but can be hand-edited into a
	// permissive state, which is the exact failure mode Art 8(1) exists to
	// prevent. Studios read this struct; they do not author it. If they need a
	// config surface, they derive one FROM this at runtime, never instead of it.
	//
	// Every default below is the RESTRICTED value. A studio shipping this
	// engine without an age gate is shipping the compliant configuration;
	// opening it up requires an Adult18 band, which is a deliberate act.
	struct SafetyProfile
	{
		// --- Art 11(1): off by default for minors ---
		bool LocationSharing      = false; // (a) geolocation and tracking
		bool MicrophoneCamera     = false; // (b) mic and camera access
		bool ContactSyncAndRecs   = false; // (c) account recommendations, contact sync
		bool PushNotifications    = false; // (d) push notifications

		// Art 11(2): these may only be changed by a minor ABOVE 15, clearly
		// informed, with explicit consent. Art 11(1)(a) geolocation additionally
		// auto-disables at session end when the minor enabled it.
		int    SettingsChangeMinAge = 15;

		// --- Art 9(3): time limits protecting school time and core sleep ---
		bool  TimeLimitsEnforced  = true;
		int   DailyLimitMinutes   = 60;
		bool  UsageBreaksEnforced = true;

		// --- Art 12: contact and interaction safeguards ---
		bool ContactRequiresPreApproval = true;  // 12(1)(a)
		bool InContactRecommendations    = false; // 12(1)(b)
		bool GroupAddNeedsAgreement      = true;  // 12(1)(c)
		bool AnonymousBlocking           = true;  // 12(1)(d)
		bool ContactDetailsShared        = false;// 12(3)(c)
		bool ContentDownloadable         = false;// 12(3)(e) no screenshots/downloads
		bool LiveStreamingByDefault      = false;// 12(3)(f)

		// --- Art 14(1)(b): AI companion safety defaults ---
		bool CarriesPriorInteractions    = false;// no memory across sessions by default
		bool AutoActivates               = false;// Art 14(2)
		bool ProminentlyDisplayed        = false;// Art 14(2)
		bool EasyToDisable               = true; // Art 14(2)

		// --- Art 15(1)(d): guardian tooling for games ---
		bool GuardianToolsMandatory = true;
		bool Under13RequiresGuardian = true;

		// Provenance, so a studio can put this in their compliance evidence.
		AgeBand DerivedFrom = AgeBand::Unknown;
	};

	// The one place a derogation is permitted. Art 8(1): only after age
	// assurance has established adulthood. An Unknown band is NOT adult.
	SafetyProfile ProfileFor(AgeBand band);

}
```

**Tests to write:**
- Every restricted default is `false`/`true` as above for `Unknown` and every minor band.
- Adult18 gets the unrestricted profile — **but only Adult18**, not `Age16To17`.
- **The Art 8(1) test:** `ProfileFor(AgeBand::Unknown).LocationSharing == false`. A studio with no age gate gets protected settings, not open ones.
- `ProfileFor` carries `DerivedFrom` so evidence can name the band that produced it.

**Verify:** pass; build both configs.

**Commit:** `feat(age): safety-by-design default profiles per Arts 8/9/11/12/14/15`

---

### Task 4b: Apple `Declared Age Range` reference provider

**Files:** Create `Age/Providers/AppleAgeSignalProvider.h/.cpp`, `tests/test_apple_age_signal.cpp`

**DECIDED: yes, ship reference implementations.** Makes the module immediately useful instead of requiring every studio to write the OS integration first, and gives the `IAgeAssuranceProvider` contract a concrete, reviewed example.

**Platform constraint (verified):** `Core.h:24-25` — `WL_PLATFORM_IOS` is defined immediately followed by `#error "IOS is not supported!"`. Same for Android at `Core.h:35-36`. **This repo cannot build for either platform.** So this provider is written compile-guarded and returns `SignalAbsent` on every platform currently buildable. Do not remove the `#error` guards to make it testable — that's a separate engine-support project, not part of this plan.

```cpp
// AppleAgeSignalProvider.h
#pragma once
#include "Wasteland/Age/AgeAssuranceProvider.h"

namespace Wasteland::Age::Providers {

	// Reference implementation over Apple's Declared Age Range API
	// (and the PermissionKit Significant Change API for later updates).
	//
	// Apple returns one of four bands: 0-12, 13-15, 16-17, 18+. Map them to
	// AgeBand directly; see D4 for why the EU's 13/14-vs-15 split is NOT
	// resolved here.
	//
	// PLATFORM: iOS only. This engine does not currently build for iOS
	// (Core.h:24-25 #error). The class compiles everywhere and returns
	// SignalAbsent on platforms where the API is unavailable -- which is the
	// correct behaviour, since an absent signal must never resolve to adult.
	class AppleAgeSignalProvider final : public IAgeAssuranceProvider
	{
	public:
		const char* Describe(AssuranceError e) const override;
		AssuranceOutcome Verify(SurfaceKind surface, std::string& errorOut) override;

		// Maps Apple's documented band strings. Pure, so it is testable on
		// desktop without the platform SDK.
		static AgeBand MapAppleBand(const std::string& appleBand);
	};

}
```

**Tests to write:**
- `MapAppleBand` on all four documented values, plus unknown/unmapped input → `AgeBand::Unknown`.
- `Verify()` on a non-iOS build returns `SignalAbsent` and sets `errorOut` — **never** `Adult18`.
- Band strings that arrive from a future OS release degrade to `Unknown`, not to adult. This is the property that survives OS updates.

The static mapper is deliberately separable from the platform call so it is testable on the platforms this repo actually builds. That's what makes Task 4b verifiable rather than aspirational.

**Verify:** pass on desktop builds; both configs build.

**Commit:** `feat(age): Apple Declared Age Range reference provider, compile-guarded`

---

### Task 4c: Google Play Age Signals reference provider

**Files:** Create `Age/Providers/GoogleAgeSignalProvider.h/.cpp`, `tests/test_google_age_signal.cpp`

Identical structure to 4b against Google Play Age Signals API (beta). Same four bands, same conservative unknown handling, same `#ifdef WL_PLATFORM_ANDROID` guard — which, per `Core.h:35-36`, currently resolves to the unsupported branch.

**Amazon `GetUserAgeData` is deliberately not shipped.** It's live only for Texas users today and Amazon Appstore is a small share of the studios on this engine; the `IAgeAssuranceProvider` seam is the intended extension point if that changes. Note it in the README rather than writing a third provider.

**Verify:** pass; both configs build.

**Commit:** `feat(age): Google Play Age Signals reference provider, compile-guarded`

---

### Task 4: `AgeAssuranceProvider` — the pluggable seam (D1)

**Files:** Create `AgeAssuranceProvider.h`, `AgeAssuranceError.h`

The interface studios implement. **Its shape is the D1 guarantee** — note what is absent.

```cpp
// AgeAssuranceProvider.h
#pragma once
#include "Wasteland/Age/AgeBand.h"
#include "Wasteland/Age/AssuranceLevel.h"
#include <functional>
#include <string>

namespace Wasteland::Age {

	// A studio implements this to plug in whichever assurance they use:
	// an EU-certified issuer's attestations, Yoti/Persona/etc, or an OS/app-store
	// age signal (Apple Declared Age Range, Google Play Age Signals, Amazon
	// GetUserAgeData).
	//
	// D1: note what this interface does NOT take. No user id. No account handle.
	// No date of birth. No document. The engine has no type capable of expressing
	// an identity, because EU proposal Art 28(1)-(2) forbids retaining or
	// combining age data with other personal data, and US SCREEN Act rules cap
	// retention at what is "strictly necessary" to perform the check. A studio
	// that needs identity does identity handling in their own code, on their own
	// storage, where their own privacy policy governs it.
	//
	// The result carries a band and nothing else -- not the raw document, not a
	// date of birth, not a provider-side user reference.
	struct AssuranceOutcome
	{
		AgeBand         Band      = AgeBand::Unknown;
		AssuranceLevel Level     = AssuranceLevel::SelfDeclared;
		std::string     Reference; // opaque provider-side handle, for support tickets
	};

	enum class AssuranceError
	{
		None,
		ProviderUnavailable,
		UserDeclined,
		SignalAbsent,        // NOT adult. Never fall through to permissive.
		VerificationFailed,
		AttestationInvalid,
		Expired,
		NotPermittedForSurface // below the JurisdictionPolicy minimum
	};

	class IAgeAssuranceProvider
	{
	public:
		virtual ~IAgeAssuranceProvider() = default;

		// Human-readable refusal, safe to show a user. Art 29(5) requires an
		// effective, free, electronic complaint route for a user who believes an
		// age assurance outcome is wrong -- studios wire that separately.
		virtual const char* Describe(AssuranceError e) const = 0;

		// Perform assurance. Implementations MUST return SignalAbsent rather
		// than assuming adulthood when no signal is available.
		virtual AssuranceOutcome Verify(SurfaceKind surface, std::string& errorOut) = 0;
	};

}
```

**Tests to write:**
- A `MockProvider` returning `SignalAbsent` yields `AgeBand::Unknown` — **never** `Adult18`. This is the single most important test in the module: defaulting an unknown signal to adult is the exact failure both acts exist to prevent.
- An `AssuranceError` from a provider cannot produce a permissive profile.
- `IsSufficient(OnlineGame, SelfDeclared)` is false, so a naive DOB-box provider is refused at the seam.

**Verify:** pass; build both configs.

**Commit:** `feat(age): IAgeAssuranceProvider seam with no identity-bearing parameters`

---

### Task 5: `GuardianPolicy` — Art 20 state machine

**Files:** Create `GuardianPolicy.h/.cpp`, `tests/test_guardian_policy.cpp`

Art 20(1) requires tools for guardians that are: age-tailored (a), easy to use/access/activate (b), changeable only with **the same degree of authorisation as the initial activation** (c), effective and not easily circumvented (d), not disproportionately restricting minors' rights (e), and respecting the minor's agency and privacy (f). Art 20(2) requires informing the minor when a guardian tool is in use. Art 20(3) requires encouraging guardians to use them.

Art 20(1)(c) is the one that produces real bugs — a guardian-activated setting that a minor can later silently change is a compliance failure and a security failure at once.

```cpp
// GuardianPolicy.h
#pragma once
namespace Wasteland::Age {

	enum class GuardianAuthorisation : int
	{
		None = 0,
		AdultSelfDeclaration = 1, // Art 26(1)(c), until the Art 26(5) DA is adopted
		OfficialDatabaseSignal = 2,// Art 26(1)(a)
		ExistingRelationship    = 3 // Art 26(1)(b)
	};

	struct GuardianLink
	{
		bool                   Active              = false;
		GuardianAuthorisation  AuthorisationAtActivation = GuardianAuthorisation::None;
		GuardianAuthorisation  LastAuthorisation  = GuardianAuthorisation::None;
		bool                   MinorInformed       = false; // Art 20(2)
		bool                   TimeLimitsManaged   = true;  // Art 20(4)(a)
		bool                   SettingsManaged     = true;  // Art 20(4)(b)
		bool                   CanReportOnBehalf   = true;  // Art 20(4)(c)
	};

	class GuardianPolicy
	{
	public:
		void Activate(GuardianAuthorisation auth);
		// Art 20(1)(c): a change requires authorisation of at least the grade
		// used at activation. Escalation is allowed; silent downgrade is not.
		bool RequestChange(GuardianAuthorisation auth);
		bool IsActive() const { return m_Link.Active; }
		const GuardianLink& Link() const { return m_Link; }
	private:
		GuardianLink m_Link{};
	};

}
```

**Tests to write:**
- A change at **lower** authorisation than activation is rejected — the Art 20(1)(c) core test.
- A change at **equal or higher** authorisation succeeds.
- Activation with `None` is refused.
- `MinorInformed` defaults false and must be set true before the link is considered compliant (Art 20(2)).
- Art 26(1)(c): `AdultSelfDeclaration` is accepted **now** and flagged as provisional, because it is only guaranteed until the Art 26(5) delegated act lands.

**Verify:** pass; build both configs.

**Commit:** `feat(age): guardian tooling state machine with Art 20(1)(c) authorisation floor`

---

### Task 5b: `GuardianToolsLayer` — the Art 20(1)(b) UI

**Files:** Create `Age/GuardianToolsLayer.h/.cpp`, `tests/test_guardian_tools_viewmodel.cpp`

**DECIDED: yes, ship UI alongside the state machine.** Art 20(1)(b) requires guardian tools that are *"easy to use, access and activate"*, and Art 20(1)(a) requires them tailored to the minor's age. A bare state machine does not satisfy either on its face — "easy to use" is a claim about what the user sees, and without a UI the engine cannot make it. Shipping only the state machine would have meant the module asserts compliance it cannot demonstrate.

Implemented as a `Wasteland::Layer` so it drops into any game's existing layer stack via `Application::PushLayer()` (`Application.h:27`), with the `OnImGuiRender()` override already in the base class (`Layer.h:17`). It is engine-side, not editor-side — this ships to players.

```cpp
// GuardianToolsLayer.h
#pragma once
#include "Wasteland/Core/Layer.h"
#include "Wasteland/Age/GuardianPolicy.h"

namespace Wasteland::Age {

	// Art 20(1): guardian tools that are age-tailored (a), easy to use, access
	// and activate (b), changeable only at the activation authorisation grade
	// (c), effective and not easily circumvented (d), not disproportionately
	// restricting the minor's rights (e), and respecting the minor's agency and
	// privacy (f).
	//
	// This layer renders (a) and (b); GuardianPolicy enforces (c); the studio's
	// game code is responsible for (d), (e) and (f) in its own feature code.
	// The engine provides the tool, not the guarantee -- docs/AGE_ASSURANCE.md
	// must state that split honestly.
	class WL_API GuardianToolsLayer : public Layer
	{
	public:
		explicit GuardianToolsLayer(GuardianPolicy& policy);

		virtual void OnAttach() override;
		virtual void OnImGuiRender() override;

		// A minor MUST be informed when a guardian tool is in use (Art 20(2)).
		// This call is what satisfies that; the layer renders a persistent
		// on-screen notice while any guardian control is active.
		bool MinorNotified() const { return m_MinorNotified; }
		void NotifyMinor() { m_MinorNotified = true; }

	private:
		GuardianPolicy& m_Policy;
		bool m_MinorNotified = false;
	};

}
```

**Art 20(2)–(3) obligations that must appear in the UI, not just the state machine:**
- Art 20(2) — the minor is informed when a guardian tool is being used. A persistent indicator, not a one-time toast.
- Art 20(3) — the provider must *encourage* guardians to use the tools and *regularly remind* them, including clear warning messaging about managing a minor's settings. So the layer needs a periodic reminder path, not just a settings panel.
- Art 20(4) — the tool must integrate time limits (a), settings management (b), and reporting on the minor's behalf (c).

**Tests to write (viewmodel, headless — no GL context):**
Rendering ImGui is not unit-testable headlessly, so keep all logic in a **viewmodel** the layer reads, and test that. This is the same discipline `test_visual_blocks.py` uses.
- Reminder scheduling honours Art 20(3)'s "regularly remind" cadence and does not fire every frame.
- A settings change requested through the viewmodel at sub-activation authorisation is refused by `GuardianPolicy` and the UI shows the refusal — Art 20(1)(c).
- `MinorNotified()` is false until `NotifyMinor()`, and the layer does not render guardian controls as active before the minor has been informed.
- A minor can always see that guardian tooling is active (Art 20(1)(f) agency/privacy) — no hidden state.

**Verify:** viewmodel tests pass headless; both configs build.

**Commit:** `feat(age): GuardianToolsLayer -- Art 20(1)(b) UI with periodic guardian reminders (20(3))`

---

### Task 6: `AuditLog` — append-only, identity-free

**Files:** Create `AuditLog.h/.cpp`, `tests/test_audit_log.cpp`

Not the Art 5 VLOP compliance plan (out of scope, ≥45M MAU). This is the ordinary evidentiary record: which assurance method produced each band, when, under which jurisdiction's rule — enough to answer a regulator months later, and enough for a studio to show a platform-holder why it holds the age signal it holds.

```cpp
// AuditLog.h
#pragma once
#include "Wasteland/Age/AgeBand.h"
#include "Wasteland/Age/AssuranceLevel.h"
#include "Wasteland/Age/SurfaceKind.h"
#include <string>
#include <vector>

namespace Wasteland::Age {

	// Deliberately has no identity field, and no constructor that accepts one
	// (D1). If this record could name a person it would be a store of personal
	// data subject to Art 28(2) minimization, and the whole module's privacy
	// posture would be a claim rather than a guarantee.
	struct AuditRecord
	{
		std::string      TimestampUtc;
		SurfaceKind      Surface      = SurfaceKind::OnlineGame;
		AssuranceLevel   Level        = AssuranceLevel::SelfDeclared;
		AgeBand           ResultingBand= AgeBand::Unknown;
		bool             AdultDerogationApplied = false;
		GuardianAuthorisation GuardianAuth = GuardianAuthorisation::None;
	};

	class AuditLog
	{
	public:
		void Append(const AuditRecord& r);
		const std::vector<AuditRecord>& Records() const { return m_Records; }
		bool ContainsIdentityData() const; // always false -- see test
	private:
		std::vector<AuditRecord> m_Records;
	};

}
```

**Tests to write:**
- A record is appended for every band resolution **and every denial**.
- `ContainsIdentityData()` returns false over a populated log — and the test is written so that adding an identity field to `AuditRecord` makes it fail.
- The log is append-only: no remove, no clear, no mutation API.

**Verify:** pass; build both configs.

**Commit:** `feat(age): append-only identity-free audit log`

---

### Task 7: `AgeService` facade + README

**Files:** Create `AgeService.h/.cpp`, `Age/README.md`

Ties it together: run a provider against a surface, resolve a band, apply `JurisdictionPolicy`, produce a `SafetyProfile`, append to `AuditLog`. Studios use this one class.

**On-by-default (DECIDED).** A singleton `AgeService` is live from engine startup with **no provider registered**, which is the fail-closed state: `SafetyProfile` returns the restricted profile and every assurance attempt reports `SignalAbsent`. A studio that never calls anything gets protected defaults — the compliant configuration is the zero-code one. Gating the module behind a build flag would mean nobody inherits the protections, which defeats the point of putting it in the engine.

```cpp
// AgeService.h (excerpt)
namespace Wasteland::Age {

	// DECIDED: on by default. With no provider registered the service is
	// fail-closed -- restricted SafetyProfile, SignalAbsent on every attempt.
	// Studios opt IN to verification, never out of protection.
	//
	// UseEuScope=false narrows policy to the binding US obligations for a
	// US-only build that will never ship to the EU. It does NOT disable the
	// defaults; it only changes which jurisdiction's rules are consulted.
	class WL_API AgeService
	{
	public:
		static AgeService& Instance();

		void RegisterProvider(IAgeAssuranceProvider* provider); // not owned
		void SetSurface(SurfaceKind surface);
		void SetEuScope(bool enabled) { m_UseEuScope = enabled; }
		bool UseEuScope() const { return m_UseEuScope; }

		// Fails closed. With no provider, returns the restricted profile.
		SafetyProfile Resolve();

		// Attempts assurance. Returns Unknown band (never Adult18) on any
		// failure path, and logs the reason to the AuditLog.
		AgeBand VerifyNow(std::string& errorOut);

		const AuditLog& Log() const { return m_Log; }

	private:
		AgeService() = default;
		IAgeAssuranceProvider* m_Provider = nullptr;
		SurfaceKind    m_Surface  = SurfaceKind::OnlineGame;
		bool           m_UseEuScope = true;
		AuditLog       m_Log;
	};

}
```

**Tests to write:**
- A fresh `AgeService` with **no provider** returns the restricted profile and `VerifyNow` yields `AgeBand::Unknown`.
- Registering a provider that returns `SignalAbsent` still yields `Unknown` — never `Adult18`, regardless of `UseEuScope`.
- `SetEuScope(false)` does not relax `SafetyProfile` defaults; it only changes which rules `JurisdictionPolicy` consults.
- Every resolution path appends exactly one `AuditRecord`.

**Verify:** pass; build both configs.

**Commit:** `feat(age): AgeService facade, on by default and fail-closed`

`README.md` is a real deliverable, not documentation busywork. Other studios read it to learn **what they inherit and what they still owe** — including that neither law is in force, that Art 29(4) gives games latitude, that Art 28(3) makes most vendor products non-compliant as written, that the engine does not do identity, that the guardian UI provides the *tool* while the studio remains responsible for Arts 20(1)(d)–(f) in their own feature code, and that the reference providers are a tested contract rather than device-exercised code.

**Verify:** facade end-to-end in a test; build both configs.

**Commit:** `feat(age): AgeService facade and module README`

---

### Task 8: Python bindings

**Files:** Modify `Wasteland/src/Wasteland/Scripting/Bindings.cpp`

Bind `AgeBand`, `AssuranceLevel`, `SurfaceKind`, `SafetyProfile`, `IAgeAssuranceProvider`, `AgeService`, `GuardianPolicy`, `GuardianToolsLayer`, `AuditLog`.

**On `SafetyProfile`, binding it read-only is now a correctness requirement, not a style choice** (per the Task 3 decision). Do not bind `LocationSharing` or any other default as `def_readwrite`. A Python-visible write to a default would let a script ship a permissive profile without ever establishing adulthood, which is precisely the Art 8(1) derogation the engine exists to gate. Expose the struct read-only via `def_property_readonly` returning a reference; changes go through `AgeService`, never the struct.

Per the pybind11 skill, the traps that apply here:
- **`def_property` getters must return a reference.** `SafetyProfile` returned by value means `profile.LocationSharing = True` mutates a temporary and vanishes with no error — a silent no-op that reads as "the feature doesn't work." Use `reference_internal` on the profile, or explicit `Set*` methods.
- **`py::enum_` for all three enums**, so studios get readable values in Python.
- **Abstract base:** register `IAgeAssuranceProvider` and have Python subclasses implement it. Pass by pointer with explicit `return_value_policy::reference`.
- **Every branch returns `py::none()`**, never a default-constructed `py::object` (NULL handle, not None).
- **`/bigobj`** already required for pybind11 projects — confirm it's set in `premake5.lua` under `filter "system:windows"` for both engine and `.pyd`.

**Verify** — the skill's warning applies directly: a fake-module harness cannot prove a binding works. Test against the real built `.pyd`:
```bash
set PYTHONPATH=C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor
python -c "import Wasteland; print(Wasteland.AgeBand.Adult18)"
```
Then assert a **read-back after write** on `SafetyProfile`: set `LocationSharing = True` through Python, read it back, confirm it stuck. A by-value getter passes every other assertion in this task.

Two assertions specific to this module, both in §10:
- `AgeService.Instance().Resolve().LocationSharing is False` and `.TimeLimitsEnforced is True` through the **Python** surface. This proves the restricted defaults survive the binding layer — a dropped or mis-mapped default would leave Python permissive while C++ stayed correct, and a game shipping from Python would be non-compliant with no compile error anywhere.
- `SafetyProfile` exposes **no writable** field. Assert `hasattr` on every field and that assignment raises. This is the Art 8(1) gate holding across the language boundary, not a style preference.

Also write the §10 reference script `DemonCore-Editor/assets/scripts/AgeGate.py` and run `python scripts/audit_script_bindings.py` against it.

**Commit:** `feat(bindings): expose Wasteland::Age to Python with reference-returning property getters`

---

### Task 9: `test_no_identity_types` — the D1 guard

**Files:** Create `tests/test_no_identity_types.cpp`

The test that keeps the module's central promise honest as it evolves. A build-time check is ideal, but a source-scanning test runs in CI on any platform:

```cpp
// Fails if the Age module ever grows an identity-bearing type. D1: the module
// must have no way to express a person, because Art 28(1)-(2) forbids retaining
// or combining age data with other personal data. This is the regression guard
// for that guarantee -- it has to be seen failing at least once to be trusted.
int main()
{
    const char* forbidden[] = {
        "UserId", "AccountId", "DeviceId", "PlayerId",
        "DateOfBirth", "BirthDate", "DOB",
        "Email", "Phone", "Passport", "IdNumber", "NationalId"
    };
    // scan Age/*.h for each token as a declared member/type name
    // fail with the file and line on any hit
}
```

Implement by reading the module headers from a configured source path. Confirm it has teeth by temporarily adding a `std::string UserId;` member and observing the failure, then reverting.

**Verify:** green, and demonstrably red under the mutation.

**Commit:** `test(age): guard that the Age module never grows an identity-bearing type`

---

### Task 10: Compliance mapping doc

**Files:** Create `docs/AGE_ASSURANCE.md`

A table for studios: obligation → article → what the engine does automatically → what the studio must still do. Every row must land in exactly one column, because "the engine handles it" for something it doesn't is worse than no doc at all.

Rows to include: Art 8(1) default, Art 9(3) time limits, Art 11(1) safe settings, Art 11(2) over-15 consent, Art 12 contact safeguards, Art 14 AI companion defaults, Art 15(1)(d) guardian tools, Art 20(1)(a)–(c) guardian conditions (engine), Art 20(1)(d)–(f) (**studio — the engine ships the tool, not the guarantee**), Art 20(2) minor notification, Art 20(3) guardian reminders, Art 26 parental responsibility, Art 27 assurance principles, Art 28(1)–(3) privacy + ZKP, Art 28(4) threshold cache, Art 29(2)/29(4) method latitude, Art 29(5) complaint route, Art 32 existing accounts.

State plainly at the top: **neither law is in force as of writing**, with the status and date. A compliance doc that implies settled law is itself a liability for the studios relying on it.

Also state the honest boundary of the on-by-default decision: a studio gets the restricted profile for free, but protections only apply where the studio calls `AgeService`. Skipping the call means no gating, and the studio owns that gap. Say so rather than implying the engine makes a game compliant by itself.

The doc should carry the §10 scripting guidance: configure once at startup rather than per entity (the script host is per-entity, `AgeService` is a singleton), branch on the `SafetyProfile` rather than re-deriving policy from a band, and treat an `Unknown` band as unverified rather than adult. Link to `AgeGate.py` as the worked example.

**Commit:** `docs(age): obligation-to-engine mapping with legal status date`

---

## 6. Validation

| Check | Command | Expect |
|---|---|---|
| Module tests | build + run each `tests/test_age_*.cpp` | 0 failures |
| **D1 guard has teeth** | temporarily add `std::string UserId;` to `AgeBand.h` | `test_no_identity_types` **fails** |
| **Art 11 default guard has teeth** | set `LocationSharing = true` in the struct default | `test_safety_profile` **fails** |
| Release build | MSBuild `Wasteland.sln` `-p:Configuration=Release` | success |
| Debug build | MSBuild `Wasteland.sln` `-p:Configuration=Debug` | success |
| Python module | `python -c "import Wasteland; print(Wasteland.AgeBand.Adult18)"` | `3` |
| Binding read-back | set `SafetyProfile.LocationSharing`, read back | value persists |
| Identity grep | grep `Age/*.h` for `dob\|passport\|email\|user_id` | only in comments/rejection text |

The two mutation checks are the point of this table. A privacy guarantee enforced only by code nobody has seen fail is a comment, not a control.

## 7. Files

**New (18):** `Wasteland/src/Wasteland/Age/` (17 files incl. README + 2 providers) + 7 `tests/test_age_*.cpp` and `test_guardian_tools_viewmodel.cpp`
**Modified (1):** `Wasteland/src/Wasteland/Scripting/Bindings.cpp` (append only)
**New doc (1):** `docs/AGE_ASSURANCE.md`
**Example (1):** `DemonCore-Editor/assets/scripts/AgeGate.py` — the §10 reference script
**`premake5.lua`: no edit.** `premake5.lua:59-60` globs `src/**.h` and `src/**.cpp`. Verify by building — if a new file doesn't appear, that's the claim to check first.

## 8. Risks

1. **The EU proposal will change.** Parliament and Council haven't adopted it. Article numbers and thresholds move. Mitigation: every article reference lives in comments and `docs/AGE_ASSURANCE.md`; no logic hardcodes article semantics. Re-check `2026/0286(COD)` per release.
2. **Art 28(3) ZKP is probably unmeetable commercially today.** Largest technical risk in the plan. Mitigation: the engine doesn't implement it; studios get the Art 29(4) latitude plus an honest statement in the README that most vendor products don't meet Art 28(3) as written. Hand studios the procurement question.
3. **Band misalignment at 15** (D4). Mitigation: threshold predicate at the edge, never reconcile in the type.
4. **A permissive default is a silent compliance failure.** One `= true` typo in `SafetyProfile` and every game on the engine ships non-compliant. Mitigation: mutation-checked defaults in Task 3 and the D1 guard in Task 9.
5. **The engine can't enforce what it can't see.** Studios can bypass every default by not calling `AgeService`. Mitigation: `docs/AGE_ASSURANCE.md` is explicit that defaults apply via `AgeService`, and the README states the engine makes the compliant path the default rather than enforcing it absolutely. Don't overclaim in either document.
6. **COPPA's deadline already passed.** Studios on this engine may be non-compliant today. The doc should say so.
7. **Scope creep into Art 5 VLOP territory.** Explicitly out. If a studio hits 45M EU MAU they need counsel, not a code generator.
8. **The reference providers cannot be exercised on any platform this repo builds.** `Core.h:24-25` and `Core.h:35-36` `#error` out iOS and Android; both OS age-signal APIs are mobile-only. So Tasks 4b/4c ship as a *verified contract* (static band mappers, fully unit-tested) plus a compile-guarded platform call that returns `SignalAbsent` today. The danger is that a reviewer reads "Apple provider implemented" as "Apple path tested end-to-end." It is not, and the README must say so plainly. Lifting those `#error` guards is a separate engine-support project — deliberately not bundled here.

## 9. Open questions

**Resolved 1 Oct 2026 — all four:**

1. ~~Which OS/app-store signal should ship as a reference implementation?~~ **DECIDED: yes — ship both.** Tasks 4b (Apple `Declared Age Range`) and 4c (Google Play Age Signals). Amazon `GetUserAgeData` deliberately not shipped; README notes it as a seam to extend. Platform constraint discovered while planning this: iOS/Android don't build in this repo, so the providers ship compile-guarded with fully-tested static band mappers and return `SignalAbsent` on desktop. See §4 and §8 risk 8.
2. ~~Should `SafetyProfile` be a runtime query or a generated config file?~~ **DECIDED: runtime query**, exposed read-only. A hand-editable config is the exact permissive-state failure Art 8(1) exists to prevent. Studios read the struct; any config surface derives from it at runtime, never replaces it. Locked into Task 3.
3. ~~Does this need a `GuardianTools` UI, or just the state machine?~~ **DECIDED: both.** Art 20(1)(b)'s "easy to use, access and activate" and 20(1)(a)'s age-tailoring are claims about what the user sees — a bare state machine cannot satisfy either on its face. Task 5b ships `GuardianToolsLayer` as a `Wasteland::Layer` (drops into any game's stack via `Application::PushLayer()`, `OnImGuiRender()` already in `Layer.h:17`), engine-side so it reaches players, with all testable logic in a headless viewmodel per `test_visual_blocks.py` discipline.
4. ~~Should the module be gated behind a build flag?~~ **DECIDED: on by default**, `UseEuScope=false` for US-only builds. No provider registered at startup *is* the fail-closed state — restricted profile, `SignalAbsent` — so a studio that writes zero code gets the compliant configuration. A build flag would mean nobody inherits the protections. Locked into Task 7.

No open questions remain.

## 10. Scripting it into a game

This is the section studios will actually read first, so it comes before sequencing.

### The structural mismatch, stated up front

The engine's script host is **per-entity**. `ScriptEngine::OnUpdateEntity` (`ScriptEngine.cpp:29`) ticks a script only for entities carrying a `ScriptComponent`, resolves it through `Project::GetAssetPath` (`ScriptEngine.cpp:58-59`), and holds one instance per entity UUID.

`AgeService` is a **process-wide singleton** (`AgeService::Instance()`). Age assurance is not an entity property — it belongs to the account/player, not to a crate, an enemy, or a spawner.

So the rule that follows, and that the docs must state explicitly:

> **Age assurance is configured once per game, from a single entity or at startup — never per entity.** Any script that reads the age state is reading one global truth, and it is a bug for two entities to disagree about it.

A script on every `Player` entity calling `AgeService::Resolve()` is the failure mode to design against. It works, it is wasteful, and it makes "what does this game think the player's age band is?" a question with N answers.

### Where to put it

Three options, in order of preference:

1. **Startup, in C++.** In the game's `main`/layer setup, before any scene loads: register the provider, set the surface, push `GuardianToolsLayer`. This is the recommended path and the one the engine supports best.
2. **A single designated entity** with a script — a `GameDirector`-style entity whose `ScriptComponent` owns the setup and then stays out of the way. Works, but it must be exactly one such entity, and the docs should say so.
3. **Not at all.** The module is on by default and fail-closed, so a game that does nothing still gets the restricted `SafetyProfile`. Doing nothing is a *valid* configuration, not an error.

### Reading the profile from a game script

After Task 8's bindings, a game script reads the enforced profile like this:

```python
# DemonCore-Editor/assets/scripts/AgeGate.py
#
# Reads the process-wide age state. ONE entity owns setup; every other script
# only reads. If you find yourself calling RegisterProvider() from a per-entity
# script, you have the wrong shape -- see "structural mismatch" above.
import Wasteland

class AgeGate:
    def __init__(self, entity):
        self.entity = entity
        self._profile = None
        self._band = None

    def on_start(self):
        # Read-only. The engine's defaults are the compliant configuration;
        # this reads what the engine decided, it does not decide.
        age = Wasteland.AgeService.Instance()
        self._band = age.VerifyNow()
        self._profile = age.Resolve()

        # Art 11(1): every one of these is False for a minor and for an
        # unverified user. Branch on the profile rather than re-deciding policy,
        # so the game's behaviour and the engine's compliance evidence agree.
        if self._profile.LocationSharing:
            self.enable_minimap()
        if not self._profile.TimeLimitsEnforced:
            self.warnings.append("Art 9(3) time limits are not enforced")

    def on_update(self, ts):
        # Art 9(3): time-limited access and usage breaks protecting school
        # time and core sleep. The engine owns the schedule; the game owns the
        # consequence of hitting it.
        if self._profile and self._profile.TimeLimitsEnforced:
            remaining = self._profile.DailyLimitMinutes - self.minutes_used
            if remaining <= 0:
                self.begin_usage_break()
```

The point of reading the profile rather than branching on a band directly: the profile is what the engine will show an auditor. A game that branches on `band == Adult18` in three places has three chances to drift from the engine's own record.

### The three rules to put in the module README

1. **Configure once, read many.** Registration and layer setup happen once at startup. Scripts read.
2. **`SafetyProfile` is read-only.** It comes back `reference` from Python but every field is a default, not a knob. Changing enforcement means changing `AgeService`, not assigning to the struct.
3. **An `Unknown` band is not `Adult18`.** A script that sees `Unknown` and proceeds as if the user were verified has written the exact bug both acts exist to prevent. Branch on the profile, which is restricted for `Unknown`.

### Verifying the integration

Script-level verification is not optional and not a fake harness — the pybind11 skill is explicit that a fake-module harness validates script logic only and cannot detect a binding defect. Test against the real built module:

```bash
set PYTHONPATH=C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor
python scripts/audit_script_bindings.py DemonCore-Editor/assets/scripts/AgeGate.py
```

Then assert a read-back after write on anything the script mutates, and confirm `SafetyProfile` reads as restricted through the Python surface:

```python
import Wasteland
p = Wasteland.AgeService.Instance().Resolve()
assert p.LocationSharing is False, "Art 11(1) default must be off through Python too"
assert p.TimeLimitsEnforced is True
```

That second assertion matters more than it looks: it proves the restricted defaults survive the binding layer. A binding that dropped or mis-mapped a default would leave the Python-visible surface permissive while C++ stayed correct — and a game shipping from Python would then be non-compliant with no compile error anywhere.

## 11. Sequencing

- **Task 1–2** (bands, policy) — pure logic, no dependencies, immediately testable.
- **Task 3–4** (safety profile, provider seam) — the legal payload and the architectural guarantee. Do these before anything else that builds on them.
- **Task 4b–4c** (reference providers) — depend on the 4 seam; independent of each other.
- **Task 5–5b** (guardian state machine, then UI) — 5b depends on 5. Both before 7.
- **Task 6** (audit) — independent of the rest.
- **Task 7–8** (facade, bindings) — needs 1–6.
- **Task 9–10** (guard, docs) — Task 9 should land early in practice even though sequenced last, because it's what keeps 3 and 4 honest. If you want one thing reordered, move this to position 4.

Task 7 depends on 5b, not just 5: `AgeService` pushes `GuardianToolsLayer` onto the layer stack by default, so the facade can't be wired up before the layer exists.

The module is useful the day Task 3 lands: a studio that calls `AgeService::Resolve` gets compliant-by-default behaviour without adopting either act's obligations voluntarily, which is the goal. Tasks 4b/4c are what let a studio skip writing the OS integration themselves — subject to the platform caveat, they get the contract and the mapping tested even where the API itself isn't reachable yet.