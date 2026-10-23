// Ray Tracing Shader for 3D Rendering

// #type compute -- a GL loader directive, not a GLSL one. It tells the
// loader this source is a compute shader; the version line below is what
// actually drives the compiler.
#type compute
// GLSL 4.30 core. The floor for image2D load/store and for the SSBO
// layouts used below, so the shader is not portable to older drivers.
#version 430 core

// GL_EXT_gpu_shader5 -- needed for the non-square array constructors used
// in the post-processing passes, e.g. float[](...) and ivec2[](...).
// ---
// DEFECT (documented, not fixed): the extension is enabled but the shader
// also relies on several array constructor forms that this extension does
// not actually govern -- they are core in 4.30. Harmless, but the
// dependency is misattributed, so dropping the line looks like it should
// work and does.
#extension GL_EXT_gpu_shader5 : enable


// An 8x8 workgroup. Every dispatch in Renderer3D computes group counts as
// (size + 7) / 8, so this constant is mirrored on the CPU side -- changing
// it there alone under-dispatches and leaves the right edge unwritten.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Per-thread BVH traversal stacks, in SHARED memory.
//
// This is the HLSL twin's layout (groupshared int gs_trav_stack[64 * 32]) and it
// exists because of a real backend divergence. The HLSL comment states the
// reason: "fxc cannot compile a dynamically-indexed local array inside the
// traversal call graph". So on DX the stack was forced into groupshared memory
// and is on-chip. The GLSL twin instead declared `int stack[32]` -- a private
// array -- and because every index into it is a runtime value, the compiler
// places it in LOCAL memory, which is thread-private storage backed by global
// memory. Every push and pop of the traversal is therefore a global-memory
// access, in the hottest function in the shader: TraceScene and IsOccluded run
// for every primary ray and every shadow ray.
//
// 64 threads per group (local_size 8x8, matching the HLSL numthreads(8,8,1))
// times 32 slots each. Each thread touches only its own 32 slots, so no
// barrier is required for correctness -- the same argument the HLSL makes.
//
// The shared array is declared once and reused by every traversal in the
// dispatch; each traversal seeds its own slots before use, so no state leaks
// between them.
shared int gs_trav_stack[64 * 32];

// Binding 0. The final colour target, written by RunComposite and
// RunUpscale. rgba32f because the tonemap runs on float values in this
// image before anything is narrowed.
layout(rgba32f, binding = 0) uniform image2D img_Output;
// Binding 1. The trace-resolution accumulation buffer: the HDR result of
// the tracing passes, before the composite upscales it.
layout(rgba32f, binding = 1) uniform image2D img_Accumulation;
// Binding 2. Bloom ping target. Threshold pass writes it, the two blur
// passes read and write it and img_Bloom_Temp in turn.
layout(rgba32f, binding = 2) uniform image2D img_Bloom;
// Binding 7. The bloom ping-pong partner. Blurring in place is impossible
// because the blur reads neighbours of the pixel being written, so the two
// images alternate.
layout(rgba32f, binding = 7) uniform image2D img_Bloom_Temp;
// Binding 5. rg16f -- velocity needs two components, so a third is not
// paid for. Written by pass 0 and read by the temporal accumulation.
layout(rg16f, binding = 5) uniform image2D img_Velocity;
// Binding 6 is shared by pass: it is the trace G-buffer during trace/velocity
// passes and the temporal-moment buffer during SVGF passes. Renderer3D binds
// the matching texture for each dispatch; reusing the variable keeps this
// compute program within OpenGL's guaranteed eight image uniforms.
layout(rgba32f, binding = 6) uniform image2D img_TraceGBuffer; // r=NDC depth (1=sky), gba=packed normal
// Binding 3. The accumulation image as a SAMPLER, for the bilinear reproject
// in the temporal pass. Distinct from img_Accumulation at the same binding
// -- same texture, two access paths, one write-capable and one sampling.
layout(binding = 3) uniform sampler2D s_Accumulation;
// Binding 4. The depth buffer, sampled for the bilateral blur's depth
// weight and the temporal pass's history rejection.
layout(binding = 4) uniform sampler2D s_DepthBuffer;
layout(binding = 8) uniform sampler2D s_Output;
layout(binding = 9) uniform sampler2D s_Bloom;
// Binding 2, a SAMPLER this time, matching img_Bloom's binding. Same
// texture read two ways again; the loader must bind both views or the
// write pass has nothing to read back.
layout(binding = 2) uniform sampler2D s_Indirect;
layout(binding = 10) uniform sampler2D s_NormalBuffer;
// Binding 3 as a write-only image: rgba8, so four bytes per texel for the
// packed normal plus roughness plus AO. writeonly lets the compiler drop
// the read path entirely.
layout(rgba8, binding = 3) uniform writeonly image2D img_MaterialPacked;
// Binding 11. The rasterised albedo, which the neural material path blends
// against the traced result.
layout(binding = 11) uniform sampler2D s_InputAlbedo;
layout(binding = 12) uniform sampler2D s_PackedMaterialMap;
// Binding 13. Read-only view of the temporal moments UAV (binding 8).
// GLSL cannot read from a writeonly image, so a sampler is bound to the
// same underlying texture for the read path in pass 13.
layout(binding = 13) uniform sampler2D s_Moments;
// Binding 4, a second image at the same binding as s_DepthBuffer's sampler.
// Albedo in RGB, a hit mask in A -- the mask lets a later pass tell a real
// hit from untouched background.
layout(rgba8, binding = 4) uniform image2D img_AlbedoHit; // RGB = albedo, A = hitMask
// One instance per traced mesh. The buffer is std430, so every member's
// alignment and padding must match what the CPU packs -- the vec4s keep
// this cheap, and the three scalars at the end are the only awkward part.
struct RayTracingInstance 
{
    // The inverse world transform, used to bring a world ray into object
    // space for HitPrimitive. Cached alongside the forward transform so the
    // hot intersection path does not invert a matrix per ray.
    mat4 InvTransform;      
    // The forward transform, for putting the local hit position and normal
    // back into world space.
    mat4 WorldTransform;    
    // Base colour, RGB with A unused by the PBR path.
    // ---
    // The alpha channel is read for the water path below, where it stands in
    // for a water amount rather than opacity.
    vec4 Albedo;            
    // x = metallic, y = roughness, z = primitive shape, w = water steepness.
    // ---
    // The shape field is what HitPrimitive switches on, which is why an
    // arbitrary shape fits in a material record rather than needing its own
    // instance type.
    vec4 MaterialParams;    
    // The local-space AABB min. Local, not world: the primitive intersection
    // runs in object space, so the bounds have to be in the same frame.
    vec4 Min;
    vec4 Max;
    // The local AABB max, matching the min above.
    vec4 Emission;
    // MaxDistance. Used to discard instances beyond a distance threshold
    // before any intersection work.
    float MaxDistance;
    // LODLevel for the packed material map lookup.
    // ---
    // DEFECT (minor, documented, not fixed): this member is declared and
    // uploaded but never READ anywhere in this shader. The equivalent
    // information is re-derived from distance and the instance's world extent
    // further down, so the CPU's LOD choice has no effect on tracing.
    int LODLevel;
    // Index into u_SceneTextures for the surface albedo / height map.
    // ---
    // DEFECT (documented, not fixed): this is an unbounded index into a
    // 32-element sampler array, with no clamp against u_SceneTextures.length().
    // A scene uploading more than 32 textures, or an instance with a bad id,
    // indexes out of bounds. In GLSL an out-of-bounds sampler array index is
    // undefined behaviour, so this reads whatever the driver returns rather
    // than failing loudly.
    int TextureID;
    // Index of the packed normal/roughness/AO map, used by GenerateMaterialMaps
    // rather than by tracing.
    int PackedMaterialMapID;
    // x,y = UV scale, z,w = the wave flow vector. Reusing one vec4 for both
    // texture scale and water flow means the two features cannot be
    // parameterised independently -- a surface with both gets one of them wrong.
    vec4 TextureScale;
    // Displacement: x = POM scale, y = bump strength, z = water flag,
    // w = max wave height for the foam mask.
    vec4 DisplacementParams;
    // Water-only wave parameters. Amplitude, wavelength, speed and chop
    // respectively. Unused for non-water instances, which is why the water
    // branch gates on DisplacementParams.z rather than on these being non-zero.
    vec4 WaveParams;  // x=amplitude, y=wavelength, z=speed, w=chop; water only
    int TerrainVertexOffset;
    int TerrainCellsPerSide;
    int TerrainPad0;
    int TerrainPad1;
};

// std430, binding 1. Unsized, so the array runs to the end of whatever the
// CPU bound -- the instance count lives in u_InstanceCount, not in the
// buffer.
layout(std430, binding = 1) buffer SceneInstances
{
    RayTracingInstance Instances[];
};

// The BVH node. Two vec4s, with the fourth component of each overloaded
// to carry either a child index or leaf metadata -- the classic flat-BVH
// packing, and the reason traversal below tests the sign.
struct BVHNode {
    // xyz = AABB min. w is the LEFT child for an inner node, or
    // -(firstInstance + 1) for a leaf -- negated so a negative w identifies a
    // leaf, which is what makes the traversal's branch unambiguous.
    vec4 MinBounds; // xyz = AABB min, w = leftChild (inner) or -(firstInstance+1) (leaf)
    // xyz = AABB max. w is the RIGHT child for an inner node, or the instance
    // count for a leaf.
    // ---
    // The pairing is the whole encoding: an inner node's two w fields are two
    // child indices, a leaf's are a range. Nothing marks which is which except
    // the sign of MinBounds.w, so a leaf with firstInstance 0 encodes as -1 and
    // an inner node with leftChild 0 encodes as 0 -- unambiguous.
    vec4 MaxBounds; // xyz = AABB max, w = rightChild (inner) or instanceCount (leaf)
};

layout(std430, binding = 2) buffer BVHBuffer
{
    BVHNode BVHNodes[];
};

// The per-instance light list, so a surface only evaluates lights that
// could affect it. Without it the lighting loop is per-pixel times
// per-light with no culling at all.
layout(std430, binding = 3) buffer LightListBuffer
{
    int LightIndices[];
};

// ---- ReSTIR DI reservoir (32 bytes per pixel) ----
// Bitterli et al. ACM TOG 2020. The reservoir stores a single selected
// light sample with its weight, plus the running sum of target PDFs and
// the sample count. bary.x stores the target PDF of the selected sample
// (needed for the W update); bary.y, normalOct and flagsValid are for
// spatial reuse validation.
struct LightReservoir {
    uint lightIndex;   // +0  selected light index
    uint M;            // +4  number of samples seen
    float W;           // +8  finalized RIS estimator weight
    float wSum;        // +12 aggregate candidate weight (target/source PDF)
    vec2 bary;         // +16 legacy name: x=selected pHat, y=receiver depth
    uint normalOct;    // +24 octahedral-encoded receiver normal (temporal validation)
    uint flagsValid;   // +28 bit 0 valid; upper bits producer-frame tag
};

// ---- ReSTIR DI reservoir ping-pong pair ----
// ReservoirRead is the SRV input for the current phase; ReservoirWrite is its
// distinct UAV output. Pass 3 writes candidates, pass 11 merges temporal
// history in place for its own pixel, then the caller swaps roles. Pass 14
// gathers spatial neighbors from the immutable temporal SRV and writes the
// opposite buffer; a second swap promotes that output to next-frame history.
// This buffer is deliberately not `writeonly` because pass 11 reads its own
// current-pixel candidate before replacing it.
layout(std430, binding = 26) readonly buffer ReservoirReadBuffer
{
    LightReservoir ReservoirRead[];
};
layout(std430, binding = 10) buffer ReservoirWriteBuffer
{
    LightReservoir ReservoirWrite[];
};

// The world-space eye. Used both to build the primary ray and as the
// reference point for every distance-based fade in the pass.
uniform vec3 u_CameraPosition;
// World from clip, for reconstructing the primary ray direction from a
// jittered pixel coordinate. The inverse of u_ViewProjection, so it must be
// uploaded consistently with it or every ray points somewhere else.
uniform mat4 u_InverseViewProjection;
// The PREVIOUS frame's view-projection. Reconstructing where this world
// position was last frame is what makes the velocity buffer and therefore
// the temporal reprojection possible.
uniform mat4 u_PrevViewProjection; 
uniform int u_InstanceCount; 
// The frame counter. Seeds every random sequence in the shader, so it is
// what makes a frame reproducible rather than merely different -- and it
// is why the Monte Carlo noise animates instead of crawling.
uniform int u_FrameIndex;
// DEAD UNIFORM (D3): declared and uploaded every frame, but never READ by any
// pass. There is no per-pixel sample loop and no bounce-depth loop that
// consults it; the real controls are u_IndirectRays (ray count, read at the
// indirect-bounce loop) and the preset's own indirect-ray tiering.
//
// u_MaxBounces = 1 at EVERY preset, so wiring it to a loop would hardcode one
// bounce and silently disable indirect lighting.
//
// KEPT IN PLACE deliberately: this is a packed constant buffer where every
// declaration occupies a fixed slot. Removing it would shift the offset of
// every uniform after it and require re-deriving the CPU-side
// D3D12_SHADER_VARIABLE_DESC offsets. The upload is cheap; a wrong offset is
// not. See references/dx12-compute-submission-safety.md and the Phase -1
// notes in the AAA upgrade plan.
// DEAD UNIFORM, kept deliberately. The long note above explains why it
// stays: this is a packed constant buffer where the declaration fixes the
// slot, so removing it would shift every later offset and require
// re-deriving the CPU-side descriptor. Reading it as an oversight invites
// a 'cleanup' that would corrupt every uniform after it.
uniform int u_SamplesPerPixel; // UNUSED -- see note above
// The pass selector. One shader, eleven call sites: main() switches on
// this and runs exactly one pass. Cheaper to switch on a uniform than to
// bind a different program per stage.
uniform int u_PassID;
// DEPTH PASS MARKER (D3). An int carrying whether a depth prepass ran, so a
// tracing pass can skip work when the rasteriser already wrote depth.
// ---
// DEFECT (documented, not fixed): this is declared and uploaded but never
// read in this shader -- the ray generation below always starts from the
// camera rather than from a depth value. The rasterised depth IS read, via
// the s_DepthBuffer sampler, so the intent is served through a different
// path than this uniform implies.
uniform int u_DepthBuffer;
// This frame's sub-pixel jitter, from a Halton sequence on the CPU. Applied
// to the primary ray so N samples per frame walk the pixel rather than
// hitting the same point.
uniform vec2 u_Jitter;

// ---------------------------------------------------------------------------
// RECONSTRUCTION FILTER KERNELS
//
// Two 1D kernels, applied separably (once along x, once along y) so a 2D
// filter costs its tap count rather than its square. Both are standard textbook
// forms written from the published definitions -- Catmull-Rom is the cubic
// B-spline with a=-0.5, and Lanczos-3 is sinc(x)*sinc(x/3) truncated at |x|<3.
//
// Neither is exotic, and neither needs a lookup table: the arguments are small
// integers offset by a fractional part, so a direct evaluation is cheaper than
// sampling a texture would be.
// ---------------------------------------------------------------------------

// Catmull-Rom cubic, the "smooth" bicubic. a = -0.5 gives the interpolating
// form; a = -1.0 would be sharper at the cost of more ringing. Returns a
// NEGATIVE value for part of the support, which is expected: the negative lobes
// are what make the kernel preserve edges rather than blur them, and they are
// why the caller must normalise by the sum of the weights.
float wl_cubic_kernel(float x)
{
    x = abs(x);
    float x2 = x * x;
    float x3 = x2 * x;
    if (x < 1.0f)
        return (1.5f * x3 - 2.5f * x2 + 1.0f);
    if (x < 2.0f)
        return (-0.5f * x3 + 2.5f * x2 - 4.0f * x + 2.0f);
    return 0.0f;
}

// Lanczos with a 3-lobe window. The window is sinc(x/3), which is what makes the
// kernel decay to zero at the boundary instead of continuing to oscillate --
// without it the sum of the weights would not converge.
float wl_lanczos3(float x)
{
    if (x == 0.0f)
        return 1.0f;
    float px = 3.14159265359f * x;
    float sinc = sin(px) / px;
    float px3 = px / 3.0f;
    float win = sin(px3) / px3;
    return sinc * win;
}

// The previous frame's jitter, so the reprojection can undo it. The
// difference between this and u_Jitter is what the velocity buffer records
// -- a jittered frame would otherwise read as camera motion.
uniform vec2 u_PrevJitter;
// 0 or 1. Non-zero resets the temporal accumulation, which is why a camera
// cut does not smear. A float rather than an int because the CPU blends it
// across frames for a smooth fade rather than snapping.
uniform float u_CameraMoved;
// The current view-projection. Used to reproject the traced depth back to
// screen space for the velocity buffer.
uniform mat4 u_ViewProjection;
// Sky gradient endpoints. Every ambient and miss-shading term in the shader
// samples between these two, so the sky is a single global gradient rather
// than a per-pixel model.
uniform vec3 u_SkyBottomColor;
uniform vec3 u_SkyTopColor;
// The base temporal blend weight. Lower means the history dominates, which
// is quieter but ghostier; the pass raises it when motion is detected.
uniform float u_AccumulationAlpha;
// Scales the generated material-map normals. An editor-facing strength
// slider that does not touch the traced geometry.
uniform float u_NormalStrength;
// Added to the derived roughness before packing. A bias, not a scale, so a
// zero bias leaves the derived value untouched.
uniform float u_RoughnessBias;
uniform float u_AOIntensity;
// 0..3, the preset index. Read at roughly twenty sites to pick layer
// counts, march distances and step counts, so it is the shader's single
// biggest quality knob.
uniform int u_QualityLevel; // 0 = Low, 1 = Medium, 2 = High, 3 = Ultra

// RECONSTRUCTION FILTER. This uniform was plumbed from the CPU (enum,
// glGetUniformLocation, glUniform1i, and all three NVRHI Set() calls) but never
// DECLARED here, so glGetUniformLocation returned -1, glUniform1i silently
// dropped the upload, and the editor's AA dropdown did nothing at all. Declaring
// it is what makes the ladder real.
//
// The values are the Renderer3D::ReconstructionFilter enum, and they split into
// two groups. The first five are SPATIAL reconstruction filters -- they change
// how the trace-resolution image is rebuilt at output resolution, which is the
// only thing an upscale can do:
//
//   0 None     -- no upscale, raw output (only meaningful at RenderScale 1.0)
//   1 Nearest  -- 1 tap, point sampling
//   2 Bilinear -- 4 tap, the previous implicit default
//   3 Box      -- 4 tap unweighted
//   4 Bicubic  -- 16 tap Catmull-Rom
//   5 Lanczos  -- 12 tap, 3-lobe windowed sinc
//
// The rest are not reconstruction filters at all -- they are separate passes
// (TAA is the accumulation pass, FSR1/TSR/SMAA/FXAA would each need their own
// dispatch). They are accepted here and fall through to bilinear rather than
// being silently ignored, so selecting one degrades gracefully instead of
// appearing to work. See docs/NOVA_FPS_DIAGNOSIS.md section 5.11.
uniform int u_ReconstructionFilter;

// SAMPLE PATTERN. Also plumbed from the CPU and also previously undeclared. This
// one is genuinely CPU-side -- GetCurrentJitter picks the pattern and the shader
// only ever sees the resulting u_Jitter -- so declaring it changes nothing
// functionally. It is declared so the upload is not a silent no-op, which is the
// failure mode that hid u_ReconstructionFilter for so long.
uniform int u_SamplePattern;

// ---------------------------------------------------------------------------
// POST-PROCESSING UNIFORMS (reconstructed; see Phase -1 D3).
//
// These were declared on the C++ side and reachable from editor sliders, but no
// pass ever read them: vignette, chromatic aberration and colour grading had
// no shader implementation at all, and the bloom threshold was hardcoded (0.8
// in RunComposite, 0.5 in RunBloomThreshold -- two different values). The
// accessors below now drive real code in RunComposite.
// ---------------------------------------------------------------------------
// Post-processing uniforms. The long note above records that these were
// once declared on the C++ side with no shader implementation at all,
// and that the bloom threshold was duplicated as two different hardcoded
// values -- 0.8 in the composite, 0.5 in the threshold pass.
// u_BloomEnabled is read only by the CPU-side dispatch, not by any pass
// below -- the gates in RunComposite and RunBloomThreshold key on quality
// and threshold instead. It is kept so the declaration fixes its slot in
// the packed constant buffer; see the note at u_SamplesPerPixel.
uniform int   u_BloomEnabled;
uniform float u_BloomIntensity;
// The one bloom threshold, now shared. Because the value now genuinely
// comes from here, the two former hardcoded constants cannot drift apart
// again.
uniform float u_BloomThreshold;
uniform int   u_VignetteEnabled;
uniform float u_VignetteIntensity;
uniform float u_VignetteRadius;
uniform float u_VignetteSoftness;
uniform int   u_ChromaticAberrationEnabled;
uniform float u_ChromaticAberrationIntensity;
uniform int   u_ColorGradingEnabled;
// The grading offsets. Negative temperature shifts cool because it lowers
// .r and raises .b below -- the comment on the uniform states the sign
// convention the CPU's slider has to match.
uniform float u_ColorTemperature;   // -1..1, negative shifts cool
uniform float u_ColorTint;          // -1..1, negative shifts green
uniform float u_ColorGamma;
uniform int   u_FilmGrainEnabled;
uniform float u_FilmGrainIntensity;
// DEAD UNIFORM, same reasoning as u_SamplesPerPixel above: kept because the
// declaration fixes its slot in the packed buffer. The note there records
// why wiring it up would be actively harmful -- it is 1 at every preset.
uniform int u_MaxBounces; // UNUSED -- see the note at u_SamplesPerPixel
// Caps how many of a surface's lights are evaluated. The cap is a cost
// control, so a scene with many lights silently drops the overflow rather
// than sampling all of them.
uniform int u_MaxLights;
// How many indirect rays to shoot per pixel. THE real indirect-lighting
// control, as the dead-uniform note above points out -- not u_MaxBounces.
uniform int u_IndirectRays;
// The blur step size for the bilateral pass. The CPU walks a 1,2,4,8 table
// and uploads one entry per dispatch, so the shader marches with geometric
// step growth rather than a fixed radius.
uniform int u_StepSize;
// The trace-resolution scale. Below 1 the tracing passes cover a corner
// subregion and the composite upscales it -- which is why the UV helpers
// below scale by it and every UV computation has to use them.
uniform float u_RenderScale;
uniform int u_LightCount;
// 32 scene textures, indexed by an instance's TextureID. Unsized in
// practice on the OpenGL path, and the unchecked index is the hazard
// documented on RayTracingInstance::TextureID.
uniform sampler2D u_SceneTextures[32];

// GLSL sampler arrays cannot be portably indexed by a per-instance value that
// varies across a draw. Keep every sampler subscript literal, matching the
// HLSL TEX_SLOT_IDX helper, so distinct scene materials fetch distinct maps.
vec4 wl_scene_tex_lod(int slot, vec2 uv, float lod)
{
    switch (slot)
    {
        case 0:  return textureLod(u_SceneTextures[0], uv, lod);
        case 1:  return textureLod(u_SceneTextures[1], uv, lod);
        case 2:  return textureLod(u_SceneTextures[2], uv, lod);
        case 3:  return textureLod(u_SceneTextures[3], uv, lod);
        case 4:  return textureLod(u_SceneTextures[4], uv, lod);
        case 5:  return textureLod(u_SceneTextures[5], uv, lod);
        case 6:  return textureLod(u_SceneTextures[6], uv, lod);
        case 7:  return textureLod(u_SceneTextures[7], uv, lod);
        case 8:  return textureLod(u_SceneTextures[8], uv, lod);
        case 9:  return textureLod(u_SceneTextures[9], uv, lod);
        case 10: return textureLod(u_SceneTextures[10], uv, lod);
        case 11: return textureLod(u_SceneTextures[11], uv, lod);
        case 12: return textureLod(u_SceneTextures[12], uv, lod);
        case 13: return textureLod(u_SceneTextures[13], uv, lod);
        case 14: return textureLod(u_SceneTextures[14], uv, lod);
        case 15: return textureLod(u_SceneTextures[15], uv, lod);
        case 16: return textureLod(u_SceneTextures[16], uv, lod);
        case 17: return textureLod(u_SceneTextures[17], uv, lod);
        case 18: return textureLod(u_SceneTextures[18], uv, lod);
        case 19: return textureLod(u_SceneTextures[19], uv, lod);
        case 20: return textureLod(u_SceneTextures[20], uv, lod);
        case 21: return textureLod(u_SceneTextures[21], uv, lod);
        case 22: return textureLod(u_SceneTextures[22], uv, lod);
        case 23: return textureLod(u_SceneTextures[23], uv, lod);
        case 24: return textureLod(u_SceneTextures[24], uv, lod);
        case 25: return textureLod(u_SceneTextures[25], uv, lod);
        case 26: return textureLod(u_SceneTextures[26], uv, lod);
        case 27: return textureLod(u_SceneTextures[27], uv, lod);
        case 28: return textureLod(u_SceneTextures[28], uv, lod);
        case 29: return textureLod(u_SceneTextures[29], uv, lod);
        case 30: return textureLod(u_SceneTextures[30], uv, lod);
        case 31: return textureLod(u_SceneTextures[31], uv, lod);
        default: return vec4(1.0);
    }
}

// Neural Rendering (OpenGL): tiny in-shader MLP radiance cache + texture detail.
// u_NeuralEnabled gates both; strengths blend 0 (classic path) -> 1 (full neural).
// Gates the neural paths. At 0 the shader is the classic PBR renderer;
// above 0 the neural contributions blend in by the strength uniforms below,
// so the feature is a continuous mix rather than a mode switch.
uniform int u_NeuralEnabled;
uniform float u_NeuralTexStrength;
uniform float u_NeuralLightStrength;
// Neural material blend. Defaults to 0 (classic PBR) until Renderer3D uploads it.
// Neural material blend, defaulting to 0. Kept separate from the texture
// and light strengths because it gates a different code path -- the
// material one, not the detail or bounce one.
uniform float u_NeuralMatStrength;

// Volumetric atmosphere (Renderer3D uploads; locations are skipped when absent).
// Fog d1 = (density, anisotropy, noiseStrength, noiseScale)
// Fog d2 = (windSpeed, heightFalloff, steps, enabled)
// Cloud d1 = (coverage, density, noiseScale, detail)
// Cloud d2 = (windSpeed, windDirX, windDirY, steps)
// Cloud d3 = (silverLining, shadowStrength, enabled, unused)
// The sun's travel direction. Every volumetric and god-ray term is phased
// against it, so it is the single light direction the atmosphere shares.
// The sun's TRAVEL direction, so -u_SunDirection is toward the sun. Every
// shadow ray, the phase functions and the volumetric tap use it, and the
// sign convention is why the lighting code negates it where it needs the
// incoming direction.
uniform vec3 u_SunDirection;
// Seconds, for the wind advection of the fog and cloud noise. Wall-clock
// rather than frame-count based, which is why the volumes keep moving at a
// constant rate when the frame rate changes.
uniform float u_Time;
// A master fog switch, separate from the per-volume .w flags. Both must
// agree for a volume to be marched: this one and the per-volume enabled bit.
uniform int u_FogEnabled;
// God-ray sun color (directional light color x intensity, or warm default).
// The sun's colour and intensity as a light colour. A direction cannot
// carry a colour, so this is what the volumetric in-scatter is tinted by.
// DEFECT (minor): the comment above describes it as 'directional light
// color x intensity, or warm default' -- the fallback is a CPU-side
// decision, so a scene that never uploads it marches fog lit by whatever
// the buffer happened to contain.
uniform vec3 u_SunLightColor;
uniform int u_FogCount;
// Fog box bounds, min and max. The volume integration raymarches only
// the segment inside this box, so the bounds are a performance control as
// much as a shape.
uniform vec3 u_FogMin[4];
uniform vec3 u_FogMax[4];
uniform vec3 u_FogColor[4];
uniform vec4 u_FogData[4];
// Fog d2: wind speed, height falloff, step count, enabled. Packed into one
// vec4 so the whole fog configuration is two uniform slots.
uniform vec4 u_FogData2[4];
uniform int u_CloudEnabled;
uniform int u_CloudCount;
// Cloud box bounds. Fixed at 2 volumes against the fog's 4 -- the shader
// indexes both with a bare loop counter, so raising either count is a
// change to the upload code and to this declaration together.
uniform vec3 u_CloudMin[2];
uniform vec3 u_CloudMax[2];
uniform vec3 u_CloudColor[2];
uniform vec3 u_CloudAmbient[2];
uniform vec4 u_CloudData[2];
uniform vec4 u_CloudData2[2];
// Cloud d3: silver lining, shadow strength, enabled, unused. The fourth
// slot is dead rather than reserved, which is worth knowing before reading
// the cloud code's d3.z checks as if the vector were full.
uniform vec4 u_CloudData3[2];
// 1 = reduced noise octaves (Low/Medium quality presets).
// 1 = reduced noise octaves, chosen by the Low and Medium presets. Read at
// two volume-sampling sites, where it swaps a 4-octave FBM for a 2-octave
// one.
// u_VolFast is an INT flag rather than being derived from u_QualityLevel
// inside the shader, even though the two always agree.
// ---
// The duplication means the CPU can in principle set them inconsistently,
// and nothing in the shader would notice.
uniform int u_VolFast;

// Upper bound on the volumetric march step count, as a uniform rather than the
// literal 32 it used to be. The march is the largest per-pixel cost in the trace
// pass (53.2 ms, 23.5% of it, measured by disabling the volumes), and by the time
// this was made tunable it was already running its cheapest path: fogPerfK at its
// 0.6 floor, the two-octave wl_vol_fbm2 forced by u_RenderScale < 0.75, shadow
// traversals hoisted out of the step loop, and the light loop capped at
// WL_VOL_LIGHT_CAP. The step count was the only remaining lever, and it trades
// directly against fog quality -- so it is a knob to be measured, not a constant
// to be edited. 0 or an unset uniform means the previous behaviour (32).
uniform int u_VolStepCap;

// Runtime cap on the per-step volumetric light loop, replacing the compile-time
// WL_VOL_LIGHT_CAP. Both exist for the same reason u_VolStepCap does: the march
// costs 36 ms of frame time on DemoGame, and halving the step count only moved
// 3 ms of it, so the step count is not where that cost lives. The light loop is
// the remaining per-step work -- 8 lights x up to 32 steps, each reading
// u_ALightData/u_ALightPos/u_ALightColor/u_ALightData2 by a loop-varying index
// and indexing the local lightVis[] array, which can spill to local memory.
// Making the cap a uniform is how that gets measured instead of argued about.
// 0 means "use WL_VOL_LIGHT_CAP", preserving the previous behaviour.
uniform int u_VolLightCap;

// 1 (the default) enables the hoisted segment range cull in the volumetric light
// loop; 0 disables it, restoring the pre-cull behaviour of traversing every
// selected light unconditionally. Exists so the cull can be A/B measured and so
// its output can be compared against the unculled path -- the claim that it is
// exact is only worth making if it can be checked.
uniform int u_VolRangeCull;

// Global atmospheric fog (distance haze everywhere)
uniform int u_GlobalFogEnabled;
uniform float u_GlobalFogDensity;
uniform float u_GlobalFogHeightFalloff;
uniform vec3 u_GlobalFogColor;
uniform float u_GlobalFogBaseHeight;
// Distance attenuation for the global fog. Separate from the density
// because the two are separate artist controls, and multiplying them here
// is what makes distant fog read as haze rather than as milk.
uniform float u_GlobalFogDistAtten;

// Screen-space god rays (radial blur from sun)
uniform int u_GodRaysEnabled;
// The sun's position in screen space, already projected on the CPU. The
// god-ray pass marches from this point, so it never has to project the sun
// itself and cannot disagree with the rasteriser about where it is.
uniform vec2 u_GodSunScreenPos;
// Whether that projection is meaningful. Zero when the sun is behind the
// camera or off-screen, which is the case the CPU cannot express as a
// position.
uniform int u_GodSunValid;
uniform float u_GodRayIntensity;
uniform float u_GodRayDecay;
// The god-ray sample count. Clamped to 32 inside the pass because GLSL
// cannot portably loop on a uniform -- so this is a request, and 32 is the
// hard ceiling.
uniform int u_GodRaySamples;
uniform float u_GodRayDensity;
uniform vec3 u_GodRayColor;

// Analytic component lights (Renderer3D uploads; locations skipped if absent).
// d1 = (type 0=dir 1=point 2=spot 3=area, intensity, range 0=inf, falloff)
// d2 = (spot innerCos, spot outerCos, areaSize, areaDoubleSided)
// u_ALightDir = travel direction (dir/spot) or rect normal facing scene (area).
// Analytic component lights, distinct from the BVH-traced emissive ones.
// Eight slots, fixed-size arrays rather than an SSBO, so the whole set is
// one constant-buffer upload.
// The analytic light count. The pass loops to a FIXED 8 and breaks on this,
// so it can never read past the end of the arrays regardless of what the
// CPU uploads.
uniform int u_ALightCount;
uniform vec3 u_ALightPos[32];
uniform vec3 u_ALightDir[32];
uniform vec3 u_ALightColor[32];
// d1 = (type, intensity, range, falloff) with range 0 meaning infinite.
// Type is read as a float and compared against 1.5 and 2.5 further down, so
// the type field is doing double duty as both a discriminator and a
// magnitude.
uniform vec4 u_ALightData[32];
uniform vec4 u_ALightData2[32];

// Temporal Super-Resolution (experimental, default-off)
uniform int u_TSREnabled;
uniform float u_TSRSharpenStrength;

// A cheap bounded sigmoid, x / (1 + |x|). Used as the neural MLP's
// activation precisely because it is smooth and saturating, where ReLU
// would give the MLP hard zeros.
// The softsign activation, inlined. Chosen over ReLU because the MLP
// weights below are hand-tuned against this specific curve: changing the
// activation without retuning them would change every neural term.
float wl_softsign(float x) { return x / (1.0f + abs(x)); }

