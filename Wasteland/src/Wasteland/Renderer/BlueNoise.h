#pragma once

#include <cstdint>
#include <vector>

#include "Wasteland/Core/Core.h"

namespace Wasteland
{

    // Spatio-temporal blue noise sampling for the TAA jitter.
    //
    // Replaces the Halton(2,3) sequence Renderer3D::GetCurrentJitter used for
    // sub-pixel sampling. That sequence was indexed `frameIndex % 16`, so it
    // cycled through only 16 distinct offsets and then repeated them exactly --
    // the same structured artefacts recurred in the accumulation buffer every
    // 16 frames instead of averaging out.
    //
    // The volume is a 128x128x64 stack where every layer is spatially blue
    // noise and successive layers are independent in time, so the offset a
    // given pixel sees decorrelates frame to frame.
    //
    // GENERATED, NOT VENDORED. This is deliberately our own data: NVIDIA's
    // STBN assets are non-commercial, research/evaluation only, and the rest of
    // the RTX Kit is under a licence that forbids the SDK being subject to an
    // open-source licence. Neither is compatible with this project shipping
    // open source. See docs/RTX_KIT_LICENSES.md.
    //
    // tools/generate_blue_noise.py builds it with the prototype+rank method
    // (Georgiev & Fajardo, EGF 2011). Measured on the generated file:
    //   - low-frequency energy ~11000x below white noise
    //   - temporal correlation -0.002 at lag 1, +0.000 at lag 2
    //   - a white-noise volume scores 1.0x on the same metric (negative
    //     control, so the number above means something)
    // The script re-verifies the asset and refuses to write one that fails.
    //
    // Works on every backend including OpenGL, because the jitter is a CPU-side
    // uniform rather than a GPU resource.
    class BlueNoise
    {
    public:
        // Loads the volume from the project's asset tree. Returns false if the
        // file is absent, leaving the sampler on its hash fallback, so a missing
        // asset degrades sampling quality rather than breaking the frame.
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