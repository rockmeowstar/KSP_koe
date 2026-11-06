#pragma once
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <stdexcept>
#include <cstring>

class DeferredStencil
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext1> context;
    Ptr<ID3DDeviceContextState> isolated;
    Ptr<ID3D11VertexShader> vertex;
    Ptr<ID3D11PixelShader> pixel;
    Ptr<ID3D11RasterizerState> raster;
    Ptr<ID3D11Buffer> constants;
    Ptr<ID3D11Texture2D> snapshot, previousSource, previousTarget;
    Ptr<ID3D11ShaderResourceView> stencilView, depthView;
    Ptr<ID3D11RenderTargetView> targetView;

    static void Check(HRESULT result)
    {
        if (FAILED(result)) throw std::runtime_error("Deferred stencil D3D11 operation failed");
    }

public:
    explicit DeferredStencil(ID3D11Device *source) : device(source)
    {
        Ptr<ID3D11Device1> version1;Check(device.As(&version1));
        Ptr<ID3D11DeviceContext> immediate;device->GetImmediateContext(&immediate);Check(immediate.As(&context));
        auto level=device->GetFeatureLevel();
        Check(version1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&isolated));
        const char *vs="float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";
        const char *ps=R"HLSL(
Texture2D<uint2> stencilImage:register(t0);
Texture2D<float> depthImage:register(t1);
cbuffer Options:register(b0){uint width,height,flip;float clearDepth;uint semanticOverride;uint3 padding;}
float4 main(float4 position:SV_Position):SV_Target
{
    int2 p=int2(position.xy);
    if(flip)p.y=int(height)-1-p.y;
    uint category=semanticOverride==0?(stencilImage.Load(int3(p,0)).g & 3):0;
    float depth=depthImage.Load(int3(p,0));
    if(category==0 && depth==clearDepth)discard;
    uint kind=semanticOverride!=0?semanticOverride:category==1?1:category==2?2:5;
    return float4(kind/255.0,0,0,128.0/255);
}
)HLSL";
        Ptr<ID3DBlob> code,error;
        Check(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error));
        Check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex));
        code.Reset();error.Reset();
        Check(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error));
        Check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&pixel));
        D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;
        rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
        Check(device->CreateRasterizerState(&rasterDesc,&raster));
        D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=32;bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bufferDesc,nullptr,&constants));
    }

    bool Capture(ID3D11DepthStencilView *sourceView,ID3D11Texture2D *target,bool clear,bool flip,bool reverse,unsigned semanticOverride=0)
    {
        if(!sourceView || !target)return false;
        Ptr<ID3D11Resource> resource;sourceView->GetResource(&resource);
        Ptr<ID3D11Texture2D> source;if(FAILED(resource.As(&source)))return false;
        D3D11_TEXTURE2D_DESC desc{},targetDesc{};source->GetDesc(&desc);target->GetDesc(&targetDesc);
        if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.Width!=targetDesc.Width || desc.Height!=targetDesc.Height)return false;
        DXGI_FORMAT family,stencil=DXGI_FORMAT_UNKNOWN,depth;
        if(desc.Format==DXGI_FORMAT_R24G8_TYPELESS || desc.Format==DXGI_FORMAT_D24_UNORM_S8_UINT)
        {family=DXGI_FORMAT_R24G8_TYPELESS;stencil=DXGI_FORMAT_X24_TYPELESS_G8_UINT;depth=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;}
        else if(desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS || desc.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        {family=DXGI_FORMAT_R32G8X24_TYPELESS;stencil=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;depth=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;}
        else if(semanticOverride!=0 && (desc.Format==DXGI_FORMAT_R16_TYPELESS || desc.Format==DXGI_FORMAT_D16_UNORM))
        {family=DXGI_FORMAT_R16_TYPELESS;depth=DXGI_FORMAT_R16_UNORM;}
        else if(semanticOverride!=0 && (desc.Format==DXGI_FORMAT_R32_TYPELESS || desc.Format==DXGI_FORMAT_D32_FLOAT))
        {family=DXGI_FORMAT_R32_TYPELESS;depth=DXGI_FORMAT_R32_FLOAT;}
        else return false;
        if(previousSource.Get()!=source.Get())
        {
            previousSource.Reset();stencilView.Reset();depthView.Reset();snapshot.Reset();
            auto copyDesc=desc;copyDesc.Format=family;copyDesc.Usage=D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;copyDesc.CPUAccessFlags=copyDesc.MiscFlags=0;
            Check(device->CreateTexture2D(&copyDesc,nullptr,&snapshot));
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels=1;
            if(stencil!=DXGI_FORMAT_UNKNOWN)
            {view.Format=stencil;Check(device->CreateShaderResourceView(snapshot.Get(),&view,&stencilView));}
            view.Format=depth;Check(device->CreateShaderResourceView(snapshot.Get(),&view,&depthView));
            previousSource=source;
        }
        if(previousTarget.Get()!=target)
        {
            previousTarget.Reset();targetView.Reset();
            D3D11_RENDER_TARGET_VIEW_DESC view{};view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
            view.Format=targetDesc.Format;
            if(view.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS)view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            if(view.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS)view.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            Check(device->CreateRenderTargetView(target,&view,&targetView));previousTarget=target;
        }
        Ptr<ID3DDeviceContextState> previous;context->SwapDeviceContextState(isolated.Get(),&previous);
        struct Restore
        {
            DeferredStencil &owner;Ptr<ID3DDeviceContextState> previous;
            ~Restore(){owner.context->SwapDeviceContextState(previous.Get(),&owner.isolated);}
        } restore{*this,previous};
        context->CopyResource(snapshot.Get(),source.Get());
        auto *view=targetView.Get();context->OMSetRenderTargets(1,&view,nullptr);
        context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        if(clear){float empty[]={0,0,0,128.f/255};context->ClearRenderTargetView(view,empty);}
        D3D11_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};
        context->RSSetViewports(1,&viewport);context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
        struct Settings{UINT width,height,flip;float clearDepth;UINT semanticOverride,padding[3];}
            settings{desc.Width,desc.Height,flip?1u:0u,reverse?0.f:1.f,semanticOverride,{0,0,0}};
        context->UpdateSubresource(constants.Get(),0,nullptr,&settings,0,0);
        auto *buffer=constants.Get();context->PSSetConstantBuffers(0,1,&buffer);
        ID3D11ShaderResourceView *images[]={stencilView.Get(),depthView.Get()};context->PSSetShaderResources(0,2,images);
        context->Draw(3,0);
        images[0]=images[1]=nullptr;context->PSSetShaderResources(0,2,images);
        return true;
    }
};