// Reduced-resolution tracing: passes 0/1/2/7 dispatch at ComputeWidth/Height
// (= full size * u_RenderScale) into the corner subregion, which RunComposite
// upscales. UV math must use the dispatched extent, not the full image.
// Identical to imageSize() at scale 1, so lower presets are unaffected.
// The extent every reduced-resolution pass actually covers: full size
// times the render scale.
// ---
// The rule the rest of the shader follows: a pass dispatched at this
// extent computes its UV as (pixel + 0.5) / THIS, never / imageSize().
// Using imageSize() silently samples outside the dispatched region and
// reads uninitialised image memory.
// The dispatched extent for the reduced-resolution passes.
// ---
// The comment above states the invariant every UV computation in the
// shader follows: a pass dispatched at THIS extent computes its UV as
// (pixel + 0.5) / this, never / imageSize(). Using imageSize() would
// sample outside the dispatched corner subregion and read image memory
// that was never written this frame.
ivec2 wl_trace_extent(ivec2 fullSize)
{
    // The scaled size, truncated to int. The truncation is why the caller's
    // group count rounds UP: (size + 7) / 8 covers the truncated extent without
    // dropping a partial group.
    ivec2 t = ivec2(vec2(fullSize) * u_RenderScale);
    // max(..., 1) on both axes. A render scale small enough to scale a
    // dimension to zero would otherwise make the extent zero, and every
    // division by it below would be a division by zero.
    // max(..., 1) on both axes: a render scale small enough to scale a
    // dimension to zero would otherwise make the extent 0, and every division
    // by it in the passes would be a division by zero.
    return ivec2(max(t.x, 1), max(t.y, 1));
}

// Trace G-buffers (depth/normal) live full-size with valid data in the trace
// subregion; trace-space UVs span [0,1] across dispatched pixels, so scale
// them to buffer space when sampling. Identity at scale 1.
// Trace-space UV to buffer-space UV. Trace G-buffers are allocated at full
// size with only a corner subregion valid, so a trace-space UV in [0,1]
// has to be scaled before it can index one.
// ---
// Identity at render scale 1, which is why the helper can be called
// unconditionally and the full-resolution presets never notice it.
// Trace-space UV to buffer-space UV, for the full-size G-buffers whose
// valid data occupies only the corner subregion. Identity at scale 1.
// ---
// This is why the velocity and bilateral passes must sample depth and
// normal through this helper rather than using the raw UV.
vec2 wl_buf_uv(vec2 traceUV) { return traceUV * u_RenderScale; }

// A 2D-to-float hash. Used for dither offsets, film grain, foam breakup
// and the Monte Carlo seeds, so its quality decides how much of the
// image's noise looks like banding rather than like grain.
float wl_hash12(vec2 p)
{
    // The Dave Hoskins hash construction: swizzle, multiply by a large
    // fraction to keep only the fractional part, then mix. Cheap and
    // well-distributed enough for dithering, which is all it is asked for.
    // fract(p * 0.1031) as the first mixing step. The constant is chosen to
    // spread integer lattice inputs across the fractional range before the
    // dot-product avalanche in the next line.
    vec3 p3 = fract(vec3(p.xyx) * 0.1031f);
    // A self-mix to decorrelate the three components. Mutates p3 in place;
    // the += is what turns a single hash into three independent ones.
    // The self-mix that decorrelates the components. Mutates p3 in place, and
    // the += is what turns one hash into three independent ones.
    p3 += dot(p3, p3.yzx + 33.33f);
    return fract((p3.x + p3.y) * p3.z);
}

// Neural texture detail: sinusoidal UV encoding x normal -> 4-unit MLP -> ~1.0 multiplier.
// A 4-unit MLP over sinusoidal UV features: 2 wave fields x (uv + normal) ->
// 4 hidden units -> one scalar detail multiplier near 1.0.
// ---
// This is a hand-weighted network, not a trained one: the weights are the
// literals in the h0..h3 rows. What it buys is a detail field that varies
// with BOTH uv and surface orientation, so the same texture reads differently
// on a wall and on the top of a table.
vec3 wl_neural_tex_detail(vec2 uv, vec3 n)
{
    // Two irrational-ish frequency pairs so the interference pattern does not
    // repeat on a texel grid. coprime-looking multipliers (6.1 vs 13.7) are what
    // stop the result from looking like a checkerboard.
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    float s2 = uv.x * 13.7f - uv.y * 11.3f;
    // The 4 input features. Each is a sine of one wave field offset by a
    // normal component, so orientation enters directly rather than as a
    // separate dot product.
    vec4 feat = vec4(sin(s1 + n.x * 3.1f),
                     sin(s1 * 1.7f + n.y * 2.3f),
                     sin(s2 + n.z * 3.7f),
                     sin(s2 * 1.3f + s1 * 0.7f));
    // Hidden unit 0. softsign (not ReLU) so the network is smooth in the
    // features -- a hard activation would put visible creases in the detail
    // field where the input crosses zero.
    float h0 = wl_softsign( 0.9f * feat.x - 0.7f * feat.y + 0.5f * feat.z - 0.3f * feat.w + 0.15f);
    float h1 = wl_softsign(-0.6f * feat.x + 0.8f * feat.y - 0.4f * feat.z + 0.6f * feat.w - 0.10f);
    float h2 = wl_softsign( 0.4f * feat.x + 0.5f * feat.y + 0.9f * feat.z + 0.2f * feat.w + 0.05f);
    float h3 = wl_softsign(-0.3f * feat.x - 0.5f * feat.y + 0.6f * feat.z - 0.8f * feat.w + 0.20f);
    // Per-cell hash grain, quantised to the uv so it is stable in texture
    // space. Deliberately absent from wl_neural_micro below: the same grain is
    // wanted in albedo sparkle but NOT in displacement, where it would emboss
    // the cell grid into the geometry.
    float grain = wl_hash12(floor(uv * 64.0f)) - 0.5f;
    // The composed scalar. Centred near 1.0 and clamped implicitly by the small
    // coefficients, so this MULTIPLIES albedo rather than replacing it.
    float d = 1.0f + 0.10f * h0 + 0.07f * h1 - 0.06f * h2 + 0.05f * h3 + 0.06f * grain;
    // A per-channel tint, applied after the scalar. This is the only place the
    // detail effect gets chromatic -- making it achromatic would read as
    // dirt, giving it hue variation reads as a material change.
    vec3 tint = vec3(1.0f + 0.03f * h1, 1.0f + 0.02f * h2, 1.0f - 0.03f * h0);
    return tint * d;
}

// Smooth-only neural micro for GEOMETRY (no per-cell hash grain): the grain
// stays in the albedo tint where it sparkles, but must not imprint voxel
// cells into displaced height.
// The same 4-unit network as wl_neural_tex_detail, minus the hash grain,
// returning a scalar rather than a tint.
// ---
// Split into its own function precisely so the grain cannot leak into
// height. One shared function with a flag would have been the smaller code
// and the easier bug to ship.
float wl_neural_micro(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    float s2 = uv.x * 13.7f - uv.y * 11.3f;
    vec4 feat = vec4(sin(s1 + n.x * 3.1f),
                     sin(s1 * 1.7f + n.y * 2.3f),
                     sin(s2 + n.z * 3.7f),
                     sin(s2 * 1.3f + s1 * 0.7f));
    float h0 = wl_softsign( 0.9f * feat.x - 0.7f * feat.y + 0.5f * feat.z - 0.3f * feat.w + 0.15f);
    float h1 = wl_softsign(-0.6f * feat.x + 0.8f * feat.y - 0.4f * feat.z + 0.6f * feat.w - 0.10f);
    float h2 = wl_softsign( 0.4f * feat.x + 0.5f * feat.y + 0.9f * feat.z + 0.2f * feat.w + 0.05f);
    float h3 = wl_softsign(-0.3f * feat.x - 0.5f * feat.y + 0.6f * feat.z - 0.8f * feat.w + 0.20f);
    // Height only. No 1.0 + centre, because the caller treats this as a
    // displacement AMOUNT, not a multiplier.
    return 0.10f * h0 + 0.07f * h1 - 0.06f * h2 + 0.05f * h3;
}

// Neural radiance cache: predicts indirect bounce (reflectance * light) from
// position/normal/albedo/roughness/metal + artist sky colors. One eval
// replaces N traced rays. Diffuse lobe is suppressed on metals (they have
// ~no diffuse response); the specular lobe stays for all materials.
// The radiance cache: predicts one indirect bounce from a 4-unit MLP plus
// the two sky colours. Costs one evaluation instead of N traced rays.
// ---
// Called with surface position, normal, albedo, roughness and metalness,
// and returns the bounce radiance directly. The trade is real and worth
// stating: this is a fixed analytic lobe with no occlusion from scene
// geometry, so it lights surfaces that should be in shadow.
vec3 wl_neural_indirect(vec3 wp, vec3 n, vec3 albedo, float rough, float metal, vec3 v)
{
    // Position scaled into the network's useful range. The 0.35 factor is
    // what makes the sine features behave near the origin instead of
    // oscillating several times per unit of world space.
    vec3 p = wp * 0.35f;
    vec4 f = vec4(sin(p.x * 2.1f + n.x * 2.0f),
                  sin(p.y * 1.7f + n.y * 2.0f),
                  sin(p.z * 2.3f + n.z * 2.0f),
                  // The fourth feature folds roughness into a spatial field, so a material
                  // with spatially varying roughness gets a correspondingly varying predicted
                  // bounce.
                  sin((p.x + p.y + p.z) * 1.3f + rough * 3.0f));
    float h0 = wl_softsign( 0.8f * f.x - 0.6f * f.y + 0.4f * f.z + 0.5f * f.w + 0.10f);
    float h1 = wl_softsign(-0.5f * f.x + 0.9f * f.y - 0.3f * f.z + 0.4f * f.w - 0.05f);
    float h2 = wl_softsign( 0.3f * f.x + 0.4f * f.y + 0.8f * f.z - 0.6f * f.w + 0.00f);
    float h3 = wl_softsign(-0.4f * f.x - 0.3f * f.y + 0.5f * f.z + 0.7f * f.w + 0.15f);
    // Map the normal's y to 0..1 for the sky gradient lookup. Sky is sampled
    // by orientation only -- no ray is cast to find it -- which is why this
    // term cannot darken an interior surface that happens to face up.
    float up = n.y * 0.5f + 0.5f;
    // The gradient lookup, same u_SkyBottomColor/u_SkyTopColor pair the
    // miss shader uses.
    vec3 sky = mix(u_SkyBottomColor, u_SkyTopColor, up);
    // The diffuse lobe, gated by (1 - metal). Metals have essentially no
    // diffuse response, so feeding them this term double-counts the specular
    // below and makes them look chalky.
    vec3 diffLobe = albedo * (1.0f - metal) * sky * (0.45f + 0.30f * h0 + 0.15f * h1);
    float ndv = clamp(dot(n, v), 0.0f, 1.0f);
    // F0 for the predicted specular: dielectric 0.04 blended toward albedo by
    // a factor that falls off with roughness. The 0.5 caps the blend at half,
    // so even a perfectly smooth metal never reaches a fully coloured F0 here.
    vec3 F0 = mix(vec3(0.04f), max(albedo, vec3(0.04f)), clamp(1.0f - rough, 0.0f, 1.0f) * 0.5f);
    // The predicted specular lobe. Modulated by ndv so it responds to view
    // angle, and attenuated by roughness -- but only by 70% of it, leaving a
    // floor so rough materials are never perfectly matte in the prediction.
    vec3 spec = F0 * sky * (0.25f + 0.35f * h2) * (0.3f + 0.7f * ndv) * (1.0f - rough * 0.7f);
    float occ = 0.85f + 0.15f * h3;
    // Sum the lobes and apply the predicted occlusion. 0.85 + 0.15*h3 keeps the
    // ambient term well above zero: fully ambient-occluding predictions would
    // make contact shadows flicker as the MLP's input phase shifts.
    return (diffLobe + spec) * occ;
}

// Neural material: learned spatially-varying PBR params. Encodes UV + world
// position + normal into a 4-unit MLP whose outputs drive porosity (darken +
// roughen), patina hue shift, sparse metallic flakes, and a dielectric
// clearcoat-like F0 lift. Feeds the analytic GGX BRDF, so glints stay
// view/light-correct instead of painted on.
// The material network: emits a spatially varying PBR parameter set rather
// than a colour.
// ---
// The outputs are (albedo, metal, rough, f0Tint) INOUT, and the caller
// feeds them to the analytic GGX BRDF afterwards. That is the important
// structural choice: the network proposes parameters, the BRDF decides
// how light interacts. So glints stay correct as the camera and lights
// move, instead of being baked in as a texture-like highlight.
void wl_neural_material(vec2 uv, vec3 wp, vec3 n,
                        inout vec3 albedo, inout float metal, inout float rough,
                        inout vec3 f0Tint)
{
    // Only ONE wave field here (no s2), because the world position q already
    // supplies the spatial variation. The two would otherwise be correlated.
    float s1 = uv.x * 6.1f + uv.y * 7.7f;
    vec3 q = wp * 0.9f;
    vec4 f = vec4(sin(s1 + q.x),
                  sin(s1 * 1.6f + q.y * 1.3f),
                  sin(s1 * 0.7f - q.z * 1.7f + n.x * 2.0f),
                  sin((q.x - q.y + q.z) * 1.1f + n.y * 2.0f));
    float h0 = wl_softsign( 0.8f * f.x - 0.5f * f.y + 0.6f * f.z + 0.2f * f.w + 0.05f);
    float h1 = wl_softsign(-0.4f * f.x + 0.9f * f.y - 0.2f * f.z + 0.5f * f.w - 0.10f);
    float h2 = wl_softsign( 0.3f * f.x + 0.2f * f.y + 0.7f * f.z - 0.6f * f.w + 0.35f);
    float h3 = wl_softsign(-0.5f * f.x - 0.4f * f.y + 0.4f * f.z + 0.8f * f.w + 0.00f);
    // Sparse metallic flakes: a hard step() over a hash of a 48x48 UV cell
    // blended with an 8x8 world cell. Quantised on purpose -- a smooth function
    // would give a continuous sheen, and the wanted look is discrete specks.
    float flakeCell = step(0.75f, wl_hash12(floor(uv * 48.0f) + floor(wp.xy * 8.0f)));
    // Porosity: darken + roughen crevices
    // Porosity from the NEGATIVE part of h0, clamped. Crevices are where the
    // hidden unit is most negative, so the sign selects them.
    // ---
    // Reused as the gate for both darkening and roughening below, which keeps
    // the two effects from drifting apart -- the same pores drive both.
    float por = clamp(-h0, 0.0f, 1.0f);
    albedo *= (1.0f - 0.25f * por);
    // Clamped to [0.03, 1]. The 0.03 floor is a floor on roughness everywhere,
    // and it matters: a true zero would make the GGX denominator collapse in
    // DistributionGGX and produce a firefly.
    rough = clamp(rough + 0.30f * por, 0.03f, 1.0f);
    // Patina: subtle hue shift
    albedo *= (vec3(1.0f) + vec3(-0.06f, 0.02f, 0.05f) * h1);
    // Metallic flakes: sparse cells go mirror-smooth, tinted by albedo
    float fl = flakeCell * clamp(h2 * 1.5f, 0.0f, 1.0f);
    metal = clamp(metal + fl * 0.9f, 0.0f, 1.0f);
    rough = clamp(mix(rough, 0.08f, fl), 0.03f, 1.0f);
    // Dielectric coating: warm F0 lift (clearcoat-like), suppressed on metals
    float coat = clamp(h3, 0.0f, 1.0f) * (1.0f - metal);
    // The coating's F0 tint: a warm (1.2, 0.9, 0.6) lift, i.e. red-heavy.
    // ---
    // This is a TINT multiplier on an F0, not an F0 value. The caller adds it
    // to the dielectric 0.04 base, so the effective F0 stays far below 1 even
    // at full coat strength. Reading this line as 'the coat's F0 is (2.2, 1.9,
    // 1.6)' would be wrong.
    f0Tint = vec3(1.0f) + vec3(1.2f, 0.9f, 0.6f) * coat;
}

// ---- Volumetric fog & clouds (box volumes submitted by Renderer3D) ----
// A 3D integer-lattice hash for the volume noise. Same Dave Hoskins
// construction as wl_hash12, on three components instead of two -- the
// fract/multiply/mix chain is the whole algorithm.
float wl_vol_hash(vec3 p)
{
    p = fract(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return fract((p.x + p.y) * p.z);
}

float wl_vol_noise(vec3 p)
{
    // Value noise: hash the lattice corners and interpolate. The cost is 8
    // hash calls, which is why wl_vol_cheap exists as an alternative.
    vec3 i = floor(p);
    vec3 f = fract(p);
    // Smoothstep the fractional coordinate. Quadratic rather than linear so the
    // noise has continuous first derivatives -- without this the volume shows
    // faint grid-aligned creases along every lattice cell boundary.
    vec3 u = f * f * (3.0f - 2.0f * f);
    // Bilinear blend of the four xy corners, then bilinear again over z. Two
    // 2D mixes nested rather than one 3D trilinear, which is the same
    // arithmetic but reads as three lines instead of eight.
    return mix(mix(mix(wl_vol_hash(i), wl_vol_hash(i + vec3(1.0f, 0.0f, 0.0f)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0f, 1.0f, 0.0f)), wl_vol_hash(i + vec3(1.0f, 1.0f, 0.0f)), u.x), u.y),
               mix(mix(wl_vol_hash(i + vec3(0.0f, 0.0f, 1.0f)), wl_vol_hash(i + vec3(1.0f, 0.0f, 1.0f)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0f, 1.0f, 1.0f)), wl_vol_hash(i + vec3(1.0f, 1.0f, 1.0f)), u.x), u.y), u.z);
}

float wl_vol_fbm(vec3 p)
{
    float v = 0.0f;
    float a = 0.5f;
    // Four octaves. The loop bound is a literal rather than a uniform, so a
    // quality change cannot vary it per-pass -- u_VolFast below picks between
    // this whole function and the 2-octave one instead.
    for (int o = 0; o < 4; o++)
    {
        // Accumulate at the current amplitude before halving it, so octave 0 has
        // the most weight. The 0.5 decay makes the sum converge below 1.0.
        v += a * wl_vol_noise(p);
        // Doubling per octave, but 2.03 rather than 2.0 -- an exact doubling makes
        // every octave land on the same lattice points and correlate into visible
        // grid artefacts. The 17.3 offset keeps the first octave from repeating the
        // caller's own noise.
        p = p * 2.03f + vec3(17.3f);
        a *= 0.5f;
    }
    return v;
}

// 2-octave variant for low quality presets / cheap taps. Rescaled to match
// wl_vol_fbm's mean so coverage thresholds behave the same.
// The cheap path. Two octaves, unrolled, rescaled by 1.25 so its MEAN
// matches wl_vol_fbm's.
// ---
// That rescale is what makes the two interchangeable: a coverage
// threshold tuned against the 4-octave version behaves identically here.
// Without it, switching presets would visibly change cloud coverage.
float wl_vol_fbm2(vec3 p)
{
    return (0.5f * wl_vol_noise(p) + 0.25f * wl_vol_noise(p * 2.03f + vec3(17.3f))) * 1.25f;
}

// Ultra-cheap single-hash noise: 1 hash call instead of 24.
// One hash call instead of the 24 a 2-octave value noise costs. Not
// interpolated at all, so it is white noise -- which is fine for the
// per-step jitter and fine for nothing else.
float wl_vol_cheap(vec3 p)
{
    p = fract(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return fract((p.x + p.y) * p.z);
}

// Slab test: returns (entry, exit) distances, negative entry range when missed.
// The slab test, shared by the fog and cloud integration. Returns
// (entry, exit) so callers can march only the inside segment.
// ---
// Returning the range rather than a bool is the difference between one
// intersection test and two: the caller needs tEnter to start the march
// and tExit to bound it, and recomputing either would double the cost.
vec2 wl_box_range(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax)
{
    // Nudge any near-zero direction component away from zero so the
    // reciprocal below cannot become infinite. sign() preserves which side of
    // the slab the ray is on, so the nudge does not flip the intersection.
    vec3 safe = rd + (vec3(1.0f) - step(vec3(0.00001f), abs(rd))) * 0.00001f;
    vec3 inv = 1.0f / safe;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    // The range: max of the entry distances, min of the exits. An entry above
    // the exit means the box was missed, and the caller detects that by
    // comparing the two.
    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

// Henyey-Greenstein phase function, normalised to 1/4pi at g = 0. g is the
// anisotropy: positive forward-scattering, negative backward. The 0.001
// floor on the denominator is what keeps g = 1 from dividing by zero.
float wl_hg(float cosT, float g)
{
    float g2 = g * g;
    return 0.07957747f * (1.0f - g2) / pow(max(1.0f + g2 - 2.0f * g * cosT, 0.001f), 1.5f);
}

// Front-to-back march through one fog box. Returns (scatteredLight, transmittance).
// sunVis gates the sun in-scatter (god-ray shafts): 1 = fully sunlit fog.
// The ray type, shared by the primitive intersectors. Capitalised fields
// because the HLSL twin uses the same names and the struct is mirrored
// between the two shaders.
// ---
// NOTE: the comment directly above this line describes wl_march_fog_vol --
// "Front-to-back march through one fog box..." -- but sits here, above an
// unrelated struct. The function it documents is defined much further down
// (it carries a second copy of the same text). The comment is stranded from
// its function and now reads as if it documented Ray. Harmless to the
// compiler, actively misleading to a reader.
struct Ray { vec3 Origin; vec3 Direction; };

// A boolean box test, used only as a coarse pre-filter -- if the ray misses
// the box the expensive per-shape solve is skipped entirely. invDir is
// passed in rather than computed because callers already have it.
bool RayAABB(Ray r, vec3 invDir, vec3 minB, vec3 maxB) 
{
    vec3 t0 = (minB - r.Origin) * invDir;
    vec3 t1 = (maxB - r.Origin) * invDir;
    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar = min(min(tMax.x, tMax.y), tMax.z);
    // Hit if the intervals overlap AND the exit is in front of the origin. The
    // second condition is what makes this a forward-ray test: a box entirely
    // behind the camera has a valid but negative exit.
    return tNear <= tFar && tFar > 0.0f;
}

// GLOBAL mutable state, initialised per-invocation from the pixel index.
// ---
// DEFECT (documented, not fixed): a global with an initialiser runs that
// initialiser for every invocation, so the hash below starts from a
// distinct value per pixel -- but it also means the variable is a single
// global that survives the whole invocation, so every call to hash()
// anywhere in this shader shares one sequence. Any pass that samples two
// unrelated stochastic quantities (e.g. a light pick and a BVH jitter)
// gets correlated draws from the same stream rather than independent
// ones. It is a global because GLSL permits globals in shaders and the
// declaration reads like it was meant to be a local.
uint seed = uint(gl_GlobalInvocationID.y * 1024 + gl_GlobalInvocationID.x) + uint(u_FrameIndex * 1000);

float hash() 
{
    // PCG-style LCG. The multipliers are the Numerical Recipes constants, and
    // the xorshift-multiply finaliser below turns the low-quality LCG bits into
    // well-distributed ones.
    seed = seed * 747796405u + 2891336453u;
    // The finaliser: shift by a seed-dependent amount, xor, multiply, then
    // fold the high bits down. The variable shift ((seed >> 28) + 4) is the
    // part that makes consecutive seeds decorrelate -- a fixed shift would
    // leave visible structure.
    uint result = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    result = (result >> 22u) ^ result;
    // Normalise to [0,1]. 4294967295 is UINT_MAX, so this is an exact divide
    // -- deliberately not /2^32, which would lose the top value.
    return result / 4294967295.0f;
}

vec3 random_in_hemisphere(vec3 normal)
{
    // Two uniforms for a cos-weighted hemisphere sample. u sets the azimuth,
    // v the elevation through z.
    // ---
    // Drawing them in this fixed order matters: the sequence is shared (see
    // the note on the global seed above), so two call sites that each wanted
    // one uniform would still receive different values, but a site wanting a
    // particular pair is coupled to every call before it.
    float u = hash();
    float v = hash();
    // z uniform in [-1,1], mapped from v.
    float z = 2.0f * v - 1.0f;
    float r = sqrt(max(1.0f - z * z, 0.0f));
    float theta = 2.0f * 3.14159265359f * u;
    // dir with uniform z on the unit sphere, then flattened into the tangent
    // frame. Building it in a fixed tangent frame rather than around `normal`
    // is cheaper; the caller re-orthogonalises.
    vec3 dir = vec3(r * cos(theta), r * sin(theta), z);
    // Reject the lower hemisphere by negating. Mirroring rather than
    // re-sampling is correct for a hemisphere because the distribution is
    // symmetric about the plane -- and cheaper, at the cost of correlating the
    // reflected sample with the original.
    return dot(dir, normal) > 0.0f ? dir : -dir;
}

// ---------------------------------------------------------------------------
// TUNING CONSTANTS
//
// WL_VOL_LIGHT_CAP -- the volumetric multi-light cap. This number MUST be
// identical in BOTH twins (Renderer3D_NovaRenderer.glsl / .hlsl).
//
// It is a PERFORMANCE multiplier, not a quality preference: every one of the
// march steps below runs one BVH traversal per light, and IsOccluded() is a
// complete scene traversal rather than a cheap test. So the per-pixel cost is
// steps x WL_VOL_LIGHT_CAP traversals. A divergence between the twins is not a
// "quality difference" -- the same scene, the same preset, renders DIFFERENT
// LIGHTING on each backend, and it costs 4x here (32 vs 8 meant the OpenGL path
// did 1024 traversals per pixel where DX did 256). tools/verify_nova_glsl.py
// asserts the two files agree on this value; changing it in one twin alone
// fails that check.
// ---------------------------------------------------------------------------
#define WL_VOL_LIGHT_CAP 8

const float PI = 3.14159265359f;

// The GGX / Trowbridge-Reitz normal distribution: how much microfacet
// area faces the half vector. Peaks at NdotH = 1.
float DistributionGGX(float NdotH, float roughness)
{
    // roughness SQUARED, then squared again -- alpha = roughness^4. The
    // exponentiation is what makes GGX's tail tighter at low roughness, and
    // skipping it gives a noticeably wrong highlight.
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    // The 0.0001 floor on the denominator. Without it, a = 0 (a perfectly
    // smooth surface) makes denom = 1 and the whole expression a2 / PI -- but
    // a near-zero denom at roughness -> 0 and NdotH -> 1 would spike without
    // bound. This is the standard GGX firefly clamp.
    return a2 / max(PI * denom * denom, 0.0001f);
}

// Schlick's approximation to the GGX geometry term, with the k = (r+1)^2/8
// Schlick-GGX fit. This is the light-vs-view half; GeometrySmith below
// multiplies two of them.
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / (NdotV * (1.0f - k) + k);
}

// The full Smith height-correlated geometry term: G = G_view * G_light.
// ---
// The independent-sampling variant (G / (4 NdotV NdotL)) would need both
// cosines divided out again at the call site; folding them in here is why
// the BRDF below can divide by just one of them.
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Schlick's Fresnel approximation, F = F0 + (1-F0)(1-cos)^5.
// ---
// The exponent 5 is what makes it fall off sharply except at grazing
// angles -- which is where the bright rim on every surface in the image
// comes from, and why clamping cosTheta below 1 is deliberate: it keeps
// the rim.
vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    // t = 1 - cosTheta, kept as a named value because it is raised to the
    // fifth power in the next line and reusing the subexpression is free here.
    float t = 1.0f - cosTheta;
    float t2 = t * t;
    return F0 + (1.0f - F0) * t2 * t2 * t;
}

vec3 GetSkyColor(vec3 dir);

// Cube (Blender: Cube). A unit cube, so the bounds are hardcoded to
// +/-0.5 -- the instance's extents are applied by the CPU when the mesh is
// created, not here.
// ---
// Returns t along the LOCAL ray or -1, matching the convention the shape
// dispatcher expects.
float HitCube(Ray localRay, out vec3 outNormal) 
{
    // The same near-zero guard as wl_box_range, written as sign * max(abs) so
    // a component of exactly -0.0f still lands on -1e-5 rather than +1e-5.
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001f));
    vec3 invDir = 1.0f / safeDir;

    vec3 tMin = (vec3(-0.5f) - localRay.Origin) * invDir;
    vec3 tMax = (vec3(0.5f) - localRay.Origin) * invDir;
    
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    
    // The entry distance is the max over axes, the exit the min. Comparing
    // them is the hit test, below.
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar  = min(min(t2.x, t2.y), t2.z);
    
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    
    // Pick the axis that produced the entry distance -- that is the face the
    // ray entered through. Floating-point equality against t1.x is fragile in
    // principle but safe here because tNear is literally one of the three
    // values returned by max, with no arithmetic in between.
    // ---
    // The normal is negated against sign(Direction) so it faces OUT of the
    // cube, whichever side the ray came from.
    if (tNear == t1.x)      outNormal = vec3(-sign(localRay.Direction.x), 0.0f, 0.0f);
    else if (tNear == t1.y) outNormal = vec3(0.0f, -sign(localRay.Direction.y), 0.0f);
    else                    outNormal = vec3(0.0f, 0.0f, -sign(localRay.Direction.z));

    return tNear;
}

// Sphere, centred on the local origin. The quadratic is solved directly,
// which is exact -- unlike the torus below, no iteration is needed.
float HitSphere(Ray localRay, float radius, out vec3 outNormal) 
{
    float a = dot(localRay.Direction, localRay.Direction);
    float b = 2.0f * dot(localRay.Origin, localRay.Direction);
    float c = dot(localRay.Origin, localRay.Origin) - (radius * radius);
    float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) return -1.0f;
    float sqrtD = sqrt(discriminant);
    // Try the near root, then the far root. The fallback matters for a ray that
    // starts INSIDE the sphere: the near root is then negative and the far one
    // is the exit.
    float t = (-b - sqrtD) / (2.0f * a);
    if (t < 0.0f) t = (-b + sqrtD) / (2.0f * a);
    if (t > 0.0f) 
    {
        outNormal = normalize(localRay.Origin + t * localRay.Direction);
        return t;
    }
    return -1.0f;
}

// --- Blender "Add > Mesh" primitives -------------------------------------
// Shape constants must match Wasteland::PrimitiveShape in
// Renderer/ProceduralMeshes.h -- that enum is what CPU writes into
// inst.MaterialParams.z.
//
// Every routine works in instance-local space and returns the ray parameter
// t along the LOCAL ray, matching HitCube/HitSphere above.

// Axis-aligned rectangular slab in the XZ plane, |y| <= thickness, facing +Y.
// Used for both Plane and Grid: a grid is a plane, just tessellated -- the
// path tracer has no use for the extra vertices.
// Plane / Grid. halfExtent.x and .z bound the slab; .y is the thickness the
// comment mentions but is deliberately not used, because the plane is a
// single-sided quad facing +Y rather than a box.
float HitPlaneXZ(Ray localRay, vec3 halfExtent, out vec3 outNormal)
{
    // Near-parallel rejection BEFORE the division. Doing the divide first
    // would produce a huge t for a nearly horizontal ray, and the extent test
    // below might then accept it -- so this guard is load-bearing, not just an
    // optimisation.
    if (abs(localRay.Direction.y) < 1e-8f) return -1.0f;

    float t = -localRay.Origin.y / localRay.Direction.y;
    if (t <= 0.0f) return -1.0f;

    vec3 p = localRay.Origin + t * localRay.Direction;
    if (abs(p.x) > halfExtent.x || abs(p.z) > halfExtent.z) return -1.0f;

    outNormal = vec3(0.0f, 1.0f, 0.0f);
    return t;
}

// Flat filled disc in the XZ plane facing +Y (Blender: Circle).
// Circle / disc. Identical to HitPlaneXZ except the extent test is a
// radial one. The duplication is deliberate: a union would add a branch to
// both, and these run once per shape test per ray.
float HitDisc(Ray localRay, float radius, out vec3 outNormal)
{
    if (abs(localRay.Direction.y) < 1e-8f) return -1.0f;

    float t = -localRay.Origin.y / localRay.Direction.y;
    if (t <= 0.0f) return -1.0f;

    vec3 p = localRay.Origin + t * localRay.Direction;
    // r^2 <= radius^2, done in dot form to avoid a sqrt.
    if (dot(p.xz, p.xz) > radius * radius) return -1.0f;

    outNormal = vec3(0.0f, 1.0f, 0.0f);
    return t;
}

// Y-axis cylinder with capped ends (Blender: Cylinder).
// Solves the infinite cylinder in XZ, then clamps to the slab and tests the
// caps separately, taking whichever is nearest.
// Cylinder. Three candidate hits are possible (two side roots, two caps)
// and the NEAREST wins, so the routine carries a running best through both
// tests rather than returning as soon as one succeeds.
float HitCylinder(Ray localRay, float radius, float halfHeight, out vec3 outNormal)
{
    float best = -1.0f;
    vec3 bestNormal = vec3(0.0f);

    vec2 o = localRay.Origin.xz;
    vec2 d = localRay.Direction.xz;
    float a = dot(d, d);
    float b = 2.0f * dot(o, d);
    float c = dot(o, o) - radius * radius;

    // A ray parallel to the Y axis has d.xz = 0, so a = 0 and the quadratic
    // degenerates. Skipping the side test is correct -- it cannot intersect.
    if (a > 1e-12f)
    {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f)
        {
            float sq = sqrt(disc);
            // Both roots, so rays starting inside the tube still register.
            // Both roots are tried, not just the near one. A ray originating inside the
            // cylinder has only a far root, and returning -1 there would make solid
            // objects leak.
            for (int k = 0; k < 2; k++)
            {
                float t = (k == 0) ? (-b - sq) / (2.0f * a) : (-b + sq) / (2.0f * a);
                if (t <= 0.0f) continue;
                float y = localRay.Origin.y + t * localRay.Direction.y;
                if (y < -halfHeight || y > halfHeight) continue;
                if (best < 0.0f || t < best)
                {
                    best = t;
                    vec3 p = localRay.Origin + t * localRay.Direction;
                    // The side normal is the radial direction at the hit point, NOT scaled by
                    // the slope -- the cylinder wall is vertical, so pure radial is right.
                    bestNormal = normalize(vec3(p.x, 0.0f, p.z));
                }
            }
        }
    }

    // Caps
    // Caps. Guarded the same way as the plane's parallel case: a horizontal
    // ray can never reach a horizontal cap.
    if (abs(localRay.Direction.y) > 1e-8f)
    {
        for (int k = 0; k < 2; k++)
        {
            float planeY = (k == 0) ? halfHeight : -halfHeight;
            float t = (planeY - localRay.Origin.y) / localRay.Direction.y;
            if (t <= 0.0f) continue;
            if (best >= 0.0f && t >= best) continue;
            vec3 p = localRay.Origin + t * localRay.Direction;
            if (dot(p.xz, p.xz) > radius * radius) continue;
            best = t;
            // Cap normals are +/-Y by construction, chosen by which cap k selected.
            bestNormal = vec3(0.0f, (k == 0) ? 1.0f : -1.0f, 0.0f);
        }
    }

    if (best <= 0.0f) return -1.0f;
    outNormal = bestNormal;
    return best;
}

