#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <emmintrin.h>

// Resource private data owns the mirror; the mirror never owns the resource.
class ShadowBufferMirror final : public IUnknown {
public:
    struct Budget {
        const size_t limit;
        std::atomic<size_t> used{0}, live{0};
        explicit Budget(size_t bytes):limit(bytes){}
        bool acquire(size_t bytes) {
            auto before=used.load(std::memory_order_relaxed);
            do {if(bytes>limit-before)return false;}
            while(!used.compare_exchange_weak(before,before+bytes));
            return true;
        }
    };
private:
    inline static const GUID key={0x73bf53d1,0xacd2,0x4975,{0x96,0x9b,0x82,0x67,0x2e,0x1d,0xae,0x61}};
    std::atomic<ULONG> refs{1};
    inline static std::atomic<uint64_t> nextIdentity{1};
    const uint64_t identity=nextIdentity.fetch_add(1);
    std::shared_ptr<Budget> budget;
    std::vector<unsigned char> bytes;
    unsigned char* writeBytes=nullptr;
    size_t writeAllocation=0;
    bool watchWrites=false;
    std::vector<void*> dirtyPages;
    bool writing=false,discardWrite=false;
    uint64_t writeGeneration=UINT64_MAX;
    std::mutex mutex;
    bool valid=false;
    uint64_t generation=0;
    bool recoveryAttempted=false;
    std::atomic<bool> observed{false};
    ShadowBufferMirror(std::shared_ptr<Budget> owner,size_t size):budget(std::move(owner)),bytes(size) {
        ++budget->live;
    }
    ~ShadowBufferMirror() {
        if(writeBytes)VirtualFree(writeBytes,0,MEM_RELEASE);
        budget->used.fetch_sub(bytes.size()+writeAllocation); --budget->live;
    }
    bool contains(size_t offset,size_t count)const {return offset<=bytes.size()&&count<=bytes.size()-offset;}
public:
    void observe(){observed.store(true,std::memory_order_relaxed);}
    bool needed()const{return observed.load(std::memory_order_relaxed);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output)override {
        if(!output)return E_POINTER;
        *output=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override {auto left=--refs;if(!left)delete this;return left;}
    static Microsoft::WRL::ComPtr<ShadowBufferMirror> get(ID3D11Buffer* buffer) {
        Microsoft::WRL::ComPtr<ShadowBufferMirror> result;
        if(!buffer)return result;
        UINT size=sizeof(IUnknown*);IUnknown* value=nullptr;
        if(SUCCEEDED(buffer->GetPrivateData(key,&size,&value))&&size==sizeof(value)&&value)
            result.Attach(static_cast<ShadowBufferMirror*>(value));
        else if(value)value->Release();
        return result;
    }
    // Call once from CreateBuffer, before publishing the buffer to callers.
    static bool attach(ID3D11Buffer* buffer,const std::shared_ptr<Budget>& owner,const void* initial) noexcept {
        if(!buffer||!owner||get(buffer))return false;
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
        if(!owner->acquire(desc.ByteWidth))return false;
        ShadowBufferMirror* mirror=nullptr;
        try {mirror=new ShadowBufferMirror(owner,desc.ByteWidth);}
        catch(...) {owner->used.fetch_sub(desc.ByteWidth);return false;}
        if(initial)mirror->upload(0,initial,desc.ByteWidth);
        auto result=buffer->SetPrivateDataInterface(key,mirror);
        mirror->Release();
        return SUCCEEDED(result);
    }
    bool upload(size_t offset,const void* data,size_t count) {
        std::lock_guard<std::mutex> lock(mutex);
        if(valid&&contains(offset,count)&&(!count||data)&&(!count||std::memcmp(bytes.data()+offset,data,count)==0))return true;
        ++generation;
        if(!contains(offset,count)||(count&&!data)) {valid=false;return false;}
        if(count)std::memcpy(bytes.data()+offset,data,count);
        if(offset==0&&count==bytes.size())valid=true;
        return true;
    }
    // Never read a driver's write-only mapping: it may be uncached PCIe memory.
    void* beginWrite(D3D11_MAP type) {
        std::lock_guard<std::mutex> lock(mutex);
        if(writing)return nullptr;
        bool discard=type==D3D11_MAP_WRITE_DISCARD;
        if(!discard&&type!=D3D11_MAP_WRITE_NO_OVERWRITE&&type!=D3D11_MAP_WRITE)return nullptr;
        if(!discard&&!valid)return nullptr;
        if(!writeBytes) {
            SYSTEM_INFO info{};GetSystemInfo(&info);
            watchWrites=bytes.size()>info.dwPageSize;
            size_t size=(bytes.size()+info.dwPageSize-1)/info.dwPageSize*info.dwPageSize;
            if(!budget->acquire(size))return nullptr;
            try{dirtyPages.resize(size/info.dwPageSize);}
            catch(...){budget->used.fetch_sub(size);return nullptr;}
            writeBytes=static_cast<unsigned char*>(VirtualAlloc(nullptr,size,MEM_COMMIT|MEM_RESERVE|(watchWrites?MEM_WRITE_WATCH:0),PAGE_READWRITE));
            if(!writeBytes){budget->used.fetch_sub(size);return nullptr;}
            writeAllocation=size;
        }
        if(!discard&&writeGeneration!=generation) {
            std::memcpy(writeBytes,bytes.data(),bytes.size());if(watchWrites)ResetWriteWatch(writeBytes,writeAllocation);
        }
        discardWrite=discard;writing=true;
        return writeBytes;
    }
    size_t finishWrite(void* destination,bool maskedStores=false) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!writing||!destination)return 0;
        size_t copied=0;
        bool changed=false;
        if(discardWrite) {
            std::memcpy(destination,writeBytes,bytes.size());copied=bytes.size();
            changed=!valid||std::memcmp(bytes.data(),writeBytes,bytes.size())!=0;
            if(changed)std::memcpy(bytes.data(),writeBytes,bytes.size());
            if(watchWrites)ResetWriteWatch(writeBytes,writeAllocation);
        }else {
            // Preserve NO_OVERWRITE: unchanged bytes may still be read by the GPU.
            auto scan=[&](size_t begin,size_t end) {
            for(size_t at=begin;at<end;) {
                if(end-at>=16) {
                    auto before=_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes.data()+at));
                    auto after=_mm_loadu_si128(reinterpret_cast<const __m128i*>(writeBytes+at));
                    auto equal=_mm_cmpeq_epi8(before,after);
                    unsigned mask=unsigned(_mm_movemask_epi8(equal));
                    if(mask==0xFFFF){at+=16;continue;}
                    if(maskedStores) {
                        _mm_maskmoveu_si128(after,_mm_xor_si128(equal,_mm_set1_epi8(-1)),static_cast<char*>(destination)+at);
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(bytes.data()+at),after);
                        unsigned bits=mask^0xFFFF;
                        bits=bits-((bits>>1)&0x5555);bits=(bits&0x3333)+((bits>>2)&0x3333);
                        bits=(bits+(bits>>4))&0x0F0F;copied+=(bits+(bits>>8))&0x1F;
                        at+=16;continue;
                    }
                }
                if(bytes[at]==writeBytes[at]){++at;continue;}
                size_t span=at++;
                while(at<end&&bytes[at]!=writeBytes[at])++at;
                std::memcpy(static_cast<unsigned char*>(destination)+span,writeBytes+span,at-span);
                std::memcpy(bytes.data()+span,writeBytes+span,at-span);
                copied+=at-span;
            }
            };
            ULONG_PTR count=dirtyPages.size();ULONG granularity=0;
            if(watchWrites&&GetWriteWatch(WRITE_WATCH_FLAG_RESET,writeBytes,writeAllocation,dirtyPages.data(),&count,&granularity)==0&&granularity) {
                for(size_t i=0;i<count;++i) {
                    size_t start=static_cast<unsigned char*>(dirtyPages[i])-writeBytes;
                    if(start<bytes.size())scan(start,(std::min)(bytes.size(),start+granularity));
                }
            }else {scan(0,bytes.size());if(watchWrites)ResetWriteWatch(writeBytes,writeAllocation);}
            changed=copied!=0;
        }
        if(changed)++generation;
        if(maskedStores&&copied)_mm_sfence();
        writeGeneration=generation;valid=true;writing=false;return copied;
    }
    void invalidate() {std::lock_guard<std::mutex> lock(mutex);valid=false;++generation;}
    bool stamp(uint64_t& id,uint64_t& version) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!valid)return false;id=identity;version=generation;return true;
    }
    bool claimInitialRecovery() {
        std::lock_guard<std::mutex> lock(mutex);
        if(recoveryAttempted)return false;recoveryAttempted=true;return true;
    }
    bool snapshot(size_t offset,size_t count,void* output,uint64_t& version) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!valid||!contains(offset,count)||(count&&!output))return false;
        if(count)std::memcpy(output,bytes.data()+offset,count);
        version=generation;return true;
    }
    bool copyFrom(ShadowBufferMirror* source,size_t destination,size_t offset,size_t count) {
        if(!source) {invalidate();return false;}
        if(source==this) {invalidate();return false;} // Conservative fallback for aliased copies.
        std::scoped_lock lock(mutex,source->mutex);
        if(valid&&source->valid&&contains(destination,count)&&source->contains(offset,count)&&
           (!count||std::memcmp(bytes.data()+destination,source->bytes.data()+offset,count)==0))return true;
        ++generation;
        if(!source->valid||!contains(destination,count)||!source->contains(offset,count)) {
            valid=false;return false;
        }
        if(count)std::memcpy(bytes.data()+destination,source->bytes.data()+offset,count);
        if(destination==0&&count==bytes.size())valid=true;
        return true;
    }
};
