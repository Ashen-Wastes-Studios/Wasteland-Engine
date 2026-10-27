#pragma once
// The D3D11 headers this whole class is written against. Note this is the
// NVRHI directory but the code here is raw D3D11, not NVRHI -- the Nova
// renderer bypasses the RHI abstraction entirely, which is what the class
// comment below says about independence.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <limits>

#include "Wasteland/Project/Project.h"
#include "Wasteland/Core/Log.h"

namespace Wasteland {
// Native compute resources are deliberately independent of NVRHI's graphics
// reflection/cache. Every dispatch reads immutable snapshots, never its UAVs.
// The Nova compute renderer, in about 90 lines and no abstraction at all.
//
// Everything here is inline and hand-packed: multiple statements per line,
// no error handling beyond the Check() helper below, and private members
// declared without initialisers. Read it as a purpose-built hot path rather
// than as engine-idiomatic code -- the density is a deliberate trade for
// keeping a multi-pass compute pipeline in one header.
//
// Note it is NOT derived from any RHI type and does not implement an
// engine interface. The window's SwitchRendererAPI path must therefore
// special-case it, which is consistent with the AutoSwitchLayer test hook
// in the editor exercising exactly that path.
class DX11Nova {
    // Local alias for ComPtr, so the members below read as Com<ID3D11Device>
    // rather than the full namespace each time.
    //
    // ComPtr rather than a raw pointer because every COM object here has a
    // non-trivial lifetime: the members outlive the device in some teardown
    // orders, and ComPtr releases determinately rather than leaking.
    template<class T> using Com = Microsoft::WRL::ComPtr<T>;
    // One render target, as a texture/UAV/SRV triple plus a private SNAPSHOT copy.
    //
    // The snapshot is the whole design. Every dispatch reads snapshots and
    // writes the live texture, so a pass can never read its own output -- which
    // is what makes multi-pass ordering safe without explicit barriers. The cost
    // is double the memory and a full-texture copy per image per dispatch (see
    // Dispatch).
    //
    // textures[i].uav for writes, textures[i].snapshot for reads. All four
    // members are separate objects with separate lifetime.
    struct Image { Com<ID3D11Texture2D> texture, snapshot; Com<ID3D11UnorderedAccessView> uav; Com<ID3D11ShaderResourceView> srv; };
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11ComputeShader> shader;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> sampler;
    // The eight ping-pong targets the shader's registers address. Eight is not
    // arbitrary: Dispatch binds them at t64..t71, and the array size and the
    // binding loop below have to agree -- see the loop at line 87.
    //
    // DEFECT: the literal 8 appears in at least four places (this array, the
    // Resize loop, the UAV array, and the CSSetShaderResources loop). Nothing
    // ties them together, so adding a ninth image means finding all four.
    // A named kImageCount constant would make the coupling explicit.
    std::array<Image,8> images;
    // The temporal-accumulation buffer, held separately from the eight.
    //
    // It is not images[9]: CommitHistory() and CommitDenoise() copy it to and
    // from specific images by index, and Dispatch binds it at t48. Keeping it a
    // distinct member makes the read in Dispatch legible at the cost of one
    // more place to keep in sync.
    Image history;
    // The SVGF moments buffer (img_Moments at u9, read_img_Moments at t73).
    // Held separately from the eight for the same reason history is: it is not
    // one of the ping-pong targets the t64..t71 snapshots address, and keeping
    // it a distinct member makes the read in Dispatch legible.
    Image moments;
    // Three structured input buffers, bound at t1..t3 by the Dispatch loop.
    // The pairing with scene[] below is by index, and the shader's register
    // layout is what fixes which slot means what.
    std::array<Com<ID3D11Buffer>,3> buffers;
    std::array<Com<ID3D11ShaderResourceView>,3> scene;
    // ReSTIR reservoir ping-pong buffers. Two buffers, each 32 bytes per pixel.
    // Read from one (t10), write to the other (u10), swap each frame.
    Com<ID3D11Buffer> reservoir[2];
    Com<ID3D11ShaderResourceView> reservoirSRV[2];
    Com<ID3D11UnorderedAccessView> reservoirUAV[2];
    UINT reservoirWidth=0, reservoirHeight=0;
    bool reservoirReadFlip=false;
    // Reflection results: every constant-buffer variable name mapped to its
    // offset and size.
    //
    // This is what lets Set() work by NAME below rather than by hardcoded
    // offset. Built once in Init from D3DReflect, so a shader reordering its
    // constants needs no change here.
    std::unordered_map<std::string,D3D11_SHADER_VARIABLE_DESC> variables;
    // The CPU-side staging copy of the constant buffer, written by Set() and
    // uploaded by Dispatch.
    //
    // DEFECT: sized in Init from the reflection's desc.Size, but the GPU buffer
    // below is allocated 16-byte aligned at (desc.Size+15)&~15. Set() bounds
    // checks against the unaligned desc.Size (via it->second.Size), so that is
    // consistent -- but nothing checks that a value written near the end of
    // bytes cannot straddle the alignment gap. Harmless today, fragile if the
    // check ever moves to the GPU-side size.
    std::vector<unsigned char> bytes;
    // Resolution, zero-initialised -- and 0 doubles as 'not yet sized'. Several
    // methods below short-circuit on it, so it is state, not just a dimension.
    UINT width=0,height=0;
    // The only error handling in the class: wrap, test, throw.
    //
    // Throwing rather than asserting or returning an HRESULT means a failed
    // device call unwinds out of Init or Dispatch into whatever called them --
    // which for a compute pass mid-frame means the exception escapes a frame
    // that has already issued GPU work. The ComPtr members clean up on unwind,
    // so there is no leak, but the frame is left in whatever state it had.
    //
    // The HRESULT is stringified but not decoded, so the message is a bare
    // hex number rather than a D3D error name -- which makes triage harder than
    // it needs to be. DXGI_FORMAT_ERROR is also not distinguished from a real
    // failure.
    //
    // Every COM call below goes through this, which is the one place the
    // consistency comes from.
    static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("DX11 Nova HRESULT " + std::to_string((unsigned long)hr)); }
    // Builds one Image: live texture, snapshot texture, and the two views.
    //
    // Takes the device implicitly from the member rather than as a parameter,
    // so it cannot be called before Init -- a caller that got the ordering wrong
    // would deref a null device inside Check.
    Image CreateImage(UINT w, UINT h, DXGI_FORMAT format) {
        // A zero-initialised descriptor, then only the fields this renderer needs.
        // The rest stay at their documented defaults: CPU access is NONE (a compute
        // UAV writes on the GPU only), Usage is DEFAULT, and no misc flags.
        //
        // BindFlags is set to UAV | SHADER_RESOURCE -- the texture is both written
        // and read, just never by the same pass.
        //
        // MipLevels=1 and ArraySize=1 mean these are flat 2D targets; there is no
        // mip chain or array layer anywhere in this class.
        Image i; D3D11_TEXTURE2D_DESC d={}; d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1; d.Format=format; d.SampleDesc.Count=1; d.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        // Two statements on one line: the live texture, then its UAV. The second
        // takes a null description, which means 'default format and all mip levels'
        // -- valid only because this texture has MipLevels=1.
        //
        // The UAV is what the compute shader writes; it is the target of the UAV
        // array bound at line 88.
        Check(device->CreateTexture2D(&d,nullptr,&i.texture)); Check(device->CreateUnorderedAccessView(i.texture.Get(),nullptr,&i.uav));
        // The descriptor is REUSED for the snapshot, with the bind flags narrowed to
        // read-only. Reusing one descriptor for both textures is why they are
        // guaranteed the same size and format -- the snapshot can never drift out
        // of step with the target it mirrors.
        //
        // The snapshot deliberately gets NO UAV: it exists only to be sampled, so
        // it cannot be written by a pass that is supposed to be reading history.
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE; Check(device->CreateTexture2D(&d,nullptr,&i.snapshot)); Check(device->CreateShaderResourceView(i.snapshot.Get(),nullptr,&i.srv)); return i;
    }
// Everything above is private implementation; the public surface below is
// the six methods a caller actually uses.
public:
    // One-time setup: adopt the device, compile the shader, reflect it, allocate
    // the constant buffer and sampler.
    //
    // @param d  Device to create resources on. Borrowed -- assigned to a ComPtr,
    //          so this class now shares ownership with the caller.
    // @param c  Immediate context. Same: borrowed, and stored.
    void Init(ID3D11Device* d, ID3D11DeviceContext* c) {
        // Idempotence guard. Calling Init twice with the same device is a no-op, so
        // a window that re-presents the same device does not recompile the shader.
        //
        // DEFECT: the guard only checks the DEVICE, not the context. Passing a
        // different ID3D11DeviceContext with the same device silently keeps the old
        // context, and every later call would record commands into a context the
        // renderer is no longer presenting. In practice a device swap comes with a
        // new device, so this is latent rather than active -- but it is the exact
        // case (device recreation with a fresh immediate context) where it would
        // bite.
        if(device.Get()==d && shader) return;
        device=d; context=c;
        // The compiled bytecode and the compiler's diagnostics. ComPtr rather than
        // a raw blob so both are released on any throw below.
        Com<ID3DBlob> code,error;
        // Resolved through Project so the compute shader comes from the active
        // project's assets rather than the process working directory.
        // The shader path, resolved through Project so it comes from the ACTIVE
        // PROJECT's assets rather than the process working directory.
        //
        // Widened to std::wstring and back to .c_str() on the next line because
        // D3DCompileFromFile takes a wide path while GetAssetPath returns a
        // std::filesystem::path. Two conversions per compile, which happens once.
        const std::wstring novaShaderPath =
            Project::Get().GetAssetPath("shaders/Renderer3D_NovaRenderer.hlsl").wstring();
        // HLSL compile, targeting cs_5_0 -- compute shader model 5, which is what
        // gives D3D11 its UAV support.
        //
        // Entry point "main" and D3DCOMPILE_ENABLE_STRICTNESS: strictness means the
        // compile fails on legacy syntax rather than silently accepting it, which is
        // the right default for a file a studio may edit.
        //
        // Optimization flags are 0, so this is an unoptimised compile. Fine -- it
        // runs once per device, not per frame.
        HRESULT hr=D3DCompileFromFile(novaShaderPath.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        // The compile error blob is turned into the exception message, which is why
        // the diagnostic actually reaches the log -- Check() on this line alone
        // would only have produced a hex HRESULT.
        //
        // The ternary guards against a null error blob. Note the HRESULT itself is
        // NOT in this message, so a compile failure reports the HLSL diagnostics
        // without the reason D3DCompileFromFile gave.
        if(FAILED(hr)) throw std::runtime_error(error ? (const char*)error->GetBufferPointer() : "Nova shader compile failed");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        // Reflection, used for exactly one purpose: discovering the constant
        // buffer's layout so Set() can write by name.
        //
        // The reflection object is a local and dies at the end of Init -- only the
        // harvested variable map persists, in members above.
        Com<ID3D11ShaderReflection> reflection; Check(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_ID3D11ShaderReflection,(void**)reflection.GetAddressOf()));
        // Looks the constant buffer up BY NAME. This is the shader contract in one
        // line: renaming cbuffer NovaConstants in the .hlsl breaks this lookup, and
        // cb would be null -- so GetDesc would dereference null rather than fail
        // cleanly. Worth a Check(cb) here; there is none.
        //
        // desc.Size drives the staging vector, bd below drives the GPU buffer.
        auto cb=reflection->GetConstantBufferByName("NovaConstants"); D3D11_SHADER_BUFFER_DESC desc={}; Check(cb->GetDesc(&desc)); bytes.resize(desc.Size);
        // Harvests every variable's name and offset into the map. One loop over the
        // reflection data, done once at Init.
        //
        // DEFECT: GetVariableByIndex(j) is dereferenced immediately without a null
        // check. The index range comes from desc.Variables so it should be in range,
        // but a Check() on the result would cost nothing and this loop runs during
        // Init, where a crash is hard to attribute.
        for(UINT j=0;j<desc.Variables;++j) { D3D11_SHADER_VARIABLE_DESC v={}; Check(cb->GetVariableByIndex(j)->GetDesc(&v)); variables[v.Name]=v; }
        // The GPU constant buffer.
        //
        // (desc.Size+15)&~15u is the standard round-up to D3D11's 16-byte
        // constant-buffer alignment requirement. Must be a multiple of 16 or
        // CreateBuffer fails -- which is why the rounding is here rather than in the
        // desc.Size consumer.
        D3D11_BUFFER_DESC bd={}; bd.ByteWidth=(desc.Size+15)&~15u; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; Check(device->CreateBuffer(&bd,nullptr,&constants));
        // Anisotropic filtering with mirror addressing and no LOD clamp.
        //
        // MIRROR rather than CLAMP or WRAP matters for the denoise and history
        // samples: it keeps the filter symmetric at the edges, which avoids the
        // darkening a clamp would introduce when sampling outside the viewport.
        //
        // MaxLOD=FLT32_MAX means no maximum level is enforced -- correct for a
        // mip-chain-free texture set like this one.
        D3D11_SAMPLER_DESC sd={}; sd.Filter=D3D11_FILTER_ANISOTROPIC; sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_MIRROR; sd.MaxAnisotropy=16; sd.MaxLOD=D3D11_FLOAT32_MAX; Check(device->CreateSamplerState(&sd,&sampler));
    }
    // Resizes every image and the history buffer, then clears.
    //
    // @param w  New width in pixels.
    // @param h  New height in pixels.
    //
    // Reallocates rather than resizes in place: D3D11 has no resize, so each
    // image is a fresh texture and every view is rebuilt.
    void Resize(UINT w,UINT h) {
        // Three guards in one condition: reject zero (a zero-extent texture is
        // invalid, and CreateImage would fail), and skip if the size is unchanged.
        //
        // The zero guard is why the class survives a minimised window being resized
        // to nothing -- a real save, and the reason Dispatch's `(w+7)/8` below never
        // sees a zero thread-group count.
        if(!w||!h||(width==w&&height==h)) return;
        Unbind(); width=w;height=h;
        // The eight images, each with a format chosen by INDEX.
        //
        // i==5 is R32G32_FLOAT, i==3 is R8G8B8A8_UNORM, everything else is
        // R32G32B32A32_FLOAT. So the pipeline carries velocity in two channels at
        // slot 5, something needing an 8-bit unorm at slot 3 (a mask or a debug
        // channel), and float RGBA elsewhere.
        //
        // DEFECT: the formats are positional magic numbers with no named constants
        // or comment mapping slot indices to their roles. Nothing in the shader
        // header here says which register holds what, so the only source of truth is
        // the .hlsl. This is the least maintainable line in the file.
        for(int i=0;i<8;++i) images[i]=CreateImage(w,h,i==5?DXGI_FORMAT_R32G32_FLOAT:(i==3?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R32G32B32A32_FLOAT));
        // History is allocated then cleared, so a resize cannot blend the new frame
        // against the old resolution's accumulation buffer. Clear() itself copies
        // texture into snapshot as well, so both sides start zeroed -- the copy
        // matters because dispatches read snapshots.
        history=CreateImage(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT);
        moments=CreateImage(w,h,DXGI_FORMAT_R32G32_FLOAT);
        CreateReservoir(w,h);
        reservoirReadFlip=false;
        Clear();
    }
    // Zeroes every image/history/moments UAV and both allocated reservoir UAVs,
    // then copies texture targets into snapshots.
    //
    // Snapshots are refreshed from their cleared textures; structured reservoir
    // buffers are explicitly zero-uploaded because they have no texture snapshots.
    //
    // Structured UAVs have DXGI_FORMAT_UNKNOWN, so typed UAV clear is invalid;
    // UpdateSubresource uploads zero bytes to each allocated reservoir buffer.
    // This path runs on resize or explicit reset, not once per dispatch.
    void Clear() {
        if(!context||!width)return;
        Unbind();
        const float zero[4]={};
        for(auto& i:images)context->ClearUnorderedAccessViewFloat(i.uav.Get(),zero);
        context->ClearUnorderedAccessViewFloat(history.uav.Get(),zero);
        context->ClearUnorderedAccessViewFloat(moments.uav.Get(),zero);
        if(reservoir[0]||reservoir[1]) {
            D3D11_BUFFER_DESC reservoirDesc={};
            if(reservoir[0])reservoir[0]->GetDesc(&reservoirDesc);
            else reservoir[1]->GetDesc(&reservoirDesc);
            constexpr UINT clearChunkBytes = 1u << 20;
            std::vector<unsigned char> zeroReservoir((std::min)(reservoirDesc.ByteWidth,clearChunkBytes));
            for(int i=0;i<2;++i)if(reservoir[i]) {
                for(UINT offset=0;offset<reservoirDesc.ByteWidth;) {
                    const UINT chunkBytes=(std::min)(clearChunkBytes,reservoirDesc.ByteWidth-offset);
                    D3D11_BOX box={};box.left=offset;box.right=offset+chunkBytes;box.top=0;box.bottom=1;box.front=0;box.back=1;
                    context->UpdateSubresource(reservoir[i].Get(),0,&box,zeroReservoir.data(),0,0);
                    offset+=chunkBytes;
                }
            }
        }
        context->CopyResource(history.snapshot.Get(),history.texture.Get());
        context->CopyResource(moments.snapshot.Get(),moments.texture.Get());
    }
    // Writes one constant by NAME, into a staging buffer.
    //
    // @param name   Shader variable name, as reflected.
    // @param value  Value to upload.
    // @param index  Array element offset, for a cbuffer array.
    //
    // Templated on the caller's type, so the memcpy size is always right and a
    // float passed where the shader wants an int is a compile-time-visible
    // mismatch only if the caller knows the shader's types -- which nothing
    // enforces.
    template<class T> void Set(const char* name,const T& value,UINT index=0) {
        // REPORTED, ONCE PER NAME. This used to be a silent return, and that
        // silence is how a whole class of Nova bug reaches shipping: a name
        // typo, or a constant removed from the shader while the call stayed,
        // leaves the shader reading whatever stale value is in the staging
        // buffer -- typically zero from the last resize -- with no diagnostic
        // at all. It produces a silently wrong render rather than a failure.
        //
        // Warned rather than thrown. Dead uniforms are a legitimate call
        // pattern here: u_MaxBounces and u_SamplesPerPixel are uploaded to
        // hold cbuffer layout while the shader ignores them, and the calls
        // must keep compiling. Throwing would turn a cosmetic no-op into a
        // startup crash. Once per name because Set is called every frame, so
        // an unconditional warn would flood the log at 60 lines a second.
        auto it=variables.find(name);
        if(it==variables.end()) {
            static std::unordered_set<std::string> warned;
            if(warned.insert(name).second)
                WL_CORE_WARN("Nova constant '{0}' is not declared by the shader -- the upload is "
                             "dropped and the shader keeps its stale value (typo, or the uniform "
                             "was removed from the shader while the call stayed)", name);
            return;
        }
        // Array elements are assumed to be 16 bytes apart. That is the HLSL packing
        // rule for a float4 array and true for most Nova constants, but it is
        // hardcoded rather than derived from the reflection's array stride -- so a
        // shader using a differently packed array would write to the wrong offset.
        //
        // The bounds check then confirms the write stays inside the variable, and
        // throws otherwise. Note the check is against it->second.Size, so it
        // correctly rejects an out-of-range index.
        //
        // An index of 0 always passes, since it is the base offset.
        UINT offset=it->second.StartOffset+(index*16); if(offset+sizeof(T)>it->second.StartOffset+it->second.Size)throw std::runtime_error("Nova constant size mismatch");
        // Copies into the CPU staging buffer. NOT uploaded here -- Dispatch does
        // that, so several Set() calls cost one upload per frame rather than one
        // each. That batching is the reason this design works.
        memcpy(bytes.data()+offset,&value,sizeof(T));
    }
    // Replaces one of the three structured input buffers.
    //
    // @param slot   Buffer index, 0..2, bound at t1..t3.
    // @param data   Source bytes.
    // @param count  Element count.
    // @param stride Bytes per element.
    //
    // Called per dispatch with scene data, so it is a per-frame allocation path
    // -- see the empty-vector note below.
    void Upload(UINT slot,const void* data,UINT count,UINT stride) {
        buffers[slot].Reset();scene[slot].Reset();
        // A zero-filled buffer allocated when count is 0, because D3D11 requires
        // initial data to be non-null.
        //
        // DEFECT: this allocates and zero-fills `stride` bytes on EVERY call even
        // when count is nonzero -- the ternary only decides which pointer is used,
        // the vector is built unconditionally. On a per-frame upload that is a
        // heap allocation and a memset each time for nothing. Constructing it only
        // in the count==0 branch would remove it.
        std::vector<unsigned char> empty(stride); D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=count?data:empty.data();
        // ByteWidth is forced to at least one element so a zero-count upload still
        // creates a valid buffer rather than a zero-byte one.
        //
        // D3D11_RESOURCE_MISC_BUFFER_STRUCTURED with StructureByteStride is what
        // makes this readable as a typed SRV in HLSL (StructuredBuffer<T>).
        //
        // BindFlags is SHADER_RESOURCE only -- the CPU fills it at creation and the
        // shader only reads it, so no UAV is needed.
        D3D11_BUFFER_DESC bd={}; bd.ByteWidth=(count?count:1)*stride;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=stride;
        // Both buffers and the SRV are Reset() first by the caller, so the old
        // resource is released before the new one is created -- otherwise the
        // device would transiently hold two.
        //
        // DEFECT: no check that slot < 3. buffers[slot] on an out-of-range index is
        // an out-of-bounds std::array access -- undefined behaviour, not a clean
        // failure. The slot values come from the caller.
        Check(device->CreateBuffer(&bd,&initial,&buffers[slot]));Check(device->CreateShaderResourceView(buffers[slot].Get(),nullptr,&scene[slot]));
    }
    // Releases EVERY compute binding: all 128 SRV slots, all 11 UAV slots, and
    // the shader.
    //
    // This is what makes the snapshot scheme safe. D3D11 refuses to bind a
    // resource as both SRV and UAV at once, and it auto-unbinds conflicting
    // slots -- silently, in ways that are easy to get wrong. Explicitly clearing
    // all 128 rather than only the used slots means a binding from a previous
    // frame can never linger.
    //
    // Called at the start and end of Dispatch, and again by Clear and the
    // Commit methods -- which is why the `if(!context)return` guard exists, for
    // calls before Init.
    void Unbind() { if(!context)return; ID3D11ShaderResourceView* nulls[128]={};ID3D11UnorderedAccessView* uavs[11]={};context->CSSetShaderResources(0,128,nulls);context->CSSetUnorderedAccessViews(0,11,uavs,nullptr);context->CSSetShader(nullptr,nullptr,0); }
    // Records one compute pass.
    //
    // @param pass     Pass index, uploaded as u_PassID so the shader branches.
    // @param w,h      Work dimensions in pixels.
    // @param textures Extra SRVs for the denoise-style passes, bound at t15+.
    //
    // The pipeline is a sequence of these, one per pass, each reading snapshots
    // and writing live textures. There is no pass table and no loop here -- the
    // caller drives the sequence, which is why the binding slots below are
    // absolute numbers rather than computed.
    void Dispatch(int pass,UINT w,UINT h,const std::vector<ID3D11ShaderResourceView*>& textures) {
        // Start from a clean slate. Every binding this method makes is then known
        // to be deliberate.
        Unbind();
        // The UI can leave last frame's output on PS t0. Remove graphics SRVs
        // before any copies/UAV writes; graphics code rebinds its own resources.
        // Clears ALL 128 graphics SRV slots before anything else.
        //
        // The comment above explains why: the UI can leave last frame's output bound
        // at PS t0, and any CopyResource or UAV write while a texture is bound as a
        // graphics SRV is a D3D11 undefined-behaviour case. Graphics code rebinds
        // its own resources afterwards, so clobbering them here is safe.
        ID3D11ShaderResourceView* nulls[128]={};context->PSSetShaderResources(0,128,nulls);
        // Refreshes every snapshot from its live texture.
        //
        // This is the barrier substitute. There is no explicit fence or barrier
        // because each Dispatch call is a separate immediate-context submission and
        // D3D11 orders them; the copy moves last pass's results into the read-only
        // side so the next pass cannot read its own output.
        //
        // DEFECT: CopyResource is a GPU-side copy with no CPU wait, so these copies
        // queue rather than execute. That is correct for ordering and means no
        // stall -- but it also means the copies are pure overhead when a pass does
        // not read the image being copied. Eight copies per dispatch, per pass.
        for(auto& i:images)context->CopyResource(i.snapshot.Get(),i.texture.Get());context->CopyResource(moments.snapshot.Get(),moments.texture.Get());
        // The pass index goes into the constant buffer by name, then the whole
        // staging buffer is uploaded in one call.
        //
        // UpdateSubresource with a null destination subresource and pSrcData set is
        // the whole-buffer upload form. The trailing 0,0 for src/dst boxes means
        // 'the entire resource', which is why every Set() this frame lands.
        Set("u_PassID",pass);context->UpdateSubresource(constants.Get(),0,nullptr,bytes.data(),0,0);
        // Constant buffer at CS b1 -- slot 1, not 0. b0 is conventionally reserved
        // for per-frame camera globals in most engines, so this class follows that
        // convention.
        //
        // The sampler goes at s0. Both are raw Get() pointers because ComPtr cannot
        // be passed where an array of raw pointers is expected -- no ownership is
        // transferred, the members keep it.
        ID3D11Buffer* cb=constants.Get();context->CSSetConstantBuffers(1,1,&cb);ID3D11SamplerState* ss=sampler.Get();context->CSSetSamplers(0,1,&ss);
        // The compute SRV table. Three regions, and they cannot overlap:
        //
        //   t1..t3  -- the three Upload() scene buffers.
        //   t15..t46 -- caller textures, capped at 32 by the i<32 guard.
        //   t48+    -- history and image reads below.
        //
        // The array is zero-initialised, so every unwritten slot binds null rather
        // than a stale pointer -- which is the other half of why Unbind() at the
        // end of the previous Dispatch mattered.
        //
        // DEFECT: the cap at i<32 silently DROPS caller textures beyond 32 rather
        // than reporting it, and the t15..t46 range is hardcoded rather than derived
        // from the shader's declared bindings. Adding a 33rd texture would be a
        // silent visual bug.
        ID3D11ShaderResourceView* srvs[128]={};for(int i=0;i<3;++i)srvs[i+1]=scene[i].Get();srvs[10]=reservoirSRV[reservoirReadFlip?1:0].Get();for(size_t i=0;i<textures.size()&&i<32;++i)srvs[15+i]=textures[i];
        // A special case: pass 9 additionally reads textures[0] at t52, which is
        // OUTSIDE the 15..46 range above and so cannot be reached by the general
        // loop.
        //
        // DEFECT: this overwrites nothing, but it is a raw pass number in a
        // binding table. There is no name for pass 9, no comment saying what it does,
        // and no check that pass 9 exists in the shader. Changing the pass order in
        // the .hlsl or in the caller silently breaks the binding. It is also the
        // only place a binding depends on the pass VALUE rather than on the slot,
        // which makes the whole table harder to reason about than it looks.
        if(pass==9 && !textures.empty())srvs[52]=textures[0];
        // The pipeline reads at t48..t51. Note t49 and t51 are BOTH images[6].srv.
        //
        // DEFECT, or at least a strong smell: the same texture is bound twice in
        // adjacent slots. That is either intentional (the shader samples the same
        // buffer for two purposes) or a copy-paste slip where one should be another
        // image. Nothing here or in the shader header distinguishes the two cases,
        // and the surrounding line offers no explanation. Worth confirming against
        // the .hlsl before changing anything.
        srvs[48]=history.srv.Get();srvs[49]=images[6].srv.Get();srvs[50]=images[0].srv.Get();srvs[51]=images[6].srv.Get();
        // t53 is s_Moments: pass 12 samples the PREVIOUS frame's moments at a
        // reprojected UV to blend them with the current frame's luminance. It is
        // the snapshot rather than the live texture on purpose -- reading the
        // texture a pass is concurrently writing is a read-write hazard, and the
        // snapshot is the copy taken before this frame's writes.
        srvs[53]=moments.srv.Get();
        // The eight image snapshots at t64..t71, read-only because they come from
        // the snapshot textures rather than the live UAV textures.
        //
        // This is the binding that matches the std::array<Image,8> above -- both
        // sizes are the literal 8, and the array size and this loop must agree.
        //
        // The whole 128-entry table is committed in one call, so no slot is left
        // holding anything from a previous pass.
        for(int i=0;i<8;++i)srvs[64+i]=images[i].srv.Get();srvs[73]=moments.srv.Get();context->CSSetShaderResources(0,128,srvs);
        // The eight image UAVs plus moments and reservoir; u8 is deliberately null.
        //
        // These are i.texture's UAVs, NOT the snapshots' -- and the snapshots have
        // no UAV at all, which is what makes the no-feedback rule structural rather
        // than a convention anyone has to remember.
        //
        // The UAV array is zero-initialised, keeping u8 null while all used slots
        // are assigned explicitly below.
        // Eleven UAV slots: u0..u7 are the eight images, u8 is deliberately null,
        // u9 is moments, and u10 is the ReSTIR reservoir working buffer. Keep the
        // register numbers identical to HLSL and GLSL/SPIR-V (u9 moments, u10
        // reservoir); moving the reservoir to u8 collides with Vulkan's b1 offset.
        ID3D11UnorderedAccessView* uavs[11]={};for(int i=0;i<8;++i)uavs[i]=images[i].uav.Get();uavs[9]=moments.uav.Get();uavs[10]=reservoirUAV[reservoirReadFlip?0:1].Get();context->CSSetUnorderedAccessViews(0,11,uavs,nullptr);
        // Binds the compute shader and dispatches.
        //
        // (w+7)/8 rounds the pixel count up to whole 8x8 thread groups -- D3D11
        // requires a group count, and the shader's group size is 8x8. Using /8
        // instead of a named group-size constant ties this line to the shader's
        // declaration, which nothing enforces.
        //
        // The trailing Unbind() means the method leaves no bindings behind, so the
        // caller never has to reason about what a dispatch left set. The cost is
        // the caller must rebind everything for the next pass -- which is why every
        // line above is absolute.
        context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch((w+7)/8,(h+7)/8,1);Unbind();
    }
    // Folds images[1] into the history accumulator, for the next frame's
    // temporal pass.
    //
    // A pure GPU copy, so no CPU readback and no stall. Called once per frame
    // after the passes that write images[1].
    void CommitHistory() {Unbind();context->CopyResource(history.snapshot.Get(),images[1].texture.Get());}
    // Copies the denoise result from images[1] to images[7], so the final image
    // the UI samples is the denoised one.
    //
    // Written as a separate method rather than folded into CommitHistory because
    // the two serve different pipelines -- history is read next frame, denoise
    // is read by Output. Both are single CopyResource calls with no validation
    // that the source image exists.
    void CommitDenoise() {Unbind();context->CopyResource(images[1].texture.Get(),images[7].texture.Get());}
    // Creates the ReSTIR reservoir ping-pong buffers. Each buffer is 32 bytes per pixel.
    void CreateReservoir(UINT w, UINT h) {
        if(!w||!h)throw std::invalid_argument("DX11Nova reservoir dimensions must be nonzero");
        const std::uint64_t pixelCount=std::uint64_t(w)*std::uint64_t(h);
        if(pixelCount>(std::numeric_limits<UINT>::max)()/32u)
            throw std::length_error("DX11Nova reservoir exceeds D3D11 ByteWidth limit");
        const UINT byteSize=static_cast<UINT>(pixelCount*32u);
        reservoirWidth = w;
        reservoirHeight = h;
        for (int i = 0; i < 2; ++i) {
            reservoir[i].Reset();
            reservoirSRV[i].Reset();
            reservoirUAV[i].Reset();
            
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = byteSize;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 32;
            
            Check(device->CreateBuffer(&bd, nullptr, &reservoir[i]));
            Check(device->CreateShaderResourceView(reservoir[i].Get(), nullptr, &reservoirSRV[i]));
            Check(device->CreateUnorderedAccessView(reservoir[i].Get(), nullptr, &reservoirUAV[i]));
        }
    }
    
    // Flips SRV/UAV roles at ReSTIR phase boundaries: after pass 11 for pass 14,
    // then after pass 14 to promote the spatial result as next-frame history.
    void SwapReservoir() {
        reservoirReadFlip = !reservoirReadFlip;
    }
    
    // Returns the SRV for the caller to bind, after syncing the live texture
    // into its snapshot.
    //
    // @param index  Which image to present.
    //
    // The CopyResource before returning is what makes the returned view contain
    // this frame's result -- the snapshot is the only side a shader may read,
    // and the UI reads through this SRV.
    //
    // DEFECT: index is unchecked, so an out-of-range index is an out-of-bounds
    // array access. Unlike the other accessors, there is no assert and no clamp,
    // and this is the method an external caller uses, so it is the most exposed
    // of the unchecked ones.
    void* Output(UINT index=0) {if(!width)return nullptr;Unbind();context->CopyResource(images[index].snapshot.Get(),images[index].texture.Get());return images[index].srv.Get();}
};
}
