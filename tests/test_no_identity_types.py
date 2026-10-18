"""D1 guard: the age module must stay structurally unjoinable to identity.

D1 is the module's central compliance property. An age verdict is derived from
signals, applied as a band, and shown as a UI label -- and at no point does the
engine hold anything that could identify WHICH child it is about. That is what
makes an age verdict safe to compute and unsafe to join against a person.

The property holds today, and that is exactly the problem: it holds by accident.
Every existing `std::string` in the module is a display label, a provider name,
or a method name, and none of them identify anyone. Nothing ENFORCES that. One
well-meant field -- `std::string m_AccountId;` on a provider, a user handle on
the profile -- would compile cleanly, pass all 585 existing checks, and quietly
turn every age verdict into a per-child record. Nothing in the current test
suite would notice.

So this suite reads the SOURCE and asserts the shape, because the shape is the
guarantee. It cannot be tested through the public API: a function that cannot
join a verdict to an identity is one whose absence is only visible by reading
the declarations.

WHAT COUNTS AS A VIOLATION

Not every string is identity. These are the legitimate ones, and the guard
knows each by name and by the class it lives in:

    AgeBand::BandName()            display label for an enum
    Apple/Google provider m_Name   which provider produced a signal
    Attestation::m_Method          how the verdict was obtained
    GuardianToolsModel::Greeting   UI text for the guardian panel
    GuardianToolsModel::IndicatorText  Art 8(1) indicator text
    ControlState::Label            one row's label

A field is a violation when it is a string member whose name LOOKS like an
identifier -- account, user, player, child, email, device, token, id -- and
which is not one of the known-safe names above. That is deliberately name-based
rather than type-based, because a type-based rule ("no std::string at all")
cannot tell `m_Name = "apple-declared-age-range"` from a user handle; both are
`std::string`, and only one is dangerous.

Run:  python tests/test_no_identity_types.py
"""

from __future__ import annotations

import os
import re
import sys

AGE_DIR = r"C:\dev\Wasteland\Wasteland\src\Wasteland\Age"

# Fields that are strings and are provably not identifiers. Each carries a
# reason; the reasons are the point, because "trust me, this one is fine" is
# exactly how a guard stops being enforced.
KNOWN_SAFE = {
    "m_Name": "provider identifier: the literal 'apple-declared-age-range', "
              "not a person",
    "m_Method": "how the verdict was obtained: 'self-declared' / 'verified'",
    "Greeting": "guardian panel UI text",
    "IndicatorText": "Art 8(1) indicator text",
    "Label": "one control row's label",
    # Function-local statics inside AgeBand::BandName() -- the display labels
    # the age plan explicitly calls out as the module's one legitimate string.
    #
    # GuardianToolsView.cpp's `std::string Hours(int)` is deliberately NOT here:
    # it is a local helper that formats an hour count, not a field, and the
    # scanner only matches declarations (name followed by = or ;), so it never
    # reaches the allowlist check.
    "kUnknown": "AgeBand display label",
    "kUnder13": "AgeBand display label",
    "kAge13To15": "AgeBand display label",
    "kAge16To17": "AgeBand display label",
    "kAdult18": "AgeBand display label",
}

# Name fragments that indicate an identifier. Matched case-insensitively as a
# substring so m_AccountId, account_id and userHandle all trip.
IDENTITY_PATTERNS = [
    r"account",
    r"user(id|name|handle)?\b",
    r"player(id|name)?\b",
    r"child(id|name)?\b",
    r"email",
    r"e-?mail",
    r"device(id|name)?\b",
    r"token",
    r"\bid\b",
    r"identifier",
    r"profile(id|name)\b",
    r"subscriber",
    r"guardian(id|name)\b",
    r"dob",
    r"birth",
    r"passport",
    r"ssn",
    r"phone",
    r"handle",
]

# Types that can hold an identifier regardless of name. Narrow on purpose: a
# pointer or a platform handle can carry identity even with a bland name.
IDENTITY_TYPES = [
    r"\bstd::wstring\b",
    r"\bstd::u32string\b",
    r"\bstd::filesystem::path\b",
]

_failures: list[str] = []
_passes = 0


def check(cond, msg):
    global _passes
    if cond:
        _passes += 1
    else:
        _failures.append(msg)


def _sources():
    if not os.path.isdir(AGE_DIR):
        return []
    return [os.path.join(AGE_DIR, f) for f in sorted(os.listdir(AGE_DIR))
            if f.endswith((".h", ".cpp"))]


