#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <fstream>
#include <filesystem>
#include <vector>
#include <iostream>
#include <stdexcept>

static std::vector<char> Read(const wchar_t *path) {
    std::ifstream f(std::filesystem::path(path),std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error("Shader fixture missing");
    auto size=f.tellg();std::vector<char> bytes(static_cast<size_t>(size));f.seekg(0);
    if(!f.read(bytes.data(),static_cast<std::streamsize>(size)))throw std::runtime_error("Shader fixture read failed");
    return bytes;
}
int wmain(int argc,wchar_t **argv) {
    if(argc!=8)return 2;
    HMODULE module=LoadLibraryW(argv[1]);if(!module)return 3;
    auto configure=reinterpret_cast<int(*)(const wchar_t*,const wchar_t*)>(GetProcAddress(module,"ConfigureVisualShaderFixes"));
    auto initialize=reinterpret_cast<int(*)(const wchar_t*,const wchar_t*,int)>(GetProcAddress(module,"InitializeCloudGuards"));
    auto counters=reinterpret_cast<void(*)(unsigned long long*,unsigned)>(GetProcAddress(module,"ReadVisualShaderFixCounters"));
    if(!configure||!initialize||!counters||configure(argv[2],argv[3])||initialize(argv[4],argv[5],0))return 4;
    ID3D11Device *device{};ID3D11DeviceContext *context{};D3D_FEATURE_LEVEL level{};
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)))return 5;
    for(int i=6;i<8;i++) {
        auto bytes=Read(argv[i]);ID3D11PixelShader *shader{};
        if(FAILED(device->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&shader))||!shader)return 6;
        shader->Release();
    }
    unsigned long long info[4]{};counters(info,4);
    context->Release();device->Release();
    if(info[0]!=1||info[1]!=1||info[2]||info[3])return 7;
    std::cout<<"Visual shader integration: both original DXBC shaders replaced through the real D3D11 hook; GPU shader creation passed.\n";
    return 0;
}
