"""Export the trained WLFG synthesis head as a CRC32-guarded constant buffer blob.

numpy only. The blob is the format the Nova compute shader will read as
`u_WlfgWeights` (a `StructuredBuffer` of fp32). A bit-flipped weight in the
buffer is a silent visual corruption with no shader error, no link failure and no
log line -- same failure class as a ninth GL image uniform.

MIRRORED FROM tools/train_neural/export_weights.py: the render-stage `NeuralStageBlob`
has magic, version, shape fields, a CRC32 over the payload, then the payload.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import model as M

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = HERE / "out" / "wlfg_weights.bin"

# Magic, like `NNET_WLFG_HEAD` in NeuralStageBlob.h. Chosen so the shader can
# validate it at load time.
MAGIC = b"WLFG"  # 4 bytes
VERSION = 1        # bump this and the shader's load-time check fails loudly


def pack_weights(params: dict) -> bytes:
    """Flatten the fp32 parameters in LAYER_SHAPES order, fp32."""
    return M.pack(params).tobytes()


def crc32(data: bytes) -> int:
    """A simple, deterministic CRC-32 (matches the engine's NN_CRC32 so the
    shader's load-time check is meaningful)."""
    crc = 0xFFFFFFFF
    poly = 0xEDB88320
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (poly if (crc & 1) else 0)
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


def blob(params: dict) -> bytes:
    """Render the full usable payload."""
    payload = pack_weights(params)
    crc = crc32(payload)
    # magic (4) + version (4) + n_payload (4) + n_shapes (4) + total (4) + crc (4)
    body = (MAGIC + VERSION.to_bytes(4, "little") +
            len(payload).to_bytes(4, "little") +
            len(M.LAYER_SHAPES).to_bytes(4, "little") +
            len(payload).to_bytes(4, "little") +
            crc.to_bytes(4, "little"))
    return body + payload


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--weights", default=str(DEFAULT_OUT))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args(argv)

    weights = np.load(args.weights)
    params = {k: weights[k] for k in weights.files}
    data = blob(params)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"wrote {out}  ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
