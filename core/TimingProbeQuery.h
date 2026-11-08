#pragma once
#include <d3d11.h>
#include <wrl/client.h>
static const GUID koeTimingProbeQueryId={0x4dc96ba4,0x3e25,0x477d,{0x91,0xd8,0x2b,0x9c,0x6b,0x51,0x5a,0x70}};
static bool IsKoeTimingProbeQuery(ID3D11Asynchronous* value){
    if(!value)return false;UINT marker=0,bytes=sizeof marker;
    if(FAILED(value->GetPrivateData(koeTimingProbeQueryId,&bytes,&marker))||marker!=1)return false;
    Microsoft::WRL::ComPtr<ID3D11Query> query;if(FAILED(value->QueryInterface(IID_PPV_ARGS(&query))))return false;
    D3D11_QUERY_DESC desc{};query->GetDesc(&desc);
    return desc.Query==D3D11_QUERY_TIMESTAMP||desc.Query==D3D11_QUERY_TIMESTAMP_DISJOINT;
}
