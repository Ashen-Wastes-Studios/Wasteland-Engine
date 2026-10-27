#pragma once

// Shared loader and ABI for the trained end-of-frame neural stage's weight blob.
//
// One loader, two backends. The OpenGL path (Renderer3D.cpp) and the DX11/DX12
// path (DX12Nova.cpp) both upload weights from this file, and a blob that one
// backend accepts and the other rejects is a bug that only shows up as a
// half-working stage on one backend. The header, the weight count and the CRC32
// are all validated here so there is exactly one definition of "valid".
//
// The blob is what tools/train_neural/export_weights.py writes to
// assets/neural/neural_weights.bin. Its layout is an ABI shared with the Python
// trainer, not just with the C++ side:
//
//   offset  size  field
//   0       4     magic "WLNR"
//   4       4     version (1)
//   8       4     n_in (8)
//   12      4     n_hidden (32)
//   16      4     n_residual (3)
//   20      4     n_gate (1)
//   24      4     n_weights (1140)
//   28      4     crc32 of the payload
//   32      ...   payload, n_weights little-endian fp32 values in LAYER_SHAPES
//                 order (see model.py)
//
// The CRC32 is not decoration. A flipped bit in an uploaded constant buffer is
// a silent visual corruption with no shader error, no link failure and no log
// line -- the same failure mode as a ninth GL image uniform. Refusing a blob
// whose CRC does not match is the only thing standing between "no enhancement"
// and "wrong image", and "no enhancement" is the safe default.
//
// This header is deliberately free of platform types: it is included by both
// the renderer and the NVRHI backend files, which do not otherwise share a
// translation unit.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Wasteland::Neural
{

// Must match NEURAL_MAX_BLOCKS in the GLSL twin. The partial-sum array holds
// four fixed-point sums per workgroup, so it has to cover the largest frame
// this engine renders: 3840x2160 at 8x8 workgroups is 480*270 = 129600 blocks.
// 8192 was tried first and silently under-counted a 2332x1451 frame by 6.5x.
// 131072 is the next power of two above 129600.
inline constexpr uint32_t kPartialBlocks = 131072;

// Bytes the globals buffer needs: the four frame means (16 bytes) followed by
// one uint32 partial per workgroup block.
// g_Mean[4] (16 bytes) + g_Partial[kPartialBlocks*4] + g_GateMean (4) +
// g_GatePartial[kPartialBlocks]. Declared in uints to match the
// RWStructuredBuffer<uint> that DX12 and the HLSL twin bind: element i is at
// byte 4*i, so asfloat/asuint are the only conversions either side needs.
inline constexpr uint32_t kGlobalsUints = 4 + kPartialBlocks * 4 + 1 + kPartialBlocks;
inline constexpr uint32_t kGlobalsBytes  = kGlobalsUints * sizeof(uint32_t);

// Element index of g_GateMean, and the first element of g_GatePartial. The
// renderer reads g_GateMean back for the editor's readout; the shaders index
// g_GatePartial. Both come from here so the GL readback and the shader cannot
// disagree about where the gate lives.
inline constexpr uint32_t kGateMeanUint        = 4 + kPartialBlocks * 4;
inline constexpr uint32_t kGatePartialBaseUint = kGateMeanUint + 1;

inline constexpr uint32_t kWeightCount = 1140;
inline constexpr uint32_t kNIn = 8;
inline constexpr uint32_t kNHidden = 32;
inline constexpr uint32_t kNResidual = 3;
inline constexpr uint32_t kNGate = 1;
inline constexpr uint32_t kBlobHeaderBytes = 32;

// Standard CRC32 (IEEE 802.3, reflected, init/final 0xFFFFFFFF). Same polynomial
// the Python side uses (zlib.crc32), so a blob written by the trainer validates
// here without either side importing the other's code.
inline uint32_t Crc32(const uint8_t *p, size_t bytes)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < bytes; ++i)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

struct WeightBlob
{
    std::vector<uint8_t> payload; // the fp32 weights, little-endian, LAYER_SHAPES order
    uint32_t nWeights = 0;
};

// Reads and validates the weight blob from `path`. Returns false on any problem
// -- missing file, short read, bad magic, wrong version, a shape that does not
// match the network the shader implements, or a CRC mismatch. The caller must
// treat a false return as "stage disabled", never as "carry on with zeros".
//
// `path` is relative to the process working directory, which is the run
// directory (bin/Release-windows-x86_64/<app>), matching where the GL path has
// always looked for assets.
inline bool LoadWeightBlob(const char *path, WeightBlob &out)
{
    out = WeightBlob{};

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t header[kBlobHeaderBytes];
    bool ok = fread(header, 1, sizeof(header), f) == sizeof(header);
    static const char kMagic[4] = {'W', 'L', 'N', 'R'};
    uint32_t field[7] = {0, 0, 0, 0, 0, 0, 0};
    if (ok)
    {
        ok = memcmp(header, kMagic, 4) == 0;
        memcpy(field, header + 4, sizeof(field));
    }
    // field: 0 version, 1 n_in, 2 n_hidden, 3 n_residual, 4 n_gate,
    //        5 n_weights, 6 crc32 of the payload.
    //
    // The shape fields are checked, not just trusted. The shader's network is
    // fixed at compile time (NEURAL_N_IN/NEURAL_N_HID/NEURAL_N_W in the GLSL),
    // so a blob for a different architecture would be read as the wrong weights
    // with no error -- exactly the corruption the CRC is there to catch, and the
    // shape check catches it earlier and more legibly.
    if (ok && field[0] != 1u)      ok = false; // version
    if (ok && field[1] != kNIn)    ok = false; // N_IN
    if (ok && field[2] != kNHidden) ok = false; // N_HIDDEN
    if (ok && field[3] != kNResidual) ok = false; // N_RESIDUAL
    if (ok && field[4] != kNGate)  ok = false; // N_GATE
    if (ok && field[5] != kWeightCount) ok = false; // N_WEIGHTS

    std::vector<uint8_t> payload;
    if (ok)
    {
        const size_t bytes = (size_t)field[5] * 4u;
        payload.resize(bytes);
        ok = fread(payload.data(), 1, bytes, f) == bytes;
    }
    fclose(f);

    if (!ok)
        return false;
    if (Crc32(payload.data(), payload.size()) != field[6])
        return false;

    out.payload = std::move(payload);
    out.nWeights = field[5];
    return true;
}

} // namespace Wasteland::Neural
