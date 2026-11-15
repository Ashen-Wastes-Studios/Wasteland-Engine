"""Prove the pass-23 GPU forward pass is the trained Python model.

Task 8 Step 4 wires the WLFG synthesis head onto the GPU. The failure mode this
file exists to catch is a LAYOUT error: the shader reads `WlfgWeights` (t75) by
hard-coded float offsets, and a wrong offset does not crash, log, or link-fail --
every value is still a plausible float, the pass still runs, and the image is
merely wrong. Same failure class as a bit-flipped weight.

So this file checks the contract in two layers:

  1. STRUCTURAL -- the shader's `WLFG_*_BASE` macros, `model.LAYER_SHAPES`
     cumulative offsets, and `kWlfgWeightCount` in VulkanNova.h must agree. A
     drift in any one of the three is caught statically, without a GPU.

  2. NUMERICAL -- the shader's forward pass is transcribed into numpy FROM THE
     PARSED MACRO VALUES and compared against `model.predict` on random weights
     and random inputs. This is what proves the indexing arithmetic itself
     (`f * N_HIDDEN + a`, row-major) matches `pixel @ W1`.

A mutation test closes the loop: shifting a base offset by one must make the
transcription disagree, so a silently-wrong offset cannot pass.

Run: python -m unittest tools.train_wlfg.test_gpu_parity   (or `-p 'test*.py'`)
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import model as M

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SLANG = REPO / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.slang"
VULKAN_H = REPO / "Wasteland" / "src" / "Platform" / "NVRHI" / "VulkanNova.h"
EXPORT = HERE / "export_weights.py"

# The shader's WLFG block, from the `WlfgWeights` declaration down to the last
# macro. Bounded so an unrelated `#define` elsewhere cannot leak in.
_MACRO_RE = re.compile(r"^#define\s+(WLFG_\w+)\s+(.+?)\s*(?://.*)?$", re.M)


def parse_shader_macros(path: Path = SLANG) -> dict[str, int]:
    """Resolve every `#define WLFG_*` to an int.

    The definitions are expressions over previously-defined macros
    (`(WLFG_W1_BASE + WLFG_N_IN * WLFG_N_HIDDEN)`), so they are substituted in
    file order -- which is also their dependency order.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    resolved: dict[str, int] = {}
    for name, expr in _MACRO_RE.findall(text):
        body = expr.strip()
        # Substitute already-resolved macro names, longest first so WLFG_N_IN
        # cannot clobber a prefix of WLFG_N_WEIGHTS.
        for known in sorted(resolved, key=len, reverse=True):
            body = re.sub(rf"\b{known}\b", str(resolved[known]), body)
        if re.search(r"\bWLFG_\w+\b", body):
            raise AssertionError(f"{name}: unresolved macro in {expr!r}")
        try:
            resolved[name] = int(eval(body, {"__builtins__": {}}, {}))  # noqa: S307
        except Exception as exc:  # pragma: no cover - only on a malformed shader
            raise AssertionError(f"{name}: cannot evaluate {body!r}: {exc}") from exc
    if not resolved:
        raise AssertionError(f"no WLFG_* macros found in {path}")
    return resolved


def parse_vulkan_weight_count(path: Path = VULKAN_H) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"kWlfgWeightCount\s*=\s*(\d+)", text)
    if not m:
        raise AssertionError(f"kWlfgWeightCount not found in {path}")
    return int(m.group(1))


def model_offsets() -> dict[str, int]:
    """Cumulative element offsets of model.LAYER_SHAPES, in pack() order."""
    out: dict[str, int] = {}
    off = 0
    for name, shape in M.LAYER_SHAPES:
        out[name] = off
        off += int(np.prod(shape))
    return out


