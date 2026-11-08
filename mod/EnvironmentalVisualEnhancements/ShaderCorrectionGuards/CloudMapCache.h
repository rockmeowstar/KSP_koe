#pragma once
#include <dxgi1_4.h>
#include <condition_variable>
#include <deque>
#include <thread>
#include <tuple>
#include <d3d11_1.h>
#include "CloudHookChain.h"

extern "C" void *koe_compile_cloud_maps(const unsigned char *, size_t, const unsigned char **, size_t *);
extern "C" int koe_pair_bc4(const unsigned char *, const unsigned char *, size_t, unsigned char *, size_t);

namespace CloudMaps
{
using Context = ID3D11DeviceContext;
using CreateTexture = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                                   const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
using UpdateTexture = void(STDMETHODCALLTYPE *)(Context *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *,
                                                UINT, UINT);
using CopyTexture = void(STDMETHODCALLTYPE *)(Context *, ID3D11Resource *, ID3D11Resource *);
using CopyRegion = void(STDMETHODCALLTYPE *)(Context *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *,
                                             UINT, const D3D11_BOX *);
using MapTexture = HRESULT(STDMETHODCALLTYPE *)(Context *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                                                D3D11_MAPPED_SUBRESOURCE *);
using UnmapTexture = void(STDMETHODCALLTYPE *)(Context *, ID3D11Resource *, UINT);
using IndexedDraw = void(STDMETHODCALLTYPE *)(Context *, UINT, UINT, INT);
using UpdateTexture1 = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext1 *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                                 const void *, UINT, UINT, UINT);
using CopyRegion1 = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext1 *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                              ID3D11Resource *, UINT, const D3D11_BOX *, UINT);
using DiscardTexture = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext1 *, ID3D11Resource *);
using DiscardView = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext1 *, ID3D11View *);
using DiscardView1 = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext1 *, ID3D11View *, const D3D11_RECT *, UINT);

static CreateTexture originalTexture{};
static UpdateTexture originalUpdate{};
static CopyTexture originalCopy{};
static CopyRegion originalRegion{};
static MapTexture originalMap{};
static UnmapTexture originalUnmap{};
static IndexedDraw originalDraw{};
static UpdateTexture1 originalUpdate1{};
static CopyRegion1 originalRegion1{};
static DiscardTexture originalDiscard{};
static DiscardView originalDiscardView{};
static DiscardView1 originalDiscardView1{};
static std::atomic<bool> active{}, installed{};
static std::atomic<bool> drawEnabled{true};
static std::atomic<size_t> cpuBytes{}, gpuBytes{}, cpuLimit{}, gpuLimit{}, forcedCpu{}, forcedGpu{};
static std::atomic<long long> prepared{}, binds{}, fallbacks{}, shaderRejects{}, jobFailures{}, sources{};
static std::atomic<unsigned long long> serial{}, epoch{1};
static std::atomic<unsigned long long> programGeneration{};
static std::mutex cacheMutex;
static std::condition_variable wake;
static std::thread worker;
static bool stopping{};
static const GUID textureIdentity = {0x2e5c26ac, 0x4b74, 0x4e39, {0xaa, 0x7c, 0xa4, 0x45, 0xc7, 0xcb, 0xe8, 0x35}};
static const GUID shaderIdentity = {0x44dc718c, 0x3a65, 0x4fb8, {0xb1, 0x9b, 0x1f, 0xd2, 0x22, 0x63, 0x69, 0x01}};
static const GUID viewIdentity = {0x124c60c7, 0x9e60, 0x4f16, {0x92, 0x67, 0xe3, 0xc6, 0x3a, 0x97, 0x85, 0x40}};

