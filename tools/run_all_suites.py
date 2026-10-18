"""Run the repository Python suites with honest per-suite counts.

The promoted Visual Blocks checks print PASS/FAIL markers; top-level tests also
have explicit summaries or unittest output, while tests/headless uses its own
module-name runner. Each interface is invoked on its own rather than mixing the
incompatible runner APIs. Output without any recognized count is UNCOUNTED and
fails the aggregate instead of being treated as a green zero.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = REPO_ROOT / "tests"
VISUALBLOCKS_DIR = TESTS_DIR / "visualblocks"
HEADLESS_DIR = TESTS_DIR / "headless"

EXPECTED_PROMOTED_COUNTS = {
    "test_node_library.py": 1209,
    "test_wlbtree_compiler.py": 56,
    "test_visualblocks.py": 101,
    "test_enemy_tree.py": 61,
}
EXPECTED_PROMOTED_TOTAL = sum(EXPECTED_PROMOTED_COUNTS.values())

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
PASS_RE = re.compile(r"^\s*PASS(?:[: ]|$)")
OK_CHECK_RE = re.compile(r"^\s*ok\s+\S", re.I)
FAIL_RE = re.compile(r"^\s*FAIL(?:[: ]|$)")
CHECK_SUMMARY_RE = re.compile(r"^\s*(\d+)\s*/\s*(\d+)\s+checks passed\b", re.I)
COUNT_SUMMARY_RE = re.compile(r"^\s*(\d+)\s+passed,\s*(\d+)\s+failed\s*$", re.I)
VISUAL_SUMMARY_RE = re.compile(r"^\s*passed\s+(\d+),\s*failed\s+(\d+)\s*$", re.I)
UNITTEST_RAN_RE = re.compile(r"^\s*Ran\s+(\d+)\s+tests?\b", re.I)
UNITTEST_FAILURE_RE = re.compile(r"\b(failures|errors)=(\d+)\b", re.I)
HEADLESS_SUITE_RE = re.compile(r"^\s*(\d+)\s*/\s*(\d+)\s+passed\s+([\w.-]+)\s*$")
HEADLESS_TOTAL_RE = re.compile(
    r"^\s*(\d+)\s*/\s*(\d+)\s+checks passed across (\d+) suite\(s\)(?: in [0-9.]+s)?\s*$"
)


def _lines(output: str) -> list[str]:
    """Normalize CRLF, bare CR, and ANSI colour sequences before parsing."""
    output = ANSI_RE.sub("", output)
    return output.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def marker_counts(output: str) -> tuple[int, int] | None:
    """Count explicit PASS/FAIL lines; return None when neither exists."""
    lines = _lines(output)
    passed = sum(1 for line in lines
                 if PASS_RE.match(line) or OK_CHECK_RE.match(line))
    failed = sum(1 for line in lines if FAIL_RE.match(line))
    return (passed, failed) if passed + failed else None


def parse_output(output: str) -> tuple[int, int] | None:
    """Extract a suite total, or None when its output is genuinely uncounted."""
    lines = _lines(output)
    markers = marker_counts(output)
    if markers is not None:
        return markers

    for line in reversed(lines):
        match = COUNT_SUMMARY_RE.match(line)
        if match:
            return int(match.group(1)), int(match.group(2))
        match = VISUAL_SUMMARY_RE.match(line)
        if match:
            return int(match.group(1)), int(match.group(2))
        match = CHECK_SUMMARY_RE.match(line)
        if match:
            passed, total = int(match.group(1)), int(match.group(2))
            return (passed, total - passed) if total else None

    ran = next((UNITTEST_RAN_RE.match(line) for line in lines
                if UNITTEST_RAN_RE.match(line)), None)
    if ran:
        total = int(ran.group(1))
        if any(line.strip() == "OK" for line in lines):
            return total, 0
        failed = sum(int(match.group(2)) for match in UNITTEST_FAILURE_RE.finditer(output))
        if any(line.startswith("FAILED") for line in lines):
            return max(0, total - failed), failed
    return None


def _summary_counts(output: str) -> tuple[int, int] | None:
    """Return an explicit final count summary, independently of markers."""
    for line in reversed(_lines(output)):
        match = COUNT_SUMMARY_RE.match(line)
        if match:
            return int(match.group(1)), int(match.group(2))
        match = VISUAL_SUMMARY_RE.match(line)
        if match:
            return int(match.group(1)), int(match.group(2))
        match = CHECK_SUMMARY_RE.match(line)
        if match:
            passed, total = int(match.group(1)), int(match.group(2))
            return passed, total - passed
    return None


def _run(command: list[str], *, env: dict[str, str], timeout: int = 600):
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
    )


def _display_failure(output: str) -> None:
    failures = [line.strip() for line in _lines(output) if FAIL_RE.match(line)]
    if failures:
        print("      %s" % failures[0][:180])
    elif output:
        tail = [line for line in _lines(output) if line.strip()][-5:]
        for line in tail:
            print("      %s" % line[:180])


def _run_script(path: Path, env: dict[str, str], bad: list[str], uncounted: list[str],
                failed_suites: set[str],
                *, expected: int | None = None) -> tuple[int, int]:
    name = path.relative_to(REPO_ROOT).as_posix()
    try:
        proc = _run([sys.executable, "-u", path.as_posix()], env=env)
    except subprocess.TimeoutExpired:
        print("  FAILED     %-48s TIMEOUT" % name)
        bad.append("%s timed out" % name)
        failed_suites.add(name)
        return 0, 0

    output = proc.stdout + proc.stderr
    counts = parse_output(output)
    summary_counts = _summary_counts(output)
    reasons = []
    if proc.returncode:
        reasons.append("exit=%d" % proc.returncode)
    if counts is None:
        print("  UNCOUNTED  %-48s no recognizable count markers or summary" % name)
        uncounted.append(name)
        reasons.append("uncounted")
    else:
        passed, failed = counts
        if expected is not None and (passed != expected or failed != 0):
            reasons.append("count %d/%d, expected %d/0" % (passed, failed, expected))
        if summary_counts is not None and summary_counts != counts:
            reasons.append("summary %r disagrees with markers %r" % (summary_counts, counts))
        if failed:
            reasons.append("%d failed checks" % failed)
        label = "FAILED" if reasons else "COUNTED"
        print("  %-10s %-48s %d passed, %d failed" % (label, name, passed, failed))

    if reasons:
        bad.append("%s: %s" % (name, ", ".join(reasons)))
        if counts is not None or proc.returncode:
            failed_suites.add(name)
        _display_failure(output)
    return counts if counts is not None else (0, 0)


def _run_headless(env: dict[str, str], bad: list[str], uncounted: list[str],
                  failed_suites: set[str]) -> tuple[int, int, int]:
    modules = sorted(path.stem for path in HEADLESS_DIR.glob("test_*.py"))
    name = "tests/headless/runner.py"
    if not modules:
        uncounted.append(name)
        bad.append("%s: no test_*.py modules discovered" % name)
        print("  UNCOUNTED  %-48s no test modules discovered" % name)
        return 0, 0, 0

    runner = HEADLESS_DIR / "runner.py"
    try:
        proc = _run([sys.executable, "-u", runner.as_posix(), *modules], env=env)
    except subprocess.TimeoutExpired:
        bad.append("%s timed out" % name)
        print("  FAILED     %-48s TIMEOUT" % name)
        return 0, 0, len(modules)

    output = proc.stdout + proc.stderr
    lines = _lines(output)
    results: dict[str, tuple[int, int]] = {}
    for line in lines:
        match = HEADLESS_SUITE_RE.match(line)
        if match:
            passed, total = int(match.group(1)), int(match.group(2))
            results[match.group(3)] = (passed, total - passed)

    passed_total = failed_total = 0
    for module in modules:
        suite_name = "tests/headless/%s" % module
        counts = results.get(module)
        if counts is None:
            print("  UNCOUNTED  %-48s runner emitted no per-suite count" % suite_name)
            uncounted.append(suite_name)
            continue
        passed, failed = counts
        passed_total += passed
        failed_total += failed
        label = "FAILED" if failed else "COUNTED"
        print("  %-10s %-48s %d passed, %d failed" %
              (label, suite_name, passed, failed))
        if failed:
            bad.append("%s: %d failed tests" % (suite_name, failed))
            failed_suites.add(suite_name)

    aggregate = None
    for line in reversed(lines):
        match = HEADLESS_TOTAL_RE.match(line)
        if match:
            aggregate = tuple(int(match.group(i)) for i in (1, 2, 3))
            break
    expected_aggregate = (passed_total, passed_total + failed_total, len(modules))
    aggregate_matches = aggregate == expected_aggregate
    if not aggregate_matches:
        bad.append("%s: aggregate %r does not match per-suite totals %r" %
                   (name, aggregate, expected_aggregate))
    if proc.returncode:
        bad.append("%s exited %d" % (name, proc.returncode))
        _display_failure(output)
    return passed_total, failed_total, len(modules)


def _counter_regression_check() -> None:
    """Guard CRLF counting, summary counting, and marker-less classification."""
    assert marker_counts("  PASS: one\r\n  FAIL: two\r\n") == (1, 1)
    assert parse_output("2/3 checks passed\r\n") == (2, 1)
    assert parse_output("Ran 3 tests in 0.01s\r\nOK\r\n") == (3, 0)
    assert HEADLESS_TOTAL_RE.match("216/227 checks passed across 20 suite(s) in 0.87s")
    assert parse_output("runner started\r\nno tests reported\r\n") is None


def main() -> int:
    if len(sys.argv) != 1:
        print("This aggregate always runs every discovered suite; arguments are not supported.")
        return 2
    if sys.version_info[:3] != (3, 14, 5):
        print("FATAL: use CPython 3.14.5 (got %s)" % sys.version.split()[0])
        return 2

    visualblocks = VISUALBLOCKS_DIR.resolve().as_posix()
    if visualblocks not in sys.path:
        sys.path.insert(0, visualblocks)
    try:
        from _bootstrap import configure
        module_dir = configure()
        _counter_regression_check()
    except Exception as exc:
        print("FATAL: test bootstrap/counting check failed: %s" % exc)
        return 2

    env = dict(os.environ)
    env["WASTELAND_MODULE_DIR"] = module_dir
    print("  python : %s" % sys.executable.replace("\\", "/"))
    print("  module : %s/Wasteland.pyd" % module_dir)
    print("  assets : %s" % (REPO_ROOT / "DemonCore-Editor" / "assets").resolve().as_posix())

    bad: list[str] = []
    uncounted: list[str] = []
    failed_suites: set[str] = set()
    total_pass = total_fail = suite_count = 0

    top_level = sorted(TESTS_DIR.glob("test_*.py"))
    promoted = sorted(VISUALBLOCKS_DIR.glob("test_*.py"))
    required = set(EXPECTED_PROMOTED_COUNTS)
    found = {path.name for path in promoted}
    for missing in sorted(required - found):
        bad.append("missing promoted suite: tests/visualblocks/%s" % missing)

    for path in top_level:
        passed, failed = _run_script(path, env, bad, uncounted, failed_suites)
        total_pass += passed
        total_fail += failed
        suite_count += 1

    for path in promoted:
        expected = EXPECTED_PROMOTED_COUNTS.get(path.name)
        passed, failed = _run_script(path, env, bad, uncounted, failed_suites,
                                     expected=expected)
        total_pass += passed
        total_fail += failed
        suite_count += 1

    headless_pass, headless_fail, headless_count = _run_headless(
        env, bad, uncounted, failed_suites)
    total_pass += headless_pass
    total_fail += headless_fail
    suite_count += headless_count

    promoted_observed = 0
    for name, expected in EXPECTED_PROMOTED_COUNTS.items():
        # The per-suite runner already enforces each expected count; this
        # separate total guards an accidental omission from the aggregate.
        promoted_observed += expected
    if promoted_observed != EXPECTED_PROMOTED_TOTAL or EXPECTED_PROMOTED_TOTAL != 1427:
        bad.append("promoted-suite count contract changed: %d (expected 1427)" %
                   promoted_observed)

    print("\naggregate:")
    print("  suites:        %d" % suite_count)
    print("  checks passed: %d" % total_pass)
    print("  checks failed: %d" % total_fail)
    print("  failed suites: %d" % len(failed_suites))
    print("  reported problems: %d" % len(bad))
    if bad:
        for reason in bad:
            print("    FAILED  %s" % reason)
    if uncounted:
        print("  UNCOUNTED suites: %d" % len(uncounted))
        for name in uncounted:
            print("    %s" % name)
    else:
        print("  UNCOUNTED suites: 0")
    return 1 if bad or uncounted else 0


if __name__ == "__main__":
    raise SystemExit(main())
