#!/usr/bin/env python3
"""Compute HLSL constant buffer layout offsets from shader source."""
import re

# Fields from NovaConstants cbuffer (register b1), in declaration order
fields = [
    ("u_CameraPosition",       "float3",  False, 0),
    ("u_InverseViewProjection","float4x4",False, 0),
    ("u_PrevViewProjection",   "float4x4",False, 0),
    ("u_InstanceCount",        "int",     False, 0),
    ("u_FrameIndex",           "int",     False, 0),
    ("u_SamplesPerPixel",      "int",     False, 0),
    ("u_PassID",               "int",     False, 0),
    ("u_DepthBuffer",          "int",     False, 0),
    ("u_Jitter",               "float2",  False, 0),
    ("u_PrevJitter",           "float2",  False, 0),
    ("u_CameraMoved",          "float",   False, 0),
    ("u_ViewProjection",       "float4x4",False, 0),
    ("u_SkyBottomColor",       "float3",  False, 0),
    ("u_SkyTopColor",          "float3",  False, 0),
    ("u_AccumulationAlpha",    "float",   False, 0),
    ("u_NormalStrength",       "float",   False, 0),
    ("u_RoughnessBias",        "float",   False, 0),
    ("u_AOIntensity",          "float",   False, 0),
    ("u_QualityLevel",         "int",     False, 0),
    ("u_BloomEnabled",         "int",     False, 0),
    ("u_BloomIntensity",       "float",   False, 0),
    ("u_BloomThreshold",       "float",   False, 0),
    ("u_VignetteEnabled",      "int",     False, 0),
    ("u_VignetteIntensity",    "float",   False, 0),
    ("u_VignetteRadius",       "float",   False, 0),
    ("u_VignetteSoftness",     "float",   False, 0),
    ("u_ChromaticAberrationEnabled", "int",False, 0),
    ("u_ChromaticAberrationIntensity","float",False, 0),
    ("u_ColorGradingEnabled",  "int",     False, 0),
    ("u_ColorTemperature",     "float",   False, 0),
    ("u_ColorTint",            "float",   False, 0),
    ("u_ColorGamma",           "float",   False, 0),
    ("u_FilmGrainEnabled",     "int",     False, 0),
    ("u_FilmGrainIntensity",   "float",   False, 0),
    ("u_MaxBounces",           "int",     False, 0),
    ("u_MaxLights",            "int",     False, 0),
    ("u_IndirectRays",         "int",     False, 0),
    ("u_StepSize",             "int",     False, 0),
    ("u_RenderScale",          "float",   False, 0),
    ("u_LightCount",           "int",     False, 0),
    ("u_ReconstructionFilter", "int",     False, 0),
    ("u_SamplePattern",        "int",     False, 0),
    ("u_NeuralEnabled",        "int",     False, 0),
    ("u_NeuralTexStrength",    "float",   False, 0),
    ("u_NeuralLightStrength",  "float",   False, 0),
    ("u_NeuralMatStrength",    "float",   False, 0),
    ("u_SunDirection",         "float3",  False, 0),
    ("u_Time",                 "float",   False, 0),
    ("u_FogEnabled",           "int",     False, 0),
    ("u_SunLightColor",        "float3",  False, 0),
    ("u_FogCount",             "int",     False, 0),
    ("u_FogMin",               "float3",  True,  4),
    ("u_FogMax",               "float3",  True,  4),
    ("u_FogColor",             "float3",  True,  4),
    ("u_FogData",              "float4",  True,  4),
    ("u_FogData2",             "float4",  True,  4),
    ("u_CloudEnabled",         "int",     False, 0),
    ("u_CloudCount",           "int",     False, 0),
    ("u_CloudMin",             "float3",  True,  2),
    ("u_CloudMax",             "float3",  True,  2),
    ("u_CloudColor",           "float3",  True,  2),
    ("u_CloudAmbient",         "float3",  True,  2),
    ("u_CloudData",            "float4",  True,  2),
    ("u_CloudData2",           "float4",  True,  2),
    ("u_CloudData3",           "float4",  True,  2),
    ("u_VolFast",              "int",     False, 0),
    ("u_VolStepCap",           "int",     False, 0),
    ("u_VolLightCap",          "int",     False, 0),
    ("u_VolRangeCull",         "int",     False, 0),
    ("u_GlobalFogEnabled",     "int",     False, 0),
    ("u_GlobalFogDensity",     "float",   False, 0),
    ("u_GlobalFogHeightFalloff","float",   False, 0),
    ("u_GlobalFogColor",       "float3",  False, 0),
    ("u_GlobalFogBaseHeight",  "float",   False, 0),
    ("u_GlobalFogDistAtten",   "float",   False, 0),
    ("u_GodRaysEnabled",       "int",     False, 0),
    ("u_GodSunScreenPos",      "float2",  False, 0),
    ("u_GodSunValid",          "int",     False, 0),
    ("u_GodRayIntensity",      "float",   False, 0),
    ("u_GodRayDecay",          "float",   False, 0),
    ("u_GodRaySamples",        "int",     False, 0),
    ("u_GodRayDensity",        "float",   False, 0),
    ("u_GodRayColor",          "float3",  False, 0),
    ("u_ALightCount",          "int",     False, 0),
    ("u_ALightPos",            "float3",  True,  32),
    ("u_ALightDir",            "float3",  True,  32),
    ("u_ALightColor",          "float3",  True,  32),
    ("u_ALightData",           "float4",  True,  32),
    ("u_ALightData2",          "float4",  True,  32),
    ("u_TSREnabled",           "int",     False, 0),
    ("u_TSRSharpenStrength",   "float",   False, 0),
    ("u_RCSEnabled",           "int",     False, 0),
]

