"""What does the WLRR identity control actually change?

verify_wlrr_runtime.py reported B (zeroed blob) != A (WL_WLRR=0). This says
whether that is a real change to the colour, a precision artefact of the UAV
round-trip, or the alpha channel being clobbered -- three very different bugs.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

A = Path("C:/tmp/wlrr_accept/A_baseline.png")
B = Path("C:/tmp/wlrr_accept/B_zeroed.png")
C = Path("C:/tmp/wlrr_accept/C_trained.png")


def load(p: Path) -> np.ndarray:
    return np.asarray(Image.open(p).convert("RGBA"), dtype=np.float64)


def report(name: str, x: np.ndarray, y: np.ndarray) -> None:
    d = x - y
    per_ch = [float(np.mean(np.abs(d[..., i]))) for i in range(4)]
    mx = [float(np.max(np.abs(d[..., i]))) for i in range(4)]
    nz = int(np.count_nonzero(d))
    print(f"{name}: mean|d| per channel R{stub(per_ch[0])} G{stub(per_ch[1])} "
          f"B{stub(per_ch[2])} A{stub(per_ch[3])}")
    print(f"{' ' * len(name)}  max|d|  R{mx[0]:.1f} G{mx[1]:.1f} B{mx[2]:.1f} A{mx[3]:.1f}"
          f"   changed pixels {nz} / {d.size}")
    if nz:
        idx = np.argwhere(np.any(d != 0, axis=-1))
        print(f"{' ' * len(name)}  first changed pixel {tuple(idx[0])} -> "
              f"{tuple(x[tuple(idx[0])])} vs {tuple(y[tuple(idx[0])])}")


def stub(v: float) -> str:
    return f"{v:.6f}"


def main() -> int:
    a, b, c = load(A), load(B), load(C)
    print(f"shape {a.shape}")
    report("B-A", a, b)
    report("C-A", a, c)
    report("C-B", b, c)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
