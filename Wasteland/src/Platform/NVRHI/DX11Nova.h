#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <unordered_map>
#include <string>
#include <stdexcept>
#include <cstring>

namespace Wasteland {
// Native compute resources are deliberately independent of NVRHI's graphics
// reflection/cache. Every dispatch reads immutable snapshots, never its UAVs.
class DX11Nova {
    template<class T> using Com = Microsoft::WRL::ComPtr<T>;
    struct Image { Com<ID3D11Texture2D> texture, snapshot; Com<ID3D11UnorderedAccessView> uav; Com<ID3D11ShaderResourceView> srv; };
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11ComputeShader> shader;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> sampler;
    std::array<Image,8> images;
    Image history;
    std::array<Com<ID3D11Buffer>,3> buffers;
    std::array<Com<ID3D11ShaderResourceView>,3> scene;
    std::unordered_map<std::string,D3D11_SHADER_VARIABLE_DESC> variables;
    std::vector<unsigned char> bytes;
    UINT width=0,height=0;
    static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("DX11 Nova HRESULT " + std::to_string((unsigned long)hr)); }
    Image CreateImage(UINT w, UINT h, DXGI_FORMAT format) {
        Image i; D3D11_TEXTURE2D_DESC d={}; d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1; d.Format=format; d.SampleDesc.Count=1; d.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&d,nullptr,&i.texture)); Check(device->CreateUnorderedAccessView(i.texture.Get(),nullptr,&i.uav));
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE; Check(device->CreateTexture2D(&d,nullptr,&i.snapshot)); Check(device->CreateShaderResourceView(i.snapshot.Get(),nullptr,&i.srv)); return i;
    }
public:
    void Init(ID3D11Device* d, ID3D11DeviceContext* c) {
        if(device.Get()==d && shader) return;
        device=d; context=c;
        Com<ID3DBlob> code,error;
        HRESULT hr=D3DCompileFromFile(L"assets/shaders/Renderer3D_NovaRenderer.hlsl",nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        if(FAILED(hr)) throw std::runtime_error(error ? (const char*)error->GetBufferPointer() : "Nova shader compile failed");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        Com<ID3D11ShaderReflection> reflection; Check(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_ID3D11ShaderReflection,(void**)reflection.GetAddressOf()));
        auto cb=reflection->GetConstantBufferByName("NovaConstants"); D3D11_SHADER_BUFFER_DESC desc={}; Check(cb->GetDesc(&desc)); bytes.resize(desc.Size);
        for(UINT j=0;j<desc.Variables;++j) { D3D11_SHADER_VARIABLE_DESC v={}; Check(cb->GetVariableByIndex(j)->GetDesc(&v)); variables[v.Name]=v; }
        D3D11_BUFFER_DESC bd={}; bd.ByteWidth=(desc.Size+15)&~15u; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; Check(device->CreateBuffer(&bd,nullptr,&constants));
        D3D11_SAMPLER_DESC sd={}; sd.Filter=D3D11_FILTER_ANISOTROPIC; sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_MIRROR; sd.MaxAnisotropy=16; sd.MaxLOD=D3D11_FLOAT32_MAX; Check(device->CreateSamplerState(&sd,&sampler));
    }
    void Resize(UINT w,UINT h) {
        if(!w||!h||(width==w&&height==h)) return;
        Unbind(); width=w;height=h;
        for(int i=0;i<8;++i) images[i]=CreateImage(w,h,i==5?DXGI_FORMAT_R32G32_FLOAT:(i==3?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R32G32B32A32_FLOAT));
        history=CreateImage(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT); Clear();
    }
    void Clear() { if(!context||!width)return; Unbind(); const float zero[4]={}; for(auto& i:images)context->ClearUnorderedAccessViewFloat(i.uav.Get(),zero); context->ClearUnorderedAccessViewFloat(history.uav.Get(),zero); context->CopyResource(history.snapshot.Get(),history.texture.Get()); }
    template<class T> void Set(const char* name,const T& value,UINT index=0) {
        auto it=variables.find(name); if(it==variables.end())return;
        UINT offset=it->second.StartOffset+(index*16); if(offset+sizeof(T)>it->second.StartOffset+it->second.Size)throw std::runtime_error("Nova constant size mismatch");
        memcpy(bytes.data()+offset,&value,sizeof(T));
    }
    void Upload(UINT slot,const void* data,UINT count,UINT stride) {
        buffers[slot].Reset();scene[slot].Reset();
        std::vector<unsigned char> empty(stride); D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=count?data:empty.data();
        D3D11_BUFFER_DESC bd={}; bd.ByteWidth=(count?count:1)*stride;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=stride;
        Check(device->CreateBuffer(&bd,&initial,&buffers[slot]));Check(device->CreateShaderResourceView(buffers[slot].Get(),nullptr,&scene[slot]));
    }
    void Unbind() { if(!context)return; ID3D11ShaderResourceView* nulls[128]={};ID3D11UnorderedAccessView* uavs[8]={};context->CSSetShaderResources(0,128,nulls);context->CSSetUnorderedAccessViews(0,8,uavs,nullptr);context->CSSetShader(nullptr,nullptr,0); }
    void Dispatch(int pass,UINT w,UINT h,const std::vector<ID3D11ShaderResourceView*>& textures) {
        Unbind();
        // The UI can leave last frame's output on PS t0. Remove graphics SRVs
        // before any copies/UAV writes; graphics code rebinds its own resources.
        ID3D11ShaderResourceView* nulls[128]={};context->PSSetShaderResources(0,128,nulls);
        for(auto& i:images)context->CopyResource(i.snapshot.Get(),i.texture.Get());
        Set("u_PassID",pass);context->UpdateSubresource(constants.Get(),0,nullptr,bytes.data(),0,0);
        ID3D11Buffer* cb=constants.Get();context->CSSetConstantBuffers(1,1,&cb);ID3D11SamplerState* ss=sampler.Get();context->CSSetSamplers(0,1,&ss);
        ID3D11ShaderResourceView* srvs[128]={};for(int i=0;i<3;++i)srvs[i+1]=scene[i].Get();for(size_t i=0;i<textures.size()&&i<32;++i)srvs[15+i]=textures[i];
        if(pass==9 && !textures.empty())srvs[52]=textures[0];
        srvs[48]=history.srv.Get();srvs[49]=images[6].srv.Get();srvs[50]=images[0].srv.Get();srvs[51]=images[6].srv.Get();
        for(int i=0;i<8;++i)srvs[64+i]=images[i].srv.Get();context->CSSetShaderResources(0,128,srvs);
        ID3D11UnorderedAccessView* uavs[8];for(int i=0;i<8;++i)uavs[i]=images[i].uav.Get();context->CSSetUnorderedAccessViews(0,8,uavs,nullptr);
        context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch((w+7)/8,(h+7)/8,1);Unbind();
    }
    void CommitHistory() {Unbind();context->CopyResource(history.snapshot.Get(),images[1].texture.Get());}
    void CommitDenoise() {Unbind();context->CopyResource(images[1].texture.Get(),images[7].texture.Get());}
    void* Output(UINT index=0) {if(!width)return nullptr;Unbind();context->CopyResource(images[index].snapshot.Get(),images[index].texture.Get());return images[index].srv.Get();}
};
}
