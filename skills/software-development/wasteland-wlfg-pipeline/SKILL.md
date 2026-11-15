---
name: wasteland-wlfg-pipeline
description: "Use when building the WLFG neural frame-synthesis head dataset export, training pipeline, or test harness. Covers the stdlib-only dataset exporter, the Pair/load_pairs schema, the numpy head, Adam training with a stop-gate, and the fp32/CRC32 weight blob."
category: software-development
tags: [wasteland, wlfg, neural, dataset, training, reproducibility, numpy, blob, clean-room]
related_skills: [ml-artifact-reproducibility, wasteland-rendering-features, test-driven-development]
---

# WLFG Dataset & Training Pipeline

Dataset and training tooling for the WLFG neural frame-synthesis head (pass 23). The pipeline is
stdlib + numpy only: no torch, ONNX, or vendor ML framework. Weights export as a packed fp32
blob with a CRC32 guard.

## When to Use

- Re-running the dataset export (rebuild frames).
- Retraining the head (new epochs, lr, holdout, seed).
- Adding or fixing the test harness (gradient check, identity-at-init, sensitivity analysis).
- Re-exporting the dataset after a shader change (a model is a statement about the renderer).

## Pipeline Overview

```
export_dataset.py  ->  engine capture (90 PNGs, 3 cameras x 5 factors)
dataset.py         ->  Pair schema, load_pairs, split_pairs, pair_arrays
model.py           ->  numpy head (7->16->16 residual, 16->3, 16->1 gate; out = anchor + gate*residual)
train.py           ->  Adam/cosine fit loop, quick_psnr, best-eval/save
test_model.py      ->  PSNR/SSIM/evaluate, verify_gradient, verify_blob_roundtrip, synthetic fixture
export_weights.py  ->  pack_weights + CRC32 blob (MAGIC b"WLFG", VERSION 1)
```

```
target/  <id>__target.png   converged present (u0 = img_Output)  full-res 2332x1451
warped/  <id>__warped.png   reprojected previous present (u1 = img_Accumulation)   trace-res  176x110
mask/    <id>__mask.png     reactive mask (alpha of trace G-buffer)                  trace-res  176x110
vel_x/   <id>__vel_x.png    velocity X                                                 trace-res  176x110
vel_y/   <id>__vel_y.png    velocity Y                                                 trace-res  176x110
depth/   <id>__depth.png     depth (NDC)                                              trace-res  176x110
```

## Dataset Export

The exporter drives `DemonCore-Editor.exe` as a headless subprocess. It is **stdlib only** (no numpy,
PIL, or torch).

```bash
# One-off capture: 3 cameras x 5 factors = 15 captures, each ~24s.
# 90 PNGs land in dataset_wlfg/{target,warped,mask,vel_x,vel_y,depth}/; manifest.json too.
python3 tools/train_wlfg/export_dataset.py --out dataset_wlfg --timeout 300 --attempts 1

# Dry run (prints the plan, no captures):
python3 tools/train_wlfg/export_dataset.py --out dataset_wlfg --dry-run

# Validate the dataset after a run:
python3 tools/train_wlfg/export_dataset.py --out dataset_wlfg --validate
```

### Environment (task-7-vetted set)

The exporter passes to the engine:

| Env var | Value | Purpose |
|---|---|---|
| `WASTELAND_PIN_CAMERA` | `0,0,0,9,-85,0` | deterministic camera pin |
| `WL_DRS_OFF` | `1` | dynamic resolution off (the readback is scale-dependent) |
| `WL_SCREENSHOT` | `<target path>` | where the full-res PNG lands |
| `WL_SCREENSHOT_FRAME` | `300` | frame index to capture (after settle) |
| `WL_SCREENSHOT_LINGER` | `6` | frames to keep the exe alive after the shot |

**Do not add** `WL_WLFG`, `WL_CAPTURE_NEURAL`, `WL_CAPTURE_ACCUM`, `WL_CAPTURE_SCENE`,
`WL_CAPTURE_PRESET`, `WL_CAPTURE_SCALE`. These change the captured frame content and break the
u0 readback (byte-identical screenshots match without them — the dataset pairs are the same source
scene under the same pin, only the sim rate differs).

### Frame schema

Each capture yields 6 PNGs:

| pass_name | dir | resolution | content |
|---|---|---|---|
| `target` | `target/` | 2332x1451 | converged present (img_Output, full-res) |
| `warped` | `warped/` | 176x110 | reprojected previous present (img_Accumulation, trace-res) |
| `mask` | `mask/` | 176x110 | reactive mask (alpha of trace G-buffer at trace-res) |
| `vel_x` | `vel_x/` | 176x110 | velocity X (trace-res) |
| `vel_y` | `vel_y/` | 176x110 | velocity Y (trace-res) |
| `depth` | `depth/` | 176x110 | NDC depth (trace-res) |

The mask, velocity and depth are **engine-per-pixel state** sampled at inference by the shader; they
are `pending` in the manifest until the readback hook is wired.

### Manifest

`dataset_wlfg/manifest.json` is a JSON object with a `frames` array. Each entry:

```json
{
  "scene": "Example3D",
  "camera": {"name": "cam_down"},
  "factor": 1,
  "pass_name": "target",
  "pending": false,
  "path": "target/Example3D__cam_low__1.png",
  "width": 2332,
  "height": 1451,
  "mean_luma": 0.5,
  "contrast_p99": 0.12,
  "sha256": "abc123..."
}
```

`load_pairs` in `dataset.py` groups by `(scene, camera, name, factor)` and requires every pair to
have `target`, `warped` (and `mask`) present and **not pending**.

