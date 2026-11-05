#include "OpticalD3D11.h"
#include "ConfidenceGate.h"
#include "TargetFrameRate.h"
#include <d3dcompiler.h>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
static void HR(HRESULT hr, const char *text)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(text) + " hr=" + std::to_string(static_cast<unsigned>(hr)));
}
static void OF(NV_OF_STATUS status, const char *text)
{
    if (status != NV_OF_SUCCESS)
        throw std::runtime_error(std::string(text) + " status=" + std::to_string(status));
}
OpticalD3D11::OpticalD3D11(ID3D11Device *source, unsigned w, unsigned h, const std::filesystem::path &,
                           bool conservative, NV_OF_PERF_LEVEL performance)
    : width(w), height(h)
{
    try
    {
        if (!conservative)
            throw std::invalid_argument("Unrestricted frame synthesis is disabled by the strict confidence policy");
        if (performance != NV_OF_PERF_LEVEL_SLOW && performance != NV_OF_PERF_LEVEL_MEDIUM &&
            performance != NV_OF_PERF_LEVEL_FAST)
            throw std::invalid_argument("Unsupported optical flow preset");
        HR(source->QueryInterface(IID_PPV_ARGS(&device)), "flow device5");
        ComPtr<ID3D11DeviceContext> base;
        source->GetImmediateContext(&base);
        HR(base.As(&context), "flow context4");
        module = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            throw std::runtime_error("Optical Flow driver library missing");
        using Create = NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST *);
        auto create = reinterpret_cast<Create>(GetProcAddress(module, "NvOFAPICreateInstanceD3D11"));
        if (!create)
            throw std::runtime_error("Optical Flow D3D11 export missing");
        OF(create(NV_OF_API_VERSION, &api), "flow API");
        OF(api.nvCreateOpticalFlowD3D11(device.Get(), context.Get(), &session), "flow session");
        NV_OF_INIT_PARAMS params{};
        params.width = w;
        params.height = h;
        params.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        params.mode = NV_OF_MODE_OPTICALFLOW;
        params.perfLevel = performance;
        params.predDirection = NV_OF_PRED_DIRECTION_BOTH;
        params.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
        OF(api.nvOFInit(session, &params), "flow init");
        completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completionEvent)
            throw std::runtime_error("flow completion event");
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        HR(device->CreateTexture2D(&td, nullptr, &output), "flow output");
        HR(device->CreateRenderTargetView(output.Get(), nullptr, &outputView), "output RTV");
        HR(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputTarget), "output UAV");
        // AI-generated comment: NvOFD3DCommon.cpp maps ABGR8 to BGRA8, not RGBA8.
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 2; i++)
        {
            HR(device->CreateTexture2D(&td, nullptr, &inputs[i]), "flow BGRA image");
            OF(api.nvOFRegisterResourceD3D11(session, inputs[i].Get(), &inputHandles[i]), "register image");
            HR(device->CreateShaderResourceView(inputs[i].Get(), nullptr, &imageViews[i]), "image SRV");
            HR(device->CreateRenderTargetView(inputs[i].Get(), nullptr, &inputTargets[i]), "image RTV");
            auto maskDesc = td;
            maskDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            HR(device->CreateTexture2D(&maskDesc, nullptr, &silhouettes[i]), "vessel silhouette history");
            HR(device->CreateShaderResourceView(silhouettes[i].Get(), nullptr, &silhouetteViews[i]), "silhouette SRV");
        }
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        td.Width = (w + 3) / 4;
        td.Height = (h + 3) / 4;
        td.Format = DXGI_FORMAT_R16G16_SINT;
        for (int i = 0; i < 2; i++)
        {
            HR(device->CreateTexture2D(&td, nullptr, &flow[i]), "flow vectors");
            OF(api.nvOFRegisterResourceD3D11(session, flow[i].Get(), &flowHandles[i]), "register vectors");
            HR(device->CreateShaderResourceView(flow[i].Get(), nullptr, &flowViews[i]), "vectors SRV");
        }
        const char *vertex = "float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
        const char *pixel = R"HLSL(
