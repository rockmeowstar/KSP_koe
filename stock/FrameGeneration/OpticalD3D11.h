#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <algorithm>
#include "nvOpticalFlowD3D11.h"

class IndependentLayers;

class OpticalD3D11
{
    struct DecisionReadback
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        std::uint64_t fenceValue{};
    };
  public:
    enum Rejection
    {
        Accepted = 0,
        NoHistory = 1,
        NotRequested = 2,
        Late = 3,
        Static = 4,
        Motion = 5,
        Contour = 6,
        Invalid = 7
    };
    struct Ticket
    {
        const OpticalD3D11 *owner{};
        bool repeated{}, eligible{}, analysisPending{}, generationQueued{}, preflightPending{};
        bool flowEstimated{}, earlyVeto{};
        bool gpuConditional{};
        bool composedChecked{};
        bool previousChecked{};
        std::uint64_t fenceValue{}, readyFenceValue{}, serial{};
        unsigned before{}, after{};
        Rejection rejection = NoHistory;
        std::uint32_t counts[12]{};
        Microsoft::WRL::ComPtr<ID3D11Buffer> decision;
        std::shared_ptr<DecisionReadback> readback;
    };
    OpticalD3D11(ID3D11Device *device, unsigned width, unsigned height, const std::filesystem::path &unused,
                 bool conservative = true, NV_OF_PERF_LEVEL performance = NV_OF_PERF_LEVEL_SLOW);
    ~OpticalD3D11();
    bool Process(ID3D11Texture2D *current, double timestamp);
    std::shared_ptr<Ticket> Submit(ID3D11Texture2D *current, double timestamp, bool analyze = true, bool gpuConditional = false,
                                   ID3D11Texture2D *silhouette = nullptr);
    bool ResolveGpu(const std::shared_ptr<Ticket> &ticket);
    void Seal(const std::shared_ptr<Ticket> &ticket);
    void CheckComposed(const std::shared_ptr<Ticket> &ticket, ID3D11ShaderResourceView *composed,
                       ID3D11ShaderResourceView *real, ID3D11ShaderResourceView *previous = nullptr);
    bool PrepareFlow(const std::shared_ptr<Ticket> &source);
    std::shared_ptr<Ticket> Phase(const std::shared_ptr<Ticket> &source, float fraction);
    ID3D11ShaderResourceView *ReferenceImage(const std::shared_ptr<Ticket> &source) const;
    void ReserveDecisions(unsigned capacity) { readbackLimit = (std::max)(readbackLimit, capacity); }
    void SetLayerInput(ID3D11Texture2D *layers,bool filmic,float cloudFade);
    bool Generate(const std::shared_ptr<Ticket> &ticket, unsigned budgetMilliseconds);
    bool Completed(const std::shared_ptr<Ticket> &ticket) const;
    bool Wait(unsigned milliseconds = 5000);
    bool WaitForInput(const std::shared_ptr<Ticket> &ticket, unsigned milliseconds);
    ID3D11Texture2D *Output() const
    {
        return output.Get();
    }
    double LastCallMilliseconds() const
    {
        return callMs;
    }
    ID3D11Texture2D *DiagnosticFlow(unsigned direction) const
    {
        return flow[direction & 1].Get();
    }
    ID3D11Texture2D *DiagnosticMask() const { return mask.Get(); }
    ID3D11Texture2D *DiagnosticContours(unsigned frame) const { return contours[frame & 1].Get(); }
    double ContourMilliseconds() const { return contourMilliseconds; }

  private:
    friend struct ConfidenceGateFixture;
    void Cleanup();
    bool WaitFor(std::uint64_t value, unsigned milliseconds);
    void Analyze(Ticket &ticket, bool preflight = false);
    void Estimate(Ticket &ticket);
    void EstimateFlow(Ticket &ticket);
    void SetPhase(float fraction);
    void QueueMaskedWarp(Ticket &ticket);
    void BuildContours(Ticket &ticket);
    void Signal(Ticket &ticket);
    void ReleaseDecision(Ticket &ticket);
    void AllocateReadback(Ticket &ticket,bool preflight=false);
    std::unique_ptr<IndependentLayers> independent;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> incomingLayers;
    bool incomingFilmic{};
    float incomingFade=1;
    void CopyImage(ID3D11ShaderResourceView *source, ID3D11RenderTargetView *target);
    HMODULE module{};
    NvOFHandle session{};
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    Microsoft::WRL::ComPtr<ID3D11Device5> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputs[2], flow[2], output;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> imageViews[2], flowViews[2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> silhouettes[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> silhouetteViews[2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> contours[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> contourViews[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> contourTargets[2];
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> contourShader;
    std::uint64_t contourSubmission{};
    struct ContourTiming
    {
        Microsoft::WRL::ComPtr<ID3D11Query> frequency, begin, end;
        bool pending{};
    };
    ContourTiming contourTimings[4];
    double contourMilliseconds{};
    bool silhouetteValid[2]{};
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
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> confidence, preflight;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> maskedWarp, dispatchGate, compareComposed;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mask;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskView, decisionSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> maskTarget, outputTarget, dispatchTarget;
    Microsoft::WRL::ComPtr<ID3D11Buffer> dispatchArguments;
    Microsoft::WRL::ComPtr<ID3D11Buffer> comparisonParameters;
    Microsoft::WRL::ComPtr<ID3D11Buffer> decisionBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> decisionView;
    std::vector<std::shared_ptr<DecisionReadback>> readbacks;
    HANDLE completionEvent{};
    std::uint64_t waitingFor{};
    unsigned readbackLimit = 4;
    unsigned phasesThisSubmission{};
    NvOFGPUBufferHandle inputHandles[2]{}, flowHandles[2]{};
    unsigned width{}, height{}, next{};
    bool previous{};
    double lastTimestamp{}, callMs{};
    std::uint64_t sequence{}, submission{};
};
