// SPDX-License-Identifier: MIT
#pragma once
#include "ShaderPatch.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>

namespace KoeVisualFix {
struct Program {
    size_t length;
    const char *sourceHash;
    const char *targetHash;
    std::vector<unsigned char> recipe;
    std::vector<unsigned char> bytes;
};
static Program programs[] = {
    {7852,"548026640ee8ccff59c525767748a79bd46da82373b443c951352f8d8d816571","84a85c517d81b8c4065e63ad5b21734e0af7329ca5f163bb005cc644575877f8",{},{}},
    {39416,"e0cc0c4f040b5c4785b73132256a925baae5d88bff3d7a5dec5630ffb85cd772","1267ffe63a65cdaaf3ec9a67838a206d9023005dacded77707d84f94d9289816",{},{}}
};
static std::atomic<bool> ready{};
static std::atomic<unsigned long long> replacements[2]{}, rejected[2]{};
static std::mutex mutex;

static bool Read(const wchar_t *path, std::vector<unsigned char> &bytes) {
    if(!path || !*path)return true;
    std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);
    if(!file)return false;
    auto length=file.tellg();
    if(length<88 || length>1024*1024)return false;
    bytes.resize(static_cast<size_t>(length));file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) &&
        std::memcmp(bytes.data(),"KOEPATCH",8)==0;
}

static int Configure(const wchar_t *wet, const wchar_t *horizon) {
    std::lock_guard<std::mutex> lock(mutex);
    if(ready.load())return -1;
    std::vector<unsigned char> first,second;
    try {
        if(!Read(wet,first)||!Read(horizon,second))return -2;
        programs[0].recipe=std::move(first);programs[1].recipe=std::move(second);
        ready.store(true,std::memory_order_release);return 0;
    }catch(...){return -3;}
}

template<class Digest>
static bool Resolve(const void *source,size_t length,const void *&bytes,size_t &size,unsigned &selected,Digest hash) {
    if(!ready.load(std::memory_order_acquire)||!source)return false;
    for(unsigned i=0;i<2;i++) {
        auto &p=programs[i];
        if(length!=p.length || p.recipe.empty() || !hash(source,length,p.sourceHash))continue;
        std::lock_guard<std::mutex> lock(mutex);
        if(p.bytes.empty()) {
            if(!KoeShaderPatch::Apply(p.recipe,source,length,p.bytes)||!hash(p.bytes.data(),p.bytes.size(),p.targetHash)) {
                p.bytes.clear();rejected[i]++;return false;
            }
        }
        bytes=p.bytes.data();size=p.bytes.size();selected=i;return true;
    }
    return false;
}

static void Created(unsigned selected,bool success) {
    if(success)replacements[selected]++;else rejected[selected]++;
}
}
