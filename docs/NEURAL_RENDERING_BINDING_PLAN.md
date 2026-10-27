# Neural Rendering binding plan — registers, pass IDs, and the three traps

Measured 2026-10-23 from source, not from a report. Every number below is the
answer to a `grep` run against this tree; the verifier that enforces it is
`tools/verify_nova_binding_budget.py` (extended in task 1 of
`.hermes/plans/2026-10-23_011500-clean-room-neural-rendering.md`).

## 0. Corrections to the task-1 draft

Three assumptions in the draft allocation were wrong. They are recorded here
because each one would have cost a debugging session:

| Draft said | Actual | Why it matters |
|---|---|---|
| "`u8` is a deliberate hole… `u9` is taken by moments" | `u8` is **not declared by the shader at all**. `u9` = `img_Moments`, `u10` = ReSTIR reservoir write, `u11` = `img_HistoryBuffer`. | The hole is an *absence*, not a reservation. Nothing enforces that `u8` stays empty. |
| "new UAV therefore needs `u12`+ on Vulkan" | `u12`/`u13` are free **and** `u8` is free. | A new pass may legitimately use `u8`. Rejecting it outright would have forced a worse layout. |
| "weights SSBO needs a `t` register ≥ t74" | The `t` range is **not** free above t4: `t5`..`t14` are unclaimed, and `t48`..`t53` / `t64`..`t73` are the snapshot blocks. | A register can be claimed by more than one declaration; the binding map is what decides. |

## 1. The binding map (HLSL registers -> Vulkan bindings)

Vulkan binding = `offset + slot`, per `vulkan-resource-bindings.cpp:100` and
mirrored in `tools/compile_nova_spirv.py:55-58`:

```
u -> +0     b -> +7     t -> +16     s -> +200
```

| Binding | Held by | Notes |
|---|---|---|
| 0..7 | `u0`..`u7` | all eight UAVs |
| **8** | **`b1`** | `cbuffer NovaConstants`. **The one collision.** |
| 9 | `u9` | `img_Moments` |
| 10 | `u10` | `RWStructuredBuffer<LightReservoir>` (ReSTIR write) |
| 11 | `u11` | `img_HistoryBuffer` |
| 12, 13 | — | **FREE** |
| 14, 15 | — | FREE |
| 16 | `t1` | `Instances` |
| 17 | `t2` | `BVHNodes` |
| 18 | `t3` | `LightIndices` |
| 19 | `t4` | `TerrainVertices` |
| 20..25 | — | FREE |
| 26 | `t10` | `ReservoirRead` |
| 27..30 | — | FREE |
| 31 | `t15` | `u_SceneTextures[32]` — **32 textures, t15..t46** |
| 32..63 | — | FREE (inside the scene-texture range) |
| 64..69 | `t48`..`t53` | sampled snapshots |
| 70..79 | — | FREE |
| 80..87 | `t64`..`t71` | readable snapshots |
| 88 | `t72` | **never declared** — padding |
| 89 | `t73` | `read_img_Moments` |
| 90, 91, 92 | — | **FREE** |
| 200 | `s0` | sampler |

### 1.1 The `t15` array is the trap inside the trap

`u_SceneTextures[32] : register(t15)` claims bindings 31..62 as a *contiguous
range*. A new `t`-register inside 15..46 would be absorbed into that array and
would not bind the intended buffer. **New `t` registers must be < 15 or > 73.**

## 2. Allocation for the neural rendering stage

| Resource | Register | Vulkan binding | Why this slot |
|---|---|---|---|
| Neural feature output UAV | `u8` | 8 | **Wrong — see §2.1.** |
| Neural feature output UAV | **`u12`** | **12** | free; above every declared `u` |
| Neural history UAV | **`u13`** | **13** | free |
| Neural weights SSBO | **`t74`** | **90** | free; **above the t64..t73 snapshot block** |
| Feature read SRV | **`t75`** | **91** | free |
| History read SRV | **`t76`** | **92** | free |
| Pass IDs | **17, 18, 19** | — | see §3 |

### 2.1 Why `u8` must not be used even though it is free

`u8` maps to Vulkan binding 8, which is held by `cbuffer NovaConstants : register(b1)`.
The plan's draft treated `u8` as "the deliberate hole" and told task 4 to use
`u12`+ — which is the right answer reached for the wrong reason. The reason
`u8` is unusable is the **b1 collision**, not the hole. Stated correctly, a new
UAV may use `u8` **only if** `NovaConstants` moves off `b1` at the same time.
It does not move in this plan, so `u12` is correct.

### 2.2 Why the weights go in a **constant buffer**, not an SSBO

The stage's weights are ~1.6k fp32 = ~6.4 KB, and they are read by every thread
of every dispatch. Two options were considered:

- **SSBO at `t74`.** Works. Costs a descriptor-table entry and a wider `t`
  range on DX12 (the `t64..t73` block becomes `t64..t76`, so
  `kImageSrvBase`/`kUavBase` shift again — `DX12Nova.h:165-192` documents that
  every such shift must move the block bases together).