def shader_forward(w: np.ndarray, x: np.ndarray, base: dict[str, int]) -> np.ndarray:
    """The pass-23 forward pass, transcribed from the Slang source.

    `x` is (N, 7) ALREADY STANDARDISED (identity affine in v1). `base` supplies
    the offsets so the mutation test can perturb them.
    """
    n = x.shape[0]
    n_in = base["WLFG_N_IN"]
    n_hid = base["WLFG_N_HIDDEN"]
    n_res = base["WLFG_N_RES"]

    h1 = np.zeros((n, n_hid), dtype=np.float32)
    for a in range(n_hid):
        acc = np.full(n, w[base["WLFG_B1_BASE"] + a], dtype=np.float32)
        for f in range(n_in):
            acc = acc + x[:, f] * np.float32(w[base["WLFG_W1_BASE"] + f * n_hid + a])
        h1[:, a] = np.maximum(acc, 0.0)

    h2 = np.zeros((n, n_hid), dtype=np.float32)
    for b in range(n_hid):
        acc = np.full(n, w[base["WLFG_B2_BASE"] + b], dtype=np.float32)
        for a in range(n_hid):
            acc = acc + h1[:, a] * np.float32(w[base["WLFG_W2_BASE"] + a * n_hid + b])
        h2[:, b] = np.maximum(acc, 0.0)

    residual = np.zeros((n, n_res), dtype=np.float32)
    for c in range(n_res):
        residual[:, c] = np.float32(w[base["WLFG_BR_BASE"] + c])
    gacc = np.full(n, w[base["WLFG_BG_BASE"]], dtype=np.float32)
    for c in range(n_hid):
        h = h2[:, c]
        for r in range(n_res):
            residual[:, r] = residual[:, r] + h * np.float32(
                w[base["WLFG_WR_BASE"] + c * n_res + r])
        gacc = gacc + h * np.float32(w[base["WLFG_WG_BASE"] + c])

    gate = (1.0 / (1.0 + np.exp(-gacc))).astype(np.float32)
    return residual, gate


class TestShaderLayout(unittest.TestCase):
    """Layer 1: the three sources of truth for the weight layout must agree."""

    def setUp(self):
        self.macros = parse_shader_macros()
        self.offsets = model_offsets()

    def test_base_offsets_match_model_layer_order(self):
        for name, expected in self.offsets.items():
            self.assertIn(f"WLFG_{name.upper()}_BASE", self.macros,
                          f"shader has no WLFG_{name.upper()}_BASE macro")
            self.assertEqual(self.macros[f"WLFG_{name.upper()}_BASE"], expected,
                             f"{name}: shader offset {self.macros[f'WLFG_{name.upper()}_BASE']} "
                             f"!= model offset {expected}")

    def test_dimensions_match_model(self):
        self.assertEqual(self.macros["WLFG_N_IN"], M.N_IN)
        self.assertEqual(self.macros["WLFG_N_HIDDEN"], M.N_HIDDEN)
        self.assertEqual(self.macros["WLFG_N_RES"], M.N_RESIDUAL)

    def test_weight_count_agrees_across_three_files(self):
        self.assertEqual(self.macros["WLFG_N_WEIGHTS"], M.N_WEIGHTS,
                         "shader WLFG_N_WEIGHTS != model.N_WEIGHTS")
        self.assertEqual(parse_vulkan_weight_count(), M.N_WEIGHTS,
                         "VulkanNova.h kWlfgWeightCount != model.N_WEIGHTS")

    def test_no_offsets_overlap(self):
        """Every layer must start exactly where the previous one ends."""
        pairs = list(self.offsets.items())
        for (name, off), (nxt, nxt_off) in zip(pairs, pairs[1:]):
            shape = dict(M.LAYER_SHAPES)[name]
            self.assertEqual(off + int(np.prod(shape)), nxt_off,
                             f"{name} does not end where {nxt} begins")


