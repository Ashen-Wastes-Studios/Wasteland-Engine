#type vertex

// ============================================================================
// SHARED DECLARATIONS (vertex + pixel stages)
// ============================================================================

// Bound textures and one sampler, shared by both stages. 32 slots is the array size
// the pixel stage unrolls against, so raising it means editing the unrolled loop
// there too -- the two are not independent.
Texture2D    u_Textures[32] : register(t0);
// A single SamplerState rather than per-texture samplers: every texture in this
// material set is sampled the same way, so there is nothing to vary per binding.
SamplerState u_Sampler      : register(s0);

// ---- GLSL-semantics helpers ----
// fract2 overloads rather than HLSL's frac(). They exist so the GLSL original ports
// across textually -- fxg has no frac, and using it would make the two shader files
// diverge in style for no benefit.
float  fract2(float x)  { return x - floor(x); }
float2 fract2(float2 x) { return x - floor(x); }
float3 fract2(float3 x) { return x - floor(x); }
float4 fract2(float4 x) { return x - floor(x); }
// lerp_safe is a pure rename of lerp, provided so the GLSL source ports
// line-for-line. In HLSL it adds nothing; the indirection is the price of keeping
// the two implementations diffable.
float  lerp_safe(float a, float b, float t)    { return lerp(a, b, t); }
float2 lerp_safe(float2 a, float2 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float t)  { return lerp(a, b, t); }
float4 lerp_safe(float4 a, float4 b, float t)  { return lerp(a, b, t); }
// The float3-vector overload of lerp_safe has NO HLSL equivalent to hide: HLSL's
// lerp does not accept a per-component t, so this overload is genuinely required
// for the port and is not a rename.
float3 lerp_safe(float3 a, float3 b, float3 t) { return lerp(a, b, t); }

// TEX is redefined per stage. The vertex stage maps texture() to SampleLevel(0)
// because a vertex shader has no valid derivatives to compute a mip from, while the
// pixel stage uses Sample() for implicit-LOD selection.
// TEX/TEX_LOD macros are defined per-stage below (Sample vs SampleLevel).

// SampleLevel with an explicit LOD 0, not Sample(). See the note above: implicit
// derivatives (ddx/ddy) are undefined in a vertex shader.
#define TEX(tex, uv)        (tex).SampleLevel(u_Sampler, (uv), 0.0f)
#define TEX_LOD(tex, uv, l) (tex).SampleLevel(u_Sampler, (uv), (l))

// The parity notes that follow are the most useful comment block in the file: they
// record what this port does and does NOT reproduce from the GLSL, which is the
// information a reader of either file needs and which neither file can state about
// itself.
// ============================================================================
// Renderer3D_Basic.hlsl -- HLSL (SM 5.0) port of Renderer3D_Basic.glsl
// (vertex + pixel) for the NVRHI D3D11/D3D12 raster path.
//
// Functional parity notes:
//  - Full port of the GLSL: neural relief POM (uv march + gradient normal +
//    self-shadow), per-vertex neural displacement, water ripples + mirror,
//    volumetric fog/clouds, analytic component lights, neural material/
//    ambient MLPs, weathering, EntityID picking.
//  - Vertex inputs use the BufferLayout attribute names as D3D semantics
//    (NVRHI passes them through verbatim); float/int formats match
//    ShaderDataTypeToNVRHIFormat.
// THE TWO-CBUFFER SPLIT, and it is a consequence of the reflection model rather than
// a design choice. NVRHIShader records one location per stage that DECLARES a
// name, and SetUniformData writes every such stage, so a uniform may be declared
// in both stage cbuffers and a single SetX call feeds both copies.
//  - u_ViewProjection lives in the VS cbuffer (b0) only. The PS cannot read it
//    directly, so the VS forwards the four rows as nointerpolation
//    interpolators and the PS reassembles the matrix -- the only way to get a
//    uniform that lives in the other stage's cbuffer back into this one. It is
//    used to commit the per-fragment relief push to SV_Depth exactly
//    (gl_FragDepth parity, with [earlydepthstencil]).
//  - u_NeuralEnabled / u_NeuralTexStrength are declared in BOTH b0 and b1,
//    because both stages need them: the PS for wl_relief_height, and the VS for
//    wl_vheight's neural micro term. That term used to be absent from the vertex
//    height field, which made displaced geometry disagree with the fragment
//    relief at silhouettes. Fixed by duplicating the declarations; no call site
//    changed, because the uniform map writes every stage that declares a name.
//  - Implicit-LOD texture() in the PS maps to Sample() (auto-mip + driver
//    anisotropy, like GL); in the VS it maps to SampleLevel(0) (GL VS
//    behavior).
// ============================================================================

// The varying struct. Every field crosses the rasteriser, so each one costs
// interpolator slots -- which is why this float-packs several small values rather
// than using more TEXCOORDs. The interpolator budget is a hard limit.
struct VSOutput
{
    float4 Position  : SV_Position;
    float4 Color     : COLOR0;
    float3 Normal    : NORMAL0;
    float2 TexCoord  : TEXCOORD0;
    float  TexIndex  : TEXCOORD1;
    // TEXCOORD1..7 carry single floats. Each costs a full interpolator slot whether it
    // needs one or not, which is the usual reason a shader like this runs out of varying
    // space before it runs out of ALU.
    float  TilingFactor : TEXCOORD2;
    float3 WorldPos  : TEXCOORD3;
    float  Metallic  : TEXCOORD4;
    float  Roughness : TEXCOORD5;
    float  Displacement : TEXCOORD6;
    float  BumpStrength : TEXCOORD7;
    // nointerpolation on an int handle: an entity id interpolated across a triangle
    // becomes a value belonging to no entity.
    nointerpolation int    v_EntityID : TEXCOORD8;
    // The VP rows, forwarded so the PS can rebuild the matrix for the per-fragment
    // relief push. nointerpolation because these are constant across the draw call,
    // not per-pixel quantities -- interpolating them would be wasted precision.
    nointerpolation float4 u_VPRow0 : TEXCOORD9;
    nointerpolation float4 u_VPRow1 : TEXCOORD10;
    nointerpolation float4 u_VPRow2 : TEXCOORD11;
    nointerpolation float4 u_VPRow3 : TEXCOORD12;
};

// The vertex-stage cbuffer. u_ViewProjection plus the two neural flags.
//
// The neural uniforms are DUPLICATED from the PS cbuffer (b1) on purpose. HLSL
// scopes a cbuffer to the stage that declares it, so the vertex stage cannot
// read u_NeuralEnabled out of b1 -- and without it wl_vheight has no neural
// micro term, so per-vertex displacement silently differs from the GLSL and the
// displaced geometry does not match the fragment relief. The engine's uniform
// map records a name once per stage that declares it and SetUniformData writes
// every stage, so one SetInt/SetFloat call feeds both copies; declaring the
// names twice is safe and requires no change at the call sites.
cbuffer TransformConstants : register(b0)
{
    float4x4 u_ViewProjection;
    // Mirrors b1's declarations exactly in name and type. The reflection offsets
    // differ (b0 vs b1), which is why the engine keys locations per stage.
    int   u_NeuralEnabled;
    float u_NeuralTexStrength;
};

// The vertex-stage neural displacement block. It mirrors the fragment relief
// stack below so that geometry and shading agree.
// ---- Neural geometric displacement (vertex stage) ----
// Same height field the fragment relief shades (luminance macro + craggy
// meso/strata + neural micro), evaluated per-vertex so silhouettes and
// intersections become real geometry. v-prefixed: the fragment stage defines
// The v- prefix on every helper: the fragment stage below defines same-named
// functions, and a single compiled program cannot hold duplicate symbols. The prefix
// is a workaround for the single-file two-stage layout, not a convention.
// same-named helpers and one program cannot hold duplicate symbols.



