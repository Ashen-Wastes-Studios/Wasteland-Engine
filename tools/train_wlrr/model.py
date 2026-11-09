"""Wasteland clean-room WLRR neural denoiser-corrector head.

numpy only. No torch, no ONNX, no vendor maths library: the forward pass and the
gradient are written out here, which is the clean-room requirement made concrete
rather than a claim in a README. This is the WLRR sibling of `tools/train_wlfg/
model.py` and deliberately shares its shape, so a reader who knows one knows the
other.

ARCHITECTURE (v3: the gate has its own first layer)
--------------------------------------------------
    per-pixel input = the INCUMBENT denoiser's output RGB, the raw INPUT RGB,
                      and their per-channel SIGNED difference (incumbent - input):
                      3 + 3 + 3 = 9 features.
    global           = not used -- the correction is local by construction.

    The nine features are SPLIT, and the split is the whole design:

      residual trunk :  6 -> 16 -> 16 -> 3      (the two arms ONLY)
      gate branch    :  9 ->  8 ->  1  sigmoid  (arms + signed difference)

    residual: 16 -> 3               -- RGB residual
    gate  :  (9 -> 8) -> 1          -- sigmoid, per pixel

    out = incumbent + gate * residual

The output is a RESIDUAL over the incumbent, never a replacement. With the
residual head initialised to zero, `out == incumbent` EXACTLY at step 0, so a
freshly-initialised (or zeroed) model is a provable no-op rather than an
approximately-neutral one. That is the same safety property the WLFG head has,
and it is why this shape was chosen: the worst case is "the incumbent's own
output", never "a wrong frame".

WHY THE ANCHOR IS THE INCUMBENT AND NOT THE INPUT
-------------------------------------------------
Task 9 measured the incumbent (SVGF A-Trous, pass 13) at partial convergence:
PSNR +0.309 dB but SSIM -0.0366, max|d| 43 -> 30. It removes worst-case error
while costing structure -- it SMEARS DETAIL. The residual this head learns is
therefore the detail the filter destroyed, which is structured and learnable, not
noise. Training against the raw input as anchor would instead ask the head to
reproduce a filter it cannot see.

WHY EXACTLY THESE SIX INPUTS -- AND WHY NOT MASK AND DEPTH
----------------------------------------------------------
1. The incumbent's RGB is the anchor: `out = incumbent + gate*residual` needs it
   to compute the residual, and it is what the composite falls back to.
2. The raw input RGB is what the incumbent saw. Without it the head cannot know
   WHAT the filter changed -- `incumbent` alone is a function of `input`, so the
   pair is the smallest set that carries the filter's effect. Their difference IS
   the signal the head learns to refine.

WHY THE DIFFERENCE IS GATE-ONLY -- three attempts, two failures, one mechanism
---------------------------------------------------------------------------
v1 (6 inputs, no difference anywhere) failed in a specific, measured way: it
helped where the incumbent was far from the input (+0.30 dB on a separation-31.9
scene) and hurt where the incumbent was already right (-1.62 dB on a
separation-2.8 scene). The target spans 1.9 to 31.9 across scenes, so the head
cannot tell the two regimes apart from the arms alone.

v2 gave the difference to a SHARED trunk (9 inputs for both heads) and it made
things far worse: **-12.8 dB**, reverted 296% of the way to the input, loss
climbing to 43.7. The mechanism is the failure the diagnosis misses: the
difference feature is visible to the RESIDUAL as well as the gate, so it hands
the head a **"rewind" button** -- `residual ~= -k*(incumbent-input)` gives
`out = incumbent - k*(incumbent-input)`, which for k>1 overshoots PAST the input.
**The gate needed the signal; the residual abused it.**

v3 is this file: the difference goes to the GATE's own first layer and to nothing
else. The trunk never sees it, so the rewind cannot be expressed; the gate can
still read the regime and close where the incumbent is already right. That is a
testable prediction, and it is being measured.

An earlier revision of this file specified EIGHT inputs, adding the reactive mask
and depth as conditioning. That was reverted, and the reason is worth recording
because the argument for them was sound and the ABI was still wrong: **the mask
and depth are not in the WLRR dataset, so the ABI could not be fed.** The neural
exporter captures RGBA arms and its PNG reader normalises every capture to three
channels; the mask (`img_Mask`, u14) and depth buffers are exported by the WLFG
exporter's `BUFFERS` table, which the WLRR arms do not use. An input the training
data cannot supply is not a feature, it is a defect that surfaces later as a
shader reading uninitialised memory.

Adding them back is a v2 ABI and needs one thing first: mask/depth arms in the
WLRR export (the WLFG exporter's `BUFFERS` + `FLOAT_BUFFERS` machinery is the
model to copy). Until then, six inputs is the honest contract and the head is not
crippled by it -- the mask says "the engine distrusts this region" and depth says
"there is an edge here", but both are already legible in the pair: the input
carries its own gradients and the incumbent's smearing shows up as a difference
between the arms.

Velocity is deliberately NOT an input. The WLFG head needs it because it corrects
a REPROJECTED frame and must know how far off the reprojection is; the WLRR head
corrects a spatially-denoised frame in screen space and never reprojects, so
velocity would be a constant-shaped distraction.

WHY A SMALL NET
---------------
Same reasoning as the WLFG head: the correction is local, the inputs are a
handful of per-pixel scalars, and ~450 parameters cannot memorise a scene.
"""

