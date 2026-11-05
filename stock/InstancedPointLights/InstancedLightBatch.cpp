#include "InstancedLightBatch.h"
#include <cstring>
#include <utility>
using Microsoft::WRL::ComPtr;

HRESULT InstancedLightBatch::Initialize(ID3D11Device *device, const void *vs, size_t vsSize, const void *ps,
                                        size_t psSize)
{
    if (!device || !vs || !ps)
        return E_INVALIDARG;
    ComPtr<ID3D11VertexShader> v;
    ComPtr<ID3D11PixelShader> p;
    ComPtr<ID3D11Buffer> c;
    HRESULT hr = device->CreateVertexShader(vs, vsSize, nullptr, &v);
    if (FAILED(hr))
        return hr;
    hr = device->CreatePixelShader(ps, psSize, nullptr, &p);
    if (FAILED(hr))
        return hr;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Record) * MaximumSources;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&desc, nullptr, &c);
    if (FAILED(hr))
        return hr;
    vertex = std::move(v);
    pixel = std::move(p);
    constants = std::move(c);
    return S_OK;
}
HRESULT InstancedLightBatch::Draw(ID3D11DeviceContext *context, const Record *records, unsigned count, unsigned indices,
                                  unsigned startIndex, int baseVertex)
{
    if (!context || !records || !count || count > MaximumSources || !indices || !constants)
        return E_INVALIDARG;
    // AI-generated comment: Geometry/tessellation shaders need their own instance-linkage handling.
    ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    context->GSGetShader(&geometry, nullptr, nullptr);
    context->HSGetShader(&hull, nullptr, nullptr);
    context->DSGetShader(&domain, nullptr, nullptr);
    if (geometry || hull || domain || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return E_NOTIMPL;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
        return hr;
    std::memcpy(mapped.pData, records, count * sizeof(Record));
    context->Unmap(constants.Get(), 0);
    ComPtr<ID3D11VertexShader> oldV;
    ComPtr<ID3D11PixelShader> oldP;
    ID3D11ClassInstance *vc[256]{};
    ID3D11ClassInstance *pc[256]{};
    UINT vn = 256, pn = 256;
    context->VSGetShader(&oldV, vc, &vn);
    context->PSGetShader(&oldP, pc, &pn);
    ComPtr<ID3D11Buffer> oldVC, oldPC;
    context->VSGetConstantBuffers(Slot, 1, &oldVC);
    context->PSGetConstantBuffers(Slot, 1, &oldPC);
    auto *cb = constants.Get();
    context->VSSetConstantBuffers(Slot, 1, &cb);
    context->PSSetConstantBuffers(Slot, 1, &cb);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    context->DrawIndexedInstanced(indices, count, startIndex, baseVertex, 0);
    context->VSSetShader(oldV.Get(), vc, vn);
    context->PSSetShader(oldP.Get(), pc, pn);
    auto *vcb = oldVC.Get();
    auto *pcb = oldPC.Get();
    context->VSSetConstantBuffers(Slot, 1, &vcb);
    context->PSSetConstantBuffers(Slot, 1, &pcb);
    for (UINT i = 0; i < vn; ++i)
        vc[i]->Release();
    for (UINT i = 0; i < pn; ++i)
        pc[i]->Release();
    return S_OK;
}
