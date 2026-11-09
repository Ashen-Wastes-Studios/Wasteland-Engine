"""Acceptance control for the WIRED WLRR passes (20/21) -- runtime, not training.

Three headless captures of the SAME pinned camera and the SAME frame index:

  A  WL_WLRR=0                 -- the baseline schedule
  B  WL_WLRR=1, zeroed blob    -- the identity control
  C  WL_WLRR=1, trained blob   -- the correction

B must be byte-identical to A. That is the whole point of the zeroed-blob
contract: alpha=0 makes the anchor the incumbent, and every matmul is zero so
gate*residual is zero, so pass 21 degenerates to `out == incumbent` and the frame
cannot change. If B differs from A the pass is doing something OTHER than the
designed identity -- an unbound UAV, a garbage payload, or a dispatch that runs
when it should not.

C must differ from A. If it does not, the head is not reaching the screen.

ONE-FRAME LAG: pass 21 writes img_Accumulation AFTER the composite (pass 6) has
already consumed it for this frame, so the corrected frame is what the NEXT
present shows. All three captures use the same frame index, so the correction has
been in force for exactly one frame when the shot lands -- the comparison is
apples-to-apples.

Run:  python tools/train_wlrr/verify_wlrr_runtime.py [--scene ...] [--keep]
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "train_neural"))
sys.path.insert(0, str(Path(__file__).resolve().parent))   # train_wlrr MUST win:
# `import model` is ambiguous otherwise -- tools/train_neural/model.py is the
# WLFG head and has no N_PAYLOAD/N_GATE_IN, and it would silently shadow this
# package's model.
import export_dataset as E          # noqa: E402
import model as M                   # noqa: E402

MAGIC = b"WLRR"                     # tools/train_wlrr/export_weights.py's MAGIC

OUT = Path("C:/tmp/wlrr_accept")
DEFAULT_SCENE = "scenes/Example3D.wastescene"
DEFAULT_CAMERA = "auto_y0"
DEFAULT_FRAME = 11


def write_zero_blob(path: Path) -> None:
    """A structurally valid WLRR blob whose payload is the identity.

    The C++ loader validates magic/version/length/CRC before uploading, so the
    control has to be a REAL blob: an all-zero file would be rejected and leave
    the buffer zeroed by a different path, which would not exercise the loader at
    all -- it would test the "no blob" branch, not the "zeroed payload" branch.

    weights 0 | mean 0 | std 1 | alpha 0:
      alpha=0   -> anchor = incumbent (the blend IS the incumbent)
      weights 0 -> gate*residual == 0
    so out == incumbent exactly.
    """
    payload = [0.0] * M.N_PAYLOAD
    for i in range(M.N_GATE_IN):                 # std = 1 so the affine is finite
        payload[M.N_WEIGHTS + M.N_GATE_IN + i] = 1.0
    raw = struct.pack("<%df" % M.N_PAYLOAD, *payload)
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    body = (MAGIC + struct.pack("<I", 1) +
            struct.pack("<I", len(raw)) +
            struct.pack("<I", len(M.LAYER_SHAPES)) +
            struct.pack("<I", len(raw)) +
            struct.pack("<I", crc))
    path.write_bytes(body + raw)
    print(f"wrote {path} ({len(body) + len(raw)} bytes; weights 0, std 1, alpha 0)")


def capture(tag: str, wlrr: int, weights: str | None, scene: str,
            camera: tuple, frame: int) -> Path:
    """One headless capture with the WLRR schedule and blob overridden.

    export_dataset.build_env() copies os.environ, so the overrides go into the
    ambient environment for the child to inherit. They are restored afterwards so
    three captures in one process cannot leak state into each other.
    """
    png = OUT / f"{tag}.png"
    env_extra = {"WL_WLRR": str(wlrr)}
    if weights:
        # FORCE var: build_env overwrites plain WL_WLRR_WEIGHTS with its own zero
        # blob, which silently made every capture the identity. The FORCE var is
        # the only way a diagnostic blob reaches the loader.
        env_extra["WL_WLRR_WEIGHTS_FORCE"] = weights
    saved = {k: os.environ.get(k) for k in env_extra}
    os.environ.update(env_extra)
    try:
        res = E.capture(scene, camera, "wlrr_incumbent", png, timeout=240)
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    size = png.stat().st_size if png.exists() else 0
    rc = res.get("returncode") if isinstance(res, dict) else res
    print(f"  {tag}: rc={rc} png={png.name} {size} bytes")
    return png


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default=DEFAULT_SCENE)
    ap.add_argument("--camera", default=DEFAULT_CAMERA)
    ap.add_argument("--frame", type=int, default=DEFAULT_FRAME)
    ap.add_argument("--trained", default="C:/tmp/wlrr_v7.bin")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)

    cams = E.derive_cameras(args.scene)
    camera = next((c for c in cams if c[0] == args.camera), None)
    if camera is None:
        print(f"error: no camera named {args.camera!r} in {args.scene}; "
              f"have {[c[0] for c in cams]}", file=sys.stderr)
        return 2

    zero = OUT / "zero.bin"
    write_zero_blob(zero)
    trained = Path(args.trained)
    if not trained.is_file():
        print(f"error: trained blob not found: {trained}", file=sys.stderr)
        return 2

    print(f"scene {args.scene} camera {camera[0]} frame {args.frame}")
    print(f"  (pass wlrr_incumbent: accum ON, scale 0.5, WL_AA_FILTER=11, "
          f"PARTIAL_FRAME={E.PARTIAL_FRAME})")
    a = capture("A_baseline", 0, None, args.scene, camera, args.frame)
    a2 = capture("A2_baseline", 0, None, args.scene, camera, args.frame)
    b = capture("B_zeroed", 1, str(zero), args.scene, camera, args.frame)
    c = capture("C_trained", 1, str(trained), args.scene, camera, args.frame)

    print()
    ok = True
    for name, p in (("A", a), ("A2", a2), ("B", b), ("C", c)):
        if not p.exists():
            print(f"  FAIL {name}: capture produced no PNG")
            ok = False
    if not ok:
        print()
        print("RESULT: FAIL")
        return 1

    import numpy as np
    from PIL import Image

    def load(p):
        return np.asarray(Image.open(p).convert("RGB"), dtype=np.float64)

    A, A2, B, C = load(a), load(a2), load(b), load(c)

    # Byte-identity is the WRONG identity test: two captures of the SAME
    # configuration differ by Monte Carlo sampling noise (WL_SAMPLE_PATTERN=5 and
    # WL_FREEZE_TIME=1 do not remove it). So measure that floor with A vs A2 and
    # require the zeroed blob to sit inside it. A bound taken from a different
    # run, or a guess, would be testing arithmetic.
    floor = np.abs(A - A2)
    ident = np.abs(B - A)
    floor_mean, floor_max = float(floor.mean()), float(floor.max())
    ident_mean, ident_max = float(ident.mean()), float(ident.max())
    # 4x the measured floor mean, and no worse than 1 LSB beyond the floor max.
    mean_bound = max(4.0 * floor_mean, 1e-3)
    max_bound = floor_max + 1.0
    ident_ok = ident_mean <= mean_bound and ident_max <= max_bound
    print(f"  noise floor  A vs A2: mean|d| {floor_mean:.6f}  max {floor_max:.1f}")
    print(f"  identity     B vs A : mean|d| {ident_mean:.6f}  max {ident_max:.1f}"
          f"   (bound mean<={mean_bound:.6f}, max<={max_bound:.1f})")
    print(f"  {'ok  ' if ident_ok else 'FAIL'} zeroed blob "
          f"{'is inside the noise floor (identity holds)' if ident_ok else 'EXCEEDS the noise floor'}")
    ok = ok and ident_ok

    corr = np.abs(C - A)
    corr_ok = float(corr.mean()) > 10.0 * max(floor_mean, 1e-6)
    print(f"  correction   C vs A : mean|d| {float(corr.mean()):.6f}  max {float(corr.max()):.1f}")
    print(f"  {'ok  ' if corr_ok else 'FAIL'} trained blob moves the frame "
          f"{'well beyond the floor' if corr_ok else 'by no more than noise'}")
    ok = ok and corr_ok
    print()
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