static bool Reserve(std::atomic<size_t> &used, size_t limit, size_t bytes)
{
    size_t old = used.load();
    do
    {
        if (bytes > limit || old > limit - bytes)
            return false;
    } while (!used.compare_exchange_weak(old, old + bytes));
    return true;
}
struct Payload
{
    std::vector<unsigned char> bytes;
    size_t reserved{};
    ~Payload()
    {
        cpuBytes.fetch_sub(reserved);
    }
};
static std::shared_ptr<Payload> Allocate(size_t bytes, bool source = false)
{
    if (!Reserve(cpuBytes, cpuLimit.load() / (source ? 2 : 1), bytes))
        return {};
    std::shared_ptr<Payload> result;
    try
    {
        result = std::make_shared<Payload>();
        result->reserved = bytes;
        result->bytes.resize(bytes);
    }
    catch (...)
    {
        if (!result)
            cpuBytes.fetch_sub(bytes);
        return {};
    }
    return result;
}
struct Texture
{
    D3D11_TEXTURE2D_DESC desc{};
    unsigned long long id = ++serial, generation{};
    bool usedForPair{}, dynamic{};
    std::shared_ptr<Payload> payload;
    Texture()
    {
        sources++;
    }
    ~Texture()
    {
        sources--;
    }
};
struct Program
{
    ID3D11PixelShader *shader{};
    ~Program()
    {
        if (shader)
            shader->Release();
    }
};
template <class T> class Owner final : public IUnknown
{
    std::atomic<ULONG> references{1};

  public:
    const std::shared_ptr<T> state;
    explicit Owner(std::shared_ptr<T> value) : state(std::move(value)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = static_cast<IUnknown *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return references.fetch_add(1) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG left = references.fetch_sub(1) - 1;
        if (!left)
            delete this;
        return left;
    }
};
template <class T> static std::shared_ptr<T> State(ID3D11DeviceChild *object, const GUID &key)
{
    if (!object)
        return {};
    IUnknown *owner{};
    UINT size = sizeof(owner);
    if (FAILED(object->GetPrivateData(key, &size, &owner)) || !owner)
        return {};
    auto value = static_cast<Owner<T> *>(owner)->state;
    owner->Release();
    return value;
}
template <class T> static bool Attach(ID3D11DeviceChild *object, const GUID &key, std::shared_ptr<T> state)
{
    auto owner = new Owner<T>(std::move(state));
    HRESULT hr = object->SetPrivateDataInterface(key, owner);
    owner->Release();
    return SUCCEEDED(hr);
}
static std::vector<std::weak_ptr<Texture>> textures;
using Key = std::tuple<unsigned long long, unsigned long long, unsigned long long, unsigned long long>;
struct Pair
{
    std::weak_ptr<Texture> a, b;
    unsigned long long aGeneration{}, bGeneration{}, cacheEpoch{}, used{};
    std::shared_ptr<Payload> red, green;
    ID3D11Device *device{};
    ID3D11ShaderResourceView *view{};
    UINT width{}, height{};
    size_t reserved{};
    bool complete{};
    ~Pair()
    {
        if (view)
            view->Release();
        if (device)
            device->Release();
        gpuBytes.fetch_sub(reserved);
    }
};
static std::map<Key, std::shared_ptr<Pair>> pairs;
static std::deque<std::shared_ptr<Pair>> jobs;
struct Mapping
{
    const void *data{};
    UINT pitch{};
    std::weak_ptr<Texture> texture;
};
static std::map<std::pair<Context *, ID3D11Resource *>, Mapping> mappings;

