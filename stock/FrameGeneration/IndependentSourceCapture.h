// SPDX-License-Identifier: MIT
#pragma once
#include "LayerReconstruction.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <map>
#include <stdexcept>

class IndependentSourceCapture
{
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct View { Ptr<ID3D11Texture2D> image;Ptr<ID3D11ShaderResourceView> view; };
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext1> context;
    Ptr<ID3DDeviceContextState> state;
    Ptr<ID3D11Texture2D> layers;
    Ptr<ID3D11UnorderedAccessView> target;
    Ptr<ID3D11ComputeShader> shader;
    Ptr<ID3D11ComputeShader> opaqueShader;
    Ptr<ID3D11Buffer> parameters;
    std::map<ID3D11Texture2D*,View> views;
    unsigned width{},height{};
    bool opaqueReady{};
    static void Check(HRESULT value){if(FAILED(value))throw std::runtime_error("Independent source capture D3D11 failure");}
    ID3D11ShaderResourceView *GetView(ID3D11Texture2D *source)
    {
        if(!source)return nullptr;
        auto found=views.find(source);if(found!=views.end())return found->second.view.Get();
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Format=desc.Format;
        if(srv.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS)srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        if(srv.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS)srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        if(srv.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS)srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        View value;value.image=source;Check(device->CreateShaderResourceView(source,&srv,&value.view));
        return views.emplace(source,std::move(value)).first->second.view.Get();
    }
public:
    IndependentSourceCapture(ID3D11Device *gpu,ID3D11DeviceContext *immediate,unsigned w,unsigned h):device(gpu),width(w),height(h)
    {
        Ptr<ID3D11Device1> device1;Check(device.As(&device1));Check(immediate->QueryInterface(IID_PPV_ARGS(&context)));
        D3D_FEATURE_LEVEL feature=device->GetFeatureLevel(),chosen{};
        Check(device1->CreateDeviceContextState(0,&feature,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&state));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.SampleDesc.Count=1;
        desc.ArraySize=LayerReconstruction::PackedSlices;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        Check(device->CreateTexture2D(&desc,nullptr,&layers));Check(device->CreateUnorderedAccessView(layers.Get(),nullptr,&target));
        const char *source=R"HLSL(
Texture2D<float4> opaque:register(t0);
Texture2D<float4> classes:register(t1);
Texture2D<float4> clouds:register(t2);
Texture2D<float4> firefly:register(t3);
RWTexture2DArray<float4> layers:register(u0);
cbuffer Settings:register(b0){uint2 size;uint hasClouds;uint hasFirefly;}
float3 inverseFilm(float3 color)
{
    float3 x=saturate(color),a=.0433513559*x-.053199999,b=.114082515*x-.00500000082;
    return (-sqrt(max(0,b*b-.0547596104*x*a))-.114082515*x+.00500000082)/((.22816503*x-.280000001)*.379999995);
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=size))return;
    int2 p=int2(id.xy),cameraPixel=int2(p.x,int(size.y)-1-p.y);
    uint finalKind=uint(round(classes.Load(int3(p,0)).r*255));
    if(finalKind==3 || finalKind==4)
        layers[uint3(id.xy,1)]=float4(finalKind/255.0,0,0,1);
    float4 cloud=0;
    if(hasClouds!=0)
    {
        uint w,h;clouds.GetDimensions(w,h);
        int2 pixel=int2((float2(p)+.5)/float2(size)*float2(w,h));pixel.y=int(h)-1-pixel.y;
        float4 value=clouds.Load(int3(pixel,0));
        cloud=value.a<1?float4(inverseFilm(value.rgb),saturate(1-value.a)):0;
    }
    layers[uint3(id.xy,2)]=cloud;
    float4 fire=hasFirefly!=0?firefly.Load(int3(cameraPixel,0)):float4(0,0,0,1);
    layers[uint3(id.xy,3)]=float4(fire.rgb,saturate(1-fire.a));
}
)HLSL";
        Ptr<ID3DBlob> code,error;
        HRESULT compiled=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(compiled))throw std::runtime_error(error?std::string(static_cast<char*>(error->GetBufferPointer()),error->GetBufferSize()):"Independent source shader failed");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        const char *opaqueSource=R"HLSL(
