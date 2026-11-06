#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "EffectMrtCapture.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <iostream>
#include <cmath>
#include <filesystem>
#include <fstream>
using Microsoft::WRL::ComPtr;
struct EffectMrtFixture
{
    static void Dump(EffectMrtCapture &capture,const char *stage)
    {
        ID3D11RenderTargetView *views[2]{};capture.context->OMGetRenderTargets(2,views,nullptr);
        std::cout<<"STATE "<<stage<<" count="<<capture.targetCount<<" attached="<<capture.attached<<" world="<<capture.world
            <<" uav="<<capture.uavMode<<" base0="<<capture.targets[0].Get()<<" base1="<<capture.targets[1].Get()
            <<" gpu0="<<views[0]<<" gpu1="<<views[1]<<"\n";
        for(auto *view:views)if(view)view->Release();
    }
    static void Register(EffectMrtCapture &capture,ID3D11PixelShader *shader,ID3DBlob *code,unsigned layer=2)
    {capture.Register(shader,code->GetBufferPointer(),code->GetBufferSize(),layer);}
    static unsigned Layer(EffectMrtCapture &capture,ID3D11PixelShader *shader)
    {auto *program=capture.Find(shader);return program?program->layer:0;}
};
static void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("MRT hook probe HRESULT "+std::to_string(unsigned(hr)));}
static ComPtr<ID3DBlob> Compile(const char *source,const char *profile)
{
    ComPtr<ID3DBlob> code,error;Check(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main",profile,0,0,&code,&error));return code;
}
static std::vector<float> Read(ID3D11Device *device,ID3D11DeviceContext *context,ID3D11Texture2D *image)
{
    D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    unsigned stride=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:16;ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc,nullptr,&staging));context->CopyResource(staging.Get(),image);
    D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::vector<float> result(desc.Width*desc.Height*4);
    for(unsigned y=0;y<desc.Height;y++)for(unsigned x=0;x<desc.Width*4;x++)
    {
        auto *row=static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch;
        if(stride==16)memcpy(&result[y*desc.Width*4+x],row+x*4,4);
        else
        {
            uint16_t value{};memcpy(&value,row+x*2,2);int exponent=(value>>10)&31;unsigned fraction=value&1023;
            float f=exponent?std::ldexp(1.f+fraction/1024.f,exponent-15):std::ldexp(float(fraction),-24);
            result[y*desc.Width*4+x]=value&0x8000?-f:f;
        }
    }
    context->Unmap(staging.Get(),0);return result;
}
int main(int argc,char **argv)
{
    try
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        auto vs=Compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0");
        auto ps=Compile("float4 main(float4 p:SV_Position):SV_Target{if(p.x>32&&p.y<10)discard;return float4(.3,.2,.5,.4);}","ps_5_0");
        auto other=Compile("float4 main():SV_Target{return float4(.8,.1,.2,1);}","ps_5_0");
        ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> original,unknown;
        Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
        Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&original));
        Check(device->CreatePixelShader(other->GetBufferPointer(),other->GetBufferSize(),nullptr,&unknown));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=64;desc.Height=48;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> scene;ComPtr<ID3D11RenderTargetView> target;Check(device->CreateTexture2D(&desc,nullptr,&scene));Check(device->CreateRenderTargetView(scene.Get(),nullptr,&target));
        auto *rt=target.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        D3D11_VIEWPORT viewport{0,0,64,48,0,1};context->RSSetViewports(1,&viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(original.Get(),nullptr,0);
        D3D11_BLEND_DESC blend{};auto &b=blend.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA;
        b.DestBlend=b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=15;
        ComPtr<ID3D11BlendState> alpha;Check(device->CreateBlendState(&blend,&alpha));context->OMSetBlendState(alpha.Get(),nullptr,0xffffffff);
        FLOAT background[]={.18f,.24f,.3f,1};context->ClearRenderTargetView(rt,background);context->Draw(3,0);
        auto once=Read(device.Get(),context.Get(),scene.Get());context->Draw(3,0);auto twice=Read(device.Get(),context.Get(),scene.Get());
        EffectMrtCapture capture(device.Get(),context.Get(),64,48);EffectMrtFixture::Register(capture,original.Get(),ps.Get());
        unsigned corpus=0;
        if(argc>1)for(const auto &file:std::filesystem::directory_iterator(argv[1]))
        {
            if(file.path().extension()!=".dxbc")continue;
            std::ifstream input(file.path(),std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),{});
            unsigned expected=unsigned(file.path().filename().string()[0]-'0');
            try{DxbcMrt::DuplicateColor(bytes.data(),bytes.size(),expected==3);}
            catch(const std::exception &error){throw std::runtime_error(file.path().filename().string()+": "+error.what());}
            ComPtr<ID3D11PixelShader> shader;Check(device->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&shader));
            if(EffectMrtFixture::Layer(capture,shader.Get())!=expected)throw std::runtime_error("Installed shader was not recognized or could not be rewritten: "+file.path().filename().string());
            corpus++;
        }
        capture.Enable(true);capture.BeginFrame(1);context->PSSetShader(original.Get(),nullptr,0);context->ClearRenderTargetView(rt,background);context->Draw(3,0);
        if(Read(device.Get(),context.Get(),scene.Get())!=once)throw std::runtime_error("Shader hook changed original pixels");
        auto *image=capture.Image(2,1);if(!image)throw std::runtime_error("Single-pass effect image missing");
        auto isolated=Read(device.Get(),context.Get(),image);
        ID3D11PixelShader *visibleShader{};context->PSGetShader(&visibleShader,nullptr,nullptr);
        bool originalVisible=visibleShader==original.Get();if(visibleShader)visibleShader->Release();
        if(!originalVisible)throw std::runtime_error("State queries exposed the replacement shader");
        ID3D11BlendState *visibleBlend{};context->OMGetBlendState(&visibleBlend,nullptr,nullptr);
        bool originalBlendVisible=visibleBlend==alpha.Get();if(visibleBlend)visibleBlend->Release();
        if(!originalBlendVisible)throw std::runtime_error("State queries exposed the replacement blend state");
        for(unsigned i=0;i<64*48;i++)for(unsigned c=0;c<3;c++)
            if(std::abs(isolated[i*4+c]+isolated[i*4+3]*background[c]-once[i*4+c])>1.5f/255)
                throw std::runtime_error("Tracked single-pass effect did not recompose");
        {
            EffectMrtCapture::Suspend pause;ComPtr<ID3D11Device1> gpu;ComPtr<ID3D11DeviceContext1> immediate;Check(device.As(&gpu));Check(context.As(&immediate));
            D3D_FEATURE_LEVEL feature=device->GetFeatureLevel(),chosen{};ComPtr<ID3DDeviceContextState> state,saved;
            Check(gpu->CreateDeviceContextState(0,&feature,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&state));
            immediate->SwapDeviceContextState(state.Get(),&saved);context->OMSetRenderTargets(0,nullptr,nullptr);context->PSSetShader(unknown.Get(),nullptr,0);
            immediate->SwapDeviceContextState(saved.Get(),&state);
        }
        context->Draw(3,0);if(Read(device.Get(),context.Get(),scene.Get())!=twice)throw std::runtime_error("Private context capture broke original shader state");
        auto beforeUI=Read(device.Get(),context.Get(),image);EffectMrtFixture::Dump(capture,"before_UI");
        capture.EndWorld();EffectMrtFixture::Dump(capture,"after_end_world");
        context->PSSetShader(original.Get(),nullptr,0);EffectMrtFixture::Dump(capture,"after_UI_shader");context->Draw(3,0);
        if(Read(device.Get(),context.Get(),image)!=beforeUI)throw std::runtime_error("UI rendering leaked into effect layer");
        ID3D11RenderTargetView *views[2]{};context->OMGetRenderTargets(2,views,nullptr);bool extra=views[1]!=nullptr;
        for(auto *view:views)if(view)view->Release();if(extra)throw std::runtime_error("UI retained the extra render target");
        context->PSSetShader(unknown.Get(),nullptr,0);ID3D11PixelShader *actual{};context->PSGetShader(&actual,nullptr,nullptr);
        bool unchanged=actual==unknown.Get();if(actual)actual->Release();if(!unchanged)throw std::runtime_error("Unknown shader was replaced");
        auto skyCode=Compile("Texture2D<float4> background:register(t0);Texture2D<float4> clouds:register(t1);SamplerState pointState:register(s0);float4 main(float4 p:SV_Position):SV_Target{float3 radiance=float3(.08,.12,.2)+p.x*.0001;float4 bg=background.SampleLevel(pointState,p.xy/float2(64,48),0);float3 base=bg.rgb*bg.rgb*.7+radiance;float4 cloud=clouds.SampleLevel(pointState,p.xy/float2(64,48),0);return float4(sqrt(base*cloud.a+cloud.rgb),1);}","ps_4_0");
        ComPtr<ID3D11PixelShader> skyShader;Check(device->CreatePixelShader(skyCode->GetBufferPointer(),skyCode->GetBufferSize(),nullptr,&skyShader));
        EffectMrtFixture::Register(capture,skyShader.Get(),skyCode.Get(),3);
        D3D11_TEXTURE2D_DESC inputDesc=desc;inputDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> inputs[2];ComPtr<ID3D11ShaderResourceView> inputViews[2];
        for(unsigned i=0;i<2;i++){Check(device->CreateTexture2D(&inputDesc,nullptr,&inputs[i]));Check(device->CreateShaderResourceView(inputs[i].Get(),nullptr,&inputViews[i]));}
        D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        ComPtr<ID3D11SamplerState> pointState;Check(device->CreateSamplerState(&sampler,&pointState));auto *state=pointState.Get();context->PSSetSamplers(0,1,&state);
        ID3D11ShaderResourceView *textures[]={inputViews[0].Get(),inputViews[1].Get()};context->PSSetShaderResources(0,2,textures);
        std::vector<float> firstSky;
        for(unsigned test=0;test<2;test++)
        {
            for(unsigned input=0;input<2;input++)
            {
                std::vector<float> values(64*48*4);
                for(unsigned i=0;i<64*48;i++){values[i*4]=test?.9f:.1f;values[i*4+1]=test?.8f:.2f;values[i*4+2]=test?.7f:.3f;values[i*4+3]=input?(test?.2f:.6f):1.f;}
                context->UpdateSubresource(inputs[input].Get(),0,nullptr,values.data(),64*16,0);
            }
            capture.EndWorld();context->PSSetShader(skyShader.Get(),nullptr,0);context->Draw(3,0);auto skyReference=Read(device.Get(),context.Get(),scene.Get());
            capture.BeginFrame(int(test+2));context->PSSetShader(skyShader.Get(),nullptr,0);context->Draw(3,0);
            if(Read(device.Get(),context.Get(),scene.Get())!=skyReference)throw std::runtime_error("Independent sky tail changed original RGB bytes");
            auto ownSky=Read(device.Get(),context.Get(),capture.Image(3,int(test+2)));
            if(test && ownSky!=firstSky)throw std::runtime_error("Sky reconstruction borrowed cloud or background RGB");
            for(unsigned i=0;i<64*48;i++)for(unsigned channel=0;channel<3;channel++)
            {
                float radiance=(channel==0?.08f:channel==1?.12f:.2f)+(float(i%64)+.5f)*.0001f;
                if(std::abs(ownSky[i*4+channel]-std::sqrt(radiance))>.001f)throw std::runtime_error("Sky tail lost its own radiance");
            }
            firstSky=std::move(ownSky);
        }
        capture.EndWorld();
        FLOAT customFactor[]={.2f,.3f,.4f,.5f};context->OMSetBlendState(alpha.Get(),customFactor,0xffffffff);
        context->OMSetBlendState(alpha.Get(),nullptr,0xffffffff);
        FLOAT restoredFactor[4]{};context->OMGetBlendState(nullptr,restoredFactor,nullptr);
        for(float value:restoredFactor)if(value!=1.f)throw std::runtime_error("A null blend factor retained a previous factor");
        long long stats[4]{};capture.Read(stats);if(stats[3])throw std::runtime_error("Unexpected shader rewrite failure");
        std::cout<<"SINGLE_PASS_HOOK_GPU_PROBE_PASS original_bytes="<<once.size()*4<<" private_state_restored=1 UI_excluded=1 unknown_shader_unchanged=1 sky_background_cloud_independent=1 recognized="<<stats[0]<<" installed_variants="<<corpus<<"\n";
    }
    catch(const std::exception &error){std::cerr<<error.what()<<"\n";return 1;}
}
