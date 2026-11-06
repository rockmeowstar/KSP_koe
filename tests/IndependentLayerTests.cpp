#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "IndependentLayers.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;
constexpr unsigned W=128,H=96,L=LayerReconstruction::Count;
static void Check(HRESULT value){if(FAILED(value))throw std::runtime_error("Probe D3D11 failure");}
static uint16_t Half(float value)
{
    uint32_t bits;memcpy(&bits,&value,4);unsigned sign=(bits>>16)&0x8000;
    int exponent=int((bits>>23)&255)-127+15;unsigned fraction=bits&0x7fffff;
    if(exponent<=0)return uint16_t(sign);
    if(exponent>=31)return uint16_t(sign|0x7c00);
    unsigned rounded=fraction+0x1000;
    if(rounded&0x800000){rounded=0;exponent++;}
    return uint16_t(sign|(unsigned(exponent)<<10)|(rounded>>13));
}
struct Pixel{float r,g,b,a;};
static std::vector<Pixel> Sources(float frame,bool perturb)
{
    std::vector<Pixel> pixels(W*H*L);
    for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++)
    {
        auto at=[&](unsigned layer)->Pixel&{return pixels[(layer*H+y)*W+x];};
        at(0)={.18f,.24f,.3f,1};
        int source=int(x)-int(frame*8);
        if(source>=32 && source<80 && y>=20 && y<76)
        {float t=float((source*13+int(y)*7)%19)/30;at(5)={.16f+t,.2f+.5f*t,.28f+.3f*t,1};}
        int cloud=int(x)+int(frame*4);
        if(cloud>8 && cloud<110 && y>6 && y<85)at(6)={.08f,.11f,.14f,.25f};
        if(perturb){at(0)={.7f,.1f,.4f,1};at(6)={.2f,.02f,.1f,.4f};at(7)={.02f,.03f,.04f,.1f};}
    }
    return pixels;
}
static std::vector<uint8_t> World(const std::vector<Pixel> &source)
{
    std::vector<uint8_t> image(W*H*4);
    for(unsigned i=0;i<W*H;i++)
    {
        float c[3]{};
        for(unsigned layer=0;layer<L;layer++)
        {
            const auto &p=source[layer*W*H+i];
            c[0]=p.r+(1-p.a)*c[0];c[1]=p.g+(1-p.a)*c[1];c[2]=p.b+(1-p.a)*c[2];
        }
        for(unsigned k=0;k<3;k++)image[i*4+k]=uint8_t(std::round((std::max)(0.f,(std::min)(1.f,c[k]))*255));
        image[i*4+3]=255;
    }
    return image;
}
static ComPtr<ID3D11Texture2D> Layers(ID3D11Device *device,const std::vector<Pixel> &source)
{
    constexpr unsigned slices=LayerReconstruction::PackedSlices;
    std::vector<uint16_t> pixels(W*H*slices*4);
    for(unsigned i=0;i<W*H;i++)
    {
        Pixel opaque=source[i];unsigned owner=0;
        for(unsigned layer=1;layer<6;layer++)if(source[layer*W*H+i].a>.5f){opaque=source[layer*W*H+i];owner=layer;}
        const unsigned kinds[]={0,2,3,4,5,1};
        Pixel values[]={opaque,{kinds[owner]/255.f,0,0,1},source[6*W*H+i],source[7*W*H+i],{0,0,0,0},{1,1,1,0}};
        for(unsigned slice=0;slice<slices;slice++)
        {
            auto &value=values[slice];size_t at=(slice*W*H+i)*4;
            pixels[at]=Half(value.r);pixels[at+1]=Half(value.g);pixels[at+2]=Half(value.b);pixels[at+3]=Half(value.a);
        }
    }
    D3D11_TEXTURE2D_DESC desc{};desc.Width=W;desc.Height=H;desc.MipLevels=desc.SampleDesc.Count=1;
    desc.ArraySize=slices;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data[slices]{};
    for(unsigned i=0;i<slices;i++){data[i].pSysMem=pixels.data()+i*W*H*4;data[i].SysMemPitch=W*8;}
    ComPtr<ID3D11Texture2D> result;Check(device->CreateTexture2D(&desc,data,&result));return result;
}
static std::vector<uint8_t> Read(ID3D11Device *device,ID3D11DeviceContext *context,ID3D11Texture2D *image,unsigned slice=0)
{
    D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);unsigned bytes=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    unsigned index=slice;desc.ArraySize=1;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;Check(device->CreateTexture2D(&desc,nullptr,&copy));
    context->CopySubresourceRegion(copy.Get(),0,0,0,0,image,index,nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::vector<uint8_t> data(desc.Width*desc.Height*bytes);
    for(unsigned y=0;y<desc.Height;y++)memcpy(data.data()+y*desc.Width*bytes,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,desc.Width*bytes);
    context->Unmap(copy.Get(),0);return data;
}
int main()
{
    try
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        IndependentLayers layers(device.Get(),W,H);
        auto before=Sources(0,false),after=Sources(1,false);
        auto first=Layers(device.Get(),before),last=Layers(device.Get(),after);
        layers.Submit(0,first.Get(),false,1);layers.Submit(1,last.Get(),false,1);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=W;desc.Height=H;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        auto previous=World(before),current=World(after);
        D3D11_SUBRESOURCE_DATA initial{previous.data(),W*4,0};ComPtr<ID3D11Texture2D> p,c,output,diagnostic;
        Check(device->CreateTexture2D(&desc,&initial,&p));initial.pSysMem=current.data();Check(device->CreateTexture2D(&desc,&initial,&c));
        Check(device->CreateTexture2D(&desc,nullptr,&output));
        ComPtr<ID3D11ShaderResourceView> pv,cv;Check(device->CreateShaderResourceView(p.Get(),nullptr,&pv));Check(device->CreateShaderResourceView(c.Get(),nullptr,&cv));
        ComPtr<ID3D11UnorderedAccessView> out,counts,diag;
        Check(device->CreateUnorderedAccessView(output.Get(),nullptr,&out));
        desc.ArraySize=L;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;Check(device->CreateTexture2D(&desc,nullptr,&diagnostic));
        Check(device->CreateUnorderedAccessView(diagnostic.Get(),nullptr,&diag));
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=48;buffer.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        buffer.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;buffer.StructureByteStride=4;
        ComPtr<ID3D11Buffer> counters;Check(device->CreateBuffer(&buffer,nullptr,&counters));Check(device->CreateUnorderedAccessView(counters.Get(),nullptr,&counts));
        desc.Width=(W/2+3)/4;desc.Height=(H/2+3)/4;desc.ArraySize=L*2;desc.Format=DXGI_FORMAT_R16G16_SINT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<int16_t> flow(desc.Width*desc.Height*2);D3D11_SUBRESOURCE_DATA flowData[L*2]{};
        std::vector<std::vector<int16_t>> slices(L*2,flow);
        for(unsigned i=0;i<L*2;i++)
        {
            int dx=i/2==5?128:(i/2==6?-64:0);if(i&1)dx=-dx;
            for(size_t j=0;j<slices[i].size();j+=2)slices[i][j]=int16_t(dx);
            flowData[i]={slices[i].data(),desc.Width*4,0};
        }
        ComPtr<ID3D11Texture2D> vectors;Check(device->CreateTexture2D(&desc,flowData,&vectors));layers.SetTestVectors(vectors.Get());
        layers.Generate(0,1,.5f,pv.Get(),cv.Get(),out.Get(),counts.Get(),diag.Get());
        auto craft=Read(device.Get(),context.Get(),diagnostic.Get(),5);
        auto result=Read(device.Get(),context.Get(),output.Get());
        unsigned changed=0;
        for(unsigned i=0;i<W*H;i++)if(memcmp(result.data()+i*4,current.data()+i*4,3))changed++;
        if(changed<500)throw std::runtime_error("Layer reconstruction did not replace moving endpoint pixels");
        auto truth=World(Sources(.5f,false));unsigned correct=0;
        for(unsigned i=0;i<W*H;i++)
        {
            bool same=true;
            for(unsigned channel=0;channel<3;channel++)same=same && std::abs(int(result[i*4+channel])-int(truth[i*4+channel]))<=1;
            if(same)correct++;
        }
        if(correct*100<W*H*95)throw std::runtime_error("Known-translation reconstruction failed color accuracy");
        // Even valid isolated sources can differ by half-float/filmic rounding.
        // A static real pixel must not become a synthetic "new" pixel for that reason.
        for(unsigned i=0;i<W*H;i++)
            if(!memcmp(previous.data()+i*4,current.data()+i*4,3) && memcmp(result.data()+i*4,current.data()+i*4,4))
                throw std::runtime_error("An unchanged endpoint pixel was rewritten by reconstruction");
        context->UpdateSubresource(p.Get(),0,nullptr,current.data(),W*4,0);
        layers.Submit(0,last.Get(),false,1);layers.Submit(1,last.Get(),false,1);
        layers.Generate(0,1,.5f,pv.Get(),cv.Get(),out.Get(),counts.Get());
        if(Read(device.Get(),context.Get(),output.Get())!=current)
            throw std::runtime_error("Identical endpoints generated a repeated image with artificial color drift");
        context->UpdateSubresource(p.Get(),0,nullptr,previous.data(),W*4,0);
        layers.Submit(0,first.Get(),false,1);layers.Submit(1,last.Get(),false,1);
        auto corrupted=current;unsigned pixel=40*W+40;
        corrupted[pixel*4]=170;corrupted[pixel*4+1]=35;corrupted[pixel*4+2]=80;
        context->UpdateSubresource(c.Get(),0,nullptr,corrupted.data(),W*4,0);
        layers.Submit(1,last.Get(),false,1);
        layers.Generate(0,1,.5f,pv.Get(),cv.Get(),out.Get(),counts.Get(),diag.Get());
        auto rejected=Read(device.Get(),context.Get(),output.Get());
        if(memcmp(rejected.data()+pixel*4,corrupted.data()+pixel*4,4))
            throw std::runtime_error("Source validation reused a previous frame's acceptance mask");
        context->UpdateSubresource(c.Get(),0,nullptr,current.data(),W*4,0);
        layers.Submit(1,last.Get(),false,1);
        layers.Generate(0,1,.5f,pv.Get(),cv.Get(),out.Get(),counts.Get(),diag.Get());
        if(Read(device.Get(),context.Get(),output.Get())!=result)
            throw std::runtime_error("Source validation remained stale when presence feedback slots were occupied");
        layers.Estimate(0,1);
        auto firstFlow=Read(device.Get(),context.Get(),layers.DiagnosticVectors(),10);
        auto otherBefore=Layers(device.Get(),Sources(0,true)),otherAfter=Layers(device.Get(),Sources(1,true));
        layers.Submit(0,otherBefore.Get(),false,1);layers.Submit(1,otherAfter.Get(),false,1);
        layers.SetTestVectors(vectors.Get());
        layers.Generate(0,1,.5f,pv.Get(),cv.Get(),out.Get(),counts.Get(),diag.Get(),false);
        auto isolated=Read(device.Get(),context.Get(),diagnostic.Get(),5);
        if(craft!=isolated)throw std::runtime_error("Craft reconstruction borrowed another layer's colors");
        // The hardware path must keep each active layer's inputs independent.
        layers.Estimate(0,1);
        auto secondFlow=Read(device.Get(),context.Get(),layers.DiagnosticVectors(),10);
        if(firstFlow!=secondFlow)throw std::runtime_error("Craft optical flow changed when other layers changed");
        layers.Generate(0,1,.5f,pv.Get(),cv.Get(),out.Get(),counts.Get(),diag.Get(),false);
        Read(device.Get(),context.Get(),output.Get());
        std::cout<<"INDEPENDENT_LAYER_GPU_PROBE_PASS changed="<<changed<<" known_translation_color_pass="<<correct<<"/"<<W*H
                 <<" isolated_craft_bytes="<<craft.size()<<" independent_flow_bytes="<<firstFlow.size()<<" active_layers="<<layers.ActiveMask()<<"\n";
        return 0;
    }
    catch(const std::exception &error){std::cerr<<error.what()<<"\n";return 1;}
}
