# Binding budget for a §5.2 SVGF moments target — measured 2026-10-05

Settles the question the options map left open ("does a 9th UAV fit on DX11?").
Answer per backend, from source, not from the D3D11 spec table.

## DX12 — HAS HEADROOM

`DX12Nova.h:92`  `static constexpr UINT kUavCount = 9;`
`DX12Nova.h:106` `static constexpr UINT kUavBase = 49;`
`DX12Nova.h:107` `kDescriptorsPerHeap = kUavBase + kUavCount`  (= 58)

The 9 UAV descriptors already exist. They are consumed through the descriptor
tables at `DX12Nova.cpp:255,258,261,272` (`ranges[0..3]` -> rootParams 4..7),
NOT through the root signature directly. `CD3DX12_ROOT_PARAMETER1 rootParams[8]`
(`DX12Nova.cpp:234`) is exactly full, but adding a UAV does **not** require a 9th
root parameter — it requires one more descriptor in `ranges[]`, which is a
one-line change inside an existing table.

So the options map's "rootParams[8] exactly full, a hard constraint" is
misleading: it constrains root *parameters*, not UAV *descriptors*.

## DX11 — still capped, but the ceiling is the ARRAY, not the API

`DX11Nova.h:66`  `std::array<Image,8> images;`
`DX11Nova.h:477` `ID3D11UnorderedAccessView* uavs[8];for(i<8) uavs[i]=images[i].uav.Get();
                   context->CSSetUnorderedAccessViews(0,8,uavs,nullptr);`
`DX11Nova.h:371` `Unbind()` clears 8 UAVs + 128 SRVs.

HLSL occupies u0..u7 (`Renderer3D_NovaRenderer.hlsl:87-99`):
  u0 img_Output  u1 img_Accumulation  u2 img_Bloom  u3 img_MaterialPacked
  u4 img_AlbedoHit  u5 img_Velocity  u6 img_TraceGBuffer  u7 img_Bloom_Temp

### Dead code does NOT free a slot

Pass 8 (`RunUpscale`, `.hlsl:4194`) is declared in both twins and dispatched
NOWHERE (`grep -rn "Dispatch(8"` over `Wasteland/src` returns nothing). It is
tempting to reuse u7 for moments on the grounds that the pass is dead.

**That is wrong.** `img_Bloom_Temp` (u7) is live:
  written by pass 7 `RunBilateralBlur` at `.hlsl:4108`
  read  by `RunComposite` at `.hlsl:4164-4167`
The PASS is dead; the RESOURCE is not. Verified, not assumed.

### Why DX11 is not actually blocked

`uavs[8]` at `:477` is a **local array rebuilt inside `Dispatch`**, and
`Dispatch` runs once per pass, with `Unbind()` clearing the table afterwards
(`:371`, called at `:489`). D3D11 binds UAVs per-dispatch, so **slot meaning is
already per-pass rebindable** — the 8 is a local-construction convention, not a
device limit.

Consequence: a moments pass that reads via SRV and writes into a slot whose
resource is not live *for that pass* needs no new UAV at all. The real DX11
constraint is "which of u0..u7 are unused by the moments pass", not "8 is the
ceiling".

### SPEC GUARANTEE (context, not a measurement)

8 UAVs at feature level 11_0; 64 at 11_1. `NVRHIContext.cpp:350` requests 11_1
first, so 64 is *likely* here — but that is a promise about the device, and this
repo's `uavs[8]` literal would still need widening. A subagent was dispatched to
measure the real number on this GPU; treat 8 as the guaranteed floor until that
returns.

## Vulkan — DO NOT add a binding here yet

`VulkanNova.cpp` shifts: uav u=0, cbuffer b=7, srv t=16, sampler s=200.
So `register(u8)` -> binding 8, which **collides with `cbuffer NovaConstants :
register(b1)`** (b-shift 7). `u9` -> binding 9 is also occupied.

`tools/verify_nova_binding_budget.py --strict` is the gate. It reports `HAZARD`
for u8/u9 and exits 1 under --strict. Do not hand-edit a binding without it.

## MEASURED on this machine — DX11 is NOT capped at 8

`tools/probe_dx_uav_slots.cpp` (build+run it; the subagent that wrote it left the
sources in `tools/`) measures the real number rather than quoting the spec table:

| profile | result |
|---|---|
| `cs_5_0` (the profile this engine uses, `DX11Nova.h:191`) | **64 UAVs** bound, dispatched, sentinel-read back |
| `cs_5_1` | **0** — `CreateComputeShader` returns `E_INVALIDARG` even at n=1 |

D3D11 exposes **no API** for the compute UAV count: `D3D11_SDK_VERSION` is 7, the
feature enum ends at `D3D11_FEATURE_D3D11_OPTIONS6` (a 4-byte payload), and
`D3D11_UAV_SLOT_COUNT` does not exist in the SDK — only `D3D11_1_UAV_SLOT_COUNT = 64`.
`CSSetUnorderedAccessViews` also returns `void`, so sentinel readback is the only way
to prove a slot was really bound. The probe carries a **negative control** (bind 64,
write only u0, assert the rest stay poisoned) so the 64 result cannot pass vacuously.

Device: RTX 3060 Ti, feature level **11_1**. This is machine-specific, NOT a
portable guarantee — 8 remains the spec floor at FL 11_0.

### Per-pass UAV rebinding: YES

UAV bindings are context state; nothing associates a resource with a slot
permanently. The probe proves it: set A bound → dispatch → set B bound to the *same
eight slots* → dispatch, and set B read back written while set A stayed untouched.

**So a moments target does NOT need slot reuse or an SRV workaround on DX11.**
Add `u8`, widen `uavs[8]`→`[9]` at **both** `DX11Nova.h:371` (`Unbind`, else a stale
binding leaks into the next pass) and `:477`, and declare `register(u8)` in the HLSL.

Caveat: do **not** "fix" anything by switching to `cs_5_1` — it is unusable here.

## Decision

Fold the packed moments (mean, mean-of-squares) into the existing per-pass
binding rather than adding a global 9th UAV:
- DX12: one more descriptor in `ranges[]` — cheap.
- DX11: reuse a slot that is dead *for that pass* — no ceiling problem.
- Vulkan: needs a real binding change; blocked pending the descriptor work.
- GL: image unit 3 is free (`Renderer3D.cpp` binds 0,1,2,4,5,6,7).

This is Option B from the options map §5.2, which packs moments into ONE RG32F
target to stay inside these limits.
