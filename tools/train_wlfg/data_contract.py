"""Shared fail-closed contract for WLFG training datasets.

Existing same-frame image-set captures are diagnostic probes, not verified
frame-synthesis pairs.  The metadata below is necessary, not proof by itself:
the capture pipeline must also verify timestamps, coordinate/color alignment,
and lossless state before it may set these values.
"""
from __future__ import annotations


TRAINING_CONFIG = {
    "dataset_status": "verified_temporal_pairs",
    # The engine does not yet expose a real present-rate multiplier (Step 5),
    # so a verified set is currently gathered at a controlled *camera-motion*
    # axis (deg/frame). The exporter and this file must agree so the harness
    # checks what the capture actually varies. When Step 5 lands, both this
    # entry and the exporter's config emit "generation_factor".
    "factor_axis": "camera_motion_deg_per_frame",
    "state_encoding": "float32_npy",
    "color_domain": "matched",
    "spatial_alignment": "verified",
}


def require_training_manifest(manifest: dict) -> None:
    config = manifest.get("config", {})
    if not isinstance(config, dict):
        raise ValueError("WLFG training provenance is missing or malformed")
    mismatches = [name for name, expected in TRAINING_CONFIG.items()
                  if config.get(name) != expected]
    if mismatches:
        raise ValueError(
            "WLFG training provenance is not verified temporal training data; "
            "missing or unverified provenance: " + ", ".join(mismatches))