// Y-axis cone with a capped base (Blender: Cone).
// Apex at local (0, +halfHeight, 0), rim radius at y = -halfHeight.
// Solved in the plane of the ray's XZ direction: the silhouette of a cone from
// that plane is two lines, so this is a quadratic rather than the naive cubic.
// Cone. The quadric is solved directly rather than by iteration -- see
// the note above for why a cone is tractable while a torus is not.
float HitCone(Ray localRay, float radius, float halfHeight, out vec3 outNormal)
{
    float best = -1.0f;
    vec3 bestNormal = vec3(0.0f);

    vec2 o = localRay.Origin.xz;
    vec2 d = localRay.Direction.xz;

    // (radial)^2 = (slope * (halfHeight - y))^2 along the surface.
    // The surface relation: radial distance equals slope times the height
    // above the base. Squaring both sides gives the quadratic below.
    // slope = radius / (2 * halfHeight). The 2 comes from the cone spanning
    // the FULL base-to-apex height of 2 * halfHeight.
    float slope = radius / (2.0f * halfHeight);
    float k = slope * slope;

    float a = dot(d, d) - k * localRay.Direction.y * localRay.Direction.y;
    float b = 2.0f * (dot(o, d) + k * localRay.Direction.y * (halfHeight - localRay.Origin.y));
    float c = dot(o, o) - k * (halfHeight - localRay.Origin.y) * (halfHeight - localRay.Origin.y);

    // abs(a) > 1e-12 rather than a > 0: a can legitimately go NEGATIVE, which
    // is the tangent-ray case and still has valid roots. Testing only for
    // positive a would silently drop those hits.
    if (abs(a) > 1e-12f)
    {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f)
        {
            float sq = sqrt(disc);
            for (int i = 0; i < 2; i++)
            {
                float t = (i == 0) ? (-b - sq) / (2.0f * a) : (-b + sq) / (2.0f * a);
                if (t <= 0.0f) continue;
                vec3 p = localRay.Origin + t * localRay.Direction;
                // Only the nappe between the apex and the base plane.
                // Restrict to the nappe between apex and base. Without this the squared
                // equation also accepts the mirror cone below the base plane, and rays
                // would hit the wrong half.
                if (p.y < -halfHeight || p.y > halfHeight) continue;
                if (best < 0.0f || t < best)
                {
                    best = t;
                    // Cone wall normal = radial, tilted by the slope.
                    vec2 radial = p.xz;
                    float rl = length(radial);
                    radial = (rl > 1e-6f) ? radial / rl : vec2(1.0f, 0.0f);
                    // The wall normal: radial, tilted up by the slope. This is the gradient of
                    // the surface relation above, so it is the correct normal for a cone whose
                    // half-angle differs from 45 degrees.
                    bestNormal = normalize(vec3(radial.x, slope, radial.y));
                }
            }
        }
    }

    // Base cap
    if (abs(localRay.Direction.y) > 1e-8f)
    {
        float t = (-halfHeight - localRay.Origin.y) / localRay.Direction.y;
        if (t > 0.0f && (best < 0.0f || t < best))
        {
            vec3 p = localRay.Origin + t * localRay.Direction;
            if (dot(p.xz, p.xz) <= radius * radius)
            {
                best = t;
                bestNormal = vec3(0.0f, -1.0f, 0.0f);
            }
        }
    }

    if (best <= 0.0f) return -1.0f;
    outNormal = bestNormal;
    return best;
}

// Distance from a point along the ray to the torus tube surface (negative
// inside). Extracted so the bracketing loop stays readable; the compiler
// inlines it.
// The signed distance to the tube surface at parameter t. Positive outside,
// negative inside -- the sign is the only thing the bracketing loop needs,
// so no normal is derived here.
float TorusSDF(Ray localRay, float t, float majorR, float minorR)
{
    vec3 p = localRay.Origin + t * localRay.Direction;
    float rxz = length(p.xz);
    float ring = (rxz > 1e-6f) ? (rxz - majorR) : majorR;
    // sqrt of the squared radial offset plus y^2, minus the tube radius: the
    // distance to the circle in the (rxz, y) cross-section.
    return sqrt(ring * ring + p.y * p.y) - minorR;
}

// Torus about the Y axis (Blender: Torus).
//
// Solved by bracketing the signed distance
//     f(t) = distance(P(t), tube centreline) - minorR
// over the ray interval already bounded by the caller's RayAABB test, then
// bisecting each sign change. A seeded-Newton solve was tried first and
// rejected: seeding from the radial-distance quadratic gives two candidates
// that frequently converge to the same crossing, so it both missed hits and
// returned the far root. Bracketing cannot miss a crossing and always yields
// the nearest one.
// Torus. The hard case in this set: a torus is a quartic, so there is no
// closed-form solve. The approach is bisection over the ray interval that
// the caller's box test already bounded.
// ---
// The long note above records that seeded Newton was tried and rejected --
// it both missed hits and returned the far root. Bisection cannot miss a
// crossing and always converges to the nearest, at the cost of a fixed
// 32-sample bracket.
float HitTorus(Ray localRay, float majorR, float minorR, vec3 boxMin, vec3 boxMax, out vec3 outNormal)
{
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001f));
    vec3 invDir = 1.0f / safeDir;

    vec3 ta = (boxMin - localRay.Origin) * invDir;
    vec3 tb = (boxMax - localRay.Origin) * invDir;
    vec3 tNear3 = min(ta, tb);
    vec3 tFar3 = max(ta, tb);

    float tEnter = max(max(tNear3.x, tNear3.y), tNear3.z);
    float tExit = min(min(tFar3.x, tFar3.y), tFar3.z);
    tEnter = max(tEnter, 0.0f);
    if (tExit <= tEnter) return -1.0f;

    // GLSL has no function-like multi-line #define, so the distance function
    // is a real (inlined) function taking the ray and a parameter.

    // 32 samples across the entry-to-exit interval, then bisection between
    // adjacent sign changes. The sample count is a literal, so torus
    // intersection cost is fixed regardless of quality preset -- unlike the
    // other shapes, it is not on the u_QualityLevel dial.
    const int kSamples = 32;
    // Uniform step over the bounded interval. Dividing by the sample count
    // rather than stepping by a world-space epsilon keeps the resolution
    // proportional to the box, which varies per instance.
    const float dt = (tExit - tEnter) / float(kSamples);

    float best = -1.0f;
    float prevT = tEnter;
    // Seed the bisection at the box entry. Evaluating the SDF once here rather
    // than treating tEnter as implicitly inside avoids a special case for the
    // first interval.
    float prevF = TorusSDF(localRay, prevT, majorR, minorR);
    // A ray starting inside the tube exits through a crossing, but that is an
    // exit rather than an entry; only accept outside->inside transitions.
    // Whether the previous sample was inside the tube. A ray that starts
    // inside has its first transition as an EXIT, which is not a surface hit
    // -- so the entry direction has to be tracked, not just the sign.
    bool prevInside = prevF < 0.0f;

    // March the bounded interval. Signed, not even/odd: an odd number of
    // crossings would mean a tangential graze, which the inside/outside test
    // rejects.
    for (int i = 1; i <= kSamples; i++)
    {
        float curT = tEnter + dt * float(i);
        float curF = TorusSDF(localRay, curT, majorR, minorR);
        bool curInside = curF < 0.0f;

        // A sign change between adjacent samples brackets exactly one surface
        // crossing. This is the whole detection mechanism -- 32 samples is a fixed
        // budget, so a very thin tube smaller than the sample spacing can be
        // stepped over entirely.
        if (curInside != prevInside)
        {
            float lo = prevT, hi = curT;
            float fLo = prevF;
            // 24 bisection iterations. Each one halves the interval, so this resolves
            // t to roughly (tExit-tEnter) / 2^24 -- far below any float precision the
            // caller needs.
            // ---
            // DEFECT (minor, documented, not fixed): the loop is a fixed 24 iterations
            // regardless of how quickly it converged, so every torus hit pays the full
            // cost. It could break once lo and hi are adjacent floats. The cost is
            // bounded and the result is correct, so this is a performance note rather
            // than a bug.
            for (int iter = 0; iter < 24; iter++)
            {
                float mid = 0.5f * (lo + hi);
                float fMid = TorusSDF(localRay, mid, majorR, minorR);
                // The standard bisection update: keep whichever half still brackets the
                // crossing, carrying fLo forward so the next comparison needs no re-eval.
                // ---
                // Comparing (fMid < 0) against (fLo < 0) instead of comparing fMid against
                // 0 is what makes this correct when the surface is exactly at the
                // midpoint, where fMid == 0 is neither inside nor outside.
                if ((fMid < 0.0f) == (fLo < 0.0f)) { lo = mid; fLo = fMid; }
                else { hi = mid; }
            }
            float hitT = 0.5f * (lo + hi);
            // Accept only outside->inside, and keep the nearest such hit. The !prevInside
            // guard is the 'entry not exit' rule; the best comparison makes the result
            // independent of which crossing the loop happens to find first.
            if (!prevInside && (best < 0.0f || hitT < best))
                best = hitT;
        }

        prevT = curT;
        prevF = curF;
        prevInside = curInside;
    }

    if (best <= 0.0f) return -1.0f;

    vec3 p = localRay.Origin + best * localRay.Direction;
    float rxz = length(p.xz);
    // Project the hit point back onto the ring circle at majorR, in the xz
    // plane. The torus normal is the offset from that ring to the point -- the
    // tube centre, not the torus origin.
    vec2 ring = (rxz > 1e-6f) ? p.xz * (majorR / rxz) : vec2(majorR, 0.0f);
    outNormal = normalize(vec3(p.x - ring.x, p.y, p.z - ring.y));
    return best;
}

// Single entry point for every analytic primitive. localBounds is the
// instance's local AABB (.xyz), already known to contain the hit.
// The shape dispatcher. One switch-free chain of early returns, so a Cube
// instance pays a single comparison rather than the whole ladder.
// ---
// This is called from the BVH leaf handler after the instance's box has
// already been tested, so the box test inside the shape routines is
// redundant here -- but harmless, and it keeps each routine independently
// correct.
float HitPrimitive(Ray localRay, RayTracingInstance inst, out vec3 outNormal)
{
    // The shape id, read from MaterialParams.z. This is a FLOAT being narrowed
    // to int, and it is the contract with the CPU's PrimitiveShape enum -- a
    // mismatch in the two enums silently changes every surface's geometry.
    // ---
    // DEFECT (documented, not fixed): no range check. An out-of-range id falls
    // through to the HitCube fallback below, so a bad id produces a cube
    // rather than an error -- the shape will look plausible and be wrong.
    int shape = int(inst.MaterialParams.z);
    // Half-extent from the local bounds. Computed for every shape though only
    // the plane path uses it, because it is two subtractions and a multiply.
    vec3 halfExtent = (inst.Max.xyz - inst.Min.xyz) * 0.5f;

    if (shape == 0) return HitCube(localRay, outNormal);              // Cube
    if (shape == 1) return HitSphere(localRay, inst.MaterialParams.w, outNormal); // Sphere

    if (shape == 2 || shape == 7) return HitPlaneXZ(localRay, halfExtent, outNormal); // Plane / Grid
    if (shape == 6) return HitDisc(localRay, inst.MaterialParams.w, outNormal);        // Circle

    // Cylinder / Cone / Torus carry radius in MaterialParams.w; the second
    // dimension (height, or minor radius) rides in Min.w, which was previously
    // an unused constant 1.0.
    // Radius comes from MaterialParams.w and the second dimension from Min.w.
    // Min.w is otherwise unused -- it was a constant 1.0 -- so these three
    // shapes read a field that used to carry no information. The CPU has to
    // write it, or height and minor radius silently become 1.0.
    if (shape == 3) return HitCylinder(localRay, inst.MaterialParams.w, inst.Min.w, outNormal);
    if (shape == 4) return HitCone(localRay, inst.MaterialParams.w, inst.Min.w, outNormal);
    if (shape == 5) return HitTorus(localRay, inst.MaterialParams.w, inst.Min.w, inst.Min.xyz, inst.Max.xyz, outNormal);

    // Unknown/newer shape: fall back to the bounding box rather than
    // silently dropping the surface.
    return HitCube(localRay, outNormal);
}

// Shadow-ray test against one instance, in its local space. maxDist bounds
// the shadow ray; skipIdx is -1 when the caller has no surface to exempt.
bool TestInstanceOcclusion(Ray r, float maxDist, int instIdx) {
    RayTracingInstance inst = Instances[instIdx];

    vec3 diff = u_CameraPosition - inst.WorldTransform[3].xyz;
    // Cheap distance cull: an instance further than MaxDistance from the
    // CAMERA cannot occlude anything. Note this is distance from the eye, not
    // from the shadow ray -- it culls by how far the object is from the viewer,
    // which is a proxy, not the same test.
    if (dot(diff, diff) > inst.MaxDistance * inst.MaxDistance) return false;

    Ray localRay;
    // Transform into object space. w=0 for the direction so translation is
    // not applied to it -- using w=1 for both would bend every ray slightly.
    localRay.Origin = (inst.InvTransform * vec4(r.Origin, 1.0f)).xyz;
    localRay.Direction = normalize((inst.InvTransform * vec4(r.Direction, 0.0f)).xyz);
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001f));
    vec3 localInvDir = 1.0f / safeDir;

    if (RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) {
        vec3 localNormal;
        float t = HitPrimitive(localRay, inst, localNormal);
        if (t > 0.0f) {
            vec3 localHit = localRay.Origin + t * localRay.Direction;
            vec3 worldHit = (inst.WorldTransform * vec4(localHit, 1.0f)).xyz;
            if (distance(r.Origin, worldHit) < maxDist) return true;
        }
    }
    return false;
}

// The any-hit shadow query: a full BVH traversal that stops at the FIRST
// occluder. Contrast with the closest-hit traversal, which must keep going
// and return the nearest -- this is why it can early-out and that is where
// its speed comes from.
bool IsOccluded(Ray r, float maxDist, int skipIdx) {
    if (u_InstanceCount == 0) return false;

    vec3 safeDir = sign(r.Direction) * max(abs(r.Direction), vec3(0.00001f));
    vec3 invDir = 1.0f / safeDir;
    // A fixed 32-deep explicit stack, so no dynamic allocation in the shader.
    // ---
    // DEFECT (documented, not fixed): the depth is bounded by the push guard
    // below (stackPtr < 30), so the traversal is SAFE -- a deep node is simply
    // skipped rather than overflowing. But silently dropping a subtree makes
    // a shadow ray report 'not occluded' through an occluder that lives in the
    // skipped branch. Correctness depends on the BVH builder keeping depth
    // under 30; nothing in this shader checks that.
    // Shared-memory stack base for this thread. See gs_trav_stack above: a
    // dynamically-indexed PRIVATE array lands in local memory (global-backed),
    // which made every push/pop here a global-memory access.
    int stackBase = int(gl_LocalInvocationIndex) * 32;
    int stackPtr = 0;
    // Seed with the root. Node 0 is assumed to be the root by the builder's
    // contract, not discovered by traversal.
    gs_trav_stack[stackBase + stackPtr++] = 0;

    // Pop until empty. Stack order means this is depth-first, and the traversal
    // is not ordered front-to-back -- so this test has no early termination on
    // tNear, unlike a closest-hit loop.
    while (stackPtr > 0) {
        int nodeIdx = gs_trav_stack[stackBase + --stackPtr];
        BVHNode node = BVHNodes[nodeIdx];

        vec3 t0 = (node.MinBounds.xyz - r.Origin) * invDir;
        vec3 t1 = (node.MaxBounds.xyz - r.Origin) * invDir;
        vec3 tMin = min(t0, t1);
        vec3 tMax = max(t0, t1);
        float tNear = max(max(tMin.x, tMin.y), tMin.z);
        float tFar = min(min(tMax.x, tMax.y), tMax.z);
        // Reject three ways: intervals disjoint (miss), entirely behind (tFar < 0),
        // or beyond the shadow ray's reach (tNear > maxDist). The third is what
        // makes the test bounded -- without it every ray would visit the whole tree.
        if (tNear > tFar || tFar < 0.0f || tNear > maxDist) continue;

        // Decode the node. A negative MinBounds.w means a leaf; the encoding
        // -(first + 1) means the first instance is recoverable as -encoded - 1.
        // ---
        // This is the dual-use of one float documented on the BVHNode struct. It
        // is only unambiguous because inner-node child indices are non-negative.
        int encoded = int(node.MinBounds.w);
        if (encoded < 0) {
            // first = -encoded - 1. The -1 offset is what lets leaf 0 encode as -1
            // rather than as 0, which would collide with an inner node's left child.
            int first = -encoded - 1;
            int count = int(node.MaxBounds.w);
            // Iterate the leaf's instance range. Contiguous because the builder sorts
            // instances by BVH leaf, so a leaf's set is a single index range.
            for (int i = first; i < first + count; i++) {
                // Never let a light shadow itself: shadow rays aim at the
                // light's center, so without this the light's own front
                // surface always occludes and kills all direct light.
                // Self-occlusion exemption, and the comment above it explains why it is
                // mandatory rather than an optimisation: a shadow ray starts on the
                // light-facing surface, which would occlude itself.
                if (i == skipIdx) continue;
                if (TestInstanceOcclusion(r, maxDist, i)) return true;
            }
        } else {
            // Push both children, guarded. The 30 leaves two slots of headroom under
            // the 32-deep array, so the write can never go out of bounds -- see the
            // note on the stack declaration for what that costs in correctness.
            if (stackPtr < 30) {
                // Right child first, then left, so the left child is popped first -- i.e.
                // the stack visits the left subtree before the right.
                gs_trav_stack[stackBase + stackPtr++] = int(node.MaxBounds.w);
                gs_trav_stack[stackBase + stackPtr++] = encoded;
            }
        }
    }
    return false;
}


// Front-to-back fog integration. Returns rgb = in-scattered light and a =
// transmittance, composited by the caller.
// ---
// This is the function the stranded comment above `struct Ray` (near the
// top of this file) actually documents -- see the note there.
vec4 wl_march_fog_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                      vec3 tint, vec4 d1, vec4 d2, float dither, float sunVis)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0f);
    float t1 = min(range.y, tMax);
    float fogLen = max(t1 - t0, 0.0f);
    // The performance tier. Below render scale 0.6 the step budget drops to
    // 60%; the smoothstep ramps it back to full by 0.9. Volumetrics are the
    // first thing to cost when the trace resolution is cut.
    float fogPerfK = mix(0.6f, 1.0f, smoothstep(0.6f, 0.9f, u_RenderScale));
    // Step count: the scene's own value, scaled by the perf tier and by a
    // fog-length factor, then clamped to [2, 32].
    // ---
    // The length factor rewards short fog segments with fewer steps, which is
    // backwards from the usual argument (a short segment needs FEWER samples
    // only if it is also thin) -- but it is a heuristic on visual quality, not
    // a correctness issue.
    // Guard: an unset uniform is 0 in GLSL, and clamp(x, 2, 0) is 0, which would
    // silently disable the volume entirely. Fall back to the previous behaviour.
    float stepCap = u_VolStepCap > 0 ? float(u_VolStepCap) : 32.0f;
    int steps = int(clamp(d2.z * fogPerfK * clamp(fogLen / 16.0f, 0.15f, 1.0f), 2.0f, stepCap));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    float dt = (t1 - t0) / float(steps);
    // The Henyey-Greenstein phase for the sun, evaluated once for the whole
    // march rather than per step: it depends only on the ray and sun
    // directions, neither of which varies along the ray.
    // ---
    // The +/-0.9 clamp on g matters -- at |g| = 1 the phase function is a
    // delta in the limit and wl_hg's denominator guard would be the only thing
    // preventing an infinity.
    float phase = wl_hg(dot(rd, u_SunDirection), clamp(d1.y, -0.9f, 0.9f));
    // The ambient term: the sky gradient sampled at its midpoint. One colour
    // for the whole box, so fog is lit by the average sky rather than by the
    // sky at each sample's height.
    vec3 ambCol = mix(u_SkyBottomColor, u_SkyTopColor, 0.5f);
    vec3 sunLight = ambCol * 0.7f + u_SunLightColor * sunVis * (0.25f + phase * 2.0f);
    float hgt = max(bmax.y - bmin.y, 0.001f);
    vec3 acc = vec3(0.0f);
    float trans = 1.0f;
    // Start at a fractional offset so successive frames sample different
    // points -- the dither breaks the slice banding that a fixed start offset
    // would produce as the camera moves.
    float t = t0 + dt * dither;
    
    // Quality gate for multi-light scattering: Low preset uses sun only,
    // Medium+ adds point/spot lights with shadow rays.
    int lightStart = 0;
    int lightEnd = 0;
    // Multi-light in-scattering is gated on quality AND on lights existing.
    // At Low the volume is lit by the sun alone, which is the cheapest
    // acceptable answer and removes a shadow ray per step per light.
    if (u_QualityLevel >= 1 && u_ALightCount > 0)
    {
        int volLightCap = u_VolLightCap > 0 ? u_VolLightCap : WL_VOL_LIGHT_CAP;
        // Never exceed the compile-time array size: lightVis is declared with
        // WL_VOL_LIGHT_CAP elements and a larger runtime cap would overrun it.
        volLightCap = min(volLightCap, WL_VOL_LIGHT_CAP);
        lightEnd = min(u_ALightCount, volLightCap);
    }

    // The shadow test is HOISTED out of the march loop.
    // ---
    // IsOccluded() is a complete BVH traversal over every scene instance, not a
    // cheap test. Running it per march step per light made the per-pixel cost
    // steps x lightEnd traversals -- up to 32 x 32 = 1024 on the OpenGL path,
    // against 32 x 8 = 256 on DX. Same scene, same preset, different lighting:
    // the divergence was a correctness bug as well as a 4x backend difference.
    //
    // A light's visibility to a given RECEIVER changes far more slowly than the
    // sample position does along the ray, so the traversal is done ONCE per light
    // per pixel, from the volume entry point -- the representative position for
    // the whole march -- and the result gates the per-step light loop.
    //
    // What stays INSIDE the loop, deliberately: the range cull (arange > 0 &&
    // dL >= arange) and the spot-cone test. Both are functions of the per-step
    // sample position p and cannot be hoisted without changing the answer.
    //
    // The guard makes the hoist a no-op at lightEnd == 0 (Low quality), which
    // matches the previous behaviour exactly: the volume is then lit by the sun
    // alone and the array is never built.
    float lightVis[WL_VOL_LIGHT_CAP];
    if (lightEnd > 0)
    {
        vec3 entry = ro + rd * t0;
        for (int li = 0; li < lightEnd; li++)
        {
            // RANGE CULL, hoisted above the traversal, tested against the whole
            // segment rather than the entry point alone.
            //
            // Why this is worth doing: the traversal below is a complete BVH walk
            // over every scene instance, and it was paid once per light per pixel
            // with no regard for whether the light can reach the volume at all. On
            // DemoGame (677 point lights, 8 selected per pixel) that is 8
            // traversals per pixel and 28.7 ms of frame time -- 79% of the fog's
            // total 36.3 ms. Cutting the light cap from 8 to 1 measured 216.7 ->
            // 188.0 ms; this cull recovers 19.7 ms of that.
            //
            // The segment test is what makes this exact. Testing only the ENTRY
            // point would be an approximation, and a wrong one: the volume is
            // 10000 m deep, so a light out of range of the entry can be well
            // inside the range of a sample further along the ray, and culling it
            // would silently drop real in-scattering. What is exact is the closest
            // point on the segment [t0, t1] to the light -- if that is beyond the
            // range, no sample in the volume can be within it, so the light
            // contributes exactly zero and skipping its traversal cannot change
            // the answer. (The test is conservative in the other direction: a
            // light inside range of the segment's closest point but outside every
            // discrete sample still pays its traversal and then contributes
            // nothing, which is the previous behaviour, not a regression.)
            //
            // The spot-cone test is NOT hoisted -- it depends on the direction
            // from the SAMPLE to the light, which varies along the ray.
            float arange = max(u_ALightData[li].z, 0.0f);
            vec3 elpos = u_ALightPos[li];
            vec3 etoL = elpos - entry;
            float edL = length(etoL);
            if (u_VolRangeCull != 0 && arange > 0.0f)
            {
                vec3 segEnd = ro + rd * t1;
                vec3 seg = segEnd - entry;
                float segLen2 = dot(seg, seg);
                float tSeg = segLen2 > 1e-6f ? clamp(dot(etoL, seg) / segLen2, 0.0f, 1.0f) : 0.0f;
                float dClosest = length(etoL - seg * tSeg);
                if (dClosest >= arange)
                {
                    lightVis[li] = 0.0f;
                    continue;
                }
            }
            // Same 1cm origin nudge the per-step ray uses, for the same reason:
            // without it the ray starts exactly on a surface and self-occludes.
            vec3 eL = etoL / max(edL, 0.0001f);
            Ray eRay = Ray(entry + eL * 0.01f, eL);
            lightVis[li] = IsOccluded(eRay, edL, -1) ? 0.0f : 1.0f;
        }
    }
    
    for (int s = 0; s < int(stepCap); s++)
    {
        if (s >= steps)
            break;
        vec3 p = ro + rd * t;
        float d = max(d1.x, 0.0f);
        float h = clamp((p.y - bmin.y) / hgt, 0.0f, 1.0f);
        // Height falloff: exp(-h * 4) where h is normalised 0..1 within the box,
        // mixed toward 1 by (1 - d2.y). So d2.y = 0 disables falloff entirely
        // and d2.y = 1 applies the full exponential.
        d *= mix(1.0f, exp(-h * 4.0f), clamp(d2.y, 0.0f, 1.0f));
        if (d1.z > 0.001f && d > 0.001f)
        {
            // Advect the noise lookup by wind. The y component is NOT advected and the
            // z factor is 0.6 -- wind is horizontal, and scaling z separately is what
            // stops the pattern from shearing into diagonal streaks.
            vec3 np = p * max(d1.w, 0.001f) + vec3(u_Time * d2.x, 0.0f, u_Time * d2.x * 0.6f);
            // Pick the noise variant. Note the second condition: a low render scale
            // forces the cheap octave count even on High quality, for the same reason
            // the step budget is cut above -- resolution and octave count are traded
            // together, not independently.
            float n = (u_VolFast == 1 || u_RenderScale < 0.75f) ? wl_vol_fbm2(np) : wl_vol_fbm(np);
            d *= (1.0f - d1.z * 0.5f) + d1.z * n;
        }
        // Convert density to per-step alpha analytically: 1 - exp(-d * dt).
        // ---
        // The exact exponential transmittance over the step, rather than the
        // common linear approximation d * dt. This is why the march is
        // step-count independent in total opacity: halving dt doubles the step
        // count and gives the same answer.
        float a = 1.0f - exp(-max(d, 0.0f) * dt);
        if (a > 0.001f)
        {
            vec3 sampleLight = sunLight;
            for (int li = lightStart; li < lightEnd; li++)
            {
                float atype = u_ALightData[li].x;
                // Directional lights (type 0) are skipped in the volumetric path -- they
                // have no position to scatter from, and the sun is already handled
                // separately by sunLight above.
                if (atype < 0.5f) continue;
                // The shadow traversal was hoisted out of this loop (see the note
                // above the march): one BVH traversal per light per pixel, instead
                // of one per step per light.
                if (lightVis[li] < 0.5f) continue;
                vec3 lpos = u_ALightPos[li];
                vec3 lcol = u_ALightColor[li] * u_ALightData[li].y;
                float arange = max(u_ALightData[li].z, 0.0f);
                vec3 toL = lpos - p;
                float dL = length(toL);
                // Range cull. arange > 0 means finite; range 0 is the 'infinite light'
                // sentinel, tested rather than compared.
                if (arange > 0.0f && dL >= arange) continue;
                vec3 L = toL / max(dL, 0.0001f);
                float att;
                if (arange > 0.0f)
                    // Finite lights get a windowed falloff: pow(1 - d/range, falloff), which
                    // reaches EXACTLY zero at the range boundary. The 0.5 floor on the
                    // exponent stops a zero falloff from making the light a hard-edged disc.
                    att = pow(clamp(1.0f - dL / arange, 0.0f, 1.0f), max(u_ALightData[li].w, 0.5f));
                else
                    // Infinite lights get inverse-square instead. The 0.1 factor is a softness
                    // constant; the max(dL*dL, 0.5) on the exponent keeps it from exploding
                    // at the origin.
                    att = 1.0f / (1.0f + 0.1f * max(u_ALightData[li].w, 0.5f) * dL * dL);
                if (atype > 1.5f && atype < 2.5f)
                {
                    float c = dot(-L, u_ALightDir[li]);
                    // The spot cone: smoothstep between the inner and outer cosines, so the
                    // penumbra is smooth. u_ALightData2.x is innerCos and .y is outerCos --
                    // note the ORDER, which is why this reads oddly: outer is the lower edge
                    att *= smoothstep(u_ALightData2[li].y, u_ALightData2[li].x, c);
                    if (att <= 0.001f) continue;
                }
                // No shadow ray in this loop any more: the BVH traversal was hoisted
                // out of it (see lightVis, above the march), so the per-step cost per
                // light is now the compare at the top of the loop rather than a
                // traversal. Only the phase depends on L.
                float lPhase = wl_hg(dot(rd, L), clamp(d1.y, -0.9f, 0.9f));
                sampleLight += lcol * att * (0.3f + lPhase * 2.0f);
            }
            // Front-to-back compositing: multiply by the transmittance accumulated so
            // far, so nearer samples correctly occlude further ones. This ordering is
            // the entire reason for the transmittance variable.
            acc += trans * a * tint * sampleLight;
            trans *= (1.0f - a);
            // Early-out once transmittance drops below 2%. The remaining samples
            // contribute less than the float precision of the accumulator, so the
            // loop would be wasted work -- and a thick fog volume is exactly where
            // this triggers early.
            if (trans < 0.02f)
            {
                trans = 0.0f;
                break;
            }
        }
        t += dt;
    }
    return vec4(acc, trans);
}

// Front-to-back march through one cloud layer box. Returns (scatteredLight, transmittance).
vec4 wl_march_cloud_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                        vec3 tint, vec3 amb, vec4 d1, vec4 d2, vec4 d3, float dither)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0f);
    float t1 = min(range.y, tMax);
    float clLen = max(t1 - t0, 0.0f);
    // Perf tier: same render-scale step scaling as wl_march_fog_vol.
    // The same perf tier as the fog march. Duplicated rather than shared -- the
    // two use different reference lengths below (8.0 vs 16.0), so they are not
    // actually the same formula.
    float cloudPerfK = mix(0.6f, 1.0f, smoothstep(0.6f, 0.9f, u_RenderScale));
    // Cloud steps. The length divisor is 8.0 against the fog's 16.0, so clouds
    // get MORE steps for a segment of the same length -- deliberate, because a
    // cloud's density profile has visible structure and the fog's does not.
    int steps = int(clamp(d2.w * cloudPerfK * clamp(clLen / 8.0f, 0.25f, 1.0f), 2.0f, 32.0f));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    float dt = (t1 - t0) / float(steps);
    vec2 wnd = vec2(d2.y, d2.z);
    // Guard against a zero wind vector before normalising: normalize(vec2(0))
    // is undefined, and d2.y/z default to 0 when a scene sets no wind.
    if (dot(wnd, wnd) < 0.00000001f)
        wnd = vec2(1.0f, 0.0f);
    // Normalise the direction, then scale by wind speed and TIME. Multiplying
    // by u_Time is what makes the advection constant-rate: the offset grows
    // with elapsed time rather than per frame.
    wnd = normalize(wnd) * d2.x * u_Time;
    // The silver lining: a 6th-power lobe around the sun direction. The high
    // exponent is what makes it a narrow forward-scatter rim rather than a
    // general brightening.
    float silverPow = pow(clamp(dot(rd, u_SunDirection), 0.0f, 1.0f), 6.0f);
    vec3 sunCol = vec3(1.2f, 1.1f, 1.0f);
    float hgt = max(bmax.y - bmin.y, 0.001f);
    float nScale = max(d1.z, 0.001f);
    vec3 acc = vec3(0.0f);
    float trans = 1.0f;
    float t = t0 + dt * dither;
    for (int s = 0; s < 32; s++)
    {
        if (s >= steps)
            break;
        vec3 p = ro + rd * t;
        // Sample position, scaled by noise scale and offset by the advected wind.
        // Note the wind is applied as an ADDITIVE offset in noise space rather than
        // by moving p -- cheaper, and equivalent for a stationary pattern.
        vec3 q = p * nScale + vec3(wnd.x, 0.0f, wnd.y);
        float base = (u_VolFast == 1 || u_RenderScale < 0.75f) ? wl_vol_fbm2(q) : wl_vol_fbm(q);
        float cov = clamp(d1.x, 0.0f, 1.0f);
        // The coverage threshold, applied as a smoothstep. The 0.35 width is the
        // softness of the cloud edge.
        // ---
        // Note the argument order: (1-cov) to (1-cov+0.35) means higher coverage
        // shifts the threshold DOWN, so more of the noise field counts as cloud.
        float d = smoothstep(1.0f - cov, 1.0f - cov + 0.35f, base);
        if (d > 0.001f && d1.w > 0.001f)
        {
            // Detail erosion: a single higher-frequency noise octave at 3.7x, which
            // carves wisps out of the base shape. Advected on y at half the wind speed,
            // so the detail slides relative to the base -- that relative motion is what
            // makes the clouds churn.
            float det = wl_vol_noise(q * 3.7f + vec3(0.0f, u_Time * d2.x * 0.5f, 0.0f));
            // d = d - det*d1.w*d, i.e. erosion scaled by the current density. Proportional
            // erosion: dense cores erode less than thin edges, which preserves the
            // silhouette while fraying the boundary.
            d = max(d - det * d1.w * d, 0.0f);
        }
        d *= max(d1.y, 0.0f);
        float h = clamp((p.y - bmin.y) / hgt, 0.0f, 1.0f);
        // Vertical density profile: ramps in over the bottom 25% of the box and
        // thins toward the top, keeping 30% at the ceiling. The asymmetric profile
        // is what makes a cloud layer have a flat base and a soft top.
        d *= smoothstep(0.0f, 0.25f, h) * (1.0f - smoothstep(0.6f, 1.0f, h) * 0.7f);
        float a = 1.0f - exp(-d * dt);
        if (a > 0.001f)
        {
            float sh = 1.0f;
            if (d3.y > 0.01f)
            {
                // Cheap self-shadow: single-octave density tap toward the sun
                // (a full second march is overkill for a soft darkening term).
                // Self-shadowing by a SINGLE noise tap toward the sun rather than a second
                // full march. Cheap, and the result is a soft darkening -- the comment
                // above says as much, and it is the right trade for a soft volumetric term.
                vec3 sp = p + u_SunDirection * dt * 2.0f;
                float sd = wl_vol_noise(sp * nScale + vec3(wnd.x, 0.0f, wnd.y)) * max(d1.y, 0.0f);
                // exp(-density * stepLength * 4 * strength) as a transmittance. The 4.0 is
                // a tuning constant folded in here rather than exposed as a uniform, so
                // the cloud shadow depth is not artist-controllable.
                sh = exp(-sd * dt * 4.0f * clamp(d3.y, 0.0f, 1.0f));
            }
            // In-scatter: ambient scaled by how lit the sample is, plus a direct sun
            // term also scaled by sh. Both fall off with the same transmittance, which
            // is why a deep cloud's interior goes dark rather than uniformly bright.
            vec3 scol = tint * (amb * (0.35f + 0.65f * sh) + sunCol * sh * 0.9f)
                      + sunCol * silverPow * clamp(d3.x, 0.0f, 1.0f) * 0.6f;
            acc += trans * a * scol;
            trans *= (1.0f - a);
            if (trans < 0.02f)
            {
                trans = 0.0f;
                break;
            }
        }
        t += dt;
    }
    return vec4(acc, trans);
}

