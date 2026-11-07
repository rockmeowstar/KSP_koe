// AI-generated comment: Guard recipes require two exact SHA256 identities;
// optional half unpacking is recognized and proved from each program's instructions.
// Guard objects remain alive so disabling guards selects the original without recompilation.
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
#include <map>
#include <memory>
#include <chrono>
#include <algorithm>
#include "MinHook.h"
#include "ShaderPatch.h"
#include "DxilHash.h"

extern "C" void *koe_compile_cloud_half(const unsigned char *, size_t, const unsigned char **, size_t *, size_t *);
extern "C" void *koe_compile_cloud_half_variant(const unsigned char *, size_t, unsigned, const unsigned char **, size_t *, size_t *);
extern "C" void koe_free_cloud_half(void *);

using CreatePS = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
                                              ID3D11PixelShader **);
using SetPS = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
static CreatePS originalCreate{};
static SetPS originalSet{};
#include "CloudMapCache.h"
static BCRYPT_ALG_HANDLE algorithm{};
static void *createTarget{};
static void *setTarget{};
static std::atomic<bool> enabled{false}, initialized{false};
static std::atomic<bool> packingEnabled{false};
static constexpr int defaultPackingMode=4;
static std::atomic<int> packingMode{defaultPackingMode};
static bool packingVariants{};
static std::atomic<long long> packingAttempts{}, packingBuilt{}, packingRejected{}, packingConversions{}, packingMicros{};
struct PackedProgram { std::array<std::vector<unsigned char>,4> bytes; std::array<size_t,4> conversions{}; };
static std::atomic<unsigned long long> bindingGeneration{};
static std::atomic<long long> livePackingPrograms{};
static const GUID packingIdentity={0x84290c31,0x750c,0x423b,{0x86,0xd3,0x3a,0x99,0x48,0xb4,0x78,0xa2}};
struct PackedState
{
    std::array<ID3D11PixelShader *,4> candidates{};
    ~PackedState(){for(auto *shader:candidates)if(shader)shader->Release();}
};
class PackedOwner final:public IUnknown
{
    // AI-generated comment: The source owns this interface; the render cache retains only its variant state.
    std::atomic<ULONG> references{1};
public:
    const std::shared_ptr<PackedState> state;
    explicit PackedOwner(std::shared_ptr<PackedState> value):state(std::move(value)){livePackingPrograms++;}
    ~PackedOwner(){bindingGeneration.fetch_add(1,std::memory_order_release);livePackingPrograms--;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **result) override
    {
        if(!result)return E_POINTER;
        *result=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *result=static_cast<IUnknown *>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return references.fetch_add(1,std::memory_order_relaxed)+1;}
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG remaining=references.fetch_sub(1,std::memory_order_acq_rel)-1;
        if(!remaining)delete this;
        return remaining;
    }
};
static std::shared_ptr<PackedState> PackedFor(ID3D11PixelShader *shader)
{
    IUnknown *value{};UINT length=sizeof(value);
    if(FAILED(shader->GetPrivateData(packingIdentity,&length,&value))||!value)return {};
    auto owner=static_cast<PackedOwner *>(value);
    auto state=owner->state;owner->Release();
    return state;
}
static std::atomic<long long> packingBinds[4]{};
static std::map<std::array<unsigned char, 32>, std::shared_ptr<const PackedProgram>> packedPrograms;
static std::mutex packingMutex;
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
static HRESULT CreatePacked(ID3D11Device *device, const void *bytes, SIZE_T length,
                            ID3D11ClassLinkage *linkage, ID3D11PixelShader **output)
{
    HRESULT originalResult = originalCreate(device, bytes, length, linkage, output);
    if(SUCCEEDED(originalResult)&&output&&*output)CloudMaps::BuildProgram(device,bytes,length,linkage,*output);
    if (FAILED(originalResult) || !packingEnabled.load(std::memory_order_relaxed) || !bytes || !output || !*output || length < 32 ||
        length > UINT_MAX || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
        return originalResult;
    if(PackedFor(*output))return originalResult;
    bool exponent = false, mantissa = false;
    for (size_t offset = 0; offset + 4 <= length; offset += 4)
    {
        uint32_t word;
        std::memcpy(&word, static_cast<const unsigned char *>(bytes) + offset, 4);
        exponent |= word == 0x7c00;
        mantissa |= word == 1023;
    }
    if (!exponent || !mantissa)
        return originalResult;
    try
    {
        std::array<unsigned char, 32> digest;
        if (BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(static_cast<const unsigned char *>(bytes)),
                       static_cast<ULONG>(length), digest.data(), static_cast<ULONG>(digest.size())) < 0)
            return originalResult;
        std::shared_ptr<const PackedProgram> program;
        {
            std::lock_guard<std::mutex> lock(packingMutex);
            auto found = packedPrograms.find(digest);
            if (found != packedPrograms.end()) program = found->second;
        }
        if (!program)
        {
            auto start = std::chrono::steady_clock::now();
            auto candidate = std::make_shared<PackedProgram>();
            for (unsigned variant = 0; variant < 4; ++variant)
            {
                if(!packingVariants&&variant+1!=defaultPackingMode)continue;
                const unsigned char *compiledBytes{};
                size_t compiledLength{}, conversions{};
                std::unique_ptr<void, decltype(&koe_free_cloud_half)> compiled(
                    koe_compile_cloud_half_variant(static_cast<const unsigned char *>(bytes), length, variant + 1,
                                                   &compiledBytes, &compiledLength, &conversions), koe_free_cloud_half);
                if (compiled && compiledLength >= 32 && compiledLength <= UINT_MAX)
                {
                    auto &code = candidate->bytes[variant];
                    code.assign(compiledBytes, compiledBytes + compiledLength);
                    candidate->conversions[variant] = conversions;
                    ComputeHashRetail(code.data() + 20, static_cast<UINT32>(compiledLength - 20), code.data() + 4);
                }
            }
            packingAttempts++;
            packingMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::lock_guard<std::mutex> lock(packingMutex);
            program = packedPrograms.emplace(digest, candidate).first->second;
        }
        auto binding=std::make_shared<PackedState>();
        bool any = false;
        for (size_t variant = 0; variant < program->bytes.size(); ++variant)
        {
            const auto &code = program->bytes[variant];
            if (code.empty()) continue;
            HRESULT hr = originalCreate(device, code.data(), code.size(), linkage, &binding->candidates[variant]);
            if (SUCCEEDED(hr) && binding->candidates[variant])
            {
                if(binding->candidates[variant]==*output)
                {
                    binding->candidates[variant]->Release();binding->candidates[variant]=nullptr;
                    continue;
                }
                packingBuilt++;
                CloudMaps::BuildProgram(device,code.data(),code.size(),linkage,binding->candidates[variant],true);
                packingConversions += program->conversions[variant];
                any = true;
            }
            else packingRejected++;
        }
        if (any)
        {
            auto owner=new PackedOwner(binding);
            HRESULT attached=(*output)->SetPrivateDataInterface(packingIdentity,owner);
            owner->Release();
            if(SUCCEEDED(attached))bindingGeneration.fetch_add(1,std::memory_order_release);
            else packingRejected++;
        }
    }
    catch (...) { packingRejected++; }
    return originalResult;
}
static HRESULT STDMETHODCALLTYPE Create(ID3D11Device *device, const void *bytes, SIZE_T length,
                                        ID3D11ClassLinkage *linkage, ID3D11PixelShader **output)
{
    HRESULT result = CreatePacked(device, bytes, length, linkage, output);
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
            HRESULT hr = CreatePacked(device, v.bytes.data(), v.bytes.size(), linkage, &candidate);
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
    if (packingEnabled.load(std::memory_order_relaxed) && shader)
    {
        struct Cache { ID3D11PixelShader *source{}; std::shared_ptr<PackedState> state; unsigned long long generation{}; };
        static thread_local Cache cache;
        auto generation = bindingGeneration.load(std::memory_order_acquire);
        if (cache.source != shader || cache.generation != generation)
        {
            cache = {shader, PackedFor(shader), generation};
        }
        int mode = packingMode.load(std::memory_order_relaxed);
        if (mode >= 1 && mode <= 4 && cache.state && cache.state->candidates[mode-1])
        {
            shader = cache.state->candidates[mode-1];
            packingBinds[mode-1]++;
        }
    }
    CloudMaps::Remember(context,shader,count);
    originalSet(context, shader, instances, count);
}
extern "C" __declspec(dllexport) void SetCloudGuardsEnabled(int value)
{
    enabled.store(value != 0);
}
extern "C" __declspec(dllexport) void SetCloudPackingEnabled(int value)
{
    packingMode.store(defaultPackingMode);
    packingEnabled.store(value != 0);
}
extern "C" __declspec(dllexport) int SetCloudPackingMode(int mode)
{
    if (mode < 0 || mode > 4 || (!packingVariants&&mode!=0&&mode!=defaultPackingMode)) return -1;
    packingMode.store(mode == 0 ? defaultPackingMode : mode);
    packingEnabled.store(mode != 0);
    return mode;
}
extern "C" __declspec(dllexport) int GetCloudPackingMode(){return packingEnabled.load()?packingMode.load():0;}
extern "C" __declspec(dllexport) long long GetLiveCloudPackingPrograms(){return livePackingPrograms.load();}
extern "C" __declspec(dllexport) void ReadCloudPackingBinds(long long *out, int count)
{
    if (!out || count != 4) return;
    for (int i=0;i<4;++i) out[i] = packingBinds[i].load();
}
extern "C" __declspec(dllexport) void ReadCloudPacking(long long *out, int count)
{
    if (!out || count != 7) return;
    long long values[] = {1, packingEnabled.load() ? 1LL : 0LL, packingAttempts.load(),
                         packingBuilt.load(), packingRejected.load(), packingConversions.load(), packingMicros.load()};
    std::memcpy(out, values, sizeof(values));
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
    wchar_t variantSetting[2]{};
    packingVariants = GetEnvironmentVariableW(L"KOE_CLOUD_VARIANTS", variantSetting, 2) == 1 && variantSetting[0] == L'1';
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
    auto releaseDevice=[](ID3D11Device *value){value->Release();};
    auto releaseContext=[](ID3D11DeviceContext *value){value->Release();};
    std::unique_ptr<ID3D11Device,decltype(releaseDevice)> deviceOwner(device,releaseDevice);
    std::unique_ptr<ID3D11DeviceContext,decltype(releaseContext)> contextOwner(context,releaseContext);
    // AI-generated comment: Indices checked against the installed Windows SDK ID3D11* C vtables.
    createTarget = (*reinterpret_cast<void ***>(device))[15];
    setTarget = (*reinterpret_cast<void ***>(context))[9];
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
    CloudMaps::Initialize(device,context);
    return 0;
}
// AI-generated comment: Hooks and retained COM references intentionally live until process exit.
// No shared MinHook global shutdown and no DLL unloading while render threads run.