from __future__ import annotations

import math

import numpy as np

# ---------------------------------------------------------------------------
# fixed architecture constants (the ABI; the shader must match)
# ---------------------------------------------------------------------------

N_INCUMBENT = 3    # the incumbent denoiser's output RGB -- the anchor
N_INPUT = 3        # the raw input RGB the incumbent filtered
# NOTE: there is deliberately no N_MASK / N_DEPTH here. An earlier revision had
# them (8 inputs), but the WLRR dataset cannot supply mask or depth -- see the
# module docstring. An input the training data cannot provide is a defect, not a
# feature.
N_IN = N_INCUMBENT + N_INPUT          # 6: what the RESIDUAL trunk sees
N_DIFF = 3                            # the per-channel signed difference
                                      # incumbent - input: the GATE's regime signal
N_GATE_IN = N_IN + N_DIFF             # 9: what the GATE sees
N_HIDDEN = 16
N_GATE_HIDDEN = 8                     # the gate's own first layer
N_RESIDUAL = 3
N_GATE = 1

# Layer order in the weight blob. The exporter and the shader must agree on this
# exactly; `export_weights.py` writes it and `test_model.py` round-trips it.
LAYER_SHAPES = [
    # The residual trunk sees ONLY the two arms (N_IN = 6). The gate's first
    # layer sees the arms PLUS their signed difference (N_GATE_IN = 9).
    #
    # This split is the whole design, and it exists because of a measured
    # failure: when the difference was added to the SHARED trunk (9 inputs for
    # both heads), the residual learned `residual ~= -k*(incumbent-input)` --
    # a rewind button -- and the holdout lost 12.8 dB. The gate needed the
    # regime signal; the residual abused it. So the signal goes to the gate and
    # nowhere else.
    ("W1", (N_IN, N_HIDDEN)),
    ("b1", (N_HIDDEN,)),
    ("W2", (N_HIDDEN, N_HIDDEN)),
    ("b2", (N_HIDDEN,)),
    ("Wr", (N_HIDDEN, N_RESIDUAL)),
    ("br", (N_RESIDUAL,)),
    ("Wg1", (N_GATE_IN, N_GATE_HIDDEN)),
    ("bg1", (N_GATE_HIDDEN,)),
    ("wg2", (N_GATE_HIDDEN, N_GATE)),
    ("bg2", (N_GATE,)),
]

N_WEIGHTS = int(sum(int(np.prod(s)) for _, s in LAYER_SHAPES))