// Applies all submitted cloud + fog volumes to a primary-ray sample.
// sunVis scales fog sun in-scatter (god rays); clouds keep self-shadowing.
// Apply every submitted volume to one primary-ray sample, in order: clouds
// first, then fog.
// ---
// The order is a real design decision. Clouds composite first, so the fog
// sits IN FRONT of the clouds -- which is right for a ground-hugging fog
// box, and wrong if a fog box overlaps a cloud layer.
vec3 wl_apply_volumetrics(vec3 ro, vec3 rd, float tHit, vec3 baseColor, float dither, float sunVis)
{
    vec3 col = baseColor;
    if (u_CloudEnabled == 1)
    {
        for (int i = 0; i < 2; i++)
        {
            if (i >= u_CloudCount)
                break;
            if (u_CloudData3[i].z < 0.5f)
                continue;
            vec4 r = wl_march_cloud_vol(ro, rd, tHit, u_CloudMin[i], u_CloudMax[i],
                                        u_CloudColor[i], u_CloudAmbient[i],
                                        u_CloudData[i], u_CloudData2[i], u_CloudData3[i], dither);
            // Composite: colour * transmittance + in-scattered. This is the standard
            // front-to-back 'over' operator, and it is why transmittance has to be
            // returned alongside the scattered light rather than applied internally.
            col = col * r.a + r.rgb;
        }
    }
    if (u_FogEnabled == 1)
    {
        for (int i = 0; i < 4; i++)
        {
            if (i >= u_FogCount)
                break;
            if (u_FogData2[i].w < 0.5f)
                continue;
            vec4 r = wl_march_fog_vol(ro, rd, tHit, u_FogMin[i], u_FogMax[i],
                                      u_FogColor[i], u_FogData[i], u_FogData2[i], dither, sunVis);
            col = col * r.a + r.rgb;
        }
    }
    return col;
}
// Global atmospheric fog: distance + height haze that applies everywhere
// The global distance/height haze, applied AFTER the ray's colour is
// known -- i.e. to a shaded pixel, not along the ray. Contrast with the box
// volumes above, which are integrated during the march.
vec3 wl_apply_global_fog(vec3 col, float dist, vec3 viewDir, float dither)
{
    if (u_GlobalFogEnabled != 1)
        return col;
    if (u_GlobalFogDensity <= 0.00001f)
        return col;
    float h = u_CameraPosition.y;
    // The camera's offset from the fog base height, split into below and above.
    // Two terms because the falloff constants differ by 10x below and above
    float hBelow = max(u_GlobalFogBaseHeight - h, 0.0f);
    float hAbove = max(h - u_GlobalFogBaseHeight, 0.0f);
    // Height falloff, with a 0.1 factor below the base and 0.01 above. The 10x
    // asymmetry means the fog is a thin ground layer with a long, faint tail
    // upward -- an artist intent, but a hardcoded one rather than a uniform.
    float heightF = exp(-hBelow * u_GlobalFogHeightFalloff * 0.1f) * 
                    exp(-hAbove * u_GlobalFogHeightFalloff * 0.01f);
    // Extinction over the pixel's distance, analytic exponential again. The
    // three factors are distance, the height term above, and the distance
    // attenuation control -- multiplying them is what makes it read as haze.
    float ext = 1.0f - exp(-dist * u_GlobalFogDensity * heightF * u_GlobalFogDistAtten);
    ext = clamp(ext, 0.0f, 1.0f);
    vec3 fc = u_GlobalFogColor;
    float luma = dot(col, vec3(0.2126f, 0.7152f, 0.0722f));
    // Blend toward the fog colour, with the dither offsetting the blend point
    // by +/-0.3. This is a cheap way to break up the smoothstep's banding --
    // but note it perturbs the RESULT, not the alpha, so it slightly changes
    // the fog's colour rather than only its coverage.
    vec3 target = mix(col, fc, 0.5f + 0.3f * dither);
    // Protect bright pixels: a smoothstep on luma reduces how much fog is
    // applied to already-bright areas. Without it a bright highlight gets grey
    // and reads as dirty.
    target = mix(col, target, smoothstep(0.0f, 0.85f, luma));
    return mix(col, target, ext);
}

// Screen-space god rays: radial blur from sun screen position
// Screen-space god rays: a radial blur toward the sun's screen position,
// sampling the already-composited output. Genuinely screen-space, so the
// shafts are occluded by whatever is already dark in the colour buffer --
// it cannot see geometry the pass has no depth for.
void RunGodRays()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    // Three gates in one early return: disabled, no valid sun position, or a
    // non-positive sample count. All three are CPU-side authoring states, so
    // the pass is skipped entirely rather than run and discarded.
    if (u_GodRaysEnabled != 1 || u_GodSunValid != 1 || u_GodRaySamples <= 0)
        return;

    vec2 sunPos = u_GodSunScreenPos;
    vec2 uv = (vec2(pos) + 0.5f) / vec2(fullSize);

    // The step direction in aspect-corrected space. Dividing x by the aspect
    // ratio makes the blur circular in pixels instead of stretching with the
    // viewport -- without this, god rays on a wide monitor smear sideways.
    vec2 delta = (sunPos - uv) * vec2(fullSize.x / max(float(fullSize.y), 1.0f), 1.0f);
    float len = length(delta);
    if (len < 0.001f) return;

    // Normalise the delta, then divide by the sample count so each step lands
    // exactly on the sun position after `samples` iterations.
    vec2 stepDir = delta / len / float(u_GodRaySamples);
    vec2 currentUV = uv;

    vec3 accum = vec3(0.0f);
    float decay = u_GodRayDecay;
    float weight = u_GodRayDensity;
    // A SECOND decay, separate from the one used on the sampled colour. So
    // falloff along the ray is decay^2 per step, because the sample is scaled
    // by illumDecay and the running weight decays again after accumulation.
    float illumDecay = 1.0f;

    // Clamp the loop bound to 32. The GLSL loop above uses the uniform, but an
    // unbounded loop over a uniform is not portable, so this fixed clamp is
    // the actual iteration count.
    int samples = clamp(int(u_GodRaySamples), 1, 32);

    for (int i = 0; i < samples; i++)
    {
        currentUV += stepDir;
        // Stop when the march leaves the screen. The remaining samples would
        // clamp-to-edge at the border and smear the border pixel outward, so
        // breaking is both cheaper and visually correct.
        if (currentUV.x < 0.0f || currentUV.x > 1.0f || currentUV.y < 0.0f || currentUV.y > 1.0f)
            break;

        vec3 s = texture(s_Output, currentUV).rgb;
        s *= illumDecay * weight;
        accum += s;
        illumDecay *= decay;
    }

    // Normalise by the sample count so the result is independent of how many
    // steps were taken. Note this divides by `samples`, not by the number
    // actually accumulated -- so a march that breaks early (pixel near the
    // screen edge) returns a DIMMER result than an identical interior pixel.
    // The artifact is a slight darkening of god rays toward the frame edge.
    accum *= u_GodRayIntensity / float(samples);
    accum *= u_GodRayColor;

    vec3 existing = imageLoad(img_Output, pos).rgb;
    imageStore(img_Output, pos, vec4(existing + accum, 1.0f));
}



// The Halton sequence, radical inverse in the given base. This is the
// source of the sub-pixel jitter -- a low-discrepancy sequence rather than
// white noise, so successive frames cover the pixel evenly instead of
// clumping.
float Halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    // Repeatedly take the digit and accumulate the fractional weight. The
    // division reuses `base` rather than an int power, which is what keeps
    // this from overflowing for large indices.
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

// The per-frame jitter, in pixels.
//
// The offset comes from u_Jitter, which the CPU fills from the GENERATED
// spatio-temporal blue-noise volume (BlueNoise::Jitter) and uploads through
// glUniform2f (Renderer3D.cpp:2998). It used to be computed here with
// Halton(index % 16, base), which cycled through only 16 distinct offsets and
// then repeated them exactly -- the same structured artefacts recurred in the
// accumulation buffer every 16 frames. The volume was generated and uploaded
// for exactly this purpose but nothing read it, so the Halton defect survived
// the generator landing.
//
// The frameIndex/cameraMoved parameters are now unused: the host resets
// s_Data.FrameIndex to 0 on a view-projection change (Renderer3D.cpp:2135,
// :2197) and on the still-frame latch (:2823), so the blue-noise sequence
// already restarts exactly when the history is invalidated. The shader-side
// cameraMoved branch would restart a sequence that is not ours to restart.
// They are kept so the signature and call site stay unchanged.
vec2 GetJitter(int frameIndex, float cameraMoved) {
    return u_Jitter;
}

// The 3x3 Sobel kernels for the gradient operators, in GLSL's column-major
// mat3 constructor order. Gx is the x-derivative, Gy the y-derivative.
const mat3 Gx = mat3(-1, 0, 1, -2, 0, 2, -1, 0, 1);
const mat3 Gy = mat3(-1, -2, -1, 0, 0, 0, 1, 2, 1);

float GetLuminance(vec3 col) {
    return dot(col, vec3(0.2126f, 0.7152f, 0.0722f));
}

// RGB to YCoCg. This is the colour space the bilateral blur runs in: the
// luminance channel carries the depth-weighting and the chroma is
// reconstructed afterwards, which stops the blur from desaturating edges.
vec3 RGB2YCoCg(vec3 rgb) {
    float Y  = 0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b;
    float Co = 0.5f  * rgb.r - 0.5f * rgb.b;
    float Cg = -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b;
    return vec3(Y, Co, Cg);
}

vec3 YCoCg2RGB(vec3 ycocg) {
    float Y  = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    // YCoCg back to RGB, written as the explicit inverse rather than a matrix
    // multiply. The forward transform's coefficients are not orthonormal, so
    // the inverse is not the transpose -- it has to be written out.
    return vec3(
        Y + Co - Cg,
        Y + Cg,
        Y - Co - Cg
    );
}

// Procedural UV assignment. There is no mesh and no UV attribute for these
// primitives, so the UV is derived from the hit position by shape.
// ---
// This and ComputeTangentFrame below MUST agree. The comment on the cube
// branch there describes exactly what happens when they diverge: the POM
// march walks a different UV space than the heights are sampled from.
vec2 CalculateUV(vec3 localPos, RayTracingInstance inst) {
    // The cube branch: per-face planar projection. The dominant axis of the
    // position picks the face, so the projection switches at the 45-degree
    // diagonals -- which is why a cube's texture is only continuous across
    // edges, not across faces.
    if (int(inst.MaterialParams.z) == 0) {
        vec3 absLocal = abs(localPos);
        float maxAxis = max(max(absLocal.x, absLocal.y), absLocal.z);

        vec2 uv;
        vec2 uvScale;
        if (maxAxis == absLocal.x) {
            // Fold the two opposite faces onto one UV pair with the sign, so a cube's
            // -X and +X faces share a texture rather than showing a mirror image.
            float signX = sign(localPos.x);
            uv = vec2(localPos.z * signX + 0.5f, localPos.y + 0.5f);
            uvScale = vec2(inst.TextureScale.z, inst.TextureScale.y);
        } else if (maxAxis == absLocal.y) {
            float signY = sign(localPos.y);
            uv = vec2(localPos.x * signY + 0.5f, localPos.z + 0.5f);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.w);
        } else {
            float signZ = sign(localPos.z);
            uv = vec2(localPos.x * signZ + 0.5f, localPos.y + 0.5f);
            uvScale = vec2(inst.TextureScale.x, inst.TextureScale.y);
        }

        return uv * uvScale;
    } else {
        // The non-cube branch: spherical for spheres, cylindrical for everything
        // else. Note this is the else of (shape == 0), so cones, cylinders, torus,
        // planes and discs ALL take the cylindrical path -- a plane gets angular
        // UVs from a flat quad.
        float phi = atan(localPos.z, localPos.x);
        float theta = acos(clamp(localPos.y, -1.0f, 1.0f));

        // atan returns [-pi, pi], so the +0.5 and /2pi map it to [0,1]. acos gives
        // theta in [0, pi] directly, so v needs no remap.
        float u = phi / (2.0f * PI) + 0.5f;
        float v = theta / PI;

        return vec2(u, v) * inst.TextureScale.xy;
    }
}

// PASS 0: the velocity buffer. Reads rasterised depth, reconstructs world
// position, reprojects through last frame's matrix, and stores the screen
// delta. This is what the temporal pass needs to reproject its history.
void RunVisibilityAndVelocity() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(pixelCoords) + 0.5f) / vec2(wl_trace_extent(imageSize(img_Output)));
    
    // Sample depth through the buffer-UV helper, not raw -- the trace G-buffer
    // occupies a subregion of its image, so the scaling is required.
    float depth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
    // Depth >= 1 means the sky (no geometry). Write zero velocity and return,
    // because a sky pixel has no surface to reproject and the temporal pass
    // must not treat it as static geometry.
    if (depth >= 1.0f) { 
        imageStore(img_Velocity, pixelCoords, vec4(0.0f, 0.0f, 0.0f, 1.0f));
        return;
    }

    // Clip-space reconstruction: UV and depth from [0,1] to [-1,1].
    // ---
    // Assumes a conventional [-1,1] depth range and the standard Y flip, so
    // the reconstruction is only correct for that projection convention --
    // Vulkan's [0,1] depth would need a different remap here.
    vec4 clipPos = vec4(uv * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
    // Unproject to world, then the perspective divide. The divide is what turns
    // the clip-space direction into a finite world position; skipping it
    // projects along the ray rather than to a point.
    vec4 worldPos = u_InverseViewProjection * clipPos;
    worldPos /= worldPos.w;

    // Reproject through the PREVIOUS frame's matrix. Using this frame's would
    // give zero velocity always, which is the single most common way to break
    // temporal accumulation.
    vec4 prevClipPos = u_PrevViewProjection * worldPos;
    prevClipPos /= prevClipPos.w;
    vec2 prevUV = prevClipPos.xy * 0.5f + 0.5f;

    // NOTE: intentionally the UNjittered grid UV. History is stored per-pixel
    // on that grid, so velocity = uv - prevUV lands the lookup on the texel
    // that saw this surface point. Biasing by the frame jitter (tempting,
    // tried) offsets the lookup by a per-frame-varying amount and smears.
    vec2 velocity = uv - prevUV;
    imageStore(img_Velocity, pixelCoords, vec4(velocity, 0.0f, 1.0f));
}

// The hit record threaded out of the tracing loop. One struct rather than
// a dozen out-params, which matters because it is written in the BVH leaf
// handler and read by every shading term.
// ---
// f0Tint defaults to 1.0 = 'classic dielectric F0'. It is the neural
// material's only channel into the BRDF, and its 1.0 default is what makes
// the neural path a no-op when disabled.
struct HitInfo {
    bool hit;
    float t;
    vec3 worldPos;
    vec3 normal;
    vec3 albedo;
    float metal;
    float rough;
    vec3 emission;
    float occlusion;
    vec3 f0Tint; // neural material specular tint, 1.0f = classic F0
};

// ---- Gerstner wave spectrum (matches WaterComponent::EvaluateSurface) ----
// 6 directional components, Phillips-style falloff, deep-water dispersion.
// Returns world-space height and normal perturbation at a world XZ position.
// baseDir = normalized primary swell direction; flowVel advects the sample
// (matches the CPU AdvectedPos) so rivers carry their waves downstream.
// Gerstner waves: 6 directional components summed, with a second harmonic
// for sharpening. Must match WaterComponent::EvaluateSurface on the CPU --
// the two implementations are a matched pair, and drifting them apart puts
// the analytic water mesh and the traced surface out of agreement.
void EvaluateGerstnerWaves(vec2 worldXZ, float time, float amplitude, float wavelength,
                           float speed, float chop, float steepness, float minWavelength,
                           vec2 baseDir, vec2 flowVel,
                           out float outHeight, out vec3 outNormal)
{
    outHeight = 0.0f;
    outNormal = vec3(0.0f, 1.0f, 0.0f);
    // Amplitude zero means flat water, and returning the identity normal here
    // is what every downstream term relies on to detect calm water.
    if (amplitude <= 0.00001f)
        return;

    // Spectrum tables (length ratios, amplitude shares, fan angles, phase offsets)
    // Mirrors CPU-side WaterComponent::SpectrumLengthRatio/Share/FanAngle/PhaseOffset
    float baseLen = max(wavelength, 0.1f);
    float t = time * speed;
    float steep = clamp(steepness, 0.0f, 1.0f);
    float chopK = clamp(chop, 0.0f, 1.0f);
    vec2 d1 = baseDir;
    if (dot(d1, d1) < 1e-8f)
        d1 = vec2(1.0f, 0.3f);
    d1 = normalize(d1);
    // Advect the sample position by the flow velocity, so waves travel
    // downstream in a river. This is the CPU's AdvectedPos equivalent -- the
    // comment above flags it as a deliberate match.
    vec2 p = worldXZ - flowVel * time;

    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    // Six components. A fixed literal bound rather than a uniform, so the
    // unroll is possible and the cost is predictable.
    // ---
    // The per-component tables below (length, share, fan angle, phase) are
    // if-chains rather than constant arrays. GLSL allows const arrays, so this
    // is a style choice -- but it does mean all six values are present as
    // branches the compiler may or may not fully resolve.
    for (int i = 0; i < 6; i++)
    {
        // Length ratio per component
        float lenRatio = 1.0f;
        if (i == 1) lenRatio = 0.55f;
        else if (i == 2) lenRatio = 0.32f;
        else if (i == 3) lenRatio = 0.19f;
        else if (i == 4) lenRatio = 0.11f;
        else if (i == 5) lenRatio = 0.06f;

        // This component's wavelength: the base scaled by its length ratio. The
        // ratios descend geometrically (1, .55, .32, .19, .11, .06), so energy
        // concentrates in the long swell and the tail is ripple.
        float li = baseLen * lenRatio;

        // Spectral LOD: fade components shorter than minWavelength
        float fade = 1.0f;
        // Spectral LOD: components shorter than minWavelength are dropped outright,
        // and one within a factor of two of it fades linearly. This is the banding
        // control -- without it, far-away water aliases into a moire grid.
        if (minWavelength > 0.00001f && li < minWavelength * 2.0f)
        {
            if (li <= minWavelength)
                continue;
            fade = (li - minWavelength) / minWavelength;
        }

        // Amplitude share per component
        float share = 0.55f;
        if (i == 1) share = 0.22f;
        else if (i == 2) share = 0.11f;
        else if (i == 3) share = 0.06f;
        else if (i == 4) share = 0.035f;
        else if (i == 5) share = 0.025f;

        // This component's amplitude. Component 0 (the primary swell) keeps full
        // amplitude; the others are scaled by chop, so turning chop to zero leaves
        // only the base wave -- a different surface, not the same one.
        float amp = amplitude * share * (i == 0 ? 1.0f : chopK) * fade;
        if (amp <= 0.0000001f)
            continue;

        float k = 6.2831853f / li;
        // Clamp the wavenumber to 40, then the angular frequency to 25. Deep-water
        // dispersion says omega = sqrt(g*k), so raising k raises omega without
        // bound -- and a 40x wavenumber gives visibly wrong phase speed at the
        // sampling rate this is marched at. Both clamps are stability guards.
        if (k > 40.0f) k = 40.0f;
        float omega = sqrt(9.81f * k);
        if (omega > 25.0f) omega = 25.0f;

        // Fan angle per component
        float angle = 0.0f;
        if (i == 1) angle = 0.40f;
        else if (i == 2) angle = -0.54f;
        else if (i == 3) angle = 0.82f;
        else if (i == 4) angle = -1.01f;
        else if (i == 5) angle = 1.24f;

        // Phase offset per component
        float phaseOff = 0.0f;
        if (i == 1) phaseOff = 1.3f;
        else if (i == 2) phaseOff = 4.1f;
        else if (i == 3) phaseOff = 2.2f;
        else if (i == 4) phaseOff = 5.0f;
        else if (i == 5) phaseOff = 0.7f;

        // Direction: fan from primary wave direction
        float ca = cos(angle);
        float sa = sin(angle);
        // Rotate the primary direction by the component's fan angle. The angles
        // are not evenly spread (0, +.40, -.54, +.82, -1.01, +1.24), which is what
        // gives the spectrum a directional character instead of a ring.
        vec2 dir = vec2(d1.x * ca - d1.y * sa, d1.x * sa + d1.y * ca);

        // Harmonic sharpness: the primary wave is softened to 0.6 and the rest are
        // full. norm then renormalises so the total amplitude is preserved when a
        // harmonic is added -- otherwise sharpening would also change wave height.
        float sharp = (i == 0 ? 0.6f : 1.0f) * chopK;
        float norm = 1.0f + 0.5f * sharp;
        // The phase, wrapped to [0, 2pi). The mod is what lets the CPU's time and
        // the shader's time agree without precision loss over a long session.
        float phi = mod(k * dot(dir, p) - omega * t, 6.2831853f);
        float s1 = sin(phi);
        float c1 = cos(phi);
        // The second harmonic at DOUBLE the phase frequency. This is what turns a
        // sinusoidal swell into the sharper, trochoidal crest a real ocean wave
        // has -- and it is the entire reason this is Gerstner and not a sum of
        // sines.
        float harmonic = 2.0f * phi + phaseOff;
        float s2 = sin(harmonic);
        float c2 = cos(harmonic);

        float wa = k * amp;
        // The Gerstner Q factor, which controls horizontal displacement. Clamped to
        // 1 because Q > 1 makes the parametric surface self-intersect at the
        // crests -- a classic Gerstner artefact.
        float qi = steep / (wa * 6.0f + 1e-6f);
        if (qi > 1.0f) qi = 1.0f;
        if (qi < 0.0f) qi = 0.0f;

        float shapeS = (s1 + 0.5f * sharp * s2) / norm;
        float shapeC = (c1 + sharp * c2) / norm;
        // Accumulate the height contribution and the normal perturbation.
        // ---
        // The normal is built from the DISPLACED wave's x/z gradient rather than
        // the undisplaced one, which is what makes steep waves shade correctly. ny
        // accumulates -Q*k*A*sin(phi), i.e. the horizontal compression folding into
        // the vertical tangent.
        outHeight += amp * shapeS;
        nx += -dir.x * wa * shapeC;
        nz += -dir.y * wa * shapeC;
        ny += -qi * wa * shapeS;
    }
    outNormal = normalize(vec3(nx, ny, nz));
}

// The tangent frame per shape. This exists so normal mapping, POM and
// detail all agree on a basis -- and it must stay in lockstep with
// CalculateUV, which is the pairing the cube comment below warns about.
void ComputeTangentFrame(vec3 localHitPos, vec3 localNormal, int shapeType, vec4 texScaleFull,
                         float shapeRadius, float shapeHalfHeight,
                         float shapeMajorRadius, float shapeMinorRadius,
                         out vec3 T, out vec3 B, out vec2 uvScale)
{
    // Plane / Grid / Circle: the normal is +Y, so the frame is world XZ and
    // uvScale matches CalculateUV's flat mapping.
    // Plane / Grid / Circle all share the +Y frame, so one branch covers three
    // shapes. Note Circle is grouped with the flat shapes here even though
    // CalculateUV sends it down the cylindrical branch -- an inconsistency
    // between the two functions for the same shape.
    if (shapeType == 2 || shapeType == 7 || shapeType == 6)
    {
        T = vec3(1.0f, 0.0f, 0.0f);
        B = vec3(0.0f, 0.0f, 1.0f);
        uvScale = vec2(texScaleFull.x, texScaleFull.z);
        return;
    }

    // Cylinder / Cone: tangent follows the surface circumferentially, the
    // bitangent runs along Y -- the basis the wall UVs are built from.
    if (shapeType == 3 || shapeType == 4)
    {
        // The circumferential tangent for a cylinder/cone wall, orthogonalised in
        // the xz plane. The 0.0001 length test catches the degenerate case where
        // the normal is straight up or down -- the cone apex.
        T = vec3(-localNormal.z, 0.0f, localNormal.x);
        float lenCyl = length(T);
        if (lenCyl < 0.0001f)
            T = vec3(1.0f, 0.0f, 0.0f);
        else
            T /= lenCyl;

        B = normalize(cross(localNormal, T));

        // UV scale in world units: circumference by height. Using real dimensions
        // rather than a flat constant keeps texel density even as the primitive is
        // resized in the editor.
        float circumference = 2.0f * PI * max(shapeRadius, 0.0001f);
        uvScale = vec2(circumference, 2.0f * max(shapeHalfHeight, 0.0001f));
        return;
    }

    // Torus: tangent along the ring, bitangent around the tube.
    if (shapeType == 5)
    {
        // The torus radial, reused as the tangent. The length guard again catches
        // the case where the normal is vertical -- the inner and outer equators.
        vec3 radial = vec3(localNormal.x, 0.0f, localNormal.z);
        float lenTorus = length(radial);
        if (lenTorus < 0.0001f)
            radial = vec3(1.0f, 0.0f, 0.0f);
        else
            radial /= lenTorus;

        T = radial;
        B = normalize(cross(localNormal, T));
        // UV scale for a torus: both axes are full circles, so the tube's minor
        // circumference scales the V axis.
        uvScale = vec2(2.0f * PI * max(shapeMajorRadius, 0.0001f),
                       2.0f * PI * max(shapeMinorRadius, 0.0001f));
        return;
    }

    if (shapeType == 1)
    {
        T = vec3(-localNormal.z, 0.0f, localNormal.x);
        float lenT = length(T);
        if (lenT < 0.0001f)
            T = vec3(1.0f, 0.0f, 0.0f);
        else
            T /= lenT;

        B = cross(localNormal, T);

        float radius = max(length(localHitPos), 0.0001f);
        float radiusXZ = length(localHitPos.xz);
        radiusXZ = max(radiusXZ, 0.05f * radius);

        // Spherical UV scale, which must be INVERSED against the radius -- the
        // bigger the sphere, the more repeats for the same texScale.
        // ---
        // The radiusXZ floor of 5% of the radius handles the poles: without it,
        // u wraps to infinity and the texture compresses into an infinitely
        // stretched singularity at the top and bottom.
        uvScale.x = (1.0f / (2.0f * PI * radiusXZ)) * texScaleFull.x;
        uvScale.y = (1.0f / (PI * radius)) * texScaleFull.y;
    }
    else
    {
        // Cube: mirror CalculateUV's per-face tiling so the POM march steps
        // in the same UV space the heights are sampled from. A flat texScale
        // here skews the march along one axis (e.g. floor top samples with
        // (x, w) = (50, 50) but marched with (x, y) = (50, 1)), producing
        // directional relief and occlusion banding.
        // The cube branch. The comment above explains why this must mirror
        // CalculateUV's per-face tiling exactly, and what the POM artifact looks
        // like when it does not.
        vec3 absN = abs(localNormal);

        if (absN.x > absN.y && absN.x > absN.z) {
            T = vec3(0.0f, 0.0f, sign(localNormal.x));
            B = vec3(0.0f, 1.0f, 0.0f);
            uvScale = vec2(texScaleFull.z, texScaleFull.y);
        } else if (absN.y > absN.x && absN.y > absN.z) {
            T = vec3(1.0f, 0.0f, 0.0f);
            B = vec3(0.0f, 0.0f, -sign(localNormal.y));
            uvScale = vec2(texScaleFull.x, texScaleFull.w);
        } else {
            T = vec3(-sign(localNormal.z), 0.0f, 0.0f);
            B = vec3(0.0f, 1.0f, 0.0f);
            uvScale = vec2(texScaleFull.x, texScaleFull.y);
        }
    }
}

// Relief height stack (Crimson-rock setup): texture macro-relief + procedural
// craggy breakup + strata banding + neural micro-surface. The linear march
// and binary refine sample the cheap macro height only; final/shadow/gradient
// taps use the full stack so detail rides on the macro silhouette.
// The CHEAP half of the relief stack: macro height from the albedo texture's
// luminance, nothing else.
// ---
// This is what the march and the refine sample. Using the full stack during
// the march would cost a noise evaluation plus a 4-unit MLP per step, which
// is why it is split -- the comment on wl_pom_full explains the trade.
float wl_pom_macro(int texID, vec2 tapUV)
{
    // Luminance as height, sampled at LOD 0. Using the green channel or the
    // texture's dedicated height would be more correct, but the scene textures
    // have no separate height channel -- so macro relief is whatever the
    // albedo is bright.
    return GetLuminance(wl_scene_tex_lod(texID, tapUV, 0.0f).rgb);
}

// The FULL height stack: macro + meso breakup + strata banding + neural
// micro. Used only for the final, shadow and gradient taps.
// ---
// The 1.5 clamp on the result matters: the summed layers can exceed 1.0,
// and an unclamped height would push geometry further than the intended
// dispScale in the silhouette pass.
float wl_pom_full(int texID, vec2 tapUV, vec3 wpBase, vec3 nRef,
                  float mesoAmp, float strataW, float neuralK)
{
    float h = wl_pom_macro(texID, tapUV);
    if (mesoAmp > 0.001f)
    {
        vec2 muv = tapUV * 6.0f;
        float n1 = wl_vol_noise(vec3(muv, 3.7f));
        float n2 = wl_vol_noise(vec3(muv * 2.3f + 5.0f, 9.1f));
        // Strata: horizontal banding as a sine of WORLD height, phase-broken by the
        // meso noise. Using world y (not UV) is what makes the bands stay level
        // across a sloped surface, which is the point of strata.
        float st = sin(wpBase.y * 5.0f + n1 * 4.0f) * 0.5f + 0.5f;
        // Two noise octaves at 70/30 weighting, centred on 0 by the -0.5 offsets,
        // so meso detail adds and subtracts symmetrically about the macro height.
        h += ((n1 - 0.5f) * 0.7f + (n2 - 0.5f) * 0.3f) * mesoAmp
           + (st - 0.5f) * mesoAmp * 0.8f * strataW;
    }
    if (neuralK > 0.001f)
        h += wl_neural_micro(tapUV, nRef) * 2.0f * neuralK;
    return clamp(h, 0.0f, 1.5f);
}

