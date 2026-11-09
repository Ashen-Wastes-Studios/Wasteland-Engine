"""Export the trained WLRR head as a CRC32-guarded constant buffer blob.

numpy only. The blob is the format the Nova compute pass will read as its weights
`StructuredBuffer` (fp32). A bit-flipped weight in the buffer is a silent visual
corruption with no shader error, no link failure and no log line -- the same
failure class as a ninth GL image uniform, which is why the CRC is here and why
the shader validates the magic at load time.

MIRRORED FROM `tools/train_wlfg/export_weights.py`, which is itself mirrored from
`tools/train_neural/export_weights.py`. The header layout is deliberately
identical so one reader serves all three:

    magic (4) | version (4) | payload bytes (4) | shape count (4) | total (4) | CRC32 (4)

The magic differs (`WLRR` not `WLFG`), so loading one head's blob into the other
head's buffer fails the load-time check instead of silently reinterpreting 468
floats as 484.
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
DEFAULT_OUT = HERE / "out" / "wlrr_weights.bin"

# Distinct from b"WLFG" on purpose: see the module docstring. A head's blob must
# not be loadable into the other head's buffer.
MAGIC = b"WLRR"  # 4 bytes
VERSION = 1       # bump this and the shader's load-time check fails loudly


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


def pack_weights(params: dict, mean=None, std=None, alpha=None) -> bytes:
    """Flatten the fp32 payload: weights, then the affine, then the blend alpha.

    The affine and the blend are part of the payload, not shader constants -- see
    `model.pack_payload`. Shipping weights without the standardisation they were
    trained against is a total, silent corruption.
    """
    return M.pack_payload(params, mean, std, alpha).tobytes()


def blob(params: dict, mean=None, std=None, alpha=None) -> bytes:
    """Render the full usable payload."""
    payload = pack_weights(params, mean, std, alpha)
    crc = crc32(payload)
    body = (MAGIC + VERSION.to_bytes(4, "little") +
            len(payload).to_bytes(4, "little") +
            len(M.LAYER_SHAPES).to_bytes(4, "little") +
            len(payload).to_bytes(4, "little") +
            crc.to_bytes(4, "little"))
    return body + payload


HEADER_BYTES = 24


def parse(data: bytes) -> tuple[dict, int]:
    """Read a blob back. Returns (header fields, payload offset).

    This exists so the format is checked by something other than the writer: a
    writer and a reader that share a misunderstanding agree with each other and
    disagree with the shader.
    """
    if len(data) < HEADER_BYTES:
        raise ValueError(f"blob is {len(data)} bytes, shorter than the {HEADER_BYTES}-byte header")
    magic = data[0:4]
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {MAGIC!r}")
    version = int.from_bytes(data[4:8], "little")
    n_payload = int.from_bytes(data[8:12], "little")
    n_shapes = int.from_bytes(data[12:16], "little")
    total = int.from_bytes(data[16:20], "little")
    crc = int.from_bytes(data[20:24], "little")
    if n_shapes != len(M.LAYER_SHAPES):
        raise ValueError(f"blob declares {n_shapes} shapes, model has {len(M.LAYER_SHAPES)}")
    if n_payload != total:
        raise ValueError(f"payload bytes {n_payload} != total {total}")
    if len(data) - HEADER_BYTES != n_payload:
        raise ValueError(f"blob carries {len(data) - HEADER_BYTES} payload bytes, "
                         f"header says {n_payload}")
    actual = crc32(data[HEADER_BYTES:])
    if actual != crc:
        raise ValueError(f"CRC32 mismatch: header {crc:#010x}, computed {actual:#010x}")
    return {"magic": magic, "version": version, "n_payload": n_payload,
            "n_shapes": n_shapes, "crc": crc}, HEADER_BYTES


def self_test() -> int:
    """Round-trip and corruption checks. No training, no GPU."""
    checks = []

    rng = np.random.default_rng(0)
    p = M.init_params(rng)
    mean = rng.uniform(10.0, 40.0, size=M.N_FEAT)
    std = rng.uniform(1.5, 8.0, size=M.N_FEAT)
    data = blob(p, mean, std)
    checks.append((f"blob length is header + N_PAYLOAD*4 ({M.N_PAYLOAD})",
                   len(data) == HEADER_BYTES + M.N_PAYLOAD * 4))

    hdr, off = parse(data)
    checks.append(("magic round-trips", hdr["magic"] == MAGIC))
    checks.append(("version round-trips", hdr["version"] == VERSION))
    checks.append(("shape count round-trips", hdr["n_shapes"] == len(M.LAYER_SHAPES)))

    payload = np.frombuffer(data[off:], dtype=np.float32)
    back, bmean, bstd, balpha = M.unpack_payload(payload)
    for name, _ in M.LAYER_SHAPES:
        if not np.array_equal(p[name], back[name]):
            checks.append((f"round-trip {name}", False))
            break
    else:
        checks.append(("every layer round-trips bit-exactly", True))

    # THE AFFINE IS PART OF THE ABI. If these do not survive the round-trip, the
    # shader standardises with the wrong constants and every weight is wrong with
    # nothing raised anywhere -- the failure this payload layout exists to prevent.
    checks.append(("the affine mean round-trips exactly",
                   np.array_equal(bmean, mean.astype(np.float32))))
    checks.append(("the affine std round-trips exactly",
                   np.array_equal(bstd, std.astype(np.float32))))
    checks.append(("the affine lands at AFFINE_OFFSET",
                   np.array_equal(payload[M.AFFINE_OFFSET:M.AFFINE_OFFSET + M.N_FEAT],
                                  mean.astype(np.float32))))

    # A reader that stopped at the weights -- the natural mistake -- must FAIL
    # rather than silently load a blob whose affine it never read.
    try:
        M.unpack_payload(payload[:M.N_WEIGHTS])
        checks.append(("a payload truncated to the weights is rejected", False))
    except ValueError:
        checks.append(("a payload truncated to the weights is rejected", True))

    # The identity must be DETECTABLE, so the exporter can refuse to ship one by
    # accident while still allowing it deliberately.
    checks.append(("the identity affine is detected",
                   M.affine_is_identity(np.zeros(M.N_FEAT), np.ones(M.N_FEAT))))
    checks.append(("a measured affine is not mistaken for the identity",
                   not M.affine_is_identity(mean, std)))

    # A single flipped bit in the payload must be caught.
    bad = bytearray(data)
    bad[HEADER_BYTES + 5] ^= 0x01
    try:
        parse(bytes(bad))
        checks.append(("a bit-flipped payload is rejected", False))
    except ValueError:
        checks.append(("a bit-flipped payload is rejected", True))

    # A bit flip in the AFFINE must be caught too -- the CRC covers the whole
    # payload, and this asserts the affine is genuinely inside it.
    bad = bytearray(data)
    bad[HEADER_BYTES + M.AFFINE_OFFSET * 4] ^= 0x01
    try:
        parse(bytes(bad))
        checks.append(("a bit-flipped affine is rejected", False))
    except ValueError:
        checks.append(("a bit-flipped affine is rejected", True))

    # The other head's magic must be rejected -- this is why WLRR != WLFG.
    wrong = b"WLFG" + data[4:]
    try:
        parse(wrong)
        checks.append(("the WLFG magic is rejected", False))
    except ValueError:
        checks.append(("the WLFG magic is rejected", True))

    # A truncated blob must be rejected.
    try:
        parse(data[:-4])
        checks.append(("a truncated blob is rejected", False))
    except ValueError:
        checks.append(("a truncated blob is rejected", True))

    for n, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {n}")
    bad_n = [n for n, ok in checks if not ok]
    print()
    print(f"RESULT: {len(checks) - len(bad_n)}/{len(checks)} self-test checks passed")
    return 1 if bad_n else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--weights", default=str(DEFAULT_OUT),
                    help="an .npz whose arrays are the LAYER_SHAPES entries")
    ap.add_argument("--stats", default=None,
                    help="dataset_stats.json carrying feature_mean/feature_std; "
                         "defaults to the file beside --weights")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--identity-affine", action="store_true",
                    help="permit an identity affine (untrained or control head only)")
    ap.add_argument("--self-test", action="store_true",
                    help="check the format round-trips and rejects corruption")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    weights = np.load(args.weights)
    missing = [n for n, _ in M.LAYER_SHAPES if n not in weights.files]
    if missing:
        raise SystemExit(f"{args.weights} is missing arrays: {', '.join(missing)}")
    params = {k: weights[k] for k in weights.files}

    # THE AFFINE IS REQUIRED, NOT OPTIONAL. Refusing to write a blob without one
    # is the point: a head trained against standardised features but shipped with
    # raw features looks plausible and is wrong everywhere.
    stats_path = Path(args.stats) if args.stats else Path(args.weights).with_name("dataset_stats.json")
    if not stats_path.is_file():
        raise SystemExit(
            f"no dataset_stats.json at {stats_path}. The affine is part of the ABI and "
            f"cannot be assumed -- pass --stats, or --identity-affine for an untrained head.")
    stats = json.loads(stats_path.read_text(encoding="utf-8"))
    try:
        mean = np.asarray(stats["feature_mean"], dtype=np.float64)
        std = np.asarray(stats["feature_std"], dtype=np.float64)
    except KeyError as exc:
        raise SystemExit(f"{stats_path} has no {exc.args[0]!r}") from exc
    if mean.shape != (M.N_FEAT,) or std.shape != (M.N_FEAT,):
        raise SystemExit(f"{stats_path} affine has shape {mean.shape}/{std.shape}, "
                         f"expected ({M.N_FEAT},) -- was it written by a different ABI?")

    # The anchor blend weight is part of the ABI for the same reason the affine is:
    # the head is anchored at `inc + alpha*(inp - inc)`, and shipping weights with
    # a different alpha silently changes what the head is correcting. A stats file
    # without it is from an older ABI and must say so.
    alpha_raw = stats.get("blend_alpha")
    if alpha_raw is None:
        raise SystemExit(
            f"{stats_path} has no 'blend_alpha'. The anchor blend is part of the ABI; "
            f"a stats file without it predates the blend anchor.")
    alpha = np.asarray(alpha_raw, dtype=np.float64)
    if alpha.shape != (M.N_INCUMBENT,):
        raise SystemExit(f"{stats_path} blend_alpha has shape {alpha.shape}, "
                         f"expected ({M.N_INCUMBENT},) -- was it written by a different ABI?")
    if not np.all(np.isfinite(alpha)):
        raise SystemExit(f"{stats_path} blend_alpha is not finite")

    if M.affine_is_identity(mean, std) and not args.identity_affine:
        raise SystemExit(
            "the affine in the stats file is the IDENTITY. That is correct only for an "
            "untrained or control head; for a trained one it means the statistics were "
            "never measured, and shipping it would standardise nothing while the weights "
            "expect standardised inputs. Pass --identity-affine if this is deliberate.")

    data = blob(params, mean, std, alpha)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    # Parse what was written rather than trusting the writer, and re-read the
    # affine from the bytes on disk -- the shader reads those bytes, not our
    # in-memory arrays.
    _, off = parse(out.read_bytes())
    _, wmean, wstd, walpha = M.unpack_payload(np.frombuffer(out.read_bytes()[off:], dtype=np.float32))
    if not np.array_equal(wmean, mean.astype(np.float32)):
        raise SystemExit("the affine written to disk does not match the stats file")
    print(f"wrote {out}  ({len(data)} bytes = {HEADER_BYTES} header + "
          f"{M.N_WEIGHTS} weights + {2 * M.N_FEAT} affine + "
          f"{M.N_INCUMBENT} blend floats)")
    print(f"affine from {stats_path}")
    print(f"  mean {np.round(wmean, 2).tolist()}")
    print(f"  std  {np.round(wstd, 2).tolist()}")
    print(f"  blend alpha {np.round(walpha, 4).tolist()}  "
          f"(anchor = incumbent + alpha*(input - incumbent))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
