# Age Verification Backend — KIDS Act Readiness Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Build a privacy-preserving age-assurance backend for the Wasteland/Necroware game and its in-game AI chat, architected to satisfy the EU KIDS Act proposal's age-assurance chapter if adopted, and the obligations already binding today.

**Architecture:** A standalone Python service holding age verdicts in a store that is *structurally unjoinable* to account identity — the design mandated by Art 28(1)–(3) of the EU proposal. Verdict resolution happens via signed assertions minted per-flow; the game backend keeps only a per-account threshold boolean permitted by Art 28(4). Age signals enter from OS/app-store APIs (already required by binding US state law) and from EU proof-of-age attestations.

**Tech Stack:** Python 3.12+ in its own `uv` venv (the engine's bundled `python314._pth` is isolated and cannot take dependencies — see `tests/headless/runner.py:5-11`), stdlib `dataclasses` + SQLite, FastAPI/uvicorn if deps are permitted. Thin C++ client in the engine reusing the existing WinHTTP `HttpClient` (`DemonCore-Editor/src/Panels/HttpClient.h`).

---

## 0. Read this first: neither named law is in force

This is the single most important fact in this plan, and it inverts the usual "get compliant before the deadline" framing. Building to these specs today is **readiness work, not remediation** — and that is a legitimate reason to build, but it is a different justification than the request implies.

**EU KIDS Act — proposal, not law.**
- Source: `COM(2026) 681 final`, procedure `2026/0286(COD)`, Commission press release `IP/26/1890`, adopted by the Commission **17 September 2026**.
- Requires European Parliament **and** Council approval. Neither had occurred as of the last update (FAQ last updated 21 September 2026; last checked today).
- Applies **6 months after entry into force** (Art 43); Art 5 from entry into force; Arts 33/35 at +12 months.
- **Fines up to 6% of total worldwide annual turnover** (Art 34(2), AI Act Art 99 route for AI companions/chatbots).
- Text may change materially during the ordinary legislative procedure. Every article reference in this plan is against the **proposal**, and is labeled as such.

**US KIDS Act — passed House only, not enacted.**
- Source: `H.R. 7757`, 119th Congress. Passed House **29 June 2026**, 267–117. Received in Senate and referred to Commerce **13 July 2026**. No further action.
- CRS R47884 (28 July 2026) is explicit: *"No federal statute explicitly requires website operators to determine the age of individuals who use their websites."*
- Bundle contents with age-verification teeth: **SCREEN Act** (covered porn platforms, >1/3 sexual material harmful to minors, "more likely than not" a minor), **KOSA**, **COPPA 2.0** (Children and Teens' Online Privacy Protection Act), **SAFE BOTS Act**, **CHAT Act**.
- Note the internal tension KOSA inherits from the CRS analysis: obligations attach when a service "knows or should have known" a user is a child (under 13) or teen (13–16), while simultaneously disclaiming any age-verification mandate. Courts and regulators resolve that post-hoc. **This is the strongest argument for the backend: not to satisfy a mandate, but to convert an unbounded negligence-style knowledge test into a bounded, documented one.**

**What actually binds you today, regardless of either bill:**

| Obligation | Status | Effective |
|---|---|---|
| FTC COPPA Rule amendments (90 FR 16918) | **In force. Compliance deadline already passed.** | Effective 23 Jun 2025; compliance due **22 Apr 2026** |
| Texas App Store Accountability Act (SB 2420) | **In force** (Dec 2025 injunction stayed by federal appeals court) | Live now |
| Utah / Louisiana ASAA | Delayed | 6 May 2027 / 1 Jul 2027 |
| Apple `Declared Age Range` API + `PermissionKit` Significant Change API | Available | — |
| Google Play Age Signals API (beta) | Available | — |
| Amazon `GetUserAgeData` API | **Live, returning responses for TX users** | — |
| DSA Art 28 + Commission Guidelines (C/2025/5519) | In force | Oct 2025 |
| UK Online Safety Act, Ofcom highly-effective age assurance | In force (separate jurisdiction) | Jul 2025 |

The COPPA amendment deadline has **already passed** (22 April 2026, ~5 months ago). If Necroware collects personal information from under-13s or has any child-directed surface, that is a live obligation with a missed date — and it is a *stronger* reason to build this backend than either pending bill. Flagged as the top open question in §8.

---

## 1. Scope

**In scope (primary):**
- Necroware, as an **online game** → EU proposal Art 15 obligations; Art 8 safety-by-design default.
- Any shipped in-game AI companion / general conversational chatbot → **Art 14**; Art 3(3)(d) excludes specialised/technical-support systems, so a narrow NPC-dialogue system is likely out, a general chat assistant is likely in.
- Game account creation and any UGC/contact/social features → Arts 6, 12.

**Out of scope (unless you say otherwise):**
- `DemonCore-Editor`'s `AIChatPanel` (`src/Panels/AIChatPanel.h`). This is a developer tool on a developer machine hitting an OpenAI-compatible endpoint with a local API key. It is not a service "accessible to recipients of the service" in the Art 2 sense, and not accessible to minors. No age assurance applies to it. Do not build age gates into the editor.
- `AshenWastesStudios` marketing website — no accounts, no UGC, no chat. Nothing in either act applies. Revisit only if it gains user accounts or a comment/community surface.
- **VLOP compliance plan + independent audit (Art 5).** Threshold is ≥45M EU MAU, VLOPs only. Explicitly out. Do not build a compliance-plan generator.

**Assumption to confirm:** the target is the shipped game and its AI chat. If the intent was the Ashen AI Base chatbot (`localhost:5000`) instead, the obligations shift to Art 14 alone and the game-specific Arts 9/11/12/15 work drops out — say the word and I will rescope.

---

## 2. The five design decisions that matter

Everything else is plumbing. These are load-bearing and cheap to get right now, expensive to retrofit.

### D1 — Verdicts must be structurally unjoinable to identity

EU proposal Art 28(1)–(2): age assurance *"shall not enable the identification of the recipient nor locate, track, target, advertise to or profile"*, and providers *"shall not maintain, acquire or process more personal data than strictly necessary"*, nor *"combine personal data stored or relating to the use of the system with personal data from any other services offered by the provider or from third-party services."*

The naive implementation — an `age_verified` boolean, or worse a `date_of_birth` column, on the user table — is precisely what this forbids. So the age store is a **separate SQLite database with no foreign key, no shared primary key space, and no join key of any kind** to the account store. The only thing that crosses between them is a boolean the account store is *entitled* to hold (D3).

This must be a **test**, not a comment. Task 4.

### D2 — Never implement the zero-knowledge cryptography

Art 28(3) is unusually absolute: *"Any age assurance measure shall be zero knowledge proof."* That is the hardest requirement in the entire proposal and it is **not** something to hand-roll.

The correct move is to be a **consumer, not a producer**, of that cryptography:
- The Commission's **EU Age Verification Scheme** (per Commission implementing Regulation (EU) 2025/1569) defines the trust model and attestation rules.
- The reference implementation is open source at `ageverification.dev`, interoperable with **European Digital Identity Wallets** under Reg (EU) 910/2014, issuing an **electronic attestation of attributes** (Art 3(44) of that Regulation) — the `proof of age attestation` in Art 3(o).
- Wallets roll out across Member States by end of 2026; the standalone app was feature-ready 15 April 2026.

So: verify attestations minted by a certified issuer against the Scheme. Do not build a prover. Concretely, budget for a signature/attestation verification dependency and treat the crypto as a supply-chain risk to be pinned and reviewed, not written.

### D3 — The Art 28(4) storage exception is narrow, and a game qualifies

Art 28(4) permits — by way of derogation from the minimization rule — storing *"at account level, the age signal that a user has successfully met a specific age threshold"*, **solely** to avoid repeated age assurance, limited to the minimum necessary. It is available to providers under **Art 8(1)** and **Art 16(4)**.

A provider of online games / AI chatbots / app stores is an **Art 8(1)** provider (Art 2(1)(d),(f),(g)). **So yes — a per-account threshold cache is legal under the proposal, and it is the only identity-linked age data permitted.**

Design consequence: the account store holds `meets_threshold: {13, 15, 18}` booleans and nothing else. No band, no issuer, no timestamp, no token, no DOB. That is the entire lawful surface.

### D4 — A game has latitude on *which* assurance method; a social network does not

This is the most useful finding in the proposal for a game developer, and it is easy to miss:

- **Art 29(2)** — providers under Art 6 (**social networking / video-sharing** only) *"shall rely exclusively on an EU age verification solution"* certified under the Scheme and verifiable against the Commission-published EU lists. No alternatives.
- **Art 29(4)** — providers complying with **Art 8(1)** and **Art 16(2)–(3)** — i.e. games, AI companions, chatbots, app stores — *"may use age assurance solutions other than the EU age verification solutions"*, provided they meet Art 27 (accuracy, reliability, security, robustness, non-intrusiveness, privacy, data protection, non-discrimination) and Art 28.

A game is therefore **not** locked to the EU Scheme. It may use a commercial age-assurance provider, subject to Art 27/28 — *including the ZKP requirement in Art 28(3)*. That is a real commercial lever, and it is the difference between "we must integrate a government-adjacent wallet scheme" and "we may procure a vendor and hold them to Art 27/28."

Caveat to carry into procurement: Art 28(3)'s ZKP mandate means most off-the-shelf vendor products (ID upload + face match + database hit) **do not** satisfy the proposal as written. Confirm with any vendor in writing whether their product meets Art 28(3), and treat "we keep your ID for 30 days for fraud prevention" as disqualifying.

### D5 — One threshold ladder, many jurisdictions

US ASAA laws, the OS APIs, the EU age rules, and KOSA all use overlapping but non-identical bands. Normalize once, at the edge:

| Normalized | US ASAA / OS APIs | EU proposal | KOSA |
|---|---|---|---|
| `UNDER_13` | 0–12 | no account / guardian-only (<13) | child |
| `AGE_13_15` | 13–15 | guardian-supervised (13–14), autonomous (15) | teen (13–16) |
| `AGE_16_17` | 16–17 | (autonomous) | teen / adult boundary |
| `ADULT_18` | 18+ | adult — unlocks Art 8(1) derogation | adult |

The overlap is genuinely awkward at 15: EU says autonomous social accounts at 15; US ASAA's 13–15 band does not distinguish 15 from 13. **Do not resolve this in the backend.** Store the normalized band, keep the jurisdiction's interpretation at the policy layer (`policy.py`), and let a single `MEETS_THRESHOLD(n)` predicate serve both. Building EU logic against US band edges is how you end up with an off-by-one that is a compliance defect in one jurisdiction and noise in the other.

---

## 3. Module layout

```
services/age-verification/          # NEW — own venv, NOT the engine's isolated python
  pyproject.toml
  app/
    main.py                    # routes
    config.py                  # settings, cwd-independent (see Ashen AI Base convention)
    thresholds.py              # D5: normalized bands + MEETS_THRESHOLD
    assurance.py               # assurance level resolution per jurisdiction
    verdicts.py                # D1: the unjoinable verdict store
    linkage.py                 # D3: account-level threshold cache (Art 28(4))
    policy.py                  # jurisdiction x surface -> obligation set
    guardian.py                # Art 26 parental responsibility
    retention.py               # deletion jobs
    audit.py                   # audit-ready evidence log (Art 5-era readiness)
    signals/
      apple.py                 # Declared Age Range API
      google_play.py           # Play Age Signals API
      amazon.py                # GetUserAgeData API
      eu_eaa.py                # EU proof-of-age attestation verification (D2)
  tests/
    test_thresholds.py
    test_verdicts_unlinkable.py   # the anti-join test — highest-value test in the suite
    test_assurance.py
    test_signals.py
    test_linkage_scope.py
    test_retention.py
    test_policy.py
```

Engine side (new, small):
```
Wasteland/src/Age/AgeAssurance.h/.cpp     # C++ client over existing WinHTTP HttpClient
```

---

## 4. Tasks

Each task: write the failing test → run it → minimal implementation → run it → commit. Suite uses the repo's stdlib runner convention (no pytest — the engine's Python is isolated; see `tests/headless/runner.py`). The *service* has its own venv and may use pytest, but match `runner.py`'s `check`/`check_eq` helper style for consistency.

---

### Task 1: Scaffold the service with an isolated venv

**Files:** Create `services/age-verification/pyproject.toml`, `services/age-verification/app/__init__.py`, `services/age-verification/tests/__init__.py`

**Step 1 — Confirm the engine Python really is isolated**

```bash
python3 -c "import sys; print(sys.prefix, sys.flags.isolated)"
```

Expected: `isolated=1` (this is what `runner.py:5-11` documents). If it is *not* isolated, revisit the no-pytest constraint and simplify.

**Step 2 — Create the venv with uv**

```bash
cd C:/dev/Wasteland/services/age-verification
uv venv --python 3.12
```

Do **not** reuse or modify the engine's interpreter. If `fastapi` is wanted: `uv pip install fastapi uvicorn`. Currently `fastapi` is **not** importable in the system Python — if dependency installation is off the table, fall back to `http.server` per the Ashen AI Base precedent and note the reduced production posture.

**Step 3 — Verify**

```bash
uv run python -c "import sys; print(sys.executable); print(sys.version)"
```

Expected: the venv's python, 3.12.x, `isolated=0`.

**Step 4 — Commit**

```bash
git add services/age-verification
git commit -m "build: scaffold age-verification service with isolated venv"
```

---

### Task 2: Normalized thresholds (D5)

**Files:** Create `app/thresholds.py`, `tests/test_thresholds.py`

**Step 1 — Write failing tests**

```python
from app.thresholds import Band, band_for_age, meets_threshold

def test_band_edges():
    check_eq(band_for_age(12), Band.UNDER_13, "12 is under 13")
    check_eq(band_for_age(13), Band.AGE_13_15, "13 is in the teen band")
    check_eq(band_for_age(15), Band.AGE_13_15, "15 is still 13-15 in the US scheme")
    check_eq(band_for_age(16), Band.AGE_16_17, "16 starts the older-teen band")
    check_eq(band_for_age(18), Band.ADULT_18, "18 is adult")

def test_meets_threshold_is_monotonic():
    # An adult meets every lower threshold. This is the property the whole
    # policy layer leans on; a non-monotonic result is a compliance defect.
    for age in (13, 15, 16, 18, 40):
        if band_for_age(age) is Band.ADULT_18:
            for n in (13, 15, 16, 18):
                check(meets_threshold(age, n), f"age {age} must meet {n}")

def test_under_13_meets_nothing():
    check(not meets_threshold(12, 13), "12 must not meet 13")
```

**Step 2 — Verify failure:** `uv run python -m tests.test_thresholds` → `ModuleNotFoundError: No module named 'app.thresholds'`

**Step 3 — Implement**

```python
"""Normalized age bands (D5).

US ASAA / OS APIs give 0-12, 13-15, 16-17, 18+. The EU proposal splits 13-15
into 13-14 (guardian) and 15 (autonomous). The bands therefore do NOT align, and
this module deliberately does not try to reconcile them: it stores the US band
and exposes MEETS_THRESHOLD(n) so policy.py can apply each jurisdiction's own
rule at the edge. See policy.py and D5 in the plan.
"""

from __future__ import annotations

from enum import IntEnum


class Band(IntEnum):
    UNDER_13 = 0
    AGE_13_15 = 1
    AGE_16_17 = 2
    ADULT_18 = 3


# Inclusive lower bound of each band.
_LOWER_BOUND = {Band.UNDER_13: 0, Band.AGE_13_15: 13, Band.AGE_16_17: 16, Band.ADULT_18: 18}

# Highest age threshold each band satisfies.
_SATISFIES = {Band.UNDER_13: 0, Band.AGE_13_15: 15, Band.AGE_16_17: 17, Band.ADULT_18: 18}


def band_for_age(age: int) -> Band:
    if not isinstance(age, int) or isinstance(age, bool):
        raise TypeError("age must be an int, not a bool or other type")
    if age < 0:
        raise ValueError("age must be non-negative")
    for band in sorted(Band):
        if age >= _LOWER_BOUND[band]:
            return band
    raise ValueError("unreachable")


def meets_threshold(age: int, threshold: int) -> bool:
    return _SATISFIES[band_for_age(age)] >= threshold
```

**Step 4 — Verify pass:** all three tests pass.

**Step 5 — Commit**

```bash
git add services/age-verification/app/thresholds.py services/age-verification/tests/test_thresholds.py
git commit -m "feat(age): normalized age bands with monotonic threshold predicate"
```

---

### Task 3: Assurance levels and jurisdiction policy

**Files:** Create `app/assurance.py`, `app/policy.py`, `tests/test_assurance.py`, `tests/test_policy.py`

The proposal draws a hard line that must live in code, not prose (D4):

```python
# app/policy.py
from enum import Enum

class AssuranceLevel(IntEnum):
    SELF_DECLARED = 0   # NEVER sufficient under the EU proposal (Art 3(i))
    OS_SIGNAL = 1       # OS / app-store age signal
    COMMERCIAL = 2      # vendor age assurance meeting Art 27/28
    EU_EAA = 3          # EU proof-of-age attestation (Art 3(o))
```

```python
# Minimum acceptable level by surface. Art 29(2) locks Art 6 social/video to
# EU_EAA exclusively; Art 29(4) lets Art 8(1) providers (games, AI chatbots,
# app stores) use alternatives meeting Art 27/28.
MINIMUM_LEVEL = {
    "social_networking": AssuranceLevel.EU_EAA,
    "video_sharing": AssuranceLevel.EU_EAA,
    "online_game": AssuranceLevel.COMMERCIAL,
    "ai_companion": AssuranceLevel.COMMERCIAL,
    "app_store": AssuranceLevel.COMMERCIAL,
}
```

**Tests to write:** `SELF_DECLARED` is rejected for every surface; `social_networking` rejects `COMMERCIAL`; `online_game` accepts `COMMERCIAL` but rejects `SELF_DECLARED`.

Note the direct conflict with today's shipping reality: the KIDS Act's SCREEN Act and state ASAA laws lean the *other* way, and `SELF_DECLARED` is the only method available on some platforms. Record that as a deliberate, documented exception in `policy.py` with a comment pointing at §8 — do not silently allow it.

**Verify:** `uv run python -m tests.test_policy` — assert `SELF_DECLARED` is refused everywhere in EU scope.

**Commit:** `feat(age): assurance levels and per-surface minimums (Art 29(2) vs 29(4))`

---

### Task 4: The unjoinable verdict store (D1) — highest-value task

**Files:** Create `app/verdicts.py`, `tests/test_verdicts_unlinkable.py`

This is the task that makes the design legal rather than merely plausible.

**Step 1 — Write the failing test**

```python
def test_no_join_key_between_stores():
    """Art 28(2): age data must not be combinable with any other personal data.

    Enforced structurally, not by convention. If someone later adds an
    account_id column to the verdict store, this test must fail.
    """
    v = VerdictStore(":memory:")
    acct = AccountStore(":memory:")

    token = v.issue(band=Band.ADULT_18, method="eu_eaa", subject_hint="opaque")
    acct.create_account("acct-1")
    acct.set_threshold_met("acct-1", 18, assertion=v.mint_assertion(token))

    v_columns = v.column_names()
    forbidden = {"account_id", "user_id", "email", "device_id", "subject",
                 "date_of_birth", "dob", "ip_address"}
    leaked = forbidden.intersection(c for c in v_columns)
    check_eq(leaked, set(), f"verdict store leaked joinable columns: {leaked}")
```

Plus the stronger runtime assertion — after a full flow, no value present in the verdict row for that flow appears anywhere in the account store:

```python
def test_no_value_overlap_after_flow():
    v = VerdictStore(":memory:")
    acct = AccountStore(":memory:")
    token = v.issue(band=Band.ADULT_18, method="eu_eaa")
    acct.create_account("acct-1")
    acct.set_threshold_met("acct-1", 18, assertion=v.mint_assertion(token))

    verdict_values = set(v.row_values(token))
    account_values = set(acct.all_values())
    overlap = verdict_values.intersection(account_values)
    check_eq(overlap, set(), f"age verdict values leaked into account store: {overlap}")
```

**Step 2 — Verify failure:** the naive design (a shared `user_id`) makes this fail. That failure is the point — it proves the test has teeth.

**Step 3 — Implement.** `VerdictStore` holds: `token` (high-entropy random, not derived from identity), `band`, `method`, `issued_at`, `expires_at`, and a **short TTL**. `mint_assertion` returns a signed, short-lived assertion carrying *only* `meets: [18]`. `AccountStore` never sees the token, band, method, or timestamp — it receives an already-verified assertion and stores booleans.

**Step 4 — Verify pass**, then additionally confirm the test fails when you deliberately add `account_id` to the verdict schema (mutation check — a test never seen red is not a test).

**Step 5 — Commit:** `feat(age): unjoinable verdict store + anti-join structural tests (Art 28(1)-(2))`

---

### Task 5: Account-level threshold cache (D3)

**Files:** Create `app/linkage.py`, `tests/test_linkage_scope.py`

**Tests:** cache stores *only* booleans per threshold; refuses to store a band, DOB, issuer, or timestamp; never re-verifies a user already known to meet a threshold (the sole permitted purpose, Art 28(4)); cache is invalidated on account deletion.

```python
def test_cache_refuses_identity_fields():
    with check_raises(TypeError):
        cache.store_threshold("acct-1", 18, band=Band.ADULT_18)      # band forbidden
    with check_raises(TypeError):
        cache.store_threshold("acct-1", 18, date_of_birth="2000-01-01")
    with check_raises(TypeError):
        cache.store_threshold("acct-1", 18, issuer="yoti")            # issuer forbidden
```

**Verify:** all raise; the happy path stores exactly `{18: True}`.

**Commit:** `feat(age): account-level threshold cache bounded to Art 28(4)`

---

### Task 6: OS / app-store signal ingest

**Files:** Create `app/signals/apple.py`, `google_play.py`, `amazon.py`, `tests/test_signals.py`

Start with **Amazon `GetUserAgeData`** — per Amazon's developer docs it is **already returning live responses for eligible Texas users**, so it is the one signal source you can integrate and observe today without waiting for the EU.

Bands returned by all three: 0–12, 13–15, 16–17, 18+.

**Tests:** map each provider band to the normalized `Band`; handle unknown/absent band conservatively (treat as `None`, never as adult); require the provider's stated consent basis before ingesting.

```python
def test_absent_signal_is_not_adult():
    check_eq(map_amazon_band(None), None, "an absent age signal must never default to adult")
```

That last test is a genuine safety property, not a formality: defaulting an unknown signal to adult is the exact failure mode both acts are written to prevent.

**Verify:** all provider mappings covered; `None` never becomes `ADULT_18`.

**Commit:** `feat(age): OS/app-store age signal ingest (Apple, Google Play, Amazon)`

---

### Task 7: EU proof-of-age attestation verification (D2)

**Files:** Create `app/signals/eu_eaa.py`, `tests/test_eu_eaa.py`

**Read before coding:** the open technical specification at `ageverification.dev` and Commission implementing Regulation (EU) 2025/1569. Do not code from this plan's summary alone.

**Tests:** a valid attestation from a certified issuer resolves to a band; an attestation from an unlisted issuer is rejected; a tampered payload fails signature verification; an expired attestation is rejected; verification **stores no identity document, no DOB, and no issuer-side identifier**.

```python
def test_rejects_unlisted_issuer():
    with check_raises(VerificationError):
        verify_attestation(UNLISTED_ISSUER_ATTESTATION)

def test_retains_no_identity():
    store = verify_attestation(VALID_ATTESTATION)
    check_eq(store.identity_fields(), set(), "no identity fields may be retained")
```

**Verify:** all pass. Record which attestation format and which issuer list snapshot you implemented against — this is the part most likely to shift as the Scheme's implementing acts land.

**Commit:** `feat(age): EU proof-of-age attestation verification per Art 3(o)/28(3)`

---

### Task 8: Guardian / parental responsibility (Art 26)

**Files:** Create `app/guardian.py`, `tests/test_guardian.py`

Art 26 permits three signals, in order: (a) signals from **freely accessible official online databases/interfaces made available by a Member State**; (b) signals already held from past engagements involving parental responsibility; (c) **self-declaration by the adult, at least until the delegated act under Art 26(5) is adopted**. Art 26(4) requires all of it be privacy-preserving, with no location/tracking/profiling of adult or minor.

This matters for a game because **Art 15(1)(d) makes access for under-13s conditional on guardian tools** — so EU under-13 access to Necroware is gated on this module working.

**Tests:** each of the three signal sources is accepted; self-declaration triggers "reasonable efforts to verify" per Art 26(3) and is flagged for review; no signal path stores adult location; a minor can never self-assert parental responsibility.

**Verify:** all pass.

**Commit:** `feat(age): parental responsibility verification (Art 26) gating under-13 game access (Art 15(1)(d))`

---

### Task 9: Safety-by-design defaults (Arts 8, 9, 11, 12, 14, 15)

**Files:** Create `app/safety_defaults.py`, `tests/test_safety_defaults.py`

Art 8(1): safety-by-design applies **by default regardless of whether the service is accessed with an account**, and a provider may only derogate after establishing the user is an adult via age assurance. So defaults are keyed on `is_adult`, not on account existence.

Encode, with an article citation per rule:
- **Art 11(1)** — off by default for minors: geolocation/tracking, mic/camera, account recommendations + contact sync, push notifications. Art 11(2): changeable only by a minor **over 15**, clearly informed, explicit consent.
- **Art 9(3)** — time-limited access and usage interruption, protecting school time and core sleep.
- **Art 12** — no unsolicited contact without pre-approval; not in contact recommendations; group adds need explicit agreement; anonymous blocking; contact details never disclosed; **no screenshot/download of minors' content**; no livestreaming by default.
- **Art 15(1)** — for games specifically: apply Art 9(1)+9(2)(b),(d) and 11(1) and 12(1),12(3)(c); **mandatory guardian tools**; under-13 access only via guardian tools.
- **Art 15(2)** — safeguard against enticing minors to move contacts to other services. Relevant if Necroware has any in-game chat or external-link feature.
- **Art 14(1)(b)** — for a shipped AI companion: by default **do not carry a minor's prior interactions into later ones**, except where needed for safety or to effect Art 11 settings. Art 14(1)(a): no design likely to create emotional dependency. Art 14(1)(e),(f): pre-launch evaluation and post-market monitoring.

**Tests:** every default is off for a minor; every default is on for an established adult; the over-15 consent gate rejects a 14-year-old's change to Art 11 defaults; an adult is *not* auto-derogated without a verified `ADULT_18`.

```python
def test_adult_derogation_requires_verified_age():
    p = profile_for(is_adult=True, verified=False)
    check(p["location_sharing"] is False,
          "Art 8(1): derogation requires established adulthood via age assurance")
```

**Verify:** all pass.

**Commit:** `feat(age): safety-by-design default profiles per Arts 8/9/11/12/14/15`

---

### Task 10: Complaint handling (Art 29(5))

**Files:** Create `app/appeals.py`, `tests/test_appeals.py`

Art 29(5) requires an **effective internal complaint-handling mechanism**, lodged **by electronic means and free of charge**, for users who believe an age-assurance outcome is incorrect. Art 19(3) additionally requires minors' reports be processed as a priority, with information about procedure and redress.

This is cheap to build and disproportionately valuable: a user wrongly gated out of the game is your most dissatisfied user, and an EU user with a working appeal path is a user who does not escalate to a national authority.

**Verify:** a free, no-login-required appeal can be lodged and reaches a resolved state; an adult wrongly denied access is restored.

**Commit:** `feat(age): free electronic appeal path for age-assurance outcomes (Art 29(5))`

---

### Task 11: Retention and deletion

**Files:** Create `app/retention.py`, `tests/test_retention.py`

Two regimes pull opposite ways and both must be satisfiable:
- **EU Art 28(2):** store no more than strictly necessary. Short verdict TTLs.
- **Texas ASAA:** data obtained for age verification may be used **for no purpose other than age verification**, and TX additionally requires **deletion** on completion of verification.

**Tests:** verdicts expire on TTL; account deletion cascades to the threshold cache; a job that finds retained age data past its purpose fails loudly rather than silently passing; the three OS/app-store data paths are all deletable on request.

**Verify:** all pass; run the expiry job twice and confirm idempotence.

**Commit:** `feat(age): retention TTLs and purpose-limited deletion (EU 28(2) / TX ASAA)`

---

### Task 12: Audit evidence log

**Files:** Create `app/audit.py`, `tests/test_audit.py`

Not the Art 5 VLOP compliance plan (out of scope). This is the ordinary evidentiary record: which assurance method produced each threshold, when, under which jurisdiction's rule — sufficient to answer a regulator's question months later.

**Tests:** an audit record is written for every threshold grant and every denial; records are append-only; **the audit log contains no identity data** (same anti-join discipline as Task 4).

**Verify:** all pass.

**Commit:** `feat(age): append-only audit log without identity data`

---

### Task 13: C++ engine client

**Files:** Create `Wasteland/src/Age/AgeAssurance.h`, `Wasteland/src/Age/AgeAssurance.cpp`; modify `premake5.lua` (Wasteland project, `files` list)

Thin client over the existing WinHTTP `HttpClient` (`DemonCore-Editor/src/Panels/HttpClient.h`) — it already does `Get`/`PostJson` with header maps, so this needs no new dependency.

```cpp
// Minimal client for the age-assurance service. Deliberately thin: it
// transports signed assertions and never inspects or stores identity data
// (EU KIDS Act proposal Art 28(1)-(2)). All age interpretation happens
// server-side so the two jurisdictions' rules stay in one place (D5).
class AgeAssurance
{
public:
    struct Result { bool Adult; bool Success; std::string Error; };
    // Resolves age from an OS/app-store signal and returns whether the
    // ADULT_18 threshold is met. Never returns a band or an exact age.
    Result ResolveAdultFromOsSignal(const std::string &osSignalToken);
};
```

**Step 1 — Build both configurations**

```bash
cd C:/dev/Wasteland
premake5 --file=premake5.lua
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" Wasteland.sln /p:Configuration=Release /v:minimal
```

Use your actual VS edition path — `msbuild` is **not** on `PATH` in this environment. Then repeat for `Debug`. Both must succeed, per your standing requirement.

**Step 2 — Verify no identity fields cross the boundary** — grep the client for anything resembling DOB or ID handling; there must be none.

**Commit:** `feat(engine): AgeAssurance client over WinHTTP HttpClient`

---

### Task 14: End-to-end flow test

**Files:** Create `services/age-verification/tests/test_e2e.py`

Full path: OS signal → verdict resolution → assertion → threshold cache → safety profile → adult gets unrestricted defaults, 12-year-old gets restricted defaults and guardian tooling, absent signal gets *neither* adult treatment nor a crash.

**Step 3 — Commit:** `test(age): end-to-end assurance flow across all four bands`

---

## 5. Validation

Run before considering any of this done:

| Check | Command | Expect |
|---|---|---|
| Service tests | `cd services/age-verification && uv run python -m tests.run` | all pass |
| Anti-join test has teeth | temporarily add `account_id` to verdict schema | Task 4 **must fail** |
| Engine Release | MSBuild `Wasteland.sln` `Configuration=Release` | success |
| Engine Debug | MSBuild `Wasteland.sln` `Configuration=Debug` | success |
| Engine headless tests | `set PYTHONPATH=C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor && python tests/test_visual_blocks.py` | pass |
| Identity-free boundary | grep service + client for `dob\|date_of_birth\|passport\|id_number` | only in *rejection* paths |

The mutation check in row 2 is the one that matters most. A privacy-preservation guarantee enforced only by code nobody has seen fail is a comment, not a control.

## 6. Files likely to change

**New:** `services/age-verification/**` (14 files), `Wasteland/src/Age/AgeAssurance.{h,cpp}`
**Modified:** `premake5.lua` (Wasteland `files` list, ~2 lines)
**Not modified:** any editor source. No age gates in `DemonCore-Editor` — it is a developer tool, out of scope per §1.

## 7. Risks and tradeoffs

1. **The EU proposal will change.** Parliament and Council have not adopted it. Article numbers and thresholds will move. Mitigation: `policy.py` is the single place thresholds and article references live; nothing else hardcodes 13/15/18 semantics. Re-check `2026/0286(COD)` before each release.
2. **Art 28(3) ZKP is the hardest requirement and may be impractical.** Almost no commercial vendor meets it today. This is the single largest technical risk in the plan. Mitigation: D2 — consume certified attestations rather than build provers; get vendor commitments in writing early; if the requirement proves unmeetable, your fallback is the Art 29(4) latitude plus documented Art 27 compliance and a documented residual-risk position.
3. **The 15-year boundary does not align between regimes** (D5). Mitigation: threshold predicate at the edge; never reconcile in storage.
4. **Texas data-purpose restriction** can conflict with fraud-prevention retention you may want. Read SB 2420's developer duties directly before designing any retention beyond verification.
5. **COPPA's deadline has already passed** (22 April 2026). If Necroware has or had any child-directed surface or collects under-13 personal information, this is live exposure today and outranks both pending bills. See §8.
6. **Vendor lock-in** if you buy assurance rather than consuming attestations. Prefer Scheme-certified issuers; keep the `AssuranceLevel` seam so a vendor can be swapped without touching `policy.py`.
7. **Scope creep into Art 5 VLOP obligations.** Explicitly out of scope. If the game ever crosses 45M EU MAU, that is a different and much larger project.

## 8. Open questions — answers change the plan

1. **Does Necroware currently collect personal information from under-13s, or have any child-directed surface?** If yes, the COPPA amended Rule is already overdue and this stops being readiness work. This is the highest-priority question.
2. **Is the target Necroware + its AI chat, or the Ashen AI Base chatbot?** The plan assumes the former. The latter drops Arts 9/11/12/15.
3. **Will Necroware ship an AI companion or general conversational chatbot?** If yes, Art 14 pre-launch evaluation (14(1)(e)) and post-market monitoring (14(1)(f)) are a substantial separate workstream — including a red-team exercise — that should be scoped explicitly rather than absorbed here.
4. **Is the under-13 EU audience in scope at all?** If Necroware is 16+ rated in the EU, Art 15(1)(d)'s guardian-tools requirement for under-13s becomes moot and `guardian.py` shrinks substantially.
5. **Are you willing to hold a commercial assurance vendor to Art 28(3)?** If not, decide now whether to rely on the Art 29(4) latitude with documented residual risk, or to defer EU launch.

## 9. Suggested sequencing

- **Weeks 1–2:** Tasks 1–5. Scaffold, thresholds, policy, and the two structural tasks (4 and 5) that are expensive to retrofit.
- **Weeks 3–4:** Tasks 6–7. Signal ingest and attestation verification. Task 7 carries the schedule risk — start the `ageverification.dev` reading in week 1 even though the code lands in week 4.
- **Weeks 5–6:** Tasks 8–12. Guardian verification, safety defaults, appeals, retention, audit.
- **Week 7:** Tasks 13–14. Engine client and end-to-end.
- **Then:** re-read `2026/0286(COD)` and re-run the compliance mapping in §0 against whatever the current legislative state is.

Nothing here requires the game to be online. The backend is useful offline as an internal compliance record and useful online as an age gate — and §0 is the reason to start now rather than when either act is final.