// Parallax occlusion mapping with a real geometric push. Takes the world
// position and normal INOUT because relief displaces the shading point --
// this is not a UV-only effect, it moves geometry.
void ApplyPOM(RayTracingInstance inst, vec3 localHitPos, vec3 localNormal,
              vec3 localViewDir, inout vec2 uv, inout vec3 worldPos,
              inout vec3 worldNormal, out float selfOcclusion, out float dispPush,
              out float outHeight)
{
    selfOcclusion = 1.0f;
    dispPush = 0.0f;
    outHeight = -1.0f;

    // POM is entirely off below Medium quality. Not reduced -- disabled. So
    // Low and Medium render bump-free, which is the cheapest way to cut the
    // per-pixel cost of a feature that needs many texture taps.
    if (u_QualityLevel <= 1)
        return;

    vec3 V = normalize(localViewDir);
    float distToCam = length(worldPos - u_CameraPosition);
    // abs(dot(V, N)) -- the absolute value so a back-facing surface is treated
    // as front-facing. The local space here has no notion of which side of the
    // mesh was hit.
    float cosView = abs(dot(V, localNormal));

    // Perf tier: pull POM range down when tracing below full resolution
    // (dynamic resolution on older GPUs) — distFade already zeroes detail at maxDist.
    float pomPerfK = smoothstep(0.6f, 0.9f, u_RenderScale);
    // Range gate. High quality gets a fixed 25m; Ultra blends 35->50m with the
    // perf tier, so the full march is affordable only where resolution is full.
    // ---
    // Note Ultra is FASTER than High here (50m vs 25m) but High gets MORE march
    // layers below. The two budgets are tuned separately.
    float maxDist = (u_QualityLevel == 2) ? 25.0f : mix(35.0f, 50.0f, pomPerfK);
    if (distToCam > maxDist)
        return;
    // Grazing views skip the UV march (the view-ray projection blows up
    // edge-on) but still push + perturb from the center height below, so
    // relief never vanishes side-on.
    // The grazing gate at cos 0.15. Below it the view-ray UV projection tends
    // to infinity and the march smears across the surface.
    // ---
    // The comment above is explicit that this is a partial gate: the march is
    // skipped but the displacement push below still runs. So relief survives at
    // grazing angles as a centre-height bulge rather than parallax.
    bool doMarch = (cosView >= 0.15f);
    // Angular march weight: the transition band above the gate still projects
    // each step across many texels (smear streaks), so collapse the offset
    // toward the center tap smoothly instead of switching hard.
    // A smooth version of that gate, from 0.10 to 0.45. The transition band
    // still marches, but with the offset scaled toward zero -- so the artifact
    // is a smooth fade to the centre tap rather than a hard pop at the gate.
    float marchK = smoothstep(0.10f, 0.45f, cosView);
    float marchLOD = (1.0f - marchK) * 2.0f;

    float dispScale = inst.DisplacementParams.x;
    float bumpStrength = inst.DisplacementParams.y;
    // The texture index bound check. This is the ONE place the shader validates
    // it -- which is why the unchecked sampler index on RayTracingInstance
    // documented earlier does not fault here: the march returns first.
    if (inst.TextureID < 0 || inst.TextureID >= 32)
        return;
    if (dispScale < 0.001f && bumpStrength < 0.001f)
        return;

    int shapeType = int(inst.MaterialParams.z);
    vec3 T, B;
    vec2 uvScale;
    // Note the same value is passed for radius AND majorRadius, and Min.w for
    // halfHeight AND minorRadius. The cylinder/cone branch reads the first pair
    // and the torus branch the second, so the duplication is harmless but
    // means a torus's dimensions are read from two fields that happen to be
    // equal here.
    ComputeTangentFrame(localHitPos, localNormal, shapeType, inst.TextureScale,
                        inst.MaterialParams.w, inst.Min.w,
                        inst.MaterialParams.w, inst.Min.w,
                        T, B, uvScale);

    int texID = inst.TextureID;
    // The gradient epsilon, 1/256 -- one texel of a 256-tall texture. Central
    // differences would be 2x more stable but need 4 taps; forward
    // differences need 3 total including the centre, which is what this uses.
    float eps = 1.0f / 256.0f;

    // Meso setup rides on Displacement Scale only (opt-in): existing scenes
    // with bump-only materials render pixel-identical to before.
    vec3 wpBase = worldPos;
    vec3 wNorm0 = normalize(worldNormal);
    float mesoAmp = 0.0f;
    // Strata weight, strongest where the surface is VERTICAL. The clamp keeps
    // it at 0.15 even on a horizontal face rather than zeroing it, so a floor
    // still shows faint layering instead of none.
    float strataW = clamp(1.0f - abs(wNorm0.y), 0.15f, 1.0f);
    float neuralK = (u_NeuralEnabled == 1) ? clamp(u_NeuralTexStrength, 0.0f, 1.0f) : 0.0f;

    float maxDisplacement = max(dispScale, bumpStrength);
    float cosAngle = max(dot(V, localNormal), 0.05f);
    
    // Base march layers: 6 at High, 8->16 at Ultra with the perf tier.
    // ---
    // A literal-count ladder keyed on u_QualityLevel, not a smooth scale. So
    // quality changes are step functions in both cost and appearance.
    int baseLayers = (u_QualityLevel == 2) ? 6 : int(mix(8.0f, 16.0f, pomPerfK));
    int maxLayers  = (u_QualityLevel == 2) ? 16 : int(mix(20.0f, 32.0f, pomPerfK));
    // Layer count stretched by the view angle: a grazing view needs more steps
    // because the same world depth spans more UV. Clamped to maxLayers, which
    // is what stops the near-grazing case from exploding.
    int numLayers  = int(clamp(float(baseLayers) / cosAngle, float(baseLayers), float(maxLayers)));

    float vnDot = dot(V, localNormal);
    vec2 viewUVDir = vec2(dot(V, T), dot(V, B));
    // The UV displacement per unit of height. Divided by |N.V| because a
    // shallow view angle covers more UV per unit of height -- the 0.05 floor
    // bounds it at the extreme.
    vec2 uvOffsetPerHeight = -viewUVDir / max(abs(vnDot), 0.05f);

    // Distance fade over the last 15m before maxDist. Without it, relief would
    // pop out of existence exactly at the range gate instead of fading.
    float distFade = clamp(1.0f - (distToCam - (maxDist - 15.0f)) / 15.0f, 0.0f, 1.0f);
    dispScale *= distFade;
    bumpStrength *= distFade;
    mesoAmp = 0.25f * clamp(dispScale, 0.0f, 1.5f);

    float dHeight = 1.0f / float(numLayers);
    // March in geometric-depth units: the step must scale with dispScale
    // (world meters of relief), NOT max(disp, bump). Bump-only materials
    // carry relief in the gradient normal; giving them full-depth parallax
    // spans dozens of tiles (uvScale ~50 on a 50 m wall) and swims with
    // the view. (Raster Basic already marches on disp only.)
    // The march step, in UV per layer per world unit of relief.
    // ---
    // The long comment above records a fixed defect class here: the step must
    // scale with dispScale ALONE. Including bumpStrength gave bump-only
    // materials full-depth parallax, which spans dozens of tiles at large
    // uvScale and swims as the view moves. Bump-only surfaces carry their
    // relief in the gradient normal instead.
    vec2 dUV = (uvOffsetPerHeight * uvScale) * dHeight * dispScale;

    vec2 prevUV = uv;
    float prevLayerH = 1.0f;
    float currLayerH = 1.0f;
    // The initial height tap, at the marched LOD rather than 0. This is the
    // same texture the march samples throughout, so the march cannot jump
    // between mip levels and produce a false intersection.
    float prevTexH = GetLuminance(wl_scene_tex_lod(texID, uv, marchLOD).rgb);
    vec2 currUV = uv;
    float currTexH = prevTexH;

    if (doMarch)
    // The linear march: advance the layer and the UV together, stop when the
    // texture height rises above the layer depth -- that is the surface.
    // ---
    // Note prevLayerH starts at 1.0 and currLayerH also at 1.0, so the very
    // first comparison is against a degenerate zero-height layer. That is the
    // intended convention (the layer depth counts down from 1.0 to 0.0) and
    // it is why a surface at height 0 is hit on the first step.
    for (int i = 0; i < numLayers; i++)
    {
        prevUV = currUV;
        prevLayerH = currLayerH;
        prevTexH = currTexH;

        currUV += dUV;
        currLayerH -= dHeight;
        currTexH = GetLuminance(wl_scene_tex_lod(texID, currUV, marchLOD).rgb);

        if (currTexH > currLayerH)
            break;
    }

    // Binary refine: crisp heightfield intersections for deep relief instead
    // of the linear-interp smear. Macro sampler only (cheap, silhouette-grade).
    int refineSteps = (u_QualityLevel >= 3) ? 5 : 3;
    if (doMarch)
    // Binary refine between the last accepted and first rejected pair. Fixed
    // 5 iterations with a runtime cap, because GLSL wants a constant loop
    // bound -- refineSteps is 5 at Ultra and 3 below.
    for (int b = 0; b < 5; b++)
    {
        if (b >= refineSteps)
            break;
        vec2 midUV = (prevUV + currUV) * 0.5f;
        float midH = wl_pom_macro(texID, midUV);
        float midL = (prevLayerH + currLayerH) * 0.5f;
        if (midH > midL)
        {
            currUV = midUV;
            currLayerH = midL;
            currTexH = midH;
        }
        else
        {
            prevUV = midUV;
            prevLayerH = midL;
            prevTexH = midH;
        }
    }

    // March skipped (grazing): pomUV stays on the center tap, weight is moot.
    vec2 pomUV = uv;
    float weight = 0.5f;
    if (doMarch)
    {
        // The parallax blend weight from the bracketing pair's signed distances.
        // Linear interpolation between the two samples -- standard, and the reason
        // the refine pass above is needed to make it look crisp.
        float d1 = prevLayerH - prevTexH;
        float d2 = currTexH - currLayerH;
        weight = d1 / max(d1 + d2, 0.0001f);
        pomUV = mix(prevUV, currUV, weight);
    }

    // The final UV offset, scaled by the angular weight so a grazing surface
    // barely shifts at all.
    vec2 uvDelta = (pomUV - uv) * marchK;
    float maxOffset = 0.15f;
    // A hard clamp on the UV offset to 0.15 -- about 15% of a tile. This is a
    // safety rail against the grazing blow-up, independent of marchK. Past
    // this the texture would sample from a visibly wrong place.
    if (length(uvDelta) > maxOffset)
        uvDelta = normalize(uvDelta) * maxOffset;

    uv += uvDelta;

    // The blended height at the chosen UV. Used as the CENTRE for the gradient
    // differences below, so bump and displacement agree on where the surface is.
    float finalHeight = mix(prevTexH, currTexH, weight);
    if (dispScale > 0.001f)
    {
        // Detail rides on the macro silhouette: enrich after the march.
        // Displaced surfaces re-evaluate the FULL stack, not the macro only. This
        // is the 'detail rides on the macro silhouette' idea: the march finds where
        // the macro surface is, then the detail layers are added on top of that
        // position.
        finalHeight = wl_pom_full(texID, uv, wpBase, wNorm0, mesoAmp, strataW, neuralK);
        // Shape-preserving push: bilateral around the mid-level so the mesh
        // keeps its base shape — features rise AND recess instead of the whole
        // surface ballooning outward by up to full dispScale.
        // The geometric push, along the world normal by (height - 0.5) * dispScale.
        // Centred on 0.5 so features rise AND recess by equal amounts -- see the
        // comment above: a 0-based push would balloon the whole surface outward.
        worldPos += worldNormal * (finalHeight - 0.5f) * dispScale;
        dispPush = (finalHeight - 0.5f) * dispScale;
        outHeight = finalHeight;
    }

    if (bumpStrength > 0.001f)
    {
        float hU = wl_pom_full(texID, uv + vec2(eps, 0.0f), wpBase, wNorm0, mesoAmp, strataW, neuralK);
        float hV = wl_pom_full(texID, uv + vec2(0.0f, eps), wpBase, wNorm0, mesoAmp, strataW, neuralK);
        // Forward differences against the centre height. Note the divisor is eps,
        // not 2*eps, because this is a forward difference.
        float dhdu = (hU - finalHeight) / eps;
        float dhdv = (hV - finalHeight) / eps;

        // UV-space gradient -> world-space gradient. The engine tiles ~1 texture
        // per meter (cube TextureScale = world scale, sphere scales are
        // circumference-based), so UV-per-world is ~1 and NO uvScale factor
        // belongs here. Multiplying by uvScale (50x on a 50m floor, up to 20x
        // near sphere poles) saturated normals into garbage and blackened
        // everything POM touched. (uvScale IS still correct for the march
        // step above, which walks in UV units.)
        // The gradient is treated as ALREADY in world units.
        // ---
        // The long comment above is a fixed-defect record: an earlier version
        // multiplied by uvScale here, which saturated the normals to garbage and
        // blackened everything POM touched at large texture scales. uvScale is
        // still correct for the march step, which genuinely walks in UV.
        float dh_dsT = dhdu;
        float dh_dsB = dhdv;

        vec3 bumpGrad = (dh_dsT * T + dh_dsB * B) * bumpStrength;
        // Subtract the gradient from the normal. The minus sign is the bump-map
        // convention: a height that rises along +T tilts the normal toward -T.
        vec3 localPerturbedNormal = localNormal - bumpGrad;
        float bumpLen = length(bumpGrad);
        // Cap the perturbation: with large texture scales (e.g. a 50m floor
        // with uvScale 50) the UV-space gradient saturates into garbage
        // normals and near-black shading. Direction kept, magnitude capped.
        if (bumpLen > 1.0f)
            localPerturbedNormal = localNormal - bumpGrad / bumpLen;
        localPerturbedNormal = normalize(localPerturbedNormal);
        mat3 normalMatrix = transpose(mat3(inst.InvTransform));
        worldNormal = normalize(normalMatrix * localPerturbedNormal);

        float gradMag = length(vec2(dh_dsT, dh_dsB));
        // Ridge self-occlusion from the gradient magnitude, weighted by how far the
        // height is ABOVE the midpoint -- so a ridge darkens and a dent does not.
        float ridgeFactor = max(0.0f, finalHeight - 0.5f) * gradMag;
        // Clamped to a 0.2 floor, so relief never blackens a surface completely.
        // The 0.04 is a tuning constant, not a physical term.
        selfOcclusion = clamp(1.0f - ridgeFactor * 0.04f * bumpStrength, 0.2f, 1.0f);
    }

    // Deep-crevice sun shadow: short heightfield march toward the sun so
    // crags cast onto each other. Displacement-gated (opt-in look change).
    if (dispScale > 0.001f)
    {
        // The sun direction in LOCAL space, so the heightfield shadow march can
        // work in the tangent frame below. The sun is directional, so translation
        // does not matter and only the rotation half of InvTransform is used.
        vec3 localSun = normalize(mat3(inst.InvTransform) * u_SunDirection);
        vec2 sunUV = vec2(dot(localSun, T), dot(localSun, B));
        float sunZ = dot(localSun, localNormal);
        // Skip if the sun is below the tangent plane. sunZ near zero means a
        // grazing sun, where the heightfield projection would be degenerate -- the
        // same reason the main POM march has its own cosine gate.
        if (sunZ > 0.05f)
        {
            int shSteps = (u_QualityLevel >= 3) ? 6 : 4;
            // The shadow step, scaled by the maximum displacement and split across the
            // steps. The 0.05 is 5% of the relief depth -- the march covers a small
            // fraction of the height range, since only nearby crags occlude.
            vec2 shStep = (sunUV / sunZ) * (maxDisplacement * 0.05f / float(shSteps));
            if (length(shStep) > 0.1f)
                shStep = normalize(shStep) * 0.1f;
            // The ray starts just ABOVE the surface (the +0.02) so the first sample
            // cannot self-shadow at t=0, and rises by the remaining height divided
            // across the steps -- a linear ramp toward the top of the heightfield.
            float rayH = finalHeight + 0.02f;
            float rayRise = (1.0f - finalHeight + 0.05f) / float(shSteps);
            float occ = 1.0f;
            vec2 suv = uv;
            for (int s = 0; s < 6; s++)
            {
                if (s >= shSteps)
                    break;
                suv += shStep;
                rayH += rayRise;
                float sh = wl_pom_full(texID, suv, wpBase, wNorm0, mesoAmp, strataW, neuralK);
                // The minimum clearance over the march, tightened each step. The *6.0
                // converts a height difference into a 0..1 shadow term with a fairly soft
                // edge.
                occ = min(occ, clamp((rayH - sh) * 6.0f, 0.0f, 1.0f));
            }
            selfOcclusion *= clamp(occ, 0.25f, 1.0f);
        }
    }
}

// The closest-hit instance test. Unlike TestInstanceOcclusion above, this
// writes a full HitInfo and must keep the NEAREST hit -- which is why the
// update is guarded by tWorld < info.t.
// ---
// isPrimaryRay gates POM. Secondary (bounce/shadow) rays skip the
// displacement push: they would need it for consistency, but the cost per
// bounce is prohibitive, so relief is effectively primary-only.
void TestInstanceHit(Ray ray, int instIdx, bool isPrimaryRay, inout HitInfo info) {
    RayTracingInstance inst = Instances[instIdx];

    vec3 camDiff = u_CameraPosition - inst.WorldTransform[3].xyz;
    if (dot(camDiff, camDiff) > inst.MaxDistance * inst.MaxDistance) return;

    Ray localRay;
    localRay.Origin = (inst.InvTransform * vec4(ray.Origin, 1.0f)).xyz;
    localRay.Direction = normalize((inst.InvTransform * vec4(ray.Direction, 0.0f)).xyz);
    
    vec3 safeDir = sign(localRay.Direction) * max(abs(localRay.Direction), vec3(0.00001f));
    vec3 localInvDir = 1.0f / safeDir;

    if (!RayAABB(localRay, localInvDir, inst.Min.xyz, inst.Max.xyz)) return;
    vec3 localNormal;
    float tLocal = HitPrimitive(localRay, inst, localNormal);
    if (tLocal > 0.0f) {
        vec3 localHitPos = localRay.Origin + tLocal * localRay.Direction;
        vec3 worldHit = (inst.WorldTransform * vec4(localHitPos, 1.0f)).xyz;
        // World-space distance from the ray origin. Note this is the distance to
        // the hit POINT, not the local t scaled by anything -- correct for a
        // uniform-scale transform, approximate for a non-uniform one.
        float tWorld = distance(ray.Origin, worldHit);
        if (tWorld < info.t) {
            info.t = tWorld;
            info.hit = true;
            info.worldPos = worldHit;
            // Local normal to world, via the transpose of the rotation part of
            // InvTransform. For a rotation this equals the forward transform's upper 3x3;
            // for a non-uniform scale the transpose is the correct inverse-transpose.
            // ---
            // The vec4(..., 0.0f) on the left is row-vector multiply order, matching
            // the column-vector transforms used everywhere else in this shader.
            info.normal = normalize((vec4(localNormal, 0.0f) * inst.InvTransform).xyz);

            vec2 uv = CalculateUV(localHitPos, inst);

            float pomH = -1.0f;
            if (isPrimaryRay)
            {
                float pomOcclusion = 1.0f;
                float pomPush = 0.0f;
                // The POM call. The view direction is negated because the local ray points
                // FROM the surface back toward the eye is the opposite of the march's
                // convention.
                ApplyPOM(inst, localHitPos, localNormal, -localRay.Direction, uv, info.worldPos, info.normal, pomOcclusion, pomPush, pomH);
                info.occlusion = pomOcclusion;
                // Screen-space displacement: re-anchor the POM hit onto the
                // primary world ray. The UV march + normal push reconstruct
                // the relief point approximately (exact only for planar,
                // uniformly-scaled surfaces); on spheres / scaled instances
                // it drifts off-ray, corrupting trace depth, secondary-ray
                // origins, and temporal stability. Intersecting the pixel ray
                // with the tangent plane through the pushed point keeps the
                // height estimate but guarantees screen-space correctness.
                // No extra texture taps. Signed push: raised features anchor
                // in front of the base hit, dented recesses slightly behind.
                float pomPushAbs = abs(pomPush);
                if (pomPushAbs > 0.00001f)
                {
                    float pomDenom = dot(ray.Direction, info.normal);
                    if (abs(pomDenom) > 1e-4)
                    {
                        // Re-anchor the displaced point onto the primary ray by intersecting with
                        // the tangent plane through it. The comment above explains why: the UV
                        // march's reconstruction drifts off-ray on spheres and scaled instances,
                        // which would corrupt trace depth, bounce origins and temporal stability.
                        float tAnchor = dot(info.worldPos - ray.Origin, info.normal) / pomDenom;
                        // Accept the re-anchor only if it stays in front of the eye AND does not
                        // push the hit beyond its own displacement allowance. The +0.001 slack
                        // absorbs float error on a flat-on tangent plane, where the intersection is
                        // numerically ill-conditioned.
                        if (tAnchor > 0.0f && tAnchor < info.t + pomPushAbs + 0.001f)
                        {
                            info.t = tAnchor;
                            info.worldPos = ray.Origin + ray.Direction * tAnchor;
                        }
                    }
                }
            }

            float dist = distance(worldHit, u_CameraPosition);
            // LOD from texel DENSITY (tiles per meter), not raw TextureScale:
            // scale grows with object size (50m floor tiles 50x) but density
            // is ~1/m like a unit cube. Raw scale pushed big surfaces to max
            // LOD meters from the camera, erasing all texture detail.
            // (Axis-aligned approx; rotation ignored.)
            // Per-axis world scale from the transform's column lengths. This is an
            // AXIS-ALIGNED approximation -- the comment above flags that rotation is
            // ignored, so a rotated non-cube object's extent is over-estimated.
            vec3 wAxis = vec3(length(inst.WorldTransform[0].xyz),
                              length(inst.WorldTransform[1].xyz),
                              length(inst.WorldTransform[2].xyz));
            vec3 wSize = max((inst.Max.xyz - inst.Min.xyz) * wAxis, vec3(0.0001f));
            float worldExtent = max(max(wSize.x, wSize.y), wSize.z);
            // Texel DENSITY: tiles per world unit, not the raw TextureScale.
            // ---
            // The long comment above records why this matters: using raw scale sent
            // large surfaces to maximum LOD within metres of the camera and erased all
            // their texture detail. Dividing by world extent makes a 50m floor with
            // 50x scale compute the same density as a unit cube with 1x.
            float scaleLen = max(length(inst.TextureScale.xy) / worldExtent, 0.0001f);
            // Gentle distance ramp (holds LOD 0 ~4x farther out) + extra blur
            // at grazing angles, where one isotropic LOD can't cover the
            // stretched footprint and shimmer/RGB striping appears.
            // (textureLod bypasses driver anisotropy, so compensate manually.)
            float ndv = abs(dot(info.normal, ray.Direction));
            float grazing = 1.0f - clamp(ndv, 0.0f, 1.0f);
            // The mip level from texel footprint, with a grazing-angle penalty.
            // --
            // The 0.008 constant is a screen-size assumption baked in. The
            // grazing*grazing*2.0 term exists because textureLod bypasses the driver's
            // anisotropy filter, so a stretched footprint has to be compensated for
            // manually -- without it, grazing surfaces show shimmer and RGB striping.
            float mipLevel = log2(max(dist * scaleLen * 0.008f, 0.0001f)) + grazing * grazing * 2.0f;
            // Clamp to 12, not 4.
            // --
            // The upper bound used to be a hardcoded 4.0f, which made the top of every
            // mip chain UNREACHABLE: NVRHITexture.cpp's ComputeMipLevels derives the
            // chain length from the largest axis and UploadMipChain fills all of it,
            // but every sample asked for at most level 4.
            // --
            // MEASURED on this repo's actual assets, so the size is honest rather than
            // assumed: the shipped textures are metal_floor/wood_floor 225x225 (8 levels),
            // wood_wall 447x447 (9), Frame 64x64 (7), Checkerboard 8x8 (4). Total chain
            // 1.55 MB, of which the levels above 4 come to ~0.002 MB. So this is NOT a
            // 33 MB memory problem -- that figure assumed 2048^2 assets, which this repo
            // does not have, and it was wrong.
            // --
            // The defect is therefore correctness-of-intent, not VRAM: the clamp was
            // documented in the HLSL twin as "a deliberate quality/performance limit",
            // but a limit that is invisible to the code that allocates the chain is an
            // accident that acquired a justification. Widening it means a distant
            // surface can now legitimately reach a coarser level than before.
            // --
            // Why 12 specifically: it is the level count of a 4096x4096 texture, the
            // largest the sampler's 16x anisotropy and the engine's assets imply, and
            // hardware clamps an explicit LOD to the chain it was given. Requesting
            // past the real chain returns the coarsest level, so an over-generous
            // constant is safe where an under-generous one silently blurs. The
            // per-texture maximum is not plumbed through as a uniform because that
            // would mean extending NovaConstants on all four backends -- and
            // DX11Nova's UAV/SRV binding tables are fixed-size literals, so a new
            // binding is a per-backend change with real collision risk for a bound
            // that is safe to over-specify.
            //
            // Widening this clamp is not free visually: levels above 4 are blurrier,
            // so distant surfaces lose detail they previously kept. That is the
            // correct trade -- the alternative was allocating the data and never
            // sampling it -- but it is why this lands AFTER the denoiser work, where
            // extra high-frequency detail has something to absorb the variance.
            mipLevel = clamp(mipLevel, 0.0f, 12.0f);

            vec3 sampledAlbedo = vec3(1.0f);
            // The ONE bounds check on the texture index in the whole hit path. An
            // out-of-range id leaves albedo at the white default rather than indexing
            // the sampler array out of bounds.
            if (inst.TextureID >= 0 && inst.TextureID < 32)
                sampledAlbedo = wl_scene_tex_lod(inst.TextureID, uv, mipLevel).rgb;

            info.albedo = sampledAlbedo * inst.Albedo.rgb;
            // Water marker: skip neural blocks for water (waves handle their own shading)
            // The water flag, read from DisplacementParams.z. Declared before both
            // neural blocks so each can skip itself for water -- waves do their own
            // shading and the neural terms would double-apply detail to it.
            bool wlWater = (inst.DisplacementParams.z > 0.5f);
            // Neural texture detail: MLP-synthesized micro-surface (OpenGL neural path)
            if (!wlWater && u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001f)
                info.albedo *= mix(vec3(1.0f), wl_neural_tex_detail(uv, info.normal), clamp(u_NeuralTexStrength, 0.0f, 1.0f));
            info.metal = clamp(inst.MaterialParams.x, 0.0f, 1.0f);
            // Roughness from MaterialParams.y, with a 0.5 default for a zero value.
            // A zero roughness is physically a mirror and numerically dangerous (the
            // GGX denominator), so a material that never set roughness gets a mid-grey
            // one instead.
            info.rough = inst.MaterialParams.y > 0.0f ? inst.MaterialParams.y : 0.5f;
            info.f0Tint = vec3(1.0f);
            // Neural material: learned PBR params feed the GGX BRDF (OpenGL neural path)
            if (!wlWater && u_NeuralEnabled == 1 && u_NeuralMatStrength > 0.001f)
            {
                float nmk = clamp(u_NeuralMatStrength, 0.0f, 1.0f);
                vec3 nma = info.albedo;
                float nmm = info.metal;
                float nmr = info.rough;
                vec3 nmf = vec3(1.0f);
                wl_neural_material(uv, info.worldPos, info.normal, nma, nmm, nmr, nmf);
                // Blend the neural proposal into the real material by nmk, rather than
                // replacing it. So a strength of 0 is a true no-op and the feature is
                // continuous -- the same pattern as the other neural gates.
                info.albedo = mix(info.albedo, nma, nmk);
                info.metal = mix(info.metal, nmm, nmk);
                info.rough = mix(info.rough, nmr, nmk);
                info.f0Tint = mix(vec3(1.0f), nmf, nmk);
            }

            // ---- Water: Gerstner swell + ripple detail + whitecaps ----
            // Water marker: DisplacementParams.z > 0.5 flags water surfaces.
            // The geometric box stays flat (no per-pixel ray re-intersection);
            // realism comes from an accurate wave normal driving the mirror
            // reflection, pixel-level ripple octaves for close-up glitter, and
            // noise-broken crest foam. Packed auxiliaries (see DrawWaterPlane
            // Nova path): TextureScale.xy = swell dir, .zw = flow velocity,
            // DisplacementParams.x = foam amount.
            // The water branch. Everything here is NORMAL perturbation plus foam:
            // there is no per-pixel ray re-intersection, so the geometry stays the flat
            // box. That is the central trade of the whole water implementation.
            if (wlWater)
            {
                float wAmplitude = inst.WaveParams.x;
                float wLength = inst.WaveParams.y;
                float wSpeed = inst.WaveParams.z;
                float wChop = clamp(inst.WaveParams.w, 0.0f, 1.0f);
                // Steepness rides in MaterialParams.w (free for cubes);
                // .z stays 0 so the shape discriminator keeps HitCube.
                // Wave steepness borrows MaterialParams.w. For a cube that field is already
                // the sphere radius, which is free because the water marker forces shape
                // back to 0 (a cube) anyway -- so the two features cannot collide.
                float wSteep = inst.MaterialParams.w;
                vec2 wBaseDir = inst.TextureScale.xy;
                vec2 wFlow = inst.TextureScale.zw;
                float wFoamAmt = clamp(inst.DisplacementParams.x, 0.0f, 1.0f);
                float wDist = distance(info.worldPos, u_CameraPosition);
                // Up-facing weight: only the surface skin takes wave normals;
                // the thin-box sides keep their geometric normal.
                // Only up-facing surfaces take the wave normal. A water box's thin sides
                // keep their geometric normal, which is what stops the sides from looking
                // like they are rippling too.
                float wUpWeight = clamp(info.normal.y, 0.0f, 1.0f);
                // Quality-scaled distance cutoff: skip waves on far water
                // The wave distance cutoff, 30/50/80/120m by preset. Beyond it the water is
                // left as a flat sheet -- the waves are sub-pixel out there and would alias.
                // ---
                // Note the initialiser is 80.0 and every branch overwrites it, including
                // the else. Harmless but the initial value is never used.
                float waveMaxDist = 80.0f;
                if (u_QualityLevel == 0) waveMaxDist = 30.0f;
                else if (u_QualityLevel == 1) waveMaxDist = 50.0f;
                else if (u_QualityLevel == 2) waveMaxDist = 80.0f;
                else waveMaxDist = 120.0f;
                float wH = 0.0f;
                vec3 wN = vec3(0.0f, 1.0f, 0.0f);
                if (wDist < waveMaxDist && wUpWeight > 0.001f)
                {
                    // Distance-based spectral LOD: fade high-frequency
                    // components that would alias at this distance
                    // (matches CPU-side LOD)
                    // The spectral LOD distance: wavelength shorter than 20% of the view
                    // distance is dropped. This mirrors the CPU-side LOD so the analytic
                    // water mesh and the traced surface agree on which components survive.
                    float minWavelength = wDist * 0.2f;
                    EvaluateGerstnerWaves(info.worldPos.xz, u_Time, wAmplitude, wLength,
                                          wSpeed, wChop, wSteep, minWavelength,
                                          wBaseDir, wFlow, wH, wN);
                    // Blend-replace: the old add-and-halve flattened swell
                    // (up + up = up), hiding waves from reflections.
                    // mix() rather than add: the wave normal REPLACES the geometric one
                    // proportionally.
                    // ---
                    // The comment above records the bug this fixed: adding the wave normal to
                    // the up normal gave (up + up) = up, so a flat surface stayed flat and
                    // waves were invisible in reflections.
                    info.normal = normalize(mix(info.normal, wN, wUpWeight));
                }
                // Ripple detail: pixel-level chop octaves (mirrors the Basic
                // raster path) so the mirror breaks into sun glitter near the
                // camera instead of staying a flat sheet.
                vec2 wRippleGrad = vec2(0.0f);
                // Ripple fade: full detail inside 25m, gone by 85m, times the up-facing
                // weight. So ripples never appear on the box's sides even close up.
                float wRippleFade = clamp(1.0f - (wDist - 25.0f) / 60.0f, 0.0f, 1.0f) * wUpWeight;
                if (wRippleFade > 0.001f && wChop > 0.001f)
                {
                    vec2 ruv = info.worldPos.xz - wFlow * u_Time;
                    float wt = u_Time;
                    wRippleGrad += vec2(0.96, 0.28) * (cos(dot(vec2(0.96, 0.28), ruv) * 2.1 + wt * 1.6) * 0.22);
                    wRippleGrad += vec2(-0.42, 0.91) * (cos(dot(vec2(-0.42, 0.91), ruv) * 3.7 - wt * 2.3 + 1.7) * 0.13);
                    wRippleGrad += vec2(0.66, -0.75) * (cos(dot(vec2(0.66, -0.75), ruv) * 7.9 + wt * 3.4 + 4.2) * 0.07);
                    float wRippleStrength = 0.55 * wRippleFade * (0.25f + 0.75f * wChop);
                    info.normal = normalize(info.normal + vec3(-wRippleGrad.x, 0.0f, -wRippleGrad.y) * wRippleStrength);
                }
                // Whitecaps: crest-gated, broken by advected cell noise so foam
                // reads as patches instead of clean bands. FoamAmount gates
                // coverage, so calm lakes stay glassy.
                float wMaxH = max(inst.DisplacementParams.w, 0.05f);
                // The crest gate: foam only appears in the top 50-90% of the wave height
                // range. Below wMaxH*0.5 there is no foam at all, which is what keeps a
                // calm lake from developing whitecaps.
                float wCrest = smoothstep(wMaxH * 0.5f, wMaxH * 0.9f, wH);
                float wFoamMask = 0.0f;
                if (wFoamAmt > 0.001f && wCrest > 0.001f)
                {
                    vec2 fuv = info.worldPos.xz * 0.9f - wFlow * (u_Time * 0.7f) + vec2(u_Time * 0.03f, -u_Time * 0.021f);
                    // Two cell-noise octaves at 60/40, quantised to a grid. Quantising is what
                    // makes foam read as PATCHES; a smooth noise would give an even wash along
                    // the crest, which is the artifact the comment above warns about.
                    float breakup = wl_hash12(floor(fuv * 3.0f)) * 0.6f + wl_hash12(floor(fuv * 7.0f + 13.1f)) * 0.4f;
                    float wFoamN = breakup * 0.7f + wCrest * 0.3f;
                    wFoamMask = smoothstep(1.0f - wFoamAmt, 1.01f - wFoamAmt * 0.5f, wFoamN);
                }
                wFoamMask = clamp(wFoamMask, 0.0f, 1.0f);
                // Foam toward a slightly blue white (0.9, 0.95, 1.0) rather than pure
                // white -- foam lit by a blue sky genuinely is, and pure white reads as a
                // chalk overlay. 0.85 keeps some water colour showing through.
                info.albedo = mix(info.albedo, vec3(0.9f, 0.95f, 1.0f), wFoamMask * 0.85f);
                // Ripple slopes roughen the mirror into glitter; foam is matte.
                // Crests themselves stay glossy so swells catch the sun.
                float wRippleMag = clamp(length(wRippleGrad), 0.0f, 1.0f);
                // Ripple slope roughens the surface, which is what breaks the mirror into
                // glitter. Scaled by the ripple fade, so distant water stays a clean
                // reflection.
                info.rough = clamp(info.rough + wRippleMag * 0.12f * wRippleFade, 0.0f, 1.0f);
                info.rough = mix(info.rough, 0.85f, wFoamMask);
                // Water is a mirror: metallic = 1.0
                // Water is fully metallic. That makes F0 equal the albedo-tinted value
                // and gives a coloured reflection rather than a 4% dielectric one -- the
                // cheap way to get the tinted, refractive-looking water without an actual
                // refraction ray.
                info.metal = 1.0f;
            }
            // Relief weathering: dust settles in pits, sun bleaches ridge tops.
            // Displacement-gated: only set when the relief march ran with push.
            if (pomH >= 0.0f)
            {
                // Cavity darkening from the relief height: pits (low height) collect dust.
                // The smoothstep is INVERTED -- 0.45 down to 0.05 -- so low values map high.
                float cav = smoothstep(0.45f, 0.05f, pomH);
                float ridge = smoothstep(0.55f, 0.95f, pomH);
                info.albedo *= (1.0f - 0.30f * cav);
                // Sun bleaching on ridge tops, tinted warm (1.10, 1.03, 0.92). This is the
                // second half of the weathering model: pits darken and desaturate, ridges
                // brighten and warm.
                info.albedo = mix(info.albedo, info.albedo * vec3(1.10f, 1.03f, 0.92f), ridge * 0.65f);
                info.occlusion *= (1.0f - 0.35f * cav);
            }
            // Emission premultiplied by its intensity here, so every downstream consumer
            // can treat info.emission as radiance and skip the multiply.
            info.emission = inst.Emission.xyz * inst.Emission.w;
        }
    }
}

