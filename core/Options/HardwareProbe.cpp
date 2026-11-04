#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "nvOpticalFlowD3D11.h"
using Microsoft::WRL::ComPtr;

// Capability query on a separate device. Installs no hooks and changes no Unity state.
extern "C" __declspec(dllexport) int ProbeOpticalFlow(unsigned vendor, unsigned deviceId, unsigned width, unsigned height)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return -1;
    ComPtr<IDXGIAdapter1> selected;
    for (unsigned i=0;;++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) && description.VendorId==vendor && description.DeviceId==deviceId) {selected=adapter;break;}
    }
    if (!selected) return -2;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context))) return -3;
    ComPtr<ID3D11Device5> modern; ComPtr<ID3D11DeviceContext4> modernContext;
    if (FAILED(device.As(&modern)) || FAILED(context.As(&modernContext))) return -4;
    HMODULE module=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return -5;
    using Create=NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D11_API_FUNCTION_LIST*);
    auto create=reinterpret_cast<Create>(GetProcAddress(module,"NvOFAPICreateInstanceD3D11"));
    NV_OF_D3D11_API_FUNCTION_LIST api{}; NvOFHandle session{};
    int result=-6;
    if (create && create(NV_OF_API_VERSION,&api)==NV_OF_SUCCESS)
    {
        NV_OF_STATUS status=api.nvCreateOpticalFlowD3D11(device.Get(),context.Get(),&session);
        result=static_cast<int>(status);
        if (status==NV_OF_SUCCESS)
        {
            NV_OF_INIT_PARAMS parameters{};
            parameters.width=width;parameters.height=height;
            parameters.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
            parameters.mode=NV_OF_MODE_OPTICALFLOW;parameters.perfLevel=NV_OF_PERF_LEVEL_SLOW;
            parameters.predDirection=NV_OF_PRED_DIRECTION_BOTH;parameters.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
            result=static_cast<int>(api.nvOFInit(session,&parameters));
            api.nvOFDestroy(session);
        }
    }
    FreeLibrary(module);
    return result;
}
