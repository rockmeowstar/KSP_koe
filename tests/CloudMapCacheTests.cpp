#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <array>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <cstring>

template <class T> struct Com
{
    T *p{};
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    Com(Com &&other) noexcept : p(other.p)
    {
        other.p = nullptr;
    }
    ~Com()
    {
        if (p)
            p->Release();
    }
    T *operator->() const
    {
        return p;
    }
    T **Out()
    {
        return &p;
    }
    void Reset()
    {
        if (p)
            p->Release();
        p = nullptr;
    }
};
static void Check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
static void Result(HRESULT hr, const char *message)
{
    Check(SUCCEEDED(hr), message);
}
using Switch = void (*)(int);
using Read = void (*)(long long *, int);
using Clear = void (*)();
using Limits = void (*)(size_t, size_t);
static Com<ID3DBlob> Compile(const char *source, const char *profile)
{
    Com<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.Out(), errors.Out());
    if (FAILED(hr) && errors.p)
        std::printf("%s\n", static_cast<char *>(errors->GetBufferPointer()));
    Result(hr, "test shader compilation");
    return code;
}
int wmain(int argc, wchar_t **argv)
{
    if (argc != 4 && argc != 5)
        return 2;
    bool withFusion = argc == 5 && wcscmp(argv[4], L"--with-fusion") == 0;
    if (argc == 5 && !withFusion) return 2;
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll)
        return 3;
    auto enable = reinterpret_cast<Switch>(GetProcAddress(dll, "SetCloudMapPackingEnabled"));
    auto gate = reinterpret_cast<Switch>(GetProcAddress(dll, "SetCloudMapDrawEnabled"));
    auto read = reinterpret_cast<Read>(GetProcAddress(dll, "ReadCloudMaps"));
    auto clear = reinterpret_cast<Clear>(GetProcAddress(dll, "ClearCloudMapCache"));
    auto shutdown = reinterpret_cast<Clear>(GetProcAddress(dll, "ShutdownCloudMaps"));
    auto limits = reinterpret_cast<Limits>(GetProcAddress(dll, "SetCloudMapCacheLimits"));
    auto initialize =
        reinterpret_cast<int (*)(const wchar_t *, const wchar_t *, int)>(GetProcAddress(dll, "InitializeCloudGuards"));
    if (!enable || !gate || !read || !clear || !shutdown || !limits || !initialize)
        return 4;
    try
    {
        enable(1);
        limits(1024 * 1024, 1024 * 1024);
        Check(initialize(argv[2], argv[3], 0) == 0, "native hooks initialize");
        if (withFusion)
        {
            auto fusion = reinterpret_cast<int (*)()>(GetProcAddress(dll,"InitializeCloudPassFusion"));
            auto stats = reinterpret_cast<int (*)(unsigned long long *,unsigned)>(GetProcAddress(dll,"CloudFusionLiveStats"));
            Check(fusion && stats, "fusion entry points exist");
            Check(fusion() == 0, "fusion initializes after map hooks without duplicate registration");
            unsigned long long values[10]{};
            Check(stats(values,10)==0 && values[7]==1, "both map and fusion hooks remain enabled");
        }
        long long stats[12]{};
        read(stats, 12);
        Check(stats[1] == 1, "map hooks enabled");
        {
            Com<ID3D11Device> device;
            Com<ID3D11DeviceContext> context;
            Result(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     device.Out(), nullptr, context.Out()),
                   "D3D11 device");
            auto vertex = Compile(
                "float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}",
                "vs_5_0");
            auto pixel = Compile(
                "Texture2D<float4> a:register(t0);Texture2D<float4> b:register(t1);SamplerState s:register(s0);SamplerState t:register(s1);float4 main(float4 p:SV_Position):SV_Target {float2 uv=p.xy/16;float4 x=a.SampleLevel(s,uv,0);float4 y=b.SampleLevel(t,uv,0);uint n=asuint(x.y+y.y);return x+y+float4(float(n&0x7c00),float(n&1023),0,0)*0.001;}",
                "ps_5_0");
            Com<ID3D11VertexShader> vs;
            Com<ID3D11PixelShader> ps;
            Result(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, vs.Out()),
                   "vertex shader");
            Result(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, ps.Out()),
                   "pixel shader");
            UINT indices[] = {0, 1, 2};
            D3D11_BUFFER_DESC ib{};
            ib.ByteWidth = sizeof(indices);
            ib.Usage = D3D11_USAGE_IMMUTABLE;
            ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{indices, 0, 0};
            Com<ID3D11Buffer> index;
            Result(device->CreateBuffer(&ib, &initial, index.Out()), "index buffer");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 8;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_BC4_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            Com<ID3D11Texture2D> red, green;
            Result(device->CreateTexture2D(&desc, nullptr, red.Out()), "red texture");
            Result(device->CreateTexture2D(&desc, nullptr, green.Out()), "green texture");
            std::array<unsigned char, 32> a{}, b{};
            for (size_t i = 0; i < a.size(); i++)
            {
                a[i] = static_cast<unsigned char>(i * 53 + 17);
                b[i] = static_cast<unsigned char>(i * 127 + 33);
            }
            context->UpdateSubresource(red.p, 0, nullptr, a.data(), 16, 32);
            context->UpdateSubresource(green.p, 0, nullptr, b.data(), 16, 32);
            Com<ID3D11ShaderResourceView> redView, greenView;
            Result(device->CreateShaderResourceView(red.p, nullptr, redView.Out()), "red view");
            Result(device->CreateShaderResourceView(green.p, nullptr, greenView.Out()), "green view");
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            Com<ID3D11SamplerState> state;
            Result(device->CreateSamplerState(&sampler, state.Out()), "sampler");
            D3D11_TEXTURE2D_DESC color{};
            color.Width = color.Height = 16;
            color.ArraySize = color.MipLevels = color.SampleDesc.Count = 1;
            color.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            color.BindFlags = D3D11_BIND_RENDER_TARGET;
            Com<ID3D11Texture2D> output, staging;
            Result(device->CreateTexture2D(&color, nullptr, output.Out()), "output");
            color.BindFlags = 0;
            color.Usage = D3D11_USAGE_STAGING;
            color.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Result(device->CreateTexture2D(&color, nullptr, staging.Out()), "readback");
            Com<ID3D11RenderTargetView> target;
            Result(device->CreateRenderTargetView(output.p, nullptr, target.Out()), "target");
            D3D11_VIEWPORT viewport{0, 0, 16, 16, 0, 1};
            auto draw = [&]() {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->IASetIndexBuffer(index.p, DXGI_FORMAT_R32_UINT, 0);
                context->VSSetShader(vs.p, nullptr, 0);
                context->PSSetShader(ps.p, nullptr, 0);
                ID3D11ShaderResourceView *views[] = {redView.p, greenView.p};
                ID3D11SamplerState *samplers[] = {state.p, state.p};
                context->PSSetShaderResources(0, 2, views);
                context->PSSetSamplers(0, 2, samplers);
                context->RSSetViewports(1, &viewport);
                context->OMSetRenderTargets(1, &target.p, nullptr);
                context->DrawIndexed(3, 0, 0);
            };
            auto image = [&]() {
                draw();
                context->CopyResource(staging.p, output.p);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                Result(context->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped), "image readback");
                std::vector<unsigned char> bytes(16 * 16 * 16);
                for (int y = 0; y < 16; y++)
                    memcpy(bytes.data() + y * 16 * 16, static_cast<char *>(mapped.pData) + y * mapped.RowPitch,
                           16 * 16);
                context->Unmap(staging.p, 0);
                return bytes;
            };
            auto ready = [&](long long expected) {
                ULONGLONG deadline = GetTickCount64() + 5000;
                for (;;)
                {
                    read(stats, 12);
                    if (stats[7] >= expected)
                        return;
                    if (GetTickCount64() > deadline)
                    {
                        std::printf("enabled=%lld sources=%lld prepared=%lld binds=%lld fallback=%lld failures=%lld\n",
                                    stats[1], stats[2], stats[7], stats[8], stats[9], stats[11]);
                        Com<ID3DBlob> assembly;
                        if (SUCCEEDED(D3DDisassemble(pixel->GetBufferPointer(), pixel->GetBufferSize(), 0, nullptr,
                                                     assembly.Out())))
                            std::printf("%s\n", static_cast<char *>(assembly->GetBufferPointer()));
                        throw std::runtime_error("background map preparation timed out");
                    }
                    Sleep(1);
                }
            };
            gate(0);
            auto baseline = image();
            gate(1);
            draw();
            ready(1);
            Check(image() == baseline, "packed output equals original");
            read(stats, 12);
            Check(stats[8] > 0, "packed draw actually executed");
            long long beforeIdentical = stats[8];
            context->UpdateSubresource(red.p, 0, nullptr, a.data(), 16, 32);
            Check(image() == baseline, "identical upload preserves exact output");
            read(stats, 12);
            Check(stats[8] > beforeIdentical && stats[7] == 1, "identical upload reuses the existing packed map");
            a[0] ^= 77;
            context->UpdateSubresource(red.p, 0, nullptr, a.data(), 16, 32);
            gate(0);
            baseline = image();
            read(stats, 12);
            long long originalBinds = stats[8];
            gate(1);
            Check(image() == baseline, "updated input never uses stale map");
            read(stats, 12);
            Check(stats[8] == originalBinds, "changing textures keep the original path");
            ID3D11ShaderResourceView *none{};
            context->PSSetShaderResources(0, 1, &none);
            redView.Reset();
            red.Reset();
            Result(device->CreateTexture2D(&desc, nullptr, red.Out()), "replacement red texture");
            Result(device->CreateShaderResourceView(red.p, nullptr, redView.Out()), "replacement red view");
            D3D11_TEXTURE2D_DESC upload = desc;
            upload.Usage = D3D11_USAGE_STAGING;
            upload.BindFlags = 0;
            upload.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            Com<ID3D11Texture2D> uploadTexture;
            Result(device->CreateTexture2D(&upload, nullptr, uploadTexture.Out()), "upload texture");
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Result(context->Map(uploadTexture.p, 0, D3D11_MAP_WRITE, 0, &mapped), "write map");
            for (int y = 0; y < 2; y++)
                memcpy(static_cast<char *>(mapped.pData) + y * mapped.RowPitch, b.data() + y * 16, 16);
            context->Unmap(uploadTexture.p, 0);
            context->CopyResource(red.p, uploadTexture.p);
            gate(0);
            baseline = image();
            gate(1);
            Check(image() == baseline, "staging copy fallback is exact");
            ready(2);
            Check(image() == baseline, "staging copy rebuild is exact");
            D3D11_BOX box{0, 0, 0, 4, 4, 1};
            context->UpdateSubresource(red.p, 0, &box, a.data(), 8, 8);
            gate(0);
            baseline = image();
            read(stats, 12);
            long long before = stats[8];
            gate(1);
            Check(image() == baseline, "partial write uses original texture");
            read(stats, 12);
            Check(stats[8] == before, "partial upload cannot select stale packed data");
            clear();
            read(stats, 12);
            Check(stats[4] == 0, "scene clear releases packed GPU cache");
            limits(1024 * 1024, 16);
            context->UpdateSubresource(red.p, 0, nullptr, a.data(), 16, 32);
            context->UpdateSubresource(green.p, 0, nullptr, b.data(), 16, 32);
            read(stats, 12);
            long long failed = stats[11];
            draw();
            ULONGLONG deadline = GetTickCount64() + 5000;
            do
            {
                Sleep(1);
                read(stats, 12);
            } while (stats[11] == failed && GetTickCount64() < deadline);
            Check(stats[11] > failed && stats[4] == 0, "small GPU budget fails closed without allocation");
            context->ClearState();
            context->Flush();
        }
        shutdown();
        read(stats, 12);
        Check(stats[3] == 0 && stats[4] == 0, "shutdown releases CPU and GPU payloads");
        std::printf(
            "Cloud maps: GPU byte parity, full upload, Map/Unmap copy, partial-write rejection, memory limit, scene clear and shutdown passed. Fusion coexistence: %s.\n", withFusion ? "enabled" : "not requested");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL: %s\n", error.what());
        shutdown();
        return 1;
    }
}
