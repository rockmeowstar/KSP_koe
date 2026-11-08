#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_3.h>
#include <dxgi1_4.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include "MinHook.h"
#include "ShadowBufferMirror.h"
#include "ShadowQueuedCommands.h"
#include "ShadowGpuState.h"
#include "../../core/TimingProbeQuery.h"
using Microsoft::WRL::ComPtr;
using Context=ID3D11DeviceContext4;
static std::recursive_mutex gate;
static thread_local bool inside=false;
struct Internal {bool before=inside;Internal(){inside=true;}~Internal(){inside=before;}};
static bool initialized=false,enabled=false,failed=false;
static uint64_t bufferEpoch=1,draws=0,eligible=0,batches=0,accepted=0,rejected=0,boundaries=0;
static size_t queueBudget=0;
static uint64_t gpuBufferAllocations=0,uploadedBytes=0;
static uint64_t meshHits=0,meshMisses=0,packetHits=0,packetMisses=0;
static uint64_t identicalGeometryReuses=0;
static uint64_t gpuCacheEvictions=0;
static std::array<uint64_t,4> missingConstants{},shortConstants{},unknownConstants{};
static std::array<uint64_t,6> mapTypes{};
static uint64_t mappedSnapshotBytes=0;
static uint64_t initialConstantReadbacks=0;
static uint64_t mappedCopyTicks=0,mappedWriteCombinedBytes=0;
static uint64_t redirectedMappings=0,mappedUploadBytes=0;
static uint64_t gpuMatrixCopies=0;
static uint64_t dynamicGeometrySkips=0;
static uint64_t singletonForwards=0,singletonSkips=0,mergedObjects=0;
static uint64_t recoveredOriginalDraws=0,submittedRecoverySkips=0;
extern "C" __declspec(dllexport) void ShadowRecoveryStats(uint64_t* values){if(values){values[0]=recoveredOriginalDraws;values[1]=submittedRecoverySkips;}}
#if defined(KOE_SHADOW_TEST)
static unsigned injectedFailure=0;
extern "C" __declspec(dllexport) void ShadowInjectFailure(unsigned point){injectedFailure=point;}
static void Inject(unsigned point){if(injectedFailure==point){injectedFailure=0;throw std::runtime_error("injected shadow failure");}}
#else
static void Inject(unsigned){}
#endif
using GeometryKey=std::array<uint64_t,13>;
struct Geometry {
    GeometryKey key{};
    GeometryKey current{};
    std::vector<uint8_t> vertices,indices;
    size_t stride=0,position=0,normal=0,indexCount=0,indexStride=0;
    int64_t bias=0;
    uint64_t used=0;
    bool singleton=false;
    size_t bytes()const{return vertices.capacity()+indices.capacity();}
};
static std::array<uint64_t,7> prepareTicks{};
static std::array<uint64_t,5> emitTicks{};
static long long cpuFrequency=0;
static constexpr bool detailedTiming=false;
template<size_t N> struct PhaseTrace {
    std::array<uint64_t,N>& counters;size_t phase=0;long long started;
    static long long now(){LARGE_INTEGER value;QueryPerformanceCounter(&value);return value.QuadPart;}
    explicit PhaseTrace(std::array<uint64_t,N>& target):counters(target),started(detailedTiming?now():0){}
    void next(size_t value){if(detailedTiming){auto end=now();counters[phase]+=uint64_t(end-started);phase=value;started=end;}}
    ~PhaseTrace(){if(detailedTiming)counters[phase]+=uint64_t(now()-started);}
};
static BCRYPT_ALG_HANDLE algorithm=nullptr;
static std::shared_ptr<ShadowBufferMirror::Budget> memoryBudget;
static std::vector<uint8_t> bankCode;
static std::wstring liveStatsPath;
static ULONGLONG nextLiveReport=0;
static bool reporting=false;
static std::atomic<bool> clearRequested{};
extern "C" __declspec(dllexport) int ShadowReport(const wchar_t*,unsigned);
static const GUID shaderTag={0x351674be,0x081c,0x45a7,{0xb5,0x37,0x18,0xb8,0xaa,0x32,0x55,0x0c}};
static const GUID layoutTag={0x69a95f21,0xfa7c,0x45dc,{0xac,0x17,0x0c,0x67,0xe5,0x22,0x75,0xac}};
static const GUID epochTag={0x286d2118,0x845b,0x42e3,{0xbd,0xb9,0xcb,0x55,0x84,0x62,0xe1,0x9a}};
static constexpr unsigned bankCapacity=D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT/7;
static std::set<ID3D11Asynchronous*> sensitiveQueries;
static ComPtr<Context> timingContext;
static ComPtr<ID3D11Query> timingDisjoint,timingBefore,timingAfter;
static bool timingActive=false;
struct Layout {UINT position,normal;};
static void require(bool ok){if(!ok)throw std::runtime_error("shadow interception invariant");}
static void check(HRESULT hr){require(SUCCEEDED(hr));}
static ID3D11Buffer* AsBuffer(ID3D11Resource* resource) {
    if(!resource)return nullptr;D3D11_RESOURCE_DIMENSION kind;resource->GetType(&kind);
    return kind==D3D11_RESOURCE_DIMENSION_BUFFER?static_cast<ID3D11Buffer*>(resource):nullptr;
}
static ComPtr<ShadowBufferMirror> Mirror(ID3D11Resource* resource) {
    auto buffer=AsBuffer(resource);auto mirror=ShadowBufferMirror::get(buffer);
    if(buffer&&!mirror&&memoryBudget) {
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
        if((desc.Usage==D3D11_USAGE_DEFAULT||desc.Usage==D3D11_USAGE_IMMUTABLE)&&(desc.BindFlags&(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER))&&!(desc.BindFlags&(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_RENDER_TARGET))&&
           ShadowBufferMirror::attach(buffer,memoryBudget,nullptr)) {
            buffer->SetPrivateData(epochTag,sizeof(bufferEpoch),&bufferEpoch);mirror=ShadowBufferMirror::get(buffer);
        }
    }
    if(mirror) {
        uint64_t epoch=0;UINT bytes=sizeof(epoch);
        if(FAILED(buffer->GetPrivateData(epochTag,&bytes,&epoch))||epoch!=bufferEpoch) {
            mirror->invalidate();buffer->SetPrivateData(epochTag,sizeof(bufferEpoch),&bufferEpoch);
        }
    }
    return mirror;
}
static void Invalidate(ID3D11Resource* resource){if(inside)return;auto mirror=Mirror(resource);if(mirror)mirror->invalidate();}
static void InvalidateView(ID3D11View* view){if(inside||!view)return;ComPtr<ID3D11Resource> resource;view->GetResource(&resource);Invalidate(resource.Get());}
static bool Marked(ID3D11DeviceChild* child) {
    if(!child)return false;UINT value=0,bytes=sizeof(value);
    return SUCCEEDED(child->GetPrivateData(shaderTag,&bytes,&value))&&value==1;
}
static std::string Hash(const void* bytes,size_t count) {
    if(!bytes||count>UINT_MAX)return {};
    unsigned char hash[32];
    if(BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(static_cast<const unsigned char*>(bytes)),
                  ULONG(count),hash,sizeof(hash))<0)return {};
    const char* hex="0123456789abcdef";std::string result;
    for(auto b:hash){result+=hex[b>>4];result+=hex[b&15];}return result;
}
struct Runtime {
    ComPtr<Context> context;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> banked;
    ComPtr<ID3D11VertexShader> sourceVs;
    ComPtr<ID3D11InputLayout> layout;
    bool knownVs=false,knownPs=false;
    std::array<std::vector<uint8_t>,4> globals;
    std::array<std::vector<uint8_t>,4> values,uploadedGlobals;
    ComPtr<ID3D11Buffer> constantBuffers[4];
    ComPtr<ID3D11Buffer> globalSources[4];
    UINT globalFirst[4]{};
    bool globalsReady=false;
    bool gpuGlobalsReady=false;
    struct OriginalDraw {ShadowGpuState state;UINT count=0,start=0;INT base=0;};
    std::unique_ptr<OriginalDraw> originalDraw;
    std::unique_ptr<ShadowCommands> commands;
    struct SourceDraw {
        ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11Buffer> vertex,index;
        UINT stride=0,offset=0,indexOffset=0,count=0,start=0;INT base=0;DXGI_FORMAT format{};
    };
    struct Mesh {std::shared_ptr<Geometry> geometry;std::array<uint8_t,112> matrix;SourceDraw source;};
    ComPtr<ID3D11Buffer> fallbackConstants;
    size_t submittedObjects=0;
    struct Packet {ComPtr<ID3D11Buffer> vertex,index;UINT count=0;size_t bytes=0;uint64_t used=0;};
    std::map<GeometryKey,std::shared_ptr<Geometry>> geometries;
    std::map<std::vector<GeometryKey>,Packet> packets;
    std::vector<Mesh> pending;
    std::vector<GeometryKey> packetKey;
    std::vector<uint8_t> matrixBytes;
    size_t cpuCacheBytes=0,gpuCacheBytes=0,cacheLimit=0,pendingVertices=0,pendingIndices=0;
    size_t maximumCacheLimit=0;
    uint64_t serial=0;
    explicit Runtime(Context* c):context(c) {
        Internal internal;c->GetDevice(&device);
        cacheLimit=memoryBudget->limit/2;
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIAdapter3> adapter3;
        if(SUCCEEDED(device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter.As(&adapter3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if(SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info)))
                cacheLimit=std::min(cacheLimit,size_t(info.Budget>info.CurrentUsage?(info.Budget-info.CurrentUsage)/8:0));
        }
        require(cacheLimit>=65536);
        maximumCacheLimit=cacheLimit;
        ComPtr<ID3D11PixelShader> currentPs;c->PSGetShader(&currentPs,nullptr,nullptr);knownPs=Marked(currentPs.Get());
        check(device->CreateVertexShader(bankCode.data(),bankCode.size(),nullptr,&banked));
        D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"KOE_DRAW",0,DXGI_FORMAT_R32_UINT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
        check(device->CreateInputLayout(elements,4,bankCode.data(),bankCode.size(),&layout));
        const size_t sizes[]={16,96,bankCapacity*112,336};
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        check(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)));
        require(options.ConstantBufferPartialUpdate!=FALSE);
        for(unsigned i=0;i<4;++i)constantBuffers[i]=allocate(sizes[i],D3D11_BIND_CONSTANT_BUFFER,true);
        fallbackConstants=allocate(112,D3D11_BIND_CONSTANT_BUFFER,true);
        commands=std::make_unique<ShadowCommands>(112,bankCapacity,queueBudget,queueBudget,
                    [this](const ShadowQueueData& data){emit(data);});
    }
    ComPtr<ID3D11Buffer> allocate(size_t count,UINT flags,bool gpuOnly=false) {
        require(count&&count<=UINT_MAX);D3D11_BUFFER_DESC desc{};
        desc.ByteWidth=UINT(count);desc.Usage=gpuOnly?D3D11_USAGE_DEFAULT:D3D11_USAGE_DYNAMIC;desc.BindFlags=flags;
        desc.CPUAccessFlags=gpuOnly?0:D3D11_CPU_ACCESS_WRITE;ComPtr<ID3D11Buffer> result;
        check(device->CreateBuffer(&desc,nullptr,&result));++gpuBufferAllocations;return result;
    }
    void upload(ID3D11Buffer* target,const void* data,size_t count,D3D11_MAP type,size_t offset=0) {
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(target,0,type,0,&mapped));
        std::memcpy(static_cast<uint8_t*>(mapped.pData)+offset,data,count);
        context->Unmap(target,0);uploadedBytes+=count;
    }
    void emit(const ShadowQueueData& data) {
        Internal internal;
        PhaseTrace<5> trace(emitTicks);
        const size_t bytes=data.vertexBytes+data.indexCount*4;
        while(gpuCacheBytes+bytes>cacheLimit&&!packets.empty()) {
            auto oldest=std::min_element(packets.begin(),packets.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            gpuCacheBytes-=oldest->second.bytes;packets.erase(oldest);++gpuCacheEvictions;
        }
        require(bytes<=cacheLimit);
        auto immutable=[&](const void* source,size_t count,UINT flags){
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(count);desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=flags;
            D3D11_SUBRESOURCE_DATA initial{source,0,0};ComPtr<ID3D11Buffer> result;
            check(device->CreateBuffer(&desc,&initial,&result));++gpuBufferAllocations;uploadedBytes+=count;return result;
        };
        Packet packet;packet.vertex=immutable(data.vertices,data.vertexBytes,D3D11_BIND_VERTEX_BUFFER);
        packet.index=immutable(data.indices,data.indexCount*4,D3D11_BIND_INDEX_BUFFER);packet.count=UINT(data.indexCount);packet.bytes=bytes;packet.used=++serial;
        auto result=packets.emplace(packetKey,std::move(packet));require(result.second);gpuCacheBytes+=bytes;
        trace.next(1);
        draw(result.first->second);
    }
    void draw(Packet& packet) {
        Internal internal;packet.used=++serial;
        ShadowGpuState saved(context.Get());
        ID3D11Buffer* vertices=packet.vertex.Get();UINT stride=28,vertexOffset=0;
        ID3D11Buffer* constants[]={constantBuffers[0].Get(),constantBuffers[1].Get(),constantBuffers[2].Get(),constantBuffers[3].Get()};
        context->IASetInputLayout(layout.Get());context->IASetVertexBuffers(0,1,&vertices,&stride,&vertexOffset);
        context->IASetIndexBuffer(packet.index.Get(),DXGI_FORMAT_R32_UINT,0);
        context->VSSetShader(banked.Get(),nullptr,0);context->VSSetConstantBuffers(0,4,constants);
        Inject(1);
        context->DrawIndexed(packet.count,0,0);submittedObjects=pending.size();++batches;
        Inject(2);
        saved.restore();ShadowGpuState actual(context.Get());require(saved.equals(actual));
    }
    void flush(){
        if(pending.empty())return;
        submittedObjects=0;
        if(pending.size()==1) {
            Internal internal;ShadowGpuState saved(context.Get());
            originalDraw->state.restore();
            context->DrawIndexed(originalDraw->count,originalDraw->start,originalDraw->base);
            submittedObjects=1;
            saved.restore();pending.front().geometry->singleton=true;++singletonForwards;
        }else {
        mergedObjects+=pending.size();
        packetKey.clear();matrixBytes.resize(pending.size()*112);
        for(size_t i=0;i<pending.size();++i){packetKey.push_back(pending[i].geometry->key);std::memcpy(matrixBytes.data()+i*112,pending[i].matrix.data(),112);}
        auto found=packets.find(packetKey);
        if(found!=packets.end()){++packetHits;draw(found->second);}
        else {
            ++packetMisses;
            for(const auto& mesh:pending){const auto& g=*mesh.geometry;
                ShadowSnapshot snapshot{g.vertices.data(),g.vertices.size(),g.stride,g.position,g.normal,
                    g.indices.data(),g.indexCount,g.indexStride,g.bias,mesh.matrix.data(),112};
                commands->push(1,snapshot);
            }
            commands->flush();
        }
        }
        pending.clear();originalDraw.reset();pendingVertices=0;pendingIndices=0;submittedObjects=0;globalsReady=false;gpuGlobalsReady=false;
        for(auto& source:globalSources)source.Reset();
    }
    void recover(){
        if(pending.empty())return;
        Internal internal;ShadowGpuState saved(context.Get());
        if(submittedObjects<pending.size()){
            originalDraw->state.restore();
            if(pending.size()==1){
                context->DrawIndexed(originalDraw->count,originalDraw->start,originalDraw->base);
                ++recoveredOriginalDraws;
            }else{
                ID3D11Buffer* buffers[]={constantBuffers[0].Get(),constantBuffers[1].Get(),fallbackConstants.Get(),constantBuffers[3].Get()};
                context->VSSetConstantBuffers(0,4,buffers);
                for(size_t i=submittedObjects;i<pending.size();i++){
                    auto& source=pending[i].source;
                    D3D11_BOX box{UINT(i*112),0,0,UINT((i+1)*112),1,1};
                    context->CopySubresourceRegion1(fallbackConstants.Get(),0,0,0,0,constantBuffers[2].Get(),0,&box,0);
                    auto* vertex=source.vertex.Get();context->IASetInputLayout(source.layout.Get());
                    context->IASetVertexBuffers(0,1,&vertex,&source.stride,&source.offset);
                    context->IASetIndexBuffer(source.index.Get(),source.format,source.indexOffset);
                    context->DrawIndexed(source.count,source.start,source.base);
                    ++recoveredOriginalDraws;
                }
            }
        }
        else ++submittedRecoverySkips;
        saved.restore();commands->discard();pending.clear();originalDraw.reset();
        pendingVertices=pendingIndices=submittedObjects=0;globalsReady=gpuGlobalsReady=false;
        for(auto& source:globalSources)source.Reset();
    }
    void remember(const GeometryKey& binding,const std::shared_ptr<Geometry>& geometry) {
        auto existing=geometries.find(binding);
        if(existing!=geometries.end()){cpuCacheBytes-=existing->second->bytes();geometries.erase(existing);}
        while(cpuCacheBytes+geometry->bytes()>cacheLimit) {
            auto oldest=geometries.end();
            for(auto it=geometries.begin();it!=geometries.end();++it)if(it->second.use_count()==1&&(oldest==geometries.end()||it->second->used<oldest->second->used))oldest=it;
            if(oldest==geometries.end()){flush();continue;}
            cpuCacheBytes-=oldest->second->bytes();geometries.erase(oldest);
        }
        geometries.emplace(binding,geometry);cpuCacheBytes+=geometry->bytes();
    }
    template<class Capture> void enqueue(const std::shared_ptr<Geometry>& geometry,ComPtr<ID3D11Buffer> const* sources,const UINT* first,SourceDraw sourceDraw,Capture capture) {
        size_t vertices=geometry->vertices.size()/geometry->stride*28,indices=geometry->indexCount*4;
        require(vertices<=queueBudget&&indices<=queueBudget);
        if(pending.size()==bankCapacity||vertices>queueBudget-pendingVertices||indices>queueBudget-pendingIndices||
           vertices+indices>cacheLimit-pendingVertices-pendingIndices)flush();
        {
            Internal internal;
            if(pending.empty()) {
                originalDraw=capture();
                for(unsigned i:{0u,1u,3u}){globalSources[i]=sources[i];globalFirst[i]=first[i];}
                globalsReady=true;
            }else if(!gpuGlobalsReady) {
                const UINT sizes[]={16,96,112,336};
                for(unsigned i:{0u,1u,3u}) {
                    D3D11_BOX region{globalFirst[i]*16,0,0,globalFirst[i]*16+sizes[i],1,1};
                    context->CopySubresourceRegion1(constantBuffers[i].Get(),0,0,0,0,globalSources[i].Get(),0,&region,0);
                }
                UINT offset=originalDraw->state.first[2]*16;
                D3D11_BOX region{offset,0,0,offset+112,1,1};
                context->CopySubresourceRegion1(constantBuffers[2].Get(),0,0,0,0,originalDraw->state.constants[2].Get(),0,&region,0);
                ++gpuMatrixCopies;gpuGlobalsReady=true;
            }
            if(!pending.empty()) {
            UINT offset=first[2]*16;
            D3D11_BOX region{offset,0,0,offset+112,1,1};
            context->CopySubresourceRegion1(constantBuffers[2].Get(),0,UINT(pending.size()*112),0,0,sources[2].Get(),0,&region,0);
            ++gpuMatrixCopies;
            }
        }
        Mesh mesh{};mesh.geometry=geometry;mesh.source=std::move(sourceDraw);pending.push_back(std::move(mesh));
        pendingVertices+=vertices;pendingIndices+=indices;
    }
};
static std::unique_ptr<Runtime> runtime;
extern "C" __declspec(dllexport) int ReadShadowDrawBatching(uint64_t* values,unsigned count){
    if(!values||count<10)return -1;
    std::lock_guard<std::recursive_mutex> lock(gate);
    values[0]=initialized;values[1]=failed;values[2]=accepted;values[3]=batches;values[4]=mergedObjects;
    values[5]=gpuMatrixCopies;values[6]=recoveredOriginalDraws;values[7]=submittedRecoverySkips;
    values[8]=mappedSnapshotBytes;values[9]=redirectedMappings;return 0;
}
static void BeforeBoundary() {
    if(inside)return;
    ++boundaries;
    if(runtime&&!failed)try{runtime->flush();}catch(...){runtime->recover();failed=true;enabled=false;}
    if(clearRequested.exchange(false))runtime.reset();
    if(!reporting&&!liveStatsPath.empty()&&GetTickCount64()>=nextLiveReport) {
        nextLiveReport=GetTickCount64()+10000;reporting=true;
        ShadowReport(liveStatsPath.c_str(),0);reporting=false;
    }
}
static void ResetBindings(Context* c){if(!inside&&runtime&&runtime->context.Get()==c){runtime->knownVs=false;runtime->knownPs=false;runtime->sourceVs.Reset();}}
struct HookSpec {unsigned slot;void* callback;void** original;};
#include "ShadowContextHooks.generated.h"

