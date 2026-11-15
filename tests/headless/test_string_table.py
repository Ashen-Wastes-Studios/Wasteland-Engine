"""String table + pseudo-localization (Phase J.7).

The plan's "all editor strings go through a string table" is a one-time sweep
over the editor panels -- editor work. The table itself and the pseudo-LOC pass
are engine behaviour tested here.
"""
import atexit
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

StringTable = Wasteland.StringTable
PseudoLocalizeText = Wasteland.PseudoLocalizeText


def test_translate_returns_the_base_text():
    table = StringTable()
    table.SetBase("file.open", "Open File")
    check_eq(table.Translate("file.open"), "Open File")


def test_a_missing_key_returns_the_key_not_empty():
    """An empty string is indistinguishable from a real empty value; a literal
    key is a visible 'this bypassed the table' defect."""
    table = StringTable()
    check_eq(table.Translate("missing.key"), "missing.key")


def test_has_and_count_reflect_the_base_entries():
    table = StringTable()
    table.SetBase("a", "A")
    table.SetBase("b", "B")
    table.SetBase("b", "B2")  # overwrite, not a new key
    check(table.Has("a") and table.Has("b"), "Has disagrees with SetBase")
    check(not table.Has("c"), "Has reports a key that was never set")
    check_eq(table.Count(), 2, "overwriting a key grew the table")


def test_the_active_locale_override_applies():
    table = StringTable()
    table.SetBase("file.open", "Open File")
    table.SetActiveLocale("fr")
    table.SetLocaleOverride("fr", "file.open", "Ouvrir le fichier")
    check_eq(table.Translate("file.open"), "Ouvrir le fichier")


def test_switching_away_from_the_locale_clears_it():
    table = StringTable()
    table.SetBase("file.open", "Open File")
    table.SetActiveLocale("fr")
    table.SetLocaleOverride("fr", "file.open", "Ouvrir le fichier")
    check_eq(table.Translate("file.open"), "Ouvrir le fichier")
    table.SetActiveLocale("de")
    table.SetLocaleOverride("de", "file.open", "Datei öffnen")
    check_eq(table.Translate("file.open"), "Datei öffnen", "the active locale leaked")


def test_an_override_for_an_inactive_locale_is_ignored():
    table = StringTable()
    table.SetBase("file.open", "Open File")
    table.SetLocaleOverride("fr", "file.open", "Ouvrir")  # fr not active
    check_eq(table.Translate("file.open"), "Open File",
             "an inactive locale's override was applied")


def test_pseudo_localization_expands_every_string():
    """The plan: 'a pseudo-localization pass (expand every string by 30%) that
    exposes hardcoded strings and truncated UI'. The growth is BYTES (UTF-8 -- the
    width that truncates in a fixed panel), not code points, so the assertion
    measures `.encode('utf-8')`. Python's len() of a str counts code points, where
    the 2-byte accents count as 1."""
    for text in ("Open File", "New Project", "Save Scene", "Settings", "X"):
        pseudo = PseudoLocalizeText(text)
        base_bytes = len(text.encode("utf-8"))
        pseudo_bytes = len(pseudo.encode("utf-8"))
        check(pseudo_bytes >= base_bytes * 1.30,
              "'%s' did not expand by 30%% in bytes (%d -> %d)"
              % (text, base_bytes, pseudo_bytes))


def test_pseudo_localization_brackets_so_truncation_shows():
    pseudo = PseudoLocalizeText("File")
    check(pseudo.startswith("["), "pseudo-localized text is not bracketed")
    check(pseudo.endswith("]"), "pseudo-localized text is not bracketed")


def test_pseudo_localization_is_deterministic_and_distinct():
    a = PseudoLocalizeText("Open File")
    b = PseudoLocalizeText("Open File")
    check_eq(a, b, "pseudo-localization is not deterministic")
    check(a != "Open File", "pseudo-localization left the string unchanged")


def test_pseudo_run_covers_every_base_key_in_insertion_order():
    table = StringTable()
    table.SetBase("one", "One Thing")
    table.SetBase("two", "Two Things")
    run = table.PseudoRun()
    check_eq(len(run), 2, "PseudoRun missed a key")
    keys = [key for key, _ in run]
    check_eq(keys, ["one", "two"], "PseudoRun is not in insertion order")
    for key, pseudo in run:
        check(pseudo.startswith("["), "PseudoRun entry '%s' is not pseudo-LOC'd" % key)


# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------

def _main():
    tests = [(name, obj) for name, obj in sorted(globals().items())
             if name.startswith("test_") and callable(obj)]

    failed = 0
    for name, test in tests:
        try:
            test()
            print(f"  {name} ...")
        except AssertionError as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {error}")
        except Exception as error:
            failed += 1
            print(f"  {name} ... FAIL")
            print(f"      {type(error).__name__}: {error}")

    print(f"\n{len(tests) - failed}/{len(tests)} tests passed")
    print(f"{len(tests) - failed}/{len(tests)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main())
