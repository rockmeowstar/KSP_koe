#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <memory>
#include <cstdint>
#include <filesystem>
#include "nvOpticalFlowD3D11.h"

class OpticalD3D11
{
public:
    struct Ticket{bool repeated{},eligible{};std::uint64_t fenceValue{};};
    OpticalD3D11(ID3D11Device* device,unsigned width,unsigned height,const std::filesystem::path& unused,bool conservative=true);
    ~OpticalD3D11();
    bool Process(ID3D11Texture2D* current,double timestamp);
    std::shared_ptr<Ticket> Submit(ID3D11Texture2D* current,double timestamp);
    bool Completed(const std::shared_ptr<Ticket>& ticket) const;
    bool Wait(unsigned milliseconds=5000);
    ID3D11Texture2D* Output() const{return output.Get();}
    double LastCallMilliseconds() const{return callMs;}
    ID3D11Texture2D* DiagnosticFlow(unsigned direction) const{return flow[direction&1].Get();}
private:
    void Cleanup();
    void CopyImage(ID3D11ShaderResourceView* source,ID3D11RenderTargetView* target);
    HMODULE module{};NvOFHandle session{};NV_OF_D3D11_API_FUNCTION_LIST api{};
    Microsoft::WRL::ComPtr<ID3D11Device5> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputs[2],flow[2],output;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> imageViews[2],flowViews[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputView;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> inputTargets[2];
    Microsoft::WRL::ComPtr<ID3D11Resource> externalResource;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> externalView;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> copyPS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    NvOFGPUBufferHandle inputHandles[2]{},flowHandles[2]{};
    unsigned width{},height{},next{};bool previous{};double lastTimestamp{},callMs{};std::uint64_t sequence{};
};
