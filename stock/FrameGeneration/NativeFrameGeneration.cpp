// DX11 presentation adapter. Unity keeps rendering/physics; a separate device
// interpolates shared GPU frames and presents a click-through client overlay.
#ifdef KOE_DIRECT_FLOW
#include "OpticalD3D11.h"
using Interpolator = OpticalD3D11;
#else
#include "FrucD3D11.h"
using Interpolator = FrucD3D11;
#endif
#include "MinHook.h"
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <algorithm>
#include <chrono>
#include <string>
#include <deque>
#include <fstream>
#include <sstream>
#include "PresentationCadence.h"
#include "ReShadeIntegration.h"
#include "FinalPresentation.h"
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static std::atomic<bool> enabled{}, allowed{}, started{}, stopWorker{};
static std::atomic<bool> adaptiveCadence{};
static std::atomic<long long> captured{}, generated{}, realPresents{}, generatedPresents{}, skipped{}, repeated{},
    failures{}, workerCallUs{};
static std::atomic<long long> cleanCopies{}, missingClean{};
static std::atomic<long long> reshadeCaptures{}, reshadeBypasses{};
static std::atomic<long long> qualityRequests{}, qualityReasons[8]{};
static std::atomic<int> captureFormat{}, captureViewFormat{};
static std::mutex stateMutex;
static std::wstring library;
#ifdef KOE_CAPTURE_AUDIT
static std::filesystem::path auditRoot;
static std::atomic<unsigned> auditInstances{};
static std::vector<double> auditSchedule;
static size_t nextAudit{};
static double auditEpoch{};
#endif
static std::atomic<HWND> gameWindow{};
static std::atomic<HWND> overlayWindow{};
static std::atomic<long long> sourcePresentCalls{}, sourcePresentUs{}, sourceZeroSync{};
static thread_local bool ownPresent = false;
static thread_local int presentationKind = 0;
extern "C" __declspec(dllexport) int GetFrameGenerationPresentationKind()
{
    return presentationKind;
}
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
static PresentFn originalPresent{};
static HANDLE wakeEvent{};
static std::thread worker;
static std::string lastError;
static void Check(HRESULT hr, const char *text)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(text) + " hr=" + std::to_string(static_cast<unsigned>(hr)));
}
struct Slot
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11Texture2D> finalTexture;
    ComPtr<IDXGIKeyedMutex> finalMutex;
    HANDLE finalHandle{};
    HANDLE handle{};
    int status{};
    double timestamp{};
    std::uint64_t serial{};
    ~Slot()
    {
        if (handle)
            CloseHandle(handle);
        if (finalHandle)
            CloseHandle(finalHandle);
    }
};
struct Shared
{
    ComPtr<ID3D11Device> source;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Texture2D> cleanWorld;
    ComPtr<ID3D11DeviceContext1> captureContext;
    ComPtr<ID3DDeviceContextState> captureState;
    ComPtr<ID3D11RenderTargetView> captureTarget;
    ComPtr<ID3D11VertexShader> captureVS;
    ComPtr<ID3D11PixelShader> capturePS;
    ComPtr<ID3D11RasterizerState> captureRaster;
    ComPtr<ID3D11Resource> captureSource;
    ComPtr<ID3D11ShaderResourceView> captureSourceView;
    D3D11_TEXTURE2D_DESC desc{};
    Slot slots[3];
    HWND window{};
    double lastPresent{}, cadence = 1.0 / 30;
    std::uint64_t cleanSerial{}, usedSerial{};
    std::atomic<double> lastCaptured{};
};
static std::shared_ptr<Shared> shared;
static double Seconds()
{
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}
static int ColorFamily(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        return 1;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        return 2;
    return 0;
}
static bool Supported(DXGI_FORMAT f)
{
    return ColorFamily(f) != 0;
}
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    return DefWindowProcW(window, message, w, l);
}
static std::shared_ptr<Shared> CreateShared(IDXGISwapChain *chain, ID3D11Texture2D *buffer, HWND window)
{
    auto value = std::make_shared<Shared>();
    value->window = window;
    value->chain = chain;
    buffer->GetDesc(&value->desc);
    Check(chain->GetDevice(IID_PPV_ARGS(&value->source)), "source device");
    value->source->GetImmediateContext(&value->context);
    auto desc = value->desc;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.Format = ColorFamily(desc.Format) == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (auto &slot : value->slots)
    {
        Check(value->source->CreateTexture2D(&desc, nullptr, &slot.texture), "shared capture texture");
        Check(slot.texture.As(&slot.mutex), "capture mutex");
        ComPtr<IDXGIResource1> resource;
        Check(slot.texture.As(&resource), "capture resource");
        Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                           &slot.handle),
              "capture handle");
        Check(value->source->CreateTexture2D(&desc, nullptr, &slot.finalTexture), "shared UI texture");
        Check(slot.finalTexture.As(&slot.finalMutex), "UI mutex");
        resource.Reset();
        Check(slot.finalTexture.As(&resource), "UI resource");
        Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                           &slot.finalHandle),
              "UI handle");
    }
    desc.MiscFlags = 0;
    Check(value->source->CreateTexture2D(&desc, nullptr, &value->cleanWorld), "pre-UI world texture");
    return value;
}
static void CapturePair(Shared &value, ID3D11Texture2D *finalImage)
{
    if (value.cleanSerial == value.usedSerial)
    {
        missingClean++;
        return;
    }
    value.usedSerial = value.cleanSerial;
    for (auto &slot : value.slots)
        if (slot.status == 0 && slot.mutex->AcquireSync(0, 0) == S_OK)
        {
            if (slot.finalMutex->AcquireSync(0, 0) != S_OK)
            {
                slot.mutex->ReleaseSync(0);
                continue;
            }
            value.context->CopyResource(slot.texture.Get(), value.cleanWorld.Get());
            value.context->CopyResource(slot.finalTexture.Get(), finalImage);
            value.context->Flush();
            Check(slot.finalMutex->ReleaseSync(1), "UI release");
            Check(slot.mutex->ReleaseSync(1), "capture release");
            slot.timestamp = Seconds();
            slot.serial = value.cleanSerial;
            value.lastCaptured = slot.timestamp;
            slot.status = 1;
            captured++;
            SetEvent(wakeEvent);
            return;
        }
    skipped++;
}
static void CaptureReShadeFinal(IDXGISwapChain *chain, HWND window)
{
    if (!enabled.load() || !allowed.load() || window != gameWindow.load() || GetForegroundWindow() != window ||
        IsIconic(window))
        return;
    try
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (!shared || shared->window != window)
            return;
        ComPtr<ID3D11Texture2D> image;
        Check(chain->GetBuffer(0, IID_PPV_ARGS(&image)), "ReShade final backbuffer");
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        if (desc.Width != shared->desc.Width || desc.Height != shared->desc.Height ||
            desc.Format != shared->desc.Format || desc.SampleDesc.Count != 1)
            return;
        CapturePair(*shared, image.Get());
        reshadeCaptures++;
    }
    catch (const std::exception &e)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lastError = e.what();
        failures++;
        enabled = false;
        if (wakeEvent)
            SetEvent(wakeEvent);
    }
}
static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain *chain, UINT sync, UINT flags)
{
    bool sourceCall = false;
    if (!ownPresent && enabled.load() && allowed.load() && !(flags & DXGI_PRESENT_TEST))
    {
        try
        {
            DXGI_SWAP_CHAIN_DESC desc{};
            Check(chain->GetDesc(&desc), "chain description");
            DWORD pid{};
            GetWindowThreadProcessId(desc.OutputWindow, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(desc.OutputWindow) &&
                GetForegroundWindow() == desc.OutputWindow && !IsIconic(desc.OutputWindow))
            {
                sourceCall = true;
                ComPtr<ID3D11Texture2D> buffer;
                Check(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "back buffer");
                D3D11_TEXTURE2D_DESC td{};
                buffer->GetDesc(&td);
                if (Supported(td.Format) && td.SampleDesc.Count == 1)
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    gameWindow = desc.OutputWindow;
                    if (!shared || shared->desc.Width != td.Width || shared->desc.Height != td.Height ||
                        shared->desc.Format != td.Format || shared->window != desc.OutputWindow)
                        shared = CreateShared(chain, buffer.Get(), desc.OutputWindow);
                    double now = Seconds();
                    if (shared->lastPresent > 0)
                        shared->cadence =
                            std::clamp(shared->cadence * .8 + (now - shared->lastPresent) * .2, 1.0 / 240, .2);
                    shared->lastPresent = now;
                    // AI-generated comment: ReShade's official callback supplies the final image only
                    // after its effects and menu have been rendered.
                    if (!ReShadeIntegration::active.load())
                        CapturePair(*shared, buffer.Get());
                }
            }
        }
        catch (const std::exception &e)
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            lastError = e.what();
            failures++;
            enabled = false;
            SetEvent(wakeEvent);
        }
    }
    auto begin = Clock::now();
    HRESULT result = originalPresent(chain, sync, flags);
    if (sourceCall)
    {
        sourcePresentCalls++;
        sourcePresentUs +=
            static_cast<long long>(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
        if (sync == 0)
            sourceZeroSync++;
    }
    return result;
}
extern "C" __declspec(dllexport) void ReadFrameGenerationPresentation(long long *values, int count)
{
    if (!values || count != 3)
        return;
    values[0] = sourcePresentCalls.load();
    values[1] = sourcePresentUs.load();
    values[2] = sourceZeroSync.load();
}
extern "C" __declspec(dllexport) void ReadFrameGenerationReShade(long long *values, int count)
{
    if (!values || count != 3)
        return;
    values[0] = ReShadeIntegration::active.load();
    values[1] = reshadeCaptures.load();
    values[2] = reshadeBypasses.load();
}
extern "C" __declspec(dllexport) void ReadFrameGenerationQuality(long long *values, int count)
{
    if (!values || count != 9)
        return;
    values[0] = qualityRequests.load();
    for (int i = 0; i < 8; i++)
        values[i + 1] = qualityReasons[i].load();
}
static void CaptureWorld(Shared &value, ID3D11Texture2D *image, DXGI_FORMAT format)
{
    if (!value.captureState)
    {
        ComPtr<ID3D11Device1> device1;
        Check(value.source.As(&device1), "capture device1");
        Check(value.context.As(&value.captureContext), "capture context1");
        D3D_FEATURE_LEVEL feature = value.source->GetFeatureLevel(), chosen{};
        Check(device1->CreateDeviceContextState(0, &feature, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen,
                                                &value.captureState),
              "capture isolated state");
        D3D11_RENDER_TARGET_VIEW_DESC target{};
        target.Format = ColorFamily(value.desc.Format) == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Check(value.source->CreateRenderTargetView(value.cleanWorld.Get(), &target, &value.captureTarget),
              "capture SDR target");
        const char *vertex = "float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
        // AI-generated comment: in capture C1, the final Unity blit copies display-encoded HDR
        // values without another gamma transform, and flips the display texture Y.
        const char *pixel =
            "Texture2D<float4> image:register(t0);float4 main(float4 p:SV_Position):SV_Target{uint w,h;image.GetDimensions(w,h);return image.Load(int3(int(p.x),int(h)-1-int(p.y),0));}";
        ComPtr<ID3DBlob> blob, errors;
        Check(D3DCompile(vertex, strlen(vertex), nullptr, nullptr, nullptr, "main", "vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors),
              "capture VS");
        Check(value.source->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                               &value.captureVS),
              "capture VS create");
        blob.Reset();
        errors.Reset();
        Check(D3DCompile(pixel, strlen(pixel), nullptr, nullptr, nullptr, "main", "ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors),
              "capture PS");
        Check(
            value.source->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &value.capturePS),
            "capture PS create");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        Check(value.source->CreateRasterizerState(&rd, &value.captureRaster), "capture rasterizer");
    }
    if (value.captureSource.Get() != image)
    {
        value.captureSourceView.Reset();
        value.captureSource = image;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = (format == DXGI_FORMAT_R16G16B16A16_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                         ? DXGI_FORMAT_R16G16B16A16_FLOAT
                     : ColorFamily(format) == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                : DXGI_FORMAT_B8G8R8A8_UNORM;
        Check(value.source->CreateShaderResourceView(image, &srv, &value.captureSourceView), "capture HDR source view");
    }
    ComPtr<ID3DDeviceContextState> previous;
    value.captureContext->SwapDeviceContextState(value.captureState.Get(), &previous);
    auto *target = value.captureTarget.Get();
    auto *view = value.captureSourceView.Get();
    value.context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(value.desc.Width), static_cast<float>(value.desc.Height), 0, 1};
    value.context->RSSetViewports(1, &vp);
    value.context->RSSetState(value.captureRaster.Get());
    value.context->IASetInputLayout(nullptr);
    value.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    value.context->VSSetShader(value.captureVS.Get(), nullptr, 0);
    value.context->PSSetShader(value.capturePS.Get(), nullptr, 0);
    value.context->PSSetShaderResources(0, 1, &view);
    value.context->Draw(3, 0);
    view = nullptr;
    value.context->PSSetShaderResources(0, 1, &view);
    value.captureContext->SwapDeviceContextState(previous.Get(), &value.captureState);
}
static void __stdcall BeforeUI(int event)
{
    if (event != 1 || !enabled.load() || !allowed.load())
        return;
    try
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (!shared)
            return;
        ComPtr<ID3D11RenderTargetView> target;
        shared->context->OMGetRenderTargets(1, &target, nullptr);
        if (!target)
            return;
        ComPtr<ID3D11Resource> resource;
        target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(resource.As(&buffer)))
            return;
        D3D11_TEXTURE2D_DESC td{};
        buffer->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        target->GetDesc(&vd);
        captureFormat = td.Format;
        captureViewFormat = vd.Format;
        bool hdr = td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS || td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (td.Width != shared->desc.Width || td.Height != shared->desc.Height || (!hdr && !Supported(td.Format)) ||
            td.SampleDesc.Count != 1)
            return;
        CaptureWorld(*shared, buffer.Get(), td.Format);
        shared->cleanSerial++;
        cleanCopies++;
    }
    catch (const std::exception &e)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lastError = e.what();
        failures++;
        enabled = false;
    }
}
extern "C" __declspec(dllexport) void *GetFrameGenerationRenderEvent()
{
    return reinterpret_cast<void *>(BeforeUI);
}
struct Presenter
{
    PresentationCadence cadencePolicy;
    HWND window{};
    std::shared_ptr<Shared> source;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap, nativeSwap;
    ComPtr<ID3D11Texture2D> back, input, finalImage, opened[3], openedFinal[3];
    ComPtr<ID3D11ShaderResourceView> finalViews[3], inputSrv, finalSrv;
    ComPtr<IDXGIKeyedMutex> mutexes[3];
    ComPtr<ID3D11RenderTargetView> inputView;
    ComPtr<ID3D11ShaderResourceView> views[3];
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, uiPS;
    ComPtr<ID3D11RenderTargetView> finalView, backView;
    ComPtr<IDXGIKeyedMutex> finalMutexes[3];
    ComPtr<ID3D11ShaderResourceView> interpolatedView;
    std::unique_ptr<Interpolator> fruc;
    double lastTimestamp{}, interval = 1.0 / 30, refreshInterval = 1.0 / 75;
    struct Pending
    {
        std::shared_ptr<Interpolator::Ticket> ticket;
        bool presented;
    };
    std::deque<Pending> pending;
#ifdef KOE_CAPTURE_AUDIT
    ComPtr<ID3D11Texture2D> auditWorld[3], auditFinal, auditGenerated, auditComposed;
    double auditTimes[3]{};
    std::uint64_t auditSerials[3]{};
    unsigned auditIndex{}, auditAge{};
    bool auditDone{};
    std::filesystem::path auditDirectory;
    bool auditGeneratedFrame{}, auditPresented{};
    int auditQualityReason{};
    std::uint32_t auditQualityCounts[4]{};
#endif
    Presenter(std::shared_ptr<Shared> value) : source(value)
    {
        ComPtr<IDXGIDevice> sourceDxgi;
        Check(source->source.As(&sourceDxgi), "source DXGI device");
        ComPtr<IDXGIAdapter> adapter;
        Check(sourceDxgi->GetAdapter(&adapter), "adapter");
        Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &device, nullptr, &context),
              "presentation device");
        Check(device.As(&device1), "presentation device1");
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"KOEFrameGeneration";
        RegisterClassW(&wc);
        window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                 L"KOEFrameGeneration", L"KOE generated frames", WS_POPUP, 0, 0, source->desc.Width,
                                 source->desc.Height, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window)
            throw std::runtime_error("presentation window");
        SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
        overlayWindow = window;
        ReShadeIntegration::ownWindow = window;
        ComPtr<IDXGIFactory> factory;
        Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "presentation factory");
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = source->desc.Width;
        sd.BufferDesc.Height = source->desc.Height;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = window;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT swapResult = factory->CreateSwapChain(device.Get(), &sd, &swap);
        if (FAILED(swapResult))
        {
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            Check(factory->CreateSwapChain(device.Get(), &sd, &swap), "presentation swap chain");
        }
        if (ReShadeIntegration::active.load())
        {
            nativeSwap = ReShadeIntegration::NativeSwap();
            if (!nativeSwap)
                throw std::runtime_error("ReShade private presentation swap chain unavailable");
        }
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        ComPtr<IDXGIDevice1> latency;
        if (SUCCEEDED(device.As(&latency)))
        {
            latency->SetMaximumFrameLatency(3);
            latency->SetGPUThreadPriority(7);
        }
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (GetMonitorInfoW(MonitorFromWindow(source->window, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency >= 30)
            refreshInterval = 1.0 / mode.dmDisplayFrequency;
        Check(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "presentation backbuffer");
        auto td = source->desc;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.MiscFlags = 0;
        td.CPUAccessFlags = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&td, nullptr, &input), "RGBA input");
        Check(device->CreateRenderTargetView(input.Get(), nullptr, &inputView), "RGBA input view");
        Check(device->CreateShaderResourceView(input.Get(), nullptr, &inputSrv), "world SRV");
        Check(device->CreateTexture2D(&td, nullptr, &finalImage), "RGBA UI image");
        Check(device->CreateRenderTargetView(finalImage.Get(), nullptr, &finalView), "UI RTV");
        Check(device->CreateShaderResourceView(finalImage.Get(), nullptr, &finalSrv), "UI SRV");
        Check(device->CreateRenderTargetView(back.Get(), nullptr, &backView), "backbuffer RTV");
        for (int i = 0; i < 3; i++)
        {
            Check(device1->OpenSharedResource1(source->slots[i].handle, IID_PPV_ARGS(&opened[i])), "open input");
            Check(opened[i].As(&mutexes[i]), "input mutex");
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format =
                ColorFamily(source->desc.Format) == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            Check(device->CreateShaderResourceView(opened[i].Get(), &srv, &views[i]), "input view");
            Check(device1->OpenSharedResource1(source->slots[i].finalHandle, IID_PPV_ARGS(&openedFinal[i])),
                  "open UI input");
            Check(openedFinal[i].As(&finalMutexes[i]), "UI input mutex");
            Check(device->CreateShaderResourceView(openedFinal[i].Get(), &srv, &finalViews[i]), "UI input SRV");
        }
        const char *vertex = "float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
        const char *pixel =
            "Texture2D<float4> image:register(t0);float4 main(float4 p:SV_Position):SV_Target{return image.Load(int3(int2(p.xy),0));}";
        ComPtr<ID3DBlob> blob, errors;
        Check(D3DCompile(vertex, strlen(vertex), nullptr, nullptr, nullptr, "main", "vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors),
              "conversion VS");
        Check(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs),
              "conversion VS create");
        blob.Reset();
        errors.Reset();
        Check(D3DCompile(pixel, strlen(pixel), nullptr, nullptr, nullptr, "main", "ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors),
              "conversion PS");
        Check(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps),
              "conversion PS create");
        const char *ui = R"HLSL(
