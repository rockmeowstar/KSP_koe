// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <stdexcept>

class PresentedFrameSample
{
    inline static bool done{};
public:
    static void Capture(ID3D11Device *device,ID3D11DeviceContext *context,ID3D11Texture2D *before,
        ID3D11Texture2D *generated,ID3D11Texture2D *after,double timestamp,unsigned phase,unsigned phases,unsigned changed)
    {
        if(done)return;
        wchar_t root[32768]{};auto count=GetEnvironmentVariableW(L"KOE_INDEPENDENT_SNAPSHOT",root,32768);
        if(!count || count>=32768)return;
        auto path=std::filesystem::path(root)/"presented";done=true;
        if(std::filesystem::exists(path))return;
        std::filesystem::create_directories(path);
        auto begin=std::chrono::steady_clock::now();
        try
        {
            ID3D11Texture2D *images[]={before,generated,after};D3D11_TEXTURE2D_DESC desc{};before->GetDesc(&desc);
            unsigned width=desc.Width,height=desc.Height,format=unsigned(desc.Format);
            for(unsigned i=0;i<3;i++)
            {
                images[i]->GetDesc(&desc);
                if(desc.Width!=width || desc.Height!=height || unsigned(desc.Format)!=format || desc.ArraySize!=1
                    || !(format==27 || format==28 || format==29 || format==87 || format==90 || format==91))
                    throw std::runtime_error("Unexpected presented frame format");
                desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
                if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))throw std::runtime_error("Presented frame staging allocation failed");
                context->CopyResource(staging.Get(),images[i]);D3D11_MAPPED_SUBRESOURCE mapped{};
                if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))throw std::runtime_error("Presented frame readback failed");
                std::ofstream file(path/(std::to_string(i)+".raw"),std::ios::binary);
                for(unsigned y=0;y<height;y++)file.write(static_cast<char*>(mapped.pData)+y*mapped.RowPitch,width*4);
                context->Unmap(staging.Get(),0);if(!file)throw std::runtime_error("Presented frame write failed");
            }
            std::ofstream info(path/"capture.json");
            info<<"{\"width\":"<<width<<",\"height\":"<<height<<",\"format\":"<<format
                <<",\"timestamp\":"<<timestamp<<",\"phase\":"<<phase<<",\"phases\":"<<phases<<",\"changedFinalPixels\":"<<changed
                <<",\"readbackMs\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<"}";
        }
        catch(const std::exception &error){std::ofstream(path/"capture-error.txt")<<error.what();}
    }
};