// The closest-hit traversal. The same BVH walk as IsOccluded, but keeping
// the nearest hit and building a full HitInfo.
// ---
// Note the difference in the node test: tNear > info.t rejects a node whose
// entry is already beyond the current best, so the traversal DOES have a
// distance bound -- unlike the shadow version, which early-outs on the first
// hit instead.
HitInfo TraceScene(Ray ray) {
    HitInfo info;
    info.hit = false;
    // t starts at 1e20, not infinity. A finite sentinel avoids inf comparisons
    // in the leaf test and gives a defined result if nothing is ever hit.
    info.t = 1e20;
    info.occlusion = 1.0f;
    info.f0Tint = vec3(1.0f);

    if (u_InstanceCount == 0) return info;

    // A ray is primary if it starts within a centimetre of the camera. This
    // is a POSITION heuristic, not a flag passed down -- so a secondary ray
    // that happens to spawn at the camera would be misclassified. In practice
    // secondary rays always start on a surface.
    bool isPrimaryRay = length(ray.Origin - u_CameraPosition) < 0.01f;
    vec3 safeDir = sign(ray.Direction) * max(abs(ray.Direction), vec3(0.00001f));
    vec3 invDir = 1.0f / safeDir;

    int stackBase = int(gl_LocalInvocationIndex) * 32;
    int stackPtr = 0;
    gs_trav_stack[stackBase + stackPtr++] = 0;

    while (stackPtr > 0) {
        int nodeIdx = gs_trav_stack[stackBase + --stackPtr];
        BVHNode node = BVHNodes[nodeIdx];

        vec3 t0 = (node.MinBounds.xyz - ray.Origin) * invDir;
        vec3 t1 = (node.MaxBounds.xyz - ray.Origin) * invDir;
        vec3 tMin = min(t0, t1);
        vec3 tMax = max(t0, t1);
        float tNear = max(max(tMin.x, tMin.y), tMin.z);
        float tFar = min(min(tMax.x, tMax.y), tMax.z);
        // The three rejection conditions again, but here the third is tNear >
        // info.t: the current best hit distance. That is what makes this a
        // closest-hit rather than an any-hit traversal -- it prunes subtrees the
        // answer already lies in front of.
        if (tNear > tFar || tFar < 0.0f || tNear > info.t) continue;

        int encoded = int(node.MinBounds.w);
        if (encoded < 0) {
            int first = -encoded - 1;
            int count = int(node.MaxBounds.w);
            for (int i = first; i < first + count; i++) {
                TestInstanceHit(ray, i, isPrimaryRay, info);
            }
        } else {
            if (stackPtr < 30) {
                gs_trav_stack[stackBase + stackPtr++] = int(node.MaxBounds.w);
                gs_trav_stack[stackBase + stackPtr++] = encoded;
            }
        }
    }
    return info;
}

// Direct lighting: emissive geometry lights (the first loop) plus analytic
// component lights (the second), sharing one Cook-Torrance BRDF.
// ---
// lightSeed drives the stochastic light subset; skipEnv suppresses the
// metal sky reflection when the caller is already tracing a real mirror
// ray, so the sky is not counted twice.
vec3 ComputeDirectLighting(HitInfo h, vec3 V, uint lightSeed, bool skipEnv) {
    // F0 from the albedo and metalness, then multiplied by the neural f0Tint.
    // The 0.04 dielectric floor is standard; the f0Tint is 1.0 unless the
    // neural material path raised it.
    vec3 F0 = mix(vec3(0.04f), max(h.albedo, vec3(0.04f)), h.metal) * h.f0Tint;
    vec3 diffuseColor = h.albedo * (1.0f - h.metal);
    vec3 directLight = vec3(0.0f);

    // The light budget ladder: 1 / 2 / 4 / u_MaxLights by preset. Low quality
    // evaluates a SINGLE light -- which is only correct because the stochastic
    // scaling below compensates.
    int maxLightsCap = (u_QualityLevel == 0) ? 1 :
                       ((u_QualityLevel == 1) ? 2 :
                       ((u_QualityLevel == 2) ? 4 : u_MaxLights));
    // min() against the actual light count, so a scene with 3 lights does not
    // loop 4 times.
    int lightsToSample = min(u_LightCount, maxLightsCap);

    // Stochastic subset: evaluate 2 rotating lights instead of all N shadow
    // rays, scaled to stay unbiased. Temporal accumulation converges the
    // rotation; ~4x cheaper direct lighting at Ultra.
    int evalCount = lightsToSample;
    int lightStart = 0;
    int lightStride = 1;
    // Above two lights, evaluate only two and scale the contribution up.
    // ---
    // This is unbiased only because lightScale divides by the number actually
    // evaluated. Two rotating lights out of N give the same expected value as
    // all N -- at 1/N of the shadow-ray cost. Temporal accumulation is what
    // resolves the residual per-frame noise.
    if (lightsToSample > 2)
    {
        evalCount = 2;
        // The random starting offset from the pixel's seed. Per-pixel, so adjacent
        // pixels rotate through different lights -- otherwise the noise would be
        // spatially coherent and read as banding.
        lightStart = int(lightSeed % uint(lightsToSample));
        // Stride to spread the two picks across the list rather than sampling two
        // adjacent lights. N/2 means the second pick is roughly the opposite half
        // of the set.
        lightStride = int(float(lightsToSample) / 2.0);
        if (lightStride < 1) lightStride = 1;
    }
    float lightScale = float(lightsToSample) / float(max(evalCount, 1.0f));

    for (int k = 0; k < evalCount; k++) {
        // The rotating index, wrapped by the modulo. With stride N/2 the two picks
        // are distinct as long as N >= 2, which the branch above guarantees.
        int li = (lightStart + k * lightStride) % lightsToSample;
        int i = LightIndices[li];
        RayTracingInstance light = Instances[i];
        vec3 toLight = light.WorldTransform[3].xyz - h.worldPos;
        float distToLight = length(toLight);
        vec3 dirToLight = toLight / distToLight;
        // The shadow ray, offset 1mm along the normal. Without it a surface point
        // self-occludes numerically -- the same class of fix as the volumetric
        // shadow's offset.
        Ray shadowRay = Ray(h.worldPos + h.normal * 0.001f, dirToLight);
        // Pass the light's own instance index so it can't self-shadow
        // (shadow rays target the light center, inside its geometry).
        // IsOccluded is passed the LIGHT's instance index so it can exempt it.
        // The comment above spells out why this is mandatory: the shadow ray ends
        // at the light's centre, which is INSIDE the light's geometry.
        if (!IsOccluded(shadowRay, distToLight, i)) {
            float NdotL = max(dot(h.normal, dirToLight), 0.0f);
            if (NdotL <= 0.0f) continue;
            vec3 H = normalize(V + dirToLight);
            vec3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);
            float D = DistributionGGX(max(dot(h.normal, H), 0.0f), h.rough);
            float G = GeometrySmith(h.normal, V, dirToLight, h.rough);
            float NdotV = max(dot(h.normal, V), 0.0f);
            // Cook-Torrance: a Lambert diffuse plus a GGX specular, divided by
            // 4*NdotV*NdotL. The 0.001 floor on the denominator is the same
            // firefly guard as the one in DistributionGGX.
            vec3 brdf = (vec3(1.0f) - F) * (1.0f - h.metal) * h.albedo / PI + (D * G * F) / max(4.0f * NdotV * NdotL, 0.001f);
            vec3 contrib = brdf * light.Emission.xyz * light.Emission.w * NdotL;

            // A luminance-based firefly clamp at 10.0. A single bright specular hit
            // can be orders of magnitude above its neighbours, and without this one
            // sample produces a persistent white speck that the temporal pass will
            // smear across a whole trail.
            float brightness = dot(contrib, vec3(0.2126f, 0.7152f, 0.0722f));
            if (brightness > 10.0f) contrib *= (10.0f / brightness);
            directLight += contrib * lightScale;
        }
    }

    // The surface's own emission, added after the light loop so it is not
    // attenuated by anything the lights did.
    directLight += h.emission;

    // Analytic component lights (directional/point/spot/area). All submitted
    // lights are evaluated (capped at 8) with shadow rays, using the same
    // Cook-Torrance BRDF as the emissive-geometry lights above.
    // The analytic lights. A FIXED 8-iteration bound rather than u_ALightCount,
    // which GLSL cannot loop on portably. All submitted lights are evaluated --
    // no stochastic subset here, unlike the emissive loop above.
    for (int a = 0; a < 8; a++) {
        if (a >= u_ALightCount) break;
        float atype = u_ALightData[a].x;
        vec3 lcol = u_ALightColor[a] * u_ALightData[a].y;
        float arange = max(u_ALightData[a].z, 0.0f);
        float afall = max(u_ALightData[a].w, 0.5f);
        vec3 L;
        float att = 1.0f;
        // The shadow ray length, 100000 for a directional light (effectively
        // infinite) and the light's distance otherwise. This is what bounds the
        // shadow traversal -- a directional light's ray must test against
        // everything between the surface and infinity.
        float shadowLen = 100000.0f;
        // Directional lights use the travel direction negated. u_ALightDir is the
        // direction light TRAVELS, while the BRDF wants the direction TO the light.
        if (atype < 0.5f) {
            L = -u_ALightDir[a];
        } else {
            vec3 toL = u_ALightPos[a] - h.worldPos;
            float d = length(toL);
            L = toL / max(d, 0.00001f);
            if (arange > 0.0f) {
                if (d >= arange) continue;
                att *= pow(clamp(1.0f - d / arange, 0.0f, 1.0f), afall);
                shadowLen = d;
            } else {
                att *= 1.0f / (1.0f + 0.1f * afall * d * d);
                // An infinite point light's shadow ray is capped at 500m rather than truly
                // infinite -- a bound that keeps the traversal from walking the whole BVH
                // for a light that is effectively directional at that range.
                shadowLen = min(d, 500.0f);
            }
            if (atype > 1.5f && atype < 2.5f) {
                float c = dot(-L, u_ALightDir[a]);
                att *= smoothstep(u_ALightData2[a].y, u_ALightData2[a].x, c);
                if (att <= 0.0f) continue;
            // Area lights: a disc approximation, not a true area integral.
            // ---
            // Double-sidedness is the .w flag -- when set, the front-facing test is
            // skipped entirely so a thin emitter lights from both sides.
            } else if (atype > 2.5f) {
                float front = dot(-L, u_ALightDir[a]);
                if (u_ALightData2[a].w < 0.5f) {
                    if (front <= 0.0f) continue;
                    att *= clamp(front * 3.0f, 0.0f, 1.0f);
                }
                float asize = max(u_ALightData2[a].z, 0.01f);
                // A 1/d falloff softened by the light's size: at d = asize the attenuation
                // is exactly 0.5. This is what makes a large area light read as a large
                // soft source rather than a point that gets arbitrarily bright close up.
                att *= asize / (d + asize);
            }
        }
        float aNdotL = max(dot(h.normal, L), 0.0f);
        if (aNdotL <= 0.0f) continue;
        Ray aShadowRay = Ray(h.worldPos + h.normal * 0.001f, L);
        if (!IsOccluded(aShadowRay, shadowLen, -1)) {
            vec3 aH = normalize(V + L);
            vec3 aF = FresnelSchlick(max(dot(aH, V), 0.0f), F0);
            float aD = DistributionGGX(max(dot(h.normal, aH), 0.0f), h.rough);
            float aG = GeometrySmith(h.normal, V, L, h.rough);
            float aNdotV = max(dot(h.normal, V), 0.0f);
            vec3 abrdf = (vec3(1.0f) - aF) * (1.0f - h.metal) * h.albedo / PI + (aD * aG * aF) / max(4.0f * aNdotV * aNdotL, 0.001f);
            vec3 acontrib = abrdf * lcol * att * aNdotL;

            float abright = dot(acontrib, vec3(0.2126f, 0.7152f, 0.0722f));
            if (abright > 10.0f) acontrib *= (10.0f / abright);
            directLight += acontrib;
        }
    }

    // The sky ambient, sampled by the normal's y at HALF the full range --
    // 0.5*(y+1) maps y in [-1,1] to [0,1].
    // ---
    // The 0.15 scale is a hardcoded ambient strength, and the division by 0.15
    // in the metal branch below exists only to undo it. So the ambient level is
    // not artist-controllable from any uniform.
    float skyT = 0.5f * (h.normal.y + 1.0f);
    vec3 skyAmbient = mix(u_SkyBottomColor, u_SkyTopColor, skyT) * 0.15f;
    directLight += diffuseColor * skyAmbient * h.occlusion;

    // Analytic sky env approx for metals. Skipped when the caller traces a
    // true mirror ray instead (Low-preset metals) so the sky isn't counted twice.
    // The metal sky reflection. skipped when the caller is tracing a real
    // mirror ray instead (the Low-preset metal path), which would otherwise
    // count the same sky twice.
    // ---
    // Skip is decided by metal > 0.0 rather than a threshold, so a surface with
    // metal = 0.001 gets the full environment term -- a very slight metallic
    // still picks up a full sky reflection.
    if (h.metal > 0.0f && !skipEnv) {
        vec3 R = reflect(-V, h.normal);
        vec3 specSky = GetSkyColor(R);
        vec3 diffSky = skyAmbient / 0.15f; 
        // Mix specular and diffuse environment by roughness squared. Squared so the
        // crossover happens late: a fairly smooth surface is still mostly specular.
        vec3 envColor = mix(specSky, diffSky, h.rough * h.rough);
        directLight += F0 * envColor * h.metal * h.occlusion;
    }

    return directLight;
}

// God-ray sun visibility: one shadow tap toward the sun for fogged pixels.
// Fog in shadowed regions keeps ambient but loses sun in-scatter, so shafts
// appear where geometry breaks the sunlight. Skipped (returns 1) when the ray
// enters no fog volume. Tested at the surface end for hits, at the camera
// for sky rays — a single-tap approximation, noted as such.
// One shadow tap toward the sun, used to modulate fog in-scatter so god-ray
// shafts appear where geometry breaks the sunlight.
// ---
// Deliberately a SINGLE tap at one point on the ray, as the comment above
// says. It does not integrate occlusion along the ray, so a fog volume
// half in shadow will not darken progressively -- the shafts are a binary
// in/out effect rather than a true volumetric occlusion.
float wl_godray_visibility(vec3 ro, vec3 rd, float tHit)
{
    if (u_FogEnabled != 1 || u_FogCount <= 0)
        return 1.0f;
    bool hitsFog = false;
    for (int i = 0; i < 4; i++)
    {
        if (i >= u_FogCount)
            break;
        if (u_FogData2[i].w < 0.5f)
            continue;
        vec2 r = wl_box_range(ro, rd, u_FogMin[i], u_FogMax[i]);
        // Does the ray actually enter any fog box before its hit? The range test
        // is intersected with [0, tHit], so a box behind the surface does not count.
        if (min(r.y, tHit) > max(r.x, 0.0f))
        {
            hitsFog = true;
            break;
        }
    }
    if (!hitsFog)
        return 1.0f;
    // The tap point: the surface hit, or the ray ORIGIN for a sky ray beyond
    // 500m. Tap at the surface rather than mid-volume so the shadow test
    // reflects the geometry actually in the way.
    vec3 tapPoint = (tHit < 500.0f) ? (ro + rd * tHit) : ro;
    Ray sunRay = Ray(tapPoint + u_SunDirection * 0.05f, u_SunDirection);
    // The shadow ray, offset 5cm toward the sun. 100000 is effectively
    // infinite -- a directional sun has no distance to attenuate over.
    if (IsOccluded(sunRay, 100000.0f, -1))
        // Shadowed fog keeps 12% of its sun in-scatter rather than none. A hard
        // 0.0 would make shadowed fog visibly black; this leaves a little ambient
        // so it reads as shadow instead of as a hole.
        return 0.12f;
    return 1.0f;
}

vec3 GetSkyColor(vec3 dir) {
    float t = 0.5f * (normalize(dir).y + 1.0f);
    return mix(u_SkyBottomColor, u_SkyTopColor, t);
}

// PASS 1: the trace. One primary ray per pixel, full material evaluation,
// indirect bounces, volumetrics, and the trace G-buffer -- then straight
// into img_Bloom.
// ---
// There is no separate denoise pass despite the name. 'AndDenoise' refers
// to the temporal accumulation that follows, not to a filter applied here.
void RunTraceAndDenoise()
{
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = imageSize(img_Output);
    ivec2 traceSize = wl_trace_extent(imgSize);
    if (pixelCoords.x >= traceSize.x || pixelCoords.y >= traceSize.y) return;

    // The per-invocation RNG seed, re-seeded from the pixel and frame.
    // ---
    // This writes the file-scope global, which every stochastic term in this
    // pass then draws from -- see the note on that declaration. lightSeed
    // below is a SEPARATE integer for the light subset, so that selection does
    // not disturb this stream.
    seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x) + uint(u_FrameIndex) * 0x9e3779b9u;
    vec2 jitter = GetJitter(u_FrameIndex, u_CameraMoved);
    // The jittered NDC position, and therefore the primary ray. The jitter is
    // the sub-pixel offset from GetJitter, so successive frames sample
    // different points inside the pixel.
    vec2 jitteredUV = ((vec2(pixelCoords) + jitter) / vec2(traceSize)) * 2.0f - 1.0f;
    
    // The primary ray from the jittered NDC via the inverse view-projection.
    // ---
    // Note the z and w of 1.0: this reconstructs a point on the FAR plane, so
    // the direction from the camera is the view ray. Using z=0 (the near plane)
    // would give the same direction for a perspective matrix, but z=1 is what
    // survives being used with an orthographic one.
    Ray primaryRay = Ray(u_CameraPosition, normalize((u_InverseViewProjection * vec4(jitteredUV, 1.0f, 1.0f)).xyz));
    HitInfo primary = TraceScene(primaryRay);

    // The albedo and a hit mask. The mask is what lets later passes tell a real
    // traced surface from the white default.
    // ---
    // The store is ternary rather than an if/else, so BOTH the sky path and the
    // surface path go through it. A sky pixel stores albedo 1.0 with mask 0.0.
    imageStore(img_AlbedoHit, pixelCoords, vec4(primary.hit ? primary.albedo : vec3(1.0f), primary.hit ? 1.0f : 0.0f));

    // Trace G-buffer: NDC depth (far on miss) + packed world normal for the
    // velocity / temporal / bilateral passes. Full-size buffer, subregion data.
    // (RunResolve was removed; binding 6 was dead. Image units are capped at
    // 0-7 on this GL driver, so depth+normal share one RGBA32F image.)
    // Sky pixels get NDC depth 1.0 (so they compare as infinitely far in the
    // temporal pass) and a neutral 0.5 normal.
    // ---
    // The comment above records a constraint worth knowing: image units are
    // capped at 0-7 on this GL driver, so depth and normal had to share one
    // RGBA32F image after a resolve pass was removed.
    if (!primary.hit) {
        imageStore(img_TraceGBuffer, pixelCoords, vec4(1.0f, 0.5f, 0.5f, 1.0f));
    } else {
        // Project the world hit into clip space, then divide by w for NDC. The
        // max() on w guards a point at the projection plane.
        vec4 tclip = u_ViewProjection * vec4(primary.worldPos, 1.0f);
        float tndc = tclip.z / max(tclip.w, 1e-6f);
        imageStore(img_TraceGBuffer, pixelCoords, vec4(clamp(tndc * 0.5f + 0.5f, 0.0f, 1.0f), primary.normal * 0.5f + 0.5f));
    }

    if (!primary.hit) {
        vec3 skyCol = GetSkyColor(primaryRay.Direction);
        // Static per-pixel dither (NOT frame-animated): accumulation converges
        // a stable field in a few frames, while a cycling dither keeps every
        // frame different and the image shimmers forever, especially upsized.
        // A STATIC per-pixel dither, not a frame-animated one.
        // ---
        // The comment above explains the reasoning and it is the right call: a
        // cycling dither keeps every frame different, so temporal accumulation
        // never converges and the sky shimmers forever, especially when upsized.
        float skyDith = wl_hash12(vec2(pixelCoords));
        // Skip the volume march entirely when the scene submits no volumes
        // (the per-volume loops would break at count 0 anyway).
        if ((u_FogEnabled == 1 && u_FogCount > 0) || (u_CloudEnabled == 1 && u_CloudCount > 0))
        {
            // Sky volumetrics marched out to a fixed 600m, since a sky ray has no
            // surface to stop at. The same visibility probe feeds the fog's sun
            // in-scatter.
            float skySunVis = wl_godray_visibility(primaryRay.Origin, primaryRay.Direction, 600.0f);
            skyCol = wl_apply_volumetrics(primaryRay.Origin, primaryRay.Direction, 600.0f, skyCol, skyDith, skySunVis);
        }
        imageStore(img_Bloom, pixelCoords, vec4(skyCol, 1.0f));
        return;
    }

    HitInfo tileHit = primary;
    Ray lightRay = primaryRay;

    vec3 directLight = vec3(0.0f);
    vec3 indirectLight = vec3(0.0f);

    if (tileHit.hit) {
        vec3 V = normalize(-lightRay.Direction);
        // A SEPARATE seed for light selection, derived from the pixel and frame but
        // not from the global. Keeping it independent is what lets the light
        // rotation change per frame without perturbing the indirect rays' stream.
        uint lightSeed = uint(pixelCoords.x * 73 + pixelCoords.y * 149 + u_FrameIndex * 2);
        // The indirect ray budget: none at Low, at most 1 up to High, and the full
        // u_IndirectRays at Ultra.
        // ---
        // So the 'indirect rays' control only has an effect at Ultra -- at every
        // other preset it is clamped to 1 regardless of what the user sets.
        int maxIndirectRays = (u_QualityLevel == 0) ? 0 :
                              ((u_QualityLevel <= 2) ? min(u_IndirectRays, 1) : u_IndirectRays);
        // Perf tier: at reduced render scale the neural cache carries diffuse
        // GI (it already blends 65% into the traced result) — skip the second
        // traced bounce for non-metals and save ~40% of the trace pass.
        // Metals keep the ray so reflections stay sharp.
        // At reduced render scale, let the neural cache stand in for the traced
        // bounce on non-metals.
        // ---
        // Metals are excluded deliberately: their indirect term is a reflection,
        // and a smooth analytic prediction there would be visibly wrong. Roughly a
        // 40% saving on the trace pass at that resolution.
        if (maxIndirectRays > 0 && u_RenderScale <= 0.75f && tileHit.metal <= 0.5f &&
            u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f)
            maxIndirectRays = 0;
        // Mirror gap-fill: Low preset traces no indirect rays, so metals
        // would only get the analytic sky approx. Trace one real mirror ray
        // instead and skip the approx inside ComputeDirectLighting.
        // The mirror gap-fill: a metal with no traced bounce gets one real mirror
        // ray instead of the analytic sky approximation.
        // ---
        // This is why skipEnv is passed to ComputeDirectLighting as doMirror --
        // otherwise the sky would be added twice, once analytically and once
        // through the traced ray.
        bool doMirror = (tileHit.metal > 0.01f && maxIndirectRays == 0);
        directLight = ComputeDirectLighting(tileHit, V, lightSeed, doMirror);

        if (maxIndirectRays > 0) {
            vec3 diffuseColor = tileHit.albedo * (1.0f - tileHit.metal);
            vec3 F0 = mix(vec3(0.04f), max(tileHit.albedo, vec3(0.04f)), tileHit.metal) * tileHit.f0Tint;
            vec3 indirectReflectance = mix(diffuseColor, F0, tileHit.metal);

            // The indirect loop. One traced bounce per iteration, each with its own
            // seed offset (+1, +2, ... times a prime) so the rays are decorrelated
            for (int r = 0; r < maxIndirectRays; r++) {
                seed = uint(pixelCoords.y * 1024 + pixelCoords.x) + uint(u_FrameIndex * 1000) + uint((r + 1) * 7919);
                
                vec3 indirectDir;
                // Russian-roulette the lobe choice by metalness: a metal always takes the
                // specular branch, a dielectric always the cosine branch, and a mixed
                // surface picks randomly in proportion to metal.
                // ---
                // This makes the estimator unbiased -- but it is only CORRECT because
                // lightScale-style compensation is absent here, which is fine for a
                // 0/1 choice by probability rather than a subset of N.
                if (hash() < tileHit.metal) {
                    vec3 specDir = reflect(-V, tileHit.normal);
                    vec3 randHem = random_in_hemisphere(tileHit.normal);
                    // Roughness-jittered reflection: blend the mirror direction toward a random
                    // hemisphere direction by roughness SQUARED, so a smooth metal reflects
                    // sharply and a rough one blurs.
                    indirectDir = normalize(mix(specDir, randHem, tileHit.rough * tileHit.rough));
                } else {
                    indirectDir = random_in_hemisphere(tileHit.normal);
                }

                // The bounce origin, offset 1mm along the normal -- the same offset every
                // shadow ray uses, for the same self-intersection reason.
                Ray indirectRay = Ray(tileHit.worldPos + tileHit.normal * 0.001f, indirectDir);
                HitInfo indirect = TraceScene(indirectRay);

                if (indirect.hit) {
                    vec3 indirectV = normalize(-indirectDir);
                    // Shade the hit point with FULL direct lighting rather than an ambient
                    // approximation. That is what makes an indirect bounce carry shadows --
                    // the bounce sees the lit surface, correctly occluded.
                    vec3 bouncedLight = ComputeDirectLighting(indirect, indirectV, lightSeed + 1u, false);
                    indirectLight += indirectReflectance * bouncedLight;
                } else {
                    indirectLight += indirectReflectance * GetSkyColor(indirectDir);
                }
            }
            // Average the bounces. Dividing by the count keeps this independent of the
            // budget, so raising u_IndirectRays changes noise but not exposure.
            indirectLight /= float(maxIndirectRays);
            indirectLight *= tileHit.occlusion;
            // Neural radiance cache blend: learned GI steadies the 1-ray estimate (OpenGL neural path).
            // Weighted down on metals so the traced specular reflection dominates.
            if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f) {
                vec3 neuralGI = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, tileHit.metal, V);
                // The neural GI blend weight: strength x 0.65, reduced by 80% of
                // metalness. The 0.65 cap means the traced estimate always retains at least
                // 35% of the weight, even at full neural strength.
                float neuralK = clamp(u_NeuralLightStrength, 0.0f, 1.0f) * 0.65f * (1.0f - tileHit.metal * 0.8f);
                indirectLight = mix(indirectLight, neuralGI * tileHit.occlusion, neuralK);
            }
        } else if (u_NeuralEnabled == 1 && u_NeuralLightStrength > 0.001f) {
            // No traced bounces at this quality level: neural cache IS the GI (e.g. Low preset)
            indirectLight = wl_neural_indirect(tileHit.worldPos, tileHit.normal, tileHit.albedo, tileHit.rough, tileHit.metal, V) * tileHit.occlusion;
        }

        // Sharp scene reflection for metals with no traced bounce (Low preset):
        // one roughness-jittered mirror ray, Fresnel-tinted. Shadow-correct
        // because the reflected hit is shaded with full shadowed direct light.
        if (doMirror) {
            // The mirror ray's own seed, offset by a distinct constant so it does not
            // reuse the indirect rays' stream.
            seed = uint(pixelCoords.y * 1024 + pixelCoords.x) + uint(u_FrameIndex * 1000) + 40543u;
            vec3 R = reflect(-V, tileHit.normal);
            vec3 mirrorDir = normalize(mix(R, random_in_hemisphere(tileHit.normal), tileHit.rough * tileHit.rough));
            // The mirror ray, offset 2mm -- larger than the 1mm used elsewhere,
            // because a mirror ray on a curved surface is far more prone to numerical
            // self-intersection than a diffuse bounce.
            Ray mirrorRay = Ray(tileHit.worldPos + tileHit.normal * 0.002f, mirrorDir);
            HitInfo mirrorHit = TraceScene(mirrorRay);
            vec3 mirrorColor;
            if (mirrorHit.hit) {
                vec3 mirrorV = normalize(-mirrorRay.Direction);
                mirrorColor = ComputeDirectLighting(mirrorHit, mirrorV, lightSeed + 7u, false);
            } else {
                mirrorColor = GetSkyColor(mirrorRay.Direction);
            }
            vec3 mF0 = mix(vec3(0.04f), max(tileHit.albedo, vec3(0.04f)), tileHit.metal) * tileHit.f0Tint;
            vec3 mF = FresnelSchlick(clamp(dot(tileHit.normal, V), 0.0f, 1.0f), mF0);
            directLight += mF * mirrorColor * tileHit.metal;
        }
    } else {
        directLight = GetSkyColor(lightRay.Direction);
    }

    // Per-channel clamps at 10 and 5. A final firefly guard after everything
    // has been summed -- the per-light clamp inside ComputeDirectLighting
    // cannot catch a bright accumulation from many contributions.
    directLight = clamp(directLight, vec3(0.0f), vec3(10.0f));
    indirectLight = clamp(indirectLight, vec3(0.0f), vec3(5.0f));

    // ComputeDirectLighting already folds albedo into the BRDF (diffuse/spec/
    // ambient/emission), and indirectLight already carries tile reflectance -
    // do NOT multiply by albedo again (albedo^2 pushed textured floors to black).
    // Albedo is NOT multiplied in again.
    // ---
    // The comment records a fixed defect here: doing so squared the albedo,
    // which pushed textured floors toward black. Both directLight and
    // indirectLight already carry reflectance.
    vec3 finalPixelColor = directLight + indirectLight;
    // Volumetric atmosphere over the primary ray (clouds, then fog).
    // Skipped when the scene submits no volumes (see sky path above).
    if ((u_FogEnabled == 1 && u_FogCount > 0) || (u_CloudEnabled == 1 && u_CloudCount > 0))
    {
        // The volume march distance: to the surface hit. So volumetrics correctly
        // stop at the first opaque surface.
        float volDist = distance(u_CameraPosition, tileHit.worldPos);
        // A per-pixel dither offset by a constant, so this volume march and the sky
        // path's use different noise fields for the same pixel.
        float volDith = wl_hash12(vec2(pixelCoords) + 7.3f);
        float volSunVis = wl_godray_visibility(lightRay.Origin, lightRay.Direction, volDist);
        finalPixelColor = wl_apply_volumetrics(lightRay.Origin, lightRay.Direction, volDist, finalPixelColor, volDith, volSunVis);
    }
    
    // Global atmospheric fog: distance haze for all surfaces
    {
        float surfDist = distance(u_CameraPosition, tileHit.worldPos);
        float gfDith = wl_hash12(vec2(pixelCoords) + 3.7f);
        finalPixelColor = wl_apply_global_fog(finalPixelColor, surfDist, lightRay.Direction, gfDith);
    }
    
    imageStore(img_Bloom, pixelCoords, vec4(finalPixelColor, 1.0f));
}

// PASS 2: temporal accumulation. Reproject the previous frame through the
// velocity buffer and blend, which is what turns the path tracer's raw
// noise into a converged image.
void RunTemporalAccumulation() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Accumulation));
    vec2 uv = (vec2(pixelCoords) + 0.5f) / vec2(imgSize);

    vec3 currentLight = imageLoad(img_Bloom, pixelCoords).rgb;
    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    // The history lookup position: this pixel's UV minus its velocity. The
    // velocity buffer is what encodes the camera's and the surface's motion
    // between frames.
    vec2 prevUV = uv - velocity;
    // Accumulation + depth buffers hold the subregion; map history UVs to it.
    // Map the full-res UV into the subregion the accumulation buffer actually
    // holds.
    // ---
    // The comment below this is the important part: outside that subregion
    // lives STALE data from a previous full-resolution frame, so an unclamped
    // history tap streaks the frame edges.
    vec2 histUV = prevUV * u_RenderScale;
    // Keep bilinear taps inside the valid subregion: outside it lives stale
    // data (e.g. from full-res frames), which would streak the frame edges.
    // --
    // BOTH sides are now clamped. This was previously min() only, with the
    // lower side unclamped, which is a real defect rather than a nit: a fast
    // leftward or upward camera motion makes prevUV negative, and an unclamped
    // histUV then samples outside the subregion and picks up stale data from a
    // previous full-resolution frame. The result is a streak trailing the frame
    // edge in exactly the direction of motion -- the most visible possible
    // symptom, and one that reads as "the temporal filter is broken" rather
    // than as a clamping bug. Clamping the lower side to 0.5/fullRes keeps the
    // bilinear tap strictly inside the subregion on both axes.
    // --
    // Note the ORDER: this must be applied BEFORE the velocity is used to
    // compute motionMag, because a clamped tap changes which history texel is
    // actually read. motionMag is computed from the velocity buffer directly
    // (below), not from the clamped UV, so the two stay consistent.
    vec2 fullRes = vec2(imageSize(img_Accumulation));
    histUV = clamp(histUV, 0.5f / fullRes, vec2(u_RenderScale) - 0.5f / fullRes);

    // History validity, part one: the reprojected UV must land inside the
    // frame. This is the check that catches a surface leaving the screen.
    // --
    // u_FrameIndex > 0 is REQUIRED and was missing here: on frame 0 the
    // history buffer has never been written, so every tap returns
    // uninitialised memory and the blend mixes that garbage in at alpha=1.0.
    // The HLSL twin has always had this guard. Adding it is what makes the two
    // twins agree, and it also removes the dependence on the history buffer
    // happening to be cleared by the backend at allocation time.
    bool validHistory = u_FrameIndex > 0 && prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f;
    // Motion magnitude in PIXELS, not UV -- multiplying by the trace extent
    // converts the velocity buffer's screen-space delta into a count of texels
    // travelled.
    float motionMag = length(velocity * vec2(imgSize));

    if (validHistory) {
        float currentDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
        float prevDepth = texture(s_DepthBuffer, histUV).r;
        // History validity, part two: the depth test. A mismatch of more than 0.02
        // means the reprojected point landed on a different surface, so its history
        // is another object's colour and must be discarded.
        // ---
        // This is the anti-ghosting test. The 0.02 threshold is a bias in NDC depth
        // units -- roughly 2% of the depth range, so it tolerates the reprojection
        // error from a slightly wrong normal but not a genuine object change.
        if (abs(currentDepth - prevDepth) > 0.02f) validHistory = false;
    }

    // The history tap, UNCONDITIONALLY -- before the validity check.
    // ---
    // This is deliberate: sampling always keeps the texture-fetch uniform and
    // branch-free. The result is discarded by the mix below when the history is
    // invalid, so an out-of-range tap costs bandwidth but not correctness.
    vec3 historyLight = texture(s_Accumulation, histUV).rgb;

    if (validHistory) {
        // Ultra gets a 3x3 neighbourhood clamp; every other tier gets the 5-tap
        // cross below. The variance clamp is the same operation in both, just with a
        // different support.
        if (u_QualityLevel >= 3) {
            vec3 m1 = vec3(0.0f);
            vec3 m2 = vec3(0.0f);
            for (int x = -1; x <= 1; x++) {
                for (int y = -1; y <= 1; y++) {
                    ivec2 sp = clamp(pixelCoords + ivec2(x, y), ivec2(0), imgSize - ivec2(1));
                    vec3 s = RGB2YCoCg(imageLoad(img_Bloom, sp).rgb);
                    m1 += s;
                    m2 += s * s;
                }
            }
            vec3 mean = m1 / 9.0f;
            // sqrt of the variance, with a max against zero. The max is the important
            // part: mean^2 can exceed m2/9 by a rounding error, and a negative variance
            // under sqrt is NaN -- which would poison the clamp bounds and silently
            // discard every history sample.
            vec3 stddev = sqrt(max(vec3(0.0f), m2 / 9.0f - mean * mean));

            vec3 historyYCoCg = RGB2YCoCg(historyLight);
            // The clamp box: mean +/- 1.25 sigma. This is variance clipping -- the
            // history is confined to the current frame's local colour range, so a value
            // that does not recur here gets pulled back rather than accumulating.
            // ---
            // 1.25 is deliberately tight. It kills fireflies and disocclusion trails
            // at the cost of some temporal detail on legitimately varying surfaces.
            vec3 minCol = mean - 1.25f * stddev;
            vec3 maxCol = mean + 1.25f * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        } else {
            // History clamp at EVERY tier (not just High/Ultra): without it,
            // per-frame variations (trace jitter cycle, shadow-seed rotation,
            // mirror-ray randomness on metals) smear unboundedly through the
            // blend and Low/Medium never resolve sharp. Pulls history toward
            // the current frame's local neighborhood; slight noise cost.
            // A 5-tap CROSS rather than a 3x3 box, using a non-square array
            // constructor -- which is what the GL_EXT_gpu_shader5 enable at the top is
            // for.
            // ---
            // 5 taps instead of 9 at a quarter of the clamp's cost, at the price of a
            // noisier sigma estimate: the cross misses the diagonal neighbours, so the
            // variance is underestimated and the clamp slightly looser.
            ivec2 offsets[5] = ivec2[](ivec2(0, 0), ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
            vec3 m1 = vec3(0.0f);
            vec3 m2 = vec3(0.0f);
            for (int i = 0; i < 5; i++) {
                ivec2 sp = clamp(pixelCoords + offsets[i], ivec2(0), imgSize - ivec2(1));
                vec3 s = RGB2YCoCg(imageLoad(img_Bloom, sp).rgb);
                m1 += s;
                m2 += s * s;
            }
            vec3 mean = m1 / 5.0f;
            vec3 stddev = sqrt(max(vec3(0.0f), m2 / 5.0f - mean * mean));

            vec3 historyYCoCg = RGB2YCoCg(historyLight);
            vec3 minCol = mean - 1.25f * stddev;
            vec3 maxCol = mean + 1.25f * stddev;
            historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
            historyLight = YCoCg2RGB(historyYCoCg);
        }
    }

    // The motion factor: 8 pixels of movement saturates it. Below that the
    // history keeps most of its weight.
    float motionFactor = clamp(motionMag / 8.0f, 0.0f, 1.0f);
    // The blend weight. Invalid history means alpha 1.0 -- i.e. discard the
    // history completely and take the current frame.
    // ---
    // Otherwise alpha ramps from u_AccumulationAlpha toward 0.8 as motion
    // grows, so a fast-moving pixel converges quickly and a static one
    // accumulates heavily. Note that u_AccumulationAlpha is the floor: it
    // sets how much a perfectly still pixel trusts its history.
    float alpha = validHistory ? mix(u_AccumulationAlpha, 0.8f, motionFactor) : 1.0f;

    vec3 result = mix(historyLight, currentLight, alpha);
    imageStore(img_Accumulation, pixelCoords, vec4(result, 1.0f));
}

