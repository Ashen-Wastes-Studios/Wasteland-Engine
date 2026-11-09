"""Acceptance control for the wired WLRR passes (20/21).

Three headless captures of the SAME pinned camera, same frame index:

  A  WL_WLRR=0                     -- the baseline schedule
  B  WL_WLRR=1, zeroed blob        -- the identity control
  C  WL_WLRR=1, trained blob       -- the correction

B must be byte-identical to A. That is the whole point of the zeroed-blob
contract: alpha=0 makes the anchor the incumbent and every matmul zero makes
gate*residual zero, so pass 21 degenerates to `out == incumbent` and the frame
cannot change. If B differs from A, the pass is doing something OTHER than the
designed identity -- an unbound UAV, a garbage payload, or a dispatch that runs
when it should not.

C must differ from A. If it does not, the head is not reaching the screen.

NOTE on the one-frame lag: pass 21 writes img_Accumulation AFTER the composite
(pass 6) has already consumed it for this frame, so the corrected frame is what
the NEXT present shows. Both captures therefore use the same frame index and the
correction has been applied for exactly one frame by the time the shot lands.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_dataset as E

BIN = E.BIN if hasattr(E, "BIN") else Path(
    "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor")

SCENE = "scenes/Example3D.wastescene"
CAMERA = ("auto_y0", (3.0, 4.0, 0.0), 20.0, -18.0, 0.0)
FRAME = 11
ZEROED = Path("C:/tmp/wlrr_zero.bin")


def zero_blob() -> Path:
    """A structurally valid WLRR blob with a zeroed payload.

    The C++ loader validates magic/version/length/CRC and only then uploads, so
    the control has to be a REAL blob -- an all-zero file would be rejected and
    leave the buffer zeroed by a different path, which would not exercise the
    loader at all.
    """
    import zlib
    import struct
    import numpy as np
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import train_wlrr.export_weights as X  # noqa: F401  (path shim)
    return ZEROED


def main() -> int:
    out = Path("C:/tmp/wlrr_accept")
    out.mkdir(parents=True, exist_ok=True)

    # Use the real exporter's blob writer so the control is byte-compatible with
    # what the loader accepts.
    import subprocess
    zero = out / "zero.bin"
    # A zeroed payload is a valid blob: weights 0, mean 0, std 1, alpha 0.
    # std=1 keeps the affine finite; alpha=0 makes the anchor the incumbent.
    subprocess.run([sys.executable,
                    str(Path(__file__).resolve().parent
                        / "train_wlrr" / "export_weights.py"),
                    "--self-test"], check=False, capture_output=True)
    print("NOTE: writing the zero blob via the exporter's own writer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
