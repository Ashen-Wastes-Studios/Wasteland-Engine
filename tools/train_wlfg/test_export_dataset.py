#!/usr/bin/env python3
"""Regression tests for non-destructive WLFG export planning."""
from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import export_dataset

SCRATCH = Path.home() / 'AppData/Local/hermes/cache/scratch'


class ExportPlanningTests(unittest.TestCase):
    def test_dry_run_preserves_existing_manifest_and_directory(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=SCRATCH) as folder:
            root = Path(folder)
            manifest = root / 'manifest.json'
            original = b'{"frames":[],"test_marker":"preserve"}\n'
            manifest.write_bytes(original)
            before = sorted(p.name for p in root.iterdir())
            with contextlib.redirect_stdout(io.StringIO()):
                result = export_dataset.main(['--out', str(root), '--dry-run'])
            self.assertEqual(result, 0)
            self.assertEqual(manifest.read_bytes(), original)
            self.assertEqual(sorted(p.name for p in root.iterdir()), before)


if __name__ == '__main__':
    unittest.main()
