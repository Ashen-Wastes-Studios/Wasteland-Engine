"""Dataset access for the clean-room WLFG neural frame synthesis head.

numpy + Pillow only. The dataset is the task-1 export: matched (warped frame,
reactive mask, target) triplets of full-res RGBA PNGs under `dataset_wlfg/`,
described by `dataset_wlfg/manifest.json`.

MEMORY
------
Each pair is a set of full-res captures; a small subset is streamed at a time so
the whole set never sits in RAM together.

WHY RANDOM CROPS AND NOT WHOLE FRAMES
--------------------------------------
A few full frames is many M pixels -- far more than a 604-ish parameter model
can use, and a whole-frame batch would be gigabytes of activations per step.
Random patches are also the honest choice for a per-pixel network: the model has
no spatial extent, so a patch is a bag of independent samples.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

from data_contract import require_training_manifest

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATASET = REPO_ROOT / "dataset_wlfg"

# Every capture in the manifest is a 3.39 M-pixel RGBA (float32) image.
R, G, B, A = 0, 1, 2, 3


@dataclass(frozen=True)
class Pair:
    key: str                 # "Scene__camera__factor"
    factor: int
    scene: str
    camera: str
    warped_path: Path        # warped frame (rgba32f -> RGBA PNG)
    mask_path: Path          # reactive mask (rgba32f)
    target_path: Path        # converged ground-truth frame (rgba32f)
    velocity_path: Path      # lossless float32 HxWx2, X then Y
    depth_path: Path         # lossless float32 HxW[x1] NDC depth
    meta: dict


def load_manifest(dataset_dir: Path | str = DEFAULT_DATASET) -> dict:
    p = Path(dataset_dir) / "manifest.json"
    if not p.exists():
        raise FileNotFoundError(
            f"no manifest at {p} -- run `python tools/train_wlfg/export_dataset.py` first")
    raw = p.read_text(encoding="utf-8").strip()
    if not raw:
        raise ValueError(f"manifest {p} is empty -- export produced no frames")
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"manifest {p} is not valid JSON: {exc}") from exc


def load_pairs(dataset_dir: Path | str = DEFAULT_DATASET) -> list[Pair]:
    d = Path(dataset_dir)
    man = load_manifest(d)
    require_training_manifest(man)
    grouped: dict[tuple[str, str, int], dict] = {}
    for f in man["frames"]:
        key = (f["scene"], f["camera"]["name"], f["factor"])
        halves = grouped.setdefault(key, {})
        if f["pass_name"] in halves:
            raise ValueError(f"duplicate {f['pass_name']} for {key}")
        halves[f["pass_name"]] = f
    pairs, incomplete = [], []
    for (scene, camera, factor), halves in sorted(grouped.items()):
        required = {"target", "warped", "mask", "velocity", "depth"}
        if not required.issubset(halves):
            incomplete.append((scene, camera, factor, sorted(halves)))
            continue
        if any(halves[h].get("pending") for h in required):
            incomplete.append((scene, camera, factor, "pending required input"))
            continue
        def image_path(name: str) -> Path:
            rel = halves[name].get("path")
            if not isinstance(rel, str) or not rel:
                raise ValueError(f"missing {name} path for {(scene, camera, factor)}")
            path = (d / rel).resolve()
            try:
                path.relative_to(d.resolve())
            except ValueError as exc:
                raise ValueError(f"{name} path escapes dataset: {rel}") from exc
            if not path.is_file():
                raise ValueError(f"missing {name} file: {path}")
            return path

        pairs.append(Pair(
            key=f"{Path(scene).stem}__{camera}__{factor}",
            scene=scene, camera=camera, factor=factor,
            warped_path=image_path("warped"), mask_path=image_path("mask"),
            target_path=image_path("target"), velocity_path=image_path("velocity"),
            depth_path=image_path("depth"), meta={name: halves[name] for name in required},
        ))
    if incomplete:
        raise ValueError(f"{len(incomplete)} incomplete pairs: {incomplete[:3]}")
    if not pairs:
        raise ValueError("WLFG training dataset has no complete pairs")
    return pairs


def split_pairs(pairs, holdout: int = 6, seed: int = 20261023):
    """Deterministic train/holdout split by PAIR, holding out a whole factor.

    `holdout` is the FACTOR to hold out (default 6): train on {1,2,3,4},
    evaluate on 6. Splitting by patch would leak: adjacent patches of one
    frame are nearly identical, so the holdout would be a second copy of the
    training set and the reported PSNR would be fiction.
    """
    hold = [p for p in pairs if p.factor == holdout]
    train = [p for p in pairs if p.factor != holdout]
    return train, hold


def read_rgba01(path: Path) -> np.ndarray:
    """PNG -> float32 HxWx4 in 0..1 (RGBA)."""
    with Image.open(path) as im:
        return np.asarray(im.convert("RGBA"), dtype=np.float32) * (1.0 / 255.0)


def read_gray01(path: Path) -> np.ndarray:
    """PNG -> float32 HxWx3 in 0..1 (grayscale)."""
    with Image.open(path) as im:
        return np.asarray(im.convert("L"), dtype=np.float32) * (1.0 / 255.0)


def pair_arrays(pair: Pair) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """(warped RGB (H,W,3), mask (H,W,1), target (H,W,3)) float32 in 0..1 (spatial,
    un-flattened). Only the warped RGBA, mask and target PNGs are held at once;
    the per-pixel flattening happens in `features` right before the model forward,
    so a single `features` path serves both the training loop and the render stage."""
    w = read_rgba01(pair.warped_path)
    m = read_rgba01(pair.mask_path)
    t = read_rgba01(pair.target_path)
    if m.shape[:2] != w.shape[:2] or t.shape[:2] != w.shape[:2]:
        raise ValueError(f"unaligned RGB/mask dimensions for {pair.key}")
    return (w[:, :, :3],
            m[:, :, :1],
            t[:, :, :3])


def _float_state(path: Path, shape: tuple[int, int], channels: int) -> np.ndarray:
    """Load lossless, aligned float32 state; never resize or decode an 8-bit PNG."""
    if path.suffix.lower() != ".npy":
        raise ValueError(f"float32 .npy state required, not quantized image: {path}")
    arr = np.load(path, allow_pickle=False)
    if arr.dtype.kind != "f" or arr.dtype.itemsize != 4:
        raise ValueError(f"float32 state required: {path}")
    if channels == 1 and arr.shape == shape:
        arr = arr[..., None]
    if arr.shape != (*shape, channels):
        raise ValueError(f"unaligned state shape {arr.shape} for {path}; expected {(*shape, channels)}")
    if not np.isfinite(arr).all():
        raise ValueError(f"non-finite state values: {path}")
    return np.asarray(arr, dtype=np.float32)


def pair_feature_arrays(pair: Pair):
    """Return aligned warped RGB, mask, target RGB, signed XY velocity and depth."""
    a, m, t = pair_arrays(pair)
    shape = a.shape[:2]
    return (a, m, t, _float_state(pair.velocity_path, shape, 2),
            _float_state(pair.depth_path, shape, 1))


def iter_random_crops(pairs, n_patches, patch, rng, repeat=1):
    """Yield (anchor, target, mask, XY velocity, depth) float32 arrays.

    `anchor` is the engine's warped frame (what pass 22 reads from `s_WlfgHistory`
    / `img_Accumulation`), `target` the converged frame, `mask` the reactive
    mask, `velocity`/`depth` the warp's reprojection state sampled at the batch's
    per-pixel coords. The model's inputs are the combined feature vector: RGB +
    mask + velocity + depth. Satellite coords are also yielded so the caller can
    look up the engine's velocity/depth tensors at the same coords during
    evaluation; for a pure-numpy training pass they equal the coords.
    """
    if not pairs:
        raise ValueError("no pairs")
    for _ in range(repeat):
        order = rng.permutation(len(pairs))
        for idx in order:
            pair = pairs[int(idx)]
            a, m, t, v, d = pair_feature_arrays(pair)
            h, w = a.shape[:2]
            if h < patch or w < patch:
                continue
            ys = rng.integers(0, h - patch + 1, size=n_patches)
            xs = rng.integers(0, w - patch + 1, size=n_patches)
            oy = ys[:, None, None] + np.arange(patch)[None, :, None]
            ox = xs[:, None, None] + np.arange(patch)[None, None, :]
            yield (a[oy, ox].reshape(-1, 3),
                   t[oy, ox].reshape(-1, 3),
                   m[oy, ox].reshape(-1, 1),
                   v[oy, ox].reshape(-1, 2),
                   d[oy, ox].reshape(-1, 1))
