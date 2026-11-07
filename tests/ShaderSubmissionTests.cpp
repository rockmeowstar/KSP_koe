#define NOMINMAX
#include <windows.h>
#undef PropertySheet
#include <MinHook.h>
#include "UnityAbi.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>
#include <chrono>
using namespace ShaderSubmission;
template<class T> Array<T> view(std::vector<T>& v) { return {v.data(),1,v.size(),v.capacity()*2}; }
template<class T> T read(const void* p) { T v; memcpy(&v,p,sizeof v); return v; }
template<class T> void store(void* p,T v) { memcpy(p,&v,sizeof v); }
static uint8_t* image;
static uint8_t* currentDevice;
static std::vector<uint64_t> commands;
static size_t checks=0;
static void require(bool b,const char* s) { ++checks; if(!b) throw std::runtime_error(s); }
static void resizeBytes(Array<uint8_t>* a,size_t n,uint32_t) {
    size_t capacity=std::max(n,(a->capacityTag>>1)*2+64);
    a->data=static_cast<uint8_t*>(realloc(a->data,capacity));
    if(!a->data) abort(); a->capacityTag=capacity*2;
}
static void growConstants(Array<uint8_t>* a) {
    size_t capacity=std::max<size_t>(4,(a->capacityTag>>1)*2);
    a->data=static_cast<uint8_t*>(realloc(a->data,capacity*24));
    if(!a->data) abort(); a->capacityTag=capacity*2;
}
static void destroyArray(Array<uint8_t>* a) { free(a->data); }
static void texture(const uint8_t* env,int32_t unit,int32_t sampler,uint8_t ms,TextureBinding* out) {
    *out={unit,sampler,ms,read<int32_t>(env)};
}
static uint8_t* realDevice() { return currentDevice; }
static const uint8_t* textures(const uint8_t* p,uint8_t*,int32_t stage) {
    auto n=read<uint32_t>(p); p+=4; commands.push_back(0x100000000ull|uint32_t(stage));commands.push_back(n);
    for(uint32_t i=0;i<n;++i,p+=16) {commands.push_back(read<uint64_t>(p)); commands.push_back(read<uint64_t>(p+8));} return p;
}
static const uint8_t* samplers(const uint8_t* p,uint8_t*,int32_t stage) {
    auto n=read<uint32_t>(p); commands.push_back(0x200000000ull|uint32_t(stage));commands.push_back(n);
    require(n==0,"unexpected sampler override");return p+4;
}
static void constant(Applier* a,const uint8_t* p) {
    commands.push_back(0x300000000ull|uint32_t(a->stage));commands.push_back(read<uint32_t>(p));
    commands.push_back(read<uint64_t>(p+8)); commands.push_back(read<uint64_t>(p+16));
    commands.push_back(a->constant? a->constant->size:0);
}
static void structured(uint8_t*,int32_t stage,int32_t binding,uint64_t buffer) {
    commands.push_back(0x400000000ull|uint32_t(stage));commands.push_back(uint32_t(binding));commands.push_back(buffer);
}
template<class T> T at(size_t rva) { return reinterpret_cast<T>(image+rva); }
static void hook(size_t rva,void* target) {
    require(MH_CreateHook(image+rva,target,nullptr)==MH_OK,"create fixture boundary hook");
    require(MH_EnableHook(image+rva)==MH_OK,"enable fixture boundary hook");
}
struct Corpus {
    std::vector<uint32_t> names,descs;
    std::vector<uint8_t> data;
    std::vector<ValueParameter> values, cbValues;
    std::vector<ConstantParameter> constants;
    std::vector<TextureParameter> textures;
    std::vector<BufferParameter> buffers;
    PropertySheet sheet{}; ProgramParameters program{};
    void property(uint32_t name,const void* bytes,size_t n,uint32_t count=1) {
        names.push_back(name); descs.push_back(uint32_t(data.size())|(count<<20));
        if(n) {auto p=static_cast<const uint8_t*>(bytes);data.insert(data.end(),p,p+n);}
    }
    void sync() {
        sheet.names=view(names);sheet.descriptions=view(descs);sheet.values=view(data);
        program.values=view(values);program.constants=view(constants);program.textures=view(textures);program.buffers=view(buffers);
        for(auto& c:constants) c.values=view(cbValues);
    }
    explicit Corpus(std::mt19937& rng,int serial) {
        int count=serial%2? (serial%4==1?1:3):18+int(rng()%70);
        uint32_t dest=0;
        for(int kind=0;kind<6;++kind) {
            sheet.starts[kind]=int32_t(names.size());
            for(int j=0;j<count;++j) {
                uint32_t id=uint32_t(j%std::max(1,count-2))+100;
                if(kind<3) {
                    unsigned width=kind==0?4:kind==1?16:64;
                    unsigned elements=unsigned(rng()%7);
                    std::vector<uint32_t> words(width/4*elements);
                    for(auto& x:words) x=rng();
                    property(id,words.data(),width*elements,elements);
                    ValueParameter p{id,int32_t(dest),int32_t(rng()%8),0,4,uint8_t(kind==0?1:4),uint8_t(kind==2),0};
                    if(serial%17==0)p.arraySize=-1;
                    if(serial%19==0)p.arraySize=65536;
                    values.push_back(p);cbValues.push_back(p);dest+=512;
                } else if(kind==3) {
                    int32_t env[6]={j%4?j+1:0,int32_t(rng()%6+1),0,0,0,0};property(id,env,sizeof env);
                    textures.push_back({id,j,j+1,uint8_t(j%2),{},j%2?1:2});
                } else if(kind==4) {
                    uint64_t cb[2]={j%3?uint64_t(j+1):0,uint64_t(j*16)|(uint64_t(128)<<32)};property(id,cb,sizeof cb);
                    ConstantParameter cp{};cp.name=id;cp.size=dest;cp.binding=j%16;cp.kind=j%3==0?2:0;
                    cp.floats=cp.vectors=cp.matrices=count;constants.push_back(cp);
                } else {
                    uint64_t b=j%2?uint64_t(j+1):0;property(id,&b,8);buffers.push_back({id,j,{}});
                }
            }
        }
        sheet.starts[6]=int32_t(names.size());program.floats=program.vectors=program.matrices=count;
        // Missing names exercise lookup termination on both sides of each type range.
        if(serial%3==0) for(auto& p:values) p.name+=1;
        sync();
    }
};
static std::vector<uint8_t> canonical(Array<uint8_t>& a,const ProgramParameters& p,size_t prefix=0) {
    std::vector<uint8_t> bytes(a.data,a.data+a.size);size_t cursor=prefix;
    auto skip=[&](const Array<ValueParameter>& params) {
        for(;;) {auto h=read<uint32_t>(&bytes.at(cursor));cursor+=4;unsigned index=h&65535;if(index==65535)break;
            const auto& v=params.data[index];cursor+=(h>>16)*((v.flags&1)?64:v.columns==1?4:16);}
    };
    skip(p.values);for(size_t i=0;i<p.constants.size;++i)skip(p.constants.data[i].values);
    auto n=read<uint32_t>(&bytes.at(cursor));cursor+=4+size_t(n)*16;
    while(read<uint32_t>(&bytes.at(cursor))!=0xffffffff)cursor+=12;cursor+=4;
    require(read<uint32_t>(&bytes.at(cursor))==0,"sampler count");cursor+=4;
    n=read<uint32_t>(&bytes.at(cursor));cursor+=4;
    for(uint32_t i=0;i<n;++i,cursor+=24)std::fill(bytes.begin()+cursor+4,bytes.begin()+cursor+8,0);
    require(cursor==bytes.size(),"packed stream end");return bytes;
}
static void prepareCases(Api& api) {
    std::mt19937 rng(0x7a35e91);
    for(int i=0;i<2000;++i) {
        Corpus c(rng,i);
        if(i%13==0)c.program.maximumBytes=0xffffffffu;
        if(i%7==0&&c.sheet.starts[1]>=12) {
            c.names.resize(12);c.descs.resize(12);for(int kind=1;kind<7;++kind)c.sheet.starts[kind]=12;c.sync();
        }
        Array<uint8_t> original{},rust{};
        original.label=rust.label=1;
        size_t prefix=i%5==0?13:0;
        if(prefix) {
            resizeBytes(&original,prefix,0);resizeBytes(&rust,prefix,0);
            memset(original.data,0x5a,prefix);memset(rust.data,0x5a,prefix);original.size=rust.size=prefix;
        }
        api.prepare(&c.program,&c.sheet,&original);
        require(KoeRustPrepare(&c.program,&c.sheet,&rust,&api)==1,"Rust prepare rejected valid corpus");
        require(canonical(original,c.program,prefix)==canonical(rust,c.program,prefix),"prepare bytes differ");
        free(original.data);free(rust.data);
    }
}
static void bulkPreparation(Api& api) {
    std::mt19937 rng(919);Corpus c(rng,2);
    ConstantParameter inlineBuffer=c.constants[0];inlineBuffer.name=0;inlineBuffer.kind=0;
    c.constants.assign(1024,inlineBuffer);c.program.maximumBytes=0xffffffffu;c.sync();
    Array<uint8_t> original{},rust{};original.label=rust.label=1;
    api.prepare(&c.program,&c.sheet,&original);
    require(KoeRustPrepare(&c.program,&c.sheet,&rust,&api)==1,"bulk preparation accepted");
    require(canonical(original,c.program)==canonical(rust,c.program),"parallel bulk bytes differ");
    uint64_t info[7]{};KoeRustParallelInfo(info,7);
    printf("BULK bytes=%zu logical=%llu workers=%llu threshold=%llu batches=%llu parallel_bytes=%llu participating_workers=%llu\n",
        rust.size,info[0],info[1],info[2],info[4],info[5],info[6]);
    free(original.data);free(rust.data);
}
static void valueCases() {
    using Apply=const uint8_t*(*)(Applier*,const Array<ValueParameter>*,const uint8_t*);
    auto original=at<Apply>(0x446440); std::mt19937 rng(0xe175d);
    const uint32_t special[]={0,0x80000000,0x7f800000,0xff800000,0x7fc12345,0x7fa00001,1,0x4f000000,0xcf000000};
    for(int test=0;test<24000;++test) {
        unsigned cols=1+rng()%4;bool matrix=test%5==0;unsigned count=rng()%22;
        ValueParameter p{1,16,0,int32_t(rng()%6),4,uint8_t(cols),uint8_t(matrix),0};Array<ValueParameter> params{&p,1,1,2};
        unsigned width=matrix?64:cols==1?4:16;
        std::vector<uint8_t> stream(8+count*width);store(stream.data(),uint32_t(count<<16));store(stream.data()+4+count*width,0xffffu);
        for(size_t j=4;j+4<=stream.size()-4;j+=4)store(&stream[j],test%3?uint32_t(rng()):special[(j/4+test)%9]);
        std::vector<uint8_t> left(4096),right;
        for(size_t j=0;j<left.size();j+=4)store(&left[j],uint32_t(rng()));
        if(test%2==0)for(unsigned k=0;k<count;++k)memcpy(&left[16+k*(matrix?64:16)],&stream[4+k*width],matrix?64:cols*4);
        right=left; ConstantBuffer a{},b{};a.data=left.data();b.data=right.data();a.size=b.size=4096;a.dirty=b.dirty=uint8_t(test%3==0);
        Applier ap{};ap.constant=&a;
        const uint8_t* end=original(&ap,&params,stream.data());
        const uint8_t* next=KoeRustApplyValues(&params,stream.data(),stream.data()+stream.size(),&b);
        if(left!=right||a.dirty!=b.dirty) {
            printf("value mismatch test=%d matrix=%d columns=%u count=%u scalar=%d dirty=%u/%u\n",test,matrix,cols,count,p.scalarType,a.dirty,b.dirty);
            require(false,"constant-buffer bytes/dirty differ");
        }
        require(end==next && next==stream.data()+stream.size(),"value stream cursor differs");
    }
}
static void outerCases(Api& api) {
    std::mt19937 rng(0x51b3);using Root=void(*)(uint8_t*,const PropertySheet*);auto original=at<Root>(0x456000);
    std::array<uintptr_t,324> vtable{};vtable[0xa18/8]=reinterpret_cast<uintptr_t>(&structured);
    void* shaderTable[3]={nullptr,nullptr,const_cast<void*>(api.shaderApply)};
    struct Shader {void** table;int32_t stage;int32_t padding;};
    for(int test=0;test<150;++test) {
        Corpus c(rng,test);std::vector<uint8_t> device(24576);currentDevice=device.data();store(device.data(),vtable.data());
        std::array<Shader,7> shaders{};std::array<ConstantBuffer,112> cb{};std::array<std::vector<uint8_t>,112> buffers;
        for(unsigned stage=0;stage<7;++stage) {
            shaders[stage]={shaderTable,int32_t(stage),0};
            if((stage+test)%3) {store(device.data()+22976+stage*8,&shaders[stage]);store(device.data()+23032+stage*8,&c.program);}
            for(unsigned slot=0;slot<16;++slot) {auto k=stage*16+slot;buffers[k].resize(150000,0x7a);cb[k].data=buffers[k].data();cb[k].size=uint32_t(buffers[k].size());store(device.data()+10304+stage*256+slot*16,&cb[k]);}
        }
        commands.clear();original(device.data(),&c.sheet);auto expected=commands;
        std::array<std::vector<uint8_t>,112> expectedBuffers=buffers;std::array<uint8_t,112> dirty{};
        for(unsigned k=0;k<112;++k) {dirty[k]=cb[k].dirty;cb[k].dirty=0;std::fill(buffers[k].begin(),buffers[k].end(),0x7a);}
        commands.clear();int result=KoeRustTrySubmit(device.data(),&c.sheet,&api,2);
        require(result>0,"Rust outer rejected valid corpus");
        unsigned expectedMask=0;for(unsigned stage=0;stage<7;++stage)if((stage+test)%3)expectedMask|=1<<stage;
        require(unsigned(result)==(1u|(expectedMask<<1)|(expectedMask<<8)),"Rust stage coverage differs");
        require(commands==expected,"driver command order/arguments differ");
        for(unsigned k=0;k<112;++k)require(buffers[k]==expectedBuffers[k]&&cb[k].dirty==dirty[k],"outer constant buffer differs");
        free(reinterpret_cast<Array<uint8_t>*>(device.data()+8616)->data);
    }
}
static void benchmark(Api& api) {
    std::mt19937 rng(81);Corpus denseCase(rng,2),compactCase(rng,1);
    for(auto* c:{&compactCase,&denseCase}) {
        Array<uint8_t> out{};out.label=1;resizeBytes(&out,1000000,0);out.size=0;
        constexpr unsigned iterations=15000;
        auto run=[&](bool rust) {
            auto begin=std::chrono::steady_clock::now();
            for(unsigned i=0;i<iterations;++i) {out.size=0;if(rust)KoeRustPrepare(&c->program,&c->sheet,&out,&api);else api.prepare(&c->program,&c->sheet,&out);}
            return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/iterations;
        };
        run(false);run(true);
        for(int pass=0;pass<3;++pass) {double a=run(false),b=run(true);printf("BENCH prepare properties=%zu original_ns=%.1f rust_ns=%.1f speedup=%.3f\n",c->names.size(),a,b,a/b);}
        free(out.data);
    }
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)return 2;image=reinterpret_cast<uint8_t*>(LoadLibraryA(argv[1]));require(image!=nullptr,"load UnityPlayer");
        require(MH_Initialize()==MH_OK,"initialize fixture hooks");
        hook(0x3f0de0,reinterpret_cast<void*>(&resizeBytes));hook(0xdd770,reinterpret_cast<void*>(&growConstants));
        hook(0xb3d60,reinterpret_cast<void*>(&destroyArray));hook(0x7ec570,reinterpret_cast<void*>(&texture));
        hook(0x891010,reinterpret_cast<void*>(&realDevice));hook(0x8954e0,reinterpret_cast<void*>(&textures));
        hook(0x895490,reinterpret_cast<void*>(&samplers));hook(0x44a840,reinterpret_cast<void*>(&constant));
        SYSTEM_INFO system{};GetSystemInfo(&system);
        KoeRustInitializeParallel(size_t(system.dwAllocationGranularity)*system.dwNumberOfProcessors,system.dwPageSize);
        Api api{resizeBytes,texture,at<decltype(Api::prepare)>(0x8972c0),realDevice,textures,samplers,constant,image+0x44ab30};
        prepareCases(api);printf("PASS prepare 2000 randomized shader/property layouts\n");
        bulkPreparation(api);
        valueCases();printf("PASS value application 24000 IEEE/integer/vector/matrix cases\n");
        outerCases(api);printf("PASS complete submission 150 seven-stage layouts\n");
        benchmark(api);
        require(MH_DisableHook(MH_ALL_HOOKS)==MH_OK,"restore Unity functions");require(MH_Uninitialize()==MH_OK,"release fixture hooks");
        printf("PASS checks=%zu\n",checks);return 0;
    } catch(const std::exception& e) {fprintf(stderr,"FAIL %s checks=%zu\n",e.what(),checks);MH_Uninitialize();return 1;}
}
