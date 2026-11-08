// AI-generated comment: Exact-source DX11 light batching. No Unity lights or light parameters change.
#define WIN32_LEAN_AND_MEAN
#include "InstancedLightBatch.h"
#include "BufferValidity.h"
#include "BlendFactorUsage.h"
#include <windows.h>
#include <d3d11_3.h>
#include <bcrypt.h>
#include <atomic>
#include <array>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <string>
#include <cstring>
#include "MinHook.h"
using Microsoft::WRL::ComPtr;
using Context = ID3D11DeviceContext4;
static thread_local bool inside = false;
struct Guard
{
    bool prior = inside;
    Guard()
    {
        inside = true;
    }
    ~Guard()
    {
        inside = prior;
    }
};
static std::atomic<int> mode{0};
static std::atomic<bool> initialized{false}, sceneAllowed{false};
static std::atomic<bool> normalizeBlendFactor{false};
static std::atomic<uint64_t> normalizationChecks{}, normalizedFactors{};
static std::atomic<long long> draws{}, queued{}, batches{}, sources{}, fallbacks{}, mirrored{}, errors{}, samples{},
    different{}, changed{}, maxBatch{}, emptySamples{}, barriers{}, missingConstants{};
static BCRYPT_ALG_HANDLE algorithm{};
struct ShaderSpec
{
    const char *vsHash;
    const char *psHash;
    const char *candidateVSHash;
    const char *candidatePSHash;
    const wchar_t *vsFile;
    const wchar_t *psFile;
    std::vector<unsigned char> vs, ps;
};
#include "LightShaders.generated.h"
struct StateKey
{
    std::array<unsigned char, 2048> bytes;
    size_t length = 0;
    bool valid = true;
    template <class T> void Add(const T &v)
    {
        if (length + sizeof(v) > bytes.size())
        {
            valid = false;
            return;
        }
        std::memcpy(bytes.data() + length, &v, sizeof(v));
        length += sizeof(v);
    }
    template <class T> void Array(const T *data, UINT count)
    {
        bool exists = data != nullptr;
        Add(exists);
        if (!data)
            return;
        size_t n = size_t(count) * sizeof(T);
        if (length + n > bytes.size())
        {
            valid = false;
            return;
        }
        std::memcpy(bytes.data() + length, data, n);
        length += n;
    }
    bool operator==(const StateKey &other) const
    {
        return valid && other.valid && length == other.length && !std::memcmp(bytes.data(), other.bytes.data(), length);
    }
};
struct PixelEntry
{
    ID3D11PixelShader *source = nullptr;
    ID3D11Device *device = nullptr;
    InstancedLightBatch renderer;
    ComPtr<ID3D11Buffer> originalCB[7];
};
static std::array<ID3D11VertexShader *, 128> vertices{};
static std::atomic<unsigned> vertexCount{};
static std::array<PixelEntry, 128> pixels{};
static std::atomic<unsigned> pixelCount{};
static std::mutex shaderMutex;
struct BufferShadow
{
    ComPtr<ID3D11Buffer> resource;
    std::vector<unsigned char> bytes;
    BufferValidity valid;
    std::atomic<void *> mapped{nullptr};
    std::atomic<bool> wanted{false};
};
static std::mutex bufferMutex;
static bool fastBufferValidity = true;
static uint64_t validityCounters[4]{};
static std::unordered_map<ID3D11Resource *, BufferShadow> shadows;
static size_t shadowBytes = 0;
static std::atomic<bool> captureEnabled{false};
struct MappedShadow
{
    Context *context = nullptr;
    ID3D11Resource *resource = nullptr;
    BufferShadow *shadow = nullptr;
};
static thread_local MappedShadow lastMapped;
struct CaptureLookup
{
    struct Entry { ID3D11Resource *resource = nullptr; BufferShadow *shadow = nullptr; };
    ComPtr<Context> context;
    std::array<Entry, 64> entries{};
};
static thread_local CaptureLookup captureLookup;
#ifdef KOE_CAPTURE_LOOKUP_AUDIT
static std::atomic<bool> fastCaptureLookup{true};
static std::atomic<uint64_t> captureLookupCounters[4]{};
#endif
static bool UseCaptureLookup()
{
#ifdef KOE_CAPTURE_LOOKUP_AUDIT
    return fastCaptureLookup.load(std::memory_order_relaxed);
#else
    return true;
#endif
}
static void CountCaptureLookup(unsigned index)
{
#ifdef KOE_CAPTURE_LOOKUP_AUDIT
    captureLookupCounters[index].fetch_add(1, std::memory_order_relaxed);
#else
    (void)index;
#endif
}
#ifdef KOE_MAP_CAPTURE_AUDIT
static std::atomic<bool> fastMapCapture{true};
static std::atomic<uint64_t> mapCaptureCounters[4]{};
#endif
static bool UseFastMapCapture()
{
#ifdef KOE_MAP_CAPTURE_AUDIT
    return fastMapCapture.load(std::memory_order_relaxed);
#else
    return true;
#endif
}
static void CountMapCapture(unsigned index)
{
#ifdef KOE_MAP_CAPTURE_AUDIT
    mapCaptureCounters[index].fetch_add(1, std::memory_order_relaxed);
#else
    (void)index;
#endif
}
static constexpr unsigned sizes[] = {3, 6, 4, 21, 6, 8, 22}, starts[] = {0, 3, 9, 13, 34, 40, 48};
struct RuntimeContext
{
    ComPtr<Context> context;
    DWORD owner = 0;
    PixelEntry *pixel = nullptr;
    ComPtr<ID3D11VertexShader> vertex;
    unsigned count = 0, indexCount = 0, startIndex = 0;
    int baseVertex = 0;
    std::array<InstancedLightBatch::Record, 32> records{};
    std::array<StateKey, 149> keys{};
    std::array<bool, 149> keyValid{};
};
static RuntimeContext runtime;
static std::atomic<Context *> active{};
static std::atomic<unsigned> pending{};
static const char *failedHook = "none";
struct Binding
{
    Context *context = nullptr;
    ID3D11VertexShader *vertex = nullptr;
    ID3D11PixelShader *pixel = nullptr;
    PixelEntry *entry = nullptr;
    bool vsKnown = false, psKnown = false;
    UINT vsClasses = 0, psClasses = 0;
};
static thread_local std::array<Binding, 8> bindings{};
static Binding &Bound(Context *c)
{
    for (auto &b : bindings)
        if (b.context == c)
            return b;
    for (auto &b : bindings)
        if (!b.context)
        {
            b.context = c;
            return b;
        }
    bindings[0] = {};
    bindings[0].context = c;
    return bindings[0];
}
static void InvalidateBindings(Context *c)
{
    auto &b = Bound(c);
    b.vsKnown = b.psKnown = false;
}
static std::atomic<long long> auditInfo[9]{};
static std::atomic<long long> directSamples{}, directLit{};
static unsigned long long directAttempts = 0, auditAttempts = 0;
static void Flush();
static bool Track(Context *c)
{
    return !inside && mode.load(std::memory_order_relaxed) != 0 && c == active.load(std::memory_order_acquire) &&
           GetCurrentThreadId() == runtime.owner && (pending.load() != 0 || Bound(c).entry != nullptr);
}
static void Barrier(Context *c)
{
    if (inside || c != active.load(std::memory_order_acquire))
        return;
    if (GetCurrentThreadId() != runtime.owner)
        return;
    if (pending.load(std::memory_order_relaxed))
    {
        barriers++;
        Flush();
    }
    runtime.keyValid.fill(false);
}
static void BeforeCached(Context *, unsigned slot, const StateKey &key)
{
    if (!runtime.keyValid[slot] || !(runtime.keys[slot] == key))
    {
        if (pending.load())
            Flush();
        auto &stored = runtime.keys[slot];
        stored.length = key.length;
        stored.valid = key.valid;
        if (key.valid)
            std::memcpy(stored.bytes.data(), key.bytes.data(), key.length);
        runtime.keyValid[slot] = key.valid;
    }
}
static void InvalidateCachedSlot(Context *c, unsigned slot)
{
    if (inside || c != active.load() || GetCurrentThreadId() != runtime.owner)
        return;
    if (pending.load())
        Flush();
    runtime.keyValid[slot] = false;
}
static void BeforeConstantBinding(Context *c)
{
    if (mode.load() == 0)
        Barrier(c);
}
static void InvalidateBuffer(ID3D11Resource *resource)
{
    if (!captureEnabled.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto it = shadows.find(resource);
    if (it != shadows.end())
        it->second.valid.Clear(fastBufferValidity);
}
static void InvalidateAllBuffers()
{
    std::lock_guard<std::mutex> lock(bufferMutex);
    for (auto &item : shadows)
        item.second.valid.Clear(fastBufferValidity);
}
struct HookSpec
{
    unsigned slot;
    void *replacement;
    void **original;
    const char *name;
};
static void STDMETHODCALLTYPE Hook_DrawIndexed(Context *, UINT, UINT, INT);
static HRESULT STDMETHODCALLTYPE Hook_Map(Context *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE *);
static void STDMETHODCALLTYPE Hook_Unmap(Context *, ID3D11Resource *, UINT);
static void STDMETHODCALLTYPE Hook_UpdateSubresource(Context *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *,
                                                     UINT, UINT);
static void STDMETHODCALLTYPE Hook_UpdateSubresource1(Context *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                                      const void *, UINT, UINT, UINT);
static void STDMETHODCALLTYPE Hook_VSSetShader(Context *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
static void STDMETHODCALLTYPE Hook_PSSetShader(Context *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
static bool BlendFactorUsed(ID3D11BlendState* state)
{
    if(!normalizeBlendFactor.load(std::memory_order_relaxed))return true;
    static thread_local BlendFactorUsage usage;
    normalizationChecks.fetch_add(1,std::memory_order_relaxed);
    bool used=usage.Used(state);
    if(!used)normalizedFactors.fetch_add(1,std::memory_order_relaxed);
    return used;
}
#include "LightContextHooks.generated.h"
using CreateVS = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
                                              ID3D11VertexShader **);
using CreatePS = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
                                              ID3D11PixelShader **);
static CreateVS originalCreateVS{};
static CreatePS originalCreatePS{};
static bool Hash(const void *data, size_t length, const char *expected)
{
    if (length > 0xffffffffu)
        return false;
    unsigned char digest[32];
    char hex[65];
    const char *digits = "0123456789abcdef";
    if (BCryptHash(algorithm, nullptr, 0, (PUCHAR)data, static_cast<ULONG>(length), digest, 32) < 0)
        return false;
    for (unsigned i = 0; i < 32; ++i)
    {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    return !std::strcmp(hex, expected);
}
static bool ReadShader(const std::filesystem::path &path, std::vector<unsigned char> &bytes, const char *hash)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    auto n = file.tellg();
    if (n < 32 || n > 1024 * 1024)
        return false;
    bytes.resize(size_t(n));
    file.seekg(0);
    return bool(file.read((char *)bytes.data(), n)) && Hash(bytes.data(), bytes.size(), hash);
}
static HRESULT STDMETHODCALLTYPE HookCreateVS(ID3D11Device *device, const void *data, SIZE_T length,
                                              ID3D11ClassLinkage *linkage, ID3D11VertexShader **output)
{
    HRESULT hr = originalCreateVS(device, data, length, linkage, output);
    if (inside || FAILED(hr) || linkage || !output || !*output)
        return hr;
    if (!Hash(data, length, shaderSpecs[0].vsHash))
        return hr;
    Guard guard;
    std::lock_guard<std::mutex> lock(shaderMutex);
    unsigned n = vertexCount.load();
    for (unsigned i = 0; i < n; ++i)
        if (vertices[i] == *output)
            return hr;
    if (n == vertices.size())
    {
        errors++;
        return hr;
    }
    (*output)->AddRef();
    vertices[n] = *output;
    vertexCount.store(n + 1, std::memory_order_release);
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookCreatePS(ID3D11Device *device, const void *data, SIZE_T length,
                                              ID3D11ClassLinkage *linkage, ID3D11PixelShader **output)
{
    HRESULT hr = originalCreatePS(device, data, length, linkage, output);
    if (inside || FAILED(hr) || linkage || !output || !*output)
        return hr;
    for (auto &spec : shaderSpecs)
    {
        if (!Hash(data, length, spec.psHash))
            continue;
        Guard guard;
        std::lock_guard<std::mutex> lock(shaderMutex);
        unsigned n = pixelCount.load();
        for (unsigned i = 0; i < n; ++i)
            if (pixels[i].source == *output)
                return hr;
        if (n == pixels.size())
        {
            errors++;
            return hr;
        }
        auto &entry = pixels[n];
        if (FAILED(entry.renderer.Initialize(device, spec.vs.data(), spec.vs.size(), spec.ps.data(), spec.ps.size())))
        {
            errors++;
            return hr;
        }
        for (unsigned i = 0; i < 7; ++i)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizes[i] * 16;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&desc, nullptr, &entry.originalCB[i])))
            {
                errors++;
                return hr;
            }
        }
        (*output)->AddRef();
        device->AddRef();
        entry.source = *output;
        entry.device = device;
        pixelCount.store(n + 1, std::memory_order_release);
        break;
    }
    return hr;
}
static bool KnownVertex(ID3D11VertexShader *v)
{
    unsigned n = vertexCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < n; ++i)
        if (vertices[i] == v)
            return true;
    return false;
}
static PixelEntry *KnownPixel(ID3D11PixelShader *p)
{
    unsigned n = pixelCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < n; ++i)
        if (pixels[i].source == p)
            return &pixels[i];
    return nullptr;
}
static BufferShadow *Shadow(ID3D11Resource *resource)
{
    auto it = shadows.find(resource);
    if (it != shadows.end())
        return &it->second;
    D3D11_RESOURCE_DIMENSION type;
    resource->GetType(&type);
    if (type != D3D11_RESOURCE_DIMENSION_BUFFER)
        return nullptr;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))
        return nullptr;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || desc.ByteWidth > 65536 ||
        shadowBytes + desc.ByteWidth > 64 * 1024 * 1024)
        return nullptr;
    auto &item = shadows[resource];
    item.resource = buffer;
    item.bytes.resize(desc.ByteWidth);
    item.valid.Resize(desc.ByteWidth);
    shadowBytes += desc.ByteWidth;
    return &item;
}
static bool IsConstant(Context *c, ID3D11Resource *resource)
{
    if (!captureEnabled.load(std::memory_order_acquire) || mode.load() == 0 || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto it = shadows.find(resource);
    return it != shadows.end() && it->second.wanted.load(std::memory_order_acquire);
}
static BufferShadow *CaptureConstant(Context *c, ID3D11Resource *resource)
{
    if (!captureEnabled.load(std::memory_order_acquire) || mode.load() == 0 || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return nullptr;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto it = shadows.find(resource);
    return it != shadows.end() && it->second.wanted.load(std::memory_order_acquire) ? &it->second : nullptr;
}
static BufferShadow *FindCaptureConstant(Context *c, ID3D11Resource *resource)
{
    if (!captureEnabled.load(std::memory_order_acquire)) return nullptr;
    CountCaptureLookup(0);
    if (!UseCaptureLookup() || mode.load() == 0)
    {
        CountCaptureLookup(2); return CaptureConstant(c, resource);
    }
    if (captureLookup.context.Get() != c)
    {
        if (c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            CountCaptureLookup(2); return nullptr;
        }
        captureLookup.context = c;
        captureLookup.entries.fill({}); CountCaptureLookup(3);
    }
    const auto key = reinterpret_cast<uintptr_t>(resource) >> 4;
    auto &entry = captureLookup.entries[(key ^ (key >> 7)) % captureLookup.entries.size()];
    // AI-generated comment: Only the wanted flag is read without bufferMutex here.
    // Buffer bytes and validity remain protected; retained shadows cannot disappear.
    if (entry.resource == resource && entry.shadow && entry.shadow->wanted.load(std::memory_order_acquire))
    {
        CountCaptureLookup(1); return entry.shadow;
    }
    CountCaptureLookup(2);
    auto *shadow = CaptureConstant(c, resource);
    if (shadow) entry = {resource, shadow};
    return shadow;
}
static HRESULT STDMETHODCALLTYPE Hook_Map(Context *c, ID3D11Resource *resource, UINT sub, D3D11_MAP type, UINT flags,
                                          D3D11_MAPPED_SUBRESOURCE *mapped)
{
    const bool fast = UseFastMapCapture();
    BufferShadow *shadow = nullptr;
    bool eligible = false;
    if (!inside && resource && sub == 0)
    {
        if (fast) { shadow = FindCaptureConstant(c, resource); eligible = shadow != nullptr; }
        else eligible = IsConstant(c, resource);
    }
    bool capture = eligible &&
                   (type == D3D11_MAP_WRITE || type == D3D11_MAP_WRITE_DISCARD ||
                    type == D3D11_MAP_WRITE_NO_OVERWRITE || type == D3D11_MAP_READ_WRITE);
    if (!inside && !capture)
        Barrier(c);
    HRESULT hr = original_Map(c, resource, sub, type, flags, mapped);
    if (capture && SUCCEEDED(hr) && mapped)
    {
        CountMapCapture(0);
        if (fast)
        {
            // AI-generated comment: Shadows retain their resources and are never erased.
            // unordered_map rehash preserves these element addresses.
            shadow->mapped.store(mapped->pData, std::memory_order_release);
            lastMapped = {c, resource, shadow};
            CountMapCapture(1); CountMapCapture(3);
        }
        else
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            shadow = Shadow(resource);
            if (shadow) shadow->mapped.store(mapped->pData, std::memory_order_release);
        }
    }
    return hr;
}
static void STDMETHODCALLTYPE Hook_Unmap(Context *c, ID3D11Resource *resource, UINT sub)
{
    bool captured = false;
    if (!inside && sub == 0 && captureEnabled.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        BufferShadow *shadow = nullptr;
        if (UseFastMapCapture() && lastMapped.context == c && lastMapped.resource == resource && lastMapped.shadow)
        {
            shadow = lastMapped.shadow; lastMapped = {};
            CountMapCapture(2); CountMapCapture(3);
        }
        else
        {
            auto it = shadows.find(resource);
            if (it != shadows.end()) shadow = &it->second;
        }
        void *data = shadow ? shadow->mapped.load(std::memory_order_acquire) : nullptr;
        if (data)
        {
            auto &item = *shadow;
            std::memcpy(item.bytes.data(), data, item.bytes.size());
            item.valid.SetAll(fastBufferValidity);
            ++validityCounters[0];
            if (fastBufferValidity) validityCounters[3] += item.bytes.size();
            item.mapped.store(nullptr, std::memory_order_release);
            mirrored += item.bytes.size();
            captured = true;
        }
    }
    if (!inside && !captured)
        Barrier(c);
    original_Unmap(c, resource, sub);
}
static bool UpdateShadow(Context *c, ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data,
                         bool discard)
{
    if (inside || !resource || !data || sub != 0 || !IsConstant(c, resource))
        return false;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto *item = Shadow(resource);
    if (!item)
        return false;
    size_t lo = box ? box->left : 0, hi = box ? box->right : item->bytes.size();
    if (lo > hi || hi > item->bytes.size())
    {
        item->valid.Clear(fastBufferValidity);
        return false;
    }
    if (discard)
        item->valid.Clear(fastBufferValidity);
    std::memcpy(item->bytes.data() + lo, data, hi - lo);
    item->valid.SetRange(lo, hi, fastBufferValidity);
    mirrored += hi - lo;
    return true;
}
static void STDMETHODCALLTYPE Hook_UpdateSubresource(Context *c, ID3D11Resource *resource, UINT sub,
                                                     const D3D11_BOX *box, const void *data, UINT row, UINT depth)
{
    if (!UpdateShadow(c, resource, sub, box, data, false) && !inside)
    {
        Barrier(c);
        InvalidateBuffer(resource);
    }
    original_UpdateSubresource(c, resource, sub, box, data, row, depth);
}
static void STDMETHODCALLTYPE Hook_UpdateSubresource1(Context *c, ID3D11Resource *resource, UINT sub,
                                                      const D3D11_BOX *box, const void *data, UINT row, UINT depth,
                                                      UINT flags)
{
    if (!UpdateShadow(c, resource, sub, box, data, (flags & D3D11_COPY_DISCARD) != 0) && !inside)
    {
        Barrier(c);
        InvalidateBuffer(resource);
    }
    original_UpdateSubresource1(c, resource, sub, box, data, row, depth, flags);
}
static void STDMETHODCALLTYPE Hook_VSSetShader(Context *c, ID3D11VertexShader *shader,
                                               ID3D11ClassInstance *const *classes, UINT count)
{
    if (!inside && pending.load() && c == active.load() && (shader != runtime.vertex.Get() || count))
        Barrier(c);
    original_VSSetShader(c, shader, classes, count);
    if (!inside)
    {
        auto &b = Bound(c);
        b.vertex = shader;
        b.vsClasses = count;
        b.vsKnown = true;
    }
}
static void STDMETHODCALLTYPE Hook_PSSetShader(Context *c, ID3D11PixelShader *shader,
                                               ID3D11ClassInstance *const *classes, UINT count)
{
    if (!inside && pending.load() && c == active.load() && (!runtime.pixel || shader != runtime.pixel->source || count))
        Barrier(c);
    original_PSSetShader(c, shader, classes, count);
    if (!inside)
    {
        auto &b = Bound(c);
        b.pixel = shader;
        b.entry = KnownPixel(shader);
        b.psClasses = count;
        b.psKnown = true;
    }
}
static bool Snapshot(Context *c, InstancedLightBatch::Record &record)
{
    ID3D11Buffer *buffers[7]{};
    UINT first[7]{}, count[7]{};
    c->VSGetConstantBuffers1(0, 4, buffers, first, count);
    c->PSGetConstantBuffers1(0, 3, buffers + 4, first + 4, count + 4);
    bool ok = true;
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        // AI-generated comment: Record construction zeros padding once. Every successful snapshot
        // overwrites all 70 live vectors; failed snapshots are never submitted.
        for (unsigned i = 0; i < 7; ++i)
        {
            auto *item = buffers[i] ? Shadow(buffers[i]) : nullptr;
            size_t offset = size_t(first[i]) * 16, size = size_t(sizes[i]) * 16;
            if (item)
                item->wanted.store(true, std::memory_order_release);
            if (item && count[i] >= sizes[i] && offset + size <= item->bytes.size())
            {
                ++validityCounters[1];
                if (fastBufferValidity && item->valid.Full())
                {
                    ++validityCounters[2]; validityCounters[3] += size;
                }
            }
            if (!item || count[i] < sizes[i] || offset + size > item->bytes.size() ||
                !item->valid.Contains(offset, offset + size, fastBufferValidity))
            {
                ok = false;
                continue;
            }
            std::memcpy(record.vectors + starts[i], item->bytes.data() + offset, size);
        }
        captureEnabled.store(true, std::memory_order_release);
    }
    for (auto *b : buffers)
        if (b)
            b->Release();
    return ok;
}
struct SavedConstants
{
    Context *context;
    ID3D11Buffer *buffers[7]{};
    UINT first[7]{}, count[7]{};
    explicit SavedConstants(Context *c) : context(c)
    {
        c->VSGetConstantBuffers1(0, 4, buffers, first, count);
        c->PSGetConstantBuffers1(0, 3, buffers + 4, first + 4, count + 4);
    }
    ~SavedConstants()
    {
        context->VSSetConstantBuffers1(0, 4, buffers, first, count);
        context->PSSetConstantBuffers1(0, 3, buffers + 4, first + 4, count + 4);
        for (auto *b : buffers)
            if (b)
                b->Release();
    }
};
static void OriginalDraws()
{
    auto *c = runtime.context.Get();
    SavedConstants saved(c);
    c->VSSetShader(runtime.vertex.Get(), nullptr, 0);
    c->PSSetShader(runtime.pixel->source, nullptr, 0);
    for (unsigned j = 0; j < runtime.count; ++j)
    {
        for (unsigned i = 0; i < 7; ++i)
        {
            auto *b = runtime.pixel->originalCB[i].Get();
            c->UpdateSubresource(b, 0, nullptr, runtime.records[j].vectors + starts[i], 0, 0);
            if (i < 4)
                c->VSSetConstantBuffers(i, 1, &b);
            else
                c->PSSetConstantBuffers(i - 4, 1, &b);
        }
        original_DrawIndexed(c, runtime.indexCount, runtime.startIndex, runtime.baseVertex);
    }
}
#ifdef KOE_LIGHT_AUDIT
static unsigned BytesPerPixel(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return 4;
    default:
        return 0;
    }
}
static bool CopyTexture(ID3D11Texture2D *source, ComPtr<ID3D11Texture2D> &destination, bool staging)
{
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 || !BytesPerPixel(desc.Format))
        return false;
    desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    if (staging)
        desc.BindFlags = 0;
    desc.MiscFlags = 0;
    return SUCCEEDED(runtime.pixel->device->CreateTexture2D(&desc, nullptr, &destination));
}
static bool ReadBytes(ID3D11Texture2D *texture, std::vector<unsigned char> &bytes)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    unsigned row = desc.Width * BytesPerPixel(desc.Format);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    auto *c = runtime.context.Get();
    if (FAILED(c->Map(texture, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    bytes.resize(size_t(row) * desc.Height);
    for (unsigned y = 0; y < desc.Height; ++y)
        std::memcpy(bytes.data() + size_t(y) * row, (char *)mapped.pData + size_t(y) * mapped.RowPitch, row);
    c->Unmap(texture, 0);
    return true;
}
static long long Difference(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b)
{
    if (a.size() != b.size())
        return -1;
    long long n = 0;
    for (size_t i = 0; i < a.size(); ++i)
        n += a[i] != b[i];
    return n;
}
// AI-generated comment: Returns false only before submitting either branch, so the caller can fall back.
static bool AuditBatch(bool directOriginal = false)
{
    auditInfo[0] = 1;
    auto *c = runtime.context.Get();
    ID3D11RenderTargetView *targets[8]{};
    ComPtr<ID3D11DepthStencilView> depthView;
    c->OMGetRenderTargets(8, targets, &depthView);
    ComPtr<ID3D11RenderTargetView> target;
    target.Attach(targets[0]);
    bool single = target != nullptr;
    unsigned targetCount = target ? 1 : 0;
    for (unsigned i = 1; i < 8; ++i)
        if (targets[i])
        {
            single = false;
            targetCount++;
            targets[i]->Release();
        }
    auditInfo[6] = targetCount;
    if (!single)
        return false;
    auditInfo[0] = 2;
    ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    ComPtr<ID3D11Texture2D> actual;
    if (FAILED(resource.As(&actual)))
        return false;
    D3D11_TEXTURE2D_DESC td{};
    actual->GetDesc(&td);
    auditInfo[1] = td.Format;
    auditInfo[2] = td.Width;
    auditInfo[3] = td.Height;
    auditInfo[4] = td.SampleDesc.Count;
    auditInfo[5] = td.MipLevels;
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    target->GetDesc(&vd);
    auditInfo[8] = vd.ViewDimension;
    auditInfo[0] = 3;
    if (vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0)
        return false;
    auditInfo[0] = 4;
    ComPtr<ID3D11Texture2D> base, readBase, readCandidate, readBefore;
    if (!CopyTexture(actual.Get(), base, false) || !CopyTexture(actual.Get(), readBase, true) ||
        !CopyTexture(actual.Get(), readCandidate, true) || !CopyTexture(actual.Get(), readBefore, true))
        return false;
    auditInfo[0] = 5;
    ComPtr<ID3D11RenderTargetView> baseView;
    if (FAILED(runtime.pixel->device->CreateRenderTargetView(base.Get(), &vd, &baseView)))
        return false;
    ComPtr<ID3D11Texture2D> actualDepth, baseDepth, readDepthA, readDepthB;
    ComPtr<ID3D11DepthStencilView> baseDepthView;
    if (depthView)
    {
        auditInfo[0] = 6;
        depthView->GetResource(&resource);
        if (FAILED(resource.As(&actualDepth)))
            return false;
        D3D11_TEXTURE2D_DESC depthDesc{};
        actualDepth->GetDesc(&depthDesc);
        auditInfo[7] = depthDesc.Format;
        auditInfo[0] = 7;
        if (!CopyTexture(actualDepth.Get(), baseDepth, false) || !CopyTexture(actualDepth.Get(), readDepthA, true) ||
            !CopyTexture(actualDepth.Get(), readDepthB, true))
            return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        depthView->GetDesc(&dd);
        auditInfo[0] = 8;
        if (FAILED(runtime.pixel->device->CreateDepthStencilView(baseDepth.Get(), &dd, &baseDepthView)))
            return false;
        c->CopyResource(baseDepth.Get(), actualDepth.Get());
    }
    c->CopyResource(base.Get(), actual.Get());
    c->CopyResource(readBefore.Get(), actual.Get());
    auto *b = baseView.Get();
    c->OMSetRenderTargets(1, &b, baseDepthView.Get());
    if (directOriginal)
        original_DrawIndexed(c, runtime.indexCount, runtime.startIndex, runtime.baseVertex);
    else
        OriginalDraws();
    auto *a = target.Get();
    c->OMSetRenderTargets(1, &a, depthView.Get());
    HRESULT hr = runtime.pixel->renderer.Draw(c, runtime.records.data(), runtime.count, runtime.indexCount,
                                              runtime.startIndex, runtime.baseVertex);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    c->CopyResource(readBase.Get(), base.Get());
    c->CopyResource(readCandidate.Get(), actual.Get());
    if (actualDepth)
    {
        c->CopyResource(readDepthA.Get(), baseDepth.Get());
        c->CopyResource(readDepthB.Get(), actualDepth.Get());
    }
    std::vector<unsigned char> x, y, z;
    bool read = ReadBytes(readBase.Get(), x) && ReadBytes(readCandidate.Get(), y) && ReadBytes(readBefore.Get(), z);
    long long diff = read ? Difference(x, y) : -1, delta = read ? Difference(x, z) : 0;
    if (actualDepth)
    {
        bool ok = ReadBytes(readDepthA.Get(), x) && ReadBytes(readDepthB.Get(), y);
        long long d = ok ? Difference(x, y) : -1;
        if (d < 0)
            diff = -1;
        else if (diff >= 0)
            diff += d;
    }
    if (FAILED(hr) || diff != 0)
    {
        c->CopyResource(actual.Get(), base.Get());
        if (actualDepth)
            c->CopyResource(actualDepth.Get(), baseDepth.Get());
        mode = 0;
        errors++;
        if (diff > 0)
            different += diff;
    }
    else if (delta > 0)
    {
        if (directOriginal)
        {
            directSamples++;
            directLit += delta;
        }
        else
        {
            samples++;
            changed += delta;
        }
    }
    else
        emptySamples++;
    auditInfo[0] = 0;
    c->OMSetRenderTargets(1, &a, depthView.Get());
    return true;
}
#endif
static void Flush()
{
    if (!runtime.count)
        return;
    Guard guard;
    unsigned count = runtime.count;
    if (mode.load() == 0 || !sceneAllowed.load())
    {
        OriginalDraws();
        fallbacks += count;
    }
#ifdef KOE_LIGHT_AUDIT
    else if (mode.load() == 2 && samples.load() < 8 && count > 1 && (auditAttempts++ % 64 == 0))
    {
        if (!AuditBatch())
        {
            OriginalDraws();
            fallbacks += count;
        }
        else
        {
            batches++;
            sources += count;
        }
    }
#endif
    else
    {
        HRESULT hr = runtime.pixel->renderer.Draw(runtime.context.Get(), runtime.records.data(), count,
                                                  runtime.indexCount, runtime.startIndex, runtime.baseVertex);
        if (FAILED(hr))
        {
            OriginalDraws();
            fallbacks += count;
            errors++;
            mode = 0;
        }
        else
        {
            batches++;
            sources += count;
        }
    }
    long long old = maxBatch.load();
    while (count > old && !maxBatch.compare_exchange_weak(old, count))
    {
    }
    runtime.count = 0;
    pending = 0;
}
static void STDMETHODCALLTYPE Hook_DrawIndexed(Context *c, UINT count, UINT start, INT base)
{
    if (inside)
    {
        original_DrawIndexed(c, count, start, base);
        return;
    }
    draws++;
    if (!mode.load() || !sceneAllowed.load() || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        Barrier(c);
        original_DrawIndexed(c, count, start, base);
        return;
    }
    if (active.load() && (c != active.load() || GetCurrentThreadId() != runtime.owner))
    {
        original_DrawIndexed(c, count, start, base);
        return;
    }
    auto &bound = Bound(c);
    if (!bound.psKnown)
    {
        ComPtr<ID3D11PixelShader> p;
        UINT classes = 0;
        c->PSGetShader(&p, nullptr, &classes);
        bound.pixel = p.Get();
        bound.entry = KnownPixel(p.Get());
        bound.psClasses = classes;
        bound.psKnown = true;
    }
    if (!bound.entry || bound.psClasses)
    {
        if (pending.load())
            Flush();
        original_DrawIndexed(c, count, start, base);
        return;
    }
    if (!bound.vsKnown)
    {
        ComPtr<ID3D11VertexShader> v;
        UINT classes = 0;
        c->VSGetShader(&v, nullptr, &classes);
        bound.vertex = v.Get();
        bound.vsClasses = classes;
        bound.vsKnown = true;
    }
    Guard guard;
    ComPtr<ID3D11VertexShader> vs = bound.vertex;
    auto *entry = bound.entry;
    if (!KnownVertex(vs.Get()) || bound.vsClasses)
    {
        Flush();
        original_DrawIndexed(c, count, start, base);
        return;
    }
    if (!active.load())
    {
        ComPtr<Context> full;
        if (FAILED(c->QueryInterface(IID_PPV_ARGS(&full))))
        {
            original_DrawIndexed(c, count, start, base);
            return;
        }
        runtime.context = full;
        runtime.owner = GetCurrentThreadId();
        active.store(c, std::memory_order_release);
    }
    if (c != active.load() || GetCurrentThreadId() != runtime.owner)
    {
        original_DrawIndexed(c, count, start, base);
        return;
    }
    if (runtime.count && (runtime.pixel != entry || runtime.vertex.Get() != vs.Get() || runtime.indexCount != count ||
                          runtime.startIndex != start || runtime.baseVertex != base || runtime.count == 32))
        Flush();
    auto &record = runtime.records[runtime.count];
    if (!Snapshot(runtime.context.Get(), record))
    {
        Flush();
        missingConstants++;
        original_DrawIndexed(c, count, start, base);
        return;
    }
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11Predicate> predicate;
    BOOL pred = FALSE;
    c->GSGetShader(&gs, nullptr, nullptr);
    c->HSGetShader(&hs, nullptr, nullptr);
    c->DSGetShader(&ds, nullptr, nullptr);
    c->GetPredication(&predicate, &pred);
    if (gs || hs || ds || predicate)
    {
        Flush();
        original_DrawIndexed(c, count, start, base);
        return;
    }
#ifdef KOE_LIGHT_AUDIT
    if (mode.load() == 2 && directSamples.load() < 4 && (directAttempts++ % 64 == 0))
    {
        Flush();
        runtime.pixel = entry;
        runtime.vertex = vs;
        runtime.indexCount = count;
        runtime.startIndex = start;
        runtime.baseVertex = base;
        runtime.records[0] = record;
        runtime.count = 1;
        bool handled = AuditBatch(true);
        runtime.count = 0;
        pending = 0;
        if (handled)
        {
            batches++;
            sources++;
            queued++;
        }
        else
        {
            original_DrawIndexed(c, count, start, base);
            fallbacks++;
        }
        return;
    }
#endif
    runtime.pixel = entry;
    runtime.vertex = vs;
    runtime.indexCount = count;
    runtime.startIndex = start;
    runtime.baseVertex = base;
    ++runtime.count;
    queued++;
    pending = runtime.count;
}
extern "C" __declspec(dllexport) void SetInstancedLightsScene(int value)
{
    sceneAllowed = value != 0;
}
extern "C" __declspec(dllexport) int ConfigureBlendFactorNormalization(int value)
{
    if(initialized.load(std::memory_order_acquire))return -1;
    normalizeBlendFactor.store(value!=0,std::memory_order_relaxed);return 0;
}
extern "C" __declspec(dllexport) int ReadBlendFactorNormalization(uint64_t* values,unsigned count)
{
    if(!values||count<3)return -1;
    values[0]=normalizeBlendFactor.load(std::memory_order_relaxed)?1:0;
    values[1]=normalizationChecks.load(std::memory_order_relaxed);
    values[2]=normalizedFactors.load(std::memory_order_relaxed);return 0;
}
extern "C" __declspec(dllexport) void SetInstancedLightsMode(int value)
{
    if (value < 0 || value > 1 || (value > 0 && errors.load() != 0))
        return;
    if (value > 0 && mode.load() == 0)
        InvalidateAllBuffers();
    mode = value;
}
#ifdef KOE_MAP_CAPTURE_AUDIT
extern "C" __declspec(dllexport) int SetMapCaptureMode(int value)
{
    if (value < 0 || value > 1) return -1;
    fastMapCapture.store(value != 0, std::memory_order_relaxed);
    return 0;
}
extern "C" __declspec(dllexport) int ReadMapCaptureCounters(uint64_t *output, int count)
{
    if (!output || count != 4) return -1;
    for (unsigned i = 0; i < 4; ++i) output[i] = mapCaptureCounters[i].load(std::memory_order_relaxed);
    return UseFastMapCapture() ? 1 : 0;
}
#endif
#ifdef KOE_CAPTURE_LOOKUP_AUDIT
extern "C" __declspec(dllexport) int SetCaptureLookupMode(int value)
{
    if (value < 0 || value > 1) return -1;
    fastCaptureLookup.store(value != 0, std::memory_order_relaxed);
    return 0;
}
extern "C" __declspec(dllexport) int ReadCaptureLookupCounters(uint64_t *output, int count)
{
    if (!output || count != 4) return -1;
    for (unsigned i = 0; i < 4; ++i) output[i] = captureLookupCounters[i].load(std::memory_order_relaxed);
    return UseCaptureLookup() ? 1 : 0;
}
#endif
extern "C" __declspec(dllexport) int MarkCaptureBufferWanted(ID3D11Resource *resource)
{
    if (!resource) return -1;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto *shadow = Shadow(resource);
    if (!shadow) return -1;
    shadow->wanted.store(true, std::memory_order_release);
    captureEnabled.store(true, std::memory_order_release);
    return 0;
}
#if defined(KOE_MAP_CAPTURE_AUDIT) || defined(KOE_CAPTURE_LOOKUP_AUDIT)
extern "C" __declspec(dllexport) size_t CapturedShadowCount()
{
    std::lock_guard<std::mutex> lock(bufferMutex);
    return shadows.size();
}
extern "C" __declspec(dllexport) int ReadLightSnapshot(ID3D11DeviceContext *context, void *output, size_t size)
{
    if (!context || !output || size != sizeof(InstancedLightBatch::Record)) return -1;
    ComPtr<Context> full;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&full)))) return -1;
    static thread_local InstancedLightBatch::Record record;
    if (!Snapshot(full.Get(), record)) return 1;
    std::memcpy(output, &record, sizeof(record));
    return 0;
}
#endif
extern "C" __declspec(dllexport) int ReadCapturedBuffer(ID3D11Resource *resource, void *output, size_t size)
{
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto it = shadows.find(resource);
    if (!output || it == shadows.end() || it->second.bytes.size() != size ||
        it->second.mapped.load() || !it->second.valid.Contains(0, size, fastBufferValidity)) return -1;
    std::memcpy(output, it->second.bytes.data(), size);
    return 0;
}
extern "C" __declspec(dllexport) int SetBufferValidityMode(int value)
{
    if (value < 0 || value > 1) return -1;
    std::lock_guard<std::mutex> lock(bufferMutex);
    if (fastBufferValidity != (value != 0))
    {
        fastBufferValidity = value != 0;
        for (auto &item : shadows) item.second.valid.Clear(fastBufferValidity);
    }
    return 0;
}
extern "C" __declspec(dllexport) int ReadBufferValidityCounters(uint64_t *output, int count)
{
    if (!output || count != 4) return -1;
    std::lock_guard<std::mutex> lock(bufferMutex);
    std::memcpy(output, validityCounters, sizeof(validityCounters));
    return fastBufferValidity ? 1 : 0;
}
extern "C" __declspec(dllexport) void ReadInstancedLights(long long *output, int count)
{
    if (!output || (count != 18 && count != 27 && count != 29))
        return;
    long long values[] = {1,
                          initialized.load(),
                          mode.load(),
                          vertexCount.load(),
                          pixelCount.load(),
                          draws.load(),
                          queued.load(),
                          batches.load(),
                          sources.load(),
                          fallbacks.load(),
                          mirrored.load(),
                          errors.load(),
                          samples.load(),
                          different.load(),
                          changed.load(),
                          maxBatch.load(),
                          emptySamples.load(),
                          missingConstants.load(),
                          auditInfo[0].load(),
                          auditInfo[1].load(),
                          auditInfo[2].load(),
                          auditInfo[3].load(),
                          auditInfo[4].load(),
                          auditInfo[5].load(),
                          auditInfo[6].load(),
                          auditInfo[7].load(),
                          auditInfo[8].load(),
                          directSamples.load(),
                          directLit.load()};
    std::memcpy(output, values, count * sizeof(long long));
}
static void __stdcall RenderEvent(int)
{
    if (GetCurrentThreadId() == runtime.owner)
        Flush();
}
extern "C" __declspec(dllexport) void *GetInstancedLightsRenderEvent()
{
    return reinterpret_cast<void *>(RenderEvent);
}
extern "C" __declspec(dllexport) const char *GetInstancedLightsFailedHook()
{
    return failedHook;
}
extern "C" __declspec(dllexport) int InitializeInstancedLights(const wchar_t *folder, int requestedMode)
{
    if (initialized.load())
        return -1;
    Guard guard;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return -2;
    for (auto &spec : shaderSpecs)
        if (!ReadShader(std::filesystem::path(folder) / spec.vsFile, spec.vs, spec.candidateVSHash) ||
            !ReadShader(std::filesystem::path(folder) / spec.psFile, spec.ps, spec.candidatePSHash))
            return -3;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> base;
    ComPtr<Context> context;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 &level, &base)) ||
        FAILED(base.As(&context)))
        return -4;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return -5;
    auto **vt = *reinterpret_cast<void ***>(context.Get());
    auto **dv = *reinterpret_cast<void ***>(device.Get());
    std::vector<void *> installed;
    auto add = [&](void *target, void *hook, void **original) {
        if (std::find(installed.begin(), installed.end(), target) != installed.end())
            return false;
        if (MH_CreateHook(target, hook, original) != MH_OK)
            return false;
        installed.push_back(target);
        return true;
    };
    bool ok = add(dv[12], reinterpret_cast<void *>(HookCreateVS), reinterpret_cast<void **>(&originalCreateVS)) &&
              add(dv[15], reinterpret_cast<void *>(HookCreatePS), reinterpret_cast<void **>(&originalCreatePS));
    for (auto &spec : contextHookSpecs)
        if (ok)
        {
            ok = add(vt[spec.slot], spec.replacement, spec.original);
            if (!ok)
                failedHook = spec.name;
        }
    if (!ok)
    {
        for (auto *target : installed)
            MH_RemoveHook(target);
        return -6;
    }
    for (auto *target : installed)
        if (MH_QueueEnableHook(target) != MH_OK)
            ok = false;
    if (!ok || MH_ApplyQueued() != MH_OK)
    {
        for (auto *target : installed)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        return -7;
    }
    mode = requestedMode;
    initialized = true;
    return 0;
}
// AI-generated comment: Hooks, source identities and COM objects live until process exit. No global
// MinHook shutdown, DLL unload or source light suppression is performed.
