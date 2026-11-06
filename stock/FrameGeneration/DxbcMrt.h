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
struct Operand
{
    size_t token{},index{},end{};unsigned type{};
};
inline Operand ParseOperand(const std::vector<uint32_t> &words,size_t at,size_t end)
{
    if(at>=end)throw std::runtime_error("Missing DXBC operand");
    Operand result;result.token=at;auto token=words[at++];result.type=(token>>12)&255;
    auto extended=token;
    while(extended&0x80000000u)
    {if(at>=end)throw std::runtime_error("Truncated DXBC operand extension");extended=words[at++];}
    unsigned dimensions=(token>>20)&3;
    for(unsigned i=0;i<dimensions;i++)
    {
        unsigned representation=(token>>(22+i*3))&7;
        if(representation==0 || representation==1 || representation==3 || representation==4)
        {
            if(at>=end)throw std::runtime_error("Truncated DXBC index");
            if(i==0)result.index=at;
            at+=representation==1||representation==4?2:1;
        }
        if(representation==2 || representation==3 || representation==4)at=ParseOperand(words,at,end).end;
        if(representation>4)throw std::runtime_error("Unsupported DXBC index");
    }
    if(result.type==4 || result.type==5)
    {
        unsigned components=(token&3)==1?1:(token&3)==2?4:0;
        if(!components)throw std::runtime_error("Unsupported immediate operand");
        at+=components*(result.type==5?2:1);
    }
    if(at>end)throw std::runtime_error("Truncated DXBC operand");result.end=at;return result;
}
inline void RemapOperand(std::vector<uint32_t> &words,size_t at,size_t end,unsigned temps)
{
    auto value=ParseOperand(words,at,end);auto token=words[at++];auto extension=token;
    while(extension&0x80000000u)extension=words[at++];
    unsigned dimensions=(token>>20)&3;
    if(value.type==3)throw std::runtime_error("Indexable sky temporaries are unsupported");
    if(value.type==0 || value.type==2)
    {
        if(dimensions!=1 || ((token>>22)&7)!=0 || !value.index)
            throw std::runtime_error("Invalid sky temporary reference");
        if(words[value.index]>=(value.type==0?temps:1u))throw std::runtime_error("Sky register is out of range");
        words[value.index]+=value.type==0?temps:1;
    }
    for(unsigned i=0;i<dimensions;i++)
    {
        unsigned representation=(token>>(22+i*3))&7;
        if(representation==0 || representation==1 || representation==3 || representation==4)
            at+=representation==1||representation==4?2:1;
        if(representation==2 || representation==3 || representation==4)
        {auto relative=ParseOperand(words,at,end);RemapOperand(words,at,end,temps);at=relative.end;}
    }
}
inline std::vector<unsigned char> DuplicateColor(const void *source,size_t length,bool isolatedSky=false)
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
            unsigned temps=0;size_t fork=0;std::vector<size_t> blocks;
            if(isolatedSky)
                for(size_t at=2;at<words.size();)
                {
                    unsigned op=words[at]&0x7ff;size_t n=(words[at]>>24)&127;
                    if(op==53)n=at+1<words.size()?words[at+1]:0;
                    if(!n || n>words.size()-at)throw std::runtime_error("Invalid sky instruction");
                    if(op==104){if(n!=2)throw std::runtime_error("Invalid temporary declaration");temps=words[at+1];}
                    if(op==31 || op==48 || op==76)blocks.push_back(at);
                    if(op==21 || op==22 || op==23){if(blocks.empty())throw std::runtime_error("Unbalanced sky control flow");blocks.pop_back();}
                    if(op>=69 && op<=74 && !fork)
                    {
                        size_t operand=at+1;while(words[operand-1]&0x80000000u)operand++;
                        for(;operand<at+n;)
                        {
                            auto value=ParseOperand(words,operand,at+n);
                            if(value.type==7 && value.index && words[value.index]==0)fork=blocks.empty()?at:blocks.front();
                            operand=value.end;
                        }
                    }
                    at+=n;
                }
            if(isolatedSky && (!fork || !temps || temps>2048 || !blocks.empty() || (words.back()&0x7ff)!=62))
                throw std::runtime_error("Unsupported independent sky tail");
            std::vector<uint32_t> result{words[0],0};unsigned duplicated=0;
            for(size_t at=2;at<words.size();)
            {
                unsigned op=words[at]&0x7ff;
                size_t size=(words[at]>>24)&127;
                if(op==53){if(at+1>=words.size())throw std::runtime_error("Invalid custom shader data");size=words[at+1];}
                if(!size || size>words.size()-at)throw std::runtime_error("Invalid shader instruction length");
                if(isolatedSky && at==fork)
                    for(unsigned reg=0;reg<temps;reg++)
                        result.insert(result.end(),{0x05000036u,0x001000f2u,reg+temps,0x00100e46u,reg});
                // AI-generated comment: Original instructions and target 0 stay intact. Only the
                // short composition tail is evaluated again for target 1, with background and
                // cloud RGB replaced by neutral inputs. The expensive atmospheric LUT prefix runs once.
                if(isolatedSky && op==62)
                {
                    if(at+size!=words.size())throw std::runtime_error("Sky has an early return");
                    for(size_t tail=fork;tail<at;)
                    {
                        unsigned operation=words[tail]&0x7ff;size_t n=(words[tail]>>24)&127;
                        if(!n || n>at-tail || operation==13 || operation==62 || operation==63 || operation==53)
                            throw std::runtime_error("Unsupported sky tail control instruction");
                        std::vector<uint32_t> copy(words.begin()+tail,words.begin()+tail+n);
                        size_t operand=1;auto extension=copy[0];
                        while(extension&0x80000000u){if(operand>=n)throw std::runtime_error("Invalid sky instruction extension");extension=copy[operand++];}
                        bool neutral=false;Operand destination{};
                        while(operand<n)
                        {
                            auto value=ParseOperand(copy,operand,n);if(!destination.end)destination=value;
                            RemapOperand(copy,operand,n,temps);
                            if(operation>=69 && operation<=74 && value.type==7 && value.index && copy[value.index]<=1)neutral=true;
                            operand=value.end;
                        }
                        if(neutral)
                        {
                            if(destination.type!=0 || !destination.index)throw std::runtime_error("Invalid sky sample destination");
                            result.insert(result.end(),{0x08000036u,copy[destination.token],copy[destination.index],0x00004002u,0u,0u,0u,0x3f800000u});
                        }
                        else result.insert(result.end(),copy.begin(),copy.end());
                        tail+=n;
                    }
                }
                size_t original=result.size();result.insert(result.end(),words.begin()+at,words.begin()+at+size);
                if(isolatedSky && op==104)result[original+1]=temps*2;
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
                        if(output && (!isolatedSky || (op>=88 && op<=106)))
                        {
                            size_t begin=result.size();result.insert(result.end(),words.begin()+at,words.begin()+at+size);
                            result[begin+operand-at+1]=1;duplicated++;
                        }
                    }
                }
                at+=size;
            }
            if(duplicated<(isolatedSky?1u:2u))throw std::runtime_error("Pixel target declaration or writes were not found");
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
