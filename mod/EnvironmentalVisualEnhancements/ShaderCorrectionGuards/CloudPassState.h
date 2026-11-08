#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstring>
#include "CloudShaderUsage.h"
using Microsoft::WRL::ComPtr;
template<class T,size_t N> struct CloudRefs {
    T* data[N]{};
    ~CloudRefs(){for(auto* p:data)if(p)p->Release();}
    bool Same(const CloudRefs& other)const{return std::equal(data,data+N,other.data);}
};
struct CloudPassState {
    CloudShaderUsage vertexUsage,pixelUsage;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> index;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11RasterizerState> raster;
    CloudRefs<ID3D11Buffer,32> vertices;
    CloudRefs<ID3D11Buffer,14> vc,pc;
    CloudRefs<ID3D11ShaderResourceView,128> vr,pr;
    CloudRefs<ID3D11SamplerState,16> vsam,psam;
    CloudRefs<ID3D11RenderTargetView,8> targets;
    UINT strides[32]{},offsets[32]{},indexOffset{},mask{},stencil{};
    UINT vfirst[14]{},vcount[14]{},pfirst[14]{},pcount[14]{};
    DXGI_FORMAT format{};D3D11_PRIMITIVE_TOPOLOGY topology{};
    float factor[4]{};
    UINT viewportCount=16,scissorCount=16;
    D3D11_VIEWPORT viewports[16]{};D3D11_RECT scissors[16]{};
    bool Capture(ID3D11DeviceContext1* c){
        CloudRefs<ID3D11ClassInstance,256> classes;UINT count=256;
        c->VSGetShader(&vs,classes.data,&count);if(count)return false;
        count=256;c->PSGetShader(&ps,classes.data,&count);if(count)return false;
        UINT bytes=sizeof vertexUsage;if(!vs||FAILED(vs->GetPrivateData(cloudUsageId,&bytes,&vertexUsage)))return false;
        bytes=sizeof pixelUsage;if(!ps||FAILED(ps->GetPrivateData(cloudUsageId,&bytes,&pixelUsage)))return false;
        ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
        c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&hs,nullptr,nullptr);c->DSGetShader(&ds,nullptr,nullptr);
        ComPtr<ID3D11Predicate> predicate;BOOL value{};c->GetPredication(&predicate,&value);
        CloudRefs<ID3D11Buffer,4> so;c->SOGetTargets(4,so.data);
        CloudRefs<ID3D11UnorderedAccessView,8> uav;c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uav.data);
        if(gs||hs||ds||predicate||std::any_of(so.data,so.data+4,[](auto*p){return p!=nullptr;})||std::any_of(uav.data,uav.data+8,[](auto*p){return p!=nullptr;}))return false;
        c->IAGetInputLayout(&layout);c->IAGetVertexBuffers(0,32,vertices.data,strides,offsets);
        c->IAGetIndexBuffer(&index,&format,&indexOffset);c->IAGetPrimitiveTopology(&topology);
        c->VSGetConstantBuffers1(0,14,vc.data,vfirst,vcount);c->PSGetConstantBuffers1(0,14,pc.data,pfirst,pcount);
        c->VSGetShaderResources(0,128,vr.data);c->PSGetShaderResources(0,128,pr.data);
        c->VSGetSamplers(0,16,vsam.data);c->PSGetSamplers(0,16,psam.data);
        c->OMGetRenderTargets(8,targets.data,&depthView);c->OMGetBlendState(&blend,factor,&mask);c->OMGetDepthStencilState(&depth,&stencil);
        c->RSGetState(&raster);c->RSGetViewports(&viewportCount,viewports);c->RSGetScissorRects(&scissorCount,scissors);
        return true;
    }
    void Apply(ID3D11DeviceContext1* c)const{
        c->ClearState();c->IASetInputLayout(layout.Get());c->IASetVertexBuffers(0,32,vertices.data,strides,offsets);
        c->IASetIndexBuffer(index.Get(),format,indexOffset);c->IASetPrimitiveTopology(topology);
        c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);
        c->VSSetConstantBuffers1(0,14,vc.data,vfirst,vcount);c->PSSetConstantBuffers1(0,14,pc.data,pfirst,pcount);
        c->VSSetShaderResources(0,128,vr.data);c->PSSetShaderResources(0,128,pr.data);
        c->VSSetSamplers(0,16,vsam.data);c->PSSetSamplers(0,16,psam.data);
        c->OMSetRenderTargets(8,targets.data,depthView.Get());c->OMSetBlendState(blend.Get(),factor,mask);c->OMSetDepthStencilState(depth.Get(),stencil);
        c->RSSetState(raster.Get());c->RSSetViewports(viewportCount,viewports);c->RSSetScissorRects(scissorCount,scissors);
    }
    bool OpaqueFloatTarget()const{
        if(!targets.data[0]||depthView||mask!=UINT_MAX||topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return false;
        for(unsigned i=1;i<8;i++)if(targets.data[i])return false;
        // No depth/stencil view is bound, so the depth-state flags cannot affect this pass.
        if(blend){D3D11_BLEND_DESC d{};blend->GetDesc(&d);if(d.AlphaToCoverageEnable)return false;for(unsigned i=0;i<2;i++){auto& r=d.RenderTarget[d.IndependentBlendEnable?i:0];if(r.BlendEnable||r.RenderTargetWriteMask!=15)return false;}}
        D3D11_RENDER_TARGET_VIEW_DESC d{};targets.data[0]->GetDesc(&d);
        return d.Format==DXGI_FORMAT_R32G32B32A32_FLOAT&&d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D&&d.Texture2D.MipSlice==0&&viewportCount==1;
    }
    unsigned GeometryDifference(const CloudPassState& b)const{
        unsigned difference=0;
        for(unsigned i=0;i<14;i++)if(vertexUsage.constants&(1u<<i))if(vc.data[i]!=b.vc.data[i]||vfirst[i]!=b.vfirst[i]||vcount[i]!=b.vcount[i])difference|=64;
        for(unsigned i=0;i<128;i++)if(vertexUsage.Resource(i)&&vr.data[i]!=b.vr.data[i])difference|=128;
        for(unsigned i=0;i<16;i++)if((vertexUsage.samplers&(1u<<i))&&vsam.data[i]!=b.vsam.data[i])difference|=256;
        if(vs!=b.vs){unsigned char aHash[32]{},bHash[32]{};UINT aSize=32,bSize=32;
            if(FAILED(vs->GetPrivateData(cloudVertexCodeId,&aSize,aHash))||FAILED(b.vs->GetPrivateData(cloudVertexCodeId,&bSize,bHash))||aSize!=32||bSize!=32||memcmp(aHash,bHash,32))difference|=1;
        }
        if(layout!=b.layout)difference|=2;if(index!=b.index||format!=b.format||indexOffset!=b.indexOffset||topology!=b.topology)difference|=4;
        if(!vertices.Same(b.vertices))difference|=8;if(memcmp(strides,b.strides,sizeof strides)||memcmp(offsets,b.offsets,sizeof offsets))difference|=16;
        if(raster!=b.raster)difference|=32;
        if(viewportCount!=b.viewportCount||memcmp(viewports,b.viewports,viewportCount*sizeof(D3D11_VIEWPORT)))difference|=512;
        if(scissorCount!=b.scissorCount||memcmp(scissors,b.scissors,scissorCount*sizeof(D3D11_RECT)))difference|=1024;
        return difference;
    }
    bool SameGeometry(const CloudPassState& b)const{return GeometryDifference(b)==0;}
};