using CreateBuffer=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_BUFFER_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Buffer**);
using CreateLayout=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,ID3D11InputLayout**);
using CreateVS=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11VertexShader**);
using CreatePS=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
static CreateBuffer originalCreateBuffer;static CreateLayout originalCreateLayout;
static CreateVS originalCreateVS;static CreatePS originalCreatePS;
static HRESULT STDMETHODCALLTYPE NewBuffer(ID3D11Device* d,const D3D11_BUFFER_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Buffer** out) {
    std::lock_guard<std::recursive_mutex> lock(gate);
    auto hr=originalCreateBuffer(d,desc,data,out);
    if(!inside&&SUCCEEDED(hr)&&out&&*out&&desc&&(desc->Usage==D3D11_USAGE_DEFAULT||desc->Usage==D3D11_USAGE_IMMUTABLE)&&(desc->BindFlags&(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER))&&!(desc->BindFlags&(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_RENDER_TARGET))) {
        if(ShadowBufferMirror::attach(*out,memoryBudget,data?data->pSysMem:nullptr))
            (*out)->SetPrivateData(epochTag,sizeof(bufferEpoch),&bufferEpoch);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE NewLayout(ID3D11Device* d,const D3D11_INPUT_ELEMENT_DESC* e,UINT n,const void* code,SIZE_T size,ID3D11InputLayout** out) {
    auto hr=originalCreateLayout(d,e,n,code,size,out);
    if(inside||FAILED(hr)||!out||!*out||!e)return hr;
    Layout layout{};unsigned fields=0;bool valid=true;
    for(UINT i=0;i<n;++i)if(e[i].SemanticName&&e[i].SemanticIndex==0) {
        bool position=_stricmp(e[i].SemanticName,"POSITION")==0,normal=_stricmp(e[i].SemanticName,"NORMAL")==0;
        if(!position&&!normal)continue;
        valid&=e[i].InputSlot==0&&e[i].Format==DXGI_FORMAT_R32G32B32_FLOAT&&e[i].AlignedByteOffset!=D3D11_APPEND_ALIGNED_ELEMENT&&e[i].InputSlotClass==D3D11_INPUT_PER_VERTEX_DATA&&e[i].InstanceDataStepRate==0;
        if(position){layout.position=e[i].AlignedByteOffset;fields|=1;}
        if(normal){layout.normal=e[i].AlignedByteOffset;fields|=2;}
    }
    if(valid&&fields==3)(*out)->SetPrivateData(layoutTag,sizeof(layout),&layout);
    return hr;
}
static HRESULT STDMETHODCALLTYPE NewVS(ID3D11Device* d,const void* code,SIZE_T size,ID3D11ClassLinkage* linkage,ID3D11VertexShader** out) {
    std::lock_guard<std::recursive_mutex> lock(gate);
    auto hr=originalCreateVS(d,code,size,linkage,out);
    if(!inside&&SUCCEEDED(hr)&&out&&*out&&!linkage&&Hash(code,size)=="aa4b0478faaa884ac199b5eb3062ce4d9c27a96984fee1de66bba81707e747de") {
        UINT yes=1;(*out)->SetPrivateData(shaderTag,sizeof(yes),&yes);
        if(!timingContext){ComPtr<ID3D11DeviceContext> base;d->GetImmediateContext(&base);base.As(&timingContext);}
    }return hr;
}
static HRESULT STDMETHODCALLTYPE NewPS(ID3D11Device* d,const void* code,SIZE_T size,ID3D11ClassLinkage* linkage,ID3D11PixelShader** out) {
    auto hr=originalCreatePS(d,code,size,linkage,out);
    if(!inside&&SUCCEEDED(hr)&&out&&*out&&!linkage&&Hash(code,size)=="8dc2c81abf29c14b28acdb4fab3069096787238c56c4ba8ae028dfb6f6632798") {
        UINT yes=1;(*out)->SetPrivateData(shaderTag,sizeof(yes),&yes);
    }return hr;
}
static void STDMETHODCALLTYPE Hook_VSSetShader(Context* c,ID3D11VertexShader* vs,ID3D11ClassInstance* const* instances,UINT count) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();
    if(!inside)try {
        bool known=Marked(vs)&&!count;
        if(known&&enabled&&c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE&&(!runtime||runtime->context.Get()!=c))
            runtime=std::make_unique<Runtime>(c);
        if(runtime&&runtime->context.Get()==c){runtime->knownVs=known;runtime->sourceVs=known?vs:nullptr;}
    }catch(...){failed=true;enabled=false;}
    original_VSSetShader(c,vs,instances,count);
}
static void STDMETHODCALLTYPE Hook_PSSetShader(Context* c,ID3D11PixelShader* ps,ID3D11ClassInstance* const* instances,UINT count) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();
    if(!inside&&runtime&&runtime->context.Get()==c)runtime->knownPs=Marked(ps)&&!count;
    original_PSSetShader(c,ps,instances,count);
}
static bool Snapshot(ID3D11Resource* resource,size_t start,size_t count,std::vector<uint8_t>& output,Context* c=nullptr) {
    auto mirror=Mirror(resource);if(!mirror)return false;
    output.resize(count);uint64_t version=0;
    if(mirror->snapshot(start,count,output.data(),version))return true;
    auto buffer=AsBuffer(resource);if(!c||!buffer)return false;
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    if(desc.BindFlags!=D3D11_BIND_CONSTANT_BUFFER||desc.ByteWidth>65536||!mirror->claimInitialRecovery())return false;
    Internal internal;ComPtr<ID3D11Device> device;c->GetDevice(&device);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    ComPtr<ID3D11Buffer> staging;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&staging)))return false;
    c->CopyResource(staging.Get(),buffer);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return false;
    bool captured=mirror->upload(0,mapped.pData,desc.ByteWidth);c->Unmap(staging.Get(),0);++initialConstantReadbacks;
    return captured&&mirror->snapshot(start,count,output.data(),version);
}
static bool QueueDraw(Context* c,UINT count,UINT start,INT base) {
    if(!runtime||runtime->context.Get()!=c||!runtime->knownVs||!runtime->knownPs||!sensitiveQueries.empty()||!count||count%3)return false;
    ++eligible;
    PhaseTrace<7> trace(prepareTicks);
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue;c->GetPredication(&predicate,&predicateValue);if(predicate)return false;
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&hs,nullptr,nullptr);c->DSGetShader(&ds,nullptr,nullptr);
    if(gs||hs||ds)return false;
    D3D11_PRIMITIVE_TOPOLOGY topology;c->IAGetPrimitiveTopology(&topology);if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return false;
    ComPtr<ID3D11InputLayout> input;c->IAGetInputLayout(&input);Layout layout{};UINT layoutSize=sizeof(layout);
    if(!input||FAILED(input->GetPrivateData(layoutTag,&layoutSize,&layout)))return false;
    ComPtr<ID3D11Buffer> vb,ib;UINT stride=0,offset=0,indexOffset=0;DXGI_FORMAT format;
    c->IAGetVertexBuffers(0,1,&vb,&stride,&offset);c->IAGetIndexBuffer(&ib,&format,&indexOffset);
    if(!vb||!ib||!stride||layout.position>stride||layout.normal>stride||stride-layout.position<12||stride-layout.normal<12)return false;
    D3D11_BUFFER_DESC vertexDesc{},indexDesc{};vb->GetDesc(&vertexDesc);ib->GetDesc(&indexDesc);
    if(vertexDesc.Usage==D3D11_USAGE_DYNAMIC||indexDesc.Usage==D3D11_USAGE_DYNAMIC){++dynamicGeometrySkips;return false;}
    unsigned indexSize=format==DXGI_FORMAT_R16_UINT?2:format==DXGI_FORMAT_R32_UINT?4:0;
    if(!indexSize||size_t(count)*indexSize>queueBudget)return false;
    trace.next(1);
    auto vertexMirror=Mirror(vb.Get()),indexMirror=Mirror(ib.Get());
    if(vertexMirror)vertexMirror->observe();if(indexMirror)indexMirror->observe();
    uint64_t vertexId=0,vertexVersion=0,indexId=0,indexVersion=0;
    if(!vertexMirror||!indexMirror||!vertexMirror->stamp(vertexId,vertexVersion)||!indexMirror->stamp(indexId,indexVersion))return false;
    GeometryKey key{vertexId,vertexVersion,indexId,indexVersion,offset,stride,layout.position,layout.normal,indexOffset,start,count,indexSize,uint64_t(int64_t(base))};
    auto binding=key;binding[1]=0;binding[3]=0;
    std::shared_ptr<Geometry> geometry;
    auto found=runtime->geometries.find(binding);
    if(found!=runtime->geometries.end()&&found->second->current==key){geometry=found->second;++meshHits;}
    else {
        ++meshMisses;geometry=std::make_shared<Geometry>();geometry->key=key;geometry->current=key;
        auto& indices=geometry->indices;auto& vertices=geometry->vertices;
        if(!Snapshot(ib.Get(),size_t(indexOffset)+size_t(start)*indexSize,size_t(count)*indexSize,indices))return false;
        trace.next(2);ShadowIndexExtent extent{};
        if(ShadowIndexRange(indices.data(),count,indexSize,&extent)!=0)return false;
        int64_t low=int64_t(extent.minimum)+base,high=int64_t(extent.maximum)+base;
        if(low<0||high<low||uint64_t(high-low+1)>queueBudget/stride||uint64_t(high-low+1)>queueBudget/28)return false;
        trace.next(3);if(!Snapshot(vb.Get(),size_t(offset)+size_t(low)*stride,size_t(high-low+1)*stride,vertices))return false;
        geometry->stride=stride;geometry->position=layout.position;geometry->normal=layout.normal;
        geometry->indexCount=count;geometry->indexStride=indexSize;geometry->bias=int64_t(base)-low;
        if(geometry->bytes()>runtime->cacheLimit||size_t(high-low+1)*28+size_t(count)*4>runtime->cacheLimit)return false;
        if(found!=runtime->geometries.end()&&found->second->bias==geometry->bias&&found->second->vertices==vertices&&found->second->indices==indices) {
            geometry=found->second;geometry->current=key;++identicalGeometryReuses;
        } else runtime->remember(binding,geometry);
    }
    geometry->used=++runtime->serial;
    if(geometry->singleton){++singletonSkips;return false;}
    trace.next(4);
    ID3D11Buffer* raw[4]{};UINT first[4]{},length[4]{};c->VSGetConstantBuffers1(0,4,raw,first,length);
    ComPtr<ID3D11Buffer> constants[4];for(unsigned i=0;i<4;++i)constants[i].Attach(raw[i]);
    const size_t sizes[]={16,96,112,336};
    for(unsigned i=0;i<4;++i){
        if(!constants[i]){++missingConstants[i];return false;}
        if(size_t(length[i])*16<sizes[i]){++shortConstants[i];return false;}
        D3D11_BUFFER_DESC desc{};constants[i]->GetDesc(&desc);
        if(size_t(first[i])*16+sizes[i]>desc.ByteWidth)return false;
    }
    trace.next(5);bool changed=false;
    if(runtime->globalsReady)for(unsigned i:{0u,1u,3u})changed|=constants[i]!=runtime->globalSources[i]||first[i]!=runtime->globalFirst[i];
    if(changed)runtime->flush();
    Runtime::SourceDraw sourceDraw;sourceDraw.layout=input;sourceDraw.vertex=vb;sourceDraw.index=ib;
    sourceDraw.stride=stride;sourceDraw.offset=offset;sourceDraw.indexOffset=indexOffset;sourceDraw.format=format;
    sourceDraw.count=count;sourceDraw.start=start;sourceDraw.base=base;
    trace.next(6);runtime->enqueue(geometry,constants,first,std::move(sourceDraw),[&](){
        auto result=std::make_unique<Runtime::OriginalDraw>();auto& state=result->state;
        state.context=c;state.ranges=c;state.shader=runtime->sourceVs;state.layout=input;state.vertex=vb;state.index=ib;
        state.stride=stride;state.vertexOffset=offset;state.indexOffset=indexOffset;state.format=format;state.restored=true;
        for(unsigned i=0;i<4;++i){state.constants[i]=constants[i];state.first[i]=first[i];state.count[i]=length[i];}
        result->count=count;result->start=start;result->base=base;return result;
    });++accepted;return true;
}
static void STDMETHODCALLTYPE Hook_DrawIndexed(Context* c,UINT count,UINT start,INT base) {
    std::lock_guard<std::recursive_mutex> lock(gate);
    if(inside){original_DrawIndexed(c,count,start,base);return;}
    ++draws;
    if(enabled&&!failed)try{if(QueueDraw(c,count,start,base))return;}catch(...){if(runtime)runtime->recover();failed=true;enabled=false;}
    ++rejected;BeforeBoundary();original_DrawIndexed(c,count,start,base);
}
static HRESULT STDMETHODCALLTYPE Hook_Map(Context* c,ID3D11Resource* r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* result) {
    std::lock_guard<std::recursive_mutex> lock(gate);
    if(!inside&&(type!=D3D11_MAP_WRITE_NO_OVERWRITE||!AsBuffer(r)))BeforeBoundary();
    auto hr=original_Map(c,r,sub,type,flags,result);
    if(!inside&&SUCCEEDED(hr)&&sub==0&&result&&unsigned(type)<mapTypes.size())++mapTypes[unsigned(type)];
    return hr;
}
static void STDMETHODCALLTYPE Hook_Unmap(Context* c,ID3D11Resource* r,UINT sub) {
    std::lock_guard<std::recursive_mutex> lock(gate);
    original_Unmap(c,r,sub);
}
static void RecordUpdate(ID3D11Resource* r,UINT sub,const D3D11_BOX* box,const void* data,bool discard) {
    if(inside||sub)return;auto buffer=AsBuffer(r);auto mirror=Mirror(r);if(!buffer||!mirror)return;
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);if(discard)mirror->invalidate();
    size_t begin=box?box->left:0,end=box?box->right:desc.ByteWidth;
    if(begin>end||end>desc.ByteWidth){mirror->invalidate();return;}
    mirror->upload(begin,data,end-begin);
}
static void STDMETHODCALLTYPE Hook_UpdateSubresource(Context* c,ID3D11Resource* r,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT depth) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();if(c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)RecordUpdate(r,sub,box,data,false);original_UpdateSubresource(c,r,sub,box,data,row,depth);
}
static void STDMETHODCALLTYPE Hook_UpdateSubresource1(Context* c,ID3D11Resource* r,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT depth,UINT flags) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();if(c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)RecordUpdate(r,sub,box,data,(flags&D3D11_COPY_DISCARD)!=0);original_UpdateSubresource1(c,r,sub,box,data,row,depth,flags);
}
static void RecordCopy(ID3D11Resource* destination,UINT sub,UINT offset,ID3D11Resource* source,UINT sourceSub,const D3D11_BOX* box) {
    if(inside||sub||sourceSub)return;
    auto target=Mirror(destination),from=Mirror(source);auto sourceBuffer=AsBuffer(source);if(!target)return;
    if(!sourceBuffer){target->invalidate();return;}
    D3D11_BUFFER_DESC desc{};sourceBuffer->GetDesc(&desc);
    size_t begin=box?box->left:0,end=box?box->right:desc.ByteWidth;
    if(begin>end){target->invalidate();return;}target->copyFrom(from.Get(),offset,begin,end-begin);
}
static void STDMETHODCALLTYPE Hook_CopyResource(Context* c,ID3D11Resource* to,ID3D11Resource* from) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();if(c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)RecordCopy(to,0,0,from,0,nullptr);original_CopyResource(c,to,from);
}
static void STDMETHODCALLTYPE Hook_CopySubresourceRegion(Context* c,ID3D11Resource* to,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* from,UINT sourceSub,const D3D11_BOX* box) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();if(c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)RecordCopy(to,sub,x,from,sourceSub,box);original_CopySubresourceRegion(c,to,sub,x,y,z,from,sourceSub,box);
}
static void STDMETHODCALLTYPE Hook_CopySubresourceRegion1(Context* c,ID3D11Resource* to,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* from,UINT sourceSub,const D3D11_BOX* box,UINT flags) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();if(c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE){if(flags&D3D11_COPY_DISCARD)Invalidate(to);RecordCopy(to,sub,x,from,sourceSub,box);}original_CopySubresourceRegion1(c,to,sub,x,y,z,from,sourceSub,box,flags);
}
static void STDMETHODCALLTYPE Hook_Begin(Context* c,ID3D11Asynchronous* query) {
    std::lock_guard<std::recursive_mutex> lock(gate);if(!IsKoeTimingProbeQuery(query))BeforeBoundary();
    if(!inside&&query){ComPtr<ID3D11Query> typed;if(SUCCEEDED(query->QueryInterface(IID_PPV_ARGS(&typed)))){
        D3D11_QUERY_DESC desc{};typed->GetDesc(&desc);if(desc.Query!=D3D11_QUERY_TIMESTAMP_DISJOINT)sensitiveQueries.insert(query);
    }}original_Begin(c,query);
}
static void STDMETHODCALLTYPE Hook_End(Context* c,ID3D11Asynchronous* query) {
    std::lock_guard<std::recursive_mutex> lock(gate);if(!IsKoeTimingProbeQuery(query))BeforeBoundary();if(!inside)sensitiveQueries.erase(query);original_End(c,query);
}
extern "C" __declspec(dllexport) int ShadowMode(unsigned mode) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();if(failed)return -1;enabled=mode!=0;return 0;
}
extern "C" __declspec(dllexport) int ClearShadowBatchCaches(){
    clearRequested=true;return 0;
}
extern "C" __declspec(dllexport) int ShadowCacheBudget(size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();
    if(failed||!runtime||bytes<65536||bytes>runtime->maximumCacheLimit)return -1;
    runtime->packets.clear();runtime->geometries.clear();runtime->gpuCacheBytes=0;runtime->cpuCacheBytes=0;runtime->cacheLimit=bytes;return 0;
}
extern "C" __declspec(dllexport) int ShadowCacheStats(uint64_t* values) {
    std::lock_guard<std::recursive_mutex> lock(gate);if(!values)return -1;
    values[0]=packetHits;values[1]=packetMisses;values[2]=gpuCacheEvictions;return failed?-2:0;
}
extern "C" __declspec(dllexport) int ShadowStartTiming() {
    std::lock_guard<std::recursive_mutex> lock(gate);
    if(!timingContext||timingActive)return -1;
    try {
        Internal internal;
        if(!timingDisjoint) {
            ComPtr<ID3D11Device> device;timingContext->GetDevice(&device);
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};check(device->CreateQuery(&desc,&timingDisjoint));
            desc.Query=D3D11_QUERY_TIMESTAMP;check(device->CreateQuery(&desc,&timingBefore));check(device->CreateQuery(&desc,&timingAfter));
        }
        timingContext->Begin(timingDisjoint.Get());timingContext->End(timingBefore.Get());timingActive=true;return 0;
    }catch(...){return -2;}
}
extern "C" __declspec(dllexport) int ShadowFinishTiming(double* milliseconds) {
    std::lock_guard<std::recursive_mutex> lock(gate);
    if(!timingActive||!milliseconds)return -1;
    BeforeBoundary();Internal internal;
    timingContext->End(timingAfter.Get());timingContext->End(timingDisjoint.Get());timingContext->Flush();timingActive=false;
    auto wait=[&](ID3D11Query* query,void* data,UINT bytes){
        auto started=GetTickCount64();HRESULT result;
        while((result=timingContext->GetData(query,data,bytes,D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE){if(GetTickCount64()-started>5000)return false;Sleep(1);}
        return result==S_OK;
    };
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};UINT64 before=0,after=0;
    if(!wait(timingDisjoint.Get(),&clock,sizeof(clock))||clock.Disjoint||!clock.Frequency||
       !wait(timingBefore.Get(),&before,sizeof(before))||!wait(timingAfter.Get(),&after,sizeof(after))||after<before)return -2;
    *milliseconds=double(after-before)*1000.0/clock.Frequency;return 0;
}
extern "C" __declspec(dllexport) int ShadowReport(const wchar_t* path,unsigned release) {
    std::lock_guard<std::recursive_mutex> lock(gate);BeforeBoundary();
    if(release){enabled=false;runtime.reset();sensitiveQueries.clear();timingBefore.Reset();timingAfter.Reset();timingDisjoint.Reset();timingContext.Reset();}
    std::ofstream file{std::filesystem::path(path)};
    file<<"{\"failed\":"<<(failed?"true":"false")<<",\"draws\":"<<draws<<",\"eligible\":"<<eligible
        <<",\"accepted\":"<<accepted<<",\"batches\":"<<batches<<",\"rejected\":"<<rejected
        <<",\"boundaries\":"<<boundaries<<",\"mirror_bytes\":"<<memoryBudget->used.load()
        <<",\"gpu_buffer_allocations\":"<<gpuBufferAllocations<<",\"uploaded_bytes\":"<<uploadedBytes
        <<",\"mesh_hits\":"<<meshHits<<",\"mesh_misses\":"<<meshMisses<<",\"packet_hits\":"<<packetHits<<",\"packet_misses\":"<<packetMisses
        <<",\"identical_geometry_reuses\":"<<identicalGeometryReuses
        <<",\"gpu_cache_evictions\":"<<gpuCacheEvictions
        <<",\"geometry_cpu_bytes\":"<<(runtime?runtime->cpuCacheBytes:0)<<",\"geometry_gpu_bytes\":"<<(runtime?runtime->gpuCacheBytes:0)
        <<",\"mirror_live\":"<<memoryBudget->live.load()<<",\"mirror_budget\":"<<memoryBudget->limit
        <<",\"queue_budget\":"<<queueBudget<<",\"prepare_cpu_ms\":[";
    for(size_t i=0;i<prepareTicks.size();++i){if(i)file<<',';file<<double(prepareTicks[i])*1000.0/cpuFrequency;}
    file<<"],\"emit_cpu_ms\":[";
    for(size_t i=0;i<emitTicks.size();++i){if(i)file<<',';file<<double(emitTicks[i])*1000.0/cpuFrequency;}
    file<<"],\"mapped_copy_ms\":"<<double(mappedCopyTicks)*1000.0/cpuFrequency
        <<",\"singleton_forwards\":"<<singletonForwards<<",\"singleton_skips\":"<<singletonSkips<<",\"merged_objects\":"<<mergedObjects
        <<",\"dynamic_geometry_skips\":"<<dynamicGeometrySkips
        <<",\"gpu_matrix_copies\":"<<gpuMatrixCopies
        <<",\"redirected_mappings\":"<<redirectedMappings<<",\"mapped_upload_bytes\":"<<mappedUploadBytes
        <<",\"mapped_uncached_bytes\":"<<mappedWriteCombinedBytes
        <<",\"initial_constant_readbacks\":"<<initialConstantReadbacks<<",\"mapped_snapshot_bytes\":"<<mappedSnapshotBytes<<",\"map_types\":[";
    for(size_t i=0;i<mapTypes.size();++i){if(i)file<<',';file<<mapTypes[i];}
    file<<"],\"constant_rejections\":[";
    for(size_t i=0;i<4;++i){if(i)file<<',';file<<"{\"missing\":"<<missingConstants[i]<<",\"short\":"<<shortConstants[i]<<",\"unknown\":"<<unknownConstants[i]<<'}';}
    file<<"]}";
    return failed?-1:file.good()?0:-2;
}
extern "C" __declspec(dllexport) int InitializeShadowBatches(const wchar_t* codePath) {
    std::lock_guard<std::recursive_mutex> lock(gate);
    if(initialized)return -1;
    std::vector<void*> targets;
    try {
        LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);cpuFrequency=frequency.QuadPart;
        std::ifstream input{std::filesystem::path(codePath),std::ios::binary|std::ios::ate};require(bool(input));
        auto length=input.tellg();require(length>0&&length<1024*1024);bankCode.resize(size_t(length));input.seekg(0);
        input.read(reinterpret_cast<char*>(bankCode.data()),bankCode.size());require(bool(input));
        MEMORYSTATUSEX memory{sizeof(memory)};require(GlobalMemoryStatusEx(&memory)!=0);
        size_t budget=size_t(memory.ullAvailPhys/64);require(budget>=65536);
        memoryBudget=std::make_shared<ShadowBufferMirror::Budget>(budget);
        DWORD bytes=0;GetLogicalProcessorInformationEx(RelationCache,nullptr,&bytes);std::vector<uint8_t> info(bytes);
        size_t cache=0;if(GetLogicalProcessorInformationEx(RelationCache,reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(info.data()),&bytes))
            for(size_t at=0;at<bytes;){auto item=reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(info.data()+at);if(!item->Size)break;if(item->Cache.Level==3)cache=std::max(cache,size_t(item->Cache.CacheSize));at+=item->Size;}
        queueBudget=std::min(budget,cache?cache:budget);
        require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0);
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> base;ComPtr<Context> context;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&base));check(base.As(&context));
        void** dv=*reinterpret_cast<void***>(device.Get());void** cv=*reinterpret_cast<void***>(context.Get());
        HMODULE owner=nullptr;wchar_t ownerPath[32768]{};
        require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(cv[12]),&owner)!=0);
        require(GetModuleFileNameW(owner,ownerPath,32768)!=0);
        auto name=std::filesystem::path(ownerPath).filename().wstring();
        std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return wchar_t(towlower(c));});
        require(name!=L"renderdoc.dll");
        auto status=MH_Initialize();require(status==MH_OK||status==MH_ERROR_ALREADY_INITIALIZED);
        auto hook=[&](void* target,void* callback,void** original){require(std::find(targets.begin(),targets.end(),target)==targets.end());require(MH_CreateHook(target,callback,original)==MH_OK);targets.push_back(target);};
        hook(dv[3],reinterpret_cast<void*>(NewBuffer),reinterpret_cast<void**>(&originalCreateBuffer));
        hook(dv[11],reinterpret_cast<void*>(NewLayout),reinterpret_cast<void**>(&originalCreateLayout));
        hook(dv[12],reinterpret_cast<void*>(NewVS),reinterpret_cast<void**>(&originalCreateVS));
        hook(dv[15],reinterpret_cast<void*>(NewPS),reinterpret_cast<void**>(&originalCreatePS));
        for(auto& spec:contextHooks)hook(cv[spec.slot],spec.callback,spec.original);
        for(auto target:targets)require(MH_QueueEnableHook(target)==MH_OK);
        require(MH_ApplyQueued()==MH_OK);initialized=true;return 0;
    }catch(...){for(auto target:targets){MH_DisableHook(target);MH_RemoveHook(target);}failed=true;return -2;}
}
extern "C" __declspec(dllexport) void UnityPluginLoad(void*) {
    if(initialized)return;
    wchar_t folder[32768]{},output[32768]{};
    if(!GetEnvironmentVariableW(L"KOE_SHADOW_BATCH_TRIAL",folder,32768))return;
    GetEnvironmentVariableW(L"KOE_SHADOW_BATCH_STATS",output,32768);
    HMODULE pinned=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                      reinterpret_cast<LPCWSTR>(UnityPluginLoad),&pinned);
    int result=InitializeShadowBatches((std::filesystem::path(folder)/L"banked-vs.dxbc").c_str());
    if(!result)result=ShadowMode(1);
    if(result) {
        if(output[0]){std::ofstream file{std::filesystem::path(output)};file<<"{\"failed\":true,\"init_result\":"<<result<<"}";}
        return;
    }
    liveStatsPath=output;nextLiveReport=GetTickCount64()+10000;
    if(!liveStatsPath.empty())ShadowReport(liveStatsPath.c_str(),0);
}
extern "C" __declspec(dllexport) void UnityPluginUnload() {
    if(liveStatsPath.empty())return;
    std::lock_guard<std::recursive_mutex> lock(gate);
    reporting=true;ShadowMode(0);ShadowReport(liveStatsPath.c_str(),1);liveStatsPath.clear();reporting=false;
}
static long long pluginEvents=0;
static void __stdcall LiveEvent(int) {std::lock_guard<std::recursive_mutex> lock(gate);++pluginEvents;BeforeBoundary();}
extern "C" __declspec(dllexport) void* __stdcall GetRenderEventFunc() {
    if(!initialized)UnityPluginLoad(nullptr);
    return failed?nullptr:reinterpret_cast<void*>(LiveEvent);
}
extern "C" __declspec(dllexport) void __stdcall ReadProbeCounters(long long* values) {
    if(!values)return;
    for(unsigned i=0;i<13;++i)values[i]=0;
    values[0]=initialized?1:0;values[1]=pluginEvents;values[2]=2;values[12]=failed?-1:0;
}
