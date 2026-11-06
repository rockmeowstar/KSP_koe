#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "FireflyLayerCapture.h"
#include "IndependentSourceCapture.h"
#include <d3dcompiler.h>
#include <vector>
#include <cstring>
#include <iostream>
#include <cmath>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Capture probe D3D11 failure");}
static ComPtr<ID3DBlob> Compile(const char *code,const char *profile)
{
    ComPtr<ID3DBlob> blob,error;Check(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"main",profile,0,0,&blob,&error));return blob;
}
static std::vector<uint16_t> Read(ID3D11Device *device,ID3D11DeviceContext *context,ID3D11Texture2D *image,unsigned slice=0)
{
    D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.ArraySize=1;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;Check(device->CreateTexture2D(&desc,nullptr,&copy));context->CopySubresourceRegion(copy.Get(),0,0,0,0,image,slice,nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::vector<uint16_t> result(desc.Width*desc.Height*4);
    for(unsigned y=0;y<desc.Height;y++)memcpy(result.data()+y*desc.Width*4,static_cast<char*>(mapped.pData)+y*mapped.RowPitch,desc.Width*8);
    context->Unmap(copy.Get(),0);return result;
}
static float Float(uint16_t half)
{
    int exponent=int((half>>10)&31);unsigned fraction=half&1023;
    if(exponent==0)return std::ldexp(float(fraction),-24);
    float value=std::ldexp(1.f+fraction/1024.f,exponent-15);return half&0x8000?-value:value;
}
int main()
{
    try
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=64;desc.Height=48;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> scene;Check(device->CreateTexture2D(&desc,nullptr,&scene));
        ComPtr<ID3D11RenderTargetView> target;Check(device->CreateRenderTargetView(scene.Get(),nullptr,&target));
        auto vs=Compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0");
        auto ps=Compile("cbuffer Color:register(b0){float4 color;}float4 main(float4 p:SV_Position):SV_Target{return color;}","ps_5_0");
        ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
        Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
        Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel));
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> color;Check(device->CreateBuffer(&cb,nullptr,&color));
        ComPtr<ID3D11BlendState> blend[2];
        for(unsigned i=0;i<2;i++)
        {
            D3D11_BLEND_DESC d{};auto &rt=d.RenderTarget[0];rt.BlendEnable=TRUE;
            rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend=rt.DestBlendAlpha=i?D3D11_BLEND_ONE:D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
            Check(device->CreateBlendState(&d,&blend[i]));
        }
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
        auto *constant=color.Get();context->PSSetConstantBuffers(0,1,&constant);
        D3D11_VIEWPORT viewport{0,0,64,48,0,1};context->RSSetViewports(1,&viewport);
        auto draw=[&](){for(unsigned i=0;i<3;i++){
            float value[]={.3f+float(i)*.1f,.2f,.5f,.4f-float(i)*.1f};
            context->UpdateSubresource(color.Get(),0,nullptr,value,0,0);
            context->OMSetBlendState(blend[i?1:0].Get(),nullptr,0xffffffff);context->Draw(3,0);
        }};
        auto *rt=target.Get();float background[]={.18f,.24f,.3f,1};
        context->OMSetRenderTargets(1,&rt,nullptr);context->ClearRenderTargetView(target.Get(),background);draw();
        auto reference=Read(device.Get(),context.Get(),scene.Get());
        context->ClearRenderTargetView(target.Get(),background);
        FireflyLayerCapture capture(device.Get(),context.Get());
        if(!capture.Begin())throw std::runtime_error("Offscreen capture did not begin");draw();capture.End();
        if(!capture.Image())throw std::runtime_error("Offscreen capture invalid");
        auto isolated=Read(device.Get(),context.Get(),capture.Image());draw();
        auto actual=Read(device.Get(),context.Get(),scene.Get());
        if(actual!=reference)throw std::runtime_error("Original scene bytes changed after isolated replay");
        auto t=Float(isolated[3]);if(std::abs(t-.6f)>.002f)throw std::runtime_error("Firefly transmittance is wrong");
        for(unsigned channel=0;channel<3;channel++)
            if(std::abs(Float(isolated[channel])+t*background[channel]-Float(reference[channel]))>1.5f/255)
                throw std::runtime_error("Independent Firefly does not recompose the original scene");
        // Construct and compile the actual source packer on this device as well.
        IndependentSourceCapture source(device.Get(),context.Get(),64,48);
        context->ClearRenderTargetView(target.Get(),background);
        D3D11_TEXTURE2D_DESC maskDesc=desc;maskDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        std::vector<uint8_t> mask(64*48*4);
        for(unsigned i=0;i<64*48;i++){mask[i*4]=1;mask[i*4+3]=128;}
        D3D11_SUBRESOURCE_DATA initial{mask.data(),64*4,0};ComPtr<ID3D11Texture2D> labels;
        Check(device->CreateTexture2D(&maskDesc,&initial,&labels));
        source.BeginFrame();source.CaptureOpaque(scene.Get(),labels.Get());
        auto empty=mask;for(auto &byte:empty)byte=0;initial.pSysMem=empty.data();ComPtr<ID3D11Texture2D> noGeometry;
        Check(device->CreateTexture2D(&maskDesc,&initial,&noGeometry));
        float laterBackground[]={.5f,.6f,.7f,1};context->ClearRenderTargetView(target.Get(),laterBackground);
        source.CaptureOpaque(scene.Get(),noGeometry.Get());
        if(!source.Capture(scene.Get(),labels.Get(),nullptr,capture.Image()))throw std::runtime_error("Source packing failed");
        auto craft=Read(device.Get(),context.Get(),source.Image(),0);
        auto terrain=Read(device.Get(),context.Get(),source.Image(),1);
        if(Float(craft[3])!=1 || std::abs(Float(craft[0])-background[0])>.001f)
            throw std::runtime_error("Opaque source was not separated into the craft layer");
        if(std::round(Float(terrain[0])*255)!=1)throw std::runtime_error("Opaque owner is not the craft");
        ComPtr<ID3D11Texture2D> sky;Check(device->CreateTexture2D(&desc,nullptr,&sky));
        ComPtr<ID3D11RenderTargetView> skyTarget;Check(device->CreateRenderTargetView(sky.Get(),nullptr,&skyTarget));
        float skyColor[]={.12f,.31f,.57f,0};context->ClearRenderTargetView(skyTarget.Get(),skyColor);
        if(!source.Capture(scene.Get(),labels.Get(),nullptr,nullptr,sky.Get()))throw std::runtime_error("Sky source packing failed");
        auto protectedCraft=Read(device.Get(),context.Get(),source.Image(),0);
        if(protectedCraft!=craft)throw std::runtime_error("Sky source contaminated craft pixels");
        source.BeginFrame();source.CaptureOpaque(scene.Get(),noGeometry.Get());
        if(!source.Capture(scene.Get(),noGeometry.Get(),nullptr,nullptr,sky.Get()))throw std::runtime_error("Sky source missing");
        auto skyPixels=Read(device.Get(),context.Get(),source.Image(),0);
        for(unsigned channel=0;channel<3;channel++)if(std::abs(Float(skyPixels[channel])-skyColor[channel])>.001f)
            throw std::runtime_error("Independent sky color did not reach its own layer");
        skyColor[3]=1;context->ClearRenderTargetView(skyTarget.Get(),skyColor);
        if(!source.Capture(scene.Get(),noGeometry.Get(),nullptr,nullptr,sky.Get(),nullptr,nullptr,nullptr,nullptr,true))
            throw std::runtime_error("Single-pass sky source packing failed");
        auto directSky=Read(device.Get(),context.Get(),source.Image(),0);
        for(unsigned channel=0;channel<3;channel++)if(std::abs(Float(directSky[channel])-skyColor[channel])>.001f)
            throw std::runtime_error("Single-pass sky availability was confused with transmittance");
        skyColor[3]=0;context->ClearRenderTargetView(skyTarget.Get(),skyColor);
        auto skyCode=Compile("Texture2D<float4> background:register(t0);float4 main(float4 p:SV_Position):SV_Target{return float4(background.Load(int3(p.xy,0)).rgb*.5+float3(.05,.07,.09),1);}","ps_5_0");
        ComPtr<ID3D11PixelShader> skyShader;Check(device->CreatePixelShader(skyCode->GetBufferPointer(),skyCode->GetBufferSize(),nullptr,&skyShader));
        ComPtr<ID3D11ShaderResourceView> backgroundView;Check(device->CreateShaderResourceView(sky.Get(),nullptr,&backgroundView));
        auto *backgroundInput=backgroundView.Get();context->PSSetShaderResources(0,1,&backgroundInput);
        context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->PSSetShader(skyShader.Get(),nullptr,0);context->Draw(3,0);
        auto skyReference=Read(device.Get(),context.Get(),scene.Get());
        FireflyLayerCapture isolatedSky(device.Get(),context.Get());
        if(!isolatedSky.Begin(true))throw std::runtime_error("Pure sky capture did not begin");
        context->PSSetShaderResources(0,1,&backgroundInput);context->Draw(3,0);isolatedSky.End();
        auto pureSky=Read(device.Get(),context.Get(),isolatedSky.Image());
        float ownSky[]={.05f,.07f,.09f};
        for(unsigned channel=0;channel<3;channel++)if(std::abs(Float(pureSky[channel])-ownSky[channel])>.001f)
            throw std::runtime_error("Sky source still contains background colors");
        context->Draw(3,0);
        if(Read(device.Get(),context.Get(),scene.Get())!=skyReference)
            throw std::runtime_error("Sky isolation failed to restore original shader resources");
        auto constantTexture=[&](float r,float g,float b,float a)
        {
            std::vector<float> data(64*48*4);float value[]={r,g,b,a};
            for(unsigned i=0;i<64*48;i++)for(unsigned channel=0;channel<4;channel++)data[i*4+channel]=value[channel];
            D3D11_TEXTURE2D_DESC textureDesc=desc;textureDesc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
            D3D11_SUBRESOURCE_DATA input{data.data(),64*16,0};ComPtr<ID3D11Texture2D> texture;
            Check(device->CreateTexture2D(&textureDesc,&input,&texture));return texture;
        };
        auto radiance=constantTexture(.2f,.3f,.4f,.8f),transmission=constantTexture(.5f,.2f,0,0);
        auto depth=constantTexture(.5f,0,0,0),plume=constantTexture(.1f,.2f,.05f,.4f);
        source.BeginFrame();source.CaptureOpaque(scene.Get(),noGeometry.Get());
        if(!source.Capture(scene.Get(),noGeometry.Get(),nullptr,capture.Image(),sky.Get(),radiance.Get(),transmission.Get(),depth.Get(),plume.Get()))
            throw std::runtime_error("Atmosphere and Waterfall source packing failed");
        auto unmodifiedSky=Read(device.Get(),context.Get(),source.Image(),0);
        for(unsigned channel=0;channel<3;channel++)if(std::abs(Float(unmodifiedSky[channel])-skyColor[channel])>.001f)
            throw std::runtime_error("Atmosphere was baked into the sky source");
        auto atmosphericColor=Read(device.Get(),context.Get(),source.Image(),4);
        auto atmosphericT=Read(device.Get(),context.Get(),source.Image(),5);
        if(Float(atmosphericColor[3])!=1 || std::abs(Float(atmosphericT[0])-.8f)>.001f
            || std::abs(Float(atmosphericT[1])-.5f)>.001f || std::abs(Float(atmosphericT[2])-.2f)>.001f)
            throw std::runtime_error("Atmosphere RGB transmittance channels were lost");
        auto emission=Read(device.Get(),context.Get(),source.Image(),3);float plumeColor[]={.1f,.2f,.05f};
        for(unsigned channel=0;channel<3;channel++)if(std::abs(Float(emission[channel])-Float(isolated[channel])-t*plumeColor[channel])>.001f)
            throw std::runtime_error("Atmosphere contaminated isolated effect emission");
        if(std::abs(Float(emission[3])-(1-t*.4f))>.001f)throw std::runtime_error("Combined effect transmittance is incorrect");
        D3D11_TEXTURE2D_DESC shared=desc;shared.ArraySize=LayerReconstruction::PackedSlices;
        shared.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> array;Check(device->CreateTexture2D(&shared,nullptr,&array));
        std::cout<<"INDEPENDENT_CAPTURE_GPU_PROBE_PASS original_bytes="<<actual.size()*2<<" transmission="<<t<<" craft_terrain_separate=1 sky_isolated=1 sky_background_excluded=1 shared_array=1\n";
        return 0;
    }
    catch(const std::exception &error){std::cerr<<error.what()<<"\n";return 1;}
}
