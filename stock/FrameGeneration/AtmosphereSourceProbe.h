// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>

class AtmosphereSourceProbe
{
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Texture2D> images[3];
    int tone{};float z[4]{};bool saved{};
public:
    void Set(ID3D11Texture2D *c,ID3D11Texture2D *t,ID3D11Texture2D *d,int mode,const float *depth)
    {images[0]=c;images[1]=t;images[2]=d;tone=mode;for(unsigned i=0;i<4;i++)z[i]=depth[i];}
    void Save(ID3D11DeviceContext *context)
    {
        if(saved || !images[0] || !images[1] || !images[2])return;
        wchar_t destination[32768]{};auto length=GetEnvironmentVariableW(L"KOE_INDEPENDENT_SNAPSHOT",destination,32768);
        if(!length || length>=32768)return;
        auto path=std::filesystem::path(destination)/"atmosphere";
        if(std::filesystem::exists(path))return;
        std::filesystem::create_directories(path);
        Ptr<ID3D11Device> device;context->GetDevice(&device);
        std::ofstream info(path/"capture.json");info<<"{\"tone\":"<<tone<<",\"z\":["<<z[0]<<","<<z[1]<<","<<z[2]<<","<<z[3]<<"],\"textures\":[";
        for(unsigned i=0;i<3;i++)
        {
            D3D11_TEXTURE2D_DESC desc{};images[i]->GetDesc(&desc);
            unsigned format=unsigned(desc.Format);
            unsigned bytes=format>=1&&format<=4?16:format>=5&&format<=8?12:
                format>=9&&format<=22?8:format>=23&&format<=47?4:
                format>=48&&format<=59?2:format>=60&&format<=65?1:
                format==87||format==88||format==90||format==91?4:0;
            if(!bytes)throw std::runtime_error("Unsupported atmosphere probe texture format "+std::to_string(format));
            info<<(i?",":"")<<"{\"width\":"<<desc.Width<<",\"height\":"<<desc.Height<<",\"format\":"<<unsigned(desc.Format)<<",\"bytes\":"<<bytes<<"}";
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Ptr<ID3D11Texture2D> staging;
            if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))throw std::runtime_error("Atmosphere probe staging allocation failed");
            context->CopyResource(staging.Get(),images[i].Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))throw std::runtime_error("Atmosphere probe readback failed");
            std::ofstream file(path/(std::to_string(i)+".raw"),std::ios::binary);
            for(unsigned y=0;y<desc.Height;y++)file.write(static_cast<char*>(mapped.pData)+y*mapped.RowPitch,desc.Width*bytes);
            context->Unmap(staging.Get(),0);
            if(!file)throw std::runtime_error("Atmosphere probe write failed");
        }
        info<<"]}";saved=true;
    }
};
