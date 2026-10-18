// Renderer2D Quad Shader (HLSL for NVRHI).
//
// A SINGLE .hlsl holding BOTH stages, separated by the #type directives below.
// The shader loader splits on those, so a stage boundary is a preprocessor
// convention rather than two files -- which keeps the shared VSOutput/PSInput
// declaration from having to be duplicated in a common include.
// Renderer2D Quad Shader (HLSL for NVRHI)

// Stage marker consumed by the loader. Everything above the SECOND #type belongs
// to the vertex stage; everything below belongs to the pixel stage.
#type vertex

// Vertex inputs, named for the D3D semantics NVRHI passes through verbatim from
// the buffer layout. The float3/float2/float/int split is not arbitrary -- it
// matches ShaderDataTypeToNVRHIFormat on the C++ side, and getting one wrong
// changes how the hardware reads the vertex, not just how the shader names it.
struct VSInput
{
    // float3, not float4: the vertex buffer stores position as three components and
    // the homogeneous 1.0 is supplied below. Saves 12% of the position stream.
    float3 Position : POSITION;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    // TexIndex as a FLOAT rather than an int. It indexes an array of bound textures
    // and has to survive interpolation as a varying, so it rides through the
    // interpolator as a float and is cast back on the pixel side.
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
    // int, because an entity id is a handle and must NOT be interpolated -- see the
    // nointerpolation qualifier on the VSOutput member below.
    int EntityID : ENTITYID;
};

// The varying struct. Everything the fragment stage needs crosses here, and the
// list is the vertex shader's entire output contract.
struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD0;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
    // nointerpolation matters for more than tidiness: an entity id is an integer
    // handle, and interpolating one across a triangle's three vertices produces
    // values that belong to no entity at all. Without this the picking buffer
    // returns garbage for any pixel not exactly on a vertex.
    nointerpolation int EntityID : ENTITYID;
};

// The camera matrix, bound at b0 in the VERTEX stage only. This shader has a
// single cbuffer where Renderer3D_Basic.hlsl needs two, because it performs no
// per-fragment work that needs uniforms -- the pixel stage below uses nothing
// but textures and the interpolated varyings.
cbuffer CameraBuffer : register(b0)
{
    float4x4 u_ViewProjection;
};

// The vertex entry point. VSInput by value: these are attributes read from the
// vertex buffer, not system values.
VSOutput main(VSInput input)
{
    // Default-constructed, then every field assigned. HLSL does not zero-initialise
    // an unassigned struct member, so leaving one out here would emit whatever was
    // in the register -- a real source of intermittent garbage, since the compiler
    // is free to elide the assignment.
    VSOutput output;
    // The one transform in the shader. mul() takes the matrix on the LEFT, matching
    // HLSL convention (GLSL would be v * M); with a row-vector convention the
    // operand order would be reversed. Getting this backwards produces a mirrored
    // or transposed image rather than an error.
    output.Position = mul(u_ViewProjection, float4(input.Position, 1.0f));
    // A straight copy. No sRGB decode here: the format on the texture carries the
    // interpretation, and decoding in the shader as well would double-convert.
    output.Color = input.Color;
    output.TexCoord = input.TexCoord;
    output.TexIndex = input.TexIndex;
    // Forwarded rather than recomputed. The tiling factor is applied in the PIXEL
    // stage below, at sample time -- multiplying it into the UV here would make the
    // interpolation rate wrong for a quad that spans several tiles.
    output.TilingFactor = input.TilingFactor;
    output.EntityID = input.EntityID;
    // Returned by value. A struct return with a fixed layout is returned in
    // registers, so there is no per-vertex memory traffic here.
    return output;
}

// The stage boundary. The loader turns everything from here to EOF into the
// pixel shader, and the VERTEX declarations above are not in scope for it.
#type pixel

