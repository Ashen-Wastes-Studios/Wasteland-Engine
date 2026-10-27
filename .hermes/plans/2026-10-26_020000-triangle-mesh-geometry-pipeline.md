# Triangle-mesh geometry pipeline — plan

**Status:** replaces `.hermes/plans/2026-10-25_195000-virtualized-instances-and-hybrid-raster.md`.
That plan optimised an analytic-primitive tracer and is **superseded**, not amended. Its
measurements are still the constraints this plan must respect; its phases are dead. See
"Constraints inherited from measurement" below before proposing anything.

**Answers that set the shape of this plan** (from the product owner, 2026-10-26):

1. **Arbitrary triangle-mesh support IS a product requirement.**
2. **Target is 4K.**
3. **The GPU is not quiet** — no clean measurement window is available.

Answer 1 is what kills the old plan: it was built to scale *analytic primitives*, and Nova has
no triangles at all. Answer 3 constrains how every number in this plan may be obtained.

---

## 1. Why the old plan cannot be extended

Nova today is a single-bounce analytic ray tracer:

- `PrimitiveShape` has 8 entries (Cube, Sphere, Plane, Cylinder, Cone, Torus, Circle, Grid).
- `HitPrimitive` switches on a shape ordinal; there is no vertex buffer, no index buffer, no UV
  attribute, no material graph — "There is no mesh and no UV attribute for these."
- `BVHPrim` is one AABB per *instance*; there is no BLAS. It is a TLAS over instances.
- `u_MaxBounces` is 1 at every preset and documented unused, so "secondary rays" means shadow
  rays only.

Measured cost, 4K, DRS off, analytic primitives only:

| component | cost |
|---|---|
| trace (primary visibility) | ~131 ms — **88% of the frame** |
| shadow rays (all of them, removed) | 0.5 ms |
| POM (removed) | ~0.0 ms |
| surface shading, lights, fog (removed) | ~0.0 ms |
| composite | 8.2 ms |
| ImGui | 3.4 ms |
| BVH traversal ordering (front-to-back) | 0.0 ms |
| instance count, 31 → 24,576 | 1.6x, but only outside gameplay views |

**The tracer is pixel-bound.** It costs ~160 ms at 4K regardless of what is in front of it.
Adding triangles makes the traversal *strictly more expensive* — a triangle BVH has orders of
magnitude more nodes than a 250-instance TLAS. So "trace the triangles instead" is not an option
at 4K on this hardware, and no amount of LOD, culling or streaming fixes a per-pixel cost.

That is the whole justification for the architecture below.

---

## 2. Target architecture

**Rasterize primary visibility; trace only what rasterization cannot answer.**

```
        ┌─────────────────────────────────────────────────────┐
        │ 1. GPU-driven cull + LOD select  (indirect draws)   │
        └──────────────────────────┬──────────────────────────┘
                                   ▼
        ┌─────────────────────────────────────────────────────┐
        │ 2. Visibility buffer:  meshId | primId | barycentrics│  ← raster, all backends
        └──────────────────────────┬──────────────────────────┘
                                   ▼
        ┌─────────────────────────────────────────────────────┐
        │ 3. Material resolve (deferred shading from vis-buf)  │
        └──────────────────────────┬──────────────────────────┘
                                   ▼
        ┌─────────────────────────────────────────────────────┐
        │ 4. Secondary rays: HW RT if available, else fallback │
        └─────────────────────────────────────────────────────┘
```

Why this shape:

- **Primary visibility by rasterization is resolution-bound, not scene-bound.** It is what GPUs
  are built for, and it is the only thing that makes 4K tractable given the 160 ms/pixel-bound
  finding.
- **A visibility buffer decouples geometry from shading.** No G-buffer per material, no
  overdraw cost in the material pass, and material evaluation happens once per pixel.
- **Secondary rays stay rays.** They are 0.5 ms today for shadows; HW RT makes them cheap and
  correct for triangle meshes.

### Backend reality (this constrains everything)

NVRHI here targets Vulkan, D3D12, D3D11 and OpenGL. Hardware ray tracing exists only on
Vulkan (`VK_KHR_ray_tracing_pipeline`) and D3D12 (`DXR`). **D3D11 and OpenGL have no RT.**

So:

