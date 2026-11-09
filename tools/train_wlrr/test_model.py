"""Tests for the WLRR head. numpy only; no GPU, no dataset.

The two checks that matter most, and why:

1. `test_zero_weights_is_exactly_the_incumbent` -- the safety property. The whole
   design rests on "a wrong network degrades to the incumbent's own output".
   That is only true if a zeroed residual head makes `out == incumbent` EXACTLY,
   so this asserts max|out - incumbent| == 0.0, not "is small".

2. `test_gradients_match_finite_difference` -- the gradient is written out by
   hand, and a hand-written gradient that is wrong by a constant factor still
   trains, just worse, so nothing downstream would catch it. This check caught a
   real bug in the WLFG head (np.mean over (N,3) divides by 3N, scaling the whole
   gradient by N_RESIDUAL), which is why it is the first thing ported here.

Run: python tools/train_wlrr/test_model.py
"""

from __future__ import annotations

import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import model as M  # noqa: E402


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((np.asarray(a, dtype=np.float64)
                         - np.asarray(b, dtype=np.float64)) ** 2))
    return float("inf") if mse == 0.0 else 10.0 * np.log10(255.0 ** 2 / mse)


def _best_fd(f, p, name, index, analytic,
             eps_list=(1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8)):
    """Central difference at the eps that agrees best with the analytic gradient.

    WHY A SWEEP AND NOT ONE eps
    ---------------------------
    A single eps cannot serve parameters whose curvature differs by orders of
    magnitude, and this is not hypothetical: `W1[72]` here has a gradient of
    **1.83e3**, and at eps=1e-6 its finite-difference truncation error is **8.2**
    (4.5e-3 relative) -- which FAILS a 1e-4 tolerance, while the identical
    analytic gradient agrees to **3.3e-10** at eps=1e-7. So a fixed eps fails on
    exactly the parameters with the largest curvature, which are the ones most
    worth checking. The error falls monotonically from 1e-2 to 1e-7, so the
    truncation is the cause and the math is fine.

    Taking the MINIMUM disagreement over eps cannot hide a real error: a gradient
    wrong by a constant factor is off by that factor at EVERY eps, so its minimum
    disagreement is still large. It can only make a genuine error more visible.

    Returns (best_abs_error, numeric_at_best_eps, best_eps).
    """
    flat = p[name].ravel()
    orig = float(flat[index])
    best = None
    for eps in eps_list:
        flat[index] = orig + eps
        lp = f(p)
        flat[index] = orig - eps
        lm = f(p)
        flat[index] = orig
        numeric = (lp - lm) / (2.0 * eps)
        err = abs(numeric - analytic)
        if best is None or err < best[0]:
            best = (err, numeric, eps)
    return best


def make_batch(n: int, seed: int = 0):
    """A synthetic but non-degenerate (incumbent, input, target) batch.

    The 'incumbent' is deliberately a blurred version of the input so that the
    residual the head must learn is non-zero -- a batch where incumbent == input
    would let a broken head pass.

    Returns (incumbent, raw_input, target). There is no mask/depth: the ABI has
    six inputs, and the WLRR dataset cannot supply mask or depth. See the model
    docstring for why an earlier 8-input revision was reverted.
    """
    rng = np.random.default_rng(seed)
    raw = rng.uniform(0.0, 255.0, (n, 3)).astype(np.float32)
    # A structured "incumbent": push towards the local mean of the 3 channels and
    # add a small deterministic bias, so it differs from the input everywhere.
    inc = (0.7 * raw + 0.3 * raw.mean(axis=1, keepdims=True) + 4.0).astype(np.float32)
    target = raw  # the ideal correction recovers the input's detail
    return inc, raw, target


