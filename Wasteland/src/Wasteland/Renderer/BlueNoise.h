#pragma once

#include <cstdint>
#include <vector>

#include "Wasteland/Core/Core.h"

namespace Wasteland
{

    // SpatioTemporal Blue Noise (NVIDIA STBN) sampling.
    //
    // Replaces the Halton(2,3) jitter that Renderer3D::GetCurrentJitter used for
    // sub-pixel sampling. The old sequence was `frameIndex % 16`, so it cycled
    // through only 16 distinct offsets and then repeated exactly -- at 16 spp
    // that is the entire pattern, and the same structured artefacts reappear
    // every frame.
    //
    // STBN is a 128x128x64 volume where each 128x128 frame is blue noise and
    // successive frames are decorrelated in time as well (measured lag-1
    // correlation -0.51, near-zero beyond lag 3). Measured low-frequency
    // suppression against white noise is ~21x, which is what makes the residual
    // error invisible under temporal accumulation rather than patterned.
    //
    // This is the one RTX Kit component that needs no hardware RT, no build
    // system change and no GPU-side resource: the jitter is a CPU-side uniform,
    // so every backend including OpenGL (TierC) benefits immediately.
    //
    // Data: assets/textures/bluenoise/stbn_scalar_128x128x64.r8, from
    // NVIDIA-RTX/STBN (CLA v1.0.1), converted from the 64 shipped PNG frames by
    // tools/extract_stbn.py. Regenerate with that script after re-vendoring STBN.
    class BlueNoise
    {
    public:
        // Loads the volume from the project's asset tree. Returns false (and
        // leaves the sampler usable via Halton fallback) if the file is absent,
        // so a missing asset degrades quality rather than breaking the frame.
        static bool Load(const std::string &path = "");

        static bool IsLoaded();

        static void Unload();

        // Two decorrelated samples in [0,1). pixelX/pixelY are the integer pixel
        // being sampled; frameIndex selects the temporal frame. Both the spatial
        // and temporal decorrelation are what separate this from a plain hash.
        // Falls back to a hash-based value when the volume is not loaded.
        static void Sample(uint32_t pixelX, uint32_t pixelY, uint32_t frameIndex,
                           uint32_t index, float &outA, float &outB);

        // Replaces GetCurrentJitter's Halton path. Returns the sub-pixel offset
        // in pixels. Signature-compatible with the old helper so call sites do
        // not change.
        static void Jitter(uint32_t frameIndex, uint32_t width, uint32_t height,
                           float &outJitterX, float &outJitterY);

    private:
        static std::vector<uint8_t> s_Volume;
        static constexpr uint32_t kSize = 128;
        static constexpr uint32_t kFrames = 64;
        static constexpr uint32_t kLayerSize = kSize * kSize;
        static bool s_Loaded;
    };

} // namespace Wasteland