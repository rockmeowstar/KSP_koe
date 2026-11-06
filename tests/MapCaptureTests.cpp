#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <thread>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
using Microsoft::WRL::ComPtr;
static unsigned checks = 0;
static void Check(bool ok, const char *name)
{
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", name); std::exit(1); }
}
template<class T> T Export(HMODULE module, const char *name)
{
    auto address = GetProcAddress(module, name); Check(address != nullptr, name);
    return reinterpret_cast<T>(address);
}
int wmain(int argc, wchar_t **argv)
{
    Check(argc == 2 || argc == 3, "asset directory"); std::filesystem::path root(argv[1]);
    bool lookup = argc == 3;
    auto module = LoadLibraryW((root / L"KoeInstancedLights.dll").c_str()); Check(module != nullptr, "load lights");
    auto init = Export<int(*)(const wchar_t *, int)>(module, "InitializeInstancedLights");
    Check(init(root.c_str(), 1) == 0, "initialize lights");
    auto select = Export<int(*)(int)>(module, lookup ? "SetCaptureLookupMode" : "SetMapCaptureMode");
    auto counters = Export<int(*)(uint64_t *, int)>(module, lookup ? "ReadCaptureLookupCounters" : "ReadMapCaptureCounters");
    auto captured = Export<int(*)(ID3D11Resource *, void *, size_t)>(module, "ReadCapturedBuffer");
    auto mark = Export<int(*)(ID3D11Resource *)>(module, "MarkCaptureBufferWanted");
    auto shadowCount = Export<size_t(*)()>(module, "CapturedShadowCount");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context)), "hardware device");
    {
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth = 16; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.Usage = D3D11_USAGE_DYNAMIC; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> unused;
        Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &unused)), "unrecognized light buffer");
        for (unsigned i = 0; i < 100; ++i)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(context->Map(unused.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "map without matching light draw");
            std::memset(mapped.pData, 93, 16); context->Unmap(unused.Get(), 0);
        }
        Check(shadowCount() == 0, "unrecognized shaders must not allocate or mirror buffers");
    }
    unsigned patterns = 0;
    for (unsigned size : {16u, 48u, 352u, 1120u, 65536u})
    {
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth = size; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.Usage = D3D11_USAGE_DYNAMIC; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> a, b, staging;
        Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &a)), "buffer A");
        Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &b)), "buffer B");
        if (mark) { Check(mark(a.Get()) == 0, "mark A wanted"); Check(mark(b.Get()) == 0, "mark B wanted"); }
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &staging)), "readback buffer");
        std::vector<unsigned char> expected(size), actual(size);
        auto write = [&](ID3D11Buffer *buffer)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "map constant buffer");
            std::memcpy(mapped.pData, expected.data(), size);
        };
        auto verifyShadow = [&](ID3D11Buffer *buffer)
        {
            Check(captured(buffer, actual.data(), actual.size()) == 0, "captured bytes valid");
            Check(actual == expected, "exact captured-byte parity");
        };
        auto verifyGpu = [&](ID3D11Buffer *buffer)
        {
            context->CopyResource(staging.Get(), buffer); D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map GPU readback");
            Check(std::memcmp(mapped.pData, expected.data(), size) == 0, "exact GPU-byte parity");
            context->Unmap(staging.Get(), 0);
        };
        for (int mode : {0, 1, 0, 1})
        {
            Check(select(mode) == 0, "select capture mode");
            for (unsigned i = 0; i < size; ++i) expected[i] = static_cast<unsigned char>((i * 37 + patterns * 19) & 255);
            write(a.Get()); write(b.Get());
            context->Unmap(a.Get(), 0); context->Unmap(b.Get(), 0);
            verifyShadow(a.Get()); verifyShadow(b.Get()); verifyGpu(a.Get()); verifyGpu(b.Get());
            write(a.Get()); std::thread unmapper([&] { context->Unmap(a.Get(), 0); }); unmapper.join();
            verifyShadow(a.Get()); verifyGpu(a.Get());
            std::thread mapper([&] { write(a.Get()); }); mapper.join(); context->Unmap(a.Get(), 0);
            verifyShadow(a.Get()); verifyGpu(a.Get());
            write(a.Get()); Check(select(1 - mode) == 0, "switch mode while mapped"); context->Unmap(a.Get(), 0);
            verifyShadow(a.Get()); verifyGpu(a.Get()); context->ClearState(); ++patterns;
        }
    }
    Check(select(1) == 0, "select fast mode for rehash");
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = 16; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.Usage = D3D11_USAGE_DYNAMIC; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> pinned; Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &pinned)), "pinned buffer");
    if (mark) Check(mark(pinned.Get()) == 0, "mark pinned buffer wanted");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(pinned.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "map before rehash");
    std::memset(mapped.pData, 91, 16);
    std::thread grow([&]
    {
        for (unsigned n = 0; n < 600; ++n)
        {
            ComPtr<ID3D11Buffer> buffer; Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer)), "grow retained shadows");
            if (mark) Check(mark(buffer.Get()) == 0, "mark rehash buffer wanted");
            D3D11_MAPPED_SUBRESOURCE data{};
            Check(SUCCEEDED(context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &data)), "map rehash buffer");
            std::memset(data.pData, 0, 16); context->Unmap(buffer.Get(), 0);
        }
    }); grow.join();
    context->Unmap(pinned.Get(), 0); unsigned char actual[16]{};
    Check(captured(pinned.Get(), actual, sizeof(actual)) == 0, "retained pointer survives rehash");
    for (auto value : actual) Check(value == 91, "rehash content parity");
    if (lookup)
    {
        ComPtr<ID3D11Buffer> unknown; Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &unknown)), "unwanted buffer");
        Check(SUCCEEDED(context->Map(unknown.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "map unwanted buffer");
        std::memset(mapped.pData, 81, 16); context->Unmap(unknown.Get(), 0);
        Check(captured(unknown.Get(), actual, sizeof(actual)) == -1, "unwanted buffer not captured");
        Check(mark(unknown.Get()) == 0, "transition to wanted");
        for (unsigned n = 0; n < 3; ++n)
        {
            Check(SUCCEEDED(context->Map(unknown.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "map newly wanted buffer");
            std::memset(mapped.pData, 81, 16); context->Unmap(unknown.Get(), 0);
            Check(captured(unknown.Get(), actual, sizeof(actual)) == 0, "newly wanted buffer captured");
            for (auto value : actual) Check(value == 81, "wanted transition byte parity");
        }
        ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext; ComPtr<ID3D11Buffer> otherBuffer;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &otherDevice, &level, &otherContext)), "second device");
        Check(SUCCEEDED(otherDevice->CreateBuffer(&desc, nullptr, &otherBuffer)), "second device buffer");
        Check(mark(otherBuffer.Get()) == 0, "second device wanted buffer");
        for (auto *current : {otherContext.Get(), context.Get(), otherContext.Get(), context.Get()})
        {
            auto *buffer = current == context.Get() ? unknown.Get() : otherBuffer.Get();
            Check(SUCCEEDED(current->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "switch context map");
            std::memset(mapped.pData, 67, 16); current->Unmap(buffer, 0);
            Check(captured(buffer, actual, sizeof(actual)) == 0, "switch context capture");
            for (auto value : actual) Check(value == 67, "context switch byte parity");
        }
    }
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
    ComPtr<ID3D11Buffer> unmappable; Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &unmappable)), "default buffer");
    Check(FAILED(context->Map(unmappable.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "failed map stays failed");
    Check(captured(unmappable.Get(), actual, sizeof(actual)) == -1, "failed map publishes no capture");
    uint64_t stats[4]{}; Check(counters(stats, 4) == 1 && stats[1] > 0 && stats[2] > 0, "optimized map/unmap paths used");
    std::printf("%s_NATIVE_PARITY_PASS checks=%u patterns=%u counters=%llu,%llu,%llu,%llu\n", lookup ? "CAPTURE_LOOKUP" : "MAP_CAPTURE",
        checks, patterns, stats[0], stats[1], stats[2], stats[3]);
}
