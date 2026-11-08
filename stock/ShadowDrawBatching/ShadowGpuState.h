#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <stdexcept>

struct ShadowGpuState {
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ranges;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> shader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex,index,constants[4];
    UINT stride=0,vertexOffset=0,indexOffset=0,first[4]{},count[4]{};
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool restored=false;
    ShadowGpuState()=default;
    explicit ShadowGpuState(ID3D11DeviceContext* value):context(value){
        UINT instances=0;context->VSGetShader(&shader,nullptr,&instances);
        if(instances)throw std::runtime_error("linked shader classes are outside the shadow prototype");
        context->IAGetInputLayout(&layout);context->IAGetVertexBuffers(0,1,&vertex,&stride,&vertexOffset);
        context->IAGetIndexBuffer(&index,&format,&indexOffset);
        ID3D11Buffer* buffers[4]{};
        if(SUCCEEDED(context.As(&ranges)))ranges->VSGetConstantBuffers1(0,4,buffers,first,count);
        else context->VSGetConstantBuffers(0,4,buffers);
        for(unsigned i=0;i<4;++i)constants[i].Attach(buffers[i]);
    }
    void restore(){
        auto vb=vertex.Get();ID3D11Buffer* buffers[4];for(unsigned i=0;i<4;++i)buffers[i]=constants[i].Get();
        context->IASetInputLayout(layout.Get());context->IASetVertexBuffers(0,1,&vb,&stride,&vertexOffset);
        context->IASetIndexBuffer(index.Get(),format,indexOffset);context->VSSetShader(shader.Get(),nullptr,0);
        if(ranges)ranges->VSSetConstantBuffers1(0,4,buffers,first,count);else context->VSSetConstantBuffers(0,4,buffers);
        restored=true;
    }
    bool equals(const ShadowGpuState& b)const{
        if(shader.Get()!=b.shader.Get()||layout.Get()!=b.layout.Get()||vertex.Get()!=b.vertex.Get()||index.Get()!=b.index.Get()
           ||stride!=b.stride||vertexOffset!=b.vertexOffset||indexOffset!=b.indexOffset||format!=b.format)return false;
        for(unsigned i=0;i<4;++i)if(constants[i].Get()!=b.constants[i].Get()||first[i]!=b.first[i]||count[i]!=b.count[i])return false;
        return true;
    }
    ~ShadowGpuState(){if(context&&!restored)restore();}
};