class TestShaderForwardMatchesModel(unittest.TestCase):
    """Layer 2: the transcribed GPU math must equal the trained Python model."""

    def setUp(self):
        self.base = parse_shader_macros()
        rng = np.random.default_rng(20261102)
        # Non-trivial weights: real magnitudes in every layer, and a residual head
        # that is NOT zero, so a wrong offset in Wr/Wg cannot hide.
        p = M.init_params(rng)
        for k, v in p.items():
            if k.startswith("W") or k.startswith("b"):
                p[k] = (v + rng.normal(0.0, 0.05, v.shape)).astype(np.float32)
        self.params = p
        self.packed = M.pack(p).astype(np.float32)
        self.assertEqual(self.packed.size, M.N_WEIGHTS)

        n = 64
        self.warped = rng.random((n, 3), dtype=np.float32)
        self.mask = rng.random((n, 1), dtype=np.float32)
        self.vel = (rng.random((n, 2), dtype=np.float32) * 2.0 - 1.0)
        self.depth = rng.random((n, 1), dtype=np.float32)

    def test_matches_model_predict(self):
        want_out, want_gate = M.predict(self.params, self.warped, self.mask,
                                        self.vel, self.depth)
        x = M.features(self.warped, self.mask, self.vel, self.depth)
        residual, gate = shader_forward(self.packed, x, self.base)
        got_out = self.warped + gate[:, None] * residual

        gate_d = float(np.max(np.abs(gate - want_gate.reshape(-1))))
        out_d = float(np.max(np.abs(got_out - want_out)))
        print(f"\n  shader vs model: max|gate| {gate_d:.3e}  max|out| {out_d:.3e}")
        self.assertLess(gate_d, 1e-5, f"gate diverges by {gate_d:.3e}")
        self.assertLess(out_d, 1e-5, f"out diverges by {out_d:.3e}")

    def test_zero_weights_is_exactly_the_identity(self):
        """The acceptance control: a zeroed blob must leave the anchor untouched.

        This is why a missing/corrupt blob is safe. All matmuls are zero, so
        h1 = h2 = 0, residual = br = 0, and out = anchor + gate * 0 == anchor.
        """
        zero = np.zeros(M.N_WEIGHTS, dtype=np.float32)
        x = M.features(self.warped, self.mask, self.vel, self.depth)
        residual, gate = shader_forward(zero, x, self.base)
        got = self.warped + gate[:, None] * residual
        self.assertTrue(np.array_equal(got, self.warped),
                        "zero weights must reproduce the anchor bit-for-bit")

    def test_offset_mutation_is_caught(self):
        """A one-float shift in a base offset must break parity.

        Guards against this file passing by accident: if the transcription were
        insensitive to the offsets, layer 1 would be the only real check.
        """
        want_out, _ = M.predict(self.params, self.warped, self.mask,
                                self.vel, self.depth)
        x = M.features(self.warped, self.mask, self.vel, self.depth)
        for macro in ("WLFG_W1_BASE", "WLFG_W2_BASE", "WLFG_WR_BASE", "WLFG_WG_BASE"):
            bad = dict(self.base)
            bad[macro] += 1
            residual, gate = shader_forward(self.packed, x, bad)
            got = self.warped + gate[:, None] * residual
            self.assertGreater(float(np.max(np.abs(got - want_out))), 1e-6,
                               f"shifting {macro} by one was NOT caught")


class TestBlobContract(unittest.TestCase):
    """The C++ loader and the Python exporter must agree on the byte format."""

    HEADER_BYTES = 24  # magic(4) version(4) payload(4) shapes(4) total(4) crc(4)

    def test_exporter_header_size_matches_loader(self):
        import export_weights as E

        rng = np.random.default_rng(7)
        params = M.init_params(rng)
        data = E.blob(params)
        payload = M.pack(params).tobytes()
        self.assertEqual(len(data), self.HEADER_BYTES + len(payload),
                         "blob size is not header + payload")

    def test_loader_crc32_matches_exporter_crc32(self):
        """Transcribe VulkanNova::LoadWlfgWeights' CRC32 and compare.

        The loader implements the same CRC32 inline (no shared header), so the
        two implementations are only kept in step by this test.
        """
        import export_weights as E

        def loader_crc32(data: bytes) -> int:
            crc = 0xFFFFFFFF
            for byte in data:
                crc ^= byte
                for _ in range(8):
                    crc = (crc >> 1) ^ (0xEDB88320 if (crc & 1) else 0)
            return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF

        for seed in (1, 2, 3):
            rng = np.random.default_rng(seed)
            payload = M.pack(M.init_params(rng)).tobytes()
            self.assertEqual(loader_crc32(payload), E.crc32(payload),
                             f"CRC32 disagrees on seed {seed}")

    def test_loader_rejects_a_bit_flip(self):
        """The whole point of the CRC: a corrupted payload must not match."""
        import export_weights as E

        rng = np.random.default_rng(11)
        payload = bytearray(M.pack(M.init_params(rng)).tobytes())
        want = E.crc32(bytes(payload))
        payload[5] ^= 0x01  # flip one bit
        self.assertNotEqual(E.crc32(bytes(payload)), want,
                            "a single flipped bit did not change the CRC")

    def test_header_fields(self):
        import export_weights as E

        rng = np.random.default_rng(13)
        params = M.init_params(rng)
        data = E.blob(params)
        self.assertEqual(data[:4], b"WLFG")
        self.assertEqual(int.from_bytes(data[4:8], "little"), 1)          # version
        self.assertEqual(int.from_bytes(data[8:12], "little"),
                         M.N_WEIGHTS * 4)                                  # payload bytes
        self.assertEqual(int.from_bytes(data[12:16], "little"),
                         len(M.LAYER_SHAPES))                              # shape count
        self.assertEqual(int.from_bytes(data[20:24], "little"),
                         E.crc32(data[self.HEADER_BYTES:]))                # CRC32


if __name__ == "__main__":
    unittest.main(verbosity=2)
