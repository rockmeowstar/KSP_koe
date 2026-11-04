// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ivan and KerbalOverEngineering contributors
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace KoeShaderPatch
{
    inline bool Digest(const void* data,size_t size,const unsigned char* expected)
    {
        if(size>0xffffffffULL)return false;
        BCRYPT_ALG_HANDLE algorithm{};
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
        unsigned char hash[32];
        bool valid=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(static_cast<const unsigned char*>(data)),static_cast<ULONG>(size),hash,32)>=0 && !std::memcmp(hash,expected,32);
        BCryptCloseAlgorithmProvider(algorithm,0);return valid;
    }
    // Receives original bytecode from the installed mod's D3D shader creation.
    // A recipe contains source-copy ranges and small KOE-authored literal edits,
    // never a complete source shader. No compiler or SDK is needed at runtime.
    inline bool Apply(const std::vector<unsigned char>& patch,const void* source,size_t sourceSize,std::vector<unsigned char>& result)
    {
        result.clear();size_t cursor=8;
        if(patch.size()<88 || std::memcmp(patch.data(),"KOEPATCH",8))return false;
        auto word=[&](uint32_t& output){if(cursor>patch.size()||patch.size()-cursor<4)return false;std::memcpy(&output,patch.data()+cursor,4);cursor+=4;return true;};
        uint32_t version,expectedSource,targetSize,count;
        if(!word(version)||!word(expectedSource)||!word(targetSize)||!word(count))return false;
        if(version!=1||expectedSource!=sourceSize||targetSize<32||targetSize>1024*1024||count>65536)return false;
        const auto* sourceHash=patch.data()+cursor;cursor+=32;
        const auto* targetHash=patch.data()+cursor;cursor+=32;
        if(!Digest(source,sourceSize,sourceHash))return false;
        result.reserve(targetSize);
        for(uint32_t i=0;i<count;i++)
        {
            if(cursor>=patch.size()){result.clear();return false;}
            unsigned operation=patch[cursor++];uint32_t offset=0,length=0;
            if(operation==0){if(!word(offset)||!word(length)||offset>sourceSize||length>sourceSize-offset){result.clear();return false;}}
            else if(operation==1){if(!word(length)||cursor>patch.size()||length>patch.size()-cursor){result.clear();return false;}}
            else {result.clear();return false;}
            if(result.size()>targetSize||length>targetSize-result.size()){result.clear();return false;}
            const auto* bytes=operation==0?static_cast<const unsigned char*>(source)+offset:patch.data()+cursor;
            result.insert(result.end(),bytes,bytes+length);
            if(operation==1)cursor+=length;
        }
        if(cursor!=patch.size()||result.size()!=targetSize||!Digest(result.data(),result.size(),targetHash)){result.clear();return false;}
        return true;
    }
}
