// Nova amplification shader -- DX12 HLSL source.
//
// WHY THIS EXISTS: the mesh shader runs ONE meshlet per group, so something has to decide
// which meshlets to run and dispatch them. That is the amplification shader's only job.
//
// THE PAYLOAD IS A LOCAL, NOT AN `out` PARAMETER. This was the Task 2.4 open question, and
// the answer is not any of the four forms the plan lists:
//
//   struct ASPayload { uint meshlet; };                FAILS: "Semantic must be defined for
//                                                       all outputs of an entry function"
//   struct ASPayload { uint meshlet : PAYLOAD; };      FAILS: "Semantic PAYLOAD is invalid
//                                                       for shader model: as"
//   struct ASPayload { uint meshlet : MESHLET; };      FAILS: "Semantic MESHLET is invalid
//                                                       for shader model: as"
//   struct ASPayload { uint meshlet : SV_DispatchMeshID; }  FAILS: same class
//
// The `out ASPayload` parameter itself is the problem: dxc treats it as a shader output that
// needs a semantic, and no payload semantic is valid for `as`. The working form is a LOCAL
// struct passed by value to DispatchMesh, exactly as dxc's own test suite does
// (DirectXShaderCompiler/tools/clang/test/CodeGenHLSL/mesh-val/multipleDispatchMesh.hlsl):
//
//   ASPayload payload;
//   payload.meshlet = ...;
//   DispatchMesh(1, 1, 1, payload);
//
// DispatchMesh MUST be called exactly once. A loop over the meshlet count fails validation
// with "Non-Dominating DispatchMesh call". So the CPU dispatches this shader once per
// meshlet and each invocation dispatches one mesh group -- tid.x is the meshlet index, and
// the dispatch count, not a loop, is what covers the mesh.

struct ASPayload
{
    // Instance index into Instances[]. The mesh shader reads this to fetch the transform
    // and the material.
    uint instance;
    // Meshlet index into Meshlets[]. One mesh shader group per meshlet.
    uint meshlet;
};

// b1, NOT b0. The mesh pipeline's root signature binds MeshConstants at b1 (the same
// per-shape region the mesh shader reads), and the AS shares it. An AS declaring b0
// makes CreatePipelineState return E_INVALIDARG because the register is uncovered --
// the AS's own b0 cbuffer is not a root parameter.
cbuffer MeshConstants : register(b1)
{
    float4x4 u_ViewProjection;
    int      u_Instance;
    int      u_Pad0;
    int      u_Pad1;
    int      u_Pad2;
};

[shader("amplification")]
[numthreads(1, 1, 1)]
void NovaMeshAS(uint3 tid : SV_GroupThreadID)
{
    ASPayload payload;
    payload.instance = (uint)u_Instance;
    payload.meshlet  = tid.x;
    DispatchMesh(1, 1, 1, payload);
}