// A 2D -> 1D hash built from the standard fract/dot construction. Used for the
// per-cell GRAIN term in the detail function below.
float wl_vhash12(float2 p)
{
    // fract2 rather than frac, and the 0.1031 multiplier: the constants are arbitrary but
    // must be identical in the vertex and fragment copies or the two height fields would
    // disagree in a way that looks like noise.
    float3 p3 = fract2(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract2((p3.x + p3.y) * p3.z);
}

// softsign: a smooth saturating activation. Chosen over a ReLU because it is
// differentiable everywhere, which matters for the gradient normal computed from
// finite differences in the fragment stage.
float wl_vsoftsign(float x) { return x / (1.0 + abs(x)); }

// The vertex-stage twin of wl_neural_tex_detail. Identical weights to the fragment
// version below, so the two agree -- this is why the numbers are duplicated rather
// than shared.
float3 wl_vneural_tex_detail(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    // Four hidden units, then a weighted combination. Weights are hand-chosen constants
    // rather than trained: there are no learned parameters in this shader, so neural
    // describes the SHAPE (an MLP evaluated in-shader), not a model.
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    // The grain term, quantised to a 64x64 grid in UV space. This is the term the
    // fragment stage drops for geometry (see wl_neural_micro), because per-cell hash
    // grain at vertex density produces visible vertex-scale artefacts.
    float grain = wl_vhash12(floor(uv * 64.0)) - 0.5;
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    // The output is a MULTIPLIER around 1.0 (tint * d), not an absolute colour. That is
    // what lets the strength uniform fade the whole term to a no-op by lerping toward
    // 1.0 rather than by branching.
    float3 tint = float3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

// The smooth-only neural micro. Identical to the fragment version except that it
// omits the hash grain, for the reason given at the grain term above.
float wl_vneural_micro(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_vsoftsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_vsoftsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_vsoftsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_vsoftsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    return 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3;
}

// A 3D hash for the value noise below. Separate from the 2D hash because value noise
// needs lattice hashing in three dimensions.
float wl_vvol_hash(float3 p)
{
    p = fract2(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract2((p.x + p.y) * p.z);
}

// Trilinear value noise: 8 lattice hashes interpolated. The smoothstep on the
// fractional part below is what makes the derivative continuous, so the gradient
// normal derived from this height field does not show lattice creases.
float wl_vvol_noise(float3 p)
{
    float3 i = floor(p);
    float3 f = fract2(p);
    // u = f*f*(3-2f), the quintic-free smoothstep. Cheap and C1-continuous, which is all
    // the finite-difference normal below requires.
    float3 u = f * f * (3.0 - 2.0 * f);
    return lerp_safe(lerp_safe(lerp_safe(wl_vvol_hash(i), wl_vvol_hash(i + float3(1.0, 0.0, 0.0)), u.x),
                   lerp_safe(wl_vvol_hash(i + float3(0.0, 1.0, 0.0)), wl_vvol_hash(i + float3(1.0, 1.0, 1.0)), u.x), u.y),
               lerp_safe(lerp_safe(wl_vvol_hash(i + float3(0.0, 0.0, 1.0)), wl_vvol_hash(i + float3(1.0, 0.0, 1.0)), u.x),
                   lerp_safe(wl_vvol_hash(i + float3(0.0, 1.0, 1.0)), wl_vvol_hash(i + float3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

// The 32-case texture switch. FXC requires LITERAL indices into a texture array
// (X3512), so a dynamic selector will not compile -- hence the unrolled switch rather
// than an array subscript. Duplicated in the pixel stage for the same reason.
float4 wl_vtex_slot(int slot, float2 uv)
{
    switch (slot)
    {
        case 0:  return TEX(u_Textures[0], uv);
        case 1:  return TEX(u_Textures[1], uv);
        case 2:  return TEX(u_Textures[2], uv);
        case 3:  return TEX(u_Textures[3], uv);
        case 4:  return TEX(u_Textures[4], uv);
        case 5:  return TEX(u_Textures[5], uv);
        case 6:  return TEX(u_Textures[6], uv);
        case 7:  return TEX(u_Textures[7], uv);
        case 8:  return TEX(u_Textures[8], uv);
        case 9:  return TEX(u_Textures[9], uv);
        case 10: return TEX(u_Textures[10], uv);
        case 11: return TEX(u_Textures[11], uv);
        case 12: return TEX(u_Textures[12], uv);
        case 13: return TEX(u_Textures[13], uv);
        case 14: return TEX(u_Textures[14], uv);
        case 15: return TEX(u_Textures[15], uv);
        case 16: return TEX(u_Textures[16], uv);
        case 17: return TEX(u_Textures[17], uv);
        case 18: return TEX(u_Textures[18], uv);
        case 19: return TEX(u_Textures[19], uv);
        case 20: return TEX(u_Textures[20], uv);
        case 21: return TEX(u_Textures[21], uv);
        case 22: return TEX(u_Textures[22], uv);
        case 23: return TEX(u_Textures[23], uv);
        case 24: return TEX(u_Textures[24], uv);
        case 25: return TEX(u_Textures[25], uv);
        case 26: return TEX(u_Textures[26], uv);
        case 27: return TEX(u_Textures[27], uv);
        case 28: return TEX(u_Textures[28], uv);
        case 29: return TEX(u_Textures[29], uv);
        case 30: return TEX(u_Textures[30], uv);
        case 31: return TEX(u_Textures[31], uv);
        // The default arm returns white rather than an error. In the vertex stage a bad slot
        // means a displaced vertex, and white (luminance 1) displaces it upward by the full
        // amount -- visible, but not a crash.
        default: return float4(1.0, 1.0, 1.0, 1.0);
    }
}

// The vertex height field: texture luminance + the macro noise and strata terms.
// Deliberately a SEPARATE function from wl_relief_height below: one program
// cannot hold two functions of the same name, so the vertex-stage helpers are
// v-prefixed twins. The two height fields must stay in agreement or the
// displaced geometry will not line up with the shaded relief.
float wl_vheight(int slot, float2 uv, float3 wp, float3 n, float disp)
{
    // Rec.601 luma weights. Luminance from RGB rather than from the alpha channel,
    // so a height map with alpha detail still produces a usable relief height.
    float h = dot(wl_vtex_slot(slot, uv).rgb, float3(0.299, 0.587, 0.114));
    // Neural micro-surface, from the b0 copy of the uniforms. This is the term
    // that was previously missing here, which made vertex displacement disagree
    // with the fragment relief height (b1's copy feeds wl_relief_height) and so
    // made displaced geometry differ from the GLSL. Now byte-identical to
    // Renderer3D_Basic.glsl's vertex height, which gates on the same two tests.
    if (u_NeuralEnabled == 1 && u_NeuralTexStrength > 0.001)
        h += wl_vneural_micro(uv, n) * 2.0 * clamp(u_NeuralTexStrength, 0.0, 1.0);
    // The displacement amplitude, capped at 1.5. The cap bounds how far a vertex can move
    // from its position, so a bright texel cannot fling geometry across the scene.
    float ma = 0.25 * clamp(disp, 0.0, 1.5);
    // The macro-detail branch. Gated on the amplitude rather than compiled out, so a
    // material that opts out pays a branch and four ALU ops -- cheap, and it keeps this
    // shader to one variant for every material.
    if (ma > 0.001)
    {
        float2 muv = uv * 6.0;
        float n1 = wl_vvol_noise(float3(muv, 3.7));
        float n2 = wl_vvol_noise(float3(muv * 2.3 + 5.0, 9.1));
        // Strata banding driven by WORLD height rather than UV, so the bands stay horizontal
        // on a surface at any orientation -- which is what makes them read as geology rather
        // than as a texture.
        float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
        // The strata weight is damped on up-facing surfaces: sin() of a near-vertical world
        // height produces bands that swim as the surface normal changes, so the 0.15 floor
        // keeps a little banding rather than none.
        float sw = clamp(1.0 - abs(n.y), 0.15, 1.0);
        // The noise is centred on zero by subtracting 0.5, so the result displaces up and down
        // around the texture height instead of only downward from it.
        h += ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * ma
           // The strata term, scaled by 0.8 -- weaker than the noise, because bands read as an
           // overlay while noise reads as surface.
           + (st - 0.5) * ma * 0.8 * sw;
    }
    return clamp(h, 0.0, 1.5);
}

// The vertex entry point. Attributes are named a_* to distinguish them from the v_*
// varyings the fragment stage consumes.
VSOutput main(float3 a_Position  : POSITION,
              // a_Color as float4 : COLOR. Alpha carries through to the fragment stage untouched,
              // which is why the shader never applies an opacity term of its own.
              float4 a_Color     : COLOR,
              // a_Normal as float3. Object- or world-space is decided by the matrix the VS applies
              // below; the interpolation is linear, which is correct for a normal under an affine
              // transform and only approximate under perspective.
              float3 a_Normal    : NORMAL,
              // a_TexCoord as float2, and the tiling factor is a SEPARATE attribute rather than
              // being folded in on the CPU -- so a change to tiling does not require rewriting UVs.
              float2 a_TexCoord  : TEXCOORD,
              // A float, cast to int at each use site. It has to be interpolatable to reach the
              // fragment stage, and the two casts below are the cost of that.
              float  a_TexIndex  : TEXINDEX,
              float  a_TilingFactor : TILINGFACTOR,
              int    a_EntityID  : ENTITYID,
              // a_Metallic and a_Roughness as bare floats, clamped at the use site rather than
              // here. A material can therefore ship an out-of-range value and get clamped
              // differently by the classical and neural paths.
              float  a_Metallic  : METALLIC,
              float  a_Roughness : ROUGHNESS,
              // a_Displacement drives VERTEX displacement; a_BumpStrength drives FRAGMENT relief.
              // Two attributes where one could do, because they are independently authored: a
              // material can have a displaced silhouette with a flat surface.
              float  a_Displacement : DISPLACEMENT,
              float  a_BumpStrength : BUMPSTRENGTH)
{
    // Default-constructed rather than zero-initialised. Every field is assigned below
    // before use, but the compiler is free to keep the struct in an undefined state
    // between assignment and read if a field were ever missed.
    VSOutput Output;
    // The vertex colour, forwarded unmodified. There is no tint or modulation applied
    // in the VS -- the fragment stage owns all colour work.
    Output.Color = a_Color;
    Output.Normal = a_Normal;
    // TexCoord forwarded WITHOUT the tiling factor, which is passed separately and
    // applied at each sample site. That is why reliefUV below can be re-tiled per use
    // without touching the varying.
    Output.TexCoord = a_TexCoord;
    Output.TexIndex = a_TexIndex;
    Output.TilingFactor = a_TilingFactor;
    // The comment above is a deliberate design statement: displacement is NOT faded with
    // distance. Geometry must not swim while fragment detail fades, so the vertex stage
    // opts out of the distance fade the fragment stage applies.
    // Neural geometric displacement: real vertices move along the (world)
    // normal by the neural height field. Unfaded by design — geometry must
    // not swim while fragment shading detail fades with distance.
    // vPos rather than reusing a_Position, because the displacement branch may move it.
    // Writing back into the attribute parameter would be legal but would make the
    // unmodified case harder to see.
    float3 vPos = a_Position;
    if (a_Displacement > 0.001)
    {
        // normalize() before the height lookup, so the function receives a unit normal. The
        // height field uses the normal only for the neural micro term's orientation, so this
        // is about a well-defined input rather than a visible effect.
        float3 vN = normalize(a_Normal);
        // One height sample per vertex. This is the whole vertex cost of relief: the fragment
        // stage will take dozens more, which is the asymmetry behind the two-stage design.
        float vh = wl_vheight((int)(a_TexIndex), a_TexCoord * a_TilingFactor, a_Position, vN, a_Displacement);
        // Displacement along the normal, scaled so the mid-level (h == 0.5) is the
        // undisplaced surface -- the shape-preserving construction the fragment stage mirrors
        // for its depth push.
        vPos = a_Position + vN * (vh - 0.5) * a_Displacement;
    }
    // The displacement block closes here. The vPos declared above is reassigned inside
    // it, so the transform below sees the DISPLACED position -- see the note at line 237.
    // WorldPos is the DISPLACED position, not the original. Every fragment-stage use
    // (distance fade, tangent frame, depth reprojection) therefore works in pushed
    // space, which is what keeps relief consistent under parallax.
    Output.WorldPos = vPos;
    Output.Metallic = a_Metallic;
    Output.Roughness = a_Roughness;
    // Displacement and BumpStrength forwarded so the fragment gate at line 847 can test
    // them per fragment rather than baking the decision into the material.
    Output.Displacement = a_Displacement;
    Output.BumpStrength = a_BumpStrength;
    // The entity id forwarded with the nointerpolation semantics declared in VSOutput.
    // It is an attribute, not a varying, so it never needs renormalising.
    Output.v_EntityID = a_EntityID;
    // Transform AFTER displacement, so the pushed vertex lands in the correct place in
    // clip space. Transforming first and then offsetting would displace along the
    // view-space axis instead of the surface normal.
    Output.Position = mul(u_ViewProjection, float4(vPos, 1.0f));
    // Pass the VP matrix rows so the PS can reproject the per-fragment
    // relief push exactly (gl_FragDepth parity).
    // The four VP rows forwarded to the PS. See the two-cbuffer note above: this is the
    // mechanism that lets the fragment stage reach a uniform it cannot bind.
    Output.u_VPRow0 = u_ViewProjection[0];
    // Row 1 of the VP matrix, read as a ROW into u_ViewProjection[row]. The float4 type is
    // what the varying carries; the four rows are reassembled into a float4x4 in the
    // fragment stage.
    Output.u_VPRow1 = u_ViewProjection[1];
    Output.u_VPRow2 = u_ViewProjection[2];
    Output.u_VPRow3 = u_ViewProjection[3];
    return Output;
}

// The pixel stage. Everything above is the vertex shader; the loader splits here on
// this directive.
#type pixel

// SHARED DECLARATIONS, repeated verbatim rather than #included. The loader
// concatenates stages into one program, and a duplicate declaration would be a
// redefinition error -- so the repetition is structural, not carelessness.
// ============================================================================
// SHARED DECLARATIONS (vertex + pixel stages)
// ============================================================================

// The same 32-slot array, rebound in the pixel stage. NVRHI binds resources per stage,
// so declaring it once is not an option.
Texture2D    u_Textures[32] : register(t0);
SamplerState u_Sampler      : register(s0);

// ---- GLSL-semantics helpers ----
// Four overloads of fract2 so a call site works whatever the operand width. The
// scalar one is the only one used directly; the others are called implicitly by the
// vector expressions throughout.
float  fract2(float x)  { return x - floor(x); }
float2 fract2(float2 x) { return x - floor(x); }
float3 fract2(float3 x) { return x - floor(x); }
float4 fract2(float4 x) { return x - floor(x); }
float  lerp_safe(float a, float b, float t)    { return lerp(a, b, t); }
float2 lerp_safe(float2 a, float2 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float t)  { return lerp(a, b, t); }
float4 lerp_safe(float4 a, float4 b, float t)  { return lerp(a, b, t); }
float3 lerp_safe(float3 a, float3 b, float3 t) { return lerp(a, b, t); }

// TEX/TEX_LOD macros are defined per-stage below (Sample vs SampleLevel).

// TEX now uses Sample() rather than SampleLevel(): the pixel stage has valid
// derivatives, so implicit-LOD selection gives the mip behaviour GL's texture() call
// would produce on the same geometry.
#define TEX(tex, uv)        (tex).Sample(u_Sampler, (uv))
#define TEX_LOD(tex, uv, l) (tex).SampleLevel(u_Sampler, (uv), (l))

// THREE render targets, and the third is the reason for most of the complexity below:
// SV_Depth lets the fragment stage commit a parallax-displaced depth, so relief
// self-occludes and intersects other geometry like real polygons.
struct PSOutput
{
    // SV_Target0, the colour target. Named with the SV_ prefix so the compiler binds it
    // to render target 0 regardless of struct field order.
    float4 Color   : SV_Target0;
    // SV_Target1 as an int. See the VSOutput note: an integer target is what makes
    // read-back exact for ids above the range a float colour channel could carry.
    int EntityID   : SV_Target1;
    // SV_Depth as a float, in the same [0,1] convention the rasteriser produces after its
    // viewport transform -- which is why the write below applies the *0.5+0.5 remap
    // rather than using raw clip-space z.
    float Depth    : SV_Depth;
};

struct PSInput
{
    // SV_Position is supplied by the rasteriser, not by the VS. It arrives as clip space
    // transformed to screen coordinates -- which is why the depth reprojection at line 957
    // goes the other way and rebuilds clip space.
    float4 Position  : SV_Position;
    float4 Color     : COLOR0;
    float3 Normal    : NORMAL0;
    float2 TexCoord  : TEXCOORD0;
    float  TexIndex  : TEXCOORD1;
    float  TilingFactor : TEXCOORD2;
    // WorldPos as a varying rather than recomputed in the PS from clip space. Passing it
    // costs interpolator space but saves a matrix multiply per fragment, and it is what
    // the distance fade and tangent frame both consume.
    float3 WorldPos  : TEXCOORD3;
    float  Metallic  : TEXCOORD4;
    float  Roughness : TEXCOORD5;
    float  Displacement : TEXCOORD6;
    float  BumpStrength : TEXCOORD7;
    // TEXCOORD8 onwards are consumed entirely by the nointerpolation rows that follow.
    // Starting at 8 is not arbitrary: 0-7 are taken by the interpolated per-pixel values.
    nointerpolation int    v_EntityID : TEXCOORD8;
    nointerpolation float4 u_VPRow0 : TEXCOORD9;
    nointerpolation float4 u_VPRow1 : TEXCOORD10;
    nointerpolation float4 u_VPRow2 : TEXCOORD11;
    // The last varying slot. Thirteen TEXCOORDs is close to the SM5 limit of 16, so
    // adding a per-pixel quantity to this shader means repacking, not adding.
    nointerpolation float4 u_VPRow3 : TEXCOORD12;
};

// THE PS cBUFFER, and every uniform the fragment stage needs. The comment blocks below
// group them by feature and record the packing of the vec4 data arrays, which is
// otherwise unreadable -- the fog and cloud data are opaque float4s whose component
// meanings are only documented in prose.
cbuffer BasicConstants : register(b1)
{
    // Camera + sky (view-dependent metal reflection)
    float3  u_CameraPosition;
    float3  u_SkyBottomColor;
    float3  u_SkyTopColor;
    // Neural rendering
    int     u_NeuralEnabled;
    float   u_NeuralTexStrength;
    float   u_NeuralLightStrength;
    float   u_NeuralMatStrength;
    // Volumetric atmosphere
    float3  u_SunDirection;
    float   u_Time;
    int     u_FogEnabled;
    float3  u_SunLightColor;
    int     u_FogCount;
    // Fog volumes as fixed-size arrays of 4, and clouds as arrays of 2. The counts are
    // separate uniforms (u_FogCount, u_CloudCount), so the array size is a hard ceiling on
    // volumes rather than a count the caller can grow.
    float3  u_FogMin[4];
    float3  u_FogMax[4];
    float3  u_FogColor[4];
    float4  u_FogData[4];
    float4  u_FogData2[4];
    int     u_CloudEnabled;
    int     u_CloudCount;
    float3  u_CloudMin[2];
    float3  u_CloudMax[2];
    float3  u_CloudColor[2];
    float3  u_CloudAmbient[2];
    float4  u_CloudData[2];
    float4  u_CloudData2[2];
    float4  u_CloudData3[2];
    // u_VolFast selects the 2-octave noise instead of the 4-octave one, for the Low and
    // Medium quality presets. It is an int rather than a bool because it shares an upload
    // block with other values and arrives as one.
    int     u_VolFast;
    // Analytic component lights
    int     u_ALightCount;
    float3  u_ALightPos[32];
    float3  u_ALightDir[32];
    float3  u_ALightColor[32];
    float4  u_ALightData[32];
    float4  u_ALightData2[32];
};
// Entity ID attachment texture slot

// [earlydepthstencil] is on the entry point below, and this is why: relief writes
// SV_Depth for SOME fragments only. Without the attribute, writing depth anywhere in
// the shader would force late-z for every fragment, including the overwhelming
// majority that never displace.
// Relief writes gl_FragDepth only for displaced fragments; this keeps the
// early-z cull for everything else instead of forcing late-z program-wide.


// Note the orphaned comment blocks that follow. They document uniform groups -- the
// VP rows, the neural block, the fog data layout, the god-ray colour, the component
// lights -- but they sit BELOW the cbuffer they describe and above the code that uses
// them, with groups of blank lines where uniform declarations used to be.
// DEFECT (documentation placement, cosmetic): these read as if they belong to
// whatever declaration happens to follow, and several have no declaration at all. The
// information is still accurate and still useful; it is just orphaned. Not fixed;
// comments only.
// Shared with the vertex stage (uploaded once per frame by Renderer3D):
// reprojects the relief-pushed position for the gl_FragDepth write.


// Camera + sky (uploaded by Renderer3D): view-dependent metal reflection.




// The neural block's own comment is accurate about the GLSL original but sits in an
// empty region -- see the note above. Defaults are 0 (off) until Renderer3D uploads
// them, so a frame that never flushes renders classically.
// Neural Rendering (OpenGL): tiny in-shader MLP for texture detail + learned ambient.
// Driven by Renderer3D neural uniforms; defaults off (0) until Flush() uploads them.



// Neural material blend. Defaults to 0 (classic response) until Renderer3D uploads it.


// The fog/cloud data layout, documented as prose because the values are unpacked
// component-wise at the use sites and would otherwise be unreadable. Fog d1/d2 and
// cloud d1/d2/d3 map directly onto the float4 members declared in the cbuffer above.
// Volumetric atmosphere (Renderer3D uploads; locations are skipped when absent).
// Fog d1 = (density, anisotropy, noiseStrength, noiseScale)
// Fog d2 = (windSpeed, heightFalloff, steps, enabled)
// Cloud d1 = (coverage, density, noiseScale, detail)
// Cloud d2 = (windSpeed, windDirX, windDirY, steps)
// Cloud d3 = (silverLining, shadowStrength, enabled, unused)



// The god-ray sun colour is a direction-light colour premultiplied by intensity, or a
// warm default when no sun exists -- so volumetrics still light correctly indoors.
// God-ray sun color (directional light color x intensity, or warm default).
















// 1 = reduced noise octaves (Low/Medium quality presets).


// Component lights. Locations of -1 are skipped, which is how an absent light is
// distinguished from a black one: a zero colour would darken rather than disable.
// Analytic component lights (Renderer3D uploads; -1 locations skipped).
// d1 = (type 0=dir 1=point 2=spot 3=area, intensity, range 0=inf, falloff)
// d2 = (spot innerCos, spot outerCos, areaSize, areaDoubleSided)







// The pixel-stage softsign, identical to wl_vsoftsign in the vertex stage. Duplicated
// for the reason given at line 81: one program, no duplicate symbols.
float wl_softsign(float x) { return x / (1.0 + abs(x)); }

// wl_hash12: a 2D hash to [0,1). The xyx swizzle repeats x into all three components
// so one line handles both the 2D and the 3D variant used by the volumetrics.
float wl_hash12(float2 p)
{
    float3 p3 = fract2(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract2((p3.x + p3.y) * p3.z);
}

// Output ~= 1.0 (neutral) is the load-bearing property: it is what allows
// u_NeuralTexStrength to fade the term by lerping toward 1.0 instead of branching,
// and it is why the tint can be a separate multiply.
// 4-feature sinusoidal encoding -> 4-unit hidden layer -> detail multiplier + tint.
// Output ~= 1.0 (neutral); keeps energy stable when strength is 0.
// The fragment twin of the vertex detail function. The weights MUST match the version
// at line 94 exactly or geometry and shading would disagree in a way that reads as
// texture swimming.
float3 wl_neural_tex_detail(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_softsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_softsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_softsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_softsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    // The grain is quantised to a 64x64 UV grid, so it is a per-cell CONSTANT rather than
    // noise. At vertex density that reads as faceting, which is why wl_neural_micro
    // omits it.
    float grain = wl_hash12(floor(uv * 64.0)) - 0.5;
    float d = 1.0 + 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3 + 0.06 * grain;
    float3 tint = float3(1.0 + 0.03 * h1, 1.0 + 0.02 * h2, 1.0 - 0.03 * h0);
    return tint * d;
}

// Smooth-only, no per-cell hash grain -- the one substantive difference from
// wl_neural_tex_detail, and the reason this exists as a separate function.
// Smooth-only neural micro for GEOMETRY (no per-cell hash grain).
float wl_neural_micro(float2 uv, float3 n)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float s2 = uv.x * 13.7 - uv.y * 11.3;
    float4 feat = float4(sin(s1 + n.x * 3.1),
                     sin(s1 * 1.7 + n.y * 2.3),
                     sin(s2 + n.z * 3.7),
                     sin(s2 * 1.3 + s1 * 0.7));
    float h0 = wl_softsign( 0.9 * feat.x - 0.7 * feat.y + 0.5 * feat.z - 0.3 * feat.w + 0.15);
    float h1 = wl_softsign(-0.6 * feat.x + 0.8 * feat.y - 0.4 * feat.z + 0.6 * feat.w - 0.10);
    float h2 = wl_softsign( 0.4 * feat.x + 0.5 * feat.y + 0.9 * feat.z + 0.2 * feat.w + 0.05);
    float h3 = wl_softsign(-0.3 * feat.x - 0.5 * feat.y + 0.6 * feat.z - 0.8 * feat.w + 0.20);
    return 0.10 * h0 + 0.07 * h1 - 0.06 * h2 + 0.05 * h3;
}

// Learned ambient: a sky gradient plus an albedo-tinted bounce lobe. There are no
// weights here, so learned describes the intent of the model, not a trained one -- the
// same caveat as the detail function above.
// Learned hemisphere ambient: sky gradient + albedo-tinted bounce lobe.
// The ambient takes albedo as an input and returns a tint to multiply BY the albedo
// at the call site. Returning irradiance rather than a final colour keeps this usable
// for both the neural and classical paths.
float3 wl_neural_ambient(float3 n, float3 albedo)
{
    // up remapped from [-1,1] to [0,1] so it can drive a lerp_safe t directly, with no
    // clamp needed.
    float up = n.y * 0.5 + 0.5;
    float3 skyRef = lerp_safe(float3(0.32, 0.30, 0.34), float3(0.55, 0.62, 0.78), up);
    // The bounce lobe is oriented along a FIXED horizontal direction (0.8, -0.6) rather
    // than along any scene light -- a constant, so ambient does not change when the sun
    // does.
    float h = wl_softsign(dot(n.xz, float2(0.8, -0.6)) * 1.5 + n.y * 1.2);
    // The bounce term is albedo-tinted, which is what makes a red object bounce red into
    // its surroundings. It is the cheapest approximation of one GI bounce.
    float3 bounce = albedo * (0.35 + 0.25 * h) * skyRef;
    return skyRef * 0.35 + bounce;
}

// The header comment above states the raster/ray-trace difference honestly: same idea,
// minus world position. Worth keeping, because the two paths are compared when tuning
// and the divergence is expected.
// Neural material (raster): same learned PBR idea as the ray-trace path, minus
// world position — porosity/patina modulate albedo, the MLP gates sparse
// metallic-flake glints, and wrap width softens the diffuse response.
// inout/out parameters rather than a struct return, so the caller can pass existing
// variables in place. albedo is modified; glint and wrapW are new.
void wl_neural_mat_raster(float2 uv, float3 n, inout float3 albedo, out float glint, out float wrapW)
{
    float s1 = uv.x * 6.1 + uv.y * 7.7;
    float4 feat = float4(sin(s1 + n.x * 2.0),
                     sin(s1 * 1.6 + n.y * 1.7),
                     sin(s1 * 0.7 + n.z * 2.3),
                     sin((s1 * 0.6 + n.x - n.y) * 1.3));
    float h0 = wl_softsign( 0.8 * feat.x - 0.5 * feat.y + 0.6 * feat.z + 0.2 * feat.w + 0.05);
    float h1 = wl_softsign(-0.4 * feat.x + 0.9 * feat.y - 0.2 * feat.z + 0.5 * feat.w - 0.10);
    float h2 = wl_softsign( 0.3 * feat.x + 0.2 * feat.y + 0.7 * feat.z - 0.6 * feat.w + 0.35);
    float h3 = wl_softsign(-0.5 * feat.x - 0.4 * feat.y + 0.4 * feat.z + 0.8 * feat.w + 0.00);
    // Porosity is taken from the NEGATIVE half of h0, so the softsign output is reused
    // rather than a second network being evaluated.
    float por = clamp(-h0, 0.0, 1.0);
    // Albedo is darkened by up to 25% in the pits. Multiplicative rather than a lerp
    // toward black, so a dark albedo stays dark instead of gaining grey.
    albedo *= (1.0 - 0.25 * por);
    albedo *= (float3(1.0, 1.0, 1.0) + float3(-0.06, 0.02, 0.05) * h1);
    // step() rather than a smooth threshold: flake cells are meant to be binary sparkle,
    // and a smooth edge would blur into the albedo instead of reading as discrete flakes.
    float flakeCell = step(0.75, wl_hash12(floor(uv * 48.0)));
    // The glint is the cell mask AND the network output, so a flake only sparkles where
    // the MLP says the material has flakes -- the mask alone would put them everywhere.
    glint = flakeCell * clamp(h2 * 1.5, 0.0, 1.0);
    wrapW = 0.35 + 0.65 * clamp(h3 * 0.5 + 0.5, 0.0, 1.0);
}

// The volumetric section. Fog and clouds share a marching structure -- same box
// intersection, same accumulation loop shape -- which is why they are adjacent.
// ---- Volumetric fog & clouds (box volumes submitted by Renderer3D) ----
// wl_vol_hash: the 3D variant, with its own constants (0.1031, 31.32) from the vertex
// twin. The values differ from wl_hash12 (0.1031, 33.33) and are not required to
// match -- the two hash different spaces.
float wl_vol_hash(float3 p)
{
    p = fract2(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract2((p.x + p.y) * p.z);
}

// The volumetric noise. Identical in form to the vertex twin at line 134; the noise
// field does not need to agree between stages because nothing displaces with it.
float wl_vol_noise(float3 p)
{
    float3 i = floor(p);
    float3 f = fract2(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    return lerp_safe(lerp_safe(lerp_safe(wl_vol_hash(i), wl_vol_hash(i + float3(1.0, 0.0, 0.0)), u.x),
                   lerp_safe(wl_vol_hash(i + float3(0.0, 1.0, 0.0)), wl_vol_hash(i + float3(1.0, 1.0, 0.0)), u.x), u.y),
               lerp_safe(lerp_safe(wl_vol_hash(i + float3(0.0, 0.0, 1.0)), wl_vol_hash(i + float3(1.0, 0.0, 1.0)), u.x),
                   lerp_safe(wl_vol_hash(i + float3(0.0, 1.0, 1.0)), wl_vol_hash(i + float3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

// 4-octave fBm with a lacunarity of 2.03 rather than 2.0. The offset keeps successive
// octaves from sampling the same lattice points, which 2.0 would do at integer
// coordinates.
float wl_vol_fbm(float3 p)
{
    // The fBm accumulator and amplitude. Amplitude starts at 0.5 and halves per octave, so
    // the sum converges to about 1.0 -- which is what the rescale at line 514 matches.
    float v = 0.0;
    float a = 0.5;
    for (int o = 0; o < 4; o++)
    {
        // Each octave contributes amplitude times noise. Because the noise is in [0,1], the
        // sum is in [0, sum(amplitudes)] -- so the output is never negative and needs no bias.
        v += a * wl_vol_noise(p);
        // The octave offset 17.3 is added to ALL three components so successive octaves sample
        // a decorrelated region. Using the offset on only one axis would keep the octaves
        // correlated in the others.
        p = p * 2.03 + float3(17.3, 17.3, 17.3);
        a *= 0.5;
    }
    // The unclamped sum. Nothing normalises it -- see the note at line 514 for where the
    // mean-matching happens instead.
    return v;
}

// 2-octave variant for low quality presets / cheap taps. Rescaled to match
// wl_vol_fbm's mean so coverage thresholds behave the same.
// The 2-octave variant, rescaled by 1.25 to MATCH THE 4-OCTAVE MEAN. Without that
// factor, switching quality presets would change fog density as well as fog detail --
// the coverage thresholds below would all need retuning per preset.
float wl_vol_fbm2(float3 p)
{
    // The 2-octave version in one expression, with the 1.25 rescale. Written as a single
    // return so there is no loop: the iteration count is fixed at 2 and the quality
    // setting picks between this and the loop version.
    return (0.5 * wl_vol_noise(p) + 0.25 * wl_vol_noise(p * 2.03 + float3(17.3, 17.3, 17.3))) * 1.25;
}

// Slab-method ray/box intersection. The safe line avoids a division by zero when a
// ray component is exactly 0 -- the case for a ray travelling exactly along an axis,
// which is common for a camera-aligned view direction.
float2 wl_box_range(float3 ro, float3 rd, float3 bmin, float3 bmax)
{
    // step() adds 1e-5 ONLY where the component is below the epsilon, so a near-zero
    // component is nudged rather than replaced. Nudging a direction component changes the
    // ray by a negligible amount but keeps the reciprocal finite.
    float3 safe = rd + (float3(1.0, 1.0, 1.0) - step(float3(0.00001, 0.00001, 0.00001), abs(rd))) * 0.00001;
    // The reciprocal of the nudged direction, computed once rather than three times.
    float3 inv = 1.0 / safe;
    // t0 and t1 are the near and far intersections per axis, which are NOT ordered yet --
    // a negative direction component swaps them, which is what min/max on the next two
    // lines resolve.
    float3 t0 = (bmin - ro) * inv;
    float3 t1 = (bmax - ro) * inv;
    float3 tmin = min(t0, t1);
    float3 tmax = max(t0, t1);
    // The result is (tEnter, tExit). Taking the max of the near set and the min of the far
    // set is the slab method: if tEnter exceeds tExit the ray misses, which the caller
    // tests with t1 <= t0.
    return float2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

// Henyey-Greenstein phase function. 0.07957747 is 1/(4*pi) -- the normalising constant
// for a spherical scattering phase function. The pow() argument is clamped so a
// forward-scattering peak cannot produce inf.
float wl_hg(float cosT, float g)
{
    // g squared, hoisted so the expression below reads clearly. g is clamped by the
    // caller, so this is safe for any input in [-0.9, 0.9].
    float g2 = g * g;
    return 0.07957747 * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosT, 0.001), 1.5);
}

// The fog marcher. Returns rgb = accumulated in-scattering, a = transmittance, so the
// caller composites with col * r.a + r.rgb -- the standard front-to-back accumulation
// order.
float4 wl_march_fog_vol(float3 ro, float3 rd, float tMax, float3 bmin, float3 bmax,
                      float3 tint, float4 d1, float4 d2, float dither, float sunVis)
{
    float2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    // Depth-adaptive steps: thin intersections cost proportionally less while
    // keeping an approximately constant world-space step size.
    // The length the march will actually cover. max() guards a missed box, where the
    // slab method returns tEnter > tExit and the difference is negative.
    float fogLen = max(t1 - t0, 0.0);
    // DEPTH-ADAPTIVE STEP COUNT: a thin box intersection gets proportionally fewer steps,
    // keeping the world-space step size roughly constant. A fixed count would either band
    // in a thin volume or waste taps in a deep one.
    int steps = (int)(clamp(d2.z * clamp(fogLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / (float)(steps);
    // The phase term, with the anisotropy clamped to [-0.9, 0.9] because the denominator
    // of a Henyey-Greenstein function approaches zero as g approaches 1.
    float phase = wl_hg(dot(rd, u_SunDirection), clamp(d1.y, -0.9, 0.9));
    // The ambient sky colour at the midpoint of the gradient. Using one colour for the
    // whole march rather than sampling per step is what keeps the fog march affordable.
    float3 ambCol = lerp_safe(u_SkyBottomColor, u_SkyTopColor, 0.5);
    // Light is resolved ONCE before the loop, not per step. The phase term is constant
    // along a straight ray, so hoisting it is exact rather than an approximation.
    float3 light = ambCol * 0.7 + u_SunLightColor * sunVis * (0.25 + phase * 2.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    float3 acc = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    // DITHER, not a fixed start: offsetting the first sample by a per-pixel random breaks
    // the banding that a constant start offset would produce as the camera moves. The
    // caller supplies a per-pixel hash, so the pattern is stable within a frame.
    float t = t0 + dt * dither;
    for (int s = 0; s < 16; s++)
    {
        if (s >= steps)
            break;
        float3 p = ro + rd * t;
        // Density read from d1.x and clamped. The clamp matters: a negative density would make
        // exp() grow instead of decay.
        float d = max(d1.x, 0.0);
        float h = clamp((p.y - bmin.y) / hgt, 0.0, 1.0);
        // Height falloff as an exponential, scaled by d2.y. Clamped to [0,1] so a falloff
        // value above 1 cannot invert the profile.
        d *= lerp_safe(1.0, exp(-h * 4.0), clamp(d2.y, 0.0, 1.0));
        if (d1.z > 0.001)
        {
            // The wind offset advances with u_Time and is applied to the NOISE COORDINATE, not to
            // the geometry -- so the fog appears to drift while the volume box stays put.
            float3 np = p * max(d1.w, 0.001) + float3(u_Time * d2.x, 0.0, u_Time * d2.x * 0.6);
            float n = (u_VolFast == 1) ? wl_vol_fbm2(np) : wl_vol_fbm(np);
            // Noise modulates density between 0.5-strength and full. The (1 - 0.5*strength)
            // baseline means raising the strength adds contrast without changing the mean.
            d *= (1.0 - d1.z * 0.5) + d1.z * n;
        }
        // The Beer-Lambert alpha for one step: 1 - exp(-density * dt). The exponential form is
        // what makes the accumulation independent of step count -- halving the steps halves dt
        // and the total transmittance is unchanged, which is the property that lets the step
        // count vary per pixel without changing the image.
        float a = 1.0 - exp(-max(d, 0.0) * dt);
        // Front-to-back accumulation: each step contributes its in-scattering multiplied by
        // the transmittance SO FAR. That ordering is what makes the composite
        // order-independent and step-count-independent.
        acc += trans * a * tint * light;
        trans *= (1.0 - a);
        // Early-out at 2% transmittance. Below that the remaining contribution is smaller than
        // 8-bit quantisation of the target, so continuing costs taps for no visible
        // difference.
        if (trans < 0.02)
        {
            trans = 0.0;
            break;
        }
        t += dt;
    }
    return float4(acc, trans);
}

// The cloud marcher. Same structure as the fog marcher, so the two are deliberately
// parallel; the differences are the coverage threshold, the self-shadow tap, and the
// silver lining term.
float4 wl_march_cloud_vol(float3 ro, float3 rd, float tMax, float3 bmin, float3 bmax,
                        // The cloud march signature. Three d* float4s against fog's two, because clouds need
                        // coverage, detail and shadow strength where fog needs only density and falloff.
                        float3 tint, float3 amb, float4 d1, float4 d2, float4 d3, float dither)
{
    float2 range = wl_box_range(ro, rd, bmin, bmax);
    float t0 = max(range.x, 0.0);
    float t1 = min(range.y, tMax);
    float clLen = max(t1 - t0, 0.0);
    // The cloud step count, from d2.w -- a different component than fog uses (d2.z), so
    // the two volumes do not have to agree on a step budget.
    int steps = (int)(clamp(d2.w * clamp(clLen / 8.0, 0.25, 1.0), 2.0, 16.0));
    if (t1 <= t0 || steps <= 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float dt = (t1 - t0) / (float)(steps);
    // Wind direction from two components of d2 rather than a packed value -- cheaper, and
    // it avoids unpacking.
    float2 wnd = float2(d2.y, d2.z);
    // A zero-length wind vector is replaced rather than normalised. normalize() of a zero
    // vector is NaN on most hardware, and a NaN here would poison the entire march.
    if (dot(wnd, wnd) < 0.00000001)
        wnd = float2(1.0, 0.0);
    // Wind is a POSITION offset scaled by time, not a velocity integrated per step. So
    // the drift rate is exactly windSpeed and does not depend on the step count.
    wnd = normalize(wnd) * d2.x * u_Time;
    // The silver lining: a 6th-power forward-scattering lobe toward the sun. The power is
    // high so the effect is confined to a narrow rim rather than washing the whole cloud
    // edge.
    float silverPow = pow(clamp(dot(rd, u_SunDirection), 0.0, 1.0), 6.0);
    // The cloud sun colour is a hardcoded warm white (1.2, 1.1, 1.0), unlike the fog
    // marcher which uses the cbuffer's sun colour. So cloud lighting and fog lighting can
    // disagree if a scene sets a coloured sun.
    float3 sunCol = float3(1.2, 1.1, 1.0);
    float hgt = max(bmax.y - bmin.y, 0.001);
    // The noise scale, floored at 0.001 -- the same floor as the other scale uniforms,
    // because a zero scale collapses all samples to one lattice point and the noise
    // becomes a constant.
    float nScale = max(d1.z, 0.001);
    float3 acc = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    float t = t0 + dt * dither;
    for (int s = 0; s < 16; s++)
    {
        if (s >= steps)
            break;
        float3 p = ro + rd * t;
        // The noise coordinate: world position scaled, then offset by wind. The wind offset is
        // a constant across the march, so it does not need recomputing per step.
        float3 q = p * nScale + float3(wnd.x, 0.0, wnd.y);
        float base = (u_VolFast == 1) ? wl_vol_fbm2(q) : wl_vol_fbm(q);
        // Coverage clamped to [0,1]. At 1.0 the smoothstep below starts at 0, so every
        // sample is cloud -- the density uniform then has nothing to modulate.
        float cov = clamp(d1.x, 0.0, 1.0);
        // The coverage threshold, with a fixed 0.35 width. That width is why the cloud edge is
        // soft; making it a uniform would need another cbuffer slot and was not done.
        float d = smoothstep(1.0 - cov, 1.0 - cov + 0.35, base);
        // The detail erosion, applied only when coverage is non-zero AND detail is enabled --
        // two conditions to keep the noise tap out of empty sky.
        if (d > 0.001 && d1.w > 0.001)
        {
            // The detail noise at 3.7x the base scale, animated on the Y axis only. Erosion (not
            // accumulation) is the standard choice -- it eats into the base shape rather than
            // adding floating detail.
            float det = wl_vol_noise(q * 3.7 + float3(0.0, u_Time * d2.x * 0.5, 0.0));
            // Erosion proportional to the current density, so it sculpts the cloud edge and
            // leaves the interior smooth. max(..., 0.0) keeps it non-negative.
            d = max(d - det * d1.w * d, 0.0);
        }
        d *= max(d1.y, 0.0);
        float h = clamp((p.y - bmin.y) / hgt, 0.0, 1.0);
        // The vertical profile: fade in over the bottom 25% and thin toward the top. The 0.7
        // at the top leaves some density rather than clipping to a flat lid.
        d *= smoothstep(0.0, 0.25, h) * (1.0 - smoothstep(0.6, 1.0, h) * 0.7);
        // The same Beer-Lambert alpha as the fog march. Sharing the formula is what makes the
        // two volumes behave identically under a step-count change.
        float a = 1.0 - exp(-d * dt);
        if (a > 0.001)
        {
            float sh = 1.0;
            // Self-shadowing is gated on d3.y > 0.01, so a volume with shadowStrength 0 pays
            // nothing for the shadow tap.
            if (d3.y > 0.01)
            {
                // The comment above is precise about the approximation: a SINGLE noise tap toward the
                // sun stands in for a second march. That is a real saving of the most expensive part of
                // a volumetric, and the result is a soft darkening rather than a correct shadow.
                // Cheap self-shadow: single-octave density tap toward the sun
                // (a full second march is overkill for a soft darkening term).
                // The shadow probe is two steps along the sun direction -- a fixed small offset rather
                // than a march, which is the entire cost saving of the approximation.
                float3 sp = p + u_SunDirection * dt * 2.0;
                float sd = wl_vol_noise(sp * nScale + float3(wnd.x, 0.0, wnd.y)) * max(d1.y, 0.0);
                // Transmittance through the shadow probe, exponential in the probed density. The 4.0
                // is a tuning constant that sets how dark a dense cloud can get.
                sh = exp(-sd * dt * 4.0 * clamp(d3.y, 0.0, 1.0));
            }
            // Cloud scattering: ambient scaled by the shadow term, plus direct sun scaled by it
            // too. Multiplying BOTH by sh is what makes a shadowed cloud go dark rather than
            // merely losing its highlight.
            float3 scol = tint * (amb * (0.35 + 0.65 * sh) + sunCol * sh * 0.9)
                      + sunCol * silverPow * clamp(d3.x, 0.0, 1.0) * 0.6;
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
    return float4(acc, trans);
}

// The compositor. Clouds are applied BEFORE fog, which is a layering choice: fog is
// nearer the camera and so composites over what is behind it.
float3 wl_apply_volumetrics(float3 ro, float3 rd, float tHit, float3 baseColor, float dither, float sunVis)
{
    // col starts as the surface colour and each volume composites over it in turn. Because
    // each step is col*transmittance + inscatter, the ORDER of the volumes matters -- see
    // the note at line 647.
    float3 col = baseColor;
    // The global enable is separate from the per-volume enabled flag below, so a scene can
    // disable volumetrics wholesale without uploading zeroed volume data.
    if (u_CloudEnabled == 1)
    {
        // A hard-coded loop bound of 2 matching the cbuffer array size. The count uniform
        // breaks out early, so an unfilled slot costs only a compare.
        for (int i = 0; i < 2; i++)
        {
            // Early break rather than a dynamic loop bound. On a GPU a dynamic trip count inhibits
            // unrolling, and these loops are small enough that the fixed bound wins.
            if (i >= u_CloudCount)
                break;
            // The per-volume enabled flag. A volume disabled but present in the array costs one
            // compare rather than a march.
            if (u_CloudData3[i].z < 0.5)
                continue;
            // The box bounds come straight from the cbuffer arrays, so the CPU chooses the volume
            // extent and the shader never derives it.
            float4 r = wl_march_cloud_vol(ro, rd, tHit, u_CloudMin[i], u_CloudMax[i],
                                        u_CloudColor[i], u_CloudAmbient[i],
                                        u_CloudData[i], u_CloudData2[i], u_CloudData3[i], dither);
            col = col * r.a + r.rgb;
        }
    }
    // Fog after clouds. Fog is the nearer medium, so compositing it last puts it in front
    // -- reversing these two blocks would visibly change the image.
    if (u_FogEnabled == 1)
    {
        for (int i = 0; i < 4; i++)
        {
            if (i >= u_FogCount)
                break;
            // The fog enabled flag is the W component of d2, which is the last field in the
            // documented layout. Kept last so the other three are usable without it.
            if (u_FogData2[i].w < 0.5)
                continue;
            float4 r = wl_march_fog_vol(ro, rd, tHit, u_FogMin[i], u_FogMax[i],
                                      u_FogColor[i], u_FogData[i], u_FogData2[i], dither, sunVis);
            col = col * r.a + r.rgb;
        }
    }
    // The composited colour, returned rather than written through an out-parameter. The
    // caller substitutes it for its own lit value.
    return col;
}

// THE RELIEF SECTION, and the comment above it is the honest summary: two triangles are
// made to read as millions of polys through a UV-space height march, a gradient normal,
// and a light march. Everything in this block serves those three steps.
// ---- Neural relief mapping (fragment-space POM) ----
// Height source: texture luminance + neural micro-relief from the existing
// detail MLP, so even flat albedo carries sub-texel geometry. Two triangles
// read as millions of polys: a UV-space height march (parallax), a
// derivative-scaled gradient normal, and a short light march for
// self-shadowing. Opt-in per material via Displacement / BumpStrength.
// SampleLevel(0) rather than Sample() for a compiler reason, not an artistic one:
// implicit derivatives are undefined in a loop whose iteration count varies per pixel,
// and FXC rejects that (X3570). This is a hard LOD-0 sample inside the march loop.
float4 wl_tex_slot(int slot, float2 uv)
{
    // SampleLevel(0) instead of Sample() to avoid implicit-gradient (ddx/ddy)
    // instructions inside the relief POM march loop, whose iteration count
    // varies per-pixel — that triggers X3570 on the gradient Sample path.
    switch (slot)
    {
        // The fragment-stage switch, all 32 arms SampleLevel(0). Identical in structure to
        // the vertex switch at line 145 except for the LOD -- see line 686 for why.
        case 0:  return u_Textures[0].SampleLevel(u_Sampler, uv, 0.0f);
        case 1:  return u_Textures[1].SampleLevel(u_Sampler, uv, 0.0f);
        case 2:  return u_Textures[2].SampleLevel(u_Sampler, uv, 0.0f);
        case 3:  return u_Textures[3].SampleLevel(u_Sampler, uv, 0.0f);
        case 4:  return u_Textures[4].SampleLevel(u_Sampler, uv, 0.0f);
        case 5:  return u_Textures[5].SampleLevel(u_Sampler, uv, 0.0f);
        case 6:  return u_Textures[6].SampleLevel(u_Sampler, uv, 0.0f);
        case 7:  return u_Textures[7].SampleLevel(u_Sampler, uv, 0.0f);
        case 8:  return u_Textures[8].SampleLevel(u_Sampler, uv, 0.0f);
        case 9:  return u_Textures[9].SampleLevel(u_Sampler, uv, 0.0f);
        case 10: return u_Textures[10].SampleLevel(u_Sampler, uv, 0.0f);
        case 11: return u_Textures[11].SampleLevel(u_Sampler, uv, 0.0f);
        case 12: return u_Textures[12].SampleLevel(u_Sampler, uv, 0.0f);
        case 13: return u_Textures[13].SampleLevel(u_Sampler, uv, 0.0f);
        case 14: return u_Textures[14].SampleLevel(u_Sampler, uv, 0.0f);
        case 15: return u_Textures[15].SampleLevel(u_Sampler, uv, 0.0f);
        case 16: return u_Textures[16].SampleLevel(u_Sampler, uv, 0.0f);
        case 17: return u_Textures[17].SampleLevel(u_Sampler, uv, 0.0f);
        case 18: return u_Textures[18].SampleLevel(u_Sampler, uv, 0.0f);
        case 19: return u_Textures[19].SampleLevel(u_Sampler, uv, 0.0f);
        case 20: return u_Textures[20].SampleLevel(u_Sampler, uv, 0.0f);
        case 21: return u_Textures[21].SampleLevel(u_Sampler, uv, 0.0f);
        case 22: return u_Textures[22].SampleLevel(u_Sampler, uv, 0.0f);
        case 23: return u_Textures[23].SampleLevel(u_Sampler, uv, 0.0f);
        case 24: return u_Textures[24].SampleLevel(u_Sampler, uv, 0.0f);
        case 25: return u_Textures[25].SampleLevel(u_Sampler, uv, 0.0f);
        case 26: return u_Textures[26].SampleLevel(u_Sampler, uv, 0.0f);
        case 27: return u_Textures[27].SampleLevel(u_Sampler, uv, 0.0f);
        case 28: return u_Textures[28].SampleLevel(u_Sampler, uv, 0.0f);
        case 29: return u_Textures[29].SampleLevel(u_Sampler, uv, 0.0f);
        case 30: return u_Textures[30].SampleLevel(u_Sampler, uv, 0.0f);
        case 31: return u_Textures[31].SampleLevel(u_Sampler, uv, 0.0f);
        // The default arm, and it returns white WITHOUT a break or fallthrough. Every other
        // arm returns, so this is only reached for slot < 0 or > 31.
        default: return float4(1.0, 1.0, 1.0, 1.0);
    }
}

// Added to final/gradient/shadow taps only (never the march) -- that constraint is what
// keeps the march stable. Adding high-frequency detail to the marched height would make
// the linear search overshoot and the surface flicker.
// Procedural craggy breakup + strata banding. Added to final/gradient/shadow
// taps only (never the march), so detail rides on the macro silhouette.
// Craggy breakup plus strata banding. amp gates the whole thing, so a material with no
// meso detail costs one branch.
float wl_relief_meso(float2 uv, float3 wp, float amp, float strataW)
{
    // amp <= 0.001 returns immediately: meso detail is opt-in per material, and this is
    // the gate that makes it free for materials that do not use it.
    if (amp <= 0.001)
        return 0.0;
    float2 muv = uv * 6.0;
    float n1 = wl_vol_noise(float3(muv, 3.7));
    float n2 = wl_vol_noise(float3(muv * 2.3 + 5.0, 9.1));
    // Strata as a sine of WORLD height plus a noise perturbation. Perturbing the phase
    // (rather than the value) is what makes the bands wander irregularly instead of
    // running as clean parallel lines.
    float st = sin(wp.y * 5.0 + n1 * 4.0) * 0.5 + 0.5;
    return ((n1 - 0.5) * 0.7 + (n2 - 0.5) * 0.3) * amp
         + (st - 0.5) * amp * 0.8 * strataW;
}

// The fragment height field: texture luminance + neural micro. This is the function the
// vertex stage CANNOT use, because the neural uniforms are pixel-stage uniforms -- the
// documented limitation, visible at the call site.
float wl_relief_height(int slot, float2 uv, float3 n, float neuralK)
{
    // The height sample, Rec.601 luma of the texture at the slot. This is one of the
    // dozens of taps the relief path takes per fragment.
    float h = dot(wl_tex_slot(slot, uv).rgb, float3(0.299, 0.587, 0.114));
    if (neuralK > 0.001)
        // The neural micro is scaled by 2.0 here, not in wl_neural_micro itself, so the
        // vertex-stage twin can use the unscaled return value and still agree with this one.
        h += wl_neural_micro(uv, n) * 2.0 * neuralK;
    return clamp(h, 0.0, 1.5);
}

// The comment above states the limitation plainly: no shadow rays, because the raster
// path has no shadow maps. True shadows are in the Nova ray-trace pipeline, and this
// shader's lighting is unshadowed apart from reliefShadow.
// Analytic component lights for the raster preview path. Mirrors the Nova
// evaluation (same falloff/cone/area model) but WITHOUT shadow rays — the
// raster path has no shadow maps; true shadows live in Nova.
// The analytic light loop: up to 8 component lights, matching the array sizes in the
// cbuffer. Each is a direction, point, spot or area light selected by a type code
// packed into the light data.
float3 wl_analytic_lights(float3 albedo, float metallic, float roughness, float3 N, float3 wp)
{
    // V computed per call rather than passed in, because the analytic light function is
    // also used from the classical path where V was computed earlier for the main term.
    float3 V = normalize(u_CameraPosition - wp);
    float3 F0 = lerp_safe(float3(0.04, 0.04, 0.04), albedo, clamp(metallic, 0.0, 1.0));
    // Roughness floored at 0.04. A zero roughness makes the exponent below astronomically
    // large and the specular term a single-pixel spike; the floor avoids that.
    float rough = clamp(roughness, 0.04, 1.0);
    float3 acc = float3(0.0, 0.0, 0.0);
    // A fixed bound of 8 with an early break on the light count -- the same shape as the
    // volume loops, because the array size is a hard ceiling.
    for (int a = 0; a < 8; a++)
    {
        // The light count test. Break rather than continue, so an unused tail costs nothing.
        if (a >= u_ALightCount) break;
        // The type code, compared with ranges rather than switched on: 0-0.5 directional,
        // 1-1.5 point, 2-2.5 spot, above 2.5 area.
        float atype = u_ALightData[a].x;
        // Colour premultiplied by intensity once, here, so the accumulation below does not
        // repeat the multiply per term.
        float3 lcol = u_ALightColor[a] * u_ALightData[a].y;
        float arange = max(u_ALightData[a].z, 0.0);
        // Falloff floored at 0.5 so a light uploaded with falloff 0 does not produce an
        // infinite spike at the origin.
        float afall = max(u_ALightData[a].w, 0.5);
        float3 L;
        float att = 1.0;
        // Directional lights need no distance term -- that is the whole difference between
        // this branch and the one below, and it is why atype is compared first.
        if (atype < 0.5) {
            L = -u_ALightDir[a];
        } else {
            float3 toL = u_ALightPos[a] - wp;
            float d = length(toL);
            // L normalised with a 1e-5 floor, for a fragment sitting exactly at the light
            // position.
            L = toL / max(d, 0.00001);
            if (arange > 0.0) {
                if (d >= arange) continue;
                att *= pow(clamp(1.0 - d / arange, 0.0, 1.0), afall);
            } else {
                // The inverse-square branch, used when range is 0 (infinite). The 0.1 coefficient is
                // a softening constant -- pure inverse-square falls off fast enough to alias.
                att *= 1.0 / (1.0 + 0.1 * afall * d * d);
            }
            // Spot: the type range is exclusive on both sides, so a code of exactly 2 falls in.
            if (atype > 1.5 && atype < 2.5) {
                float c = dot(-L, u_ALightDir[a]);
                // smoothstep(outer, inner, c) -- the REVERSED edges are deliberate and are the standard
                // way to express a cone falloff: it returns 1 when c exceeds the inner cosine and 0
                // below the outer one.
                att *= smoothstep(u_ALightData2[a].y, u_ALightData2[a].x, c);
                if (att <= 0.0) continue;
            } else if (atype > 2.5) {
                // Area lights are one-sided or two-sided, controlled by a flag rather than inferred
                // from geometry.
                float front = dot(-L, u_ALightDir[a]);
                if (u_ALightData2[a].w < 0.5) {
                    if (front <= 0.0) continue;
                    att *= clamp(front * 3.0, 0.0, 1.0);
                }
                float asize = max(u_ALightData2[a].z, 0.01);
                // The area-light size term: as the fragment approaches the light plane the term
                // approaches 1, which is the correct behaviour for an area source rather than a point.
                att *= asize / (d + asize);
            }
        }
        // NdotL clamped rather than discarded: a zero contribution and a negative one are the
        // same thing here, and clamping lets the compiler use a max() instead of a branch.
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;
        float3 H = normalize(V + L);
        float3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        // The exponent lerps from 128 (sharp) to 8 (broad) with roughness. Written as a lerp
        // between two powers rather than as a roughness curve because the visual tuning was
        // done against these two numbers.
        float spec = pow(max(dot(N, H), 0.0), lerp_safe(128.0, 8.0, rough));
        // The whole accumulation in one statement: a Lambert diffuse for dielectrics plus a
        // Schlick-weighted specular, both scaled by NdotL and the attenuation. One line
        // because there is no separate ambient term here -- ambient is handled above.
        acc += (albedo * (1.0 - metallic) * NdotL + F * spec * NdotL) * lcol * att;
    }
    return acc;
}

// [earlydepthstencil] -- see the note at line 341. This attribute is what preserves
// early-z for the fragments that do not write SV_Depth.
[earlydepthstencil]
PSOutput main(PSInput Input)
{
    PSOutput o;
    // The default depth. Every fragment that does NOT displace keeps the interpolated
    // triangle depth, which is what makes the early-z path correct rather than merely fast.
    o.Depth = Input.Position.z; // fragments that never write depth keep it
    float4x4 u_ViewProjection_PS = float4x4(Input.u_VPRow0, Input.u_VPRow1,
                                            Input.u_VPRow2, Input.u_VPRow3);
    // THE HARDCODED SUN. The cbuffer declares a sun light colour and a sun direction
    // uniform, and this line ignores both in favour of a literal direction.
    // DEFECT (hardcoded light): the key light cannot be rotated without editing and
    // recompiling the shader, while the volumetrics below march toward the cbuffer's sun
    // direction. So the god rays and the surface lighting can disagree about where the sun
    // is. Not fixed; comments only.
    float3 lightDir = normalize(float3(0.3, 1.0, 0.4));
    float3 norm0 = normalize(Input.Normal);
    float3 V = normalize(u_CameraPosition - Input.WorldPos);
    // Water is marked by a NEGATIVE bump strength. The relief gate above requires values
    // above 0.001, so water is excluded from relief by construction rather than by a
    // separate flag -- one fewer attribute, at the cost of overloading a meaningful-looking
    // field with a sentinel.
    // Water marker: water verts arrive with BumpStrength = -1 (the relief
    // path above already skipped them: its gate needs values > 0.001).

    // ---- Neural relief: UV-space height march (parallax) + gradient normal
    // + light self-shadow. Distance- and grazing-faded, zero taps when the
    // material opts out (Displacement and BumpStrength both 0).
    // The relief UV, tiled once here. Every sample below uses this, so a parallax offset
    // moves all of them together -- which is the point, since the offset IS the parallax.
    float2 reliefUV = Input.TexCoord * Input.TilingFactor;
    float3 norm = norm0;
    float reliefShadow = 1.0;
    float reliefPush = 0.0;
    float reliefH = -1.0;
    // strataW from the WORLD normal, not the relief-perturbed one: strata banding should not
    // wobble with the high-frequency normal, only with the surface orientation.
    float mesoAmp = 0.0;
    float strataW = clamp(1.0 - abs(norm0.y), 0.15, 1.0);
    {
        float disp = Input.Displacement;
        float bump = Input.BumpStrength;
        int slot = (int)(Input.TexIndex);
        float dist = length(u_CameraPosition - Input.WorldPos);
        // The distance fade: full effect inside 18 units, gone by 48. Hardcoded rather than
        // uniform-driven, so a project cannot tune it without a shader edit.
        float fade = clamp(1.0 - (dist - 18.0) / 30.0, 0.0, 1.0);
        float ndv = abs(dot(V, norm0));
        // The same angular collapse the Nova path uses: parallax smear is worst at grazing
        // angles, so the effect is killed as the view angle approaches 0. Sharing the constant
        // with the ray-trace path is what keeps the two looking similar.
        // Same angular collapse as Nova: kill POM smear streaks edge-on.
        float marchK = smoothstep(0.10, 0.45, ndv);
        // Tangent frame from screen-space derivatives: uniform control flow,
        // so no undefined-derivative issues inside the gated march below.
        // The tangent frame from screen-space derivatives rather than from a UV tangent
        // attribute. The comment above explains why this matters: these are evaluated in UNIFORM
        // control flow, outside the gated loop below, so the derivatives are well-defined even
        // though the march itself is inside a branch.
        float3 dpx = ddx(Input.WorldPos);
        float3 dpy = ddy(Input.WorldPos);
        float2 dtx = ddx(reliefUV);
        float2 dty = ddy(reliefUV);
        float3 T = dpx * dty.y - dpy * dtx.y;
        // The 1e-6 bias keeps normalize() from producing NaN where the cross product
        // degenerates -- which happens for a degenerate triangle or at a UV singularity.
        T = normalize(T - norm0 * dot(norm0, T) + float3(1e-6, 1e-6, 1e-6));
        float3 B = normalize(cross(norm0, T));
        float neuralK = (u_NeuralEnabled == 1) ? clamp(u_NeuralTexStrength, 0.0, 1.0) : 0.0;
        // THE GATE. Three conditions, all of which must hold: the material opts in
        // (displacement or bump), the fragment is near enough, and it is not edge-on. Outside
        // this, zero texture taps happen -- which is why the shader is not uniformly expensive.
        if ((disp > 0.001 || bump > 0.001) && fade > 0.001 && ndv > 0.12)
        {
            // The march runs only when displacement is active. Bump-only materials skip the
            // parallax entirely and take only the gradient normal, which is the cheap case.
            float ds = disp * fade;
            // bs is the distance-faded bump strength. Separate from ds above because a material
            // can have one without the other, and both are gated on the same fade.
            float bs = bump * fade;
            // The meso amplitude is derived from the DISPLACEMENT strength, not from the bump
            // strength -- so a bump-only material gets no meso breakup.
            mesoAmp = 0.25 * clamp(ds, 0.0, 1.5);
            // The parallax depth scale, capped at 0.12 UV units. The cap bounds how far the march
            // can shift the sample point, which bounds the artefact when the height field is wrong.
            float pScale = clamp(ds * 0.05, 0.0, 0.12);
            float finalH = wl_relief_height(slot, reliefUV, norm0, neuralK);
            if (ds > 0.001)
            {
                // The view vector in tangent space. This is the frame the march step is computed in,
                // which is why the tangent frame had to be built from derivatives rather than from an
                // attribute.
                float3 viewTS = float3(dot(V, T), dot(V, B), dot(V, norm0));
                // The layer count falls from 14 (face-on) to 6 (grazing). Fewer layers where the
                // parallax is short, which is where the effect is least visible anyway.
                int layers = (int)(lerp_safe(14.0, 6.0, ndv));
                // The step per layer. Dividing pScale by the layer count makes the total march length
                // independent of the layer count, so the quality setting changes the step size rather
                // than the depth reached.
                float2 stepUV = (-viewTS.xy / max(viewTS.z, 0.08)) * (pScale / (float)(layers));
                float dh = 1.0 / (float)(layers);
                float2 cuv = reliefUV;
                float clH = 1.0;
                float cTexH = finalH;
                float2 puv = cuv;
                float plH = clH;
                float pTexH = cTexH;
                // The linear march. Saves the PREVIOUS step before advancing, because the binary refine
                // below bisects between the last two samples -- which is why the three march variables
                // exist as separate variables rather than being recomputed.
                for (int i = 0; i < 14; i++)
                {
                    if (i >= layers)
                        break;
                    puv = cuv;
                    plH = clH;
                    pTexH = cTexH;
                    cuv += stepUV;
                    clH -= dh;
                    // One height tap per march layer, which is the dominant cost of the whole shader.
                    cTexH = wl_relief_height(slot, cuv, norm0, neuralK);
                    // The intersection test: the ray height has fallen below the surface, so stop. Testing
                    // after the advance (rather than before) is what makes the loop a forward march.
                    if (cTexH > clH)
                        break;
                }
                // Binary refine: 4 bisections turn a linear-search bracket into a tight intersection.
                // The cost is 4 extra height taps per displaced fragment, bought for a crisp
                // silhouette.
                // Binary refine: crisp intersections for deep relief (4 taps).
                // The refine loop reuses the variable name b, which shadows nothing here -- the march
                // loop above used i. Worth knowing before adding a variable between the two loops.
                for (int b = 0; b < 4; b++)
                {
                    // The midpoint of the bracket, and one more height tap. Four of these is the entire
                    // cost of the crisp silhouette.
                    float2 midUV = (puv + cuv) * 0.5;
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
                // rd1 and rd2 are the ray-height-minus-surface-height at the two bracket ends. One is
                // positive and one negative after a successful intersection, which is what makes the
                // ratio below a valid interpolation weight.
                float rd1 = plH - pTexH;
                float rd2 = cTexH - clH;
                // The interpolation weight between the bracketing samples. The 1e-4 floor in the
                // denominator prevents a division by zero when both differences are ~0.
                float rw = rd1 / max(rd1 + rd2, 0.0001);
                // lerp between the bracket endpoints by the weight, minus the original UV to give a
                // DELTA rather than an absolute position. marchK scales it, so the whole parallax
                // collapses to zero at grazing angles.
                float2 delta = (lerp_safe(puv, cuv, rw) - reliefUV) * marchK;
                // The offset clamp. An 8% UV shift is the point past which parallax becomes an obvious
                // smear rather than depth, so it is capped rather than allowed to run to the edge of the
                // texture.
                if (length(delta) > 0.08)
                    delta = normalize(delta) * 0.08;
                // reliefUV is mutated here, so every later sample (gradient, shadow, albedo) uses the
                // parallax-shifted coordinate. That is what makes the effect read as depth rather than
                // as a shifted texture.
                reliefUV += delta;
                // The height is interpolated between the bracket ends rather than resampled -- one
                // fewer tap, and the value is already the converged one from the refine loop.
                finalH = lerp_safe(pTexH, cTexH, rw);
                // The meso detail is added AFTER the march, to the converged height. Adding it to the
                // marched height instead would make the linear search chase its own noise.
                finalH = clamp(finalH + wl_relief_meso(reliefUV, Input.WorldPos, mesoAmp, strataW), 0.0, 1.5);
                reliefH = finalH;
                // Shape-preserving push (bilateral around the mid-level): the base surface survives and
                // features rise and recess. Multiplying (finalH - 0.5) rather than finalH is what keeps
                // a flat texture flat instead of inflating the mesh.
                // Shape-preserving push (bilateral around the mid-level, like
                // Nova): the base shape survives, features rise and recess.
                reliefPush = (finalH - 0.5) * ds;
            }
            if (bs > 0.001)
            {
                // The finite-difference epsilon in UV units. 0.004 is small enough to resolve
                // texel-scale detail and large enough not to be swamped by the noise floor of the
                // height field.
                float e = 0.004;
                float hU = wl_relief_height(slot, reliefUV + float2(e, 0.0), norm0, neuralK)
                         + wl_relief_meso(reliefUV + float2(e, 0.0), Input.WorldPos, mesoAmp, strataW);
                float hV = wl_relief_height(slot, reliefUV + float2(0.0, e), norm0, neuralK)
                         + wl_relief_meso(reliefUV + float2(0.0, e), Input.WorldPos, mesoAmp, strataW);
                // The central difference. One-sided differences would be cheaper and would introduce a
                // directional bias in the normal -- visible as a lighting seam at UV tile edges.
                float dhdu = (hU - finalH) / e;
                float dhdv = (hV - finalH) / e;
                // The comment above records a genuine port problem: the raster path's UV-per-world ratio
                // is per-pixel and unbounded, where Nova could assume roughly one UV per metre. The
                // gradient is scaled by the measured ratio to compensate.
                // UV-per-world measured per pixel: raster faces span 0-1 UV
                // over arbitrary world sizes, so Nova's ~1 UV/m assumption
                // does not hold here.
                // The UV-to-world ratio, clamped to [0.05, 200]. Both ends matter: the lower prevents a
                // division blow-up at a UV singularity, the upper prevents a degenerate zero gradient.
                float worldPerUV = clamp(length(dpx) / max(length(dtx), 1e-6), 0.05, 200.0);
                // The gradient is scaled by 1/worldPerUV so the perturbation is in world units. A
                // large UV-to-world ratio (a small mesh in UV terms) yields a small bump, which is what
                // stops distant or large-mapped surfaces from looking sandpapered.
                float3 grad = (T * dhdu + B * dhdv) * (bs / worldPerUV);
                float gl = length(grad);
                // Gradient magnitude clamp at 1.5. Without it a high-frequency height field produces a
                // near-90-degree normal perturbation and the surface reads as noise.
                if (gl > 1.5)
                    grad /= gl / 1.5;
                // norm0 MINUS the gradient, not plus. The height field rises toward the light-facing
                // side, so the surface normal tilts the other way from the height gradient.
                norm = normalize(norm0 - grad);
            }
            if (ds > 0.001)
            {
                // The self-shadow march: 4 taps toward the light, each raising the ray height and taking
                // the minimum clearance. Cheap, and it is what makes deep relief read as sculpted rather
                // than embossed.
                // Cheap heightfield self-shadow: 4 taps toward the light.
                // The light in tangent space, for the shadow march -- the same frame as the view march,
                // which is what lets both use the same step formulation.
                float3 lts = float3(dot(lightDir, T), dot(lightDir, B), dot(lightDir, norm0));
                float2 sStep = (lts.xy / max(lts.z, 0.1)) * (pScale / 4.0);
                // The shadow ray starts 0.03 above the surface: a surface shadowing itself is the
                // degenerate case, and the bias prevents every fragment from being fully occluded.
                float rayH = finalH + 0.03;
                float occ = 1.0;
                float2 suv = reliefUV;
                for (int i = 0; i < 4; i++)
                {
                    suv += sStep;
                    rayH += 0.06;
                    float hh = wl_relief_height(slot, suv, norm0, neuralK)
                             + wl_relief_meso(suv, Input.WorldPos, mesoAmp, strataW);
                    // occ accumulates the MINIMUM clearance over the taps. Minimum, not average -- a
                    // shadow is cast by the single highest obstruction along the ray.
                    occ = min(occ, clamp((rayH - hh) * 8.0, 0.0, 1.0));
                }
                // Clamped to a 0.3 floor so relief self-shadowing can darken but never fully black out a
                // surface. A full-black term would read as an albedo change.
                reliefShadow = clamp(occ, 0.3, 1.0);
            // End of the relief block. Everything after this point works on the possibly-modified
            // reliefUV and norm.
            }
        }
    }

    // THE DEPTH COMMIT, and the reason this shader needs SV_Depth at all: without it the
    // displaced surface would still be depth-tested at its undisplaced position, so relief
    // could not self-occlude or intersect other geometry.
    // Screen-space displacement: commit the relief to the depth buffer so it
    // self-occludes and intersects other geometry like real polygons. Untouched
    // (== interpolated triangle depth) for every non-relief fragment.
    // The depth write is conditional on a nonzero push. This is what lets the majority of
    // fragments -- flat, distant, or edge-on -- keep early-z.
    if (abs(reliefPush) > 0.00001)
    {
        // The reprojection uses the REBUILT matrix from the interpolated rows, not the
        // view-projection uniform directly -- see the two-cbuffer note above. The normal used is
        // the undisplaced one, so the push is along the surface.
        float4 dispClip = mul(u_ViewProjection_PS, float4(Input.WorldPos + norm0 * reliefPush, 1.0));
        // The clip-space to [0,1] remap, matching the convention of the value assigned to
        // o.Depth at entry. Both halves must agree or every displaced fragment would write a
        // wildly wrong depth.
        o.Depth = (dispClip.z / dispClip.w) * 0.5 + 0.5;
    }

    float4 texColor = Input.Color;

    // A SECOND texture-array selector, separate from wl_tex_slot above. Redundant-looking,
    // but the two sample at different LODs and in different contexts: this one uses TEX
    // (Sample, implicit LOD) for the albedo, while wl_tex_slot uses SampleLevel(0) because it
    // sits inside the march.
    // Evaluate hardware texture slots using an integer cast selector
    // The albedo selector, and the second of three texture-array switches in this file
    // (the others at lines 145 and 686). All three exist for the same FXC reason.
    switch((int)(Input.TexIndex))
    {
        // texColor *= rather than texColor = : the vertex colour is a multiplier, so a white
        // vertex gives the raw texture and a tinted vertex tints it.
        case 0:  texColor *= TEX(u_Textures[0],  reliefUV); break;
        case 1:  texColor *= TEX(u_Textures[1],  reliefUV); break;
        case 2:  texColor *= TEX(u_Textures[2],  reliefUV); break;
        case 3:  texColor *= TEX(u_Textures[3],  reliefUV); break;
        case 4:  texColor *= TEX(u_Textures[4],  reliefUV); break;
        case 5:  texColor *= TEX(u_Textures[5],  reliefUV); break;
        case 6:  texColor *= TEX(u_Textures[6],  reliefUV); break;
        case 7:  texColor *= TEX(u_Textures[7],  reliefUV); break;
        case 8:  texColor *= TEX(u_Textures[8],  reliefUV); break;
        case 9:  texColor *= TEX(u_Textures[9],  reliefUV); break;
        case 10: texColor *= TEX(u_Textures[10], reliefUV); break;
        case 11: texColor *= TEX(u_Textures[11], reliefUV); break;
        case 12: texColor *= TEX(u_Textures[12], reliefUV); break;
        case 13: texColor *= TEX(u_Textures[13], reliefUV); break;
        case 14: texColor *= TEX(u_Textures[14], reliefUV); break;
        case 15: texColor *= TEX(u_Textures[15], reliefUV); break;
        case 16: texColor *= TEX(u_Textures[16], reliefUV); break;
        case 17: texColor *= TEX(u_Textures[17], reliefUV); break;
        case 18: texColor *= TEX(u_Textures[18], reliefUV); break;
        case 19: texColor *= TEX(u_Textures[19], reliefUV); break;
        case 20: texColor *= TEX(u_Textures[20], reliefUV); break;
        case 21: texColor *= TEX(u_Textures[21], reliefUV); break;
        case 22: texColor *= TEX(u_Textures[22], reliefUV); break;
        case 23: texColor *= TEX(u_Textures[23], reliefUV); break;
        case 24: texColor *= TEX(u_Textures[24], reliefUV); break;
        case 25: texColor *= TEX(u_Textures[25], reliefUV); break;
        case 26: texColor *= TEX(u_Textures[26], reliefUV); break;
        case 27: texColor *= TEX(u_Textures[27], reliefUV); break;
        case 28: texColor *= TEX(u_Textures[28], reliefUV); break;
        case 29: texColor *= TEX(u_Textures[29], reliefUV); break;
        case 30: texColor *= TEX(u_Textures[30], reliefUV); break;
        case 31: texColor *= TEX(u_Textures[31], reliefUV); break;
    }

    // Clamped at the use site. The attribute itself is unclamped, so a material can carry
    // a value above 1 and it is brought into range here rather than at upload.
    float metallic = clamp(Input.Metallic, 0.0, 1.0);
    float roughness = clamp(Input.Roughness, 0.04, 1.0);

    // Weathering driven by the relief height rather than by a separate map: dust in the
    // pits, sun on the ridges. It costs nothing extra because the relief height is already
    // computed -- and it is skipped entirely where relief did not run.
    // Relief weathering: dust settles in pits, sun bleaches ridge tops.
    if (reliefH >= 0.0)
    {
        // Cavity from a REVERSED smoothstep: 1 when the height is below 0.05, 0 above 0.45.
        // The same reversed-edge idiom as the spot cone at line 783.
        float cav = smoothstep(0.45, 0.05, reliefH);
        // Ridge tops by the ordinary smoothstep, from 0.55 to 0.95. The two bands do not
        // overlap, so a fragment is either dusty, bleached, or neither.
        float ridge = smoothstep(0.55, 0.95, reliefH);
        texColor.rgb *= (1.0 - 0.30 * cav);
        // The bleach tint (1.10, 1.03, 0.92) is warm and non-normalised -- a multiplier above 1
        // in red. It is applied by lerping the already-multiplied texColor, so the ridge term
        // can brighten past the texture value.
        texColor.rgb = lerp_safe(texColor.rgb, texColor.rgb * float3(1.10, 1.03, 0.92), ridge * 0.65);
    }

    // The water block. The comment above is unusually candid about the design: water is
    // routed through the metal-gated environment term rather than given a proper dielectric
    // path, because a dielectric gets no sky reflection from a term gated on metallic.
    // ---- Water: mirror + pixel ripple detail (Unreal-style) ----
    // Dielectric water would get no sky reflection from the metal-gated env
    // term below, so route water through the mirror path instead: diffuse
    // dies (water has none), F0 picks up the water tint, and the sky mirror
    // + sun glints take over. norm (Gerstner mesh normal) is augmented with
    // animated pixel-level ripple octaves so chop survives at any mesh
    // density; the existing fresnel/spec/analytic-light terms then respond
    // to the perturbed normal with zero further changes.
    // A bump strength below -0.5 as the water test. A magic threshold on a sentinel value,
    // documented at line 815. It works because the relief gate rejects anything at or below
    // 0.001, so the two tests cannot both be true for the same fragment.
    bool wlWater = (Input.BumpStrength < -0.5);
    if (wlWater)
    {
        // The ripple UV is WORLD xz, not the mesh UV. So chop is continuous across a water
        // mesh made of several pieces, and does not swim when the mesh is re-tiled.
        float2 wpuv = Input.WorldPos.xz;
        float wdist = length(u_CameraPosition - Input.WorldPos);
        // A separate, more distant fade than the relief fade at line 833 -- water survives to
        // 85 units where relief dies at 48, because water is large-scale and relief is detail.
        float wfade = clamp(1.0 - (wdist - 25.0) / 60.0, 0.0, 1.0);
        if (wfade > 0.001)
        {
            float wt = u_Time;
            // Three crossing wave octaves at different frequencies and angles. Summed as gradient
            // contributions and applied to the normal below, rather than displacing geometry -- which
            // is what keeps chop alive at any mesh density.
            float2 wg = float2(0.0, 0.0);
            // The first ripple octave. Each subsequent one is a different direction, frequency and
            // speed; three crossing waves is the minimum that reads as water rather than as a
            // single corrugation.
            wg += float2(0.96, 0.28) * (cos(dot(float2(0.96, 0.28), wpuv) * 2.1 + wt * 1.6) * 0.22);
            wg += float2(-0.42, 0.91) * (cos(dot(float2(-0.42, 0.91), wpuv) * 3.7 - wt * 2.3 + 1.7) * 0.13);
            wg += float2(0.66, -0.75) * (cos(dot(float2(0.66, -0.75), wpuv) * 7.9 + wt * 3.4 + 4.2) * 0.07);
            // The wave gradient perturbs ONLY x and z -- y is untouched, so the water surface stays
            // horizontal. A 3D perturbation here would make the surface look like it had relief.
            norm = normalize(norm + float3(-wg.x, 0.0, -wg.y) * (0.55 * wfade));
        }
        // metallic = 1.0 FORCES the water down the mirror path. This is the line the comment
        // above describes as the workaround, and it is why water has no diffuse term at all:
        // metallic multiplies it by zero.
        metallic = 1.0;
    }

    // The metallic PBR terms. Note no shadow maps on the raster path -- this is the known
    // gap against the Nova pipeline, repeated here at the code.
    // Metallic PBR terms (shared by neural + classic paths). Diffuse dies
    // out on metals; reflection + specular take over. No shadow maps on the
    // raster path — true shadows live in the Nova ray-trace pipeline.
    // F0 from albedo scaled by metallic. Recomputed in the neural path below as nF0; the
    // classical path reuses this one.
    float3 F0 = lerp_safe(float3(0.04, 0.04, 0.04), texColor.rgb, metallic);
    // The reflected vector from the POSITION-perturbed norm. So the ripple perturbs the
    // reflection as well as the diffuse, which is what makes it read as a surface.
    float3 R = reflect(-V, norm);
    // The sky reflection, sampled by the REFLECTED vector rather than by the normal, which
    // is what makes it view-dependent -- and metal reflecting sky is most of what a metal
    // surface looks like.
    float3 specSky = lerp_safe(u_SkyBottomColor, u_SkyTopColor, R.y * 0.5 + 0.5);
    // Diffuse sky sampled by the surface normal and specular sky by the reflection. Two
    // samples rather than one because a mirror and a matte surface see different sky.
    float3 diffSky = lerp_safe(u_SkyBottomColor, u_SkyTopColor, norm.y * 0.5 + 0.5);
    // roughness-squared blends between the sharp reflection and the diffuse average.
    // Squaring rather than linear makes rough surfaces converge on the average faster, which
    // is the usual reason for the square.
    float3 envColor = lerp_safe(specSky, (specSky + diffSky) * 0.5, roughness * roughness);
    float NdotL = max(dot(norm, lightDir), 0.0);
    float3 H = normalize(lightDir + V);
    float specTerm = pow(max(dot(norm, H), 0.0), lerp_safe(128.0, 8.0, roughness))
                   // reliefShadow multiplies the specular term too, not just the diffuse. Self-shadowing
                   // from relief should darken the highlight -- that is most of what makes carved detail
                   // read as carved.
                   * (1.0 - roughness * 0.7) * reliefShadow;
    float3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    // Schlick Fresnel with the standard 1/5 exponent. F0 is the reflectivity at normal
    // incidence; the curve rises to 1 at grazing angles.
    float diffuseIntensity = NdotL * reliefShadow;
    // The ambient constant. A flat 0.2 rather than anything sky-derived, so ambient is not
    // affected by the sky colours a scene sets.
    float ambientIntensity = 0.2;

    // The neural branch: an entirely separate shading path, not a modulation of the
    // classical one below. Both are present in the compiled shader and the uniform selects
    // between them per draw.
    if (u_NeuralEnabled == 1)
    {
        if (!wlWater)
        {
            // The texture detail term, skipped for water -- a mirror should not have albedo detail
            // modulating it, and water has no albedo to modulate.
            float3 detail = wl_neural_tex_detail(reliefUV, norm);
            texColor.rgb *= lerp_safe(float3(1.0, 1.0, 1.0), detail, clamp(u_NeuralTexStrength, 0.0, 1.0));
        }

        // The learned ambient, fed the CURRENT texColor so it responds to the texture rather
        // than to a flat grey.
        float3 namb = wl_neural_ambient(norm, texColor.rgb);
        // Both neural strengths clamped to [0,1] at the use site, so an upload above 1 cannot
        // over-drive the terms. The same clamp appears at every other neural uniform use.
        float k = clamp(u_NeuralLightStrength, 0.0, 1.0);
        // matStrength controls how far the neural material is blended toward the MLP output.
        // It appears in four separate terms below, which is why it is stored once here.
        float km = clamp(u_NeuralMatStrength, 0.0, 1.0);
        // Neural material: porosity/patina albedo, wrap diffuse, flake glints
        float matGlint = 0.0;
        float matWrap = 0.5;
        float3 matAlbedo = texColor.rgb;
        {
            // Declared then immediately filled from the out-parameters -- the inner block exists so
            // glint and wrapW have a scope that ends before the branch below uses them.
            float g;
            float w;
            wl_neural_mat_raster(reliefUV, norm, matAlbedo, g, w);
            matGlint = g;
            matWrap = w;
        }
        if (!wlWater)
            texColor.rgb = lerp_safe(texColor.rgb, matAlbedo, km);
        // Re-derive F0 from the neural albedo so flakes tint reflections.
        // F0 re-derived from the neural albedo, not reused from the classical path above. That
        // is what lets the flake tint reach the reflection.
        float3 nF0 = lerp_safe(float3(0.04, 0.04, 0.04), texColor.rgb, metallic);
        float3 nF = nF0 + (1.0 - nF0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        // WRAP DIFFUSE, and the divide is bounded to [0,1] because a large wrap width would
        // otherwise push the term above 1 and over-brighten the surface.
        float wrapDiffuse = clamp((dot(norm, lightDir) + matWrap * km) / (1.0 + matWrap * km), 0.0, 1.0);
        float diffTerm = lerp_safe(diffuseIntensity, wrapDiffuse, km * 0.7) * (1.0 - metallic);
        float3 warmLight = float3(1.0, 0.98, 0.94);
        // The warm light tint (1.0, 0.98, 0.94) is a hardcoded approximation of sunlight. Note
        // the cbuffer sun colour is NOT used here -- the same hardcoded-sun issue recorded at
        // line 812.
        float3 lit = texColor.rgb * (diffTerm * warmLight + lerp_safe(float3(ambientIntensity, ambientIntensity, ambientIntensity), namb * 2.2, k) * (1.0 - metallic * 0.85));
        lit += nF * envColor * metallic;
        lit += specTerm * nF * NdotL;
        lit += matGlint * km * (0.25 + 0.75 * metallic) * diffuseIntensity * float3(1.3, 1.25, 1.15);
        // The analytic lights are added LAST in this path, so they receive the relief shadow but
        // not the neural ambient. The ordering is a choice, not a derivation.
        lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
        {
            // The view ray for the volumetric pass, built from the world position rather than from
            // the SV_Position depth. Same vector the fragment used for V, negated.
            float3 vdir = Input.WorldPos - u_CameraPosition;
            float vdist = length(vdir);
            // The dither for the volume march is a HASH OF THE FRAGMENT POSITION, which is what
            // breaks the banding without a noise texture. Being a hash of pixel coordinates, it is
            // stable for a static camera and re-randomises as it moves.
            lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(Input.Position.xy), 1.0);
        }
        o.Color = float4(lit, texColor.a);
        o.EntityID = Input.v_EntityID;
        // The neural path returns EARLY, before the classical code below. So the two paths are
        // mutually exclusive branches in one entry point rather than two shaders -- one
        // pipeline state, one draw.
        return o;
    }

    // The classical path, and it is much shorter than the neural one above -- which is the
    // honest measure of what the neural-enabled uniform actually costs per fragment.
    float3 diffTerm = texColor.rgb * (1.0 - metallic) * (ambientIntensity + diffuseIntensity);
    // The same ordering as the neural path: diffuse, then environment, then specular.
    // Consistency between the two paths is what keeps toggling the uniform from looking like
    // a lighting change.
    float3 lit = diffTerm + F * envColor * metallic + specTerm * F * NdotL;
    lit += wl_analytic_lights(texColor.rgb, metallic, roughness, norm, Input.WorldPos) * reliefShadow;
    {
        float3 vdir = Input.WorldPos - u_CameraPosition;
        float vdist = length(vdir);
        lit = wl_apply_volumetrics(u_CameraPosition, vdir / max(vdist, 0.00001), vdist, lit, wl_hash12(Input.Position.xy), 1.0);
    }

    // The classical output. Colour and entity id only -- o.Depath was set at entry and
    // possibly overwritten at line 958, which is why it is not assigned here.
    o.Color = float4(lit, texColor.a);
    o.EntityID = Input.v_EntityID;
    return o;
// End of the pixel stage, and of the file. Both stages are compiled into one program by
// the loader, which is why every shared helper is declared twice.
}