# Per-feature standardisation. PART OF THE ABI: the shader must apply the same
# affine before the first matmul, or every weight is wrong -- and NOTHING would
# error. A head trained against standardised features but evaluated against raw
# 0..255 pixels produces a plausible-looking image with every weight wrong.
#
# This is why the affine travels INSIDE the blob payload (see AFFINE_OFFSET)
# rather than being a pair of constants in the shader: the weights and the affine
# they require cannot then drift apart, because they are one artefact.
#
# The WLFG head ships the identity because it was trained that way. This head is
# the first with a MEASURED affine, so it is the first where this matters.
FEATURE_MEAN = np.zeros(N_IN + N_DIFF, dtype=np.float64)
FEATURE_STD = np.ones(N_IN + N_DIFF, dtype=np.float64)
assert FEATURE_MEAN.shape == (N_IN + N_DIFF,) and FEATURE_STD.shape == (N_IN + N_DIFF,)
assert np.all(FEATURE_STD > 0)

# The ANCHOR BLEND weight. `out = anchor + gate*residual` with
# `anchor = incumbent + alpha*(input - incumbent)`.
#
# WHY THIS IS NOT THE INCUMBENT ANY MORE -- measured, not designed:
# a single global scalar alpha beats BOTH arms on 11/11 scenes
# (tools/train_wlrr/diagnose_oracle.py). Example3D 44.399/43.698 -> 45.225 with
# alpha=0.413; MaterialGallery 33.552/35.212 -> 35.613 with alpha=0.715; ScriptTest
# 24.236/31.004 -> 31.186 with alpha=1.116. The optimal correction is largely a
# BLEND toward the input, and the head was being asked to produce the whole
# correction from an anchor that is the WORST of the two arms on several scenes.
#
# Anchoring at the measured blend makes the head's job the per-pixel DEVIATION
# from a near-optimal starting point -- much better conditioned, and it is the
# difference between "the head must rediscover a blend" and "the head refines a
# blend". The blend remains a reported baseline: the head must beat IT, not just
# the incumbent.
#
# alpha=0 reproduces the old incumbent anchor exactly, so this is a strict
# generalisation and the previous results are still reproducible.
BLEND_ALPHA = np.zeros(N_INCUMBENT, dtype=np.float64)

# Clip the STANDARDISED features to +/- this many sigma.
#
# WHY: ScriptTest's separation is 10.089 against 1.997 for the next-highest of
# eleven scenes -- 5x outside the training distribution. After standardisation
# its difference feature sits many sigma out, the ReLU/sigmoid saturate, and the
# head cannot respond at all: measured, its deviation from the blend on that
# scene is 0.0150/255 (it does nothing) while the blend itself is 0.92 dB below
# the raw input. Clipping is the standard remedy for out-of-distribution inputs
# and it is a NO-OP for in-distribution data (nothing near the clip bound).
#
# None disables clipping (the pre-clip behaviour).
FEATURE_CLIP: float | None = None

# Payload layout: the flattened weights, then mean[N_FEAT], then std[N_FEAT],
# where N_FEAT is the FULL feature width (arms + difference).
# The affine is APPENDED rather than put in the header so the 24-byte header
# stays byte-identical across all three heads and one reader still serves them.
AFFINE_OFFSET = N_WEIGHTS
N_FEAT = N_IN + N_DIFF
BLEND_OFFSET = AFFINE_OFFSET + 2 * N_FEAT
N_PAYLOAD = BLEND_OFFSET + N_INCUMBENT

# A gate below this is, for practical purposes, "the network declined to act on
# this pixel".
GATE_FLOOR = 0.05

# Loss weight for the anchor term: `lam * mean((gate*residual)^2)`. Trades
# correction against fidelity to the incumbent's own output.
LAMBDA_ANCHOR = 0.05


