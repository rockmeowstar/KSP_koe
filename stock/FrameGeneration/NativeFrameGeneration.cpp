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
#include <dxgi1_4.h>
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
#include <array>
#include <climits>
#include "PresentationCadence.h"
#include "ReShadeIntegration.h"
#include "FinalPresentation.h"
#include "FrameBudget.h"
#include "GpuQueueLimit.h"
#include "TargetFrameRate.h"
#include "DisplayRefresh.h"
#include "LayerTiming.h"
#include "LayerPassAudit.h"
#include "DeferredStencil.h"
#include "FireflyLayerCapture.h"
#include "IndependentSourceCapture.h"
#include "AtmosphereSourceProbe.h"
#include "PresentedFrameSample.h"
using Microsoft::WRL::ComPtr;
static bool IndependentMode()
{
    static bool mode=[](){char value[8]{};return GetEnvironmentVariableA("KOE_INDEPENDENT_LAYERS",value,8)==1&&value[0]=='1';}();
    return mode;
}
static std::atomic<float> independentCloudFade{1};
static std::atomic<long long> independentPacked{},independentUnavailable{},independentFireflyCopies{},independentParityRejected{};
static std::atomic<long long> publishedChanged{},publishedComposed{},publishedPixelTotal{};
static std::atomic<long long> observedDisplayed{},displayStatisticsFailures{},displayStatisticsSamples{};
extern "C" __declspec(dllexport) void SetFrameGenerationCloudFade(float fade){independentCloudFade=fade;}
extern "C" __declspec(dllexport) void ReadFrameGenerationIndependent(long long *values,int count)
{
    if(!values||count!=11)return;
    values[0]=IndependentMode()?1:0;values[1]=independentPacked.load();values[2]=independentUnavailable.load();
    values[3]=independentFireflyCopies.load();values[4]=independentParityRejected.load();
    values[5]=publishedChanged.load();values[6]=publishedComposed.load();values[7]=publishedPixelTotal.load();
    values[8]=observedDisplayed.load();values[9]=displayStatisticsFailures.load();values[10]=displayStatisticsSamples.load();
}
using Clock = std::chrono::steady_clock;
static std::atomic<bool> enabled{}, allowed{}, started{}, stopWorker{};
static std::atomic<bool> adaptiveCadence{true}, fullGeneration{};
static std::atomic<double> requestedTargetFPS{TargetFrameRate::Default};
static std::atomic<long long> monitorMilliHz{}, effectiveMilliFPS{60000}, scheduledPhases{};
static std::atomic<long long> captured{}, generated{}, realPresents{}, generatedPresents{}, skipped{}, repeated{},
    failures{}, workerCallUs{};
static std::atomic<long long> cleanCopies{}, missingClean{};
static std::atomic<long long> reshadeCaptures{}, reshadeBypasses{};
static std::atomic<long long> qualityRequests{}, qualityReasons[8]{};
static std::atomic<long long> flowExecutions{}, preflightVetoes{};
static std::atomic<long long> inputWaitUs{}, decisionAgeUs{};
static std::atomic<long long> analyzedPixels{}, safePixels{}, synthesizedPixels{}, composedPixels{};
static std::atomic<long long> silhouetteCopies{}, silhouetteFrames{};
static std::atomic<long long> layerGpuUs[4]{}, fireflyPairs{};
static std::atomic<long long> layerSourceFailures{};
static std::atomic<long long> stencilAttempts{}, stencilSuccesses{}, stencilFailures{};
static std::atomic<long long> celestialStencilFailures{};
static std::atomic<int> captureFormat{}, captureViewFormat{};
static std::mutex stateMutex;
static AtmosphereSourceProbe atmosphereProbe;
extern "C" __declspec(dllexport) void SetFrameGenerationAtmosphereProbe(void *c,void *t,void *d,int tone,const float *z)
{
    if(!z)return;
    std::lock_guard<std::mutex> lock(stateMutex);
    atmosphereProbe.Set(static_cast<ID3D11Texture2D*>(c),static_cast<ID3D11Texture2D*>(t),static_cast<ID3D11Texture2D*>(d),tone,z);
}
static ComPtr<ID3D11Texture2D> silhouetteInput;
static ComPtr<ID3D11Texture2D> cloudInput;
static ComPtr<ID3D11Texture2D> waterInput;
static std::wstring library;
#ifdef KOE_CAPTURE_AUDIT
static std::filesystem::path auditRoot;
static std::atomic<unsigned> auditInstances{};
static std::vector<double> auditSchedule;
static size_t nextAudit{};
static double auditEpoch{};
static std::atomic<int> datasetRequest{-1}, datasetComplete{-1};
static std::atomic<bool> datasetBusy{};
static std::atomic<double> datasetEarliest{};
static bool datasetMode{};
extern "C" __declspec(dllexport) int RequestFrameGenerationDataset(int id)
{
    if(id<0 || auditRoot.empty())return -1;
    if(!enabled.load() || !allowed.load())return 0;
    if(datasetBusy.exchange(true))return 0;
    try
    {
        if(std::filesystem::exists(auditRoot/("case-"+std::to_string(id))))
        {datasetBusy=false;return -1;}
    }
    catch(...) { datasetBusy=false;return -1; }
    datasetEarliest=std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    datasetRequest=id;return 1;
}
extern "C" __declspec(dllexport) int CompletedFrameGenerationDataset() { return datasetComplete.load(); }
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
    ComPtr<ID3D11Texture2D> silhouette;
    ComPtr<IDXGIKeyedMutex> silhouetteMutex;
    ComPtr<ID3D11Texture2D> layers;
    ComPtr<IDXGIKeyedMutex> layerMutex;
    HANDLE layerHandle{};
    bool hasLayers{},cloudFilmic{};
    float cloudFade=1;
    HANDLE silhouetteHandle{};
    bool hasSilhouette{};
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
        if (silhouetteHandle) CloseHandle(silhouetteHandle);
        if (layerHandle) CloseHandle(layerHandle);
    }
};
struct Contribution
{
    ComPtr<ID3D11Texture2D> source, sourceResource, before, mask, after;
    ComPtr<ID3D11ShaderResourceView> sourceView, beforeView, maskView, afterView;
    ComPtr<ID3D11RenderTargetView> target;
    bool ready{}, readable{};
};
struct Shared
{
    ComPtr<ID3D11Device> source;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Texture2D> cleanWorld;
    ComPtr<ID3D11Texture2D> cleanSilhouette;
    ComPtr<ID3D11RenderTargetView> silhouetteTarget;
    ComPtr<ID3D11Resource> silhouetteSource;
    ComPtr<ID3D11ShaderResourceView> silhouetteSourceView;
    Contribution contributions[2];
    ComPtr<ID3D11Texture2D> cloudSource;
    ComPtr<ID3D11ShaderResourceView> cloudView;
    ComPtr<ID3D11Texture2D> waterSource;
    ComPtr<ID3D11ShaderResourceView> waterView;
    int layerFrame = -1;
    int stencilMaskFrame = -1;
    std::unique_ptr<DeferredStencil> stencilDecoder;
    std::unique_ptr<FireflyLayerCapture> independentFirefly;
    std::unique_ptr<FireflyLayerCapture> independentSky;
    std::unique_ptr<FireflyLayerCapture> independentWaterfall;
    std::unique_ptr<IndependentSourceCapture> independentSources;
    int independentFireflyFrame=-1;
    int independentSkyFrame=-1;
    int independentWaterfallFrame=-1;
    int independentEffectTiming=-1;
    ComPtr<ID3D11Texture2D> independentLocalClasses;
    bool independentLocalClassesValid{};
    bool cleanHasLayers{};
    float cleanCloudFade=1;
    LayerTiming layerTiming;
    int geometryTiming = -1;
    int waterTiming = -1;
    std::uint64_t silhouetteSerial{}, usedSilhouetteSerial{};
    bool cleanHasSilhouette{};
    ComPtr<ID3D11DeviceContext1> captureContext;
    ComPtr<ID3DDeviceContextState> captureState;
    ComPtr<ID3D11RenderTargetView> captureTarget;
    ComPtr<ID3D11VertexShader> captureVS;
    ComPtr<ID3D11PixelShader> capturePS;
    ComPtr<ID3D11PixelShader> silhouetteCapturePS;
    ComPtr<ID3D11PixelShader> contributionPS;
    ComPtr<ID3D11BlendState> contributionBlend;
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
static std::shared_ptr<Shared> independentCaptureShared;
static std::shared_ptr<Shared> independentSkyCaptureShared;
static std::shared_ptr<Shared> independentWaterfallCaptureShared;
extern "C" __declspec(dllexport) int CanFrameGenerationIndependentCapture()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return IndependentMode() && shared!=nullptr && enabled.load() && allowed.load()?1:0;
}
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
#ifdef KOE_CAPTURE_AUDIT
    LayerPassAudit::Start(value->context.Get(), auditRoot);
