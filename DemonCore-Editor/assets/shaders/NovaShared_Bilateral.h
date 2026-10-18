// ============================================================================
// NovaShared_Bilateral.h -- SHARED ALGORITHM SOURCE for the bilateral blur.
//
// WHAT THIS FILE IS
// One copy of the RunBilateralBlur algorithm, written in the subset of the
// GLSL and HLSL languages that both accept verbatim. The twins pull it in
// behind a thin per-language macro shim (see the include sites in
// Renderer3D_NovaRenderer.glsl / .hlsl) so the arithmetic, the tap layout,
// the weights and the comments exist exactly once.
//
// WHY IT IS A .h AND NOT A .glsl/.hlsl
// The HLSL side gets it via D3D_COMPILE_STANDARD_FILE_INCLUDE. The GLSL side
// needs GL_GOOGLE_include_directive, which is an EXTENSION, not core GLSL 4.30
// -- see the note at the include site in the .glsl. That is the load-bearing
// caveat of this arrangement and is why the shared form is confined to ONE
// pass rather than attempted file-wide.
//
// THE SHIM CONTRACT
// Every name below is defined by BOTH twins before this file is included. The
// twin supplies only the binding/access syntax; this file owns the algorithm.
//
//   NOVA_POS          the dispatch coordinate of this thread, ivec2/int2
//   NOVA_V2/V3/V4     the language's 2/3/4-component float vector types
//   NOVA_I2           the language's 2-component signed integer type
//   NOVA_IMG_SIZE(e)  the extent the pass must respect
//   NOVA_IMG_LOAD(e,p)/NOVA_IMG_STORE(e,p,v)  image read/write
//   NOVA_TEX(t,uv)    explicit-LOD texture sample
//   NOVA_INT(x)       float->int truncation
//   NOVA_V3_ZERO/NOVA_I2_ZERO/NOVA_I2_ONE  the zero/one vectors
//
// The last three exist only because fxc has no scalar-splat constructor
// (X3014 on float3(0.0f) and int2(0)), while GLSL accepts the one-argument
// form. They are the only non-obvious entries in either shim.
//
// It deliberately does NOT define: the uniform block, the register/binding
// numbers, or the resource declarations. Those stay in the twins, because they
// are the cross-file contracts with DX11Nova.h, DX12Nova.cpp and the Vulkan
// register shift.
// ============================================================================