Texture2D<float4> beforeImage : register(t0);
Texture2D<float4> afterImage : register(t1);
Texture2D<int2> forwardFlow : register(t2);
Texture2D<int2> backwardFlow : register(t3);
SamplerState linearClamp : register(s0);
#if KOE_LOCAL_MASK
Texture2D<uint> confidenceMask : register(t4);
Texture2D<float4> beforeSilhouette : register(t5);
Texture2D<float4> afterSilhouette : register(t6);
#endif
cbuffer Dimensions : register(b0)
{
    float2 size;
    float2 gridSize;
#if KOE_TEMPORAL_PHASE
    float phase;
    uint silhouetteEnabled;
    float2 padding;
#endif
}
float largest(float3 v)
{
    return max(v.r, max(v.g, v.b));
}
bool preserveEndpoint(float3 color)
{
    // AI-generated comment: Optical flow cannot establish correspondence in empty space or clipped
    // white emission. Retain the current source there, including a one-pixel
    // border, rather than importing a displaced planet or vessel silhouette.
    float hi = largest(color), lo = min(color.r, min(color.g, color.b));
    return hi < 0.02 || (hi > 0.93 && lo > 0.8);
}
float2 flowAt(float2 pixelPosition, bool backward)
{
    float2 g = clamp((pixelPosition - 1.5) * 0.25, 0, gridSize - 1);
    int2 a = int2(floor(g)), b = min(a + 1, int2(gridSize) - 1);
    float2 f = frac(g);
    float2 p00 = backward ? backwardFlow.Load(int3(a, 0)) : forwardFlow.Load(int3(a, 0));
    float2 p10 = backward ? backwardFlow.Load(int3(b.x, a.y, 0)) : forwardFlow.Load(int3(b.x, a.y, 0));
    float2 p01 = backward ? backwardFlow.Load(int3(a.x, b.y, 0)) : forwardFlow.Load(int3(a.x, b.y, 0));
    float2 p11 = backward ? backwardFlow.Load(int3(b, 0)) : forwardFlow.Load(int3(b, 0));
#if KOE_LOCAL_MASK
    if (silhouetteEnabled)
    {
        int2 last = int2(size) - 1;
        int2 center = clamp(int2(floor(pixelPosition + .5)), 0, last);
        bool hull = (backward ? afterSilhouette.Load(int3(center, 0)).r : beforeSilhouette.Load(int3(center, 0)).r) > .5;
        int2 taps[4] = {a, int2(b.x,a.y), int2(a.x,b.y), b};
        float4 weights = float4((1-f.x)*(1-f.y), f.x*(1-f.y), (1-f.x)*f.y, f.x*f.y);
        [unroll] for (int i=0; i<4; ++i)
        {
            int2 sample = clamp(int2(float2(taps[i])*4+1.5), 0, last);
            bool sampleHull = (backward ? afterSilhouette.Load(int3(sample,0)).r : beforeSilhouette.Load(int3(sample,0)).r) > .5;
            if (sampleHull != hull) weights[i] = 0;
        }
        float total = dot(weights, 1);
        return total > 0 ? (p00*weights.x+p10*weights.y+p01*weights.z+p11*weights.w)/(32*total) : float2(0,0);
    }
#endif
    return lerp(lerp(p00, p10, f.x), lerp(p01, p11, f.x), f.y) / 32.0;
}
#if KOE_LOCAL_MASK
bool hullAt(float2 p, bool backward)
{
    int2 pixel = clamp(int2(floor(p+.5)), 0, int2(size)-1);
    return (backward ? afterSilhouette.Load(int3(pixel,0)).r : beforeSilhouette.Load(int3(pixel,0)).r) > .5;
}
bool sampleHullColor(float2 p, bool backward, bool hull, out float4 color)
{
    int2 base = int2(floor(p));
    float2 f = frac(p);
    float4 weights = float4((1-f.x)*(1-f.y), f.x*(1-f.y), (1-f.x)*f.y, f.x*f.y);
    int2 offsets[4] = {int2(0,0),int2(1,0),int2(0,1),int2(1,1)};
    color = 0;
    float total = 0;
    [unroll] for(int i=0;i<4;++i)
    {
        int2 tap=clamp(base+offsets[i],0,int2(size)-1);
        if(hullAt(tap,backward)!=hull)continue;
        color += weights[i]*(backward ? afterImage.Load(int3(tap,0)) : beforeImage.Load(int3(tap,0)));
        total += weights[i];
    }
    if(total<=0)return false;
    color/=total;
    return true;
}
#endif
float4 warpPixel(float4 position)
{
#if KOE_CONSERVATIVE_FLOW
    int2 pixel = int2(position.xy), lastPixel = int2(size) - 1;
    float4 currentCenter = afterImage.Load(int3(pixel, 0));
#if KOE_LOCAL_MASK
    if (confidenceMask.Load(int3(pixel, 0)) != 1)
        return currentCenter;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            if (confidenceMask.Load(int3(clamp(pixel + int2(x, y), 0, lastPixel), 0)) & 6)
                return currentCenter;
#endif
    bool protectedPixel = preserveEndpoint(currentCenter.rgb);
    protectedPixel =
        protectedPixel || preserveEndpoint(afterImage.Load(int3(clamp(pixel + int2(1, 0), 0, lastPixel), 0)).rgb);
    protectedPixel =
        protectedPixel || preserveEndpoint(afterImage.Load(int3(clamp(pixel + int2(-1, 0), 0, lastPixel), 0)).rgb);
    protectedPixel =
        protectedPixel || preserveEndpoint(afterImage.Load(int3(clamp(pixel + int2(0, 1), 0, lastPixel), 0)).rgb);
    protectedPixel =
        protectedPixel || preserveEndpoint(afterImage.Load(int3(clamp(pixel + int2(0, -1), 0, lastPixel), 0)).rgb);
    if (protectedPixel)
        return currentCenter;
#endif
    float2 center = position.xy - 0.5;
    float2 p = center, q = center;
#if KOE_TEMPORAL_PHASE
    float t = phase;
#else
    float t = .5;
#endif
    [unroll] for (int i = 0; i < 3; i++)
    {
        p = center - t * flowAt(p, false);
        q = center - (1 - t) * flowAt(q, true);
    }
    float2 aFlow = flowAt(p, false), bFlow = flowAt(q, true);
    float aError = length(aFlow + flowAt(p + aFlow, true)), bError = length(bFlow + flowAt(q + bFlow, false));
    float wa = 1.0 / (0.1 + aError * aError), wb = 1.0 / (0.1 + bError * bError);
#if KOE_TEMPORAL_PHASE
    wa *= 1 - t;
    wb *= t;
#endif
    float4 a = beforeImage.SampleLevel(linearClamp, (p + 0.5) / size, 0),
           b = afterImage.SampleLevel(linearClamp, (q + 0.5) / size, 0);
#if KOE_LOCAL_MASK
    if (silhouetteEnabled)
    {
        bool hull = hullAt(p, false);
        if (hull != hullAt(q, true) || !sampleHullColor(p, false, hull, a) || !sampleHullColor(q, true, hull, b))
            return currentCenter;
    }
#endif
#if KOE_CONSERVATIVE_FLOW
    float4 previousCenter = beforeImage.SampleLevel(linearClamp, position.xy / size, 0);
    float3 endpointDelta = abs(previousCenter.rgb - currentCenter.rgb);
    float difference = max(endpointDelta.r, max(endpointDelta.g, endpointDelta.b));
    // AI-generated comment: An unchanged hull, star or background has no new information to synthesize.
    // This also rejects locally plausible but arbitrary flow in featureless space.
    // Half a quantization step of slack keeps every one-byte difference inside
    // the gate despite UNORM-to-float rounding in the texture sampler.
    if (difference < 1.5 / 255.0)
        return currentCenter;
    float3 photoA = abs(a.rgb - afterImage.SampleLevel(linearClamp, (p + aFlow + 0.5) / size, 0).rgb);
    float3 photoB = abs(b.rgb - beforeImage.SampleLevel(linearClamp, (q + bFlow + 0.5) / size, 0).rgb);
    float photo = max(max(photoA.r, max(photoA.g, photoA.b)), max(photoB.r, max(photoB.g, photoB.b)));
    float gradient =
        max(length(aFlow - flowAt(p + float2(4, 0), false)), length(aFlow - flowAt(p + float2(0, 4), false)));
    gradient = max(gradient,
                   max(length(bFlow - flowAt(q + float2(4, 0), true)), length(bFlow - flowAt(q + float2(0, 4), true))));
    bool inconsistent = max(aError, bError) > 0.75 || photo > 0.025 || gradient > 1.5;
    bool unprovenMotion = difference < 0.008 && max(length(aFlow), length(bFlow)) > 3;
    bool outside = any(p < 0) || any(q < 0) || any(p > size - 1) || any(q > size - 1);
    if (inconsistent || unprovenMotion || outside)
        return currentCenter;
#endif
    float4 result = (a * wa + b * wb) / (wa + wb);
#if KOE_LOCAL_MASK
    if (silhouetteEnabled)
    {
        float3 delta = abs(round(saturate(result.rgb)*255)-round(saturate(currentCenter.rgb)*255));
        if (largest(delta)>3) return currentCenter;
        result.a = currentCenter.a;
    }
#endif
    return result;
}
#if KOE_COMPUTE_WARP
RWTexture2D<float4> destination : register(u0);
RWStructuredBuffer<uint> decision : register(u1);
groupshared uint changedPixels;
[numthreads(8, 8, 1)] void computeMain(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    if (lane == 0) changedPixels = 0;
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < uint2(size)))
    {
        float4 current = afterImage.Load(int3(id.xy, 0));
        float4 value = warpPixel(float4(float2(id.xy) + 0.5, 0, 1));
        if (any(round(saturate(value.rgb) * 255) != round(saturate(current.rgb) * 255)))
        {
            destination[id.xy] = value;
            InterlockedAdd(changedPixels, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && changedPixels) InterlockedAdd(decision[5], changedPixels);
}
#else
float4 main(float4 position : SV_Position) : SV_Target { return warpPixel(position); }
#endif
)HLSL";
        ComPtr<ID3DBlob> blob, errors;
        HR(D3DCompile(vertex, strlen(vertex), nullptr, nullptr, nullptr, "main", "vs_5_0",
                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors),
           "warp VS");
        HR(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs), "warp VS create");
        D3D_SHADER_MACRO defines[] = {{"KOE_CONSERVATIVE_FLOW", conservative ? "1" : "0"}, {nullptr, nullptr}};
        blob.Reset();
        errors.Reset();
        HRESULT compiled = D3DCompile(pixel, strlen(pixel), nullptr, defines, nullptr, "main", "ps_5_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(compiled))
            throw std::runtime_error(errors ? static_cast<const char *>(errors->GetBufferPointer())
                                            : "warp shader compile");
        HR(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps), "warp PS create");
        blob.Reset();
        errors.Reset();
        D3D_SHADER_MACRO maskedDefines[] = {{"KOE_CONSERVATIVE_FLOW", "1"}, {"KOE_LOCAL_MASK", "1"},
                                          {"KOE_COMPUTE_WARP", "1"}, {"KOE_TEMPORAL_PHASE", "1"}, {nullptr, nullptr}};
        HR(D3DCompile(pixel, strlen(pixel), nullptr, maskedDefines, nullptr, "computeMain", "cs_5_0",
                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors), "masked warp compile");
        HR(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &maskedWarp),
           "masked warp shader");
        const char *copy =
            "Texture2D<float4> image:register(t0);float4 main(float4 p:SV_Position):SV_Target{return image.Load(int3(int2(p.xy),0));}";
        blob.Reset();
        errors.Reset();
        HR(D3DCompile(copy, strlen(copy), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                      0, &blob, &errors),
           "copy PS");
        HR(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &copyPS),
           "copy PS create");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        HR(device->CreateSamplerState(&sd, &sampler), "warp sampler");
        float values[] = {static_cast<float>(w), static_cast<float>(h), static_cast<float>((w + 3) / 4),
                          static_cast<float>((h + 3) / 4), .5f, 0, 0, 0};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(values);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{values, 0, 0};
        HR(device->CreateBuffer(&bd, &data, &constants), "warp constants");
        blob.Reset();
        errors.Reset();
        D3D_SHADER_MACRO maskDefines[] = {{"KOE_LOCAL_MASK", "1"}, {"KOE_TEMPORAL_PHASE", "1"}, {nullptr, nullptr}};
        HR(D3DCompile(ConfidenceGate::Shader, strlen(ConfidenceGate::Shader), nullptr, maskDefines, nullptr, "main",
                      "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors),
           "confidence compile");
        HR(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &confidence),
           "confidence shader");
        blob.Reset();
        errors.Reset();
        D3D_SHADER_MACRO preflightDefines[] = {{"KOE_PREFLIGHT", "1"}, {nullptr, nullptr}};
        HR(D3DCompile(ConfidenceGate::Shader, strlen(ConfidenceGate::Shader), nullptr, preflightDefines, nullptr,
                      "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors), "preflight compile");
        HR(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &preflight),
           "preflight shader");
        D3D11_BUFFER_DESC decision{};
        decision.ByteWidth = 48;
        decision.Usage = D3D11_USAGE_DEFAULT;
        decision.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        decision.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        decision.StructureByteStride = 4;
        HR(device->CreateBuffer(&decision, nullptr, &decisionBuffer), "confidence counters");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 12;
        HR(device->CreateUnorderedAccessView(decisionBuffer.Get(), &uav, &decisionView), "confidence UAV");
        HR(device->CreateShaderResourceView(decisionBuffer.Get(), nullptr, &decisionSrv), "confidence SRV");
        auto maskDesc = td;
        maskDesc.Width = width;
        maskDesc.Height = height;
        maskDesc.Format = DXGI_FORMAT_R32_UINT;
        maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HR(device->CreateTexture2D(&maskDesc, nullptr, &mask), "confidence mask");
        HR(device->CreateShaderResourceView(mask.Get(), nullptr, &maskView), "mask SRV");
        HR(device->CreateUnorderedAccessView(mask.Get(), nullptr, &maskTarget), "mask UAV");
        D3D11_BUFFER_DESC args{};
        args.ByteWidth = 16;
        args.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        args.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        HR(device->CreateBuffer(&args, nullptr, &dispatchArguments), "indirect dispatch arguments");
        D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        raw.Buffer.NumElements = 4;
        raw.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        HR(device->CreateUnorderedAccessView(dispatchArguments.Get(), &raw, &dispatchTarget), "dispatch UAV");
        const char *gate = R"HLSL(
StructuredBuffer<uint> counters : register(t0);
RWByteAddressBuffer arguments : register(u0);
cbuffer Dimensions : register(b0) { float2 size; float2 gridSize; }
[numthreads(1, 1, 1)] void main()
{
    uint total = uint(size.x) * uint(size.y);
    bool valid = counters[3] == total && counters[0] <= total && counters[4] > 0 &&
                 counters[4] <= counters[0] && counters[1] <= counters[0] && counters[2] <= counters[0];
    arguments.Store4(0, uint4(valid ? (uint(size.x) + 7) / 8 : 0, (uint(size.y) + 7) / 8, 1, 0));
}
)HLSL";
        blob.Reset();
        errors.Reset();
        HR(D3DCompile(gate, strlen(gate), nullptr, nullptr, nullptr, "main", "cs_5_0",
                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors), "dispatch gate compile");
        HR(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &dispatchGate),
           "dispatch gate shader");
        const char *compare = R"HLSL(
