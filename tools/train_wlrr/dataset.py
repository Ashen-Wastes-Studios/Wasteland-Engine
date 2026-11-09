"""Dataset access for the WLRR neural denoiser-corrector head.

numpy + Pillow only. The dataset is the Task 10 arm export: matched
(incumbent, input, target) triples under `<out>/wlrr_incumbent/`,
`<out>/wlrr_input/` and `<out>/gt/`, described by `<out>/manifest.json`.

FAIL-CLOSED, AND WHAT THAT MEANS HERE
-------------------------------------
`load_triples` calls `require_training_manifest()` before it returns a single
frame. That check reads the DECLARED provenance in the manifest's `config` block
and refuses the dataset outright if it is missing or does not describe a verified
denoiser-correction set. A warning would be ignored, and the failure mode this
guards against -- training on the wrong kind of data -- does not announce itself
in the loss curve.

The declared block is necessary but not sufficient, so the loader ALSO verifies
structurally, from the frame records themselves, the two things it can check
without trusting anyone:

  1. Every triple has all three arms, with no duplicates.
  2. Every arm of a triple has IDENTICAL width and height.

(2) is the one that matters and the one a declaration cannot cover: the head
reads the incumbent and the input at the SAME pixel coords, so if the arms differ
in size the pair is meaningless and every weight it trains is wrong. The arms are
captured at different render scales (0.5 for the input and incumbent, 1.0 for the
target) but are upsampled to the same window size, so identical dimensions is the
correct expectation -- and it is measured here rather than assumed.

MEMORY
------
Each triple is three full-res frames (2332x1451). A few random patches are
streamed at a time so the whole set never sits in RAM together.

WHY RANDOM CROPS AND NOT WHOLE FRAMES
--------------------------------------
A few full frames is many M pixels -- far more than a 452-parameter model can use,
and a whole-frame batch would be gigabytes of activations per step. Random patches
are also the honest choice for a per-pixel network: the model has no spatial
extent, so a patch is a bag of independent samples.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

from data_contract import require_training_manifest

# The feature width is part of the model ABI. `compute_feature_stats` must
# produce exactly the columns `model.features` builds, in the same order, so the
# import is load-bearing rather than cosmetic: if the ABI grows a feature group,
# the statistics follow automatically instead of silently staying at the old width.
import model as _M

N_IN = _M.N_IN
N_DIFF = _M.N_DIFF

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATASET = REPO_ROOT / "dataset_wlrr"

# The three arms, and which role each plays. `wlrr_incumbent` is the ANCHOR the
# residual is added to; `wlrr_input` is what the incumbent filtered.
ARM_INPUT = "wlrr_input"
ARM_INCUMBENT = "wlrr_incumbent"
# `wlrr_gt`, NOT `gt`. Pass 21 runs on img_Accumulation (linear, pre-composite),
# so its target must be the converged ACCUMULATION. The shared `gt` arm is
# img_Output -- the tonemapped present -- and is a different colour domain:
# measured within-scene std ~1.9 (composite) vs ~15.1 (accumulation). Training
# on `gt` produced a head whose runtime correction was ~60x its trained one.
#
# This is fail-closed on purpose: an older dataset has no `wlrr_gt` directory,
# so it is refused with the fix named rather than silently trained on the wrong
# domain. Re-export with `--passes wlrr_input --passes wlrr_incumbent
# --passes wlrr_gt`.
ARM_TARGET = "wlrr_gt"
REQUIRED_ARMS = (ARM_INPUT, ARM_INCUMBENT, ARM_TARGET)


@dataclass(frozen=True)
class Triple:
    key: str                  # "Scene__camera"
    scene: str
    camera: str
    input_path: Path
    incumbent_path: Path
    target_path: Path
    meta: dict


def load_manifest(dataset_dir: Path | str = DEFAULT_DATASET) -> dict:
    p = Path(dataset_dir) / "manifest.json"
    if not p.exists():
        raise FileNotFoundError(
            f"no manifest at {p} -- run `python tools/train_neural/export_dataset.py "
            f"--passes wlrr_input --passes wlrr_incumbent --passes wlrr_gt` first")
    raw = p.read_text(encoding="utf-8").strip()
    if not raw:
        raise ValueError(f"manifest {p} is empty -- export produced no frames")
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"manifest {p} is not valid JSON: {exc}") from exc


def load_triples(dataset_dir: Path | str = DEFAULT_DATASET,
                 allow_incomplete: bool = False) -> list[Triple]:
    """Return the complete (input, incumbent, target) triples, or raise.

    Strict by default: a dataset that is partly unusable is a dataset whose
    contents nobody has checked, and silently training on the remainder is how a
    missing arm becomes an unexplained model.

    `allow_incomplete=True` is the escape hatch for a large export where one scene
    fails for a reason unrelated to the data (a blank frame, a scene that will not
    frame). It is NOT silent -- every skipped triple is named on stderr and the
    caller is expected to report the count. Making the skip loud is the whole point:
    the failure mode this guards against is a training run quietly using 9 of 12
    scenes and reporting a number as if it used all of them.
    """
    d = Path(dataset_dir)
    man = load_manifest(d)
    require_training_manifest(man)

    # A recorded capture failure contributes no frame, so it leaves no trace in the
    # grouping below -- the dataset loads "clean" with 9 captures missing. Report it
    # every time: a run that quietly used 11 of 12 scenes must not be described as
    # having used the export.
    failed = man.get("failures") or []
    if failed:
        by_scene = {}
        for f in failed:
            by_scene.setdefault(str(f.get("id", "?")).split("__")[0], 0)
            by_scene[str(f.get("id", "?")).split("__")[0]] += 1
        print(f"WARNING: the export recorded {len(failed)} failed capture(s): "
              + ", ".join(f"{s} x{n}" for s, n in sorted(by_scene.items()))
              + " -- those arms are absent from this dataset.", file=sys.stderr)

    grouped: dict[tuple[str, str], dict] = {}
    for f in man.get("frames", []):
        camera = f.get("camera", {})
        key = (f.get("scene"), camera.get("name"))
        arms = grouped.setdefault(key, {})
        name = f.get("pass_name")
        if name in arms:
            raise ValueError(f"duplicate {name} for {key}")
        arms[name] = f

    triples, problems = [], []
    for (scene, camera), arms in sorted(grouped.items(), key=lambda kv: str(kv[0])):
        missing = [a for a in REQUIRED_ARMS if a not in arms]
        if missing:
            problems.append(f"{scene} / {camera}: missing {', '.join(missing)}")
            continue
        if any(arms[a].get("pending") for a in REQUIRED_ARMS):
            problems.append(f"{scene} / {camera}: an arm is still pending")
            continue

        # (2) identical dimensions. This is the check a declaration cannot make.
        sizes = {(arms[a].get("width"), arms[a].get("height")) for a in REQUIRED_ARMS}
        only = next(iter(sizes))
        if len(sizes) != 1 or None in only:
            problems.append(
                f"{scene} / {camera}: arms differ in size "
                + ", ".join(f"{a}={arms[a].get('width')}x{arms[a].get('height')}"
                            for a in REQUIRED_ARMS))
            continue

        def arm_path(arm: str) -> Path:
            rel = arms[arm].get("path")
            if not isinstance(rel, str) or not rel:
                raise ValueError(f"missing {arm} path for {scene} / {camera}")
            path = (d / rel).resolve()
            try:
                path.relative_to(d.resolve())
            except ValueError as exc:
                raise ValueError(f"{arm} path escapes the dataset: {rel}") from exc
            if not path.is_file():
                raise ValueError(f"missing {arm} file: {path}")
            return path

        triples.append(Triple(
            key=f"{Path(scene).stem}__{camera}",
            scene=scene, camera=camera,
            input_path=arm_path(ARM_INPUT),
            incumbent_path=arm_path(ARM_INCUMBENT),
            target_path=arm_path(ARM_TARGET),
            meta={a: arms[a] for a in REQUIRED_ARMS},
        ))

    if problems:
        if not allow_incomplete:
            raise ValueError(f"{len(problems)} unusable triple(s): " + "; ".join(problems[:3]))
        for p in problems:
            print(f"WARNING: skipping {p}", file=sys.stderr)
        print(f"WARNING: {len(problems)} of {len(problems) + len(triples)} triples skipped "
              f"-- report this alongside any result computed from this dataset.",
              file=sys.stderr)
    if not triples:
        raise ValueError("WLRR dataset has no complete (input, incumbent, target) triples")
    return triples


def read_rgb(path: Path) -> np.ndarray:
    """PNG -> float32 HxWx3 in 0..255.

    NOT 0..1. The model's `features` standardises with `FEATURE_MEAN`/`FEATURE_STD`
    and `test_model.py` builds its batches in 0..255, so a loader that normalised
    to 0..1 would silently train a different function from the one the tests
    verify. The shader reads the same 0..255 domain from its image buffers.
    """
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.float32)


def triple_arrays(t: Triple) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """(incumbent, input, target) as HxWx3 float32 in 0..255, spatially aligned."""
    inc = read_rgb(t.incumbent_path)
    inp = read_rgb(t.input_path)
    tgt = read_rgb(t.target_path)
    if not (inc.shape == inp.shape == tgt.shape):
        raise ValueError(f"unaligned arms for {t.key}: {inc.shape}, {inp.shape}, {tgt.shape}")
    return inc, inp, tgt


def compute_feature_stats(triples, samples: int = 400_000, seed: int = 20261105,
                          per_triple: int = 20_000) -> tuple[np.ndarray, np.ndarray]:
    """Per-feature mean/std of the model's N_IN inputs, in the 0..255 domain.

    Returns (mean, std) each shaped (N_IN + N_DIFF,), in `model.features` order:
    incumbent RGB, input RGB, then the per-channel SIGNED difference
    (incumbent - input) -- see model.py for why the difference is a feature.

    WHY THIS EXISTS
    ---------------
    `FEATURE_MEAN`/`FEATURE_STD` ship as the identity, and `init_params` sizes the
    weights for STANDARDISED inputs. Until these are the measured values, the
    pre-activations run to ~1e3, sigmoid saturates and the gate is exactly 0 --
    measured, and the root cause of a test that failed for the wrong reason. The
    WLFG head needed the same step; this is that step, for this head.

    The difference column's mean is near zero and its std is the arm separation,
    so it standardises to a well-spread feature -- which is what makes it a
    useful regime signal rather than a near-constant. It is part of the GATE's
    input only; the residual trunk slices it off (see `model.forward`).

    Sampled rather than exhaustive: the frames are 3.4 M pixels each and the
    statistics converge long before the whole set has been read.
    """
    if not triples:
        raise ValueError("no triples to measure")
    rng = np.random.default_rng(seed)
    n_feat = N_IN + N_DIFF
    acc_n = 0
    acc_sum = np.zeros(n_feat, dtype=np.float64)
    acc_sq = np.zeros(n_feat, dtype=np.float64)

    for t in triples:
        if acc_n >= samples:
            break
        inc, inp, _ = triple_arrays(t)
        h, w = inc.shape[:2]
        take = min(per_triple, samples - acc_n)
        ys = rng.integers(0, h, size=take)
        xs = rng.integers(0, w, size=take)
        i = inc[ys, xs]
        j = inp[ys, xs]
        block = np.concatenate([i, j, i - j], axis=1).astype(np.float64)
        acc_sum += block.sum(axis=0)
        acc_sq += (block * block).sum(axis=0)
        acc_n += take

    if acc_n == 0:
        raise ValueError("feature statistics sampled no pixels")
    mean = acc_sum / acc_n
    var = np.maximum(acc_sq / acc_n - mean * mean, 0.0)
    std = np.sqrt(var)
    # A feature that is constant across the sample would divide by zero in
    # `features`. Floor the std rather than raising: a constant feature carries no
    # information, and standardising it to 0 is the honest representation of that.
    std = np.where(std < 1e-6, 1.0, std)
    return mean.astype(np.float32), std.astype(np.float32)


def compute_blend_alpha(triples, samples: int = 400_000, seed: int = 20261105,
                        per_triple: int = 20_000) -> np.ndarray:
    """Per-channel least-squares alpha in `gt ~= inc + alpha*(inp - inc)`.

    MEASURED, NOT DESIGNED: a single global alpha beats BOTH arms on 11/11 scenes
    (`diagnose_oracle.py`), so the blend is a near-optimal anchor and the head
    should learn only the per-pixel deviation from it.

    Measured on the TRAIN SPLIT ONLY -- the same rule as the feature affine. An
    alpha fitted on the holdout would put holdout pixels into the anchor the
    model is measured against.

    Returns (3,) in 0..255 units, one weight per channel.
    """
    if not triples:
        raise ValueError("no triples to measure")
    rng = np.random.default_rng(seed)
    acc_n = 0
    num = np.zeros(3, dtype=np.float64)
    den = np.zeros(3, dtype=np.float64)
    for t in triples:
        if acc_n >= samples:
            break
        inc, inp, gt = triple_arrays(t)
        h, w = inc.shape[:2]
        take = min(per_triple, samples - acc_n)
        ys = rng.integers(0, h, size=take)
        xs = rng.integers(0, w, size=take)
        d = (inp[ys, xs].astype(np.float64) - inc[ys, xs].astype(np.float64))
        e = (gt[ys, xs].astype(np.float64) - inc[ys, xs].astype(np.float64))
        num += np.sum(d * e, axis=0)      # per-channel: sum_n d[n,c]*e[n,c]
        den += np.sum(d * d, axis=0)
        acc_n += take
    alpha = np.where(den > 0.0, num / np.maximum(den, 1e-12), 0.0)
    return np.clip(alpha, 0.0, 2.0)


def apply_blend_alpha(alpha) -> None:
    """Install the anchor blend weight on the model module (and validate it)."""
    import model as M
    a = np.asarray(alpha, dtype=np.float64).reshape(-1)
    if a.shape != (M.N_INCUMBENT,):
        raise ValueError(f"expected ({M.N_INCUMBENT},) alpha, got {a.shape}")
    if not np.all(np.isfinite(a)):
        raise ValueError("blend alpha must be finite")
    M.BLEND_ALPHA = a


def apply_feature_stats(mean: np.ndarray, std: np.ndarray) -> None:
    """Install the measured affine into the model module.

    Mutates the module-level ABI constants, so it must be called BEFORE any
    `features`/`predict` call whose result is compared against the shader -- the
    shader applies the same affine, and a mismatch there is a silently wrong
    image, not an error.
    """
    import model as M
    mean = np.asarray(mean, dtype=np.float64).reshape(-1)
    std = np.asarray(std, dtype=np.float64).reshape(-1)
    if mean.shape != (M.N_FEAT,) or std.shape != (M.N_FEAT,):
        raise ValueError(f"expected ({M.N_FEAT},) mean and std, got {mean.shape} and {std.shape}")
    if not np.all(np.isfinite(mean)) or not np.all(np.isfinite(std)) or np.any(std <= 0):
        raise ValueError("feature statistics must be finite with strictly positive std")
    M.FEATURE_MEAN = mean
    M.FEATURE_STD = std


def split_triples(triples, holdout_camera: str | None = None, holdout_scene=None,
                  seed: int = 20261105, frac: float = 0.25):
    """Deterministic train/holdout split BY TRIPLE, never by patch.

    Splitting by patch would leak: adjacent patches of one frame are nearly
    identical, so the holdout would be a second copy of the training set and the
    reported PSNR would be fiction. Holding out a whole camera or scene is the
    strongest form, and is the default when one is named.
    """
    if holdout_camera is not None or holdout_scene is not None:
        hold = [t for t in triples
                if (holdout_camera is not None and t.camera == holdout_camera)
                or (holdout_scene is not None and t.scene == holdout_scene)]
        train = [t for t in triples if t not in hold]
    else:
        rng = np.random.default_rng(seed)
        order = rng.permutation(len(triples))
        n_hold = max(1, int(round(len(triples) * frac)))
        hold = [triples[int(i)] for i in order[:n_hold]]
        train = [triples[int(i)] for i in order[n_hold:]]
    if not train or not hold:
        raise ValueError(f"split produced {len(train)} train / {len(hold)} holdout triples")
    return train, hold


def iter_random_crops(triples, n_patches: int, patch: int, rng, repeat: int = 1):
    """Yield (incumbent, input, target) flattened float32 batches in 0..255.

    Each yielded triple is (N,3) incumbent RGB, (N,3) input RGB, (N,3) target RGB,
    where N = n_patches * patch * patch. The incumbent is the model's ANCHOR, so
    the caller must pass it as `anchor_rgb` -- not the input.
    """
    if not triples:
        raise ValueError("no triples")
    for _ in range(repeat):
        order = rng.permutation(len(triples))
        for idx in order:
            t = triples[int(idx)]
            inc, inp, tgt = triple_arrays(t)
            h, w = inc.shape[:2]
            if h < patch or w < patch:
                continue
            ys = rng.integers(0, h - patch + 1, size=n_patches)
            xs = rng.integers(0, w - patch + 1, size=n_patches)
            oy = ys[:, None, None] + np.arange(patch)[None, :, None]
            ox = xs[:, None, None] + np.arange(patch)[None, None, :]
            yield (inc[oy, ox].reshape(-1, 3),
                   inp[oy, ox].reshape(-1, 3),
                   tgt[oy, ox].reshape(-1, 3))
