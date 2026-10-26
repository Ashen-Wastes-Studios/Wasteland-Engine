"""Dataset access for the clean-room neural enhancement network.

The dataset is the task-2 export: 36 (input, gt) pairs of 2332x1451 RGB PNGs
under `dataset/`, described by `dataset/manifest.json`. `input` is the engine
frame at RenderScale 0.5 with temporal accumulation; `gt` is the same frame at
RenderScale 1.0, converged. Both are the engine's own output -- no external
data, no network fetch.

MEMORY
------
A full-res pair is ~80 MB as float32, and the whole set is ~2.9 GB. Nothing
here holds more than one pair at a time: `iter_random_crops` loads a pair,
yields a batch of patches, and drops it. Training therefore streams.

WHY RANDOM CROPS AND NOT WHOLE FRAMES
-------------------------------------
36 full frames is 122 M pixels -- far more than a 604-parameter model can use,
and a whole-frame batch would be 40 MB of activations per step. Random patches
are also the honest choice for a per-pixel network: the model has no spatial
extent, so a patch is a bag of independent samples.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATASET = REPO_ROOT / "dataset"

IMAGE_SIZE = (2332, 1451)   # (width, height) -- every capture in the manifest


@dataclass(frozen=True)
class Pair:
    key: str                 # "Scene__camera"
    scene: str
    camera: str
    input_path: Path
    gt_path: Path
    meta: dict

    @property
    def id(self) -> str:
        return self.key


def load_manifest(dataset_dir: Path | str = DEFAULT_DATASET) -> dict:
    p = Path(dataset_dir) / "manifest.json"
    if not p.exists():
        raise FileNotFoundError(
            f"no manifest at {p} -- run `python tools/train_neural/export_dataset.py` first")
    return json.loads(p.read_text(encoding="utf-8"))


def load_pairs(dataset_dir: Path | str = DEFAULT_DATASET) -> list[Pair]:
    """Group the manifest into (input, gt) pairs keyed by scene + camera.

    The input half is the `noisy` pass when the dataset has one and `input`
    otherwise. This is not cosmetic: the converged `input` pass has ~0.08 dB of
    affine headroom against `gt`, so training on it teaches the network almost
    nothing, while the single-frame `noisy` pass has ~2.0 dB. A dataset exported
    with `--passes gt --passes noisy` has no `input` frames at all, and keying
    on `input` alone made every pair look incomplete.

    Raises if any pair is half-present: a network trained on an unmatched frame
    would be learning from a scene it never saw the answer for.
    """
    d = Path(dataset_dir)
    man = load_manifest(d)
    grouped: dict[tuple[str, str], dict] = {}
    for f in man["frames"]:
        key = (f["scene"], f["camera"]["name"])
        grouped.setdefault(key, {})[f["pass_name"]] = f

    def input_name(halves: dict) -> str | None:
        return "noisy" if "noisy" in halves else ("input" if "input" in halves else None)

    pairs, incomplete = [], []
    for (scene, camera), halves in sorted(grouped.items()):
        name = input_name(halves)
        if "gt" not in halves or name is None:
            incomplete.append((scene, camera, sorted(halves)))
            continue
        g, i = halves["gt"], halves[name]
        pairs.append(Pair(
            key=f"{Path(scene).stem}__{camera}",
            scene=scene, camera=camera,
            input_path=d / name / f"{i['id']}.png",
            gt_path=d / "gt" / f"{g['id']}.png",
            meta={"gt": g, "input": i, "input_pass": name},
        ))
    if incomplete:
        raise ValueError(f"{len(incomplete)} incomplete pairs: {incomplete[:3]}")
    return pairs


def read_rgb01(path: Path) -> np.ndarray:
    """PNG -> float32 HxWx3 in 0..1. RGB only; the captures are 8-bit RGB."""
    with Image.open(path) as im:
        a = np.asarray(im.convert("RGB"), dtype=np.float32)
    return a * (1.0 / 255.0)


def pair_arrays(pair: Pair) -> tuple[np.ndarray, np.ndarray]:
    """(input, gt) as float32 HxWx3 in 0..1. ~80 MB for a full-res pair."""
    return read_rgb01(pair.input_path), read_rgb01(pair.gt_path)


def pair_global(anchor_img: np.ndarray) -> np.ndarray:
    """The (4,) frame-level conditioning vector for an anchor frame.

    Imported lazily so the module stays usable without the model layer."""
    from model import frame_global
    return frame_global(anchor_img)


def iter_random_crops(pairs, n_patches: int, patch: int, rng: np.random.Generator,
                      repeat: int = 1):
    """Yield (anchor, target, global) float32 arrays in 0..1.

    `anchor` is the engine's own frame -- the same texture pass 18 reads from
    `s_Output` at inference, and the same array the residual is added to.
    `global` is the (4,) frame-mean conditioning vector, computed from the
    ANCHOR and never from the ground truth.
    """
    if not pairs:
        raise ValueError("no pairs")
    from model import frame_global
    for _ in range(repeat):
        order = rng.permutation(len(pairs))
        for idx in order:
            anchor_img, target_img = pair_arrays(pairs[int(idx)])
            g = frame_global(anchor_img)
            h, w = anchor_img.shape[:2]
            if h < patch or w < patch:
                continue
            ys = rng.integers(0, h - patch + 1, size=n_patches)
            xs = rng.integers(0, w - patch + 1, size=n_patches)
            # (n_patches, patch, patch, 3) -> (n_patches*patch*patch, 3)
            oy = ys[:, None, None] + np.arange(patch)[None, :, None]
            ox = xs[:, None, None] + np.arange(patch)[None, None, :]
            a = anchor_img[oy, ox].reshape(-1, 3)
            t = target_img[oy, ox].reshape(-1, 3)
            yield a.astype(np.float32), t.astype(np.float32), g


def split_pairs(pairs, holdout: int = 6, seed: int = 20261023):
    """Deterministic train/holdout split by PAIR, never by patch.

    Splitting by patch would leak: adjacent patches of one frame are nearly
    identical, so the holdout would be a second copy of the training set and the
    reported PSNR would be fiction.
    """
    rng = np.random.default_rng(seed)
    order = rng.permutation(len(pairs))
    n_hold = min(holdout, max(1, len(pairs) // 5))
    hold_idx = set(order[:n_hold].tolist())
    train = [p for i, p in enumerate(pairs) if i not in hold_idx]
    hold = [p for i, p in enumerate(pairs) if i in hold_idx]
    return train, hold
