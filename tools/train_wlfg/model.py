"""Wasteland clean-room WLFG neural frame synthesis head.

numpy only. No torch, no ONNX, no vendor maths library: the forward pass and the
gradient are written out here, which is the clean-room requirement made concrete
rather than a claim in a README.

ARCHITECTURE (v1 -- deliberately small, per ``.hermes/plans/...`` Task 8 Step 2)
--------------------
    per-pixel input = the warped frame's RGB, the reactive mask, the velocity,
                      and the depth: 3 + 1 + 2 + 1 = 7 features.
    global           = not used -- a per-pixel map cannot know the scene
    (the plan's factor-gated schedule keeps the correction scene-local anyway)

    stage1: 7 -> 16 -> 16          (shared representation)
    residual: 16 -> 3                -- RGB residual, added to the warped RGB
    gate  : 16 -> 1                  -- sigmoid, per pixel

    out = anchor + gate * residual      anchor = warped frame's RGB

The output is a RESIDUAL, never a replacement. `anchor` is the warped frame and
`gate` in [0,1] is learned per pixel, so the worst case (network wrong) degrades
to "no enhancement" instead of "a wrong frame is shown".

WHY THESE INPUTS
----------------
1. The reactive mask is the strongest signal about where the warp is unreliable
   (occlusion, depth discontinuity): the plan's reactive-mask pass (pass 24)
   marks exactly these regions. Conditioning on it lets the head decide whether
   to trust the warp or fall back to the real frame.
2. The velocity and depth are the warp's own reprojection state: they tell the
   head how far off the previous present is. Depth discontinuities are exactly
   where the warp's address test (|curDepth - prevDepth| > 0.02) rejects, so
   feeding depth lets the head disambiguate.
3. The RGB is the anchor itself -- the engine's reprojected present. Everything
   else is conditioning. This is the same convention as the existing neural
   stage (`RunNeuralFeatures` reads `r,g,b` straight from `s_Output`, the same
   texture pass 17 reads, so it costs no extra register).

WHY NOT THE EXISTING STAGE'S (RGB + LUMA + GLOBAL)
---------------------------------------------------
The existing neural stage is scene-conditioned (per-frame global mean colour +
luma) and operates on the raw degraded frame. The WLFG head is *per-frame* but
needs the warp's state (mask, velocity, depth) so it can decide, per pixel,
whether to apply the warped frame or fall back to the real frame. It is
therefore the warp's sibling, not its successor, and it does not need the
scene-global conditioning the render-stage stage needs.

WHY A SMALL NET
---------------
The correction is local and the inputs are a handful of vectors. Four layers
with 16 hidden units (~1.5k parameters) are enough to model the mask/velocity/
depth-conditioned residual; a bigger net would just memorise the dataset.
"""

from __future__ import annotations

import numpy as np

# ---------------------------------------------------------------------------
# fixed architecture constants (the ABI; the shader must match)
# ---------------------------------------------------------------------------

N_PIX = 3          # warped RGB (r, g, b)
N_MASK = 1         # reactive-mask (0 = clean, 1 = unreliable)
N_VEL = 2          # velocity (x, y), in screen coords
N_DEPTH = 1        # depth (z, NDC)
N_IN = N_PIX + N_MASK + N_VEL + N_DEPTH
N_HIDDEN = 16
N_RESIDUAL = 3
N_GATE = 1

# Layer order in the weight blob. The exporter and the shader must agree on this
# exactly; `export_weights.py` writes it and `test_model.py` round-trips it.
LAYER_SHAPES = [
    ("W1", (N_IN, N_HIDDEN)),
    ("b1", (N_HIDDEN,)),
    ("W2", (N_HIDDEN, N_HIDDEN)),
    ("b2", (N_HIDDEN,)),
    ("Wr", (N_HIDDEN, N_RESIDUAL)),
    ("br", (N_RESIDUAL,)),
    ("Wg", (N_HIDDEN, N_GATE)),
    ("bg", (N_GATE,)),
]