static size_t PhysicalBudget()
{
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory))
        return 0;
    // AI-generated comment: Cache policy uses available memory and a small share of installed RAM, not model names.
    size_t limit = static_cast<size_t>(std::min(memory.ullAvailPhys / 16, memory.ullTotalPhys / 128));
    size_t requested = forcedCpu.load();
    return requested ? std::min(limit, requested) : limit;
}
static size_t VideoBudget(ID3D11Device *device)
{
    IDXGIDevice *dxgi{};
    IDXGIAdapter *adapter{};
    IDXGIAdapter3 *modern{};
    size_t limit = 0;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))))
    {
        if (SUCCEEDED(dxgi->GetAdapter(&adapter)))
        {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
                limit = static_cast<size_t>(desc.DedicatedVideoMemory / 32);
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&modern))))
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
                if (SUCCEEDED(modern->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
                {
                    UINT64 free = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
                    size_t available = static_cast<size_t>((free + gpuBytes.load()) / 16);
                    limit = limit ? std::min(limit, available) : available;
                }
                modern->Release();
            }
            adapter->Release();
        }
        dxgi->Release();
    }
    size_t requested = forcedGpu.load();
    return requested ? std::min(limit, requested) : limit;
}
static bool Valid(const std::shared_ptr<Pair> &pair)
{
    auto a = pair->a.lock(), b = pair->b.lock();
    return a && b && a->generation == pair->aGeneration && b->generation == pair->bGeneration &&
           pair->cacheEpoch == epoch.load();
}
static void TrimLocked(size_t target)
{
    for (auto it = pairs.begin(); it != pairs.end();)
        if (!Valid(it->second))
            it = pairs.erase(it);
        else
            ++it;
    while (gpuBytes.load() > target && !pairs.empty())
    {
        auto oldest = pairs.end();
        for (auto it = pairs.begin(); it != pairs.end(); ++it)
            if (it->second->complete && it->second->reserved &&
                (oldest == pairs.end() || it->second->used < oldest->second->used))
                oldest = it;
        if (oldest == pairs.end())
            break;
        pairs.erase(oldest);
    }
    textures.erase(std::remove_if(textures.begin(), textures.end(), [](const auto &value) { return value.expired(); }),
                   textures.end());
}
static void Prepare(const std::shared_ptr<Pair> &pair)
{
    size_t bytes = pair->red->bytes.size() * 2;
    auto packed = Allocate(bytes);
    if (!packed || koe_pair_bc4(pair->red->bytes.data(), pair->green->bytes.data(), pair->red->bytes.size(),
                                packed->bytes.data(), packed->bytes.size()))
        return;
    size_t limit = VideoBudget(pair->device);
    gpuLimit.store(limit);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (!active.load() || !Valid(pair))
            return;
        TrimLocked(limit);
        if (!Reserve(gpuBytes, limit, bytes))
            return;
        pair->reserved = bytes;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = pair->width;
    desc.Height = pair->height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_BC5_UNORM;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{packed->bytes.data(), ((desc.Width + 3) / 4) * 16, static_cast<UINT>(bytes)};
    ID3D11Texture2D *texture{};
    ID3D11ShaderResourceView *view{};
    HRESULT hr = originalTexture(pair->device, &desc, &data, &texture);
    if (SUCCEEDED(hr))
    {
        hr = pair->device->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (SUCCEEDED(hr) && active.load() && Valid(pair))
    {
        pair->view = view;
        prepared++;
    }
    else if (view)
        view->Release();
}
static void Work()
{
    for (;;)
    {
        std::shared_ptr<Pair> job;
        {
            std::unique_lock<std::mutex> lock(cacheMutex);
            wake.wait_for(lock, std::chrono::seconds(1), [] { return stopping || !jobs.empty(); });
            if (stopping)
                return;
            cpuLimit.store(PhysicalBudget());
            TrimLocked(gpuLimit.load());
            if (cpuBytes.load() > cpuLimit.load())
                for (auto &weak : textures)
                {
                    if (auto texture = weak.lock())
                        texture->payload.reset();
                    if (cpuBytes.load() <= cpuLimit.load())
                        break;
                }
            if (jobs.empty())
                continue;
            job = jobs.front();
            jobs.pop_front();
            if (!Valid(job))
            {
                job->red.reset();
                job->green.reset();
                continue;
            }
        }
        try
        {
            Prepare(job);
        }
        catch (...)
        {
            jobFailures++;
        }
        std::shared_ptr<Payload> retiredRed, retiredGreen;
        ID3D11Device *retiredDevice{};
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            retiredRed = std::move(job->red);
            retiredGreen = std::move(job->green);
            job->complete = true;
            retiredDevice = job->device;
            job->device = nullptr;
            if (!job->view)
                jobFailures++;
            if (!job->view && job->reserved)
            {
                gpuBytes.fetch_sub(job->reserved);
                job->reserved = 0;
            }
        }
        // AI-generated comment: Release completed-job resources after unlocking the render cache.
        if (retiredDevice)
            retiredDevice->Release();
    }
}
static std::shared_ptr<Texture> TextureFor(ID3D11Resource *resource)
{
    if (!resource)
        return {};
    D3D11_RESOURCE_DIMENSION type{};
    resource->GetType(&type);
    return type == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? State<Texture>(resource, textureIdentity)
                                                      : std::shared_ptr<Texture>{};
}
static void Invalidate(ID3D11Resource *resource)
{
    if (!active.load())
        return;
    auto texture = TextureFor(resource);
    if (!texture)
        return;
    std::lock_guard<std::mutex> lock(cacheMutex);
    texture->generation++;
    texture->payload.reset();
    if (texture->usedForPair)
        texture->dynamic = true;
}
static std::shared_ptr<Payload> Read(const std::shared_ptr<Texture> &texture, const void *data, UINT pitch)
{
    if (!texture || !data)
        return {};
    size_t row = ((size_t(texture->desc.Width) + 3) / 4) * 8, rows = (size_t(texture->desc.Height) + 3) / 4;
    if (pitch < row)
        return {};
    auto payload = Allocate(row * rows, true);
    if (!payload)
        return {};
    for (size_t y = 0; y < rows; y++)
        std::memcpy(payload->bytes.data() + y * row, static_cast<const char *>(data) + y * pitch, row);
    return payload;
}
static bool Full(const std::shared_ptr<Texture> &texture, const D3D11_BOX *box)
{
    return texture && (!box || (!box->left && !box->top && !box->front && box->right == texture->desc.Width &&
                                box->bottom == texture->desc.Height && box->back == 1));
}
static bool SameUpload(Context *context, ID3D11Resource *resource, UINT subresource,
                       const D3D11_BOX *box, const void *data, UINT pitch)
{
    if (!active.load() || subresource || !data || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    auto texture = TextureFor(resource);
    if (!Full(texture, box))
        return false;
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (texture->dynamic || !texture->payload)
        return false;
    size_t row = ((size_t(texture->desc.Width) + 3) / 4) * 8;
    if (pitch < row)
        return false;
    size_t rows = (size_t(texture->desc.Height) + 3) / 4;
    for (size_t y = 0; y < rows; ++y)
        if (std::memcmp(texture->payload->bytes.data() + y * row, static_cast<const char *>(data) + y * pitch, row))
            return false;
    return true;
}
static bool SameCopy(Context *context, ID3D11Resource *destination, ID3D11Resource *source,
                     UINT subresource, UINT x, UINT y, UINT z, UINT sourceSubresource, const D3D11_BOX *box)
{
    if (!active.load() || subresource || sourceSubresource || x || y || z ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    auto a = TextureFor(source), b = TextureFor(destination);
    if (!Full(a, box) || !b || a->desc.Width != b->desc.Width || a->desc.Height != b->desc.Height)
        return false;
    std::lock_guard<std::mutex> lock(cacheMutex);
    return !a->dynamic && !b->dynamic && a->payload && b->payload &&
           (a->payload == b->payload || a->payload->bytes == b->payload->bytes);
}
static void Capture(Context *context, ID3D11Resource *resource, UINT subresource, const D3D11_BOX *box,
                    const void *data, UINT pitch)
{
    if (!active.load() || subresource || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return;
    auto texture = TextureFor(resource);
    if (!Full(texture, box))
        return;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (texture->dynamic)
            return;
    }
    auto payload = Read(texture, data, pitch);
    std::lock_guard<std::mutex> lock(cacheMutex);
    texture->payload = std::move(payload);
}
static HRESULT STDMETHODCALLTYPE CreateTextureHook(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *desc,
                                                   const D3D11_SUBRESOURCE_DATA *data, ID3D11Texture2D **out)
{
    HRESULT hr = originalTexture(device, desc, data, out);
    if (FAILED(hr) || !active.load() || !desc || !out || !*out ||
        (desc->Format != DXGI_FORMAT_BC4_UNORM && desc->Format != DXGI_FORMAT_BC4_TYPELESS) ||
        desc->MipLevels != 1 || desc->ArraySize != 1 || desc->SampleDesc.Count != 1 || desc->MiscFlags ||
        (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ||
        (desc->Usage == D3D11_USAGE_DEFAULT && desc->CPUAccessFlags))
        return hr;
    try
    {
        auto texture = std::make_shared<Texture>();
        texture->desc = *desc;
        if (data)
            texture->payload = Read(texture, data->pSysMem, data->SysMemPitch);
        if (Attach(*out, textureIdentity, texture))
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            textures.push_back(texture);
        }
    }
    catch (...)
    {
        jobFailures++;
    }
    return hr;
}
static void STDMETHODCALLTYPE UpdateBody(Context *context, ID3D11Resource *resource, UINT subresource,
                                         const D3D11_BOX *box, const void *data, UINT row, UINT depth)
{
    if (!active.load() || !TextureFor(resource))
    {
        originalUpdate(context, resource, subresource, box, data, row, depth);
        return;
    }
    if (SameUpload(context, resource, subresource, box, data, row))
    {
        originalUpdate(context, resource, subresource, box, data, row, depth);
        return;
    }
    Invalidate(resource);
    originalUpdate(context, resource, subresource, box, data, row, depth);
    try
    {
        Capture(context, resource, subresource, box, data, row);
    }
    catch (...)
    {
        jobFailures++;
    }
}
static void CopyPayload(Context *context, ID3D11Resource *destination, ID3D11Resource *source, UINT subresource, UINT x,
                        UINT y, UINT z, UINT sourceSubresource, const D3D11_BOX *box)
{
    if (!active.load() || subresource || sourceSubresource || x || y || z ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return;
    auto a = TextureFor(source), b = TextureFor(destination);
    if (!Full(a, box) || !b || a->desc.Width != b->desc.Width || a->desc.Height != b->desc.Height)
        return;
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (!b->dynamic)
        b->payload = a->payload;
}
static void STDMETHODCALLTYPE CopyBody(Context *context, ID3D11Resource *destination, ID3D11Resource *source)
{
    if (SameCopy(context, destination, source, 0, 0, 0, 0, 0, nullptr))
    {
        originalCopy(context, destination, source);
        return;
    }
    Invalidate(destination);
    originalCopy(context, destination, source);
    CopyPayload(context, destination, source, 0, 0, 0, 0, 0, nullptr);
}
static void STDMETHODCALLTYPE RegionBody(Context *context, ID3D11Resource *destination, UINT subresource, UINT x,
                                         UINT y, UINT z, ID3D11Resource *source, UINT sourceSubresource,
                                         const D3D11_BOX *box)
{
    if (SameCopy(context, destination, source, subresource, x, y, z, sourceSubresource, box))
    {
        originalRegion(context, destination, subresource, x, y, z, source, sourceSubresource, box);
        return;
    }
    Invalidate(destination);
    originalRegion(context, destination, subresource, x, y, z, source, sourceSubresource, box);
    CopyPayload(context, destination, source, subresource, x, y, z, sourceSubresource, box);
}
static HRESULT STDMETHODCALLTYPE MapBody(Context *context, ID3D11Resource *resource, UINT subresource, D3D11_MAP type,
                                         UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
    if (!active.load() || !TextureFor(resource))
        return originalMap(context, resource, subresource, type, flags, out);
    bool writing = type != D3D11_MAP_READ;
    if (writing)
        Invalidate(resource);
    HRESULT hr = originalMap(context, resource, subresource, type, flags, out);
    if (SUCCEEDED(hr) && writing && active.load() && !subresource && out &&
        context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        try
        {
            auto texture = TextureFor(resource);
            if (texture)
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                mappings[{context, resource}] = {out->pData, out->RowPitch, texture};
            }
        }
        catch (...)
        {
            jobFailures++;
        }
    }
    return hr;
}
static void STDMETHODCALLTYPE UnmapBody(Context *context, ID3D11Resource *resource, UINT subresource)
{
    if (!active.load() || !TextureFor(resource))
    {
        originalUnmap(context, resource, subresource);
        return;
    }
    Mapping mapping;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = mappings.find({context, resource});
        if (it != mappings.end())
        {
            mapping = it->second;
            mappings.erase(it);
            found = true;
        }
    }
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Payload> payload;
    try
    {
        if (found && !subresource)
        {
            texture = mapping.texture.lock();
            payload = Read(texture, mapping.data, mapping.pitch);
        }
    }
    catch (...)
    {
        jobFailures++;
    }
    originalUnmap(context, resource, subresource);
    if (texture)
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        texture->payload = std::move(payload);
    }
}
static void STDMETHODCALLTYPE UpdateBody1(ID3D11DeviceContext1 *context, ID3D11Resource *resource, UINT subresource,
                                          const D3D11_BOX *box, const void *data, UINT row, UINT depth, UINT flags)
{
    if (!active.load() || !TextureFor(resource))
    {
        originalUpdate1(context, resource, subresource, box, data, row, depth, flags);
        return;
    }
    if (SameUpload(context, resource, subresource, box, data, row))
    {
        originalUpdate1(context, resource, subresource, box, data, row, depth, flags);
        return;
    }
    Invalidate(resource);
    originalUpdate1(context, resource, subresource, box, data, row, depth, flags);
    try
    {
        Capture(context, resource, subresource, box, data, row);
    }
    catch (...)
    {
        jobFailures++;
    }
}
static void STDMETHODCALLTYPE RegionBody1(ID3D11DeviceContext1 *context, ID3D11Resource *destination, UINT subresource,
                                          UINT x, UINT y, UINT z, ID3D11Resource *source, UINT sourceSubresource,
                                          const D3D11_BOX *box, UINT flags)
{
    if (SameCopy(context, destination, source, subresource, x, y, z, sourceSubresource, box))
    {
        originalRegion1(context, destination, subresource, x, y, z, source, sourceSubresource, box, flags);
        return;
    }
    Invalidate(destination);
    originalRegion1(context, destination, subresource, x, y, z, source, sourceSubresource, box, flags);
    CopyPayload(context, destination, source, subresource, x, y, z, sourceSubresource, box);
}
static void STDMETHODCALLTYPE DiscardBody(ID3D11DeviceContext1 *context, ID3D11Resource *resource)
{
    Invalidate(resource);
    originalDiscard(context, resource);
}
static void InvalidateView(ID3D11View *view)
{
    if (!active.load() || !view)
        return;
    ID3D11Resource *resource{};
    view->GetResource(&resource);
    Invalidate(resource);
    resource->Release();
}
static void STDMETHODCALLTYPE DiscardViewBody(ID3D11DeviceContext1 *context, ID3D11View *view)
{
    InvalidateView(view);
    originalDiscardView(context, view);
}
static void STDMETHODCALLTYPE DiscardViewBody1(ID3D11DeviceContext1 *context, ID3D11View *view,
                                               const D3D11_RECT *rectangles, UINT count)
{
    InvalidateView(view);
    originalDiscardView1(context, view, rectangles, count);
}

