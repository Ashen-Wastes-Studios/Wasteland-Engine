"""Wasteland clean-room neural enhancement network.

numpy only. No torch, no ONNX, no vendor maths library: the forward pass and
the gradient are written out here, which is the clean-room requirement made
concrete rather than a claim in a README.

ARCHITECTURE (v1)
-----------------
    pixel  = 4   (r, g, b, luma)        -- this pixel, from s_Output + img_NeuralFeatures
    global = 4   (mean_r, mean_g, mean_b, mean_luma)   -- the whole anchor frame
    in     = 8   pixel ++ global

    stage1: 8 -> 32 -> 8
    stage2: 8 -> 32 -> 8
    head  : 8 -> 3                     residual, added to the anchor
    gate  : 8 -> 1                     sigmoid, per pixel

    out = anchor + gate * residual

The output is a RESIDUAL, never a replacement. `anchor` is the engine frame and
`gate` in [0,1] is learned per pixel, so the worst case (network wrong) degrades
to "no enhancement" instead of "wrong image".

WHY 8 INPUTS: 4 PER-PIXEL + 4 GLOBAL
------------------------------------
The plan (`.hermes/plans/...` section 2.2) specifies 18 per-pixel features. Two
measurements rule that out, and a third says what to do instead.

1. **The binding budget.** The reservation gives the feature pass one `rgba32f`
   UAV -- four floats -- and `RunNeuralFeatures` stages
   `(luma, depth01, velocity.x, velocity.y)`
   (`Renderer3D_NovaRenderer.hlsl:3970`). Depth and velocity are NOT in the
   task-2 dataset: the exporter captures colour PNGs only. Training on constant
   depth/velocity would teach the network to ignore them, so they are staged but
   not consumed. Pass 18 reads `r,g,b` straight from `s_Output` -- the same
   texture pass 17 reads, so it costs no extra register.

2. **A luma-only model is measurably worse than doing nothing.**

   | model | PSNR vs gt, 36 pairs |
   |---|---|
   | raw input (do nothing) | 33.609 dB |
   | global per-channel affine (12 params) | 36.014 dB |
   | 64-bin luma tone curve (best possible 1-input luma model) | 27.833 dB |

   Luma is a lossy 1-D projection: unrelated colours that share a luma get
   mapped to the same output. That is a -5.8 dB regression, so the network takes
   RGB.

3. **A per-pixel model cannot be scene-dependent, and the correction is.**
   Fitting a per-channel affine *per scene* reaches 38.257 dB against 36.014 dB
   for one global affine -- the difference is +2.24 dB of scene conditioning.
   The coefficients are nothing alike: DemoGame's are gains ~[0.885, 0.867,
   0.897] (a contrast reduction), WaterShowcase's have *negative* gains and a
   0.224 blue bias. A per-pixel map has no way to know which scene it is
   looking at, so it converges to the global compromise.

   The global features are the frame's own mean colour and mean luma. They are
   computed from the ANCHOR (never the ground truth -- that would be leakage),
   reduced once per frame, and broadcast to every pixel. Still a per-pixel map,
   but one that knows what frame it is in.
"""

from __future__ import annotations

import numpy as np

# ---- fixed architecture constants (the ABI; the shader must match) ----
N_PIX = 4          # (r, g, b, luma) for this pixel
N_GLOB = 4         # (mean_r, mean_g, mean_b, mean_luma) for the whole anchor frame
N_IN = N_PIX + N_GLOB
N_HIDDEN = 32
N_RESIDUAL = 3
N_GATE = 1

# Layer order in the weight blob. The exporter and the shader must agree on
# this exactly; `export_weights.py` writes it and `test_model.py` round-trips it.
LAYER_SHAPES = [
    ("W1", (N_IN, N_HIDDEN)),
    ("b1", (N_HIDDEN,)),
    ("W2", (N_HIDDEN, N_IN)),
    ("b2", (N_IN,)),
    ("W3", (N_IN, N_HIDDEN)),
    ("b3", (N_HIDDEN,)),
    ("W4", (N_HIDDEN, N_IN)),
    ("b4", (N_IN,)),
    ("Wh", (N_IN, N_RESIDUAL)),
    ("bh", (N_RESIDUAL,)),
    ("Wg", (N_IN, N_GATE)),
    ("bg", (N_GATE,)),
]

N_WEIGHTS = int(sum(int(np.prod(s)) for _, s in LAYER_SHAPES))

LUMA_WEIGHTS = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)

