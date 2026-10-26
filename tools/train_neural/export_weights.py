"""Export trained weights to the blob the engine uploads.

    python tools/train_neural/export_weights.py
    python tools/train_neural/export_weights.py --weights out/weights.npz --check

Writes:
    assets/neural/neural_weights.bin   raw fp32 little-endian
    assets/neural/neural_weights.json  shapes, feature order, provenance

BYTE LAYOUT -- this is the shader's read order, and it is an ABI
----------------------------------------------------------------
    offset 0   char[4]   magic "WLNR"
    offset 4   uint32    version (1)
    offset 8   uint32    n_in
    offset 12  uint32    n_hidden
    offset 16  uint32    n_residual
    offset 20  uint32    n_gate
    offset 24  uint32    n_weights
    offset 28  uint32    crc32 of the weight payload (little-endian bytes)
    offset 32  float32   weights[n_weights], in LAYER_SHAPES order

The CRC32 is not decoration. The engine uploads this blob into a constant
buffer and a single flipped bit would be a silent visual corruption with no
shader error, no link failure and no log line -- the same failure mode as the
ninth GL image uniform. `deserialize` refuses a blob whose CRC does not match.

Every matrix is row-major in the shape given by `model.LAYER_SHAPES`, so
`W1[i*H + j]` multiplies input feature `i` into hidden unit `j`. `W2` is
`(H, n_in)`, `Wh` is `(n_in, 3)`, `Wg` is `(n_in, 1)`.

`--check` re-reads the written file and asserts it is bit-identical to the
in-memory parameters, and that a corrupted payload is refused -- the two
assertions the shader's packing order depends on.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import model as M
import test_model as T

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
DEFAULT_WEIGHTS = HERE / "out" / "weights.npz"
DEFAULT_OUT_DIR = REPO_ROOT / "assets" / "neural"

MAGIC = b"WLNR"
VERSION = 1
HEADER_BYTES = 32


def serialize(params: dict) -> bytes:
    """Pack the parameters into the on-disk blob.

    Refuses a parameter set whose shapes do not match the ABI. Without this, a
    stale `.npz` from an older architecture would silently serialize into a blob
    that is the wrong length or the wrong shape -- and the only symptom would be
    a wrong image, because the shader reads by offset.
    """
    for name, shape in M.LAYER_SHAPES:
        if name not in params:
            raise ValueError(f"missing parameter {name!r}")
        got = tuple(np.asarray(params[name]).shape)
        if got != tuple(shape):
            raise ValueError(
                f"parameter {name!r} has shape {got}, the ABI requires {tuple(shape)}. "
                f"These weights came from a different architecture -- retrain rather "
                f"than exporting them.")
    vec = M.pack(params).astype("<f4")
    payload = vec.tobytes()
    header = struct.pack("<4s7I", MAGIC, VERSION, M.N_IN, M.N_HIDDEN,
                         M.N_RESIDUAL, M.N_GATE, vec.size, zlib.crc32(payload))
    assert len(header) == HEADER_BYTES
    return header + payload


def deserialize(raw: bytes) -> dict:
    """Inverse of `serialize`. Refuses a truncated, wrong-magic, wrong-shape or
    corrupted blob."""
    if len(raw) < HEADER_BYTES:
        raise ValueError(f"blob is {len(raw)} bytes, header alone is {HEADER_BYTES}")
    magic, version, n_in, n_h, n_res, n_gate, n_w, crc = struct.unpack_from("<4s7I", raw, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {MAGIC!r}")
    if version != VERSION:
        raise ValueError(f"version {version}, expected {VERSION}")
    if (n_in, n_h, n_res, n_gate) != (M.N_IN, M.N_HIDDEN, M.N_RESIDUAL, M.N_GATE):
        raise ValueError(
            f"shape header {(n_in, n_h, n_res, n_gate)} does not match the model "
            f"{(M.N_IN, M.N_HIDDEN, M.N_RESIDUAL, M.N_GATE)} -- the shader and the "
            f"trainer have diverged")
    if n_w != M.N_WEIGHTS:
        raise ValueError(f"header says {n_w} weights, model has {M.N_WEIGHTS}")
    expect = HEADER_BYTES + n_w * 4
    if len(raw) != expect:
        raise ValueError(f"blob is {len(raw)} bytes, expected {expect}")
    payload = raw[HEADER_BYTES:]
    actual = zlib.crc32(payload)
    if actual != crc:
        raise ValueError(f"crc32 mismatch: header {crc:#010x}, payload {actual:#010x} "
                         f"-- the blob is corrupted")
    return M.unpack(np.frombuffer(payload, dtype="<f4", count=n_w))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", default=str(DEFAULT_WEIGHTS))
    ap.add_argument("--out-dir", default=str(DEFAULT_OUT_DIR))
    ap.add_argument("--check", action="store_true",
                    help="re-read the written blob and assert bit-identity")
    args = ap.parse_args(argv)

    src = Path(args.weights)
    if not src.exists():
        print(f"no weights at {src} -- run `python tools/train_neural/train.py` first")
        return 2
    params = T.load_weights(src)

    blob = serialize(params)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_path = out_dir / "neural_weights.bin"
    bin_path.write_bytes(blob)

    meta_path = out_dir / "neural_weights.json"
    meta = {
        "magic": MAGIC.decode("ascii"),
        "version": VERSION,
        "header_bytes": HEADER_BYTES,
        "n_weights": M.N_WEIGHTS,
        "bytes": len(blob),
        "dtype": "float32",
        "endian": "little",
        "features": ["r", "g", "b", "luma", "mean_r", "mean_g", "mean_b", "mean_luma"],
        "feature_note": ("Pass 18 reads r,g,b from s_Output and luma from "
                         "img_NeuralFeatures.x. depth01 and velocity are staged by "
                         "pass 17 but unused by this v1 model: the task-2 dataset "
                         "captures colour only. The four mean_* values are the "
                         "ANCHOR frame's own mean colour and mean luma, reduced "
                         "once per frame -- never from the ground truth, which "
                         "would be leakage."),
        "layers": [{"name": n, "shape": list(s)} for n, s in M.LAYER_SHAPES],
        "forward": [
            "h1 = relu(x @ W1 + b1)",
            "h2 = relu(h1 @ W2 + b2)",
            "h3 = relu(h2 @ W3 + b3)",
            "h4 = relu(h3 @ W4 + b4)",
            "residual = h4 @ Wh + bh",
            "gate = sigmoid(h4 @ Wg + bg)",
            "out = anchor + gate * residual",
        ],
        "source": str(src.resolve()),
        "source_meta": str(src.with_suffix(".json").resolve()),
    }
    if src.with_suffix(".json").exists():
        m = json.loads(src.with_suffix(".json").read_text(encoding="utf-8"))
        meta["training"] = m.get("train")
    meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")

    print(f"wrote {bin_path}  ({len(blob)} bytes: {HEADER_BYTES} header + "
          f"{M.N_WEIGHTS} fp32 = {M.N_WEIGHTS * 4})")
    print(f"wrote {meta_path}")

    if args.check:
        raw = bin_path.read_bytes()
        back = deserialize(raw)
        same = all(np.array_equal(np.asarray(back[k]), np.asarray(params[k])) for k in params)
        magic_ok = raw[:4] == MAGIC
        print(f"  magic {raw[:4]!r}  header n_weights "
              f"{struct.unpack_from('<I', raw, 24)[0]}  bit-identical {same}")
        # and a negative control: a corrupted byte must be caught
        bad = bytearray(blob); bad[-1] ^= 0xFF
        try:
            deserialize(bytes(bad))
            print("  FAIL: corrupted blob was accepted")
            return 1
        except ValueError as e:
            print(f"  corrupted blob rejected: {e}")
        print("  PASS" if (same and magic_ok) else "  FAIL")
        return 0 if (same and magic_ok) else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