### Capture pitfalls

- The engine does **not** self-terminate after a capture. `WL_SCREENSHOT_LINGER` keeps it alive for
  `WL_SCREENSHOT_FRAME + 6` frames so the exe can be reaped with `proc.wait(timeout=...)`.
- `WL_SCREENSHOT_FRAME` must be large enough for the present to converge (frame 300 is the verified
  value). A small frame reads an unconverged present.
- `WL_CAPTURE_NEURAL`/`WL_CAPTURE_ACCUM`/`WL_WLFG` **break the u0 readback** — do not set them.
- The pipeline runs on `bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe`. A stale
  exe silently means a stale capture. Always check the exe mtime after a build (see
  `wasteland-build-verification`).

## Dataset Schema

`tools/train_wlfg/dataset.py` is the contract between the exporter and the trainer.

- **`Pair`** — the per-sample record. Fields: `key`, `scene`, `camera`, `factor`, `warped_path`,
  `mask_path`, `target_path`, `vel_x_path`, `vel_y_path`, `depth_path`, `meta`.
- **`load_pairs(data_dir)`** — reads `manifest.json`, groups by `(scene, camera, name, factor)`,
  and raises `ValueError` if any required input is missing or pending. Returns a list of `Pair`.
- **`split_pairs(pairs, holdout=6, seed=20261023)`** — pair-level split; `holdout` is the factor to
  hold out. Train on `{1,2,3,4}`, evaluate on `{6}`. Splitting by patch would leak, so it is done
  at the pair level, not the patch level.
- **`pair_arrays(p, patch=16)`** — returns spatial `(H,W,3)` arrays for warped, mask, target. Velocity
  and depth are mocked as zeros (sampled at inference by the shader).
- **`iter_random_crops(pairs, n_patches, patch, rng)`** — yields `(a, m, t, vy, vx)` per crop.
- **`validate_dir(out_root)`** — validates the dataset directory after a capture run; returns a
  compact summary and an exit code (2 on missing dir).

## Model (numpy head)

`tools/train_wlfg/model.py`:

- **Features** — `out = anchor + gate * residual` (identity at init). Inputs per pixel: warped RGB
  (3), mask (1), velocity (2), depth (1) = 7 features.
- **Stage 1** — `7 -> 16 -> 16` (two hidden layers).
- **Residual** — `16 -> 3` (residual RGB).
- **Gate** — `16 -> 1` sigmoid.
- **Loss** — MSE + `lam * mean((gate * residual)**2)` (the anchor-penalty keeps the gate near 0
  when the head should not change the input).
- **Optimizer** — Adam (lr 3e-3, b1 0.9, b2 0.999, eps 1e-8) with cosine LR decay.
- **Packing** — `pack_weights` / `unpack_weights` (flat fp32, length-mismatch refused).

## Training

```bash
# 3-epoch smoke on a tiny synthetic fixture (no real data):
python3 tools/train_wlfg/test_model.py --smoke

# Full run on real data; holdout factor 6 for the stop-gate eval:
python3 tools/train_wlfg/train.py --dataset dataset_wlfg --out model_wlfg/out_weights.npz \
  --epochs 3 --lr 3e-3 --holdout 6
```

`train.py` CLI: `--dataset <manifest.json> --out <npz> --epochs N --lr F --holdout F --patch 16
--patches-per-pair 64 --lam 0.05 --seed 20261023 --log-every 10`.

`quick_psnr(params, pairs, n=20000, seed=7)` reports the PSNR of the enhanced output vs the
converged target on a random sample; each pair contributes its own warped-frame conditioning
vector. `evaluate(params, pairs, n=20000, seed=7)` reports PSNR/SSIM on a pair-level split, plus
the stop-gate metrics (mean gate, fraction of gates below `GATE_FLOOR=0.05`).

`test_model.py` conventions:

- **Exit non-zero on a failing stop-gate** — if `delta <= 0` the commit is a no-op and must not
  claim an enhancement.
- **Gradient verification** — `verify_gradient` runs finite-difference against the analytic
  `loss_and_grads` in float64, tolerance 0.25, for all 4 weight matrices.
- **Blob roundtrip** — `verify_blob_roundtrip` re-packs and re-unpacks the fp32 blob; must be
  bit-identical.
- **Identity at init** — a freshly exported blob is exactly the identity (PSNR of raw warped input
  == PSNR of enhanced output); confirms the head does not change anything at init (the residual and
  gate are zeroed).

## Retention

- The dataset and model are **regenerable** — do not store the 1.4 GB of PNGs in git.
- The manifest is the provenance record: per-sample `sha256`, producer identity, and the capture
  config. A model trained on a fresh capture after a shader change belongs to a different dataset.

## Verification

1. Export: `python3 tools/train_wlfg/export_dataset.py --out dataset_wlfg --timeout 300 --attempts 1`
   (expect 90 PNGs, manifest, exit 0)
2. Validate: `python3 tools/train_wlfg/export_dataset.py --out dataset_wlfg --validate`
   (expect `VALIDATE OK: 90 frames`)
3. Test harness: `python3 tools/train_wlfg/test_model.py --smoke` (expect exit 0, gradient check OK)
4. Train: `python3 tools/train_wlfg/train.py --dataset dataset_wlfg --out model_wlfg/out_weights.npz --epochs 3 --lr 3e-3 --holdout 6`
5. Export weights: `python3 tools/train_wlfg/export_weights.py --weights model_wlfg/out_weights.npz`

Run the shader parity verifier after any shader change that affects the capture
(`tools/verify_nova_shader_parity.py`); the dataset corresponds to the renderer that produced it.
