"""Shared fail-closed contract for WLRR training datasets.

The WLRR sibling of `tools/train_wlfg/data_contract.py`. The principle is the same
and is worth restating, because it is the whole reason this file exists: a dataset
that does not declare its provenance cannot be checked, and an unchecked dataset
trains a model that looks fine and renders wrong. `require_training_manifest`
therefore REFUSES a manifest rather than warning about it.

WHAT IS DIFFERENT FROM THE WLFG CONTRACT, AND WHY
-------------------------------------------------
The two datasets are not the same kind of thing, so the same values would be a
lie. The WLFG set is temporal frame pairs; the WLRR set is a denoiser-correction
triple (input, incumbent, gt) at a fixed camera. Each field below is the value
that is actually true for the WLRR arms.

`state_encoding` is the field to read carefully. The WLRR arms are **8-bit PNGs**,
NOT the lossless float32 `.npy` the WLFG set uses, and that is a real limitation
rather than a detail:

    measured arm separation (wlrr_input vs wlrr_incumbent), 3 cameras:
        mean|d| 0.98 - 1.24 / 255,  std 1.43 - 1.74 steps
    so the correction the head must learn is ~1.5 LSB, against a quantisation
    floor of +/-0.5 LSB.

That is above the floor -- `|d| <= 1` accounts for only 68-79% of samples, with
real mass at +/-2 (8-10%) and +/-3 (2-4%) -- so the dataset is usable. But the
margin is ~2-3x, not comfortable, and a float export would be strictly better. The
exporter has that path (`WL_NOVA_CAPTURE_SET_FLOAT`). Recording `png8` here means
nobody later has to rediscover why the residuals look chunky.
"""
from __future__ import annotations


TRAINING_CONFIG = {
    # A verified denoiser-correction triple set, not temporal pairs.
    "dataset_status": "verified_denoise_arms",
    # The axis the arms actually vary. NOT a generation factor and NOT camera
    # motion: every arm shares a camera, and what differs is the render scale
    # (0.5 vs 1.0) and the accumulation state (partial convergence vs converged).
    "factor_axis": "render_scale_and_convergence",
    # 8-bit PNG, stated honestly. See the module docstring for the measurement.
    "state_encoding": "png8",
    # The buffer the arms were read back from, named rather than described.
    # "matched" used to sit here and it is exactly the claim that let a real bug
    # through: the arms were captured from img_Output (the tonemapped present)
    # while pass 21 samples img_Accumulation (linear, pre-composite). Both are
    # "one engine run with one tonemap" and both are internally consistent, so
    # "matched" was true and useless. The two differ by ~8x in within-scene
    # contrast (std 1.9 vs 15.1 measured), and a head trained on one runs ~60x
    # too strong on the other.
    #
    # The exporter sets WL_NOVA_CAPTURE_IMAGE=1 for the WLRR arms, which is what
    # makes this value true. Changing that index requires changing this string.
    "color_domain": "img_Accumulation_linear_pre_composite",
    # `--validate` checks that every arm at a given scene+camera has identical
    # dimensions, so the arms are pixel-comparable. See export_dataset.py.
    "spatial_alignment": "verified",
}


def require_training_manifest(manifest: dict) -> None:
    """Refuse a manifest whose provenance is missing or unverified.

    Raises ValueError. Deliberately not a warning: a warning is ignored, and the
    failure mode this guards against -- training on the wrong kind of data -- does
    not announce itself in the loss curve.
    """
    config = manifest.get("config", {})
    if not isinstance(config, dict):
        raise ValueError("WLRR training provenance is missing or malformed")
    mismatches = [name for name, expected in TRAINING_CONFIG.items()
                  if config.get(name) != expected]
    if mismatches:
        raise ValueError(
            "WLRR training provenance is not verified denoiser-correction data; "
            "missing or unverified provenance: " + ", ".join(mismatches))
