// SPDX-License-Identifier: MIT
#pragma once
#include "DxbcMrt.h"
#include "MrtPrograms.h"
#include <d3d11.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <climits>

class EffectMrtCapture
{
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    using CreatePS=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
    using SetPS=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11PixelShader*,ID3D11ClassInstance *const*,UINT);
    using SetRT=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView *const*,ID3D11DepthStencilView*);
    using SetUAV=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView *const*,ID3D11DepthStencilView*,UINT,UINT,ID3D11UnorderedAccessView *const*,const UINT*);
    using SetBlend=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11BlendState*,const FLOAT*,UINT);
    using GetPS=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11PixelShader**,ID3D11ClassInstance**,UINT*);
    using GetRT=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView**,ID3D11DepthStencilView**);
    using GetUAV=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView**,ID3D11DepthStencilView**,UINT,UINT,ID3D11UnorderedAccessView**);
    using GetBlend=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11BlendState**,FLOAT*,UINT*);
    inline static CreatePS originalCreate{};
    inline static SetPS originalPS{};
    inline static SetRT originalRT{};
    inline static SetUAV originalUAV{};
    inline static SetBlend originalBlend{};
    inline static GetPS originalGetPS{};
    inline static GetRT originalGetRT{};
    inline static GetUAV originalGetUAV{};
    inline static GetBlend originalGetBlend{};
    inline static std::atomic<EffectMrtCapture*> owner{};
    inline static thread_local unsigned suspended{};
    struct Program{Ptr<ID3D11PixelShader> original,copy;unsigned layer;};
    struct Blend{Ptr<ID3D11BlendState> original,copy;};
    struct Layer{Ptr<ID3D11Texture2D> image;Ptr<ID3D11RenderTargetView> target;bool touched{};};
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    std::unordered_map<ID3D11PixelShader*,Program> programs;
    std::unordered_map<ID3D11PixelShader*,Program*> copies;
    std::unordered_map<ID3D11BlendState*,Blend> blends;
    std::unordered_map<ID3D11BlendState*,Ptr<ID3D11BlendState>> blendOriginals;
    std::mutex programMutex;
    BCRYPT_ALG_HANDLE hash{};
    Ptr<ID3D11PixelShader> expectedPS;
    Program *program{};
    Ptr<ID3D11BlendState> expectedBlend;
    FLOAT factors[4]{1,1,1,1};UINT sampleMask=0xffffffff;
    Ptr<ID3D11RenderTargetView> targets[8];Ptr<ID3D11DepthStencilView> depth;
    unsigned targetCount{},width{},height{};
    Layer layers[2];
    int frame=-1;
    bool attached{},uavMode{},world{};
    std::atomic<bool> enabled{};
    std::atomic<long long> recognized{},attachments{},unsupported{},rewriteFailures{};
    friend struct EffectMrtFixture;
    static void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Single-pass capture D3D11 failure "+std::to_string(unsigned(hr)));}
    static EffectMrtCapture *Source(ID3D11DeviceContext *ctx)
    {auto *capture=owner.load();return capture && !suspended && capture->context.Get()==ctx?capture:nullptr;}
    unsigned Known(const void *data,size_t size)
    {
        const auto *bytes=static_cast<const unsigned char*>(data);
        if(size<32 || memcmp(bytes,"DXBC",4) || DxbcMrt::Read(bytes+24)>size)return 0;
        unsigned count=DxbcMrt::Read(bytes+28);if(count>(size-32)/4)return 0;
        const unsigned char *code=nullptr,*signature=nullptr;unsigned codeSize=0,signatureSize=0;
        for(unsigned i=0;i<count;i++)
        {
            unsigned at=DxbcMrt::Read(bytes+32+i*4);if(at>size-8)return 0;
            unsigned length=DxbcMrt::Read(bytes+at+4);if(length>size-at-8)return 0;
            if(!memcmp(bytes+at,"SHEX",4)||!memcmp(bytes+at,"SHDR",4)){code=bytes+at+8;codeSize=length;}
            if(!memcmp(bytes+at,"OSGN",4)){signature=bytes+at+8;signatureSize=length;}
        }
        if(!code || !signature || size>UINT_MAX)return 0;
        std::vector<unsigned char> input(8+codeSize+signatureSize);DxbcMrt::Write(input.data(),codeSize);
        memcpy(input.data()+4,code,codeSize);DxbcMrt::Write(input.data()+4+codeSize,signatureSize);
        memcpy(input.data()+8+codeSize,signature,signatureSize);unsigned char digest[32]{};
        if(BCryptHash(hash,nullptr,0,input.data(),static_cast<ULONG>(input.size()),digest,32)<0)return 0;
        size_t low=0,high=sizeof(MrtKnownPrograms)/sizeof(MrtKnownPrograms[0]);
        while(low<high){size_t middle=(low+high)/2;int order=memcmp(digest,MrtKnownPrograms[middle].digest,32);if(order>0)low=middle+1;else high=middle;}
        return low<sizeof(MrtKnownPrograms)/sizeof(MrtKnownPrograms[0]) && !memcmp(digest,MrtKnownPrograms[low].digest,32)?MrtKnownPrograms[low].layer:0;
    }
    void Register(ID3D11PixelShader *shader,const void *data,size_t size,unsigned layer)
    {
        auto bytes=DxbcMrt::DuplicateColor(data,size);Ptr<ID3D11PixelShader> copy;
        Check(originalCreate(device.Get(),bytes.data(),bytes.size(),nullptr,&copy));
        std::lock_guard<std::mutex> lock(programMutex);
        auto entry=programs.emplace(shader,Program{shader,copy,layer});
        if(entry.second){copies.emplace(copy.Get(),&entry.first->second);recognized++;}
    }
    Program *Find(ID3D11PixelShader *shader)
    {
        std::lock_guard<std::mutex> lock(programMutex);auto it=programs.find(shader);
        if(it!=programs.end())return &it->second;
        auto copy=copies.find(shader);return copy==copies.end()?nullptr:copy->second;
    }
    bool OwnTargets(UINT count,ID3D11RenderTargetView *const *views)const
    {return count==2 && views && views[1] && (views[1]==layers[0].target.Get() || views[1]==layers[1].target.Get());}
    void StoreTargets(UINT count,ID3D11RenderTargetView *const *views,ID3D11DepthStencilView *dsv)
    {
        targetCount=(std::min)(count,8u);for(unsigned i=0;i<8;i++)targets[i]=i<targetCount&&views?views[i]:nullptr;
        depth=dsv;attached=false;uavMode=false;
    }
    ID3D11BlendState *CaptureBlend()
    {
        auto found=blends.find(expectedBlend.Get());if(found!=blends.end())return found->second.copy.Get();
        D3D11_BLEND_DESC desc{};
        if(expectedBlend)expectedBlend->GetDesc(&desc);
        else
        {
            auto &rt=desc.RenderTarget[0];rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;
            rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        auto rt=desc.RenderTarget[0];
        if(!rt.BlendEnable){rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_ONE;rt.DestBlend=D3D11_BLEND_ZERO;rt.BlendOp=D3D11_BLEND_OP_ADD;}
        bool source=rt.SrcBlend==D3D11_BLEND_ZERO||rt.SrcBlend==D3D11_BLEND_ONE||rt.SrcBlend==D3D11_BLEND_SRC_COLOR
            ||rt.SrcBlend==D3D11_BLEND_INV_SRC_COLOR||rt.SrcBlend==D3D11_BLEND_SRC_ALPHA||rt.SrcBlend==D3D11_BLEND_INV_SRC_ALPHA;
        bool destination=rt.DestBlend==D3D11_BLEND_ZERO||rt.DestBlend==D3D11_BLEND_ONE
            ||rt.DestBlend==D3D11_BLEND_SRC_ALPHA||rt.DestBlend==D3D11_BLEND_INV_SRC_ALPHA;
        Blend value;value.original=expectedBlend;
        if(source && destination && rt.BlendOp==D3D11_BLEND_OP_ADD && (rt.RenderTargetWriteMask&7)==7)
        {
            desc.IndependentBlendEnable=TRUE;rt.SrcBlendAlpha=D3D11_BLEND_ZERO;rt.DestBlendAlpha=rt.DestBlend;
            rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;desc.RenderTarget[1]=rt;
            if(FAILED(device->CreateBlendState(&desc,&value.copy)))value.copy.Reset();
        }
        if(!value.copy)unsupported++;
        else blendOriginals.emplace(value.copy.Get(),expectedBlend);
        return blends.emplace(expectedBlend.Get(),std::move(value)).first->second.copy.Get();
    }
    Layer *Target()
    {
        if(!program || !enabled.load() || !world || uavMode || targetCount!=1 || !targets[0])return nullptr;
        Ptr<ID3D11Resource> resource;targets[0]->GetResource(&resource);Ptr<ID3D11Texture2D> image;
        if(FAILED(resource.As(&image)))return nullptr;D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC view{};targets[0]->GetDesc(&view);
        if(desc.Width!=width || desc.Height!=height || desc.SampleDesc.Count!=1 || desc.ArraySize!=1
            ||view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice!=0)return nullptr;
        auto &layer=layers[program->layer-1];D3D11_TEXTURE2D_DESC old{};if(layer.image)layer.image->GetDesc(&old);
        if(!layer.image || old.Width!=width || old.Height!=height)
        {
            layer.target.Reset();layer.image.Reset();desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=desc.CPUAccessFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;
            Check(device->CreateTexture2D(&desc,nullptr,&layer.image));Check(device->CreateRenderTargetView(layer.image.Get(),nullptr,&layer.target));
            FLOAT clear[]={0,0,0,1};context->ClearRenderTargetView(layer.target.Get(),clear);
        }
        return &layer;
    }
    void RestoreTargets()
    {ID3D11RenderTargetView *views[8]{};for(unsigned i=0;i<targetCount;i++)views[i]=targets[i].Get();originalRT(context.Get(),targetCount,views,depth.Get());attached=false;}
    void Apply()
    {
        Suspend pause;
        Layer *layer=nullptr;ID3D11BlendState *blend=nullptr;
        try{layer=Target();if(layer)blend=CaptureBlend();}catch(...){rewriteFailures++;layer=nullptr;}
        if(layer && blend)
        {
            ID3D11RenderTargetView *views[]={targets[0].Get(),layer->target.Get()};originalRT(context.Get(),2,views,depth.Get());
            originalPS(context.Get(),program->copy.Get(),nullptr,0);originalBlend(context.Get(),blend,factors,sampleMask);
            attached=true;layer->touched=true;attachments++;
        }
        else
        {
            if(attached)RestoreTargets();
            if(program)originalPS(context.Get(),expectedPS.Get(),nullptr,0);
            originalBlend(context.Get(),expectedBlend.Get(),factors,sampleMask);
        }
    }
    static HRESULT STDMETHODCALLTYPE CreateHook(ID3D11Device *gpu,const void *data,SIZE_T size,ID3D11ClassLinkage *linkage,ID3D11PixelShader **shader)
    {
        HRESULT result=originalCreate(gpu,data,size,linkage,shader);auto *capture=owner.load();
        if(SUCCEEDED(result)&&shader&&*shader&&!linkage&&capture&&capture->device.Get()==gpu)
            try{unsigned layer=capture->Known(data,size);if(layer)capture->Register(*shader,data,size,layer);}catch(...){capture->rewriteFailures++;}
        return result;
    }
    static void STDMETHODCALLTYPE ShaderHook(ID3D11DeviceContext *ctx,ID3D11PixelShader *shader,ID3D11ClassInstance *const *instances,UINT count)
    {
        auto *capture=Source(ctx);if(!capture){originalPS(ctx,shader,instances,count);return;}
        Suspend pause;
        capture->program=count?nullptr:capture->Find(shader);
        capture->expectedPS=capture->program?capture->program->original.Get():shader;
        originalPS(ctx,shader,instances,count);capture->Apply();
    }
    static void STDMETHODCALLTYPE TargetHook(ID3D11DeviceContext *ctx,UINT count,ID3D11RenderTargetView *const *views,ID3D11DepthStencilView *depth)
    {
        auto *capture=Source(ctx);if(!capture || count>8){originalRT(ctx,count,views,depth);return;}
        Suspend pause;
        bool own=capture->OwnTargets(count,views);
        capture->StoreTargets(own?1:count,views,depth);originalRT(ctx,count,views,depth);
        if(own)capture->attached=true;
        capture->Apply();
    }
    static void STDMETHODCALLTYPE UavHook(ID3D11DeviceContext *ctx,UINT count,ID3D11RenderTargetView *const *views,ID3D11DepthStencilView *depth,UINT start,UINT number,ID3D11UnorderedAccessView *const *uavs,const UINT *values)
    {
        auto *capture=Source(ctx);
        Suspend pause;
        if(capture)
        {
            if(capture->OwnTargets(count,views))
            {
                capture->StoreTargets(1,views,depth);capture->attached=true;
                originalUAV(ctx,count,views,depth,start,number,uavs,values);capture->Apply();return;
            }
            if(capture->attached)capture->RestoreTargets();
            if(capture->program)originalPS(ctx,capture->expectedPS.Get(),nullptr,0);
            originalBlend(ctx,capture->expectedBlend.Get(),capture->factors,capture->sampleMask);
            if(count!=D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)capture->StoreTargets(count,views,depth);
            capture->uavMode=true;
        }
        originalUAV(ctx,count,views,depth,start,number,uavs,values);
    }
    static void STDMETHODCALLTYPE BlendHook(ID3D11DeviceContext *ctx,ID3D11BlendState *state,const FLOAT *factor,UINT mask)
    {
        auto *capture=Source(ctx);if(!capture){originalBlend(ctx,state,factor,mask);return;}
        Suspend pause;
        auto active=capture->blends.find(capture->expectedBlend.Get());
        if(active==capture->blends.end() || active->second.copy.Get()!=state)
        {
            auto alias=capture->blendOriginals.find(state);
            capture->expectedBlend=alias==capture->blendOriginals.end()?state:alias->second.Get();
        }
        capture->sampleMask=mask;if(factor)memcpy(capture->factors,factor,sizeof(capture->factors));capture->Apply();
    }
    static void STDMETHODCALLTYPE ShaderGetHook(ID3D11DeviceContext *ctx,ID3D11PixelShader **shader,ID3D11ClassInstance **instances,UINT *count)
    {
        auto *capture=Source(ctx);
        if(!capture || !capture->attached){originalGetPS(ctx,shader,instances,count);return;}
        if(shader){*shader=capture->expectedPS.Get();if(*shader)(*shader)->AddRef();}
        if(count)*count=0;
    }
    void GetTargets(UINT count,ID3D11RenderTargetView **views,ID3D11DepthStencilView **dsv)
    {
        if(views)for(unsigned i=0;i<count;i++){views[i]=i<targetCount?targets[i].Get():nullptr;if(views[i])views[i]->AddRef();}
        if(dsv){*dsv=depth.Get();if(*dsv)(*dsv)->AddRef();}
    }
    static void STDMETHODCALLTYPE TargetGetHook(ID3D11DeviceContext *ctx,UINT count,ID3D11RenderTargetView **views,ID3D11DepthStencilView **dsv)
    {
        auto *capture=Source(ctx);
        if(!capture || !capture->attached || count>8){originalGetRT(ctx,count,views,dsv);return;}
        capture->GetTargets(count,views,dsv);
    }
    static void STDMETHODCALLTYPE UavGetHook(ID3D11DeviceContext *ctx,UINT count,ID3D11RenderTargetView **views,ID3D11DepthStencilView **dsv,UINT start,UINT number,ID3D11UnorderedAccessView **uavs)
    {
        auto *capture=Source(ctx);
        if(!capture || !capture->attached || count>8){originalGetUAV(ctx,count,views,dsv,start,number,uavs);return;}
        Suspend pause;originalGetUAV(ctx,0,nullptr,nullptr,start,number,uavs);capture->GetTargets(count,views,dsv);
    }
    static void STDMETHODCALLTYPE BlendGetHook(ID3D11DeviceContext *ctx,ID3D11BlendState **state,FLOAT *factor,UINT *mask)
    {
        auto *capture=Source(ctx);
        if(!capture || !capture->attached){originalGetBlend(ctx,state,factor,mask);return;}
        if(state){*state=capture->expectedBlend.Get();if(*state)(*state)->AddRef();}
        if(factor)memcpy(factor,capture->factors,sizeof(capture->factors));if(mask)*mask=capture->sampleMask;
    }
    template<class T>static void Hook(void *address,void *callback,T *original)
    {if(MH_CreateHook(address,callback,reinterpret_cast<void**>(original))!=MH_OK || MH_EnableHook(address)!=MH_OK)throw std::runtime_error("Single-pass capture hook failed");}
public:
    static EffectMrtCapture *Current(){return owner.load();}
    struct Suspend{Suspend(){suspended++;}~Suspend(){suspended--;}};
    EffectMrtCapture(ID3D11Device *gpu,ID3D11DeviceContext *ctx,unsigned w,unsigned h):device(gpu),context(ctx),width(w),height(h)
    {
        if(owner.load())throw std::runtime_error("Single-pass capture already initialized");
        if(BCryptOpenAlgorithmProvider(&hash,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Shader identity provider unavailable");
        auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("Single-pass capture hook initialization failed");
        ID3D11RenderTargetView *views[8]{};ID3D11DepthStencilView *dsv{};context->OMGetRenderTargets(8,views,&dsv);
        for(unsigned i=0;i<8;i++){targets[i].Attach(views[i]);if(views[i])targetCount=i+1;}depth.Attach(dsv);
        ID3D11BlendState *blend{};context->OMGetBlendState(&blend,factors,&sampleMask);expectedBlend.Attach(blend);
        ID3D11PixelShader *shader{};context->PSGetShader(&shader,nullptr,nullptr);expectedPS.Attach(shader);
        auto deviceTable=*reinterpret_cast<void***>(gpu),table=*reinterpret_cast<void***>(ctx);
        Hook(table[9],reinterpret_cast<void*>(&ShaderHook),&originalPS);Hook(table[33],reinterpret_cast<void*>(&TargetHook),&originalRT);
        Hook(table[34],reinterpret_cast<void*>(&UavHook),&originalUAV);Hook(table[35],reinterpret_cast<void*>(&BlendHook),&originalBlend);
        Hook(table[74],reinterpret_cast<void*>(&ShaderGetHook),&originalGetPS);Hook(table[89],reinterpret_cast<void*>(&TargetGetHook),&originalGetRT);
        Hook(table[90],reinterpret_cast<void*>(&UavGetHook),&originalGetUAV);Hook(table[91],reinterpret_cast<void*>(&BlendGetHook),&originalGetBlend);
        owner.store(this);
        try{Hook(deviceTable[15],reinterpret_cast<void*>(&CreateHook),&originalCreate);}
        catch(...){owner.store(nullptr);BCryptCloseAlgorithmProvider(hash,0);hash=nullptr;throw;}
    }
    ~EffectMrtCapture(){owner.store(nullptr);if(hash)BCryptCloseAlgorithmProvider(hash,0);}
    void Enable(bool value){enabled=value;}
    void Resize(unsigned w,unsigned h){width=w;height=h;}
    void BeginFrame(int value)
    {
        if(frame!=value)
        {
            frame=value;FLOAT clear[]={0,0,0,1};for(auto &layer:layers){layer.touched=false;if(layer.target)context->ClearRenderTargetView(layer.target.Get(),clear);}
        }
        world=true;Apply();
    }
    void EndWorld(){world=false;Apply();}
    ID3D11Texture2D *Image(unsigned layer,int value)const
    {return layer>=1&&layer<=2&&frame==value&&layers[layer-1].touched?layers[layer-1].image.Get():nullptr;}
    void Read(long long *values)const
    {values[0]=recognized.load();values[1]=attachments.load();values[2]=unsupported.load();values[3]=rewriteFailures.load();}
};