static std::shared_ptr<Texture> ViewTexture(ID3D11ShaderResourceView *view)
{
    if (!view)
        return {};
    if (auto texture = State<Texture>(view, viewIdentity))
        return texture;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    view->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_BC4_UNORM || desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        desc.Texture2D.MostDetailedMip || desc.Texture2D.MipLevels != 1)
        return {};
    ID3D11Resource *resource{};
    view->GetResource(&resource);
    auto texture = TextureFor(resource);
    resource->Release();
    // AI-generated comment: The view retains its resource; updates mutate this same Texture state.
    if (texture)
        Attach(view, viewIdentity, texture);
    return texture;
}
static std::shared_ptr<Pair> PairFor(Context *context, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b)
{
    if (a == b)
        return {};
    auto red = ViewTexture(a), green = ViewTexture(b);
    if (!red || !green || red == green || red->desc.Width != green->desc.Width ||
        red->desc.Height != green->desc.Height)
        return {};
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (red->dynamic || green->dynamic)
        return {};
    Key key{red->id, red->generation, green->id, green->generation};
    auto found = pairs.find(key);
    if (found != pairs.end())
    {
        found->second->used = ++serial;
        return found->second->view ? found->second : std::shared_ptr<Pair>{};
    }
    if (!red->payload || !green->payload)
        return {};
    red->usedForPair = green->usedForPair = true;
    if (jobs.size() >= std::max(1u, std::thread::hardware_concurrency()))
        return {};
    auto pair = std::make_shared<Pair>();
    pair->a = red;
    pair->b = green;
    pair->aGeneration = red->generation;
    pair->bGeneration = green->generation;
    pair->cacheEpoch = epoch.load();
    pair->used = ++serial;
    pair->red = red->payload;
    pair->green = green->payload;
    pair->width = red->desc.Width;
    pair->height = red->desc.Height;
    context->GetDevice(&pair->device);
    pairs.emplace(key, pair);
    jobs.push_back(pair);
    wake.notify_one();
    return {};
}
struct Binding
{
    Context *context{};
    ID3D11PixelShader *source{};
    std::weak_ptr<Program> program;
    unsigned long long epoch{};
};
static thread_local Binding binding;
static void Remember(Context *context, ID3D11PixelShader *shader, UINT instances)
{
    binding = {};
    if (!active.load() || !shader || instances || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return;
    struct Lookup
    {
        ID3D11PixelShader *source{};
        std::weak_ptr<Program> program;
        unsigned long long generation{};
        bool positive{};
    };
    static thread_local std::array<Lookup, 64> lookup;
    auto generation = programGeneration.load(std::memory_order_acquire);
    auto &entry = lookup[(reinterpret_cast<uintptr_t>(shader) >> 4) % lookup.size()];
    if (entry.source != shader || entry.generation != generation || (entry.positive && entry.program.expired()))
    {
        auto program = State<Program>(shader, shaderIdentity);
        entry = {shader, program, generation, static_cast<bool>(program)};
    }
    binding = {context, shader, entry.program, epoch.load()};
}
static void STDMETHODCALLTYPE DrawBody(Context *context, UINT count, UINT start, INT base)
{
    if (!active.load() || !drawEnabled.load() || binding.context != context || binding.epoch != epoch.load())
    {
        originalDraw(context, count, start, base);
        return;
    }
    auto program = binding.program.lock();
    if (!program)
    {
        originalDraw(context, count, start, base);
        return;
    }
    ID3D11PixelShader *source{};
    context->PSGetShader(&source, nullptr, nullptr);
    if (source != binding.source)
    {
        if (source)
            source->Release();
        binding = {};
        originalDraw(context, count, start, base);
        return;
    }
    ID3D11ShaderResourceView *views[2]{};
    ID3D11SamplerState *samplers[2]{};
    context->PSGetShaderResources(0, 2, views);
    context->PSGetSamplers(0, 2, samplers);
    std::shared_ptr<Pair> pair;
    if (samplers[0] && samplers[0] == samplers[1])
    {
        D3D11_SAMPLER_DESC desc{};
        samplers[0]->GetDesc(&desc);
        if (!(static_cast<unsigned>(desc.Filter) & 0x180) && desc.AddressU != D3D11_TEXTURE_ADDRESS_BORDER &&
            desc.AddressV != D3D11_TEXTURE_ADDRESS_BORDER)
            try
            {
                pair = PairFor(context, views[0], views[1]);
            }
            catch (...)
            {
                jobFailures++;
            }
    }
    if (pair)
    {
        originalSet(context, program->shader, nullptr, 0);
        context->PSSetShaderResources(0, 1, &pair->view);
        originalDraw(context, count, start, base);
        context->PSSetShaderResources(0, 1, views);
        originalSet(context, source, nullptr, 0);
        binds++;
    }
    else
    {
        fallbacks++;
        originalDraw(context, count, start, base);
    }
    for (auto *view : views)
        if (view)
            view->Release();
    for (auto *sampler : samplers)
        if (sampler)
            sampler->Release();
    source->Release();
}
static void BuildProgram(ID3D11Device *device, const void *bytes, SIZE_T length, ID3D11ClassLinkage *linkage,
                         ID3D11PixelShader *source, bool recognized = false)
{
    if (!active.load() || !source || linkage || !bytes || length < 32 || length > UINT_MAX ||
        State<Program>(source, shaderIdentity))
        return;
    if (!recognized)
    {
        bool exponent = false, mantissa = false;
        for (size_t offset = 0; offset + 4 <= length; offset += 4)
        {
            uint32_t word{};
            std::memcpy(&word, static_cast<const unsigned char *>(bytes) + offset, 4);
            exponent |= word == 0x7c00;
            mantissa |= word == 1023;
        }
        if (!exponent || !mantissa)
            return;
    }
    try
    {
        const unsigned char *code{};
        size_t size{};
        std::unique_ptr<void, decltype(&koe_free_cloud_half)> compiled(
            koe_compile_cloud_maps(static_cast<const unsigned char *>(bytes), length, &code, &size),
            koe_free_cloud_half);
        if (!compiled)
            return;
        std::vector<unsigned char> fixed(code, code + size);
        ComputeHashRetail(fixed.data() + 20, static_cast<UINT32>(size - 20), fixed.data() + 4);
        auto program = std::make_shared<Program>();
        if (SUCCEEDED(originalCreate(device, fixed.data(), fixed.size(), nullptr, &program->shader)) &&
            program->shader != source)
        {
            if (Attach(source, shaderIdentity, program))
                programGeneration.fetch_add(1, std::memory_order_release);
            else
                shaderRejects++;
        }
        else
            shaderRejects++;
    }
    catch (...)
    {
        shaderRejects++;
    }
}
static void Clear(bool releaseSources)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    epoch++;
    pairs.clear();
    jobs.clear();
    mappings.clear();
    if (releaseSources)
        for (auto &weak : textures)
            if (auto texture = weak.lock())
            {
                texture->generation++;
                texture->payload.reset();
                texture->usedForPair = texture->dynamic = false;
            }
    TrimLocked(gpuLimit.load());
    wake.notify_one();
}
static CloudHookChain::Endpoint updateChain, copyChain, regionChain, mapChain, unmapChain, drawChain,
    regionChain1, updateChain1, discardChain, discardViewChain, discardViewChain1;