// The bilateral blur, for edge-aware spatial denoising at High and Ultra.
// ---
// Gated off at Low and Medium, where the temporal accumulation is doing the
// denoising instead.
void RunBilateralBlur()
{
    if (u_QualityLevel <= 1) return;

    NOVA_I2 pos = NOVA_POS;
    // The DISPATCHED extent, not the texture's own size. At u_RenderScale < 1
    // the pass runs over the corner subregion only, and every UV below divides
    // by THIS rather than by imageSize() -- otherwise it reads memory the pass
    // never wrote. Identical to the texture size at scale 1.
    NOVA_I2 imgSize = wl_trace_extent(NOVA_IMG_SIZE(img_Bloom_Temp));
    if (pos.x >= imgSize.x || pos.y >= imgSize.y) return;

    NOVA_V3 centerColor = NOVA_IMG_LOAD(img_Accumulation, pos).rgb;

    // A NaN/Inf guard. This is the one place in the shader that checks for
    // non-finite values.
    // ---
    // The tolerance is the .r channel only -- a NaN in .g or .b passes through
    // and propagates through the weighted average below.
    if (isnan(centerColor.r) || isinf(centerColor.r)) {
        NOVA_IMG_STORE(img_Bloom_Temp, pos, NOVA_V4(0.0f, 0.0f, 0.0f, 1.0f));
        return;
    }

    NOVA_V2 uv = (NOVA_V2(pos) + 0.5f) / NOVA_V2(imgSize);
    float centerDepth = NOVA_TEX(s_DepthBuffer, wl_buf_uv(uv)).r;
    // Sky pixels pass through unfiltered. There is no depth to edge-stop
    // against, and blurring sky against sky is a no-op anyway.
    if (centerDepth >= 1.0f) {
        NOVA_IMG_STORE(img_Bloom_Temp, pos, NOVA_V4(centerColor, 1.0f));
        return;
    }

    // img_TraceGBuffer is (r = NDC depth, gba = packed normal). The normal is in
    // .gba, NOT .rgb; reading .rgb mixes depth into the first two normal
    // channels and shifts the third, so edge-stopping compared a garbage vector.
    // Unpack the normal from .gba.
    NOVA_V3 centerNormal = NOVA_TEX(s_NormalBuffer, wl_buf_uv(uv)).gba * 2.0f - 1.0f;
    float centerLuma = GetLuminance(centerColor);

    // The step multiplier from the CPU's 1/2/4/8 table. The blur radius is
    // therefore geometric across successive dispatches rather than a single
    // fixed width.
    int step = NOVA_INT(max(1.0f, float(u_StepSize)));
    NOVA_V3 totalColor = NOVA_V3_ZERO;
    float totalWeight = 0.0f;

    if (u_QualityLevel == 2) {
        // BRACE initialisers, not GLSL's ivec2[](...) array-constructor form.
        // The constructor form is rejected by fxc (X3000: unexpected token
        // '['), and the brace form is legal in both languages, so it is the
        // only spelling of this table that can be shared.
        NOVA_I2 offsets[9] = {
            NOVA_I2(0, 0),
            NOVA_I2(-1, 0), NOVA_I2(1, 0), NOVA_I2(0, -1), NOVA_I2(0, 1),
            NOVA_I2(-2, 0), NOVA_I2(2, 0), NOVA_I2(0, -2), NOVA_I2(0, 2)
        };
        for (int i = 0; i < 9; i++) {
            // Spatial weights: 0.375 centre, 0.125 for the immediate cross, 0.03125
            // for the outer ring. They sum to 1.0 across the 9 taps.
            NOVA_I2 samplePos = clamp(pos + offsets[i] * step, NOVA_I2_ZERO, imgSize - NOVA_I2_ONE);

            NOVA_V3 neighborColor = NOVA_IMG_LOAD(img_Accumulation, samplePos).rgb;
            NOVA_V2 sampleUV = (NOVA_V2(samplePos) + 0.5f) / NOVA_V2(imgSize);

            float neighborDepth = NOVA_TEX(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
            // .gba, not .rgb -- the trace G-buffer packs depth in .r and the normal in
            // .gba. See the matching comment at the centerNormal read above.
            NOVA_V3 neighborNormal = NOVA_TEX(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
            float neighborLuma = GetLuminance(neighborColor);

            float spatialWeight = (i == 0) ? 0.375f : ((i <= 4) ? 0.125f : 0.03125f);
            // The normal term: 16 here versus 32 at Ultra. A high exponent makes this
            // nearly a step function, so it rejects any neighbour facing a different
            // direction outright -- which is the point, since only normals should
            // block the blur.
            float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 16.0f);
            float depthDiff = abs(centerDepth - neighborDepth);
            // Depth similarity, exp(-diff * 100). The 100 makes it a very tight gate:
            // a depth difference of 0.05 already drops the weight to 0.7%.
            // ---
            // NDC depth is non-linear, so this is not a metric distance -- the gate is
            // much tighter near the far plane than near the camera.
            float depthWeight = exp(-depthDiff * 100.0f);
            float lumaDiff = abs(centerLuma - neighborLuma);
            // Luminance similarity, exp(-diff * 4). Much looser than the depth gate
            // (4 versus 100), so the luminance term protects edges in colour rather
            // than in geometry -- a floor's texture change stops the blur, while a
            // small shading gradient does not.
            float lumaWeight = exp(-lumaDiff * 4.0f);

            float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

            totalColor += neighborColor * weight;
            totalWeight += weight;
        }
    }
    // Ultra's variant: a 5x5 kernel with a STATISTICS-ADAPTIVE luma term,
    // against High's fixed one above.
    // ---
    // That is the substantive difference, not the larger radius. Adapting the
    // luma width to the local variance keeps a smooth gradient from being
    // blurred (where a fixed width would) while still killing noise.
    else if (u_QualityLevel >= 3) {
        float lumaSum = 0.0f;
        float lumaSqSum = 0.0f;
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                NOVA_I2 sp = clamp(pos + NOVA_I2(x, y), NOVA_I2_ZERO, imgSize - NOVA_I2_ONE);
                float l = GetLuminance(NOVA_IMG_LOAD(img_Accumulation, sp).rgb);
                lumaSum += l;
                lumaSqSum += l * l;
            }
        }
        float lumaMean = lumaSum / 9.0f;
        // Local standard deviation of luminance, with the same max-against-negative
        // guard as the temporal clamp's -- the same rounding-error hazard, and the
        // same NaN consequence.
        float lumaVar = sqrt(max(0.0f, (lumaSqSum / 9.0f) - (lumaMean * lumaMean)));

        // A separable 3-tap kernel (0.375, 0.25, 0.0625) indexed by |x| and |y|,
        // giving the 5x5 a separable form for 9 effective weights instead of 25
        // distinct ones.
        const float kernel[3] = { 0.375f, 0.25f, 0.0625f };

        for (int x = -2; x <= 2; x++) {
            for (int y = -2; y <= 2; y++) {
                NOVA_I2 samplePos = clamp(pos + NOVA_I2(x, y) * step, NOVA_I2_ZERO, imgSize - NOVA_I2_ONE);

                NOVA_V3 neighborColor = NOVA_IMG_LOAD(img_Accumulation, samplePos).rgb;
                NOVA_V2 sampleUV = (NOVA_V2(samplePos) + 0.5f) / NOVA_V2(imgSize);

                float neighborDepth = NOVA_TEX(s_DepthBuffer, wl_buf_uv(sampleUV)).r;
                NOVA_V3 neighborNormal = NOVA_TEX(s_NormalBuffer, wl_buf_uv(sampleUV)).gba * 2.0f - 1.0f;
                float neighborLuma = GetLuminance(neighborColor);

                float spatialWeight = kernel[abs(x)] * kernel[abs(y)];
                float normalWeight = pow(max(0.0f, dot(centerNormal, neighborNormal)), 32.0f);
                float depthDiff = abs(centerDepth - neighborDepth);
                float depthWeight = exp(-depthDiff * 100.0f);
                float lumaDiff = abs(centerLuma - neighborLuma);
                // The adaptive luma weight: the local stddev replaces the fixed constant.
                // ---
                // The +0.001 guard keeps the divisor non-zero in a perfectly flat region,
                // where the blur is already a no-op but a division by zero would not be.
                float lumaWeight = exp(-lumaDiff / (lumaVar * 4.0f + 0.001f));

                float weight = spatialWeight * normalWeight * depthWeight * lumaWeight;

                totalColor += neighborColor * weight;
                totalWeight += weight;
            }
        }
    }

    // Normalised by the accumulated weight, so the blur cannot darken the
    // image -- and falls back to the centre sample if every weight was
    // rejected, which happens on a hard silhouette.
    NOVA_V3 finalFiltered = (totalWeight > 0.0001f) ? (totalColor / totalWeight) : centerColor;
    NOVA_IMG_STORE(img_Bloom_Temp, pos, NOVA_V4(finalFiltered, 1.0f));
}