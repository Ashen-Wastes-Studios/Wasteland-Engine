// The Renderer3D workhorse. One file, two programs: the `#type vertex` and
// `#type fragment` markers below are stripped by the engine's shader
// preprocessor, which splits this text into two GLSL sources before handing
// them to the compiler.

// Written against GLSL 450 so the same text serves OpenGL 4.5+ and Vulkan.
// Renderer3D Unified Basic Shader with Global Illumination (OpenGL & Vulkan Compatible)
// Handles textures, coloring, custom tiling, normals, ambient occlusion, and global illumination.

// Preprocessor marker, not GLSL. The engine's splitter looks for this exact
// token to find the boundary.
#type vertex
// GLSL 4.50 core profile. Matches glad 2 and the DSA calls (glCreateBuffers,
// glTextureStorage) the rest of the engine uses.
#version 450 core

// Vertex attributes, pinned to explicit locations.

// Explicit locations rather than a pre-bound vertex format: the engine
// uploads one interleaved buffer and relies on these numbers matching
// BufferLayout's computed offsets in Renderer/Buffer.h. Get the order wrong
// and every vertex reads the wrong field.

// The a_ prefix marks engine-supplied data, as against the Output./Input.
// interpolants below, which belong to the shader pipeline itself.
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec3 a_Normal;
layout(location = 3) in vec2 a_TexCoord;
// Which of the 32 bound textures this vertex samples.

// A FLOAT, not an int, and it stays a float all the way into the fragment
// stage because it crosses as an interpolated varying. Every use casts it
// back -- `int(Input.TexIndex)` at the fragment stage's slot switch and
// `int(a_TexIndex)` in the vertex stage's height evaluation. The cast is
// safe because all vertices of a triangle share the same value, so
// interpolation cannot produce a fractional result.
layout(location = 4) in float a_TexIndex;
layout(location = 5) in float a_TilingFactor;
layout(location = 6) in int a_EntityID;
layout(location = 7) in float a_Metallic;
layout(location = 8) in float a_Roughness;
layout(location = 9) in float a_Displacement;
// Relief strength for this material, opt-in per material.

// Overloaded as a FLAG as well as a magnitude: values at or below 0.001 mean
// "no relief", and a value below -0.5 marks the vertex as WATER (see the
// wlWater test in the fragment stage). One float doing three jobs, which is
// why the fragment stage tests it three different ways.
layout(location = 10) in float a_BumpStrength;

// Camera transform, applied once in the vertex stage.

// Declared in BOTH stages of this file. In the fragment stage it is not used
// to transform anything -- it is needed to reproject the displaced world
// position for the gl_FragDepth write, which the comment there explains.
// Uploaded once per frame, not per draw.
uniform mat4 u_ViewProjection;

// The varying block. Declared separately in each stage with identical
// members, which is what makes it a matched interface: the vertex stage
// writes location 0 and the fragment stage reads location 0.
struct VertexOutput
{
    vec4 Color;
    vec3 Normal;
    vec2 TexCoord;
    float TexIndex;
    float TilingFactor;
    vec3 WorldPos;
    float Metallic;
    float Roughness;
    float Displacement;
    float BumpStrength;
};

// Note the space before the paren -- cosmetic, and this file is not
// internally consistent about it (compare line 36 below).
layout (location = 0) out VertexOutput Output;
// The entity ID varying, at location 10 rather than 1.

// `flat` is mandatory, not an optimisation: without it the integer would be
// interpolated -- undefined for int, and averaged across the triangle, so
// every fragment would report a different entity. Flat takes the provoking
// vertex's value whole.

// Location 10 leaves 1-9 free, presumably for the integer attachment the
// fragment stage writes at location 1, though that is a separate mechanism.
layout (location = 10) out flat int v_EntityID;

// The vertex-stage half of the neural displacement system. Its job is to
// move real vertices so silhouettes and intersections are genuine geometry
// rather than a shading trick.
// ---- Neural geometric displacement (vertex stage) ----
// Same height field the fragment relief shades (luminance macro + craggy
// meso/strata + neural micro), evaluated per-vertex so silhouettes and
// intersections become real geometry. v-prefixed: the fragment stage defines
// same-named helpers and one program cannot hold duplicate symbols.
// 32 texture units bound to one array.

// An ARRAY rather than 32 separate samplers, so a material selects its map
// with a single varying (a_TexIndex) instead of 32 uniform slots. That is why
// the wl_vtex_slot / wl_tex_slot switch functions exist: GLSL cannot index a
// sampler array with a non-constant expression.
layout(binding = 0) uniform sampler2D u_Textures[32];
uniform int u_NeuralEnabled;
uniform float u_NeuralTexStrength;

// Vertex-stage helpers are all prefixed wl_v and their fragment-stage twins
// are prefixed wl_. The comment at line 41 explains why: both stages are
// preprocessed into ONE program, which cannot hold duplicate symbols. The
// v/wl split is a naming convention, not a language feature.

