#include "ConfidenceGate.h"
#include "OpticalD3D11.h"
#include <d3dcompiler.h>
#include <vector>
#include <array>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <cmath>
#include <algorithm>
using Microsoft::WRL::ComPtr;
static unsigned checks{};
static void Require(bool value, const char *text)
{
    ++checks;
    if (!value)
        throw std::runtime_error(text);
}
static void Check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D fixture failure " + std::to_string(unsigned(hr)));
}
static ComPtr<ID3DBlob> Compile(const char *text, const char *profile, const D3D_SHADER_MACRO *macros = nullptr)
{
    ComPtr<ID3DBlob> code, error;
    auto hr = D3DCompile(text, strlen(text), nullptr, macros, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                         0, &code, &error);
    if (FAILED(hr))
        throw std::runtime_error(error ? static_cast<const char *>(error->GetBufferPointer())
                                       : "shader compilation failed");
    return code;
}
// AI-generated comment: Deterministic flow provider for the positive pipeline case. The real NVOF
// driver is exercised separately; never weaken vetoes to make it approve an
// ambiguous/featureless field. This fixture has no production entry point.
struct ConfidenceGateFixture
{
    static ID3D11ShaderResourceView *CurrentImage(OpticalD3D11 &value, const OpticalD3D11::Ticket &source)
    {
        return value.imageViews[source.after].Get();
    }
    static std::vector<unsigned char> Controlled(OpticalD3D11 &value, OpticalD3D11::Ticket &ticket,
                                                  bool mixed = false, unsigned delta = 2)
    {
        Require(value.Wait(), "Controlled flow cannot overwrite in-flight resources");
        std::vector<unsigned char> image(value.width * value.height * 4, 80);
        for (unsigned i = 3; i < image.size(); i += 4)
            image[i] = 255;
        if (mixed)
            for (unsigned i = 0; i < image.size(); i += 4)
            {
                image[i + 1] = 110;
                image[i + 2] = 150;
            }
        value.context->UpdateSubresource(value.inputs[ticket.before].Get(), 0, nullptr, image.data(), value.width * 4,
                                         0);
        for (unsigned i = 0; i < image.size(); ++i)
            if (i % 4 != 3)
                image[i] += static_cast<unsigned char>(delta);
        if (mixed)
            for (unsigned y = 0; y < value.height; ++y)
                for (unsigned x = 0; x < value.width; ++x)
                    if (x < 16 || x + 16 >= value.width || x == value.width / 2)
                        for (unsigned c = 0; c < 3; ++c)
                            image[(y * value.width + x) * 4 + c] = x < 16 ? 0 : x + 16 >= value.width ? 255 : 200;
        value.context->UpdateSubresource(value.inputs[ticket.after].Get(), 0, nullptr, image.data(), value.width * 4,
                                         0);
        unsigned gw = (value.width + 3) / 4, gh = (value.height + 3) / 4;
        std::vector<short> field(gw * gh * 2, 0);
        for (auto &flow : value.flow)
            value.context->UpdateSubresource(flow.Get(), 0, nullptr, field.data(), gw * 4, 0);
        value.Analyze(ticket);
        Check(value.context->Signal(value.fence.Get(), ++value.sequence));
        ticket.fenceValue = value.sequence;
        value.context->Flush();
        for (unsigned i = 0; i < image.size(); i += 4)
            std::swap(image[i], image[i + 2]);
        return image;
    }
    static void QueueMasked(OpticalD3D11 &value, OpticalD3D11::Ticket &ticket)
    {
        value.QueueMaskedWarp(ticket);
        value.Signal(ticket);
    }
    static void Silhouette(OpticalD3D11 &value)
    {
        std::vector<unsigned char> pixels(value.width * value.height * 4, 0);
        for (unsigned direction = 0; direction < 2; ++direction)
        {
            for (unsigned y = 0; y < value.height; ++y)
                for (unsigned x = 0; x < value.width; ++x)
                {
                    auto offset = (y * value.width + x) * 4;
                    bool hull = x < value.width / 2;
                    if (direction && y < value.height / 2) hull = !hull;
                    pixels[offset] = pixels[offset+1] = pixels[offset+2] = hull ? 255 : 0;
                    pixels[offset+3] = 255;
                }
            value.context->UpdateSubresource(value.silhouettes[direction].Get(), 0, nullptr, pixels.data(), value.width*4, 0);
            value.silhouetteValid[direction] = true;
        }
        value.SetPhase(.5f);
    }
    static void SilhouetteSamples(OpticalD3D11 &value, const OpticalD3D11::Ticket &ticket)
    {
        Require(value.Wait(), "Silhouette sampler test overlaps pending GPU work");
        std::vector<unsigned char> image(value.width*value.height*4,255);
        for(unsigned y=0;y<value.height;++y)
            for(unsigned x=0;x<value.width;++x)
                for(unsigned c=0;c<3;++c)image[(y*value.width+x)*4+c]=x<value.width/2?220:40;
        value.context->UpdateSubresource(value.inputs[ticket.before].Get(),0,nullptr,image.data(),value.width*4,0);
        unsigned gw=(value.width+3)/4,gh=(value.height+3)/4;
        std::vector<short> flow(gw*gh*2,0);
        for(unsigned y=0;y<gh;++y)
            for(unsigned x=0;x<gw;++x)flow[(y*gw+x)*2]=x*4+1.5f<value.width/2?64:-96;
        value.context->UpdateSubresource(value.flow[0].Get(),0,nullptr,flow.data(),gw*4,0);
        std::ifstream file(std::filesystem::path(__FILE__).parent_path().parent_path()/"stock/FrameGeneration/OpticalD3D11.cpp");
        std::string source((std::istreambuf_iterator<char>(file)),{});
        auto start=source.find("const char *pixel = R\"HLSL(");
        Require(start!=std::string::npos,"Production warp shader missing");
        start=source.find('(',start)+1;
        auto end=source.find(")HLSL\";",start);
        std::string shader=source.substr(start,end-start)+R"HLSL(
RWStructuredBuffer<float4> samples:register(u0);
[numthreads(1,1,1)]void sampleMain(uint3 id:SV_DispatchThreadID)
{
 float4 hull,background;
 bool a=sampleHullColor(float2(size.x*.5-.25,size.y*.75),false,true,hull);
 bool b=sampleHullColor(float2(size.x*.5-.25,size.y*.75),false,false,background);
 samples[0]=float4(hull.rgb,a?1:0);samples[1]=float4(background.rgb,b?1:0);
 samples[2]=float4(flowAt(float2(size.x*.5-.75,size.y*.75),false),0,0);
 samples[3]=float4(flowAt(float2(size.x*.5-.25,size.y*.75),false),0,0);
}
)HLSL";
        D3D_SHADER_MACRO macros[]={{"KOE_CONSERVATIVE_FLOW","1"},{"KOE_LOCAL_MASK","1"},{"KOE_TEMPORAL_PHASE","1"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> code,error;
        auto compiled=D3DCompile(shader.data(),shader.size(),nullptr,macros,nullptr,"sampleMain","cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(compiled))throw std::runtime_error(error?static_cast<char *>(error->GetBufferPointer()):"Silhouette sampler compilation failed");
        ComPtr<ID3D11ComputeShader> cs;
        Check(value.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs));
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=64;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=16;
        ComPtr<ID3D11Buffer> results,read;
        Check(value.device->CreateBuffer(&desc,nullptr,&results));
        ComPtr<ID3D11UnorderedAccessView> target;
        Check(value.device->CreateUnorderedAccessView(results.Get(),nullptr,&target));
        desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(value.device->CreateBuffer(&desc,nullptr,&read));
        ID3D11ShaderResourceView *views[]={value.imageViews[ticket.before].Get(),value.imageViews[ticket.after].Get(),
            value.flowViews[0].Get(),value.flowViews[1].Get(),value.maskView.Get(),
            value.silhouetteViews[ticket.before].Get(),value.silhouetteViews[ticket.after].Get()};
        auto cb=value.constants.Get();auto uav=target.Get();
        value.context->CSSetShader(cs.Get(),nullptr,0);value.context->CSSetConstantBuffers(0,1,&cb);
        value.context->CSSetShaderResources(0,7,views);value.context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        value.context->Dispatch(1,1,1);
        for(auto &view:views)view=nullptr;uav=nullptr;
        value.context->CSSetShaderResources(0,7,views);value.context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        value.context->CSSetShader(nullptr,nullptr,0);value.context->CopyResource(read.Get(),results.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(value.context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        float actual[16];memcpy(actual,mapped.pData,sizeof(actual));value.context->Unmap(read.Get(),0);
        Require(actual[3]==1 && std::abs(actual[0]-220.f/255)<.0001f,"Hull sampling imported background color");
        Require(actual[7]==1 && std::abs(actual[4]-40.f/255)<.0001f,"Background sampling imported hull color");
        Require(std::abs(actual[8]-2)<.0001f,"Hull motion blended with background motion");
        Require(std::abs(actual[12]+3)<.0001f,"Background motion blended with hull motion");
    }
    static void SceneBoundaries(OpticalD3D11 &value, OpticalD3D11::Ticket &ticket)
    {
        Require(value.Wait(), "Scene boundary test overlaps GPU work");
        std::vector<unsigned char> pixels(value.width*value.height*4,80);
        std::vector<unsigned char> labels(pixels.size(),0);
        for(unsigned y=0;y<value.height;++y)
            for(unsigned x=0;x<value.width;++x)
            {
                auto offset=(y*value.width+x)*4;
                pixels[offset+3]=255;
                unsigned section=x/(value.width/8);
                labels[offset]=static_cast<unsigned char>(section+1);
                labels[offset+1]=17;
                labels[offset+2]=23;
                labels[offset+3]=y<value.height/4?128:255;
                if(y>=value.height/2)pixels[offset]=pixels[offset+1]=pixels[offset+2]=160;
            }
        unsigned gw=(value.width+3)/4,gh=(value.height+3)/4;
        std::vector<short> vectors(gw*gh*2,0);
        for(unsigned i=0;i<2;++i)
        {
            value.context->UpdateSubresource(value.inputs[i].Get(),0,nullptr,pixels.data(),value.width*4,0);
            value.context->UpdateSubresource(value.silhouettes[i].Get(),0,nullptr,labels.data(),value.width*4,0);
            value.context->UpdateSubresource(value.flow[i].Get(),0,nullptr,vectors.data(),gw*4,0);
            value.silhouetteValid[i]=true;
        }
        value.SetPhase(.5f);
        value.contourSubmission=0;
        value.BuildContours(ticket);
        auto desc=D3D11_TEXTURE2D_DESC{};
        value.contours[0]->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;
        Check(value.device->CreateTexture2D(&desc,nullptr,&read));
        for(unsigned frame=0;frame<2;++frame)
        {
            value.context->CopyResource(read.Get(),value.contours[frame].Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};Check(value.context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
            auto at=[&](unsigned x,unsigned y){return static_cast<unsigned char *>(mapped.pData)[y*mapped.RowPitch+x];};
            for(unsigned section=1;section<8;++section)
                Require(!(at(section*(value.width/8)-1,3)&1),"Different scene IDs share a horizontal link");
            Require(!(at(3,value.height/2-1)&2),"A visible contrast boundary remained connected");
            Require(!(at(3,value.height/4-1)&2),"Different effect layers remained connected despite identical RGB IDs");
            Require((at(3,3)&3)==3,"An object interior was incorrectly disconnected");
            value.context->Unmap(read.Get(),0);
        }
        value.silhouetteValid[0]=value.silhouetteValid[1]=false;
        value.contourSubmission=0;
    }
    static void LinearMotion(OpticalD3D11 &value, OpticalD3D11::Ticket &source)
    {
        Require(value.Wait(),"Temporal fixture overwrote unfinished work");
        for(unsigned direction=0;direction<2;++direction)
        {
            std::vector<unsigned char> image(value.width*value.height*4,255);
            for(unsigned y=0;y<value.height;++y)
                for(unsigned x=0;x<value.width;++x)
                    for(unsigned c=0;c<3;++c)
                        image[(y*value.width+x)*4+c]=80+static_cast<unsigned>(std::max(0,int(x)-(direction?4:0))%128)*3/4;
            value.context->UpdateSubresource(value.inputs[direction?source.after:source.before].Get(),0,nullptr,
                image.data(),value.width*4,0);
            unsigned gw=(value.width+3)/4,gh=(value.height+3)/4;
            std::vector<short> field(gw*gh*2,0);
            for(unsigned i=0;i<gw*gh;++i)field[i*2]=direction?-128:128;
            value.context->UpdateSubresource(value.flow[direction].Get(),0,nullptr,field.data(),gw*4,0);
        }
        value.silhouetteValid[0]=value.silhouetteValid[1]=false;
        source.flowEstimated=true;
    }
    static bool Resolve(OpticalD3D11 &value, const std::shared_ptr<OpticalD3D11::Ticket> &ticket)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!value.ResolveGpu(ticket))
        {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            SwitchToThread();
        }
        return true;
    }
    static void CompareComposed(OpticalD3D11 &value, const std::shared_ptr<OpticalD3D11::Ticket> &ticket, bool hidden)
    {
        ComPtr<ID3D11ShaderResourceView> output;
        Check(value.device->CreateShaderResourceView(value.Output(), nullptr, &output));
        value.CheckComposed(ticket, output.Get(), hidden ? output.Get() : value.imageViews[ticket->after].Get());
        value.Seal(ticket);
    }
    static std::vector<unsigned> Mask(OpticalD3D11 &value)
    {
        D3D11_TEXTURE2D_DESC desc{};
        value.mask->GetDesc(&desc);
        desc.BindFlags = desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(value.device->CreateTexture2D(&desc, nullptr, &staging));
        value.context->CopyResource(staging.Get(), value.mask.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(value.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<unsigned> result(value.width * value.height);
        for (unsigned y = 0; y < value.height; ++y)
            memcpy(result.data() + y * value.width, static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch,
                   value.width * sizeof(unsigned));
        value.context->Unmap(staging.Get(), 0);
        return result;
    }
};
int main(int argc, char **argv)
{
    try
    {
        Require(argc == 2 || argc == 3, "Retained pre-gate OpticalD3D11.cpp and optional preset required");
        auto preset = argc == 3 ? static_cast<NV_OF_PERF_LEVEL>(std::stoi(argv[2])) : NV_OF_PERF_LEVEL_SLOW;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        auto code = Compile(ConfidenceGate::Shader, "cs_5_0");
        ComPtr<ID3D11ComputeShader> cs;
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
        D3D_SHADER_MACRO preflightDefines[] = {{"KOE_PREFLIGHT", "1"}, {nullptr, nullptr}};
        auto preflightCode = Compile(ConfidenceGate::Shader, "cs_5_0", preflightDefines);
        ComPtr<ID3D11ComputeShader> preflight;
        Check(device->CreateComputeShader(preflightCode->GetBufferPointer(), preflightCode->GetBufferSize(), nullptr, &preflight));
        constexpr unsigned w = 65, h = 49, gw = (w + 3) / 4, gh = (h + 3) / 4;
        ComPtr<ID3D11Texture2D> inputs[4];
        ComPtr<ID3D11ShaderResourceView> views[4];
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (unsigned i = 0; i < 4; ++i)
        {
            if (i == 2)
            {
                td.Width = gw;
                td.Height = gh;
                td.Format = DXGI_FORMAT_R16G16_SINT;
            }
            Check(device->CreateTexture2D(&td, nullptr, &inputs[i]));
            Check(device->CreateShaderResourceView(inputs[i].Get(), nullptr, &views[i]));
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        ComPtr<ID3D11Buffer> counters, read, constants;
        Check(device->CreateBuffer(&bd, nullptr, &counters));
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = 4;
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateUnorderedAccessView(counters.Get(), &uv, &uav));
        bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateBuffer(&bd, nullptr, &read));
        float dims[] = {float(w), float(h), float(gw), float(gh)};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.CPUAccessFlags = 0;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{dims, 0, 0};
        Check(device->CreateBuffer(&bd, &data, &constants));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        Check(device->CreateSamplerState(&sd, &sampler));
        std::vector<unsigned char> images[2];
        for (auto &image : images)
            image.resize(w * h * 4);
        std::vector<short> field(gw * gh * 2);
        auto fill = [&](unsigned before, unsigned after) {
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x)
                    for (unsigned c = 0; c < 4; ++c)
                    {
                        images[0][(y * w + x) * 4 + c] = c == 3 ? 255 : static_cast<unsigned char>(before);
                        images[1][(y * w + x) * 4 + c] = c == 3 ? 255 : static_cast<unsigned char>(after);
                    }
        };
        auto evaluate = [&](int f, int b, bool sourceOnly = false) {
            for (unsigned i = 0; i < 2; ++i)
                context->UpdateSubresource(inputs[i].Get(), 0, nullptr, images[i].data(), w * 4, 0);
            for (unsigned i = 2; i < 4; ++i)
            {
                for (unsigned v = 0; v < field.size(); ++v)
                    field[v] = v % 2 ? 0 : short((i == 2 ? f : b) * 32);
                context->UpdateSubresource(inputs[i].Get(), 0, nullptr, field.data(), gw * 4, 0);
            }
            UINT zero[4]{};
            context->ClearUnorderedAccessViewUint(uav.Get(), zero);
            ID3D11ShaderResourceView *resources[] = {views[0].Get(), views[1].Get(), views[2].Get(), views[3].Get()};
            auto *target = uav.Get();
            auto *cb = constants.Get();
            auto *state = sampler.Get();
            context->CSSetShader(sourceOnly ? preflight.Get() : cs.Get(), nullptr, 0);
            context->CSSetShaderResources(0, 4, resources);
            context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
            context->CSSetConstantBuffers(0, 1, &cb);
            context->CSSetSamplers(0, 1, &state);
            context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            target = nullptr;
            ID3D11ShaderResourceView *empty[4]{};
            context->CSSetShaderResources(0, 4, empty);
            context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
            context->CSSetShader(nullptr, nullptr, 0);
            context->CopyResource(read.Get(), counters.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(context->Map(read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            std::array<std::uint32_t, 4> result{};
            memcpy(result.data(), mapped.pData, 16);
            context->Unmap(read.Get(), 0);
            Require(result[3] == w * h, "Missing pixels/partial workgroups");
            return result;
        };
        fill(80, 80);
        auto result = evaluate(0, 0);
        Require(!ConfidenceGate::Accept(result.data(), w * h) && result[0] == 0, "Static frames must not be generated");
        fill(80, 82);
        result = evaluate(0, 0);
        Require(ConfidenceGate::Accept(result.data(), w * h), "Vacuous always-off confidence gate");
        fill(80, 120);
        result = evaluate(0, 0);
        Require(!ConfidenceGate::Accept(result.data(), w * h) && result[1] > 0, "Photometric mismatch not rejected");
        fill(80, 82);
        result = evaluate(2, 2);
        Require(!ConfidenceGate::Accept(result.data(), w * h) && result[1] > 0,
                "Inconsistent bidirectional motion accepted");
        result = evaluate(200, -200);
        Require(!ConfidenceGate::Accept(result.data(), w * h), "Off-image motion accepted");
        for (unsigned stripe : {1u, 2u, 3u, 4u, 6u, 16u})
        {
            fill(80, 80);
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 32 - stripe / 2; x < 32 - stripe / 2 + stripe; ++x)
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        images[0][(y * w + x) * 4 + c] = 89;
                        images[1][(y * w + x) * 4 + c] = 92;
                    }
            result = evaluate(32, -32);
            if (ConfidenceGate::Accept(result.data(), w * h) || result[2] == 0)
                throw std::runtime_error("Runway veto missing: width=" + std::to_string(stripe) + " candidates=" +
                                         std::to_string(result[0]) + " motion=" + std::to_string(result[1]) +
                                         " contour=" + std::to_string(result[2]));
            ++checks;
        }
        // AI-generated comment: A hole inside a flat region has no local contour. Wrong but mutually
        // consistent flow can sample matching background in both frames. The strict
        // real-source envelope must veto this too, not just the visible boundary.
        fill(80, 80);
        for (unsigned y = 12; y <= 36; ++y)
            for (unsigned x = 20; x <= 44; ++x)
                for (unsigned c = 0; c < 3; ++c)
                    images[0][(y * w + x) * 4 + c] = images[1][(y * w + x) * 4 + c] = 92;
        for (unsigned c = 0; c < 3; ++c)
            images[0][(24 * w + 32) * 4 + c] = 89;
        result = evaluate(32, -32);
        Require(result[0] == 1 && result[1] == 1 && result[2] == 0 && !ConfidenceGate::Accept(result.data(), w * h),
                "Flat-interior hole bypassed the source envelope");
        unsigned preflightVetoes = 0, preflightPasses = 0;
        std::uint32_t random = 0x13579bdu;
        for (unsigned trial = 0; trial < 128; ++trial)
        {
            bool uniform = trial % 4 == 0;
            bool changing = uniform ? (trial / 4) % 2 != 0 : trial % 2 != 0;
            fill(80, changing ? 82 : 80);
            if (trial % 4)
                for (unsigned y = 0; y < h; ++y)
                    for (unsigned x = 0; x < w; ++x)
                    {
                        random = random * 1664525u + 1013904223u;
                        if (random % 7 == 0 || (x == w / 2 && trial % 3 == 0))
                            for (unsigned c = 0; c < 3; ++c)
                            {
                                images[0][(y * w + x) * 4 + c] = 88;
                                images[1][(y * w + x) * 4 + c] = 96;
                            }
                    }
            int movement = trial % 4 == 0 ? 0 : int(trial % 33) - 16;
            auto early = evaluate(movement, -movement, true);
            auto full = evaluate(movement, -movement);
            Require(early[0] == full[0] && early[3] == full[3], "Preflight changed source-pixel accounting");
            bool veto = early[2] || !early[0];
            Require(!veto || !ConfidenceGate::Accept(full.data(), w * h), "Preflight rejected a frame accepted by the full gate");
            preflightVetoes += veto;
            preflightPasses += !veto;
        }
        Require(preflightVetoes && preflightPasses, "Preflight proof did not exercise both outcomes");
        fill(80, 80);
        for (unsigned y = 12; y <= 36; ++y)
            for (unsigned x = 20; x <= 44; ++x)
                for (unsigned c = 0; c < 3; ++c)
                    images[0][(y * w + x) * 4 + c] = images[1][(y * w + x) * 4 + c] = 92;
        for (unsigned c = 0; c < 3; ++c)
            images[0][(24 * w + 32) * 4 + c] = 89;
        evaluate(32, -32);
        // Non-vacuous regression: the retained warp erases the grey stripe while
        // both flow directions agree and the photometric comparison sees background.
        std::ifstream old(argv[1]);
        std::stringstream stream;
        stream << old.rdbuf();
        auto text = stream.str();
        auto begin = text.find("R\"HLSL("), end = text.find(")HLSL\"", begin);
        Require(begin != std::string::npos && end != std::string::npos, "Missing retained shader");
        auto shader = text.substr(begin + 7, end - begin - 7);
        D3D_SHADER_MACRO macros[] = {{"KOE_CONSERVATIVE_FLOW", "1"}, {nullptr, nullptr}};
        auto pixel = Compile(shader.c_str(), "ps_5_0", macros);
        ComPtr<ID3D11PixelShader> ps;
        Check(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps));
        auto vertex =
            Compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        ComPtr<ID3D11VertexShader> vs;
        Check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
        td.Width = w;
        td.Height = h;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> output, staging;
        Check(device->CreateTexture2D(&td, nullptr, &output));
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateRenderTargetView(output.Get(), nullptr, &rtv));
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&td, nullptr, &staging));
        auto *rt = rtv.Get();
        context->OMSetRenderTargets(1, &rt, nullptr);
        D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        auto *cb = constants.Get();
        auto *state = sampler.Get();
        ID3D11ShaderResourceView *resources[] = {views[0].Get(), views[1].Get(), views[2].Get(), views[3].Get()};
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetSamplers(0, 1, &state);
        context->PSSetShaderResources(0, 4, resources);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView *empty[4]{};
        context->PSSetShaderResources(0, 4, empty);
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        unsigned oldCentre = static_cast<unsigned char *>(mapped.pData)[24 * mapped.RowPitch + 32 * 4];
        context->Unmap(staging.Get(), 0);
        Require(oldCentre == 80, "Negative control must reproduce lost grey runway");
        // AI-generated comment: Actual NVOF + split production Submit/Generate: rejected work never writes
        // the output texture. Poison detects even an interpolation later discarded.
        constexpr unsigned n = 256;
        OpticalD3D11 backend(device.Get(), n, n, L"", true, preset);
        td.Width = td.Height = n;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> input;
        Check(device->CreateTexture2D(&td, nullptr, &input));
        std::vector<unsigned char> source(n * n * 4, 80);
        for (unsigned i = 3; i < source.size(); i += 4)
            source[i] = 255;
        context->UpdateSubresource(input.Get(), 0, nullptr, source.data(), n * 4, 0);
        auto first = backend.Submit(input.Get(), 1);
        Require(backend.WaitForInput(first, 5000), "GPU input readiness was not signaled");
        Require(!backend.WaitForInput(nullptr, 0), "Missing GPU input was marked ready");
        Require(!backend.Generate(first, 5000) && backend.Wait(), "Initial history generated a frame");
        bool unrestrictedRejected = false;
        try
        {
            OpticalD3D11 unsafe(device.Get(), n, n, L"", false);
        }
        catch (const std::invalid_argument &)
        {
            unrestrictedRejected = true;
        }
        Require(unrestrictedRejected, "Strict policy can be bypassed by unrestricted backend");
        ComPtr<ID3D11RenderTargetView> outputView;
        Check(device->CreateRenderTargetView(backend.Output(), nullptr, &outputView));
        const float poison[] = {1, 0, 1, 1};
        context->ClearRenderTargetView(outputView.Get(), poison);
        for (unsigned i = 0; i < source.size(); ++i)
            if (i % 4 != 3)
                source[i] = 120;
        context->UpdateSubresource(input.Get(), 0, nullptr, source.data(), n * 4, 0);
        auto denied = backend.Submit(input.Get(), 1.02);
        Require(denied->analysisPending, "Fixture accidentally reset frame history");
        Require(!backend.Generate(denied, 5000) && !denied->generationQueued && !denied->eligible,
                "Rejected frame queued generation");
        Require(denied->flowEstimated && !denied->earlyVeto, "Preflight bypassed required motion analysis");
        Require(backend.Wait(), "Analysis did not complete");
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readOutput;
        Check(device->CreateTexture2D(&td, nullptr, &readOutput));
        context->CopyResource(readOutput.Get(), backend.Output());
        Check(context->Map(readOutput.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        bool unchanged = true;
        for (unsigned y = 0; y < n; ++y)
            for (unsigned x = 0; x < n; ++x)
            {
                auto *p = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * 4;
                unchanged &= p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255;
            }
        context->Unmap(readOutput.Get(), 0);
        Require(unchanged, "Rejected frame wrote interpolated pixels before decision");
        auto noDemand = backend.Submit(input.Get(), 1.04, false);
        Require(!noDemand->analysisPending && !backend.Generate(noDemand, 0), "Unrequested frame analyzed/generated");
        Require(!backend.Generate(denied, 0) && denied->rejection == OpticalD3D11::Invalid, "Stale decision reused");
        auto late = backend.Submit(input.Get(), 1.06);
        Require(backend.Wait(), "Quality preflight failed");
        late->fenceValue = std::numeric_limits<std::uint64_t>::max();
        Require(!backend.Generate(late, 0) && late->rejection == OpticalD3D11::Late && !late->generationQueued,
                "Unknown/incomplete decision approved");
        auto approved = backend.Submit(input.Get(), 1.08, false);
        ConfidenceGateFixture::Controlled(backend, *approved);
        if (!backend.Generate(approved, 5000) || !approved->generationQueued)
            throw std::runtime_error(
                "Production positive fixture rejected: reason=" + std::to_string(approved->rejection) + " completed=" +
                std::to_string(backend.Completed(approved)) + " changing=" + std::to_string(approved->counts[0]) +
                " motion=" + std::to_string(approved->counts[1]) + " contour=" + std::to_string(approved->counts[2]));
        ++checks;
        Require(backend.Wait(), "Approved interpolation did not finish");
        context->CopyResource(readOutput.Get(), backend.Output());
        Check(context->Map(readOutput.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        bool bounded = true;
        unsigned synthesized = 0;
        for (unsigned y = 0; y < n; ++y)
            for (unsigned x = 0; x < n; ++x)
                for (unsigned c = 0; c < 3; ++c)
                {
                    unsigned v = static_cast<unsigned char *>(mapped.pData)[y * mapped.RowPitch + x * 4 + c], real = 82;
                    bounded &= std::abs(int(v) - int(real)) <= 3;
                    synthesized += v != real;
                }
        context->Unmap(readOutput.Get(), 0);
        Require(bounded && synthesized > 0, "Approved result is unbounded or merely duplicates the real frame");
        // AI-generated comment: A retained decision must survive later submissions and pool reuse.
        auto retained = backend.Submit(input.Get(), 1.10);
        Require(backend.Wait(), "Retained decision did not complete");
        std::uint32_t retainedCounts[4]{};
        Check(context->Map(retained->decision.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        memcpy(retainedCounts, mapped.pData, sizeof(retainedCounts));
        context->Unmap(retained->decision.Get(), 0);
        ID3D11Buffer *reused = nullptr;
        bool reusedBuffer = false;
        for (unsigned i = 0; i < 32; ++i)
        {
            auto next = backend.Submit(input.Get(), 1.12 + i * .02);
            Require(next->decision.Get() != retained->decision.Get(), "Live decision buffer was overwritten");
            reusedBuffer |= reused && next->decision.Get() == reused;
            reused = next->decision.Get();
            backend.Wait(1);
            Require(backend.Wait(), "Completion event failed after a bounded wait");
            backend.Generate(next, 5000);
            Require(next->earlyVeto && !next->flowEstimated && next->rejection == OpticalD3D11::Static,
                    "Static input launched optical flow");
        }
        Require(reusedBuffer, "Completed decision buffers were never reused");
        Check(context->Map(retained->decision.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Require(memcmp(retainedCounts, mapped.pData, sizeof(retainedCounts)) == 0, "Retained decision changed during reuse");
        context->Unmap(retained->decision.Get(), 0);
        context->ClearRenderTargetView(outputView.Get(), poison);
        for (unsigned y = 0; y < n; ++y)
            for (unsigned c = 0; c < 3; ++c)
                source[(y * n + n / 2) * 4 + c] = 125;
        context->UpdateSubresource(input.Get(), 0, nullptr, source.data(), n * 4, 0);
        auto thinVeto = backend.Submit(input.Get(), 1.76);
        Require(!backend.Generate(thinVeto, 5000) && thinVeto->earlyVeto && !thinVeto->flowEstimated &&
                    thinVeto->rejection == OpticalD3D11::Contour, "Thin contour launched optical flow or synthesis");
        context->CopyResource(readOutput.Get(), backend.Output());
        Check(context->Map(readOutput.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto *centre = static_cast<unsigned char *>(mapped.pData) + (n / 2) * mapped.RowPitch + (n / 2) * 4;
        Require(centre[0] == 255 && centre[1] == 0 && centre[2] == 255 && centre[3] == 255,
                "Early contour veto wrote the interpolation output");
        context->Unmap(readOutput.Get(), 0);
        auto mixed = backend.Submit(input.Get(), 1.78, false, true);
        auto mixedReal = ConfidenceGateFixture::Controlled(backend, *mixed, true);
        ConfidenceGateFixture::QueueMasked(backend, *mixed);
        bool mixedResolved = backend.Wait() && ConfidenceGateFixture::Resolve(backend, mixed);
        if (!mixedResolved || !mixed->eligible)
        {
            std::cerr << "Mixed mask: resolved=" << mixedResolved << " reason=" << mixed->rejection << " counts=";
            for (auto count : mixed->counts) std::cerr << count << ',';
            std::cerr << '\n';
        }
        Require(mixedResolved && mixed->eligible, "Mixed frame was rejected wholesale");
        Require(mixed->counts[1] && mixed->counts[2] && mixed->counts[5], "Mixed fixture did not cover unsafe and synthesized pixels");
        auto mask = ConfidenceGateFixture::Mask(backend);
        context->CopyResource(readOutput.Get(), backend.Output());
        Check(context->Map(readOutput.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        unsigned protectedPixels = 0, changedPixels = 0;
        bool protectedEqual = true, safeBounded = true;
        for (unsigned y = 0; y < n; ++y)
            for (unsigned x = 0; x < n; ++x)
            {
                bool protect = mask[y * n + x] != 1;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        protect |= (mask[std::clamp(int(y) + dy, 0, int(n) - 1) * n +
                                         std::clamp(int(x) + dx, 0, int(n) - 1)] & 6) != 0;
                auto *actual = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * 4;
                auto *real = mixedReal.data() + (y * n + x) * 4;
                protectedPixels += protect;
                changedPixels += memcmp(actual, real, 4) != 0;
                for (unsigned c = 0; c < 4; ++c)
                {
                    if (protect) protectedEqual &= actual[c] == real[c];
                    else safeBounded &= std::abs(int(actual[c]) - int(real[c])) <= 3;
                }
            }
        context->Unmap(readOutput.Get(), 0);
        Require(protectedPixels && protectedEqual, "GPU synthesis changed a protected pixel");
        Require(safeBounded && changedPixels > 0 && changedPixels == mixed->counts[5],
                "GPU synthesis counter is false or output escaped the source envelope");
        for (unsigned delta : {0u, 40u})
        {
            auto rejected = backend.Submit(input.Get(), delta == 0 ? 1.80 : 1.82, false, true);
            auto real = ConfidenceGateFixture::Controlled(backend, *rejected, false, delta);
            ConfidenceGateFixture::QueueMasked(backend, *rejected);
            Require(backend.Wait() && ConfidenceGateFixture::Resolve(backend, rejected) && !rejected->eligible && rejected->counts[5] == 0,
                    "Empty or wholly unsafe GPU mask generated a frame");
            context->CopyResource(readOutput.Get(), backend.Output());
            Check(context->Map(readOutput.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            bool identical = true;
            for (unsigned y = 0; y < n; ++y)
                identical &= memcmp(static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch,
                                    real.data() + y * n * 4, n * 4) == 0;
            context->Unmap(readOutput.Get(), 0);
            Require(identical, "Empty GPU dispatch changed the real fallback image");
        }
        for (bool hidden : {false, true})
        {
            auto composed = backend.Submit(input.Get(), hidden ? 1.86 : 1.84, false, true);
            ConfidenceGateFixture::Controlled(backend, *composed, true);
            ConfidenceGateFixture::QueueMasked(backend, *composed);
            ConfidenceGateFixture::CompareComposed(backend, composed, hidden);
            Require(backend.Wait() && ConfidenceGateFixture::Resolve(backend, composed), "Composed GPU decision did not resolve");
            Require(composed->eligible == !hidden, "UI-hidden synthesis was counted as a visible generated frame");
            Require(hidden ? composed->counts[6] == 0 : composed->counts[6] == composed->counts[5],
                    "Composed pixel counter does not match the actual displayed image");
        }
        auto silhouette = backend.Submit(input.Get(), 1.88, false, true);
        ConfidenceGateFixture::Controlled(backend, *silhouette);
        ConfidenceGateFixture::Silhouette(backend);
        ConfidenceGateFixture::QueueMasked(backend, *silhouette);
        Require(backend.Wait() && ConfidenceGateFixture::Resolve(backend, silhouette) && silhouette->eligible,
                "Silhouette veto rejected the whole frame");
        context->CopyResource(readOutput.Get(), backend.Output());
        Check(context->Map(readOutput.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        bool disagreementPreserved = true;
        unsigned agreementChanged = 0;
        for (unsigned y=0;y<n;++y)
            for(unsigned x=0;x<n;++x)
            {
                auto pixel = static_cast<unsigned char *>(mapped.pData)+y*mapped.RowPitch+x*4;
                bool changed = pixel[0]!=82 || pixel[1]!=82 || pixel[2]!=82;
                if(y<n/2)disagreementPreserved &= !changed;
                else agreementChanged += changed;
            }
        context->Unmap(readOutput.Get(), 0);
        Require(disagreementPreserved, "Hull and background were blended across incompatible silhouettes");
        Require(agreementChanged > 0, "Compatible hull/background interiors stopped interpolating");
        auto missingMask = backend.Submit(input.Get(), 1.90, false, true);
        ConfidenceGateFixture::Controlled(backend, *missingMask);
        ConfidenceGateFixture::QueueMasked(backend, *missingMask);
        Require(backend.Wait() && ConfidenceGateFixture::Resolve(backend, missingMask) && missingMask->eligible,
                "A missing silhouette reused stale masks or broke ordinary interpolation");
        context->CopyResource(readOutput.Get(), backend.Output());
        Check(context->Map(readOutput.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto unmaskedPixel=static_cast<unsigned char *>(mapped.pData)+8*mapped.RowPitch+8*4;
        bool unmaskedChanged=unmaskedPixel[0]!=82;
        context->Unmap(readOutput.Get(),0);
        Require(unmaskedChanged,"Missing silhouette left the old class disagreement active");
        ConfidenceGateFixture::Silhouette(backend);
        ConfidenceGateFixture::SilhouetteSamples(backend,*missingMask);
        ConfidenceGateFixture::SceneBoundaries(backend,*missingMask);
        auto temporalSource=backend.Submit(input.Get(),1.92,false,true);
        ConfidenceGateFixture::LinearMotion(backend,*temporalSource);
        Require(backend.PrepareFlow(temporalSource),"Prepared source lost its optical flow");
        std::array<std::shared_ptr<OpticalD3D11::Ticket>,3> temporal;
        std::array<ComPtr<ID3D11Texture2D>,3> temporalImages;
        std::array<ComPtr<ID3D11ShaderResourceView>,3> temporalViews;
        ComPtr<ID3D11ShaderResourceView> currentView;
        Check(device->CreateShaderResourceView(input.Get(),nullptr,&currentView));
        D3D11_TEXTURE2D_DESC temporalDesc{};
        backend.Output()->GetDesc(&temporalDesc);
        temporalDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        for(unsigned i=0;i<3;++i)
        {
            Check(device->CreateTexture2D(&temporalDesc,nullptr,&temporalImages[i]));
            Check(device->CreateShaderResourceView(temporalImages[i].Get(),nullptr,&temporalViews[i]));
            temporal[i]=backend.Phase(temporalSource,float(i+1)/4);
            context->CopyResource(temporalImages[i].Get(),backend.Output());
            backend.CheckComposed(temporal[i],temporalViews[i].Get(),ConfidenceGateFixture::CurrentImage(backend,*temporalSource),
                i?temporalViews[i-1].Get():backend.ReferenceImage(temporalSource));
            backend.Seal(temporal[i]);
        }
        Require(backend.Wait(),"Multiple temporal phases did not finish");
        std::uint64_t previousHash=0;
        for(unsigned i=0;i<3;++i)
        {
            bool ready=ConfidenceGateFixture::Resolve(backend,temporal[i]);
            if(!ready || !temporal[i]->eligible)
            {
                std::ostringstream detail;
                detail<<"Temporal phase "<<i<<" rejected: ready="<<ready<<" reason="<<temporal[i]->rejection<<" counts=";
                for(auto count:temporal[i]->counts)detail<<count<<',';
                throw std::runtime_error(detail.str());
            }
            ++checks;
            auto &counts=temporal[i]->counts;
            auto hash=(std::uint64_t(counts[8])<<32)|counts[9];
            Require(i==0 || hash!=previousHash,"Multiple phases repeated the same composed image");
            previousHash=hash;
            context->CopyResource(readOutput.Get(),temporalImages[i].Get());
            Check(context->Map(readOutput.Get(),0,D3D11_MAP_READ,0,&mapped));
            auto pixel=static_cast<unsigned char *>(mapped.pData)+16*mapped.RowPitch+19*4;
            unsigned expected=i==0?93:92;
            bool correct=pixel[0]==expected && pixel[1]==expected && pixel[2]==expected;
            context->Unmap(readOutput.Get(),0);
            Require(correct,"Temporal phase did not use its requested interpolation position");
        }
        bool fourthBlocked=false;
        try {backend.Phase(temporalSource,.9f);}
        catch(const std::length_error &) {fourthBlocked=true;}
        Require(fourthBlocked,"The generator accepted a fourth additional frame");
        try {backend.Submit(input.Get(),1.92,false,true);}
        catch(const std::runtime_error &) {}
        bool invalidResetBlocked=false;
        try {backend.Phase(temporalSource,.9f);}
        catch(const std::length_error &) {invalidResetBlocked=true;}
        Require(invalidResetBlocked,"An invalid submission reset the hard generation budget");
        auto duplicateSource=backend.Submit(input.Get(),1.94,false,true);
        ConfidenceGateFixture::LinearMotion(backend,*duplicateSource);
        Require(backend.PrepareFlow(duplicateSource),"New source did not reset the three-frame budget");
        auto duplicate=backend.Phase(duplicateSource,.5f);
        ComPtr<ID3D11ShaderResourceView> duplicateView;
        Check(device->CreateShaderResourceView(backend.Output(),nullptr,&duplicateView));
        backend.CheckComposed(duplicate,duplicateView.Get(),ConfidenceGateFixture::CurrentImage(backend,*duplicateSource),temporalViews[1].Get());
        backend.Seal(duplicate);
        Require(backend.Wait() && ConfidenceGateFixture::Resolve(backend,duplicate) && !duplicate->eligible
                && duplicate->counts[7]==0,"An identical temporal phase was accepted as a new image");
        Require(duplicate->counts[8]==temporal[1]->counts[8] && duplicate->counts[9]==temporal[1]->counts[9],
                "Identical images produced inconsistent GPU fingerprints");
        std::uint32_t corrupt[] = {w * h + 1, 0, 0, w * h};
        Require(!ConfidenceGate::Accept(corrupt, w * h), "Corrupt candidate count accepted");
        std::cout << "Frame confidence: " << checks << " GPU checks passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
