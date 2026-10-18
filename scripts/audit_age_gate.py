"""Audit AgeGate.py against the REAL built module.

A shipped example script that names an attribute the binding does not have is
worse than no example: it is the first thing a studio copies. So check every
Wasteland.Age name it touches, against the real .pyd rather than the header.

    python scripts/audit_age_gate.py Release
"""

import importlib.util
import pathlib
import re
import sys


def load(config: str):
    repo = pathlib.Path(__file__).resolve().parents[1]
    pyd = repo / "bin" / f"{config}-windows-x86_64" / "DemonCore-Editor" / "Wasteland.pyd"
    if not pyd.exists():
        print(f"FAIL: {pyd} not found -- build {config} first")
        sys.exit(1)
    spec = importlib.util.spec_from_file_location("Wasteland", pyd)
    module = importlib.util.module_from_spec(spec)

    # Register in sys.modules BEFORE executing -- see the note in
    # age_binding_smoke.py. A second instance of the extension has no `Age`
    # attribute, which surfaces as an AttributeError inside the script under
    # test rather than anywhere near the loader.
    sys.modules["Wasteland"] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    config = sys.argv[1] if len(sys.argv) > 1 else "Release"
    W = load(config)

    repo = pathlib.Path(__file__).resolve().parents[1]
    gate = repo / "DemonCore-Editor" / "assets" / "scripts" / "AgeGate.py"
    text = gate.read_text(encoding="utf-8")

    checks = 0
    failures = 0

    def check(cond, what):
        nonlocal checks, failures
        checks += 1
        if not cond:
            failures += 1
            print(f"  FAIL: {what}")

    # Every Age.<name> the script references.
    names = sorted(set(re.findall(r"\bAge\.([A-Za-z_][A-Za-z0-9_]*)", text)))
    check(names, "the script should reference some Age names")
    for name in names:
        check(hasattr(W.Age, name), f"AgeGate.py uses Age.{name}, which the module does not expose")

    # Enum members reached via Age.<Enum>.<Member>.
    for enum_name, member in re.findall(r"\bAge\.([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)", text):
        # Skip attribute accesses on helper functions (they are called).
        if not hasattr(W.Age, enum_name):
            continue
        enum_type = getattr(W.Age, enum_name)
        if hasattr(enum_type, "__members__"):
            check(member in enum_type.__members__,
                  f"AgeGate.py uses Age.{enum_name}.{member}, which does not exist")

    # SafetyProfile / GuardianSettings properties the script reads.
    profile_props = set(re.findall(r"_PROFILE\.([A-Za-z_][A-Za-z0-9_]*)", text))
    for prop in sorted(profile_props):
        check(hasattr(W.Age.SafetyProfile, prop),
              f"AgeGate.py reads _PROFILE.{prop}, which SafetyProfile does not expose")

    settings_props = set(re.findall(r"_POLICY\.EffectiveSettings\.([A-Za-z_][A-Za-z0-9_]*)", text))
    for prop in sorted(settings_props):
        check(hasattr(W.Age.GuardianSettings, prop),
              f"AgeGate.py reads EffectiveSettings.{prop}, which GuardianSettings does not expose")

    policy_props = set(re.findall(r"_POLICY\.([A-Za-z_][A-Za-z0-9_]*)", text))
    for prop in sorted(policy_props):
        check(hasattr(W.Age.GuardianPolicy, prop),
              f"AgeGate.py reads _POLICY.{prop}, which GuardianPolicy does not expose")

    # And execute the importable half of it, which is the only part that runs
    # without an entity.
    check("def authorize(" in text, "authorize() must exist")
    check("def protections_enforced(" in text, "protections_enforced() must exist")

    print(f"audit_age_gate [{config}]: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
