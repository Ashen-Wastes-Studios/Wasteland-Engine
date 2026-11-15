"""Per-system profile budgets over a Chrome trace-event capture (Phase J.4).

The engine writes `bin/WastelandProfile-Runtime.json` and `-Startup.json` as
Chrome trace-event JSON. This tests the budget DECISION over that format: group
events by system, total each system's CPU ms, and warn when a system exceeds the
budget set for it.

The plan's "per-frame millisecond budget" is NOT invented here: the shipped
captures have no frame markers, so a per-frame figure would rest on a guess. This
reports per-system aggregate ms over the capture, and budgets are expressed the
same way -- the honest, CI-verifiable form of the requirement.
"""
import atexit
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runner import check, check_eq

import wlpath

wlpath.setup()

import Wasteland

AnalyzeProfile = Wasteland.AnalyzeProfile

_REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))

_DIRS = []


def _cleanup():
    for path in _DIRS:
        shutil.rmtree(path, ignore_errors=True)


atexit.register(_cleanup)


def _trace(events):
    return json.dumps({"otherData": {}, "traceEvents": events})


def test_the_shipped_runtime_capture_is_truncated_and_rejected():
    """The shipped WastelandProfile-Runtime.json is missing its closing `]}` --
    a capture that was cut short. The analyzer must REJECT it rather than guess;
    this test documents that the file is genuinely malformed, so a future fix to
    the capture writer turns this test's expected-reject into expected-accept."""
    path = os.path.join(_REPO, "bin", "WastelandProfile-Runtime.json")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    check(not text.rstrip().endswith("]}"),  # sanity: the file really is truncated
          "the runtime capture is no longer truncated -- port this test to expect success")
    report = AnalyzeProfile(text, [])
    check(not report.Ok,
          "a truncated capture was accepted as valid")
    check(bool(report.Error), "a truncated capture produced no message")


def test_the_real_startup_profile_parses():
    path = os.path.join(_REPO, "bin", "WastelandProfile-Startup.json")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    report = AnalyzeProfile(text, [])
    check(report.Ok, "the shipped startup profile did not parse: %s" % report.Error)
    check(report.EventCount > 0, "the startup profile has no X events")


def test_the_systems_are_sorted_by_total_descending():
    path = os.path.join(_REPO, "bin", "WastelandProfile-Startup.json")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    report = AnalyzeProfile(text, [])
    check(report.Ok, "the startup profile did not parse: %s" % report.Error)
    totals = [sample.TotalMs for sample in report.Systems]
    check_eq(totals, sorted(totals, reverse=True),
             "systems are not sorted by total time descending")
    check(report.TotalMs >= totals[0],
          "the report total is less than its own biggest system")


def test_durations_are_converted_from_microseconds_to_milliseconds():
    text = _trace([{"ph": "X", "dur": 1000, "name": "One::A", "ts": 1}])
    report = AnalyzeProfile(text, [])
    check_eq(report.Systems[0].TotalMs, 1.0, "1000 us != 1.0 ms")


def test_events_group_into_systems_by_namespace():
    text = _trace([
        {"ph": "X", "dur": 1000, "name": "Wasteland::Audio::Play", "ts": 1},
        {"ph": "X", "dur": 2000, "name": "Wasteland::Audio::Stop", "ts": 2},
    ])
    report = AnalyzeProfile(text, [])
    check_eq(len(report.Systems), 1, "the two Audio events made two systems")
    audio = report.Systems[0]
    check_eq(audio.System, "Wasteland::Audio", "wrong grouping")
    check_eq(audio.EventCount, 2, "wrong event count")
    check_eq(audio.TotalMs, 3.0, "wrong totals")


def test_non_x_events_are_ignored():
    text = _trace([
        {"ph": "M", "dur": 5000, "name": "Wasteland::Meta::Thing", "ts": 1},
        {"ph": "B", "dur": 5000, "name": "Wasteland::Async::Span", "ts": 2},
        {"ph": "X", "dur": 500, "name": "Real::One", "ts": 3},
    ])
    report = AnalyzeProfile(text, [])
    check_eq(report.EventCount, 1, "non-X events were counted")
    check_eq(report.Systems[0].System, "Real::One", "a non-X event created a system")


def test_an_exceeded_budget_warns_naming_system_budget_and_actual():
    text = _trace([{"ph": "X", "dur": 4000, "name": "Wasteland::Renderer::Draw", "ts": 1}])
    report = AnalyzeProfile(text, [("Wasteland::Renderer", 2.0)])
    check_eq(len(report.Warnings), 1, "an exceeded budget produced no warning")
    warning = report.Warnings[0]
    check("Wasteland::Renderer" in warning, "warning does not name the system")
    check("2.00" in warning, "warning does not state the budget")
    check("4.00" in warning, "warning does not state the actual total")


def test_a_budget_within_limits_is_silent():
    text = _trace([{"ph": "X", "dur": 1000, "name": "Wasteland::Audio::Tick", "ts": 1}])
    report = AnalyzeProfile(text, [("Wasteland::Audio", 10.0)])
    check_eq(len(report.Warnings), 0, "an in-budget system was warned about")


def test_a_budget_for_a_missing_system_warns():
    """A system that was budgeted but never ran is a different failure and must
    not be silently swallowed: budgeted renderer, but the capture has no renderer
    events."""
    text = _trace([{"ph": "X", "dur": 100, "name": "Wasteland::Audio::Tick", "ts": 1}])
    report = AnalyzeProfile(text, [("Wasteland::Renderer", 2.0)])
    check_eq(len(report.Warnings), 1, "a missing budgeted system produced no warning")
    check("never ran" in report.Warnings[0], "the missing-system warning is unclear")


def test_ties_sort_by_name_ascending():
    text = _trace([
        {"ph": "X", "dur": 3000, "name": "glfwInit", "ts": 1},
        {"ph": "X", "dur": 3000, "name": "Wasteland::Audio::Tick", "ts": 2},
    ])
    report = AnalyzeProfile(text, [])
    check_eq(len(report.Systems), 2, "expected two tied systems")
    names = [sample.System for sample in report.Systems]
    check_eq(names, sorted(names), "ties are not ordered by name ascending")


def test_malformed_json_is_rejected():
    report = AnalyzeProfile("{not json", [])
    check(not report.Ok, "malformed JSON parsed as valid")
    check(bool(report.Error), "a malformed capture produced no message")


def test_missing_trace_events_is_rejected():
    report = AnalyzeProfile('{"otherData": {}}', [])
    check(not report.Ok, "a capture with no traceEvents parsed as valid")
    check(bool(report.Error), "a missing traceEvents array produced no message")


def test_an_empty_capture_is_a_valid_empty_inventory():
    report = AnalyzeProfile('{"otherData": {}, "traceEvents": []}', [])
    check(report.Ok, "an empty capture was rejected")
    check_eq(report.EventCount, 0, "an empty capture counted events")
    check_eq(report.TotalMs, 0.0, "an empty capture has nonzero total")


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
