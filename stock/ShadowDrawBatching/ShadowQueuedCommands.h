#pragma once
#include <cstdint>
#include <functional>
#include <stdexcept>

struct ShadowSnapshot {
    const uint8_t* vertices;size_t vertexBytes,stride,position,normal;
    const uint8_t* indices;size_t indexCount,indexStride;int64_t indexBias;
    const uint8_t* constants;size_t constantBytes;
};
struct ShadowQueueData {
    const uint8_t* vertices;size_t vertexBytes;
    const uint32_t* indices;size_t indexCount;
    const uint8_t* constants;size_t constantBytes,objects;
};
extern "C" void* ShadowQueueCreate(size_t,size_t,size_t,size_t);
extern "C" int ShadowQueuePush(void*,uint64_t,const ShadowSnapshot*);
extern "C" int ShadowQueueView(const void*,ShadowQueueData*);
extern "C" void ShadowQueueClear(void*);
extern "C" void ShadowQueueFree(void*);
struct ShadowIndexExtent {uint32_t minimum,maximum;};
extern "C" int ShadowIndexRange(const uint8_t*,size_t,size_t,ShadowIndexExtent*);

class ShadowCommands {
    void* queue;
    std::function<void(const ShadowQueueData&)> emit;
public:
    size_t batches=0,objects=0,boundaries=0;
    ShadowCommands(size_t bankBytes,size_t capacity,size_t vertexBudget,size_t indexBudget,
                   std::function<void(const ShadowQueueData&)> executor)
        : queue(ShadowQueueCreate(bankBytes,capacity,vertexBudget,indexBudget)),emit(std::move(executor)) {
        if(!queue)throw std::runtime_error("shadow queue allocation/limits");
    }
    ShadowCommands(const ShadowCommands&)=delete;
    ShadowCommands& operator=(const ShadowCommands&)=delete;
    ~ShadowCommands(){ShadowQueueFree(queue);}
    void discard(){ShadowQueueClear(queue);}
    void flush(){
        ShadowQueueData data{};
        if(ShadowQueueView(queue,&data)!=0)throw std::runtime_error("shadow queue view failed");
        if(!data.objects)return;
        emit(data);
        ++batches;objects+=data.objects;
        ShadowQueueClear(queue);
    }
    void beforeBoundary(){++boundaries;flush();}
    void push(uint64_t state,const ShadowSnapshot& data){
        int status=ShadowQueuePush(queue,state,&data);
        if(status==0){beforeBoundary();status=ShadowQueuePush(queue,state,&data);}
        if(status!=1)throw std::runtime_error("shadow snapshot rejected");
    }
};