def features(incumbent: np.ndarray, raw_input: np.ndarray) -> np.ndarray:
    """(N,3) incumbent RGB + (N,3) raw input RGB -> (N,9) standardised input.

    The third group is the per-channel SIGNED difference `incumbent - input`. It
    is the regime signal -- how far the incumbent moved this pixel -- and it is
    deliberately NOT part of the residual trunk's input: see the docstring's
    "WHY THE DIFFERENCE IS DELIBERATELY NOT AN INPUT" section, and `LAYER_SHAPES`.

    `forward` slices it: the trunk uses `pixel[:, :N_IN]`, the gate uses all nine.

    The caller must fetch both at the SAME pixel coords. In the shader that is
    two reads of already-bound images; during training the pairs come from the
    exported dataset, whose `--validate` step checks that every arm at a given
    scene+camera has identical dimensions and is therefore pixel-comparable.
    """
    inc = np.asarray(incumbent, dtype=np.float32).reshape(-1, N_INCUMBENT)
    inp = np.asarray(raw_input, dtype=np.float32).reshape(-1, N_INPUT)
    diff = (inc - inp).astype(np.float32)
    x = np.concatenate([inc, inp, diff], axis=1).astype(np.float64)
    z = (x - FEATURE_MEAN) / FEATURE_STD
    if FEATURE_CLIP is not None:
        # Inclusive clip, and it MUST be symmetric: an asymmetric clip would
        # systematically bias the correction toward one arm.
        z = np.clip(z, -FEATURE_CLIP, FEATURE_CLIP)
    return z.astype(np.float32)


def init_params(rng: np.random.Generator, scale: float = 1.0) -> dict:
    """He initialisation, assuming standardised inputs. The residual head is
    zeroed separately so the model starts as the identity -- out == incumbent
    exactly at step 0."""
    p = {}
    for name, shape in LAYER_SHAPES:
        if name.startswith("W"):
            fan_in = shape[0]
            p[name] = rng.normal(0.0, scale * np.sqrt(2.0 / fan_in), shape).astype(np.float32)
        else:
            p[name] = np.zeros(shape, dtype=np.float32)
    # Zero the residual head: the model IS the incumbent at step 0.
    p["Wr"] = np.zeros((N_HIDDEN, N_RESIDUAL), dtype=np.float32)
    p["br"] = np.zeros((N_RESIDUAL,), dtype=np.float32)
    return p


def forward(p: dict, pixel: np.ndarray, anchor_rgb: np.ndarray):
    """Returns (out, cache). `pixel` is the STANDARDISED (N,9) feature vector.

    `anchor_rgb` is the INCUMBENT's RGB -- not the raw input. See the module
    docstring for why that distinction is the whole design.

    THE SPLIT, and why it is not cosmetic: the trunk -- `pixel[:, :N_IN]`, the two
    arms only -- produces the residual. The gate gets all nine columns including
    the signed difference, through its OWN first layer. The gate's branch feeds
    nothing back into the trunk, so the residual cannot express
    `residual = -k*(incumbent-input)`. Measured: the shared-trunk version did
    exactly that and lost 12.8 dB.
    """
    x_trunk = pixel[:, :N_IN]
    z1 = x_trunk @ p["W1"] + p["b1"]
    h1 = np.maximum(z1, 0.0)
    z2 = h1 @ p["W2"] + p["b2"]
    h2 = np.maximum(z2, 0.0)

    residual = h2 @ p["Wr"] + p["br"]

    zg1 = pixel @ p["Wg1"] + p["bg1"]
    hg1 = np.maximum(zg1, 0.0)
    gate = 1.0 / (1.0 + np.exp(-(hg1 @ p["wg2"] + p["bg2"])))

    out = anchor_rgb + gate * residual
    cache = (pixel, anchor_rgb, z1, h1, z2, h2, residual, gate, zg1, hg1)
    return out, cache


# ---------------------------------------------------------------------------
# Adam + cosine lr schedule (written out, no framework)
# ---------------------------------------------------------------------------

