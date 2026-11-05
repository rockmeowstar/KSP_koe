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
    static void Controlled(OpticalD3D11 &value, OpticalD3D11::Ticket &ticket)
    {
        Require(value.Wait(), "Controlled flow cannot overwrite in-flight resources");
        std::vector<unsigned char> image(value.width * value.height * 4, 80);
        for (unsigned i = 3; i < image.size(); i += 4)
            image[i] = 255;
        value.context->UpdateSubresource(value.inputs[ticket.before].Get(), 0, nullptr, image.data(), value.width * 4,
                                         0);
        for (unsigned i = 0; i < image.size(); ++i)
            if (i % 4 != 3)
                image[i] = 82;
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
    }
};
int main(int argc, char **argv)
{
    try
    {
        Require(argc == 2, "Retained pre-gate OpticalD3D11.cpp required");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        auto code = Compile(ConfidenceGate::Shader, "cs_5_0");
        ComPtr<ID3D11ComputeShader> cs;
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
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
        auto evaluate = [&](int f, int b) {
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
            context->CSSetShader(cs.Get(), nullptr, 0);
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
        OpticalD3D11 backend(device.Get(), n, n, L"", true);
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
