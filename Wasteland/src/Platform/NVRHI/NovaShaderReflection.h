// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Produced by tools/gen_nova_cb_layout.py from the DXIL blob
//   DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.cs.cso
//
// This table replaces the D3DReflect() call that used to live in
// DX12Nova::Init(). D3DReflect reads DXBC (SM5.x) and returns E_INVALIDARG
// on the SM6 DXIL container Slang emits, so the layout is extracted at
// shader-build time instead of at engine runtime.
//
// Variable size is the span to the next member (last member: to the end of
// the cbuffer). Padding makes a span larger than the member, never smaller,
// so it is a safe upper bound for DX12Nova::Set()'s bounds check.
//
// Regenerate with: python3 tools/gen_nova_cb_layout.py
// Verify with:     python3 tools/gen_nova_cb_layout.py --check

#pragma once

namespace Wasteland {
namespace NovaReflection {

struct CbVariable
{
    const char *Name;
    unsigned int Offset;
    unsigned int Size;
};

inline constexpr unsigned int kCbSize = 4496u;
inline constexpr unsigned int kCbVariableCount = 101u;

inline constexpr CbVariable kCbVariables[kCbVariableCount] = {
    {"u_CameraPosition", 0u, 16u},
    {"u_InverseViewProjection", 16u, 64u},
    {"u_PrevViewProjection", 80u, 64u},
    {"u_InstanceCount", 144u, 4u},
    {"u_FrameIndex", 148u, 4u},
    {"u_SamplesPerPixel", 152u, 4u},
    {"u_PassID", 156u, 4u},
    {"u_DepthBuffer", 160u, 4u},
    {"u_Jitter", 164u, 12u},
    {"u_PrevJitter", 176u, 8u},
    {"u_CameraMoved", 184u, 8u},
    {"u_ViewProjection", 192u, 64u},
    {"u_SkyBottomColor", 256u, 16u},
    {"u_SkyTopColor", 272u, 12u},
    {"u_AccumulationAlpha", 284u, 4u},
    {"u_NormalStrength", 288u, 4u},
    {"u_RoughnessBias", 292u, 4u},
    {"u_AOIntensity", 296u, 4u},
    {"u_QualityLevel", 300u, 4u},
    {"u_BloomEnabled", 304u, 4u},
    {"u_BloomIntensity", 308u, 4u},
    {"u_BloomThreshold", 312u, 4u},
    {"u_VignetteEnabled", 316u, 4u},
    {"u_VignetteIntensity", 320u, 4u},
    {"u_VignetteRadius", 324u, 4u},
    {"u_VignetteSoftness", 328u, 4u},
    {"u_ChromaticAberrationEnabled", 332u, 4u},
    {"u_ChromaticAberrationIntensity", 336u, 4u},
    {"u_ColorGradingEnabled", 340u, 4u},
    {"u_ColorTemperature", 344u, 4u},
    {"u_ColorTint", 348u, 4u},
    {"u_ColorGamma", 352u, 4u},
    {"u_FilmGrainEnabled", 356u, 4u},
    {"u_FilmGrainIntensity", 360u, 4u},
    {"u_MaxBounces", 364u, 4u},
    {"u_MaxLights", 368u, 4u},
    {"u_IndirectRays", 372u, 4u},
    {"u_StepSize", 376u, 4u},
    {"u_RenderScale", 380u, 4u},
    {"u_LightCount", 384u, 4u},
    {"u_ReconstructionFilter", 388u, 4u},
    {"u_SamplePattern", 392u, 4u},
    {"u_NeuralEnabled", 396u, 4u},
    {"u_NeuralTexStrength", 400u, 4u},
    {"u_NeuralLightStrength", 404u, 4u},
    {"u_NeuralMatStrength", 408u, 8u},
    {"u_SunDirection", 416u, 12u},
    {"u_Time", 428u, 4u},
    {"u_FogEnabled", 432u, 4u},
    {"u_SunLightColor", 436u, 12u},
    {"u_FogCount", 448u, 16u},
    {"u_FogMin", 464u, 64u},
    {"u_FogMax", 528u, 64u},
    {"u_FogColor", 592u, 64u},
    {"u_FogData", 656u, 64u},
    {"u_FogData2", 720u, 64u},
    {"u_CloudEnabled", 784u, 4u},
    {"u_CloudCount", 788u, 12u},
    {"u_CloudMin", 800u, 32u},
    {"u_CloudMax", 832u, 32u},
    {"u_CloudColor", 864u, 32u},
    {"u_CloudAmbient", 896u, 32u},
    {"u_CloudData", 928u, 32u},
    {"u_CloudData2", 960u, 32u},
    {"u_CloudData3", 992u, 32u},
    {"u_VolFast", 1024u, 4u},
    {"u_VolStepCap", 1028u, 4u},
    {"u_VolLightCap", 1032u, 4u},
    {"u_VolRangeCull", 1036u, 4u},
    {"u_GlobalFogEnabled", 1040u, 4u},
    {"u_GlobalFogDensity", 1044u, 4u},
    {"u_GlobalFogHeightFalloff", 1048u, 8u},
    {"u_GlobalFogColor", 1056u, 12u},
    {"u_GlobalFogBaseHeight", 1068u, 4u},
    {"u_GlobalFogDistAtten", 1072u, 4u},
    {"u_GodRaysEnabled", 1076u, 4u},
    {"u_GodSunScreenPos", 1080u, 8u},
    {"u_GodSunValid", 1088u, 4u},
    {"u_GodRayIntensity", 1092u, 4u},
    {"u_GodRayDecay", 1096u, 4u},
    {"u_GodRaySamples", 1100u, 4u},
    {"u_GodRayDensity", 1104u, 4u},
    {"u_GodRayColor", 1108u, 12u},
    {"u_ALightCount", 1120u, 16u},
    {"u_ALightPos", 1136u, 512u},
    {"u_ALightDir", 1648u, 512u},
    {"u_ALightColor", 2160u, 512u},
    {"u_ALightData", 2672u, 512u},
    {"u_ALightData2", 3184u, 512u},
    {"u_TSREnabled", 3696u, 4u},
    {"u_TSRSharpenStrength", 3700u, 4u},
    {"u_RCSEnabled", 3704u, 4u},
    {"u_UseRasterVisibility", 3708u, 4u},
    {"u_EffectsFlags", 3712u, 4u},
    {"u_DecalCount", 3716u, 12u},
    {"u_DecalPos", 3728u, 128u},
    {"u_DecalRight", 3856u, 128u},
    {"u_DecalUp", 3984u, 128u},
    {"u_DecalDir", 4112u, 128u},
    {"u_DecalSize", 4240u, 128u},
    {"u_DecalColor", 4368u, 128u},
};

} // namespace NovaReflection
} // namespace Wasteland