class Adam:
    """Adam, lr 3e-3 with cosine decay -- the WLFG training config, reused so the
    two heads are tuned identically and a comparison between them is meaningful.

    `clip_norm` is GLOBAL gradient-norm clipping. It is not decoration: both the
    unweighted and the weighted run ended WORSE than their own best point
    (unweighted loss 1.160 at step 2500 -> 3.143 at 3000; weighted 1.696 -> 5.735),
    i.e. the last 500-1000 steps of the schedule were destroying the model. The
    per-batch separation weights vary by orders of magnitude, so some batches
    produce enormous gradients, and a single one of those can wreck a checkpoint.
    """

    def __init__(self, params: dict, lr: float = 3e-3, b1: float = 0.9,
                 b2: float = 0.999, eps: float = 1e-8, clip_norm: float | None = 1.0):
        self.lr, self.b1, self.b2, self.eps = lr, b1, b2, eps
        self.clip_norm = clip_norm
        self.m = {k: np.zeros_like(v) for k, v in params.items()}
        self.v = {k: np.zeros_like(v) for k, v in params.items()}
        self.t = 0

    def step(self, params: dict, grads: dict) -> None:
        self.t += 1
        if self.clip_norm is not None:
            total = 0.0
            for k in params:
                total += float(np.sum(np.asarray(grads[k], dtype=np.float64) ** 2))
            norm = math.sqrt(total)
            if norm > self.clip_norm and norm > 0.0:
                scale = self.clip_norm / norm
                grads = {k: np.asarray(grads[k], dtype=np.float64) * scale
                         for k in grads}
        for k in params:
            g = grads[k]
            self.m[k] = self.b1 * self.m[k] + (1 - self.b1) * g
            self.v[k] = self.b2 * self.v[k] + (1 - self.b2) * (g * g)
            mhat = self.m[k] / (1 - self.b1 ** self.t)
            vhat = self.v[k] / (1 - self.b2 ** self.t)
            params[k] -= self.lr * mhat / (np.sqrt(vhat) + self.eps)


def cosine_lr(base: float, step: int, total: int) -> float:
    import math
    return base * 0.5 * (1.0 + math.cos(math.pi * min(step, total) / max(total, 1)))


def separation_weights(incumbent: np.ndarray, raw_input: np.ndarray,
                       floor: float = 1.0) -> np.ndarray:
    """Per-pixel weight `1 / (sep² + floor)`, where sep = mean|incumbent - input|.

    WHAT FOR
    --------
    The target signal is heteroscedastic by ~17x across scenes: `mean|incumbent -
    input|` runs 1.9 to 31.9 (measured, `tools/train_wlrr/diagnose_affine.py`). An
    unweighted MSE is therefore dominated by the few high-separation scenes --
    a separation-32 pixel contributes ~256x the squared error of a separation-2
    one -- so the easy scenes, where the incumbent is already right, contribute
    almost nothing to the gradient and the head never learns not to touch them.

    Weighting by the inverse separation makes the loss measure RELATIVE error
    instead of absolute. A pixel where the arms agree exactly gets weight 1 (the
    `floor`), so the head is still told to reproduce it; a pixel 32 apart gets
    ~1/1025, so it must be corrected to within a few percent rather than to
    within an absolute count.

    `floor` is what keeps the exactly-agreeing pixels in the loss at all.
    """
    inc = np.asarray(incumbent, dtype=np.float64)
    inp = np.asarray(raw_input, dtype=np.float64)
    if inc.shape != inp.shape:
        raise ValueError(f"arm shapes differ: {inc.shape} vs {inp.shape}")
    sep = np.mean(np.abs(inc - inp), axis=1)
    return (1.0 / (sep * sep + float(floor))).astype(np.float64)