#endif
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
        auto maskDesc = desc;
        maskDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        Check(value->source->CreateTexture2D(&maskDesc, nullptr, &slot.silhouette), "shared silhouette");
        Check(slot.silhouette.As(&slot.silhouetteMutex), "silhouette mutex");
        resource.Reset();
        Check(slot.silhouette.As(&resource), "silhouette resource");
        Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &slot.silhouetteHandle), "silhouette handle");
        if(IndependentMode())
        {
            auto layers=desc;layers.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            layers.ArraySize=LayerReconstruction::PackedSlices;
            Check(value->source->CreateTexture2D(&layers,nullptr,&slot.layers),"independent shared array");
            Check(slot.layers.As(&slot.layerMutex),"independent layer mutex");
            resource.Reset();Check(slot.layers.As(&resource),"independent layer resource");
            Check(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,
                nullptr,&slot.layerHandle),"independent layer handle");
        }
    }
    desc.MiscFlags = 0;
    Check(value->source->CreateTexture2D(&desc, nullptr, &value->cleanWorld), "pre-UI world texture");
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Check(value->source->CreateTexture2D(&desc, nullptr, &value->cleanSilhouette), "clean silhouette");
    Check(value->source->CreateRenderTargetView(value->cleanSilhouette.Get(), nullptr, &value->silhouetteTarget), "silhouette target");
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
            if (slot.silhouetteMutex->AcquireSync(0, 0) != S_OK)
            {
                slot.finalMutex->ReleaseSync(0);
                slot.mutex->ReleaseSync(0);
                continue;
            }
            if(slot.layerMutex && slot.layerMutex->AcquireSync(0,0)!=S_OK)
            {
                slot.silhouetteMutex->ReleaseSync(0);slot.finalMutex->ReleaseSync(0);slot.mutex->ReleaseSync(0);
                continue;
            }
            value.context->CopyResource(slot.texture.Get(), value.cleanWorld.Get());
            value.context->CopyResource(slot.finalTexture.Get(), finalImage);
            slot.hasSilhouette = value.cleanHasSilhouette;
            if (slot.hasSilhouette) silhouetteFrames++;
            if (slot.hasSilhouette) value.context->CopyResource(slot.silhouette.Get(), value.cleanSilhouette.Get());
            slot.hasLayers=value.cleanHasLayers;slot.cloudFade=value.cleanCloudFade;
            slot.cloudFilmic=cloudInput!=nullptr;
            if(slot.hasLayers)value.context->CopyResource(slot.layers.Get(),value.independentSources->Image());
            value.context->Flush();
            if(slot.layerMutex)Check(slot.layerMutex->ReleaseSync(1),"independent source release");
            Check(slot.silhouetteMutex->ReleaseSync(1), "silhouette release");
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
    if (!values || (count != 9 && count != 11 && count != 13 && count != 17))
        return;
    values[0] = qualityRequests.load();
    for (int i = 0; i < 8; i++)
        values[i + 1] = qualityReasons[i].load();
    if (count >= 11)
    {
        values[9] = flowExecutions.load();
        values[10] = preflightVetoes.load();
    }
    if (count >= 13)
    {
        values[11] = inputWaitUs.load();
        values[12] = decisionAgeUs.load();
    }
    if (count >= 17)
    {
        values[13] = analyzedPixels.load();
        values[14] = safePixels.load();
        values[15] = synthesizedPixels.load();
        values[16] = composedPixels.load();
    }
}
static void CaptureWorld(Shared &value, ID3D11Texture2D *image, DXGI_FORMAT format, bool silhouette = false)
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
        const char *maskPixel = R"HLSL(