Texture2D<float4> colorImage:register(t0);
Texture2D<float4> localClasses:register(t1);
RWTexture2DArray<float4> layers:register(u0);
cbuffer Settings:register(b0){uint2 size;uint reset;uint hasClasses;}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=size))return;
    int2 pixel=int2(id.x,size.y-1-id.y);
    float4 label=hasClasses!=0?localClasses.Load(int3(pixel,0)):0;
    uint kind=uint(round(label.r*255));
    if(reset!=0 || (kind>=1 && kind<=5))
    {
        layers[uint3(id.xy,0)]=float4(colorImage.Load(int3(pixel,0)).rgb,1);
        layers[uint3(id.xy,1)]=float4(kind/255.0,0,0,1);
    }
}
)HLSL";
        code.Reset();error.Reset();compiled=D3DCompile(opaqueSource,strlen(opaqueSource),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(compiled))throw std::runtime_error("Independent opaque accumulation shader failed");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&opaqueShader));
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&cb,nullptr,&parameters));
    }
    bool Capture(ID3D11Texture2D *opaque,ID3D11Texture2D *classes,ID3D11Texture2D *clouds,ID3D11Texture2D *firefly)
    {
        if(!opaque||!classes)return false;
        if(!opaqueReady)return false;
        if(views.size()>12)views.clear();
        Ptr<ID3DDeviceContextState> previous;context->SwapDeviceContextState(state.Get(),&previous);
        try
        {
            unsigned settings[]={width,height,unsigned(clouds!=nullptr),unsigned(firefly!=nullptr)};
            context->UpdateSubresource(parameters.Get(),0,nullptr,settings,0,0);auto *cb=parameters.Get();auto *uav=target.Get();
            ID3D11ShaderResourceView *inputs[]={GetView(opaque),GetView(classes),GetView(clouds),GetView(firefly)};
            context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,&cb);
            context->CSSetShaderResources(0,4,inputs);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
            context->Dispatch((width+7)/8,(height+7)/8,1);
            for(auto &input:inputs)input=nullptr;uav=nullptr;
            context->CSSetShaderResources(0,4,inputs);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        }
        catch(...){context->SwapDeviceContextState(previous.Get(),&state);throw;}
        context->SwapDeviceContextState(previous.Get(),&state);return true;
    }
    ID3D11Texture2D *Image()const{return layers.Get();}
    void BeginFrame(){opaqueReady=false;}
    void CaptureOpaque(ID3D11Texture2D *color,ID3D11Texture2D *localClasses)
    {
        if(!color)return;
        if(views.size()>12)views.clear();
        Ptr<ID3DDeviceContextState> previous;context->SwapDeviceContextState(state.Get(),&previous);
        try
        {
            unsigned settings[]={width,height,unsigned(!opaqueReady),unsigned(localClasses!=nullptr)};
            context->UpdateSubresource(parameters.Get(),0,nullptr,settings,0,0);auto *cb=parameters.Get();auto *uav=target.Get();
            ID3D11ShaderResourceView *inputs[]={GetView(color),GetView(localClasses)};
            context->CSSetShader(opaqueShader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,&cb);
            context->CSSetShaderResources(0,2,inputs);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
            context->Dispatch((width+7)/8,(height+7)/8,1);
            for(auto &input:inputs)input=nullptr;uav=nullptr;
            context->CSSetShaderResources(0,2,inputs);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
            opaqueReady=true;
        }
        catch(...){context->SwapDeviceContextState(previous.Get(),&state);throw;}
        context->SwapDeviceContextState(previous.Get(),&state);
    }
};