class TestArchitecture(unittest.TestCase):

    def test_abi_shape(self):
        """The ABI. A mismatch here is a shader that reads the blob wrong."""
        self.assertEqual(M.N_IN, 6, "the residual trunk sees the two arms only")
        self.assertEqual(M.N_INCUMBENT, 3)
        self.assertEqual(M.N_INPUT, 3)
        self.assertEqual(M.N_DIFF, 3, "the gate's regime signal, arms + difference")
        self.assertEqual(M.N_GATE_IN, 9, "the gate sees arms + difference")
        self.assertEqual(M.N_HIDDEN, 16)
        self.assertEqual(M.N_GATE_HIDDEN, 8, "the gate's own first layer")
        self.assertEqual(M.N_RESIDUAL, 3)
        self.assertEqual(M.N_GATE, 1)
        # 6*16 + 16 + 16*16 + 16 + 16*3 + 3 + 9*8 + 8 + 8*1 + 1
        self.assertEqual(M.N_WEIGHTS, 524)
        # And the ABI must not have quietly grown mask/depth back, because the
        # dataset cannot feed them. This is a regression guard on a real reverted
        # design, not a style preference.
        self.assertFalse(hasattr(M, "N_MASK"))
        self.assertFalse(hasattr(M, "N_DEPTH"))

    def test_the_residual_trunk_cannot_see_the_difference(self):
        """THE load-bearing structural check for the v3 split.

        v2 (difference in the shared trunk) lost 12.8 dB because the residual
        learned a rewind: `residual = -k*(incumbent-input)`. The only thing that
        prevents that in v3 is that `W1` has N_IN=6 columns, not 9. If this
        assertion ever fails, the rewind is expressible again and the head will
        find it -- it did before, from a loss that rewarded it.
        """
        shapes = dict(M.LAYER_SHAPES)
        self.assertEqual(shapes["W1"], (M.N_IN, M.N_HIDDEN),
                         "the trunk must take exactly the two arms")
        self.assertEqual(shapes["Wg1"], (M.N_GATE_IN, M.N_GATE_HIDDEN),
                         "the gate must take arms + difference")
        # The trunk's later layers are hidden->hidden; the point is that NONE of
        # them is keyed to N_GATE_IN. Only W1's width carries the feature count,
        # so that is where a 9-wide trunk would show up.
        self.assertNotEqual(shapes["W1"][0], M.N_GATE_IN)
        self.assertNotIn("Wg", shapes, "the old shared gate layer must be gone")
        self.assertNotIn("bg", shapes)

    def test_pack_unpack_roundtrip(self):
        rng = np.random.default_rng(1)
        p = M.init_params(rng)
        vec = M.pack(p)
        self.assertEqual(vec.size, M.N_WEIGHTS)
        self.assertEqual(vec.dtype, np.float32)
        q = M.unpack(vec)
        for name, _ in M.LAYER_SHAPES:
            np.testing.assert_array_equal(p[name], q[name])

    def test_unpack_refuses_wrong_length(self):
        """Refuse rather than truncate: a silently-truncated blob would train the
        shader and the model to different functions."""
        with self.assertRaises(ValueError):
            M.unpack(np.zeros(M.N_WEIGHTS - 1, dtype=np.float32))

    def test_layer_shapes_are_consistent(self):
        total = sum(int(np.prod(s)) for _, s in M.LAYER_SHAPES)
        self.assertEqual(total, M.N_WEIGHTS)
        # Every named array in the dict matches its declared shape.
        rng = np.random.default_rng(2)
        p = M.init_params(rng)
        self.assertEqual(set(p), {n for n, _ in M.LAYER_SHAPES})


