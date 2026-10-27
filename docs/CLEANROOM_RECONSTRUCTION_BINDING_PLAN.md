# Clean-Room Reconstruction Stack — Binding Plan

**Status:** Task 1 complete. Registers and pass IDs reserved. No shader or renderer
change landed — this document records the reservation so the tasks that follow
cannot collide.

**Source of truth:** `.hermes/plans/2026-10-26_001250-clean-room-dlss-class-stack.md`
(§1.2, §1.3) is the plan; this file is the reservation table it calls for, and
`tools/verify_nova_binding_budget.py` is the executable form of it. The three must
not drift: the doc is for humans, the verifier is for CI, the plan is why.

---

## 1. The constraint that shapes everything

`GL_MAX_IMAGE_UNITS = 8` on the target GPU (RTX 3060 Ti, 4.5.0 NVIDIA 591.59),
measured and logged at `OpenGLContext.cpp:151-153`.

**The limit is a DECLARATION COUNT, not a binding index.** This distinction was
the first thing to get wrong, and it changes the design:

* `verify_nova_glsl.py:213` asserts `len(image_uniforms) <= 8`.
* The script's own negative control injects a ninth uniform **at a legal binding
  index** (`binding = 7`) and expects rejection.
* The driver error is **C7624**, from a ninth *declaration* becoming active.

So a **rebind to an occupied unit is legal** — and is already the engine's own
pattern:

> `Renderer3D.cpp:5225,5238` rebinds image unit 6 from `img_TraceGBuffer` to the
> moments texture and back, inside one frame's SVGF dispatches. The guard is the
> comment "the two textures are not used together in these SVGF dispatches".

**Consequence:** the reconstruction stack declares **no ninth image uniform**. It
borrows an occupied unit per dispatch. The three fallbacks the draft plan
considered (`shared`-memory-only WLRR head, analytic-only WLFG) are **withdrawn** —
they were a response to a constraint that does not exist in this form.

---

## 2. The reservation

| Resource | Register | GL unit | Displaces | Notes |
|---|---|---|---|---|
| WLSR input | — | 7 (sampler) | `img_Bloom_Temp` | read-only during the dispatch |
| WLSR output | — | 0 | `img_Output` | same read/write pair as the dead pass 8 |
| WLRR scratch UAV | `u13` | 7 (image write) | `img_Bloom_Temp` | post-composite; texture is dead |
| WLRR scratch SRV | `t75` | — | free | `t74` is NeuralWeights |
| WLFG previous-present UAV | `u13` (shared) | 7 (image write) | `img_Bloom_Temp` | never used together with WLRR |
| WLFG warp output | — | 0 | `img_Output` | |
| Reactive mask | — | 7, **channel a** | `img_Bloom_Temp` | see §4 |

**`u13` and `t75` are shared between WLRR and WLFG.** They are never dispatched
together (WLRR runs pre-composite, WLFG on the interpolation present only), so one
scratch texture serves both. The alternative — two textures — costs a GL image
unit the path does not have.

**Why unit 7 and not a free unit:** there are none free. The GL units bound by
`glBindImageTexture` in `Renderer3D.cpp` are `0,1,2,3,4,5,6,7,8`; unit 8 belongs
to an unrelated generator (`Renderer3D.cpp:6632`), not Nova. So 7 is the only
honest choice for a post-composite borrow.

---

## 3. Pass IDs

| Pass | Name | Status | Notes |
|---|---|---|---|
| 8 | `RunWlsrEasu` | **revive + rewrite** | replaces `RunUpscale` |
| 15 | `RunWlsrRcas` | **revive + rewrite** | replaces `RunTSRResolve` — see §5, this is *wire up*, not revive |
| 16 | `RunWlaaSharpen` | **revive + rewrite** | replaces `RunTSRCopy` |
| 20 | `RunWlrrDenoise` | new | |
| 21 | `RunWlrrComposite` | new | |
| 22 | `RunWlfgWarp` | new | |
| 23 | `RunWlfgBlend` | new | |
| 24 | `RunReactiveMask` | new | |

**Repurposing 8/15/16 is deliberate.** They have `case N:` arms in both twins and
no dispatch site — measured by grepping `pass(N)` in `Renderer3D.cpp`, which
yields `0..7, 10..14` only. Leaving three dead cases in the switch while adding
four more new ones makes the switch unreadable, and their names are close enough
to the new features' purpose that a future reader would reasonably assume the
wrong thing.

**`tools/verify_nova_binding_budget.py` enforces this.** `check_pass_ids()` fails
if a *dispatched* pass ID is still in `DEAD_PASSES`, and fails on any pass ID
above 19 that is not in `RECONSTRUCTION_PASSES`. Task 3 (which claims pass 8)
must remove 8 from `DEAD_PASSES` in the same change.

**Do not confuse these with `SampleSceneTexture`'s cases.** The shader's second
switch cases over `u_SceneTextures[0..31]` (`.glsl:570-601`). A whole-file
`case N:` scan reports 32 phantom pass IDs; the verifier scopes to arms whose
body calls a `Run*` function.