# Per-feature standardisation, measured on the 30 training pairs (see
# `tools/train_neural/dataset_stats.json`). THIS IS PART OF THE ABI: the shader
# must apply the same affine before the first matmul, or every weight is wrong.
#
# It exists because the network silently collapsed without it. With raw inputs
# in [0,1] the feature means are ~0.15-0.20 and the stds ~0.06-0.12, so the
# layer-1 pre-activation had a std of 0.0118 -- 85x too small. Half the ReLU
# units died at initialisation, the signal attenuated to nothing through four
# layers, and training converged to `out = anchor + constant`: a single number,
# worth +2.4 dB, indistinguishable from a broken model. Measured: the output was
# byte-identical for global-vector values of 0.0, 0.2, 0.4 and 1.0, and stayed
# identical after perturbing W1 by +1.0.
FEATURE_MEAN = np.array([0.1744, 0.1700, 0.2243, 0.1749,
                         0.1742, 0.1699, 0.2244, 0.1748], dtype=np.float64)
FEATURE_STD = np.array([0.1331, 0.0835, 0.1145, 0.0863,
                        0.0964, 0.0688, 0.0969, 0.0687], dtype=np.float64)
assert FEATURE_MEAN.shape == (N_IN,) and FEATURE_STD.shape == (N_IN,)
assert np.all(FEATURE_STD > 0)


def luma(rgb: np.ndarray) -> np.ndarray:
    """Rec. 709 luma. Matches `dot(color, float3(0.2126, 0.7152, 0.0722))` in
    `RunNeuralFeatures` -- a flat average is wrong by up to 0.13 on saturated
    blue, which is larger than the residual this network is trying to learn."""
    return rgb[..., 0] * LUMA_WEIGHTS[0] + rgb[..., 1] * LUMA_WEIGHTS[1] + rgb[..., 2] * LUMA_WEIGHTS[2]


def frame_global(anchor_rgb: np.ndarray, step: int = 8) -> np.ndarray:
    """Whole-frame conditioning vector, (4,) = (mean_r, mean_g, mean_b, mean_luma).

    Computed from the ANCHOR frame -- the engine's own degraded output -- never
    from the ground truth. Subsampled because a mean over 3.39 M pixels and a mean
    over 52 k of them differ by far less than the correction this feeds.
    """
    a = np.asarray(anchor_rgb, dtype=np.float64)
    if a.ndim == 3:
        a = a[::step, ::step].reshape(-1, 3)
    m = a.mean(axis=0)
    return np.array([m[0], m[1], m[2], float(m @ LUMA_WEIGHTS)], dtype=np.float64)


def features(anchor_rgb: np.ndarray, global_rgb: np.ndarray | None = None) -> np.ndarray:
    """(N,3) anchor colour in 0..1 -> (N,8) STANDARDISED model input
    [r, g, b, luma, mean_r, mean_g, mean_b, mean_luma].

    `global_rgb` is (4,) or (N,4); None gives zeros, which is the unconditioned
    v0 model and is what the identity-at-init check uses.

    The output is standardised to zero mean / unit variance using FEATURE_MEAN and
    FEATURE_STD. Without that the ReLU stack dies -- see the note above.
    """
    rgb = np.asarray(anchor_rgb, dtype=np.float32)
    pix = np.concatenate([rgb, luma(rgb)[:, None]], axis=1)
    if global_rgb is None:
        glob = np.zeros((rgb.shape[0], N_GLOB), dtype=np.float32)
    else:
        g = np.asarray(global_rgb, dtype=np.float32)
        glob = np.broadcast_to(g, (rgb.shape[0], N_GLOB)) if g.ndim == 1 else g
        glob = np.ascontiguousarray(glob, dtype=np.float32)
    x = np.concatenate([pix, glob], axis=1).astype(np.float64)
    return ((x - FEATURE_MEAN) / FEATURE_STD).astype(np.float32)


def init_params(rng: np.random.Generator, scale: float = 1.0) -> dict:
    """He initialisation (`scale * sqrt(2/fan_in)`), which assumes standardised
    inputs -- see FEATURE_MEAN above. `scale` was 0.1 originally, which with raw
    [0,1] inputs left the layer-1 pre-activation with a std of 0.0118 and killed
    the network. The head is zeroed separately so the model starts as the
    identity regardless of scale."""
    p = {}
    for name, shape in LAYER_SHAPES:
        if name.startswith("W"):
            fan_in = shape[0]
            p[name] = rng.normal(0.0, scale * np.sqrt(2.0 / fan_in), shape).astype(np.float32)
        else:
            p[name] = np.zeros(shape, dtype=np.float32)
    # Start the residual head at zero: out == anchor exactly at step 0.
    p["Wh"] = np.zeros((N_IN, N_RESIDUAL), dtype=np.float32)
    p["bh"] = np.zeros((N_RESIDUAL,), dtype=np.float32)
    return p