Texture2D<float4> generatedImage : register(t0);
Texture2D<float4> cleanWorld : register(t1);
Texture2D<float4> finalImage : register(t2);
bool uiAt(int2 p)
{
    return any(cleanWorld.Load(int3(p, 0)) != finalImage.Load(int3(p, 0)));
}
float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    uint w, h;
    cleanWorld.GetDimensions(w, h);
    int2 maximum = int2(w, h) - 1;
    bool mask = uiAt(p) || uiAt(clamp(p + int2(1, 0), 0, maximum)) || uiAt(clamp(p - int2(1, 0), 0, maximum)) ||
                uiAt(clamp(p + int2(0, 1), 0, maximum)) || uiAt(clamp(p - int2(0, 1), 0, maximum));
    return mask ? finalImage.Load(int3(p, 0)) : generatedImage.Load(int3(p, 0));
}
)HLSL";
        blob.Reset();
        errors.Reset();
        Check(D3DCompile(ui, strlen(ui), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                         &blob, &errors),
              "UI compositor PS");
        Check(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &uiPS),
              "UI compositor PS create");
        fruc.reset(new Interpolator(device.Get(), td.Width, td.Height, library));
        Check(device->CreateShaderResourceView(fruc->Output(), nullptr, &interpolatedView), "interpolated SRV");
