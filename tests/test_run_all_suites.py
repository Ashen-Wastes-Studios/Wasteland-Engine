"""Regression checks for the aggregate's count and uncounted-output rules."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = (REPO_ROOT / "tools").resolve().as_posix()
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import run_all_suites


class AggregateCounterTests(unittest.TestCase):
    def test_promoted_suite_counts_are_pinned(self):
        self.assertEqual(
            run_all_suites.EXPECTED_PROMOTED_COUNTS,
            {
                "test_node_library.py": 1209,
                "test_wlbtree_compiler.py": 56,
                "test_visualblocks.py": 101,
                "test_enemy_tree.py": 61,
            },
        )
        self.assertEqual(run_all_suites.EXPECTED_PROMOTED_TOTAL, 1427)

    def test_pass_fail_markers_count_with_crlf(self):
        output = "  PASS: one\r\n  FAIL: two\r\n1 passed, 1 failed\r\n"
        self.assertEqual(run_all_suites.marker_counts(output), (1, 1))
        self.assertEqual(run_all_suites.parse_output(output), (1, 1))

    def test_summary_only_suite_output_has_an_explicit_count(self):
        self.assertEqual(run_all_suites.parse_output("54/56 checks passed\r\n"), (54, 2))
        self.assertEqual(run_all_suites.parse_output("passed 101, failed 0\n"), (101, 0))

    def test_existing_visualblocks_ok_markers_are_counted(self):
        output = "  ok   one check\r\n  ok   second check\r\npassed 2, failed 0\r\n"
        self.assertEqual(run_all_suites.marker_counts(output), (2, 0))
        self.assertEqual(run_all_suites.parse_output(output), (2, 0))

    def test_unittest_summary_is_counted(self):
        self.assertEqual(
            run_all_suites.parse_output("Ran 4 tests in 0.01s\r\n\r\nOK\r\n"),
            (4, 0),
        )

    def test_headless_total_summary_with_elapsed_time_is_recognized(self):
        line = "216/227 checks passed across 20 suite(s) in 0.87s"
        match = run_all_suites.HEADLESS_TOTAL_RE.match(line)
        self.assertIsNotNone(match)
        self.assertEqual(tuple(map(int, match.groups())), (216, 227, 20))

    def test_output_without_markers_or_summary_is_uncounted(self):
        output = "runner started\r\nfinished without a test report\r\n"
        self.assertIsNone(run_all_suites.marker_counts(output))
        self.assertIsNone(run_all_suites.parse_output(output))

    def test_script_nonzero_exit_fails_even_after_green_summary(self):
        proc = subprocess.CompletedProcess(
            args=[], returncode=3221225477,
            stdout="Ran 1 test in 0.01s\n\nOK\n", stderr="",
        )
        bad, uncounted, failed_suites = [], [], set()
        with mock.patch.object(run_all_suites, "_run", return_value=proc), \
             mock.patch.object(run_all_suites, "_display_failure"):
            counts = run_all_suites._run_script(
                Path(__file__), {}, bad, uncounted, failed_suites,
            )
        self.assertEqual(counts, (1, 0))
        self.assertEqual(uncounted, [])
        self.assertIn("exit=3221225477", bad[0])
        self.assertIn(Path(__file__).relative_to(run_all_suites.REPO_ROOT).as_posix(),
                      failed_suites)

    def test_headless_nonzero_exit_fails_after_all_summaries(self):
        output = (
            "  1/1 passed  test_dummy\n"
            "1/1 checks passed across 1 suite(s) in 0.01s\n"
        )
        proc = subprocess.CompletedProcess(
            args=[], returncode=3221225477, stdout=output, stderr="",
        )
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "test_dummy.py").write_text("", encoding="utf-8")
            bad, uncounted, failed_suites = [], [], set()
            with mock.patch.object(run_all_suites, "HEADLESS_DIR", Path(directory)), \
                 mock.patch.object(run_all_suites, "_run", return_value=proc), \
                 mock.patch.object(run_all_suites, "_display_failure"):
                counts = run_all_suites._run_headless({}, bad, uncounted, failed_suites)
        self.assertEqual(counts, (1, 0, 1))
        self.assertEqual(uncounted, [])
        self.assertTrue(any("exited 3221225477" in item for item in bad))


if __name__ == "__main__":
    unittest.main(verbosity=2)
