// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <map>
#include <cstdint>

class LayerTiming
{
    struct Sample
    {
        Microsoft::WRL::ComPtr<ID3D11Query> clock, begin, end;
        std::uint64_t frame{};
        unsigned kind{};
        bool pending{}, open{};
    };
    struct Frame { double milliseconds[3]{}; bool merged{}, incomplete{}; };
    Sample samples[32];
    std::map<std::uint64_t, Frame> frames;
  public:
    double milliseconds[3]{};
    void Poll(ID3D11DeviceContext *context)
    {
        for (auto &sample : samples)
        {
            if (!sample.pending || sample.open) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
            UINT64 begin{}, end{};
            if (context->GetData(sample.clock.Get(), &clock, sizeof(clock), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
                || context->GetData(sample.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
                || context->GetData(sample.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (!clock.Disjoint && clock.Frequency && end >= begin)
            {
                auto &frame = frames[sample.frame];
                frame.milliseconds[sample.kind] += 1000.0 * double(end - begin) / double(clock.Frequency);
                if (sample.kind == 2) frame.merged = true;
            }
            sample.pending = false;
        }
        for (auto it = frames.begin(); it != frames.end();)
        {
            bool pending = false;
            for (const auto &sample : samples)
                if (sample.pending && sample.frame == it->first) pending = true;
            if (it->second.merged && !pending)
            {
                if(!it->second.incomplete)
                    for (unsigned i=0;i<3;++i) milliseconds[i]=it->second.milliseconds[i];
                it=frames.erase(it);
            }
            else ++it;
        }
        while (frames.size()>32) frames.erase(frames.begin());
    }
    int Begin(ID3D11Device *device, ID3D11DeviceContext *context, unsigned kind, std::uint64_t frame)
    {
        auto &result=frames[frame];
        for (int i=0;i<32;++i)
        {
            auto &sample=samples[i];
            if(sample.pending)continue;
            if(!sample.clock)
            {
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
                if(FAILED(device->CreateQuery(&desc,&sample.clock))){result.incomplete=true;return -1;}
                desc.Query=D3D11_QUERY_TIMESTAMP;
                if(FAILED(device->CreateQuery(&desc,&sample.begin))||FAILED(device->CreateQuery(&desc,&sample.end)))
                {sample.clock.Reset();result.incomplete=true;return -1;}
            }
            sample.kind=kind;sample.frame=frame;sample.pending=sample.open=true;
            context->Begin(sample.clock.Get());context->End(sample.begin.Get());
            return i;
        }
        result.incomplete=true;return -1;
    }
    void End(ID3D11DeviceContext *context,int index)
    {
        if(index<0)return;
        auto &sample=samples[index];
        if(!sample.open)return;
        context->End(sample.end.Get());context->End(sample.clock.Get());sample.open=false;
    }
};
