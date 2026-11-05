#include "OpticalD3D11.h"
#include "ConfidenceGate.h"
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
                           bool conservative)
    : width(w), height(h)
{
    try
    {
        if (!conservative)
            throw std::invalid_argument("Unrestricted frame synthesis is disabled by the strict confidence policy");
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
        params.perfLevel = conservative ? NV_OF_PERF_LEVEL_SLOW : NV_OF_PERF_LEVEL_MEDIUM;
        params.predDirection = NV_OF_PRED_DIRECTION_BOTH;
        params.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
        OF(api.nvOFInit(session, &params), "flow init");
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
        // AI-generated comment: NvOFD3DCommon.cpp maps ABGR8 to BGRA8, not RGBA8.
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 2; i++)
        {
            HR(device->CreateTexture2D(&td, nullptr, &inputs[i]), "flow BGRA image");
            OF(api.nvOFRegisterResourceD3D11(session, inputs[i].Get(), &inputHandles[i]), "register image");
            HR(device->CreateShaderResourceView(inputs[i].Get(), nullptr, &imageViews[i]), "image SRV");
            HR(device->CreateRenderTargetView(inputs[i].Get(), nullptr, &inputTargets[i]), "image RTV");
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
cbuffer Dimensions : register(b0)
{
    float2 size;
    float2 gridSize;
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
    return lerp(lerp(p00, p10, f.x), lerp(p01, p11, f.x), f.y) / 32.0;
}
float4 main(float4 position : SV_Position) : SV_Target
{
#if KOE_CONSERVATIVE_FLOW
    int2 pixel = int2(position.xy), lastPixel = int2(size) - 1;
    float4 currentCenter = afterImage.Load(int3(pixel, 0));
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
    [unroll] for (int i = 0; i < 3; i++)
    {
        p = center - 0.5 * flowAt(p, false);
        q = center - 0.5 * flowAt(q, true);
    }
    float2 aFlow = flowAt(p, false), bFlow = flowAt(q, true);
    float aError = length(aFlow + flowAt(p + aFlow, true)), bError = length(bFlow + flowAt(q + bFlow, false));
    float wa = 1.0 / (0.1 + aError * aError), wb = 1.0 / (0.1 + bError * bError);
    float4 a = beforeImage.SampleLevel(linearClamp, (p + 0.5) / size, 0),
           b = afterImage.SampleLevel(linearClamp, (q + 0.5) / size, 0);
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
    return (a * wa + b * wb) / (wa + wb);
}
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
                          static_cast<float>((h + 3) / 4)};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(values);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{values, 0, 0};
        HR(device->CreateBuffer(&bd, &data, &constants), "warp constants");
        blob.Reset();
        errors.Reset();
        HR(D3DCompile(ConfidenceGate::Shader, strlen(ConfidenceGate::Shader), nullptr, nullptr, nullptr, "main",
                      "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors),
           "confidence compile");
        HR(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &confidence),
           "confidence shader");
        D3D11_BUFFER_DESC decision{};
        decision.ByteWidth = 16;
        decision.Usage = D3D11_USAGE_DEFAULT;
        decision.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        decision.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        decision.StructureByteStride = 4;
        HR(device->CreateBuffer(&decision, nullptr, &decisionBuffer), "confidence counters");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 4;
        HR(device->CreateUnorderedAccessView(decisionBuffer.Get(), &uav, &decisionView), "confidence UAV");
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
    if (module)
    {
        FreeLibrary(module);
        module = nullptr;
    }
}
bool OpticalD3D11::Wait(unsigned milliseconds)
{
    if (!fence || !sequence || fence->GetCompletedValue() >= sequence)
        return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return false;
    HRESULT hr = fence->SetEventOnCompletion(sequence, event);
    context->Flush();
    bool done = SUCCEEDED(hr) && WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done;
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
std::shared_ptr<OpticalD3D11::Ticket> OpticalD3D11::Submit(ID3D11Texture2D *current, double timestamp, bool analyze)
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
    auto ticket = std::make_shared<Ticket>();
    ticket->owner = this;
    ticket->serial = ++submission;
    ticket->before = 1 - next;
    ticket->after = next;
    if (!previous)
        CopyImage(imageViews[next].Get(), outputView.Get());
    else if (analyze)
    {
        NV_OF_EXECUTE_INPUT_PARAMS in{};
        NV_OF_EXECUTE_OUTPUT_PARAMS out{};
        in.inputFrame = inputHandles[1 - next];
        in.referenceFrame = inputHandles[next];
        in.disableTemporalHints = NV_OF_TRUE;
        out.outputBuffer = flowHandles[0];
        out.bwdOutputBuffer = flowHandles[1];
        OF(api.nvOFExecute(session, &in, &out), "execute optical flow");
        Analyze(*ticket);
    }
    else
        ticket->rejection = NotRequested;
    HR(context->Signal(fence.Get(), ++sequence), "flow signal");
    ticket->fenceValue = sequence;
    context->Flush();
    previous = true;
    lastTimestamp = timestamp;
    next = 1 - next;
    callMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return ticket;
}
void OpticalD3D11::Analyze(Ticket &ticket)
{
    UINT zeros[4]{};
    context->ClearUnorderedAccessViewUint(decisionView.Get(), zeros);
    ID3D11ShaderResourceView *resources[] = {imageViews[ticket.before].Get(), imageViews[ticket.after].Get(),
                                             flowViews[0].Get(), flowViews[1].Get()};
    auto *cb = constants.Get();
    auto *state = sampler.Get();
    auto *view = decisionView.Get();
    context->CSSetShader(confidence.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 4, resources);
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetSamplers(0, 1, &state);
    context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView *empty[4]{};
    view = nullptr;
    context->CSSetShaderResources(0, 4, empty);
    context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    D3D11_BUFFER_DESC read{};
    read.ByteWidth = 16;
    read.Usage = D3D11_USAGE_STAGING;
    read.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HR(device->CreateBuffer(&read, nullptr, &ticket.decision), "confidence readback");
    context->CopyResource(ticket.decision.Get(), decisionBuffer.Get());
    ticket.analysisPending = true;
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
    if (!Completed(ticket) && (!Wait(budgetMilliseconds) || !Completed(ticket)))
    {
        ticket->analysisPending = false;
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
        ticket->analysisPending = false;
        ticket->rejection = Late;
        return false;
    }
    HR(read, "confidence map");
    memcpy(ticket->counts, data.pData, 16);
    context->Unmap(ticket->decision.Get(), 0);
    ticket->decision.Reset();
    ticket->analysisPending = false;
    if (!ConfidenceGate::Accept(ticket->counts, std::uint64_t(width) * height))
    {
        ticket->rejection = ticket->counts[2]   ? Contour
                            : ticket->counts[1] ? Motion
                            : ticket->counts[0] ? Invalid
                                                : Static;
        return false;
    }
    // AI-generated comment: No interpolated image is queued until the complete analysis has passed.
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
