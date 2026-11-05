// AI-generated comment: Real DXGI dispatch + a synthetic external OSD hook. No game/RTSS settings.
#include "FinalPresentation.h"
#include "GpuQueueLimit.h"
#include "MinHook.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
static PresentFn trampoline{};
static IDXGISwapChain *expectedChain{};
static unsigned hits{}, checks{}, lastInterval{}, lastFlags{};
static HRESULT injectedResult = S_OK;
static std::array<unsigned char, 4> frameColor{};
static void Require(bool value, const char *message)
{
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
static void Check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D/DXGI fixture failure");
}
static void Pixels(ID3D11DeviceContext *context, ID3D11Texture2D *image, bool osd)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(image, 0, D3D11_MAP_READ, 0, &mapped));
    bool equal = true;
    for (unsigned y = 0; y < 24; ++y)
        for (unsigned x = 0; x < 32; ++x)
        {
            auto *pixel = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * 4;
            const bool marker = osd && x >= 3 && x < 8 && y >= 2 && y < 7;
            auto color = marker ? std::array<unsigned char, 4>{255, 0, 255, 255} : frameColor;
            for (unsigned c = 0; c < 4; ++c)
                equal &= pixel[c] == color[c];
        }
    context->Unmap(image, 0);
    Require(equal, osd ? "OSD must be drawn after composition" : "OSD contaminated input/history");
}
static HRESULT STDMETHODCALLTYPE ExternalOSD(IDXGISwapChain *chain, UINT interval, UINT flags)
{
    ++hits;
    lastInterval = interval;
    lastFlags = flags;
    Require(chain == expectedChain, "Wrong presentation target");
    if (FAILED(injectedResult))
        return injectedResult;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    Check(chain->GetDevice(IID_PPV_ARGS(&device)));
    device->GetImmediateContext(&context);
    Check(context.As(&context1));
    ComPtr<ID3D11Texture2D> back;
    Check(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
    ComPtr<ID3D11RenderTargetView> view;
    Check(device->CreateRenderTargetView(back.Get(), nullptr, &view));
    const float magenta[] = {1, 0, 1, 1};
    D3D11_RECT rectangle{3, 2, 8, 7};
    context1->ClearView(view.Get(), magenta, &rectangle, 1);
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> read;
    Check(device->CreateTexture2D(&desc, nullptr, &read));
    context->CopyResource(read.Get(), back.Get());
    Pixels(context.Get(), read.Get(), true);
    return trampoline(chain, interval, flags);
}
// Represents ReShade's outer wrapper. Calling it twice would reapply effects.
struct EffectProxy final : IDXGISwapChain
{
    IDXGISwapChain *real;
    unsigned effects{};
    explicit EffectProxy(IDXGISwapChain *value) : real(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a, void **b) override
    {
        return real->QueryInterface(a, b);
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return real->AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        return real->Release();
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a, UINT b, const void *c) override
    {
        return real->SetPrivateData(a, b, c);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a, const IUnknown *b) override
    {
        return real->SetPrivateDataInterface(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a, UINT *b, void *c) override
    {
        return real->GetPrivateData(a, b, c);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID a, void **b) override
    {
        return real->GetParent(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID a, void **b) override
    {
        return real->GetDevice(a, b);
    }
    HRESULT STDMETHODCALLTYPE Present(UINT a, UINT b) override
    {
        ++effects;
        return real->Present(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT a, REFIID b, void **c) override
    {
        return real->GetBuffer(a, b, c);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a, IDXGIOutput *b) override
    {
        return real->SetFullscreenState(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *a, IDXGIOutput **b) override
    {
        return real->GetFullscreenState(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *a) override
    {
        return real->GetDesc(a);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e) override
    {
        return real->ResizeBuffers(a, b, c, d, e);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *a) override
    {
        return real->ResizeTarget(a);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **a) override
    {
        return real->GetContainingOutput(a);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *a) override
    {
        return real->GetFrameStatistics(a);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *a) override
    {
        return real->GetLastPresentCount(a);
    }
};
int main()
{
    HWND window{};
    void *target{};
    bool hooked = false;
    try
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"KOEFinalPresentationFixture";
        RegisterClassW(&wc);
        window = CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 32, 24, nullptr, nullptr, wc.hInstance, nullptr);
        Require(window != nullptr, "Fixture window");
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 32;
        desc.BufferDesc.Height = 24;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> swap;
        Check(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            &desc, &swap, &device, nullptr, &context));
        ComPtr<IDXGIDevice1> queueDevice;
        Check(device.As(&queueDevice));
        UINT originalLatency{}, latency{};
        Check(queueDevice->GetMaximumFrameLatency(&originalLatency));
        Check(queueDevice->SetMaximumFrameLatency(3));
        {
            GpuQueueLimit limit;
            limit.Enable(device.Get());
            limit.Enable(device.Get());
            Check(queueDevice->GetMaximumFrameLatency(&latency));
            Require(latency == 1, "Active interpolation retained queued source frames");
            limit.Disable();
            Check(queueDevice->GetMaximumFrameLatency(&latency));
            Require(latency == 3, "Disabling interpolation did not restore the source queue");
            limit.Enable(device.Get());
            Check(queueDevice->SetMaximumFrameLatency(2));
        }
        Check(queueDevice->GetMaximumFrameLatency(&latency));
        Require(latency == 2, "Queue cleanup overwrote a later external setting");
        Check(queueDevice->SetMaximumFrameLatency(1));
        {
            GpuQueueLimit limit;
            limit.Enable(device.Get());
        }
        Check(queueDevice->GetMaximumFrameLatency(&latency));
        Require(latency == 1, "An existing one-frame queue was changed on cleanup");
        Check(queueDevice->SetMaximumFrameLatency(originalLatency));
        expectedChain = swap.Get();
        target = (*reinterpret_cast<void ***>(swap.Get()))[8];
        Require(MH_Initialize() == MH_OK, "Hook initialization");
        Require(MH_CreateHook(target, reinterpret_cast<void *>(&ExternalOSD), reinterpret_cast<void **>(&trampoline)) ==
                    MH_OK,
                "OSD hook creation");
        Require(MH_EnableHook(target) == MH_OK, "OSD hook activation");
        hooked = true;
        // Negative control: the former saved-trampoline route bypasses late OSD.
        trampoline(swap.Get(), 0, 0);
        Require(hits == 0, "Negative control did not bypass OSD");
        EffectProxy proxy(swap.Get());
        for (unsigned kind = 0; kind < 4; ++kind)
        {
            frameColor =
                kind % 2 ? std::array<unsigned char, 4>{0, 255, 0, 255} : std::array<unsigned char, 4>{0, 0, 255, 255};
            float color[] = {frameColor[0] / 255.f, frameColor[1] / 255.f, frameColor[2] / 255.f, 1};
            ComPtr<ID3D11Texture2D> back;
            Check(swap->GetBuffer(0, IID_PPV_ARGS(&back)));
            ComPtr<ID3D11RenderTargetView> view;
            Check(device->CreateRenderTargetView(back.Get(), nullptr, &view));
            context->ClearRenderTargetView(view.Get(), color);
            D3D11_TEXTURE2D_DESC td{};
            back->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> history;
            Check(device->CreateTexture2D(&td, nullptr, &history));
            context->CopyResource(history.Get(), back.Get());
            const unsigned before = hits;
            // Real/generated images, with/without an already-applied effect wrapper.
            HRESULT hr = kind < 2 ? FinalPresentation::Present(swap.Get(), nullptr, 0, 0)
                                  : FinalPresentation::Present(&proxy, swap.Get(), 0, 0);
            Check(hr);
            Require(hits == before + 1, "Final OSD dispatch missing or repeated");
            Require(proxy.effects == 0, "ReShade effects applied twice");
            Require(lastInterval == 0 && lastFlags == 0, "Present parameters changed");
            Pixels(context.Get(), history.Get(), false);
        }
        injectedResult = DXGI_ERROR_DEVICE_REMOVED;
        Require(FinalPresentation::Present(swap.Get(), nullptr, 1, DXGI_PRESENT_TEST) == injectedResult,
                "Present error swallowed");
        Require(lastInterval == 1 && lastFlags == DXGI_PRESENT_TEST, "Present parameters not forwarded");
        Require(FinalPresentation::Present(nullptr, nullptr, 0, 0) == E_POINTER, "Missing swap chain not rejected");
        Require(FinalPresentation::Present(nullptr, swap.Get(), 0, 0) == injectedResult,
                "Native-only dispatch unavailable");
        MH_DisableHook(target);
        MH_Uninitialize();
        hooked = false;
        DestroyWindow(window);
        std::cout << "Final presentation: " << checks << " DXGI checks passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        if (hooked)
            MH_DisableHook(target);
        MH_Uninitialize();
        if (window)
            DestroyWindow(window);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