- **Constant buffer.** The weights are appended to `NovaConstants` itself
  (offset ≥ 1616, size 1744 today). No new register, no descriptor-table change,
  no DX12 block shift.

**Decision: constant buffer, appended to `NovaConstants`.** The trade-off is
that the constant-buffer upload path must grow and `kConstantBufferSize` must
be re-derived — `tools/compile_nova_spirv.py` asserts the block size against
DXC, so it will fail loudly rather than silently. That is the acceptable cost.

Consequence for the table above: **`t74`, `t75`, `t76` are NOT consumed by
weights.** They remain free for the feature/history read SRVs (`t75`, `t76`)
and one spare. The verifier enforces that a declared-but-unused `t` register in
the snapshot range would fail `VerifySpirvBindings()` on Vulkan
(`VulkanNova.cpp:40-43`).

## 3. Pass IDs

| ID | Pass | Status today |
|---|---|---|
| 0, 1, 2 | visibility+velocity, trace, temporal accum | **dispatched** |
| 3, 11, 14 | ReSTIR generate / combine / spatial | dispatched (when `RestirEnabled`) |
| 4, 5 | bloom blur H/V | **dispatched** |
| 6 | composite | **dispatched** |
| 7 | bilateral blur | dispatched |
| 8 | `RunUpscale` | **never dispatched** |
| 9 | `GenerateMaterialMaps` | dispatched (explicit call site) |
| 10 | god rays | dispatched |
| 12, 13 | temporal moments / A-Trous | dispatched |
| 15, 16 | `RunTSRResolve` / `RunTSRCopy` | **never dispatched** |
| **17, 18, 19** | **neural feature / eval / stability** | **new** |

`case 17/18/19` must be added to **both** `main()` switches
(`.glsl:5529`, `.hlsl:3910`) and dispatched from `RunNovaPassSchedule`
(`Renderer3D.cpp:3306`) after the god-rays block.

**Do not reuse 8, 15 or 16.** They are reserved for the upscale/TSR passes that
are written and waiting; taking one would silently break the day that path
lands. The `u_PassID` comment at `.glsl:4985-4995` already documents that an
undispatched `u_PassID` value is a silent no-op costing a full group's
execution.

## 4. What each backend needs when task 4 lands

**Vulkan** — `kImageCount` 9 → 11 and `kImageUavSlot[]` gains 12, 13;
`kReadImageCount`/`kReadImageSlot`/`kReadImageIndex` gain t75/t76;
`CreateImages` gains two formats; `CreateBindingSet` gains two UAV items and
two SRV items. `VerifySpirvBindings()` will fail the pipeline if a declared
binding is unused or a used one is missing, so the SPIR-V must be regenerated
(`tools/compile_nova_spirv.py`) **before** the pipeline is created.

**DX12** — `kUavCount` 12 → 14; `kDescriptorsPerHeap` follows. The
`kSamplerBase`/`kImageSrvBase`/`kUavBase` block bases must move **together**;
`DX12Nova.h:165-192` is the record of the last time this was done and is the
template.

**DX11** — `std::array<Image,8> images` at `DX11Nova.h:69` becomes 10; the
`uavs[11]` local at `:525` becomes 13; the `srvs[128]` table gains the new
`t` slots. **The 8 is a construction convention, not a device limit** —
`Dispatch` rebuilds the table per pass and `Unbind()` clears it
(`docs/NOVA_BINDING_BUDGET.md`, DX11 section).

