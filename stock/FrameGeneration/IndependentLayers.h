// SPDX-License-Identifier: MIT
#pragma once
#include "LayerReconstruction.h"
#include "nvOpticalFlowD3D11.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

class IndependentLayers
{
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Motion
    {
        NvOFHandle session{};
        Ptr<ID3D11Texture2D> input[2],flow[2];
        Ptr<ID3D11RenderTargetView> target[2];
        NvOFGPUBufferHandle inputHandle[2]{},flowHandle[2]{};
    };
    struct Parameters
    {
        unsigned width,height,gridWidth,gridHeight;
        float phase,scale;
        unsigned cloudFilmic,writeLayers;
        float fadeBefore,fadeAfter;
        unsigned verifyComposition,motionMask;
    };
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> history[2],vectors;
    Ptr<ID3D11ShaderResourceView> historyView[2],vectorView;
    Ptr<ID3D11ComputeShader> shader;
    Ptr<ID3D11ComputeShader> presenceShader;
    Ptr<ID3D11Buffer> presence;
    Ptr<ID3D11UnorderedAccessView> presenceTarget;
    struct PresenceSample
    {
        Ptr<ID3D11Buffer> readback;
        Ptr<ID3D11Query> ready;
        std::uint64_t serial{};
        bool pending{};
    };
    PresenceSample presenceSamples[3];
    unsigned activeMask=511,estimatedMask=511;
    std::uint64_t inputSerial{},lastPresenceSerial{};
    Ptr<ID3D11VertexShader> vertex;
    Ptr<ID3D11PixelShader> downsample;
    Ptr<ID3D11Buffer> parameters,downsampleParameters;
    Ptr<ID3D11RasterizerState> rasterizer;
    std::array<Motion,LayerReconstruction::Count> motion;
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    HMODULE module{};
    unsigned width,height,scale,flowWidth,flowHeight,gridWidth,gridHeight;
    bool valid[2]{};
    bool cloudFilmic{};
    float fade[2]{1,1};
    static void Check(HRESULT hr)
    {
        if(FAILED(hr))throw std::runtime_error("Independent layers D3D11 failure "+std::to_string(unsigned(hr)));
    }
    static void Optical(NV_OF_STATUS status)
    {
        if(status!=NV_OF_SUCCESS)throw std::runtime_error("Independent layers optical-flow failure "+std::to_string(unsigned(status)));
    }
    Ptr<ID3DBlob> Compile(const char *source,const char *profile)
    {
        Ptr<ID3DBlob> code,error;
        HRESULT hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main",profile,
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(hr))throw std::runtime_error(error?std::string(static_cast<char*>(error->GetBufferPointer()),error->GetBufferSize()):"Independent layers shader compilation failed");
        return code;
    }
    void StartMotion(Motion &value)
    {
        if(value.session)return;
        if(!module)
        {
            module=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!module)throw std::runtime_error("Optical-flow driver unavailable");
            using Create=NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D11_API_FUNCTION_LIST*);
            auto create=reinterpret_cast<Create>(GetProcAddress(module,"NvOFAPICreateInstanceD3D11"));
            if(!create)throw std::runtime_error("Optical-flow D3D11 entry unavailable");
            Optical(create(NV_OF_API_VERSION,&api));
        }
        Optical(api.nvCreateOpticalFlowD3D11(device.Get(),context.Get(),&value.session));
        NV_OF_INIT_PARAMS init{};init.width=flowWidth;init.height=flowHeight;
        init.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;init.mode=NV_OF_MODE_OPTICALFLOW;
        init.perfLevel=NV_OF_PERF_LEVEL_FAST;init.predDirection=NV_OF_PRED_DIRECTION_BOTH;
        init.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;Optical(api.nvOFInit(value.session,&init));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=flowWidth;desc.Height=flowHeight;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        for(unsigned i=0;i<2;i++)
        {
            Check(device->CreateTexture2D(&desc,nullptr,&value.input[i]));
            Check(device->CreateRenderTargetView(value.input[i].Get(),nullptr,&value.target[i]));
            Optical(api.nvOFRegisterResourceD3D11(value.session,value.input[i].Get(),&value.inputHandle[i]));
        }
        desc.Width=gridWidth;desc.Height=gridHeight;desc.Format=DXGI_FORMAT_R16G16_SINT;
        for(unsigned i=0;i<2;i++)
        {
            Check(device->CreateTexture2D(&desc,nullptr,&value.flow[i]));
            Optical(api.nvOFRegisterResourceD3D11(value.session,value.flow[i].Get(),&value.flowHandle[i]));
        }
    }
    void RenderInput(Motion &value,unsigned image,unsigned layer)
    {
        unsigned settings[4]={width,height,layer,scale};context->UpdateSubresource(downsampleParameters.Get(),0,nullptr,settings,0,0);
        auto *cb=downsampleParameters.Get();auto *target=value.target[image].Get();auto *source=historyView[image].Get();
        context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);
        context->OMSetDepthStencilState(nullptr,0);
        D3D11_VIEWPORT viewport{0,0,float(flowWidth),float(flowHeight),0,1};
        context->RSSetViewports(1,&viewport);context->RSSetState(rasterizer.Get());
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(downsample.Get(),nullptr,0);
        context->PSSetConstantBuffers(0,1,&cb);context->PSSetShaderResources(0,1,&source);context->Draw(3,0);
        source=nullptr;context->PSSetShaderResources(0,1,&source);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