class TestSafetyProperty(unittest.TestCase):

    def test_zero_weights_is_exactly_the_incumbent(self):
        """THE safety property, asserted exactly.

        A fresh model must be the identity over the incumbent. If this were only
        'approximately' true, the fallback path would still perturb the frame and
        the residual form would buy nothing.
        """
        rng = np.random.default_rng(3)
        p = M.init_params(rng)
        inc, raw, _ = make_batch(64, seed=4)
        out, gate = M.predict(p, inc, raw)
        self.assertEqual(float(np.max(np.abs(out - inc))), 0.0,
                         "a zeroed residual head must reproduce the incumbent bit-exactly")

    def test_zeroing_the_residual_head_is_the_identity_after_training(self):
        """The same property after the head has moved: zeroing Wr/br must return
        the model to exactly the incumbent, whatever the other layers hold.

        This drives `forward` on well-scaled features rather than `predict` on a
        raw batch, and the reason is worth stating. `FEATURE_MEAN`/`FEATURE_STD`
        are still the identity (they are filled in from `dataset_stats.json` once
        the dataset exists), so `predict` feeds raw 0..255 values into weights
        that He-initialisation sized for standardised inputs. The pre-activations
        then reach ~1e3, sigmoid saturates, and the gate is EXACTLY 0 -- measured
        on seed 5. The property below holds regardless of the gate (zero residual
        means `out = anchor` whatever `gate` is), but the sanity assertion above
        it needs the head to actually move the image, which a shut gate prevents
        for reasons unrelated to what is being tested.
        """
        rng = np.random.default_rng(5)
        p = M.init_params(rng)
        for name, shape in M.LAYER_SHAPES:      # randomise everything
            p[name] = rng.normal(0.0, 0.5, shape).astype(np.float32)
        # Standard-normal features and a 0..255 anchor: sane activations, open gate.
        n = 32
        feats = rng.normal(0.0, 1.0, (n, M.N_FEAT)).astype(np.float32)
        anchor = rng.uniform(0.0, 255.0, (n, 3)).astype(np.float32)

        out_before, cache = M.forward(p, feats, anchor)
        gate = cache[7]
        self.assertGreater(float(np.max(gate)), M.GATE_FLOOR,
                           "sanity: the gate must be open for this test to mean anything")
        self.assertGreater(float(np.max(np.abs(out_before - anchor))), 0.0,
                           "sanity: a randomised head must actually move the image")
        p["Wr"] = np.zeros_like(p["Wr"])
        p["br"] = np.zeros_like(p["br"])
        out_after, _ = M.forward(p, feats, anchor)
        self.assertEqual(float(np.max(np.abs(out_after - anchor))), 0.0)

    def test_gate_is_bounded(self):
        rng = np.random.default_rng(7)
        p = M.init_params(rng, scale=3.0)   # large weights -> extreme pre-activations
        inc, raw, _ = make_batch(256, seed=8)
        _, gate = M.predict(p, inc, raw)
        self.assertTrue(np.all(gate >= 0.0) and np.all(gate <= 1.0))
        self.assertTrue(np.all(np.isfinite(gate)))