def loss_and_grads(p: dict, pixel: np.ndarray, target: np.ndarray, lam: float = LAMBDA_ANCHOR,
                   anchor_rgb: np.ndarray | None = None,
                   weights: np.ndarray | None = None):
    """L = mean((out - target)^2) + lam * mean((gate*residual)^2).

    `lam` prices the anchor term, which keeps the correction small where the
    incumbent is already right. `anchor_rgb` must be the incumbent's RGB; it
    defaults to `pixel[:, :N_INCUMBENT]` only for unstandardised v0 callers.

    `weights` is an optional per-sample weight (N,) -- see `separation_weights`.
    When given, BOTH loss terms become weighted means with the same normaliser
    `m = 3 * sum(w)`, so the anchor term prices corrections hardest exactly where
    the arms agree. `None` is bit-identical to the unweighted path.
    """
    if anchor_rgb is None:
        anchor_rgb = pixel[:, :N_INCUMBENT]
    # np.mean() over an (N,3) array divides by 3N, not N. Getting this wrong
    # scales the whole gradient by exactly N_RESIDUAL -- caught by the
    # finite-difference check in test_model.py, which is why that check exists.
    n = pixel.shape[0]
    if weights is None:
        w = np.ones(n, dtype=np.float64)
    else:
        w = np.asarray(weights, dtype=np.float64).reshape(-1)
        if w.shape != (n,):
            raise ValueError(f"expected ({n},) weights, got {w.shape}")
        if not np.all(np.isfinite(w)) or np.any(w < 0.0):
            raise ValueError("weights must be finite and non-negative")
    total_w = float(np.sum(w))
    if total_w <= 0.0:
        raise ValueError("the weights sum to zero -- the loss is undefined")
    m = n * N_RESIDUAL if weights is None else N_RESIDUAL * total_w
    out, (xc, anc, z1, h1, z2, h2, residual, gate, zg1, hg1) = forward(p, pixel, anchor_rgb)
    diff = out - target
    wc = w[:, None]
    mse = float(np.sum(wc * diff * diff) / m)
    gr = gate * residual
    anchor_pen = float(np.sum(wc * gr * gr) / m)
    loss = mse + lam * anchor_pen

    dout = (2.0 / m) * wc * diff
    dres = dout * gate + (2.0 * lam * wc / m) * (gate * gate) * residual
    dgate = (np.sum(dout * residual, axis=1, keepdims=True)
             + (2.0 * lam * wc / m) * np.sum(gate * (residual * residual), axis=1,
                                             keepdims=True))

    grads = {}
    grads["Wr"] = h2.T @ dres
    grads["br"] = dres.sum(axis=0)

    # The gate's own branch. NOTE it does not touch the trunk: `dgate` is the only
    # term feeding it, and nothing from here reaches `dh2`. That one-directional
    # dependency is what keeps the residual from seeing the difference.
    dzg2 = dgate * gate * (1.0 - gate)
    grads["wg2"] = hg1.T @ dzg2
    grads["bg2"] = dzg2.sum(axis=0)
    dhg1 = dzg2 @ p["wg2"].T
    dzg1 = dhg1 * (zg1 > 0)
    grads["Wg1"] = xc.T @ dzg1
    grads["bg1"] = dzg1.sum(axis=0)

    # The trunk's gradient comes from the residual ONLY.
    dh2 = dres @ p["Wr"].T
    dz2 = dh2 * (z2 > 0)
    grads["W2"] = h1.T @ dz2
    grads["b2"] = dz2.sum(axis=0)
    dh1 = dz2 @ p["W2"].T
    dz1 = dh1 * (z1 > 0)
    grads["W1"] = xc[:, :N_IN].T @ dz1
    grads["b1"] = dz1.sum(axis=0)

    return loss, mse, anchor_pen, grads


# ---------------------------------------------------------------------------
# predict: per-pixel forward using the same features as training.
# ---------------------------------------------------------------------------

def blend_anchor(incumbent: np.ndarray, raw_input: np.ndarray,
                 alpha: np.ndarray | None = None) -> np.ndarray:
    """The measured anchor: `incumbent + alpha * (input - incumbent)`.

    alpha=0 gives the incumbent exactly, so every pre-blend result is still
    reproducible by setting BLEND_ALPHA to zero.
    """
    a = BLEND_ALPHA if alpha is None else np.asarray(alpha, dtype=np.float64).reshape(-1)
    if a.shape != (N_INCUMBENT,):
        raise ValueError(f"alpha must have shape ({N_INCUMBENT},), got {a.shape}")
    inc = np.asarray(incumbent, dtype=np.float64)
    inp = np.asarray(raw_input, dtype=np.float64)
    return inc + a[None, :] * (inp - inc)


def predict(p: dict, incumbent: np.ndarray, raw_input: np.ndarray
            ) -> tuple[np.ndarray, np.ndarray]:
    """Per-pixel forward: out = blend_anchor + gate * residual.

    Returns:
        out:  (N,3) corrected RGB.
        gate: (N,) gate, 1 = apply the full correction, 0 = keep the anchor.
    """
    feats = features(incumbent, raw_input)
    out, cache = forward(p, feats, blend_anchor(incumbent, raw_input))
    return out, cache[7].reshape(-1)


def pack(p: dict) -> np.ndarray:
    """Concatenate the parameters in LAYER_SHAPES order -> flat fp32 vector."""
    return np.concatenate([np.asarray(p[name], dtype=np.float32).ravel()
                           for name, _ in LAYER_SHAPES])


