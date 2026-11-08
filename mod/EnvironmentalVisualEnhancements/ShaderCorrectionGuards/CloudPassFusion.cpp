#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <bcrypt.h>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include "MinHook.h"
#include "CloudPassState.h"
#include "CloudQueries.h"
#include "CloudHookChain.h"
#include "../../../core/TimingProbeQuery.h"
#include "DxilHash.h"
using Context=ID3D11DeviceContext4;
static const GUID kindId={0x614b54fd,0xad19,0x4f43,{0x98,0xe4,0x62,0xdc,0x31,0x87,0x36,0xb1}};
static thread_local bool inside=false;
static thread_local int boundKind=-1;
struct Scope{bool old=inside;Scope(){inside=true;}~Scope(){inside=old;}};
static std::recursive_mutex gate;
static std::atomic<ID3D11DeviceContext*> waiting{};
static std::atomic<uint64_t> seen{},queued{},fused{},fallback{},rejected{},failures{};
static BCRYPT_ALG_HANDLE algorithm{};
static std::vector<std::pair<std::array<unsigned char,32>,int>> identities;
static std::vector<char> fusedCode;
static std::atomic<bool> enabled{},clearRequested{};
static bool initialized{};
static std::vector<char> sourceCodes[2];
extern "C" void* koe_fuse_cloud_passes(const unsigned char*,size_t,const unsigned char*,size_t,const unsigned char**,size_t*);
extern "C" void koe_free_fused_cloud(void*);
extern "C" void ObserveCloudShaderCreations(void(*)(ID3D11PixelShader*,const void*,size_t));
static std::atomic<uint64_t> tagged{},observed{};
static std::atomic<uint64_t> lastFlushReason{};
static std::atomic<uint64_t> lastGeometryDifference{};
extern "C" __declspec(dllexport) void ObserveCloudFusionShader(ID3D11PixelShader* shader,const void* bytes,size_t size) noexcept{
    if(!shader||!bytes||size>UINT_MAX||!algorithm)return;
    try{
    ++observed;std::array<unsigned char,32> hash{};
    if(BCryptHash(algorithm,nullptr,0,(PUCHAR)bytes,(ULONG)size,hash.data(),32)<0)return;
    for(auto& item:identities)if(item.first==hash){shader->SetPrivateData(kindId,sizeof(int),&item.second);TagCloudUsage(shader,bytes,size);++tagged;
        std::lock_guard<std::recursive_mutex> lock(gate);
        auto& code=sourceCodes[item.second==0?0:1];
        if(code.empty())code.assign(static_cast<const char*>(bytes),static_cast<const char*>(bytes)+size);
        if(fusedCode.empty()&&!sourceCodes[0].empty()&&!sourceCodes[1].empty()){
            const unsigned char* compiled{};size_t length{};
            std::unique_ptr<void,decltype(&koe_free_fused_cloud)> owner(
                koe_fuse_cloud_passes((const unsigned char*)sourceCodes[0].data(),sourceCodes[0].size(),(const unsigned char*)sourceCodes[1].data(),sourceCodes[1].size(),&compiled,&length),koe_free_fused_cloud);
            if(owner){
                fusedCode.assign(compiled,compiled+length);
                if(length>=32&&length<=UINT_MAX)ComputeHashRetail((unsigned char*)fusedCode.data()+20,UINT(length-20),(unsigned char*)fusedCode.data()+4);
                else{fusedCode.clear();++failures;}
            }else ++failures;
        }
        break;}
    }catch(...){++failures;enabled=false;}
}
struct HookSpec{unsigned slot;void* replacement;void** original;};
struct Runtime {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext1> deferred;
    ComPtr<ID3D11PixelShader> shader;ComPtr<ID3D11CommandList> replay;
    std::unique_ptr<CloudPassState> producer;
    ComPtr<ID3D11Buffer> constantCopies[5];
    std::vector<ID3D11Resource*> protectedResources;
    UINT count{},start{};INT base{};
};
static std::unique_ptr<Runtime> runtime;
static int Kind(ID3D11PixelShader* shader){int kind=-1;UINT n=sizeof kind;if(shader)shader->GetPrivateData(kindId,&n,&kind);return kind;}
static void Flush(ID3D11DeviceContext* c){
    if(waiting.load()!=c)return;
    Scope scope;
    c->ExecuteCommandList(runtime->replay.Get(),TRUE);
    runtime->producer.reset();runtime->replay.Reset();waiting=nullptr;++fallback;
}
static void BeforeBoundary(ID3D11DeviceContext* c,unsigned reason=999){if(waiting.load()!=c)return;std::lock_guard<std::recursive_mutex> lock(gate);lastFlushReason=reason;Flush(c);}
static void ResourceBoundary(ID3D11DeviceContext* c,std::initializer_list<ID3D11Resource*> resources,unsigned reason){
    if(waiting.load()!=c)return;std::lock_guard<std::recursive_mutex> lock(gate);
    for(auto* r:resources)if(r&&std::find(runtime->protectedResources.begin(),runtime->protectedResources.end(),r)!=runtime->protectedResources.end()){lastFlushReason=reason;Flush(c);return;}
}
static void ViewBoundary(ID3D11DeviceContext* c,ID3D11View* view,unsigned reason){if(waiting.load()!=c||!view)return;ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ResourceBoundary(c,{resource.Get()},reason);}
static void BeginCloudQuery(ID3D11DeviceContext* c,ID3D11Asynchronous* query) noexcept{
    if(inside||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!CloudQueryChangesCounts(query))return;
    std::lock_guard<std::recursive_mutex> lock(gate);
    try{auto state=CloudQueryCount::Get(c,true);++state->active;}catch(...){++failures;enabled=false;}
}
static void EndCloudQuery(ID3D11DeviceContext* c,ID3D11Asynchronous* query) noexcept{
    if(inside||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!CloudQueryChangesCounts(query))return;
    std::lock_guard<std::recursive_mutex> lock(gate);auto state=CloudQueryCount::Get(c,false);if(state&&state->active)--state->active;
}
#include "CloudFusionHooks.generated.h"
using CreatePS=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
extern "C" HRESULT STDMETHODCALLTYPE KoeCreateCloudShaderOriginal(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
static CreatePS originalCreate=KoeCreateCloudShaderOriginal;
using CreateVS=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11VertexShader**);
static CreateVS originalCreateVS{};
static HRESULT STDMETHODCALLTYPE CreateVertex(ID3D11Device* d,const void* bytes,SIZE_T size,ID3D11ClassLinkage* linkage,ID3D11VertexShader** output){
    auto hr=originalCreateVS(d,bytes,size,linkage,output);
    if(!inside&&SUCCEEDED(hr)&&output&&*output&&!linkage){
        TagCloudUsage(*output,bytes,size);unsigned char hash[32]{};
        if(size<=UINT_MAX&&BCryptHash(algorithm,nullptr,0,(PUCHAR)bytes,(ULONG)size,hash,32)>=0)(*output)->SetPrivateData(cloudVertexCodeId,32,hash);
    }
    return hr;
}
extern "C" void RememberCloudFusionBinding(ID3D11DeviceContext* c,ID3D11PixelShader* shader){
    if(!inside&&enabled&&c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)boundKind=Kind(shader);
}
static bool SameResource(ID3D11View* a,ID3D11View* b){if(!a||!b)return false;ComPtr<ID3D11Resource> x,y;a->GetResource(&x);b->GetResource(&y);return x==y;}
static bool Compatible(const CloudPassState& a,const CloudPassState& b){
    lastGeometryDifference=a.GeometryDifference(b);
    if(!a.SameGeometry(b)||!b.OpaqueFloatTarget()||SameResource(a.targets.data[0],b.targets.data[0])||!SameResource(a.targets.data[0],b.pr.data[7]))return false;
    ComPtr<ID3D11Resource> ar,br;a.targets.data[0]->GetResource(&ar);b.targets.data[0]->GetResource(&br);
    ComPtr<ID3D11Texture2D> at,bt;if(FAILED(ar.As(&at))||FAILED(br.As(&bt)))return false;
    D3D11_TEXTURE2D_DESC ad{},bd{};at->GetDesc(&ad);bt->GetDesc(&bd);
    if(ad.Width!=bd.Width||ad.Height!=bd.Height||ad.SampleDesc.Count!=1||bd.SampleDesc.Count!=1||ad.ArraySize!=1||bd.ArraySize!=1)return false;
    if(a.viewports[0].TopLeftX!=0||a.viewports[0].TopLeftY!=0||a.viewports[0].Width!=ad.Width||a.viewports[0].Height!=ad.Height)return false;
    for(unsigned i=0;i<128;i++){
        if(a.pixelUsage.Resource(i)&&(SameResource(a.pr.data[i],a.targets.data[0])||SameResource(a.pr.data[i],b.targets.data[0])))return false;
        if(i!=7&&b.pixelUsage.Resource(i)&&SameResource(b.pr.data[i],a.targets.data[0]))return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};b.pr.data[7]->GetDesc(&s);
    return s.Format==DXGI_FORMAT_R32G32B32A32_FLOAT&&s.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&s.Texture2D.MostDetailedMip==0;
}
static void STDMETHODCALLTYPE Hook_DrawIndexed(Context* c,UINT count,UINT start,INT base){
    if(!inside&&!enabled&&waiting.load()==c)BeforeBoundary(c,1004);
    if(inside||!enabled||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||(boundKind<0&&!waiting.load())){original_DrawIndexed(c,count,start,base);return;}
    std::lock_guard<std::recursive_mutex> lock(gate);Scope scope;
    if(waiting.load()&&waiting.load()!=c){original_DrawIndexed(c,count,start,base);return;}
    try{
        ComPtr<ID3D11PixelShader> current;c->PSGetShader(&current,nullptr,nullptr);int kind=Kind(current.Get());
        if(waiting.load()==c){
            CloudPassState consumer;
            if(kind==2&&count==runtime->count&&start==runtime->start&&base==runtime->base&&consumer.Capture(c)&&Compatible(*runtime->producer,consumer)){
                auto& p=*runtime->producer;
                ID3D11ShaderResourceView* empty{};c->PSSetShaderResources(7,1,&empty);
                ID3D11RenderTargetView* targets[]={consumer.targets.data[0],p.targets.data[0]};c->OMSetRenderTargets(2,targets,nullptr);
                c->PSSetConstantBuffers1(5,5,p.pc.data,p.pfirst,p.pcount);c->PSSetShaderResources(16,12,p.pr.data);c->PSSetSamplers(8,8,p.psam.data);
                c->PSSetShader(runtime->shader.Get(),nullptr,0);original_DrawIndexed(c,count,start,base);
                ++fused;waiting=nullptr;runtime->replay.Reset();runtime->producer.reset();
                c->PSSetShader(consumer.ps.Get(),nullptr,0);c->PSSetShaderResources(16,12,consumer.pr.data+16);c->PSSetSamplers(8,8,consumer.psam.data+8);
                c->PSSetConstantBuffers1(5,5,consumer.pc.data+5,consumer.pfirst+5,consumer.pcount+5);
                c->OMSetRenderTargets(8,consumer.targets.data,consumer.depthView.Get());c->PSSetShaderResources(7,1,consumer.pr.data+7);
                return;
            }
            lastFlushReason=1000+kind+1;Flush(c);
        }
        if(kind!=0||count!=6||fusedCode.empty()){original_DrawIndexed(c,count,start,base);return;}
        ++seen;
        auto queries=CloudQueryCount::Get(c,false);if(queries&&queries->active){++rejected;original_DrawIndexed(c,count,start,base);return;}
        if(!runtime){
            auto r=std::make_unique<Runtime>();c->GetDevice(&r->device);
            ComPtr<ID3D11DeviceContext> deferred;
            if(FAILED(r->device->CreateDeferredContext(0,&deferred))||FAILED(deferred.As(&r->deferred))||FAILED(originalCreate(r->device.Get(),fusedCode.data(),fusedCode.size(),nullptr,&r->shader))){++rejected;original_DrawIndexed(c,count,start,base);return;}
            runtime=std::move(r);
        }
        ComPtr<ID3D11Device> device;c->GetDevice(&device);
        auto state=std::make_unique<CloudPassState>();
        if(device!=runtime->device||!state->Capture(c)||!state->OpaqueFloatTarget()){++rejected;original_DrawIndexed(c,count,start,base);return;}
        for(unsigned i=0;i<5;i++)if(state->pc.data[i]){
            D3D11_BUFFER_DESC desc{},cached{};state->pc.data[i]->GetDesc(&desc);
            if(runtime->constantCopies[i])runtime->constantCopies[i]->GetDesc(&cached);
            if(cached.ByteWidth!=desc.ByteWidth){
                runtime->constantCopies[i].Reset();desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.MiscFlags=0;
                if(FAILED(device->CreateBuffer(&desc,nullptr,&runtime->constantCopies[i]))){++rejected;original_DrawIndexed(c,count,start,base);return;}
            }
            c->CopyResource(runtime->constantCopies[i].Get(),state->pc.data[i]);state->pc.data[i]->Release();
            state->pc.data[i]=runtime->constantCopies[i].Get();state->pc.data[i]->AddRef();
        }
        auto& protectedResources=runtime->protectedResources;protectedResources.clear();
        auto add=[&](ID3D11Resource* r){if(r)protectedResources.push_back(r);};
        auto view=[&](ID3D11View* v){if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);add(r.Get());}};
        add(state->index.Get());for(auto* p:state->vertices.data)add(p);
        for(unsigned i=0;i<14;i++){if(state->vertexUsage.constants&(1u<<i))add(state->vc.data[i]);if(state->pixelUsage.constants&(1u<<i))add(state->pc.data[i]);}
        for(unsigned i=0;i<128;i++){if(state->vertexUsage.Resource(i))view(state->vr.data[i]);if(state->pixelUsage.Resource(i))view(state->pr.data[i]);}view(state->targets.data[0]);
        state->Apply(runtime->deferred.Get());runtime->deferred->DrawIndexed(count,start,base);
        if(FAILED(runtime->deferred->FinishCommandList(FALSE,&runtime->replay))){++rejected;original_DrawIndexed(c,count,start,base);return;}
        runtime->producer=std::move(state);runtime->count=count;runtime->start=start;runtime->base=base;waiting=c;++queued;
    }catch(...){++failures;Flush(c);original_DrawIndexed(c,count,start,base);}
}
using Present=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);static Present originalPresent{};
static void EndFusionFrame(){if(auto*c=waiting.load())BeforeBoundary(c);if(clearRequested.exchange(false)){std::lock_guard<std::recursive_mutex> lock(gate);runtime.reset();}}
static HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* s,UINT a,UINT b){EndFusionFrame();return originalPresent(s,a,b);}
using Present1=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);static Present1 originalPresent1{};
static HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* s,UINT a,UINT b,const DXGI_PRESENT_PARAMETERS* p){EndFusionFrame();return originalPresent1(s,a,b,p);}
extern "C" __declspec(dllexport) int CloudFusionLiveStats(uint64_t* out,unsigned n){if(!out||n<8)return -1;out[0]=seen;out[1]=queued;out[2]=fused;out[3]=fallback;out[4]=rejected;out[5]=failures;out[6]=waiting.load()!=nullptr;out[7]=enabled;if(n>=10){out[8]=observed;out[9]=tagged;}if(n>=11)out[10]=lastFlushReason;if(n>=12)out[11]=lastGeometryDifference;return 0;}
extern "C" __declspec(dllexport) int SetCloudFusionLive(int on){
    std::lock_guard<std::recursive_mutex> lock(gate);if(auto*c=waiting.load())Flush(c);
    enabled=on!=0;seen=queued=fused=fallback=rejected=failures=0;return 0;
}

