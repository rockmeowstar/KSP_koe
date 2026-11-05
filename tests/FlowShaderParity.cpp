#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <random>
#include <algorithm>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT h)
{
    if (FAILED(h))
        throw std::runtime_error("D3D failure " + std::to_string(unsigned(h)));
}
int main(int argc, char **argv)
{
    try
    {
        if (argc != 3)
            throw std::runtime_error("Expected candidate and retained reference OpticalD3D11.cpp paths");
        std::ifstream file(argv[1]);
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string cpp = buffer.str();
        auto begin = cpp.find("R\"HLSL(");
        auto end = cpp.find(")HLSL\"", begin);
        if (begin == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Missing production shader");
        std::string candidate = cpp.substr(begin + 7, end - begin - 7);
        std::ifstream oldFile(argv[2]);
        std::stringstream oldBuffer;
        oldBuffer << oldFile.rdbuf();
        std::string oldCpp = oldBuffer.str();
        begin = oldCpp.find("R\"HLSL(");
        end = oldCpp.find(")HLSL\"", begin);
        if (begin == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Missing retained reference shader");
        std::string reference = oldCpp.substr(begin + 7, end - begin - 7);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        auto compile = [](const std::string &s, const char *profile, const D3D_SHADER_MACRO *defines) {
            ComPtr<ID3DBlob> blob, errors;
            HRESULT h = D3DCompile(s.data(), s.size(), nullptr, defines, nullptr, "main", profile,
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
            if (FAILED(h))
                throw std::runtime_error(errors ? static_cast<const char *>(errors->GetBufferPointer())
                                                : "compile failed");
            return blob;
        };
        auto vertex = compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                              "vs_5_0", nullptr);
        ComPtr<ID3D11VertexShader> vs;
        Check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
        constexpr unsigned w = 257, h = 193, gw = (w + 3) / 4, gh = (h + 3) / 4;
        ComPtr<ID3D11Texture2D> textures[4], outputs[2], staging[2];
        ComPtr<ID3D11ShaderResourceView> views[4];
        ComPtr<ID3D11RenderTargetView> targets[2];
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 4; i++)
        {
            if (i == 2)
            {
                td.Width = gw;
                td.Height = gh;
                td.Format = DXGI_FORMAT_R16G16_SINT;
            }
            Check(device->CreateTexture2D(&td, nullptr, &textures[i]));
            Check(device->CreateShaderResourceView(textures[i].Get(), nullptr, &views[i]));
        }
        td.Width = w;
        td.Height = h;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 2; i++)
        {
            Check(device->CreateTexture2D(&td, nullptr, &outputs[i]));
            Check(device->CreateRenderTargetView(outputs[i].Get(), nullptr, &targets[i]));
        }
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto &texture : staging)
            Check(device->CreateTexture2D(&td, nullptr, &texture));
        float dimensions[] = {float(w), float(h), float(gw), float(gh)};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(dimensions);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA data{dimensions, 0, 0};
        ComPtr<ID3D11Buffer> constants;
        Check(device->CreateBuffer(&bd, &data, &constants));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        Check(device->CreateSamplerState(&sd, &sampler));
        D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        auto *c = constants.Get();
        auto *s = sampler.Get();
        context->PSSetConstantBuffers(0, 1, &c);
        context->PSSetSamplers(0, 1, &s);
        std::mt19937 rng(0x4b4f45);
        std::vector<unsigned char> images[2];
        for (auto &image : images)
            image.resize(w * h * 4);
        std::vector<short> flow(gw * gh * 2);
        unsigned long long compared = 0, synthesized = 0;
        for (int conservative = 0; conservative < 2; conservative++)
        {
            D3D_SHADER_MACRO defines[] = {{"KOE_CONSERVATIVE_FLOW", conservative ? "1" : "0"}, {nullptr, nullptr}};
            ComPtr<ID3D11PixelShader> ps[2];
            for (int i = 0; i < 2; i++)
            {
                auto code = compile(i ? candidate : reference, "ps_5_0", defines);
                Check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps[i]));
            }
            for (int mode = 0; mode < 16; mode++)
            {
                for (auto &v : images[0])
                    v = static_cast<unsigned char>(rng());
                images[1] = images[0];
                for (size_t i = 0; i < images[1].size(); i++)
                {
                    if (mode == 1)
                        images[1][i] = static_cast<unsigned char>(rng());
                    else if (mode == 2)
                        images[1][i] = 0;
                    else if (mode == 3)
                        images[1][i] = 255;
                    else if (mode >= 4)
                    {
                        int delta = (int(rng() % unsigned(mode - 2)) - (mode - 3) / 2);
                        images[1][i] =
                            static_cast<unsigned char>(std::max(0, std::min(255, int(images[1][i]) + delta)));
                    }
                }
                int shift = mode == 12 ? 2 : mode == 13 ? -4 : mode == 14 ? 8 : 0;
                if (shift)
                    for (unsigned y = 0; y < h; y++)
                        for (unsigned x = 0; x < w; x++)
                            for (unsigned channel = 0; channel < 4; channel++)
                                images[1][(y * w + x) * 4 + channel] =
                                    images[0][(y * w + unsigned((int(x) - shift + int(w)) % int(w))) * 4 + channel];
                for (int i = 0; i < 2; i++)
                    context->UpdateSubresource(textures[i].Get(), 0, nullptr, images[i].data(), w * 4, 0);
                for (int i = 2; i < 4; i++)
                {
                    for (size_t v = 0; v < flow.size(); v++)
                        flow[v] = shift                     ? (v % 2 ? 0 : short(shift * 32 * (i == 2 ? 1 : -1)))
                                  : mode == 0 || mode == 15 ? 0
                                                            : static_cast<short>(int(rng() % 8193) - 4096);
                    context->UpdateSubresource(textures[i].Get(), 0, nullptr, flow.data(), gw * 4, 0);
                }
                ID3D11ShaderResourceView *resources[] = {views[0].Get(), views[1].Get(), views[2].Get(),
                                                         views[3].Get()};
                for (int i = 0; i < 2; i++)
                {
                    auto *t = targets[i].Get();
                    context->OMSetRenderTargets(1, &t, nullptr);
                    context->PSSetShaderResources(0, 4, resources);
                    context->PSSetShader(ps[i].Get(), nullptr, 0);
                    context->Draw(3, 0);
                    context->OMSetRenderTargets(0, nullptr, nullptr);
                    context->CopyResource(staging[i].Get(), outputs[i].Get());
                }
                D3D11_MAPPED_SUBRESOURCE mapped[2];
                for (int i = 0; i < 2; i++)
                    Check(context->Map(staging[i].Get(), 0, D3D11_MAP_READ, 0, &mapped[i]));
                size_t differences = 0;
                for (unsigned y = 0; y < h; y++)
                    for (unsigned x = 0; x < w * 4; x++)
                        differences += static_cast<unsigned char *>(mapped[0].pData)[y * mapped[0].RowPitch + x] !=
                                       static_cast<unsigned char *>(mapped[1].pData)[y * mapped[1].RowPitch + x];
                if (conservative && shift)
                    for (unsigned y = 0; y < h; y++)
                        for (unsigned x = 0; x < w * 4; x++)
                            synthesized += static_cast<unsigned char *>(mapped[1].pData)[y * mapped[1].RowPitch + x] !=
                                           images[1][y * w * 4 + x];
                for (int i = 0; i < 2; i++)
                    context->Unmap(staging[i].Get(), 0);
                compared += w * h * 4;
                if (differences)
                    throw std::runtime_error("Pixel parity failure: conservative=" + std::to_string(conservative) +
                                             " mode=" + std::to_string(mode) +
                                             " differentBytes=" + std::to_string(differences));
            }
        }
        if (synthesized < 1000)
            throw std::runtime_error("The test did not execute the interpolation branch.");
        std::cout << "Flow shaders: 32 GPU cases passed; " << compared << " bytes compared, " << synthesized
                  << " synthesized.\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