// A 2D-to-1D hash producing a value in [0,1) for arbitrary input.
float wl_vhash12(vec2 p)
{
    // p.xyx splats a vec2 into a vec3 by wrapping. The 0.1031 is the standard
    // scale that keeps the multiply in a well-conditioned range for float
    // precision.
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// "Softsign" activation: linear near zero, saturating towards +/-1.

// A cheap stand-in for a neural-network activation. It is smooth and
// differentiable everywhere, which matters here because these functions feed
// a displacement that gets written to gl_FragDepth -- a discontinuity would
// produce cracks in the relief.
float wl_vsoftsign(float x) { return x / (1.0 + abs(x)); }

// The detail function: a per-channel tint times a scalar multiplier, centred
// near 1.0 so that a strength of 0 leaves the material unchanged.

// Called "neural" but it is a fixed 4-feature encoding through four fixed
// softsign units -- hand-authored weights, not learned ones, despite the
// u_Neural* uniforms that suggest otherwise.
vec3 wl_vneural_tex_detail(vec2 uv, vec3 n)
{
    // Two rotated coordinate projections: s1 roughly along the UV diagonal, s2
    // across it. Two independent directions are what stop the sin() features
    // below forming a grid of separable ripples.
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    // The 4-feature encoding: each feature is a sinusoid whose phase is offset by
    // a different normal component.

    // Tying features to the surface normal is what makes the detail appear to
    // wrap the form rather than sit flat on it.
    vec4 feat = vec4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    // Per-cell hash grain, in [-0.5, 0.5).

    // floor() quantises UVs to a 64x64 grid so each cell gets ONE constant hash
    // value, giving blocky per-cell grain rather than smooth noise.

    // DEFECT: floor(uv * 64.0) is discontinuous across cell boundaries, and this
    // value is added to the DISPLACEMENT height field through wl_vheight. So the
    // displacement has a step discontinuity at every cell edge, and that step is
    // written into gl_FragDepth. The visible result is a grid of tiny ledges in
    // the depth buffer on any material with displacement above 0. The
    // fragment-stage twin has the same line but feeds only albedo, which is
    // harmless because albedo never reaches the depth buffer.
    float grain = wl_vhash12(floor(uv * 64.0)) - 0.5;
    // The scalar detail multiplier: 1.0 plus a small weighted sum of the four
    // activations and the grain.

    // Bounded well away from zero -- the weights sum to 0.16 in magnitude -- so
    // d stays near 1.0, which is what lets the function be mixed in by a strength
    // uniform without the surface darkening when that strength is 0.
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    vec3 tint = vec3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

// The smooth-only micro function.

// Deliberately the same four features and weights as wl_vneural_tex_detail
// MINUS the hash grain. That omission is the entire difference: this one feeds
// the geometric displacement, where a discontinuous grain would tear the
// silhouette and write broken depth.
float wl_vneural_micro(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    vec4 feat = vec4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    // Just the weighted activation sum -- no `1.0 +`, no grain, no tint.

    // Signed and zero-mean, unlike its sibling's near-1.0 return. Correct here:
    // the caller ADDS this to an existing height, so a signed delta is what is
    // wanted.
    return 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3;
}

// 3D value-noise hash, using different constants from the 2D hash above
// (31.32 vs 33.33, .zyx vs .yzx) to decorrelate the two.
float wl_vvol_hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// Trilinear value noise: hash the eight corners of the containing cell and
// blend them.

// Value noise rather than gradient/Perlin noise -- cheaper, and its C0
// continuity suffices because the result feeds a displacement that is itself
// bilinearly interpolated across the triangle.
float wl_vvol_noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    // The smoothstep curve applied to the fractional cell position: 3f^2 - 2f^3.

    // This is what makes the noise continuous at cell boundaries -- the derivative
    // vanishes at f=0 and f=1, so neighbouring cells meet smoothly.
    vec3 u = f * f * (3.0 - 2.0 * f);
    // Eight nested mixes: two per axis, blending corner hashes by u.x, then u.y,
    // then u.z.

    // Written as one deeply nested expression rather than eight named locals. That
    // is a legibility cost, not a correctness one -- but it does mean which corner
    // pairs with which axis has to be read off the vector literals and cannot be
    // checked against a variable.
    return mix(mix(mix(wl_vvol_hash(i), wl_vvol_hash(i + vec3(1.0, 0.0, 0.0)), u.x),
                   mix(wl_vvol_hash(i + vec3(0.0, 1.0, 0.0)), wl_vvol_hash(i + vec3(1.0, 1.0, 1.0)), u.x), u.y),
               mix(mix(wl_vvol_hash(i + vec3(0.0, 0.0, 1.0)), wl_vvol_hash(i + vec3(1.0, 0.0, 1.0)), u.x),
                   mix(wl_vvol_hash(i + vec3(0.0, 1.0, 1.0)), wl_vvol_hash(i + vec3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

// Sampler-array indexing by switch.

// GLSL requires sampler array indices to be constant expressions, so a
// runtime `slot` cannot index u_Textures directly. A switch with one case per
// element is the standard workaround, and the reason this function is 39
// lines long.

// Each case is one texture() call, so this compiles to 32 constant-indexed
// fetches with only one taken. Most hardware does not cost the untaken
// branches a fetch, but the code size is real.
vec4 wl_vtex_slot(int slot, vec2 uv)
{
    switch (slot)
    {
        case 0:  return texture(u_Textures[0], uv);
        case 1:  return texture(u_Textures[1], uv);
        case 2:  return texture(u_Textures[2], uv);
        case 3:  return texture(u_Textures[3], uv);
        case 4:  return texture(u_Textures[4], uv);
        case 5:  return texture(u_Textures[5], uv);
        case 6:  return texture(u_Textures[6], uv);
        case 7:  return texture(u_Textures[7], uv);
        case 8:  return texture(u_Textures[8], uv);
        case 9:  return texture(u_Textures[9], uv);
        case 10: return texture(u_Textures[10], uv);
        case 11: return texture(u_Textures[11], uv);
        case 12: return texture(u_Textures[12], uv);
        case 13: return texture(u_Textures[13], uv);
        case 14: return texture(u_Textures[14], uv);
        case 15: return texture(u_Textures[15], uv);
        case 16: return texture(u_Textures[16], uv);
        case 17: return texture(u_Textures[17], uv);
        case 18: return texture(u_Textures[18], uv);
        case 19: return texture(u_Textures[19], uv);
        case 20: return texture(u_Textures[20], uv);
        case 21: return texture(u_Textures[21], uv);
        case 22: return texture(u_Textures[22], uv);
        case 23: return texture(u_Textures[23], uv);
        case 24: return texture(u_Textures[24], uv);
        case 25: return texture(u_Textures[25], uv);
        case 26: return texture(u_Textures[26], uv);
        case 27: return texture(u_Textures[27], uv);
        case 28: return texture(u_Textures[28], uv);
        case 29: return texture(u_Textures[29], uv);
        case 30: return texture(u_Textures[30], uv);
        case 31: return texture(u_Textures[31], uv);
        // An out-of-range slot returns opaque white.

        // DEFECT: white is a real colour, so an out-of-range TexIndex is
        // indistinguishable from a material that legitimately sampled a white
        // texture -- there is no error signal and nothing to debug against. Combined
        // with a_TexIndex arriving as an interpolated float, an off-by-one in the
        // engine's slot assignment renders as a blank white surface rather than a
        // visible failure. Magenta or a debug colour would at least be conspicuous.
        default: return vec4(1.0);
    }
}

// The vertex stage's height field: what gets displaced along the normal.

// Mirrors the fragment stage's relief height so the two agree on the surface
// shape. If they disagreed, the geometry would not line up with the shading
// and the parallax would visibly drift from the silhouette.
float wl_vheight(int slot, vec2 uv, vec3 wp, vec3 n, float disp)
{
    // Texture LUMINANCE as the base height, using the Rec. 709 luma weights.

    // The premise of relief mapping: a texture's luminance reads as a height
    // field. This works for surfaces where light and dark correlate with high and
    // low -- brick, foliage, rock -- and produces nonsense for others, which is why
    // the effect is opt-in per material.
    float h = dot(wl_vtex_slot(slot, uv).rgb, vec3(0.299, 0.587, 0.114));
    // Neural micro-relief, gated on the uniform AND a non-negligible strength.

    // Both conditions matter: the second avoids evaluating the whole MLP for a
    // strength that would contribute nothing, and it is what lets a studio upload
    // the uniforms unconditionally and control the effect from content.
    if (u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001)
        h += wl_vneural_micro(uv, n) * 2.0 * clamp(u_NeuralTexStrength, 0.0, 1.0);
    // Meso-structure amplitude: 25% of the displacement, clamped.

    // The 1.5 ceiling caps how far one material can exaggerate. A content typo
    // above it produces a ceiling rather than an error, so the mistake is silent.
    float ma = 0.25 * clamp(disp, 0.0, 1.5);
    // Gate for the craggy/strata block -- the same epsilon-gating idea as above,
    // consistently applied throughout this file.
    if (ma > 0.001)
    {
        // Six-fold UV scale for the meso noise. A fixed constant, so meso detail is
        // locked to UV space and therefore scales with the texture rather than with
        // world size: a model scaled 10x gets 10x coarser meso detail.
        vec2 muv = uv * 6.0;
        float n1 = wl_vvol_noise(vec3(muv, 3.7));
        float n2 = wl_vvol_noise(vec3(muv * 2.3 + 5.0, 9.1));
        // Strata: horizontal banding driven by WORLD height, noise-warped by n1.

        // wp.y rather than UV keeps the bands level in world space across
        // differently-scaled objects -- the reason for using world position here while
        // the noise above uses UV.
        float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
        // Strata weight by surface orientation: full on vertical faces, a floor of
        // 0.15 on near-horizontal ones.

        // Physically sensible -- strata show on cut faces, not on top surfaces. The
        // 0.15 floor leaves a trace on flat ground rather than none at all.
        float sw = clamp(1.0 - abs(n.y), 0.15, 1.0);
        h += ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * ma
           + (st - 0.5) * ma * 0.8 * sw;
    }
    // Clamped to 1.5, not 1.0 -- the displacement path deliberately allows
    // overshoot above white so features can rise without the sum saturating.

    // The caller then offsets by (h - 0.5), so 1.5 means half a unit up and 0
    // means half a unit down.
    return clamp(h, 0.0, 1.5);
}

// Vertex entry point.
void main()
{
    Output.Color = a_Color;
    Output.Normal = a_Normal;
    Output.TexCoord = a_TexCoord;
    Output.TexIndex = a_TexIndex;
    Output.TilingFactor = a_TilingFactor;
    // Neural geometric displacement: real vertices move along the (world)
    // normal by the neural height field. Unfaded by design — geometry must
    // not swim while fragment shading detail fades with distance.
    // The possibly-displaced position. Starts as the raw attribute and is
    // overwritten below if displacement is active.

    // A COPY rather than writing a_Position directly: vertex attributes are
    // read-only in GLSL, so this is required rather than stylistic.
    vec3 vPos = a_Position;
    // Displacement gate. Note it tests a_Displacement but NOT a_BumpStrength,
    // unlike the fragment stage's combined test below -- so the two stages can
    // disagree about whether relief is active for a given material.

    // For water (BumpStrength < -0.5) with Displacement 0 this stage does
    // nothing, which is what the fragment stage's water comment relies on when it
    // says the relief path "already skipped them".
    if (a_Displacement > 0.001)
    {
        vec3 vN = normalize(a_Normal);
        // The height lookup. Two details:

        // 1. a_TexCoord is multiplied by a_TilingFactor, and the fragment stage does
        // the same to build reliefUV, so both stages agree on where in the texture
        // they are sampling.
        // 2. a_Position is passed as the world position, not vPos -- correct only
        // because this is the first displacement pass and no vPos exists yet.
        float vh = wl_vheight(int(a_TexIndex), a_TexCoord * a_TilingFactor, a_Position, vN, a_Displacement);
        // Displace ALONG THE NORMAL by (height - 0.5) scaled by the displacement
        // amount.

        // The -0.5 recentres so a flat texture (height 0.5) does not move. Using the
        // normalised normal makes the displacement uniform regardless of the
        // attribute's original length.

        // Displacing along the normal rather than world-up means a sphere bulges
        // outward instead of skewing -- which is what makes the silhouette read as
        // rounded rather than as a lump.
        vPos = a_Position + vN * (vh - 0.5) * a_Displacement;
    }
    // The DISPLACED position, not a_Position.

    // Everything downstream sees the displaced position: the fragment stage's
    // own height taps, its lighting, and the depth reprojection all use it.
    Output.WorldPos = vPos;
    Output.Metallic = a_Metallic;
    Output.Roughness = a_Roughness;
    Output.Displacement = a_Displacement;
    Output.BumpStrength = a_BumpStrength;
    v_EntityID = a_EntityID;

    // Clip-space position from the displaced vertex, so the silhouette itself
    // moves. This is what makes the displacement real geometry rather than a
    // shading effect.
    gl_Position = u_ViewProjection * vec4(vPos, 1.0);
}

// The second program in this file. Everything below here is fragment-stage
// code and shares the vertex stage's program object.
#type fragment
#version 450 core

layout(location = 0) out vec4 color;
// An INTEGER output carrying the entity ID to a second colour attachment.

// A framebuffer with two attachments: the main colour target and an ID target
// a picking or outline pass reads. Written on every exit path (see both the
// neural and classic endings below); nothing in this file reads it.
layout(location = 1) out int color2; // Entity ID attachment texture slot

// Relief writes gl_FragDepth only for displaced fragments; this keeps the
// early-z cull for everything else instead of forcing late-z program-wide.
// Declares that this shader does not need to run before the depth test.

// Correct here because the relief block WRITES gl_FragDepth, and the comment
// above explains the tradeoff: only displaced fragments override depth, so
// everything else keeps early-z. Without this declaration any gl_FragDepth
// write would force late-z program-wide, costing early-z on every fragment to
// speed up the displaced minority.
layout(early_fragment_tests) in;

struct VertexOutput
{
    vec4 Color;
    vec3 Normal;
    vec2 TexCoord;
    float TexIndex;
    float TilingFactor;
    vec3 WorldPos;
    float Metallic;
    float Roughness;
    float Displacement;
    float BumpStrength;
};

layout (location = 0) in VertexOutput Input;
layout (location = 10) in flat int v_EntityID;

// The same 32-unit array as the vertex stage, with the same binding so one
// set of texture units serves both.
layout (binding = 0) uniform sampler2D u_Textures[32];

// Shared with the vertex stage (uploaded once per frame by Renderer3D):
// reprojects the relief-pushed position for the gl_FragDepth write.
// Shared with the vertex stage; uploaded once per frame.

// Used here NOT to transform anything -- a fragment stage has no vertices --
// but to reproject the relief-displaced world position into clip space so
// gl_FragDepth can be written.
uniform mat4 u_ViewProjection;

// Camera + sky (uploaded by Renderer3D): view-dependent metal reflection.
uniform vec3 u_CameraPosition;
// Sky gradient endpoints. Two uniforms rather than a sampled cubemap because
// the environment lighting model is an analytic gradient: cheaper, and free of
// seams.
uniform vec3 u_SkyBottomColor;
uniform vec3 u_SkyTopColor;

// Neural Rendering (OpenGL): tiny in-shader MLP for texture detail + learned ambient.
// Driven by Renderer3D neural uniforms; defaults off (0) until Flush() uploads them.
// The master switch for every neural path in this file.

// Compared with `== 1` rather than used as a bool, at every use site. So an
// engine uploading any value other than exactly 1 disables all neural shading
// -- 0 and 2 behave identically, and a truthy-but-not-1 upload would
// unexpectedly turn everything OFF.
uniform int u_NeuralEnabled;
uniform float u_NeuralTexStrength;
uniform float u_NeuralLightStrength;
// Neural material blend. Defaults to 0 (classic response) until Renderer3D uploads it.
uniform float u_NeuralMatStrength;

// Volumetric atmosphere (Renderer3D uploads; locations are skipped when absent).
// Fog d1 = (density, anisotropy, noiseStrength, noiseScale)
// Fog d2 = (windSpeed, heightFalloff, steps, enabled)
// Cloud d1 = (coverage, density, noiseScale, detail)
// Cloud d2 = (windSpeed, windDirX, windDirY, steps)
// Component-by-component packing documentation for the cloud uniform arrays.

// The only place this layout is written down, which is why it is worth four
// separate comment lines: a change to the packing that does not update these
// comments becomes silent corruption rather than a compile error.
// Cloud d3 = (silverLining, shadowStrength, enabled, unused)
uniform vec3 u_SunDirection;
uniform float u_Time;
uniform int u_FogEnabled;
// God-ray sun color (directional light color x intensity, or warm default).
uniform vec3 u_SunLightColor;
uniform int u_FogCount;
// Four fog volume slots, each an axis-aligned box: min corner, max corner,
// colour, plus two vec4 parameter sets.

// Fixed-size arrays of 4 (fog) and 2 (clouds) rather than a texture or SSBO,
// so the volume count is a shader constant and changing it means editing this
// file. The loops below are correspondingly written with literal bounds.
uniform vec3 u_FogMin[4];
uniform vec3 u_FogMax[4];
uniform vec3 u_FogColor[4];
uniform vec4 u_FogData[4];
uniform vec4 u_FogData2[4];
uniform int u_CloudEnabled;
uniform int u_CloudCount;
uniform vec3 u_CloudMin[2];
uniform vec3 u_CloudMax[2];
uniform vec3 u_CloudColor[2];
uniform vec3 u_CloudAmbient[2];
uniform vec4 u_CloudData[2];
uniform vec4 u_CloudData2[2];
uniform vec4 u_CloudData3[2];
// 1 = reduced noise octaves (Low/Medium quality presets).
// Quality-preset flag: 1 selects the 2-octave noise variant everywhere it is
// tested, trading detail for speed on Low/Medium.
uniform int u_VolFast;

// Analytic component lights (Renderer3D uploads; -1 locations skipped).
// d1 = (type 0=dir 1=point 2=spot 3=area, intensity, range 0=inf, falloff)
// d2 = (spot innerCos, spot outerCos, areaSize, areaDoubleSided)
// Up to 8 analytic lights. The loop below is bounded by the literal 8 to
// match the array sizes.
uniform int u_ALightCount;
uniform vec3 u_ALightPos[8];
uniform vec3 u_ALightDir[8];
uniform vec3 u_ALightColor[8];
uniform vec4 u_ALightData[8];
uniform vec4 u_ALightData2[8];

// The activation function, byte-identical to the vertex stage's wl_vsoftsign
// above and duplicated for the merged-program reason given there.
float wl_softsign(float x) { return x / (1.0 + abs(x)); }

float wl_hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// 4-feature sinusoidal encoding -> 4-unit hidden layer -> detail multiplier + tint.
// Output ~= 1.0 (neutral); keeps energy stable when strength is 0.
// The fragment-stage twin of wl_vneural_tex_detail. Same features, same
// weights, same near-1.0 output.

// A separate copy rather than a shared helper precisely because duplicate
// symbols are illegal in the merged program. That is why every helper in
// this file exists in two copies.
vec3 wl_neural_tex_detail(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    vec4 feat = vec4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_softsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_softsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_softsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_softsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    // The same per-cell hash grain as the vertex-stage twin.

    // Safe HERE in a way it is not there: this copy modulates only ALBEDO, which
    // is interpolated and never written to depth. The vertex-stage copy feeds
    // displacement, where the cell-boundary discontinuity becomes a step in
    // gl_FragDepth. See the note at line 68.
    float grain = wl_hash12(floor(uv * 64.0)) - 0.5;
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    vec3 tint = vec3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

// Smooth-only neural micro for GEOMETRY (no per-cell hash grain).
float wl_neural_micro(vec2 uv, vec3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    vec4 feat = vec4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_softsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_softsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_softsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_softsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    return 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3;
}

// Learned hemisphere ambient: sky gradient + albedo-tinted bounce lobe.
// Learned ambient: a sky gradient plus an albedo-tinted bounce lobe.

// Stands in for an irradiance probe. Cheap and always available, but it
// cannot capture occlusion -- a corner receives the same ambient as an open
// field.
vec3 wl_neural_ambient(vec3 n, vec3 albedo)
{
    float up = n.y * 0.5 + 0.5;
    vec3 skyRef = mix(vec3(0.32, 0.30, 0.34), vec3(0.55, 0.62, 0.78), up);
    float h = wl_softsign(dot(n.xz, vec2(0.8, -0.6)) * 1.5 + n.y * 1.2);
    vec3 bounce = albedo * (0.35 + 0.25 * h) * skyRef;
    // Ambient sky term plus the bounce lobe.

    // The bounce is multiplied by albedo, so a dark surface also receives less
    // bounce. That single property is what makes ambient light feel attached to
    // the surface rather than floating over it.
    return skyRef * 0.35 + bounce;
}

// Neural material (raster): same learned PBR idea as the ray-trace path, minus
// world position — porosity/patina modulate albedo, the MLP gates sparse
// metallic-flake glints, and wrap width softens the diffuse response.
// The neural material model for the raster path.

// Note the parameter directions: `inout vec3 albedo` is MODIFIED in place,
// while glint and wrapW are pure outputs. So calling this mutates the caller's
// albedo -- which is why the call site below passes a separate matAlbedo copy
// and only blends the result in afterwards.
void wl_neural_mat_raster(vec2 uv, vec3 n, inout vec3 albedo, out float glint, out float wrapW)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    vec4 feat = vec4(sin(s1 + n.x * 2.0),
                     sin(s1 * 1.6 + n.y * 1.7),
                     sin(s1 * 0.7 + n.z * 2.3),
                     sin((s1 * 0.6 + n.x - n.y) * 1.3));
    float h0 = wl_softsign( 0.8 * feat.x - 0.5 * feat.y + 0.6 * feat.z + 0.2 * feat.w + 0.05);
    float h1 = wl_softsign(-0.4 * feat.x + 0.9 * feat.y - 0.2 * feat.z + 0.5 * feat.w - 0.10);
    float h2 = wl_softsign( 0.3 * feat.x + 0.2 * feat.y + 0.7 * feat.z - 0.6 * feat.w + 0.35);
    float h3 = wl_softsign(-0.5 * feat.x - 0.4 * feat.y + 0.4 * feat.z + 0.8 * feat.w + 0.00);
    float por = clamp(-h0, 0.0, 1.0);
    // Porosity darkening: up to 25% darker in the pores.

    // por is the clamped negative of h0, so a signed activation is reused as a
    // mask. Multiplying rather than mixing toward black preserves the hue.
    albedo *= (1.0 - 0.25 * por);
    albedo *= (vec3(1.0) + vec3(-0.06, 0.02, 0.05) * h1);
    // Sparse metallic flakes: a per-cell hash thresholded at 0.75, so roughly a
    // quarter of cells qualify.

    // DEFECT (aliasing): step() on a per-cell hash produces hard cell-sized
    // squares with no filtering. At 48 cells across a UV unit these are large
    // blocks, and as the camera moves they pop in and out cell by cell. The same
    // problem as line 68, but worse here because the threshold AMPLIFIES the cell
    // boundary -- the value flips between 0 and 1 across a single pixel.
    float flakeCell = step(0.75, wl_hash12(floor(uv * 48.0)));
    glint = flakeCell * clamp(h2 * 1.5, 0.0, 1.0);
    // Diffuse wrap width in [0.35, 1.0].

    // Wrapped diffuse lights a surface slightly past its terminator, standing in
    // for subsurface scattering. The narrow range keeps the effect subtle by
    // design.
    wrapW = 0.35 + 0.65 * clamp(h3 * 0.5 + 0.5, 0.0, 1.0);
}

// ---- Volumetric fog & clouds (box volumes submitted by Renderer3D) ----
// 3D noise helpers for fog and clouds -- duplicates of the wl_v* pair above,
// for the merged-program reason.
float wl_vol_hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float wl_vol_noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(wl_vol_hash(i), wl_vol_hash(i + vec3(1.0, 0.0, 0.0)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0, 1.0, 0.0)), wl_vol_hash(i + vec3(1.0, 1.0, 0.0)), u.x), u.y),
               mix(mix(wl_vol_hash(i + vec3(0.0, 0.0, 1.0)), wl_vol_hash(i + vec3(1.0, 0.0, 1.0)), u.x),
                   mix(wl_vol_hash(i + vec3(0.0, 1.0, 1.0)), wl_vol_hash(i + vec3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

// Fractal Brownian motion: 4 octaves at halving amplitude.
float wl_vol_fbm(vec3 p)
{
    float v = 0.0;
    float a = 0.5;
    // A fixed 4 iterations with a CONSTANT bound, which lets the driver unroll
    // the loop.

    // Because the bound is constant, the runtime step count used elsewhere
    // cannot apply here -- that is handled by the separate loops below, which
    // break early against a uniform value.
    for (int o = 0; o < 4; o++)
    {
        v += a * wl_vol_noise(p);
        // Each octave doubles frequency AND is offset, so the octaves do not align
        // on a lattice and produce visible grid artefacts.

        // 2.03 rather than exactly 2.0 for the same reason -- an exact doubling makes
        // the noise self-similar in a way that reads as repetition.
        p = p * 2.03 + vec3(17.3);
        a *= 0.5;
    }
    return v;
}

// 2-octave variant for low quality presets / cheap taps. Rescaled to match
// wl_vol_fbm's mean so coverage thresholds behave the same.
// The 2-octave variant for low quality presets.

// The rescale by 1.25 at the end is the important part: without it the
// reduced octave count would change the MEAN, so coverage thresholds tuned
// against the 4-octave version would behave differently at low quality and the
// same scene would look different per preset.
float wl_vol_fbm2(vec3 p)
{
    return (0.5 * wl_vol_noise(p) + 0.25 * wl_vol_noise(p * 2.03 + vec3(17.3))) * 1.25;
}

// Slab-method ray/AABB intersection, returning (tEnter, tExit).

// Standard: invert the ray per axis, then intersect the resulting intervals.
vec2 wl_box_range(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax)
{
    // Guards the division below against a zero direction component.

    // step(edge, x) is 0 when x < edge, so this nudges any near-zero component
    // away from exactly 0 -- because 1.0/0.0 is an infinity that would poison
    // the min/max for that axis. The classic ray-box robustness fix.
    vec3 safe = rd + (vec3(1.0) - step(vec3(0.00001), abs(rd))) * 0.00001;
    vec3 inv = 1.0 / safe;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    // Entry is the largest near-plane hit; exit is the smallest far-plane hit.

    // When entry > exit the ray misses the box, which the callers detect and turn
    // into an early return. tEnter can also be negative for a camera inside the
    // box, which is why the callers clamp it with max(range.x, 0.0).
    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

// Henyey-Greenstein phase function: how strongly scattering is
// forward-biased.
float wl_hg(float cosT, float g)
{
    float g2 = g * g;
    // The 1/4pi normalisation (0.07957747) times the HG term raised to 3/2.

    // The max() on the denominator clamps it away from zero. cosT near 1 with a
    // positive g drives the true denominator towards (1-g)^2, which is small but
    // finite, so the clamp is there so a g of exactly 1 -- a legal parameter value
    // -- cannot divide by zero. g is clamped to 0.9 at the call site as well, since
    // at g=1 the phase function is a true forward delta that cannot be represented
    // this way.
    return 0.07957747 * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosT, 0.001), 1.5);
}

// Ray-marches one fog volume front to back.

// Returns (accumulated colour, remaining transmittance) -- the standard
// front-to-back compositing pair, which is why the caller composites as
// `col * r.a + r.rgb`.
vec4 wl_march_fog_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                      vec3 tint, vec4 d1, vec4 d2, float dither, float sunVis)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    // Depth-adaptive steps: thin intersections cost proportionally less while
    // keeping an approximately constant world-space step size.
    float fogLen = max(t1 - t0, 0.0);
    // Requested step count, scaled by thickness and clamped to [2, 16].

    // The 16 is a HARD ceiling and it is also the loop bound below, so this clamp
    // is load-bearing rather than merely a quality knob: the loop cannot iterate
    // more than 16 times whatever d2.z asks for. The loop still needs its own
    // break, because a smaller runtime value is legal.
    int steps = int(clamp(d2.z * clamp(fogLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / float(steps);
    // Forward-scattering phase, sampled against the sun direction.

    // The clamp to +/-0.9 keeps the phase function finite -- see wl_hg.
    float phase = wl_hg(dot(rd, u_SunDirection), clamp(d1.y, -0.9, 0.9));
    // A single flat ambient colour: the sky gradient sampled at its midpoint.

    // Averaged rather than height-varying, so fog is lit uniformly regardless of
    // where it sits. Cheap, and the reason fog far from the sky still reads as
    // sky-lit.
    vec3 ambCol = mix(u_SkyBottomColor, u_SkyTopColor, 0.5);
    vec3 light = ambCol * 0.7 + u_SunLightColor * sunVis * (0.25 + phase * 2.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    vec3 acc = vec3(0.0);
    float trans = 1.0;
    // The first sample is offset WITHIN the first step by a per-pixel dither.

    // The standard fix for banding in a low-step ray march: without it every pixel
    // in a 2x2 quad starts at the same depth and the march bands. The caller passes
    // a per-pixel hash (wl_hash12(gl_FragCoord.xy)), so the offset decorrelates
    // between pixels and the banding becomes noise that filters can absorb.
    float t = t0 + dt * dither;
    // Constant loop bound matching the clamp above, plus an early break on the
    // runtime count below.
    for (int s = 0; s < 16; s++)
    {
        // The dynamic exit. Both bounds are needed: the constant one lets the driver
        // unroll, this one stops early.
        if (s >= steps)
            break;
        vec3 p = ro + rd * t;
        float d = max(d1.x, 0.0);
        // Height fraction within the volume box: 0 at its floor, 1 at its ceiling.

        // Normalised, so the falloff below is relative to the box rather than to
        // absolute world height -- fog behaviour therefore depends on how the volume
        // box was authored.
        float h = clamp((p.y - bmin.y) / hgt, 0.0, 1.0);
        // Height falloff: density decreases exponentially with height, blended in by
        // d2.y.

        // The mix rather than a multiply means the effect can be faded in
        // continuously: d2.y = 0 means no falloff at all, 1 means full.
        d *= mix(1.0, exp(-h * 4.0), clamp(d2.y, 0.0, 1.0));
        if (d1.z > 0.001)
        {
            // Noise coordinates, scrolling with time.

            // The Z component of the scroll is 0.6x the X and Y is 0, so the wind is
            // horizontal along a fixed diagonal. Simple and effective, though it means fog
            // noise never drifts vertically.

            // max(d1.w, 0.001) prevents a zero noise scale from collapsing every sample
            // onto one point.
            vec3 np = p * max(d1.w, 0.001) + vec3(u_Time * d2.x, 0.0, u_Time * d2.x * 0.6);
            float n = (u_VolFast == 1) ? wl_vol_fbm2(np) : wl_vol_fbm(np);
            // Modulate density by the noise. d1.z acts as both the strength and the
            // "amount of thinning", with the 0.5 factor keeping density non-negative.
            d *= (1.0 - d1.z * 0.5) + d1.z * n;
        }
        // Alpha for this step: Beer-Lambert over the step length.

        // The exponential form is what makes the result independent of step count --
        // halving dt and doubling the step count converges to the same accumulated
        // transmittance. That is the property that makes the adaptive step count above
        // safe to vary per pixel.
        float a = 1.0 - exp(-max(d, 0.0) * dt);
        // Front-to-back accumulation: this step's contribution weighted by the
        // transmittance still arriving.
        acc += trans * a * tint * light;
        // Transmittance update -- multiplicative, which is what makes front-to-back
        // compositing converge regardless of step order.
        trans *= (1.0 - a);
        // Early-out once the volume is essentially opaque, saving the remaining steps
        // of a dense march.

        // Snaps transmittance to exactly 0 on the next line rather than leaving the
        // 0.019 residual, which avoids accumulating a faint tail across many steps.
        if (trans < 0.02)
        {
            // Snapped to fully opaque, discarding the small residual.
            trans = 0.0;
            break;
        }
        t += dt;
    }
    // Premultiplied colour plus transmittance -- NOT straight alpha. The caller
    // composites as col * r.a + r.rgb, which only works with premultiplied
    // output.
    return vec4(acc, trans);
}

// Ray-marches one cloud volume, same front-to-back structure as the fog march
// above but with a self-shadowing term and a silver-lining forward-scatter
// term.

// Takes three vec4 parameter sets (d1, d2, d3) against fog's two, because
// clouds need coverage, density, noise, detail, wind, steps AND two shading
// parameters.
vec4 wl_march_cloud_vol(vec3 ro, vec3 rd, float tMax, vec3 bmin, vec3 bmax,
                        vec3 tint, vec3 amb, vec4 d1, vec4 d2, vec4 d3, float dither)
{
    vec2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    float clLen = max(t1 - t0, 0.0);
    // The same depth-adaptive clamp as the fog march, reading steps from d2.w
    // here because the cloud packing differs.

    // Same hard 16 ceiling matching the loop bound below.
    int steps = int(clamp(d2.w * clamp(clLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / float(steps);
    // Wind vector assembled from the packed data.
    vec2 wnd = vec2(d2.y, d2.z);
    // Guards a zero-length wind vector, which would make normalize() below
    // produce NaN and poison every sample downstream.
    if (dot(wnd, wnd) < 0.00000001)
        wnd = vec2(1.0, 0.0);
    // Normalised, then scaled by wind speed and time so the cloud field advects.

    // Multiplying by u_Time means the offset grows without bound over a long
    // session. Since the noise is not periodic, that is harmless -- but it does
    // mean the noise coordinates eventually leave any range where float precision
    // is comfortable, after which the cloud detail degrades.
    wnd = normalize(wnd) * d2.x * u_Time;
    // Forward-scatter "silver lining": bright rims where the view direction is
    // close to the sun.

    // The 6th power is a sharp lobe, so the effect is confined to a narrow cone
    // around the sun rather than washing the whole cloud.
    float silverPow = pow(clamp(dot(rd, u_SunDirection), 0.0, 1.0), 6.0);
    // A hard-coded warm sun colour for cloud lighting, NOT u_SunLightColor.

    // DEFECT (inconsistency): the fog march above uses the u_SunLightColor
    // uniform (documented as "directional light colour x intensity"), while the
    // cloud march hard-codes its own. So fog and clouds in the same scene are lit
    // by different suns, and a studio that changes u_SunLightColor to re-time the
    // day will see clouds not respond.
    vec3 sunCol = vec3(1.2, 1.1, 1.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    float nScale = max(d1.z, 0.001);
    vec3 acc = vec3(0.0);
    float trans = 1.0;
    float t = t0 + dt * dither;
    for (int s = 0; s < 16; s++)
    {
        if (s >= steps)
            break;
        vec3 p = ro + rd * t;
        vec3 q = p * nScale + vec3(wnd.x, 0.0, wnd.y);
        float base = (u_VolFast == 1) ? wl_vol_fbm2(q) : wl_vol_fbm(q);
        float cov = clamp(d1.x, 0.0, 1.0);
        // Coverage threshold: the 0.35-wide smoothstep band turns the raw fbm into
        // cloud density.

        // At cov = 0 the range starts at 1.0, above any fbm value, so nothing is
        // dense and the volume contributes nothing. Correct rather than a divide-by-
        // zero, and worth noting because it is not obvious.
        float d = smoothstep(1.0 - cov, 1.0 - cov + 0.35, base);
        // Erosion, only where there is already density to erode.
        if (d > 0.001 && d1.w > 0.001)
        {
            float det = wl_vol_noise(q * 3.7 + vec3(0.0, u_Time * d2.x * 0.5, 0.0));
            // Subtractive erosion: higher-frequency noise removes density.

            // Multiplied by d itself, so erosion scales with how dense the sample
            // already is -- thin regions lose proportionally less.
            d = max(d - det * d1.w * d, 0.0);
        }
        d *= max(d1.y, 0.0);
        float h = clamp((p.y - bmin.y) / hgt, 0.0, 1.0);
        // Vertical density profile: fade in over the bottom quarter, then taper over
        // the top 40%.

        // The result is a slab-shaped cloud rather than one filling the whole box,
        // which is what stops a volume box from reading as a box.
        d *= smoothstep(0.0, 0.25, h) * (1.0 - smoothstep(0.6, 1.0, h) * 0.7);
        float a = 1.0 - exp(-d * dt);
        if (a > 0.001)
        {
            float sh = 1.0;
            // Self-shadowing gate.
            if (d3.y > 0.01)
            {
                // Cheap self-shadow: single-octave density tap toward the sun
                // (a full second march is overkill for a soft darkening term).
                // The shadow tap: one step toward the sun, scaled by the local step size and
                // doubled.

                // A single tap, not a march -- the file's comment above says a full second
                // march is overkill for a soft darkening term. Cheap, and biased bright as a
                // result, since one tap cannot see an occluder further along the ray.
                vec3 sp = p + u_SunDirection * dt * 2.0;
                float sd = wl_vol_noise(sp * nScale + vec3(wnd.x, 0.0, wnd.y)) * max(d1.y, 0.0);
                // Beer-Lambert again, for the shadow term.
                sh = exp(-sd * dt * 4.0 * clamp(d3.y, 0.0, 1.0));
            }
            // Cloud scattering: ambient plus forward-scattered sun, plus the silver
            // lining on top.

            // The 0.35 floor means ambient never reaches zero -- fully shadowed cloud
            // still receives some light, which is physically wrong (a dense core should
            // be nearly black) but avoids cloud interiors going pure black.
            vec3 scol = tint * (amb * (0.35 + 0.65 * sh) + sunCol * sh * 0.9)
                      + sunCol * silverPow * clamp(d3.x, 0.0, 1.0) * 0.6;
            // Same premultiplied front-to-back accumulation as the fog march.
            acc += trans * a * scol;
            trans *= (1.0 - a);
            if (trans < 0.02)
            {
                trans = 0.0;
                break;
            }
        }
        t += dt;
    }
    return vec4(acc, trans);
}

// Composites every fog and cloud volume onto a base colour.

// The single entry point both shading paths call at the end, which is why the
// neural and classic branches duplicate the call rather than sharing the tail.
vec3 wl_apply_volumetrics(vec3 ro, vec3 rd, float tHit, vec3 baseColor, float dither, float sunVis)
{
    vec3 col = baseColor;
    if (u_CloudEnabled == 1)
    {
        for (int i = 0; i < 2; i++)
        {
            if (i >= u_CloudCount)
                break;
            if (u_CloudData3[i].z < 0.5)
                continue;
            vec4 r = wl_march_cloud_vol(ro, rd, tHit, u_CloudMin[i], u_CloudMax[i],
                                        u_CloudColor[i], u_CloudAmbient[i],
                                        u_CloudData[i], u_CloudData2[i], u_CloudData3[i], dither);
            // Premultiplied over: multiply by remaining transmittance, then add the
            // scattered colour.

            // Order matters. Clouds are applied FIRST and fog second, so a scene with
            // both gets clouds composited then fogged over them. Since fog is usually the
            // nearer, denser medium, that ordering happens to be right -- but it is a
            // fixed assumption, not a sorted one.
            col = col * r.a + r.rgb;
        }
    }
    if (u_FogEnabled == 1)
    {
        for (int i = 0; i < 4; i++)
        {
            if (i >= u_FogCount)
                break;
            // Per-slot enable flag, checked before marching.

            // Four fog slots are iterated regardless, so uploading fewer is free.
            if (u_FogData2[i].w < 0.5)
                continue;
            vec4 r = wl_march_fog_vol(ro, rd, tHit, u_FogMin[i], u_FogMax[i],
                                      u_FogColor[i], u_FogData[i], u_FogData2[i], dither, sunVis);
            col = col * r.a + r.rgb;
        }
    }
    return col;
}

// ---- Neural relief mapping (fragment-space POM) ----
// Height source: texture luminance + neural micro-relief from the existing
// detail MLP, so even flat albedo carries sub-texel geometry. Two triangles
// read as millions of polys: a UV-space height march (parallax), a
// derivative-scaled gradient normal, and a short light march for
// self-shadowing. Opt-in per material via Displacement / BumpStrength.
// The fragment-stage sampler switch. A byte-identical twin of wl_vtex_slot
// above, duplicated for the merged-program reason.

// Explicit LOD 0, not mipmapped -- correct for the parallax march, which offsets
// UVs per step and would otherwise sample across a mip transition. textureLod is
// required rather than merely preferable: this is reached from the march loop,
// whose trip count varies per pixel, and implicit-LOD fetches need derivatives
// that are undefined in non-uniform control flow. Renderer3D_Basic.hlsl pins the
// same LOD via SampleLevel(0) for the identical reason (FXC X3570), so the two
// backends now read the same height texels at every step.
vec4 wl_tex_slot(int slot, vec2 uv)
{
    switch (slot)
    {
        case 0:  return textureLod(u_Textures[0], uv, 0.0);
        case 1:  return textureLod(u_Textures[1], uv, 0.0);
        case 2:  return textureLod(u_Textures[2], uv, 0.0);
        case 3:  return textureLod(u_Textures[3], uv, 0.0);
        case 4:  return textureLod(u_Textures[4], uv, 0.0);
        case 5:  return textureLod(u_Textures[5], uv, 0.0);
        case 6:  return textureLod(u_Textures[6], uv, 0.0);
        case 7:  return textureLod(u_Textures[7], uv, 0.0);
        case 8:  return textureLod(u_Textures[8], uv, 0.0);
        case 9:  return textureLod(u_Textures[9], uv, 0.0);
        case 10: return textureLod(u_Textures[10], uv, 0.0);
        case 11: return textureLod(u_Textures[11], uv, 0.0);
        case 12: return textureLod(u_Textures[12], uv, 0.0);
        case 13: return textureLod(u_Textures[13], uv, 0.0);
        case 14: return textureLod(u_Textures[14], uv, 0.0);
        case 15: return textureLod(u_Textures[15], uv, 0.0);
        case 16: return textureLod(u_Textures[16], uv, 0.0);
        case 17: return textureLod(u_Textures[17], uv, 0.0);
        case 18: return textureLod(u_Textures[18], uv, 0.0);
        case 19: return textureLod(u_Textures[19], uv, 0.0);
        case 20: return textureLod(u_Textures[20], uv, 0.0);
        case 21: return textureLod(u_Textures[21], uv, 0.0);
        case 22: return textureLod(u_Textures[22], uv, 0.0);
        case 23: return textureLod(u_Textures[23], uv, 0.0);
        case 24: return textureLod(u_Textures[24], uv, 0.0);
        case 25: return textureLod(u_Textures[25], uv, 0.0);
        case 26: return textureLod(u_Textures[26], uv, 0.0);
        case 27: return textureLod(u_Textures[27], uv, 0.0);
        case 28: return textureLod(u_Textures[28], uv, 0.0);
        case 29: return textureLod(u_Textures[29], uv, 0.0);
        case 30: return textureLod(u_Textures[30], uv, 0.0);
        case 31: return textureLod(u_Textures[31], uv, 0.0);
        default: return vec4(1.0);
    }
}

// Procedural craggy breakup + strata banding. Added to final/gradient/shadow
// taps only (never the march), so detail rides on the macro silhouette.
// Procedural craggy breakup plus strata banding, added to the FINAL,
// gradient and shadow taps but never to the march itself.

// Deliberately excluded from the march: if it perturbed the marched height
// field, the intersection point would move and the silhouette would disagree
// with the geometry. Adding it only afterwards decorates without displacing.

// The cost of that choice is that the meso detail does not affect the
// parallax offset at all, so a deeply displaced surface shows meso detail
// sliding across it as the view moves.
float wl_relief_meso(vec2 uv, vec3 wp, float amp, float strataW)
{
    // Early out when there is no meso amplitude -- the common case, since amp
    // derives from the displacement that is usually 0.
    if (amp <= 0.001)
        return 0.0;
    vec2 muv = uv * 6.0;
    float n1 = wl_vol_noise(vec3(muv, 3.7));
    float n2 = wl_vol_noise(vec3(muv * 2.3 + 5.0, 9.1));
    float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
    return ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * amp
         + (st - 0.5) * amp * 0.8 * strataW;
}

float wl_relief_height(int slot, vec2 uv, vec3 n, float neuralK)
{
    float h = dot(wl_tex_slot(slot, uv).rgb, vec3(0.299, 0.587, 0.114));
    if (neuralK > 0.001)
        h += wl_neural_micro(uv, n) * 2.0 * neuralK;
    return clamp(h, 0.0, 1.5);
}

// Analytic component lights for the raster preview path. Mirrors the Nova
// evaluation (same falloff/cone/area model) but WITHOUT shadow rays — the
// raster path has no shadow maps; true shadows live in Nova.
// Analytic lights for the raster preview path.

// The file's comment above is explicit that this mirrors the Nova ray-trace
// evaluation -- same falloff, cone and area model -- but WITHOUT shadow rays,
// because the raster path has no shadow maps.

// So lit geometry here and ray-traced geometry elsewhere will disagree in
// shadowed areas. That is a known, documented limitation rather than a bug,
// and it is worth knowing when comparing the two backends.
vec3 wl_analytic_lights(vec3 albedo, float metallic, float roughness, vec3 N, vec3 wp)
{
    vec3 V = normalize(u_CameraPosition - wp);
    // The dielectric/metal reflectance blend: 0.04 for an insulator, full albedo
    // for a metal.
    vec3 F0 = mix(vec3(0.04), albedo, clamp(metallic, 0.0, 1.0));
    float rough = clamp(roughness, 0.04, 1.0);
    vec3 acc = vec3(0.0);
    // The 8-light loop, bounded by the constant matching the uniform array size.
    for (int a = 0; a < 8; a++)
    {
        // Dynamic exit, so an unused slot costs nothing beyond the branch.
        if (a >= u_ALightCount) break;
        float atype = u_ALightData[a].x;
        vec3 lcol = u_ALightColor[a] * u_ALightData[a].y;
        float arange = max(u_ALightData[a].z, 0.0);
        // Falloff exponent, floored at 0.5.

        // The floor silently changes authored behaviour: a falloff of 0 becomes 0.5
        // rather than producing the divide-by-zero the formula below would otherwise
        // risk. A material asking for zero falloff gets a mild curve instead.
        float afall = max(u_ALightData[a].w, 0.5);
        vec3 L;
        float att = 1.0;
        // Type 0: directional.
        if (atype < 0.5) {
            // NEGATED. u_ALightDir is stored as the direction light travels, so the
            // vector toward the light is its negative.

            // An easy sign to get wrong, and a wrong one here makes every surface light
            // as though the sun were below it.
            L = -u_ALightDir[a];
        } else {
            vec3 toL = u_ALightPos[a] - wp;
            float d = length(toL);
            L = toL / max(d, 0.00001);
            if (arange > 0.0) {
                if (d >= arange) continue;
                // Finite-range falloff: linear distance term raised to the falloff exponent.
                att *= pow(clamp(1.0 - d / arange, 0.0, 1.0), afall);
            } else {
                // Infinite-range falloff: inverse-square-ish, softened by the exponent.

                // The 0.1 factor means it is not true inverse-square; d*d keeps the shape
                // while the constant keeps a point light from exploding at close range.
                att *= 1.0 / (1.0 + 0.1 * afall * d * d);
            }
            // Type 2: spot. The range test on atype rather than == keeps this an else-if
            // chain after the directional branch.
            if (atype > 1.5 && atype < 2.5) {
                float c = dot(-L, u_ALightDir[a]);
                // Cone falloff, with the EDGE arguments deliberately swapped.

                // smoothstep(edge0, edge1, x) requires edge0 < edge1 to increase. Here
                // innerCos is passed as edge1 and outerCos as edge0, so the result goes 0 at
                // the cone edge and 1 at the centre. A correct but easily misread use of a
                // function whose arguments are order-sensitive.
                att *= smoothstep(u_ALightData2[a].y, u_ALightData2[a].x, c);
                if (att <= 0.0) continue;
            } else if (atype > 2.5) {
                float front = dot(-L, u_ALightDir[a]);
                // Type 3: area light. The double-sided flag gates the front-hemisphere test.
                if (u_ALightData2[a].w < 0.5) {
                    if (front <= 0.0) continue;
                    att *= clamp(front * 3.0, 0.0, 1.0);
                }
                float asize = max(u_ALightData2[a].z, 0.01);
                // Area-light falloff by apparent solid angle -- a size over distance term.

                // Cheap, and it has the right qualitative behaviour (a large light falls off
                // slowly), though it is not the correct solid-angle integral.
                att *= asize / (d + asize);
            }
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;
        vec3 H = normalize(V + L);
        vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        // Blinn-Phong-style specular with an exponent interpolated from 128 (smooth)
        // to 8 (rough).

        // Not a GGX microfacet lobe, so this is an approximation of the PBR response
        // rather than the real thing -- visible as a slightly wrong highlight shape at
        // mid roughness. Adequate for a preview path that also lacks shadows.
        float spec = pow(max(dot(N, H), 0.0), mix(128.0, 8.0, rough));
        acc += (albedo * (1.0 - metallic) * NdotL + F * spec * NdotL) * lcol * att;
    }
    return acc;
}

void main()
{
    // The key light direction -- and it is HARDCODED, not a uniform.

    // DEFECT: every lit pixel in this shader uses this one direction. The engine
    // can set u_SunDirection for fog and clouds, and the analytic lights come from
    // u_ALight*, but the primary diffuse and specular response ignores all of
    // them. So a scene with a directional light pointing elsewhere is lit from
    // (0.3, 1.0, 0.4) regardless, and there is no way to change it from content.
    vec3 lightDir = normalize(vec3(0.3, 1.0, 0.4));
    // The geometric normal. Everything that perturbs shading uses a separate
    // variable so the original stays available for the height taps and the depth
    // write.
    vec3 norm0 = normalize(Input.Normal);
    vec3 V = normalize(u_CameraPosition - Input.WorldPos);
    // Water marker: water verts arrive with BumpStrength = -1 (the relief
    // path above already skipped them: its gate needs values > 0.001).

    // ---- Neural relief: UV-space height march (parallax) + gradient normal
    // + light self-shadow. Distance- and grazing-faded, zero taps when the
    // material opts out (Displacement and BumpStrength both 0).
    vec2 reliefUV = Input.TexCoord * Input.TilingFactor;
    vec3 norm = norm0;
    float reliefShadow = 1.0;
    float reliefPush = 0.0;
    float reliefH = -1.0;
    float mesoAmp = 0.0;
    float strataW = clamp(1.0 - abs(norm0.y), 0.15, 1.0);
    {
        float disp = Input.Displacement;
        float bump = Input.BumpStrength;
        int slot = int(Input.TexIndex);
        // Distance to camera, recomputed here although V above already encodes a
        // normalised view vector. Duplicated work, one square root per fragment.
        float dist = length(u_CameraPosition - Input.WorldPos);
        // Distance fade: full within 18 units, fading to zero by 48.

        // Magic numbers in world units, so the effect's range depends on the scene's
        // scale. A large open world gets no fade at all; a small one loses the effect
        // almost immediately.
        float fade = clamp(1.0 - (dist - 18.0) / 30.0, 0.0, 1.0);
        float ndv = abs(dot(V, norm0));
        // Same angular collapse as Nova: kill POM smear streaks edge-on.
        // Grazing-angle collapse: the parallax offset is scaled toward zero as the
        // surface turns edge-on.

        // The file's comment notes this matches Nova. Necessary because at grazing
        // angles the parallax step length approaches infinity and produces streaks.
        float marchK = smoothstep(0.10, 0.45, ndv);
        // Tangent frame from screen-space derivatives: uniform control flow,
        // so no undefined-derivative issues inside the gated march below.
        vec3 dpx = dFdx(Input.WorldPos);
        vec3 dpy = dFdy(Input.WorldPos);
        vec2 dtx = dFdx(reliefUV);
        vec2 dty = dFdy(reliefUV);
        // Tangent from screen-space derivatives: the cross product of the world-
        // position derivative with the UV derivative, weighted by their determinants.

        // No attributes for tangent/bitangent are uploaded, so this reconstructs the
        // frame per pixel. It costs derivatives and can degenerate on degenerate
        // UVs -- hence the guard on the next line.
        vec3 T = dpx * dty.y - dpy * dtx.y;
        // Gram-Schmidt: remove the component along the normal so T is genuinely
        // tangent.

        // The vec3(1e-6) addition is a divide-by-zero guard -- normalize() of a zero
        // vector would give NaN, which then propagates through the whole march.

        // DEFECT: it adds a constant to ALL THREE components, so the resulting
        // tangent is not exactly orthogonal to the normal even after the projection.
        // The error is on the order of 1e-6 and is invisible, but the correct form
        // is to normalise first and only then add the epsilon to one component.
        T = normalize(T - norm0 * dot(norm0, T) + vec3(1e-6));
        vec3 B = normalize(cross(norm0, T));
        float neuralK = (u_NeuralEnabled == 1) ? clamp(u_NeuralTexStrength, 0.0, 1.0) : 0.0;
        // The relief gate: any of displacement or bump, within the fade range, and
        // not too edge-on.

        // Three separate early-outs, and all three matter for cost -- a material with
        // both zero skips the entire block below, which is the common case.

        // Note ndv > 0.12 here is a separate, HARDER threshold than marchK's smooth
        // step from 0.10: relief is skipped entirely below 0.12 rather than scaled
        // down, which prevents the streak artefacts at extreme grazing angles.
        if ((disp > 0.001 || bump > 0.001) && fade > 0.001 && ndv > 0.12)
        {
            float ds = disp * fade;
            float bs = bump * fade;
            mesoAmp = 0.25 * clamp(ds, 0.0, 1.5);
            // The parallax depth scale, capped at 0.12 UV units.

            // The cap bounds how far a fragment can shift, which bounds both the artefact
            // risk and the cost of the clamp on delta below.
            float pScale = clamp(ds * 0.05, 0.0, 0.12);
            float finalH = wl_relief_height(slot, reliefUV, norm0, neuralK);
            if (ds > 0.001)
            {
                vec3 viewTS = vec3(dot(V, T), dot(V, B), dot(V, norm0));
                // Layer count interpolated from 14 (facing) to 6 (grazing) -- fewer steps at
                // grazing angles, where each step is longer anyway.

                // Clamped by the loop bound of 14 below; layers can never exceed it.
                int layers = int(mix(14.0, 6.0, ndv));
                // The per-step UV advance, in tangent space.

                // NEGATED so the march goes INTO the surface rather than out of it. Divided
                // by viewTS.z so the step is perspective-correct -- the same ratio a real
                // ray would take. max(, 0.08) prevents a huge step when the view direction is
                // nearly parallel to the surface.
                vec2 stepUV = (-viewTS.xy / max(viewTS.z, 0.08)) * (pScale / float(layers));
                float dh = 1.0 / float(layers);
                vec2 cuv = reliefUV;
                float clH = 1.0;
                float cTexH = finalH;
                vec2 puv = cuv;
                float plH = clH;
                float pTexH = cTexH;
                for (int i = 0; i < 14; i++)
                {
                    if (i >= layers)
                        break;
                    puv = cuv;
                    plH = clH;
                    pTexH = cTexH;
                    cuv += stepUV;
                    clH -= dh;
                    cTexH = wl_relief_height(slot, cuv, norm0, neuralK);
                    // Intersection test: the surface height has risen above the ray height, so
                    // the ray has entered the surface.

                    // The comparison is strict, so an exact tie is treated as "not yet", which
                    // costs one extra step but avoids a zero-width intersection.
                    if (cTexH > clH)
                        break;
                }
                // Binary refine: crisp intersections for deep relief (4 taps).
                // Binary refinement, 4 iterations narrowing the bracket by 16x.
                for (int b = 0; b < 4; b++)
                {
                    vec2 midUV = (puv + cuv) * 0.5;
                    float midH = wl_relief_height(slot, midUV, norm0, neuralK);
                    float midL = (plH + clH) * 0.5;
                    if (midH > midL)
                    {
                        cuv = midUV;
                        clH = midL;
                        cTexH = midH;
                    }
                    else
                    {
                        puv = midUV;
                        plH = midL;
                        pTexH = midH;
                    }
                }
                float rd1 = plH - pTexH;
                float rd2 = cTexH - clH;
                // The intersection parameter, interpolated between the bracketing steps.

                // Linear interpolation of the height difference rather than a second linear
                // solve -- cheaper, slightly less accurate. The max() guards a zero sum when
                // both bracketing differences are zero.
                float rw = rd1 / max(rd1 + rd2, 0.0001);
                vec2 delta = (mix(puv, cuv, rw) - reliefUV) * marchK;
                // Hard clamp on the parallax offset at 0.08 UV units -- the same 0.08 that
                // bounds pScale.

                // Clamping by magnitude rather than per-component, so the offset keeps its
                // direction and just gets shorter. Correct choice; a per-component clamp would
                // distort the direction.
                if (length(delta) > 0.08)
                    delta = normalize(delta) * 0.08;
                reliefUV += delta;
                finalH = mix(pTexH, cTexH, rw);
                finalH = clamp(finalH + wl_relief_meso(reliefUV, Input.WorldPos, mesoAmp, strataW), 0.0, 1.5);
                // The height at the surface intersection, later used for the weathering
                // tint below. Negative (-1) means no relief ran.
                reliefH = finalH;
                // Shape-preserving push (bilateral around the mid-level, like
                // Nova): the base shape survives, features rise and recess.
                reliefPush = (finalH - 0.5) * ds;
            }
            if (bs > 0.001)
            {
                // The finite-difference epsilon for the gradient normal, in UV units.

                // A fixed epsilon trades accuracy for stability: too small amplifies
                // precision noise, too large loses detail. 0.004 is a middle choice that
                // works at typical texture scales.
                float e = 0.004;
                float hU = wl_relief_height(slot, reliefUV + vec2(e, 0.0), norm0, neuralK)
                         + wl_relief_meso(reliefUV + vec2(e, 0.0), Input.WorldPos, mesoAmp, strataW);
                float hV = wl_relief_height(slot, reliefUV + vec2(0.0, e), norm0, neuralK)
                         + wl_relief_meso(reliefUV + vec2(0.0, e), Input.WorldPos, mesoAmp, strataW);
                float dhdu = (hU - finalH) / e;
                float dhdv = (hV - finalH) / e;
                // UV-per-world measured per pixel: raster faces span 0-1 UV
                // over arbitrary world sizes, so Nova's ~1 UV/m assumption
                // does not hold here.
                // World units per UV unit, measured per pixel from the derivatives.

                // The file's comment explains why this is measured rather than assumed:
                // raster faces span 0-1 UV over arbitrary world sizes, so Nova's ~1 UV/m
                // assumption does not hold here.

                // Clamped to [0.05, 200] to bound a degenerate case -- a UV derivative near
                // zero (a surface seen exactly face-on with flat UVs) would otherwise produce
                // an enormous scale and a wild normal.
                float worldPerUV = clamp(length(dpx) / max(length(dtx), 1e-6), 0.05, 200.0);
                vec3 grad = (T * dhdu + B * dhdv) * (bs / worldPerUV);
                float gl = length(grad);
                // Gradient magnitude clamp, for the same reason as the offset clamp: bound
                // the perturbation so a noisy height field cannot produce a black or inverted
                // normal.
                if (gl > 1.5)
                    grad /= gl / 1.5;
                norm = normalize(norm0 - grad);
            }
            if (ds > 0.001)
            {
                // Cheap heightfield self-shadow: 4 taps toward the light.
                vec3 lts = vec3(dot(lightDir, T), dot(lightDir, B), dot(lightDir, norm0));
                vec2 sStep = (lts.xy / max(lts.z, 0.1)) * (pScale / 4.0);
                float rayH = finalH + 0.03;
                float occ = 1.0;
                vec2 suv = reliefUV;
                for (int i = 0; i < 4; i++)
                {
                    suv += sStep;
                    rayH += 0.06;
                    float hh = wl_relief_height(slot, suv, norm0, neuralK)
                             + wl_relief_meso(suv, Input.WorldPos, mesoAmp, strataW);
                    occ = min(occ, clamp((rayH - hh) * 8.0, 0.0, 1.0));
                }
                reliefShadow = clamp(occ, 0.3, 1.0);
            }
        }
    }

    // Screen-space displacement: commit the relief to the depth buffer so it
    // self-occludes and intersects other geometry like real polygons. Untouched
    // (== interpolated triangle depth) for every non-relief fragment.
    // The depth write, gated on a non-trivial push.

    // The gate is the point of the early_fragment_tests declaration: without
    // it, every fragment would write gl_FragDepth and late-z would be forced
    // program-wide.
    if (abs(reliefPush) > 0.00001)
    {
        vec4 dispClip = u_ViewProjection * vec4(Input.WorldPos + norm0 * reliefPush, 1.0);
        // NDC z remapped from [-1,1] to [0,1], the OpenGL depth range.

        // Reprojecting rather than interpolating is what makes the displacement
        // self-occlude: the fragment claims the depth of its DISPLACED position, so a
        // pushed-in surface is correctly occluded by pushed-out geometry.

        // DEFECT (precision): this is a full 32-bit float depth write with no
        // correction for the interpolation that the varying already performed. The
        // displaced world position went through perspective-correct interpolation, so
        // reprojecting it and then adding the 0.5*z + 0.5 bias can disagree slightly
        // with the rasteriser's own depth at the same pixel. On a 24-bit depth buffer
        // this is usually invisible; with a far plane far away it can produce z-fighting
        // along relief silhouettes.
        gl_FragDepth = (dispClip.z / dispClip.w) * 0.5 + 0.5;
    }

    vec4 texColor = Input.Color;

    // Evaluate hardware texture slots using an integer cast selector
    switch(int(Input.TexIndex))
    {
        case 0:  texColor *= texture(u_Textures[0],  reliefUV); break;
        case 1:  texColor *= texture(u_Textures[1],  reliefUV); break;
        case 2:  texColor *= texture(u_Textures[2],  reliefUV); break;
        case 3:  texColor *= texture(u_Textures[3],  reliefUV); break;
        case 4:  texColor *= texture(u_Textures[4],  reliefUV); break;
        case 5:  texColor *= texture(u_Textures[5],  reliefUV); break;
        case 6:  texColor *= texture(u_Textures[6],  reliefUV); break;
        case 7:  texColor *= texture(u_Textures[7],  reliefUV); break;
        case 8:  texColor *= texture(u_Textures[8],  reliefUV); break;
        case 9:  texColor *= texture(u_Textures[9],  reliefUV); break;
        case 10: texColor *= texture(u_Textures[10], reliefUV); break;
        case 11: texColor *= texture(u_Textures[11], reliefUV); break;
        case 12: texColor *= texture(u_Textures[12], reliefUV); break;
        case 13: texColor *= texture(u_Textures[13], reliefUV); break;
        case 14: texColor *= texture(u_Textures[14], reliefUV); break;
        case 15: texColor *= texture(u_Textures[15], reliefUV); break;
        case 16: texColor *= texture(u_Textures[16], reliefUV); break;
        case 17: texColor *= texture(u_Textures[17], reliefUV); break;
        case 18: texColor *= texture(u_Textures[18], reliefUV); break;
        case 19: texColor *= texture(u_Textures[19], reliefUV); break;
        case 20: texColor *= texture(u_Textures[20], reliefUV); break;
        case 21: texColor *= texture(u_Textures[21], reliefUV); break;
        case 22: texColor *= texture(u_Textures[22], reliefUV); break;
        case 23: texColor *= texture(u_Textures[23], reliefUV); break;
        case 24: texColor *= texture(u_Textures[24], reliefUV); break;
        case 25: texColor *= texture(u_Textures[25], reliefUV); break;
        case 26: texColor *= texture(u_Textures[26], reliefUV); break;
        case 27: texColor *= texture(u_Textures[27], reliefUV); break;
        case 28: texColor *= texture(u_Textures[28], reliefUV); break;
        case 29: texColor *= texture(u_Textures[29], reliefUV); break;
        case 30: texColor *= texture(u_Textures[30], reliefUV); break;
        case 31: texColor *= texture(u_Textures[31], reliefUV); break;
    }

    float metallic = clamp(Input.Metallic, 0.0, 1.0);
    float roughness = clamp(Input.Roughness, 0.04, 1.0);

    // Relief weathering: dust settles in pits, sun bleaches ridge tops.
    if (reliefH >= 0.0)
    {
        // Cavity mask: 1 in pits, 0 on high ground. Note the REVERSED smoothstep
        // edges -- 0.45 to 0.05, not 0.05 to 0.45 -- which inverts the result.
        float cav = smoothstep(0.45, 0.05, reliefH);
        float ridge = smoothstep(0.55, 0.95, reliefH);
        // Dust settles in pits: up to 30% darkening in the cavities.

        // Multiplied rather than mixed toward a dust colour, so it darkens without
        // shifting hue.
        texColor.rgb *= (1.0 - 0.30 * cav);
        // Sun bleaching on ridge tops: a warm tint scaled by the ridge mask and
        // capped at 0.65 so it tints rather than replaces.
        texColor.rgb = mix(texColor.rgb, texColor.rgb * vec3(1.10, 1.03, 0.92), ridge * 0.65);
    }

    // ---- Water: mirror + pixel ripple detail (Unreal-style) ----
    // Dielectric water would get no sky reflection from the metal-gated env
    // term below, so route water through the mirror path instead: diffuse
    // dies (water has none), F0 picks up the water tint, and the sky mirror
    // + sun glints take over. norm (Gerstner mesh normal) is augmented with
    // animated pixel-level ripple octaves so chop survives at any mesh
    // density; the existing fresnel/spec/analytic-light terms then respond
    // to the perturbed normal with zero further changes.
    // The water marker. The vertex stage's comment explains the convention:
    // water vertices arrive with BumpStrength = -1.

    // So the SAME float that means "bump amount" for a normal material means
    // "this is water" here. Convenient, since it needs no extra attribute, but
    // it means a material cannot have both water behaviour and a bump amount.
    bool wlWater = (Input.BumpStrength < -0.5);
    if (wlWater)
    {
        // Ripple coordinates from WORLD xz rather than UV.

        // Correct for water: ripples should stay fixed in world space, so two
        // adjacent water meshes with different UVs still show one continuous surface.
        // Using UV here would make the ripples swim when the mesh is scaled.
        vec2 wpuv = Input.WorldPos.xz;
        float wdist = length(u_CameraPosition - Input.WorldPos);
        // Distance fade: full within 25 units, gone by 85.

        // Separate constants from the relief fade above (18/30), so the two effects
        // do not share a distance profile.
        float wfade = clamp(1.0 - (wdist - 25.0) / 60.0, 0.0, 1.0);
        if (wfade > 0.001)
        {
            float wt = u_Time;
            vec2 wg = vec2(0.0);
            // Three travelling cosine waves at different directions, frequencies and
            // amplitudes.

            // Each `dot(dir, wpuv)` is a plane wave along that direction; the amplitude
            // falls with frequency (0.22, 0.13, 0.07) so the chop has a natural spectrum
            // rather than uniform amplitude.

            // Cheap and it works, but it is a standing-wave sum rather than a Gerstner
            // or FBM ocean, so the pattern repeats visibly across a large water body.
            wg += vec2(0.96, 0.28) * (cos(dot(vec2(0.96, 0.28), wpuv) * 2.1 + wt * 1.6) * 0.22);
            wg += vec2(-0.42, 0.91) * (cos(dot(vec2(-0.42, 0.91), wpuv) * 3.7 - wt * 2.3 + 1.7) * 0.13);
            wg += vec2(0.66, -0.75) * (cos(dot(vec2(0.66, -0.75), wpuv) * 7.9 + wt * 3.4 + 4.2) * 0.07);
            // Perturb the surface normal by the ripple gradient.

            // NEGATED for the same reason the directional light is negated above: the
            // gradient points toward increasing wave height, and a surface rising toward
            // the viewer should tilt away.
            norm = normalize(norm + vec3(-wg.x, 0.0, -wg.y) * (0.55 * wfade));
        }
        // Water is forced fully metallic.

        // NOT physical -- water is a dielectric and should have F0 near 0.02 -- but
        // it is a deliberate route around the file's own explanation above: a
        // dielectric gets no sky reflection from the metal-gated environment term, so
        // forcing metallic is how the mirror gets its reflection. The comment above
        // calls this out explicitly.

        // The consequence is that water reflects the full sky gradient at grazing
        // angles but has no Fresnel falloff toward vertical, where real water becomes
        // transparent.
        metallic = 1.0;
    }

    // Metallic PBR terms (shared by neural + classic paths). Diffuse dies
    // out on metals; reflection + specular take over. No shadow maps on the
    // raster path — true shadows live in the Nova ray-trace pipeline.
    vec3 F0 = mix(vec3(0.04), texColor.rgb, metallic);
    vec3 R = reflect(-V, norm);
    vec3 specSky = mix(u_SkyBottomColor, u_SkyTopColor, R.y * 0.5 + 0.5);
    vec3 diffSky = mix(u_SkyBottomColor, u_SkyTopColor, norm.y * 0.5 + 0.5);
    vec3 envColor = mix(specSky, (specSky + diffSky) * 0.5, roughness * roughness);
    float NdotL = max(dot(norm, lightDir), 0.0);
    vec3 H = normalize(lightDir + V);
    float specTerm = pow(max(dot(norm, H), 0.0), mix(128.0, 8.0, roughness))
                   * (1.0 - roughness * 0.7) * reliefShadow;
    // Schlick Fresnel: rises to 1.0 at grazing angles.

    // The 5.0 exponent is the standard Schlick constant for F0 = 0.04.
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    float diffuseIntensity = NdotL * reliefShadow;
    // A flat ambient constant rather than a uniform.

    // No studio control over ambient level without recompiling the shader -- the
    // same class of limitation as the hard-coded lightDir and sunCol.
    float ambientIntensity = 0.2;

    // The neural shading branch, taken as a whole and exited early below.

    // `== 1` rather than a bool test, consistent with every other use in this
    // file.
    if (u_NeuralEnabled == 1)
    {
        if (!wlWater)
        {
            // The detail lookup, using the possibly-relief-offset UV so the detail
            // tracks the parallax rather than sliding across it.
            vec3 detail = wl_neural_tex_detail(reliefUV, norm);
            texColor.rgb *= mix(vec3(1.0), detail, clamp(u_NeuralTexStrength, 0.0, 1.0));
        }

        vec3 namb = wl_neural_ambient(norm, texColor.rgb);
        float k = clamp(u_NeuralLightStrength, 0.0, 1.0);
        float km = clamp(u_NeuralMatStrength, 0.0, 1.0);
        // Neural material: porosity/patina albedo, wrap diffuse, flake glints
        float matGlint = 0.0;
        float matWrap = 0.5;
        vec3 matAlbedo = texColor.rgb;
        {
            float g;
            float w;
            wl_neural_mat_raster(reliefUV, norm, matAlbedo, g, w);
            matGlint = g;
            matWrap = w;
        }
        if (!wlWater)
            texColor.rgb = mix(texColor.rgb, matAlbedo, km);
        // Re-derive F0 from the neural albedo so flakes tint reflections.
        vec3 nF0 = mix(vec3(0.04), texColor.rgb, metallic);
        vec3 nF = nF0 + (1.0 - nF0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        // Wrapped diffuse: the lighting term is offset toward the light by the wrap
        // width, so the surface stays lit slightly past its terminator.

        // The /(1 + matWrap*km) renormalises so the maximum stays at 1 and wrapping
        // does not simply brighten the surface.
        float wrapDiffuse = clamp((dot(norm, lightDir) + matWrap * km) / (1.0 + matWrap * km), 0.0, 1.0);
        float diffTerm = mix(diffuseIntensity, wrapDiffuse, km * 0.7) * (1.0 - metallic);
        vec3 warmLight = vec3(1.0, 0.98, 0.94);
        // The neural diffuse assembly.

        // `1.0 - metallic * 0.85` never fully removes diffuse from a metal -- at
        // metallic = 1 it leaves 15%. That looks like an intentional fudge to keep
        // metals from going flat black, but it is physically wrong (a metal has no
        // diffuse) and it is easy to misread as a typo.
        vec3 lit = texColor.rgb * (diffTerm * warmLight + mix(vec3(ambientIntensity), namb * 2.2, k) * (1.0 - metallic * 0.85));
        lit += nF * envColor * metallic;
        lit += specTerm * nF * NdotL;
        // Sparse metallic flake glints, tinted warm.

        // Scaled by diffuseIntensity so a glint only appears where the surface is
        // lit -- a flake in shadow contributes nothing, which is the right behaviour
        // for a specular effect.
        lit += matGlint * km * (0.25 + 0.75 * metallic) * diffuseIntensity * vec3(1.3, 1.25, 1.15);
        // The analytic lights, multiplied by the relief shadow term computed much
        // earlier.

        // So the heightfield self-shadow darkens the analytic contribution. Note
        // this multiplies the LIGHT, not the result, so it attenuates but does not
        // tint.
        lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
        {
            vec3 vdir = Input.WorldPos - u_CameraPosition;
            float vdist = length(vdir);
            lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(gl_FragCoord.xy), 1.0);
        }
        color = vec4(lit, texColor.a);
        // Both exits write the entity ID to the integer attachment.

        // Written on every path, including the early neural return below -- so the
        // attachment is never left undefined for a fragment that did reach it.
        color2 = v_EntityID;
        return;
    }

    // The CLASSIC path: the same model without the neural terms.

    // Diffuse dies out on metals entirely here (no 0.85 fudge as in the neural
    // branch), so the classic and neural renderings of the same metal differ.

    // That means toggling u_NeuralEnabled changes the lighting response, not just
    // the detail -- a content change with a visible global effect.
    vec3 diffTerm = texColor.rgb * (1.0 - metallic) * (ambientIntensity + diffuseIntensity);
    vec3 lit = diffTerm + F * envColor * metallic + specTerm * F * NdotL;
    lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
    {
        vec3 vdir = Input.WorldPos - u_CameraPosition;
        float vdist = length(vdir);
        lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(gl_FragCoord.xy), 1.0);
    }

    color = vec4(lit, texColor.a);
    color2 = v_EntityID;
}