static void STDMETHODCALLTYPE UpdateHook(Context *c, ID3D11Resource *r, UINT s, const D3D11_BOX *b, const void *d, UINT row, UINT depth)
{ CloudHookChain::Invoke(updateChain, UpdateBody, c, r, s, b, d, row, depth); }
static void STDMETHODCALLTYPE CopyHook(Context *c, ID3D11Resource *d, ID3D11Resource *s)
{ CloudHookChain::Invoke(copyChain, CopyBody, c, d, s); }
static void STDMETHODCALLTYPE RegionHook(Context *c, ID3D11Resource *d, UINT sub, UINT x, UINT y, UINT z, ID3D11Resource *s, UINT ss, const D3D11_BOX *b)
{ CloudHookChain::Invoke(regionChain, RegionBody, c, d, sub, x, y, z, s, ss, b); }
static HRESULT STDMETHODCALLTYPE MapHook(Context *c, ID3D11Resource *r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{ return CloudHookChain::Invoke(mapChain, MapBody, c, r, sub, type, flags, out); }
static void STDMETHODCALLTYPE UnmapHook(Context *c, ID3D11Resource *r, UINT sub)
{ CloudHookChain::Invoke(unmapChain, UnmapBody, c, r, sub); }
static void STDMETHODCALLTYPE DrawHook(Context *c, UINT count, UINT start, INT base)
{ CloudHookChain::Invoke(drawChain, DrawBody, c, count, start, base); }
static void STDMETHODCALLTYPE UpdateHook1(ID3D11DeviceContext1 *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *b, const void *d, UINT row, UINT depth, UINT flags)
{ CloudHookChain::Invoke(updateChain1, UpdateBody1, c, r, sub, b, d, row, depth, flags); }
static void STDMETHODCALLTYPE RegionHook1(ID3D11DeviceContext1 *c, ID3D11Resource *d, UINT sub, UINT x, UINT y, UINT z, ID3D11Resource *s, UINT ss, const D3D11_BOX *b, UINT flags)
{ CloudHookChain::Invoke(regionChain1, RegionBody1, c, d, sub, x, y, z, s, ss, b, flags); }
static void STDMETHODCALLTYPE DiscardHook(ID3D11DeviceContext1 *c, ID3D11Resource *r)
{ CloudHookChain::Invoke(discardChain, DiscardBody, c, r); }
static void STDMETHODCALLTYPE DiscardViewHook(ID3D11DeviceContext1 *c, ID3D11View *v)
{ CloudHookChain::Invoke(discardViewChain, DiscardViewBody, c, v); }
static void STDMETHODCALLTYPE DiscardViewHook1(ID3D11DeviceContext1 *c, ID3D11View *v, const D3D11_RECT *r, UINT n)
{ CloudHookChain::Invoke(discardViewChain1, DiscardViewBody1, c, v, r, n); }

static std::vector<void *> hookTargets;
static bool Hook(void *target, void *replacement, void **original,
                 CloudHookChain::Endpoint *endpoint = nullptr, void *body = nullptr)
{
    if (std::find(hookTargets.begin(), hookTargets.end(), target) != hookTargets.end())
        return false;
    if (MH_CreateHook(target, replacement, original) != MH_OK)
        return false;
    if (endpoint && !CloudHookChain::registry.Add(*endpoint, target, body))
    {
        MH_RemoveHook(target);
        return false;
    }
    try { hookTargets.push_back(target); }
    catch (...)
    {
        if (endpoint) CloudHookChain::registry.Remove(target);
        MH_RemoveHook(target);
        return false;
    }
    return true;
}
static bool Initialize(ID3D11Device *device, Context *context)
{
    if (!active.load())
        return true;
    cpuLimit.store(PhysicalBudget());
    gpuLimit.store(VideoBudget(device));
    auto dv = *reinterpret_cast<void ***>(device), cv = *reinterpret_cast<void ***>(context);
    bool ok = Hook(dv[5], reinterpret_cast<void *>(CreateTextureHook), reinterpret_cast<void **>(&originalTexture)) &&
              Hook(cv[48], reinterpret_cast<void *>(UpdateHook), reinterpret_cast<void **>(&originalUpdate), &updateChain, reinterpret_cast<void *>(UpdateBody)) &&
              Hook(cv[47], reinterpret_cast<void *>(CopyHook), reinterpret_cast<void **>(&originalCopy), &copyChain, reinterpret_cast<void *>(CopyBody)) &&
              Hook(cv[46], reinterpret_cast<void *>(RegionHook), reinterpret_cast<void **>(&originalRegion), &regionChain, reinterpret_cast<void *>(RegionBody)) &&
              Hook(cv[14], reinterpret_cast<void *>(MapHook), reinterpret_cast<void **>(&originalMap), &mapChain, reinterpret_cast<void *>(MapBody)) &&
              Hook(cv[15], reinterpret_cast<void *>(UnmapHook), reinterpret_cast<void **>(&originalUnmap), &unmapChain, reinterpret_cast<void *>(UnmapBody)) &&
              Hook(cv[12], reinterpret_cast<void *>(DrawHook), reinterpret_cast<void **>(&originalDraw), &drawChain, reinterpret_cast<void *>(DrawBody));
    ID3D11DeviceContext1 *modern{};
    if (ok && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&modern))))
    {
        auto v = *reinterpret_cast<void ***>(modern);
        ok = Hook(v[115], reinterpret_cast<void *>(RegionHook1), reinterpret_cast<void **>(&originalRegion1), &regionChain1, reinterpret_cast<void *>(RegionBody1)) &&
             Hook(v[116], reinterpret_cast<void *>(UpdateHook1), reinterpret_cast<void **>(&originalUpdate1), &updateChain1, reinterpret_cast<void *>(UpdateBody1)) &&
             Hook(v[117], reinterpret_cast<void *>(DiscardHook), reinterpret_cast<void **>(&originalDiscard), &discardChain, reinterpret_cast<void *>(DiscardBody)) &&
             Hook(v[118], reinterpret_cast<void *>(DiscardViewHook), reinterpret_cast<void **>(&originalDiscardView), &discardViewChain, reinterpret_cast<void *>(DiscardViewBody)) &&
             Hook(v[133], reinterpret_cast<void *>(DiscardViewHook1), reinterpret_cast<void **>(&originalDiscardView1), &discardViewChain1, reinterpret_cast<void *>(DiscardViewBody1));
        modern->Release();
    }
    if (ok)
        for (auto target : hookTargets)
            if (MH_QueueEnableHook(target) != MH_OK)
            {
                ok = false;
                break;
            }
    if (ok)
        ok = MH_ApplyQueued() == MH_OK;
    if (!ok)
    {
        active = false;
        for (auto target : hookTargets)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
            CloudHookChain::registry.Remove(target);
        }
        hookTargets.clear();
        return false;
    }
    try
    {
        worker = std::thread(Work);
        installed = true;
    }
    catch (...)
    {
        active = false;
        return false;
    }
    return true;
}
}