public:
    IndependentLayers(ID3D11Device *gpu,unsigned w,unsigned h):device(gpu),width(w),height(h),
        scale((std::min)(w,h)>=128?4u:2u),flowWidth((w+scale-1)/scale),flowHeight((h+scale-1)/scale),
        gridWidth((flowWidth+3)/4),gridHeight((flowHeight+3)/4)
    {
        gpu->GetImmediateContext(&context);
        auto code=Compile(LayerReconstruction::Shader,"cs_5_0");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        const char *vs="float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";
        code=Compile(vs,"vs_5_0");Check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex));
        const char *ps=R"HLSL(
Texture2DArray<float4> layers:register(t0);
cbuffer Settings:register(b0){uint2 size;uint layer;uint scale;}
float4 sampleOwn(int2 p)
{
    if(layer<6)
    {
        uint kind=uint(round(layers.Load(int4(p,1,0)).r*255));
        uint owner=kind==1?5:kind==2?1:kind==3?2:kind==4?3:kind==5?4:0;
        return owner==layer?float4(layers.Load(int4(p,0,0)).rgb,1):0;
    }
    return layers.Load(int4(p,layer-4,0));
}
float4 main(float4 position:SV_Position):SV_Target
{
    int2 p=int2(position.xy)*int(scale);float4 value=0;
    for(uint y=0;y<scale;y++)for(uint x=0;x<scale;x++)
        value+=sampleOwn(min(p+int2(x,y),int2(size)-1));
    value/=float(scale*scale);float3 c=value.rgb/max(value.a,.01);c=c/(1+max(c,0));
    float coverage=layer==7?float(value.a>.01 || max(abs(value.r),max(abs(value.g),abs(value.b)))>.0001):saturate(value.a);
    if(layer==7)c=value.rgb/(1+max(value.rgb,0));
    return float4(c*coverage,1);
}
)HLSL";
        code=Compile(ps,"ps_5_0");Check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&downsample));
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Parameters);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&cb,nullptr,&parameters));cb.ByteWidth=16;
        Check(device->CreateBuffer(&cb,nullptr,&downsampleParameters));
        D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
        raster.DepthClipEnable=TRUE;Check(device->CreateRasterizerState(&raster,&rasterizer));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.ArraySize=LayerReconstruction::PackedSlices;
        desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        for(unsigned i=0;i<2;i++)
        {
            Check(device->CreateTexture2D(&desc,nullptr,&history[i]));
            Check(device->CreateShaderResourceView(history[i].Get(),nullptr,&historyView[i]));
        }
        desc.Width=gridWidth;desc.Height=gridHeight;desc.ArraySize=LayerReconstruction::Count*2;
        desc.Format=DXGI_FORMAT_R16G16_SINT;Check(device->CreateTexture2D(&desc,nullptr,&vectors));
        Check(device->CreateShaderResourceView(vectors.Get(),nullptr,&vectorView));
        const char *presenceSource=R"HLSL(
