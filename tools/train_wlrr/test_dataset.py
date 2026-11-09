"""Tests for the WLRR dataset loader and its fail-closed contract.

numpy + Pillow only. Builds a small synthetic dataset on disk so the loader is
exercised end to end, including the paths that must REFUSE.

The refusals are the point. A loader that accepts a dataset it should reject
trains a model on unverified data and reports a plausible loss, and nothing
downstream can tell. Each rejection below is a case that would otherwise reach
training silently.

Run: python tools/train_wlrr/test_dataset.py
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import data_contract as C     # noqa: E402
import dataset as D           # noqa: E402
import model as M             # noqa: E402


def write_png(path: Path, arr: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(arr.astype(np.uint8), mode="RGB").save(path)


def build_dataset(root: Path, *, arms=(D.ARM_INPUT, D.ARM_INCUMBENT, D.ARM_TARGET),
                  size=(40, 30), provenance: bool = True, scene: str = "scenes/Test.wastescene",
                  camera: str = "auto_y0", seed: int = 0, value: int = 100) -> Path:
    """Write a minimal but structurally valid WLRR dataset and return its root."""
    rng = np.random.default_rng(seed)
    frames = []
    for arm in arms:
        rel = f"{arm}/{Path(scene).stem}__{camera}__{arm}.png"
        arr = rng.integers(0, 256, (*size, 3), dtype=np.uint8)
        write_png(root / rel, arr)
        frames.append(dict(
            id=f"{Path(scene).stem}__{camera}__{arm}",
            scene=scene, pass_name=arm,
            camera=dict(name=camera, focal=[35.0], distance=5.0, pitch=0.0, yaw=0.0),
            render_scale=0.5 if arm != D.ARM_TARGET else 1.0,
            width=size[0], height=size[1], path=rel, blank=False,
        ))
    config = dict(converged_frame=400, noisy_frame=400, min_contrast_p99=4.0)
    if provenance:
        config.update(C.TRAINING_CONFIG)
    (root / "manifest.json").write_text(
        json.dumps(dict(version=1, config=config, frames=frames, failures=[])), encoding="utf-8")
    return root


class TestContract(unittest.TestCase):

    def test_accepts_a_verified_manifest(self):
        C.require_training_manifest({"config": dict(C.TRAINING_CONFIG)})

    def test_refuses_missing_or_malformed_provenance(self):
        for bad in ({}, {"config": {}}, {"config": "nope"}, {"config": None},
                    {"config": dict(C.TRAINING_CONFIG, state_encoding="float32_npy")},
                    {"config": dict(C.TRAINING_CONFIG, dataset_status="verified_temporal_pairs")}):
            with self.assertRaises(ValueError, msg=f"accepted {bad!r}"):
                C.require_training_manifest(bad)

    def test_the_error_names_what_is_wrong(self):
        """A refusal that does not say which field failed is a refusal nobody can
        act on, which in practice means it gets bypassed."""
        with self.assertRaises(ValueError) as ctx:
            C.require_training_manifest({"config": dict(C.TRAINING_CONFIG,
                                                        color_domain="converted")})
        self.assertIn("color_domain", str(ctx.exception))


class TestLoader(unittest.TestCase):

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wlrr_ds_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_loads_a_valid_dataset(self):
        build_dataset(self.tmp)
        triples = D.load_triples(self.tmp)
        self.assertEqual(len(triples), 1)
        t = triples[0]
        self.assertEqual(t.camera, "auto_y0")
        self.assertTrue(t.input_path.is_file() and t.incumbent_path.is_file()
                        and t.target_path.is_file())

    def test_refuses_a_dataset_without_provenance(self):
        build_dataset(self.tmp, provenance=False)
        with self.assertRaises(ValueError) as ctx:
            D.load_triples(self.tmp)
        self.assertIn("provenance", str(ctx.exception).lower())

    def test_refuses_a_missing_arm(self):
        """Two arms is not a triple. Skipping the incomplete one would train on a
        subset nobody chose."""
        build_dataset(self.tmp, arms=(D.ARM_INPUT, D.ARM_INCUMBENT))
        with self.assertRaises(ValueError) as ctx:
            D.load_triples(self.tmp)
        self.assertIn("missing", str(ctx.exception))

    def test_refuses_arms_of_different_sizes(self):
        """THE check a declaration cannot make. The head reads the incumbent and
        the input at the same pixel coords, so mismatched sizes make the pair
        meaningless -- and every weight trained on it wrong."""
        build_dataset(self.tmp)
        man = json.loads((self.tmp / "manifest.json").read_text())
        for f in man["frames"]:
            if f["pass_name"] == D.ARM_INPUT:
                f["width"], f["height"] = 20, 15
        (self.tmp / "manifest.json").write_text(json.dumps(man))
        with self.assertRaises(ValueError) as ctx:
            D.load_triples(self.tmp)
        self.assertIn("differ in size", str(ctx.exception))

    def test_allow_incomplete_skips_loudly_but_still_refuses_when_nothing_is_usable(self):
        """The escape hatch must not become a silent default. With one of two
        cameras broken, the flag yields the good triple AND names the skip; with
        everything broken it still raises rather than returning an empty set."""
        build_dataset(self.tmp, camera="auto_y0")
        man = json.loads((self.tmp / "manifest.json").read_text())
        rng = np.random.default_rng(9)
        for arm in (D.ARM_INPUT, D.ARM_INCUMBENT, D.ARM_TARGET):
            rel = f"{arm}/Test__auto_y90__{arm}.png"
            write_png(self.tmp / rel, rng.integers(0, 256, (30, 40, 3), dtype=np.uint8))
            e = dict(man["frames"][0])
            e.update(id=f"Test__auto_y90__{arm}", pass_name=arm, path=rel)
            e["camera"] = dict(e["camera"], name="auto_y90")
            man["frames"].append(e)
        (self.tmp / "manifest.json").write_text(json.dumps(man))

        # Break auto_y90 only: drop its incumbent.
        man["frames"] = [f for f in man["frames"]
                         if not (f["camera"]["name"] == "auto_y90"
                                 and f["pass_name"] == D.ARM_INCUMBENT)]
        (self.tmp / "manifest.json").write_text(json.dumps(man))

        with self.assertRaises(ValueError):
            D.load_triples(self.tmp)                    # strict by default

        import contextlib, io
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            triples = D.load_triples(self.tmp, allow_incomplete=True)
        self.assertEqual([t.camera for t in triples], ["auto_y0"])
        self.assertIn("auto_y90", err.getvalue(), "the skip must be named")
        self.assertIn("skipped", err.getvalue())

        # Nothing usable -> refuse even with the flag.
        man["frames"] = [f for f in man["frames"]
                         if f["camera"]["name"] != "auto_y0"]
        (self.tmp / "manifest.json").write_text(json.dumps(man))
        with self.assertRaises(ValueError) as ctx:
            D.load_triples(self.tmp, allow_incomplete=True)
        self.assertIn("no complete", str(ctx.exception))

    def test_refuses_a_duplicate_arm(self):
        build_dataset(self.tmp)
        man = json.loads((self.tmp / "manifest.json").read_text())
        man["frames"].append(dict(man["frames"][0]))
        (self.tmp / "manifest.json").write_text(json.dumps(man))
        with self.assertRaises(ValueError) as ctx:
            D.load_triples(self.tmp)
        self.assertIn("duplicate", str(ctx.exception))

    def test_refuses_an_empty_manifest(self):
        (self.tmp / "manifest.json").write_text("")
        with self.assertRaises(ValueError) as ctx:
            D.load_triples(self.tmp)
        self.assertIn("empty", str(ctx.exception))

    def test_missing_manifest_says_how_to_make_one(self):
        with self.assertRaises(FileNotFoundError) as ctx:
            D.load_triples(self.tmp)
        self.assertIn("export_dataset", str(ctx.exception))

    def test_read_rgb_is_0_to_255_not_normalised(self):
        """The domain is part of the ABI. `test_model.py` builds batches in 0..255
        and the shader reads the same range, so a loader that normalised to 0..1
        would silently train a different function from the one under test."""
        build_dataset(self.tmp)
        t = D.load_triples(self.tmp)[0]
        arr = D.read_rgb(t.input_path)
        self.assertEqual(arr.dtype, np.float32)
        self.assertGreaterEqual(float(arr.min()), 0.0)
        self.assertLessEqual(float(arr.max()), 255.0)
        self.assertGreater(float(arr.max()), 1.0, "looks normalised to 0..1")


class TestStatistics(unittest.TestCase):

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wlrr_ds_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_stats_have_the_right_shape_and_are_usable(self):
        build_dataset(self.tmp, seed=3)
        triples = D.load_triples(self.tmp)
        mean, std = D.compute_feature_stats(triples, samples=5_000, per_triple=5_000)
        self.assertEqual(mean.shape, (M.N_FEAT,))
        self.assertEqual(std.shape, (M.N_FEAT,))
        self.assertTrue(np.all(np.isfinite(mean)))
        self.assertTrue(np.all(std > 0))
        # Uniform noise in 0..255 -> mean ~127, std ~74. This catches an affine
        # measured in the wrong domain (0..1 would give mean ~0.5).
        #
        # Checked on the two ARM groups only: the difference group rightly has
        # mean ~0 and a std equal to the arm separation, so a whole-array average
        # would be meaningless for it.
        arm = slice(0, 6)
        self.assertGreater(float(mean[arm].mean()), 100.0)
        self.assertGreater(float(std[arm].mean()), 50.0)

    def test_apply_feature_stats_installs_and_validates(self):
        saved_mean, saved_std = M.FEATURE_MEAN.copy(), M.FEATURE_STD.copy()
        try:
            D.apply_feature_stats(np.full(M.N_FEAT, 100.0), np.full(M.N_FEAT, 50.0))
            self.assertAlmostEqual(float(M.FEATURE_MEAN[0]), 100.0)
            with self.assertRaises(ValueError):
                D.apply_feature_stats(np.zeros(M.N_FEAT - 1), np.ones(M.N_FEAT - 1))
            with self.assertRaises(ValueError):
                D.apply_feature_stats(np.zeros(M.N_FEAT), np.zeros(M.N_FEAT))  # std must be > 0
        finally:
            M.FEATURE_MEAN, M.FEATURE_STD = saved_mean, saved_std

    def test_standardising_with_the_measured_affine_keeps_the_gate_sane(self):
        """The measured affine is what stops the pre-activations running to ~1e3
        and saturating the gate shut. This asserts the fix, not just the shape."""
        saved_mean, saved_std = M.FEATURE_MEAN.copy(), M.FEATURE_STD.copy()
        try:
            build_dataset(self.tmp, seed=7)
            triples = D.load_triples(self.tmp)
            mean, std = D.compute_feature_stats(triples, samples=4_000, per_triple=4_000)
            D.apply_feature_stats(mean, std)
            rng = np.random.default_rng(1)
            p = M.init_params(rng)
            inc, inp, _ = next(D.iter_random_crops(triples, 4, 16, rng))
            _, cache = M.forward(p, M.features(inc, inp), inc)
            gate = cache[7]
            self.assertTrue(np.all(np.isfinite(gate)))
            self.assertGreater(float(np.max(gate)), 0.0,
                               "the gate is shut -- standardisation is not working")
        finally:
            M.FEATURE_MEAN, M.FEATURE_STD = saved_mean, saved_std


class TestSplit(unittest.TestCase):

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wlrr_ds_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _multi(self):
        """Four cameras, so a camera holdout has something left to train on."""
        root = self.tmp
        frames = []
        for i, cam in enumerate(("auto_y0", "auto_y90", "auto_y180", "auto_y270")):
            rng = np.random.default_rng(i)
            for arm in (D.ARM_INPUT, D.ARM_INCUMBENT, D.ARM_TARGET):
                rel = f"{arm}/Test__{cam}__{arm}.png"
                write_png(root / rel, rng.integers(0, 256, (30, 40, 3), dtype=np.uint8))
                frames.append(dict(
                    id=f"Test__{cam}__{arm}", scene="scenes/Test.wastescene",
                    pass_name=arm,
                    camera=dict(name=cam, focal=[35.0], distance=5.0, pitch=0.0, yaw=0.0),
                    render_scale=0.5, width=40, height=30, path=rel, blank=False))
        (root / "manifest.json").write_text(json.dumps(
            dict(version=1, config=dict(C.TRAINING_CONFIG), frames=frames, failures=[])))
        return D.load_triples(root)

    def test_splits_are_disjoint_and_cover_everything(self):
        triples = self._multi()
        train, hold = D.split_triples(triples, seed=1)
        self.assertTrue(set(t.key for t in train).isdisjoint(t.key for t in hold))
        self.assertEqual(len(train) + len(hold), len(triples))
        self.assertTrue(train and hold)

    def test_camera_holdout_is_whole_camera(self):
        triples = self._multi()
        train, hold = D.split_triples(triples, holdout_camera="auto_y90")
        self.assertEqual({t.camera for t in hold}, {"auto_y90"})
        self.assertNotIn("auto_y90", {t.camera for t in train})

    def test_refuses_a_split_that_empties_a_side(self):
        triples = self._multi()
        with self.assertRaises(ValueError):
            D.split_triples(triples, holdout_camera="not_a_camera")


if __name__ == "__main__":
    unittest.main(verbosity=2)