extern "C" __declspec(dllexport) void SetCloudMapPackingEnabled(int value)
{
    CloudMaps::active.store(value != 0);
    if (!value)
        CloudMaps::Clear(true);
}
extern "C" __declspec(dllexport) void ClearCloudMapCache()
{
    CloudMaps::Clear(false);
}
extern "C" __declspec(dllexport) void SetCloudMapDrawEnabled(int value)
{
    CloudMaps::drawEnabled.store(value != 0);
}
extern "C" __declspec(dllexport) void SetCloudMapCacheLimits(size_t cpu, size_t gpu)
{
    CloudMaps::forcedCpu = cpu;
    CloudMaps::forcedGpu = gpu;
    CloudMaps::cpuLimit = CloudMaps::PhysicalBudget();
    CloudMaps::Clear(true);
}
extern "C" __declspec(dllexport) long long GetCloudMapBindCount()
{
    return CloudMaps::binds.load();
}
extern "C" __declspec(dllexport) void ReadCloudMaps(long long *out, int count)
{
    if (!out || count != 12)
        return;
    std::lock_guard<std::mutex> lock(CloudMaps::cacheMutex);
    long long values[] = {1,
                          CloudMaps::active.load() && CloudMaps::installed.load() ? 1LL : 0LL,
                          CloudMaps::sources.load(),
                          static_cast<long long>(CloudMaps::cpuBytes.load()),
                          static_cast<long long>(CloudMaps::gpuBytes.load()),
                          static_cast<long long>(CloudMaps::pairs.size()),
                          static_cast<long long>(CloudMaps::jobs.size()),
                          CloudMaps::prepared.load(),
                          CloudMaps::binds.load(),
                          CloudMaps::fallbacks.load(),
                          CloudMaps::shaderRejects.load(),
                          CloudMaps::jobFailures.load()};
    std::memcpy(out, values, sizeof(values));
}
extern "C" __declspec(dllexport) void ShutdownCloudMaps()
{
    CloudMaps::active = false;
    CloudMaps::Clear(true);
    {
        std::lock_guard<std::mutex> lock(CloudMaps::cacheMutex);
        CloudMaps::stopping = true;
        CloudMaps::wake.notify_one();
    }
    if (CloudMaps::worker.joinable())
        CloudMaps::worker.join();
}