Texture2DArray<float4> image:register(t0);
RWStructuredBuffer<uint> bits:register(u0);
groupshared uint groupBits;
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex)
{
    if(lane==0)groupBits=0;GroupMemoryBarrierWithGroupSync();
    uint w,h,s;image.GetDimensions(w,h,s);
    if(id.x<w && id.y<h)
    {
        float4 label=image.Load(int4(id.xy,1,0));uint kind=uint(round(label.r*255));
        uint layer=kind==1?5:kind==2?1:kind==3?2:kind==4?3:kind==5?4:0;
        float3 base=image.Load(int4(id.xy,0,0)).rgb;
        uint selected=(layer!=0 || max(base.r,max(base.g,base.b))>=.02)?1u<<layer:0;
        if(image.Load(int4(id.xy,2,0)).a>.01)selected|=1u<<6;
        float4 fire=image.Load(int4(id.xy,3,0));
        if(fire.a>.01 || max(abs(fire.r),max(abs(fire.g),abs(fire.b)))>.0001)selected|=1u<<7;
        if(image.Load(int4(id.xy,4,0)).a>.01)selected|=1u<<8;
        InterlockedOr(groupBits,selected);
    }
    GroupMemoryBarrierWithGroupSync();if(lane==0)InterlockedOr(bits[0],groupBits);
}
)HLSL";
        code=Compile(presenceSource,"cs_5_0");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&presenceShader));
        D3D11_BUFFER_DESC count{};count.ByteWidth=4;count.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        count.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;count.StructureByteStride=4;
        Check(device->CreateBuffer(&count,nullptr,&presence));Check(device->CreateUnorderedAccessView(presence.Get(),nullptr,&presenceTarget));
        count.Usage=D3D11_USAGE_STAGING;count.BindFlags=count.MiscFlags=0;count.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        for(auto &sample:presenceSamples)
        {
            Check(device->CreateBuffer(&count,nullptr,&sample.readback));
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};Check(device->CreateQuery(&query,&sample.ready));
        }
    }
    ~IndependentLayers()
    {
        for(auto &value:motion)if(value.session)
        {
            for(unsigned i=0;i<2;i++)
            {
                if(value.inputHandle[i])api.nvOFUnregisterResourceD3D11(value.inputHandle[i]);
                if(value.flowHandle[i])api.nvOFUnregisterResourceD3D11(value.flowHandle[i]);
            }
            api.nvOFDestroy(value.session);
        }
        if(module)FreeLibrary(module);
    }
    void Submit(unsigned index,ID3D11Texture2D *layers,bool filmic,float cloudFade)
    {
        if(index>1)throw std::invalid_argument("Layer history index");
        valid[index]=false;if(!layers)return;
        D3D11_TEXTURE2D_DESC desc{};layers->GetDesc(&desc);
        if(desc.Width!=width||desc.Height!=height||desc.ArraySize!=LayerReconstruction::PackedSlices
            ||desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||desc.SampleDesc.Count!=1)
            throw std::invalid_argument("Independent layer descriptor mismatch");
        context->CopyResource(history[index].Get(),layers);valid[index]=true;
        cloudFilmic=filmic;fade[index]=cloudFade;
        for(auto &sample:presenceSamples)if(!sample.pending)
        {
            UINT zero[4]{};auto *target=presenceTarget.Get();auto *view=historyView[index].Get();
            context->ClearUnorderedAccessViewUint(target,zero);
            context->CSSetShader(presenceShader.Get(),nullptr,0);context->CSSetShaderResources(0,1,&view);
            context->CSSetUnorderedAccessViews(0,1,&target,nullptr);context->Dispatch((width+7)/8,(height+7)/8,1);
            view=nullptr;target=nullptr;context->CSSetShaderResources(0,1,&view);context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
            context->CopyResource(sample.readback.Get(),presence.Get());context->End(sample.ready.Get());
            sample.serial=++inputSerial;sample.pending=true;break;
        }
    }
    bool Valid(unsigned before,unsigned after)const{return valid[before]&&valid[after];}
    void Estimate(unsigned before,unsigned after)
    {
        if(!Valid(before,after))throw std::runtime_error("Independent layer inputs unavailable");
        for(auto &sample:presenceSamples)if(sample.pending && context->GetData(sample.ready.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(context->Map(sample.readback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)==S_OK)
            {
                if(sample.serial>lastPresenceSerial)
                {activeMask=*static_cast<unsigned*>(mapped.pData)&511u;lastPresenceSerial=sample.serial;}
                context->Unmap(sample.readback.Get(),0);sample.pending=false;
            }
        }
        estimatedMask=activeMask;
        UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        context->RSGetViewports(&viewportCount,viewports);
        Ptr<ID3D11RasterizerState> previousRaster;context->RSGetState(&previousRaster);
        for(unsigned layer=0;layer<LayerReconstruction::Count;layer++)
        {
            if((estimatedMask&(1u<<layer))==0)continue;
            auto &value=motion[layer];StartMotion(value);
            RenderInput(value,before,layer);RenderInput(value,after,layer);
            NV_OF_EXECUTE_INPUT_PARAMS in{};NV_OF_EXECUTE_OUTPUT_PARAMS out{};
            in.inputFrame=value.inputHandle[before];in.referenceFrame=value.inputHandle[after];
            in.disableTemporalHints=NV_OF_TRUE;out.outputBuffer=value.flowHandle[0];out.bwdOutputBuffer=value.flowHandle[1];
            Optical(api.nvOFExecute(value.session,&in,&out));
            for(unsigned direction=0;direction<2;direction++)
                context->CopySubresourceRegion(vectors.Get(),layer*2+direction,0,0,0,value.flow[direction].Get(),0,nullptr);
        }
        context->RSSetViewports(viewportCount,viewports);context->RSSetState(previousRaster.Get());
    }
    void SetTestVectors(ID3D11Texture2D *source){context->CopyResource(vectors.Get(),source);estimatedMask=511;}
    ID3D11Texture2D *DiagnosticVectors()const{return vectors.Get();}
    unsigned ActiveMask()const{return estimatedMask;}
    void Generate(unsigned before,unsigned after,float phase,ID3D11ShaderResourceView *previous,
        ID3D11ShaderResourceView *current,ID3D11UnorderedAccessView *output,ID3D11UnorderedAccessView *counts,
        ID3D11UnorderedAccessView *diagnostic=nullptr,bool verify=true)
    {
        Parameters settings{width,height,gridWidth,gridHeight,phase,float(scale),unsigned(cloudFilmic),unsigned(diagnostic!=nullptr),
            fade[before],fade[after],unsigned(verify),estimatedMask};
        context->UpdateSubresource(parameters.Get(),0,nullptr,&settings,0,0);auto *cb=parameters.Get();
        ID3D11ShaderResourceView *views[]={historyView[before].Get(),historyView[after].Get(),vectorView.Get(),current,previous};
        ID3D11UnorderedAccessView *targets[]={output,counts,diagnostic};
        UINT zeros[4]{};context->ClearUnorderedAccessViewUint(counts,zeros);
        context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,&cb);
        context->CSSetShaderResources(0,5,views);context->CSSetUnorderedAccessViews(0,3,targets,nullptr);
        context->Dispatch((width+7)/8,(height+7)/8,1);
        for(auto &view:views)view=nullptr;for(auto &target:targets)target=nullptr;
        context->CSSetShaderResources(0,5,views);context->CSSetUnorderedAccessViews(0,3,targets,nullptr);
        context->CSSetShader(nullptr,nullptr,0);
    }
};