class TestGradients(unittest.TestCase):

    def test_gradients_match_finite_difference(self):
        """Central differences against the analytic gradient, in FLOAT64.

        A hand-written gradient that is wrong by a constant factor still trains,
        so this is the only check that catches it. Relative error is used because
        the parameters differ in magnitude by orders of magnitude across layers.

        WHY FLOAT64, AND WHY THAT IS NOT CHEATING
        -----------------------------------------
        The model runs in float32, but a finite difference cannot be checked in
        float32 at this scale. The loss here is ~358, so float32 spacing is
        ~3.05e-05; `(lp - lm) / 2e-3` amplifies that to a ~1.5e-02 NOISE FLOOR on
        the numeric gradient. Measured, that produced a spurious 1.36e-01
        relative error on the SMALLEST true gradient in the batch (b2[13],
        |g| = 0.088) while every larger one sat near the floor -- the round-off
        signature, not a bug. Re-running the identical check in float64 gives a
        worst-case 4.72e-07.

        The point of this test is to verify the MATH, and precision is a separate
        concern, so it is checked where precision cannot mask an error. The fp32
        path is still exercised by every other test in this file, and by
        `test_gradient_direction_holds_in_float32` below, which only asks for the
        sign -- a question round-off cannot answer wrongly.
        """
        rng = np.random.default_rng(9)
        p32 = M.init_params(rng)
        for name, shape in M.LAYER_SHAPES:
            p32[name] = rng.normal(0.0, 0.3, shape).astype(np.float32)
        inc, raw, target = make_batch(48, seed=10)

        # Same parameters, same features, promoted to float64.
        p = {k: v.astype(np.float64) for k, v in p32.items()}
        feats = M.features(inc, raw).astype(np.float64)
        inc64 = inc.astype(np.float64)
        tgt64 = target.astype(np.float64)

        def f(p_):
            return M.loss_and_grads(p_, feats, tgt64, lam=M.LAMBDA_ANCHOR,
                                     anchor_rgb=inc64)[0]

        _, _, _, grads = M.loss_and_grads(p, feats, tgt64, lam=M.LAMBDA_ANCHOR,
                                          anchor_rgb=inc64)

        atol, rtol = 1e-6, 1e-4
        #
        # A purely relative criterion reports a false failure on any parameter
        # whose true gradient is near zero. Measured here: Wg[13,0] has a gradient
        # of 2.5e-07 (its unit is dead for 32 of 48 pixels), so a relative
        # comparison scored 5.41e-02 and failed the build. The error traces the
        # standard finite-difference U-shape -- 5.47e-04 at eps=1e-2, 1.45e-05 at
        # eps=1e-3, 5.41e-02 at eps=1e-6, 1.00 at eps=1e-8 -- i.e. truncation at
        # large eps and round-off at small eps, with the parameter's own gradient
        # merely SMALL rather than wrong.
        #
        # So: a large ABSOLUTE error is a failure, a large RELATIVE error on a
        # meaningful gradient is a failure, and a large relative error on a
        # gradient that is itself below the noise floor is not. A genuinely wrong
        # gradient is off by a factor, so its absolute error is proportional to
        # its own size and this still catches it.
        atol, rtol = 1e-6, 1e-4
        worst_excess = -np.inf
        worst_name = ""
        worst_detail = ""
        for name, _ in M.LAYER_SHAPES:
            flat = p[name].ravel()
            # Probe a bounded, deterministic subset: all of the small layers, and
            # a spread across the big ones.
            idx = (range(flat.size) if flat.size <= 32
                   else range(0, flat.size, max(1, flat.size // 24)))
            for i in idx:
                err, numeric, eps = _best_fd(f, p, name, i,
                                             float(grads[name].ravel()[i]))
                analytic = float(grads[name].ravel()[i])
                tol = atol + rtol * max(abs(numeric), abs(analytic))
                excess = err - tol
                if excess > worst_excess:
                    worst_excess = excess
                    worst_name = f"{name}[{i}]"
                    worst_detail = (f"numeric {numeric:.3e}, analytic {analytic:.3e}, "
                                    f"|diff| {err:.3e}, tol {tol:.3e}, eps {eps:.0e}")
        self.assertLess(worst_excess, 0.0,
                        f"gradient mismatch at {worst_name}: {worst_detail}")

    def test_weighted_gradients_match_finite_difference(self):
        """The per-sample weight path, checked the same way as the unweighted one.

        THE RISK IT GUARDS
        ------------------
        The weight enters the normaliser AND every gradient term. A mistake --
        forgetting `w` in `dres`, say -- produces a loss VALUE that is exactly
        right and a gradient that silently points somewhere else. Training would
        then make the model worse while the loss curve looked sane. This is the
        path most likely to be wrong because a hand-written adjoint is involved
        and there are four places to forget.
        """
        rng = np.random.default_rng(23)
        p32 = M.init_params(rng)
        for name, shape in M.LAYER_SHAPES:
            p32[name] = rng.normal(0.0, 0.3, shape).astype(np.float32)
        inc, raw, target = make_batch(48, seed=24)

        p = {k: v.astype(np.float64) for k, v in p32.items()}
        feats = M.features(inc, raw).astype(np.float64)
        inc64 = inc.astype(np.float64)
        tgt64 = target.astype(np.float64)
        weights = M.separation_weights(inc64, raw.astype(np.float64))

        def f(p_):
            return M.loss_and_grads(p_, feats, tgt64, lam=M.LAMBDA_ANCHOR,
                                    anchor_rgb=inc64, weights=weights)[0]

        _, _, _, grads = M.loss_and_grads(p, feats, tgt64, lam=M.LAMBDA_ANCHOR,
                                          anchor_rgb=inc64, weights=weights)

        atol, rtol = 1e-6, 1e-4
        worst_excess, worst_name, worst_detail = -np.inf, "", ""
        for name, _ in M.LAYER_SHAPES:
            flat = p[name].ravel()
            idx = (range(flat.size) if flat.size <= 32
                   else range(0, flat.size, max(1, flat.size // 24)))
            for i in idx:
                err, numeric, eps = _best_fd(f, p, name, i,
                                             float(grads[name].ravel()[i]))
                analytic = float(grads[name].ravel()[i])
                tol = atol + rtol * max(abs(numeric), abs(analytic))
                excess = err - tol
                if excess > worst_excess:
                    worst_excess = excess
                    worst_name = f"{name}[{i}]"
                    worst_detail = (f"numeric {numeric:.3e}, analytic {analytic:.3e}, "
                                    f"|diff| {err:.3e}, tol {tol:.3e}, eps {eps:.0e}")
        self.assertLess(worst_excess, 0.0,
                        f"weighted gradient mismatch at {worst_name}: {worst_detail}")

    def test_weighted_loss_matches_an_independent_computation(self):
        """The VALUE, not just the gradient: the weighted loss must measure
        relative error, so a pixel with a large separation must be worth less."""
        rng = np.random.default_rng(31)
        p = M.init_params(rng)
        inc, raw, target = make_batch(64, seed=32)
        feats = M.features(inc, raw)
        w = M.separation_weights(inc, raw)

        loss, mse, anchor, _ = M.loss_and_grads(p, feats, target,
                                                lam=M.LAMBDA_ANCHOR, anchor_rgb=inc,
                                                weights=w)
        # Independent recomputation.
        out, _ = M.predict(p, inc, raw)
        diff = out - target
        W = float(w.sum())
        expect = float(np.sum(w[:, None] * diff * diff) / (3.0 * W))
        self.assertAlmostEqual(mse, expect, places=6)
        self.assertAlmostEqual(loss, mse + M.LAMBDA_ANCHOR * anchor, places=9)

    def test_separation_weights_behave(self):
        """The weighting itself: exactly-agreeing pixels keep weight ~1, pixels
        far apart are worth almost nothing, and the array has the right shape."""
        inc = np.tile(np.array([[10.0, 20.0, 30.0]]), (5, 1))
        raw = np.array([[10.0, 20.0, 30.0],      # sep 0
                        [11.0, 21.0, 31.0],      # sep 1
                        [12.0, 22.0, 32.0],      # sep 2
                        [42.0, 52.0, 62.0],      # sep 32
                        [16.0, 26.0, 36.0]])     # sep 6
        w = M.separation_weights(inc, raw)
        self.assertEqual(w.shape, (5,))
        self.assertTrue(np.all(w > 0))
        # sep 0 -> 1/(0+1) = 1
        self.assertAlmostEqual(float(w[0]), 1.0, places=9)
        # sep 32 -> 1/(1024+1)
        self.assertAlmostEqual(float(w[3]), 1.0 / 1025.0, places=9)
        # monotone decreasing in the separation
        order = np.argsort([0, 1, 2, 32, 6])
        self.assertTrue(np.all(np.diff(w[order]) < 0))
        # And a mismatched arm is refused rather than silently broadcast.
        with self.assertRaises(ValueError):
            M.separation_weights(np.zeros((3, 3)), np.zeros((4, 3)))

    def test_gradient_direction_holds_in_float32(self):
        """The dtype the model actually runs in, asked only what round-off cannot
        answer wrongly: does a step against the gradient lower the loss?"""
        rng = np.random.default_rng(91)
        p = M.init_params(rng)
        for name, shape in M.LAYER_SHAPES:
            p[name] = rng.normal(0.0, 0.3, shape).astype(np.float32)
        inc, raw, target = make_batch(256, seed=92)
        feats = M.features(inc, raw)

        loss0, _, _, grads = M.loss_and_grads(p, feats, target, lam=M.LAMBDA_ANCHOR,
                                              anchor_rgb=inc)
        # A LINE SEARCH, not one fixed step. A fixed step cannot answer "is this a
        # descent direction": measured here, scale=1e-3 with a gradient of ~1.8e3
        # moves a parameter by 1.8 units and the loss went 1383 -> 347650. That is
        # overshoot, not a wrong gradient -- the analytic gradient is verified
        # against finite differences elsewhere in this file.
        #
        # "Descent direction" MEANS there is a small enough step that lowers the
        # loss, so the test must look for one. A wrong gradient fails every scale.
        lowered = []
        for scale in (1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7):
            q = {k: (v - scale * grads[k].astype(np.float32)).astype(np.float32)
                 for k, v in p.items()}
            l1, _, _, _ = M.loss_and_grads(q, feats, target, lam=M.LAMBDA_ANCHOR,
                                           anchor_rgb=inc)
            lowered.append((scale, float(l1)))
        best_scale, best_loss = min(lowered, key=lambda t: t[1])
        self.assertLess(best_loss, loss0,
                        f"no scale lowered the loss; loss0={loss0}, tried {lowered}")

    def test_loss_decreases_with_gradient_steps(self):
        """The gradient must actually be a descent direction."""
        rng = np.random.default_rng(11)
        p = M.init_params(rng)
        inc, raw, target = make_batch(512, seed=12)
        feats = M.features(inc, raw)
        opt = M.Adam(p, lr=3e-3)
        first = None
        for _ in range(60):
            loss, _, _, grads = M.loss_and_grads(p, feats, target, lam=M.LAMBDA_ANCHOR,
                                                 anchor_rgb=inc)
            if first is None:
                first = loss
            opt.step(p, grads)
        last, _, _, _ = M.loss_and_grads(p, feats, target, lam=M.LAMBDA_ANCHOR,
                                         anchor_rgb=inc)
        self.assertLess(last, first * 0.5, f"loss {first:.4f} -> {last:.4f}")


class TestLearning(unittest.TestCase):

    def test_training_beats_the_incumbent_on_a_held_out_split(self):
        """End to end on a synthetic problem the head SHOULD solve: the incumbent
        is a blur of the input, so the correction is to restore the input's
        detail. Held-out data, so this cannot pass by memorisation."""
        rng = np.random.default_rng(13)
        p = M.init_params(rng)
        inc_tr, raw_tr, tgt_tr = make_batch(2048, seed=14)
        inc_te, raw_te, tgt_te = make_batch(1024, seed=15)
        f_tr = M.features(inc_tr, raw_tr)
        f_te = M.features(inc_te, raw_te)

        before = psnr(M.predict(p, inc_te, raw_te)[0], tgt_te)
        opt = M.Adam(p, lr=3e-3)
        for _ in range(300):
            _, _, _, grads = M.loss_and_grads(p, f_tr, tgt_tr, lam=M.LAMBDA_ANCHOR,
                                              anchor_rgb=inc_tr)
            opt.step(p, grads)
        after = psnr(M.predict(p, inc_te, raw_te)[0], tgt_te)
        self.assertGreater(after, before,
                           f"held-out PSNR did not improve: {before:.3f} -> {after:.3f}")

    def test_the_anchor_term_keeps_the_correction_small(self):
        """A large `lam` must produce a smaller correction than a small one. This
        is what stops the head from 'improving' by replacing the frame."""
        def train_and_measure(lam: float) -> float:
            rng = np.random.default_rng(16)
            p = M.init_params(rng)
            inc, raw, target = make_batch(1024, seed=17)
            # Target far from the incumbent, so the mse term WANTS a big move.
            target = np.clip(target + 40.0, 0.0, 255.0).astype(np.float32)
            feats = M.features(inc, raw)
            opt = M.Adam(p, lr=3e-3)
            for _ in range(150):
                _, _, _, grads = M.loss_and_grads(p, feats, target, lam=lam,
                                                  anchor_rgb=inc)
                opt.step(p, grads)
            out, _ = M.predict(p, inc, raw)
            return float(np.mean(np.abs(out - inc)))

        small = train_and_measure(0.001)
        large = train_and_measure(1.0)
        self.assertLess(large, small,
                        f"a larger anchor penalty must shrink the correction "
                        f"({large:.3f} vs {small:.3f})")


class TestPayloadABI(unittest.TestCase):
    """The blob payload: weights THEN the affine.

    The affine is in the payload because the weights are meaningless without it
    and nothing would error. These tests exist so that a future edit cannot move
    the affine out (into shader constants, say) without failing here.
    """

    def test_payload_layout(self):
        self.assertEqual(M.AFFINE_OFFSET, M.N_WEIGHTS)
        self.assertEqual(M.N_FEAT, M.N_IN + M.N_DIFF)
        self.assertEqual(M.N_PAYLOAD, M.BLEND_OFFSET + M.N_INCUMBENT)
        self.assertEqual(M.BLEND_OFFSET, M.AFFINE_OFFSET + 2 * M.N_FEAT)

    def test_payload_round_trips_weights_and_affine(self):
        rng = np.random.default_rng(11)
        p = M.init_params(rng)
        mean = rng.uniform(10.0, 40.0, M.N_FEAT)
        std = rng.uniform(1.0, 9.0, M.N_FEAT)
        vec = M.pack_payload(p, mean, std)
        self.assertEqual(vec.shape, (M.N_PAYLOAD,))
        back, bmean, bstd, balpha = M.unpack_payload(vec)
        for name, _ in M.LAYER_SHAPES:
            self.assertTrue(np.array_equal(p[name], back[name]), name)
        self.assertTrue(np.array_equal(bmean, mean.astype(np.float32)))
        self.assertTrue(np.array_equal(bstd, std.astype(np.float32)))

    def test_the_blob_affine_is_the_affine_features_applies(self):
        """THE invariant that makes the shader correct.

        `features()` standardises with FEATURE_MEAN/FEATURE_STD; the shader will
        standardise with whatever the blob carries. If those two ever diverge, the
        image is plausible and every weight is wrong. Defaulting `pack_payload` to
        the module constants is what keeps them equal -- this asserts the default
        really does track a later `apply_feature_stats`.
        """
        saved_mean, saved_std = M.FEATURE_MEAN.copy(), M.FEATURE_STD.copy()
        try:
            mean = np.linspace(11.0, 31.0, M.N_FEAT)
            std = np.linspace(1.5, 7.5, M.N_FEAT)
            # A MEASURED mean is always self-consistent: the mean of a difference
            # is the difference of the means. An arbitrary affine is not, and the
            # claim this test checks -- "a pixel at the measured mean standardises
            # to 0 in every feature" -- is only meaningful for one a real dataset
            # could have produced. It also pins mean[6:9] ~ 0, which is what a
            # real measured difference column looks like.
            mean[6:9] = mean[:3] - mean[3:6]
            M.FEATURE_MEAN, M.FEATURE_STD = mean, std
            rng = np.random.default_rng(5)
            p = M.init_params(rng)
            _, bmean, bstd, balpha = M.unpack_payload(M.pack_payload(p))
            self.assertTrue(np.array_equal(bmean, mean.astype(np.float32)))
            self.assertTrue(np.array_equal(bstd, std.astype(np.float32)))
            # And the model really is applying that affine: standardising the
            # measured mean must produce ~0 for that feature.
            inc = np.tile(mean[:3].astype(np.float32), (4, 1))
            raw = np.tile(mean[3:6].astype(np.float32), (4, 1))
            feats = M.features(inc, raw)
            self.assertLess(float(np.max(np.abs(feats))), 1e-5)
        finally:
            M.FEATURE_MEAN, M.FEATURE_STD = saved_mean, saved_std

    def test_refuses_a_payload_without_the_affine(self):
        """A reader that stopped at the weights is the natural mistake, and it
        must fail rather than load a blob whose standardisation it never read."""
        rng = np.random.default_rng(2)
        vec = M.pack_payload(M.init_params(rng))
        with self.assertRaises(ValueError):
            M.unpack_payload(vec[:M.N_WEIGHTS])

    def test_refuses_a_non_positive_std(self):
        rng = np.random.default_rng(2)
        p = M.init_params(rng)
        with self.assertRaises(ValueError):
            M.pack_payload(p, np.zeros(M.N_FEAT), np.zeros(M.N_FEAT))
        bad = M.pack_payload(p, np.zeros(M.N_FEAT), np.ones(M.N_FEAT))
        bad[M.AFFINE_OFFSET + M.N_FEAT] = 0.0
        with self.assertRaises(ValueError):
            M.unpack_payload(bad)

    def test_refuses_a_wrong_shaped_affine(self):
        rng = np.random.default_rng(2)
        p = M.init_params(rng)
        with self.assertRaises(ValueError):
            M.pack_payload(p, np.zeros(M.N_IN + 1), np.ones(M.N_IN + 1))

    def test_identity_is_detectable(self):
        self.assertTrue(M.affine_is_identity(np.zeros(M.N_IN), np.ones(M.N_IN)))
        self.assertFalse(M.affine_is_identity(np.full(M.N_IN, 26.5), np.ones(M.N_IN)))
        self.assertFalse(M.affine_is_identity(np.zeros(M.N_IN), np.full(M.N_IN, 2.0)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
