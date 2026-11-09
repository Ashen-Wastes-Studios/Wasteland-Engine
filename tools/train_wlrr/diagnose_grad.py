"""Diagnose the W1 gradient discrepancy: is the analytic gradient wrong, or is the
finite difference losing precision? Sweeps eps and reports the U-shape."""
import sys
import numpy as np

sys.path.insert(0, r"C:\dev\Wasteland\tools\train_wlrr")
import model as M

rng = np.random.default_rng(9)
p32 = M.init_params(rng)
for name, shape in M.LAYER_SHAPES:
    p32[name] = rng.normal(0.0, 0.3, shape).astype(np.float32)

sys.path.insert(0, r"C:\dev\Wasteland\tools\train_wlrr")
from test_model import make_batch

inc, raw, target = make_batch(48, seed=10)

p = {k: v.astype(np.float64) for k, v in p32.items()}
feats = M.features(inc, raw).astype(np.float64)
inc64 = inc.astype(np.float64)
tgt64 = target.astype(np.float64)

_, _, _, grads = M.loss_and_grads(p, feats, tgt64, lam=M.LAMBDA_ANCHOR,
                                  anchor_rgb=inc64)
ana = float(grads["W1"].ravel()[72])
print(f"analytic W1[72] = {ana:.6e}")
print(f"loss scale: {M.loss_and_grads(p, feats, tgt64, lam=M.LAMBDA_ANCHOR, anchor_rgb=inc64)[0]:.1f}")
print(f"gate range: {float(grads['wg2'].min()):.3e} .. {float(grads['wg2'].max()):.3e}")

flat = p["W1"].ravel()
print(f"\n{'eps':>10s} {'numeric':>16s} {'|diff|':>12s} {'rel':>10s}")
for eps in (1e-2, 3e-3, 1e-3, 3e-4, 1e-4, 3e-5, 1e-5, 1e-6, 1e-7, 1e-8):
    orig = float(flat[72])
    flat[72] = orig + eps
    lp = M.loss_and_grads(p, feats, tgt64, lam=M.LAMBDA_ANCHOR, anchor_rgb=inc64)[0]
    flat[72] = orig - eps
    lm = M.loss_and_grads(p, feats, tgt64, lam=M.LAMBDA_ANCHOR, anchor_rgb=inc64)[0]
    flat[72] = orig
    num = (lp - lm) / (2.0 * eps)
    print(f"{eps:10.0e} {num:16.6e} {abs(num-ana):12.4e} {abs(num-ana)/abs(ana):10.2e}")
