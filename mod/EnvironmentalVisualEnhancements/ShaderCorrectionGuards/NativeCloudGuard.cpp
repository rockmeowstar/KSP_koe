// AI-generated comment: Experimental DX11 bytecode substitution. Only two exact SHA256 identities match.
// Both objects remain alive so disabling selects the original without recompilation.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <bcrypt.h>
#include <atomic>
#include <array>
#include <vector>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <cstring>
#include "MinHook.h"
#include "ShaderPatch.h"

using CreatePS = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
                                              ID3D11PixelShader **);
using SetPS = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
static CreatePS originalCreate{};
static SetPS originalSet{};
static BCRYPT_ALG_HANDLE algorithm{};
static void *createTarget{};
static void *setTarget{};
static std::atomic<bool> enabled{false}, initialized{false};
static std::mutex publishMutex;
struct Variant
{
    SIZE_T sourceSize;
    const char *sourceHash;
    const char *candidateHash;
    std::vector<unsigned char> recipe;
    std::vector<unsigned char> bytes;
};
static Variant variants[] = {{114104,
                              "da29201451a10e0edd62ab037e5b4211368abe5bbd8e4f84366a6325ff028ec1",
                              "04e00f9599ad879681934517e2ab19834c2d56688e14937294613daa6b9258ef",
                              {},
                              {}},
                             {119680,
                              "3475c9b577a06288c501fbdb489bb7221e1ce9417627261fab081b3288954e30",
                              "237f4974991511b6f89d4d6bc278f1fa8be474572a04efb912d5bb0490c5212a",
                              {},
                              {}}};
struct Entry
{
    ID3D11PixelShader *source{};
    ID3D11PixelShader *candidate{};
    int variant{};
};
static std::array<Entry, 64> entries{};
static std::atomic<unsigned> entryCount{};
static std::atomic<long long> creates{}, matches[2]{}, built[2]{}, binds[2]{}, failures{}, unknownSized{};
static bool Hash(const void *bytes, SIZE_T length, const char *expected)
{
    if (length > 0xffffffffULL)
        return false;
    unsigned char digest[32];
    char hex[65];
    if (BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(static_cast<const unsigned char *>(bytes)),
                   static_cast<ULONG>(length), digest, 32) < 0)
        return false;
    constexpr char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
    {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    return std::strcmp(hex, expected) == 0;
}
static HRESULT STDMETHODCALLTYPE Create(ID3D11Device *device, const void *bytes, SIZE_T length,
                                        ID3D11ClassLinkage *linkage, ID3D11PixelShader **output)
{
    HRESULT result = originalCreate(device, bytes, length, linkage, output);
    creates++;
    if (FAILED(result) || !output || !*output || !bytes)
        return result;
    for (int i = 0; i < 2; i++)
    {
        auto &v = variants[i];
        if (length != v.sourceSize)
            continue;
        if (!Hash(bytes, length, v.sourceHash))
        {
            unknownSized++;
            continue;
        }
        matches[i]++;
        try
        {
            std::lock_guard<std::mutex> guard(publishMutex);
            unsigned count = entryCount.load();
            for (unsigned j = 0; j < count; j++)
                if (entries[j].source == *output)
                    return result;
            if (count >= entries.size())
            {
                failures++;
                return result;
            }
            if (v.bytes.empty() && (!KoeShaderPatch::Apply(v.recipe, bytes, length, v.bytes) ||
                                    !Hash(v.bytes.data(), v.bytes.size(), v.candidateHash)))
            {
                failures++;
                return result;
            }
            ID3D11PixelShader *candidate{};
            HRESULT hr = originalCreate(device, v.bytes.data(), v.bytes.size(), linkage, &candidate);
            if (FAILED(hr) || !candidate)
            {
                failures++;
                return result;
            }
            (*output)->AddRef();
            entries[count] = {*output, candidate, i};
            entryCount.store(count + 1, std::memory_order_release);
            built[i]++;
        }
        catch (...)
        {
            failures++;
        }
        break;
    }
    return result;
}
static void STDMETHODCALLTYPE Set(ID3D11DeviceContext *context, ID3D11PixelShader *shader,
                                  ID3D11ClassInstance *const *instances, UINT count)
{
    if (enabled.load(std::memory_order_relaxed) && shader)
    {
        unsigned size = entryCount.load(std::memory_order_acquire);
        for (unsigned i = 0; i < size; i++)
            if (entries[i].source == shader)
            {
                binds[entries[i].variant]++;
                shader = entries[i].candidate;
                break;
            }
    }
    originalSet(context, shader, instances, count);
}
extern "C" __declspec(dllexport) void SetCloudGuardsEnabled(int value)
{
    enabled.store(value != 0);
}
extern "C" __declspec(dllexport) void ReadCloudGuards(long long *out, int count)
{
    if (!out || count != 12)
        return;
    long long values[] = {1,
                          initialized.load() ? 1LL : 0LL,
                          enabled.load() ? 1LL : 0LL,
                          creates.load(),
                          matches[0].load(),
                          matches[1].load(),
                          built[0].load(),
                          built[1].load(),
                          binds[0].load(),
                          binds[1].load(),
                          failures.load(),
                          unknownSized.load()};
    std::memcpy(out, values, sizeof(values));
}
extern "C" __declspec(dllexport) int InitializeCloudGuards(const wchar_t *basePath, const wchar_t *topPath, int enable)
{
    if (initialized.load())
        return -1;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return -2;
    const wchar_t *paths[] = {basePath, topPath};
    try
    {
        for (int i = 0; i < 2; i++)
        {
            std::ifstream stream(std::filesystem::path(paths[i]), std::ios::binary | std::ios::ate);
            if (!stream)
                return -3;
            auto length = stream.tellg();
            if (length < 32 || length > 1024 * 1024)
                return -4;
            auto &bytes = variants[i].recipe;
            bytes.resize(static_cast<size_t>(length));
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                return -5;
            if (bytes.size() < 88 || std::memcmp(bytes.data(), "KOEPATCH", 8))
                return -6;
        }
    }
    catch (...)
    {
        return -7;
    }
    ID3D11Device *device{};
    ID3D11DeviceContext *context{};
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &level, &context);
    if (FAILED(hr))
        return -8;
    // AI-generated comment: Indices checked against the installed Windows SDK ID3D11* C vtables.
    createTarget = (*reinterpret_cast<void ***>(device))[15];
    setTarget = (*reinterpret_cast<void ***>(context))[9];
    context->Release();
    device->Release();
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return -9;
    if (MH_CreateHook(createTarget, reinterpret_cast<void *>(Create), reinterpret_cast<void **>(&originalCreate)) !=
        MH_OK)
        return -10;
    if (MH_CreateHook(setTarget, reinterpret_cast<void *>(Set), reinterpret_cast<void **>(&originalSet)) != MH_OK)
    {
        MH_RemoveHook(createTarget);
        return -11;
    }
    if (MH_EnableHook(createTarget) != MH_OK)
    {
        MH_RemoveHook(setTarget);
        MH_RemoveHook(createTarget);
        return -12;
    }
    if (MH_EnableHook(setTarget) != MH_OK)
    {
        MH_DisableHook(createTarget);
        MH_RemoveHook(setTarget);
        MH_RemoveHook(createTarget);
        return -13;
    }
    enabled = enable != 0;
    initialized = true;
    return 0;
}
// AI-generated comment: Hooks and retained COM references intentionally live until process exit.
// No shared MinHook global shutdown and no DLL unloading while render threads run.
