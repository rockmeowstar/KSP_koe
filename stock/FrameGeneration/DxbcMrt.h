// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include "../../third_party/DxbcHash/DxilHash.h"

namespace DxbcMrt
{
inline uint32_t Read(const unsigned char *p){uint32_t value;memcpy(&value,p,4);return value;}
inline void Write(unsigned char *p,uint32_t value){memcpy(p,&value,4);}
inline std::vector<unsigned char> DuplicateColor(const void *source,size_t length)
{
    const auto *input=static_cast<const unsigned char*>(source);
    if(length<32 || memcmp(input,"DXBC",4) || Read(input+24)>length || Read(input+24)<32)throw std::runtime_error("Invalid DXBC container");
    length=Read(input+24);
    uint32_t count=Read(input+28);if(count>(length-32)/4)throw std::runtime_error("Invalid DXBC chunk table");
    struct Chunk{uint32_t kind;std::vector<unsigned char> bytes;};std::vector<Chunk> chunks;
    bool signature=false,program=false;
    for(unsigned i=0;i<count;i++)
    {
        uint32_t offset=Read(input+32+i*4);
        if(offset>length-8 || Read(input+offset+4)>length-offset-8)throw std::runtime_error("Invalid DXBC chunk bounds");
        uint32_t size=Read(input+offset+4),kind=Read(input+offset);
        Chunk chunk{kind,std::vector<unsigned char>(input+offset+8,input+offset+8+size)};
        if(!memcmp(input+offset,"OSGN",4))
        {
            auto &body=chunk.bytes;
            if(size<32 || Read(body.data())!=1 || Read(body.data()+12)!=0 || Read(body.data()+24)!=0
                || Read(body.data()+20)!=3 || (Read(body.data()+28)&255)!=15)
                throw std::runtime_error("MRT capture requires one float4 target output");
            unsigned name=Read(body.data()+8);
            if(name>=size || size-name<10 || memcmp(body.data()+name,"SV_Target",10))
                throw std::runtime_error("Unsupported pixel output semantic");
            std::vector<unsigned char> result(size+24);memcpy(result.data(),body.data(),32);
            memcpy(result.data()+32,body.data()+8,24);memcpy(result.data()+56,body.data()+32,size-32);
            Write(result.data(),2);Write(result.data()+8,name+24);Write(result.data()+32,name+24);
            Write(result.data()+36,1);Write(result.data()+48,1);body=std::move(result);signature=true;
        }
        if(!memcmp(input+offset,"SHEX",4) || !memcmp(input+offset,"SHDR",4))
        {
            if(size<8 || size%4)throw std::runtime_error("Invalid tokenized shader");
            std::vector<uint32_t> words(size/4);memcpy(words.data(),chunk.bytes.data(),size);
            if((words[0]>>16)!=0 || ((words[0]>>4)&15)<4 || words[1]!=words.size())
                throw std::runtime_error("Only tokenized pixel shaders are supported");
            std::vector<uint32_t> result{words[0],0};unsigned duplicated=0;
            for(size_t at=2;at<words.size();)
            {
                unsigned op=words[at]&0x7ff;
                size_t size=(words[at]>>24)&127;
                if(op==53){if(at+1>=words.size())throw std::runtime_error("Invalid custom shader data");size=words[at+1];}
                if(!size || size>words.size()-at)throw std::runtime_error("Invalid shader instruction length");
                result.insert(result.end(),words.begin()+at,words.begin()+at+size);
                size_t operand=at+1;
                if(op!=53)
                {
                    auto extended=words[at];
                    while(extended&0x80000000u)
                    {if(operand>=at+size)throw std::runtime_error("Invalid extended instruction");extended=words[operand++];}
                    if(operand+1<at+size)
                    {
                        auto token=words[operand];
                        bool output=((token>>12)&255)==2 && ((token>>20)&3)==1 && ((token>>22)&7)==0
                            && !(token&0x80000000u) && words[operand+1]==0;
                        if(output)
                        {
                            size_t begin=result.size();result.insert(result.end(),words.begin()+at,words.begin()+at+size);
                            result[begin+operand-at+1]=1;duplicated++;
                        }
                    }
                }
                at+=size;
            }
            if(duplicated<2)throw std::runtime_error("Pixel target declaration or writes were not found");
            result[1]=static_cast<uint32_t>(result.size());chunk.bytes.resize(result.size()*4);
            memcpy(chunk.bytes.data(),result.data(),chunk.bytes.size());program=true;
        }
        chunks.push_back(std::move(chunk));
    }
    if(!signature || !program)throw std::runtime_error("Unsupported DXBC program or signature");
    std::vector<unsigned char> output(32+count*4);memcpy(output.data(),input,32);memset(output.data()+4,0,16);
    for(unsigned i=0;i<count;i++)
    {
        auto &chunk=chunks[i];size_t at=output.size();Write(output.data()+32+i*4,static_cast<uint32_t>(at));
        output.resize(at+8+chunk.bytes.size());Write(output.data()+at,chunk.kind);Write(output.data()+at+4,static_cast<uint32_t>(chunk.bytes.size()));
        memcpy(output.data()+at+8,chunk.bytes.data(),chunk.bytes.size());while(output.size()%4)output.push_back(0);
    }
    Write(output.data()+24,static_cast<uint32_t>(output.size()));
    ComputeHashRetail(output.data()+20,static_cast<UINT>(output.size()-20),output.data()+4);return output;
}
}
