# RTX Kit license constraints

Treat RTX Kit components individually: the restricted RTX SDKs and STBN are not the same license case as RTXMU.

## License boundary

- **Restricted RTX SDKs** (including NRD, RTXTF, RTXDI, OMM, RTXGI, and RTXPT): the NVIDIA RTX SDKs LICENSE §4(e) bars use that would make the SDK subject to an open-source license. Do not include or distribute components covered by those terms in open-source Wasteland.
- **STBN** has a separate NVIDIA source-code license restricting use to non-commercial research or evaluation. Do not ship it as an ordinary open-source asset.
- **RTXMU v1.4** is **MIT-licensed upstream**, not covered by the restricted-SDK conclusion above. RTXMU source is not present or fetched in this source tree.

The SDK restriction is not a blanket prohibition on every artifact associated with RTX Kit; it applies to the components governed by those terms.

## Current RTXMU status

- NVRHI exposes the optional `NVRHI_WITH_RTXMU` CMake hook, default **OFF** at `Wasteland/vendor/nvrhi/CMakeLists.txt:38`; the current cache is also **OFF** at `Wasteland/vendor/nvrhi/build/CMakeCache.txt:401`.
- **Decision: defer.** Revisit only after Wasteland has real acceleration-structure usage and measured AS-memory data shows a need for compaction. See `.hermes/plans/2026-10-04_0327-rendering-options-map.md:3261-3263`.

Open-source prohibition remains in force for components subject to restricted NVIDIA SDK or STBN terms. It does not prohibit MIT-licensed RTXMU; any future adoption should retain its upstream license and provenance.

## The neural rendering stage is clean-room

The trained neural stage documented in `docs/NEURAL_RENDERING.md` contains **no vendor
code, no vendor SDK, and no vendor-derived asset**, and it is not a wrapper around any
of them. Stated explicitly, because "neural rendering" and "DLSS" are easy to conflate:

- It is **not** DLSS 5, not DLSS 4, and not a reimplementation of any vendor model. It
  is a 1140-weight MLP trained on frames this engine captured from itself.
- It is **not** bit-compatible with anything. No vendor tensor layout, no vendor
  accumulation scheme, no vendor model weights.
- It is **not** RTX-gated. It requires compute shaders only, and runs on plain OpenGL —
  a backend with no hardware ray tracing at all.
- It uses **no** RTX Kit component: not NRD, RTXTF, RTXDI, OMM, RTXGI, RTXPT, STBN, or
  RTXMU. There is no acceleration-structure dependency and no vendor maths.
- Its training data is exported from the Wasteland engine by
  `tools/train_neural/export_dataset.py`, which is stdlib-only. Nothing leaves the tree
  to train it, and no third-party dataset is used.

The licence boundary above is therefore unchanged: this stage adds no restricted
component and removes none. The clean-room claim is about provenance, not about the
restrictions — the restrictions never applied to it because there was never any vendor
code in it to restrict.