N_WEIGHTS = int(sum(int(np.prod(s)) for _, s in LAYER_SHAPES))

# Per-feature standardisation, measured on the training pairs (see
# `dataset_stats.json`). PART OF THE ABI: the shader must apply the same affine
# before the first matmul, or every weight is wrong. v1 uses the identity affine
# (mean 0, std 1) until the dataset is exported; it MUST be replaced with the
# measured values from `dataset_stats.json` before the model is frozen.
FEATURE_MEAN = np.zeros(N_IN, dtype=np.float64)
FEATURE_STD = np.ones(N_IN, dtype=np.float64)
assert FEATURE_MEAN.shape == (N_IN,) and FEATURE_STD.shape == (N_IN,)
assert np.all(FEATURE_STD > 0)

# A gate below this is, for practical purposes, "the network declined to act on
# this pixel". Chosen so that gate*residual < 0.05 * typical_residual.
GATE_FLOOR = 0.05

# Anchor penalty -- the "engine frame is unyielding" term as a number, priced by
# `lam` in the loss: trades enhancement against fidelity to the warped frame.
LAMBDA = 0.05

# Loss weight for the "engine frame is unyielding" term.
LAMBDA_ANCHOR = 0.05


def frame_global(anchor_rgb: np.ndarray) -> np.ndarray:
    """Not used -- the WLFG head is per-pixel, no scene conditioning."""
    return np.zeros(4, dtype=np.float64)


def features(warped_rgb: np.ndarray, mask: np.ndarray,
             velocity: np.ndarray, depth: np.ndarray) -> np.ndarray:
    """(N,3) warped RGB + (N,1) mask + (N,2) velocity + (N,1) depth
    -> (N,7) standardised model input.

    Velocity is signed screen-space displacement; the other groups are supplied
    in their documented capture domains. `velocity`/`depth` and `warped_rgb`/`mask` must be fetched
    at the same pixel coords by the caller (the engine does this in the shader;
    during training we pass them already sampled, e.g. from the captured
    buffers or, in tests, from mock arrays).
    """
    warped = np.asarray(warped_rgb, dtype=np.float32).reshape(-1, 3)
    m = np.asarray(mask, dtype=np.float32).reshape(-1, 1)
    v = np.asarray(velocity, dtype=np.float32).reshape(-1, 2)
    d = np.asarray(depth, dtype=np.float32).reshape(-1, 1)
    x = np.concatenate([warped, m, v, d], axis=1).astype(np.float64)
    return ((x - FEATURE_MEAN) / FEATURE_STD).astype(np.float32)


def init_params(rng: np.random.Generator, scale: float = 1.0) -> dict:
    """He initialisation, assuming standardised inputs. The residual head is
    zeroed separately so the model starts as the identity -- out == anchor
    exactly at step 0."""
    p = {}
    for name, shape in LAYER_SHAPES:
        if name.startswith("W"):
            fan_in = shape[0]
            p[name] = rng.normal(0.0, scale * np.sqrt(2.0 / fan_in), shape).astype(np.float32)
        else:
            p[name] = np.zeros(shape, dtype=np.float32)
    # Initialise the residual head at zero so the model is the identity at step 0.
    p["Wr"] = np.zeros((N_HIDDEN, N_RESIDUAL), dtype=np.float32)
    p["br"] = np.zeros((N_RESIDUAL,), dtype=np.float32)
    return p


def forward(p: dict, pixel: np.ndarray, anchor_rgb: np.ndarray):
    """Returns (out, cache). `pixel` is the STANDARDISED (N,7) feature vector."""
    z1 = pixel @ p["W1"] + p["b1"]
    h1 = np.maximum(z1, 0.0)
    z2 = h1 @ p["W2"] + p["b2"]
    h2 = np.maximum(z2, 0.0)

    residual = h2 @ p["Wr"] + p["br"]
    gate = 1.0 / (1.0 + np.exp(-(h2 @ p["Wg"] + p["bg"])))

    out = anchor_rgb + gate * residual
    cache = (pixel, anchor_rgb, z1, h1, z2, h2, residual, gate)
    return out, cache