---

## 4. The reactive mask's channel

The mask (pass 24) is stored in `img_Bloom_Temp`'s **alpha** channel rather than
in a new image uniform.

**The claim, stated correctly:** the alpha is **written but never read**.

* Pass 7 (`RunBilateralBlur`) writes `vec4(finalFiltered, 1.0f)` at `.glsl:4504`
  and `.glsl:5137` — rgb *and* alpha = 1.0.
* A grep for `imageLoad(...).a` across the entire shader returns **nothing**.

The draft plan said "pass 7 writes rgb only". That is false. Both statements are
true in a sense, but only one is the load-bearing one: a future change that
*writes* the alpha again is harmless (the mask overwrites it), while one that
*reads* it is a silent no-op.

`tools/verify_reactive_mask_budget.py` encodes the correct direction. It checks
that the mask's channel is written by its pass, read only by its declared
consumers, and declared in both twins.

---

## 5. Findings that change later tasks

### 5.1 Passes 15/16 are a complete, undispatched TSR resolve

`RunTSRResolve` (`.glsl:5690`) is not an empty shell. It reads `img_Output`,
reprojects with motion vectors, samples a history buffer, does 3x3 YCoCg
neighbourhood clamping, blends and sharpens, then writes `img_Accumulation` and
the history. `RunTSRCopy` (`.glsl:5694`) copies `img_Accumulation` to
`img_Output` to avoid a read-write race.

The shader comment at `.glsl:5695-5698` states the requirement:

> "The history buffer reuses `img_Bloom_Temp` (binding 7), which is free after the
> upscale pass. **The C++ side must bind the history texture to binding 7 for the
> TSR dispatch** and clear it on camera cuts and frame 0."

**That CPU-side rebind does not exist.** A grep for `binding = 7` /
`kHistorySlot` / `HistoryTexture` in `Renderer3D.cpp` returns nothing.

Dispatching pass 15 today would read a history buffer that nothing writes:
garbage, silently. **Task 5 is therefore *wire up*, not *revive*** — it must add
the binding and verify the buffer is cleared on camera cuts, or WLAA will "work"
while showing ghosts from frame 0.

### 5.2 WLAA is closer to existing than the plan claimed

Because §5.1, there is already a history-reproject-clamp-blend-sharpen chain in
the tree. The missing piece is the history *buffer*, not the algorithm. Task 5's
honest deliverable may be smaller than planned — and that is a finding to report,
not a gap to paper over.

### 5.3 The DX12 UAV guard already exists, in prose

`DX12Nova.h:162-176` documents the exact hazard the new
`check_dx12_uav_count()` encodes:

> "This constant MUST be >= (highest shader UAV index + 1) … a shader that gains
> a UAV without bumping this fails CreateComputePipelineState with E_INVALIDARG
> (0x80070057) during Init — the backend then silently falls back to raster and
> logs 'DX12 Nova init failed'. That is exactly what happened when
> img_HistoryBuffer moved to u11 while this stayed 11."

The verifier is the executable form of that comment. Adding `u13` requires
`kUavCount` 13 → 14 **and** the readable-SRV block to grow to reach `t75`; the
header's own "Heap layout" comment records the last such move (52 → 53) and is
the template.

---

## 6. Verification

```bash
python3 tools/verify_nova_binding_budget.py
python3 tools/verify_nova_binding_budget.py --self-test
python3 tools/verify_nova_binding_budget.py --self-test-dup
python3 tools/verify_nova_binding_budget.py --self-test-scenetex
python3 tools/verify_nova_binding_budget.py --self-test-wlsr
python3 tools/verify_reactive_mask_budget.py
python3 tools/verify_reactive_mask_budget.py --self-test
```

The `--self-test-wlsr` mode injects eight hazard shapes and asserts each is
detected: duplicate UAV slot, t-register inside `u_SceneTextures`, dispatched
pass still listed dead, unreserved pass ID, `SampleSceneTexture` phantom
arm, DX12 `kUavCount` too low, GL borrow naming an undeclared unit, and a ninth
image declaration.

`--self-test` on the reactive-mask verifier injects five: stray read of the mask
channel, undispatched writer, pass arm missing from one twin, writer storing to
the wrong image, and an undispatched consumer.

**A verifier that cannot fail is a comment, not a guard.** Both self-test modes
exist for that reason.

---

## 7. Backend status — stated honestly

| Backend | Status |
|---|---|
| **OpenGL** | the only path that actually renders on this machine; the verification target |
| **DX11** | HLSL twin + `DX11Nova.h`; contract-verified |
| **DX12** | hangs during init when forced on this machine; contract-verified only |
| **Vulkan** | `VulkanNova` compiled but never instantiated; the `.spv` is built from the **HLSL**, not the GLSL |

No Vulkan claim in this stack may be a rendering claim. Verify by contract:
`tools/verify_vulkan_nova_contract.py`,
`tools/verify_vulkan_reservoir_allocation.py`, `tools/compile_nova_spirv.py --check`.