| stage | Vulkan | D3D12 | D3D11 | OpenGL |
|---|---|---|---|---|
| cull + indirect draw | yes | yes | yes (indirect) | yes (indirect) |
| visibility buffer raster | yes | yes | yes | yes |
| material resolve | yes | yes | yes | yes |
| secondary rays, HW RT | yes | yes | **no** | **no** |
| secondary rays, fallback | analytic tracer / SSAO | analytic tracer / SSAO | analytic tracer / SSAO | analytic tracer / SSAO |

**Raster primary must be the common path, not a mode.** Secondary-ray quality becomes a
backend-tiered feature. If that is unacceptable, the alternative is dropping D3D11 and OpenGL —
a product decision, not an engineering one, and it should be made before Phase 4.

### Clean-room boundary (non-negotiable)

Nova may not vendor NVIDIA RTX SDKs — NRD, RTXTF, STBN, OMM and RTXDI were removed 2026-10-04.
DXR and `VK_KHR_ray_tracing` are Microsoft/Khronos APIs and are fine. Anything that looks like a
virtual-geometry cluster hierarchy must be written from the published literature, not ported.

---

## 3. Phases

Each phase has a gate. **Do not start a phase whose gate the previous phase did not pass.**

### Phase A — Mesh representation and a single-mesh raster path

*Objective:* get one triangle mesh on screen through the new path, with the existing analytic
tracer still working, so nothing regresses while the pipeline is built.

1. `MeshAsset`: interleaved vertex buffer (position, normal, uv, tangent), index buffer, per-mesh
   bounds, material index. Serialized in the scene format next to the existing components.
2. `MeshComponent` in entt alongside the current renderer component; the existing analytic path
   keeps working, so scenes load either way.
3. Upload path: SSBO/vertex buffer per backend, reusing the existing buffer abstractions.
4. Raster the mesh with a plain depth prepass into the existing trace target, no shading.

**Gate A:** a single mesh renders at the correct position, scale and orientation from all four
backends, verified **by screenshot** against a reference, with the analytic scene still intact.
A clean build and passing tests are not evidence — every phase here changes what is drawn.

### Phase B — Visibility buffer and material resolve

1. 64-bit visibility buffer: `meshId | primitiveId`, plus barycentrics reconstructed in the
   resolve pass (or stored, if the bandwidth is acceptable — measure both).
2. Material resolve: fetch the triangle, interpolate attributes, evaluate the material.
3. Wire the existing shading/fog/lighting into the resolve so output matches the analytic path
   for a test scene that contains only analytic primitives.

**Gate B:** a scene rendered through the visibility buffer is visually indistinguishable from the
same scene rendered by the analytic tracer, at 4K, verified by screenshot and by a pixel-diff
with a stated tolerance. This is the parity gate; it is the most important gate in the plan.

### Phase C — GPU-driven culling and indirect draws

*Objective:* the CPU never decides what to draw.

1. Compute cull pass: frustum + Hi-Z occlusion (build the depth pyramid from the previous frame's
   depth — it does not exist yet).
2. Compacted indirect draw list via an atomic counter.
3. A/B against the existing CPU cull as the reference; the GPU result must match within an
   explicit tolerance before the CPU path is retired.

**Gate C:** measured draw count matches the CPU reference; frame time improves or holds at
identical visual output. Under contention, compare **within one run** (see §4).

### Phase D — Secondary rays

1. HW RT path: BLAS per mesh, TLAS over instances, shadow rays in a ray-tracing pipeline
   (Vulkan + D3D12).
2. Fallback path for D3D11/OpenGL: keep the analytic tracer for analytic primitives, and use
   screen-space shadows/AO for meshes, clearly labelled as a lower tier.
3. Gate the tier in the editor so the difference is visible and testable.

**Gate D:** shadow correctness verified against the analytic tracer on analytic geometry; on
meshes, verified by screenshot per backend tier.

### Phase E — LOD and large worlds (only if needed)

Cluster hierarchies, meshlet culling, LOD generation. **Do not build this before Phase C
measures what the cull actually rejects.** At the instance counts measured so far (16,041 in a
gameplay view) this may never be needed, and virtual-geometry cluster schemes carry patent
exposure that needs counsel review before engineering time is committed.