def unpack(vec: np.ndarray) -> dict:
    """Inverse of `pack`. Refuses a length mismatch rather than truncating."""
    if vec.size != N_WEIGHTS:
        raise ValueError(f"expected {N_WEIGHTS} weights, got {vec.size}")
    p, off = {}, 0
    for name, shape in LAYER_SHAPES:
        k = int(np.prod(shape))
        p[name] = vec[off:off + k].reshape(shape).astype(np.float32)
        off += k
    return p


def pack_payload(p: dict, mean=None, std=None, alpha=None) -> np.ndarray:
    """The blob payload: weights, mean[N_FEAT], std[N_FEAT], alpha[N_INCUMBENT].

    The affine and the blend weight go in the payload so the weights and the
    preprocessing/anchor they require are ONE artefact. The alternative -- shader
    constants -- is the failure this whole design exists to prevent: someone
    retrains, updates the weights file, forgets the constants, and gets a
    plausible image with every weight wrong and no error anywhere.

    Defaults to the module's current affine/blend, so an exporter that has just
    called `dataset.apply_feature_stats` / `apply_blend_alpha` needs no arguments.
    """
    mean = FEATURE_MEAN if mean is None else mean
    std = FEATURE_STD if std is None else std
    alpha = BLEND_ALPHA if alpha is None else alpha
    mean = np.asarray(mean, dtype=np.float32).reshape(-1)
    std = np.asarray(std, dtype=np.float32).reshape(-1)
    alpha = np.asarray(alpha, dtype=np.float32).reshape(-1)
    if mean.shape != (N_FEAT,) or std.shape != (N_FEAT,):
        raise ValueError(f"expected ({N_FEAT},) mean and std, got {mean.shape} and {std.shape}")
    if alpha.shape != (N_INCUMBENT,):
        raise ValueError(f"expected ({N_INCUMBENT},) alpha, got {alpha.shape}")
    if not np.all(np.isfinite(mean)) or not np.all(np.isfinite(std)):
        raise ValueError("the affine must be finite")
    if np.any(std <= 0.0):
        raise ValueError("std must be strictly positive; a zero would divide by zero")
    return np.concatenate([pack(p), mean, std, alpha]).astype(np.float32)


def unpack_payload(vec: np.ndarray) -> tuple[dict, np.ndarray, np.ndarray, np.ndarray]:
    """Inverse of `pack_payload` -> (params, mean, std, alpha).

    Refuses any length other than N_PAYLOAD. A reader that truncated to the
    weights and ignored a trailing affine would load a blob written for a
    different standardisation and never say so.
    """
    vec = np.asarray(vec, dtype=np.float32).reshape(-1)
    if vec.size != N_PAYLOAD:
        raise ValueError(f"expected {N_PAYLOAD} payload floats "
                         f"({N_WEIGHTS} weights + {2 * N_FEAT} affine "
                         f"+ {N_INCUMBENT} blend), got {vec.size}")
    p = unpack(vec[:N_WEIGHTS])
    mean = vec[AFFINE_OFFSET:AFFINE_OFFSET + N_FEAT]
    std = vec[AFFINE_OFFSET + N_FEAT:AFFINE_OFFSET + 2 * N_FEAT]
    alpha = vec[BLEND_OFFSET:BLEND_OFFSET + N_INCUMBENT]
    if np.any(std <= 0.0):
        raise ValueError("blob carries a non-positive std")
    if not np.all(np.isfinite(alpha)):
        raise ValueError("blob carries a non-finite blend alpha")
    return p, mean, std, alpha


def affine_is_identity(mean, std, tol: float = 1e-9) -> bool:
    """True when the affine is exactly the identity (mean 0, std 1).

    Used by the exporter to refuse shipping an untrained-affine blob by accident:
    the identity is legitimate for a deliberately untrained or control head, and
    is a silent, total corruption for a head trained against measured statistics.
    """
    mean = np.asarray(mean, dtype=np.float64).reshape(-1)
    std = np.asarray(std, dtype=np.float64).reshape(-1)
    return bool(np.all(np.abs(mean) <= tol) and np.all(np.abs(std - 1.0) <= tol))