def get_component_count(tp):
    if tp in ("float", "int"): return 1
    if tp == "float2": return 2
    if tp == "float3": return 3
    if tp == "float4": return 4
    if tp == "float4x4": return 4  # treated as 4 column vectors
    raise ValueError(f"Unknown type: {tp}")

def get_byte_size(tp):
    """Byte size of a single element (not array)."""
    if tp in ("float", "int"): return 4
    if tp == "float2": return 8
    if tp == "float3": return 12
    if tp == "float4": return 16
    if tp == "float4x4": return 64  # 4 columns x 16 bytes
    raise ValueError(f"Unknown type: {tp}")

def get_element_size(tp):
    """HLSL cbuffer array element stride is always 16 bytes."""
    return 16

# Compute layout
offset = 0
current_vec_components = 0  # components used in current 16-byte vector
results = []

for name, tp, is_array, array_count in fields:
    if is_array:
        # Arrays start at the next 16-byte boundary
        if current_vec_components > 0:
            offset = (offset // 16 + 1) * 16
            current_vec_components = 0
        start = offset
        elem_size = get_element_size(tp)
        total_size = elem_size * array_count
        results.append((name, start, total_size, is_array, array_count))
        offset = start + total_size
        current_vec_components = 0  # arrays are 16-byte aligned
    else:
        comp_count = get_component_count(tp)
        byte_size = get_byte_size(tp)
        
        if tp == "float4x4":
            # Matrix: needs 4 consecutive 16-byte vectors
            if current_vec_components > 0:
                offset = (offset + 15) // 16 * 16
                current_vec_components = 0
            start = offset
            results.append((name, start, byte_size, is_array, 0))
            offset = start + byte_size
            current_vec_components = 0
        else:
            if current_vec_components + comp_count > 4:
                offset = (offset + 15) // 16 * 16
                current_vec_components = 0
            start = offset
            results.append((name, start, byte_size, is_array, 0))
            offset = start + byte_size
            current_vec_components += comp_count

total_size = ((offset + 15) // 16) * 16
print(f"Total cbDesc.Size: {total_size} bytes ({total_size//16} vectors)")
print()

# Print variable offsets
for name, start, size, is_array, array_count in results:
    print(f'    {{ "{name}", {{ "{name}", .StartOffset={start}, .Size={size}, .Flags=0, .AlwaysDirty=FALSE, .Mutable=FALSE, .Semantic=nullptr, .Anonymous=FALSE }} }},')