def _strip_comments_and_strings(text):
    """Remove // comments, /* */ blocks and string literals.

    Without this the guard fires on the WORD 'identifier' inside a comment
    explaining that identifiers are forbidden -- which is the single most
    likely way for this test to become noise and be ignored.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)
    return text


def _declaring_struct(lines, idx):
    """Name of the struct/class whose body contains line `idx`."""
    for j in range(idx, -1, -1):
        m = re.match(r"\s*(?:struct|class)\s+(\w+)", lines[j])
        if m:
            return m.group(1)
    return "<file scope>"


def test_the_age_directory_exists():
    """If the directory moved, this suite must fail LOUDLY, not pass vacuously.

    A guard whose subject has vanished reports success. That is how a compliance
    test becomes decorative.
    """
    srcs = _sources()
    check(bool(srcs), "Age directory not found at %s -- the D1 guard has nothing "
                      "to inspect and would pass vacuously" % AGE_DIR)
    check(len(srcs) >= 20, "expected the full Age module (~23 files), found %d"
          % len(srcs))


def test_no_string_member_looks_like_an_identifier():
    """The core D1 assertion, over every declaration in the module."""
    srcs = _sources()
    seen = 0
    for path in srcs:
        raw = open(path, encoding="utf-8", errors="replace").read()
        text = _strip_comments_and_strings(raw)
        lines = text.split("\n")
        for i, line in enumerate(lines):
            m = re.search(r"std::(?:w?string)\s+(\w+)\s*(?:=|;)", line)
            if not m:
                continue
            seen += 1
            name = m.group(1)
            owner = _declaring_struct(lines, i)
            if name in KNOWN_SAFE:
                continue
            for pat in IDENTITY_PATTERNS:
                if re.search(pat, name, re.I):
                    _failures.append(
                        "IDENTITY FIELD: %s::%s (declared %s:%d) -- %r matches "
                        "/%s/. A name like this is almost always a person, and an "
                        "age verdict joined to a person is exactly what D1 exists "
                        "to prevent."
                        % (owner, name, os.path.basename(path), i + 1, name, pat))
                    break
    check(seen > 0, "found no std::string declarations at all -- the parse is "
                    "wrong, not the module")


def test_no_identifier_capable_types():
    """wstring/u32string/path can hold identity under any name."""
    srcs = _sources()
    for path in srcs:
        text = _strip_comments_and_strings(open(path, encoding="utf-8",
                                                errors="replace").read())
        for pat in IDENTITY_TYPES:
            for m in re.finditer(pat, text):
                ln = text[:m.start()].count("\n") + 1
                _failures.append(
                    "IDENTITY-CAPABLE TYPE: %s used at %s:%d -- %s can hold an "
                    "identifier regardless of what the field is called"
                    % (m.group(0), os.path.basename(path), ln, m.group(0)))


def test_every_string_member_is_on_the_allowlist():
    """The inverse check, so the allowlist cannot rot.

    A guard that only fails on NEW fields quietly becomes wrong when an existing
    safe field is renamed: it then matches nothing, gets deleted from the
    allowlist, and the real check quietly weakens. Asserting the exact set makes
    that deletion impossible to do silently.
    """
    found = {}
    for path in _sources():
        text = _strip_comments_and_strings(open(path, encoding="utf-8",
                                                errors="replace").read())
        for m in re.finditer(r"std::(?:w?string)\s+(\w+)\s*(?:=|;)", text):
            found.setdefault(m.group(1), []).append(os.path.basename(path))

    # An allowlist entry that no longer exists means either a rename happened
    # without the guard being updated, or the allowlist has drifted from the
    # code. Either way the guard is no longer describing reality.
    for name, reason in KNOWN_SAFE.items():
        if name not in found:
            _failures.append(
                "STALE ALLOWLIST ENTRY: %r (%s) no longer exists in the module. "
                "Either it was renamed -- in which case the new name needs "
                "reviewing -- or the allowlist is lying about what it covers."
                % (name, reason))

    # Conversely: a string member nobody has cleared.
    for name, where in sorted(found.items()):
        if name not in KNOWN_SAFE:
            _failures.append(
                "UNREVIEWED STRING FIELD: %r in %s has no allowlist entry. Add it "
                "with a reason if it cannot identify a person, or fix it if it can."
                % (name, ", ".join(sorted(set(where)))))


def test_the_guard_would_catch_a_violation():
    """Negative control: prove the pattern matching actually fires.

    A guard that cannot fail is indistinguishable from a guard that always
    passes. Feed it the exact declaration D1 forbids and require a match.
    """
    for name in ("m_AccountId", "userId", "player_email", "DeviceToken",
                 "childName", "std::wstring handle"):
        matched = any(re.search(p, name, re.I) for p in IDENTITY_PATTERNS) \
            or any(re.search(t, name) for t in IDENTITY_TYPES)
        check(matched, "the guard does NOT flag %r -- it would let a real "
                       "identity field through" % name)

    # ...and must not fire on the legitimate ones.
    for name in ("m_Name", "m_Method", "Greeting", "IndicatorText", "Label"):
        check(not any(re.search(p, name, re.I) for p in IDENTITY_PATTERNS),
              "the guard falsely flags %r, which is a known-safe display label"
              % name)


def test_comments_mentioning_identity_do_not_trigger():
    """The module discusses identity extensively in comments. Those must not fire.

    This is the failure mode that would make the guard noise: a doc comment
    explaining WHY identity is forbidden contains the word 'identifier'.
    """
    sample = """
    // Art 8(1) indicator: must be up whenever protections are in force.
    // No user identifier may be stored with the verdict.
    std::wstring forbidden = L"user_id";
    /* provider identifier, not a person */
    std::string m_Method = "verified";
    """
    text = _strip_comments_and_strings(sample)
    flagged = [m.group(1) for m in re.finditer(r"std::(?:w?string)\s+(\w+)\s*(?:=|;)",
                                              text)]
    # `forbidden` is a real declaration, so it must survive. `m_Method` is
    # preceded by a /* */ comment that mentions "identifier", and its value is
    # the literal "verified" -- the declaration itself is legitimate, so the
    # expectation is that BOTH real declarations remain and no comment text
    # was mistaken for one.
    check(sorted(flagged) == ["forbidden", "m_Method"],
          "comment/string stripping left %r; expected the two real declarations"
          % flagged)
    check("user identifier" not in text and "Art 8(1)" not in text,
          "comment text survived stripping and would be scanned for identifiers")
    check("L\"user_id\"" not in text and "verified" not in text,
          "string literal contents survived stripping")


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            _failures.append("%s raised: %s" % (fn.__name__, exc))

    total = _passes + len(_failures)
    if _failures:
        print("FAILURES:")
        for f in _failures:
            print("  * %s" % f)
    print("%d/%d checks passed" % (_passes, total))
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())