# ---------------------------------------------------------------------------
# Adam + cosine lr schedule (written out, no framework)
# ---------------------------------------------------------------------------

class Adam:
    """Adam, lr 3e-3 with cosine decay is the WLFG training config."""

    def __init__(self, params: dict, lr: float = 3e-3, b1: float = 0.9,
                 b2: float = 0.999, eps: float = 1e-8):
        self.lr, self.b1, self.b2, self.eps = lr, b1, b2, eps
        self.m = {k: np.zeros_like(v) for k, v in params.items()}
        self.v = {k: np.zeros_like(v) for k, v in params.items()}
        self.t = 0

    def step(self, params: dict, grads: dict) -> None:
        self.t += 1
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


def loss_and_grads(p: dict, pixel: np.ndarray, target: np.ndarray, lam: float = 0.05,
                   anchor_rgb: np.ndarray | None = None):
    """L = mean((out - target)^2) + lam * mean((gate*residual)^2.

    `lam` trades enhancement against fidelity to the anchor (the warped frame).
    `anchor_rgb` defaults to `pixel[:, :3]` only for the unstandardised v0
    callers; every current caller passes it explicitly.
    """
    if anchor_rgb is None:
        anchor_rgb = pixel[:, :3]
    # np.mean() over an (N,3) array divides by 3N, not N. Getting this wrong
    # scales the whole gradient by exactly N_RESIDUAL -- caught by the
    # finite-difference check in test_model.py.
    m = pixel.shape[0] * N_RESIDUAL
    out, (xc, anc, z1, h1, z2, h2, residual, gate) = forward(p, pixel, anchor_rgb)
    diff = out - target
    mse = float(np.mean(diff * diff))
    gr = gate * residual
    anchor_pen = float(np.mean(gr * gr))
    loss = mse + lam * anchor_pen

    dout = (2.0 / m) * diff
    dres = dout * gate + (2.0 * lam / m) * (gate * gate) * residual
    dgate = (np.sum(dout * residual, axis=1, keepdims=True)
             + (2.0 * lam / m) * np.sum(gate * (residual * residual), axis=1, keepdims=True))
    dzg = dgate * gate * (1.0 - gate)

    grads = {}
    grads["Wr"] = h2.T @ dres
    grads["br"] = dres.sum(axis=0)
    grads["Wg"] = h2.T @ dzg
    grads["bg"] = dzg.sum(axis=0)

    dh2 = dres @ p["Wr"].T + dzg @ p["Wg"].T
    dz2 = dh2 * (z2 > 0)
    grads["W2"] = h1.T @ dz2
    grads["b2"] = dz2.sum(axis=0)
    dh1 = dz2 @ p["W2"].T
    dz1 = dh1 * (z1 > 0)
    grads["W1"] = xc.T @ dz1
    grads["b1"] = dz1.sum(axis=0)

    return loss, mse, anchor_pen, grads


# ---------------------------------------------------------------------------
# predict: per-pixel forward using the same seven features as training.
# ---------------------------------------------------------------------------

def predict(p: dict, warped: np.ndarray, mask: np.ndarray,
            velocity: np.ndarray, depth: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Per-pixel forward: out = anchor + gate * residual.

    Args:
        p: model parameters.
        warped: (N,3) warped RGB.
        mask:  (N,1) reactive mask.
        velocity: (N,2) signed velocity [vx, vy].
        depth: (N,1) NDC depth.

    Returns:
        out: (N,3) enhanced RGB.
        gate: (N,) gate, 1 = apply full enhancement, 0 = show the real frame.
    """
    feats = features(warped, mask, velocity, depth)
    out, cache = forward(p, feats, warped)
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