---

## 4. Measurement discipline (the GPU is not quiet)

Cross-run comparison is **invalid** on this machine. Established today:

- The same config produced 131.3 ms and 36.5 ms on different runs — 3.6x apart.
- The engine's own GPU timers were broken in three separate ways and reported ~1 ms against a
  146 ms frame. Fixed, but treat them as suspect until re-validated.

Rules that follow:

1. **Compare arms inside one run**, or interleaved with repeats. Never compare run N to run M.
2. **Use `WL_GL_FINISH=1`** for any GPU cost claim. `glFinish` cannot lie; the queries can.
3. **Noise floor first.** Five identical runs gave ±2% (app), ±3.3% (gpu), ±1.2% (finish). An
   effect smaller than that is not measured.
4. **Prefer instance counts and draw counts** over milliseconds when contended — they are
   exact.
5. **`WL_MAX_INSTANCES` / env overrides exist** for A/B without rebuilds. Extend that pattern
   rather than adding compile-time constants.
6. **Build `DemonCore-Editor/DemonCore-Editor.vcxproj`**, not `WastelandPython.vcxproj`. The
   latter rebuilds only `Wasteland.pyd` and leaves the exe stale, which silently measures old
   code. Use `/m:1 /nodeReuse:false`; kill stray MSBuild by PID if the PCH locks.

---

## 5. Constraints inherited from measurement

Things this plan must not re-litigate:

| finding | number | consequence |
|---|---|---|
| tracer is pixel-bound | ~160 ms at 4K | rasterize primary; do not trace it |
| shadow rays | 0.5 ms | not worth optimising; keep as rays |
| POM | ~0.0 ms | not a cost lever |
| surface shading | ~0.0 ms | not a cost lever |
| traversal ordering | 0.0 ms | do not optimise the analytic BVH further |
| instance cap 24,576 | 16,041 needed in gameplay views | the cap does not bind; do not raise it for play |
| DRS floors | 0.25/0.25/0.40/0.50 per preset → 38.6/37.1/20.7/15.1 fps | resolution is the only proven lever today |
| 10k scene renders correctly | verified by screenshot | the analytic path is sound; keep it as the parity reference |

---

## 6. Risks

| risk | impact | mitigation |
|---|---|---|
| **Parity gate fails** | raster path looks different from the tracer; no clean cutover | Phase B gate is the parity check; if it fails, ship as a mode, not a replacement |
| **Backend divergence** | D3D11/GL have no RT; features differ per backend | decide the backend tiering in §2 before Phase D, not during |
| **Visibility-buffer bandwidth** | 64-bit vis-buf + barycentrics at 4K may exceed budget | measure in Phase B; store or reconstruct barycentrics based on the number |
| **Binding budget** | shader silently fails to bind → black frame | `u12`/`t74` already taken; check the budget after every new SSBO |
| **GLSL/HLSL drift** | backends render differently | twins in the same commit; `verify_nova_glsl.py` in every gate |
| **Patent exposure** | virtual-geometry patents exist | counsel review before Phase E; clean-room avoids code copying, not patents |
| **Contended GPU** | every timing number is fiction | §4 discipline; in-run A/B only |

---

## 7. Open questions

1. **Backend tiering** — is dropping D3D11 and OpenGL acceptable, or must they ship with a
   degraded secondary-ray tier? This decides whether Phase D has one path or three.
2. **Mesh source** — are assets authored as glTF/FBX, or generated procedurally? This decides the
   importer and whether a DCC round-trip is in scope.
3. **Materials** — the current system is analytic (metallic/roughness floats, no textures per
   primitive beyond POM). A triangle pipeline needs a real material model with texture binding.
   Is that in scope for this plan or a separate one?
4. **Does the analytic path stay?** It is the parity reference and it works. Keeping it means two
   renderers to maintain; removing it means losing the reference. Recommend: keep until Phase D
   passes its gate, then decide.

---

## 8. Recommended first increment

**Phase A only.** One mesh, four backends, verified by screenshot, with the analytic scene still
intact.

It is the smallest change that proves the raster path can coexist with the tracer, it needs no
new bindings and no RT, and it produces the one thing this project has been missing all along: a
second renderer to compare against. Everything after it is a bet on that comparison being
clean.
