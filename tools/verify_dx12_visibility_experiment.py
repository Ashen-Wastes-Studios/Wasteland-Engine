#!/usr/bin/env python3
"""Source contract for the opt-in DX12 visibility experiment; not a GPU test.

These checks encode invariants that were each learned from a real failure, so a future
edit that reintroduces one is caught before a build:

  * the gate geometry must never overwrite the scene instance buffer at slot 0;
  * the pass must be opt-in until the compute schedule actually reads the G-buffer;
  * the vertex transforms must use the compute path's column-vector convention;
  * the shape filter and per-shape constant-buffer regions must exist, because a single
    shared region makes every draw read whichever shape was written last.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / 'Wasteland/src/Platform/NVRHI/DX12Nova.cpp'
HEADER = ROOT / 'Wasteland/src/Platform/NVRHI/DX12Nova.h'
RENDERER = ROOT / 'Wasteland/src/Wasteland/Renderer/Renderer3D.cpp'
SHADER = ROOT / 'DemonCore-Editor/assets/shaders/Renderer3D_NovaVisibility.slang'
NOVA_SHADER = ROOT / 'DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.slang'
VK_HEADER = ROOT / 'Wasteland/src/Platform/NVRHI/VulkanNova.h'

def violations(cpp, header, renderer, shader, nova_shader, vk_header):
    errors = []
    geometry = cpp.split('void DX12Nova::CreateVisibilityGeometry()', 1)[1].split('// Draws the visibility pass', 1)[0]
    draw = cpp.split('void DX12Nova::DrawVisibility()', 1)[1].split('void DX12Nova::SubmitVisibility()', 1)[0]
    srvs = cpp.split('void DX12Nova::WriteVisibilitySrvs()', 1)[1].split('void DX12Nova::DrawVisibility()', 1)[0] \
              if 'void DX12Nova::WriteVisibilitySrvs()' in cpp else ''

    # The gate geometry must not touch the scene instance buffer.
    if re.search(r'\bUpload\s*\(\s*0\s*,', geometry):
        errors.append('gate geometry overwrites the scene instance buffer at slot 0')

    # The gate keeps its own buffer and is only the fallback.
    if 'visInstanceBuffer' not in header:
        errors.append('gate instance buffer is not declared')
    if 'visInstanceBuffer.Get()' not in draw:
        errors.append('gate instance buffer is not the non-scene fallback')

    # The scene path must select the compute path's own buffer so meshId stays
    # SV_InstanceID -- an index into the SAME reordered list the tracer uses.
    if not re.search(r'structuredBuffers\[0\]\.Get\(\)', draw):
        errors.append('scene draw does not bind structuredBuffers[0]')
    if 'visSceneInstanceCount > 0' not in draw:
        errors.append('scene draw is not gated on a non-zero scene instance count')

    # Per-shape meshes: one draw per shape, instances of other shapes clipped in VS.
    if 'buildFromMesh' not in geometry:
        errors.append('per-shape meshes are not built from ProceduralMeshes')
    if 'uploadShape(0,' not in geometry:
        errors.append('shape 0 (cube) mesh is not uploaded')

    # Per-shape constant-buffer regions. A single shared region races the GPU:
    # it can execute a recorded draw long after the CPU wrote the next shape.
    if 'kVisCbRegionSize' not in header or 'kVisShapeOffset' not in header:
        errors.append('per-shape constant-buffer region layout is not declared')
    if '+ shape * kVisCbRegionSize' not in draw:
        errors.append('draw does not select its own shape region')
    if re.search(r'visConstantBufferMapped\[currentFrame\]\s*\+\s*kVisShapeOffset', draw):
        errors.append('shape is written into the constant buffer between draws (GPU race)')

    # The view-projection changes every frame, so the buffer must be per-slot.
    if not (re.search(r'visConstantBuffer\s*\[\s*kFramesInFlight\s*\]', header)
            or re.search(r'kFramesInFlight\s*>\s*visConstantBuffer', header)):
        errors.append('visibility constant buffer is not per-slot')

    # Opt-in until the compute schedule reads the G-buffer.
    if 'WASTELAND_VIS_TEST' not in cpp or 'WASTELAND_VIS_TEST' not in renderer:
        errors.append('incomplete visibility experiment must be opt-in')

    # Column-vector convention shared with the compute path.
    if 'mul(inst.WorldTransform, float4(i.pos, 1.0))' not in shader or 'mul(u_ViewProjection,' not in shader:
        errors.append('visibility vertex transforms must match compute column-vector convention')
    if 'u_Shape' not in shader:
        errors.append('visibility shader has no per-shape filter')

    # Task 1.3: the raster G-buffer must be bound as SRVs the compute path reads.
    if 's_VisibilityIds' not in nova_shader or 's_VisibilityDepth' not in nova_shader:
        errors.append('compute shader does not declare the visibility G-buffer SRVs')
    if 'TryRasterPrimary' not in nova_shader or 'u_UseRasterVisibility' not in nova_shader:
        errors.append('compute shader has no gated raster-primary path')
    if 'kVisibilityIdsSrvSlot' not in header or 'kVisibilityDepthSrvSlot' not in header:
        errors.append('visibility SRV slots are not declared')
    if not re.search(r'ranges\[2\]\.Init\([^,]+,\s*18,\s*64\)', cpp):
        errors.append('readable SRV range does not cover t81')
    if not re.search(r'kUavBase\s*=\s*60', header):
        errors.append('UAV block was not moved for the widened SRV range')
    # The range is contiguous from t64, so t76/t77 sit inside it and must be
    # populated even though DXIL never declares them.
    if 'kImageSrvBase + 12' not in srvs or 'kImageSrvBase + 13' not in srvs:
        errors.append('t76/t77 stand-ins are not written inside the bound SRV range')
    if 'WriteVisibilitySrvs' not in cpp:
        errors.append('visibility SRV writer is missing')
    if 'kVisibilityIdsSlot' not in vk_header:
        errors.append('Vulkan layout does not declare the visibility SRVs')
    # The G-buffer is sampled as SRVs by the compute schedule, so it must leave
    # its write states -- reading RENDER_TARGET/DEPTH_WRITE is undefined.
    if 'NON_PIXEL_SHADER_RESOURCE' not in draw:
        errors.append('visibility G-buffer is not transitioned to a shader-readable state')
    if 'depthState' not in header:
        errors.append('depth target has no separate state tracker')
    return errors

def main():
    source = [p.read_text(encoding='utf-8') for p in (CPP, HEADER, RENDERER, SHADER, NOVA_SHADER, VK_HEADER)]
    errors = violations(*source)
    if errors:
        print('\n'.join('FAIL: ' + error for error in errors))
        raise SystemExit(1)

    # Negative controls: each mutation must be detected, otherwise the check is vacuous.
    race_write = ('memcpy((unsigned char*)visConstantBufferMapped[currentFrame]'
                  ' + kVisShapeOffset, &all, sizeof(all));')
    controls = [
        ('single shared shape region',
         0, lambda s: s.replace('+ shape * kVisCbRegionSize', '')),
        ('per-draw shape write (GPU race)',
         0, lambda s: s.replace('int all = -1;', 'int all = -1; ' + race_write)),
        ('non-per-slot constant buffer',
         1, lambda s: s.replace('kFramesInFlight> visConstantBuffer', 'kFramesInFlight> visInstanceBuffer')),
        ('no scene path',
         0, lambda s: s.replace('visSceneInstanceCount > 0', 'false')),
        ('no gate fallback buffer',
         0, lambda s: s.replace('visInstanceBuffer.Get()', 'structuredBuffers[0].Get()')),
        ('no shape filter',
         3, lambda s: s.replace('u_Shape', 'u_Unused')),
        ('row-vector transform',
         3, lambda s: s.replace('mul(u_ViewProjection,', 'mul(')),
        ('SRV range not widened',
         0, lambda s: s.replace('ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 18, 64)',
                                'ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 12, 64)')),
        ('UAV block not moved',
         1, lambda s: s.replace('kUavBase = 60', 'kUavBase = 54')),
        ('t76/t77 stand-ins dropped',
         0, lambda s: s.replace('kImageSrvBase + 12', 'kImageSrvBase + 99')),
        ('no Vulkan mirror',
         5, lambda s: s.replace('kVisibilityIdsSlot', 'kUnusedSlot')),
        ('no shader-readable transition',
         0, lambda s: s.replace('D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE',
                                'D3D12_RESOURCE_STATE_RENDER_TARGET')),
        ('no depth state tracker',
         1, lambda s: s.replace('depthState', 'state')),
    ]
    for name, index, mutate in controls:
        bad = source.copy()
        bad[index] = mutate(bad[index])
        if not violations(*bad):
            raise SystemExit('negative control did not detect: ' + name)

    print('PASS: opt-in scene visibility pass -- isolated gate buffer, per-shape meshes, '
          'per-shape CBV regions, per-slot constants, column-vector transforms, '
          't78..t81 SRV binding with t76/t77 populated')

if __name__ == '__main__':
    main()