#ifdef KOE_CAPTURE_AUDIT
        if (!auditRoot.empty())
            auditDirectory = auditRoot / ("presenter-" + std::to_string(auditInstances++));
        if (!auditSchedule.empty())
            auditDone = true;
#endif
    }
    ~Presenter()
    {
        overlayWindow = nullptr;
        if (window)
            ShowWindow(window, SW_HIDE);
        if (fruc)
        {
            fruc->Wait();
            Resolve();
        }
        fruc.reset();
        swap.Reset();
        if (window)
            DestroyWindow(window);
    }
    void Resolve()
    {
        while (!pending.empty() && fruc->Completed(pending.front().ticket))
        {
            auto item = pending.front();
            pending.pop_front();
            if (!item.ticket->eligible)
                continue;
            if (item.ticket->repeated)
                repeated++;
            else
            {
                generated++;
                if (item.presented)
                    generatedPresents++;
            }
        }
    }
#ifdef KOE_CAPTURE_AUDIT
    void Snapshot(ID3D11Texture2D *texture, ComPtr<ID3D11Texture2D> &result)
    {
        if (!result)
        {
            D3D11_TEXTURE2D_DESC d{};
            texture->GetDesc(&d);
            d.BindFlags = 0;
            d.MiscFlags = 0;
            Check(device->CreateTexture2D(&d, nullptr, &result), "audit snapshot");
        }
        context->CopyResource(result.Get(), texture);
    }
    void SavePixels(ID3D11Texture2D *texture, const char *name)
    {
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> stage;
        Check(device->CreateTexture2D(&d, nullptr, &stage), "audit staging");
        context->CopyResource(stage.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE m{};
        Check(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "audit map");
        std::ofstream stream(auditDirectory / name, std::ios::binary);
        for (unsigned y = 0; y < d.Height; y++)
            stream.write(static_cast<const char *>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * 4);
        context->Unmap(stage.Get(), 0);
        if (!stream)
            throw std::runtime_error("audit write failed");
    }
    void SaveAudit()
    {
        if (auditDone || auditDirectory.empty() || auditIndex != 3 || !auditFinal)
            return;
        std::filesystem::create_directories(auditDirectory);
        SavePixels(auditWorld[0].Get(), "world-0.rgba");
        SavePixels(auditWorld[1].Get(), "world-1.rgba");
        SavePixels(auditWorld[2].Get(), "world-2.rgba");
        SavePixels(auditFinal.Get(), "final-2.rgba");
        if (auditGeneratedFrame && auditGenerated)
            SavePixels(auditGenerated.Get(), "generated-12.rgba");
        if (auditPresented && auditComposed)
            SavePixels(auditComposed.Get(), "composed-12.rgba");
        std::ofstream info(auditDirectory / "capture.json");
        info.precision(17);
        info << "{\"width\":" << source->desc.Width << ",\"height\":" << source->desc.Height
             << ",\"format\":\"RGBA8\",\"times\":[" << auditTimes[0] << "," << auditTimes[1] << "," << auditTimes[2]
             << "],\"serials\":[" << auditSerials[0] << "," << auditSerials[1] << "," << auditSerials[2]
             << "],\"generated\":" << auditGeneratedFrame << ",\"presented\":" << auditPresented
             << ",\"qualityReason\":" << auditQualityReason << ",\"qualityCounts\":[" << auditQualityCounts[0] << ","
             << auditQualityCounts[1] << "," << auditQualityCounts[2] << "," << auditQualityCounts[3]
             << "],\"scope\":\"diagnostic GPU readbacks, excluded from production\"}";
        auditDone = true;
        for (auto &texture : auditWorld)
            texture.Reset();
        auditFinal.Reset();
        auditGenerated.Reset();
        auditComposed.Reset();
    }
#endif
    bool Visible()
    {
        return enabled.load() && allowed.load() && Seconds() - source->lastCaptured.load() < .5 &&
               IsWindow(source->window) && GetForegroundWindow() == source->window && !IsIconic(source->window);
    }
    void Show()
    {
        RECT r{};
        GetClientRect(source->window, &r);
        POINT p{};
        ClientToScreen(source->window, &p);
        SetWindowPos(window, HWND_TOPMOST, p.x, p.y, r.right, r.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    void Convert(ID3D11ShaderResourceView *image, ID3D11RenderTargetView *target)
    {
        context->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(source->desc.Width), static_cast<float>(source->desc.Height),
                                0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &image);
        context->Draw(3, 0);
        image = nullptr;
        context->PSSetShaderResources(0, 1, &image);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    bool Present(ID3D11Texture2D *texture, bool midpoint = false)
    {
        if (!Visible())
            return false;
        if (midpoint)
        {
            auto *target = backView.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(uiPS.Get(), nullptr, 0);
            ID3D11ShaderResourceView *images[] = {interpolatedView.Get(), inputSrv.Get(), finalSrv.Get()};
            context->PSSetShaderResources(0, 3, images);
            context->Draw(3, 0);
            ID3D11ShaderResourceView *empty[3]{};
            context->PSSetShaderResources(0, 3, empty);
            context->OMSetRenderTargets(0, nullptr, nullptr);
#ifdef KOE_CAPTURE_AUDIT
            if (!auditDone && auditIndex == 3 && !auditDirectory.empty())
                Snapshot(back.Get(), auditComposed);
#endif
        }
        else
            context->CopyResource(back.Get(), texture);
        // AI-generated comment: External OSD hooks run after composition, on both real and generated
        // frames. The worker's ownPresent guard prevents recursive capture;
        // the ReShade native chain still avoids a second effects/UI pass.
        struct PresentationScope
        {
            int previous;
            PresentationScope(int kind) : previous(presentationKind)
            {
                presentationKind = kind;
            }
            ~PresentationScope()
            {
                presentationKind = previous;
            }
        } scope(midpoint ? 2 : 1);
        HRESULT hr = FinalPresentation::Present(swap.Get(), nativeSwap.Get(), 1, 0);
        if (nativeSwap)
            reshadeBypasses++;
        Check(hr, "generated presentation");
        return hr == S_OK;
    }
    void Process(int index, double timestamp, double sourceInterval, std::uint64_t serial)
    {
        if (mutexes[index]->AcquireSync(1, 2000) != S_OK)
            throw std::runtime_error("worker mutex acquire timeout/failure");
        if (finalMutexes[index]->AcquireSync(1, 2000) != S_OK)
            throw std::runtime_error("worker UI mutex timeout");
        Convert(views[index].Get(), inputView.Get());
        Convert(finalViews[index].Get(), finalView.Get());
        context->Flush();
        Check(finalMutexes[index]->ReleaseSync(0), "worker UI release");
        Check(mutexes[index]->ReleaseSync(0), "worker release");
#ifdef KOE_CAPTURE_AUDIT
        if (!auditRoot.empty() && !auditSchedule.empty() && (auditDone || auditIndex == 0) &&
            nextAudit < auditSchedule.size() && Seconds() - auditEpoch >= auditSchedule[nextAudit])
        {
            auditDirectory = auditRoot / ("capture-" + std::to_string(nextAudit++));
            auditDone = false;
            auditIndex = 0;
            auditAge = 29;
        }
        if (!auditDone && !auditDirectory.empty() && ++auditAge >= 30 && auditIndex < 3)
        {
            if (auditIndex > 0 && serial != auditSerials[auditIndex - 1] + 1)
                auditIndex = 0;
            Snapshot(input.Get(), auditWorld[auditIndex]);
            auditTimes[auditIndex] = timestamp;
            auditSerials[auditIndex] = serial;
            auditIndex++;
            if (auditIndex == 3)
                Snapshot(finalImage.Get(), auditFinal);
        }
#endif
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            source->slots[index].status = 0;
        }
        interval = sourceInterval;
        lastTimestamp = timestamp;
        Resolve();
        auto begin = Clock::now();
        bool midpointAllowed = false;
#ifdef KOE_DIRECT_FLOW
        bool midpointRequested =
            Visible() && cadencePolicy.Midpoint(interval, refreshInterval, true, adaptiveCadence.load());
        auto ticket = fruc->Submit(input.Get(), timestamp, midpointRequested);
        if (midpointRequested)
        {
            qualityRequests++;
            // AI-generated comment: Only the presentation worker can wait, within this frame's half-
            // interval budget. A late/unknown decision is a veto, never approval.
            double remaining = std::max(0.0, timestamp + interval * .5 - Seconds());
            midpointAllowed = fruc->Generate(ticket, static_cast<unsigned>(remaining * 1000));
            qualityReasons[static_cast<int>(ticket->rejection)]++;
        }
#else
        auto ticket = fruc->Submit(input.Get(), timestamp);
#endif
        workerCallUs = static_cast<long long>(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
#ifdef KOE_CAPTURE_AUDIT
        if (!auditDone && !auditDirectory.empty() && auditIndex == 3)
        {
            auditGeneratedFrame = midpointAllowed;
            auditPresented = false;
            auditGenerated.Reset();
            auditComposed.Reset();
#ifdef KOE_DIRECT_FLOW
            auditQualityReason = ticket->rejection;
            memcpy(auditQualityCounts, ticket->counts, sizeof(auditQualityCounts));
#endif
            if (auditGeneratedFrame)
                Snapshot(fruc->Output(), auditGenerated);
        }
#endif
        bool midPresented = false;
        if (!Visible())
        {
            pending.push_back({ticket, false});
#ifdef KOE_CAPTURE_AUDIT
            auditIndex = 0;
            auditComposed.Reset();
#endif
            ShowWindow(window, SW_HIDE);
            return;
        }
        Show();
        if (midpointAllowed)
        {
            midPresented = Present(fruc->Output(), true);
            auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                                            std::max(0.0, interval * .5 - refreshInterval)));
            while (Clock::now() < until && !stopWorker.load() && Visible())
            {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
                if (remaining > 1)
                    Sleep(static_cast<DWORD>(std::min<long long>(remaining, 4)));
                else
                    SwitchToThread();
            }
        }
        if (Present(finalImage.Get()))
            realPresents++;
        pending.push_back({ticket, midPresented});
        Resolve();
#ifdef KOE_CAPTURE_AUDIT
        auditPresented = midPresented;
        SaveAudit();
#endif
    }
};
static void Worker()
{
    ownPresent = true;
    std::unique_ptr<Presenter> presenter;
    try
    {
        while (!stopWorker.load())
        {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::shared_ptr<Shared> current;
            int index = -1;
            double timestamp{}, cadence{};
            std::uint64_t serial{};
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                current = shared;
                if (current && enabled.load() && allowed.load())
                    for (int i = 0; i < 3; i++)
                        if (current->slots[i].status == 1 && (index < 0 || current->slots[i].timestamp > timestamp))
                        {
                            index = i;
                            timestamp = current->slots[i].timestamp;
                        }
                if (current)
                    cadence = current->cadence;
                if (index >= 0)
                {
                    current->slots[index].status = 2;
                    serial = current->slots[index].serial;
                }
            }
            if (!enabled.load() || !allowed.load() || !current || GetForegroundWindow() != current->window)
            {
                if (presenter)
                    ShowWindow(presenter->window, SW_HIDE);
                WaitForSingleObject(wakeEvent, 10);
                continue;
            }
            if (!presenter || presenter->source != current)
                presenter.reset(new Presenter(current));
            if (index >= 0)
            {
                // AI-generated comment: Release older queued images without interpolation; never build latency.
                for (int i = 0; i < 3; i++)
                    if (i != index)
                    {
                        bool old = false;
                        {
                            std::lock_guard<std::mutex> lock(stateMutex);
                            old = current->slots[i].status == 1 && current->slots[i].timestamp < timestamp;
                            if (old)
                                current->slots[i].status = 2;
                        }
                        if (old)
                        {
                            if (presenter->mutexes[i]->AcquireSync(1, 2000) != S_OK ||
                                presenter->finalMutexes[i]->AcquireSync(1, 2000) != S_OK)
                                throw std::runtime_error("stale input mutex timeout");
                            Check(presenter->finalMutexes[i]->ReleaseSync(0), "stale UI release");
                            Check(presenter->mutexes[i]->ReleaseSync(0), "stale input release");
                            std::lock_guard<std::mutex> lock(stateMutex);
                            current->slots[i].status = 0;
                            skipped++;
                        }
                    }
                presenter->Process(index, timestamp, cadence, serial);
            }
            else
            {
                if (!presenter->Visible())
                    ShowWindow(presenter->window, SW_HIDE);
                WaitForSingleObject(wakeEvent, 5);
            }
        }
    }
    catch (const std::exception &e)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lastError = e.what();
        failures++;
        enabled = false;
    }
}
extern "C" __declspec(dllexport) int InitializeFrameGeneration(const wchar_t *dll)
{
    if (started.exchange(true))
        return -1;
    try
    {
#ifndef KOE_DIRECT_FLOW
        throw std::runtime_error("This frame-generation backend has no pre-generation confidence gate");
#endif
        library = std::filesystem::absolute(dll).wstring();
        wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wakeEvent)
            throw std::runtime_error("wake event");
#ifdef KOE_CAPTURE_AUDIT
        wchar_t audit[32768]{};
        DWORD length = GetEnvironmentVariableW(L"KOE_FRAME_GENERATION_AUDIT_ROOT", audit, 32768);
        if (length && length < 32768)
            auditRoot = std::filesystem::absolute(audit);
        char schedule[1024]{};
        DWORD scheduleLength = GetEnvironmentVariableA("KOE_FRAME_GENERATION_AUDIT_TIMES", schedule, 1024);
        if (scheduleLength && scheduleLength < 1024)
        {
            std::istringstream values(schedule);
            double time;
            while (values >> time)
            {
                if (time < 0 || (!auditSchedule.empty() && time <= auditSchedule.back()))
                    throw std::runtime_error("audit times must increase");
                auditSchedule.push_back(time);
            }
        }
        auditEpoch = Seconds();
#endif
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&InitializeFrameGeneration), &self))
            throw std::runtime_error("pin presentation module");
        ReShadeIntegration::Start(self, &CaptureReShadeFinal);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"KOEFrameProbe";
        RegisterClassW(&wc);
        HWND window =
            CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window)
            throw std::runtime_error("probe window");
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = 64;
        sd.BufferDesc.Height = 64;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = window;
        sd.Windowed = TRUE;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> swap;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &context);
        DestroyWindow(window);
        Check(hr, "probe device");
        auto **table = *reinterpret_cast<void ***>(swap.Get());
        auto status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("MinHook initialize");
        if (MH_CreateHook(table[8], reinterpret_cast<void *>(HookPresent),
                          reinterpret_cast<void **>(&originalPresent)) != MH_OK ||
            MH_EnableHook(table[8]) != MH_OK)
            throw std::runtime_error("Present hook");
        worker = std::thread(Worker);
        enabled = true;
        return 0;
    }
    catch (const std::exception &e)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lastError = e.what();
        failures++;
        return -2;
    }
}
extern "C" __declspec(dllexport) void SetFrameGeneration(int on, int scene)
{
    bool next = on != 0 && failures.load() == 0;
    bool wasEnabled = enabled.exchange(next);
    bool previous = allowed.exchange(scene != 0);
    if ((previous && scene == 0) || (wasEnabled && !next))
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        shared.reset();
        HWND window = overlayWindow.load();
        if (window)
            ShowWindowAsync(window, SW_HIDE);
    }
    if (wakeEvent)
        SetEvent(wakeEvent);
}
extern "C" __declspec(dllexport) void SetAdaptiveFrameCadence(int on)
{
    adaptiveCadence = on != 0;
}
extern "C" __declspec(dllexport) void InvalidateFrameGenerationHistory()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    shared.reset();
    HWND window = overlayWindow.load();
    if (window)
        ShowWindowAsync(window, SW_HIDE);
    if (wakeEvent)
        SetEvent(wakeEvent);
}
extern "C" __declspec(dllexport) void ReadFrameGeneration(long long *result, int count)
{
    if (!result || count != 10)
        return;
    long long values[] = {enabled.load(),      allowed.load(),           captured.load(), generated.load(),
                          realPresents.load(), generatedPresents.load(), skipped.load(),  repeated.load(),
                          failures.load(),     workerCallUs.load()};
    memcpy(result, values, sizeof(values));
}
extern "C" __declspec(dllexport) void ReadFrameGenerationUI(long long *result)
{
    if (result)
    {
        result[0] = cleanCopies.load();
        result[1] = missingClean.load();
    }
}
extern "C" __declspec(dllexport) int GetFrameGenerationCaptureFormat()
{
    return captureFormat.load() + (captureViewFormat.load() << 16);
}
extern "C" __declspec(dllexport) const char *GetFrameGenerationError()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return lastError.c_str();
}
extern "C" __declspec(dllexport) void StopFrameGeneration()
{
    enabled = false;
    allowed = false;
    stopWorker = true;
    HWND window = overlayWindow.load();
    if (window)
        ShowWindowAsync(window, SW_HIDE);
    if (wakeEvent)
        SetEvent(wakeEvent);
    // AI-generated comment: Unity's UI thread must never join a thread that destroys a native window.
    // The module is pinned until process exit and all worker resources own COM refs.
    if (worker.joinable())
        worker.detach();
}
#ifdef KOE_CAPTURE_AUDIT
extern "C" __declspec(dllexport) int TestFrameGenerationCapture(long long *result)
{
    if (!result)
        return -1;
    for (int i = 0; i < 5; i++)
        result[i] = 0;
    try
    {
        Shared value;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &value.source, nullptr, &value.context),
              "capture test device");
        constexpr unsigned w = 32, h = 24;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        value.desc = td;
        Check(value.source->CreateTexture2D(&td, nullptr, &value.cleanWorld), "capture test target");
        unsigned short half[] = {0, 0x3400, 0x3800, 0x3a00, 0x3c00};
        std::vector<unsigned short> pixels(w * h * 4);
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++)
            {
                size_t i = (y * w + x) * 4;
                pixels[i] = half[x % 5];
                pixels[i + 1] = half[y % 5];
                pixels[i + 2] = half[(x + y) % 5];
                pixels[i + 3] = 0x3c00;
            }
        td.Format = DXGI_FORMAT_R16G16B16A16_TYPELESS;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), w * 8, 0};
        ComPtr<ID3D11Texture2D> image;
        Check(value.source->CreateTexture2D(&td, &data, &image), "capture test HDR");
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> target;
        Check(value.source->CreateRenderTargetView(image.Get(), &vd, &target), "capture test RTV");
        auto *rt = target.Get();
        value.context->OMSetRenderTargets(1, &rt, nullptr);
        D3D11_VIEWPORT viewport{2, 3, 20, 10, .1f, .9f};
        value.context->RSSetViewports(1, &viewport);
        const char *pixel = "float4 main():SV_Target{return float4(0.1,0.2,0.3,1);}";
        ComPtr<ID3DBlob> blob, errors;
        Check(D3DCompile(pixel, strlen(pixel), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors),
              "capture test PS");
        ComPtr<ID3D11PixelShader> shader;
        Check(value.source->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader),
              "capture test PS create");
        value.context->PSSetShader(shader.Get(), nullptr, 0);
        unsigned constants[] = {0x12345678, 0xdeadbeef, 0xabcdef12, 0x76543210};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{constants, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        Check(value.source->CreateBuffer(&bd, &initial, &buffer), "capture test constants");
        auto *cb = buffer.Get();
        value.context->PSSetConstantBuffers(0, 1, &cb);
        CaptureWorld(value, image.Get(), td.Format);
        ComPtr<ID3D11RenderTargetView> restoredTarget;
        value.context->OMGetRenderTargets(1, &restoredTarget, nullptr);
        ComPtr<ID3D11PixelShader> restoredPS;
        value.context->PSGetShader(&restoredPS, nullptr, nullptr);
        ComPtr<ID3D11Buffer> restoredCB;
        value.context->PSGetConstantBuffers(0, 1, &restoredCB);
        D3D11_VIEWPORT restoredVP{};
        UINT n = 1;
        value.context->RSGetViewports(&n, &restoredVP);
        result[1] = restoredTarget.Get() != target.Get();
        result[2] = (restoredPS.Get() != shader.Get()) + (restoredCB.Get() != buffer.Get()) +
                    (memcmp(&viewport, &restoredVP, sizeof(viewport)) != 0);
        value.cleanWorld->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        Check(value.source->CreateTexture2D(&td, nullptr, &stage), "capture test readback");
        value.context->CopyResource(stage.Get(), value.cleanWorld.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        Check(value.context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "capture test map");
        unsigned char expected[] = {0, 64, 128, 191, 255};
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++)
            {
                auto *p = static_cast<unsigned char *>(m.pData) + size_t(y) * m.RowPitch + x * 4;
                unsigned originalY = h - 1 - y;
                unsigned indices[] = {x % 5, originalY % 5, (x + originalY) % 5, 4};
                for (unsigned c = 0; c < 4; c++)
                {
                    bool tie = c < 3 && indices[c] == 2 && p[c] == 127;
                    if (tie)
                        result[4]++;
                    else
                        result[0] += p[c] != expected[indices[c]];
                    result[3]++;
                }
            }
        value.context->Unmap(stage.Get(), 0);
        return result[0] || result[1] || result[2] ? 2 : 0;
    }
    catch (const std::exception &e)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lastError = e.what();
        return -2;
    }
}
#endif
// AI-generated comment: Module and Present trampoline remain loaded until process exit.