Texture2D<float4> image:register(t0);
Texture2D<float> firefly:register(t1);
Texture2D<float> transparentEffects:register(t2);
Texture2D<float4> clouds:register(t3);
Texture2D<float> oceanDepth:register(t4);
float4 main(float4 p:SV_Position):SV_Target
{
    int2 pixel=int2(p.xy);
    float4 layer=image.Load(int3(pixel,0));
    uint flags=uint(round(layer.a*255));
    uint surface=uint(round(layer.r*255));
    if(surface>=1 && surface<=5)flags=128;
    uint ow,oh;oceanDepth.GetDimensions(ow,oh);
    if(ow>2 && oh>2)
    {
        uint w,h;image.GetDimensions(w,h);
        int2 oceanPixel=int2((p.xy/float2(w,h))*float2(ow,oh));
        oceanPixel.y=int(oh)-1-oceanPixel.y;
        float depth=oceanDepth.Load(int3(oceanPixel,0));
        if(depth>0 && depth<1)layer.rgb=float3(3.0/255,0,0);
    }
    uint cw,ch;clouds.GetDimensions(cw,ch);
    if(cw>2 && ch>2)
    {
        uint w,h;image.GetDimensions(w,h);
        int2 cloudPixel=int2((p.xy/float2(w,h))*float2(cw,ch));
        cloudPixel.y=int(ch)-1-cloudPixel.y;
        float transmission=clouds.Load(int3(cloudPixel,0)).a;
        if(transmission<1.0-1.0/1024)flags|=2;
    }
    uint w,h,fw,fh;image.GetDimensions(w,h);firefly.GetDimensions(fw,fh);
    if(w==fw && h==fh)
    {
        int2 effectPixel=int2(pixel.x,int(h)-1-pixel.y);
        if(firefly.Load(int3(effectPixel,0))>.5)flags|=1;
    }
    transparentEffects.GetDimensions(fw,fh);
    if(w==fw && h==fh && transparentEffects.Load(int3(pixel.x,int(h)-1-pixel.y,0))>.5)flags|=4;
    layer.a=flags/255.0;
    return layer;
}
)HLSL";
        blob.Reset();
        errors.Reset();
        Check(D3DCompile(maskPixel, strlen(maskPixel), nullptr, nullptr, nullptr, "main", "ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors), "silhouette capture PS");
        Check(value.source->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
            &value.silhouetteCapturePS), "silhouette capture PS create");
        const char *difference=R"HLSL(
Texture2D<float4> beforeImage:register(t0);Texture2D<float4> afterImage:register(t1);
float main(float4 p:SV_Position):SV_Target
{
    return any(abs(beforeImage.Load(int3(int2(p.xy),0)).rgb-afterImage.Load(int3(int2(p.xy),0)).rgb)>1.0/1024)?1:0;
}
)HLSL";
        blob.Reset();errors.Reset();
        Check(D3DCompile(difference,strlen(difference),nullptr,nullptr,nullptr,"main","ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors),"contribution shader compile");
        Check(value.source->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&value.contributionPS),"contribution shader");
        D3D11_BLEND_DESC blend{};
        auto &rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;
        rt.SrcBlend=rt.DestBlend=rt.SrcBlendAlpha=rt.DestBlendAlpha=D3D11_BLEND_ONE;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_MAX;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
        Check(value.source->CreateBlendState(&blend,&value.contributionBlend),"contribution union blend");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        Check(value.source->CreateRasterizerState(&rd, &value.captureRaster), "capture rasterizer");
    }
    auto &source = silhouette ? value.silhouetteSource : value.captureSource;
    auto &sourceView = silhouette ? value.silhouetteSourceView : value.captureSourceView;
    if (source.Get() != image)
    {
        sourceView.Reset();
        source = image;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = (format == DXGI_FORMAT_R16G16B16A16_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                         ? DXGI_FORMAT_R16G16B16A16_FLOAT
                     : ColorFamily(format) == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                : DXGI_FORMAT_B8G8R8A8_UNORM;
        Check(value.source->CreateShaderResourceView(image, &srv, &sourceView), "capture source view");
    }
    ComPtr<ID3DDeviceContextState> previous;
    value.captureContext->SwapDeviceContextState(value.captureState.Get(), &previous);
    auto *target = silhouette ? value.silhouetteTarget.Get() : value.captureTarget.Get();
    auto *view = sourceView.Get();
    value.context->OMSetRenderTargets(1, &target, nullptr);
    value.context->OMSetBlendState(nullptr,nullptr,0xffffffff);
    value.context->OMSetDepthStencilState(nullptr,0);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(value.desc.Width), static_cast<float>(value.desc.Height), 0, 1};
    value.context->RSSetViewports(1, &vp);
    value.context->RSSetState(value.captureRaster.Get());
    value.context->IASetInputLayout(nullptr);
    value.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    value.context->VSSetShader(value.captureVS.Get(), nullptr, 0);
    value.context->PSSetShader(silhouette ? value.silhouetteCapturePS.Get() : value.capturePS.Get(), nullptr, 0);
    value.context->PSSetShaderResources(0, 1, &view);
    ID3D11ShaderResourceView *effects[4]{};
    if (silhouette)
        for (unsigned i=0;i<2;++i) if(value.contributions[i].ready)effects[i]=value.contributions[i].maskView.Get();
    if(silhouette && cloudInput)
    {
        ComPtr<ID3D11Device> owner;cloudInput->GetDevice(&owner);
        if(owner.Get()==value.source.Get())
        {
            if(value.cloudSource!=cloudInput)
            {
                value.cloudView.Reset();value.cloudSource=cloudInput;
                D3D11_TEXTURE2D_DESC desc{};cloudInput->GetDesc(&desc);
                D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
                srv.Format=desc.Format;
                if(desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS)srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
                if(desc.Format==DXGI_FORMAT_R32G32B32A32_TYPELESS)srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
                if(ColorFamily(desc.Format)==1)srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                if(ColorFamily(desc.Format)==2)srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
                if(FAILED(value.source->CreateShaderResourceView(cloudInput.Get(),&srv,&value.cloudView)))layerSourceFailures++;
            }
            effects[2]=value.cloudView.Get();
        }
    }
    if(silhouette && waterInput)
    {
        ComPtr<ID3D11Device> owner;waterInput->GetDevice(&owner);
        if(owner.Get()==value.source.Get())
        {
            if(value.waterSource.Get()!=waterInput.Get())
            {
                value.waterView.Reset();value.waterSource=waterInput;
                D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
                Check(value.source->CreateShaderResourceView(waterInput.Get(),&srv,&value.waterView),"ocean coverage SRV");
            }
            effects[3]=value.waterView.Get();
        }
    }
    value.context->PSSetShaderResources(1, 4, effects);
    int layerTimer = silhouette ? value.layerTiming.Begin(value.source.Get(),value.context.Get(),2,value.cleanSerial+1) : -1;
    value.context->Draw(3, 0);
    value.layerTiming.End(value.context.Get(),layerTimer);
    view = nullptr;
    value.context->PSSetShaderResources(0, 1, &view);
    effects[0]=effects[1]=effects[2]=effects[3]=nullptr;
    value.context->PSSetShaderResources(1, 4, effects);
    value.captureContext->SwapDeviceContextState(previous.Get(), &value.captureState);
}
static void CaptureContribution(Shared &value, unsigned index, bool end)
{
    auto &effect=value.contributions[index];
    if(!end)
    {
        effect.source.Reset();
        ComPtr<ID3D11RenderTargetView> target;
        value.context->OMGetRenderTargets(1,&target,nullptr);
        if(!target)return;
        ComPtr<ID3D11Resource> resource;target->GetResource(&resource);
        if(FAILED(resource.As(&effect.source)))return;
    }
    if(!effect.source)return;
    D3D11_TEXTURE2D_DESC desc{};effect.source->GetDesc(&desc);
    if(desc.Width!=value.desc.Width || desc.Height!=value.desc.Height || desc.SampleDesc.Count!=1)return;
    auto format=desc.Format;
    if(format==DXGI_FORMAT_R16G16B16A16_TYPELESS || format==DXGI_FORMAT_R16G16B16A16_FLOAT)format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    else if(ColorFamily(format)==1)format=DXGI_FORMAT_R8G8B8A8_UNORM;
    else if(ColorFamily(format)==2)format=DXGI_FORMAT_B8G8R8A8_UNORM;
    else return;
    if(!value.captureState)CaptureWorld(value,effect.source.Get(),desc.Format);
    bool readable=(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)!=0;
    if(readable && effect.sourceResource.Get()!=effect.source.Get())
    {
        effect.sourceView.Reset();effect.sourceResource=effect.source;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;
        srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        Check(value.source->CreateShaderResourceView(effect.source.Get(),&srv,&effect.sourceView),"contribution source view");
    }
    D3D11_TEXTURE2D_DESC previous{};
    if(effect.before)effect.before->GetDesc(&previous);
    if(!effect.before || previous.Format!=desc.Format || previous.Width!=desc.Width || previous.Height!=desc.Height || effect.readable!=readable)
    {
        if(end){effect.source.Reset();return;}
        effect.beforeView.Reset();effect.before.Reset();effect.maskView.Reset();effect.target.Reset();effect.mask.Reset();
        effect.after.Reset();effect.afterView.Reset();
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=desc.CPUAccessFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;
        Check(value.source->CreateTexture2D(&desc,nullptr,&effect.before),"contribution history");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;
        srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        Check(value.source->CreateShaderResourceView(effect.before.Get(),&srv,&effect.beforeView),"contribution history view");
        if(!readable)
        {
            Check(value.source->CreateTexture2D(&desc,nullptr,&effect.after),"contribution unreadable target copy");
            Check(value.source->CreateShaderResourceView(effect.after.Get(),&srv,&effect.afterView),"contribution target copy view");
        }
        desc.Format=DXGI_FORMAT_R8_UNORM;desc.BindFlags|=D3D11_BIND_RENDER_TARGET;
        Check(value.source->CreateTexture2D(&desc,nullptr,&effect.mask),"contribution coverage");
        Check(value.source->CreateShaderResourceView(effect.mask.Get(),nullptr,&effect.maskView),"contribution coverage view");
        Check(value.source->CreateRenderTargetView(effect.mask.Get(),nullptr,&effect.target),"contribution coverage target");
        effect.ready=false;
        effect.readable=readable;
    }
    int timer=value.layerTiming.Begin(value.source.Get(),value.context.Get(),1,value.cleanSerial+1);
    if(!end)value.context->CopyResource(effect.before.Get(),effect.source.Get());
    else
    {
        if(!readable)value.context->CopyResource(effect.after.Get(),effect.source.Get());
        ComPtr<ID3DDeviceContextState> restore;
        value.captureContext->SwapDeviceContextState(value.captureState.Get(),&restore);
        auto *target=effect.target.Get();
        if(!effect.ready){float zeros[4]{};value.context->ClearRenderTargetView(target,zeros);}
        value.context->OMSetRenderTargets(1,&target,nullptr);
        value.context->OMSetBlendState(value.contributionBlend.Get(),nullptr,0xffffffff);
        D3D11_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};
        value.context->RSSetViewports(1,&viewport);value.context->RSSetState(value.captureRaster.Get());
        value.context->IASetInputLayout(nullptr);value.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        value.context->VSSetShader(value.captureVS.Get(),nullptr,0);value.context->PSSetShader(value.contributionPS.Get(),nullptr,0);
        ID3D11ShaderResourceView *views[]={effect.beforeView.Get(),readable?effect.sourceView.Get():effect.afterView.Get()};
        value.context->PSSetShaderResources(0,2,views);value.context->Draw(3,0);
        views[0]=views[1]=nullptr;value.context->PSSetShaderResources(0,2,views);
        value.captureContext->SwapDeviceContextState(restore.Get(),&value.captureState);
        effect.ready=true;effect.source.Reset();if(index==0)fireflyPairs++;
    }
    value.layerTiming.End(value.context.Get(),timer);
}
static void __stdcall BeforeUI(int event)
{
    bool effectCapture=event==13||event==15||event==21||event==23||event==25||event==27;
    if (((event < 1 || event > 27) && (event & 255) != 11 && (event & 255) != 12 && (event & 7) != 6)
        || ((!enabled.load() || !allowed.load()) && !effectCapture))
        return;
    try
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if(effectCapture && IndependentMode())
        {
            bool sky=event==21||event==23;
            bool waterfall=event==25||event==27;
            auto &pinned=waterfall?independentWaterfallCaptureShared:sky?independentSkyCaptureShared:independentCaptureShared;
            if(event==13||event==21||event==25)
            {
                if(!shared)return;
                auto &capture=waterfall?shared->independentWaterfall:sky?shared->independentSky:shared->independentFirefly;
                if(!capture)capture=std::make_unique<FireflyLayerCapture>(shared->source.Get(),shared->context.Get());
                if(capture->Begin(sky))
                {
                    pinned=shared;
                    shared->independentEffectTiming=shared->layerTiming.Begin(shared->source.Get(),shared->context.Get(),1,shared->cleanSerial+1);
                }
            }
            else
            {
                auto captured=pinned;
                if(!captured)return;
                auto &capture=waterfall?captured->independentWaterfall:sky?captured->independentSky:captured->independentFirefly;
                capture->End();
                captured->layerTiming.End(captured->context.Get(),captured->independentEffectTiming);captured->independentEffectTiming=-1;
                (waterfall?captured->independentWaterfallFrame:sky?captured->independentSkyFrame:captured->independentFireflyFrame)=captured->layerFrame;
                if(!sky && !waterfall && capture->Image())independentFireflyCopies++;
                pinned.reset();
            }
            return;
        }
        if (!shared)
            return;
        if ((event & 7) == 6)
        {
            int frame = event >> 3;
            if (frame != shared->layerFrame)
            {
                shared->layerFrame = frame;
                for(auto &effect:shared->contributions)effect.ready=false;
                shared->independentLocalClassesValid=false;
                if(shared->independentSources)shared->independentSources->BeginFrame();
            }
            return;
        }
        if(event==19){shared->independentLocalClassesValid=false;return;}
        if((event&255)==11 || (event&255)==12)
        {
            stencilAttempts++;
            bool celestial=(event&255)==12;
            auto &failures=celestial?celestialStencilFailures:stencilFailures;
            int timer=-1;
            try
            {
                ComPtr<ID3D11DepthStencilView> depth;
                shared->context->OMGetRenderTargets(0,nullptr,&depth);
                if(!depth || !silhouetteInput){failures++;return;}
                if(!shared->stencilDecoder)shared->stencilDecoder=std::make_unique<DeferredStencil>(shared->source.Get());
                unsigned options=unsigned(event)>>8;
                timer=shared->layerTiming.Begin(shared->source.Get(),shared->context.Get(),0,shared->cleanSerial+1);
                bool success=shared->stencilDecoder->Capture(depth.Get(),silhouetteInput.Get(),
                    (options&1)!=0 || shared->stencilMaskFrame!=shared->layerFrame,(options&2)!=0,(options&4)!=0,celestial?4u:0u);
                if(success && IndependentMode() && !celestial)
                {
                    if(!shared->independentLocalClasses)
                    {
                        D3D11_TEXTURE2D_DESC desc{};silhouetteInput->GetDesc(&desc);
                        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                        Check(shared->source->CreateTexture2D(&desc,nullptr,&shared->independentLocalClasses),"independent local camera classes");
                    }
                    shared->independentLocalClassesValid=shared->stencilDecoder->Capture(depth.Get(),shared->independentLocalClasses.Get(),true,false,(options&4)!=0);
                }
                shared->layerTiming.End(shared->context.Get(),timer);
                timer=-1;
                if(success){stencilSuccesses++;shared->stencilMaskFrame=shared->layerFrame;shared->silhouetteSerial++;}
                else failures++;
            }
            catch(const std::exception &)
            {if(timer>=0)shared->layerTiming.End(shared->context.Get(),timer);failures++;}
            return;
        }
        if(event==7 || event==8)
        {
            if(event==7)shared->waterTiming=shared->layerTiming.Begin(shared->source.Get(),shared->context.Get(),1,shared->cleanSerial+1);
            else { shared->layerTiming.End(shared->context.Get(),shared->waterTiming);shared->waterTiming=-1; }
            return;
        }
        if (event==5)
        {
            if(shared->geometryTiming>=0)shared->layerTiming.End(shared->context.Get(),shared->geometryTiming);
            shared->geometryTiming=shared->layerTiming.Begin(shared->source.Get(),shared->context.Get(),0,shared->cleanSerial+1);
            return;
        }
        if (event==3 || event==4)
        {
            CaptureContribution(*shared,0,event==4);
            return;
        }
        if(event==9 || event==10 || event==20)
        {
            CaptureContribution(*shared,1,event==10);
            if(event!=10 && IndependentMode())
            {
                if(!shared->independentSources)shared->independentSources=std::make_unique<IndependentSourceCapture>(shared->source.Get(),shared->context.Get(),shared->desc.Width,shared->desc.Height);
                shared->independentSources->CaptureOpaque(shared->contributions[1].before.Get(),
                    event==9 && shared->independentLocalClassesValid?shared->independentLocalClasses.Get():nullptr);
            }
            return;
        }
        if (event == 2)
        {
            shared->layerTiming.End(shared->context.Get(),shared->geometryTiming);
            shared->geometryTiming=-1;
            if (!silhouetteInput) return;
            D3D11_TEXTURE2D_DESC desc{};
            silhouetteInput->GetDesc(&desc);
            ComPtr<ID3D11Device> owner;
            silhouetteInput->GetDevice(&owner);
            if (owner.Get() != shared->source.Get() || desc.Width != shared->desc.Width || desc.Height != shared->desc.Height
                || desc.SampleDesc.Count != 1 || !Supported(desc.Format)) return;
            shared->silhouetteSerial++;
            shared->stencilMaskFrame=shared->layerFrame;
            return;
        }
        ComPtr<ID3D11RenderTargetView> target;
        shared->layerTiming.Poll(shared->context.Get());
        if(IndependentMode() && independentFireflyCopies.load()>10)atmosphereProbe.Save(shared->context.Get());
        for(unsigned i=0;i<3;++i)layerGpuUs[i]=static_cast<long long>(1000*shared->layerTiming.milliseconds[i]);
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
        shared->cleanHasSilhouette = shared->silhouetteSerial != shared->usedSilhouetteSerial;
        if(shared->cleanHasSilhouette && silhouetteInput)
        {
            D3D11_TEXTURE2D_DESC maskDesc{};silhouetteInput->GetDesc(&maskDesc);
            CaptureWorld(*shared,silhouetteInput.Get(),maskDesc.Format,true);
            silhouetteCopies++;
        }
        shared->usedSilhouetteSerial = shared->silhouetteSerial;
        shared->cleanHasLayers=false;
        if(IndependentMode() && shared->cleanHasSilhouette && shared->contributions[1].ready)
        {
            if(!shared->independentSources)shared->independentSources=std::make_unique<IndependentSourceCapture>(shared->source.Get(),shared->context.Get(),td.Width,td.Height);
            ID3D11Texture2D *fire=shared->independentFirefly && shared->independentFireflyFrame==shared->layerFrame
                ?shared->independentFirefly->Image():nullptr;
            ID3D11Texture2D *sky=shared->independentSky && shared->independentSkyFrame==shared->layerFrame
                ?shared->independentSky->Image():nullptr;
            ID3D11Texture2D *waterfall=shared->independentWaterfall && shared->independentWaterfallFrame==shared->layerFrame
                ?shared->independentWaterfall->Image():nullptr;
            bool atmosphere=atmosphereProbe.Tone()==2;
            int packTiming=shared->layerTiming.Begin(shared->source.Get(),shared->context.Get(),2,shared->cleanSerial+1);
            shared->cleanHasLayers=shared->independentSources->Capture(shared->contributions[1].before.Get(),shared->cleanSilhouette.Get(),cloudInput.Get(),fire,sky,
                atmosphere?atmosphereProbe.Color():nullptr,atmosphere?atmosphereProbe.Extinction():nullptr,atmosphere?atmosphereProbe.Depth():nullptr,waterfall);
            shared->layerTiming.End(shared->context.Get(),packTiming);
            shared->cleanCloudFade=independentCloudFade.load();
        }
        if(IndependentMode())
        {if(shared->cleanHasLayers)independentPacked++;else independentUnavailable++;}
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
extern "C" __declspec(dllexport) void SetFrameGenerationSilhouette(ID3D11Texture2D *image)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    silhouetteInput = image;
}
extern "C" __declspec(dllexport) void ReadFrameGenerationStencil(long long *values,int count)
{
    if(!values || count<3)return;
    values[0]=stencilAttempts;values[1]=stencilSuccesses;values[2]=stencilFailures;
    if(count>=4)values[3]=celestialStencilFailures;
}
extern "C" __declspec(dllexport) void SetFrameGenerationCloudLayer(ID3D11Texture2D *image)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    cloudInput=image;
    if(!image && shared) { shared->cloudView.Reset();shared->cloudSource.Reset(); }
}
extern "C" __declspec(dllexport) void SetFrameGenerationWaterLayer(ID3D11Texture2D *image)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    waterInput=image;
    if(!image && shared) { shared->waterView.Reset();shared->waterSource.Reset(); }
}
extern "C" __declspec(dllexport) void ReadFrameGenerationLayers(long long *values,int count)
{
    if(!values || count<5)return;
    for(unsigned i=0;i<4;++i)values[i]=layerGpuUs[i].load();
    values[4]=fireflyPairs.load();
    if(count>=6)values[5]=layerSourceFailures.load();
}
extern "C" __declspec(dllexport) void ReadFrameGenerationSilhouette(long long *values, int count)
{
    if (!values || count != 2) return;
    values[0] = silhouetteCopies.load();
    values[1] = silhouetteFrames.load();
}
extern "C" __declspec(dllexport) void *GetFrameGenerationRenderEvent()
{
    return reinterpret_cast<void *>(BeforeUI);
}
struct Presenter
{
    PresentationCadence cadencePolicy;
    TargetFrameRate::Planner targetPlanner;
    TargetFrameRate::PresentationClock presentationClock;
    HWND window{};
    std::shared_ptr<Shared> source;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap, nativeSwap;
    UINT lastDisplayCount{};
    bool displayCountKnown{};
    ComPtr<ID3D11Texture2D> back, input, finalImage, opened[3], openedFinal[3];
    ComPtr<ID3D11Texture2D> openedSilhouette[3], silhouetteFrame;
    ComPtr<ID3D11Texture2D> openedLayers[3];
    ComPtr<IDXGIKeyedMutex> layerMutexes[3];
    ComPtr<ID3D11Texture2D> diagnosticLayers;
    inline static unsigned independentSnapshots{};
    void SnapshotIndependent(int index,double timestamp)
    {
        if(!IndependentMode() || independentSnapshots>=2 || !source->slots[index].hasLayers)return;
        wchar_t destination[32768]{};DWORD length=GetEnvironmentVariableW(L"KOE_INDEPENDENT_SNAPSHOT",destination,32768);
        if(!length || length>=32768)return;
        bool fire=independentFireflyCopies.load()>10;
        if((independentSnapshots==0 && independentPacked.load()<30) || (independentSnapshots==1 && !fire))return;
        auto directory=std::filesystem::path(destination)/(independentSnapshots?"firefly":"initial");
        if(std::filesystem::exists(directory))throw std::runtime_error("Independent snapshot already exists");
        std::filesystem::create_directories(directory);auto start=Clock::now();
        auto save=[&](ID3D11Texture2D *image,const char *name)
        {
            D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
            unsigned bytes=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&desc,nullptr,&staging),"independent snapshot staging");
            context->CopyResource(staging.Get(),image);
            for(unsigned slice=0;slice<desc.ArraySize;slice++)
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(staging.Get(),slice,D3D11_MAP_READ,0,&mapped),"independent snapshot map");
                std::ofstream file(directory/(std::string(name)+"-"+std::to_string(slice)+".raw"),std::ios::binary);
                for(unsigned y=0;y<desc.Height;y++)file.write(static_cast<char*>(mapped.pData)+y*mapped.RowPitch,desc.Width*bytes);
                context->Unmap(staging.Get(),slice);if(!file)throw std::runtime_error("Independent snapshot write failed");
            }
        };
        save(input.Get(),"world");save(finalImage.Get(),"final");save(openedLayers[index].Get(),"layers");
        std::ofstream info(directory/"capture.json");
        info<<"{\"width\":"<<source->desc.Width<<",\"height\":"<<source->desc.Height
            <<",\"timestamp\":"<<timestamp<<",\"cloudFilmic\":"<<source->slots[index].cloudFilmic
            <<",\"cloudFade\":"<<source->slots[index].cloudFade<<",\"readbackMs\":"
            <<std::chrono::duration<double,std::milli>(Clock::now()-start).count()<<"}";
        independentSnapshots++;
    }
    ComPtr<ID3D11ShaderResourceView> finalViews[3], inputSrv, finalSrv;
    ComPtr<IDXGIKeyedMutex> mutexes[3];
    ComPtr<ID3D11RenderTargetView> inputView;
    ComPtr<ID3D11ShaderResourceView> views[3];
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, uiPS;
    ComPtr<ID3D11RenderTargetView> finalView, backView;
    ComPtr<IDXGIKeyedMutex> finalMutexes[3];
    ComPtr<IDXGIKeyedMutex> silhouetteMutexes[3];
    ComPtr<ID3D11ShaderResourceView> interpolatedView;
    std::unique_ptr<Interpolator> fruc;
    GpuQueueLimit sourceQueue;
    struct GpuFrame
    {
        struct PhaseFrame
        {
            std::shared_ptr<Interpolator::Ticket> ticket;
            ComPtr<ID3D11Texture2D> composed;
            ComPtr<ID3D11RenderTargetView> target;
            ComPtr<ID3D11ShaderResourceView> view;
            bool resolved{};
        };
        std::shared_ptr<Interpolator::Ticket> ticket;
        ComPtr<ID3D11Texture2D> composed, real;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11ShaderResourceView> composedView, realView;
        double timestamp{}, cadence{}, deadline{};
        bool requested{}, resolved{}, late{};
        std::vector<PhaseFrame> phases;
        unsigned phaseCount{};
        unsigned nextPhase{};
    };
    std::array<GpuFrame, 2> frames;
    std::deque<unsigned> readyFrames;
    bool displaying{};
    bool lastHashKnown{};
    std::uint64_t lastPresentedHash{};
    unsigned phaseCapacity = 1;
    double nextDisplayCheck{};
    double lastTimestamp{}, interval = 1.0 / 30, refreshInterval = 1.0 / TargetFrameRate::Default;
    struct Pending
    {
        std::shared_ptr<Interpolator::Ticket> ticket;
        bool presented;
    };
    std::deque<Pending> pending;
