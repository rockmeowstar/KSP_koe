#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <stdexcept>

static const GUID cloudQueryCountId={0x89061b0b,0x95a5,0x4390,{0xa4,0x14,0x15,0x3f,0x60,0x9a,0x9b,0xf1}};
class CloudQueryCount final : public IUnknown {
    std::atomic<ULONG> references{1};
public:
    size_t active=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output)override{
        if(!output)return E_POINTER;*output=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
    ULONG STDMETHODCALLTYPE Release()override{auto count=--references;if(!count)delete this;return count;}
    static Microsoft::WRL::ComPtr<CloudQueryCount> Get(ID3D11DeviceContext* context,bool create){
        IUnknown* stored{};UINT bytes=sizeof stored;Microsoft::WRL::ComPtr<CloudQueryCount> result;
        if(SUCCEEDED(context->GetPrivateData(cloudQueryCountId,&bytes,&stored))&&stored){result.Attach(static_cast<CloudQueryCount*>(stored));return result;}
        if(!create)return result;
        result.Attach(new CloudQueryCount());
        if(FAILED(context->SetPrivateDataInterface(cloudQueryCountId,result.Get())))throw std::runtime_error("Cloud query tracking unavailable");
        return result;
    }
};
static bool CloudQueryChangesCounts(ID3D11Asynchronous* value){
    if(!value)return true;
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    if(FAILED(value->QueryInterface(IID_PPV_ARGS(&query))))return true;
    D3D11_QUERY_DESC desc{};query->GetDesc(&desc);
    return desc.Query!=D3D11_QUERY_TIMESTAMP_DISJOINT&&desc.Query!=D3D11_QUERY_TIMESTAMP&&desc.Query!=D3D11_QUERY_EVENT;
}
