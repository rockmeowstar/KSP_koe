#pragma once
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
static const GUID cloudUsageId={0xdc714760,0x6acd,0x4489,{0xa8,0x69,0x6a,0xce,0x39,0x3a,0x86,0xee}};
static const GUID cloudVertexCodeId={0xf83c9aa8,0x1cad,0x4670,{0xaa,0x21,0x08,0x9a,0xf4,0xf3,0x2b,0x56}};
struct CloudShaderUsage {
    uint32_t constants{},samplers{};uint64_t resources[2]{};
    bool Resource(unsigned slot)const{return (resources[slot/64]>>(slot%64))&1;}
};
static void TagCloudUsage(ID3D11DeviceChild* shader,const void* bytes,size_t size){
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    if(FAILED(D3DReflect(bytes,size,IID_PPV_ARGS(&reflection))))return;
    D3D11_SHADER_DESC desc{};if(FAILED(reflection->GetDesc(&desc)))return;
    CloudShaderUsage usage;
    for(UINT i=0;i<desc.BoundResources;i++){
        D3D11_SHADER_INPUT_BIND_DESC bind{};if(FAILED(reflection->GetResourceBindingDesc(i,&bind)))return;
        if(bind.BindCount==0||bind.BindCount>128)return;
        for(UINT n=0;n<bind.BindCount;n++){
            UINT slot=bind.BindPoint+n;if(slot<bind.BindPoint)return;
            switch(bind.Type){
                case D3D_SIT_CBUFFER:if(slot>=14)return;usage.constants|=1u<<slot;break;
                case D3D_SIT_SAMPLER:if(slot>=16)return;usage.samplers|=1u<<slot;break;
                case D3D_SIT_TEXTURE:case D3D_SIT_TBUFFER:case D3D_SIT_STRUCTURED:case D3D_SIT_BYTEADDRESS:
                    if(slot>=128)return;usage.resources[slot/64]|=uint64_t(1)<<(slot%64);break;
                default:return;
            }
        }
    }
    shader->SetPrivateData(cloudUsageId,sizeof usage,&usage);
}