Texture2D<float4> composed : register(t0);
Texture2D<float4> realImage : register(t1);
RWStructuredBuffer<uint> counters : register(u0);
cbuffer Dimensions : register(b0) { float2 size; float2 gridSize; }
groupshared uint changed, imageXor, imageSum, realXor, realSum;
cbuffer Comparison : register(b1) { uint counterIndex; uint3 padding; }
uint fingerprint(float4 color, uint position)
{
    uint3 rgb=uint3(round(saturate(color.rgb)*255));
    uint v=(rgb.x|(rgb.y<<8)|(rgb.z<<16)) ^ (position*0x9e3779b9u);
    v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    if (lane == 0) {changed = 0;imageXor=imageSum=realXor=realSum=0;}
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < uint2(size)))
    {
        float4 a=composed.Load(int3(id.xy,0)),b=realImage.Load(int3(id.xy,0));
        if(any(a!=b)) InterlockedAdd(changed, 1);
        if(counterIndex==6)
        {
            uint position=id.y*uint(size.x)+id.x;
            uint ha=fingerprint(a,position),hb=fingerprint(b,position);
            InterlockedXor(imageXor,ha);InterlockedAdd(imageSum,ha);
            InterlockedXor(realXor,hb);InterlockedAdd(realSum,hb);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && changed) InterlockedAdd(counters[counterIndex], changed);
    if(lane==0 && counterIndex==6)
    {
        InterlockedXor(counters[8],imageXor);InterlockedAdd(counters[9],imageSum);
        InterlockedXor(counters[10],realXor);InterlockedAdd(counters[11],realSum);
    }
}
)HLSL";
        blob.Reset();
        errors.Reset();
        HR(D3DCompile(compare, strlen(compare), nullptr, nullptr, nullptr, "main", "cs_5_0",
                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors), "composed comparison compile");
        HR(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &compareComposed),
           "composed comparison shader");
        D3D11_BUFFER_DESC comparison{};
        comparison.ByteWidth = 16;
        comparison.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HR(device->CreateBuffer(&comparison, nullptr, &comparisonParameters), "comparison parameters");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        HR(device->CreateRasterizerState(&rd, &rasterizer), "warp rasterizer");
        HR(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "flow completion fence");
    }
    catch (...)
    {
        Cleanup();
        throw;
    }
}
OpticalD3D11::~OpticalD3D11()
{
    Cleanup();
}
void OpticalD3D11::Cleanup()
{
    if (session)
    {
        Wait();
        for (auto handle : inputHandles)
            if (handle)
                api.nvOFUnregisterResourceD3D11(handle);
        for (auto handle : flowHandles)
            if (handle)
                api.nvOFUnregisterResourceD3D11(handle);
        api.nvOFDestroy(session);
        session = nullptr;
    }
    if (completionEvent)
    {
        CloseHandle(completionEvent);
        completionEvent = nullptr;
    }
    if (module)
    {
        FreeLibrary(module);
        module = nullptr;
    }
}
bool OpticalD3D11::Wait(unsigned milliseconds)
{
    return WaitFor(sequence, milliseconds);
}
bool OpticalD3D11::WaitForInput(const std::shared_ptr<Ticket> &ticket, unsigned milliseconds)
{
    return ticket && ticket->owner == this && ticket->readyFenceValue && WaitFor(ticket->readyFenceValue, milliseconds);
}
bool OpticalD3D11::WaitFor(std::uint64_t value, unsigned milliseconds)
{
    if (!fence || !value || fence->GetCompletedValue() >= value)
        return true;
    if (!milliseconds || !completionEvent)
        return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;)
    {
        auto completed = fence->GetCompletedValue();
        if (completed >= value)
            return true;
        if (completed >= waitingFor)
        {
            ResetEvent(completionEvent);
            if (FAILED(fence->SetEventOnCompletion(value, completionEvent)))
                return false;
            waitingFor = value;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return fence->GetCompletedValue() >= value;
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        if (WaitForSingleObject(completionEvent, static_cast<DWORD>(remaining)) == WAIT_FAILED)
            return false;
    }
}
bool OpticalD3D11::Completed(const std::shared_ptr<Ticket> &ticket) const
{
    return ticket && fence->GetCompletedValue() >= ticket->fenceValue;
}
void OpticalD3D11::CopyImage(ID3D11ShaderResourceView *source, ID3D11RenderTargetView *target)
{
    context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(rasterizer.Get());
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(copyPS.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &source);
    context->Draw(3, 0);
    ID3D11ShaderResourceView *empty = nullptr;
    context->PSSetShaderResources(0, 1, &empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}
bool OpticalD3D11::Process(ID3D11Texture2D *current, double timestamp)
{
    auto ticket = Submit(current, timestamp);
    bool generated = Generate(ticket, 5000);
    if (!Wait())
        throw std::runtime_error("flow completion timeout");
    return generated;
}
std::shared_ptr<OpticalD3D11::Ticket> OpticalD3D11::Submit(ID3D11Texture2D *current, double timestamp, bool analyze,
                                                       bool gpuConditional, ID3D11Texture2D *silhouette)
{
    auto started = std::chrono::steady_clock::now();
    if (!std::isfinite(timestamp) || (previous && timestamp <= lastTimestamp))
        throw std::runtime_error("flow timestamp");
    D3D11_TEXTURE2D_DESC desc{};
    current->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.SampleDesc.Count != 1)
        throw std::runtime_error("flow input descriptor");
    if (externalResource.Get() != current)
    {
        externalView.Reset();
        externalResource = current;
        HR(device->CreateShaderResourceView(current, nullptr, &externalView), "external image SRV");
    }
    // AI-generated comment: Never bridge a pause, focus loss or a long loading hitch with old motion.
    if (previous && timestamp - lastTimestamp > 0.25)
        previous = false;
    CopyImage(externalView.Get(), inputTargets[next].Get());
    silhouetteValid[next] = false;
    if (silhouette)
    {
        D3D11_TEXTURE2D_DESC maskDesc{};
        silhouette->GetDesc(&maskDesc);
        if (maskDesc.Width != width || maskDesc.Height != height || maskDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
            || maskDesc.SampleDesc.Count != 1)
            throw std::invalid_argument("Silhouette dimensions or format differ from world input");
        context->CopyResource(silhouettes[next].Get(), silhouette);
        silhouetteValid[next] = true;
    }
    SetPhase(.5f);
    auto ticket = std::make_shared<Ticket>();
    ticket->owner = this;
    ticket->gpuConditional = gpuConditional;
    ticket->serial = ++submission;
    phasesThisSubmission = 0;
    ticket->before = 1 - next;
    ticket->after = next;
    HR(context->Signal(fence.Get(), ++sequence), "input readiness signal");
    ticket->readyFenceValue = sequence;
    if (!previous)
        CopyImage(imageViews[next].Get(), outputView.Get());
    else if (analyze)
    {
        if (gpuConditional)
        {
            Estimate(*ticket);
            QueueMaskedWarp(*ticket);
        }
        else
            Analyze(*ticket, true);
    }
    else
        ticket->rejection = NotRequested;
    Signal(*ticket);
    previous = true;
    lastTimestamp = timestamp;
    next = 1 - next;
    callMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return ticket;
}
void OpticalD3D11::Signal(Ticket &ticket)
{
    HR(context->Signal(fence.Get(), ++sequence), "flow signal");
    ticket.fenceValue = sequence;
    if (ticket.readback)
        ticket.readback->fenceValue = sequence;
    context->Flush();
}
void OpticalD3D11::Estimate(Ticket &ticket)
{
    SetPhase(.5f);
    EstimateFlow(ticket);
    Analyze(ticket);
}
void OpticalD3D11::SetPhase(float fraction)
{
    if (!std::isfinite(fraction) || fraction <= 0 || fraction >= 1)
        throw std::invalid_argument("Interpolation phase must be between the endpoints");
    float values[] = {float(width), float(height), float((width+3)/4), float((height+3)/4), fraction, 0, 0, 0};
    unsigned maskEnabled = silhouetteValid[0] && silhouetteValid[1] ? 1u : 0u;
    memcpy(values + 5, &maskEnabled, sizeof(maskEnabled));
    context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
}
bool OpticalD3D11::PrepareFlow(const std::shared_ptr<Ticket> &source)
{
    if (!source || source->owner != this || source->serial != submission)
        throw std::invalid_argument("Expired interpolation source");
    if (source->rejection == NoHistory) return false;
    if (!source->flowEstimated) EstimateFlow(*source);
    return true;
}
ID3D11ShaderResourceView *OpticalD3D11::ReferenceImage(const std::shared_ptr<Ticket> &source) const
{
    if (!source || source->owner != this || source->serial != submission)
        throw std::invalid_argument("Expired reference image");
    return imageViews[source->before].Get();
}
std::shared_ptr<OpticalD3D11::Ticket> OpticalD3D11::Phase(const std::shared_ptr<Ticket> &source, float fraction)
{
    if (phasesThisSubmission >= TargetFrameRate::MaxAdditionalFrames)
        throw std::length_error("At most three extra frames per source pair");
    if (!source || source->owner != this || source->serial != submission || !source->flowEstimated)
        throw std::invalid_argument("Optical flow is not prepared for this source");
    SetPhase(fraction);
    auto ticket = std::make_shared<Ticket>();
    ticket->owner = this;
    ticket->serial = source->serial;
    ticket->before = source->before;
    ticket->after = source->after;
    ticket->gpuConditional = true;
    Analyze(*ticket);
    QueueMaskedWarp(*ticket);
    Signal(*ticket);
    return ticket;
}
void OpticalD3D11::EstimateFlow(Ticket &ticket)
{
    ticket.flowEstimated = true;
    NV_OF_EXECUTE_INPUT_PARAMS in{};
    NV_OF_EXECUTE_OUTPUT_PARAMS out{};
    in.inputFrame = inputHandles[ticket.before];
    in.referenceFrame = inputHandles[ticket.after];
    in.disableTemporalHints = NV_OF_TRUE;
    out.outputBuffer = flowHandles[0];
    out.bwdOutputBuffer = flowHandles[1];
    OF(api.nvOFExecute(session, &in, &out), "execute optical flow");
}
void OpticalD3D11::Analyze(Ticket &ticket, bool sourceOnly)
{
    UINT zeros[4]{};
    context->ClearUnorderedAccessViewUint(decisionView.Get(), zeros);
    ID3D11ShaderResourceView *resources[] = {imageViews[ticket.before].Get(), imageViews[ticket.after].Get(),
                                             flowViews[0].Get(), flowViews[1].Get()};
    auto *cb = constants.Get();
    auto *state = sampler.Get();
    ID3D11UnorderedAccessView *targets[] = {decisionView.Get(), sourceOnly ? nullptr : maskTarget.Get()};
    context->CSSetShader(sourceOnly ? preflight.Get() : confidence.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 4, resources);
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetSamplers(0, 1, &state);
    context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView *empty[4]{};
    targets[0] = targets[1] = nullptr;
    context->CSSetShaderResources(0, 4, empty);
    context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    D3D11_BUFFER_DESC read{};
    read.ByteWidth = 48;
    read.Usage = D3D11_USAGE_STAGING;
    read.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    auto completed = fence->GetCompletedValue();
    for (auto &slot : readbacks)
        if (slot.use_count() == 1 && completed >= slot->fenceValue)
        {
            ticket.readback = slot;
            break;
        }
    if (!ticket.readback)
    {
        ticket.readback = std::make_shared<DecisionReadback>();
        HR(device->CreateBuffer(&read, nullptr, &ticket.readback->buffer), "confidence readback");
        // AI-generated comment: Retained tickets must not grow the reusable pool without a bound.
        if (readbacks.size() < readbackLimit)
            readbacks.push_back(ticket.readback);
    }
    ticket.decision = ticket.readback->buffer;
    if (!ticket.gpuConditional)
        context->CopyResource(ticket.decision.Get(), decisionBuffer.Get());
    ticket.analysisPending = true;
    ticket.preflightPending = sourceOnly;
}
void OpticalD3D11::ReleaseDecision(Ticket &ticket)
{
    ticket.analysisPending = false;
    ticket.preflightPending = false;
    ticket.decision.Reset();
    ticket.readback.reset();
}
void OpticalD3D11::QueueMaskedWarp(Ticket &ticket)
{
    if (phasesThisSubmission >= TargetFrameRate::MaxAdditionalFrames)
        throw std::length_error("At most three extra frames per source pair");
    ++phasesThisSubmission;
    CopyImage(imageViews[ticket.after].Get(), outputView.Get());
    auto *cb = constants.Get();
    auto *counts = decisionSrv.Get();
    auto *arguments = dispatchTarget.Get();
    context->CSSetShader(dispatchGate.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetShaderResources(0, 1, &counts);
    context->CSSetUnorderedAccessViews(0, 1, &arguments, nullptr);
    context->Dispatch(1, 1, 1);
    counts = nullptr;
    arguments = nullptr;
    context->CSSetShaderResources(0, 1, &counts);
    context->CSSetUnorderedAccessViews(0, 1, &arguments, nullptr);
    ID3D11ShaderResourceView *images[] = {imageViews[ticket.before].Get(), imageViews[ticket.after].Get(),
                                        flowViews[0].Get(), flowViews[1].Get(), maskView.Get(),
                                        silhouetteViews[ticket.before].Get(), silhouetteViews[ticket.after].Get()};
    ID3D11UnorderedAccessView *targets[] = {outputTarget.Get(), decisionView.Get()};
    auto *state = sampler.Get();
    context->CSSetShader(maskedWarp.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 7, images);
    context->CSSetSamplers(0, 1, &state);
    context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
    context->DispatchIndirect(dispatchArguments.Get(), 0);
    ID3D11ShaderResourceView *empty[7]{};
    targets[0] = targets[1] = nullptr;
    context->CSSetShaderResources(0, 7, empty);
    context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(ticket.decision.Get(), decisionBuffer.Get());
    ticket.generationQueued = true;
}
void OpticalD3D11::Seal(const std::shared_ptr<Ticket> &ticket)
{
    if (!ticket || ticket->owner != this)
        throw std::invalid_argument("Foreign GPU frame ticket");
    Signal(*ticket);
}
void OpticalD3D11::CheckComposed(const std::shared_ptr<Ticket> &ticket, ID3D11ShaderResourceView *composed,
                                ID3D11ShaderResourceView *real, ID3D11ShaderResourceView *previous)
{
    if (!ticket || ticket->owner != this || !ticket->generationQueued)
        throw std::invalid_argument("No synthesized image to compare");
    auto *cb = constants.Get();
    context->CSSetShader(compareComposed.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &cb);
    auto *parameters = comparisonParameters.Get();
    context->CSSetConstantBuffers(1, 1, &parameters);
    auto compare = [&](ID3D11ShaderResourceView *reference, unsigned index) {
        unsigned values[] = {index, 0, 0, 0};
        context->UpdateSubresource(comparisonParameters.Get(), 0, nullptr, values, 0, 0);
        ID3D11ShaderResourceView *images[] = {composed, reference};
        auto *target = decisionView.Get();
        context->CSSetShaderResources(0, 2, images);
        context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        images[0] = images[1] = nullptr;
        target = nullptr;
        context->CSSetShaderResources(0, 2, images);
        context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    };
    compare(real, 6);
    if (previous) compare(previous, 7);
    context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(ticket->decision.Get(), decisionBuffer.Get());
    ticket->composedChecked = true;
    ticket->previousChecked = previous != nullptr;
}
bool OpticalD3D11::ResolveGpu(const std::shared_ptr<Ticket> &ticket)
{
    if (!ticket || ticket->owner != this || !ticket->gpuConditional || !Completed(ticket))
        return false;
    if (!ticket->analysisPending)
        return true;
    D3D11_MAPPED_SUBRESOURCE data{};
    HRESULT read = context->Map(ticket->decision.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data);
    if (read == DXGI_ERROR_WAS_STILL_DRAWING)
        return false;
    HR(read, "GPU decision map");
    memcpy(ticket->counts, data.pData, sizeof(ticket->counts));
    context->Unmap(ticket->decision.Get(), 0);
    ReleaseDecision(*ticket);
    ticket->eligible = ConfidenceGate::AcceptMasked(ticket->counts, std::uint64_t(width) * height);
    if (ticket->composedChecked)
        ticket->eligible = ticket->eligible && ticket->counts[6] > 0 && ticket->counts[6] <= ticket->counts[5];
    if (ticket->previousChecked)
        ticket->eligible = ticket->eligible && ticket->counts[7] > 0;
    ticket->rejection = ticket->eligible ? Accepted
                        : ticket->counts[2] ? Contour
                        : ticket->counts[1] ? Motion
                        : ticket->counts[3] != std::uint64_t(width) * height ? Invalid : Static;
    return true;
}
bool OpticalD3D11::Generate(const std::shared_ptr<Ticket> &ticket, unsigned budgetMilliseconds)
{
    if (!ticket || ticket->owner != this || ticket->serial != submission || ticket->generationQueued)
    {
        if (ticket)
            ticket->rejection = Invalid;
        return false;
    }
    if (!ticket->analysisPending)
        return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMilliseconds);
    for (;;)
    {
        auto now = std::chrono::steady_clock::now();
        unsigned remaining = now >= deadline ? 0 : static_cast<unsigned>(
            std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
        if (!Completed(ticket) && (!Wait(remaining) || !Completed(ticket)))
        {
            ReleaseDecision(*ticket);
            ticket->rejection = Late;
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE data{};
        HRESULT read;
        do
        {
            read = context->Map(ticket->decision.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data);
            if (read != DXGI_ERROR_WAS_STILL_DRAWING || std::chrono::steady_clock::now() >= deadline)
                break;
            SwitchToThread();
        } while (true);
        if (read == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            ReleaseDecision(*ticket);
            ticket->rejection = Late;
            return false;
        }
        HR(read, "confidence map");
        memcpy(ticket->counts, data.pData, 16);
        context->Unmap(ticket->decision.Get(), 0);
        bool sourceOnly = ticket->preflightPending;
        ReleaseDecision(*ticket);
        if (!sourceOnly)
            break;
        if (ticket->counts[3] != std::uint64_t(width) * height || ticket->counts[0] > ticket->counts[3])
        {
            ticket->rejection = Invalid;
            return false;
        }
        if (ticket->counts[2] || !ticket->counts[0])
        {
            ticket->earlyVeto = true;
            ticket->rejection = ticket->counts[2] ? Contour : Static;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ticket->rejection = Late;
            return false;
        }
        Estimate(*ticket);
        Signal(*ticket);
    }
    if (!ConfidenceGate::Accept(ticket->counts, std::uint64_t(width) * height))
    {
        ticket->rejection = ticket->counts[2]   ? Contour
                            : ticket->counts[1] ? Motion
                            : ticket->counts[0] ? Invalid
                                                : Static;
        return false;
    }
    // AI-generated comment: No interpolated image is queued until the complete analysis has passed.
    if (phasesThisSubmission >= TargetFrameRate::MaxAdditionalFrames)
        throw std::length_error("At most three extra frames per source pair");
    ++phasesThisSubmission;
    ID3D11RenderTargetView *target = outputView.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(rasterizer.Get());
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView *resources[] = {imageViews[ticket->before].Get(), imageViews[ticket->after].Get(),
                                             flowViews[0].Get(), flowViews[1].Get()};
    context->PSSetShaderResources(0, 4, resources);
    auto *cb = constants.Get();
    auto *state = sampler.Get();
    context->PSSetConstantBuffers(0, 1, &cb);
    context->PSSetSamplers(0, 1, &state);
    context->Draw(3, 0);
    ID3D11ShaderResourceView *empty[4]{};
    context->PSSetShaderResources(0, 4, empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ticket->rejection = Accepted;
    ticket->eligible = true;
    ticket->generationQueued = true;
    HR(context->Signal(fence.Get(), ++sequence), "generation fence");
    ticket->fenceValue = sequence;
    context->Flush();
    return true;
}
