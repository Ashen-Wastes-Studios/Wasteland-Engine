# RTX Kit license constraints

Read this before adding any NVIDIA RTX Kit component to Wasteland.

**Short version: nothing in the RTX Kit can legally go into an open-source Wasteland.** Not one component. This is not a build problem or a technical blocker — it is a licensing wall, and it is the single most consequential finding in the whole RTX Kit effort.

This matters because the rest of the plan is genuinely actionable: OMM compiles against our NVRHI today, RTXMU is one CMake flag, RTXDI has zero graphics-API coupling. All of that is true and all of it is unusable if the project is open source.

## The clause that decides everything

`NVIDIA RTX SDKs LICENSE` (governs NRD, RTXTF, RTXDI, OMM, RTXMU, RTXGI, RTXPT, and the rest) §4(e):

> "You may not use the SDK in any manner that would cause it to become subject to an open source software license. As examples, licenses that require as a condition of use, modification, and/or distribution that the SDK be: (i) disclosed or distributed in source code form; (ii) licensed for the purpose of making derivative works; or (iii) redistributable at no charge."

Every clause in that list describes an OSI-style license. MIT, Apache-2.0, GPL, BSD — all of them trip §4(e). §4(b) separately forbids sublicensing and distributing the SDK as a standalone product. §7.b requires derivative works to carry the notice *"This software contains source code provided by NVIDIA Corporation."*

## Per-component status

| Component | License | Open-source OK? |
|---|---|---|
| **STBN** | NVIDIA Source Code License for STBN generation — §3.3: *"only may be used or intended for use non-commercially … for research or evaluation purposes only"* | **NO.** Worse than proprietary: non-commercial means Necroware cannot ship it either. |
| **NRD** | NVIDIA RTX SDKs LICENSE — §4(e) present | **NO** |
| **RTXTF** | NVIDIA RTX SDKs LICENSE — §4(e) present | **NO** |
| **OMM** | NVIDIA RTX SDKs LICENSE — §4(e) present | **NO** |
| **RTXMU** | **MIT** (verified in-tree) | **YES** — genuinely OSI-clean |
| **Donut** | MIT (NVIDIA GameWorks) | **YES** |
| **Streamline** | Mixed; NGX headers `LicenseRef-NvidiaProprietary` | **NO** |

## Already committed — needs a decision

| Commit | Component | Status |
|---|---|---|
| `dcafd98` | NRD v4.9.4 as a submodule + `NRDDenoise.cpp` + premake link | Landed under an assumption this file invalidates |
| `8e8d7e1` | STBN volume + `BlueNoise.cpp` wired into `GetCurrentJitter()` | Same. **Also non-commercial.** |
| `0d20ab4` | Plan feasibility audit | Fine — documentation only |

The STBN one is the sharper problem. A submodule pin is arguably not distribution, but the `.r8` volume is a committed derivative of NVIDIA's work, and the engine *calls into it at runtime*.

## The way out that keeps the engineering value

Every algorithm here is published. The license restricts redistribution of NVIDIA's *implementation*, not use of the *idea*.

- **Blue noise** — the algorithmic generator is described in the STBN paper (Schied et al., *Spatiotemporal Variance-Guided Filtering*, JCGT 2018). `tools/extract_stbn.py` decodes NVIDIA's PNGs. Rewriting it to *generate* blue noise instead of *vendoring* it is a bounded, well-understood task and removes the license entirely.
- **Denoising** — already implemented natively: `Renderer3D`'s bilateral pass. NRD was to replace it, but the fallback was always the requirement.
- **ReSTIR / DDGI** — these are published algorithms with open implementations. Implementing them directly against NVRHI is the Phase 5 plan's existing strategy and carries no license at all.

That path costs more effort than vendoring. It is the only one that leaves Wasteland open source.

## Decision needed

1. **Keep Wasteland open source** → remove NRD and STBN (`dcafd98`, `8e8d7e1`) and implement the techniques natively. RTXTF/OMM/RTXDI stay off the table. RTXMU via `NVRHI_WITH_RTXMU=ON` stays, since RTXMU is MIT.
2. **Go proprietary** → keep everything, and the whole kit opens up in the order the plan already has.
3. **Split** → open-source core engine, proprietary binary/plugin layer. Realistic and common in the industry, but it is an architectural commitment.

I have not removed anything yet. The vendored code compiles and is committed, so nothing is lost while this is open — but nothing further should be vendored until you pick.