extern "C" __declspec(dllexport) void ClearCloudPassFusion(){clearRequested=true;}
extern "C" __declspec(dllexport) void DisableCloudPassFusion(){enabled=false;clearRequested=true;}
extern "C" __declspec(dllexport) int InitializeCloudPassFusion(){
    std::lock_guard<std::recursive_mutex> lock(gate);if(initialized)return -1;
    std::vector<void*> installed;
    try{
        const char* reviewed[]={"004373872942b4ef54182944c5a7eba90cccf9357766ede12d386e113006f7a6","1c0fba47ca693200df332fbcc3264f16df8418323f1168a4e3b2438af92e9def"};
        identities.clear();
        for(unsigned i=0;i<2;i++){
            std::array<unsigned char,32> hash{};
            for(unsigned j=0;j<32;j++){unsigned x{};if(sscanf_s(reviewed[i]+2*j,"%2x",&x)!=1)return -2;hash[j]=(unsigned char)x;}
            identities.push_back({hash,i==0?0:2});
        }
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return -3;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=16;desc.BufferDesc.Height=16;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=GetDesktopWindow();desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<IDXGISwapChain> swap;
        if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&context)))return -4;
        ComPtr<ID3D11DeviceContext4> newer;if(FAILED(context.As(&newer)))return -5;
        auto result=MH_Initialize();if(result!=MH_OK&&result!=MH_ERROR_ALREADY_INITIALIZED)return -6;
        auto hook=[&](void* target,void* replacement,void** original){
            auto shared=CloudHookChain::registry.Stage(target,replacement,original);
            if(shared==CloudHookChain::StageResult::Staged)return;
            if(shared==CloudHookChain::StageResult::Conflict)throw std::runtime_error("Fusion hook chain unavailable");
            if(MH_CreateHook(target,replacement,original)!=MH_OK)throw std::runtime_error("Fusion hook unavailable");
            installed.push_back(target);
        };
        auto dv=*reinterpret_cast<void***>(device.Get());hook(dv[12],(void*)CreateVertex,(void**)&originalCreateVS);
        auto cv=*reinterpret_cast<void***>(newer.Get());for(auto& h:contextHooks)hook(cv[h.slot],h.replacement,h.original);
        auto sv=*reinterpret_cast<void***>(swap.Get());hook(sv[8],(void*)PresentHook,(void**)&originalPresent);
        ComPtr<IDXGISwapChain1> swap1;if(SUCCEEDED(swap.As(&swap1))){auto v=*reinterpret_cast<void***>(swap1.Get());hook(v[22],(void*)Present1Hook,(void**)&originalPresent1);}
        for(auto target:installed)if(MH_QueueEnableHook(target)!=MH_OK)throw std::runtime_error("Fusion hook activation unavailable");
        if(MH_ApplyQueued()!=MH_OK)throw std::runtime_error("Fusion activation failed");
        ObserveCloudShaderCreations(ObserveCloudFusionShader);
        CloudHookChain::registry.Commit();
        initialized=true;enabled=true;return 0;
    }catch(...){
        CloudHookChain::registry.Cancel();
        for(auto target:installed){MH_DisableHook(target);MH_RemoveHook(target);}
        enabled=false;return -7;
    }
}