#ifdef KOE_CAPTURE_AUDIT
    ComPtr<ID3D11Texture2D> auditWorld[3], auditFinal, auditGenerated, auditComposed;
    ComPtr<ID3D11Texture2D> auditSilhouette[3];
    static constexpr unsigned DatasetFrames = 5;
    ComPtr<ID3D11Texture2D> datasetWorld[DatasetFrames], datasetLayers[DatasetFrames];
    double datasetTimes[DatasetFrames]{};
    std::uint64_t datasetSerials[DatasetFrames]{};
    unsigned datasetIndex{};
    int datasetID = -1;
    double datasetStart{};
    bool auditHasSilhouette[3]{};
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
        sd.BufferCount = 4;
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
        UpdateDisplay();
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
        Check(device->CreateTexture2D(&td, nullptr, &silhouetteFrame), "private silhouette");
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
            Check(device1->OpenSharedResource1(source->slots[i].silhouetteHandle, IID_PPV_ARGS(&openedSilhouette[i])), "open silhouette");
            Check(openedSilhouette[i].As(&silhouetteMutexes[i]), "worker silhouette mutex");
            if(source->slots[i].layerHandle)
            {
                Check(device1->OpenSharedResource1(source->slots[i].layerHandle,IID_PPV_ARGS(&openedLayers[i])),"open independent layer array");
                Check(openedLayers[i].As(&layerMutexes[i]),"worker independent layer mutex");
            }
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
#ifdef KOE_DIRECT_FLOW
        fruc.reset(new Interpolator(device.Get(), td.Width, td.Height, library, true, NV_OF_PERF_LEVEL_FAST));
        std::uint64_t bytesPerPhase = std::uint64_t(td.Width)*td.Height*4*frames.size();
        DXGI_ADAPTER_DESC adapterDesc{};
        adapter->GetDesc(&adapterDesc);
        std::uint64_t reserve = adapterDesc.DedicatedVideoMemory/16;
        if (!reserve) reserve = adapterDesc.SharedSystemMemory/64;
        ComPtr<IDXGIAdapter3> budgetAdapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (SUCCEEDED(adapter.As(&budgetAdapter))
            && SUCCEEDED(budgetAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
            reserve = memory.Budget > memory.CurrentUsage ? (memory.Budget-memory.CurrentUsage)/4 : 0;
        phaseCapacity = static_cast<unsigned>(std::min<std::uint64_t>(reserve/bytesPerPhase, UINT_MAX));
        phaseCapacity = std::min(phaseCapacity, TargetFrameRate::MaxAdditionalFrames);
        fruc->ReserveDecisions(phaseCapacity*static_cast<unsigned>(frames.size())+2);
#else
        fruc.reset(new Interpolator(device.Get(), td.Width, td.Height, library));
#endif
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
    void SavePixels(ID3D11Texture2D *texture, const char *name, const std::filesystem::path &directory = {})
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
        std::ofstream stream((directory.empty() ? auditDirectory : directory) / name, std::ios::binary);
        for (unsigned y = 0; y < d.Height; y++)
            stream.write(static_cast<const char *>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * 4);
        context->Unmap(stage.Get(), 0);
        if (!stream)
            throw std::runtime_error("audit write failed");
    }
    void CaptureDataset(bool hasLayers, double timestamp, std::uint64_t serial)
    {
        int request=datasetRequest.exchange(-1);
        if(request>=0)
        {
            datasetID=request;datasetIndex=0;datasetStart=datasetEarliest.load();
        }
        if(datasetID<0 || timestamp<datasetStart || !hasLayers
            || (datasetIndex && serial<=datasetSerials[datasetIndex-1]))return;
        Snapshot(input.Get(),datasetWorld[datasetIndex]);
        Snapshot(silhouetteFrame.Get(),datasetLayers[datasetIndex]);
        datasetTimes[datasetIndex]=timestamp;datasetSerials[datasetIndex]=serial;
        if(++datasetIndex<DatasetFrames)return;
        auto directory=auditRoot/("case-"+std::to_string(datasetID));
        if(!std::filesystem::create_directories(directory))throw std::runtime_error("Dataset output already exists");
        for(unsigned i=0;i<DatasetFrames;++i)
        {
            SavePixels(datasetWorld[i].Get(),("world-"+std::to_string(i)+".rgba").c_str(),directory);
            SavePixels(datasetLayers[i].Get(),("layers-"+std::to_string(i)+".rgba").c_str(),directory);
            datasetWorld[i].Reset();datasetLayers[i].Reset();
        }
        {
            std::ofstream info(directory/"capture.json.tmp");info.precision(17);
            info<<"{\"width\":"<<source->desc.Width<<",\"height\":"<<source->desc.Height<<",\"id\":"<<datasetID
                <<",\"frames\":"<<DatasetFrames<<",\"times\":[";
            for(unsigned i=0;i<DatasetFrames;++i)info<<(i?",":"")<<datasetTimes[i];
            info<<"],\"serials\":[";
            for(unsigned i=0;i<DatasetFrames;++i)info<<(i?",":"")<<datasetSerials[i];
            info<<"],\"scope\":\"lossless pre-UI source sequence; diagnostic readbacks, not an FPS benchmark\"}";
            if(!info)throw std::runtime_error("Dataset metadata write failed");
        }
        std::filesystem::rename(directory/"capture.json.tmp",directory/"capture.json");
        datasetComplete=datasetID;datasetID=-1;datasetBusy=false;
    }
    void SaveAudit()
    {
        if (auditDone || auditDirectory.empty() || auditIndex != 3 || !auditFinal)
            return;
        std::filesystem::create_directories(auditDirectory);
        SavePixels(auditWorld[0].Get(), "world-0.rgba");
        SavePixels(auditWorld[1].Get(), "world-1.rgba");
        SavePixels(auditWorld[2].Get(), "world-2.rgba");
        for(unsigned i=0;i<3;++i)
            if(auditHasSilhouette[i]) SavePixels(auditSilhouette[i].Get(), ("silhouette-"+std::to_string(i)+".rgba").c_str());
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
             << ",\"silhouettes\":[" << auditHasSilhouette[0] << "," << auditHasSilhouette[1] << "," << auditHasSilhouette[2] << "]"
             << ",\"qualityReason\":" << auditQualityReason << ",\"qualityCounts\":[" << auditQualityCounts[0] << ","
             << auditQualityCounts[1] << "," << auditQualityCounts[2] << "," << auditQualityCounts[3]
             << "],\"scope\":\"diagnostic GPU readbacks, excluded from production\"}";
        auditDone = true;
        for (auto &texture : auditWorld)
            texture.Reset();
        for (auto &texture : auditSilhouette) texture.Reset();
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
    void UpdateDisplay()
    {
        if (Seconds() >= nextDisplayCheck)
        {
            double hz = ReadDisplayRefresh(source->window);
            if (hz > 0)
            {
                refreshInterval = 1/hz;
                monitorMilliHz = static_cast<long long>(std::llround(hz*1000));
            }
            nextDisplayCheck = Seconds()+1;
        }
        effectiveMilliFPS = static_cast<long long>(std::llround(TargetFrameRate::Effective(fullGeneration.load(), requestedTargetFPS.load(),
            1/refreshInterval)*1000));
    }
    void Compose(ID3D11RenderTargetView *target, ID3D11ShaderResourceView *world = nullptr)
    {
        D3D11_VIEWPORT viewport{0,0,float(source->desc.Width),float(source->desc.Height),0,1};
        context->RSSetViewports(1,&viewport);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(uiPS.Get(), nullptr, 0);
        ID3D11ShaderResourceView *images[] = {world ? world : interpolatedView.Get(), inputSrv.Get(), finalSrv.Get()};
        context->PSSetShaderResources(0, 3, images);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *empty[3]{};
        context->PSSetShaderResources(0, 3, empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    bool Present(ID3D11Texture2D *texture, bool midpoint = false, bool composed = false)
    {
        if (!Visible())
            return false;
        if (midpoint && !composed)
        {
            Compose(backView.Get());
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
        if(IndependentMode() && hr==S_OK)
        {
            DXGI_FRAME_STATISTICS statistics{};auto *chain=nativeSwap?nativeSwap.Get():swap.Get();
            if(chain->GetFrameStatistics(&statistics)==S_OK)
            {
                if(displayCountKnown && statistics.PresentCount>=lastDisplayCount)
                    observedDisplayed+=statistics.PresentCount-lastDisplayCount;
                lastDisplayCount=statistics.PresentCount;displayCountKnown=true;displayStatisticsSamples++;
            }
            else{displayStatisticsFailures++;displayCountKnown=false;}
        }
        if (nativeSwap)
            reshadeBypasses++;
        Check(hr, "generated presentation");
        return hr == S_OK;
    }
    void ResolveFrames()
    {
        for (auto &frame : frames)
        {
            if (!frame.ticket || frame.resolved) continue;
            bool complete=fruc->Completed(frame.ticket);
            if(!complete && !IndependentMode())continue;
            for (unsigned i=0;i<frame.phaseCount;++i)
            {
                auto &phase = frame.phases[i];
                if (phase.resolved) continue;
                if (!fruc->ResolveGpu(phase.ticket)) { complete = false; continue; }
                phase.resolved = true;
                const auto &ticket = *phase.ticket;
                if(IndependentMode())independentParityRejected+=ticket.counts[1];
                bool late = Seconds() > frame.deadline;
                qualityReasons[late ? Interpolator::Late : static_cast<int>(ticket.rejection)]++;
                decisionAgeUs = static_cast<long long>((Seconds()-frame.timestamp)*1000000);
                analyzedPixels += ticket.counts[3];
                safePixels += ticket.counts[4];
                synthesizedPixels += ticket.counts[5];
                composedPixels += ticket.counts[6];
            }
            frame.resolved = complete;
        }
        while (!readyFrames.empty())
        {
            auto &frame = frames[readyFrames.front()];
            if (!frame.resolved && !IndependentMode()) return;
            frame.late = Seconds() > frame.deadline;
            for (unsigned i=frame.nextPhase;i<frame.phaseCount;++i)
            {
                auto &phase = frame.phases[i];
                if(!phase.resolved)return;
                const auto &ticket = *phase.ticket;
                auto hash = (std::uint64_t(ticket.counts[8])<<32)|ticket.counts[9];
                if (!ticket.eligible || frame.late || Seconds()>frame.deadline
                    || (lastHashKnown && hash==lastPresentedHash)) {frame.nextPhase=i+1;continue;}
                if(!presentationClock.Ready(Seconds()))return;
                Show();
                displaying = true;
                if (Present(phase.composed.Get(), true, true))
                {
                    generated++;
                    generatedPresents++;
                    if(IndependentMode())
                    {
                        publishedChanged+=ticket.counts[5];publishedComposed+=ticket.counts[6];
                        publishedPixelTotal+=std::uint64_t(source->desc.Width)*source->desc.Height;
                        if(independentFireflyCopies.load()>10)
                            PresentedFrameSample::Capture(device.Get(),context.Get(),frame.composed.Get(),phase.composed.Get(),frame.real.Get(),frame.timestamp,i+1,frame.phaseCount,ticket.counts[6]);
                    }
                    lastHashKnown = true;
                    lastPresentedHash = hash;
                    presentationClock.Presented(Seconds(),effectiveMilliFPS.load()/1000.);
                }
                frame.nextPhase=i+1;
                return;
            }
            if(!frame.resolved)return;
            if (Visible() && displaying && !frame.late)
            {
                if(!presentationClock.Ready(Seconds()))return;
                if (Present(frame.real.Get()))
                {
                    presentationClock.Presented(Seconds(),effectiveMilliFPS.load()/1000.);
                    realPresents++;
                    lastHashKnown = frame.phaseCount != 0;
                    if(lastHashKnown)
                    {
                        const auto &counts=frame.phases[0].ticket->counts;
                        lastPresentedHash=(std::uint64_t(counts[10])<<32)|counts[11];
                    }
                }
            }
            else
            {
                if (IsWindowVisible(window)) ShowWindow(window, SW_HIDE);
                displaying = false;
                lastHashKnown = false;
                presentationClock.Reset();
            }
            for (auto &phase : frame.phases) phase.ticket.reset();
            frame.ticket.reset();
            readyFrames.pop_front();
        }
    }
    bool ProcessQueued(int index, double timestamp, double sourceInterval)
    {
        auto slot = std::find_if(frames.begin(), frames.end(), [](const GpuFrame &frame) { return !frame.ticket; });
        if (slot == frames.end()) return false;
        HRESULT acquired = mutexes[index]->AcquireSync(1, 0);
        if (acquired == WAIT_TIMEOUT) return false;
        if (acquired != S_OK) throw std::runtime_error("queued world ownership lost");
        acquired = finalMutexes[index]->AcquireSync(1, 0);
        if (acquired != S_OK)
        {
            Check(mutexes[index]->ReleaseSync(1), "queued world retry");
            if (acquired == WAIT_TIMEOUT) return false;
            throw std::runtime_error("queued UI ownership lost");
        }
        acquired = silhouetteMutexes[index]->AcquireSync(1, 0);
        if (acquired != S_OK)
        {
            Check(finalMutexes[index]->ReleaseSync(1), "queued UI retry");
            Check(mutexes[index]->ReleaseSync(1), "queued world retry");
            if (acquired == WAIT_TIMEOUT) return false;
            throw std::runtime_error("queued silhouette ownership lost");
        }
        if(layerMutexes[index])
        {
            acquired=layerMutexes[index]->AcquireSync(1,0);
            if(acquired!=S_OK)
            {
                Check(silhouetteMutexes[index]->ReleaseSync(1),"queued layers silhouette retry");
                Check(finalMutexes[index]->ReleaseSync(1),"queued layers UI retry");
                Check(mutexes[index]->ReleaseSync(1),"queued layers world retry");
                if(acquired==WAIT_TIMEOUT)return false;
                throw std::runtime_error("Independent layer ownership lost");
            }
        }
        auto begin = Clock::now();
        UpdateDisplay();
        Convert(views[index].Get(), inputView.Get());
        Convert(finalViews[index].Get(), finalView.Get());
        bool hasSilhouette = source->slots[index].hasSilhouette;
        if (hasSilhouette) context->CopyResource(silhouetteFrame.Get(), openedSilhouette[index].Get());
        if(IndependentMode())SnapshotIndependent(index,timestamp);
        context->Flush();
#ifdef KOE_CAPTURE_AUDIT
        CaptureDataset(hasSilhouette,timestamp,source->slots[index].serial);
        if(datasetMode)
        {
            if(layerMutexes[index])Check(layerMutexes[index]->ReleaseSync(0),"dataset independent layers release");
            Check(silhouetteMutexes[index]->ReleaseSync(0),"dataset silhouette release");
            Check(finalMutexes[index]->ReleaseSync(0),"dataset UI release");
            Check(mutexes[index]->ReleaseSync(0),"dataset world release");
            std::lock_guard<std::mutex> lock(stateMutex);
            source->slots[index].status=0;
            return true;
        }
#endif
        interval = sourceInterval;
        double pairInterval=lastTimestamp>0?timestamp-lastTimestamp:sourceInterval;
        lastTimestamp = timestamp;
        if (!slot->real)
        {
            D3D11_TEXTURE2D_DESC desc{};
            input->GetDesc(&desc);
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            Check(device->CreateTexture2D(&desc, nullptr, &slot->real), "queued real image");
            Check(device->CreateShaderResourceView(slot->real.Get(), nullptr, &slot->realView), "queued real SRV");
            desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
            Check(device->CreateTexture2D(&desc, nullptr, &slot->composed), "queued composed image");
            Check(device->CreateRenderTargetView(slot->composed.Get(), nullptr, &slot->target), "queued compositor target");
            Check(device->CreateShaderResourceView(slot->composed.Get(), nullptr, &slot->composedView), "queued composed SRV");
        }
        unsigned count = targetPlanner.Count(pairInterval, effectiveMilliFPS.load()/1000., Visible(), phaseCapacity);
        if (!adaptiveCadence.load()) count = std::min(count, 1u);
        slot->requested = count != 0;
        slot->timestamp = timestamp;
        slot->cadence = interval;
        slot->resolved = slot->late = false;
        // AI-generated comment: Two buffers cover the current GPU frame and one frame of presentation lookahead.
        slot->deadline = timestamp + interval * frames.size();
#ifdef KOE_DIRECT_FLOW
        if(IndependentMode())fruc->SetLayerInput(source->slots[index].hasLayers?openedLayers[index].Get():nullptr,
            source->slots[index].cloudFilmic,source->slots[index].cloudFade);
#endif
        slot->ticket = fruc->Submit(input.Get(), timestamp, false, true,
            hasSilhouette ? silhouetteFrame.Get() : nullptr);
        context->Flush();
        if(layerMutexes[index])Check(layerMutexes[index]->ReleaseSync(0),"queued independent layers release");
        Check(silhouetteMutexes[index]->ReleaseSync(0), "queued silhouette release");
        Check(finalMutexes[index]->ReleaseSync(0), "queued UI release");
        Check(mutexes[index]->ReleaseSync(0), "queued world release");
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            source->slots[index].status = 0;
        }
        context->CopyResource(slot->real.Get(), finalImage.Get());
        slot->phaseCount = 0;
        slot->nextPhase = 0;
        if (count && fruc->PrepareFlow(slot->ticket))
        {
            flowExecutions++;
            Compose(slot->target.Get(), fruc->ReferenceImage(slot->ticket));
            if (slot->phases.size()<count) slot->phases.resize(count);
            for(unsigned i=0;i<count;++i)
            {
                auto &phase=slot->phases[i];
                if(!phase.composed)
                {
                    D3D11_TEXTURE2D_DESC desc{};
                    slot->composed->GetDesc(&desc);
                    Check(device->CreateTexture2D(&desc,nullptr,&phase.composed),"temporal phase image");
                    Check(device->CreateRenderTargetView(phase.composed.Get(),nullptr,&phase.target),"temporal phase target");
                    Check(device->CreateShaderResourceView(phase.composed.Get(),nullptr,&phase.view),"temporal phase view");
                }
                phase.resolved=false;
                phase.ticket=fruc->Phase(slot->ticket,float(i+1)/float(count+1));
                layerGpuUs[3]=static_cast<long long>(1000*fruc->ContourMilliseconds());
                Compose(phase.target.Get());
                fruc->CheckComposed(phase.ticket,phase.view.Get(),slot->realView.Get(),
                    i ? slot->phases[i-1].view.Get() : slot->composedView.Get());
                fruc->Seal(phase.ticket);
                slot->phaseCount++;
                qualityRequests++;
            }
        }
        else if(count) {qualityRequests++;qualityReasons[Interpolator::NoHistory]++;}
        scheduledPhases=slot->phaseCount;
        fruc->Seal(slot->ticket);
        readyFrames.push_back(static_cast<unsigned>(slot - frames.begin()));
        workerCallUs = static_cast<long long>(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
        return true;
    }
#ifdef KOE_CAPTURE_AUDIT
    void Process(int index, double timestamp, double sourceInterval, std::uint64_t serial)
    {
        if (mutexes[index]->AcquireSync(1, 2000) != S_OK)
            throw std::runtime_error("worker mutex acquire timeout/failure");
        if (finalMutexes[index]->AcquireSync(1, 2000) != S_OK)
            throw std::runtime_error("worker UI mutex timeout");
        if (silhouetteMutexes[index]->AcquireSync(1, 2000) != S_OK)
            throw std::runtime_error("worker silhouette mutex timeout");
        Convert(views[index].Get(), inputView.Get());
        Convert(finalViews[index].Get(), finalView.Get());
        bool hasSilhouette = source->slots[index].hasSilhouette;
        if (hasSilhouette) context->CopyResource(silhouetteFrame.Get(), openedSilhouette[index].Get());
        if(layerMutexes[index])
        {
            if(layerMutexes[index]->AcquireSync(1,2000)!=S_OK)throw std::runtime_error("Diagnostic independent layers timeout");
            if(source->slots[index].hasLayers)
            {
                if(!diagnosticLayers)
                {
                    D3D11_TEXTURE2D_DESC desc{};openedLayers[index]->GetDesc(&desc);desc.MiscFlags=0;
                    Check(device->CreateTexture2D(&desc,nullptr,&diagnosticLayers),"diagnostic layer array");
                }
                context->CopyResource(diagnosticLayers.Get(),openedLayers[index].Get());
            }
            context->Flush();Check(layerMutexes[index]->ReleaseSync(0),"diagnostic independent layers release");
#ifdef KOE_DIRECT_FLOW
            fruc->SetLayerInput(source->slots[index].hasLayers?diagnosticLayers.Get():nullptr,
                source->slots[index].cloudFilmic,source->slots[index].cloudFade);
#endif
        }
        context->Flush();
        Check(silhouetteMutexes[index]->ReleaseSync(0), "worker silhouette release");
        Check(finalMutexes[index]->ReleaseSync(0), "worker UI release");
        Check(mutexes[index]->ReleaseSync(0), "worker release");
#ifdef KOE_CAPTURE_AUDIT
        CaptureDataset(hasSilhouette,timestamp,serial);
        if(datasetMode)
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            source->slots[index].status=0;
            return;
        }
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
            if (auditIndex > 0 && serial <= auditSerials[auditIndex - 1])
                auditIndex = 0;
            Snapshot(input.Get(), auditWorld[auditIndex]);
            auditHasSilhouette[auditIndex] = hasSilhouette;
            if(hasSilhouette) Snapshot(silhouetteFrame.Get(), auditSilhouette[auditIndex]);
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
        auto ticket = fruc->Submit(input.Get(), timestamp, midpointRequested, false,
            hasSilhouette ? silhouetteFrame.Get() : nullptr);
        if (midpointRequested)
        {
            qualityRequests++;
            auto inputBegin = Seconds();
            double inputBudget = FrameBudget::Input(timestamp, interval, inputBegin);
            bool ready = fruc->WaitForInput(ticket, static_cast<unsigned>(std::ceil(inputBudget * 1000)));
            double readyAt = Seconds();
            inputWaitUs = static_cast<long long>((readyAt - inputBegin) * 1000000);
            double remaining = ready ? FrameBudget::Decision(timestamp, interval, readyAt, Seconds()) : 0;
            midpointAllowed = fruc->Generate(ticket, static_cast<unsigned>(remaining * 1000));
            decisionAgeUs = static_cast<long long>((Seconds() - timestamp) * 1000000);
            qualityReasons[static_cast<int>(ticket->rejection)]++;
            if (ticket->flowEstimated)
                flowExecutions++;
            if (ticket->earlyVeto)
                preflightVetoes++;
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
        // AI-generated comment: Rejected work must not delay the game's normal real-frame presentation.
        if (!Visible() || !midpointAllowed)
        {
            pending.push_back({ticket, false});
#ifdef KOE_CAPTURE_AUDIT
            auditComposed.Reset();
            SaveAudit();
#endif
            if (IsWindowVisible(window))
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
#endif
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
            if (presenter)
            {
                std::shared_ptr<Shared> observed;
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    observed = shared;
                }
                if (presenter->source == observed && presenter->Visible())
                    presenter->ResolveFrames();
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
                if (index >= 0)
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    current->slots[index].status = 1;
                }
                if (presenter)
                {
                    presenter->sourceQueue.Disable();
                    presenter->displaying = false;
                    ShowWindow(presenter->window, SW_HIDE);
                }
                WaitForSingleObject(wakeEvent, 10);
                continue;
            }
            if (!presenter || presenter->source != current)
                presenter.reset(new Presenter(current));
            presenter->sourceQueue.Enable(current->source.Get());
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
                            HRESULT world = presenter->mutexes[i]->AcquireSync(1, 0);
                            HRESULT ui = world == S_OK ? presenter->finalMutexes[i]->AcquireSync(1, 0) : WAIT_TIMEOUT;
                            HRESULT hull = ui == S_OK ? presenter->silhouetteMutexes[i]->AcquireSync(1, 0) : WAIT_TIMEOUT;
                            HRESULT layers=hull==S_OK && presenter->layerMutexes[i]?presenter->layerMutexes[i]->AcquireSync(1,0):hull;
                            if (world != S_OK || ui != S_OK || hull != S_OK || layers!=S_OK)
                            {
                                if(hull==S_OK)Check(presenter->silhouetteMutexes[i]->ReleaseSync(1),"stale silhouette retry");
                                if (ui == S_OK) Check(presenter->finalMutexes[i]->ReleaseSync(1), "stale UI retry");
                                if (world == S_OK) Check(presenter->mutexes[i]->ReleaseSync(1), "stale input retry");
                                if ((world != S_OK && world != WAIT_TIMEOUT) || (ui != S_OK && ui != WAIT_TIMEOUT)
                                    || (hull != S_OK && hull != WAIT_TIMEOUT) || (layers!=S_OK && layers!=WAIT_TIMEOUT))
                                    throw std::runtime_error("stale input ownership lost");
                                std::lock_guard<std::mutex> lock(stateMutex);
                                current->slots[i].status = 1;
                                continue;
                            }
                            if(presenter->layerMutexes[i])Check(presenter->layerMutexes[i]->ReleaseSync(0),"stale independent layers release");
                            Check(presenter->silhouetteMutexes[i]->ReleaseSync(0), "stale silhouette release");
                            Check(presenter->finalMutexes[i]->ReleaseSync(0), "stale UI release");
                            Check(presenter->mutexes[i]->ReleaseSync(0), "stale input release");
                            std::lock_guard<std::mutex> lock(stateMutex);
                            current->slots[i].status = 0;
                            skipped++;
                        }
                    }
#ifdef KOE_CAPTURE_AUDIT
                presenter->Process(index, timestamp, cadence, serial);
#else
                if (!presenter->ProcessQueued(index, timestamp, cadence))
                {
                    {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        current->slots[index].status = 1;
                    }
                    WaitForSingleObject(wakeEvent, 1);
                }
#endif
            }
            else
            {
                if (!presenter->Visible())
                    ShowWindow(presenter->window, SW_HIDE);
                WaitForSingleObject(wakeEvent, 1);
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
        datasetMode=GetEnvironmentVariableW(L"KOE_LAYER_DATASET_ROOT",nullptr,0)>0;
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
extern "C" __declspec(dllexport) void SetFrameGenerationTargetFPS(int fps)
{
    requestedTargetFPS = std::max(TargetFrameRate::Minimum, fps);
    if(wakeEvent) SetEvent(wakeEvent);
}
extern "C" __declspec(dllexport) double GetFrameGenerationMonitorHz(HWND window)
{
    return ReadDisplayRefresh(window ? window : GetForegroundWindow());
}
extern "C" __declspec(dllexport) void ConfigureFrameGenerationTarget(int full, double target)
{
    requestedTargetFPS=std::isfinite(target) && target>0 ? target : TargetFrameRate::Default;
    fullGeneration=full!=0;
    adaptiveCadence=true;
    if(wakeEvent)SetEvent(wakeEvent);
}
extern "C" __declspec(dllexport) void ReadFrameGenerationTarget(long long *values, int count)
{
    if(!values || count!=5)return;
    values[0]=static_cast<long long>(std::llround(std::min(requestedTargetFPS.load(),double(INT_MAX))*1000));
    values[1]=monitorMilliHz.load();
    values[2]=effectiveMilliFPS.load();
    values[3]=scheduledPhases.load();
    values[4]=fullGeneration.load()?1:0;
}
extern "C" __declspec(dllexport) void InvalidateFrameGenerationHistory()
{
#ifdef KOE_CAPTURE_AUDIT
    if(datasetMode)return;
#endif
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
#ifdef KOE_CAPTURE_AUDIT
    LayerPassAudit::Stop();
#endif
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
        auto maskDesc = td;
        maskDesc.Usage = D3D11_USAGE_DEFAULT;
        maskDesc.CPUAccessFlags = 0;
        maskDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(value.source->CreateTexture2D(&maskDesc, nullptr, &value.cleanSilhouette), "capture test silhouette");
        Check(value.source->CreateRenderTargetView(value.cleanSilhouette.Get(), nullptr, &value.silhouetteTarget), "capture test silhouette target");
        CaptureWorld(value, image.Get(), DXGI_FORMAT_R16G16B16A16_TYPELESS, true);
        value.context->CopyResource(stage.Get(), value.cleanSilhouette.Get());
        Check(value.context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "silhouette orientation map");
        for(unsigned y=0;y<h;++y)
            for(unsigned x=0;x<w;++x)
            {
                auto p=static_cast<const unsigned char *>(m.pData)+size_t(y)*m.RowPitch+x*4;
                unsigned indices[]={x%5,y%5,(x+y)%5,4};
                for(unsigned c=0;c<4;++c)
                {
                    bool tie=c<3 && indices[c]==2 && p[c]==127;
                    if(tie)result[4]++;
                    else result[0]+=p[c]!=expected[indices[c]];
                    result[3]++;
                }
            }
        value.context->Unmap(stage.Get(),0);
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
#ifdef KOE_CAPTURE_AUDIT
extern "C" __declspec(dllexport) int TestFrameGenerationLayerPassAudit(const wchar_t *directory, long long *result)
{
    if(!directory || !result)return -1;
    result[0]=result[1]=result[2]=0;
    try
    {
        std::filesystem::path path(directory);
        if(std::filesystem::exists(path / "layer-pass-states.jsonl"))return -3;
        std::filesystem::create_directories(path);
        auto initialized=MH_Initialize();
        if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED)return -4;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &device,nullptr,&context),"pass audit test device");
        LayerPassAudit::Start(context.Get(),path,true);
        if(LayerPassAudit::observed.load()!=context.Get())return -5;
        D3D11_DEPTH_STENCIL_DESC stateDesc{};
        stateDesc.DepthEnable=TRUE;stateDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        stateDesc.DepthFunc=D3D11_COMPARISON_LESS;stateDesc.StencilEnable=TRUE;
        stateDesc.StencilReadMask=stateDesc.StencilWriteMask=7;
        stateDesc.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};
        stateDesc.BackFace=stateDesc.FrontFace;
        ComPtr<ID3D11DepthStencilState> state;
        Check(device->CreateDepthStencilState(&stateDesc,&state),"pass audit test state");
        context->OMSetDepthStencilState(state.Get(),3);
        ComPtr<ID3D11DepthStencilState> restored;UINT reference{};
        context->OMGetDepthStencilState(&restored,&reference);
        result[0]=restored.Get()!=state.Get() || reference!=3;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=16;desc.Height=12;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R24G8_TYPELESS;
        desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc,nullptr,&texture),"pass audit test depth");
        D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};viewDesc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;
        viewDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> view;
        Check(device->CreateDepthStencilView(texture.Get(),&viewDesc,&view),"pass audit test depth view");
        context->ClearDepthStencilView(view.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.25f,11);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        Check(device->CreateTexture2D(&desc,nullptr,&readback),"pass audit test readback");
        context->CopyResource(readback.Get(),texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"pass audit test map");
        for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x)
        {
            auto pixel=reinterpret_cast<const UINT *>(static_cast<const unsigned char *>(mapped.pData)+mapped.RowPitch*y)[x];
            result[1]+=(pixel>>24)!=11 || std::abs(int(pixel&0xffffff)-4194304)>1;
            result[2]++;
        }
        context->Unmap(readback.Get(),0);LayerPassAudit::Stop();
        return result[0]||result[1]?2:0;
    }
    catch(const std::exception &error){LayerPassAudit::Stop();lastError=error.what();return -2;}
}

