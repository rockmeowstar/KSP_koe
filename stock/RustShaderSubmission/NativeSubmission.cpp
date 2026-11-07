#define NOMINMAX
#include <Windows.h>
#undef PropertySheet
#include <bcrypt.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <vector>
#include "UnityAbi.h"
using namespace ShaderSubmission;
using Submit=void(*)(uint8_t*,const PropertySheet*);
static Submit original;
static Api api{};
static std::atomic<uint32_t> mode{0};
static void* target;
static void initializeWorkers() {
    SYSTEM_INFO system{};GetSystemInfo(&system);
    DWORD bytes=0;GetLogicalProcessorInformationEx(RelationCache,nullptr,&bytes);
    std::vector<uint8_t> topology(bytes);size_t cache=0;BYTE level=0;
    if(bytes&&GetLogicalProcessorInformationEx(RelationCache,
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(topology.data()),&bytes)) {
        for(size_t offset=0;offset<bytes;) {
            auto* item=reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(topology.data()+offset);
            if(!item->Size||item->Size>bytes-offset)break;
            if(item->Cache.Type==CacheUnified||item->Cache.Type==CacheData) {
                if(item->Cache.Level>level) {level=item->Cache.Level;cache=0;}
                if(item->Cache.Level==level)cache+=item->Cache.CacheSize;
            }
            offset+=item->Size;
        }
    }
    if(!cache)cache=system.dwAllocationGranularity*size_t(system.dwNumberOfProcessors);
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof memory;
    if(GlobalMemoryStatusEx(&memory)&&system.dwNumberOfProcessors)
        cache=(std::min)(cache,static_cast<size_t>(memory.ullAvailPhys/system.dwNumberOfProcessors));
    KoeRustInitializeParallel(cache,system.dwPageSize);
}
static constexpr struct Identity { size_t rva,size;const char* hash; } identities[]={
    {0x456000,180,"8d0cbbf44c1df11044529a87ab4b8661fcdc9b9f9364821d6ec001179eca2a82"},
    {0x8972c0,1146,"ed81ecb0d1d88f61e3fe92da41335fcdcc221d4fd132a0202d3917d8366eae8c"},
    {0x8934f0,1375,"22d62a7b00916b9606fea9d287c644e7f6387fc62472ba5355d27d9e792fa006"},
    {0x44ab30,561,"0aa4c16a015747a68d9d34842462c23f7227c7b009139d915e88ba045c58156f"}
};
static bool verify(uint8_t* image) {
    BCRYPT_ALG_HANDLE algorithm{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    bool valid=true;
    for(const auto& item:identities) {
        unsigned char hash[32]{};char hex[65]{};
        if(BCryptHash(algorithm,nullptr,0,image+item.rva,static_cast<ULONG>(item.size),hash,32)<0) {valid=false;break;}
        for(unsigned i=0;i<32;++i) {hex[i*2]="0123456789abcdef"[hash[i]>>4];hex[i*2+1]="0123456789abcdef"[hash[i]&15];}
        if(std::strcmp(hex,item.hash)) {valid=false;break;}
    }
    BCryptCloseAlgorithmProvider(algorithm,0);return valid;
}
#ifdef KOE_SUBMISSION_AUDIT
static std::atomic<uint64_t> counters[16]{};
template<class T> static T read(const uint8_t* p) {T x;std::memcpy(&x,p,sizeof x);return x;}
using ApplyValues=const uint8_t*(*)(Applier*,const Array<ValueParameter>*,const uint8_t*);
static ApplyValues originalValues;
struct AuditScratch {std::vector<uint8_t> original,shadow,left,right;const uint8_t* textures;uint32_t remaining;};
static thread_local AuditScratch audit;
static void noResize(Array<uint8_t>*,size_t,uint32_t) {}
static void replayTexture(const uint8_t*,int32_t unit,int32_t sampler,uint8_t ms,TextureBinding* output) {
    if(audit.remaining==0) {++counters[7];*output={};return;}
    std::memcpy(output,audit.textures,16);audit.textures+=16;--audit.remaining;
    if(output->textureUnit!=unit||output->samplerUnit!=sampler||output->multisampled!=ms)++counters[7];
}
static const uint8_t* skipValues(const Array<ValueParameter>& params,const uint8_t* cursor) {
    for(;;) {
        auto header=read<uint32_t>(cursor);cursor+=4;auto index=header&65535;if(index==65535)return cursor;
        const auto& p=params.data[index];cursor+=(header>>16)*((p.flags&1)?64:p.columns==1?4:16);
    }
}
static const uint8_t* compareValues(const Array<ValueParameter>& params,const uint8_t* cursor,const uint8_t* end,ConstantBuffer* buffer) {
    if(!buffer||!buffer->data)return skipValues(params,cursor);
    audit.left.assign(buffer->data,buffer->data+buffer->size);audit.right=audit.left;
    ConstantBuffer left=*buffer,right=*buffer;left.data=audit.left.data();right.data=audit.right.data();
    Applier applier{};applier.constant=&left;
    auto expected=originalValues(&applier,&params,cursor);
    auto actual=KoeRustApplyValues(&params,cursor,end,&right);++counters[3];
    if(expected!=actual||left.dirty!=right.dirty||audit.left!=audit.right)++counters[4];
    return expected;
}
static void validate(uint8_t* device,const ProgramParameters* program,const PropertySheet* sheet,Array<uint8_t>* output,uint8_t* shader) {
    audit.original.assign(output->data,output->data+output->size);
    auto begin=audit.original.data(),end=begin+audit.original.size();const uint8_t* cursor=begin;
    cursor=skipValues(program->values,cursor);
    for(size_t i=0;i<program->constants.size;++i)cursor=skipValues(program->constants.data[i].values,cursor);
    audit.remaining=read<uint32_t>(cursor);audit.textures=cursor+4;
    audit.shadow.resize(output->size+65536,0);Array<uint8_t> shadow{audit.shadow.data(),1,0,audit.shadow.size()*2};
    Api replay=api;replay.texture=replayTexture;replay.resize=noResize;
    ++counters[1];
    if(!KoeRustPrepare(program,sheet,&shadow,&replay)||shadow.data!=audit.shadow.data()||shadow.size!=output->size||audit.remaining) {++counters[2];return;}
    cursor+=4+size_t(read<uint32_t>(cursor))*16;
    while(read<uint32_t>(cursor)!=0xffffffff)cursor+=12;cursor+=4;
    auto samplers=read<uint32_t>(cursor);cursor+=4;
    if(samplers) {++counters[2];return;}
    auto n=read<uint32_t>(cursor);cursor+=4;
    for(uint32_t i=0;i<n;++i,cursor+=24) {
        size_t offset=cursor-begin+4;std::memset(audit.original.data()+offset,0,4);std::memset(audit.shadow.data()+offset,0,4);
    }
    if(std::memcmp(audit.original.data(),audit.shadow.data(),output->size))++counters[2];
    int stage=read<int32_t>(shader+8);
    if(stage<0||stage>=7||api.realDevice()!=device)return;
    cursor=compareValues(program->values,output->data,output->data+output->size,nullptr);
    for(size_t i=0;i<program->constants.size;++i) {
        const auto& param=program->constants.data[i];ConstantBuffer* buffer=nullptr;
        if(param.binding>=0&&param.binding<16)buffer=read<ConstantBuffer*>(device+10304+stage*256+param.binding*16);
        cursor=compareValues(param.values,cursor,output->data+output->size,buffer);
    }
}
static void auditSubmission(uint8_t* device,const PropertySheet* sheet) {
    auto* output=reinterpret_cast<Array<uint8_t>*>(device+8616);
    for(size_t stage=0;stage<7;++stage) {
        auto* shader=read<uint8_t*>(device+22976+stage*8);auto* program=read<ProgramParameters*>(device+23032+stage*8);
        if(!shader||!program)continue;
        output->size=0;api.prepare(program,sheet,output);validate(device,program,sheet,output,shader);
        auto table=read<uintptr_t*>(shader);
        reinterpret_cast<void(*)(uint8_t*,const ProgramParameters*,const uint8_t*,uint8_t)>(table[2])(shader,program,output->data,0);
    }
}
#endif
static void submit(uint8_t* device,const PropertySheet* sheet) {
    auto selected=mode.load(std::memory_order_relaxed);
#ifdef KOE_SUBMISSION_AUDIT
    if(selected==3&&sheet->names.size) {++counters[0];auditSubmission(device,sheet);return;}
    if(selected>=4) {
        LARGE_INTEGER begin,end;QueryPerformanceCounter(&begin);
        int result=0;
        if(selected==4)original(device,sheet);
        else if(!(result=KoeRustTrySubmit(device,sheet,&api,selected==5?1:2)))original(device,sheet);
        QueryPerformanceCounter(&end);
        unsigned index=8+(selected-4)*2;
        counters[index].fetch_add(1,std::memory_order_relaxed);
        counters[index+1].fetch_add(end.QuadPart-begin.QuadPart,std::memory_order_relaxed);
        if(selected==6) {
            unsigned prepared=(result>>1)&127,applied=(result>>8)&127;
            while(prepared) {++counters[14];prepared&=prepared-1;}
            while(applied) {++counters[15];applied&=applied-1;}
        }
        return;
    }
#endif
    if(selected&&KoeRustTrySubmit(device,sheet,&api,selected))return;
    original(device,sheet);
}
extern "C" __declspec(dllexport) int InstallShaderSubmission() {
    if(target)return -1;
    auto* image=reinterpret_cast<uint8_t*>(GetModuleHandleW(L"UnityPlayer.dll"));if(!image)return -2;
    if(!verify(image))return -3;
    initializeWorkers();
    api={reinterpret_cast<decltype(Api::resize)>(image+0x3f0de0),reinterpret_cast<decltype(Api::texture)>(image+0x7ec570),
        reinterpret_cast<decltype(Api::prepare)>(image+0x8972c0),reinterpret_cast<decltype(Api::realDevice)>(image+0x891010),
        reinterpret_cast<decltype(Api::applyTextures)>(image+0x8954e0),reinterpret_cast<decltype(Api::applySamplers)>(image+0x895490),
        reinterpret_cast<decltype(Api::applyConstant)>(image+0x44a840),image+0x44ab30};
#ifdef KOE_SUBMISSION_AUDIT
    originalValues=reinterpret_cast<ApplyValues>(image+0x446440);
#endif
    auto result=MH_Initialize();if(result!=MH_OK&&result!=MH_ERROR_ALREADY_INITIALIZED)return -4;
    void* candidate=image+0x456000;
    if(MH_CreateHook(candidate,reinterpret_cast<void*>(&submit),reinterpret_cast<void**>(&original))!=MH_OK)return -5;
    if(MH_EnableHook(candidate)!=MH_OK) {MH_RemoveHook(candidate);return -6;}
    target=candidate;return 0;
}
extern "C" __declspec(dllexport) int SetShaderSubmissionMode(int value) {
#ifdef KOE_SUBMISSION_AUDIT
    constexpr int maximum=6;
#else
    constexpr int maximum=2;
#endif
    if(!target||value<0||value>maximum)return -1;mode.store(uint32_t(value),std::memory_order_relaxed);return 0;
}
extern "C" __declspec(dllexport) void GetShaderSubmissionParallelInfo(uint64_t* output,int count) {
    if(count>0)KoeRustParallelInfo(output,static_cast<size_t>(count));
}
#ifdef KOE_SUBMISSION_AUDIT
extern "C" __declspec(dllexport) void GetShaderSubmissionCounters(uint64_t* output,int count) {
    for(int i=0;i<count&&i<16;++i)output[i]=counters[i].load(std::memory_order_relaxed);
}
#endif