// Pass 12: temporal moments accumulation.
// Computes running mean (m1) and mean-of-squares (m2) of the luminance
// channel, reprojected to the previous frame. The A-Trous filter (pass 13)
// derives variance from these moments: var = m2 - m1^2.
// --
// The reprojection logic is identical to RunTemporalAccumulation: same
// velocity lookup, same histUV computation, same clamp, same depth test.
// The only difference is that we blend scalar moments instead of RGB colour.
void RunTemporalMoments() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Accumulation));
    vec2 uv = (vec2(pixelCoords) + 0.5f) / vec2(imgSize);

    // Current frame luminance from the bloom buffer (same source as temporal accumulation).
    vec3 currentLight = imageLoad(img_Bloom, pixelCoords).rgb;
    float currentLum = dot(currentLight, vec3(0.2126f, 0.7152f, 0.0722f));

    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    vec2 prevUV = uv - velocity;
    vec2 histUV = prevUV * u_RenderScale;
    vec2 fullRes = vec2(imageSize(img_Accumulation));
    histUV = clamp(histUV, 0.5f / fullRes, vec2(u_RenderScale) - 0.5f / fullRes);

    bool validHistory = u_FrameIndex > 0 && prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f;
    float motionMag = length(velocity * vec2(imgSize));

    if (validHistory) {
        float currentDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
        float prevDepth = texture(s_DepthBuffer, histUV).r;
        if (abs(currentDepth - prevDepth) > 0.02f) validHistory = false;
    }

    vec2 prevMoments = vec2(0.0f);
    if (validHistory) {
        prevMoments = texture(s_Moments, histUV).xy;
    }

    float motionFactor = clamp(motionMag / 8.0f, 0.0f, 1.0f);
    float alpha = validHistory ? mix(u_AccumulationAlpha, 0.8f, motionFactor) : 1.0f;

    float newM1 = mix(prevMoments.x, currentLum, alpha);
    float newM2 = mix(prevMoments.y, currentLum * currentLum, alpha);

    imageStore(img_TraceGBuffer, pixelCoords, vec4(newM1, newM2, 0.0f, 0.0f));
}

// Pass 13: the A-Trous wavelet filter.
// ---
// A fixed 4-iteration edge-preserving filter that reads the temporal moments
// from pass 12 and uses the local variance to drive the edge-stopping weight.
// Higher variance means a wider/stronger filter: the kernel reaches further
// and smooths more aggressively in noisy regions, while flat regions are
// left untouched (kernel weights sum to 1).
//
// The step ladder {1, 2, 4, 8} is the same shape as RunBilateralBlur, but
// applied recursively: each iteration reads the result of the previous one,
// so the effective kernel grows exponentially. This is the classic "a trous"
// (French for "with holes") wavelet decomposition.
//
// The edge-stopping weight combines three terms:
//   - depth difference: prevents blurring across geometry edges
//   - normal difference: prevents blurring across shading discontinuities
//   - variance term: adapts the filter strength to the local noise level
// Pass 13: the A-Trous wavelet filter -- ONE iteration per dispatch.
// ---
// This pass performs a single edge-preserving wavelet iteration at the step
// size held in u_StepSize, reading img_Accumulation and writing img_Bloom_Temp.
// The C++ side drives the 4-step ladder (1, 2, 4, 8) by dispatching this pass
// four times and calling CommitDenoise() between them, which copies
// img_Bloom_Temp back into img_Accumulation -- the same ping-pong mechanism
// RunBilateralBlur already relies on.
//
// WHY ONE ITERATION PER DISPATCH, NOT A LOOP IN THE SHADER:
// a real A-Trous filter must feed iteration N's OUTPUT into iteration N+1. A
// loop that re-reads img_Accumulation on every pass computes four independent
// copies of iteration 1 and keeps only the last, so the step ladder does
// nothing at all -- the filter degenerates to a single 3x3 blur. The loop also
// cannot ping-pong internally: one image unit cannot be read and written in the
// same dispatch without a barrier, and this shader has no resource that is both.
// Driving the ladder from C++ sidesteps both problems with machinery that is
// already here and already tested.
//
// The B3-spline kernel weights (0.375 centre, 0.25 edge, 0.0625 corner) sum to
// 1 at every step, so a flat region comes back unchanged.
void RunATrousFilter() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Accumulation));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    // One step per dispatch, supplied by the C++ ladder (1, 2, 4, 8).
    // max(1,...) guards a zero step, which would collapse the kernel onto the
    // centre tap and turn the whole pass into a no-op.
    int step = max(1, u_StepSize);

    vec3 inputColor = imageLoad(img_Accumulation, pos).rgb;

    // Centre moments. m1 is the accumulated mean luminance and m2 the mean of
    // squares, so the variance is m2 - m1^2. The max against zero matters:
    // m1^2 can exceed m2 by a rounding error, and a negative variance under
    // sqrt is NaN -- which would poison every weight in the kernel at once.
    vec2 moments = imageLoad(img_TraceGBuffer, pos).xy;
    float centerVariance = max(moments.y - moments.x * moments.x, 0.0f);

    vec2 uv = (vec2(pos) + 0.5f) / vec2(imgSize);
    float centerDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;

    // Sky has no depth to edge-stop against; pass it through untouched.
    if (centerDepth >= 1.0f) {
        imageStore(img_Bloom_Temp, pos, vec4(inputColor, 1.0f));
        return;
    }

    // The trace G-buffer packs the world normal into .yzw (see the writer in
    // RunTraceAndDenoise: vec4(ndcDepth, normal * 0.5 + 0.5)), so the normal
    // decodes from .gba -- NOT .rgb, whose .r holds depth.
    vec3 centerNormal = texture(s_NormalBuffer, wl_buf_uv(uv)).gba * 2.0f - 1.0f;

    // Tolerance scale for the luminance weight, in units of the centre's own
    // standard deviation. See the weight's comment below for why it lives here.
    const float sigma_l = 4.0f;

    vec3 totalColor = vec3(0.0f);
    float totalWeight = 0.0f;

    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            ivec2 samplePos = clamp(pos + ivec2(x, y) * step, ivec2(0), imgSize - ivec2(1));
            vec2 sampleUV = (vec2(samplePos) + 0.5f) / vec2(imgSize);

            vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
            float neighborDepth = texture(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
            vec3 neighborNormal = texture(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
            float neighborMean = imageLoad(img_TraceGBuffer, samplePos).x;

            // B3-spline spatial weights: centre 0.375, edge 0.25, corner 0.0625.
            float spatialWeight = (x == 0 && y == 0) ? 0.375f :
                                  ((x == 0 || y == 0) ? 0.25f : 0.0625f);

            // Depth edge-stop. 100 is the sharpness: a depth difference of 1% of
            // the depth range already halves the weight, so silhouettes survive
            // without rejecting a whole sloped surface.
            float depthWeight = exp(-abs(centerDepth - neighborDepth) * 100.0f);

            // Normal edge-stop. The exponent separates two faces meeting at a
            // crease while still admitting neighbours on a gently curved surface:
            // 5 degrees off normal keeps ~0.88, 10 degrees keeps ~0.61.
            float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 32.0f);

            // THE SVGF LUMINANCE WEIGHT -- the term that must vary per tap. It
            // compares the NEIGHBOUR's accumulated mean against the centre's,
            // with the CENTRE's variance setting the tolerance:
            //
            //     w_l = exp( -|mean_c - mean_n| / (sigma_l * sqrt(var_c) + eps) )
            //
            // A noisy region has a large var_c, so the tolerance widens and the
            // filter smooths harder; a converged region has a small var_c, so it
            // backs off and preserves detail. That is the entire point of feeding
            // temporal moments into a spatial filter.
            //
            // The previous version multiplied by (1 + var_c * 5) instead. That
            // term was inert: var_c is the CENTRE pixel's variance, so it was the
            // same constant for all nine taps, and a constant factor cancels
            // exactly in the normalisation below. It changed nothing whatsoever.
            float luminanceWeight =
                exp(-abs(moments.x - neighborMean) / (sigma_l * sqrt(centerVariance) + 1e-4f));

            float weight = spatialWeight * depthWeight * normalWeight * luminanceWeight;
            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    }

    // Normalise so the kernel sums to 1 and a flat region is unchanged.
    vec3 filtered = (totalWeight > 0.0001f) ? (totalColor / totalWeight) : inputColor;
    imageStore(img_Bloom_Temp, pos, vec4(filtered, 1.0f));
}

// The bloom threshold pass.
// ---
// DEFECT (documented, not fixed): this function is never called. main()'s
// switch jumps from case 2 to case 4, so no u_PassID selects it and the
// blur passes below run against an image the trace pass wrote directly --
// i.e. the whole bloom chain threshold against nothing. The case numbers
// 3 and 11 are both absent from the switch, and 3 is the only gap that
// would fit this pass.
// The same defect exists in the .hlsl twin: RunBloomThreshold is defined
// there too and is likewise never dispatched.
void RunBloomThreshold() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 color = imageLoad(img_Output, pos);
    
    float threshold = u_BloomThreshold;
    float knee = 0.1f; 
    
    float brightness = dot(color.rgb, vec3(0.2126f, 0.7152f, 0.0722f));
    // A soft threshold: smoothstep across a knee width of 0.1. A hard cutoff
    // would produce a visible edge where a highlight starts blooming; the knee
    // makes the onset gradual.
    // ---
    // The threshold now comes from u_BloomThreshold. It used to be hardcoded
    // 0.5 here while the composite used 0.8, so the two passes disagreed
    // about what counted as bright.
    float soft = smoothstep(threshold - knee, threshold + knee, brightness);
    
    imageStore(img_Bloom, pos, color * soft);
}

void RunBloomBlurHorizontal() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    // A 5-tap Gaussian, weights summing to 1.0. The values are the binomial
    // approximation -- separable, so this is 5 taps horizontally and 5
    // vertically rather than 25 in 2D.
    float weights[5] = float[](0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f);
    vec4 sum = imageLoad(img_Bloom, pos) * weights[0];
    
    for(int i = 1; i < 5; i++) {
        sum += imageLoad(img_Bloom, pos + ivec2(i, 0)) * weights[i];
        sum += imageLoad(img_Bloom, pos - ivec2(i, 0)) * weights[i];
    }
    // Read from img_Bloom, write to img_Bloom_Temp. The ping-pong is mandatory:
    // a horizontal blur reads neighbours of the pixel it is writing, which
    // would otherwise feed its own output back in.
    imageStore(img_Bloom_Temp, pos, sum);
}

void RunBloomBlurVertical()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    // The same weights again. Duplicated rather than shared because a GLSL
    // global float array would have to be initialised at global scope, and the
    // duplication is 1 line.
    float weights[5] = float[](0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f);
    vec4 sum = imageLoad(img_Bloom_Temp, pos) * weights[0];

    for(int i = 1; i < 5; i++) {
        sum += imageLoad(img_Bloom_Temp, pos + ivec2(0, i)) * weights[i];
        sum += imageLoad(img_Bloom_Temp, pos + ivec2(0, -i)) * weights[i];
    }

    // The vertical pass is where the bloom is ADDED back to the output, not
    // written over it -- so this pass both finishes the blur and composites.
    vec3 existing = imageLoad(img_Output, pos).rgb;
    // u_BloomIntensity was unreachable before; the sum was added at full weight.
    // Scaled by u_BloomIntensity, which was unreachable before: the sum was
    // added at full weight, so the editor's bloom intensity slider did nothing.
    vec3 finalColor = existing + sum.rgb * u_BloomIntensity;
    imageStore(img_Output, pos, vec4(finalColor, 1.0f));
}

// Bilinear tap into img_Accumulation at a full-frame UV, honouring the internal
// render scale. Extracted from the upscale block below so chromatic aberration
// can resample the same way instead of duplicating (and drifting from) it.
// A bilinear tap into the subregion-scale accumulation buffer from a
// full-frame UV.
// ---
// Extracted specifically so chromatic aberration resamples identically to
// the upscale. The comment above names the failure it prevents: two copies
// of this arithmetic would drift.
vec3 BilinearAccum(vec2 uv, float renderScale)
{
    ivec2 fullSize = imageSize(img_Output);
    ivec2 srcSize = ivec2(vec2(fullSize) * max(renderScale, 0.001f));
    srcSize = max(srcSize, ivec2(1));

    vec2 srcCoord = uv * vec2(srcSize);
    // The half-texel shift, so a full-frame UV maps to the CENTRE of the source
    // texel rather than to its corner. Skipping it shifts the image by half a
    // source pixel -- a subtle but real blur on every frame.
    vec2 srcPixel = srcCoord - vec2(0.5f);
    ivec2 s0 = clamp(ivec2(floor(srcPixel)), ivec2(0), srcSize - ivec2(1));
    ivec2 s1 = clamp(s0 + ivec2(1), ivec2(0), srcSize - ivec2(1));
    vec2 f = clamp(srcPixel - vec2(s0), vec2(0.0f), vec2(1.0f));

    vec3 c00 = imageLoad(img_Accumulation, s0).rgb;
    vec3 c10 = imageLoad(img_Accumulation, ivec2(s1.x, s0.y)).rgb;
    vec3 c01 = imageLoad(img_Accumulation, ivec2(s0.x, s1.y)).rgb;
    vec3 c11 = imageLoad(img_Accumulation, s1).rgb;
    // Two sequential lerps: x first, then y. That is bilinear, and the order
    // does not matter -- but doing it as one 4-way mix would need the same
    // weights computed differently.
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

// PASS 4: the composite. Upscale the accumulation buffer, apply the
// post-processing chain, tonemap, and write the final image.
void RunComposite() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    // img_Accumulation / img_Bloom_Temp already hold FINAL shaded color
    // (albedo * direct + indirect, see RunTraceAndDenoise), so it must NOT
    // be multiplied by albedo again here. The old `denoised * albedo` applied
    // albedo twice (albedo^2 on direct, albedo-scaled indirect) and pushed
    // textured surfaces (e.g. the floor) toward black.
    // The accumulation buffer holds FINAL shaded colour already.
    // ---
    // The long comment above records a fixed defect: an earlier version applied
    // albedo a second time here, so albedo landed squared on direct light and
    // scaled the indirect term, pushing textured surfaces toward black.
    vec3 denoised;

    if (u_RenderScale < 1.0f) {
        ivec2 srcSize = ivec2(vec2(fullSize) * u_RenderScale);
        if (srcSize.x < 1) srcSize.x = 1;
        if (srcSize.y < 1) srcSize.y = 1;

        // Map the output pixel into the source subregion, then to texel space and
        // off the half-texel again -- the same sequence as BilinearAccum.
        vec2 srcCoord = (vec2(pos) + 0.5f) * u_RenderScale;
        vec2 srcPixel = srcCoord - vec2(0.5f);
        ivec2 s0 = clamp(ivec2(floor(srcPixel)), ivec2(0), srcSize - ivec2(1));
        ivec2 s1 = clamp(s0 + ivec2(1), ivec2(0), srcSize - ivec2(1));
        // Clamp the interpolation weights to [0,1].
        // ---
        // At a frame border srcPixel can go slightly outside the source extent,
        // where unclamped weights would EXTRAPOLATE and ring a bright or dark
        // edge along the image border.
        vec2 f = clamp(srcPixel - vec2(s0), vec2(0.0f), vec2(1.0f));

        // THE RECONSTRUCTION LADDER. This used to be a hardcoded bilinear, which
        // is why the AA dropdown in the editor had no effect: the uniform was
        // uploaded and then dropped because the shader never declared it. Each
        // branch below is a different way of rebuilding one output pixel from the
        // trace-resolution image, and they are ordered cheapest-first.
        //
        // The tap weights are computed from the SAME fractional coordinate `f`
        // that bilinear used, so switching filters changes the reconstruction
        // kernel and nothing else -- no change to the trace, the accumulation or
        // the denoiser.
        //
        // Values outside the spatial range (Temporal, FSR1, TSR, SMAA, FXAA,
        // SpatialDenoise) are separate passes, not reconstruction filters; they
        // fall through to bilinear rather than being ignored, so picking one
        // degrades gracefully instead of appearing to do something it cannot.
        if (u_ReconstructionFilter == 1)
        {
            // NEAREST -- 1 tap, point sampling. The cheapest possible upscale,
            // and the most aliased: it is what a "no AA" mode looks like, which
            // is a legitimate choice for a diagnostic baseline.
            denoised = imageLoad(img_Accumulation, s0).rgb;
        }
        else if (u_ReconstructionFilter == 3)
        {
            // BOX -- 4 tap unweighted. The 2x2 footprint bilinear samples, with
            // the weights forced to 0.5 regardless of position. Softer than
            // bilinear at edges because it does not interpolate, which is the
            // trade: less ringing, more blur.
            vec3 b00 = imageLoad(img_Accumulation, ivec2(s0.x, s0.y)).rgb;
            vec3 b10 = imageLoad(img_Accumulation, ivec2(s1.x, s0.y)).rgb;
            vec3 b01 = imageLoad(img_Accumulation, ivec2(s0.x, s1.y)).rgb;
            vec3 b11 = imageLoad(img_Accumulation, ivec2(s1.x, s1.y)).rgb;
            denoised = (b00 + b10 + b01 + b11) * 0.25f;
        }
        else if (u_ReconstructionFilter == 4)
        {
            // BICUBIC -- 16 tap Catmull-Rom. The classic "sharp" upscale: it
            // overshoots slightly at edges, which reads as crispness on geometry
            // and as ringing on high-contrast ones. The 1D kernel is applied
            // separably, which is why this is 16 taps rather than 16 separate
            // 2D samples. Weights are normalised by their sum, so the filter
            // cannot darken or brighten the image as a whole.
            vec3 acc = vec3(0.0f);
            float wsum = 0.0f;
            for (int j = -1; j <= 2; j++)
            {
                float fy = wl_cubic_kernel(float(j) - f.y);
                for (int i = -1; i <= 2; i++)
                {
                    float w = wl_cubic_kernel(float(i) - f.x) * fy;
                    ivec2 c = clamp(s0 + ivec2(i, j), ivec2(0), srcSize - ivec2(1));
                    acc += imageLoad(img_Accumulation, c).rgb * w;
                    wsum += w;
                }
            }
            denoised = acc / max(wsum, 1e-5f);
        }
        else if (u_ReconstructionFilter == 5)
        {
            // LANCZOS -- 12 tap, 3-lobe windowed sinc. The strongest of the
            // spatial filters: best edge preservation of the group, at the cost
            // of the most ringing. The window is what keeps the negative lobes
            // from making the kernel unbounded.
            vec3 acc = vec3(0.0f);
            float wsum = 0.0f;
            for (int j = -2; j <= 3; j++)
            {
                float fy = wl_lanczos3(float(j) - f.y);
                for (int i = -2; i <= 3; i++)
                {
                    float w = wl_lanczos3(float(i) - f.x) * fy;
                    ivec2 c = clamp(s0 + ivec2(i, j), ivec2(0), srcSize - ivec2(1));
                    acc += imageLoad(img_Accumulation, c).rgb * w;
                    wsum += w;
                }
            }
            denoised = acc / max(wsum, 1e-5f);
        }
        else
        {
            // BILINEAR (the default) and the fall-through for every filter that
            // is a separate pass rather than a reconstruction kernel.
            vec3 c00 = imageLoad(img_Accumulation, s0).rgb;
            vec3 c10 = imageLoad(img_Accumulation, ivec2(s1.x, s0.y)).rgb;
            vec3 c01 = imageLoad(img_Accumulation, ivec2(s0.x, s1.y)).rgb;
            vec3 c11 = imageLoad(img_Accumulation, s1).rgb;
            denoised = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
        }
    } else {
        // At full resolution there is no upscale to do, so the composite reads the
        // accumulation buffer directly -- except at Ultra, where the bloom chain
        // left its final blurred result in img_Bloom_Temp.
        denoised = (u_QualityLevel >= 3) ? imageLoad(img_Bloom_Temp, pos).rgb : imageLoad(img_Accumulation, pos).rgb;
    }

    vec3 combined = denoised;

    vec2 uv = (vec2(pos) + 0.5f) / vec2(fullSize);
    vec2 centered = uv * 2.0f - 1.0f;

    // CHROMATIC ABERRATION. Samples the accumulation buffer per channel at
    // slightly different radial offsets. Done here, on the upscaled image,
    // rather than during the trace: it is a lens/sensor effect, so it belongs
    // after the resolve, and it costs one extra bilinear tap per channel.
    // Chromatic aberration, sampled per channel at opposite radial offsets.
    // ---
    // The comment above justifies doing it here rather than during the trace:
    // it is a lens effect, so it belongs after the resolve. The cost is one
    // extra bilinear tap, shared with the upscale path.
    if (u_ChromaticAberrationEnabled == 1 && u_ChromaticAberrationIntensity > 0.001f) {
        // The offset is intensity x 0.01 -- so the uniform is a scale factor where
        // 1.0 means a 1% radial shift. A very small default maximum, which is
        // appropriate: this effect reads as a defect almost immediately.
        float caAmount = u_ChromaticAberrationIntensity * 0.01f;
        vec2 dir = centered * caAmount;
        combined.r = BilinearAccum(uv + dir, u_RenderScale).r;
        combined.b = BilinearAccum(uv - dir, u_RenderScale).b;
    }

    // VIGNETTE. Radial falloff from the frame centre, with u_VignetteRadius
    // setting how far the darkening starts and u_VignetteSoftness how gradually
    // it ramps in. Smoothstep gives the soft shoulder; the pow() keeps the
    // centre genuinely untouched instead of merely near-1.
    if (u_VignetteEnabled == 1 && u_VignetteIntensity > 0.001f) {
        // The vignette radius, normalised so r = 1 is where the falloff begins.
        // Dividing rather than comparing directly means u_VignetteRadius is in
        // half-screen units -- 1.0 puts the start at the frame's half-diagonal.
        float r = length(centered) / max(u_VignetteRadius, 0.001f);
        // smoothstep from the radius INWARD to radius-minus-softness, so the darkening
        // ramps up as r passes 1. The softness is clamped away from 0 so the two
        // smoothstep edges can never cross.
        float vig = smoothstep(1.0f, 1.0f - max(u_VignetteSoftness, 0.001f), r);
        combined *= mix(1.0, vig, clamp(u_VignetteIntensity, 0.0, 2.0));
    }

    // TONEMAP (Reinhard), then colour grading in display space.
    // Reinhard tonemapping: c / (c + 1).
    // ---
    // The simplest of the tonemap curves -- it rolls off asymptotically, so
    // highlights compress smoothly but colours desaturate toward white as they
    // approach the asymptote. Extended Reinhard with a white point, or ACES,
    // would preserve saturation better; this is the cheap choice.
    vec3 mapped = combined / (combined + vec3(1.0f));

    if (u_ColorGradingEnabled == 1) {
        // Temperature and tint are signed scalars, not colours: temperature
        // scales R/B against G, tint scales G against R+B. The 0.04 offsets
        // keep a neutral setting from shifting the image.
        // Colour grading as signed offsets. The 0.04/0.06 terms mean a 'neutral'
        // setting still shifts the image slightly -- they are a bias to keep the
        // adjustment centred, not a correction.
        mapped.r += u_ColorTemperature * 0.10 + u_ColorTint * 0.04;
        mapped.b -= u_ColorTemperature * 0.10 - u_ColorTint * 0.04;
        mapped.g += u_ColorTint * 0.06;
        mapped = clamp(mapped, vec3(0.0), vec3(1.0));
    }

    // The gamma encode, with the exponent floored at 0.1. A gamma of 0 would
    // mean an infinite exponent, so the floor keeps a mis-set slider from
    // producing infinities.
    vec3 finalColor = pow(mapped, vec3(1.0f / max(u_ColorGamma, 0.1f)));

    // FILM GRAIN. Animated per frame and scaled by local luminance so it sits
    // in the midtones instead of crushing the blacks. This runs AFTER gamma, so
    // the grain is perceptually even rather than being crushed by the curve.
    if (u_FilmGrainEnabled == 1 && u_FilmGrainIntensity > 0.001f) {
        // The classic sin-dot-fract hash, offset by the frame index times 17 so
        // the grain animates.
        // ---
        // This is the one stochastic value in the composite that is deliberately
        // NOT temporally stable -- grain that did not move would read as a dirt
        // texture rather than as film.
        float n = fract(sin(dot(pos + vec2(float(u_FrameIndex) * 17.0f), vec2(12.9898f, 78.233f))) * 43758.5453f);
        float lum = dot(finalColor, vec3(0.2126f, 0.7152f, 0.0722f));
        // Grain amplitude weighted by (1 - |2*luma - 1|), which peaks in the
        // midtones and falls to zero at both black and white.
        // ---
        // The comment above explains why this runs after gamma: applying grain in
        // linear space would crush almost all of it into the shadows, where the
        // display curve compresses hardest.
        finalColor += (n - 0.5) * u_FilmGrainIntensity * 0.15 * (1.0 - abs(lum * 2.0 - 1.0));
    }

    finalColor = clamp(finalColor, 0.0, 1.0);
    imageStore(img_Output, pos, vec4(finalColor, 1.0f));

    // Bloom threshold must run at ANY scale: passes 4/5 blur img_Bloom at full
    // res, so leaving stale trace-subregion data in it (by skipping this when
    // scaled) smears a ghost copy of the frame over the output.
    // Bloom source. Threshold was hardcoded 0.8f here while RunBloomThreshold
    // used 0.5f, so the same effect had two different thresholds depending on
    // which path produced it. Both now read u_BloomThreshold.
    // Bloom is off entirely at Low quality -- the one quality gate on this
    // effect, and it means the blur passes have nothing to blur there.
    if (u_QualityLevel >= 1) {
        float brightness = dot(combined, vec3(0.2126f, 0.7152f, 0.0722f));
        float threshold = u_BloomThreshold;
        // The knee here is 0.15, against the 0.1 in RunBloomThreshold. Two
        // different knee widths for the same effect, which is a small inconsistency
        // between the two bloom paths.
        float knee = 0.15f;
        float soft = smoothstep(threshold - knee, threshold + knee, brightness);
        // The bloom SOURCE, written from the composite itself.
        // ---
        // This is the path that actually works. The comment above records why it
        // must run at every scale: passes 4/5 blur img_Bloom at full resolution, so
        // skipping this when scaled would leave stale trace-subregion data in that
        // image and smear a ghost copy of the frame over the output.
        // ---
        // It also records the old defect: this hardcoded 0.8 while
        // RunBloomThreshold used 0.5, so the same effect had two thresholds
        // depending on which path produced it.
        imageStore(img_Bloom, pos, vec4(combined * soft, 1.0f));
    }
}

// The bilateral blur, for edge-aware spatial denoising at High and Ultra.
//
// RESTORED INLINE after a shared-algorithm probe was REJECTED. The probe moved this pass
// into NovaShared_Bilateral.h. Reverted because the GLSL half then required
// GL_GOOGLE_include_directive, which OpenGLShader::PreProcess cannot provide (it only
// splits the source on its "#type" markers and never expands an include), so the
// OpenGL backend would have silently depended on a driver extension the engine never
// checks -- while tools/verify_nova_glsl.py still passed, which is the worst possible
// failure mode: green verification, broken backend. The verifier also guards only
// validHistory / histUV / mipLevel / GetJitter, so a silent SEMANTIC change inside a
// shared pass would still slip through. The body below is the pre-probe text, verbatim.
// See docs/NOVA_SHADER_PIPELINE.md.
void RunBilateralBlur() {
    if (u_QualityLevel <= 1) return;

    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize = wl_trace_extent(imageSize(img_Bloom_Temp));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    vec3 centerColor = imageLoad(img_Accumulation, pos).rgb;
    
    if (isnan(centerColor.r) || isinf(centerColor.r)) {
        imageStore(img_Bloom_Temp, pos, vec4(0.0f, 0.0f, 0.0f, 1.0f));
        return;
    }

    vec2 uv = (vec2(pos) + 0.5f) / vec2(imgSize);
    float centerDepth = texture(s_DepthBuffer, wl_buf_uv(uv)).r;
    if (centerDepth >= 1.0f) { 
        imageStore(img_Bloom_Temp, pos, vec4(centerColor, 1.0f));
        return;
    }

    vec3 centerNormal = texture(s_NormalBuffer, wl_buf_uv(uv)).gba * 2.0f - 1.0f;
    float centerLuma = GetLuminance(centerColor);

    int step = int(max(1.0f, float(u_StepSize)));
    vec3 totalColor = vec3(0.0f);
    float totalWeight = 0.0f;

    if (u_QualityLevel == 2) {
        ivec2 offsets[9] = ivec2[](
            ivec2(0, 0), 
            ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
            ivec2(-2, 0), ivec2(2, 0), ivec2(0, -2), ivec2(0, 2)
        );
        for (int i = 0; i < 9; i++) {
            ivec2 samplePos = clamp(pos + offsets[i] * step, ivec2(0), imgSize - ivec2(1));

            vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
            vec2 sampleUV = (vec2(samplePos) + 0.5f) / vec2(imgSize);

            float neighborDepth = texture(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
            vec3 neighborNormal = texture(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
            float neighborLuma = GetLuminance(neighborColor);

            float spatialWeight = (i == 0) ? 0.375f : ((i <= 4) ? 0.125f : 0.03125f);
            float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 16.0f);
            float depthDiff = abs(centerDepth - neighborDepth);
            float depthWeight = exp(-depthDiff * 100.0f);
            float lumaDiff = abs(centerLuma - neighborLuma);
            float lumaWeight = exp(-lumaDiff * 4.0f);

            float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    } else if (u_QualityLevel >= 3) {
        float lumaSum = 0.0f;
        float lumaSqSum = 0.0f;
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                ivec2 sp = clamp(pos + ivec2(x, y), ivec2(0), imgSize - ivec2(1));
                float l = GetLuminance(imageLoad(img_Accumulation, sp).rgb);
                lumaSum += l;
                lumaSqSum += l * l;
            }
        }
        float lumaMean = lumaSum / 9.0f;
        float lumaVar = sqrt(max(0.0f, (lumaSqSum / 9.0f) - (lumaMean * lumaMean)));

        const float kernel[3] = float[](0.375f, 0.25f, 0.0625f);

        for (int x = -2; x <= 2; x++) {
            for (int y = -2; y <= 2; y++) {
                ivec2 samplePos = clamp(pos + ivec2(x, y) * step, ivec2(0), imgSize - ivec2(1));

                vec3 neighborColor = imageLoad(img_Accumulation, samplePos).rgb;
                vec2 sampleUV = (vec2(samplePos) + 0.5f) / vec2(imgSize);

                float neighborDepth = texture(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
                vec3 neighborNormal = texture(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
                float neighborLuma = GetLuminance(neighborColor);

                float spatialWeight = kernel[abs(x)] * kernel[abs(y)];
                float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 32.0f);
                float depthDiff = abs(centerDepth - neighborDepth);
                float depthWeight = exp(-depthDiff * 100.0f);
                float lumaDiff = abs(centerLuma - neighborLuma);
                float lumaWeight = exp(-lumaDiff / (lumaVar * 4.0f + 0.001f));

                float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

                totalColor += neighborColor * weight;
                totalWeight += weight;
            }
        }
    }

    vec3 finalFiltered = (totalWeight > 0.0001f) ? (totalColor / totalWeight) : centerColor;
    imageStore(img_Bloom_Temp, pos, vec4(finalFiltered, 1.0f));
}
// Derive a packed normal/roughness/AO map from the rasterised albedo, for
// the raster pipeline rather than the path tracer.
// ---
// The same relief the traced path computes with POM, here as a 3x3 Sobel
// over the albedo image. It reads s_InputAlbedo -- the RASTERISED buffer,
// not the traced one -- so this pass is independent of the path tracer.
void GenerateMaterialMaps() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(img_MaterialPacked);
    
    // A 3x3 luminance neighbourhood, gathered before the convolution so each
    // texel is fetched once and reused by both gradients.
    float I[3][3];
    for(int i = -1; i <= 1; i++) {
        for(int j = -1; j <= 1; j++) {
            ivec2 samplePos = clamp(pos + ivec2(i, j), ivec2(0), size - ivec2(1));
            vec2 uv = (vec2(samplePos) + 0.5f) / vec2(size);
            I[i+1][j+1] = GetLuminance(texture(s_InputAlbedo, uv).rgb);
        }
    }

    float resX = 0.0f;
    float resY = 0.0f;
    for(int i = 0; i < 3; i++) {
        for(int j = 0; j < 3; j++) {
            resX += I[i][j] * Gx[i][j];
            resY += I[i][j] * Gy[i][j];
        }
    }

    // The normal strength, an editor-facing multiplier on the Sobel result.
    // Applied to both components together, so the slider scales the derived
    // normal's tilt uniformly.
    float normalStrength = u_NormalStrength; 
    // Normal from the height gradient, then normalised. The z is fixed at 1
    // and the xy scaled by the gradient -- so a flat region gives exactly
    // (0,0,1) with no special case.
    vec3 normal = normalize(vec3(-resX * normalStrength, -resY * normalStrength, 1.0f));

    // Only xy are packed, into 0..1. The reconstruction is the same *2-1
    // unpack used for the trace G-buffer, so the two representations match.
    // ---
    // Storing 2 channels rather than 3 loses the reconstructed z, which is
    // recoverable as sqrt(1 - x^2 - y^2) since the normal is unit length -- so
    // nothing is actually lost.
    vec2 packedNormal = normal.xy * 0.5f + 0.5f;
    
    // Ambient occlusion from the gradient magnitude: strong edges darken.
    // ---
    // Note the 5.0 constant and that this can go NEGATIVE -- there is no clamp
    // before the pack. An unclamped negative AO packs below 0 and reads back as
    // a brightened surface. The fixed defect class here is a missing clamp, not
    // a wrong formula.
    float ao = 1.0f - (length(vec2(resX, resY)) * 5.0f * u_AOIntensity);
    // Roughness as the INVERSE of the centre luminance -- dark pixels are read
    // as rough, bright as smooth. A convention, not a measurement, plus the
    // bias uniform and a clamp to a valid range.
    float rough = clamp((1.0f - I[1][1]) + u_RoughnessBias, 0.0f, 1.0f);
    
    vec4 packedData = vec4(packedNormal.x, packedNormal.y, rough, ao);
    
    imageStore(img_MaterialPacked, pos, packedData);
}

