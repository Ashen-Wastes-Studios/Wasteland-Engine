# Nova SVGF denoiser — wiring status

**Source wiring is present; GPU execution and visual quality are not verified.** Shader, resource, and dispatch wiring for SVGF passes 12/13 exists on OpenGL and the DX11/DX12/Vulkan Nova paths. This is not evidence that the denoiser has run successfully on a GPU.

## Current status

- **Default-off:** `SvfgEnabled = false` and `RestirEnabled = false` in `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:679-684`.
- **OpenGL:** moments texture creation/binding and gated pass-12/pass-13 dispatch with image barriers are in `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:2069-2075, 3334-3338, 3559-3581`.
- **NVRHI Nova (DX11/DX12/Vulkan):** the shared dispatch schedule is in `Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:2481-2506`; moments resources/bindings are in `Wasteland/src/Platform/NVRHI/DX11Nova.h:286-307, 497-525`, `DX12Nova.cpp:542-572`, and `VulkanNova.cpp:32-43, 522-538`.
- **Shaders:** pass 12 moments and pass 13 A-Trous filter are declared in `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.glsl:3957-3992, 4033-4050` and `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl:2443-2478, 2519-2536`.
- **ReSTIR DI:** when enabled, the shared schedule is pass 3 → pass 11 → swap → pass 14 → swap → pass 2. Pass 14 reads an immutable temporal reservoir, re-evaluates four cardinal neighbors at the center receiver, and writes the distinct output buffer. Passes 11/14 currently carry accepted history/neighbor `M` unconditionally and normalize by total `M`; they do not implement ReSTIR's source-support normalization for the final selected sample. Receiver-dependent visibility or target support can therefore darken the estimate.[1] Correlation alone is not the bias proof; under valid support and conditional resampling it changes variance. The path remains experimental/default-off pending a support-aware estimator and GPU/visual validation (`Renderer3D.cpp:2481-2506`; shader twins `RunRestirSpatial`). `tools/verify_restir_spatial_correlation.py` contains an exact finite-model witness, not GPU proof.
- **Vulkan:** `VulkanNova` is constructed and uses the shared Nova dispatch schedule (`Renderer3D.cpp:1868-1873, 2986`). The source-level dispatch and binding contracts are checked, but fresh Vulkan startup, validation-layer cleanliness, and displayed output remain unverified; hardware-RT capability is not claimed.
- **Trained neural stage (passes 17/18/19):** the wiring status is *different* from the denoiser's, and the difference is worth stating plainly. Unlike SVGF, this stage **has been seen to run and has been verified against its own model**: the GL shader reproduces the Python forward pass to `mean|d| 0.000026`, a zero-weight control reproduces the anchor exactly, and the panel's `gate mean` is a real reduction measured on a live capture. **Only on OpenGL.** The DX11/DX12/Vulkan twins carry the same stage and pass `tools/verify_neural_stage_parity.py`, but none of the three has rendered — DX12 hangs during init here and VulkanNova is never instantiated — and the gate readback exists only on GL. See `docs/NEURAL_RENDERING.md` §5. Do not read "verified" off the parity tool: it proves the twins agree, not that a GPU ran them.

## Filter and verification limits

The filter uses variance in its per-tap edge-stopping weights, but its dispatch ladder is fixed: up to four A-Trous passes with step sizes **1, 2, 4, 8**, not a variance-adaptive iteration count (`Wasteland/src/Wasteland/Renderer/Renderer3D.cpp:2506-2524, 3568-3581`; shader weighting in `DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl:2531-2600`).

## Sources

[1] https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf — Bitterli et al. 2020 — Spatiotemporal reservoir resampling

Builds, shader compilation, binding checks, and math/static verifiers can validate only the properties they test; they do not prove GPU execution, correct runtime synchronization, or visual quality. **No runtime screenshot or visual-quality result is verified** for the denoiser, ReSTIR DI, or any of the DX11/DX12/Vulkan paths.

The one exception in this file's scope is the trained neural stage **on OpenGL**, which has a float-dump comparison against its own model and a live screenshot of the editor panel. That is a real runtime result for one backend, not a general claim — see `docs/NEURAL_RENDERING.md`.
