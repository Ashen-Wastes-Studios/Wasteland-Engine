#!/usr/bin/env python3
"""Regression tests for WLFG capture provenance and model feature parity."""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np
from PIL import Image

import data_contract
import dataset as ds
import export_dataset as exporter
from data_contract import TRAINING_CONFIG, require_training_manifest

SCRATCH = Path.home() / "AppData/Local/hermes/cache/scratch"


class RequireTrainingManifestTests(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)

    def test_rejects_missing_config(self):
        with self.assertRaisesRegex(ValueError, "WLFG training provenance"):
            require_training_manifest({"frames": []})

    def test_rejects_unknown_value(self):
        manifest = {"config": dict(TRAINING_CONFIG,
                                   dataset_status="partial"),
                    "frames": []}
        with self.assertRaisesRegex(ValueError, "not verified temporal training"):
            require_training_manifest(manifest)

    def test_accepts_verified_config(self):
        manifest = {"config": dict(TRAINING_CONFIG), "frames": []}
        require_training_manifest(manifest)

    def test_accepts_exact_verified_config(self):
        manifest = {"config": {k: v for k, v in TRAINING_CONFIG.items()},
                    "frames": []}
        require_training_manifest(manifest)


class LoadPairsFailClosedTests(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)

    def _write_manifest(self, config, frames, name="manifest.json"):
        (SCRATCH / name).write_text(json.dumps({"config": config, "frames": frames}),
                                    encoding="utf-8")

    def test_loading_motion_probe_raises(self):
        self._write_manifest(
            {"dataset_status": "motion_probe",
             "factor_axis": "camera motion (deg/frame), not present-rate"},
            [{"id": "s", "scene": "s", "camera": {"name": "n"}, "factor": 1,
              "pass_name": "target", "path": "target/s.png"}])
        with self.assertRaisesRegex(ValueError, "(?i)(template|provenance|training)"):
            ds.load_pairs(SCRATCH)

    def test_loading_plain_no_wlfg_raises(self):
        self._write_manifest({}, [{"id": "f", "scene": "s", "camera": {"name": "n"},
                                   "factor": 1, "pass_name": "target", "path": "t.png"}])
        with self.assertRaisesRegex(ValueError, "(?i)(template|provenance|training)"):
            ds.load_pairs(SCRATCH)

    def test_training_pairs_require_all_inputs(self):
        self._write_manifest(
            config=dict(TRAINING_CONFIG), frames=[])
        with self.assertRaisesRegex(ValueError, "(?i)incomplete|no complete"):
            ds.load_pairs(SCRATCH)
if __name__ == "__main__":
    unittest.main()
