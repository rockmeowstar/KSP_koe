// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <map>
#include <stdexcept>

class FireflyLayerCapture
{
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    using SetBlend=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11BlendState*,const FLOAT*,UINT);
    inline static SetBlend original{};
    inline static FireflyLayerCapture *active{};
    struct Blend { Ptr<ID3D11BlendState> source,layer; };
    std::map<ID3D11BlendState*,Blend> states;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> image;
    Ptr<ID3D11RenderTargetView> target,previousTargets[8];
    Ptr<ID3D11DepthStencilView> previousDepth;
    Ptr<ID3D11BlendState> lastBlend;
    FLOAT lastFactors[4]{};
    UINT lastMask{};
    bool valid{},recording{};
    static void Check(HRESULT value){if(FAILED(value))throw std::runtime_error("Firefly layer capture D3D11 failure");}
    static void STDMETHODCALLTYPE BlendHook(ID3D11DeviceContext *ctx,ID3D11BlendState *state,const FLOAT *factor,UINT mask)
    {
        auto *capture=active;
        if(capture && capture->recording && capture->context.Get()==ctx)
        {
            capture->lastBlend=state;capture->lastMask=mask;
            if(factor)memcpy(capture->lastFactors,factor,sizeof(capture->lastFactors));
            try
            {
                auto it=capture->states.find(state);
                if(it==capture->states.end())
                {
                    D3D11_BLEND_DESC desc{};
                    if(state)state->GetDesc(&desc);
                    else
                    {
                        desc.RenderTarget[0].SrcBlend=desc.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
                        desc.RenderTarget[0].DestBlend=desc.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
                        desc.RenderTarget[0].BlendOp=desc.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
                        desc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
                    }
                    auto &rt=desc.RenderTarget[0];
                    if(!rt.BlendEnable){rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_ONE;rt.DestBlend=D3D11_BLEND_ZERO;rt.BlendOp=D3D11_BLEND_OP_ADD;}
                    if(desc.IndependentBlendEnable || rt.BlendOp!=D3D11_BLEND_OP_ADD
                        || (rt.DestBlend!=D3D11_BLEND_ONE && rt.DestBlend!=D3D11_BLEND_ZERO
                            && rt.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA && rt.DestBlend!=D3D11_BLEND_SRC_ALPHA)
                        || (rt.RenderTargetWriteMask&D3D11_COLOR_WRITE_ENABLE_ALL)!=D3D11_COLOR_WRITE_ENABLE_ALL)
                        throw std::runtime_error("Unsupported Firefly layer blend");
                    rt.SrcBlendAlpha=D3D11_BLEND_ZERO;rt.DestBlendAlpha=rt.DestBlend;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
                    Blend value;value.source=state;Check(capture->device->CreateBlendState(&desc,&value.layer));
                    it=capture->states.emplace(state,std::move(value)).first;
                }
                original(ctx,it->second.layer.Get(),factor,mask);return;
            }
            catch(...){capture->valid=false;}
        }
        original(ctx,state,factor,mask);
    }
public:
    FireflyLayerCapture(ID3D11Device *gpu,ID3D11DeviceContext *ctx):device(gpu),context(ctx)
    {
        if(!original)
        {
            auto table=*reinterpret_cast<void***>(ctx);
            auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("Firefly layer hook initialization failed");
            if(MH_CreateHook(table[35],reinterpret_cast<void*>(&BlendHook),reinterpret_cast<void**>(&original))!=MH_OK)
                throw std::runtime_error("Firefly layer blend hook failed");
            auto enabled=MH_EnableHook(table[35]);if(enabled!=MH_OK && enabled!=MH_ERROR_ENABLED)throw std::runtime_error("Firefly layer blend hook enable failed");
        }
    }
    ~FireflyLayerCapture(){if(recording)End();if(active==this)active=nullptr;}
    bool Begin()
    {
        if(active||recording)return false;
        ID3D11RenderTargetView *targets[8]{};ID3D11DepthStencilView *depth{};
        context->OMGetRenderTargets(8,targets,&depth);
        for(unsigned i=0;i<8;i++)previousTargets[i].Attach(targets[i]);previousDepth.Attach(depth);
        if(!previousTargets[0])return false;
        Ptr<ID3D11Resource> resource;previousTargets[0]->GetResource(&resource);
        Ptr<ID3D11Texture2D> source;Check(resource.As(&source));D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1||desc.ArraySize!=1)return false;
        D3D11_TEXTURE2D_DESC old{};if(image)image->GetDesc(&old);
        if(!image||old.Width!=desc.Width||old.Height!=desc.Height)
        {
            target.Reset();image.Reset();desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.MipLevels=desc.ArraySize=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags=desc.CPUAccessFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;
            Check(device->CreateTexture2D(&desc,nullptr,&image));Check(device->CreateRenderTargetView(image.Get(),nullptr,&target));
        }
        ID3D11BlendState *blend{};context->OMGetBlendState(&blend,lastFactors,&lastMask);lastBlend.Attach(blend);
        float black[4]={0,0,0,1};context->ClearRenderTargetView(target.Get(),black);
        auto *rt=target.Get();context->OMSetRenderTargets(1,&rt,previousDepth.Get());
        valid=recording=true;active=this;
        // Set the cached state's tracked-alpha equivalent even if Unity skips a redundant state call.
        BlendHook(context.Get(),lastBlend.Get(),lastFactors,lastMask);
        return true;
    }
    void End()
    {
        if(!recording)return;
        active=nullptr;recording=false;
        ID3D11RenderTargetView *targets[8]{};for(unsigned i=0;i<8;i++)targets[i]=previousTargets[i].Get();
        context->OMSetRenderTargets(8,targets,previousDepth.Get());
        original(context.Get(),lastBlend.Get(),lastFactors,lastMask);
        for(auto &rt:previousTargets)rt.Reset();previousDepth.Reset();lastBlend.Reset();
    }
    ID3D11Texture2D *Image()const{return valid&&!recording?image.Get():nullptr;}
};
