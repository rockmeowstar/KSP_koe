#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
static unsigned checks;
static void Check(bool ok, const char *message)
{
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", message); std::exit(1); }
}
template<class T> T Export(HMODULE module, const char *name)
{
    auto entry = GetProcAddress(module, name); Check(entry != nullptr, name); return reinterpret_cast<T>(entry);
}
int wmain(int argc, wchar_t **argv)
{
    Check(argc == 2, "asset folder"); std::filesystem::path root(argv[1]);
    auto module = LoadLibraryW((root / L"KoeInstancedLights.dll").c_str()); Check(module != nullptr, "load adapter");
    auto init = Export<int(*)(const wchar_t *, int)>(module, "InitializeInstancedLights");
    Check(init(root.c_str(), 1) == 0, "initialize adapter");
    auto snapshot = Export<int(*)(ID3D11DeviceContext *, void *, size_t)>(module, "ReadLightSnapshot");
    auto shadowCount = Export<size_t(*)()>(module, "CapturedShadowCount");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context)), "create hardware device");
    constexpr unsigned sizes[] = {3, 6, 4, 21, 6, 8, 22};
    std::array<ComPtr<ID3D11Buffer>, 7> buffers;
    ID3D11Buffer *bindings[7]{};
    for (unsigned i = 0; i < 7; ++i)
    {
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth = sizes[i] * 16;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; desc.Usage = D3D11_USAGE_DYNAMIC; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffers[i])), "create light buffer"); bindings[i] = buffers[i].Get();
    }
    context->VSSetConstantBuffers(0, 4, bindings); context->PSSetConstantBuffers(0, 3, bindings + 4);
    std::array<unsigned char, 2048> actual, expected{}; actual.fill(0xcd);
    Check(shadowCount() == 0, "cold adapter owns no mirrors");
    Check(snapshot(context.Get(), actual.data(), actual.size()) == 1, "cold snapshot retains original draw");
    for (auto byte : actual) Check(byte == 0xcd, "failed snapshot must publish no partial record");
    Check(shadowCount() == 7, "only seven bound light buffers enrolled");
    for (unsigned turn = 0; turn < 40; ++turn)
    {
        unsigned offset = 0;
        for (unsigned i = 0; i < 7; ++i)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(context->Map(buffers[i].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)), "update light constants");
            for (unsigned j = 0; j < sizes[i] * 16; ++j)
                expected[offset + j] = static_cast<unsigned char>(turn * 37 + i * 11 + j * 19);
            std::memcpy(mapped.pData, expected.data() + offset, sizes[i] * 16); context->Unmap(buffers[i].Get(), 0);
            offset += sizes[i] * 16;
        }
        actual.fill(0xcd);
        Check(snapshot(context.Get(), actual.data(), actual.size()) == 0, "complete snapshot available");
        Check(actual == expected, "all 70 vectors byte-identical and 58 padding vectors zero");
        auto *missing = bindings[2]; bindings[2] = nullptr; context->VSSetConstantBuffers(0, 4, bindings);
        actual.fill(0xcd);
        Check(snapshot(context.Get(), actual.data(), actual.size()) == 1, "missing slot retains original draw");
        for (auto byte : actual) Check(byte == 0xcd, "failed subsequent snapshot publishes no stale values");
        bindings[2] = missing; context->VSSetConstantBuffers(0, 4, bindings);
    }
    Check(snapshot(context.Get(), actual.data(), actual.size() - 1) == -1, "short destination rejected");
    std::printf("LIGHT_SNAPSHOT_PARITY_PASS checks=%u records=40\n", checks);
}
