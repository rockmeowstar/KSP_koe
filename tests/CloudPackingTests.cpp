#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <fstream>
#include <vector>
#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 5) return 2;
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) return 3;
    auto initialize = reinterpret_cast<int (*)(const wchar_t *, const wchar_t *, int)>(GetProcAddress(module, "InitializeCloudGuards"));
    auto enable = reinterpret_cast<void (*)(int)>(GetProcAddress(module, "SetCloudPackingEnabled"));
    auto read = reinterpret_cast<void (*)(long long *, int)>(GetProcAddress(module, "ReadCloudPacking"));
    auto activeMode = reinterpret_cast<int (*)()>(GetProcAddress(module, "GetCloudPackingMode"));
    auto live = reinterpret_cast<long long (*)()>(GetProcAddress(module, "GetLiveCloudPackingPrograms"));
    if (!initialize || !enable || !read || !activeMode || !live) return 4;
    enable(1);
    if (initialize(argv[2], argv[3], 1)) return 5;
    int defaultMode=activeMode();
    ID3D11Device *device{};
    ID3D11DeviceContext *context{};
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                D3D11_SDK_VERSION, &device, &level, &context))) return 6;
    if (level < D3D_FEATURE_LEVEL_11_0) return 7;
    std::vector<std::vector<char>> sources;
    ID3D11PixelShader *firstShader{};
    for (int i = 4; i < argc; ++i)
    {
        std::ifstream input(argv[i], std::ios::binary);
        sources.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        auto &bytes = sources.back();
        if (bytes.empty()) return 8;
        ID3D11PixelShader *shader{};
        if (FAILED(device->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &shader)) || !shader) return 9;
        if (!firstShader) { firstShader=shader; firstShader->AddRef(); }
        context->PSSetShader(shader, nullptr, 0);
        context->PSSetShader(nullptr, nullptr, 0);
        shader->Release();
    }
    long long packed[7]{};
    read(packed, 7);
    if (packed[1] != 1 || packed[3] == 0 || packed[4] != 0 || packed[5] == 0) return 10;
    auto readGuards = reinterpret_cast<void (*)(long long *, int)>(GetProcAddress(module, "ReadCloudGuards"));
    if (!readGuards) return 13;
    long long guards[12]{};
    readGuards(guards, 12);
    for (int i = 0; i < 2; ++i)
        if (guards[4 + i] > 0 && (guards[6 + i] == 0 || guards[8 + i] == 0)) return 14;
    enable(0);
    ID3D11PixelShader *original{};
    if (FAILED(device->CreatePixelShader(sources.front().data(), sources.front().size(), nullptr, &original))) return 11;
    original->Release();
    original=firstShader;
    auto select = reinterpret_cast<int (*)(int)>(GetProcAddress(module, "SetCloudPackingMode"));
    if (!select) return 15;
    wchar_t setting[2]{};
    bool variants = GetEnvironmentVariableW(L"KOE_CLOUD_VARIANTS", setting, 2) == 1 && setting[0] == L'1';
    for (int trial = 0; trial <= (variants?4:1); ++trial)
    {
        int mode=variants?trial:(trial==0?0:defaultMode);
        if (select(mode) != mode) return 16;
        context->PSSetShader(original, nullptr, 0);
        ID3D11PixelShader *actual{};
        context->PSGetShader(&actual, nullptr, nullptr);
        bool correct = actual && ((mode == 0) == (actual == original));
        if (actual) actual->Release();
        if (!correct) return 17;
    }
    context->PSSetShader(nullptr, nullptr, 0);
    select(0);
    original->Release();
    long long disabled[7]{};
    read(disabled, 7);
    if (disabled[1] != 0 || disabled[3] != packed[3]) return 12;
    long long retained=live();
    enable(1);
    for(int repeat=0;repeat<8;++repeat)
    {
        ID3D11PixelShader *temporary{};
        if(FAILED(device->CreatePixelShader(sources[1].data(),sources[1].size(),nullptr,&temporary)))return 18;
        context->PSSetShader(temporary,nullptr,0);
        context->PSSetShader(nullptr,nullptr,0);
        temporary->Release();
    }
    if(live()>retained)return 19;
    enable(0);
    std::printf("sources=%d attempts=%lld optimized=%lld driver_rejections=%lld conversions=%lld compile_ms=%.3f disabled_bypass=passed\n",
                argc - 4, packed[2], packed[3], packed[4], packed[5], packed[6] / 1000.0);
    std::printf("guard_builds=%lld,%lld guard_binds=%lld,%lld\n", guards[6], guards[7], guards[8], guards[9]);
    std::printf("source_lifetime=passed live_programs=%lld default_mode=%d\n",live(),defaultMode);
    context->Release();
    device->Release();
    return 0;
}