// The fragment input. It mirrors the vertex output field for field -- and the
// semantics differ where they must: TEXCOORD/TEXINDEX/TILINGFACTOR become
// TEXCOORD0/1/2, because a single 32-bit interpolator cannot carry a
// float2 and a float and another float in one slot.
struct PSInput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD0;
    float TexIndex : TEXINDEX;
    float TilingFactor : TILINGFACTOR;
    // nointerpolation repeated here rather than inherited: this is a separate
    // struct declaration in a separate stage, and the qualifier has to be restated
    // on both sides or the linker treats them as mismatched semantics.
    nointerpolation int EntityID : ENTITYID;
};

// TWO render targets. SV_Target0 is colour; SV_Target1 is the entity id written
// as an int, which is the picking pass.
struct PSOutput
{
    float4 Color : SV_Target0;
    // The entity id as a render target rather than a colour: it is read back by an
    // integer copy, so a float4 colour channel would lose ids above 2^24 and would
    // be interpolated across the triangle without the nointerpolation qualifier.
    int EntityID : SV_Target1;
};

// 32 bound textures in one array at t0-t31. The pixel shader below unrolls a
// 32-iteration loop rather than indexing dynamically, which is why the array
// size is a hard limit rather than a soft one -- see the comment there.
Texture2D u_Textures[32] : register(t0);
SamplerState u_Sampler : register(s0);

// The pixel entry point. PSInput by value, consistent with the vertex stage.
PSOutput main(PSInput input)
{
    PSOutput output;

    // The comment above is the reason for everything in this block: FXC targeting
    // SM 5.0 requires LITERAL indices into a sampler or texture array (error X3512),
    // so `u_Textures[texIndex]` will not compile. The unrolled loop with a literal
    // index is the standard workaround.
    //
    // The cost is real: 32 iterations per fragment even when the loop breaks early,
    // because `break` still costs the compare on all unrolled iterations. At 32
    // slots that is a measurable fragment cost for a 2D quad shader. Not fixed;
    // comments only.
    // FXC (SM 5.0) requires literal indices into sampler/texture arrays
    // (X3512), so select the slot with an unrolled loop instead of
    // u_Textures[texIndex]. SampleLevel avoids needing gradients in a loop.
    // clamp() to 0..31 rather than a modulo or a bare cast. The clamp is what makes
    // an out-of-range or garbage TexIndex select a VALID slot instead of reading
    // out of bounds -- the array bound is a safety property, not just an index.
    int texIndex = clamp(int(input.TexIndex), 0, 31);
    // White as the default. If the unrolled loop somehow never matches -- which the
    // clamp above prevents -- the quad renders white rather than black, which is
    // the less alarming of the two failure modes while debugging.
    float4 texColor = float4(1.0, 1.0, 1.0, 1.0);
    // [unroll] is what turns this into 32 literal-indexed samples that FXC will
    // accept. Without it the loop is a real loop and the dynamic index inside
    // reintroduces exactly the X3512 the unroll avoids.
    [unroll]
    for (int i = 0; i < 32; i++)
    {
        if (i == texIndex)
        {
            // SampleLevel(..., 0.0f) rather than Sample(): Sample computes implicit
            // gradients with ddx/ddy, which are undefined inside a loop whose iteration
            // count varies per pixel (error X3570). Level 0 also matches what a 2D sprite
            // quad wants -- no mip selection, since the texture is authored at the size it
            // is drawn.
            texColor = u_Textures[i].SampleLevel(u_Sampler, input.TexCoord * input.TilingFactor, 0.0f);
            // The break makes the remaining 30 iterations cheap compares but not free. It
            // exists so the loop is not 32 real samples; without it this would be a
            // correctness bug as well as a performance one.
            break;
        }
    }

    // Modulate rather than lerp toward the vertex colour: the vertex colour is an
    // alpha and tint multiplier, and multiplying it through preserves both. An
    // additive blend mode downstream would make the multiply wrong, so this
    // assumes the usual alpha-blend pipeline.
    // Returned rather than written to a global: with two render targets the return
    // struct is the clearest way to populate both, and the compiler treats it the
    // same as two SV_Target writes.
    output.Color = texColor * input.Color;
    output.EntityID = input.EntityID;

    return output;
}