**OpenGL** — image unit **8 is already used** by an unrelated generator
(`Renderer3D.cpp:6632`, `glBindImageTexture(8, s_Data.GeneratedTextureID, ...)`).
Units 0,1,2,4,5,6,7 are bound by the Nova path (`:4460-4466`, `:4793`). The new
images therefore need units **9 and 10**, and the shader's comment at
`.glsl:66-68` ("keeps this compute program within OpenGL's guaranteed eight
image uniforms") becomes wrong — it must be updated, not worked around.

Note the two namespaces: `glBindImageTexture` units and `glActiveTexture` units
are **separate** (`glActiveTexture(GL_TEXTURE0+3)` does not conflict with
`glBindImageTexture(3, ...)`). A previous survey confused them; the verifier
must not.

## 5. Verifier requirements (task 1)

`tools/verify_nova_binding_budget.py` must, after task 1:

1. Parse the HLSL registers and assert no two declarations share a Vulkan
   binding (the existing `declared_bindings()` collision check, generalised to
   *any* kind, not just `u8`).
2. Assert the `t15` array range (31..62) is not claimed by any other `t`.
3. Assert `u8` is not claimed by a UAV while `b1` exists — the specific trap,
   now generalised.
4. Assert the reserved neural slots (`u12`, `u13`, `t74`..`t76`, passes
   17/18/19) are either unclaimed or claimed by the intended resource.
5. Assert the GLSL image-unit allocation (`0,1,2,4,5,6,7` + new `9,10`) and
   flag unit 8 as belonging to the generator, not to Nova.
6. `--self-test` (existing) plus **two** new self-test modes:
   - inject a second declaration of an already-claimed binding and assert FAIL;
   - inject a `t` register inside the `t15..t46` range and assert FAIL.

A verifier that only catches one collision shape is the failure mode the file's
own docstring warns about (lines 21-28).

## 6. Open items

- **Is `img_Velocity` readable by pass 19?** It is an `rg16f` UAV with no
  sampler twin. Reading it needs either a new sampled snapshot (`t54`, free) or
  the feature pass can copy velocity into the feature texture. Decide in task 4;
  do not assume either works.
- **Constant-buffer size.** `NovaConstants` is 1744 bytes with 84 members
  (`VulkanNova.cpp:45-132`). Appending weights changes the size and the offsets
  of nothing (appended at the end), but `kConstantBufferSize` and the DXC
  assertion must both be updated in the same commit.
- **`u_SceneTextures` on DX12 starts at descriptor 4**, not 0 — indices 1..3 are
  unused padding (`DX12Nova.h:172`). Any new scene texture must respect that.
- **GL image-unit budget.** The new stage needs two more image units, and unit 8
  is already the generator's (`Renderer3D.cpp:6632`), so units **9 and 10**. The
  shader comment at `.glsl:66-68` ("keeps this compute program within OpenGL's
  guaranteed eight image uniforms") is already stale — it counts *declarations*,
  and the GLSL already declares 8 while the engine binds 8 units. Update the
  comment; do not work around it.
- **`t72` is declared nowhere and `t67` is declaration-only.** Both are padding
  in the readable-snapshot block (`VulkanNova.cpp:36-43`). If a new read SRV is
  needed, prefer reusing `t67` over adding `t72`, because `t67`'s absence from
  the SPIR-V is already handled.

## 7. Pre-existing verifier failures found while writing this plan

Both reproduce against `git HEAD` with the task-1 edit stashed, and both are
recorded here so the next person does not chase them as a regression. **The
second is a real cross-backend bug, not a verifier artifact — fix it.**

- **`tools/verify_texture_sampler_indexing.py` FAILS**: "expected 9 image
  declarations, found 8". Measured: the GLSL has **8** `uniform image2D`
  declarations (`img_Output` b0, `img_Accumulation` b1, `img_Bloom` b2,
  `img_Bloom_Temp` b7, `img_Velocity` b5, `img_TraceGBuffer` b6,
  `img_MaterialPacked` b3, `img_AlbedoHit` b4). **This was already fixed in
  commit `d4d2c09`** (the tree was 54-modified when this plan was written; the
  user committed during the session and the expectation is now 8). If it fails
  again, re-measure rather than assuming.

- **`tools/verify_nova_shader_parity.py` FAILS** — and this one is a genuine
  defect, confirmed at `HEAD` (`d4d2c09`) in committed code:

  ```
  ComputeDirectLighting:
    GLSL:  int maxLightsCap = (u_QualityLevel == 0) ? 1 :
                              ((u_QualityLevel == 1) ? 2 :
                              ((u_QualityLevel == 2) ? 4 : u_MaxLights));
    HLSL:  int maxLightsCap = (u_QualityLevel == 0) ? 1 :
                              ((u_QualityLevel == 1) ? 2 :
                              ((u_QualityLevel == 2) ? 4 : 32));
  ```

  The GLSL reads `u_MaxLights`; the HLSL uses the literal `32`. Measured:
  - `u_MaxLights` is declared in the HLSL cbuffer and **read zero times** — it
    is a dead uniform there, exactly as the GLSL's own comment at
    `Renderer3D_NovaRenderer.glsl` warns (`u_MaxBounces`/`u_SamplesPerPixel`
    are the other two dead ones).
  - `ApplyQualityPreset` sets `s_Data.MaxLights` to **1 / 2 / 4 / 4** for
    Low / Medium / High / Ultra (`Renderer3D.cpp:5060+`).

  **Consequence: the two backends evaluate a different number of lights per
  pixel at Low, Medium and High.** The HLSL caps the light budget at 32 where
  the GLSL honours the preset's 1/2/4 — so a DX11/DX12/Vulkan frame at Medium
  lights more sources than the OpenGL frame of the same scene, preset and
  camera. At Ultra both reach 4, so the divergence is invisible on the preset
  people actually test.

  This is the exact failure class `wasteland-large-world-rendering` describes:
  *"a loop bound that differs … is up to 1024 vs 256 traversals per pixel — a 4x
  cost difference AND a different image on the same scene, preset and frame."*

  **Fix:** make the HLSL read `u_MaxLights`, matching the GLSL. Then confirm
  the parity verifier passes and the preset's light budget is honoured on all
  four backends. Note the fix must not be made by deleting the GLSL's read —
  that would silently disable the preset's light-budget control on OpenGL.

Both must be resolved before task 4 adds passes, because a shader edit cannot
be verified against a verifier that is already red.