extern "C" __declspec(dllexport) int TestFrameGenerationLayers(long long *result)
{
    if(!result)return -1;
    result[0]=result[1]=result[2]=0;
    struct RestoreInputs
    {
        ComPtr<ID3D11Texture2D> cloud=cloudInput,water=waterInput;
        ~RestoreInputs(){cloudInput=cloud;waterInput=water;}
    } restoreInputs;
    try
    {
        Shared value;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &value.source,nullptr,&value.context),"layer test device");
        constexpr unsigned w=16,h=12;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        value.desc=desc;
        Check(value.source->CreateTexture2D(&desc,nullptr,&value.cleanWorld),"layer test world");
        Check(value.source->CreateTexture2D(&desc,nullptr,&value.cleanSilhouette),"layer test output");
        Check(value.source->CreateRenderTargetView(value.cleanSilhouette.Get(),nullptr,&value.silhouetteTarget),"layer test output view");
        std::vector<unsigned char> pixels(w*h*4,80),layers(w*h*4,0),clouds(w*h*4,255);
        std::vector<float> water(w*h,0);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
        {
            auto p=(y*w+x)*4;pixels[p+3]=255;
            layers[p]=2;layers[p+1]=9;layers[p+3]=128;
            if(x<3 && y<4)clouds[p+3]=128;
            if(x>=13 && y>=8)water[y*w+x]=.5f;
        }
        D3D11_SUBRESOURCE_DATA initial{pixels.data(),w*4,0};
        ComPtr<ID3D11Texture2D> color,mask;
        Check(value.source->CreateTexture2D(&desc,&initial,&color),"layer test color");
        initial.pSysMem=layers.data();Check(value.source->CreateTexture2D(&desc,&initial,&mask),"layer test labels");
        initial.pSysMem=clouds.data();Check(value.source->CreateTexture2D(&desc,&initial,&cloudInput),"layer test cloud");
        auto waterDesc=desc;waterDesc.Format=DXGI_FORMAT_R32_FLOAT;
        initial.pSysMem=water.data();Check(value.source->CreateTexture2D(&waterDesc,&initial,&waterInput),"layer test water");
        ComPtr<ID3D11RenderTargetView> originalTarget;
        Check(value.source->CreateRenderTargetView(color.Get(),nullptr,&originalTarget),"layer test original target");
        auto *rt=originalTarget.Get();value.context->OMSetRenderTargets(1,&rt,nullptr);
        for(unsigned index=0;index<2;++index)
        {
            CaptureContribution(value,index,false);
            unsigned x0=index?9:4,x1=index?12:8,y0=index?2:3,y1=index?5:9;
            std::vector<unsigned char> change((x1-x0)*(y1-y0)*4,160);
            for(unsigned i=3;i<change.size();i+=4)change[i]=255;
            D3D11_BOX box{x0,y0,0,x1,y1,1};
            value.context->UpdateSubresource(color.Get(),0,&box,change.data(),(x1-x0)*4,0);
            CaptureContribution(value,index,true);
        }
        CaptureWorld(value,mask.Get(),desc.Format,true);
        ComPtr<ID3D11RenderTargetView> restored;
        value.context->OMGetRenderTargets(1,&restored,nullptr);
        result[1]=restored.Get()!=originalTarget.Get();
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;
        Check(value.source->CreateTexture2D(&desc,nullptr,&read),"layer test readback");
        value.context->CopyResource(read.Get(),value.cleanSilhouette.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(value.context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped),"layer test map");
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
        {
            unsigned rawY=h-1-y;
            bool ocean=x>=13&&rawY>=8;
            unsigned flags=128;
            if(x>=4&&x<8&&rawY>=3&&rawY<9)flags|=1;
            if(x<3&&rawY<4)flags|=2;
            if(x>=9&&x<12&&rawY>=2&&rawY<5)flags|=4;
            unsigned expected[]={ocean?3u:2u,ocean?0u:9u,0,flags};
            auto actual=static_cast<unsigned char *>(mapped.pData)+y*mapped.RowPitch+x*4;
            for(unsigned c=0;c<4;++c){result[0]+=actual[c]!=expected[c];result[2]++;}
        }
        value.context->Unmap(read.Get(),0);
        return result[0]||result[1]?2:0;
    }
    catch(const std::exception &error){lastError=error.what();return -2;}
}
#endif
// AI-generated comment: Module and Present trampoline remain loaded until process exit.
