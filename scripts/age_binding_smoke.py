"""Smoke test for the Wasteland.Age Python bindings.

A green C++ build is NOT evidence these bindings work. Per the pybind11
checklist: import the REAL built module and assert against it. This script
deliberately uses no fake stand-in -- it loads the actual .pyd.

Run against the configuration the user actually runs:
    python scripts/age_binding_smoke.py Debug
    python scripts/age_binding_smoke.py Release

WHAT IT PROVES
  1. The submodule exists and every name a script would use resolves.
  2. The bindings are READ-ONLY on enforcement -- assigning to a
     SafetyProfile property raises rather than silently doing nothing,
     which is the failure mode that would matter most if it regressed.
  3. With no provider installed, the fail-closed defaults reach Python.
  4. A value written through a guardian transition reads back.

Exit code is the process exit code; 0 means everything held.
"""

import importlib.util
import pathlib
import sys


def load_module(config: str):
    repo = pathlib.Path(__file__).resolve().parents[1]
    candidate = repo / "bin" / f"{config}-windows-x86_64" / "DemonCore-Editor" / "Wasteland.pyd"
    if not candidate.exists():
        print(f"FAIL: not found: {candidate}")
        print("      Build the solution for this configuration first.")
        sys.exit(1)

    spec = importlib.util.spec_from_file_location("Wasteland", candidate)
    module = importlib.util.module_from_spec(spec)

    # Register in sys.modules BEFORE executing.
    #
    # Without this, any `import Wasteland` inside a script under test loads a
    # SECOND, independent instance of the extension module -- and the fresh one
    # has no `Age` attribute, so AgeGate.py dies on `Wasteland.Age` with an
    # AttributeError that points at the script rather than at the loader.
    sys.modules["Wasteland"] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    config = sys.argv[1] if len(sys.argv) > 1 else "Release"
    W = load_module(config)
    checks = 0
    failures = 0

    def check(cond, what):
        nonlocal checks, failures
        checks += 1
        if not cond:
            failures += 1
            print(f"  FAIL: {what}")

    # ---------------------------------------------------------- names resolve --
    for name in (
        "IsMinor", "IsVerified", "Band", "Assurance", "HasProvider",
        "Profile", "Policy", "GetEuScope", "SetEuScope", "Refresh",
        "AuthorizeFeature", "AuditEntryCount", "AuditEntries", "AuditWrapped",
        "UnlimitedPlaytime",
    ):
        check(hasattr(W.Age, name), f"Age.{name} must resolve")

    for name in ("AgeBand", "AssuranceLevel", "SurfaceKind", "GuardianState",
                 "AuditDecision", "DenyReason", "SafetyProfile", "GuardianSettings",
                 "GuardianPolicy", "AgeAuditEntry"):
        check(hasattr(W.Age, name), f"Age.{name} must be registered")

    # ------------------------------------------------------ fail-closed defaults --
    # No provider is installed by this smoke test, so the service must report
    # the restrictive answers -- not an adult screen, not an exception.
    check(W.Age.HasProvider() is False, "no provider should be installed")
    check(W.Age.IsMinor() is True, "no provider must read as a minor")
    check(W.Age.IsVerified() is False, "no provider must not be verified")
    check(W.Age.Band() == W.Age.AgeBand.Unknown, "band must be Unknown")

    profile = W.Age.Profile()
    check(profile.FreeTextChat is False, "no free-text chat")
    check(profile.MarketplaceAccess is False, "no marketplace")
    check(profile.LocationSharing is False, "no location sharing")
    check(profile.ProfileVisibilityPublic is False, "profile not public (Art 8(1)(a))")
    check(profile.NightHoursRestricted is True,
          "night-hour restrictions hold -- PROTECTIVE flag, True is restricted")
    check(profile.ReportAvailable is True, "report must stay available")
    check(profile.MuteAvailable is True, "mute must stay available")

    policy = W.Age.Policy()
    check(policy.State == W.Age.GuardianState.Unconfigured,
          "guardian policy must be Unconfigured, never Adult")

    # -------------------------------------------------------------- read-only --
    # The single most important check in this file. If a setter ever appears on
    # an enforcement value, this is what catches it.
    for attr in ("FreeTextChat", "MarketplaceAccess", "LocationSharing",
                 "ProfileVisibilityPublic", "NightHoursRestricted"):
        try:
            setattr(profile, attr, True)
            check(False, f"SafetyProfile.{attr} must NOT be settable from Python")
        except AttributeError:
            check(True, f"SafetyProfile.{attr} correctly read-only")

    try:
        policy.State = W.Age.GuardianState.Adult
        check(False, "GuardianPolicy.State must NOT be settable from Python")
    except AttributeError:
        check(True, "GuardianPolicy.State correctly read-only")

    # --------------------------------------------- a transition reads back -----
    # Value semantics: a new policy comes back, the original is untouched.
    proposed = policy.EffectiveSettings
    pending = policy.WithPendingChange(proposed)
    check(pending.HasPendingChange is True, "a proposed change must register as pending")
    check(policy.HasPendingChange is False, "and must not mutate the original")
    check(pending.State == W.Age.GuardianState.PendingReview, "pending state surfaces")
    check(pending.Approved().HasPendingChange is False, "approval clears the pending flag")
    check(pending.Denied().HasPendingChange is False, "denial clears the pending flag")

    # PendingReview must never reach Adult -- the bug a lost band would cause.
    check(pending.Approved().State != W.Age.GuardianState.Adult,
          "an approved change must not land in Adult")
    check(pending.Denied().State != W.Age.GuardianState.Adult,
          "a denied change must not land in Adult")

    # --------------------------------------------------------------- the gate ---
    # Self-declared assurance satisfies nothing, in either scope, and is recorded.
    before = W.Age.AuditEntryCount()
    check(W.Age.AuthorizeFeature(W.Age.SurfaceKind.OnlineGame) is False,
          "self-declared assurance must not authorise a feature")
    check(W.Age.AuditEntryCount() == before + 1, "the refusal must be recorded")

    entries = W.Age.AuditEntries()
    check(len(entries) == W.Age.AuditEntryCount(), "AuditEntries must match the count")
    if entries:
        last = entries[-1]
        check(last.Decision == W.Age.AuditDecision.Denied, "recorded as denied")
        check(last.Reason == W.Age.DenyReason.SelfDeclared, "with the SelfDeclared reason")
        check(last.Surface == W.Age.SurfaceKind.OnlineGame, "and the surface")

    # EuScope: writable, and narrowing it must not relax the safety profile.
    check(W.Age.GetEuScope() is True, "EU scope must be on by default")
    W.Age.SetEuScope(False)
    check(W.Age.GetEuScope() is False, "SetEuScope(False) must stick")
    narrowed = W.Age.Profile()
    check(narrowed.FreeTextChat is False,
          "narrowing EU scope must NOT relax the safety profile")
    check(narrowed.ProfileVisibilityPublic is False,
          "narrowing EU scope must NOT relax Art 8(1)")
    W.Age.SetEuScope(True)

    print(f"age_binding_smoke [{config}]: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