def forward(p: dict, x: np.ndarray, anchor_rgb: np.ndarray):
    """Returns (out, cache).

    `x` is the STANDARDISED feature vector (N,8); `anchor_rgb` is the ORIGINAL
    (N,3) colour in 0..1 that the residual is added to. They are separate
    arguments on purpose: when `features()` started standardising, defaulting the
    anchor to `x[:, :3]` silently made the model add its residual to
    `(r - 0.1498) / 0.1205` instead of `r`, and the run diverged to 19 dB.
    """
    z1 = x @ p["W1"] + p["b1"]
    h1 = np.maximum(z1, 0.0)
    z2 = h1 @ p["W2"] + p["b2"]
    h2 = np.maximum(z2, 0.0)
    z3 = h2 @ p["W3"] + p["b3"]
    h3 = np.maximum(z3, 0.0)
    z4 = h3 @ p["W4"] + p["b4"]
    h4 = np.maximum(z4, 0.0)

    residual = h4 @ p["Wh"] + p["bh"]
    gate = 1.0 / (1.0 + np.exp(-(h4 @ p["Wg"] + p["bg"])))

    out = anchor_rgb + gate * residual
    cache = (x, anchor_rgb, z1, h1, z2, h2, z3, h3, z4, h4, residual, gate)
    return out, cache


def loss_and_grads(p: dict, x: np.ndarray, y: np.ndarray, lam: float,
                   anchor_rgb: np.ndarray | None = None):
    """L = mean((out - y)^2) + lam * mean((gate*residual)^2).

    The second term is the "engine frame is unyielding" term as a number: it
    prices deviation from the anchor, so `lam` trades enhancement against
    fidelity to the engine's own output.

    `anchor_rgb` defaults to `x[:, :3]` only for the unstandardised v0 callers;
    every current caller passes it explicitly.
    """
    if anchor_rgb is None:
        anchor_rgb = x[:, :3]
    # np.mean() over an (N,3) array divides by 3N, not N. Getting this wrong
    # scales the whole gradient by exactly N_RESIDUAL -- caught by the
    # finite-difference check in test_model.py, which reported a clean 3.000x.
    m = x.shape[0] * N_RESIDUAL
    out, (xc, anc, z1, h1, z2, h2, z3, h3, z4, h4, residual, gate) = forward(p, x, anchor_rgb)

    diff = out - y
    mse = float(np.mean(diff * diff))
    gr = gate * residual
    anchor_pen = float(np.mean(gr * gr))
    loss = mse + lam * anchor_pen

    # d(MSE)/d(out)
    dout = (2.0 / m) * diff
    # d(lam*mean((g*r)^2))/d(residual) = 2*lam*g^2*r/m
    dres = dout * gate + (2.0 * lam / m) * (gate * gate) * residual
    # d/d(gate): sum over channels of dout*residual, plus the anchor penalty's
    # 2*lam*g*r^2/m -- also summed over channels. gate is (N,1) and residual is
    # (N,3), so both terms must reduce over the channel axis or the broadcast
    # silently produces an (N,3) "gate gradient".
    dgate = (np.sum(dout * residual, axis=1, keepdims=True)
             + (2.0 * lam / m) * np.sum(gate * (residual * residual), axis=1, keepdims=True))

    g = gate
    dzg = dgate * g * (1.0 - g)

    grads = {}
    grads["Wh"] = h4.T @ dres
    grads["bh"] = dres.sum(axis=0)
    grads["Wg"] = h4.T @ dzg
    grads["bg"] = dzg.sum(axis=0)

    dh4 = dres @ p["Wh"].T + dzg @ p["Wg"].T
    dz4 = dh4 * (z4 > 0)
    grads["W4"] = h3.T @ dz4
    grads["b4"] = dz4.sum(axis=0)
    dh3 = dz4 @ p["W4"].T

    dz3 = dh3 * (z3 > 0)
    grads["W3"] = h2.T @ dz3
    grads["b3"] = dz3.sum(axis=0)
    dh2 = dz3 @ p["W3"].T

    dz2 = dh2 * (z2 > 0)
    grads["W2"] = h1.T @ dz2
    grads["b2"] = dz2.sum(axis=0)
    dh1 = dz2 @ p["W2"].T

    dz1 = dh1 * (z1 > 0)
    grads["W1"] = xc.T @ dz1
    grads["b1"] = dz1.sum(axis=0)

    return loss, mse, anchor_pen, grads


def pack(p: dict) -> np.ndarray:
    """Concatenate the parameters in LAYER_SHAPES order -> flat fp32 vector."""
    return np.concatenate([np.asarray(p[name], dtype=np.float32).ravel() for name, _ in LAYER_SHAPES])


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


class Adam:
    """Adam, written out. lr 3e-3 with cosine decay is the training config."""

    def __init__(self, params: dict, lr: float = 3e-3, b1: float = 0.9, b2: float = 0.999, eps: float = 1e-8):
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
