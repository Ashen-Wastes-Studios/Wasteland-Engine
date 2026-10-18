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