// A plain bilinear upscale of the bilateral-blur output into the final
// image.
void RunUpscale() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    ivec2 srcSize = ivec2(vec2(fullSize) * u_RenderScale);
    if (srcSize.x < 1) srcSize.x = 1;
    if (srcSize.y < 1) srcSize.y = 1;

    vec2 srcCoord = (vec2(pos) + 0.5f) * u_RenderScale;
    vec2 srcPixel = srcCoord - vec2(0.5f);
    // The floor is taken BEFORE the clamp below, so a negative src0 is possible
    // for a moment -- which is why the clamp is not optional here.
    ivec2 src0 = ivec2(floor(srcPixel));
    ivec2 src1 = src0 + ivec2(1);
    // The fractional weight between the two taps. Unlike the composite's
    // version this is NOT clamped to 0..1, because srcPixel is always within
    // [0, srcSize) after the two clamps -- so the fraction is in range by
    // construction.
    vec2 frac_ = srcPixel - vec2(src0);

    src0 = clamp(src0, ivec2(0), srcSize - ivec2(1));
    src1 = clamp(src1, ivec2(0), srcSize - ivec2(1));

    vec3 c00 = imageLoad(img_Bloom_Temp, src0).rgb;
    vec3 c10 = imageLoad(img_Bloom_Temp, ivec2(src1.x, src0.y)).rgb;
    vec3 c01 = imageLoad(img_Bloom_Temp, ivec2(src0.x, src1.y)).rgb;
    vec3 c11 = imageLoad(img_Bloom_Temp, src1).rgb;

    vec3 c0 = mix(c00, c10, frac_.x);
    vec3 c1 = mix(c01, c11, frac_.x);
    vec3 color = mix(c0, c1, frac_.y);

    imageStore(img_Output, pos, vec4(color, 1.0f));
}

// The single entry point. One shader, eleven dispatch sites, selected by
// u_PassID -- cheaper than binding a separate program per stage.
// ---
// DEFECT (documented, not fixed): cases 3 and 11 are MISSING. The sequence
// runs 0,1,2,4,5,6,7,8,9,10 -- there is no case 3 and no case 11.
//
// The gap at 3 is the consequential one: RunBloomThreshold is defined in
// this file and is never dispatched by ANY value of u_PassID. So the
// standalone threshold pass is dead code, and the bloom chain relies on the
// inline threshold written at the end of RunComposite instead.
//
// Consequence for a CPU dispatcher: anything that uploads u_PassID = 3
// falls through the switch and runs NOTHING -- the dispatch is silently
// a no-op that still costs a full group's execution.
//
// The identical defect exists in the .hlsl twin: its main() has the same
// cases 0,1,2,4,5,6,7,8,9,10 with RunBloomThreshold defined and never
// dispatched.

// ============================================================================
// ReSTIR DI Reservoir (Bitterli et al. ACM TOG 2020)
// ============================================================================
// The reservoir is a per-pixel data structure that stores one selected light
// sample. Each frame, M candidates are drawn from the light list; the
// reservoir updates via weighted reservoir sampling. Temporal reuse combines
// the current reservoir with the previous frame's (reprojected). Spatial
// reuse combines with neighbouring pixels' reservoirs.
//
// The MIS combine is Alg. 4 from the paper: the weight W is
//   W = (targetPdf(selected) / p_hat(selected)) * (1 / M)
// where p_hat is the PDF of the sampling strategy that produced the sample.
// For uniform light sampling, p_hat = 1/N, so W = targetPdf * N / M.

// Octahedral normal encoding for receiver-surface validation
uint wl_oct_encode(vec3 n) {
    vec2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 signNotZero = vec2(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
    vec2 e = (n.z >= 0.0f) ? p : (1.0f - abs(p.yx)) * signNotZero;
    return uint((e.x * 0.5f + 0.5f) * 65535.0f) | (uint((e.y * 0.5f + 0.5f) * 65535.0f) << 16);
}

vec3 wl_oct_decode(uint encoded) {
    vec2 e = vec2(encoded & 0xffffu, encoded >> 16) / 65535.0f * 2.0f - 1.0f;
    vec3 n = vec3(e, 1.0f - abs(e.x) - abs(e.y));
    float fold = max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -fold : fold;
    n.y += n.y >= 0.0f ? -fold : fold;
    return normalize(n);
}

bool wl_restir_has_sample(LightReservoir r) {
    return (r.flagsValid & 1u) != 0u;
}

// Tag each stored reservoir with the frame that produced it. Temporal reuse
// accepts only the immediately previous frame, rejecting uninitialized or stale records.
bool wl_restir_is_previous_frame(LightReservoir r) {
    uint previousFrameTag = (uint(u_FrameIndex) - 1u) & 0x7fffffffu;
    // M proves the record was written; bit 0 independently indicates whether
    // that record has a selected sample. Keep zero-target proposal counts.
    return r.M > 0 && (r.flagsValid >> 1) == previousFrameTag;
}

bool wl_restir_is_current_frame(LightReservoir r) {
    uint currentFrameTag = uint(u_FrameIndex) & 0x7fffffffu;
    return r.M > 0 && (r.flagsValid >> 1) == currentFrameTag;
}

// Reservoir update: incorporate one candidate sample into the reservoir.
// targetPdf: the target function value (e.g. light contribution weight)
// sourcePdf: the PDF of the sampling strategy that produced this sample
// rnd: random number in [0,1)
void ReservoirUpdate(inout LightReservoir r, uint lightIdx, float targetPdf, float sourcePdf, float rnd) {
    // M is set to the total number of proposals after the candidate loop, so
    // zero-weight proposals (occluded/back-facing/zero-emission lights) count
    // too. This keeps the estimator normalized to the actual proposal budget.
    float w = (sourcePdf > 0.0f) ? (max(targetPdf, 0.0f) / sourcePdf) : 0.0f;
    float newSum = r.wSum + w;
    if (w > 0.0f && rnd * newSum < w) {
        r.lightIndex = lightIdx;
        r.bary.x = targetPdf; // selected target value, used to finalize W
        r.flagsValid = 1;
    }
    r.wSum = newSum;
}

// Target used by this first ReSTIR DI slice: diffuse Lambert response to a
// point-sampled emissive instance. The light-list model matches the existing
// center-point source approximation; metal/specular and area-emitter sampling
// are not represented by the G-buffer data used here.
float RestirTarget(uint lightListIndex, vec3 receiverPos, vec3 receiverNormal, vec3 receiverAlbedo) {
    uint lightCount = uint(min(u_LightCount, 64));
    if (lightListIndex >= lightCount) return 0.0f;
    int instanceIndex = LightIndices[lightListIndex];
    RayTracingInstance light = Instances[instanceIndex];
    vec3 toLight = light.WorldTransform[3].xyz - receiverPos;
    float distanceToLight = length(toLight);
    if (distanceToLight <= 0.001f) return 0.0f;
    vec3 dirToLight = toLight / distanceToLight;
    float NdotL = max(dot(receiverNormal, dirToLight), 0.0f);
    if (NdotL <= 0.0f) return 0.0f;
    Ray shadowRay = Ray(receiverPos + receiverNormal * 0.001f, dirToLight);
    if (IsOccluded(shadowRay, distanceToLight, instanceIndex)) return 0.0f;
    vec3 emission = light.Emission.xyz * light.Emission.w;
    vec3 diffuseContribution = receiverAlbedo * emission * (NdotL / PI);
    return max(dot(diffuseContribution, vec3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

// Reuse a previous-frame reservoir at this receiver. Its selected target must
// be re-evaluated here; old W alone is not a valid merge weight after reprojection.
// Algorithm 6 source-support correction: the history's represented proposals
// count toward the denominator only when the history's source PDF supports the
// current target (historyTarget > 0). Unconditional total-M normalization is
// biased under source-support mismatch.
LightReservoir CombineReservoirs(LightReservoir current, LightReservoir history,
                                 float currentTarget, float historyTarget, float rnd) {
    LightReservoir result = current;
    float currentSum = (wl_restir_has_sample(current) && currentTarget > 0.0f) ? max(current.wSum, 0.0f) : 0.0f;
    float currentM = float(current.M);
    float historyCount = (history.M > 0)
        ? min(float(history.M), max(0.0f, 32.0f - currentM)) : 0.0f;
    float historyWeight = historyTarget * max(history.W, 0.0f) * historyCount;
    float totalWeight = currentSum + historyWeight;
    // Z(y) = sum_i M_i * 1[pHat_qi(y) > 0]: the history source supports the
    // current target only when historyTarget > 0; otherwise its M is excluded.
    float totalM = currentM + ((historyTarget > 0.0f) ? historyCount : 0.0f);

    result.wSum = totalWeight;
    result.M = uint(totalM);
    result.flagsValid = 0;
    result.W = 0.0f;
    if (totalWeight > 1e-8f && totalM > 0.0f) {
        if (historyWeight > 0.0f && rnd * totalWeight < historyWeight) {
            result.lightIndex = history.lightIndex;
            result.bary.x = historyTarget;
            result.flagsValid = 1;
        } else if (currentSum > 0.0f && currentTarget > 0.0f) {
            result.bary.x = currentTarget;
            result.flagsValid = 1;
        }
        if (result.flagsValid != 0 && result.bary.x > 1e-8f)
            result.W = totalWeight / (result.bary.x * totalM);
    }
    return result;
}

// Pass 3: ReSTIR Generate
// For each pixel, draw M candidates from the light list and update the reservoir.
// This replaces the stochastic subset in ComputeDirectLighting.
void RunRestirGenerate() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Accumulation);
    ivec2 imgSize = wl_trace_extent(fullSize);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    // Integer boundaries partition the full-res plane into non-overlapping
    // tiles. Supported texture extents keep these 32-bit coordinate products bounded.
    ivec2 reservoirMin = pixelCoords * fullSize / imgSize;
    ivec2 reservoirMax = (pixelCoords + ivec2(1)) * fullSize / imgSize;
    int flatIdx = reservoirMin.y * fullSize.x + reservoirMin.x;
    LightReservoir r;
    r.lightIndex = 0;
    r.M = 0;
    r.W = 0.0f;
    r.wSum = 0.0f;
    r.bary = vec2(0.0f);
    r.normalOct = 0;
    r.flagsValid = 0;

    vec4 gbuffer = imageLoad(img_TraceGBuffer, pixelCoords);
    float depth = gbuffer.r;
    if (depth < 1.0f) {
        vec2 uv = (vec2(pixelCoords) + 0.5f) / vec2(imgSize);
        vec2 ndc = uv * 2.0f - 1.0f;
        vec4 worldPos4 = u_InverseViewProjection * vec4(ndc, depth * 2.0f - 1.0f, 1.0f);
        vec3 worldPos = worldPos4.xyz / worldPos4.w;
        vec3 normal = normalize(gbuffer.gba * 2.0f - 1.0f);
        r.normalOct = wl_oct_encode(normal);
        r.bary.y = depth; // previous receiver depth for temporal rejection

        vec4 albedoHit = imageLoad(img_AlbedoHit, pixelCoords);
        int M = 4; // proposals per pixel per frame
        int lightCount = min(u_LightCount, 64);
        if (albedoHit.a >= 0.5f && lightCount > 0) {
            seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x) + uint(u_FrameIndex) * 0x9e3779b9u;
            float sourcePdf = 1.0f / float(lightCount);
            for (int m = 0; m < M; m++) {
                uint lightIdx = min(uint(hash() * lightCount), uint(lightCount - 1));
                int i = LightIndices[lightIdx];
                RayTracingInstance light = Instances[i];
                vec3 toLight = light.WorldTransform[3].xyz - worldPos;
                float distToLight = length(toLight);
                float targetPdf = 0.0f;
                if (distToLight > 0.001f) {
                    vec3 dirToLight = toLight / distToLight;
                    float NdotL = max(dot(normal, dirToLight), 0.0f);
                    if (NdotL > 0.0f) {
                        Ray shadowRay = Ray(worldPos + normal * 0.001f, dirToLight);
                        if (!IsOccluded(shadowRay, distToLight, i)) {
                            vec3 emission = light.Emission.xyz * light.Emission.w;
                            vec3 diffuseContribution = albedoHit.rgb * emission * (NdotL / PI);
                            targetPdf = dot(diffuseContribution, vec3(0.2126f, 0.7152f, 0.0722f));
                        }
                    }
                }
                // Submit every draw, including zero-target candidates, so M is
                // the actual proposal count for the uniform source distribution.
                ReservoirUpdate(r, lightIdx, targetPdf, sourcePdf, hash());
            }
            r.M = M;
            if (r.flagsValid != 0 && r.bary.x > 1e-8f)
                r.W = r.wSum / (float(M) * r.bary.x);
        }
    }

    if (r.M > 0)
        r.flagsValid = (r.flagsValid & 1u) | ((uint(u_FrameIndex) & 0x7fffffffu) << 1);
    // Write the tile representative once, then fill every other slot in the tile.
    ReservoirWrite[flatIdx] = r;
    for (int y = reservoirMin.y; y < reservoirMax.y; ++y) {
        for (int x = reservoirMin.x; x < reservoirMax.x; ++x) {
            int tileFlatIdx = y * fullSize.x + x;
            if (tileFlatIdx != flatIdx)
                ReservoirWrite[tileFlatIdx] = r;
        }
    }
}

// Pass 11: ReSTIR temporal combine (spatial gather deliberately disabled).
// It reads the current pixel from ReservoirWrite, previous history from
// ReservoirRead, and writes its complete non-overlapping full-resolution tile.
// Tile ownership avoids write races. Neighbor reads from the same write SSBO
// would still race; spatial reuse needs a separate immutable source and output buffer.
// Then shade using the selected sample.
void RunRestirCombine() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Accumulation);
    ivec2 imgSize = wl_trace_extent(fullSize);
    if (pixelCoords.x >= imgSize.x || pixelCoords.y >= imgSize.y) return;

    // Match Generate's integer partition; the tile's top-left is its representative.
    ivec2 reservoirMin = pixelCoords * fullSize / imgSize;
    ivec2 reservoirMax = (pixelCoords + ivec2(1)) * fullSize / imgSize;
    int flatIdx = reservoirMin.y * fullSize.x + reservoirMin.x;
    // The CURRENT pixel comes from ReservoirWrite, not ReservoirRead: pass 3
    // wrote this frame's fresh candidates there. Reading ReservoirRead here
    // would start from the previous frame's result and never see pass 3 at all.
    LightReservoir r = ReservoirWrite[flatIdx];

    vec4 gb = imageLoad(img_TraceGBuffer, pixelCoords);
    float depth = gb.r;
    vec3 receiverN = normalize(gb.gba * 2.0f - 1.0f);
    vec3 receiverPos = vec3(0.0f);
    if (depth < 1.0f) {
        vec2 receiverUV = (vec2(pixelCoords) + 0.5f) / vec2(imgSize);
        vec2 receiverNdc = receiverUV * 2.0f - 1.0f;
        vec4 receiverWorld4 = u_InverseViewProjection * vec4(receiverNdc, depth * 2.0f - 1.0f, 1.0f);
        receiverPos = receiverWorld4.xyz / receiverWorld4.w;
    }

    // Temporal reuse: reproject to previous frame
    vec2 velocity = imageLoad(img_Velocity, pixelCoords).xy;
    vec2 uv = (vec2(pixelCoords) + 0.5f) / vec2(imgSize);
    vec2 prevUV = uv - velocity;

    bool validHistory = depth < 1.0f && u_FrameIndex > 0 && prevUV.x >= 0.0f && prevUV.x <= 1.0f && prevUV.y >= 0.0f && prevUV.y <= 1.0f;

    if (validHistory) {
        // Address the stable full-resolution reservoir plane directly. The
        // current trace extent can differ from the one that produced history.
        ivec2 prevReservoirPixel = clamp(ivec2(prevUV * vec2(fullSize)), ivec2(0), fullSize - ivec2(1));
        int prevFlatIdx = prevReservoirPixel.y * fullSize.x + prevReservoirPixel.x;
        LightReservoir prevR = ReservoirRead[prevFlatIdx];

        // Validate against surface metadata stored with the previous
        // reservoir, not two samples from the current frame's depth texture.
        vec3 prevNormal = wl_oct_decode(prevR.normalOct);
        vec3 receiverAlbedo = imageLoad(img_AlbedoHit, pixelCoords).rgb;
        float historyTarget = (wl_restir_is_previous_frame(prevR) && wl_restir_has_sample(prevR))
            ? RestirTarget(prevR.lightIndex, receiverPos, receiverN, receiverAlbedo) : 0.0f;
        float currentTarget = (wl_restir_has_sample(r))
            ? RestirTarget(r.lightIndex, receiverPos, receiverN, receiverAlbedo) : 0.0f;
        if (wl_restir_is_previous_frame(prevR) && abs(depth - prevR.bary.y) < 0.02f && dot(receiverN, prevNormal) > 0.9f) {
            seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x) + uint(u_FrameIndex) * 0x9e3779b9u;
            r = CombineReservoirs(r, prevR, currentTarget, historyTarget, hash());
        }
    }

    // Pass 14 performs spatial reuse and the single final shading write.

    // Keep the current receiver's surface metadata after temporal selection.
    r.bary.y = depth;
    r.normalOct = wl_oct_encode(normalize(gb.gba * 2.0f - 1.0f));
    if (r.M > 0)
        r.flagsValid = (wl_restir_has_sample(r) ? 1u : 0u) |
                       ((uint(u_FrameIndex) & 0x7fffffffu) << 1);
    else
        r.flagsValid = 0u;
    // Write the updated reservoir across this invocation's complete tile.
    ReservoirWrite[flatIdx] = r;
    for (int y = reservoirMin.y; y < reservoirMax.y; ++y) {
        for (int x = reservoirMin.x; x < reservoirMax.x; ++x) {
            int tileFlatIdx = y * fullSize.x + x;
            if (tileFlatIdx != flatIdx)
                ReservoirWrite[tileFlatIdx] = r;
        }
    }
}

// Experimental spatial reuse with the Algorithm 6 source-support correction:
// a neighbor's represented proposals count toward the denominator only when
// the neighbor's source PDF supports the center receiver's target at the
// neighbor's selected sample. Differing visibility/target support no longer
// darkens the estimate through unconditional total-M normalization.
void RunRestirSpatial() {
    ivec2 pixelCoords = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Accumulation);
    ivec2 imgSize = wl_trace_extent(fullSize);
    if (any(lessThan(pixelCoords, ivec2(0))) || any(greaterThanEqual(pixelCoords, imgSize))) return;

    // One invocation owns one trace pixel's complete full-resolution tile.
    ivec2 reservoirMin = pixelCoords * fullSize / imgSize;
    ivec2 reservoirMax = (pixelCoords + ivec2(1)) * fullSize / imgSize;
    int flatIdx = reservoirMin.y * fullSize.x + reservoirMin.x;
    LightReservoir r = ReservoirRead[flatIdx];

    vec4 gb = imageLoad(img_TraceGBuffer, pixelCoords);
    float depth = gb.r;
    vec3 receiverN = normalize(gb.gba * 2.0f - 1.0f);
    vec4 albedoHit = imageLoad(img_AlbedoHit, pixelCoords);
    vec3 receiverAlbedo = albedoHit.rgb;
    vec3 receiverPos = vec3(0.0f);
    if (depth < 1.0f) {
        vec2 receiverUV = (vec2(pixelCoords) + 0.5f) / vec2(imgSize);
        vec2 receiverNdc = receiverUV * 2.0f - 1.0f;
        vec4 receiverWorld4 = u_InverseViewProjection * vec4(receiverNdc, depth * 2.0f - 1.0f, 1.0f);
        receiverPos = receiverWorld4.xyz / receiverWorld4.w;
    }
    r.bary.y = depth;
    r.normalOct = wl_oct_encode(receiverN);

    uint totalM = r.M;
    float totalWeight = 0.0f;
    float selectedTarget = 0.0f;
    bool selectedValid = false;
    if (wl_restir_has_sample(r) && r.M > 0 && r.W > 0.0f && r.wSum > 0.0f && r.bary.x > 0.0f) {
        totalWeight = r.wSum;
        selectedTarget = r.bary.x;
        selectedValid = true;
    } else {
        r.W = 0.0f;
        r.wSum = 0.0f;
        r.bary.x = 0.0f;
        r.flagsValid = 0u;
    }

    // A distinct deterministic seed salt reduces direct RNG stream reuse; it
    // does not prove statistical independence of the input reservoir histories.
    seed = uint(pixelCoords.y) * uint(imgSize.x) + uint(pixelCoords.x)
         + uint(u_FrameIndex) * 0x9e3779b9u + 0x85ebca6bu;
    if (depth < 1.0f && albedoHit.a >= 0.5f) {
        for (int direction = 0; direction < 4; ++direction) {
            ivec2 neighborTrace = pixelCoords;
            if (direction == 0) neighborTrace.x -= 1;
            else if (direction == 1) neighborTrace.x += 1;
            else if (direction == 2) neighborTrace.y -= 1;
            else neighborTrace.y += 1;
            if (any(lessThan(neighborTrace, ivec2(0))) || any(greaterThanEqual(neighborTrace, imgSize))) continue;

            ivec2 neighborMin = neighborTrace * fullSize / imgSize;
            int neighborFlatIdx = neighborMin.y * fullSize.x + neighborMin.x;
            LightReservoir neighborR = ReservoirRead[neighborFlatIdx];
            if (!wl_restir_is_current_frame(neighborR)) continue;
            vec3 neighborNormal = wl_oct_decode(neighborR.normalOct);
            if (abs(depth - neighborR.bary.y) >= 0.02f || dot(receiverN, neighborNormal) <= 0.9f) continue;

            uint neighborCount = neighborR.M;
            float neighborTarget = 0.0f;
            if (wl_restir_has_sample(neighborR) && neighborR.W > 0.0f)
                neighborTarget = RestirTarget(neighborR.lightIndex, receiverPos, receiverN, receiverAlbedo);
            float neighborWeight = neighborTarget * max(neighborR.W, 0.0f) * float(neighborCount);
            float combinedWeight = totalWeight + neighborWeight;
            if (neighborWeight > 0.0f && hash() * combinedWeight < neighborWeight) {
                r.lightIndex = neighborR.lightIndex;
                selectedTarget = neighborTarget;
                selectedValid = true;
            }
            // Algorithm 6 source-support correction: count the neighbor's
            // represented proposals only when the neighbor's source PDF
            // supports the center receiver's target at the neighbor's
            // selected sample (neighborTarget > 0). Unconditional total-M
            // normalization is biased under source-support mismatch.
            totalWeight = combinedWeight;
            if (neighborTarget > 0.0f)
                totalM += neighborCount;
        }
    }

    r.M = totalM;
    r.wSum = totalWeight;
    r.bary.x = selectedValid ? selectedTarget : 0.0f;
    if (selectedValid && totalM > 0 && totalWeight > 1e-8f && selectedTarget > 1e-8f) {
        r.W = totalWeight / (selectedTarget * float(totalM));
        r.flagsValid = 1u;
    } else {
        r.W = 0.0f;
        r.flagsValid = 0u;
    }
    if (totalM > 0)
        r.flagsValid = (r.flagsValid & 1u) | ((uint(u_FrameIndex) & 0x7fffffffu) << 1);
    else
        r.flagsValid = 0u;

    vec3 directLight = vec3(0.0f);
    if (depth < 1.0f && albedoHit.a >= 0.5f && wl_restir_has_sample(r) && r.W > 0.0f) {
        int i = LightIndices[r.lightIndex];
        RayTracingInstance light = Instances[i];
        vec3 toLight = light.WorldTransform[3].xyz - receiverPos;
        float distToLight = length(toLight);
        if (distToLight > 0.001f) {
            vec3 dirToLight = toLight / distToLight;
            float NdotL = max(dot(receiverN, dirToLight), 0.0f);
            vec3 emission = light.Emission.xyz * light.Emission.w;
            directLight = receiverAlbedo * emission * (NdotL / PI) * r.W;
        }
        vec4 traceColor = imageLoad(img_Bloom, pixelCoords);
        imageStore(img_Bloom, pixelCoords, vec4(traceColor.rgb + directLight, traceColor.a));
    }

    // ReservoirRead is immutable during this dispatch; only the opposite
    // buffer is written, including every slot in this trace pixel's tile.
    ReservoirWrite[flatIdx] = r;
    for (int y = reservoirMin.y; y < reservoirMax.y; ++y) {
        for (int x = reservoirMin.x; x < reservoirMax.x; ++x) {
            int tileFlatIdx = y * fullSize.x + x;
            if (tileFlatIdx != flatIdx)
                ReservoirWrite[tileFlatIdx] = r;
        }
    }
}

// ============================================================================
// Temporal Super-Resolution (experimental, default-off)
// ============================================================================
// Pass 15: TSR resolve. Reads the current frame color (img_Output), reprojects
// using motion vectors, samples the history buffer, rejects outliers via 3x3
// YCoCg neighborhood clamping, blends, and sharpens. Writes the result to
// img_Accumulation and the history buffer.
// Pass 16: copies img_Accumulation to img_Output (avoids read-write race).
//
// The history buffer reuses img_Bloom_Temp (binding 7), which is free after
// the upscale pass. The C++ side must bind the history texture to binding 7
// for the TSR dispatch and clear it on camera cuts and frame 0.
// ============================================================================
void RunTSRResolve() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;

    vec3 currentColor = imageLoad(img_Output, pos).rgb;

    if (u_TSREnabled != 1) {
        imageStore(img_Accumulation, pos, vec4(currentColor, 1.0f));
        return;
    }

    vec2 velocity = imageLoad(img_Velocity, pos).xy;
    vec2 uv = (vec2(pos) + 0.5f) / vec2(fullSize);
    vec2 prevUV = uv - velocity;

    bool validHistory = u_FrameIndex > 0 &&
                        prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
                        prevUV.y >= 0.0f && prevUV.y <= 1.0f;

    if (validHistory) {
        float currentDepth = texture(s_DepthBuffer, uv).r;
        float prevDepth = texture(s_DepthBuffer, prevUV).r;
        if (abs(currentDepth - prevDepth) > 0.02f) validHistory = false;
    }

    vec3 historyColor = vec3(0.0f);

    if (validHistory) {
        // Bilinear sample from history buffer (reuses img_Bloom_Temp, binding 7)
        vec2 historyCoord = prevUV * vec2(fullSize) - vec2(0.5f);
        ivec2 h0 = clamp(ivec2(floor(historyCoord)), ivec2(0), fullSize - ivec2(1));
        ivec2 h1 = clamp(h0 + ivec2(1), ivec2(0), fullSize - ivec2(1));
        vec2 hf = clamp(historyCoord - vec2(h0), vec2(0.0f), vec2(1.0f));
        vec3 c00 = imageLoad(img_Bloom_Temp, h0).rgb;
        vec3 c10 = imageLoad(img_Bloom_Temp, ivec2(h1.x, h0.y)).rgb;
        vec3 c01 = imageLoad(img_Bloom_Temp, ivec2(h0.x, h1.y)).rgb;
        vec3 c11 = imageLoad(img_Bloom_Temp, h1).rgb;
        historyColor = mix(mix(c00, c10, hf.x), mix(c01, c11, hf.x), hf.y);

        // Neighborhood clamping (3x3 YCoCg variance clamp)
        vec3 m1 = vec3(0.0f);
        vec3 m2 = vec3(0.0f);
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                ivec2 sp = clamp(pos + ivec2(x, y), ivec2(0), fullSize - ivec2(1));
                vec3 s = RGB2YCoCg(imageLoad(img_Output, sp).rgb);
                m1 += s;
                m2 += s * s;
            }
        }
        vec3 mean = m1 / 9.0f;
        vec3 stddev = sqrt(max(vec3(0.0f), m2 / 9.0f - mean * mean));

        vec3 historyYCoCg = RGB2YCoCg(historyColor);
        vec3 minCol = mean - 1.25f * stddev;
        vec3 maxCol = mean + 1.25f * stddev;
        historyYCoCg = clamp(historyYCoCg, minCol, maxCol);
        historyColor = YCoCg2RGB(historyYCoCg);
    }

    // Blend history with current frame
    float motionMag = length(velocity * vec2(fullSize));
    float motionFactor = clamp(motionMag / 8.0f, 0.0f, 1.0f);
    float alpha = validHistory ? mix(u_AccumulationAlpha, 0.8f, motionFactor) : 1.0f;
    vec3 result = mix(historyColor, currentColor, alpha);

    // Sharpening (unsharp mask)
    if (u_TSRSharpenStrength > 0.001f) {
        vec3 sharp = vec3(0.0f);
        sharp += imageLoad(img_Output, pos).rgb * 4.0f;
        sharp -= imageLoad(img_Output, pos + ivec2(1, 0)).rgb;
        sharp -= imageLoad(img_Output, pos - ivec2(1, 0)).rgb;
        sharp -= imageLoad(img_Output, pos + ivec2(0, 1)).rgb;
        sharp -= imageLoad(img_Output, pos - ivec2(0, 1)).rgb;
        result = clamp(result + sharp * u_TSRSharpenStrength * 0.25f, 0.0f, 1.0f);
    }

    imageStore(img_Accumulation, pos, vec4(result, 1.0f));
    imageStore(img_Bloom_Temp, pos, vec4(result, 1.0f));
}

// Pass 16: copy TSR result to output.
void RunTSRCopy() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(img_Output);
    if (pos.x >= fullSize.x || pos.y >= fullSize.y) return;
    vec3 color = imageLoad(img_Accumulation, pos).rgb;
    imageStore(img_Output, pos, vec4(color, 1.0f));
}

void main()
{
    switch(u_PassID)
    {
        // Pass 0: the velocity buffer from rasterised depth.
        // Pass 1: the primary trace.
        // Pass 2: temporal accumulation.
        case 0: RunVisibilityAndVelocity(); break;
        case 1: RunTraceAndDenoise(); break;
        case 2: RunTemporalAccumulation(); break;
        // Passes 4 and 5: the separable bloom blur, horizontal then vertical.
        // The vertical pass is the one that adds the bloom back into the output.
        case 4: RunBloomBlurHorizontal(); break;
        case 5: RunBloomBlurVertical(); break;
        case 6: RunComposite(); break;
        // Pass 7: the edge-aware bilateral blur. The last pass to write
        // img_Bloom_Temp, which is why RunUpscale below reads that image rather
        // than the accumulation buffer.
        case 7: RunBilateralBlur(); break;
        case 8: RunUpscale(); break;
        case 9: GenerateMaterialMaps(); break;
        // Pass 10: the god rays, run after the composite so it can read the
        // finished colour buffer as its occlusion source.
        case 10: RunGodRays(); break;
        case 12: RunTemporalMoments(); break;
        // Pass 13: the A-Trous wavelet filter. Reads the temporal moments
        // from pass 12 and applies a variance-driven edge-preserving filter.
        case 13: RunATrousFilter(); break;
        // Pass 3: ReSTIR DI generate (reservoir update)
        case 3: RunRestirGenerate(); break;
        // Pass 11: temporal combine writes the per-pixel reservoir only.
        case 11: RunRestirCombine(); break;
        // Pass 14: immutable-input spatial reuse and the sole ReSTIR shading write.
        case 14: RunRestirSpatial(); break;
        // Pass 15: Temporal Super-Resolution resolve (experimental).
        case 15: RunTSRResolve(); break;
        // Pass 16: copy TSR result to output.
        case 16: RunTSRCopy(); break;
    }
}